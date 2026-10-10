#!/usr/bin/env python3
"""The in-game cutscenes, found in the decomp's map code, for the port's Demo menu
(src/port/demo_menu.c).

A cutscene is a map event (s_EventData in src/maps/<map>/<map>_events_data.c with sysState
SysState_EventCallback) whose function, from the map's g_MapEventFuncs table, uses cutscene
machinery: letterbox borders, the cutscene timer or flag, or camera/animation data (DMS). Functions
still in assembly are checked by the functions they call. FMVs (SysState_Fmv) are separate events
and aren't listed, nor are later parts of a cutscene (events started by another cutscene event's
completion flag, or that depend on what an earlier event set up: DMS data they read but don't load,
a character they spawn but don't load). When such a later part's required flag is set by another
event on the map that starts by itself, that event is listed instead (map4_s01 func_800D1FF0, which
loads Cybil for func_800D2408).

    python3 tools/port/cutscene_list.py --report          # list with each event's first lines of dialogue
    python3 tools/port/cutscene_list.py --c OUT.c         # the table compiled into the port
"""
import argparse
import glob
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

MAPS = ["map0_s00", "map0_s01", "map0_s02",
        "map1_s00", "map1_s01", "map1_s02", "map1_s03", "map1_s04", "map1_s05", "map1_s06",
        "map2_s00", "map2_s01", "map2_s02", "map2_s03", "map2_s04",
        "map3_s00", "map3_s01", "map3_s02", "map3_s03", "map3_s04", "map3_s05", "map3_s06",
        "map4_s00", "map4_s01", "map4_s02", "map4_s03", "map4_s04", "map4_s05", "map4_s06",
        "map5_s00", "map5_s01", "map5_s02", "map5_s03",
        "map6_s00", "map6_s01", "map6_s02", "map6_s03", "map6_s04", "map6_s05",
        "map7_s00", "map7_s01", "map7_s02", "map7_s03"]  # VIN/MAPn_Snn.BIN order (MapIdx)

C_MARKERS = re.compile(r"SysFlag_CutsceneActive|CutsceneBorder|Event_CutsceneTimerAdvance|g_Cutscene_|Dms")
ASM_MARKERS = re.compile(r"\bjal\s+\w*(?:Cutscene|Dms)\w*|%(?:hi|lo)\(\w*(?:Cutscene|Dms)\w*\)")


def read(path):
    return open(path, errors="replace").read()


def c_body(name, files):
    for f in files:
        s = read(f)
        m = re.search(r"\n[^\n;]*\b" + re.escape(name) + r"\(void\)[^\n;]*\n\{", s)
        if m:
            i, depth = m.end(), 1
            while depth and i < len(s):
                depth += {"{": 1, "}": -1}.get(s[i], 0)
                i += 1
            return s[m.end():i]
    return None


def asm_body(name, mp):
    for f in glob.glob(os.path.join(REPO, "asm", "USA", "maps", mp, "**", name + ".s"), recursive=True):
        return read(f)
    return None


def events(mp):
    """[(index, fields)] from the map's MAP_EVENTS."""
    s = read(os.path.join(REPO, "src", "maps", mp, mp + "_events_data.c"))
    out = []
    for m in re.finditer(r"// \[(\d+)\]\s*\{(.*?)\n    \}", s, re.S):
        fields = dict(re.findall(r"\.(\w+)\s*=\s*([^,\n]+),", m.group(2)))
        out.append((int(m.group(1)), fields))
    return out


def resumes(body):
    """A part whose cutscene timer starts past 0 goes on from an earlier part (whose flag may be set by
    code still in assembly, which the flag check above can't see)."""
    m = re.search(r"g_Cutscene_Timer\s*=\s*Q12\(([\d.]+)f?\)", body)
    return bool(m) and float(m.group(1)) > 0


_MAP_FUNCS = {}


def map_functions(mp):
    """name -> body of every C function in the map's files."""
    if mp not in _MAP_FUNCS:
        out = {}
        for f in glob.glob(os.path.join(REPO, "src", "maps", mp, "*.c")):
            s = read(f)
            for m in re.finditer(r"\n[A-Za-z_][\w \t\*]*?\b(\w+)\([^;{]*\)[^;{\n]*\n\{", s):
                i, depth = m.end(), 1
                while depth and i < len(s):
                    depth += {"{": 1, "}": -1}.get(s[i], 0)
                    i += 1
                out.setdefault(m.group(1), s[m.end():i])
        _MAP_FUNCS[mp] = out
    return _MAP_FUNCS[mp]


