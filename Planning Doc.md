# Planning Doc — Silent Hill (PS1) → PS2 Port

Living plan for the port. Check items off as they land; record *how* each change was made in
`Running Walkthrough.md`. Background research lives in `Engine Info.txt`,
`High-Level Porting Ideas.txt` and `Medium-Level Porting Ideas.txt` (see "Corrections" below for
what in those notes is wrong or outdated).

## Strategy

Recompile the decompiled C for the PS2 Emotion Engine with ps2sdk, and replace Sony's PSY-Q
libraries with a PS1 compatibility layer (HAL) implemented on ps2sdk. Game code stays as close to the
decomp as possible.

Ground rules:

- **The matching PS1 build must keep matching.** Port changes go behind `SH_PORT` (or behave
  identically on PS1), and `make` must still pass the checksum after every change. This keeps us
  able to merge upstream decomp work and gives a free regression test.
- **32-bit pointers are an asset.** The EE uses 32-bit pointers, so struct layouts, file-data
  relocation (`isLoaded` + header-relative offsets) and pointer/int casts stay valid as-is.
- **Prefer bit-exact over fast.** Software GTE first; vector-unit acceleration only after profiling.

## Current state of the codebase (as of 2026-10-03)

- Code is essentially fully decompiled: 7 `INCLUDE_ASM` stubs remain (5 are JP-only text drawing,
  plus `Gfx_Inventory_ItemDescriptionDraw` and `libkpad` `func_8009E198`).
- Data is partially migrated: ~188 splat-generated `rodata`/`data`/`bss` segments are still asm.
- PSY-Q libraries are linked as prebuilt `.o` files from `lib/`.
- `gte_*` macros (COP2 inline asm / DMPSX opcodes) are used in 32 source files.

## Steps

### Step 1 — Fixed addresses / shiftability  *(done)*

- [x] Matching build environment working (Docker image, CHD → BIN/CUE, submodules)
- [x] Single abstraction for fixed addresses: `include/decomp/psx_mem.h`
      (`PSX_RAM_ADDR`, `PSX_SCRATCH`, `PSX_ADDR_CANON`)
- [x] Route all hardcoded RAM buffer addresses in C through `PSX_RAM_ADDR`
      (`fsqueue.h`, `fsmem.h`, `demo.h`, `memcard.h`, credits, sound, screen, title, maps, b_konami)
- [x] Route raw scratchpad literals through `PSX_SCRATCH`
- [x] Replace the 24-bit pointer mask in `Fs_QueueDoBuffersOverlap` with `PSX_ADDR_CANON`
- [x] Verify matching build checksum still passes with the changes (all 51 outputs OK)
- [x] Audit splat-generated data asm for raw pointer words (`.word 0x80xxxxxx`) that should be symbols
      — 102 raw words (66 distinct) in 5 linked files, see below
- [x] Symbolise/migrate the raw pointer words:
  - [x] `MAP_MESSAGES` tables in map4_s00, map4_s06, map6_s05 — migrated to C like map4_s01
  - [x] map7_s03 `23CA0.data.s`: pointer tables into its own rodata now use symbols (rodata blobs
        split with extra `INCLUDE_RODATA`); the two fixed buffers are `g_FsBuffer18`/`g_FsBuffer20`
  - [x] bodyprog `D_80028A18` (`3EB8.rodata.s`) — not a pointer: unreferenced compiler padding
- [x] Re-audit: no raw pointer words remain in linked data; remaining `0x80……` words are
      `LOADABLE_INVENTORY_ITEMS` bytes (`0x80222120`) and colours (`0x80808080`, `0x80FF8080`)
- [x] Check whether any fixed buffer overlaps a linked section
      — map overlays end ≤ `0x800F5978`, below the lowest buffer (`0x800F5E00`): no overlap.
      Screen overlays (STREAM/SAVELOAD/OPTION/STF_ROLL) load at `0x801E2600` *on top of*
      `FS_BUFFER_1`/`5`/`6`/`21`/`9`/`10`/`16`, `IMAGE_BUFFER_3` and the `0x801E2E00` OT. That
      area is dead while a screen is active; in the port it simply stops being clobbered.
- [x] Add a regression check script that fails on new raw address literals
      (`tools/port/find_fixed_addresses.py`)
