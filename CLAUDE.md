# CLAUDE.md

Notes for AI agents working on this repository: a fork of the Silent Hill (PS1) decompilation that ports the game to
the PlayStation 2. The upstream decompilation is human-written; port work happens in this fork only.

## Where things are

| What | Where |
|---|---|
| Plan, step status, hardware findings | `Planning Doc.md` |
| Dated log of everything done and tried | `Running Walkthrough.md` |
| Known open bugs | `docs/ToFix.md` |
| Console play recording and replay | `replays.md` |
| Performance ideas, 720p | `PS2 Optimizations.md`, `docs/720p.md` |
| Past agent sessions | `Claude Conversation.md` (archive, see below) |
| Port code (HAL, GTE, GPU→GS, recorder, profiler) | `src/port/`, `src/port/ps2/`, `include/port/` |
| IOP agent module (network control, log) | `src/port/iop/sh1agent/` |
| Port tools | `tools/port/` |

Port-specific changes to game code sit under `#ifdef SH_PORT` so the matching PS1 build stays unchanged.

**Before planning any console debugging technique, search `Running Walkthrough.md` and `Planning Doc.md` for what
has already been tried on hardware.**

## Building for the console

Use `./build-and-deploy` (`--restart` restarts the UDPFS server and log receiver, `--no-build`, `--prof`). By hand:

```
export SH1_FPS=60 SH1_NO_RECOMP=1
bash tools/port/ee_compile_check.sh   # compiles the game C files; port_link.py alone does not
python3 tools/port/port_link.py
bash tools/port/make_iso.sh
python3 tools/port/ps2_ctl.py deploy
```

- Always set `SH1_FPS=60`. Without it gameplay locks to 30 fps, which looks like a bug.
- Always run all three build steps. A link-only build silently leaves out game C changes.
- Profiling switches: `SH1_PROF=1` (stage timers, incl. `ig:` in-game timers), `SH1_LITEPROF=1` (light counters,
  GS cache stats), `SH1_PROF_GTE=1` (per-GTE-command counters; changes timing enough to affect replay sync).
- `SH1_EXTRA_CFLAGS` doesn't reach `src/port/*`; add a dedicated switch in `port_link.py` instead.
- `port_link.py` puts ps2dev's IOP toolchain first on PATH because PSn00bSDK ships a compiler of the same name.
  If `sh1agent.irx` fails with "R_MIPS_LO16 without R_MIPS_HI16", the wrong compiler is being used.

## The console

- Neutrino boots the ISO over UDPFS. `ps2_ctl.py` controls the game through the IOP agent; the console log
  arrives in `build/port/ps2.log` (`ps2_log.py`). When filtering the log, filter by line number, not timestamp:
  the file contains old runs.
- Remote restarts (`ps2_ctl.py deploy`/restart) often freeze 5–8 s after boot. A cold boot works.
- **When the PS2 hangs, ask the user to power-cycle** (hold power, restart Neutrino from uLaunchELF). RESET
  never revives Neutrino on their console.
- If Neutrino shows a black screen and drops to OSDSYS, the UDPFS server is stale:
  `./build-and-deploy --restart --no-build`.
- EE hardware data breakpoints never fire under Neutrino. Use software checks from port code (e.g.
  `SH1_SENTINEL` in `gte.c`).
- `ps2_ctl.py where` is useless: it always reports the kernel EPC `0x81fe0`.

## Emulators

- **PCSX2:** run through `tools/port/pcsx2_run.py`. It uses the software GS renderer (`Renderer = 13`); keep it.
  - 15 emulated seconds without a new frame means a crash; end the run instead of waiting for a timeout.
  - **Never dismiss "TLB Miss" lines.** A real PS2 faults where PCSX2 carries on (decompiled PS1 code reads
    through NULL freely). Collect the PCs and fix each.
  - Cutscene tests pass once they reach 30 emulated seconds of gameplay (`--gameplay 30`).
  - PCSX2 doesn't model the GS wait costs that dominate on hardware; measure performance on the console.
- **DuckStation** is the PS1 reference. Keep it on the Software renderer and muted (`OutputMuted = true` in any
  launcher). Avoid per-frame gdb stops. Its memory card holds the port's saves: "Infirmary" is File 2 / Data 10.
- Verify the renderer by comparing PCSX2 running the port's GS renderer against DuckStation, not against the
  port's own software renderer.

## Shell gotchas

- Don't `pkill -f PATTERN` from a command line that contains PATTERN: it kills the calling shell. Stop background
  tasks by task id or a PID read first.

## Working with the user

- Do not access tcrf.net (The Cutting Room Floor) in any form. Ask the user to paste the relevant information.
- "Docks" means map6_s00, a slow map, tested at Resolution x2 + 32-bit (`ps2_ctl.py options 3`).
- Notify the user at stopping points (a question for them, or a finished step); they aren't always watching.
- Sound: keep the original sound code and assets, change them only as the SPU2 needs, and use one SPU2 core.
- Commit only when asked.

## Conversation archive

Keep `Claude Conversation.md` up to date at milestones. `tools/port/archive_conversation.py` rewrites its whole
output file, so concatenate **all** session `.jsonl` files in date order before running it, or render only the
new session to a temporary file and append it. Check that `git diff` shows no removed lines.
