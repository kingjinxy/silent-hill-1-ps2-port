# TAS Testing Plan

Use TASVideos submission #10742 (`~/Downloads/submission10742/Silent Hill (USA) NG Good+.bk2`) as a long, scripted
regression test: replay its input in the port and check the game state against the PS1 game replaying the same
movie. The method follows the MGS port's plan (`../mgs1_ps2/docs/TAS Testing Plan.md`, scripts in its
`tools/port/tas/`).

## What the movie is

- berrimeow and arandomgameTASer, Silent Hill "Good+", 29:13 IGT, 51:37 real time. Sync verified on TASVideos
  (2026-09-19, Tompa). Status at the time of saving: New, so the file may be replaced before publication.
- BizHawk 2.11.1 (started on 2.9.1), Nymashock core, 185,305 frames at 59.83 fps, 62,291 rerecords.
- Starts from power-on with no save data. Silent Hill (USA) 1.0. BizHawk's game hash is `F1B5EB6C`.
- BIOS `ps-30a.bin` (SHA-1 `0555c6fae8906f3f09baf5988f00e55f88e9f30b`), in `~/Downloads/NOTps1bios/`.
- One DualShock in port 1. First press at frame 1,452. Analog sticks off-centre on 113,646 frames, so the port's
  analog path gets a real test. No resets or disc swaps.

## Why a plain replay won't work

Input is per video frame, and the port's load times and frame timing differ from the emulator's. So, as for MGS:
record what the game read from the pad on each game loop, feed the port the same values on the same loops,
and compare state on every loop to find the first difference.

SH1 differs from MGS here:
- Build at the PS1's frame rate. `SH1_FPS=60` changes game timing, so the replay build leaves it unset.
- The game's delta time depends on measured vertical blanks, so the port must take each loop's delta time from
  the trace as well as the pad.
- Keep each loop's RNG in the trace. Things that advance it during loads or on time-dependent paths are the likely
  first differences.
- The port already has a recorder and a replay with the PS1's load timing (`replays.md`, `src/port/demo_rec.c`).
  Extend these, rather than porting MGS's `pad_script.c` approach.

## Phase 1: reference replay

- [x] BizHawk 2.11.1 for Linux in `~/emu/bizhawk-2.11.1` (needs Mono), with `ps-30a.bin` in `Firmware/`; movie as `~/emu/sh-tas/sh.bk2`. Started by `tools/port/tas/run_bizhawk.sh`.
- [x] Load `rom/image/SLUS-00707.cue`: BizHawk reports hash `F1B5EB6C` (2026-11-02). A plain CRC32 of the `.bin` is `1d4a3ff7`, which is not comparable.
- [x] Unattended full run (2026-11-02, twice, about 30 min each; `tools/port/tas/synccheck.lua`, log in `~/emu/sh-tas/`). Good map order to the end (map index 42), final boss on screen at frame 185,300 and apparently dying. The movie stops while still firing at the boss. Resuming from a state 10 frames before its end, holding its last input (X, L2, R2) and then pressing Start (`tools/port/tas/to_results.lua`) reaches the ending, credits and result screen (`~/emu/sh-tas/results_screen.png`).

## Phase 2: per-loop trace

- [x] `tools/port/tas/trace.lua` (2026-11-02): line format at the top of the script. Nymashock has no memory hooks in BizHawk (callbacks register but never fire), so it samples once per video frame.
- [x] Deterministic: two runs gave identical files (184,222 lines).
- [x] Reference in `tas/reference/trace.log.xz`, movie in `tas/movie/`.
- [ ] Loop boundaries: `gameStateCounter` turned out to advance every vertical blank, not every loop, so the trace is per video frame, not per game loop. Derive loops from changes in the in-game timer and RNG, or add a real loop signal. Pad and delta time must be matched to loops before Phase 3.

## Phase 3: replay in the port

Findings, 2026-11-02 (`tools/port/tas/trace_to_loops.py`):
- Walking the trace by `g_VBlanks` gives about 107,500 loops. 99.2% of the 44,900 timer changes fall within one frame of a walked loop, 66% exactly: the walk drifts by a frame, so re-anchor it at each timer change.
- The timer stops on about two thirds of in-game frames (cutscenes), where only the walk and RNG mark loops.
- The port's recorder holds 65,536 steps and starts from a save. The TAS needs at least 107,504 steps and starts at power-on, so the replay needs a larger buffer and a start from boot.

