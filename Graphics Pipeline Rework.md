# Graphics pipeline rework

Goal: change how the port's renderer uses the PS2 so the EE stops being the limit (ideally the GS becomes it), without changing a single output pixel. Background: [PS2 Decomps Analysis.md](PS2%20Decomps%20Analysis.md) (how Dark Cloud and ICO feed VU1 and the GS) and [PS2 Optimizations.md](PS2%20Optimizations.md) (measurements so far).

Every step must keep the output identical. The checks:
- `tools/port/bench_frames.sh` (attract demo 0 frame dumps) against the build before the step.
- `tools/port/bench_cutscene.sh 4` (Kaufmann) only between builds of the same speed: the cutscene follows its voice clips, which play in emulated time while the benchmark runs uncapped, so a faster or slower build reaches each line at another frame (different shots and poses in the dumps). Shown 2026-10-23: the same build twice gives identical dumps; the same build with only a delay loop added in GpuGs_Commands gives 7 of 10 different. Earlier "Kaufmann dumps identical" checks across speed changes were luck.
- `tools/port/gpu_test.py` results unchanged.
- `tools/port/gte_test` and `tools/port/lib_test` for anything touching GTE maths.

## Console estimates

Console tests aren't possible right now, so console figures below are estimates from PCSX2 and earlier console/PCSX2 pairs; they'll be replaced by measurements.

How the two compare so far:
- map4_s02 on the console (77c0e2ae5): 35.9 fps at 2% idle, so ~8.2M EE cycles a frame. PCSX2 benchmark build of the same scene: ~3.5M busy cycles a frame. **Ratio ~2.3x.**
- 2026-10-09 console profile: DrawOTag 3,270k cycles a frame on the console in a scene PCSX2 put at roughly half that; the sound task 430-580k against ~110k (synchronous SPU sends).
- The ratio is largest where the EE waits (DMA sends, GS readbacks, cache misses): PCSX2 doesn't model EE caches or GS timing. So changes that remove waits should gain more on the console than PCSX2 shows.

Working rule for estimates: console cycles ≈ PCSX2 cycles × 2.3 overall; waiting time (sends, syncs, readbacks) × 3-5.

| | PCSX2 (measured) | Console (estimate) |
|---|---|---|
| map4_s02 before 434b10b7f | ~3.5M cycles, ~24-26 fps in the warp test | 35.9 fps (measured) |
| map4_s02 after 434b10b7f (filtered CLUT cache) | DrawOTag −470k; ~150 fewer uploads and syncs a frame | ~6.5-7M cycles: **~42-45 fps** |
| map4_s02 after step 1 (async sends) | DrawOTag −105k more | ~5.5-6.5M cycles: **~45-50 fps** |
| map4_s02 after step 2 (inline uploads) | DrawOTag −18k more; sends 31 → 6 a frame | ~5.3-6.2M cycles: **~47-52 fps** |

## Steps

### 1. EE/GS overlap: asynchronous GIF DMA — done in PCSX2, console test pending
Before, every send waited for its transfer (`dma_send_impl`: `FlushCache(0)`, `dmaKit_send`, `dmaKit_wait`), so EE and GS time added up.
- [x] Sends start the GIF channel (D2 registers) and return; the wait moved to the next send (the two packet buffers alternate) and to `GpuGs_Sync` (uploads reusing the staging buffer, readbacks, display, DrawSync).
- [x] Polling only, no interrupts, so it's safe with interrupts off. The 2026-10-09 attempt "hung at the logos", but the later boot-freeze finding (Planning Doc.md, ResetCallback) points at another cause for those freezes. **The console test must confirm boots and warm restarts.**
- [x] No full `FlushCache(0)` per send: the packet buffers are written through the uncached-accelerated mirror (`sync.l` only); the upload staging buffer writes back only its own lines (`SyncDCache`).
- [x] `GpuGs_DisplayShow` waits for the frame's last packet before switching DISPFB, as when sends waited (the snapshot and movie paths already did).
- [x] ~~Frame-level overlap~~: not worth it. The EE translates every primitive inside DrawOTag, so when DrawOTag returns only the last packet is left for the GS; packet-level overlap already hides almost all GS time. Real frame-level overlap needs the translation off the EE (step 5).

