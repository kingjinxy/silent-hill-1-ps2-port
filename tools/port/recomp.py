#!/usr/bin/env python3
"""Static recompiler: PS1 (MIPS-I, o32, REL) object code -> C for the port.

    recomp.py <obj.o> <out_dir> [--protos header.h ...] [--prefix NAME]

Writes <out_dir>/<obj>.c (functions) and <out_dir>/<obj>.data.s (the object's data sections, assembled
with the EE toolchain). Each function becomes `static void rc_<name>(RcRegs* r)`; global functions also
get a wrapper with their original name and argument count (from the prototypes in --protos headers),
so game code calls them as before. See include/port/recomp.h for the runtime.

Semantics kept from the PS1: branch delay slots (condition evaluated before the slot), load delays
where the next instruction reads the loaded register, HI/LO, DIV by zero, GTE via src/port/gte.c.
Anything the translator doesn't handle (COP0, unknown relocations, jr through unknown addresses,
load delay into a branch) is an error, never a guess.
"""
import argparse
import os
import re
import sys

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

REG = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]
R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6


class RecompError(Exception):
    pass


def c_ident(name):
    return re.sub(r"\W", "_", name)


def load_protos(headers):
    """name -> (returns_value, nargs) from C prototypes."""
    protos = {}
    for h in headers:
        text = open(h, errors="replace").read()
        text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
        text = re.sub(r"//[^\n]*", " ", text)
        for m in re.finditer(r"(?:^|;|\})\s*(?:extern\s+)?([A-Za-z_][\w\s\*]*?)\b([A-Za-z_]\w*)\s*\(([^;{}()]*)\)\s*;", text, re.M):
            ret, name, params = m.group(1).strip(), m.group(2), m.group(3).strip()
            if not ret or ret in ("return", "else", "typedef"):
                continue
            if params in ("", "void"):
                n = 0
            elif "..." in params:
                n = -1
            else:
                n = len(params.split(","))
            protos[name] = (not re.fullmatch(r"(?:static\s+)?void", ret), n)
    return protos


