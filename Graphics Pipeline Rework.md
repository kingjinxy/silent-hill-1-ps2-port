# Graphics pipeline rework

Goal: change how the port's renderer uses the PS2 so the EE stops being the limit (ideally the GS becomes it), without changing a single output pixel. Background: [PS2 Decomps Analysis.md](PS2%20Decomps%20Analysis.md) (how Dark Cloud and ICO feed VU1 and the GS) and [PS2 Optimizations.md](PS2%20Optimizations.md) (measurements so far).

Every step must keep the output identical. The checks:
- `tools/port/bench_frames.sh` (attract demo 0 frame dumps) and `tools/port/bench_cutscene.sh 4` (Kaufmann) against the build before the step.
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

### 3. GS packets straight from the hot mesh paths
World mesh, character mesh and particles write GS packets into a side buffer and leave a marker in the OT, skipping the GP0 encode/decode (~1,300 cycles a primitive today). Checked per mesh against the PS1 packets' translation.

### 4. GTE maths on MMI
Batched transforms, point light dot products and colour maths with 128-bit integer SIMD (`PMULTH`/`PMADDH`), with a range guard falling back to the C path so results and flags stay exact.

### 5. VU1 packet expansion
Compact primitive records expanded into GS packets by a VU1 microprogram (integer moves only, so exact): fog pairs, filtered two-pass polygons, sprites.

### 6. Hardware profile of the game logic
EE performance counters (cache misses, stalls) on the console; the scratchpad change from 2026-10-22; compiler flags (`-G0`).

## Log
- 2026-10-23: plan written. Step 1 done (asynchronous GIF sends), console test pending.
- 2026-10-23: step 2 done (small uploads inline in the packet stream), console test pending.
