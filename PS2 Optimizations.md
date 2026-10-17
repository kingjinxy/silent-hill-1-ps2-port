# PS2 Optimizations

A working plan for making the port fast on the PS2 without giving up accuracy. It covers how the PS1 and PS2 differ, where Silent Hill is tuned for the PS1, the options for the port (ranked), and a log of what was tried, measured and kept. Updated as the work goes on.

Sources:
- Rodrigo Copetti, [PlayStation Architecture](https://www.copetti.org/writings/consoles/playstation/).
- Rodrigo Copetti, [PlayStation 2 Architecture](https://www.copetti.org/writings/consoles/playstation-2/).
- [PsyZ](https://github.com/Xeeynamo/psyz): a native PSY-Q replacement library, with a PSP backend.
- psx-spx (GTE, GPU).
- This repository's own profiles.

---

## 1. The two machines

| | PS1 | PS2 (EE side) | Consequence for the port |
|---|---|---|---|
| CPU | MIPS R3000A, 33.87 MHz, scalar, 5-stage pipeline | MIPS R5900 "EE", 294.9 MHz, 2-way superscalar, 6-stage, 32 × 128-bit registers, MMI SIMD instructions | ~9× the clock. But memory latency hasn't scaled with it, so cache misses dominate. |
| Caches | 4 KB I-cache, **no data cache**, 1 KB scratchpad (D-cache used as SRAM) | 16 KB I-cache, **8 KB D-cache** (2-way), 16 KB scratchpad (SPR, 0x70000000) | PS1 code assumes every load costs the same. On the EE a D-cache miss to RDRAM costs tens of cycles. |
| RAM | 2 MB EDO | 32 MB RDRAM, dual channel (~3.2 GB/s theoretical) | 16× the space: files can stay resident, buffers needn't be shared. |
| 3D maths | GTE (COP2): fixed point; RTPS/RTPT, NCLIP, MVMVA, DPCS/DCPL, NCCS... in a few to ~20 cycles each | VU0 (COP2 macro mode, or microprograms), VU1 (microprograms only, feeds the GS) | No GTE; the port computes GTE results in C/MMI. The VUs are float hardware (see §5). |
| Graphics | GPU: 2D rasterizer, 1 MB VRAM shared by framebuffer and textures, no Z-buffer (ordering tables), affine texturing, 4/8/16-bit textures with CLUTs, 2 KB texture cache, dithering | GS: 147 MHz, 4 MB eDRAM, 8 textured / 16 untextured pixels per cycle, Z-buffer, perspective-correct texturing, CLUT buffer, per-vertex fog (FOG/FOGCOL), alpha blending, three GIF paths (PATH1 VU1, PATH2 VIF1, PATH3 direct) | Fill rate is effectively free at PS1 resolutions. The cost is the CPU turning PS1 GPU packets into GS packets and state. |
| DMA | Per-device channels; the CPU stalls while DMA owns the bus | DMAC: 128-bit transfers, chains (tags), slice mode, channels for VIF0/1, GIF, SIF, SPR | GS packets can be built and sent while the CPU keeps working (double buffering). |
| Video | MDEC | IPU (MPEG-2 macroblocks) | Done (FMV path). |
| Sound | SPU, 24 voices, 512 KB | SPU2: 2 cores × 24 voices, 2 MB, on the IOP | Done (one SPU2 core, PS1 driver unchanged). |
| Storage | 2× CD-ROM | DVD (on hardware tests: Neutrino over Ethernet) | Faster, and with RAM to spare, loads can be avoided entirely. |
| I/O processor | — | IOP: R3000-class CPU, the PS1's CPU in backward-compatibility mode | Runs our sound and disc modules. |

The key asymmetry: the PS1 put its expensive work into fixed-function hardware (GTE, GPU) and kept the CPU's part small. On the PS2 neither exists in that form, so the EE does the GTE's maths and the GPU's packet interpretation. The EE is fast, but sensitive to cache misses and to many small, dependent operations.

## 2. Where Silent Hill 1 is tuned for the PS1

- **GTE everywhere.** World and character meshes transform with RTPT (three vertices per command). They use NCLIP for back faces, MVMVA for light dot products, and DPCS/DCPL for depth cueing (fog) and lighting per vertex. Each is a few cycles on the PS1; in the port each is an inline sequence or a function.
- **Scratchpad as a register file.** Matrices, temporary vectors and packet scratch live in the 1 KB scratchpad (`PSX_SCRATCH`, e.g. `func_800CB25C` in map5_s00).
- **Ordering tables instead of a Z-buffer.** Primitives are linked into depth buckets; the GPU walks the chain. The port walks the same chain in `DrawOTag` and converts each primitive.
- **Fog as a second polygon.** Fogged world faces are drawn twice: a textured polygon and a semi-transparent untextured "fog" polygon over it. In the Kaufmann cutscene, prim_state's non-texture part was recomputed for 1,047 of 1,448 polygons, because the state alternates every polygon.
- **CLUT-heavy 4/8-bit textures.** About 500 texture page / CLUT changes per frame in heavy scenes.
- **The 2 MB budget.** Per-map overlays, small shared file buffers (FS_BUFFER_n), reloads on every map change. The DMS buffer reuse that breaks some Demo starts comes from this.
- **Frame rate.** Cutscenes run at 15–30 fps on the PS1: median ~22 in the 2026-10-10 DuckStation sweep, with a range of 9.8 (monster Cybil) to 59.7 (static shots).

## 3. Where the port spends its time today

Kaufmann cutscene (map3_s00 func_800D0CF8), PS2 hardware, profiling build, 2026-10-10. Budget at 60 fps: 4,915k EE cycles per frame.

| Part | k-cycles per frame | Notes |
|---|---|---|
| Game update | ~2,800 | Game logic plus GTE work (MVMVA 419 → 195 after the light dot product moved to C) |
| `DrawOTag` (PS1 packets → GS) | ~2,600 | Polygon setup ~1,600, per-primitive state the rest; ~500 texture/CLUT changes |
| GsSwapDispBuff | ~40 | |
| Pad polling | 15 | Was 216 before mode info was cached |

Frame rate:
- Kaufmann cutscene: 50–55 fps on hardware, 52.8 fps in PCSX2, 19.8 fps on the PS1.
- Across 43 cutscenes, the port in PCSX2 against the PS1 in DuckStation: median 2.00×, range 1.00×–3.07×.
- PCSX2 doesn't model the EE caches, so hardware figures can differ.

## 4. Options, ranked

Ranked by expected gain relative to risk. Each must keep output identical to the PS1, or prove its differences invisible, using the frame comparisons against DuckStation and the deterministic benchmark (tools/port/bench_frames.sh, compare_dumps.py).

1. **Collapse the fog double polygons.** The GS's per-vertex fog (FOG in the vertex data, FOGCOL, FGE in PRIM) blends toward a fog colour, as DPCS does. One fogged, textured polygon instead of a textured one plus a fog overlay would roughly halve world primitives and end the state alternation. Accuracy question: the GS interpolates fog per pixel and rounds differently from the PS1's blend of the overlay. The plan is to measure, then compare frames.
2. **Exact batched GTE work in the game's own mesh code.** (First written as "native C for libgs's model renderer". That was wrong: the game calls `GsSortObject4J`/`GsTMDfast*` only from the item screens. Characters and world are drawn by its own C code, Gfx_MeshDrawPort and helpers; see the 2026-10-10 log entry.) Replace per-command GTE emulation in the hot loops with exact batch functions (Gte_RtpBatch, Gte_NcBatch), the last command of each loop still on the GTE so its registers end up as on the PS1. Verified with SH_PORT_CHECK_BATCH builds.
3. **Scratchpad on the EE's SPR.** `g_PsxScratch` is ordinary cached RAM today (0x8a2f40). At 0x70000000 every access is single-cycle and nothing is evicted. Watch out for any DMA straight from scratchpad memory: SPR needs its own DMA channel.
4. **Texture page and CLUT cache.** Largely in place already: gpu_gs.c caches 4/8-bit pages (28 slots, PSMT4/PSMT8 uploaded from g_PortVram) and CLUTs (128 rows, CSM2). What's left: ~540 TEX0 changes and as many CLUT loads per frame in the Kaufmann cutscene. A CLUT load is skipped only when the GS still holds the same CLUT, so alternating textures reload every time. Possible: order or batch by texture within an OT bucket (not allowed: draw order is visible with semi-transparency), or make a reload cheaper. (PsyZ's PSP backend uses the same page/CLUT cache design.)
5. **CPU/GS overlap with a double display list.** Build frame N+1's GIF packets while frame N's DMA runs, waiting only for the previous frame (PsyZ's PSP model). Async GIF sends were tried and reverted on 2026-10-09; those freezes were probably the ResetCallback bug. Retry on hardware.
6. **Packet building with MMI and the SPR.** 128-bit `sq` stores of whole GIF qwords; packets built in SPR and sent with the fromSPR → GIF DMA.
7. **Use the RAM.** Keep a map's files (and its neighbours') resident, cache XA headers, preload the next scene. Faster loads, and it supports a future movie mode.
8. **VU1 for primitive conversion.** PS1 packets in, GIF packets out on PATH1. The largest potential gain and the largest job. GTE results stay exact on the EE; VU1 only does format conversion, which must be validated pixel by pixel. After items 1–5.

Not recommended: moving GTE maths to VU0 or VU1 (§5).

## 5. Why the Vector Units can't do the GTE's integer maths

Copetti describes the VU's execution unit as split into two parallel halves: "the first one multiplies or adds floats, while the other one divides floats or operates on integers". That suggests integer GTE work could run there. It can't, in practice:

- **What the halves are.** The upper unit does the real work: four 32-bit float multiply-adds per cycle (FMAC). The lower unit handles the divide/sqrt unit (FDIV), loads and stores, branches, and 16 × **16-bit** integer registers (VI00–VI15). Its integer instructions are add, subtract, AND, OR, add-immediate and branch-compare. **There is no integer multiply or multiply-accumulate.** Those registers are for loop counters, VU memory addresses and flags, so a microprogram can run its own loop while the float unit computes.
- **What the GTE needs.** 16×16-bit products accumulated into 44-bit registers. Exact saturation and overflow flags (FLAG register), the shift by 12 (sf), limiting (lm), and the UNR-table Newton-Raphson division for perspective. Silent Hill's output depends on those bits: the port's inline GTE paths match the hardware with 0 mismatches over millions of calls (SH_PORT_CHECK_BATCH).
- **Why float isn't enough.** A VU float has a 24-bit mantissa, so integers above 2²⁴ aren't exact. One 16×16 product can reach 2³¹, and a matrix row sums three of them. Rounding then moves vertex positions, depth buckets and light levels by a unit here and there. `ITOF12`/`FTOI12` (fixed point with 12 fraction bits) look made for the GTE's Q12 values, but they only convert at the edges; the arithmetic in between is still 24-bit float.
- **Workarounds, and why not.** (a) Split values into pieces small enough that every partial product is exact, then recombine: exact, but 3–4× the work, and flags and saturation still need integer logic the VU lacks; likely slower than the EE path. (b) Prove input ranges per call site so float is exact: possible for a few sites, fragile, and a wrong assumption silently changes output.
- **Where exact integer GTE maths belongs:** the EE's MMI instructions, which have real integer multiplies. `PMULTH` does eight 16×16 multiplies at once, `PMADDH`/`PHMADH` multiply-accumulate halfwords, `PMULTW` gives 32-bit products. The port's GTE work is on this path.
- **Where the VUs fit:** float-friendly work, or work whose output isn't GTE maths, e.g. building GS packets from already-final coordinates (option 8).

## 6. Ideas from PsyZ

PsyZ trades accuracy for compatibility ("not meant for accuracy"), but several ideas fit an accuracy-first port:

- **Native libraries instead of recompiled ones.** PsyZ compiles the decompiled PSY-Q libraries natively, with no MMIO, BIOS or chip emulation. On Castlevania SotN: 1% CPU native, against 10% recompiled and 16% emulated. For us, this means libgs's object and TMD renderers (option 2).
- **A GTE validated against real hardware.** psyz/src/psyz/libgte.c is integer-exact, with unit tests confirmed green on a real PS1 (psyz/tests/test_libgte.c). Its comments name the PS2 as a target for "different code paths to use hardware accelerated math, while ensuring a decent level of accuracy". Their test vectors could check our MMI GTE paths for free.
- **PSP GPU backend** (src/psp/psp_gpu.c, against a fixed-function GPU like the GS):
  - vertices batched into a ring, one draw call per run of identical state;
  - texture-page and CLUT caches, invalidated on VRAM writes;
  - uncached writes for display lists;
  - two display lists, kicked asynchronously with stall addresses, waiting only on the previous frame.
  Most of this maps directly onto GIF packets and the DMAC.
- **Layout.** A clean platform split ("no `#ifdef` spam") and sample programs and self-tests that run on real hardware. A model for the port as it grows.

## 7. Log

### 2026-10-10: plan written
Profile and options above. First item: the fog collapse (option 1), measured in PCSX2.

### 2026-10-10: fog pairs measured (option 1)

What the world mesh code draws (Gfx_MeshDrawPort's lit-and-fogged path, src/bodyprog/gfx/bodyprog_80056D8C.c ~1060-1135, and the same in the mesh function at ~1880):
- `poly1`: POLY_G4, colour per corner `DPCS(field_C, t)`, with `t = 1 − depth·16 − field_4`. It lerps between the fog colour field_C and the GTE far colour FC.
- `poly3`: POLY_GT4, colour per corner `DCPL(field_8, light, depth)`: the base colour times the light, lerped toward FC with depth.
- Common branch: `poly1` opaque underneath, `poly3` semi-transparent on top. Face flag bit 15 branch: the same pair with mask-bit priority packets around it (16 check-mask prims a frame).

Counted in gpu_gs.c (`gs fog pairs per frame`, a probe in polygon()):
- Kaufmann cutscene: **459–565 pairs a frame, all one kind**: an opaque gouraud quad under a semi-transparent textured gouraud quad in **mode 1 (additive, B+F)**, on the same four corners.
- That's ~64% of the ~1,445 primitives a frame.

PS1 result per pixel:
- texels with STP set: `clamp(underlay + texel × colour)`, i.e. with FC = 0, `texel × light × (1−d) + fog × d'`;
- texels without STP: `texel × colour`, with no fog;
- transparent texels (0x0000): the underlay alone.

GS fog (FOG per vertex, FOGCOL, FGE) computes `C × f + FOGCOL × (1−f)` per pixel, for every texel.

So the collapse has the right form, but it isn't exact:
- texels without STP would gain fog;
- the fog factor is one 8-bit value interpolated per pixel, against the PS1's two separately rounded gouraud colours;
- `d'` (DPCS) and `d` (DCPL) differ by field_4;
- FOGCOL is one register, while the underlay's colour can vary per mesh (field_C).

Ceiling test (`SH1_FOG_TEST=1`, `SH_PORT_FOG_TEST` in gpu_gs.c: the underlay of each pair is dropped, so the picture is wrong). PCSX2, Kaufmann cutscene, averaged over all profile reports:

| | DrawOTag k-cycles | gs: polygon | game update | fps (`demo: ... fps`) |
|---|---|---|---|---|
| normal | 2,918 | 1,929 | 2,841 | 50.3 |
| underlays dropped | 2,233 | 1,217 | 2,854 | 56.6 |

The collapse can save at most ~685k cycles a frame (23% of DrawOTag, 12% fps here), and only by giving up exactness.

Exact alternative to try first, **pair fusion**: keep both GS primitives but convert the pair as one unit. Parse once, compute the four XYZ once and share them, and emit the two GS primitives back to back with their two fixed states prebuilt (no prim_state recomputation as the state alternates). Expected: a good part of the 685k, with identical output (checkable with bench_frames.sh/compare_dumps.py).

### 2026-10-10: pair fusion (option 1, exact): kept

gpu_gs.c GpuGs_Commands:
- An opaque gouraud quad (GP0 0x38) is held until the next command. If that is its partner (0x3E, semi-transparent textured gouraud quad, additive, same four corners), polygon() draws both under the textured one's state (`under`). Otherwise the held quad is drawn first, as before.
- Only the PRIM bits differ (no TME, no ABE), so nothing is switched between the two.
- The underlay's alpha is 0x40 instead of 0: it passes the textured state's alpha test (alpha != 0), and the frame buffer stores alpha's top bit (0) as before, so the mask bit is unchanged; FBA sets it with set-mask either way.
- Every other entry point (LoadImage, MoveImage, ClearImage, Download, DisplayCopy, Flush, Sync, StateLost) draws a held quad first (pend_flush).
- SH1_NO_FOG_PAIRS=1 builds draw the pairs separately (the reference).

Exactness, checked with a new deterministic cutscene benchmark (tools/port/bench_cutscene.sh N OUTDIR):
- SH1_BENCH builds step Demo cutscenes by one simulated vertical blank a frame (game_main.c), with the VSync callback run by the main loop.
- The sound driver's voice line timing reads the simulated clock (sd_call.c SD_XA_VSYNC_COUNT, libetc_ps2.c Port_SimVBlanks). Only there: making VSync(-1) itself simulated froze the game (the disc's seek-timing loop waits within a frame for it to move).
- The Demo menu dumps a frame every 200 frames after the event starts.
- Two runs of one build: 10 of 10 identical.
- Fused against separate, Kaufmann cutscene: walls, floor and every fogged surface identical. Differences only in subtitle rollout (pages start when a voice line has loaded from disc, in real time) and character poses tied to it. Inspected visually (dumps 1 and 6).

Performance, PCSX2, Kaufmann cutscene, profiling build, averaged over the scene:

| | DrawOTag k-cycles | gs: polygon | gs: fog pair | fps |
|---|---|---|---|---|
| separate (SH1_NO_FOG_PAIRS) | 2,928 | 1,927 | — | 50.2 |
| fused | 2,366 | 612 | 739 | 55.3 |
| ceiling (underlays dropped) | 2,233 | 1,217 | — | 56.6 |

−562k cycles a frame (−19% of DrawOTag), +10% fps: 82% of what dropping the underlays saved, with the picture unchanged.

Next: the remaining ~2.4M in DrawOTag (per-primitive parsing and packing, ~540 TEX0/CLUT changes), and option 2 (native libgs renderer) for the 2.8M game update.

### 2026-10-10: character meshes, exact batches (option 2): kept

Where the game update goes. Kaufmann cutscene, PCSX2, GTE command call sites ("gtesites" lines, matched against the build that produced them):

| call site | command | calls a frame |
|---|---|---|
| Vw_MultiplyAndTransformMatrix | MVMVA | 318 (bone matrices) |
| func_8005A900 | RTPT | 221 (character vertices, 3 each) |
| func_8005AA08 | NCT | 220 (character vertex lighting, 3 each) |
| Particle_SnowDraw | RTPS | 117 |
| func_8006B318, WorldEnv_LightTransform | MVMVA, GPF | 94, 82, 41 |

Costs per command through the general GTE emulator: RTPT ~1,400 cycles, NCT ~840, MVMVA ~360, RTPS ~600.

An -finstrument-functions profile also lists Port_DepthCue/Dcpl/Dpcs (the fog colours, ~3,400 calls a frame) high, but that build turns inline functions into real calls with a hook each, so small functions are much inflated there.

Done:
- func_8005A900 (port version): all vertex triples but the last through Gte_RtpBatch, as func_80057B7C already did for the world mesh. Before the last RTPT, SZ3 is set to the batch's last depth, so the FIFO push leaves SZ0 as the PS1 loop does.
- func_8005AA08: new Gte_NcBatch (gte.c: NC with sf = 1, lm = 1, as NCT 0x0D80420 computes each vertex; the arithmetic of normal_color_small mode 0). All normal triples but the last go through it. The last runs through the original code: scratch fields, ldv3c, NCT, strgb3.
- Check build (SH1_EXTRA_CFLAGS=-DSH_PORT_CHECK_BATCH), Kaufmann cutscene: **0 mismatches** over 2,162,691 character vertices and 2,146,305 normals (world batch and NCLIP checks also 0).

PCSX2, Kaufmann cutscene, profiling build:

| | game update | GTE RTPT | GTE NCT | fps |
|---|---|---|---|---|
| before | 2,849 | 379 | 185 | 55.3 |
| after | 2,621 | 109 | 29 | 57.3 |

Next in the game update: MVMVA (178k, bone matrices in Vw_MultiplyAndTransformMatrix), RTPS (115k, mostly snow particles), the remaining ~1.9M of game C code (to profile with PROF sections, since the instrumented build distorts small functions).

### 2026-10-11: bone matrix transforms (Vw_MultiplyAndTransformMatrix MMI): kept

- `Vw_MultiplyAndTransformMatrix` (src/bodyprog/view/vw_calc.c) moved off emulated GTE MVMVA (`gte_rtir`/`gte_rtirtr`) to EE MMI (`PHMADH` 128-bit SIMD) with bit-exact scalar C fallback.
- GTE call site profile: eliminated all 318 MVMVA calls per frame from `Vw_MultiplyAndTransformMatrix`.
- Dynamic verification (`-DSH_PORT_CHECK_BATCH`): **0 mismatches** over >540,000 bone matrix checks in the Kaufmann cutscene.
- Pixel exactness checked with `bench_cutscene.sh`: frame diff bounding box is `None` (100% bit-for-bit identical rendered pixels).
- Review fixes (2026-10-16): the translation now takes the low 16 bits of `inMat->t`, as `gte_ldlvl` loads them into IR1-3, and adds with wraparound like MAC (the cutscene's values fit in 16 bits, so its check couldn't see this); `hi`/`lo` added to the asm clobbers (PHMADH writes them).
- Check build warp sweep (all 43 maps, 15 s each, 2026-10-16): 43/43 pass, **0 mismatches** over ~491k calls.
- PCSX2, Kaufmann cutscene, profiling build, real time, averaged over 117 s (2026-10-16):

| | game update | GTE MVMVA | fps |
|---|---|---|---|
| before | 2,666 | 178 | 57.0 |
| after | 2,581 | 79 | 57.8 |

  About 85k cycles a frame saved (3% of the game update); the 79k MVMVA left is other call sites. Most of the gain is skipping the GTE emulator: only the 3x3 product uses MMI, while packing the halfwords and the translation are scalar.
- Next up: remaining MVMVA sites (e.g. `WorldEnv_LightTransform`, `func_8006B318`) and particle RTPS (`Particle_SnowDraw`).


### 2026-10-16: GTE control writes refresh only what changed: kept

The MVMVA left after the bone matrices (Kaufmann cutscene, calls a frame): func_8006B318 (collision: two `rtv0` per subcell, after loading two rotation rows) 84, WorldEnv_LightTransform (`rtv0`, `gpf12`, `rtv0` per model) 72 + 36 GPF, rc_RotTrans 7. Both functions write the rotation matrix before their commands, and any control write made the next command unpack all three matrices (RT, LLM, LCM) and redo the TR/BK/FC range checks.

- `g_GteCtrlDirty` is now a bit per control register written (gte.h inline writes, `Gte_CtrlWrite`); `ctrl_refresh` unpacks only the matrices whose registers changed and redoes the range checks only after TR/BK/FC writes. Helps every command after a matrix write.
- tools/port/gte_test host check, seeds 0-5: 0 of 2,720 tests differ in each.

PCSX2, Kaufmann cutscene, profiling build, real time, averaged over 117 s:

| | game update | GTE MVMVA | GTE RTPS | fps |
|---|---|---|---|---|
| before | 2,581 | 79 | 115 | 57.8 |
| after | 2,556 | 55 | 108 | 58.0 |

Not done: rewriting func_8006B318 / WorldEnv_LightTransform in plain C would save at most most of the remaining ~55k (~2% of the game update), but each leaves MAC/IR/FLAG and half-written rotation rows in the GTE that later code could read, so an exact rewrite has to reproduce that state.

### 2026-10-16: snow particle RTPS batch: reverted (no measurable gain)

A new-game snow scene (PCSX2, profiling build, 60 fps, 28% idle): `Particle_Update` 393k a frame of a 1,692k game update; its draw loop 305k, of which the snow RTPS 191k (298 flakes, ~640 cycles each); clamping 9k, depth colour 19k; movement loop 86k.

- Tried: Particle_SnowDraw run twice (collect each flake's vertex, `Gte_RtpBatch` over all but the last, then draw from the results; the last flake through the GTE with the FIFO preset, so the GTE ends as on the PS1). Check build warp sweep: 43/43, 0 mismatches over 60k+ flakes.
- Kaufmann cutscene, benchmark build (SH1_BENCH=1): 50.16 fps / 5.2% idle without, 50.03 / 4.6% with: no difference beyond noise. Frame dumps: 9 of 10 identical; the 10th differs only in a subtitle glyph (text timing varies between runs; the two runs took 8,092 and 8,097 vertical blanks for the same frames).
- Why: profiling builds send every GTE command through the general dispatcher and time each one, roughly doubling RTPS's apparent cost; normal builds already take the direct RTPS path, so batching saves only ~100-150 cycles a flake. Read "GTE ..." profile lines with that in mind. Patch kept outside the repo.

### 2026-10-17: GS memory and display: kept

- GS memory map (4 MB): 0x000000 gsKit's frame buffer / the 448i snapshot (640x480x16 at most), 0x0A0000-0x2F0000 texture page cache (37 slots of 64 KB), 0x2F0000 CLUT cache (128 rows), 0x300000 the PS1's VRAM (1024x512x16, drawn to and scanned out directly).
- No per-frame display copy (the GS's display reads the VRAM image), except a 16-bit snapshot for single-buffered 448i screens.
- No EE copy of VRAM: the CPU mirror loops for clears and copies are gone (a 320x224 clear is 72k pixel writes a frame; 143k on the 448i screens), ~3 MB of EE RAM free. Steady play does 0 readbacks (fills patch cached pages and CLUTs).

### 2026-10-17: DrawOTag, first pass (exact): kept

Measuring: `SH1_LITEPROF=1` (include/port/prof.h `LP_*`): inline COP0 Count reads around the parts of DrawOTag, a few cycles each (the benchmark's fps didn't change with them on), printed with the GS stats. PROF_BEGIN/END pairs cost hundreds of cycles a call and distorted per-primitive work. (A timer-interrupt PC sampler doesn't work under the ps2sdk kernel: handlers run after the kernel has replaced EPC with its own return stub.) The benchmark's fps saturates at 60 for most of the cutscene and includes the frame-dump stalls, so the cycle counts are the measure; the frame dumps are the exactness check.

Kaufmann cutscene (benchmark build), k-cycles a frame:

| | DrawOTag | OT walk + per packet | polygon parse | polygon state | polygon emit |
|---|---|---|---|---|---|
| before | 1,763 | 166 | 436 | 375 | 430 |
| fog pair counting only in SH1_PROF builds (a memcmp and two memcpy per polygon) | 1,635 | | 304 | | |
| prim_state: a generation counter for the GP0 E2-E6 settings instead of a 4-word key from 12 globals; slow paths noinline (the fast path no longer saves 10 registers) | 1,589 | | | 334 | |
| emit: a quad's bounding box decides the 1023x511 fit (12 checks only for big quads); a fog pair's underlay in the textured register layout (UV ignored without TME), one GIF tag for the pair | **1,523** | | | | 370 |

All 10 frame dumps identical to before. Per polygon call (959 a frame, half of them fog pairs): parse ~316 cycles, state ~350 (texture state 493 calls, ~330 each), emit ~385.

Next: write GS data straight into the packet while parsing (no rgbaq/uv/xyz arrays), 128-bit stores, texture state (page/CLUT lookups), the OT walk (~110 cycles a packet).

### 2026-10-17: DrawOTag, straight into the packet (exact): kept

New benchmarks next to Kaufmann (`bench_cutscene.sh N`): Demo #36 **CybilFightStart** (map6_s04 func_800E1D50, renamed in tools/port/cutscene_list.py NAMES; the slowest in the 2026-10-10 sweep) and #50 **MapEvent_KaufmannBarFightCutscene**. Each is deterministic for a given build, but its picture depends on timing: the same source with the light counters on or off gives different frame dumps (a few to a few hundred pixels) and slightly different vertical blank counts. So they measure speed only; exactness is checked on Kaufmann (#4), whose dumps only vary in one subtitle glyph between builds.

Where CybilFightStart's frame goes (profiling build, 34 fps real time): game update 6.3M cycles a frame, DrawOTag 1.4M. In the game update, func_80057658 (the world mesh's per-vertex point light, `UnkGfxEnum_1`) issues ~3,550 RTPS a frame, two per vertex: the next target there (batch like Gte_RtpBatch).

Change: a quad that fits (bounding box within 1023x511, nearly all) is written straight into the packet after the state lookup, the fog pair's underlay first; RGBAQ/XYZ are computed there instead of into arrays during parsing; big quads and triangles keep the emit_poly path.

DrawOTag k-cycles a frame (benchmark builds, SH1_LITEPROF=1):

| | Kaufmann #4 | CybilFightStart #36 | Bar fight #50 |
|---|---|---|---|
| before (end of the first pass) | 1,523 | 1,520 | 1,198 |
| after | 1,396 | 1,418 | 1,132 |

Kaufmann frame dumps identical. Per polygon call now (Kaufmann): parse ~222 cycles, state ~352, emit ~351.

### 2026-10-17: func_80057658 point light, one exact call per vertex: kept

func_80057658 (world mesh per-vertex point light) uses RTPS as three dot products: RT rows 0 and 1 hold the light and the normal, and for each vertex RT row 2 and V0 are both set to the vertex-to-light vector. Each vertex wrote two RT registers, which made the next command unpack all three matrices again. `Gte_CmdRtpsRow2` (gte.c) writes row 2 and V0 in place (registers and the unpacked copies, nothing marked dirty) and runs the same rtp_small as Gte_CmdRtps.

- Check build (`-DSH_PORT_CHECK_BATCH`, Port_RtpsRow2Check: both paths from the same GTE state, all 64 registers compared): 0 mismatches over 6.2M calls (#36 and #50).
- Benchmark builds, fps over dump-free seconds: CybilFightStart #36 27.5 → **30.8**; bar fight #50 40.8 → **42.4**.

### 2026-10-17: func_80057658 point light batched (exact): kept

Re-profile of CybilFightStart (#36) after Gte_CmdRtpsRow2: game update 5.8M cycles a frame, DrawOTag 1.4M. Function-level profile (`-finstrument-functions`, inflates small functions; the port's GTE code is counted in its caller): func_80057658 3.5M, Fs_QueueWaitForEmpty 1.1M (waiting on file loads), func_80057B7C 0.9M, Gfx_MeshDrawPort 0.8M.

Change: `Port_PointLight` (bodyprog_80056D8C.c) collects every vertex-to-light vector in the loop's visiting order, `Gte_RtpsDotBatch` (gte.c) computes SXY2, SZ3 and IR0 for all but the last (the rtp_small arithmetic, rows light/normal/vector), and the original arithmetic runs on the results. The last vertex goes through the GTE with the SXY/SZ FIFO preset, so the GTE ends as after the original loop.

- Check build (`Port_PointLightCheck`: against the original loop from the same state; light values, the whole scratch struct, all 64 GTE registers): 0 mismatches over 46k meshes (#36, #50).
- CybilFightStart, profiling build at real time: game update 5,849 → **5,446** k-cycles a frame (−7%); 36.9 → 39.0 fps. Whole-run benchmark: #36 1,918 → 1,849 vertical blanks for its 801 frames, #50 6,492 → 6,040 for 4,017.
- Smaller than the instrumented profile suggested: that build inflates the function's many small calls.

### 2026-10-17: the EE paths in gte.c were never compiled into the game: fixed

Light counters on the world mesh (CybilFightStart): point light 2.1M cycles a frame (of which Gte_RtpsDotBatch 1.42M for ~3,800 vertices, ~370 cycles each), transform (func_80057B7C, Gte_RtpBatch) 0.91M, packets (Gfx_MeshDraw) 0.48M. The batch's code called `__clzdi2` with ~18 register spills per vertex, although clz32 uses PLZCW under `_EE`.

Cause: only src/port/ps2/ is compiled with `-D_EE`; gte.c (src/port) and the game code are not. So every `#ifdef _EE` path in gte.c (PHMADH rt_dot3, MULT/MFHI mul32, PLZCW) and Gemini's MMI bone matrices (vw_calc.c `#if defined(_EE)`) were left out of the game; only tools/port/gte_test's EE checker (which passes -D_EE) ran them. Now keyed on the compiler's own `_MIPS_ARCH_R5900` as well (gte.c `GTE_EE`).

Checks: gte_test on the EE (all 7 seeds, every register and FLAG) and on the host: all match. Check build (#36, #4): batch, chara RTPT/NC, light, mesh, NCLIP, point light, Vw_MultiplyAndTransformMatrix: 0 mismatches. Kaufmann frame dumps identical.

| | CybilFightStart #36 (801 frames) | Bar fight #50 (~4,030 frames) |
|---|---|---|
| vertical blanks before | 1,849 | 6,040 |
| after | **1,709** (−7.6%) | **5,723** (−5.2%) |
| point light batch (k-cycles a frame) | 1,419 → 988 | |
| mesh transform | 910 → 640 | |

- Tried: Gte_RtpsDotBatch's dot products with PHMADH (as rt_dot3). Exact (check build: 0 mismatches over 110k meshes) but slower: batch 988 → 1,146 k-cycles a frame (CybilFightStart). The 128-bit store and reload around PHMADH cost more than the nine scalar multiplies. Reverted.
- Same for rt_dot3 (PHMADH in rtp_small and Gte_RtpBatch): scalar is a little faster now that the scalar path has MULT/MFHI and PLZCW. Mesh transform 640 → 613 k-cycles a frame (CybilFightStart), 1,709 → 1,702 vertical blanks; bar fight 521 → 497. rt_dot3 and the MMI copy of RT removed; gte_test EE and host still match, Kaufmann dumps identical. MMI pays off when data stays in 128-bit registers across several operations, not for one dot product with a store and reload.

### 2026-10-17: point light arithmetic in registers (exact): kept

Port_PointLight's second pass kept the original's scratch variables (screen position, depth, matrix rows, normal) up to date per vertex. Each light value depends only on its own vertex's RTPS results and is written to its own position, so the loop now runs in registers and the scratch fields get only their final values (the last vertex, through the GTE as before). Check build (whole scratch struct, light values, all GTE registers): 0 mismatches over 117k meshes.

CybilFightStart: apply 469 → **182** k-cycles a frame (point light 1,654 → 1,368); 1,702 → **1,662** vertical blanks. Bar fight: apply 387 → 193.

### 2026-10-17: world mesh packets, lit path (exact): kept

Every world face in CybilFightStart takes Gfx_MeshDrawPort's lit path without fog (~2,300 faces a frame, ~200 cycles each); the lit-and-fogged and fog paths aren't used there. The lit loop now keeps the face's indices, depth and light bytes in locals instead of re-reading the scratch fields, and checks the colour memo's inputs once per mesh instead of once per corner; the scratch fields get the original's final values. (A first version compared the screen bound signed; the original's bound is a u32, so the test is unsigned: the mesh check caught it.)

Also: the lit-and-fogged path (the flashlight) now memoizes its two corner colours (its fog colour shares the fog path's memo; its DCPL by light and fog byte in a 2048-entry direct-mapped table keyed on both). Exact by construction; not exercised by the benchmarks.

Mesh check (packets and OT against the original): 0 mismatches (#36, #50, #4, #1-3). Kaufmann dumps identical. Mesh packets: CybilFightStart 479 → **404** k-cycles a frame, 1,661 → **1,652** vertical blanks; bar fight 310 → 278.
