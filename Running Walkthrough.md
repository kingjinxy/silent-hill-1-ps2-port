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
