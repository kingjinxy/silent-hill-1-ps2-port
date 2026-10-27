# Replays

Status as of 2026-10-26. Play recordings can now be made on the console and replayed on the console or in
PCSX2. A recording can fall out of sync when a file finishes loading on a different step than it did while
recording. A fix is built but not yet tested on a new recording.

## What works

### Recording (console)

1. `python3 tools/port/ps2_ctl.py rec NAME` arms the recorder (names are up to 8 characters).
2. Load a save. Recording starts at the load.
3. `ps2_ctl.py rec stop`, or hold Select + Start + L1 + R1, stops it.
4. `python3 tools/port/rec_extract.py --name NAME` rebuilds `build/port/rec/NAME.dem` from the console log.

The recording is streamed through the log as it goes, so a crash loses at most a second. A `.dem` file holds
the save, the full config, the RNG seed, and one 16-byte frame per step.

### Debug Options set on the title carry over to the loaded save

If you set Debug Options from the title (Option, then Triangle) before loading a save, they replace the
save's own settings, and saving again writes them into the save file. The log prints
`port: Debug Options from the title replace the save's: rendering bits N`. The override is applied once, when
the Load Game screen reads the config from the card. `ps2_ctl.py options N` sent from the title works the
same way. This is how `docks1` was re-recorded with the intended settings.

### Console replay: the title's REPLAY entry

- REPLAY sits below OPTION on the title menu. It opens the Debug Options list on the Options background.
- Its settings replace the recording's own Debug Options for the replay. They're remembered until the next
  boot, and on first open they copy the current Debug Options.
- Start waits for a recording ("Waiting for replay"); Cancel goes back to the title.
- `python3 tools/port/ps2_ctl.py replay NAME` uploads `build/port/rec/NAME.dem` in 1 KB chunks. Each chunk is
  acknowledged in the log, and the total size and a checksum are checked before the start command is sent.
  The upload can be sent before or after pressing Start. The replay starts once Start has been pressed and
  every button is released, because a button press during playback ends the replay, as it does for attract
  demos.
- The game starts straight from the save stored in the recording, with no memory card involved.
- At the end, the log prints the frame rate and the player gets control back.

Code:
- REPLAY page: `src/screens/options/options.c` (`s_PortReplay`)
- Title entry: `src/bodyprog/events/title.c` (`MainMenuEntry_Replay`; the menu column moved up 20 px)
- Receiving and starting: `src/port/demo_rec.c` (`Port_RecUpload`, `Port_RecRemotePlay`, `Port_RecReplayBegin`)
- Agent commands `UP` and `RP`: `src/port/ps2/agent_ps2.c` and the IOP agent
  `src/port/iop/sh1agent/main.c`, which copies up to 1 KB of data into an EE buffer passed as `up=0x...`
- Host side: `tools/port/ps2_ctl.py` (`replay`)

### PCSX2 replay

Put `play NAME` in `build/port/rec.txt` and `999999 load=2:10 mode=N` in `build/port/capture.txt`, then run
`tools/port/pcsx2_run.py build/port/sh1_ps2.iso --until "rec: replay NAME finished"`. Pressing Start on the
REPLAY page in PCSX2 also plays `rec.txt`'s recording.

### Traces

During both recording and replay the log prints:
- `rec trace` every 600 steps
- `rec step N: ...` every 30 steps and whenever the system state changes: Harry's position and health, the
  frame time, the RNG seed, and the file queue's length and read index

To check sync, compare the `rec step` lines of two runs.

## The desync, and what we found

The new `docks1` (2,647 steps: map6_s01, then the Docks, then into map6_s02 around step 2236) sometimes
replays wrongly. Harry gets hit and stuck near the transition point, and map6_s02 never loads. No STEP ERROR
is reported, because the game state never differs.