Results (map4_s02 warp, PCSX2, k-cycles a frame): DrawOTag 737 → **632**; flush/DMA 90 → **7**; texture state 202 → **122** (uploads no longer wait on the send before them). Output: `gpu_test.py` identical, attract demo 0 dumps 10 of 10 identical.

Console estimate: each send used to pay a full D-cache writeback and wait for the GS to take the whole packet, ~31 sends a frame in map4_s02 now (~150 before 434b10b7f); PCSX2 charges almost nothing for the GS side. Estimated saving 0.5-1.5M cycles a frame on the console: map4_s02 **~45-50 fps** (from ~42-45 estimated after 434b10b7f, 35.9 measured before it).

### 2. Fewer sends — done in PCSX2, console test pending
After step 1, map4_s02 still made ~31 sends a frame, almost all from uploads: each took three (the packet so far, the transfer registers, the data).
- [x] Uploads up to 2 KB (CLUT rows, filtered CLUTs, small LoadImages) go into the packet being built: the transfer registers, then an IMAGE tag with the data. Same GS commands in the same order, no extra sends.
- [x] Readbacks (each makes the EE wait for the GS to finish everything): 0 a frame in map4_s02 now (7 before 434b10b7f, whose filtered CLUT cache keeps its own copy of each CLUT read from VRAM). Light-counter builds print `gs readbacks per frame` by source.
- Bigger or chained packet buffers: not needed. A frame's drawing fits the 64 KB buffers; the 6 sends left a frame are the display and frame boundaries.

Results (map4_s02 warp, PCSX2): sends 31 → **6** a frame; DrawOTag 632 → **614** k-cycles; texture state 122 → **92**. Output: `gpu_test.py` identical, attract demo 0 dumps 10 of 10 identical.

Console estimate: ~25 fewer send waits a frame (each a wait for the GS to take the packet so far, and before step 1 a full cache writeback); ~0.2-0.4M cycles. map4_s02 **~47-52 fps**.

### 3. Cheaper primitive translation — in progress
Original idea: world mesh, character mesh and particles write GS packets into a side buffer and leave a marker in the OT, skipping the GP0 encode/decode. Measuring first changed the order:
- map4_s02 barely uses the mesh paths (mesh packets ~1k cycles a frame); its primitives are effects and particles.
- Kaufmann, per polygon (~1,550 a frame, PCSX2): parse ~390 cycles, state ~780, emit ~850. Of emit, exact blending (`blend_exact`, modes 0/2/3, three draws each) is 752k a frame for 132 polygons (~5,700 cycles each). Of state, ~1,070 recomputes a frame came from the primitive kind alternating against a one-entry memo.
- So skipping the GP0 decode only saves the ~390 of parse; state and emit have to happen at DrawOTag time either way.

Done:
- [x] Larger filtered CLUT caches (see below): Kaufmann readbacks 16 → 0 a frame.
- [x] Per-kind state memo (`prim_state`, 1x only): a kind seen again under the same drawing settings only re-sets its six registers. Kaufmann "state: other" 458 → 157 k-cycles, DrawOTag 3,815 → 3,745 (−2%).
- [x] Tried, reverted: `blend_exact` restoring the registers it changed instead of dropping every memo (memo drops 262 → 1 a frame, but the restores cost as much: DrawOTag unchanged).

Exactness for these: `gpu_test.py` identical; the 7 DuckStation captures replayed through the renderer (`compare_frames.py --replay`, timing-independent) byte-identical VRAM to HEAD's; attract demo 0 dumps identical.