- [x] Shift test: map overlays padded by 0x110 after their header (branch `shift-test`,
      `tools/port/shift_test.py`); all 43 warped into via `tools/port/gdb/warp_sweep.py`. 36 pass;
      the other 7 fail identically on the unmodified disc (warp artefact), so nothing shift-specific.
      main/bodyprog can't be shifted until Step 2 (other binaries reference them by fixed address).
- [x] Linker-script symbol assignments overriding the binary's own definitions (found after the
      shift test; it couldn't detect them): 402 entries in splat's `undefined_*_auto` files and
      `lib_externs.ld` pinned symbols the binary itself defines (116 of them map data variables used
      from C). `tools/port/prune_undefined_syms.py` now filters those lists at link time; 4 interior
      symbols are defined relative to their container in `configs/USA/relative_syms.ld`.
- [x] Re-run the warp sweep on a shifted image with the linker fix: 36/43 pass, the same 7 that
      fail on the unmodified disc fail; map data variables now move with the shift
- Deferred to Step 2: `g_OvlBodyprog` / `g_OvlDynamic` overlay load addresses in `src/main/main.c`
  (they disappear once overlays are statically linked).

## Reference: the SH1 PC port

`/home/alex/Downloads/silent-hill-decomp-nx-pc-port` is a playable PC port of the same decomp on
PsyCross (SDL2/OpenGL PS1-library reimplementation; repo `SlickAmogus/silent-hill-decomp`, branch
`pc-port`). The download is not a git checkout and its `pc_port/PsyCross/` submodule is empty.
Useful to us (`pc_port/` paths):

- `docs/Port_Fixes_Index.md` — game-code bugs the PS1 tolerated (NULL/small-pointer reads,
  integer divide by zero, fixed-point overflow, IPD buffer sizing). Check each against the EE.
- `src/map_registry.c`, `maps/CMakeLists.txt` — per-map builds with renamed symbols
  (`-DSH_MAP_NAME=…`) and a swapped `g_MapOverlayHdr` pointer: a working Step 2 design.
- `include/gpu_gte_pc.h` — every custom GTE macro in `gpu.h` mapped onto C calls (Step 3 checklist).
- `include/psx_memory.h` — same 2 MiB arena + `& 0x1FFFFF` scheme as our `psx_mem.h`.
- `XA_RESEARCH.md`, `docs/ordering_table_and_drawtag_pipeline.md` — background for Steps 4 and 7.

Not applicable: the 64-bit struct reformatting (`ipd_reformat.c`, `dms_reformat.c`,
`struct_offset_portability.md`) — EE pointers are 32-bit. Their 700+ "zero-stub" data tables come
from not being able to use the MIPS data asm; the EE is MIPS, so we should assemble splat's data
`.s` files as-is and get the real tables. PsyCross itself is OpenGL/SDL and can't run on PS2, but
its C GTE is a candidate for Step 3.

### Step 2 — Static linking of overlays

Proof of concept (`tools/port/static_link_poc.py`): each map `ld -r`-merged, its
`g_MapOverlayHdr` renamed `g_MapOverlayHdr_<map>` and everything else made local; screen overlays
keep only the symbols main/bodyprog reference. main + bodyprog + screens + 43 maps then link with
**no duplicate definitions** and no game-source changes. Still unresolved:

- [x] Map/screen references to bodyprog addresses bodyprog doesn't name: `Math_MatrixTransform`
      was `static` in `load_screen.c` (now global; matching unchanged); the other six
      (`D_800C39A0`, `g_Player_AnimResetRequest`, `D_800A99xx`, `g_SaveScreen_IsLoadError`) are in
      `configs/USA/port_relative_syms.ld`, port link only
- [x] map7_s03 → `func_801E2E28`/`func_801E2ED8`/`func_801E2FC0` and map6_s02 → `func_801E386C`/
      `func_801E3970`: deliberate calls into STF_ROLL.BIN (credits), which those scenes load next to
      the map via bodyprog `GameFs_StfRollBinLoad()`. The PoC now keeps every overlay symbol any
      other part references, so these resolve
- [ ] Overlay "loads" in the port: all go through `Fs_QueueStartRead(<overlay file>, <load addr>)`
      (maps → `g_OvlDynamic`, screens and STF_ROLL → `FS_BUFFER_1`, bodyprog/B_KONAMI decrypted at
      boot). One hook: for overlay file indices, skip the read and restore that overlay's
      `.data`/`.bss` to its initial state (and set the current-map header pointer for maps)
