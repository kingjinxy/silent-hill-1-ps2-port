# Running Walkthrough

A chronological log of changes made for the PS2 port: what changed, why, and how it was verified.
The checklist itself lives in `Planning Doc.md`.

---

## 2026-10-03 — Step 1: Fixed addresses

### Build environment

The decomp's supported build path is its Docker image. Setup on this machine (Windows 10):

1. Initialised the git submodules (`tools/maspsx`, `tools/m2c`, `tools/asm-differ`,
   `tools/decomp-permuter`); `maspsx` is required for building.
2. Converted the disc image, since `dumpsxiso` reads BIN, not CHD:
   ```
   chdman extractcd -i "rom/image/Silent Hill (USA).chd" -o rom/image/SLUS-00707.cue -ob rom/image/SLUS-00707.bin
   ```
   (single track, `MODE2/2352`). The Makefile expects `rom/image/<GAME_NAME>.bin`, i.e.
   `SLUS-00707.bin`. `rom/` is gitignored.
3. Started Docker Desktop and built the image: `docker build --platform linux/amd64 -t sh-decomp-buildenv .`
4. Ran setup and the matching build inside the container (from Git Bash, `MSYS_NO_PATHCONV=1`
   stops MSYS from rewriting `/app`):
   ```
   MSYS_NO_PATHCONV=1 docker run --platform linux/amd64 --rm -v "$(pwd -W)":/app -w /app \
       sh-decomp-buildenv bash -c 'make setup && make -j8'
   ```
   `make setup` extracts the disc into `rom/USA/`, unpacks `SILENT.`/`HILL.` into `assets/`,
   and runs splat to generate `asm/` and `linkers/`. `make` then builds and checks SHA-256
   against `configs/USA/checksum.sha`.

### The problem

The game uses an absolute memory map. Besides code and data placed by the linker, it puts load
buffers, ordering tables and sound buffers at hardcoded RAM addresses (e.g.
`FS_BUFFER_0 = 0x8010A600`, the FS heap at `0x1C0000`), and uses the PS1 scratchpad at
`0x1F800000` directly. On PS2 neither of those addresses is ours to use: ps2sdk programs live
in KUSEG around `0x00100000`, and the EE scratchpad is at `0x70000000`.

### The approach

One header, `include/decomp/psx_mem.h` (included from `common.h`), owns every fixed address:

| Macro | PS1 matching build | Port build (`SH_PORT`) |
|---|---|---|
| `PSX_RAM_ADDR(addr)` | `((void*)(addr))` — identical literal | `&g_PsxRam[addr & 0x1FFFFF]` |
| `PSX_SCRATCH` | `((void*)0x1F800000)` | `g_PsxScratch` |
| `PSX_SCRATCH_ADDR(off)` | unchanged definition, now built on `PSX_SCRATCH` | same |
| `PSX_ADDR_CANON(ptr)` | `(u32)(ptr) & 0xFFFFFF` | `(u32)(ptr)` |