- [ ] Replay input format from the trace, in a build with the same loop-by-loop state output.
- [ ] Run in PCSX2 (software GS) to the first difference, fix, repeat. Collect any TLB Miss lines as well.
- [ ] Compare tool (`tools/port/tas/compare_trace.py`), reporting the first differing loop and field.

First replay in PCSX2 (2026-11-02, `tools/port/tas/trace_to_dem.py`, `compare_trace.py`, `Port_RecTas`): starts at frame 2,109 from a dumped savegame instead of power-on, one step per in-game loop, 66,898 steps. It ran 3,167 steps without a crash, then ended on STEP ERROR. It wasn't in sync: Harry was already about 1,500 units (under 0.4 m) off by step 30 and in the wrong map by the end. Open causes: step alignment in the map 0 opening, trace rows sampled at a different point of the loop than the port's lines, and file load timing, which the trace doesn't record (`g_FsQueue.postLoad.idx` to add). The steps cover in-game loops only, so inputs in menus and the item screen (game states 14, 19) aren't replayed yet.

Second round (2026-11-02): the trace now records `g_FsQueue.postLoad.idx` (held to on replay, with a limit of 8 holds a step so a blocking load can't deadlock), and steps cover all of `Demo_StateGet`'s step states (69,185 steps). Result: worse. Harry was within 57 units at step 600, about 47,000 off at step 900 (the first round matched there exactly), and from step ~2,200 the replay crawled on "file loads still behind" waits until the run's time limit. Sampled file marks shouldn't be held to until steps line up exactly. Next: an exact loop signal (candidate: the pad data's `pulseTicks`, or re-tracing with a per-loop counter), then compare step by step from step 0.

Loop signal (2026-11-02): `g_ActiveBufferIdx` is now in the trace (last field) and holds up. 103,171 flips, 58,393 of them in game state 11. Of 44,900 timer changes, 92.3% fall on a flip and 99.93% within one frame, against 66% and 99.2% for the `g_VBlanks` walk. Flip spacing matches a nearby `g_VBlanks` in 96.5% of state 11 loops; most mismatches are in the load state (10), where that isn't expected to match. Timer changes one frame off a flip show the timer and the flip aren't sampled at the same point of the loop.

Pad offset test (2026-11-02, 60,519 steps from flips, file holds off, a line per step for the first 4,000): the row at the flip is best. Offset 0 stays within 2,000 units until step 737, +1 until step 48, -1 until step 50 and fails sooner. At offset 0 Harry follows the opening cutscene's path (within ~200-500 units) but its jump comes about 50 steps early (step 737, trace ~790), and the run ends on STEP ERROR at step 2,893 (expected inventory, still in game). The cutscene appears to advance on something other than steps, probably voice or real time. Seeds never match within 20 rows, so the RNG comparison is still not usable: to work out where in the loop each side samples it.

Voice clock test (2026-11-02): `SD_XA_VSYNC_COUNT` driven by the recorded steps' time made no difference: the first difference stayed at step 737 and the STEP ERROR at step 2,893. Reverted. So the voice clock isn't what moves the opening cutscene's jump. Still to check: XA load and seek time, and the cutscene's own script around that step.

Final focused round (2026-11-02): reseeding from sampled seeds was wrong in principle, as the trace samples them mid-loop, so the test used only the first seed and let the game's draws carry on. No change: the first difference stayed at step 737 and no logged port seed matched the trace within 40 rows, even at step 1, so the RNG comparison remains unusable and its cause unknown. In the trace, the cutscene's move at frame 4,359 follows the file queue's change, so the opening's jump looks tied to file load timing, which the port finishes sooner. Experiment reverted.

## Phase 4: console

- [ ] Full replay on the console in 4:3, watching for crashes. The same replay in other modes (2x, 32-bit,
      widescreen) checks that settings don't change gameplay.

## Limits

- Replaying the game's loops isn't a test of timing, so it won't find the cutscene voice desync. Run that
  separately, at 60 fps, against the reference video.
- The route skips optional areas (any% style glitches), so it complements a normal playthrough.