- [x] Relative symbols in the merged link. Linker-script expressions come out ABS in an `ld -r`
      link, so: map-internal zero-offset aliases (`D_800CD768_tbl`) are resolved by renaming the
      reference in the map's objects before merging; bodyprog ones are applied in the final link
      (checked: `D_800C15B4` = `D_800C15B0` + 4 etc. in the PoC ELF)
- [x] bodyprog/SAVELOAD `pad` clash: gone once SAVELOAD's internal symbols are localised
- PoC final link: only `g_MapOverlayHdr` (→ current-map pointer), `g_FsBuffer18/20` (→ arena) and
  `main_*` section markers (→ PS2 crt) are left undefined

- [ ] Namespace per-map symbols (`Map_WorldObjectsInit`/`Update` ×42, `sharedFunc_*` in 95 files)
      — compare with the PC port's per-map `SH_MAP_NAME` renaming
- [x] Replace "load overlay + jump" with a per-map dispatch table (`Port_OverlayActivate`)
      — cf. PC port `map_registry.c` (swaps a `g_MapOverlayHdr` pointer per map)
- [ ] Reset each overlay's `.data` on "load" (snapshot/restore), matching PS1 reload semantics
- [x] Remove `g_OvlBodyprog` / `g_OvlDynamic` fixed addresses (now `PSX_RAM_ADDR`)
- [ ] Decide what to do with the remaining `INCLUDE_ASM` functions (need C for the port)
- [ ] Assemble splat's data/rodata `.s` files for the EE as-is (no zero-stubbed tables)

### Step 3 — Software GTE

- [ ] Evaluate PsyCross's C GTE (github.com/OpenDriver2/PsyCross) for bit-exactness before
      writing our own; use PC port `gpu_gte_pc.h` as the list of macros to cover
- [x] Bit-exact C GTE (register file struct, saturation/flags, UNR divide table): `src/port/gte.c`,
      written from psx-spx; verified against the PS1 GTE in DuckStation with
      `tools/port/gte_test` (every opcode × sf/lm, all 256 MVMVA variants; 19,040 tests, 0 diffs)
- [x] Replace `gte_*` macros and DMPSX raw opcodes with calls into it under `SH_PORT`:
      `tools/port/gen_gte_inline.py` translates the asm macros of `inline_c.h`, `inline_no_dmpsx.h`
      and `gpu.h` into `include/port/gte_from_*.h`; two hand-written asm blocks (`vw_calc.c`,
      `bodyprog_80056D8C.c`) have `SH_PORT` C versions. All 451 linked C files compile for the EE
- [ ] C replacements for Sony's prebuilt libgte functions (`RotTransPers`, `ApplyRotMatrixLV`, ...)
      on top of `gte.c` (part of the HAL)
- [ ] `INCLUDE_ASM` functions still assemble as R3000 code inside EE objects: check for COP2 use
      and R3000-only behaviour (load delay slots) before relying on them

### HAL inventory (first full PS2 link, `tools/port/port_link.py`)

The whole game (main + bodyprog + screens + 43 merged maps, EE objects) links against ps2sdk with no
duplicate definitions. 193 undefined symbols remain — the HAL's exact scope:

| Library | # | Notes |
|---|---|---|
| libgte | 46 | matrix/vector helpers, RotTransPers etc. — on `gte.c` |
| libgpu | 30 | prims, DrawOTag, Load/StoreImage, draw/disp env — Step 4 |
| libgs | 30 | Sony's 3D/2D layer (GsSortObject4J, GsDrawOt, coords) — on libgte + libgpu |
| libcd | 19 | CdRead/CdControl, St* streaming — Steps 5/7 |
| libspu | 19 | voices, reverb, transfers — Step 6 |
| libapi | 15 | events, root counters, critical sections, memcard files |
| libkmath | 8 | Konami asm math (`src/bodyprog/libkmath/libkmath.s`, has GTE ops) — needs C |
| libpad | 8 | controller — Step 5 |
| libcard | 7 | memory card — Step 5 |
| libpress | 5 | MDEC decode — Step 7 |
| libetc | 3 | VSync, callbacks |
| ours | 3 | `g_MapOverlayHdr` (current-map pointer), `g_FsBuffer18/20` (arena) |

- [x] libgte + libkmath: recompiled from Sony's/Konami's PS1 objects by `tools/port/recomp.py`
      (`tools/port/recomp_all.sh`); `InitGeom` hand-written (`src/port/libgte_port.c`). Verified
      against the PS1 originals with `tools/port/lib_test` (48 functions, 384 tests, 0 diffs).
      `GsTMDfast*` (also libgte) still to test, with libgs
