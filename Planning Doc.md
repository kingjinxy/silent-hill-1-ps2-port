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
- [x] GS translation speed-ups: polygons packed straight into REGLIST data, quads as one triangle strip, direct page/CLUT cache lookups, row copies for page uploads
- [x] Opening new-game cutscene at a solid 60 fps (`SH1_FPS=60`; PCSX2 software renderer): direct GTE command entry points, 32-bit DPCS, port code at -O3, late frames outside gameplay no longer snap to 30 fps
- [ ] Gameplay after the cutscene: 60 fps but only ~7-15% EE idle; heavier rooms will need more (GTE RTPT/DPCS, GS translation)
- [x] Disc reads asynchronous and batched (CdRead starts, CdReadSync moves along; ~27 HILL. sectors per DVD read)
- [x] Fast warp sweeps: with host:warp.txt no boot logos, no title wait, only the first demo; follow-on loads part of a map's test; stuck-run detection (`pcsx2_run.py --progress`); 43/43 pass in a few minutes
- [x] Exact batched mesh vertex transform (`Gte_RtpBatch`) and port Gfx_MeshDraw (inline NCLIP, fog/light colours memoized by their byte): checked packet-for-packet against the original (`-DSH_PORT_CHECK_BATCH`); cutscene idle ~16% -> ~27%, gameplay ~14% -> ~24% (both changes)
- [x] Play-through to the cafe: fixed a wrong spawn after an area load (`g_ItemTriggerEvents` unsized), frozen pad in heavy scenes (pad polled only after VSync waits), dropped held buttons (a pad command every poll), two freezes in the alley (decompiled code relying on the PS1 stack layout: los.c ray trace, particle.c hull)
- [x] Debugging tools: load watchdog (RAM dump), `world_diag.py` (PS2 save state / RAM dump / PS1 RAM), DuckStation world watch (gdb), registers from PCSX2 save states
- [ ] Alley end (child ghosts) and onward: 34-55 fps with the EE fully busy for minutes; profile and optimise
- [ ] `Gfx_EffectsUpdate` reads through a null pointer (address 8) at the start of map0_s00 (harmless in PCSX2)
- [ ] VU0/VU1 GTE: an exact VU transform saves little over the EE batch (the UNR divide needs split-float maths and per-vertex table reads); revisit for other paths if the profile points there
- [ ] map6_s00 (4-7% idle): other drawing paths (func_8005A900/func_8005AC50, likely world geometry); map5_s00 / map0_s00 drop frames with 30-50% idle (not EE-bound: loading or event pacing?)
- [ ] Sweep: maps at a steady ~66% idle may be showing a fade/black screen rather than the map (the 7 maps that fail from the title demo "pass" by not crashing); 30 fps maps with idle left (map1_s06, map7_s00, map7_s03); heavy: map5_s00 (39 fps avg), map0_s00 (46), map6_s00 (4-7% idle)
- [x] All-map 60 fps sweep (one boot): most maps 54-59 fps average; slowest map7_s03 (~43)
- [ ] Overlay .data/.bss reset on reload (code in place, off: breaks the second demo)
- [x] Frame comparison tool vs DuckStation (`tools/port/compare_frames.py`): same demo frame, display + VRAM + GP0 stream + game state
- [x] First attract demo in sync with the PS1 (Harry's run animation froze: `variableFunc` called without its model argument)
- [ ] Renderer rounding: display pixels mostly +-1/31 off DuckStation's software renderer (identical GP0 streams)
- [ ] Crash in the second attract demo (map2_s00 again, ~7 s in; world chunk streaming, `WorldMap_CollisionDataGet`): timing dependent
- [ ] Flashlight lighting / player textures: re-check once the above are done
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

### Hardware test link: run and debug on a real PS2 over Ethernet (planned)

Goal: test on a real PS2 without moving discs or memory cards. A small ELF, started once from
launchELF, connects to this VM over Ethernet. From the VM we can then (re)start the newest build,
read its log live, get crash reports and RAM dumps, and reset it, all with the same tools we use in
PCSX2. Motivation: on hardware (OPL, ISO on the internal HDD) the attract demo plays, but starting a
new game crashes, and we can't see why.

**Design decisions** (recommendations; to be confirmed):
- **The game reads its data over the network itself.** The port's libcd (`libcd_ps2.c`) gets a
  second back end that reads SILENT./HILL. from the network instead of the disc drive. The
  alternative, booting the ISO through Neutrino's disc emulation, takes the network away from the
  game: Neutrino owns the IOP and the Ethernet driver, so there'd be no live log or remote reset. The
  disc path stays as it is for DVD/OPL builds.
- **Protocol: UDPFS rather than UDPBD.** Both are rickgaiser's (Neutrino). UDPBD serves a raw
  block device (port 48573), so the ISO and any output files would have to live inside a disk image
  (exFAT) that the VM can't safely touch while the PS2 has it mounted. UDPFS serves a folder file by
  file (reference server in Python, read/write), so the VM can serve `build/port/` directly: the
  ISO, the ELF, and the files the port already uses through `host:` (warp.txt, input.txt,
  capture.txt, ramdump.bin, frame dumps). Fallback if UDPFS doesn't work out: UDPBD with an exFAT
  image, results read back after each run.
- **`host:` keeps working on hardware.** In the network build `host:` paths map to the UDPFS share,
  so warp sweeps, scripted input, frame capture and the load watchdog work on the PS2 unchanged.

**Pieces**:
1. *VM side: `tools/port/ps2_link.py`* (one program, several channels):
   - file server: the UDPFS server for `build/port/`;
   - log receiver: the port's console output (udptty, UDP), written to a log file and the terminal
     in the same format as `pcsx2_run.py` output, so the existing parsers (heartbeat, `--until`,
     `--gameplay`, stall detection, the sweep) work on hardware;
   - control: commands to the PS2 (`run` [args], `reset`, `ping`, `dump`), sent over UDP;
   - CLI: `ps2_link.py run --until ... --gameplay 30`, as close to `pcsx2_run.py` as possible
     (ideally `pcsx2_run.py --hardware`).
   - The VM needs a bridged network adapter (the PS2 must reach it on the LAN); a fixed IP for both.
2. *PS2 side: launcher ELF `sh1link.elf`* (started from launchELF; small, rarely changes):
   - loads the IOP modules: DEV9, Ethernet (SMAP), the UDP transport, UDPFS client, udptty;
   - network settings from a config file next to the ELF (IP, VM address), or DHCP;
   - waits for `run`, then loads `sh1.elf` from the share and starts it (LoadExecPS2 with arguments:
     `net`, the VM address), so every run is the newest build without touching the PS2;
   - shows a status screen (IP, "waiting for VM", last error) so problems are visible on the TV.
3. *Game side: network mode in the port* (`SH1_NET=1` build, or the `net` argument):
   - after its own IOP reset the game loads the same network modules again (embedded IRX);
   - libcd back end reading `sh1_ps2.iso` from the share (sector reads at file offsets; the same
     async request/chunking as the DVD path);
   - console output through udptty (printf already goes to the console);
   - a debug agent: a high-priority EE thread woken by a SIF command from a small IOP module that
     listens for control packets. So `reset` (LoadExecPS2 back into `sh1link.elf`) and `dump` work
     even while the main loop is stuck, though not after a hard EE lockup;
   - the crash handler (`crash_ps2.c`) logs registers and a stack dump over the network, writes
     `ramdump.bin` to the share, then waits for `reset`.
4. *Recovery:* if the EE is hard-locked (interrupts off, kernel crash), only a physical reset or
   power cycle helps; launchELF can be set to autostart `sh1link.elf` to make that one button
   press.

**Progress**:
- [x] Phase 0: Neutrino v1.8.0 boots `sh1_ps2.iso` over UDPFS from the VM on the user's Fat PS2 (VM on
  a bridged adapter: 192.168.1.222; PS2 192.168.1.10). One launchELF version failed to start it.