def needs_setup(body, mp):
    """C code that relies on an earlier event: it reads DMS tracks without loading a DMS file (also
    in the map's helper functions it calls), or spawns a character that only another event loads
    (the map's own characters: its charaGroupIds, loaded with it, and those its other code loads,
    e.g. map0_s01's Map_WorldObjectsInit)."""
    fns = map_functions(mp)
    event_fns = set(funcs(mp))
    whole, seen, todo = body, set(), [body]
    for _ in range(3):  # helpers of the map it calls, three levels deep
        calls = {c for b in todo for c in re.findall(r"\b(\w+)\(", b) if c in fns and c not in event_fns and c not in seen}
        seen |= calls
        todo = [fns[c] for c in calls]
        whole += "".join(todo)
    # DMS buffers it reads (Dms_*(..., FS_BUFFER_n)) but never loads a DMS file into: map7_s03
    # func_800E3B6C reads FS_BUFFER_20, which func_800E3390 loaded.
    used = set(re.findall(r"\bDms_\w+\([^;]*?\bFS_BUFFER_(\d+)", whole))
    loaded = set(re.findall(r"Fs_QueueStartRead\(FILE_\w*_DMS,\s*(?:\([^)]*\))?\s*FS_BUFFER_(\d+)", whole))
    if used - loaded:
        return "DMS data in FS_BUFFER_%s, which it doesn't load" % ", ".join(sorted(used - loaded))
    header = read(os.path.join(REPO, "src", "maps", mp, mp + "_header.c"))
    m = re.search(r"\.charaGroupIds\s*=\s*\{([^}]*)\}", header)
    own = set(re.findall(r"Chara_\w+", m.group(1))) if m else set()  # loaded with the map
    for name, b in fns.items():
        if name not in event_fns:
            own |= set(re.findall(r"Chara_Load\([^,]*,\s*(Chara_\w+)", b))
    loaded = set(re.findall(r"Chara_Load\([^,]*,\s*(Chara_\w+)", whole))
    for chara in re.findall(r"Chara_Spawn\((Chara_\w+)", whole):
        if chara not in loaded and chara not in own:
            return "%s, which only another event loads" % chara
    return None


def flag_setters(mp, files):
    """flag -> [(event, function)] of the map's event-callback events that set it (their
    completion flag, or a flag their code sets), and only those that need no flag to start."""
    setters = {}
    table = funcs(mp)
    for ev, fields in events(mp):
        if fields.get("sysState") != "SysState_EventCallback" or fields.get("requiredEventFlag") not in (None, "EventFlag_None"):
            continue
        try:
            name = table[int(fields.get("eventParam", "0"), 0)]
        except (ValueError, IndexError):
            continue
        body = c_body(name, files) or ""
        flags = {fields.get("completeEventFlag")} | set(re.findall(r"Savegame_EventFlagSet\w*\((EventFlag_\w+)\)", body))
        for f in flags:
            setters.setdefault(f, []).append((ev, name))
    return setters


def funcs(mp):
    s = read(os.path.join(REPO, "src", "maps", mp, mp + "_header.c"))
    m = re.search(r"g_MapEventFuncs\[\]\)\(\) = \{(.*?)\};", s, re.S)
    return re.findall(r"([A-Za-z_][A-Za-z_0-9]*)\s*,", m.group(1)) if m else []


EXCLUDED = []  # (cutscene, reason): later parts left out by needs_setup

# Later parts the rules above don't see, found by playing the list on the PS2 (2026-10-10).
KNOWN_PARTS = {
    ("map6_s04", "func_800E2950"): "waits on monster Cybil's animation in npcs[0], spawned by an earlier part",
    ("map6_s04", "func_800E3244"): "its parasite has no bone coordinates without the earlier parts",
}


