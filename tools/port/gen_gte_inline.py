#!/usr/bin/env python3
"""Translates the GTE inline-asm macros into C calls to the software GTE (src/port/gte.c).

For each source header, writes include/port/gte_from_<name>.h, which #undefs and redefines every
`gte_*` macro of that header whose body contains `__asm__`, with each asm statement replaced by
equivalent C. The source header includes its translation at the end under SH_PORT, so the macro
override order (inline_c.h -> inline_no_dmpsx.h -> gpu.h) is unchanged.

The translation is static (done here, not interpreted at run time). Supported: lwc2/swc2,
mtc2/mfc2/ctc2/cfc2, `.word <cop2 instruction>`, and the CPU instructions these macros use on
scratch registers ($12-$15): lw/lh/lhu/lbu, sw/sh/sb, sll/srl/sra, or/and/addu/subu/addi/negu/move.
Anything else is a hard error, so a new macro can't be translated silently wrong.

    python3 tools/port/gen_gte_inline.py   (rerun after editing any of the source headers)
"""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SOURCES = {
    "include/psyq/inline_c.h": "include/port/gte_from_inline_c.h",
    "include/inline_no_dmpsx.h": "include/port/gte_from_inline_no_dmpsx.h",
    "include/gpu.h": "include/port/gte_from_gpu.h",
}


class Unsupported(Exception):
    pass


def balanced(s, i):
    """Index just past the parenthesised group starting at s[i] == '('."""
    depth = 0
    in_str = False
    j = i
    while j < len(s):
        ch = s[j]
        if in_str:
            if ch == "\\":
                j += 1
            elif ch == '"':
                in_str = False
        elif ch == '"':
            in_str = True
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return j + 1
        j += 1
    raise Unsupported("unbalanced parentheses")


def split_top(s, sep):
    """Split on `sep` at paren depth 0, outside strings."""
    parts, depth, in_str, cur = [], 0, False, ""
    i = 0
    while i < len(s):
        ch = s[i]
        if in_str:
            cur += ch
            if ch == "\\":
                cur += s[i + 1]
                i += 1
            elif ch == '"':
                in_str = False
        elif ch == '"':
            in_str = True
            cur += ch
        elif ch in "([":
            depth += 1
            cur += ch
        elif ch in ")]":
            depth -= 1
            cur += ch
        elif ch == sep and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
        i += 1
    parts.append(cur)
    return parts


def parse_operands(s):
    ops = []
    for part in split_top(s, ","):
        part = part.strip()
        if not part:
            continue
        m = re.match(r'"([^"]*)"\s*\((.*)\)\s*$', part, re.S)
        if not m:
            raise Unsupported("operand %r" % part)
        ops.append((m.group(1), m.group(2).strip()))
    return ops