- [x] Phase 1: console output over the network: Neutrino's ministack already broadcasts IOP tty
  writes (which carry the port's printf) to UDP 18194; `tools/port/ps2_log.py` receives them.
  First hardware crash found and fixed (NULL read in Gfx_EffectsUpdate starting a new game), then
  a second (WorldMap_ChunkLoadStateGet entering map0_s01).
- [x] Crash restart: after the report, a 6 s countdown, then `LoadExecPS2` of the disc's boot ELF,
  which Neutrino turns into a reload from the VM; Triangle held 3 s keeps the crashed state. An IOP
  reset of our own (to restart Neutrino from USB) hangs under Neutrino (the module loader never
  answers afterwards, with or without `-gc=3`).
- [x] NULL-read safety net on hardware: addresses 0-0x1FFF mapped read-only to zeros (as PCSX2
  behaves); skipped in PCSX2 (host file system present), where "TLB Miss" lines show sites to fix.
- [x] Phase 3: remote control. `sh1agent.irx` (src/port/iop/sh1agent, embedded in the ELF, loaded by
  the game; only loads where ministack is present) listens on UDP 62968 and DMAs each command into
  an EE mailbox; the vertical blank handler wakes an agent thread above the game's priority.
  `tools/port/ps2_ctl.py ping|restart|deploy`; `tools/port/udpfs_serve.py` (Neutrino's server plus a
  switch of open images to the new build on deploy). Restart ~7 s; deploy tested end to end.
- [x] Hardware testing tools: crashes keep their state by default (`SH1_CRASH_RESTART=1` for the
  countdown), `ps2_ctl.py md` reads memory, `SH1_WATCH_PACKET=1` checks the packet pointer around
  recompiled calls; found the item-pickup crash (GsTMDfast*LFG declared void)
- [ ] Exit to the PS2 browser under Neutrino (LoadExecPS2 rom0:OSDSYS fails in its environment)
- [ ] Phase 4: tool integration (`pcsx2_run.py --hardware`, sweeps and benchmarks on hardware, RAM
  dumps over the network).

**Phases** (each ends with a test on the real PS2):
- *0. Verify the building blocks.* Which modules exist (ps2sdk here has `smap.irx`, `udptty.irx`,
  `bdm.irx`, `bdmfs_fatfs.irx`, `netman.irx`, `ps2ip*`; `smap_udpbd` and the UDPFS client come from
  Neutrino's sources); the UDPFS protocol and server; whether udptty and the UDPFS/UDPBD transport
  can share the Ethernet driver; PS2 model (Fat with network adapter or Slim). Test: Neutrino boots
  the ISO over UDPFS on the user's PS2 (rules out network/VM setup problems early).
- *1. Log only.* `sh1.elf` started from launchELF/USB as today, with udptty: its console appears
  on the VM. Test: the new-game crash is captured with the crash handler's registers. This alone
  may explain the hardware crash.
- *2. Game data over the network.* UDPFS client and the libcd back end; `host:` mapped to the share.
  Test: boot to the title and the attract demo with the ISO on the VM; scripted input reaches
  gameplay.
- *3. Launcher and remote control.* `sh1link.elf`, `run`/`reset`, debug agent. Test: from the VM:
  build, run, reset, run again, without touching the PS2.
- *4. Tool integration.* `pcsx2_run.py --hardware`, crash/hang dumps to the share, the warp sweep
  and the cutscene benchmark on hardware. Test: one sweep on the real PS2.

**Risks / unknowns**: Ethernet throughput with the game's streaming (UDPFS should manage MB/s; the
game needs a few hundred KB/s); the network modules' IOP memory next to the sound driver later;
timing differences from network reads (the async libcd path already copes with slow reads); a
hard lockup can't be reset remotely.

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

- 2026-10-08 (hardware): attract-demo freeze found. The main thread was spinning in gpu_gs.c download_rows (water effect StoreImage → GS→EE VRAM readback), waiting on VIF1 DMA (D1_CHCR) forever. It found this by reading the stack over the network: `ps2_ctl.py md 0x1FFC000 0x4000`. Fix: clear FINISH, wait for the GS's FINISH before turning the bus around, add spin timeouts, and on a stuck transfer stop D1 and reset VIF1 with a log line ("VRAM download stuck"). The vblank EPC sampler (`ps2_ctl.py where`) shows only the kernel inside INTC handlers, so use the stack instead. A remote restart from this state hangs, so press RESET.

- Sound (next topic): keep the original sound effects, music and sound code, changing the code only where the SPU2 needs it. Use one SPU2 core only, so the two cores never need syncing.

## Hardware performance findings (2026-10-08 play session, real PS2)

The opening area and alley ran at 35–53 fps. The town (map2_s00) and school (map1_s00/s01) dropped much further, with long stretches (up to 55 s) at 19–32 fps and no idle time. The flashlight's lighting looks like a large part of it: the user saw the drops follow the flashlight. Nothing crashed or froze after the VRAM-download fix.

Ideas to try, roughly by expected payoff:
1. **Measure first.** The vblank EPC sampler only ever records the kernel, so it is useless. Add per-section cycle counters (COP0 Count) around the main loop's parts: world/chunk drawing, characters, the flashlight/lighting path (GsTMDfast*LFG, NormalColor*, LoadAverageCol), the GTE wrappers, GS submission and the water readback. Print them with the heartbeat so a play session gives a breakdown per map.
2. **Lighting and GTE on VU0.** The flashlight path runs, per vertex, the recompiled GTE lighting ops: NCS/NCT (light matrix × normal, colour matrix, depth cue) and RTPT. On the EE these are scalar C. Do them in VU0 macro mode (vmulq/vmadd with 4-wide FMAC), or batch whole meshes into VU0 micro programs. This is the "GTE work to the vector units" step already planned, with the lighting ops first.
3. **Water VRAM readback.** StoreImage currently downloads all of VRAM (1 MB, two 256-row DMAs, waiting on the GS) whenever the water effect asks. Download only the requested rectangle, and skip it when the rectangle hasn't changed since the last frame.
4. **Batch Gfx_MeshDraw.** Planned already: fewer per-primitive calls and better reuse of GS state (TEX0/CLUT changes).
5. **Cache use.** Keep the hot GTE register file and lighting matrices in scratchpad RAM (16 KB at 0x70000000). Check that the -O3 recomp code doesn't thrash the 8 KB data cache with large per-vertex buffers.
6. **Fog/flashlight geometry.** If the flashlight raises the number of primitives (for example by subdividing, or by keeping more chunks in view), check how many primitives each frame sends to the GS in lit areas compared with unlit ones, using the existing gs stats line.

## Sound, step 1: the SPU on SPU2 core 0 (2026-10-08)

- [src/port/libspu_port.c](src/port/libspu_port.c) implements the PS1 libspu calls the driver uses (voice attributes, keys, key status, uploads, reverb, master volume) as SPU2 core 0 register writes. The original driver, sound banks and sequences are unchanged.
  - Sound memory: PS1 address A maps to SPU2 byte address A + 0x20000. That puts the end of the reverb area on a 128 KB boundary (EEA = 4).
  - Pitch is scaled by 44100/48000.
  - Reverb presets are libspu's own table, taken from BODYPROG.BIN.
- [src/port/ps2/spu_ps2.c](src/port/ps2/spu_ps2.c) (EE side):
  - Sends commands by SIF DMA into a ring buffer in sh1spu.irx: batched register writes, uploads in 16 KB pieces, and clears.
  - Reads the IOP's status block, which reports bytes run and each voice's ENVX, through uncached memory.
  - Runs the sound tick: root counter 2's interrupt becomes a kernel alarm (in horizontal blanks) that wakes a priority-20 thread, which delivers the counter event (libapi_port.c Port_EventDeliver) at about 578 Hz.
- [src/port/iop/sh1spu](src/port/iop/sh1spu) runs the commands. It uses ps2sdk's freesd.irx (sceSdInit, plus sceSdVoiceTrans for uploads) and writes registers directly. Core 1 only passes core 0 through (its MMIX takes core 0's input, BVOL 0x7FFF, MVOL 0x3FFF).
- A "spu:" line with statistics appears every 10 heartbeats.
- Patching the IOP module loader twice breaks it, so agent_ps2.c Port_ModuleLoadInit now runs only once.
- Not yet done: XA voice lines (CD input), external input, and the echo/delay feedback parameters.
- Fixes from testing on hardware (2026-10-08):
  - The IOP module loader aligns data to 16 bytes only. SPU2 DMA needs 64, so sh1spu aligns the ring at run time. Before this, uploads failed silently and all 24 voices played blank memory.
  - The control register (ATTR) needs bit 14 (unmute), as the PS1's SPUCNT does. PCSX2 ignores it. Both cores now use 0xC000.
  - Key on and key off writes are spaced by more than two SPU2 sample periods. Batched writes let the SPU2 miss a key off, which left an instrument note stuck in the café cutscene's music.
