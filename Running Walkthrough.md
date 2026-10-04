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