def translate_asm(asm):
    """asm: the text inside __asm__ volatile( ... ). Returns a C statement."""
    sections = split_top(asm, ":")
    template = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', sections[0]))
    outputs = parse_operands(sections[1]) if len(sections) > 1 else []
    inputs = parse_operands(sections[2]) if len(sections) > 2 else []
    operands = outputs + inputs
    # Inputs the asm writes to (used as scratch, e.g. gpu.h gte_LoadVector0_XYZ): copied into locals,
    # leaving the caller's expression untouched, as the C code around the asm assumes.
    scratch_inputs = {}

    def opnd(tok, as_lvalue=False):
        tok = tok.strip()
        m = re.fullmatch(r"%(\d+)", tok)
        if m:
            k = int(m.group(1))
            constraint, expr = operands[k]
            if k in scratch_inputs:
                return scratch_inputs[k]
            if as_lvalue and not constraint.startswith("="):
                scratch_inputs[k] = "_in%d" % k
                return scratch_inputs[k]
            if not as_lvalue and constraint.startswith("="):
                raise Unsupported("output operand %s read before written" % tok)
            return "(%s)" % expr
        m = re.fullmatch(r"\$(\d+|zero)", tok)
        if m:
            n = m.group(1)
            if n in ("0", "zero"):
                if as_lvalue:
                    raise Unsupported("write to $zero")
                return "0u"
            if not 12 <= int(n) <= 15:
                raise Unsupported("CPU register %s" % tok)
            return "_r%s" % n
        raise Unsupported("operand %r" % tok)

    def mem(tok):
        m = re.fullmatch(r"(-?\w+)?\s*\(\s*(%\d+|\$\d+)\s*\)", tok.strip())
        if not m:
            raise Unsupported("memory operand %r" % tok)
        off = m.group(1) or "0"
        return "((unsigned char*)(%s) + (%s))" % (opnd(m.group(2)), off)

    def gte_reg(tok):
        m = re.fullmatch(r"\$(\d+)", tok.strip())
        if not m or not 0 <= int(m.group(1)) <= 31:
            raise Unsupported("GTE register %r" % tok)
        return m.group(1)

    out = []
    for ins in re.split(r";|\\n|\n", template):
        ins = re.sub(r"/\*.*?\*/", "", ins).strip()
        if not ins or ins == "nop":
            continue
        op, _, rest = ins.partition(" ")
        if "\t" in op:
            op, _, rest2 = ins.partition("\t")
            rest = rest2
        args = [a.strip() for a in split_top(rest.strip(), ",")] if rest.strip() else []
        if op == "lwc2":
            out.append("Gte_DataWrite(%s, *(unsigned int*)%s);" % (gte_reg(args[0]), mem(args[1])))
        elif op == "swc2":
            out.append("*(unsigned int*)%s = Gte_DataRead(%s);" % (mem(args[1]), gte_reg(args[0])))
        elif op in ("mtc2", "ctc2"):
            fn = "Gte_DataWrite" if op == "mtc2" else "Gte_CtrlWrite"
            out.append("%s(%s, (unsigned int)%s);" % (fn, gte_reg(args[1]), opnd(args[0])))
        elif op in ("mfc2", "cfc2"):
            fn = "Gte_DataRead" if op == "mfc2" else "Gte_CtrlRead"
            out.append("%s = %s(%s);" % (opnd(args[0], True), fn, gte_reg(args[1])))
        elif op == ".word":
            w = args[0]
            if not re.fullmatch(r"0x[0-9A-Fa-f]+", w) or (int(w, 16) & 0xFE000000) != 0x4A000000:
                raise Unsupported("non-COP2 word %s (DMPSX placeholder?)" % w)
            out.append("Gte_Command(0x%07X);" % (int(w, 16) & 0x1FFFFFF))
        elif op in ("lw", "lh", "lhu", "lbu"):
            t = {"lw": "unsigned int", "lh": "short", "lhu": "unsigned short", "lbu": "unsigned char"}[op]
            out.append("%s = (unsigned int)*(%s*)%s;" % (opnd(args[0], True), t, mem(args[1])))
        elif op in ("sw", "sh", "sb"):
            t = {"sw": "unsigned int", "sh": "unsigned short", "sb": "unsigned char"}[op]
            out.append("*(%s*)%s = (%s)%s;" % (t, mem(args[1]), t, opnd(args[0])))
        elif op in ("sll", "srl"):
            sym = "<<" if op == "sll" else ">>"
            out.append("%s = %s %s %d;" % (opnd(args[0], True), opnd(args[1]), sym, int(args[2], 0)))
        elif op == "sra":
            out.append("%s = (unsigned int)((int)%s >> %d);" % (opnd(args[0], True), opnd(args[1]), int(args[2], 0)))
        elif op in ("or", "and", "addu", "subu"):
            sym = {"or": "|", "and": "&", "addu": "+", "subu": "-"}[op]
            out.append("%s = %s %s %s;" % (opnd(args[0], True), opnd(args[1]), sym, opnd(args[2])))
        elif op == "addi":
            out.append("%s = %s + (unsigned int)(%d);" % (opnd(args[0], True), opnd(args[1]), int(args[2], 0)))
        elif op == "negu":
            out.append("%s = 0u - %s;" % (opnd(args[0], True), opnd(args[1])))
        elif op == "move":
            out.append("%s = %s;" % (opnd(args[0], True), opnd(args[1])))
        else:
            raise Unsupported("instruction %r" % ins)

    used = sorted(set(re.findall(r"_r(1[2-5])\b", " ".join(out))))
    decl = "unsigned int %s; " % ", ".join("_r" + r for r in used) if used else ""
    for k, name in sorted(scratch_inputs.items()):
        decl += "unsigned int %s = (unsigned int)(%s); " % (name, operands[k][1])
    return "do { %s%s } while (0)" % (decl, " ".join(out))


def translate_body(body):
    """Replace every __asm__ [volatile] ( ... ) in a macro body with its C translation."""
    out, i = "", 0
    for m in re.finditer(r"__asm__\s*(?:volatile|__volatile__)?\s*\(", body):
        if m.start() < i:
            continue
        start = m.end() - 1
        end = balanced(body, start)
        out += body[i:m.start()] + translate_asm(body[start + 1:end - 1])
        i = end
    return out + body[i:]


def macros(text):
    """Yields (name, params, body) for every #define gte_* with a (possibly continued) body."""
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        m = re.match(r"\s*#\s*define\s+(gte_\w+)\s*(\([^)]*\))?(.*)", lines[i])
        if not m:
            i += 1
            continue
        body = m.group(3)
        while body.rstrip().endswith("\\"):
            body = body.rstrip()[:-1] + "\n"
            i += 1
            body += lines[i]
        yield m.group(1), m.group(2) or "", body
        i += 1


def main():
    failed = 0
    for src, dst in SOURCES.items():
        text = open(os.path.join(ROOT, src)).read()
        text = re.sub(r"/\*.*?\*/", lambda m: " " * len(m.group(0)) if "\n" not in m.group(0) else "\n" * m.group(0).count("\n"), text, flags=re.S)
        guard = "_PORT_GTE_FROM_%s" % re.sub(r"\W", "_", os.path.basename(src)).upper()
        lines = ["/* Generated by tools/port/gen_gte_inline.py from %s. Do not edit. */" % src,
                 "#ifndef %s" % guard, "#define %s" % guard, "", '#include "port/gte.h"', ""]
        n = 0
        for name, params, body in macros(text):
            if "__asm__" not in body:
                continue
            try:
                c = translate_body(body)
            except Unsupported as e:
                print("%s: %s: %s" % (src, name, e), file=sys.stderr)
                failed += 1
                lines += ["#undef %s" % name, "#define %s%s __gte_untranslatable_%s()" % (name, params, name), ""]
                continue
            c = " ".join(l.strip() for l in c.split("\n") if l.strip())
            lines += ["#undef %s" % name, "#define %s%s %s" % (name, params, c), ""]
            n += 1
        lines += ["#endif", ""]
        open(os.path.join(ROOT, dst), "w").write("\n".join(lines))
        print("%s: %d macros -> %s" % (src, n, dst))
    if failed:
        print("%d macros could not be translated (they expand to a call to an undefined function)" % failed)


if __name__ == "__main__":
    main()