- It isn't randomness: the RNG is reseeded from the recording every step and matches throughout.
- It isn't frame pacing: each step's frame time is replayed exactly.
- It isn't floating point: the GTE emulation and maths helpers use none.
- It is file timing. A per-step comparison showed the NPC state first differing at step 1844, then Harry's
  at step 1978 when the enemy reaches him. Three files load around then. In the console run that stayed in
  sync, the first read finished at step 1842 and the next file was queued at 1843. In PCSX2, which drifted,
  both happened one step later (1843 and 1844). That single step changes how the enemy is set up.
- The recorder only held map chunk loads to their recorded timing, not other files. So whether a replay
  matches depends on EE speed relative to disc and real time:
  - PCSX2 always drifted.
  - On the console, builds with the per-GTE profiling wrapper stayed in sync; builds without it drifted.

## The fix (built, not yet tested)

`src/port/demo_rec.c`, plus a hook in `src/main/fsqueue.c`:

- **Recording:** every step stores the file queue's post-load index (the point at which the game sees a file
  as loaded) in `field_A[1]`, as `idx % 255 + 1`. 0 means not recorded, so older recordings play as before.
- **Playback:** `Port_RecFileHold` stops the queue from post-processing a file before its recorded step. It
  only holds between files, never part-way through one. At each step start, `file_sync` waits for files that
  are late, up to 10 s.
- Chunk-load holds now use only `field_A[0]`. No existing recording ever set them.

To test: record a new `docks1` on the console with this build, then replay it in PCSX2 and on the console
with the GTE wrapper left out. Both used to drift in that configuration. If every `rec step` line matches the
live run, the fix works.

## Other findings from this session

- **IOP modules weren't being rebuilt.** `port_link.py` appended ps2dev's IOP compiler to the end of PATH,
  and `~/.bashrc` puts PSn00bSDK's compiler of the same name earlier. Its assembler emits relocations in an
  order `iopfixup` rejects, so the console had been running the Oct 19 `sh1agent.irx`. The fix puts ps2dev
  first (`tools/port/port_link.py`). The Oct 20 reboot-bug change has now reached the console and doesn't fix
  it: remote restarts still freeze about 5–8 s after boot (vblank interrupts stop at `gameState 1`, step 3).
  A cold boot from Neutrino works.
- **`SH1_EXTRA_CFLAGS` doesn't reach the port's own code** (`src/port/*`). The per-GTE profiling counters
  now have their own switch, `SH1_PROF_GTE=1`, and are off in a plain `SH1_PROF=1` build.
- **`ps2_ctl.py where` is unusable.** The vblank handler records the kernel's EPC (always `0x81fe0`), not the
  game's PC.
- **The UDPFS server can get stuck** after long uptimes and many redeploys, so Neutrino can't open the ISO.
  `./build-and-deploy --restart --no-build` restarts it.

## Performance results from replays (Docks, x2 / 32-bit)

- At x2 / 32-bit, the GS texture page cache has only 7 slots (`gpu_gs.c`, `s_CacheSlots`), against 27 at
  x2 / 16-bit. The Docks miss about 5 pages a frame. Each miss waits for the GS (`GpuGs_Sync`), costing about
  1.6 ms on hardware but almost nothing in PCSX2. At 16-bit the misses drop to about 1 and texture switching
  costs a third as much.
- Game logic is about 10.6 ms a frame on the console: `WorldGfx_Draw` about 5.1 ms, NPCs 2.2, Harry's draw
  and combat 1.2, map particles 1.2. The `ig:` timers in `src/bodyprog/events/game_sys_states.c` break it
  down in `SH1_PROF=1` builds.

## Next steps

1. Record a new `docks1` with the file-timing build, and check its replays in PCSX2 and on the console.
2. Optional: check whether the title-screen attract demos stay in sync between runs, using the `rec step`
   trace or an equivalent.
3. Then the texture-streaming work: a page miss shouldn't wait for the GS (double-buffer the page staging),
   and there should be more page slots at 32-bit. A future 720p mode makes this more important.

Nothing from this session is committed yet.