- [x] libgs recompiled (all 24 objects; jump tables and `jalr` through `GsFCALL4` supported). State
      setters verified by `lib_test` (GsSetFlatLight/LightMatrix/LsMatrix/Projection/Ambient)
- [ ] Verify libgs packet builders (`GsSortObject4J`, `GsSortFastSprite`, `GsSortOt`,
      `GsLinkObject4`, `GsMapModelingData`) and `GsTMDfast*` with real game data: compare the packets
      built per frame against the PS1 once the port renders

### First boot (done 2026-10-04)

- [x] `build/port/sh1.elf` links (110 HAL functions auto-stubbed, each logging its first calls) and
      runs the game's PS1 `main` in PCSX2: ResetCallback, CdInit, VSync, ResetGraph, ClearImage2,
      DrawSync, PutDispEnv, SpuInit, then the file queue retries CD reads forever (stubs)
- [x] DVD image: `tools/port/make_iso.sh` → `build/port/sh1_ps2.iso` (`tools/port/mkiso.py`:
      ISO9660, 2048-byte sectors, retail PS2 DVD layout — path tables at 257, root at 261, root size
      = bytes used). Boots as DVD media in PCSX2 with both the v1.00 and v1.60 BIOS
- [ ] Title ID `SHPS_000.01` is a placeholder (SYSTEM.CNF, ELF name, memory card folder)
- [x] Overlay-load hook: `g_OvlBodyprog`/`g_OvlDynamic` go through `PSX_RAM_ADDR` (overlay files are
      still read, into the arena); `Fs_QueueStartRead` calls `Port_OverlayActivate` (src/port/overlay.c),
      which points `g_MapOverlayHdrPtr` at the loaded map's header
- [ ] Overlay .data/.bss reset on reload (snapshot/restore)
- [x] libcd (file-queue subset) on libcdvd: `src/port/ps2/libcd_ps2.c`; SILENT. sector-for-sector,
      HILL. raw 2336-byte sectors; synchronous reads. The game now gets through `main` into bodyprog
      init (pad, SPU, root counters, events, main loop)
- [x] libgpu: primitive builders + OpenTIM/ReadTIM recompiled; sys.o recompiled for its packet
      builders only (`--exports`, rest private); `src/port/libgpu_port.c` (software 1024x512 VRAM,
      Load/Store/ClearImage, ClearOTagR, DrawOTag/DrawPrim walking OTs into `Gpu_Submit`, draw/disp
      envs). `src/port/ps2/libetc_ps2.c`: VSync/VSyncCallback on the EE VBlank interrupt
- [x] Port compiled with `-fno-toplevel-reorder` (the game relies on in-file variable order, e.g.
      the big OT's last tag is the `__pad_bss_800B9CC4` word after `g_OtTags1`)
- [ ] sys.o's `GEnv` isn't initialised by our ResetGraph: check SetDrawEnv's packets against the PS1
- [x] Software PS1 GPU (`src/port/gpu_soft.c`) behind `Gpu_Submit`; the display area is shown via
      gsKit (`src/port/ps2/display_ps2.c`). First visible frame: the warning screen
- [x] Output mode follows the PS1's DISPENV: 240p or 480i (isinter and > 256 lines), the PS1's own
      width via the GS's MAGH (256/320/368/512/640) and its line count; pixels copied 1:1 (no
      scaling or filtering)
- [x] Root counters from the EE cycle counter (`src/port/ps2/rcnt_ps2.c`): frame timing works, the
      game advances warning screen → Konami → KCET logo states