class ObjectFile:
    def __init__(self, path):
        self.path = path
        self.base = c_ident(os.path.basename(path)[:-2])
        f = open(path, "rb")
        self.elf = ELFFile(f)
        self.sections = {}
        for i, s in enumerate(self.elf.iter_sections()):
            self.sections[i] = s
        self.symtab = self.elf.get_section_by_name(".symtab")
        self.syms = list(self.symtab.iter_symbols())
        self.relocs = {}  # section name -> {offset: (type, symbol index)}
        for s in self.elf.iter_sections():
            if isinstance(s, RelocationSection):
                target = self.sections[s["sh_info"]].name
                self.relocs[target] = {r["r_offset"]: (r["r_info_type"], r["r_info_sym"]) for r in s.iter_relocations()}

    def section_label(self, secname):
        return "__rc_%s_%s" % (self.base, c_ident(secname.lstrip(".")))

    def sym_ref(self, idx):
        """Symbol index -> (C/asm name, is_section, section name or None, value)."""
        s = self.syms[idx]
        if s["st_info"]["type"] == "STT_SECTION":
            sec = self.sections[s["st_shndx"]].name
            return self.section_label(sec), True, sec, 0
        name = s.name
        if s["st_shndx"] == "SHN_UNDEF":
            return name, False, None, 0
        sec = self.sections[s["st_shndx"]].name
        if s["st_info"]["bind"] == "STB_LOCAL":
            name = "__rc_%s_%s" % (self.base, c_ident(name))
        return name, False, sec, s["st_value"]


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def translate_object(path, out_dir, protos, prefix):
    obj = ObjectFile(path)
    text = obj.elf.get_section_by_name(".text")
    code = text.data() if text is not None else b""
    words = [int.from_bytes(code[i:i + 4], "little") for i in range(0, len(code), 4)]
    trel = obj.relocs.get(".text", {})

    # Function symbols in .text.
    funcs = sorted({(s["st_value"], s.name, s["st_info"]["bind"]) for s in obj.syms
                    if s["st_shndx"] != "SHN_UNDEF" and isinstance(s["st_shndx"], int)
                    and obj.sections[s["st_shndx"]].name == ".text" and s.name
                    and s["st_info"]["type"] in ("STT_FUNC", "STT_NOTYPE")})
    names_at = {}
    for v, n, b in funcs:
        names_at.setdefault(v, []).append((n, b))
    # Calls (jal) to labels inside this .text that aren't symbols become functions of their own.
    for off in range(0, len(code), 4):
        w = words[off // 4]
        if w >> 26 == 3 and off in trel:
            name, is_sec, sec, value = obj.sym_ref(trel[off][1])
            tgt = ((w & 0x3FFFFFF) << 2) + value
            if sec == ".text" and tgt not in names_at:
                names_at[tgt] = [("__rc_%s_sub_%X" % (obj.base, tgt), "STB_LOCAL")]
    # Branches/jumps that leave their function (hand-written asm sharing code tails) make the target
    # an entry point too; repeat until no branch crosses a function boundary.
    def branch_target(off, w):
        op = w >> 26
        if op in (1, 4, 5, 6, 7):
            return off + 4 + (sext16(w & 0xFFFF) << 2)
        if op == 2:
            return ((w & 0x3FFFFFF) << 2) + (obj.sym_ref(trel[off][1])[3] if off in trel else 0)
        return None

    while True:
        starts = sorted(names_at)
        added = False
        for i, st in enumerate(starts):
            en = starts[i + 1] if i + 1 < len(starts) else len(code)
            for off in range(st, en, 4):
                t = branch_target(off, words[off // 4])
                if t is not None and not st <= t < en and t not in names_at:
                    names_at[t] = [("__rc_%s_sub_%X" % (obj.base, t), "STB_LOCAL")]
                    added = True
        if not added:
            break
    starts = sorted(names_at)

    # HI16/LO16 pairing: addend AHL for each HI16 from the following LO16 of the same symbol.
    hi_addend = {}
    pending = []
    for off in sorted(trel):
        rtype, sidx = trel[off]
        if rtype == R_MIPS_HI16:
            pending.append(off)
        elif rtype == R_MIPS_LO16:
            lo_imm = sext16(words[off // 4] & 0xFFFF)
            for h in pending:
                if trel[h][1] == sidx:
                    hi_addend[h] = ((words[h // 4] & 0xFFFF) << 16) + lo_imm
            pending = [h for h in pending if trel[h][1] != sidx]
    if pending:
        raise RecompError("unpaired HI16 relocation at %s" % ", ".join(hex(p) for p in pending))
    last_hi_addend = {}
    lo_addend = {}
    for off in sorted(trel):
        rtype, sidx = trel[off]
        if rtype == R_MIPS_HI16:
            last_hi_addend[sidx] = hi_addend[off]
        elif rtype == R_MIPS_LO16:
            if sidx in last_hi_addend:
                lo_addend[off] = last_hi_addend[sidx]
            else:
                lo_addend[off] = sext16(words[off // 4] & 0xFFFF)

    externs = set()
    # .text offsets stored in data (jump tables): jump targets for `jr reg`.
    code_refs = set()
    for secname, rels in obj.relocs.items():
        if secname == ".text":
            continue
        sec = obj.elf.get_section_by_name(secname)
        for roff, (rtype, sidx) in rels.items():
            name, is_sec, rsec, value = obj.sym_ref(sidx)
            if rtype == R_MIPS_32 and rsec == ".text":
                code_refs.add(int.from_bytes(sec.data()[roff:roff + 4], "little") + value)

    def addr_expr(off, kind):
        """C expression for a relocated HI/LO half."""
        rtype, sidx = trel[off]
        name, is_sec, sec, value = obj.sym_ref(sidx)
        add = (hi_addend[off] if kind == "hi" else lo_addend[off]) + value
        externs.add(name)
        full = "((unsigned int)(unsigned long)%s + %du)" % (name, add & 0xFFFFFFFF)
        if kind == "hi":
            return "((%s + 0x8000u) & 0xFFFF0000u)" % full
        return "(unsigned int)(int)(short)(%s & 0xFFFFu)" % full

    def R(n):
        return "0u" if n == 0 else "r->r[%d]" % n

    def W(n, expr):
        return "" if n == 0 else "r->r[%d] = %s;" % (n, expr)

    def imm_expr(off, w):
        if off in trel and trel[off][0] == R_MIPS_LO16:
            return addr_expr(off, "lo")
        return "%du" % (sext16(w & 0xFFFF) & 0xFFFFFFFF)

    def reads(w):
        """Registers an instruction reads (for load-delay hazards)."""
        op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        if op == 0:
            fn = w & 63
            if fn in (0, 2, 3):
                return {rt}
            if fn in (8, 9):
                return {rs}
            if fn in (0x11, 0x13):
                return {rs}
            if fn in (0x10, 0x12, 0x0C, 0x0D):
                return set()
            return {rs, rt}
        if op == 1 or op in (6, 7):
            return {rs}
        if op in (4, 5):
            return {rs, rt}
        if op in (2, 3, 0xF):
            return set()
        if op == 0x12:
            sub = rs
            return {rt} if sub in (4, 6) else set()
        if op in (0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x22, 0x26):
            return {rs, rt}
        return {rs}

    def is_control(w):
        op = w >> 26
        return op in (1, 2, 3, 4, 5, 6, 7) or (op == 0 and (w & 63) in (8, 9))

    def simple(off, w):
        """Non-control instruction -> (C statement, loaded register or None)."""
        op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        sa, fn, imm = (w >> 6) & 31, w & 63, w & 0xFFFF
        if w == 0:
            return "", None
        if op == 0:
            if fn == 0: return W(rd, "%s << %d" % (R(rt), sa)), None
            if fn == 2: return W(rd, "%s >> %d" % (R(rt), sa)), None
            if fn == 3: return W(rd, "(unsigned int)((int)%s >> %d)" % (R(rt), sa)), None
            if fn == 4: return W(rd, "%s << (%s & 31)" % (R(rt), R(rs))), None
            if fn == 6: return W(rd, "%s >> (%s & 31)" % (R(rt), R(rs))), None
            if fn == 7: return W(rd, "(unsigned int)((int)%s >> (%s & 31))" % (R(rt), R(rs))), None
            if fn == 0x10: return W(rd, "r->hi"), None
            if fn == 0x11: return "r->hi = %s;" % R(rs), None
            if fn == 0x12: return W(rd, "r->lo"), None
            if fn == 0x13: return "r->lo = %s;" % R(rs), None
            if fn == 0x18: return "Rc_Mult(r, %s, %s);" % (R(rs), R(rt)), None
            if fn == 0x19: return "Rc_Multu(r, %s, %s);" % (R(rs), R(rt)), None
            if fn == 0x1A: return "Rc_Div(r, %s, %s);" % (R(rs), R(rt)), None
            if fn == 0x1B: return "Rc_Divu(r, %s, %s);" % (R(rs), R(rt)), None
            if fn in (0x20, 0x21): return W(rd, "%s + %s" % (R(rs), R(rt))), None  # add: overflow trap not modelled
            if fn in (0x22, 0x23): return W(rd, "%s - %s" % (R(rs), R(rt))), None
            if fn == 0x24: return W(rd, "%s & %s" % (R(rs), R(rt))), None
            if fn == 0x25: return W(rd, "%s | %s" % (R(rs), R(rt))), None
            if fn == 0x26: return W(rd, "%s ^ %s" % (R(rs), R(rt))), None
            if fn == 0x27: return W(rd, "~(%s | %s)" % (R(rs), R(rt))), None
            if fn == 0x2A: return W(rd, "(unsigned int)((int)%s < (int)%s)" % (R(rs), R(rt))), None
            if fn == 0x2B: return W(rd, "(unsigned int)(%s < %s)" % (R(rs), R(rt))), None
            if fn == 0x0D: return "/* break */", None
            raise RecompError("SPECIAL fn 0x%02X at 0x%X" % (fn, off))
        if op in (8, 9): return W(rt, "%s + %s" % (R(rs), imm_expr(off, w))), None
        if op == 0x0A: return W(rt, "(unsigned int)((int)%s < (int)%s)" % (R(rs), imm_expr(off, w))), None
        if op == 0x0B: return W(rt, "(unsigned int)(%s < %s)" % (R(rs), imm_expr(off, w))), None
        if op in (0x0C, 0x0D, 0x0E):
            if off in trel:
                raise RecompError("relocated logical immediate at 0x%X" % off)
            sym = {0x0C: "&", 0x0D: "|", 0x0E: "^"}[op]
            return W(rt, "%s %s 0x%Xu" % (R(rs), sym, imm)), None
        if op == 0x0F:
            if off in trel and trel[off][0] == R_MIPS_HI16:
                return W(rt, addr_expr(off, "hi")), None
            return W(rt, "0x%Xu" % (imm << 16)), None
        ea = "%s + %s" % (R(rs), imm_expr(off, w))
        if op == 0x20: return "(unsigned int)RC_R8(%s)" % ea, rt
        if op == 0x21: return "(unsigned int)RC_R16(%s)" % ea, rt
        if op == 0x23: return "RC_R32(%s)" % ea, rt
        if op == 0x24: return "(unsigned int)RC_RU8(%s)" % ea, rt
        if op == 0x25: return "(unsigned int)RC_RU16(%s)" % ea, rt
        if op == 0x22: return W(rt, "Rc_Lwl(%s, %s)" % (R(rt), ea)), None
        if op == 0x26: return W(rt, "Rc_Lwr(%s, %s)" % (R(rt), ea)), None
        if op == 0x28: return "RC_RU8(%s) = (unsigned char)%s;" % (ea, R(rt)), None
        if op == 0x29: return "RC_RU16(%s) = (unsigned short)%s;" % (ea, R(rt)), None
        if op == 0x2B: return "RC_R32(%s) = %s;" % (ea, R(rt)), None
        if op == 0x2A: return "Rc_Swl(%s, %s);" % (R(rt), ea), None
        if op == 0x2E: return "Rc_Swr(%s, %s);" % (R(rt), ea), None
        if op == 0x32: return "Gte_DataWrite(%d, RC_R32(%s));" % (rt, ea), None
        if op == 0x3A: return "RC_R32(%s) = Gte_DataRead(%d);" % (ea, rt), None
        if op == 0x12:
            if rs & 0x10:
                return "Gte_Command(0x%07X);" % (w & 0x1FFFFFF), None
            if rs == 0: return "Gte_DataRead(%d)" % rd, rt
            if rs == 2: return "Gte_CtrlRead(%d)" % rd, rt
            if rs == 4: return "Gte_DataWrite(%d, %s);" % (rd, R(rt)), None
            if rs == 6: return "Gte_CtrlWrite(%d, %s);" % (rd, R(rt)), None
        raise RecompError("opcode 0x%02X (word %08X) at 0x%X" % (op, w, off))

    func_bodies = []
    for fi, start in enumerate(starts):
        end = starts[fi + 1] if fi + 1 < len(starts) else len(code)
        fname = c_ident(names_at[start][0][0])
        if start % 4 or end <= start:
            continue
        lines = []
        targets = set()
        n = (end - start) // 4
        for k in range(n):
            off = start + k * 4
            w = words[off // 4]
            op = w >> 26
            if op in (1, 4, 5, 6, 7):
                targets.add(off + 4 + (sext16(w & 0xFFFF) << 2))
            elif op == 2:
                targets.add(((w & 0x3FFFFFF) << 2) + (obj.sym_ref(trel[off][1])[3] if off in trel else 0))
        k = 0
        while k < n:
            off = start + k * 4
            w = words[off // 4]
            op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
            lab = "L_%X:" % off
            nxt = words[off // 4 + 1] if k + 1 < n else 0

            def delay():
                s, ld = simple(off + 4, nxt)
                if is_control(nxt):
                    raise RecompError("control instruction in delay slot at 0x%X" % (off + 4))
                return s if ld is None else W(ld, s)

            if is_control(w):
                if op == 0 and (w & 63) == 8:  # jr
                    # jr $ra, or jr through a register holding a copy of $ra (`addiu/addu/or X, ra, 0`):
                    # a return, since recompiled calls return through C.
                    ra_copy = rs == 31 or any(
                        ((x >> 26) == 9 and (x >> 21) & 31 == 31 and (x >> 16) & 31 == rs and x & 0xFFFF == 0)
                        or ((x >> 26) == 0 and (x & 63) in (0x21, 0x25) and (x >> 11) & 31 == rs
                            and {(x >> 21) & 31, (x >> 16) & 31} == {31, 0})
                        for x in words[start // 4:off // 4])
                    if ra_copy:
                        lines.append("%s { %s return; }" % (lab, delay()))
                    else:
                        # Jump table: the register holds a code address loaded from data, which the data
                        # sections store as RC_TEXT_MARK + .text offset (see the data emitter).
                        cases = sorted(t for t in code_refs if start <= t < end)
                        if not cases:
                            raise RecompError("jr $%s at 0x%X with no code addresses in data" % (REG[rs], off))
                        sw = " ".join("case 0x%08Xu: goto L_%X;" % (0xC0DE0000 + t, t) for t in cases)
                        lines.append("%s { unsigned int t = %s; %s switch (t) { %s default: Rc_BadJump(t); return; } }"
                                     % (lab, R(rs), delay(), sw))
                elif op == 0 and (w & 63) == 9:
                    # jalr: call through a C function pointer (e.g. libgs's GsFCALL4 table, filled from C).
                    # Arguments: a0-a3, then the o32 stack words at sp+16.. (extra ones are ignored).
                    if (w >> 11) & 31 != 31:
                        raise RecompError("jalr with link register %d at 0x%X" % ((w >> 11) & 31, off))
                    stk = ", ".join("RC_R32(r->r[29] + %d)" % (16 + 4 * i) for i in range(8))
                    lines.append("%s { unsigned int t = %s; %s r->r[31] = 0; r->r[2] = ((unsigned int (*)())(unsigned long)t)"
                                 "(r->r[4], r->r[5], r->r[6], r->r[7], %s); goto L_%X; }" % (lab, R(rs), delay(), stk, off + 8))
                elif op in (2, 3):
                    if off not in trel or trel[off][0] != R_MIPS_26:
                        tgt = (w & 0x3FFFFFF) << 2
                        if op == 3:
                            raise RecompError("jal without relocation at 0x%X" % off)
                        lines.append("%s { %s goto L_%X; }" % (lab, delay(), tgt))
                    else:
                        name, is_sec, sec, value = obj.sym_ref(trel[off][1])
                        tgt = ((w & 0x3FFFFFF) << 2) + value
                        if op == 2 and sec == ".text" and start <= tgt < end:
                            lines.append("%s { %s goto L_%X; }" % (lab, delay(), tgt))
                            k += 1
                            continue
                        if sec == ".text":
                            callee = c_ident(names_at[tgt][0][0]) if tgt in names_at else None
                            if callee is None:
                                raise RecompError("call into the middle of a function at 0x%X" % off)
                            call = "rc_%s(r);" % callee
                        else:
                            # Another object: recompiled code is called directly (shared registers); for plain C
                            # functions tools/port/recomp_bridges.py generates rc_<name> bridges.
                            call = "rc_%s(r);" % c_ident(name)
                            externs.add(("call", name))
                        if op == 3:
                            lines.append("%s { %s r->r[31] = 0; %s goto L_%X; }" % (lab, delay(), call, off + 8))
                        else:
                            lines.append("%s { %s %s return; }" % (lab, delay(), call))
                else:
                    tgt = off + 4 + (sext16(w & 0xFFFF) << 2)
                    if op == 4: cond = "%s == %s" % (R(rs), R(rt))
                    elif op == 5: cond = "%s != %s" % (R(rs), R(rt))
                    elif op == 6: cond = "(int)%s <= 0" % R(rs)
                    elif op == 7: cond = "(int)%s > 0" % R(rs)
                    else:
                        if rt in (0x10, 0x11):
                            raise RecompError("bltzal/bgezal at 0x%X" % off)
                        cond = "(int)%s < 0" % R(rs) if rt == 0 else "(int)%s >= 0" % R(rs)
                    if start <= tgt < end:
                        lines.append("%s { int c = %s; %s if (c) goto L_%X; goto L_%X; }" % (lab, cond, delay(), tgt, off + 8))
                    else:
                        lines.append("%s { int c = %s; %s if (c) { rc_%s(r); return; } goto L_%X; }"
                                     % (lab, cond, delay(), c_ident(names_at[tgt][0][0]), off + 8))
                k += 1
                continue

            stmt, loaded = simple(off, w)
            if loaded is not None and loaded != 0:
                hazard = k + 1 < n and loaded in reads(nxt)
                if hazard:
                    if (off + 4) in targets:
                        raise RecompError("load delay hazard into a branch target at 0x%X" % off)
                    if is_control(nxt):
                        raise RecompError("load delay hazard into a branch at 0x%X" % off)
                    s2, ld2 = simple(off + 4, nxt)
                    s2 = s2 if ld2 is None else W(ld2, s2)
                    lines.append("%s { unsigned int v = %s; %s r->r[%d] = v; goto L_%X; }" % (lab, stmt, s2, loaded, off + 8))
                    k += 1
                    continue
                stmt = W(loaded, stmt)
            elif loaded == 0:
                stmt = "(void)%s;" % stmt
            lines.append("%s %s" % (lab, stmt))
            k += 1

        used = {int(m, 16) for m in re.findall(r"goto L_([0-9A-F]+);", "\n".join(lines))}
        body = []
        for l in lines:
            m = re.match(r"L_([0-9A-F]+):\s?(.*)", l)
            if int(m.group(1), 16) in used:
                body.append("    L_%s: %s" % (m.group(1), m.group(2) or ";"))
            elif m.group(2):
                body.append("    " + m.group(2))
        tail_off = start + n * 4
        # Falling off the end continues into the next function (split at a call target).
        nxt_func = c_ident(names_at[end][0][0]) if end in names_at else None
        func_bodies.append((fname, start, body, tail_off, nxt_func))

    # Emit C.
    base = prefix or obj.base
    out_c = os.path.join(out_dir, base + ".c")
    lines = ["/* Generated by tools/port/recomp.py from %s. Do not edit. */" % path, '#include "port/recomp.h"', ""]
    data_names = sorted(n for n in externs if isinstance(n, str))
    for n in data_names:
        lines.append("extern char %s[];" % n)
    calls = sorted(n for k, n in (e for e in externs if isinstance(e, tuple)))
    for n in calls:
        lines.append("void rc_%s(RcRegs* r);" % c_ident(n))
    lines.append("")
    for fname, _, _, _, _ in func_bodies:
        lines.append("void rc_%s(RcRegs* r);" % fname)
    lines.append("")
    for fname, start, body, tail, nxt_func in func_bodies:
        lines.append("/* %s (.text+0x%X) */" % (fname, start))
        lines.append("void rc_%s(RcRegs* r)\n{" % fname)
        lines += body
        tail_used = ("goto L_%X;" % tail) in "\n".join(body)
        lines.append("    %s%s return;" % ("L_%X: " % tail if tail_used else "", "rc_%s(r);" % nxt_func if nxt_func else ""))
        lines.append("}\n")

    # Wrappers for global functions.
    wrapped = []
    for start in starts:
        for name, bind in names_at[start]:
            if bind != "STB_GLOBAL":
                continue
            if name not in protos:
                print("warning: no prototype for %s; no wrapper" % name, file=sys.stderr)
                continue
            ret, nargs = protos[name]
            if nargs < 0:
                raise RecompError("variadic %s" % name)
            params = ", ".join("unsigned int a%d" % i for i in range(nargs)) or "void"
            lines.append("%s %s(%s)\n{" % ("unsigned int" if ret else "void", name, params))
            lines.append("    RcRegs regs = { { 0 } };\n    unsigned int stack[RC_STACK_WORDS];")
            lines.append("    regs.r[29] = (unsigned int)(unsigned long)&stack[RC_STACK_WORDS - 64];")
            for i in range(nargs):
                if i < 4:
                    lines.append("    regs.r[%d] = a%d;" % (4 + i, i))
                else:
                    lines.append("    stack[RC_STACK_WORDS - 64 + %d] = a%d;" % (i, i))
            lines.append("    rc_%s(&regs);" % c_ident(names_at[start][0][0]))
            if ret:
                lines.append("    return regs.r[2];")
            lines.append("}\n")
            wrapped.append(name)
    open(out_c, "w").write("\n".join(lines))

    # Emit data sections as assembly.
    out_s = os.path.join(out_dir, base + ".data.s")
    s_lines = ["/* Generated by tools/port/recomp.py from %s. Do not edit. */" % path, ".set noreorder"]
    for sec in obj.elf.iter_sections():
        if sec.name not in (".data", ".rodata", ".rdata", ".sdata", ".bss", ".sbss") or sec["sh_size"] == 0:
            continue
        nobits = sec["sh_type"] == "SHT_NOBITS"
        kind = ".section .bss,\"aw\",@nobits" if nobits else (".section .data" if sec.name in (".data", ".sdata") else ".section .rodata")
        # Labels are global: the generated .c references them. Local symbols are already prefixed
        # with the object name, so they can't clash.
        s_lines += ["", kind, ".balign %d" % max(4, sec["sh_addralign"]), ".globl %s" % obj.section_label(sec.name),
                    "%s:" % obj.section_label(sec.name)]
        labels = {}
        for s in obj.syms:
            if isinstance(s["st_shndx"], int) and obj.sections[s["st_shndx"]] is not None and obj.sections[s["st_shndx"]].name == sec.name and s.name and s["st_info"]["type"] != "STT_SECTION":
                name, _, _, _ = obj.sym_ref(obj.syms.index(s))
                labels.setdefault(s["st_value"], []).append((name, s["st_info"]["bind"] == "STB_GLOBAL"))
        rel = obj.relocs.get(sec.name, {})
        data = b"" if nobits else sec.data()
        size = sec["sh_size"]
        o = 0
        while o < size:
            for name, glob in labels.get(o, []):
                s_lines += [".globl %s" % name, "%s:" % name]
            if nobits:
                nxt = min([x for x in labels if x > o] + [size])
                s_lines.append("    .space %d" % (nxt - o))
                o = nxt
                continue
            if o in rel:
                rtype, sidx = rel[o]
                if rtype != R_MIPS_32:
                    raise RecompError("data relocation type %d in %s" % (rtype, sec.name))
                name, is_sec, rsec, value = obj.sym_ref(sidx)
                add = int.from_bytes(data[o:o + 4], "little") + value
                if rsec == ".text":
                    s_lines.append("    .word 0x%08X  /* code address .text+0x%X */" % (0xC0DE0000 + add, add))
                else:
                    s_lines.append("    .word %s + %d" % (name, add))
                o += 4
                continue
            s_lines.append("    .byte %d" % data[o])
            o += 1
    open(out_s, "w").write("\n".join(s_lines) + "\n")
    return out_c, out_s, wrapped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("obj")
    ap.add_argument("out_dir")
    ap.add_argument("--protos", nargs="*", default=[])
    ap.add_argument("--prefix")
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)
    try:
        c, s, wrapped = translate_object(a.obj, a.out_dir, load_protos(a.protos), a.prefix)
    except RecompError as e:
        sys.exit("%s: %s" % (a.obj, e))
    print("%s -> %s, %s (%s)" % (a.obj, c, s, ", ".join(wrapped)))


if __name__ == "__main__":
    main()
