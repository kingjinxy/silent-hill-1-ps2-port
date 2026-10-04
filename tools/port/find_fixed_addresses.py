#!/usr/bin/env python3
"""Report raw PS1 RAM/scratchpad address literals in C sources.

Fixed addresses must go through the macros in `include/decomp/psx_mem.h` (`PSX_RAM_ADDR`,
`PSX_SCRATCH`, ...) so the port build can redirect them. This script flags any literal in the
PS1 main RAM range (0x80010000-0x801FFFFF) or scratchpad range (0x1F800000-0x1F8003FF) that is
not wrapped in `PSX_RAM_ADDR(...)` and is not inside a comment.

Exit status is 1 if anything is reported, so it can be used as a check.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCAN_DIRS = ["src", "include"]
SKIP_DIRS = ["include/psyq"]

# Known literals that are values, not addresses, or are handled by a later step.
# Format: (path relative to repo root, literal).
ALLOWLIST = {
    # The abstraction itself.
    ("include/decomp/psx_mem.h", "0x1F800000"),
    # Overlay load addresses; removed by Step 2 (static linking of overlays).
    ("src/main/main.c", "0x800CBAA8"),
    ("src/main/main.c", "0x800CBBD0"),
    ("src/main/main.c", "0x800C9578"),
    ("src/main/main.c", "0x80024B60"),
    # GPU/colour data words, not addresses.
    ("src/bodyprog/bodyprog_80089090.c", "0x80008080"),
    ("src/bodyprog/text/bodyprog_8003652C.c", "0x80048084"),
}

LITERAL_RE = re.compile(r"(?<![\w])0x([0-9A-Fa-f]{8})\b")
WRAPPED_RE = re.compile(r"PSX_RAM_ADDR\(\s*0x[0-9A-Fa-f]+\s*\)")


def strip_comments(text):
    """Replace comments with spaces, keeping line structure."""
    def repl(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    return re.sub(r"//[^\n]*|/\*.*?\*/", repl, text, flags=re.S)


def is_fixed_address(value):
    return 0x80010000 <= value <= 0x801FFFFF or 0x1F800000 <= value <= 0x1F8003FF


def scan_file(path):
    rel = path.relative_to(ROOT).as_posix()
    text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    hits = []
    for lineno, line in enumerate(text.splitlines(), 1):
        line = WRAPPED_RE.sub("", line)
        for m in LITERAL_RE.finditer(line):
            literal = "0x" + m.group(1).upper()
            if not is_fixed_address(int(m.group(1), 16)):
                continue
            if (rel, literal) in ALLOWLIST:
                continue
            hits.append((rel, lineno, literal, line.strip()))
    return hits


def main():
    hits = []
    for d in SCAN_DIRS:
        for path in sorted((ROOT / d).rglob("*")):
            if path.suffix not in (".c", ".h", ".inc"):
                continue
            rel = path.relative_to(ROOT).as_posix()
            if any(rel.startswith(s + "/") for s in SKIP_DIRS):
                continue
            hits.extend(scan_file(path))

    for rel, lineno, literal, line in hits:
        print(f"{rel}:{lineno}: {literal}: {line}")

    if hits:
        print(f"\n{len(hits)} raw fixed address literal(s) found; wrap them in PSX_RAM_ADDR()/PSX_SCRATCH.")
        return 1

    print("No raw fixed address literals found.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
