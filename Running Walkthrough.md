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

### Shift test, rerun with the fix

Rebuilt the shifted image (0x110 padding in every map) on top of the linker fix; map data
variables now move (map7_s03 `D_800F4806` → `0x800F4916`). Warp sweep: 36/43 pass, and the 7
failures are exactly the ones that fail on the unmodified disc (map1_s04, map2_s01, map2_s03,
map3_s06, map4_s00, map4_s06, map6_s05). Step 1 done.

---

## 2026-10-04 — PS2 toolchain (prebuilt ps2dev)

No need to build the toolchain from source: ps2dev publishes prebuilt Linux archives.

```
export PS2DEV=$HOME/ps2dev && mkdir -p $PS2DEV
curl -L -o ps2dev-latest.tar.gz https://github.com/ps2dev/ps2dev/releases/download/latest/ps2dev-ubuntu-latest.tar.gz
tar -xf ps2dev-latest.tar.gz --strip-components 1 -C $PS2DEV
```

Installed version: `ps2dev-ubuntu-latest.tar.gz` asset updated 2026-10-03T23:22:30Z,
SHA-256 `d053f43c2c3cf7be597270e9afe08f3d03ced33430f1650ca66a898b8111633e`; EE compiler
`mips64r5900el-ps2-elf-gcc` 15.2.0. Runs on Debian 13 as is. Installed under `~/ps2dev` (no sudo);
environment added to `~/.bashrc`:

```
export PS2DEV=$HOME/ps2dev
export PS2SDK=$PS2DEV/ps2sdk
export GSKIT=$PS2DEV/gsKit
export PATH=$PATH:$PS2DEV/bin:$PS2DEV/ee/bin:$PS2DEV/iop/bin:$PS2DEV/dvp/bin:$PS2SDK/bin
```

Check: a hello world using `$(PS2SDK)/samples/Makefile.pref` + `Makefile.eeglobal` and
`-ldebug` (`init_scr`/`scr_printf`) builds a static EE ELF. Not run yet: no PS2 emulator installed.
Note the EE compiler is GCC 15 (the PS1 build uses GCC 2.8.1): expect new warnings/errors in the
decomp's C, and C23 defaults — pass an explicit `-std=` in the port build.

### PCSX2

PCSX2 v2.8.2 (AppImage in `~/Downloads`), BIOS `ps2-0100j-20000117` (Japan v1.00). Booting an ELF:
`pcsx2-qt -batch -fastboot -nofullscreen -logfile <log> -elf <file.elf> -- <file.elf>`.

ps2sdk's `printf` shows up in PCSX2's **IOP** console (it goes through the IOP's tty), so both
`EnableEEConsole` and `EnableIOPConsole` must be on to see program output in the log.
`tools/port/pcsx2_run.py <elf> [--seconds N] [--until TEXT]` boots an ELF with those turned on,
prints the program's output, closes PCSX2 (whole process group), and restores `PCSX2.ini`.

`port/hello/` (`make -C port/hello`) prints "Silent Hill PS2 port: toolchain OK"; confirmed in
PCSX2 ~1 s after boot. The toolchain → ELF → emulator path works.

---

## 2026-10-04 — Port build: C front-end pass with the EE compiler

First step towards a port build: run every compilable C file through
`mips64r5900el-ps2-elf-gcc -fsyntax-only` (GCC 15) with `-DSH_PORT -DNON_MATCHING`, `-std=gnu89`,
`-nostdinc` and the decomp's own include paths, with GCC 14+'s new hard errors for old C
(implicit declarations, int/pointer conversions, implicit int, return mismatch) downgraded to
warnings for now. `tools/port/ee_syntax_check.sh` does this (skips `src/maps/*.c` that are only
`#include`d into maps).

Initially 18 real files failed (14 bodyprog, 4 maps). Fixes, all byte-identical on PS1 (50/50):

- `include/decomp/port.h` (from `common.h`):
  - `MATCH_STATIC` — `static` on PS1, external in the port. 78 definitions that are `static` but
    declared `extern` in a header (GCC 15 rejects that; on PS1 other files reach them by absolute
    address anyway), plus a block-scope `static` function declaration in `npc_main.c`.
  - `MATCH_CONST` — `const` on PS1, mutable in the port. Used for `g_MapOverlayHdr` (the extern in
    `bodyprog.h` and all 43 map header definitions): bodyprog and maps write to it at runtime
    (`bgmCmd`, `ambientAudioIdx`, `charaGroupIds`, `charaUpdateFuncs`), and a modern compiler could
    fold reads of a `const` object to its initial value.
- Header declarations brought in line with definitions: `const` added to
  `sharedData_800CB094_3_s01`, `D_80028A20`, `D_800297B8`; `Sd_BgmInit` returns `bool`
  (header said `s32`), and `background_sound_init.c` now includes its own header.
- `func_8003FE04` writes through `arg0`, so `arg0` is no longer pointer-to-`const`.
- `world_effects.c`: forward declaration for `func_8003F654` (called before its definition).

Result: 0 of 456 files fail. Remaining warnings to work through before turning them back into
errors: mostly `-Wincompatible-pointer-types` and `-Wbuiltin-declaration-mismatch` (the decomp's
own libc-like prototypes), plus a few `-Woverflow` / `-Wshift-count-overflow` worth checking for
real bugs. This is front-end only: GTE inline asm and `INCLUDE_ASM` aren't assembled yet.

---

## 2026-10-04 — Step 3: software GTE, verified against the PS1 GTE

### Full EE compile

Compiling (not just syntax-checking) every C file the USA build links (451, from `linkers/`)
with the EE compiler: after adding `-Wa,-Iinclude` (for `INCLUDE_ASM`'s `macro.inc`), 95 files fail,
all on GTE code: `lwc2`/`swc2`/`mfc2`/`mtc2`/`cfc2`/`ctc2` don't exist on the R5900 (its COP2 is
VU0). 76 distinct `gte_*` macros are used (`include/psyq/inline_c.h`, `gtemac.h`, `gpu.h`).

VU0 was considered and deferred: it's floating point, while the game depends on the GTE's exact
fixed-point results (saturation, flags, UNR divide). The C GTE is the reference; hot paths can move
to the VUs later if profiling says so (EE ~295 MHz vs PS1 33.8 MHz).

### `src/port/gte.c` / `include/port/gte.h`

Own implementation from psx-spx's GTE chapter (repo is GPL-3.0; PsyCross is MIT and would have been
usable, DuckStation's licence doesn't allow reuse, but it's fine as a test oracle). API mirrors the
CPU's view: `Gte_DataWrite/Read` (MTC2/MFC2, LWC2/SWC2), `Gte_CtrlWrite/Read` (CTC2/CFC2),
`Gte_Command` (the 25-bit COP2 immediate). Includes register quirks (sign extension, SXYP
move-on-write, IRGB/ORGB, LZCS/LZCR, H read-back bug), 44-bit MAC checks after every addition,
RTPS's IR3 flag quirk, the MVMVA FC bug and garbage matrix, and the UNR divide table.

### Verification: `tools/port/gte_test`

- `gen.py` → 340 command words: every opcode × sf × lm, and MVMVA × sf × mx × v × cv × lm.
- `test_ps1.c` (bare-metal PS-EXE built with Debian's `mipsel-linux-gnu-gcc -march=r3000`,
  `-static -no-pie`): per command, 8 input sets from a shared xorshift PRNG (`inputs.h`; odd =
  fully random, even = realistic ranges), runs the real COP2 instruction, stores all 64 registers.
- `run.py [seed]`: boots the EXE in DuckStation (as the boot file: `-- gte_test.exe`; `-exe` alone
  exits in batch mode), waits at `gte_test_done` via gdb, dumps the results; `check` replays the
  same inputs through `gte.c` and compares every register.

Bugs found by it:
- Test generator: `pack16(rand(), rand())` — argument evaluation order differs between the MIPS and
  host compilers, so the two sides generated different inputs. RNG calls now sequenced explicitly.
- `noinline` on `gte_test_done`, or the breakpoint never hits (it got inlined into `main`).
- GTE: in `MAC+(FC-MAC)*IR0`, IR saturates from the shifted difference **truncated to 32 bits**,
  while the overflow flags come from the full value (only visible with sf=0 and extreme inputs).

Result: 19,040 tests (seeds 0-6), 0 differences in any register, FLAG included.

Next: map the 76 `gte_*` macros onto `gte.c` under `SH_PORT`.

### GTE macros → software GTE

The GTE macros are inline asm in three headers, applied in order: Sony's `include/psyq/inline_c.h`,
the decomp's `include/inline_no_dmpsx.h` (real COP2 words instead of DMPSX placeholders) and
`include/gpu.h` (custom macros). `gtemac.h` only composes them, so it needs no changes.

`tools/port/gen_gte_inline.py` statically translates every asm macro into C calls to `gte.c`,
writing `include/port/gte_from_{inline_c,inline_no_dmpsx,gpu}.h`; each source header includes its
translation at the end under `SH_PORT` (`#undef` + redefine), so the override order is unchanged.
Supported: lwc2/swc2, mtc2/mfc2/ctc2/cfc2, `.word <COP2>` (→ `Gte_Command(word & 0x1FFFFFF)`; e.g.
`0x4B400006` is a real NCLIP — bit 24 belongs to the immediate), and the CPU instructions the macros
use on `$12-$15` (loads/stores, shifts, or/and/addu/subu/addi/negu/move). Anything else, including
Sony's raw DMPSX placeholders (132 in `inline_c.h`, none used by the game), becomes a call to an
undefined function, so it fails at link time instead of being translated wrongly. Statement-
expression macros with outputs (`gte_stIR1()` etc.) and asm that writes its *input* operands as
scratch (`gpu.h` `gte_LoadVector0_XYZ`, `gte_SetLightSourceXY`; locals in C) are handled. Rerun the
script after editing any of the three headers.

Two hand-written asm blocks got `SH_PORT` C versions: `Vw_TransformAndProjectPoint`
(`vw_calc.c`; returns via `$v0` from asm — note it saves TRY in the 16-bit `VZ1`, so the restored TRY
is truncated, reproduced as-is) and the local `gte_strgb3_vec` (`bodyprog_80056D8C.c`).

`gte.h`/`gte.c` no longer use `<stdint.h>` (the port compiles with `-nostdinc`); GTE test re-run:
still 0 differences.

Result: `tools/port/ee_compile_check.sh` (replaces `ee_syntax_check.sh`; compiles the 451 C files
listed in `linkers/USA/*.ld` to EE objects) — 0 failures. PS1 build 50/50. Still open: Sony's
prebuilt libgte functions, and `INCLUDE_ASM` functions that are still R3000 asm.

---

## 2026-10-04 — First full PS2 link: HAL inventory

`tools/port/port_link.py` links the whole game for the EE: EE objects for the 451 C files
(`ee_compile_check.sh`), splat's 190 data `.s` files assembled with the EE toolchain, `src/port/*.c`,
maps and screen overlays merged/localised as in the PoC, then a final link with ps2sdk's crt0 and
libc (`$PS2SDK/ee/startup/linkfile`). Left out on purpose: Sony's prebuilt PS1 libraries (`lib/*.o`),
the PS-EXE header, `footer_data` padding, `src/main/libsn/snmain.s` (PS1 startup: clears BSS and calls
`main`; ps2sdk's crt0 replaces it — it was the only user of the `main_*` section markers), and
`src/bodyprog/libkmath/libkmath.s` (Konami math: 11 functions plus tables, 17 GTE instructions).

`configs/USA/relative_syms.ld` anchors `D_800C4454` on `screenPosY.53`, a GCC 2.8 static-local name
that GCC 15 doesn't produce. It's a standalone bss-gap variable, so the port simply defines it in
`src/port/port_data.c` (a PROVIDE only applies when the symbol is otherwise undefined).

Result: no duplicate definitions (game vs ps2sdk/newlib), 193 undefined symbols, grouped by the
Sony library that defined them on PS1 — table in the Planning Doc ("HAL inventory").

---

## 2026-10-04 — libgte and libkmath via a static recompiler

The game needs 45 libgte functions (~1,700 instructions) and Konami's libkmath (11 functions, ~400
instructions; `src/bodyprog/libkmath/libkmath.s`); libgs is another ~2,100. Rather than hand-port
these (and risk small numeric differences the game is sensitive to), they're recompiled from the
PS1 machine code.

### `tools/port/recomp.py` (+ `include/port/recomp.h`)

Reads a PS1 relocatable object (pyelftools), decodes MIPS-I itself, and writes `<obj>.c` plus
`<obj>.data.s` (the object's data sections, relocated words as `.word sym+addend`, assembled for
the EE). Each function becomes `void rc_<name>(RcRegs* r)` over a shared register file, one label
per instruction; global functions get a wrapper with the original name, sized from the PSY-Q
prototypes, that sets up registers and an emulated stack (args 5+ at sp+16, as o32 expects).
Semantics kept: branch delay slots (condition evaluated before the slot), load delays when the next
instruction reads the loaded register, HI/LO and MIPS DIV-by-zero results, unaligned lwl/lwr/swl/swr,
COP2 via `gte.c`, HI16/LO16 relocation pairing. Works because EE pointers are 32-bit too.
Hand-asm idioms handled: calls/branches/jumps into the middle of another function (made separate
entry points; crossing becomes a tail call), `jr` through a saved copy of `$ra` (a return), `j` with
a relocation inside the same function (a goto). Unsupported input (COP0, jump tables, a load delay
hazard into a branch) is a hard error.

`tools/port/recomp_all.sh` recompiles the 42 libgte objects the game needs and libkmath into
`build/port/recomp/`; `port_link.py` compiles them in. `tools/port/recomp_protos.h` gives argument
counts for libkmath functions no header declares (map7_s03 calls `Math_RotMatrixZxy` implicitly).
`Math_RotMatrixGte` gets no wrapper: it's an internal helper with a non-C convention (inputs in
`$v0/$v1/$a3`, results in `$t` registers). `InitGeom` (patches the PS1 kernel via COP0) is
hand-written in `src/port/libgte_port.c`: it only sets ZSF3/ZSF4/H/DQA/DQB/OFX/OFY defaults.

### Verification: `tools/port/lib_test`

Same idea as the GTE test, for library functions: test list and call stubs generated from the
prototypes; pointer arguments point into a 2 KB arena of pseudo-random data, integers are
pseudo-random, GTE registers set to realistic random values. The PS1 build links Sony's *original*
objects (`lib/libgte/*.o` as an archive, plus the PS1 libkmath object) and runs in DuckStation; the EE
build links the recompiled code, embeds the PS1 results and runs in PCSX2 (`pcsx2_run.py`),
comparing return value (pointers as arena offsets), the whole arena and all 64 GTE registers.
`python3 tools/port/lib_test/run.py`.