def cutscenes():
    found = []
    shared = glob.glob(os.path.join(REPO, "src", "maps", "shared", "**", "*.h"), recursive=True)
    for idx, mp in enumerate(MAPS):
        table = funcs(mp)
        files = glob.glob(os.path.join(REPO, "src", "maps", mp, "*.c")) + shared
        for ev, fields in events(mp):
            if fields.get("sysState") != "SysState_EventCallback":
                continue
            try:
                param = int(fields.get("eventParam", "0"), 0)
                name = table[param]
            except (ValueError, IndexError):
                continue
            body = c_body(name, files)
            if body is not None:
                hit = C_MARKERS.search(body)
            else:
                body = asm_body(name, mp) or ""
                hit = ASM_MARKERS.search(body)
            if hit:
                found.append({"map": mp, "mapIdx": idx, "event": ev, "func": name, "body": body,
                              "required": fields.get("requiredEventFlag"), "complete": fields.get("completeEventFlag")})
    # Continuations: an event that needs the flag another cutscene event on its map sets when it ends is
    # the next part of that cutscene (MapEvent_CutsceneCybilDeath goes on from func_800E2950 at 96 s,
    # with the DMS data the first part loaded). Only the first part is listed; the rest follow by itself.
    # The flags that end a part: its completion flag, flags its code sets, and the completion flags
    # of FMV events that a part starts (an FMV can sit between two parts).
    done = {(c["map"], c["complete"]) for c in found if c["complete"] not in (None, "EventFlag_None")}
    for c in found:
        done |= {(c["map"], f) for f in re.findall(r"Savegame_EventFlagSet\w*\((EventFlag_\w+)\)", c["body"])}
    for mp in MAPS:  # an FMV between two parts: one started by a part's flag
        done |= {(mp, f.get("completeEventFlag")) for _, f in events(mp)
                 if f.get("sysState") == "SysState_Fmv" and (mp, f.get("requiredEventFlag")) in done}
    found = [c for c in found if (c["map"], c["required"]) not in done and not resumes(c["body"])]
    # Parts that rely on an earlier event's setup (C code only): left out, and the event that sets
    # their required flag listed instead, if it starts by itself and needs no setup either.
    kept = []
    for c in found:
        why = KNOWN_PARTS.get((c["map"], c["func"])) or \
            (None if "glabel" in c["body"] else needs_setup(c["body"], c["map"]))
        if not why:
            kept.append(c)
            continue
        EXCLUDED.append((c, why))
        files = glob.glob(os.path.join(REPO, "src", "maps", c["map"], "*.c")) + \
            glob.glob(os.path.join(REPO, "src", "maps", "shared", "**", "*.h"), recursive=True)
        setters = flag_setters(c["map"], files) if c["required"] not in (None, "EventFlag_None") else {}
        for ev, name in setters.get(c["required"], []):
            body = c_body(name, files) or ""
            if name != c["func"] and body and not needs_setup(body, c["map"]):
                kept.append({"map": c["map"], "mapIdx": c["mapIdx"], "event": ev, "func": name, "body": body,
                             "required": None, "complete": None})
                break
    found = kept
    # One entry per function and map (several events can start the same cutscene).
    seen, out = set(), []
    for c in found:
        if (c["map"], c["func"]) not in seen:
            seen.add((c["map"], c["func"]))
            out.append(c)
    return sorted(out, key=lambda c: (c["func"].lower(), c["map"]))


def label(c):
    return c["func"] if not c["func"].startswith("func_") else "%s %s" % (c["map"], c["func"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--c", metavar="OUT")
    args = ap.parse_args()
    cs = cutscenes()
    if args.report:
        for c in cs:
            lines = re.findall(r'//\s*(".*?")', c["body"])[:3]
            print("%-9s ev %2d  %-44s %s" % (c["map"], c["event"], c["func"], " / ".join(lines)))
        print("%d cutscenes" % len(cs))
        for c, why in EXCLUDED:
            print("left out: %-9s ev %2d %-28s (%s)" % (c["map"], c["event"], c["func"], why))
    if args.c:
        with open(args.c, "w") as f:
            f.write("/* Generated by tools/port/cutscene_list.py: the Demo menu's cutscenes (src/port/demo_menu.c). */\n")
            f.write("#include \"port/demo_menu.h\"\n\nconst PortCutscene g_PortCutscenes[] = {\n")
            for c in cs:
                f.write('    { "%s", %d, %d },\n' % (label(c), c["mapIdx"], c["event"]))
            f.write("};\nconst int g_PortCutsceneCount = %d;\n" % len(cs))


if __name__ == "__main__":
    main()
