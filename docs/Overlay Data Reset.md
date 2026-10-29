# Overlay data reset plan

Every overlay is linked into the executable (`tools/port/port_link.py`). On the PS1, reading an
overlay file resets its `.data` to initial values and its `.bss` to zero. In the port, linked-in
overlays keep their variables from the previous load. The reset already exists in `src/port/overlay.c`
behind `SH_PORT_OVERLAY_RESET`, but it's off. This plan finishes it. It is the open item "Overlay
.data/.bss reset on reload (snapshot/restore)" in `Planning Doc.md`.

## State

- `Port_OverlaySnapshot` copies each overlay's `.data` at startup, using the ranges between
  `port_link.py`'s markers (`g_PortOverlayRanges`).
- `Port_OverlayActivate(fileIdx)`, called from `Fs_QueueStartRead`, restores `.data` and clears
  `.bss`, then switches `g_MapOverlayHdrPtr`.
- Result: it fixes a map loaded twice in a row (map0_s01 in the warp sweep). It also breaks the second
  attract demo: map2_s00 loaded again, and its world object lookups then read garbage.
- The screen overlays (STREAM/SAVELOAD/OPTION/STF_ROLL) share the `0x801E2600` buffer region on the
  PS1. In the port their code is outside the arena, so that sharing goes away (`Running Walkthrough.md`).

## Hypotheses for the attract-demo break

1. **Timing.** The reset runs when the read is *queued*. On the PS1, the bytes land only when the read
   completes, and the game may still run or write overlay variables in between. Test: move the reset
   to the point where the read completes.
2. **No real reload.** The second demo may not reload map2_s00 the way the first does, e.g. a read
   that isn't followed by running the overlay from scratch, or a header that's set up before the read.
   Test: log each map file read and each reset during the two demos, and compare with DuckStation (a
   gdb break on the file-read function, kept to one stop per load).
3. **State outside the ranges.** Resident code (bodyprog) or the map header keeps pointers into
   overlay data, or overlay data that matters lives outside the marked ranges (e.g. `.sdata`/`.sbss`,
   or data merged into another section). Test: dump the ranges for map2_s00 and check them against
   the merged object's section headers.
4. **The world object tables.** Find which variables the lookups read on the second demo, and
   whether they belong to the overlay (and should be reset) or are filled by resident code after the
   load (and must not be cleared).

## Steps

1. Reproduce: build with `SH_PORT_OVERLAY_RESET`, then run the attract demos twice in PCSX2.
2. Test hypotheses 2 and 3 first (logging only, cheap), then 1 and 4.
3. Fix the reset point and the ranges. Retest both cases (the map0_s01 warp sweep and the second
   attract demo), plus a map revisited during play and a save/load. Compare with DuckStation.
4. Turn the flag on by default. Update `Planning Doc.md` and `Running Walkthrough.md`.