- Test tool: `pcsx2_run.py --audio out.wav` unmutes PCSX2 and records the sound output with pw-record.

## Demo menu (2026-10-08)
- The main menu has a DEMO entry between START and OPTION (title.c, under SH_PORT). It takes the unused Extra slot.
- DEMO opens a scrolling list of in-game cutscenes, without FMVs, sorted by function name ([src/port/demo_menu.c](src/port/demo_menu.c)). The list is generated at link time by [tools/port/cutscene_list.py](tools/port/cutscene_list.py): it takes map events with SysState_EventCallback whose function uses cutscene machinery (CutsceneBorder, the cutscene timer or flags, DMS). There are 64 entries.
- Picking one starts a new game on that map with Harry at the trigger point. It sets the event's required flag, clears its completion flag, and starts the event directly from Event_Update. After one second of player control, a warm boot returns to the list.
- To do: give the unnamed func_ cutscenes proper names in the decomp.

## Sound, step 2: XA voice lines (2026-10-08)
- The game's XA command sequence reaches the port's libcd ([libcd_ps2.c](src/port/ps2/libcd_ps2.c)): CdlSetmode with real-time mode, CdlSetfilter, CdlSeekL, then CdlReadN. libcd turns it into an OP_XA command for sh1spu.irx. CdlPause and CdlStop send OP_XASTOP. The XA files sit inside HILL. (g_FileXaLoc starts at its first sector).
- [src/port/iop/sh1spu/xa.c](src/port/iop/sh1spu/xa.c):
  - A reader thread reads raw HILL. sectors through the IOP's cdvdman and keeps the audio sectors of the chosen file and channel, in a FIFO of 12 sectors. It stops at the end-of-file flag.
  - A mixer thread, woken by each ADMA half interrupt, decodes 4-bit XA-ADPCM (mono or stereo, 37.8 or 18.9 kHz). It resamples to 48 kHz with the PS1 SPU's Gaussian table ([gauss_table.h](src/port/iop/sh1spu/gauss_table.h), from psx-spx) into core 0's ADMA loop buffer: two halves of 1024 frames.
  - Sample uploads now use DMA channel 1, because channel 0 belongs to core 0's ADMA. ATTR writes keep the ADMA mode bits.