Result: 48 functions × 8 tests, 0 differences. (`GsTMDfast*` need real TMD data; tested with libgs.)

HAL inventory after this: 140 undefined symbols (libgs 31, libgpu 30, libcd 19, libspu 19, libapi 15,
libpad 8, libcard 7, libpress 5, libetc 3, ours 3).

---

## 2026-10-04 — libgs recompiled

All 24 libgs objects go through `tools/port/recomp.py`. New recompiler features:

- **Jump tables** (`jr $v0` after loading from a table): the data emitter already stores `.text`
  addresses as `RC_TEXT_MARK + offset`; `jr reg` becomes a `switch` over every code address this
  object's data refers to (within the function), default `Rc_BadJump()` (`src/port/recomp_rt.c`).
- **`jalr`**: a call through a C function pointer (libgs's `GsFCALL4` table is filled from game C
  code, so the pointers are C functions, recompiled wrappers included): a0-a3 plus the o32 stack words.
- **Cross-object calls** are now `rc_<name>(r)` (shared registers). `tools/port/recomp_bridges.py`
  writes a `bridge_<name>.c` for every `rc_` name no recompiled object defines (plain C functions:
  libc, the HAL), one file each so that linking the recompiled code as an **archive** (now done in
  `port_link.py` and `lib_test`) only pulls in what's used.
- Wrappers zero the register file (callee-saved registers get saved before use; deterministic).
- `reg11.o` (`SetFarColor`) added: libgs calls it, the game doesn't.

### Test-input fixes (not port bugs)

- `SquareRoot0`/`SquareRoot12` with a **negative** argument compute a negative index and read
  memory *before* Sony's `SQRT` table — undefined, memory-layout dependent (results changed with the
  link order). The test now gives them non-negative arguments only.
- `GsSetFlatLight` normalises its light vector through `SquareRoot0` on the squared length; with
  random (or random 16-bit-half) 32-bit components that overflows to negative — same issue. It now
  gets small 32-bit values.

Result: `lib_test` 53 functions × 8 tests, 0 differences. libgs's packet builders and libgte's
`GsTMDfast*` need real model/OT data; they'll be compared against the PS1 in-game. Full link: 113
undefined symbols (libgpu 31, libcd 19, libspu 19, libapi 17, libpad 8, libcard 7, libpress 5,
libetc 4, ours 3).

---

## 2026-10-04 — First boot on the PS2, DVD image

### Making it run

- `src/port/port_main.c`: the PS2 `main()` prints a banner and calls the game's PS1 `main`, which
  `port_link.py` renames to `Game_PsxMain` in the combined object.
- `src/main/main.c`: under `SH_PORT`, skip `Fs_DecryptOverlay` of BODYPROG/B_KONAMI into their PS1
  load addresses (`0x80024B60`, `0x800C9578` — EE kernel memory); both are linked in.
- `configs/USA/port_syms.ld`: `g_FsBuffer18/20` = `g_PsxRam + offset`; `g_MapOverlayHdr` =
  `g_MapOverlayHdr_map0_s00` for now (the overlay-load hook will switch maps).
- `port_link.py` stubs every remaining undefined function (`build/port/hal_stubs.c`, each logs its
  first 3 calls) and links `build/port/sh1.elf`.
- First run crashed in newlib startup (`__retarget_lock_init_recursive`: NULL from `malloc`). Cause:
  the game's own `memcpy` (`src/main/memcpy.c`, a matching reconstruction pinned to GCC 2.8 register
  allocation) replaced newlib's for everything, including ps2sdk/newlib internals; under GCC 15 it
  miscopies. It's now PS1-only (`#ifndef SH_PORT`). (Checked: no other game/port symbol overlaps
  libc/libkernel/libcglue/libps2sdkc.)

Result: banner, then ResetCallback, CdInit, VSync, ResetGraph, ClearImage2, DrawSync, PutDispEnv,
SpuInit, then CdIntToPos/CdControl/CdReset retried forever — the file queue waiting for CD reads.

### DVD image

`tools/port/make_iso.sh` builds `build/port/sh1_ps2.iso`: SYSTEM.CNF (`BOOT2 = cdrom0:\SHPS_000.01;1`,
placeholder title ID), the ELF, and the original `SILENT.`/`HILL.` (HILL.'s 2336-byte XA sectors are
just file data; the port's CD layer will read them by offset). Written by `tools/port/mkiso.py`
(ISO9660 level 1, 2048-byte sectors, standard both-endian fields), padded with sparse zero sectors to
460,000 sectors (~942 MB) — more than a CD holds, which is what makes it DVD media.

How PCSX2 decides (from its source): `CDVDcommon.cpp FindDiskType`: > 452,849 sectors = DVD; below
that it guesses from the PVD root-directory record (u16 at offset 166 vs 171 — for a correct
both-endian size < 64 KiB these always match, i.e. "CD"); `InputIsoFile.cpp tryIsoType` labels
"Image type" CD if the root directory is exactly 2048 bytes. Experiments: a 64 KiB root directory
makes the v1.00 BIOS fail to find files; zeroing the BE copy of the root size breaks the BIOS
(reads BE), zeroing the LE copy breaks PCSX2's own loader (reads LE). Padding is the clean answer.

Open problem: as DVD media, the SCPH-10000 v1.00 BIOS can't open any file (`open fail name
SYSTEM.CNF;1` even in OSDSYS), with/without a UDF bridge (`genisoimage -udf`), fast or slow boot.
As CD media (unpadded) the same image boots. Needs testing with a later BIOS. For development the
ELF is booted directly (`pcsx2_run.py build/port/sh1.elf`); `pcsx2_run.py` now also boots images,
with `--slowboot` for a full BIOS boot.

### DVD image fixed: retail layout (checked against Silent Hill 2)

The "open fail" on DVD media wasn't the BIOS: the v1.60 BIOS (`ps2-0160j-20010427`) failed the same
way. With `CdvdVerboseReads` on (`pcsx2_run.py --cdvd-verbose`), the BIOS read sector 16 (PVD), then
**sector 257**, and gave up — our path table was at 18.

The first 8 MB of a retail PS2 DVD (Silent Hill 2 USA; `chdman extractraw --inputbytes 8388608`):
sectors 18/19 `BEA01`/`NSR02` and an anchor at 256 (UDF bridge), L path table at **257**, M at 259,
root directory at **261**, root size **468** bytes (bytes used, not rounded to a sector). The latter
is also PCSX2's DVD signature: u16 at PVD+166 (`0x01D4`) ≠ u16 at PVD+171 (`0x0100`).

`tools/port/mkiso.py` now uses that layout (path tables 257/259, root 261, exact root size; sectors
18-256 left empty — the BIOS doesn't need the UDF part). With it, PCSX2 sees DVD media without any
padding (607 MB image), and the game boots from the image with both the v1.00 and v1.60 BIOS
(`pcsx2_run.py --bios <file>` overrides the BIOS for one run). The earlier padding and root-directory
experiments are gone.

---

## 2026-10-04 — Overlay hook and CD reads: the game reaches bodyprog

### Overlays

- `src/main/main.c`: `g_OvlBodyprog`/`g_OvlDynamic` are `PSX_RAM_ADDR(...)` (identical on PS1). In the
  port, overlay files are still read through the file queue — into the RAM arena at their PS1 load
  address, never executed — so queue behaviour and timing don't change.
- `Fs_QueueStartRead` calls `Port_OverlayActivate(fileIdx)` under `SH_PORT` (`src/port/overlay.c`):
  for a map file it sets `g_MapOverlayHdrPtr` to that map's header (`g_MapOverlayHdr_<map>`, from
  the merge). `bodyprog.h`: under `SH_PORT` and not `SH_MAP_OVERLAY` (new flag, set for map sources
  in `ee_compile_check.sh`), `g_MapOverlayHdr` is `(*g_MapOverlayHdrPtr)`; map code uses its own
  header. Starts at map0_s00's header (on PS1 the address holds B_KONAMI's bytes until a map loads).
- Not yet: restoring an overlay's .data/.bss on reload.

### libcd on the DVD drive (`src/port/ps2/libcd_ps2.c`)

The subset the file queue uses: CdInit/CdReset (`sceCdInit`, DVD media mode, `sceCdSearchFile` for
`\SILENT.;1` and `\HILL.;1`), CdIntToPos/CdPosToInt (BCD, 150-sector lead-in), CdControl(B)
(CdlSetloc records the position; everything completes immediately), CdSync → CdlComplete,
CdRead (synchronous), CdReadSync → 0. PS1 sector L: SILENT. if 64 ≤ L < 64+39295 (DVD sector =
SILENT.lsn + L-64), else HILL. (raw 2336-byte sectors: data at +8 after the subheader; read through
a 64-byte-aligned bounce buffer). `src/port/ps2/` is compiled with ps2sdk headers instead of the
game's; `libcdvd-common.h` `#define`s `CdInit`/`CdRead`/`CdSync`/`CdPosToInt`/`CdIntToPos` to its own
functions, so those are `#undef`'d. Linked with `-lcdvd`.

Result (DVD image, PCSX2): `libcd: SILENT. at DVD sector 3355 (39295 sectors), HILL. at 42650`;
the warning TIM loads, main's fade loop runs, then bodyprog init: root counters, memory card, events,
pad, SPU and reverb, OTs, AddPrim — the main loop. Remaining crashes come from stubs returning NULL
(`ReadTIM` → `Fs_QueuePostLoadTim`; a NULL `strcpy` source). 102 HAL stubs left.

---

## 2026-10-04 — libgpu, VSync: the main loop runs

### libgpu

- Recompiled: the pure objects (p06/p09/p14/p16-18/p26/p33/p34 — AddPrim, TermPrim, SetPoly*, SetTile,
  SetDrawTPage, SetDrawMove — and tmd.o, OpenTIM/ReadTIM).
- sys.o (the GPU system layer) mixes hardware access into everything (its driver routines read
  hardware register addresses from its own data), so it's recompiled with
  `recomp.py --exports SetDrawMode SetDrawArea SetDrawOffset SetDrawStp SetDrawEnv SetPriority
  SetTexWindow GetGraphDebug`: only those get C wrappers. The other global functions become
  **static** — otherwise other recompiled code (libgs) called sys.o's own `rc_ClearOTagR` etc.
  directly and ended up in PS1 hardware code; now those calls go through bridges to the C versions.
- Recompiler: code addresses in *writable* data (`.data`/`.sdata`) are function pointers (sys.o's
  driver table) and become entry points; `jalr` to an `RC_TEXT_MARK` value dispatches to them through a
  switch (read-only data = jump tables, as before).
- `src/port/libgpu_port.c`: 1024x512 16-bit VRAM array; Load/Store/ClearImage (with VRAM wrap);
  ClearOTagR; DrawOTag/DrawPrim walk 24-bit-linked packets into `Gpu_Submit` (counts only for now,
  logged every 60 calls); PutDrawEnv builds `env->dr_env` with Sony's SetDrawEnv, submits it and does
  the isbg clear; Put/GetDrawEnv/DispEnv keep state.
- `src/port/ps2/libetc_ps2.c`: VSync on the EE VBlank interrupt (semaphore; mode 0 waits one, n
  waits n, <0 returns the count), VSyncCallback runs from the handler, GetVideoMode = NTSC.

### A layout bug: `-fno-toplevel-reorder`

Ordering tables didn't terminate (NULL links). Comparing with the real game (DuckStation + gdb,
breakpoint on `DrawOTag` at `0x80018A6C`): `g_OrderingTable0[1]` = {length 11, org
`0x800B7CC8`, tag `0x800B9CC4`} — the tag is one word *past* `g_OtTags1`, i.e. the
`__pad_bss_800B9CC4` word that `screen_data.c` declares right after the array. GCC 15 reordered the
file's variables, so in the port that word was `g_DeltaTime`, and the frame delta overwrote an OT
link. The port is now compiled with `-fno-toplevel-reorder` (declaration order, like GCC 2.8). Note
GCC still aligns arrays to 8, and layout *across* files follows link order, not the PS1's.

Result: OTs terminate; steady loop, ~1,700 DrawOTag calls a minute, 240 packets per 60 calls
(probably the first screen: nothing is drawn or read yet). 70 HAL stubs left.

---

## 2026-10-04 — First pixels: software GPU, display modes, timing

### Software GPU and display

- `src/port/gpu_soft.c`: PS1 GP0 commands into `g_PortVram`, from psx-spx: polygons (flat/gouraud,
  textured 4/8/15-bit, raw/modulated; quads as triangles 1-2-3 and 2-3-4; edge functions with a
  top-left rule), rectangles (texpage from E1, flips), lines/polylines, fill, VRAM copy, E1-E6,
  semi-transparency modes 0-3 (textured only when texel bit 15 is set), mask set/check, texture
  window, dithering (shaded/modulated polygons and lines). Plain C, no PS2 dependencies; it's the
  reference for the future GS renderer.
- `src/port/ps2/display_ps2.c` (gsKit, `-lgskit -ldmakit`): `PutDispEnv` presents the display area —
  copied out of VRAM, uploaded as PSMCT16 (same bit layout as PS1 15-bit pixels), drawn as one
  full-screen sprite with alpha test/blending off.
- Debug: frames 30/120/300/600/900/1200 are written to `host:frame_NNNN.ppm` and the whole VRAM to
  `host:vram_NNNN.ppm` (PCSX2 HostFs; `pcsx2_run.py` now enables `HostFs` and `OutputMuted`).

First frame was black: the draw area came out as `E30FFFFF`/`E40FFFFF`. Sony's SetDrawEnv
(recompiled sys.o) clamps the clip rect against `GEnv`, which the PS1's ResetGraph initialises.
Dumped `GEnv` from the real game (DuckStation + gdb at the first DrawOTag): only `+0 = 0x100`,
`+4 = 0x02000400` (VRAM 1024x512), `+8 = 1` come from ResetGraph; the rest is per-frame state. Our
ResetGraph now sets those three. Result: the warning screen renders correctly (texture, CLUT, the
SPRT strips, subtractive fade).

### Output modes (240p / 480i)

The PS2 output follows the game's `DISPENV`: 480i (640x448, GS interlaced field mode) when
`isinter` is on and the display has more than 256 lines, else 240p (640x240, progressive). gsKit is
re-initialised on a mode change. Observed: 240p for the warning screen, 480i from the Konami logo
state on.

### Timing

`GameState_Init` waited forever for its fade: the frame delta is `GsGetVcount()` =
`GetRCnt(RCntCNT1)` (horizontal blanks), a stub returning 0. `src/port/ps2/rcnt_ps2.c` models the
root counters from the EE's CP0 Count (294.912 MHz, extended to 64 bits): counter 0 system clock,
1 NTSC hblanks (15,734 Hz), 2 system clock/8, 3 vblanks; 16-bit, wrapping at the target. No counter
interrupts yet. `src/port/libspu_port.c` makes every libspu call succeed (transfers complete at once;
the game waited on `SpuIsTransferCompleted` before). The game now goes through game states 0 → 1 → 2.
46 HAL stubs left.