- [x] Exact blending, one pass per quad: a quad whose triangles (0,1,2) and (1,2,3) lie strictly on either side of their shared edge (`quad_split`) goes through `blend_exact` once over its whole box instead of once per triangle (each pixel belongs to one triangle under both the PS1's and the GS's top-left fill, so it's blended once either way). Quads that fold over keep the per-triangle path. Kaufmann: blend exact 755 → **313** k-cycles a frame, DrawOTag 3,745 → **3,319** (−11%). Same checks, all identical.

Left: the mesh-path bypass (parse ~600k + PS1 packet building ~340k a frame in Kaufmann), better done with step 5's packet format.

Console estimate for Kaufmann-like scenes (exact blending, filtered CLUTs): DrawOTag 8.2M → 3.3M cycles in PCSX2 over today's changes; at ~2.3x, and more for the readbacks and uploads removed, these scenes should gain the most on the console.

### 4. GTE maths on MMI — done, much smaller than estimated
Plan: batched transforms, point light dot products and colour maths with 128-bit integer SIMD (`PMULTH`/`PMADDH`), with a range guard falling back to the C path.

What measuring showed (PCSX2, light counters, per frame):
- The GTE batches matter in lit 3D scenes: CybilFightStart point light batch 1,042k (~260 cycles a vertex, ~4,000 vertices) and transform batch 650k (~166 a vertex), together more than its DrawOTag (1,610k). Kaufmann: transform batch 394k.
- MMI doesn't fit: the dot products are ~20 of ~160 instructions a vertex; the rest is the projection (UNR divide, clamps, two more multiplies), which is scalar. MMI was already tried for the point light on 2026-10-17 (PHMADH with a 128-bit store and reload around it): slower. The compiled loops have no 64-bit library calls.

Done instead:
- [x] `Gte_RtpBatch` in 32-bit arithmetic when the rotation rows are small enough (`rows_small`: each row's absolute values sum under 65,536, so a row dotted with a 16-bit vector stays within 32 bits). (TR << 12 + R.v) >> 12 = TR + (R.v >> 12) exactly; the old 64-bit loop stays for other matrices. Transform batch −9% (Kaufmann 394 → 360, CybilFightStart 650 → 594 k-cycles).
- [x] Tried, reverted: the same for the point light batch (slower: 1,042 → 1,144-1,201, probably register pressure in its longer loop).

Exactness: the check build (`SH1_EXTRA_CFLAGS=-DSH_PORT_CHECK_BATCH`, every batch against the GTE per vertex) in Kaufmann and CybilFightStart: ~12M and ~3M batched vertices, 54,272 point-lit meshes, 0 mismatches; attract demo dumps and `gpu_test.py` identical.

What's left in the GTE batches is mostly the projection's divide and clamps per vertex.

### 4b. The point light (func_80057658) — done
CybilFightStart, k-cycles a frame: point light 1,440 = collect 191 + batch 1,042 + apply 191 (~4,000 vertices).
- [x] One pass (`Gte_PointLightBatch`): each vertex's vector, its RTPS and its light value in one loop, writing the light value straight to its slot; only the last vertex's vector and the FIFO's earlier results are kept for the GTE run of the last vertex (`Port_PointLightLast`, shared with the old two-pass path). Point light 1,440 → **1,155** (−20%).
- Meshes under 5 visits keep the original loop (the two-pass path read one element before its arrays at exactly 4).
- Tried, not kept: the batch's arithmetic in 32 bits (twice; slower in PCSX2 both times: 1,042 → 1,144-1,225). Each vertex needs ~15 multiplies either way, and PCSX2 seems to charge them heavily; the console may differ.
- Exactness: check build (`Port_PointLightCheck`: light values, scratch fields and all 64 GTE registers against the original loop) 54,272 meshes in CybilFightStart, 0 mismatches; attract demo dumps identical.

### Order from here (2026-10-28)
Steps 5-9 follow how Dark Cloud and ICO use the PS2 ([PS2 Decomps Analysis.md](PS2%20Decomps%20Analysis.md)), within one limit: vertex transforms stay on the EE. The game reads the GTE's results back (screen coordinates, OT depth, flags), and VU1's floats wouldn't give the same bits. What moves is the work after the transform: building GS packets and managing GS memory.

1. Step 6 (console baseline) first: every console figure above is an estimate. It decides the order of the next two: if readbacks and GS waits dominate, step 7; if DrawOTag dominates, step 5.
2. Step 7 (VRAM master in EE RAM).
3. Step 5 (VU1 packet expansion), starting with `blend_exact` and fog pairs.
4. Step 8 (mesh bypass), then step 9 (frame-level double buffering).

Not planned, as they change pixels or don't fit a PS1 game: a per-scene GS allocator or textures converted at load (the PS1 game writes VRAM at any time, so the LRU cache stays), Z buffers, mipmaps, float transforms, threads for speed (one EE core; steps 1-2 already hide the waits).

### 5. VU1 packet expansion
The EE writes a compact record per primitive; a VU1 microprogram expands it into GS packets and sends them over PATH1. Integer moves only, so exact. The "emit" stage (~850 cycles a polygon in Kaufmann, the largest of parse/state/emit) mostly leaves the EE; VU1 is idle today.
- [ ] Record format: vertices (screen XY, Z for the OT order only), colours, UVs, page cache slot / CLUT row, blend mode, kind. Measure its size against today's GS packets.
- [ ] VU1 setup: microprogram upload at boot, double-buffered VU1 data memory (XTOP), records sent by VIF1 DMA, PATH1 output; PATH3 (uploads) ordering against PATH1 kept with the current GS commands' order.
- [ ] First kind: `blend_exact` (three draws a polygon; 313k cycles a frame in Kaufmann).
- [ ] Then fog pairs, filtered two-pass polygons, sprites and particles (most of map4_s02's primitives).
- [ ] Fallback: any kind not handled stays on the EE path, in order with the VU1 ones.
- Checks: `gpu_test.py` identical, the 7 DuckStation captures replayed (`compare_frames.py --replay`) byte-identical, attract demo 0 dumps identical; a check build that builds each packet both ways and compares them.
- Measure: light counters for DrawOTag and emit (PCSX2), then the console.

### 6. Hardware profile of the game logic
EE performance counters (cache misses, stalls) on the console; the scratchpad change from 2026-10-22; compiler flags (`-G0`).
- [ ] Console baseline before steps 5 and 7-9 (the Floatstinger crash is fixed, 2026-10-26): map4_s02, map4_s03, Docks at 2x + 32-bit, Kaufmann; `SH1_PROF=1` stage timers and `SH1_LITEPROF=1` (readbacks a frame, GS waits). Replace the console estimates above with the figures. Docks figures come from the `docks1` replay, counted only up to its divergence (NPC state first differs at step 1844, replays.md); the steps after that are ignored until the file-timing fix is confirmed.
  - PCSX2 counts, 2026-10-28 (`SH1_LITEPROF=1 SH1_BENCH=1`, means of 30-frame reports; counts match the console, timings don't). Console times estimated at ~0.3 ms a page miss (Docks: ~1.6 ms for ~5, console, replays.md):

    | Scene | Mode | Prims | Uploads | Readbacks | Page misses | StoreImage | Est. console wait |
    |---|---|---|---|---|---|---|---|
    | map4_s02 (warp, 34 s) | 1x | 432 | 8.6 | <1 | <1 | 0 | ~0 |
    | map4_s03 (warp, only 4 s before the demo left the map) | 1x | 172 | 1.3 | <1 | <1 | 0 | ~0 |
    | Kaufmann (demo cutscene 4) | 1x | 2,114 | 31.7 | 0.01 (max 2) | 0 | 0 | ~0 |
    | Docks, `docks1` steps to 1830 | 2x/32 | 389 (max 1,481) | 10.2 | 2.2 (max 12) | 1.7 (max 10) | 0.33 | ~0.5 ms mean, ~3 ms worst |
    | Kaufmann (demo cutscene 4, `capture.txt` `999999 mode=3`) | 2x/32 | 1,966 | 38.6 | 7.2 (max 13) | 7.0 (max 12) | 0 | ~2.1 ms mean, ~3.6 ms worst |
    | Docks, after 1830 (ignored: diverged) | 2x/32 | 1,155 | 35.5 | 8.4 | 7.4 | 1.0 | — |

    Counts are integer means a frame per report, so "<1" means under 30 in 30 frames. At 1x the read-backs are already near 0: step 7's gain is at 2x/32-bit, where only 7 page slots fit. Kaufmann at 2x/32-bit misses ~7 pages a frame: the scene step 7 helps most. To do: the console run to confirm the per-miss cost (Docks and Kaufmann only for now).

### 7. PS1 VRAM master in EE RAM (uploads only)
`g_PortVram` becomes the only full copy of the PS1's VRAM; GS memory holds only what the GS draws into or samples (as Dark Cloud's texture manager: EE copy is the master, the GS a cache). Plan details: [docs/720p.md](docs/720p.md).
- [x] Inventory every GS→EE read (2026-10-28). All go through `download_from` in `gpu_gs.c` (one DMA from the GS, waiting for it to finish; `s_StReadbacks` counts each):
  - Page cache miss, `page_get`: a 4/8-bit page's 256 rows read back to re-upload them as T4/T8 (`s_StPageMiss`). The biggest one: ~5 a frame in the Docks at 2x/32-bit, ~1.6 ms. Gone with the EE master copy.
  - CLUT cache miss, `clut_get`: one row of 16 or 256 entries, read in 64-pixel units (`s_StClutMiss`). Gone with the EE copy.
  - Filtered CLUT source, `filt_raw`: same as above, kept in `s_FiltRaw` (`s_StRbFilt`). Gone with the EE copy.
  - Game StoreImage (`s_StRbStore`). Per frame: lens flare visibility, `func_8008D850` (water.c), 2 pixels of the frame buffer behind the sun — a true read of drawn pixels, stays. Occasional: `WorldGfx_CharaModelTransparentSet` (a character CLUT), Twinfeeler `func_800D0840` (one CLUT row), paper map, background texture events, FMV, map screen, credits, map6_s02 — texture/CLUT areas are served from the EE copy; any that cover a drawn area keep the GS read.
  - MoveImage (libgpu_port.c): StoreImage + LoadImage, only on the save/load screen. GS-side `move` (DrawOTag copies) uses BITBLT inside GS memory, no read.
  - Port only: frame dumps (`display_ps2.c`, `GpuGs_StoreHires`, `GpuGs_MovieReadRow`); debug, not counted.
  - Per-frame counts: `SH1_LITEPROF=1` already prints readbacks, page/CLUT misses, filtered sources and StoreImage calls a frame (`GpuGs_Stats`). They come from the step 6 baseline; no new counter needed.
- [x] EE copy of VRAM as the caches' source (2026-10-28, HAL `gpu_gs.c`: `s_Vram`, 1 MB). LoadImage, fills and VRAM copies update it too; each primitive's clip area marks 16x16 tiles stale (`s_VramStale`, at `state_other`). CLUT reads, filtered CLUT sources and StoreImage bring stale tiles up to date with one read-back from the GS first (`vram_refresh`); texture pages are built from the copy as it is (pages 0-2 span the frame buffers, whose texels no 4/8-bit texture samples). The GS VRAM image stays for now.
  - Read-backs a frame (PCSX2, 2x/32-bit): Kaufmann 7.2 → **0.14**, Docks to step 1830 2.2 → **0.27** (what's left: the lens flare's StoreImage). Page misses unchanged (7.0, 1.7): each still waits for the GS before reusing the staging buffer (`GpuGs_Sync` in `cache_get`), but no longer for a read-back.
  - Exactness: `gpu_test.py` 1,461 differing pixels before and after (same cases); attract demo dumps 10 of 10 identical at 1x and at 2x/32-bit (640x448); the `docks1` replay's state at step 1830 identical.
  - EE cost (PCSX2 `SH1_PROF=1`, k-cycles a frame, 2x/32-bit, old → new): Kaufmann DrawOTag 3,721 → 3,750 (+0.8%), `prim_state (other)` 503 → 552; Docks DrawOTag 788 → 830, game state update 956 → 997. The tile marking first cost +205k in Kaufmann; skipping a clip area already marked (`VRAM_MARK`, inline) took most of that back. PCSX2 charges almost nothing for a read-back, so the saving (up to ~7 a frame in Kaufmann, ~0.3 ms each by the earlier console figure) can only show on the console.
  - Next for page misses: upload from the copy without waiting (a staging buffer per slot, or DMA straight from `s_Vram` rows), then more slots (the GS layout below).
- [ ] Display buffers only: the two PS1 display areas as GS buffers (PSMCT16 at 1x; the shadow buffer at 2x/32-bit), scanned out with DISPFB as now.
- [x] Draw-target tracking: done with the EE copy above (16x16 tiles; `SH1_LITEPROF` readbacks count what's left).
- [ ] Feedback effects (motion blur, the load screen) keep sampling the GS buffer they read; 15-bit pages outside it are uploaded into the page cache like 4/8-bit pages.
- [ ] New GS layout: display buffers, page cache, CLUT cache; the freed 1 MB to page cache slots. Target: 2x 32-bit with ≥ 20 slots.
- Checks: as step 5, plus `gpu_test.py`'s StoreImage and VRAM copy tests, and the 2x and 32-bit modes' dumps against the build before.
- Measure: readbacks a frame (target 0 in gameplay), Docks at 2x + 32-bit on the console.

### 8. Mesh bypass
World and character meshes write step 5's records directly and leave a marker in the OT (draw order kept), skipping PS1 GP0 packet building and DrawOTag's decode (as Dark Cloud's per-object header and prebuilt block). Kaufmann: parse ~600k + PS1 packet building ~340k cycles a frame; map4_s02 barely uses meshes.
- [ ] OT marker format, and DrawOTag handling it in order with ordinary packets.
- [ ] World mesh path, then character meshes.
- [ ] Anything else the game does with the PS1 packets (reads, patches after building) found first; such meshes keep the old path.
- Checks: as step 5; the check build also builds the PS1 packets and compares the decoded result.

### 9. Frame-level double buffering
A frame's records and packets go out as one DMA chain at the end of the frame, so VU1 and the GS draw frame N while the EE runs frame N+1 (Dark Cloud's `MGEndFrame`). Worth little before step 5 (step 1 found DrawOTag leaves only the last packet to the GS); after it, the EE's share of drawing is writing records.
- [ ] Two frame buffers of records; wait for the previous frame's chain only when reusing its buffer.
- [ ] DrawSync, StoreImage and the display switch wait for the chain they depend on, as `GpuGs_Sync` does now.
- [ ] One frame of added latency at most: check against the PS1's (pad to screen) and the replay recorder's sync.
- Checks: as step 5; replays (replays.md) stay in sync.

### Found on the way: the filtered CLUTs cost Kaufmann more than map4_s02
The Kaufmann cutscene (map3_s00) does ~1,400 filtered CLUT lookups a frame. Light counters, k-cycles a frame (PCSX2):

| | DrawOTag | texture state | flush/DMA | uploads | readbacks |
|---|---|---|---|---|---|
| 714abc587 (before today) | 8,172 | 5,377 | 2,936 | 470 | 121 |
| d11e842fb (steps 1-2) | 3,879 | 666 | 5 | 91 | 16 |
| larger caches (4-bit filtered slots 256 → 512, CLUT sources 16 → 64, plain CLUT rows 104 → 88) | **3,778** | **555** | 5 | **32** | **0** |

So the filtered CLUTs (2026-10-22) had more than doubled Kaufmann's DrawOTag (it was ~1,500 on 2026-10-17); today's changes take most of that back. Plain CLUT misses stay at most 1 a report with 88 rows. Readbacks matter most on the console (each waits for the GS to finish everything).

### 4c. map6_s00's grid effect (func_800EC4B4) — done
Found profiling map6_s00 (the user's play session: much faster than the PS1 but not a locked 60). An instrumented profile of the warped attract demo put most of the game logic in one map function: `func_800EC4B4`, a 17x17 grid around Harry drawn every frame ("transition world effect"), which per point ran `func_80055ECC`: the light transform again (same result every time), the point light's formula through a full RTPS with GTE setup and restore, and a DPCS of the world tint. Rain (~270 drops) is next (~480k instrumented).
- [x] Lighting: point 0 runs as before; points 1-284 from one `Gte_RtpsDotBatch` plus the point light's formula, colours memoized by light value (DPCS depends only on it within a frame); the last four points run as before, so the GTE's FIFOs and registers end as after the original loop.
- [x] Projection: the 289 grid points are a lattice (unless a row or column wraps past 16 bits: then the GTE loop runs): points 0-284 through `Gte_RtpGrid`, the last triple and the RTPS through the GTE with SZ3 preset (as map6_s02's grid).
- Result, light counter around the whole function while it runs (PCSX2): 932 → 490 (lighting) → **419** k-cycles a frame (−55%).
- Exactness: check build (`SH1_EXTRA_CFLAGS=-DSH_PORT_CHECK_BATCH`): ~131k grid points' colours against `func_80055ECC`, the GTE after the loop against all 289 points run as before (no differences), and ~131k projected points against the GTE loop, 0 mismatches; attract demo dumps identical.
- Left in it: the per-point fade bands (integer divides) and the 16x16 polygon loop.

## Later: record play on the console, replay it anywhere
Idea (2026-10-23): record a real play session on the console and replay it in PCSX2 (and on the console) to measure performance on real routes, instead of warped attract demos.
- The game's own demo format does nearly all of it: `MISC/DEMO****.DAT` (options and savegame snapshot) and `MISC/PLAY****.DAT` (16 bytes a frame: pad incl. analog, expected game state, `videoPresentInterval`, RNG seed; include/bodyprog/demo.h). The demo player already replays these deterministically and reports `STEP ERROR` on divergence.
- To build: a recorder writing that format (snapshot at start, one record a frame), sent off the console through the IOP agent / network log (~58 KB a minute) or to the memory card; a way to start playback of an arbitrary recording.
- Determinism risks: disc load timing differs (network on the console, host file in PCSX2): record frames held per load and hold playback to match (as `compare_frames.py --ps1-holds`); real-time voice clips (record when they end).
- Payoff: repeatable runs of real routes (map4_s02, map4_s03) for the light counters, and exact before/after comparisons on hardware.

## Console test 2026-10-23 (5244326ad)
Boots, a cold restart and a warm restart (deploy) into a debug build were tried; the session went to the Floatstinger crash (docs/ToFix.md), so no frame-rate measurements yet. The warm restart from a kept crash state hung (the known stall), a cold boot ran normally. Still to do: map4_s02/s03, map6_s00 door 7, and the cutscenes.

## Log

- 2026-10-23: plan written. Step 1 done (asynchronous GIF sends), console test pending.
- 2026-10-23: step 2 done (small uploads inline in the packet stream), console test pending.
- 2026-10-23: filtered CLUT caches enlarged (Kaufmann readbacks 16 → 0 a frame). Kaufmann dumps shown to depend on build speed.
- 2026-10-23: step 3 re-scoped after measuring; per-kind state memo (−2% DrawOTag in Kaufmann).
- 2026-10-23: exact blending once per quad (−11% DrawOTag in Kaufmann).
- 2026-10-23: step 4: MMI doesn't fit; 32-bit transform batch instead (−9% of the batch).
- 2026-10-23: point light in one pass (−20% in CybilFightStart).
- 2026-10-23: map6_s00's grid effect batched (−55% of the effect).
- 2026-10-28: steps 5-9 as tasks, after comparing GS memory and pipelines with Dark Cloud and ICO: VU1 packet expansion, console baseline, VRAM master in EE RAM, mesh bypass, frame-level double buffering.
- 2026-10-28: step 7's read inventory done (above). Floatstinger blocker removed from step 6; Docks measurements count `docks1` only up to step ~1840.
- 2026-10-28: PCSX2 baseline counts for step 6 (table there): read-backs near 0 at 1x; Docks at 2x/32 ~1.7 page misses a frame up to the replay's divergence (est. ~0.5 ms, ~3 ms worst).
- 2026-10-28: Kaufmann at 2x/32-bit in PCSX2: ~7 page misses a frame (est. ~2.1 ms on the console). The run hit a TLB Miss in ps2sdk's `_request_end` during a bench frame dump (docs/ToFix.md).
- 2026-10-28: step 7, EE copy of VRAM feeding the caches: 2x/32-bit read-backs Kaufmann 7.2 → 0.14, Docks 2.2 → 0.27 a frame (PCSX2); output identical.
- 2026-10-28: step 7's tile marking made cheaper (+205k → +49k k-cycles in Kaufmann's `prim_state`); EE cost now +1-5% in PCSX2, read-back savings console-only.