- libspu: the CD volume goes to core 0's AVOL. CD mix and CD reverb go to core 0's MMIX (0x0C0 dry, 0x030 wet).
- The IOP logs "sh1spu: XA file F channel C" and "XA end of file".
- XA fixes (2026-10-09):
  - The ADMA input's volume register is BVOL (0x76C/0x76E), not AVOL. libspu writes the CD volume to both pairs.
  - The mixer refills the half that IOP DMA channel 4 (D4_MADR) isn't reading, once per pass. The interrupt can fire more often than once per half.
  - While a stream runs, the IOP logs the registers, peak level and refill count about every 1.27 s ("XA diag").
- XA static fixed (2026-10-09): the SPU2's ADMA input takes blocks of 256 left samples (512 bytes) followed by 256 right samples, not 128 + 128. With the wrong layout, left/right pieces played out of order.
- XA test tools ([tools/port/xa_test](tools/port/xa_test)):
  - `run.sh INDEX FILE CHAN` builds sh1spu's xa.c natively, decodes a voice line from HILL., and compares it sample for sample with an independent Python reference decoder (xa_ref.py, from psx-spx). They are currently bit-exact.
  - `track.py` / `compare_recording.py` line a PCSX2 recording up against the reference.
  - `SH1_XA_SOLO=1` mutes everything but the XA input.
  - `pcsx2_run.py --audio` turns PCSX2's time-stretching off; with it on, the recording drops and repeats 10–20 ms pieces.
  - Recordings still lose 128-frame chunks steadily, probably the host audio output running behind emulation.