- [x] libspu placeholder (`src/port/libspu_port.c`): everything succeeds, nothing plays
- [x] Konami logo screen (recompiler added symbol offsets twice for weak `.bss` symbols)
- [x] Events (`OpenEvent`/`TestEvent`/...) + libcard reporting empty slots; boots to the title menu
- [x] Pad input (libpad over the BIOS's PADMAN); attract demo plays in-game (town, 320x224)
- [ ] RCnt interrupts (sound driver tick), memory card
- [ ] Movies skipped under `SH_PORT` (`movie_main`) until CD streaming + MDEC exist
- [x] GS hardware renderer (`src/port/ps2/gpu_gs.c`), default; software renderer kept (`SH1_GPU=soft|compare`)
- [ ] GS renderer: polygon edge / rounding differences (in-game speed is fine: see walkthrough)
- [ ] Render comparison tool: DuckStation vs PCSX2 (GS renderer), RenderDoc-style
- [x] 60 fps option (`SH1_FPS=60`: gameplay `g_IntervalVBlanks` 1 instead of 2); benchmark `SH1_BENCH=1`
- [x] Profiling counters (`SH1_PROF=1`, include/port/prof.h)
- [x] GTE speed-up (bit-exact, checked on the host and on the EE): fast paths, cached matrices, inlined register moves, MMI (PLZCW, PHMADH)
- [x] 60 fps in the demo scene: EE 11-38% idle; the remaining dips are file loads (VSync waits while streaming), as on the PS1
- [ ] Further GS translation speed-ups if heavier scenes need them (parse straight into packets; state per primitive is ~500 cycles)
- [x] All-map 60 fps sweep (one boot): most maps 54-59 fps average; slowest map7_s03 (~43)
- [ ] Overlay .data/.bss reset on reload (code in place, off: breaks the second demo)
- [ ] Graphics vs DuckStation (first attract demo): world largely unrendered, flashlight lighting, wrong player textures
- [ ] GS renderer gaps: rectangle flips, wrapping VRAM copies, 24-bit display (FMV)
- [ ] GS hardware renderer (speed), checked against gpu_soft.c

### Step 4 — Graphics (libgpu → GS)

- [ ] Walk the PS1 OT at `DrawOTag`, translate primitives to GIF packets
- [ ] VRAM emulation: 1024×512 shadow + texture cache keyed by tpage/CLUT, invalidated on
      `LoadImage`/`MoveImage`/CLUT writes
- [ ] STP-bit semi-transparency (TEXA/AEM + alpha-test two-pass)
- [ ] Blend modes (correct table below), texture window via `REGION_REPEAT`, dithering decision

### Step 5 — Pad, CD sectors, memory card

- [x] libpad shim over `rom0:PADMAN` (libkpad recompiled with the game)
- [ ] libcd shim: LBA reads from `SILENT.`/`HILL.` (sector offsets relative to container files)
- [ ] Memory card: save format + `icon.sys`/icon, real title-ID folder

### Step 6 — Sound driver on the IOP

- [ ] Port Konami SMF sequencer (`src/bodyprog/libsd`) to an IRX, or batch SPU commands over SIF
- [ ] Pitch rescale for SPU2's 48 kHz (×44100/48000); reverb mode mapping

### Step 7 — XA audio and STR movies

- [ ] Raw Mode 2 Form 2 sector reads, software XA-ADPCM decode, stream to SPU2
- [ ] Software MDEC decode on the EE

### Throughout

- [ ] Go through PC port `docs/Port_Fixes_Index.md`, sections 1, 3, 4, 6, 7 (section 2 is the
      zero-stub problem, section 5 is 64-bit only) and apply the fixes that are real game-code
      bugs, under `SH_PORT`

### Optional — PC host build

- [ ] Bring the HAL up on PC first (cf. PsyCross) for easier debugging, then swap in PS2 backend

## Corrections to the earlier notes

- PS1 → GS blend modes. `GS_SET_ALPHA(A,B,C,D,FIX)` = `(A−B)·C/128 + D`
  (A/B/D: 0=Cs, 1=Cd, 2=0; C: 0=As, 1=Ad, 2=FIX). Enable COLCLAMP.

  | PS1 mode | GS_SET_ALPHA |
  |---|---|
  | 0: B/2 + F/2 | `(0,1,2,1,0x40)` |
  | 1: B + F | `(0,2,2,1,0x80)` |
  | 2: B − F | `(1,0,2,2,0x80)` |
  | 3: B + F/4 | `(0,2,2,1,0x20)` |

- ps2sdk API names are `sceCd*` (libcdvd) and `sceSd*` (IOP libsd); the EE cannot call `libsd`
  directly, only via SIF RPC.
- SPU pitch is a ratio, not Hz, and SPU2 runs at 48 kHz.
- `BISCPS-10080` is not a real title ID; PS2 saves need `icon.sys` + icon.
- Float/VU0 transforms contradict the "keep GTE math bit-exact" requirement; use a software GTE.
- XA audio (HILL. and STR movies) is not covered by the notes and needs its own subsystem.
- PS2 ps2sdk programs run in KUSEG (~0x00100000), so any PS1 code that assumes pointers are
  ≥ 0x80000000 or uses address-mirror masks must be found (see Step 1).