`g_PsxRam` (2 MiB) and `g_PsxScratch` (1 KiB) are defined in `src/port/psx_mem.c`, which only the
future port build compiles (the Makefile only searches each target's own source directory).

Why an arena instead of turning each buffer into its own array: the game's buffers deliberately
overlap and sub-allocate each other (e.g. `FS_BUFFER_12`/`FS_BUFFER_4` are sub-buffers;
`HELD_ITEM_LM_BUFFER` is `HARRY_LM_BUFFER + file size`; `g_OrderingTable1` uses `FS_BUFFER_1`
and `0x801E2E00` adjacent to it). Keeping the PS1 layout inside one arena keeps all of those
relationships intact without having to understand every one of them first. Masking with
`0x1FFFFF` also folds the KUSEG/KSEG0/KSEG1 mirrors, so `0x1C0000` and `0x801C0000` land on the
same byte, as they do on hardware.

Why `PSX_ADDR_CANON`: `Fs_QueueDoBuffersOverlap` compared `(u32)ptr & 0xFFFFFF` so that mirrored
addresses compare equal. On the host there are no mirrors, and a 24-bit mask would wrongly alias
two pointers 16 MiB apart, so the port build compares full pointers.

### Changes

- `include/decomp/psx_mem.h` — new; macros above.
- `include/decomp/common.h` — `PSX_SCRATCH`/`PSX_SCRATCH_ADDR` moved to `psx_mem.h`, which it now includes.
- `include/main/fsqueue.h` — all 50 `FS_BUFFER_*`, `IMAGE_BUFFER_*`, `*_LM_BUFFER`, `IPD_BUFFER`,
  `TEMP_MEMORY_ADDR`, `CD_ADDR_0`, `FONT24_BUFFER`, `MAP_CHARA_BASE` defines wrapped in `PSX_RAM_ADDR`.
- `include/main/fsmem.h` — `FS_MEM_BASE` is now `PSX_RAM_ADDR(0x1C0000)` (was a bare integer passed as `u8*`).
- `include/bodyprog/demo.h` — `g_Demo_ActiveState`.
- `include/bodyprog/memcard.h` — `SAVEGAME_ENTRY_BUFFER_0/1`.
- `src/bodyprog/credits_init.c` — JP-only width/color table pointers.
- `src/bodyprog/sound/sound_data.c` — `g_Sd_VabBuffers`, `g_Sd_KdtBuffer`.
- `src/bodyprog/screen/screen_data.c` — second ordering table at `0x801E2E00`.
- `src/bodyprog/demo.c` — `g_Demo_PlaybackFrames`.
- `src/bodyprog/events/title.c` — raw scratchpad writes now use `PSX_SCRATCH_ADDR`/`PSX_SCRATCH`;
  `func_8003B7BC` buffer pointer.
- `src/bodyprog/text/text_draw_jp.c` — JP font upload source.
- `src/maps/map3_s03/map3_s03.c`, `src/maps/map5_s00/map5_s00.c` — map-local buffer addresses.
- `src/screens/b_konami/b_konami.c` — LZSS/decrypt work buffers.
- `src/main/fsqueue.c` — `Fs_QueueDoBuffersOverlap` uses `PSX_ADDR_CANON`.
- `src/port/psx_mem.c` — new; arena definitions (port build only).

Intentionally not changed yet: `g_OvlBodyprog`/`g_OvlDynamic` in `src/main/main.c`. They are
overlay load addresses and go away entirely in Step 2 (static linking).

### Verification

- `make setup && make -j8` (USA) built with the changes above applied: all 51 outputs
  (`SLUS_007.07`, `BODYPROG.BIN`, `B_KONAMI.BIN`, the four screen overlays and all 43 map overlays)
  pass the SHA-256 checksum, so the change is byte-identical on PS1.
- `python tools/port/find_fixed_addresses.py` reports no raw address literals (allowlist: the
  macro definition, the Step 2 overlay addresses, and two data words that aren't addresses).
- Not yet verified: the `SH_PORT` expansion has not been compiled, since no port build exists yet.

### Test image

`make insert-ovl` writes the built executable/overlays into `SILENT.` and runs `mkpsxiso`; the
image appears as `build/SLUS_007.07.bin/.cue` (moved to `build/USA/out/`). It is not byte-identical
to the original dump (2 filesystem sectors, plus 34 sectors inside `HILL.` from the
`dumpsxiso`→`mkpsxiso` round trip), but all game code is. Tested by the user: the PS1 build appears to still work.

### Audit: raw pointers in data asm

Data still in splat-generated asm can contain pointers that splat emitted as plain numbers
(`.word 0x800CA8D0`) instead of symbols. Those would keep pointing at the old address if anything
moved. Scanning only the data `.s` files that are actually linked (193, from `build/USA/asm/**/data`):

| File | Raw words | What they are |
|---|---|---|
| `maps/map4_s00/data/365C.data.s` | 15 | `MAP_MESSAGES` string pointers |
| `maps/map4_s06/data/363C.data.s` | 15 | `MAP_MESSAGES` string pointers |
| `maps/map6_s05/data/3744.data.s` | 15 | `MAP_MESSAGES` string pointers |
| `maps/map7_s03/data/23CA0.data.s` | 56 | tables into its own rodata; 2 fixed buffers (`0x80180600`, `0x80185600`) |
| `bodyprog/data/3EB8.rodata.s` | 1 | `D_80028A18` = `0x80052F00`, a bodyprog code address |

### Audit: fixed buffers vs. linked sections

Load ranges from the linker map symbols (`*_VRAM`, `*_END`):

| Binary | Range |
|---|---|
| `SLUS_007.07` | `80010000`– |
| `BODYPROG.BIN` | `80024B60`– |
| 44 map overlays + `B_KONAMI.BIN` | `800C9578`–max `800F5978` (MAP2_S00) |
| STREAM / SAVELOAD / OPTION / STF_ROLL | `801E2600`–max `801F5100` |

The lowest fixed buffer is `g_Demo_PlaybackFrames` at `0x800F5E00`, so map overlays never overlap
a buffer. The screen overlays are deliberately loaded over the `0x801E2600` buffer region
(the same address as `FS_BUFFER_1`/`CD_ADDR_0`); the game doesn't use those buffers while a
screen overlay is active. In the port the overlay code lives outside the arena, so that
sharing just goes away. Nothing to fix.

---

## 2026-10-04 — Build environment moved to a Debian VM

Docker on Windows was slow, so the build now runs natively on a Debian 13 VM (8 cores, 16 GB RAM,
80 GB disk), without Docker.

1. System packages (the same ones the Dockerfile installs, plus `chdman`):
   ```
   sudo apt install make gcc binutils-mips-linux-gnu cpp-mips-linux-gnu bchunk mame-tools python3-venv
   ```
2. Python dependencies in a venv (`.venv/` is gitignored), since Debian blocks system-wide pip.
   `requirements.txt` installs cleanly on Python 3.13:
   ```
   python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
   source .venv/bin/activate     # before every make
   ```
3. `git submodule update --init`
4. ROM: `chdman extractcd` as before, producing `rom/image/SLUS-00707.bin/.cue`.
5. `make setup && make -j8 build`.

Note `make -j8` with no target no longer builds anything on this machine (it treats a generated
`.i` file as the default goal); use `make -j8 build` explicitly.

Disk: the original 18 GB root filesystem filled up mid-build. The disk was grown to 80 GB; a 1 GB
swap partition sat between `/` and the new space, so it was replaced with a 4 GB `/swapfile`
(`/etc/fstab` updated, `RESUME=none` in `/etc/initramfs-tools/conf.d/resume`) and `/` was grown
with `parted resizepart` + `resize2fs`.

Verification: with the Step 1 changes, a clean `make setup && make -j8 build` takes about 20 s and
all 50 checksum entries are OK (the "51 outputs" above was a miscount; `configs/USA/checksum.sha`
lists 50). `tools/port/find_fixed_addresses.py` reports no raw address literals.

---

## 2026-10-04 — Step 1: symbolising raw pointer words in data asm

The `MAP_MESSAGES` tables (map4_s00, map4_s06, map6_s05) were already migrated to C in the
Windows session (commit `86dea3a72`). This entry covers the rest.

### map7_s03

`23CA0.data.s` holds tables of `s_800ED7E0_ptr*` (`D_800ED7E0`, `D_800ED8B0`, `D_800ED8EC`, and
more further down) whose entries point at elements of struct arrays in map7_s03's rodata. splat
only emits a symbol when a word matches a known label exactly, and most targets were in the
*middle* of `INCLUDE_RODATA` blobs (`D_800CC348`, `D_800CC63C`, `D_800CCD20`, `D_800CCE80`),
so it wrote them as numbers.

Fix, without changing any bytes:

- `configs/USA/maps/sym.map7_s03.txt`: new `.rodata` section naming each target
  (`g_rodata_800CC320`, `D_800CC35C` … `D_800CD09C`, 23 symbols). splat now splits the blobs at
  those addresses and the data tables reference them by name.
  - `D_800CC348` had to be named explicitly: once `0x800CC320` (already a C array,
    `g_rodata_800CC320[40]`) was a symbol, splat merged the auto-named `D_800CC348` into it.
    The struct at `0x800CC320` really spans both (60 bytes); `D_800CC348` is its tail.
  - Custom `type:` annotations aren't accepted (spimdisasm wants a known type or a capitalised
    name), so the symbols are untyped.
- `src/maps/map7_s03/map7_s03_3.c`: an extra `INCLUDE_RODATA` after each split blob, in address
  order, so the assembled rodata is contiguous and identical.
- The two fixed-buffer words in `D_800ED230` (`0x80185600`, `0x80180600`, used as
  `s_DmsHeader*` for cutscene data) are now symbols `g_FsBuffer20`/`g_FsBuffer18`. They're
  outside every segment, so splat writes them to `linkers/USA/maps/undefined_syms_auto.map7_s03.txt`
  as absolute addresses. The port's linker script can define them as `g_PsxRam + 0x185600` etc.,
  matching `PSX_RAM_ADDR`.

### bodyprog `D_80028A18`

`0x80052F00` is in bodyprog's text range, but the yaml already marks `0x3EB8` as
"Garbage padding" and nothing references it. Left as is.

### Re-audit

All 190 linked data `.s` files (from `linkers/`): the only `0x80xxxxxx` words left are
`LOADABLE_INVENTORY_ITEMS` (`0x80222120` = item IDs `20 21 22 80`) in each map, RGBA colours
(`0x80808080`, `0x80FF8080`) and the padding word above. None are pointers.

### Verification

`make setup && make -j8 build`: all 50 checksums OK.

### Build gotcha

maspsx reads its input from stdin unless stdin is a TTY or empty. When `make` runs with stdin
attached to an open pipe/socket (e.g. a backgrounded job), it blocks forever on the first `.c.s`.
Run `make ... < /dev/null` in that case.

---

## 2026-10-04 — Step 1: shift test

### What was shifted

`tools/port/shift_test.py` (branch `shift-test` only) inserts `. += 0x110` into every map
overlay's linker script right after `<map>_header.c.o(.rodata)`. The header stays at the overlay
base (bodyprog reads it there); everything after it (rest of rodata, text, data, bss) moves.
Largest map then ends at `0x800F5A88`, still below the first fixed buffer (`0x800F5E00`).

main and bodyprog were not shifted: every other binary refers to their symbols by absolute address
(`undefined_syms_auto` from the `sym.*.txt` files), so moving them breaks callers by construction.
That can only be tested after Step 2.

Build: `make setup`, `python3 tools/port/shift_test.py`, delete `build/USA/out/VIN/MAP*` (the
Makefile doesn't depend on the linker scripts), `make build CHECKSUM=0`, `make insert-ovl`.

### insertovl.py bug

The first shifted image hung after the KCET logo. `tools/silentassets/insertovl.py` mis-laid-out
`SILENT.` whenever an overlay grew: block counts were rounded down (every map lost its last
16 bytes), and position shifts were only carried to the next resized overlay, so STREAM, OPTION,
SAVELOAD and STF_ROLL (after the maps) pointed at the wrong data — STREAM plays the intro movie.
Rewrote the layout as one sequential pass: each file keeps its original span unless a rebuilt
overlay outgrows it; later files (and all `HILL.` entries) move by the accumulated growth.
Verified every `SILENT.` file at its new position, no overlaps, and `HILL.` at the LBA the new
table expects (`0x99CC`, read from the image's ISO directory). Fix is on master.

### DuckStation + gdb

- DuckStation: Settings → Advanced → GDB server (port 2345).
- `tools/port/gdb/sh1.py` loads main/bodyprog symbols and adds:
  - `warp <map>`: breakpoint on `GameBoot_MapLoad` (`0x8003521C`) rewrites `$a0` and
    `g_SavegamePtr->mapIdx` for the next map load, and loads that map's ELF symbols.
  - `sh-skip-intro on`: skips the title FMV. When attached during the logos it patches
    `jal open_main` in `GameState_MovieIntro_Update` (STREAM.BIN) via a breakpoint on MainLoop's
    state dispatch; when attached mid-movie it sets `max_frame` (`0x801E3F40`) to 0.
- VS Code: `.vscode/launch.json` "DuckStation (attach)" (local, gitignored) sources `sh1.py`.

Notes: each gdb breakpoint hit pauses the emulator, so per-frame breakpoints cause visible
stutter; and in all-stop mode, enabling/disabling a breakpoint from a `gdb.post_event` while the
target runs only takes effect at the next stop (change it inside `stop()` instead).

### Warp sweep

`tools/port/gdb/warp_sweep.py`: per map, boot DuckStation at unlimited speed, skip the intro,
warp the first title-screen demo into the map, and pass when the next map load is reached. It
temporarily edits DuckStation's `settings.ini` and restores it (and recovers from a killed run).
DuckStation runs in its own process group so the whole AppImage gets closed.

Results on the shifted image: 36/43 pass. The 7 failures (map1_s04, map2_s01, map2_s03,
map3_s06, map4_s00, map4_s06, map6_s05) fail the same way on the **unmodified** disc — stuck on
the title screen re-reading the CD, or frozen — so they come from warping a demo (wrong spawn,
story flags and recorded inputs), not from the shift. Warping is a smoke test; it doesn't replace
playing through.

Conclusion: map overlays are position-independent within their load region. Step 1 done.

---

## 2026-10-04 — Step 1 correction: absolute symbol assignments

While surveying symbols for Step 2, found that the shift test was weaker than it looked.

### The problem

Each binary links with `-T undefined_syms_auto.X.txt -T undefined_funcs_auto.X.txt
-T configs/USA/lib_externs.ld`. Those files assign symbols fixed PS1 addresses, and a linker-script
assignment **overrides** an object's own definition: the final ELF shows the symbol as `ABS`.
splat lists some symbols there even though its own data asm defines them (e.g. `dlabel D_800F4806`
in map7_s03's data), and `lib_externs.ld` assigns main's own library functions. 402 such entries
across 45 binaries; 116 distinct map `.data` variables are used from C this way.

In the shift test those variables stayed at their old addresses while the data moved 0x110 bytes,
so map code read/wrote the wrong bytes. The 36 passing maps just didn't crash in a short demo.

### Fix (matching build unchanged, all 50 checksums OK)

- `tools/port/prune_undefined_syms.py prune`, run in the Makefile's link rule: copies each binary's
  `undefined_*_auto` files and `lib_externs.ld` into `build/.../<target>.*` without the names the
  binary's objects define (including the prebuilt `lib/*.o`), and links with those copies.
- 4 referenced names have no definition at all — a byte inside another variable, or a bss gap the
  generated linker script only reserves with `. += N`: `D_800C15B4`, `D_800C391E`, `D_800C4454`
  (bodyprog) and map1_s04's `D_800CD768_tbl` alias. They're now `PROVIDE(name = anchor + off)` in
  `configs/USA/relative_syms.ld`, with offsets taken from the matching layout. Note `D_800C4454` is
  anchored on `screenPosY.53`, a compiler-numbered static local; regenerate if that file changes.
  (`relativize` mode regenerates entries from a matching build.)
- First attempt computed the relative offsets on every link; that's wrong under a shift (it
  measures the stale ABS address against moved neighbours). Offsets must be fixed config.

Verified with map1_s04 padded by 0x110: `D_800CD768_tbl` now follows `D_800CD768` (`0x800CD878`).

Audit afterwards: referenced `ABS` symbols inside a binary's own range are only `main_*_SIZE`
(linker constants), the 4 relative ones (`ABS` in the ELF but computed from their anchor), and
bodyprog's references to `g_MapOverlayHdr` / `GameState_KonamiLogo_Update` (cross-binary; Step 2).