## Voice lines vs. frame rate and timing (2026-10-09)
- Frame drops at each voice line: the IOP's XA reader waited on its disc reads with `sceCdSync(0)` (50–120 ms). During that wait the sh1spu command thread didn't run, so the EE's sound driver (tick thread, priority 20) spun on stale voice status and starved the game: one frame of 100–230 ms per line. The reader now polls `sceCdSync(1)` with a 1 ms delay. Per-second frame counts at line starts went from 36–41 to 47–52, and the spikes are gone.
- Frame time loss: counter 1 (GsGetVcount) was reset to "now" after drawing, so the GS conversion time between the read and the reset was never counted, and slow scenes ran in slow motion. ResetRCnt(CNT1) now goes back to the last read (rcnt_ps2.c).
- At 60 fps (SH1_FPS=60), message text now rolls out by elapsed time, as at 30 fps on the PS1 (map_msg_display.c).
- Still open: in map3_s00 func_800D0CF8, message 15 is shown twice (steps 5/7, then step 9 restarts it), so every later voice clip plays one page early and is cut when its page ends. Clip and page lengths match exactly when shifted by one. This looks like a race between the message's 1 s page and the cutscene timer reaching 25.
- Debug build switch: SH1_MSG_DEBUG=1 (with SH1_EXTRA_CFLAGS=-DSH_PORT_MSG_DEBUG for the game files). It logs per-second frames, frame time and text timer; XA line starts; libcd/SPU calls over 2 ms; and spikes, which are PROF sections over 30 ms.
- PS1 drive timing (2026-10-09). The PS1 reference ([tools/port/duckstation_cutscene.py](tools/port/duckstation_cutscene.py) with [gdb/cutscene_ps1.py](tools/port/gdb/cutscene_ps1.py)) plays a Demo-list cutscene on the unmodified game in DuckStation and logs steps, pages, XA load states and starts/stops:
  - It starts a real New Game through the menu, then warps to the map.
  - Options: `--steps`, `--fps60` (DuckStation's 60 FPS patch), `--overclock`. DuckStation runs muted.
  - Findings: the PS1 pairs every voice line with its page at 20 fps, and also at 60 fps with a 300% overclock. So frame rate wasn't the cause.
  - The difference was the drive. A voice line's page starts once its read starts, which on the PS1 waits for the seek: about 26 blanks from the map data to the voice area, 4–7 blanks after the game's preload.
  - libcd now models seek time in real time: after CdlSeekL, CdSync(1) reports busy for 4 + 44·√(distance/200000) vertical blanks (SEEK_* in libcd_ps2.c).
  - Result: map3_s00 func_800D0CF8 pairs every line like the PS1. 29 of 32 lines play to the end; the PS1 cuts the same three at the same points (their clips end in silence).
- SH_PORT_CUTSCENE_VBLANKS (default 1) is the minimum vertical blanks per cutscene frame in 60 fps mode. Test only.

## Performance pass 1 (2026-10-09), Kaufmann cutscene (map3_s00 func_800D0CF8)
- Profile on the PS2 (SH1_PROF=1, printed over the network), per frame at a 4,915k-cycle 60 fps budget: about 7,050k-cycles in total.
  - GS conversion (DrawOTag): 3,270 (polygon setup 2,023, per-primitive state 1,206, DMA sends 327).
  - GTE: ~1,640.
  - Rest of the game: ~2,100.
  - Sound task: 430–580 on hardware against ~110 in PCSX2 (synchronous SPU sends).
- Done: prim_state is split into a non-texture memo and a texture memo. Texture page/CLUT changes (~540 a frame) only redo the texture registers. State cost went from 1,206 to ~760k-cycles on hardware; frames are unchanged in PCSX2.
- Remote start: `ps2_ctl.py demo "<Demo list name or number>"` starts a Demo cutscene. From inside the game it soft-resets to the menu first. A second request is ignored while one is being started.
- Tried and reverted, both of which hung a real PS2 at the logo screens with interrupts off (PCSX2 was fine):
  - Asynchronous GIF sends: GIF DMA channel registers driven directly, waiting only before the next send.
  - Asynchronous SPU command sends: double-buffered SIF DMA without waiting. Sends run inside DI(); the kernel seems to start queued SIF transfers only from its interrupt handler, so SifSetDma spun forever once the queue was full.
  - Both need designs that never wait with interrupts off: for example, a sender thread for SPU commands, and GIF sends checked on hardware first.
- Tried, no measurable gain on hardware, removed: a 16-entry cache of recent texture page/CLUT pairs. The lookups aren't the cost of a texture change.
- Next candidates:
  - Finer profile of the per-texture-change and per-polygon cost on hardware.
  - SPU sends from a sender thread.
  - GTE on MMI.
  - Primitive conversion on VU1.