### Native widths, 1:1 pixels, comparison settings

- Display: the PS1's width is shown at its own resolution through the GS `DISPLAY` register's
  horizontal magnification (NTSC line = 2560 video clocks; MAGH 9/7/6/4/3 for 256/320/368/512/640 —
  368 uses the same divider 7 as the PS1), DH = the PS1's line count, centred on gsKit's start
  coordinates. gsKit computes MAGH from the framebuffer width (wrong for 368), so after gsKit's setup
  (640-wide framebuffer) `DISPLAY1`/`DISPLAY2` are rewritten. The display area is copied 1:1 into the
  framebuffer with nearest sampling: no scaling or filtering (the PS1 GPU doesn't filter textures),
  so dumps are pixel-exact. Observed: 320x240p, 320x224p, then 640x448i at the Konami logo state.
- `pcsx2_run.py` enforces the comparison settings per run — native resolution
  (`upscale_multiplier 1`), "Bilinear (Sharp)" output (`linear_present_mode 2`), texture filtering as
  the program sets it (`filter 2`) — and runs at unlimited speed by default (`--realtime` to turn it
  off). The DuckStation runners (`warp_sweep.py`, `gte_test`, `lib_test`) enforce native resolution,
  nearest texture filtering and bilinear-sharp scaling; they already ran at unlimited speed.

### Konami logo, events, title menu

- The Konami logo state drew nothing: its draw environment had an empty clip area
  (`E3008000 E4007C00`; on the PS1 `E4077D3F`, 320x448 from y 32). The libgs objects reference
  their own weak `.bss` variables (`GsDISPENV`, `CLIP2`, `PSDIDX`, `PSDOFSX`, ...) by symbol, and
  `recomp.py` added the symbol's offset within `.bss` on top of the symbol's address, so libgs read
  and wrote the wrong variables (e.g. `GsDISPENV+0x98`). The symbol name already is the address:
  HI16/LO16 halves and data words no longer add it (jump targets still use it, being offsets into
  `.text`). Also affected libcd (`StRingAddr`), libgpu (`_que`), libpad and libspu. The strcpy
  null-pointer read seen at boot is gone too.
- `src/port/libapi_port.c`: PS1 kernel events (handles `0xF1000000 | n`; `TestEvent` polls and
  clears, enabled `EvMdINTR` events call their handler on delivery) and a libcard for empty slots:
  every `_card_*` call is accepted and ends with `EvSpTIMOUT` (SwCARD and HwCARD). The KCET state's
  memory card check then finds no cards, the intro movie is skipped (no MDEC yet) and the game
  reaches the title menu (state 7, 320x448i), matching the PS1's image.

### Controller, attract demo in-game

- `src/port/ps2/libpad_ps2.c`: PS1 libpad over ps2sdk's libpad with the BIOS's `rom0:SIO2MAN` /
  `rom0:PADMAN` (present on every BIOS). The PS1 receive buffers (status, ID, buttons, sticks) have
  the same 8-byte layout as `padButtonStatus` and are refreshed from `VSync`, as the PS1 BIOS does
  every vertical blank. `PadInfoMode` answers from values read once per frame: with the BIOS's
  PADMAN it can go through an IOP RPC, and the game's vibration code calls it several times per frame
  (the game hung in it in-game).
- Game code passes NULL to string functions where the PS1 reads address 0 (whose first byte is 0):
  `MemCard_WorkSet` (`strcat`, filename NULL) and `StringCopy` (`Texture_Init(..., NULL, ...)` from
  the world map). Both guarded under `SH_PORT`.
- `movie_main` returns at once under `SH_PORT` (as when a movie file isn't found) until CD streaming
  and MDEC exist; the intro movie used to loop forever waiting for stream data.
- `-fno-zero-initialized-in-bss` for the game's EE objects: GCC 15 moved explicitly zero-initialized
  variables to `.bss`, breaking layouts the game relies on. `HARRY_BASE_ANIM_INFOS` is declared with
  57 entries followed by zeroed variables that are really the rest of the array (the game copies up
  to 152 entries into it: weapon anims at 56-75, a map's Harry anims from 76; map6_s04 has 76). With
  those in `.bss`, the copy overwrote the libkpad function table `D_800AFD08` and the game jumped
  into data.
- libspu placeholder: key on/off state per voice (`SpuGetKeyStatus` returns `SPU_ON`/`SPU_OFF`, -1
  for an empty mask); the sound driver waits for a keyed-on voice to report on.
- Main thread priority lowered (64), so higher-priority threads run while the game busy-waits.
- Crash detection: a heartbeat thread prints the vertical blank and frame counts every 60 vertical
  blanks; `pcsx2_run.py` stops a run when no new frame is shown for 15 emulated seconds, or no
  heartbeat comes for 15 real seconds (`--stall`). `src/port/ps2/crash_ps2.c` reports TLB/address
  exceptions on hardware (PCSX2 logs those and carries on instead of raising them).
- Debugging aid: `SH1_EXTRA_CFLAGS` is passed to the game's EE compile
  (`tools/port/ee_compile_check.sh`), e.g. `-finstrument-functions` for a shadow call stack.
- Result: title menu → attract demo; the demo plays in the town (fog, snow, road, Harry) without
  faults. The software GPU makes it run at ~10% speed in PCSX2.

### GS hardware renderer

`src/port/ps2/gpu_gs.c` executes the GP0 packets with GS primitives, drawing into an image of the
PS1's VRAM in GS memory (PSMCT16 1024x512 at 0x300000, out of reach of gsKit's display setup; 4/8-bit
texture page cache at 0x200000). The PS1 features map onto GS state almost directly:

| PS1 | GS |
|---|---|
| 15-bit VRAM + mask bit | PSMCT16 frame buffer (bit 15 = alpha MSB) |
| Texture × colour / 128, raw texture | MODULATE (same formula), DECAL |
| Texel 0x0000 transparent, STP bit | TEXA (AEM, TA0 0x40, TA1 0x80) + alpha test |
| Semi-transparency only on STP texels | PABE |
| Blend modes 0-3 | ALPHA with FIX 0x40/0x80/0x80/0x20, COLCLAMP |
| Set / check mask | FBA / DATE |
| Dithering | DTHE + DIMX = the PS1 matrix |
| Texture window | CLAMP REGION_REPEAT |
| CLUTs in VRAM | CSM2 (TEXCLUT COU/COV = CLUT position) |
| 4/8-bit pages | PSMT4/PSMT8 uploaded from g_PortVram (same packing), cached |
| 15-bit pages | sampled straight from the VRAM buffer |

- Display: a GS-local 1:1 copy of the display area into gsKit's frame buffer.
- `StoreImage` and the debug dumps read VRAM back from the GS (local-to-host transfer through
  BUSDIR / VIF1 DMA).
- Fixes found by comparing against the software renderer (`SH1_GPU=compare`: both renderers, dumps
  write both images and a difference count): `TEX0` must be written again when only `TEXCLUT` changes
  (the CLUT loads on `TEX0`; Harry had another texture's colours); flat untextured colours are
  truncated to 5 bits before blending, as on the PS1 (fades were off by one everywhere). Konami/KCET
  screens now match pixel for pixel; in-game, nearly all remaining differences are 1/31 steps from
  interpolation rounding, plus ~150 edge pixels per frame.
- Renderer chosen at build time: `SH1_GPU=gs` (default), `soft`, `compare` (`tools/port/port_link.py`).
- At unlimited speed the game went from ~10% in-game to far above full speed. At real-time speed it
  first stuck on the Konami logo: two timing bugs.
  - `VSync` returned without a vertical blank: after gsKit's setup runs again (display mode
    change), something else signals our semaphore (likely a gsKit/dmaKit semaphore ID reused; the GS
    renderer's DMA transfers triggered it). `VSync` now waits for the vertical blank count to
    advance, using the semaphore only to sleep. The handler is also reinstalled after gsKit's setup.
  - Root counter 1 (the game's frame time, `GsGetVcount`) now counts whole fields from the vertical
    blank interrupts (262.5 lines each) instead of the EE cycle counter, which doesn't track time
    between vertical blanks under emulation.
- In-game speed: the attract demo runs at 20 fps at real-time speed, which is by design: demo
  playback uses the recording's frame interval (3 vertical blanks). Measured with the EE cycle
  counter: a frame is 14.8 M cycles (3 fields), the GS translation of its ordering tables ~1.8 M,
  and the EE waits in VSync for over half of the time, so normal gameplay (2 fields, 30 fps) has
  room to spare.

### 60 fps: option and first profile

- The GameShark 60 fps code (`A70A8FF0 00020001`) changes `g_IntervalVBlanks` from 2 to 1 (DuckStation
  code type A7: 16-bit write if equal). Its second line (`A70AB38C`) points into `gSDXATable` in this
  version, probably for another revision. `SH1_FPS=60` (`tools/port/ee_compile_check.sh`) makes
  gameplay set 1 instead of 2; menus already run at 1.
- `SH1_BENCH=1` also runs the attract demo at one frame per vertical blank (it desyncs, but renders
  the same town): at real-time speed, 30-60 fps depending on the view (frames that don't fit one
  field drop to 30).
- `SH1_PROF=1`: named cycle counters (include/port/prof.h, src/port/prof.c) around the main loop's
  parts, `DrawOTag`, each GTE command and every recompiled library entry point; reported every 120
  frames. Off by default (the macros are empty).
- First profile (demo scene, kcycles per frame; a field is 4,900): game state update 3,150, of which
  GTE commands ~1,970 (RTPT ~1,930 cycles per call, RTPS ~710, MVMVA ~540, DPCS ~500, NCLIP ~140);
  swap and draw 1,280 (the GS translation of the ordering tables); total ~4,400. The software GTE is
  written for bit-exactness (64-bit MACs with an overflow check after each addition, matrices
  unpacked per call): RTPS is ~850 instructions.

### Faster GTE (bit-exact)

Chosen over VU0: VU0 computes in 32-bit floats (24-bit mantissas), but GTE products of 16-bit
values reach 2^30 and sums 2^43, so large coordinates would round (1-unit differences in depth and
screen positions, i.e. wobble/cracks the PS1 doesn't have). The EE's MMI integer SIMD is exact.

Test bench first:
- `tools/port/gte_test`: all seven seeds recorded again from DuckStation and kept
  (`results_seedN.bin`); a third input class (every fourth test) is game-like (nothing saturates),
  so the fast paths are covered. `check` takes the seed at run time.
- `check_ee.elf` / `run_ee.py`: the same checker built for the EE, run in PCSX2 (results read
  through `host:`), so the EE-only code (MMI) is checked too; it also prints EE cycles per command,
  for game-like inputs run 256 times in a row.

Changes in `src/port/gte.c` (19,040 tests, 0 differences on the host and on the EE):
- `s_Small`: while TR, BK and FC are within +-2^30, no 44-bit MAC check can trigger (every other
  operand is 16-bit), so the checks are skipped (not for GPF/GPL, which add a shifted MAC).
- RT/LLM/LCM unpacked when written instead of per command.
- Fast paths for RTPS/RTPT (per vertex), DPCS and MVMVA: computed in registers, committed only when
  nothing saturates (FLAG stays 0), otherwise the full code runs on the untouched registers. The
  full RTPS/RTPT code also sums directly when `s_Small` (for vertices that saturate, ~16% in-game).
- 16x16 products done as 32-bit MULTs (some had become 64x64 multiplies: `__muldi3` calls).
- MMI: `PLZCW` for the UNR division's leading zeros (was a bit loop); `PHMADH` for RT*V in the
  RTPS/RTPT fast path (no faster than scalar MULTs under PCSX2's timing, exact; kept for hardware).
- `include/port/gte.h`: register moves are inline (the game's macros name constant registers, so
  a plain register is one load/store); registers with side effects still call gte.c.
- Command handlers `noinline`, so `Gte_Command`'s dispatch doesn't save a dozen registers.

Game-like cycles per command (EE, PCSX2): RTPS 767 → 343, RTPT 2007 → 897, DPCS 530 → 259, MVMVA
547 → 242, NCLIP 146 → 106. In-game (demo scene): GTE ~1.97 → ~1.42 M cycles per frame, game state
update 3.15 → 2.42 M. With `SH1_FPS=60 SH1_BENCH=1` at real-time speed: 60 fps in most views,
average 57.7 (was ~45), dips to 43-55 in heavy views. The GS translation (1.3 M) is now the biggest
single item.

### GS translation speed

Profiled inside `gpu_gs.c` (`SH1_PROF=1` now also covers `src/port/ps2/`): about 712 polygons per
frame in the demo scene at ~1,700 cycles each (parse ~320, state ~530, emission ~510); DMA is cheap
(two flushes per frame).
- `prim_state` memoised: consecutive primitives with the same inputs (texture page, CLUT, blending,
  dithering, window, clip area, mask) reuse the last result; `s_Epoch` invalidates it whenever state
  changes elsewhere (reset, fills, display copies, texture cache changes).
- `set`/`ad` inline fast paths (the flush path is out of line).
- Vertices go out as GIF REGLIST runs (PRIM and vertex registers listed once per tag, 64 bits per
  register, consecutive primitives of the same shape share a tag): half the data of A+D.
Under PCSX2's timing these gave little (1.28 → ~1.2 M cycles; REGLIST halves the GS traffic, which
matters more on hardware). More importantly, measuring idle time (vertical blank waits) showed the
EE is 11-38% idle per second in-game at 60 fps; the frame rate dips (36-54 fps for a second or two)
come with *more* idle time: they are file loads (the loader waits in VSync loops), as on the PS1.
`SH1_FPS=60 SH1_BENCH=1`: average 58.3 fps; the rendering itself fits in a field in this scene.

### Faster GTE (2), one-boot warp sweep, strict aliasing

- `tools/port/warp_sweep_ps2.py`: sweeps measure the game's frame rate in emulated time (frames per
  60 vertical blanks, from the heartbeat, which now also reports the share of time spent waiting in
  VSync), so the emulator's speed doesn't matter. Now one boot visits every map: `warp.txt` lists
  the maps; each run of the first attract demo warps to the next one, the port presses L2 after
  `seconds=N` to end it (`Port_WarpButtons`), other demos are ended after 5 s. Same-demo warps
  only: warping another demo carries its recorded state into the map (map4_s02 crashed that way).
- Lean exact GTE paths for `s_Small` (saturation and flags inline): RTPS/RTPT, DPCS/DPCT, DCPL,
  NCS/NCT/NCCS/NCDS/NCDT. A 32x32->64 multiply via MULT/MFHI (`mul32`): GCC turned some of these
  into `__muldi3` calls.
- Control register writes inline and lazy: matrix/TR/BK/FC writes only store and mark the cache dirty;
  the next command refreshes it once. The profile (`src/port/prof_fn.c`, function-level exclusive
  cycles with `-finstrument-functions`) had shown ~8,000 out-of-line control writes per frame in
  map7_s00, each unpacking a matrix.
- `-fno-strict-aliasing` for game, port and recompiled code: with the register writes inlined, GCC
  reordered the GTE macros' type-punned matrix reads (s16 matrices read as words) before the stores
  that built the matrix: the second attract demo then read garbage world-object pointers. The PS1's
  GCC 2.8 never assumed strict aliasing.
- Results (60 fps benchmark, emulated time): most maps average 54-59 fps; map7_s00 35 -> 55-58,
  map1_s06 36 -> 49-58, map5_s00 37 -> 51-59; map7_s03 is the slowest (~43). A few maps fail only in
  the one-boot sweep (visited after others): not investigated.
- Overlay data reset (`SH_PORT_OVERLAY_RESET`, off): restores an overlay's .data/.bss when its file
  is read again (port_link.py markers + startup snapshot). Fixes a map loaded twice in a row in the
  sweep, but the PS1 overlay memory isn't fully modelled yet; TODO.

### Comparing with DuckStation frame by frame

`tools/port/compare_frames.py --frames 26,100,400` captures the same frames of the first attract demo
(by `g_Demo_DemoStep`) from both:
- PS2 (`src/port/ps2/capture_ps2.c`, `host:capture.txt`): every GP0 packet the frame drew, then at the
  next PutDispEnv the VRAM (read back from the GS), display rectangle, `g_SysWork`, `g_GameWork`,
  `g_DeltaTime`.
- PS1 (`tools/port/gdb/capture_ps1.py`, DuckStation with the software renderer): the ordering tables
  walked at DrawOTag plus PutDrawEnv's packet; VRAM by calling the game's own StoreImage/DrawSync
  from gdb in 64-line chunks through a scratch RAM area (restored after); the same state structures.
It writes PS1 | PS2 | difference images of the display and of VRAM, and reports differing pixels,
the first differing GP0 packets (ignoring the bits the GPU ignores: the upper half of a textured
polygon's later UV words holds leftover memory) and differing state fields by name
(`tools/port/struct_layout.py` reads the struct layout from DWARF).

Findings:
- The game state diverged from frame 26 of the demo: Harry's run animation stopped advancing, so his
  position, the camera and the world drawn all differed (much of the "unrendered world").
  `Anim_DurationGet` calls `duration.variableFunc()` with no argument, but
  `Player_VariableAnimDurationGet(s_Model*)` reads the model: on PS1 `a0` still holds it. Under
  `SH_PORT` the model is passed. Now `g_SysWork`/`g_GameWork` match (except vertical blank counters
  and pad state) and the GP0 streams are identical through frame 400.
- The game reads the live sticks during the demo; PCSX2's pad rests at 7Fh, PS1 pads at 80h: sticks
  within 8 of the centre now read 80h.
- Rendering: with identical GP0 streams, ~20,000 of 71,680 display pixels differ, nearly all by one
  5-bit step (more often darker on the PS2): rounding in shading/blending/dithering. Next to fix.
- Open: the second demo (map2_s00 again) crashes ~7 s in, in world chunk streaming; timing dependent.

### Frame debugger

`python3 tools/port/frame_debugger.py`, then http://127.0.0.1:8765/ in a browser: for the frames
captured by `compare_frames.py`, a PS1-level frame debugger (`tools/port/frame_debugger/`):
- draw list decoded from both GP0 streams (type, vertices xy/colour/uv, texture page, CLUT, blending,
  dither, clip, offset, mask, texture window, raw words), draws whose commands differ marked;
- the display image (PS1, PS2 or difference) with the selected draw's triangles outlined; click a
  pixel to select the draw that last wrote it;
- blame: each differing display pixel attributed to the last draw covering it (coverage rasterised
  with the PS1's rules), ranked.
`?sync=1&frame=N&select=D` loads synchronously (headless screenshots for checking the page).

Frame 400: 2 draws differ in their commands (trailing 2x1 VRAM copies the PS1 sends); the top blamed
draws are all semi-transparent (fog). Last-writer blame can't tell which earlier draw introduced a
difference that a blend then carries along: that needs per-draw results (replay from the frame's
starting VRAM, and per-draw readback on the PS2).

### PCSX2 host GPU load: CLUTs out of the render target

PCSX2 showed ~99% (host) GPU in-game (the new-game cutscene dropped to 40 fps). Per-frame GS
statistics (printed with the libgpu line, `GpuGs_Stats`) in the demo: ~1,700 primitives, ~330
TEX0 changes, each one a CLUT load from the VRAM buffer, which is the render target (CSM2), and
50-85 check-mask (DATE) primitives; no texture sampled from the render target. A CLUT loaded from a
render target makes PCSX2's hardware renderer bring the target's contents back, for each load.
CLUTs now come from a CLUT cache (`clut_get`: one PSMCT16 row each at 0x2F0000, copied from
g_PortVram, dropped when VRAM over them changes); GP0 fills and VRAM copies also update g_PortVram.
Frame comparison with DuckStation unchanged (19,474 / 20,847 differing display pixels at frames 100
/ 400). `compare_frames.py` and the frame debugger now compare each side's own displayed buffer (the
double buffer's parity can differ between runs).

### Opening cutscene at 60 fps

Test: title -> START -> Normal (`build/port/input.txt`: `34 start`, `38 cross`), the opening FMV is
skipped, then the in-game cutscene (~49-66 emulated s) and gameplay. `pcsx2_run.py --gameplay 30`
ends a run after 30 emulated seconds of unbroken gameplay (the port prints `port: gameplay` /
`port: gameplay ended` on entering and leaving it) and counts it as passed. Measured with the
heartbeat (frames per 60 vertical blanks), PCSX2's software renderer, `SH1_FPS=60`.

Before: the cutscene ran 30-45 fps for several seconds (frames needed ~1.1 fields: game update
~2.9M EE cycles, of it ~2M in the GTE, plus ~2.5M translating GP0 to GS packets).
- GTE: constant command words for RTPS, RTPT, NCLIP and DPCS go straight to their own entry points
  (`Gte_CmdRtps` ... in gte.c, chosen in gte.h at compile time; not in `SH1_PROF` builds, which
  count commands), and DPCS has a 32-bit path when the far colour is within +-2^18 (`dpcs_tiny`).
  `tools/port/gte_test/run_ee.py`: all tests still match.
- GS translation: polygons are packed straight into REGLIST data (no intermediate vertex structs);
  quads go out as one 4-vertex triangle strip instead of two triangles (the same pixels:
  `compare_frames.py` counts are identical with strips on and off, and against the previous
  renderer); texture page and CLUT caches are looked up through direct tables; page uploads copy
  whole rows into the DMA packet. A 4-entry state memo (replaying recorded registers) measured
  slower than the single-entry one and was dropped.
- The port's own code is compiled at -O3 (`SH1_PORT_OPT` overrides): 5-7% of a frame more idle
  time in the cutscene. The game's code gains nothing from -O3 and stays at -O2.
- Outside gameplay, MainLoop always waited for one more vertical blank after a frame, so a frame
  slightly over one field fell to 30 fps. With `SH1_FPS=60` it now waits only when the frame took
  under one vertical blank, as gameplay does.
- The fixed debug frame dumps (frames 30 ... 2400) each stalled the game for about a second of
  emulated time, which showed up as fake frame drops; they now need `SH1_DUMP=1`. `dump` lines in
  `host:input.txt` (`src/port/ps2/input_ps2.c`) still work.

After: cutscene 1019 frames in 1020 vertical blanks (60 fps every second, 8-29% EE idle); 30 s of
gameplay at 60 fps every second (7-15% idle). The only dips are while the map loads after choosing
the difficulty: disc reads are synchronous, each read stalling 0-8 vertical blanks behind the fade
(the cutscene itself reads nothing). Frame comparison with DuckStation (attract demo): 20,582 /
20,911 differing display pixels at frames 100 / 400, the same as the previous renderer now that both
sides show the same buffer (the ±1 rounding still open).

### Asynchronous disc reads; faster warp sweeps

`src/port/ps2/libcd_ps2.c`: CdRead now starts a request and returns; CdReadSync(1), which the file
queue and the sound loader poll, reports the sectors still to come and moves the request along (a
finished DVD chunk is copied out and the next one started); CdReadSync(0) waits. Chunks are up to 32
DVD sectors: a run of raw HILL. sectors is one contiguous byte range, so ~27 PS1 sectors come in one
DVD read instead of one each. Two bugs found on the way:
- The sound code sends `CdlNop` every frame (status poll). Treating every command but Setloc as
  "stop the read" ended file reads early and reported them complete: half-loaded files (a TIM with
  no header: null pointer in `Fs_QueuePostLoadTim`). Only Stop/Pause/Init end a read now, and that
  read then reports an error (the file queue resets and retries).
- map1_s04 reads an empty file (0 sectors). The synchronous code finished it at once; the async code
  failed it, and the queue retried forever (the game sat on the title screen). 0 sectors now
  completes immediately.
The loading after choosing the difficulty went from 37-51 fps to 50-60 fps in its worst seconds.

Warp sweeps (`host:warp.txt`, `Port_WarpActive` in game.h): no boot logos, no 20 s title wait, only
the first demo (never the others or the intro movie); a warped map's own loads (area transitions,
e.g. map0_s00 into map0_s01) count as part of its test, and only the demo's boot load is redirected
(`Port_MapLoadWarp(mapIdx, demoBoot)`). `pcsx2_run.py --progress TEXT` fails a run when TEXT hasn't
appeared for `--progress-seconds` emulated seconds (a game on the title screen still draws frames,
so the stall check alone missed it); the sweep uses it with `port: map load`. One boot reaches the
first map ~3 s (real time) after boot; 15 s per map takes ~2.5 s.

Sweep (`SH1_FPS=60 SH1_BENCH=1`, 15 s per map): 43/43 pass. Most maps 60 fps; below: map5_s00 (38.9
avg, 10% low 20), map0_s00 (46.3, 30), map7_s01 (57.0, 33), map7_s02 (58.6, 47); 30 fps with idle
left (a non-gameplay state): map1_s06, map7_s00, map7_s03; tight: map6_s00 (4-7% idle). Many maps
sit at a steady 66-67% idle: probably a fade or black screen rather than the map itself (the maps
known to fail from the title demo pass by not crashing); the sweep checks survival, not rendering.

### Mesh rendering on the EE: exact batches instead of per-call GTE

The user chose exact results for any GTE work moved off the per-command path. The function profile
(`SH1_EXTRA_CFLAGS=-finstrument-functions SH1_PROF=1`) in the cutscene: Gfx_MeshDraw ~1.9M cycles
per frame (instrumented), the vertex pass func_80057B7C ~1.06M, snow ~0.6M.
- `Gte_RtpBatch` (gte.c): RTPS for a whole vertex array in place, exactly (no per-vertex FLAG or
  register bookkeeping). func_80057B7C batches all triples but the last, which still goes through
  the GTE. Vertex pass ~1.06M -> ~0.36M.
- Port Gfx_MeshDraw: the back-face test is computed inline (the GTE's NCLIP formula); the fog path's
  two colours per corner depend only on the vertex's fog byte, and the lit path's colour only on the
  corner's light byte, so they are memoized by that byte for the call (first use through the GTE).
  4-8 DPCS per face become a few per mesh.
- Checks (`SH1_EXTRA_CFLAGS=-DSH_PORT_CHECK_BATCH`): the batch against the GTE per triple (0
  mismatches, 4.8M vertices over all maps); Gfx_MeshDraw's original and port versions into the same
  packet area and OT, compared byte for byte (0 mismatches, 86,016 meshes over all maps, 49,152 in
  the cutscene run). A first run showed mismatches from the check harness itself (96-entry copies;
  meshes have up to 99 vertices).
- Cutscene idle (average per second) ~20% -> ~27%, gameplay ~18% -> ~24% (both 60 fps); sweep:
  map2_s00 idle 25% -> 31%.
- VU: an exact VU0 transform would need split-float arithmetic for the dot products and the UNR
  divide (25-34-bit intermediates), with per-vertex table reads through the 16-bit integer unit and
  copies in and out of VU memory: little or nothing to gain over the EE batch, so not done.
- Not helped: map6_s00 (4-7% idle; other drawing paths, func_8005A900/func_8005AC50); map5_s00 and
  map0_s00 drop frames with 30-50% idle, so something other than EE time limits them.
- `Vw_MultiplyAndTransformMatrix` (vw_calc.c): character bone matrices transformed via EE MMI
  (`PHMADH` SIMD) with a portable scalar C fallback. Eliminated all 318 MVMVA calls/frame from this site.
  Tested with `-DSH_PORT_CHECK_BATCH` (0 mismatches across >540,000 checks) and `bench_cutscene.sh`
  (zero pixel diffs).


### Playing to the cafe: four port bugs

The user played the new game in PCSX2 (`pcsx2_run.py --realtime --stall 0`, disc-read trace
`SH1_TRACE_CD=1`); each problem was found from save states or logs.
- **Map load hung on a black screen** (THR). `world_diag.py` on the save state: four active chunks
  needed 10 full-page textures for 8 slots, so materials stayed without textures and
  WorldGfx_ChunkInitCheck never passed. The same transition in DuckStation (`duckstation_world_watch.py`,
  a gdb script logging every map load with PS1 RAM) spawned the player at x -96.4 instead of 0.4:
  `g_MapPoint.positionX` had been zeroed. `s_EventData* g_ItemTriggerEvents[];` (unsized: one entry
  with GCC; 0x14 bytes in the PS1 layout) was cleared as 5 entries, over `g_RadioNoise` and
  `g_MapPoint`. Sized under SH_PORT. An audit of PS1 symbol sizes against the port found no other
  overrun.
- **Harry walked in circles, controls dead** (heavy area). Two save states: PADMAN's DMA buffer was
  live, the port's receive buffer frozen. The pad was refreshed only after a VSync wait, and 60 fps
  frames running late never wait. Now refreshed at most once per vertical blank from any VSync
  call.
- **Held Square dropped every half second** (after that change): half the reads came while the pad
  was executing a command, and a failed read marked the pad disconnected for a frame. Failed reads
  keep the previous data; vibration (padSetActDirect) is only sent when the values change. A log
  line counts reads that weren't ready.
- **Two freezes in the alley** (child ghosts). The first: Stalker code with a garbage character
  pointer: `func_8006FD90` (los.c) passed a `VECTOR3` local to `Ray_CharaTraceQuery`, which writes an
  `s_RayTrace`; on the PS1 the rest landed in the next local (which the code then reads), on the EE on
  saved registers. The second: the PC (from the save state's `cpuRegs`) in `func_800CC8FC`
  (particle.c), whose hull walk read `((DVECTOR*)&sp10)[i + 8]`, meaning `sp30[i]` only in the PS1's
  stack layout. Both fixed under SH_PORT; a search for other indexing past stack locals found none.

New tools: a load watchdog (`libgpu_port.c`: map load screen with an empty file queue for 10 s
writes `host:ramdump.bin`, `src/port/ps2/debug_ps2.c`), `tools/port/world_diag.py` (world streaming
diagnosis from a PCSX2 save state, a port RAM dump or PS1 RAM), `tools/port/duckstation_world_watch.py`
(DuckStation at normal speed with a gdb script dumping PS1 RAM and the diagnosis at every map load).
The per-second libgpu line also shows sysState and the file queue.

Performance from the session: the alley end and onward ran 34-55 fps with the EE fully busy for
long stretches (e.g. 207-297 s at 46-54 fps); the cafe cutscene plays.

### Real hardware over Ethernet (phases 0 and 1)

Setup: VirtualBox adapter switched to bridged (the PS2 finds the UDPFS server by broadcast, which
NAT doesn't pass); Neutrino v1.8.0 (now github.com/ps2max32/neutrino) unpacked to `~/ps2tools/`, a
copy for the USB stick in `build/port/usb/neutrino` with `config/system.toml` defaults
`default_bsd = "udpfs"`, `default_dvd = "udpfs:sh1_ps2.iso"`, the PS2 at 192.168.1.10
(`config/bsd-udpfs.toml`). On the VM: `python3 -u ~/ps2tools/neutrino_v1.8.0/udpfs_server/udpfs_server.py
-d build/port` and `python3 tools/port/ps2_log.py` (log in build/port/ps2.log). Started from
launchELF, the game boots from the VM (~1.4 MB/s reads).

- Neutrino's `ministack.irx` replaces the IOP tty device with UDP broadcasts to port 18194, so the
  port's console output (printf goes through the IOP tty) arrives on the VM without changes.
- **New game crashed on hardware** (also from the internal HDD with OPL): `CRASH: TLB load at
  pc=00114798 badvaddr=00000008`, Gfx_EffectsUpdate reading `g_WorldGfxWork.mapInfo->waterZones`
  while mapInfo is NULL in the first frames. The PS1 reads RAM there; PCSX2 logs "TLB Miss" and goes
  on (dismissed earlier as harmless); the PS2 faults. Fixed under SH_PORT (no map: no water zones).
  A sweep plus the cutscene run then showed only one other TLB miss (strncmp on map2_s01, warped).
- **Second crash** entering map0_s01: WorldMap_ChunkLoadStateGet with a loaded queue entry but no
  header (`ipdHdr` NULL): invalid under SH_PORT.
- **NULL-read safety net**: at startup on hardware, one TLB entry maps addresses 0-0x1FFF read-only
  to a page of zeros (`map_zero_page`, crash_ps2.c), so remaining NULL+offset reads return 0 as in
  PCSX2; writes still fault. Skipped when `host:sh1_ps2.iso` opens (PCSX2), where the TLB Miss lines
  are how such sites are found.
- **Crash restart**: crash_ps2.c counts down 6 s (over the network log), then `LoadExecPS2`s
  `cdrom0:\SHPS_000.01;1`; Neutrino's EE core loads it from the emulated disc, i.e. the image on the
  VM. Triangle held for 3 s keeps the crashed state. Tested with `SH1_TEST_CRASH=1` (reads address 8
  30 s after boot). First attempts reset the IOP ourselves and loaded USB drivers to restart
  Neutrino from the stick: with screen colours per step (GS BGCOLOR) the reset itself worked but
  the module loader never answered afterwards, also with Neutrino's `-gc=3` (unhook syscalls).
- Rebuilding the ISO while the PS2 ran from it gave garbage crashes (mixed old and new data);
  `make_iso.sh` now writes a temporary file and renames it over the image.

### Remote control (phase 3)

- `src/port/iop/sh1agent/` (built by port_link.py with ps2sdk's IOP makefiles, embedded with bin2c):
  binds UDP port 62968 in Neutrino's ministack (its "mstack" exports: udp_bind, udp_packet_init,
  udp_packet_send_ll); the first two payload bytes are the command ("RS", "PI"). It answers the
  sender and copies the command into a 16-byte EE mailbox by SIF DMA (address passed as "mb=0x...").
  In PCSX2 the module doesn't link (no ministack: -200) and nothing changes.
- `src/port/ps2/agent_ps2.c`: loads it (SifExecModuleBuffer after sbv_patch_enable_lmb), reads the
  mailbox uncached from the vertical blank handler, wakes an agent thread at priority 2 (the game:
  64) which restarts the game the crash reporter's way (Port_Restart).
- `tools/port/ps2_ctl.py ping|restart|deploy`; `deploy` first has `tools/port/udpfs_serve.py`
  (Neutrino's udpfs_server.py loaded unchanged, plus a watcher thread) reopen open files whose path
  now names a new file (make_iso.sh's rename), then restarts: the game comes back on the new build.
- The startup line shows the build time (`SH1 port: starting (build ...)`).
- On the PS2: ping answered; restart ~7 s from command to the game starting; deploy: build at
  19:48:28, the restarted game reported that build.

Workflow now: `python3 tools/port/udpfs_serve.py` and `python3 tools/port/ps2_log.py` running on the
VM; Neutrino started once on the PS2 from launchELF; then build and `python3 tools/port/ps2_ctl.py
deploy` for each change.

### The item-pickup crash: a void prototype for libgs's GsTMDfast*LFG

On the PS2 (and, it turned out, in PCSX2 once the first attract demo was allowed to run to Harry's
item pickup) the game crashed in the world-map code with garbage pointers. Memory read over the
network (`ps2_ctl.py md`) showed `g_WorldMapWork` and the variables before it (g_OtTags1,
g_Map_GfxPackets) full of mesh packets: GsOUT_PACKET_P had been moved into the game's variables.
A check of GsOUT_PACKET_P around every wrapped recompiled call (`SH1_WATCH_PACKET=1`:
RC_WATCH_BEGIN/END in the recompiler's wrappers, Port_WatchBegin/End in crash_ps2.c) caught
GsSortObject4J setting it to 0 or 1; logging the GsTMDfast* calls it makes through the GsFCALL4
table showed GsTMDfastTG3LFG returning the packet pointer unchanged. `include/gpu.h` declared the
four GsTMDfast*LFG functions `void` (Sony's libgs.h: `PACKET*`), so the recompiler generated `void`
wrappers that dropped the returned packet pointer, and GsSortObject4J read whatever was left in v0.
Declared `PACKET*` (only their addresses are used in C, so the PS1 build is unchanged); every other
void wrapper matches Sony's declarations. The earlier chunk-header range check in
WorldMap_ChunkLoadStateGet treated a symptom of this; it stays as a safety net.

Notes: the EE's hardware data breakpoint (ee_debug's ee_dbg_set_bpw/bpv) never fired under
Neutrino, even for a deliberate store; the software checks did the job. GsLinkObject4 rewrites a
model's first primitive header on purpose (group count and type).

- 2026-10-08 (hardware): attract-demo freeze found. The main thread was spinning in gpu_gs.c download_rows (water effect StoreImage → GS→EE VRAM readback), waiting on VIF1 DMA (D1_CHCR) forever. It found this by reading the stack over the network: `ps2_ctl.py md 0x1FFC000 0x4000`. Fix: clear FINISH, wait for the GS's FINISH before turning the bus around, add spin timeouts, and on a stuck transfer stop D1 and reset VIF1 with a log line ("VRAM download stuck"). The vblank EPC sampler (`ps2_ctl.py where`) shows only the kernel inside INTC handlers, so use the stack instead. A remote restart from this state hangs, so press RESET.

### Hardware freezes: the network console (2026-10-10)

Under Neutrino, every printf goes to ministack's udptty, and it can stall for good during disc loads. On the old builds every thread that printed then waited forever, so the PS2 looked dead: a black screen, no log, no `where`, but `ping` still answered (the IOP agent replies without printing).

Now:
- The game's output goes through its own path: a ring buffer on the EE (log_ps2.c), sent to sh1agent.irx by SIF DMA, then broadcast by the agent to `ps2_log.py`.
- sh1spu's messages are buffered and printed by a low-priority thread (sh1spu/log.c).

If the PS2 seems stuck:
- `python3 tools/port/ps2_ctl.py ping`, twice a few seconds apart. The `EE:` line shows the vertical blank count, with the change since the last ping, plus the frame count, the game thread's state (1 running, 2 ready, 4 waiting; wait type 2 = a semaphore) and log bytes pending/dropped.
  - Count not moving: the EE stopped.
  - Count moving but frame count not: the game thread is stuck.
- The TV's colour, from the watchdog thread:
  - red: the EE-to-IOP SIF DMA is stuck;
  - yellow: no frame for 10 s;
  - black: the EE stopped (or nothing is wrong yet).
- When the PS2 is stuck, `deploy` can't restart it. RESET doesn't bring Neutrino back on this console: power-cycle it (hold the power button) and start Neutrino again from uLaunchELF. The server switches to the newest build on `touch build/port/.udpfs_refresh` (deploy does this too).

### Split Head, the display straight from VRAM, no EE copy of VRAM (2026-10-16/17)

Split Head (first boss, map1_s05), two more spots of decompiled code relying on the PS1 stack layout:
- `sharedFunc_800D3388_1_s05` reached `sp38[i]` as `&sp18[i * 16] + 32`; in GCC's frame the copy overwrote a saved global base, and the next store crashed (console log: `addr store at pc=001EF850 badvaddr=FFFF9840`).
- `sharedFunc_800D2D74_1_s05` read `sp20[unkIdx + 4]` (meaning `sp50[unkIdx]`): garbage hitbox height, so Harry's vertical aim (the middle of the hitbox) was wrong.
- Both under `SH_PORT` with the arrays named directly; an audit for the pattern is on the planning list.

Gemini's bone matrix MMI (`Vw_MultiplyAndTransformMatrix`), reviewed: the translation now takes the low 16 bits of `inMat->t` as `gte_ldlvl` loads them into IR, with MAC's wraparound; `hi`/`lo` added to the asm clobbers (PHMADH writes them). Warp sweep with `-DSH_PORT_CHECK_BATCH`: 43/43, 0 mismatches over ~491k calls.

GTE control writes: `g_GteCtrlDirty` has a bit per register, so a rotation write no longer unpacks all three matrices and redoes the TR/BK/FC range checks (gte_test seeds 0-5: 0 differences).

Display (display_ps2.c, gpu_gs.c `GpuGs_DisplayShow`):
- The PS1 display area is scanned out straight from the VRAM image in GS memory (DISPFB1 at 0x300000, PSMCT16, DBX/DBY = the PS1's display start); the copy into gsKit's 640x480x32 buffer is gone. PS1 15-bit pixels have the GS's 16-bit layout; PMODE uses a fixed alpha so bit 15 doesn't blend.
- Single-buffered screens: the 448-line interlaced title and inventory redraw the area on screen. Shown live, the TV caught them half redrawn (text missing). Checked: no progressive screen in the game is single-buffered (all maps and Demo cutscenes). Interlaced 480-line screens with DFE off are shown from a snapshot (16-bit, SNAP_ADDR 0); everything else straight from VRAM.
- The PS1's own way (DFE off: no drawing on the lines of the field being scanned out) is implemented with SCANMSK behind `SH1_INTERLACE_FIELDS=1`. VRAM held the complete text at every swap, but PCSX2's output lacked most of it with either field mapping; to test on hardware.
- Mode switches: the output is blanked for 3 frames (BGCOLOR black) after a change; gsKit's setup showed its stale buffer. `SH1_MODESWITCH=0|1|2` chooses gsKit's full setup (a GS reset, which stops the video signal, then SetGsCrt), SetGsCrt alone, or SMODE2 and the display window only; each switch logs its setup time and the longest interval between vertical blanks. PCSX2: setup 1-17 ms / 0.4 ms / 0.02 ms; no vertical blank gap (PCSX2 doesn't model signal loss). Hardware test pending.

VRAM: the GS image is now the only copy.
- The software renderer (gpu_soft.c) and the compare mode are retired; comparisons are against DuckStation.
- `g_PortVram` (1 MB of EE RAM) is gone, with two more 1 MB buffers that only served it. LoadImage uploads straight from the game's data; fills and VRAM copies are GS-only (no CPU mirror loops); StoreImage reads back just its rectangle.
- 4/8-bit pages and CLUTs are read back from the GS on a cache miss. Fills write their value into cached pages and CLUTs they touch instead of dropping them: the screen clear covers rows of an 8-bit page whose textures sit above the frame buffer, which otherwise meant 2-4 readbacks a frame.
- Frame dumps and DuckStation captures read VRAM back in 32-row bands. (A `malloc` of 1 MB for them corrupted the file I/O RPC: TLB miss in sifrpc `_request_end`.)
- gsKit's frame buffer is 16-bit and only used by its setup: the texture page cache has 37 slots (was 28).
- Kaufmann benchmark: all 10 frame dumps identical to before.

DrawOTag, first pass: see PS2 Optimizations.md (2026-10-17). `SH1_LITEPROF=1` adds cheap cycle counters around its parts. 1,763 → 1,523 k-cycles a frame in the Kaufmann cutscene, frame dumps identical.

DrawOTag, second step (2026-10-17): quads that fit are written straight into the packet (Kaufmann 1,523 → 1,396 k-cycles a frame, dumps identical). New speed benchmarks: Demo #36 CybilFightStart (map6_s04 func_800E1D50, renamed in the Demo menu) and #50 MapEvent_KaufmannBarFightCutscene. Their frame dumps change with timing alone (the same source with or without the light counters differs by a few to a few hundred pixels), so exactness stays checked on Kaufmann. CybilFightStart's frame is mostly the game update (6.3M cycles), led by func_80057658's ~3,550 RTPS a frame (per-vertex point light).

func_80057658 (world mesh point light, ~3,550 RTPS a frame in CybilFightStart): each vertex set RT row 2 and V0 to the same vector, so every RTPS followed a matrix write and a full matrix refresh. `Gte_CmdRtpsRow2` does the three in one, exactly (check build: 0 mismatches over 6.2M calls, all registers). CybilFightStart 27.5 → 30.8 fps, bar fight 40.8 → 42.4 (benchmark builds).

func_80057658 batched: the loop's vectors are collected, `Gte_RtpsDotBatch` computes all but the last, the original arithmetic runs on the results, and the last vertex goes through the GTE with the FIFO preset. Check build: 0 mismatches over 46k meshes (light values, scratch data, all GTE registers). CybilFightStart's game update 5.85M → 5.45M cycles a frame.

The GTE emulator's EE code (MMI PHMADH dot products, MULT/MFHI multiplies, PLZCW) had never been in the game: `#ifdef _EE` guarded it, and only src/port/ps2/ is compiled with -D_EE (gte_test's EE checker passes it, so the tests ran it). Found from a `__clzdi2` call in the point-light batch. gte.c and vw_calc.c now also key on `_MIPS_ARCH_R5900`. All GTE tests and check builds: 0 mismatches. CybilFightStart takes 7.6% fewer vertical blanks, the bar fight 5.2%.

Point light, second pass in registers: only the final scratch values are written (the last vertex's, as the original leaves them). 0 mismatches over 117k meshes; CybilFightStart's light arithmetic 469 → 182 k-cycles a frame, 1,702 → 1,662 vertical blanks. Also learned: MMI dot products (PHMADH) with a 128-bit store and reload around each are slower than scalar multiplies here; rt_dot3 was removed (mesh transform 640 → 613).

World mesh packets: CybilFightStart's faces all take Gfx_MeshDrawPort's lit path; it now runs in locals with the colour memo checked once per mesh (479 → 404 k-cycles a frame, packets identical). The original's screen bound is a u32, so its bound test is unsigned; a signed first version drew extra faces until the mesh check flagged it.

DrawOTag, third step: `polygon()` is now eight copies, one per command shape (gouraud, quad, textured as constants), and `tex_state` memoizes TEX0/TEXCLUT by page, CLUT and raw until a cache slot changes. DrawOTag 1,409 → 1,371 k-cycles a frame (CybilFightStart), 1,398 → 1,355 (Kaufmann, dumps identical). A texture change still costs ~400 cycles, mostly the A+D block around it; PCSX2 doesn't model the EE caches, so the rest of DrawOTag's tuning wants hardware numbers.

All-cutscene profile (2026-10-17, PCSX2): two sweeps of the 52 Demo cutscenes, a benchmark build with light counters and an instrumented function profile. Busy time per displayed frame (elapsed × (1 − idle)) is the measure; the demo fps line counts simulated vertical blanks in benchmark builds. Only CybilFightStart misses 60 fps by much (50.6). In the heavy cutscenes: DrawOTag 34%, point light 18%, mesh transform 12%, mesh packets 9%, rest 28%. New targets: map6_s02's effect grid (func_800D2364), the world mesh's fog paths, character mesh packets and snow particles. See PS2 Optimizations.md.

map6_s02 effect grid (func_800D2364): each layer's first 284 points now go through `Gte_RtpGrid` (gte.c), a batched RTPS over a lattice that steps MAC1-3 by R·du and R·dv instead of multiplying; the last triple and two RTPS still use the GTE. Check build: 0 mismatches over 4.7M points. The function went 1,179 → 653 k-cycles a frame; busy time per frame −12% and −19% in map6_s02's two cutscenes.

World mesh, remaining face loops: the fog, lit-and-fogged, lit-with-black-fog and flat loops of Gfx_MeshDrawPort now share `Port_FaceTest` and run in locals with the colour memos; the mesh check counts meshes per path (0 mismatches over 1.47M; the flat loop didn't run in the benchmarks). Mesh packets 617 → 478 k-cycles a frame in map2_s01, 481 → 399 in CafeCutscene; busy time −2% to −3%.

Tried and reverted: func_8005AC50 (character mesh packets) in locals. Exact (packet, OT and scratch check: 0 mismatches over 1.2M meshes) but only ~2% of a 110-130k cycle loop. Also measured: the point light's vertex visits repeat a vertex 15% of the time, too little to share per-vertex work.

Gameplay profile: the five title-screen attract demos, uncapped in benchmark builds (the demo's own timing, no vertical blank waits), now logged per demo (`attract demo N started/stopped`). In PCSX2 they average 89-243 fps, and the slowest second is 69 fps. The time splits as in the cutscenes, plus snow particles outdoors. The function profiler hung once its 1,024-slot table filled (prof_fn.c); now 8,192 slots and no endless probing.

Snow particles in gameplay (attract demos 0-2): light counters show 170-200k cycles a frame for ~190 flakes (6-8% of busy time), of which ~70k is the flakes' RTPS. Batching those would save ~1.5% at most, so the snow is left as is. The counters stay (light-counter builds only).

## Saving to the PS2 memory card (2026-10-17)

The game's own save code (src/bodyprog/memcard.c) runs unchanged. Underneath it, the port now provides what the PS1 gave it:
- **libcard and the BIOS file functions** (src/port/psx_card.c): `_card_info`, `_card_clear`, `_card_load` and the raw sector calls deliver the SwCARD/HwCARD events the game polls. `open`/`read`/`write`/`lseek`/`close`/`firstfile`/`nextfile`/`erase`/`rename`/`format` work on `buXY:` paths; memcard.c maps its calls to `Port_Bu*`, since the plain names belong to the PS2's C library.
- **One PS1 card per PS1 slot,** a 128 KB image in the standard PS1 format (an emulator's .mcd), kept in EE memory. The game sees a 15-block PS1 card; its save file BASLUS-00707SILENT00 is one block of 11 save slots. Multitap slots report no card.
- **On the PS2 card** (src/port/ps2/memcard_ps2.c, rom0:MCMAN/MCSERV with libmc): PS1 slot 1 is the card in port 1, slot 2 port 2. Everything is in one save folder, `BASLUS-00707SH1` (CARD.MCD, icon.sys, SH1.ICO, a flat icon made from the game's PS1 save icon), created at the first write. Writes go straight to the card. Card queries are asynchronous, because the game asks about every slot each frame: one query per slot at most every half second, its event delivered from TestEvent when it ends.
- **Testing:** `pcsx2_run.py --memcards DIR` gives PCSX2 its own card folder for a run (PCSX2 creates unformatted cards). `SH1_MC_FORMAT=1` builds format an unformatted card in port 1 and leave a copy of the PS1 card at host:card1.mcd after every write. `SH1_MC_SELFTEST=1` runs the file functions once at the first card load. The self-test passes in PCSX2, and the host copy is a valid PS1 card image (checksums, directory, data).
- Found on the way: the function profiler's table filled up in long runs and hung the game (prof_fn.c, fixed earlier today).
- **Importing PS1 saves:** `SH1_MC_IMPORT=1` builds turn a PS1 card image at host:importN.mcd into the save when the PS2 card in port N has none. The two `.mcs` exports (BASLUS-00707SILENT00 and 01) were put into one image this way and imported into PCSX2's Mcd001.ps2, formatting the card first with `SH1_MC_FORMAT=1` (2026-10-18). A normal build then loads the image from that card.

## Console test, 2026-10-18

Build with the light counters, memory card saving, and everything since the last console test.
- **The PS2 memory card works on hardware.** The card in port 1 was found, and the save folder pasted from a .psu (tools/port/mc_export_psu.py) loaded. The card in port 2 was found too, with no save folder yet.
- **Stalls from tiny VRAM readbacks** (gpu_gs.c download_rect). Each new 16-colour CLUT entering the cache was read back from the GS as 16x1 pixels: 2 quadwords. On hardware, these reads never finished. The DMA takes the VIF1 FIFO's data in 8-quadword bursts, and a shorter transfer is never sent; PCSX2 doesn't model this. Each one waited ~1 s for the timeout, until the watchdog reported no frames for 10 s. Readbacks are now rounded out to 64-pixel boundaries (128-byte rows, whole bursts). There have been no stuck reads since, and Kaufmann's dumps are unchanged. This appeared once the EE copy of VRAM was gone (2026-10-17), since CLUTs are now read from the GS.
- **Wrong digits** (save screen "Time: 2:0U:0T", "0F"; ammo "0>"). Gfx_StringDrawInt (text_draw.c and text_draw_jp.c) computed `(val / 10) >> 32`, the decomp's match for the PS1 compiler's multiply-high division. On a 32-bit value that's undefined, and the EE's GCC made the quotient 0, so each number came out as one "digit" holding its whole value. Now a plain division under SH_PORT.
- **Loops cut short:** game code is now compiled with -fno-aggressive-loop-optimizations (ee_compile_check.sh). The decomp declares some arrays smaller than the PS1 code uses them; the loops run into the next variables, as on the PS1. GCC took the out-of-bounds access as proof that the loop stops earlier. The item screen's setup loop stopped after 7 of its 10 displayed items, so items 7-9 kept stale transforms; the lighting loop was cut the same way. The other flagged loops (GameFs_WeaponInfoUpdate's weapon animations, map6_s04, func_80066184) compiled unchanged. No such warnings remain.
- **A hang after a remote restart** (ps2_ctl.py deploy) on the title screen. The vertical-blank count stopped, all threads were waiting, and the EE sat in the kernel. It didn't happen on a cold boot of the same build, so it's in the warm-restart path. To investigate if it recurs. Recovery: power-cycle and start Neutrino again; RESET doesn't bring Neutrino back on this console.
  - Second time (2026-10-18, build 19:31:57): the same point. After the warm restart, the boot passes the Konami logo and the 640x448i mode switch and checks host:warp.txt. About 2 s later, at gameState 1 step 3, the log stops. Next would be the memory card notice and the intro movie, with another mode switch. Cold boots of the same builds are fine. Suspect: the full GS setup of a mode switch (SH1_MODESWITCH=0: GS reset + SetGsCrt) on the state the previous session left; to try SH1_MODESWITCH=1/2 after a warm restart.
  - Third time (build 19:52:26): the same point (gameState 1 step 3, the Konami logo's fade). ps2_ctl.py ping: EE at pc 00081FE0 (kernel), game thread waiting on a semaphore, and **IOP DMA channel 10 (SIF0, IOP to EE) busy: CHCR 01000701**. The first hang's ping showed the same. So after a warm restart an IOP-to-EE transfer doesn't complete (sh1spu's status block, the log or sh1agent's mailbox), and the EE waits in the kernel. A better lead than the mode switch, which comes later. To investigate: which module sends it, and whether the EE's SIF0 receive (EE DMA channel 5) is set up again after the restart.

## 2x rendering: what reads the screen (2026-10-18)

Inventory build `SH1_SCREENREAD=1` (gpu_gs.c): logs every use of the screen's contents, with its texture page and CLUT or rectangle, the caller of LoadImage/StoreImage, and the frame. The screen means the last two display areas. Covered: textured polygons and sprites (by their UV range), copies from or into the screen, uploads into it and readbacks of it. Run over all 52 Demo-menu cutscenes, the five attract demos and the warp sweep (43/43 maps).

Found (the source search covers maps the runs didn't reach):
- **Feedback / motion-blur effects:** sprites or quads textured from the 15-bit pages over the frame buffers, drawing the other buffer back over the frame with blending. 16 sites in 13 functions:
  - Screen_BackgroundMotionBlur (background_draw.c)
  - sharedFunc_800CE688_1_s03 (unk_draw_m1s03.c: map1_s03, map5_s01, map6_s01, map6_s02)
  - map1_s05 func_800D4D1C
  - map4_s01 func_800D3420
  - map4_s03 func_800D8FC0
  - map6_s00 func_800EB11C
  - map6_s02 func_800D2364 (x2)
  - map6_s04 func_800E3EF4, func_800E558C
  - map7_s00 func_800D1604 (x2)
  - map7_s02 func_800DD2D4, func_800DE1FC (x2)
  - map7_s03 func_800E3F30
  - Seen in the runs: map6_s02's two cutscenes, map7_s02 func_800DE1FC, attract demo 0 (map2_s00) and map2_s01.
- **Lens flare occlusion test** (water.c func_8008D78C / func_8008D850): an 8x8 copy from the screen at the light's projected position to (784 + 8 x buffer, 112), then a 2-pixel StoreImage2 next frame: the flare shows when they're white. The only CPU read of screen contents. Seen in many cutscenes: map1_s06, map4_s01, map5_s03, map6_s01, map7_s01/s02, the phone call.
- **Nothing else:** no polygons textured from the screen, no uploads into it, no copies into it. (The log's StoreImage bands were the benchmark build's own frame dumps.)
- **FMV movies:** don't use the screen in the port (movie_ps2.c shows them from their own 32-bit buffer).
- **Menus and title (console, 2026-10-18,** inventory build with the game state logged per case). Visited: boot, the main menu, Load, the load status screen, Options, the inventory, the map, the save screen and gameplay.
  - **None of the menus use the screen** (inventory, map, save, load, options, status).
  - **The main load screen** draws the last frame back with Screen_BackgroundMotionBlur (already listed).
  - **New kind: images uploaded into the frame-buffer area.** The Konami logo at boot, and the title screen's background, are 8-bit textures in VRAM at (64,0)-(192,512), inside the area of the frame buffers; the title's CLUT is at (224,15). The game uploads them there and draws sprites from them. They aren't screen contents. Under 2x they must keep sampling the 1:1 VRAM, so the renderer has to tell areas last drawn into (the 2x buffer holds them) from areas last uploaded (the 1:1 VRAM does).
- **Not covered:** the credits (only at the end of the game).

## 2x renderer prototype (2026-10-18)

Build flag `SH1_HIRES=1` (src/port/ps2/gpu_gs.c, display_ps2.c); normal builds are unchanged.
- **Hires buffer:** VRAM's frame-buffer area, x 0-319 and y 0-511, at twice the resolution: a 640x1024 PSMCT16 buffer at GS address 0, shared with the snapshot area and gsKit's frame buffer. To make room, the texture page cache drops from 37 to 27 slots (CACHE2_ADDR 0x140000).
- **When it's on:** whenever the PS1 display is 320 wide and progressive (gameplay, cutscenes). The GS mode is then 640 x 2h interlaced (480i), scanning out the hires buffer at (2x, 2y).
  - Mode changes turn it off before gsKit's setup (which clears the bottom of GS memory) and on again after.
  - Movies turn it off: their buffer is in the same memory.
- **Drawing:** primitives whose clip area lies inside the hires area are drawn into it, with coordinates and the clip area doubled. Clip areas that straddle its edge, such as the full-VRAM one at boot, are drawn at 1x and logged ("hires: clip area ... straddles").
- **Feedback effects:** 15-bit pages over the area (motion blur, the load screen) are sampled from the hires buffer, with doubled UVs and the texture window adjusted.
- **Keeping VRAM in step, lazily:**
  - Uploads, fills and VRAM copies into the area are made in VRAM and scaled up into the hires buffer.
  - Anything that reads the area scales the hires buffer down into VRAM first: texture page and CLUT cache misses, StoreImage, and copies from it (the lens-flare test).
  - Scaling is a DECAL sprite, nearest pixel, with bit 15 kept. Uploaded images (the boot logo, the title, the font atlas at y 496-511 and its CLUT at (304,511)) survive the round trip exactly.
- **Bug found while testing:** scaling down to the area's bottom edge put texel row 1024 in UV's 14-bit V field, which wrapped to 0. That overwrote the font atlas, and subtitles became cyan stripes. Fixed by scaling down in bands of 128 rows, with the texture based at each band's page row.
- **PCSX2 results:**
  - The Kaufmann cutscene (Demo 4) and the attract demos run at 2x with no crashes.
  - Subtitles and the 3D scene are sharp, and the intro movie still plays.
  - Frame rates in PCSX2 say nothing about the GS cost (4x the pixels); that needs the console.
- **Prototype limits:**
  - Lines stay 1 pixel wide.
  - Vertices beyond about ±1024 1x pixels from the origin wrap in 2x (GS coordinate range).
  - The output is 480i (480p is still tabled).
  - The credits are untested.

### 2x on hardware, first pass (2026-10-18)

The Extra Options "Resolution" setting (x1/x2) switches the 2x renderer at run time:
- **Where it lives:** options.c under SH_PORT, stored in config.unused_26, so it is saved with the options.
- **Renderer side:** gpu_gs.c changes the GS memory layout between 37 and 27 page cache slots. display_ps2.c applies the option on the next frame.

First console test: toggling worked, with full speed at 2x (60 frames per 60 vertical blanks, 33-56% idle). But the picture had errors, and text got worse with each toggle. Compared with DuckStation at 2x internal resolution (Vulkan renderer; tools/port/duckstation_cutscene.py --scale 2 --shots N screenshots through the F10 hotkey under XWayland), two bugs showed up:
- **Room walls as flat bands** (in PCSX2 too, missed earlier). Doubling absolute coordinates overflowed the GS's 0-4095 vertex range for far-off vertices. Fixed: at 2x, vertices are sent relative to the PS1 drawing offset, doubled, with XYOFFSET moved to match (HR_X/HR_Y; E5 changes redo the state at 2x).
- **Text corrupted by toggling** (hardware). Every toggle and every texture/CLUT cache miss near the frame buffers scaled the whole area down and up again, font atlas and CLUTs included. Fixed:
  - Only 16x16 tiles actually drawn at 2x (the clip area at each state change) are scaled down.
  - Uploaded data never makes the round trip.
  - Scaling samples mid-texel (+1/8 texel up, +1/2 down), so it can't depend on the GS's sampling convention.

### Debug Options page (2026-10-18)

Port settings now have their own page, starting with Resolution (moved from Extra Options):
- **Opening it:** Triangle on the main options screen.
- **Look:** the same as Extra Options. It is that screen (same states, fades, drawing and controls) showing another list: options.c `s_PortDebugPage` and the `PORT_DEBUG_ENTRIES` table. A new entry needs a name, a highlight width, and its value and change cases (`Port_DebugEntryValue`, `Port_DebugEntryChange`).
- **Button conflicts checked:**
  - Triangle is one of the default Cancel buttons (all three default layouts: Triangle/Circle/Square). On the main options screen it now only opens Debug Options, and Cancel there is Circle/Square (OPTIONS_MAIN_CANCEL). This is temporary, until a better way in is chosen.
  - Triangle is ignored while Start or Select is held (the soft reset combos: Select+Start, and Start+Triangle+Square).
  - L1/L2/R1/R2 still open Extra Options.
- **Tested in PCSX2:** title -> Option -> Triangle -> Resolution x1 -> x2 (the options screen itself goes 2x) -> Triangle back (stays in Options, cursor on Exit) -> R1 opens Extra Options without Resolution.

### Color Depth: 32-bit drawing (2026-10-18)

How the emulators do it:
- **DuckStation's True Color** (gpu_hw_shadergen.cpp): the shader keeps 8 bits a channel through texture modulation and blending, instead of reducing to 5 bits each draw. Dithering is never on with it. Textures stay 5:5:5. VRAM reads for palettes and the CPU are converted back to 5:5:5. It has been off by default since 2020: too many games broke.
- **PCSX2's Dithering setting**, "Force 32bit" mode: treats 16-bit draws as 32-bit (PS2 games).

Here, the 2x shadow buffer became a shadow buffer with a scale (1x/2x) and a format (16/32-bit), set by the Debug Options Resolution and Color Depth (config.unused_26 bits 0 and 1):
- **32-bit buffer:** a PSMCT32 buffer drawn with dithering off and no 5-bit quantization of flat colours.
- **Mask bit:** alpha's bit 7. FBA and DATE use it for PSMCT32 as bit 15 for PSMCT16.
- **Syncing with 16-bit VRAM:** scaling down converts 32-bit to 16-bit (truncated). Scaling up widens 5 bits to 8, with bit 15 to alpha through TEXA.
- **Feedback effects** sample a 32-bit buffer as PSMCT24 with TEXA, so black is transparent as for 16-bit pages (the mask bit is lost there).

GS memory:

| Mode | Buffer | Page cache slots |
|---|---|---|
| 1x 32-bit | 640 KiB | 37 (unchanged) |
| 2x 16-bit | 1.25 MiB | 27 |
| 2x 32-bit | 2.5 MiB | 7 |

Kaufmann cutscene (Demo 4) in PCSX2:
- **Speed:** 1x/2x 32-bit run with no crashes. 2x 32-bit re-reads 9-14 texture pages a frame (0 for the others), which will be slow on hardware.
- **Picture:** no dither pattern, and about 16% brighter on average than 16-bit (no 5-bit truncation in each blend pass). DuckStation's True Color is about 13% brighter than its dithered mode in the same cutscene (tools/port/duckstation_cutscene.py --dithering TrueColor), and looks the same.

## FMV playback: decoder prototype (2026-10-18)

The movie player (src/screens/stream/stream.c) is skipped in the port (movie_main returns under SH_PORT). On the PS1 it streams with libcd (StSetRing/StSetStream/CdRead2), decodes each frame's bitstream on the CPU (DecDCTvlc) and with the MDEC (DecDCTin/DecDCTout, slices uploaded by a callback), and shows the result in 24-bit display mode. Plan: a port movie_main that reads the movie's raw sectors itself, decodes in C, shows frames from a 32-bit GS buffer, and starts the IOP's XA player (xa.c) on the movie's audio channel.

The movies are in HILL. (raw 2336-byte sectors), found by their start sector in the file table (XA/C1_20670 the intro, C2_20670, M1_03500 the opening, M2-ME):
- **Sectors:** video is file 0, channel 1 (submode 0x48); audio is file 1, channel 1 (submode 0x64, coding 0x01), about one sector in 10.
- **Frames:** 9 video sectors each, at 320x208 and 15 fps.
- **Video sector header:** 32 bytes: 0x0160, 0x8001, chunk, chunks, frame number, size, width, height.
- **Frame header:** run-length code count, 0x3800, quantizer scale (1), version 2.

Decoder (tools/port/str/strdec.c, plain C for the EE as well; tools/port/str/strdump.c tests it on the host):
- **Bitstream:** version 2: 16-bit words read MSB-first; per macroblock Cr, Cb, Y1-Y4; per block a 10-bit DC, then MPEG-1 DCT coefficient codes with a 6+10-bit escape; macroblocks in 16-pixel columns.
- **Reconstruction:** the PS1's default quantization table (MPEG's default intra matrix, DC 2), the inverse DCT (float for now), YCbCr to RGB.
- **Result:** M1_03500 and C1_20670 decode without bitstream errors, and their frames look right (the jeep scene; the intro's sunset, Cybil, the night scene).
- **Not yet checked:** a pixel comparison with DuckStation's MDEC output, which uses integer arithmetic with its own rounding.

## FMV playback in the port (2026-10-18)

movie_main (src/screens/stream/stream.c) now plays movies under SH_PORT with the game's own loop: Joy_Update, the skip button, the soft reset, the frame limit and the CD-audio volume calls. The PS2 side is src/port/ps2/movie_ps2.c:
- **One reader, as the PS1's drive.** The IOP's XA reader (sh1spu xa.c) plays the movie's audio (file 1, channel 1) and forwards its video sectors (file 0) to the EE. Each sector's 2048 data bytes go by SIF DMA into a 256-sector ring, followed by a sector count (OP_XAFWD; the EE acknowledges with OP_XAACK every 16 sectors, and a full ring holds the reader up).
  - A first version read the video itself from the EE. With the IOP's XA reader on the same drive, sceCdSync(1) reported the other reader's read as done: from the second chunk on, buffers held stale sectors (238 frames dropped) and the EE hung after 7 s.
  - The reader waits while its audio queue is full, so the video arrives at the audio's pace.
- **Frames:** assembled from the STR headers, decoded by src/port/ps2/strdec.c into 32-bit RGB, and shown from a PSMCT32 buffer in the snapshot area (gpu_gs.c GpuGs_MovieShow, display_ps2.c Display_PresentMovie). The display is 320x224 progressive, the 208-line picture centred. The PS1's VRAM and its 24-bit display mode aren't used, so movies don't need the 2x screen work either.
- **Timing:** frame n is shown 4(n - 1) vertical blanks after the start: 10 sectors a frame at 150 sectors a second.
- **Decoder speed:**
  - The first version (bit-by-bit reads, a float matrix IDCT) managed ~10.5 fps.
  - Now: a two-word bit window, an integer IDCT that skips empty columns and treats DC-only blocks as one value, and integer colour conversion. Output within 4 levels of the float version (49 dB PSNR), the same bits consumed.
  - In PCSX2: ~9M EE cycles a frame (about half the 15 fps budget), a steady 15 fps, ~50% idle.
- **Result (PCSX2):** the intro plays all 2060 frames with no dropped frames, then the main menu shows normally. Audio: the XA player decodes the soundtrack (levels in its diagnostics); not listened to yet.
- **Still to do:** check the sound and lip sync by ear, test on hardware, compare the picture with DuckStation's MDEC output, and play the in-game movies (M1 the opening, M2-ME).

## FMV decoder: bit-exact with DuckStation (2026-10-18)

tools/port/duckstation_movie.py compares the decoder with the PS1 game in DuckStation:
- **DuckStation side:** tools/port/gdb/movie_ps1.py breaks at PutDispEnv called from the stream overlay's movie loop. The frame on screen there is frame_cnt - 1, since strNextVlc has already fetched the next frame. It reads the whole VRAM through the game's StoreImage, as capture_ps1.py does.
- **Port side:** the same frames from tools/port/str/strdump on the host. Per-channel statistics and side-by-side PNGs with a difference image go to build/port/cap.
- **Setup:** DuckStation runs on its software renderer, muted. It played the regular intro, C1_20670 at sector 133127; the port in PCSX2 had played C2, because the memory-card settings differ.

Progress, intro frames 100/300/600/900, share of channel values identical:
- First version (ideal cosines, float colour): 45-59%, max difference 8-10 of 255.
- With the MDEC's IDCT matrix as libpress uploads it, 11-bit coefficient saturation and Mednafen's colour rounding: 61-74%.
- With the arithmetic DuckStation models: **100%**. Read from its source (src/core/mdec.cpp, DecodeRLE_New/IDCT_New/YUVToRGB_New), reimplemented here, not copied:
  - Dequantization keeps 4 extra bits: DC (v * qt0) << 4, AC (floor(v * q * qt / 8)) << 4, a half step toward zero, saturated to -0x4000..0x3FFF.
  - Each IDCT pass rounds with (sum + 0x20000) >> 18, floor, and saturates to 16 bits.
  - The result wraps to 9 bits, then is clamped to -128..127.
- **Data:** the libpress tables in the stream overlay match what strdec.c uses: the quantization table (MDEC command 0x40000001, luma = chroma = MPEG's default, DC 2) and the IDCT matrix (0x60000000, cos x 32768).
- **Cost:** the exact arithmetic first cost ~17M EE cycles a frame in PCSX2, close to the 19.6M budget at 15 fps.
  - Now each pass's sum is two 32-bit halves (each fits: |coefficient| <= 16384, |matrix| <= 32767), combined exactly with an average, as DuckStation does. Zero columns and rows are skipped, and a DC-only block is one value.
  - ~11.5M cycles a frame (59% of the budget), 15 fps steady, ~35% idle.

## All FMVs in a row (2026-10-18)

- **The movies:** 21 of the 30 XA files: C1/C2 (intros), M1-ME (in-game), Z1, Z3, Z4, ZC and ZZ, file table entries 2053-2073 (C1_20670 to ZZ_14239). The 9 before them are voice audio only.
- **Playing them all:** the Demo list's first row, ALL MOVIES, plays them one after another through the game's own open_main (stream.c Port_MoviePlayAll). Skip ends the current movie; the soft reset combo ends the run and returns to the list. The run marks the game state as the intro movie's while playing, because the game ignores the soft reset combo on the main menu.
- **Test runs:** a `movies` line in host:demo.txt starts the same run once the main menu is up, with a log line per movie ("movie test: N of 21 ... played to frame F of F").
- **First attempt:** title.c's Demo list code only read the menu when no remote request was pending, tested as `chosen < 0`, which also swallowed the new request value (-3).
- **PCSX2:** all 21 play to their frame limits with no dropped frames.

## Matching DuckStation pixel for pixel: lighting, demos, renderer (2026-10-21/22)

New comparison tools, all against DuckStation's software renderer:
- `compare_frames.py --demo N` (any of the five attract demos; DuckStation only stops once the wanted demo starts), `--load F,D` (a save through Load Game: file F, Data D on card 1; logos and intro movie skipped), `--walk` (with --load: turn 90 degrees right, walk 1.5 s), `--mode N` (Debug Options rendering bits).
- `--replay`: DuckStation's GP0 packets drawn by the port's GS renderer from DuckStation's VRAM, so only the renderer differs.
- `--state A:B [--globals ...] [--ps1-holds]`: the game state (g_SysWork, g_GameWork, RNG seed, delta time, any globals) at every demo step on both sides, with a per-field summary; --ps1-holds gives the port's demo playback the PS1's load timing.
- `tools/port/gpu_test.py`: the same VRAM and packets drawn on both (81 primitive configurations in cells); `--uv` decodes the texel each pixel sampled; `--dark` keeps vertex colours at 0-127.

Bugs found and fixed:
- **Brightness 0** in the PCSX2 card's saved options made the port look darker than DuckStation (not a code bug).
- **Checkered rectangle in front of Harry** (also in the PS2's PS1 mode): the flashlight draws an additive black quad; the PS1 dithers a polygon's colour before blending (clamped at 0), the GS the blended result, so the negative dither cells lost a level. Faint primitives are now undithered; dithered 4/8-bit textures in the adding/subtracting modes use a filtered CLUT (texels too faint to add anything left out, faint channels raised to exactly 4) and a second, undithered pass writes the left-out texels' mask bit. CLUT rows are never refiltered in place (PCSX2 read the row after its replacement's upload). The flare is dithered like DuckStation's, within 0.7%.
- **Harry's jacket and pants lit wrongly**: `func_8005AA08`'s batched NCT stored its last triple at `field_21C[arg1 + triples - 1]` instead of `+ (triples - 1) * 3` (one colour per normal); check builds never ran that line.
- **Attract demo desyncs**: `func_80080540` was PS1 inline asm that relied on the PS1 compiler's register allocation; on the EE it lost x * x and squared an unset register, so Air Screamers aimed differently. Now C. All five demos match the PS1's game state at every step (with --ps1-holds).
- **Texture coordinates**: the PS1 rounds interpolated UVs (DuckStation starts every attribute half a unit up), the GS truncates; polygon UVs now carry half a texel. Synthetic test 20205 -> 4894 differing pixels, demo frames about halved.
- **Blend modes 0, 2, 3**: the source is drawn into a scratch area (the last page cache slot) dithered as the PS1, then composited, which makes the GS's 8-bit blend the PS1's 5-bit one; quads triangle by triangle. Synthetic test 4894 -> 1461.

Left: gouraud colour rounding (accepted; docs/ToFix.md). Replays now differ from DuckStation by ~1-2% of pixels, scattered single-level differences with no dither-pattern bias.

## 2x resolution against DuckStation (2026-10-22)

Tools:
- `compare_frames.py --replay --mode N` replays DuckStation's packets with the Debug Options rendering bits set and also dumps the shadow buffer (`ps2r_N_hires.bin`).
- `compare_frames.py --ds-hw 16|16s|32` runs DuckStation's OpenGL renderer at 2x (unscaled dithering, scaled dithering, or true colour), fullscreen at an integer scale under XWayland (`QT_QPA_PLATFORM=xcb`), and captures its render widget with `xwd -id` right after the frame is shown (`ds2x_N.png`; `xwd -root` is refused by XWayland). DuckStation's settings are restored after.
- `tools/port/compare_hires.py N [--ds2x shot.png]`: the port's 2x image against DuckStation's 1x frame (2x2 blocks averaged) or a 2x screenshot: brightness ratio, differences in levels, mean bias per dither cell, a side-by-side image.

Results (demo 1 step 100, the Infirmary flare, the walk test):
- **2x 16-bit**: against the PS1 reference (software, 1x) brightness 0.995-0.997, 80-92% of channels exact. Its dithering is DuckStation's "Scaled" (the GS applies the 4x4 matrix per 2x pixel): against DuckStation Scaled the bias is flat over the dither cells (-0.27 levels, the gouraud rounding on the fog), against Unscaled it follows the matrix. "Unscaled" (2x2-pixel cells) isn't possible with the GS dither unit (its matrix repeats every 4 pixels).
- **2x 32-bit**: brighter than the PS1 reference (blacks lifted, +0.6 to +1 level) because faint additive layers keep their fractions in true colour; against DuckStation's TrueColor 2x it matches (brightness 0.98-1.0, 85-90% exact).
- DuckStation's OpenGL and software runs can draw slightly different frames at the same point after a save load (timing); demo frames are identical.

## The HAL moves to its own repository (2026-10-27)

The PSY-Q compatibility layer now lives in `../psx-to-ps2-HAL` (a sibling repository, to become a GitHub
repository later), so the Metal Gear Solid port (`../mgs1_ps2`) can share it. Work was planned and done from the
MGS port's sessions; the file-by-file sort and the hook list are in `../psx-to-ps2-HAL/docs/Split Plan.md`.
Branch: `hal-split` (from `upstream-sync` at 4179e9d1c).

What moved (copied unchanged into the HAL's first commit, then edited there):
- `src/port/*.c` → HAL `src/`: libapi, libgpu (+ packets), libgs, libgte (+ tables), libspu, gte.c, psx_card.c,
  psx_mem.c, port_main.c, prof.c, prof_fn.c, recomp_rt.c.
- `src/port/ps2/*.c` → HAL `src/ps2/`: all but capture, rec, demo_sweep, warp and sentinel (SH1's tools).
- `include/port/` → HAL `include/port/`: gte*.h, pad.h, prof.h, recomp.h, spr.h. The `port/` prefix is kept,
  so no game `#include` changed.
- `src/port/iop/sh1agent`, `sh1spu` → HAL `iop/` (names kept for now).

What stayed in SH1: demo menu/recorder, overlay.c, port_data.c, libkmath (SH1's own library), the five
SH1 tool files in `src/port/ps2/`, and all of `tools/port/` (most tools name SH1's ELF, disc files or game
state; moving them needs its own pass).

The SH1 parts of the moved files are now hooks or settings:
- `src/port/hal_game.c` (new): `HalGame_OnPacket`/`OnDrawOTag`/`Status`/`OnPutDispEnv`/`DisplayMode`/`Init`,
  the frame capture, state line, load watchdog, screen-read context and Debug Options code that was in
  libgpu_port.c, plus the overlay snapshot that port_main.c called. The HAL has weak no-op defaults
  (`src/hal_weak.c`), also for the agent's demo/replay commands and `Port_WarpButtons`.
- `include/hal_game_config.h` (new): banner name, RAM size (crash_ps2.c), disc files and SILENT.'s PS1 sector
  (libcd_ps2.c), memory card names and the save icon (memcard_ps2.c, psx_card.c).
- `include/port/prof_game.h` (new): SH1's `LP_*` light-profiling probes; prof.h/gpu_gs.c take both lists as
  X-macros (the liteprof report now prints the HAL probes first).
- `src/port/sentinel_gte.c` (new): the Floatstinger check that gte.c ran (`SH1_SENTINEL=1`).

Build changes: `port_link.py` and `ee_compile_check.sh` find the HAL at `$SH1_HAL` (default
`../psx-to-ps2-HAL`): its include directory, `src/*.c`, `src/ps2/*.c`, and the IRX Makefiles, which now take
`IOP_OBJS_DIR` from port_link.py. `gte_test`, `lib_test` and `xa_test` point at the HAL's files.

Checks: compiles (274/274), links with the same 15 HAL stubs; same text size, every symbol the same size except
the edited functions and the new hooks (data/bss shrank by alignment padding from the new object order).
PCSX2, 90 s from boot, before and after: identical console logs apart from code addresses and timing counters
(same game states, packet and word counts, the same TLB-miss lines on scratchpad addresses during start-up,
which the pre-move build also logs). `gte_test`: 0 of 2720 differ (seeds 0 and 3).

Deferred: renaming `SH_PORT_*`/`SH1_*` switches and `sh1agent`/`sh1spu` to HAL names; moving the generic tools;
a general disc map in libcd_ps2.c (now two named files, as SH1's disc), decided once the MGS disc layout is known.

## HAL: PS1 threads and real critical sections (2026-10-27)

For the MGS port the HAL gained PS1 BIOS threads (`../psx-to-ps2-HAL/src/ps2/thread_ps2.c`; details in the HAL's
`docs/Walkthrough.md`). For SH1 this means `EnterCriticalSection`/`ExitCriticalSection` are no longer link stubs:
they disable/enable EE interrupts (used around the sound timer setup in `smf_main.c` and the card events in
`memcard.c`). Checked: 150 s in PCSX2 from boot, the same status lines and TLB-miss lines as the build before.
The link has 13 HAL stubs now (was 15). SH1 opens no threads, so the thread switch hook in the vertical blank
handler returns at once.

## HAL: libpress and more PSY-Q functions for MGS (2026-10-27)

The HAL (`../psx-to-ps2-HAL`, its `docs/Walkthrough.md`) now has libpress (psyz's VLC and this port's
`strdec.c` reconstruction, extended with `StrDec_Macroblock` for the MDEC's run-level input) and a few more
libgte/libgpu functions. SH1's link has 9 HAL stubs (was 13: its libpress ones are now real). SH1 plays movies
through `Port_Movie*`, so its own libpress calls (`stream.c`) aren't reached. 150 s in PCSX2 as before.
All 12860 frames of SH1's movies decode identically through the new MDEC-input path (`tools/mdec_test` in the
HAL).

## HAL: pad changes for MGS (2026-10-27)

The HAL's pad layer gained MGS's `PadInitDirect`/`PadStartCom`/`PadStopCom` and a polling thread
(`HAL_GAME_PAD_THREAD`, unset for SH1: VSync still polls; details in the HAL's `docs/Walkthrough.md`). One change
reaches SH1: `PadGetState` (`src/ps2/libpad_psx.c`, called by libkpad) now returns the PS1's values. A pad
without DualShock functions (PS2 FINDCTP1) gives 2 (`PadStateFindCTP1`, was 6) and a command in progress
(EXECCMD) gives 5 (was 2). libkpad's handlers were written for these values. Checked: links (9 stubs); 150 s in
PCSX2 through the attract demos, pad stable, no TLB misses. Not yet checked on hardware or with a digital pad.

## Widescreen, anamorphic 16:9 (2026-10-28)

Debug Options "Widescreen" (`config.unused_26` bit 3; `SH1_WIDESCREEN=1` forces it for test builds). The picture
stays 320 wide and a TV in 16:9 mode stretches it. Overview in `include/port/widescreen.h`.
- **3D, hor+:** `vbSetWorldScreenMatrix` (vw_calc.c) scales the view matrix's X row by 3/4. All culling found
  (`Vw_AabbVisibleInScreenCheck`, `Vw_AabbVisibleInFrustumCheck`, `WorldMap_SubcellVisibleCheck`, the mesh
  renderer's screen clamps) tests projected screen X through this matrix, so it widens with no change.
  `vcWork.scr_half_ang_wx` (camera framing only) is left at 4:3, so the camera behaves as before.
- **2D:** HAL hooks. `HalGame_OnOtTag` (called for each empty OT entry) and `GpuGs_SetXScale` squeeze vertex X
  around the drawing offset; the clip area is squeezed too. Per game state (hal_game.c):
  - gameplay: the 2D layers (`g_OtTags0`) are squeezed, except slot 4 (cutscene borders) and slot 5 (fades);
  - black side bars are cleared at each DrawOTag in the whole-frame states, and the clip area is squeezed
    too, so what was drawn just off the 4:3 screen stays hidden (the title showed a white column);
  - title, logos, load screens, the inventory and the paper map: the whole frame is squeezed (the
    projection squeeze is off there, or the inventory's 3D items were squeezed twice);
  - other menus (options, save/load): unchanged, stretched by the TV;
  - FMVs: pillarboxed. `GpuGs_MovieShow` resamples each row linearly to 3/4 width on the EE, in 64 KB
    bands, and centres it (`GpuGs_SetMovieXScale`). Movie frame dumps are now read back from the GS
    buffer being shown, not taken from the decoded frame;
  - full-screen pictures (`background_draw.c`, e.g. item inspection images) in gameplay: they share the
    fog backdrop's OT entry, so they are bracketed by two empty marker packets (`PORT_PICTURE_START`/
    `_END`) that `HalGame_OnOtTag` recognises. Markers are only linked in widescreen gameplay: at boot
    the function runs with no packet memory (and draws nothing).
- **Cutscenes, 14:9:** while the borders show, the projection distance is multiplied by up to 7/6
  (vc_main.c, as the border shade goes from 0 to 1). The full 224 lines then show what the 192 lines between
  the original bars showed, and the bars become 20-pixel side bars (cutscene_border.c). This applies to fade
  types 2 and 3.
- The title's Debug Options carry into the next loaded save with all five bits now (memcard.c, was 3).
- Checked in PCSX2: the attract demo, the Infirmary save (pause text, map, inventory), the map2_s01 church
  cutscene, map6_s02 "That light!?", the title and the intro FMV. Not checked on hardware yet.
- Frame dumps (`input.txt dump`) during cutscene sweeps sometimes cause TLB misses: in sifrpc `_request_end`
  (widescreen build, map6_s02), and once at pc 0x82000/0x8001044 (widescreen off, map2_s01, 1 of 4 runs). No
  run without dumps had them. This matches the dump RPC problem noted under the 2x work.
- Squeezed text at 1x loses columns: a 12-texel glyph gets 9 pixels with point sampling. At 2x resolution
  it gets 18 and keeps them all (compared on "PAUSED").
- Debug Options "2D Filter" (bit 4, Nearest/Bilinear): squeezed textured sprites (text, the inventory's
  portrait, the title's tiles) can be drawn with bilinear filtering (`rectangle` in gpu_gs.c: TEX1 linear around the sprite), so 1x keeps every glyph stroke.
  Polygons, including 3D, stay point-sampled. Glyph edges get a faint dark fringe from the transparent
  black texels around them. Tried first offline: nearest 2x, then bilinear was a little sharper but would
  need an offscreen pass.

## Graphics pipeline step 7: an EE copy of VRAM feeds the texture caches (2026-10-28)

- The HAL's GS renderer (`gpu_gs.c`) keeps the PS1 VRAM in EE RAM again (`s_Vram`, 1 MB; removed 2026-10-17
  when it was only a mirror). This time it is the source of the texture page, CLUT and filtered CLUT caches and
  of StoreImage, instead of reading the GS back.
- Where the GS draws, the copy goes stale: each primitive's clip area marks 16x16 tiles (`vram_mark` in
  `state_other`). CLUT reads and StoreImage bring stale tiles up to date with one read-back
  (`vram_refresh`); texture pages are built from the copy without it.
- First try refreshed for pages too: Kaufmann only went 7.2 → 3.4 read-backs a frame. A log of the refreshes
  showed pages 0-2 (x 0-191, both halves) spanning the frame buffers (0,32)-(319,479); the game's 4/8-bit
  texels there are in rows 0-31 and 480-511.
- PCSX2, 2x/32-bit, read-backs a frame: Kaufmann 7.2 → 0.14, Docks (`docks1` to step 1830) 2.2 → 0.27.
  Output: `gpu_test.py` unchanged (1,461 pixels, as before), attract demo dumps identical at 1x and 2x/32-bit,
  `docks1` state identical at step 1830.
- Baseline counts and the plan: Graphics Pipeline Rework.md, steps 6 and 7. A TLB Miss in ps2sdk's sifrpc
  during a bench frame dump turned up on the way (docs/ToFix.md).


## Console playthrough to the ending (2026-11-01)

- Full run on hardware, build of Nov 1 07:56:30 (one power cycle partway), from map4_s05 through Nowhere to the Good+ ending and a new game plus save. No crash, exception or hang logged.
- Newspaper fix confirmed: the clipping could be read in Nowhere.
- Music runs too fast in Video Mode 480p, correct in 240p. Found by test; other bugs seen are in docs/ToFix.md.
