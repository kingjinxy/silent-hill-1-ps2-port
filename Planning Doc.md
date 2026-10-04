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

### Step 1 — Fixed addresses / shiftability  *(in progress)*

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
- [ ] Symbolise/migrate the raw pointer words:
  - [ ] `MAP_MESSAGES` tables in map4_s00 (`365C.data.s`), map4_s06 (`363C.data.s`), map6_s05 (`3744.data.s`)
        — migrate to C like map4_s01 does
  - [ ] map7_s03 `23CA0.data.s`: pointer tables into its own rodata (`D_800ED7E0` etc.) and two
        fixed-buffer pointers (`D_800ED230` = `FS_BUFFER_20`, `FS_BUFFER_18`)
  - [ ] bodyprog `D_80028A18` (`3EB8.rodata.s`) = `0x80052F00`, a code pointer
- [x] Check whether any fixed buffer overlaps a linked section
      — map overlays end ≤ `0x800F5978`, below the lowest buffer (`0x800F5E00`): no overlap.
      Screen overlays (STREAM/SAVELOAD/OPTION/STF_ROLL) load at `0x801E2600` *on top of*
      `FS_BUFFER_1`/`5`/`6`/`21`/`9`/`10`/`16`, `IMAGE_BUFFER_3` and the `0x801E2E00` OT. That
      area is dead while a screen is active; in the port it simply stops being clobbered.
- [x] Add a regression check script that fails on new raw address literals
      (`tools/port/find_fixed_addresses.py`)
- [ ] Shift test: build with padding inserted and confirm the game still boots in an emulator
- Deferred to Step 2: `g_OvlBodyprog` / `g_OvlDynamic` overlay load addresses in `src/main/main.c`
  (they disappear once overlays are statically linked).

### Step 2 — Static linking of overlays

- [ ] Namespace per-map symbols (`Map_WorldObjectsInit`/`Update` ×42, `sharedFunc_*` in 95 files)
- [ ] Replace "load overlay + jump" with a per-map dispatch table
- [ ] Reset each overlay's `.data` on "load" (snapshot/restore), matching PS1 reload semantics
- [ ] Remove `g_OvlBodyprog` / `g_OvlDynamic` fixed addresses
- [ ] Decide what to do with the remaining `INCLUDE_ASM` functions (need C for the port)

### Step 3 — Software GTE

- [ ] Bit-exact C GTE (register file struct, saturation/flags, UNR divide table)
- [ ] Replace `gte_*` macros and DMPSX raw opcodes with calls into it under `SH_PORT`

### Step 4 — Graphics (libgpu → GS)

- [ ] Walk the PS1 OT at `DrawOTag`, translate primitives to GIF packets
- [ ] VRAM emulation: 1024×512 shadow + texture cache keyed by tpage/CLUT, invalidated on
      `LoadImage`/`MoveImage`/CLUT writes
- [ ] STP-bit semi-transparency (TEXA/AEM + alpha-test two-pass)
- [ ] Blend modes (correct table below), texture window via `REGION_REPEAT`, dithering decision

### Step 5 — Pad, CD sectors, memory card

- [ ] libpad / libkpad shim over `padman`
- [ ] libcd shim: LBA reads from `SILENT.`/`HILL.` (sector offsets relative to container files)
- [ ] Memory card: save format + `icon.sys`/icon, real title-ID folder

### Step 6 — Sound driver on the IOP

- [ ] Port Konami SMF sequencer (`src/bodyprog/libsd`) to an IRX, or batch SPU commands over SIF
- [ ] Pitch rescale for SPU2's 48 kHz (×44100/48000); reverb mode mapping

### Step 7 — XA audio and STR movies

- [ ] Raw Mode 2 Form 2 sector reads, software XA-ADPCM decode, stream to SPU2
- [ ] Software MDEC decode on the EE

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
