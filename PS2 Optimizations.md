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
2. **Native C for libgs's model renderer.** `GsSortObject4J` and the `GsTMDfast*` family (per-vertex transform, light and fog; every character) still run as recompiled MIPS (build/port/recomp/libgs_*.c, libgte_g3/g4.c), through wrappers, without our inline GTE paths. Rewriting them in C from the decomp lets GCC optimise them and lets them use the exact inline GTE helpers. This is PsyZ's central idea.
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
