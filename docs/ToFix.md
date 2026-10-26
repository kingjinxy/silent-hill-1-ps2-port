# To fix

Known issues, tabled for now (collected 2026-10-19, mostly from the console cutscene test: tools/port/console_cutscenes.py, results in build/port/console_cutscenes_summary.md). Newest at the bottom of each section.

## Game / cutscenes

- **map6_s02 func_800CF0B8 (Demo #1) soft-locks.**
  - What happens: the game keeps drawing at 60 fps, but the cutscene never advances.
  - Where it happened: on hardware at 1x 16-bit, 2x 16-bit and 2x 32-bit. It finished once, at 1x 32-bit, so it isn't caused by the renderer modes and is probably timing- or state-dependent.
  - Suspect: every time, it ran right after the bar fight (#50), so state left over from that cutscene.
- **The Demo list started "ALL MOVIES" on its own.**
  - What happens: after map7_s02 func_800DE1FC finished and the game returned to the list, the "ALL MOVIES" row started by itself one second later ("movie test: requested"). There was no remote request; a stray pad press or the list's selection landing on that row are possible causes.
  - Knock-on effect: a soft reset during the movie test goes back through the intro FMV, because the test runs with gameState set to the intro movie (Port_MoviePlayAll).
- **The demo "fps:" line can be negative.**
  - Example: map7_s02 func_800DE1FC printed "-8025656 frames in -8018726 vertical blanks". Its start tick/vblank (s_StartTick, s_StartVBlank in demo_menu.c) were stale for that run.
- **The fps line includes FMVs.**
  - The demo fps line counts from the event's start to player control, so a cutscene with an FMV (e.g. CybilFightStart) averages in the movie's 15 fps.
  - console_cutscenes.py now reports an in-engine fps without the FMV seconds. The game's own line still mixes the two.
- **"STEP ERROR" on screen during attract demos.**
  - The original game's debug text in demo playback (demo.c): a recorded attract demo found the game in a different state than it expects. It's harmless and seen on hardware, probably when a soft reset interrupts a demo.
  - It's logged now ("demo: STEP ERROR at step ..."); still to find out which demo and step.
- **map4_s02 warp: jump to address 0.** One of five PCSX2 warp runs into map4_s02 (2026-10-23) ended in a TLB miss at pc 0x82000, address 0, after ~25 s. Not seen in normal play yet.
- **Warm-restart hang.**
  - What happens: after a remote deploy or restart, the game sometimes hangs at the title, and the PS2 needs a power cycle.
  - Clue (2026-10-18): rom0:MCSERV was loaded a second time, not by the port, just before the log stopped. The IOP SIF0 DMA was busy. See Planning Doc.md.
  - Clue (2026-10-19, two deploys in a row): both hung at the same point, about 5 s after the game starts (vblank 360-420, Konami logo step 2→3), as on 2026-10-18. Up to there the log matches a cold boot line for line, including the IOP module ids, so the IOP reboot itself is fine. After the hang the IOP agent still answers pings, but the EE's status (sent from its vertical blank interrupt over SIF) and its log both stop: either the EE is dead or its SIF DMA to the IOP is.
  - Finding (2026-10-19, later): it's a SIF stall, not a dead EE. In one run the game kept running at the title (the TV picture kept going) while for 56 s nothing from the EE reached the IOP: status frozen at vblank 1800, no log, IOP ch10 (SIF1) MADR unchanged between pings and CHCR 0x00000701 (not running). A restart command (seq 1) sent during the stall never took effect, so IOP-to-EE was stuck too. A second command (seq 2) got through: the log resumed with vblank 5160 and the restart began, then stalled after "port: sound shut down" (black screen, no Neutrino debug colours). So deploys that land during a stall fail, and the title "hang" was probably the same stall. Suspects: sh1agent.irx (src/port/iop/sh1agent/main.c) busy-waits on sceSifDmaStat in to_ee(), from ministack's receive path and from its log thread, and both write the shared mailbox; or sifman's SIF1 rearm being missed.
  - Fix tried (2026-10-19): sh1agent.irx no longer waits in the network driver's receive thread (which also receives the UDPFS disc data); commands go to the EE from a thread of its own, without busy-waiting, and the mailbox is locked. Since then: 12 of 12 warm restarts worked (one restart command was not acted on, before the loop resent missed ones). Not yet proven over a longer run. The restart also logs the EE's SIF DMA state and shows green/orange for 2 s before LoadExecPS2 (crash_ps2.c); remove once settled.
  - Still stalls (2026-10-19, evening), about 1 restart in 4: inside LoadExecPS2, after all of our steps (green shown, then black). The UDPFS server sees no request at all afterwards (no DISCOVERY, no reads), and the old IOP agent keeps answering pings: Neutrino's EE core never got as far as rebooting the IOP or loading the ELF. Quieting the IOP agent first (Port_LogStop, 'SH1Q' status) didn't change it. Also seen: lost IOP-to-EE answers (a mailbox command; padPortClose's RPC reply, now timed out). Next: Neutrino's EE core, 2nd-start path (ee_core/src/main.c: WipeUserMemory, New_Reset_Iop2, services_start), e.g. a build of ee_core with its debug colours at each step.
  - 2026-10-23: `ps2_ctl.py deploy` from a kept crash state (Floatstinger) into a debug build (SH1_WATCH_STACK, hardware data breakpoint armed at start): the new build started, armed the breakpoint, loaded its modules and reached the title (frame 240, ~5.6 s, host:warp.txt checked), then the log stopped and the screen went black; power cycle needed. (A ping got no answer, but the user was already power-cycling then, so it says nothing about the IOP agent.) A cold boot of the same build ran normally, so the warm restart caused it. Until the stall is fixed: power-cycle instead of deploying over a kept crash state.

- **Floatstinger crash (map4_s05): fixed 2026-10-26, confirmed on hardware.**
  - What happened: at the first frames of the fight, `Anim_BonesUpdate` faulted on an animation header pointer `FC00xxxx` (7 of 7 times on hardware).
  - Cause: `Floatstinger_ControlUpdate` calls its per-state function as `D_800D7A04[state]()`, declared `(void)`, though all but `func_800D341C` take the Floatstinger. The PS1 code left the pointer in a0 by chance. In the port the call is inlined into `Floatstinger_Update` right after `func_800D1968`, so a0 held what that left (a stack address), and the state function wrote Floatstinger fields into the stack. One was -1024 (Q12_ANGLE(-90.0f)) over the upper half of `Floatstinger_Update`'s saved header at sp+0.
  - Fix: the table is typed `(s_SubCharacter*)` and called with the Floatstinger (floatstinger.h/.c).
  - Not hardware only after all: PCSX2 logs the same bad load as `TLB Miss, pc=0x169eb8` and carries on (a black screen). The earlier "no fault in PCSX2" runs were checked for `CRASH` lines only. Found by replaying a console recording (src/port/demo_rec.c, `fstinger`: Reception save, map4_s04 door, into the fight); the fixed build replays it with no TLB miss.
  - Same bug elsewhere: none left (2026-10-26). A scan of the ELF for indirect calls whose a0 wasn't set since an earlier call flags this call before the fix and 22 sites after it, all checked: map header fields and state tables whose targets take no arguments, the animation duration call (already fixed, anim.c Anim_DurationGet), and false positives from the scan following address order rather than branches. It checks a0 only; typing the 194 untyped function pointer declarations would let the compiler check every call.

## Renderer (gpu_gs.c)

- **2x 32-bit: texture cache thrash.**
  - Cause: only 7 page cache slots are left beside the 2.5 MB shadow buffer.
  - Cost on hardware: 3-10 texture readbacks a frame on average, with peaks of 131 (Café) and 119-157 a frame in map6_s02. 2x 32-bit runs 10-40% slower than the other modes (e.g. Kaufmann 43.9 vs 59.7 fps, Café 36.3 vs 59.6).
  - Fix: texture streaming, the PS1 VRAM image in EE RAM (docs/720p.md, step 1).
- **2x 32-bit: strips blinking in and out** in map6_s02 on hardware, alongside the readback thrash. Not reproduced in PCSX2 yet.
- **Phone call at 2x 16-bit: 33 fps with 88% idle.**
  - What happens: MapEvent_CutscenePhoneCall (Demo #48) ran at 33 fps while the EE was waiting, not busy. It's a single sample: 2x 32-bit ran the same cutscene at 53 fps.
  - Next step: re-run it. If it repeats, suspect the lens-flare screen readback (each read scales the 2x tiles down and waits for the GS).
- **Text corrupted by toggling Resolution on hardware: fix not yet verified.**
  - Seen on 2026-10-18: text got worse with each toggle.
  - Fixed since: dirty-tile tracking and mid-texel sampling in the scale blits. Not yet confirmed on hardware.
- **Gouraud colour rounding (accepted 2026-10-22).**
  - The PS1 interpolates colours from half a unit up (rounds them); the GS truncates. One level (of 32) darker on ~10% of gouraud pixels, mostly the ground fog (textured gouraud, additive). Not visible by eye.
  - Tried: vertex colours as 2c + 1 halved by the blend (exact for untextured gouraud, close for textured), but the GS clamps the modulated colour to 255 before the blend, so only colours up to 63 qualify: ~50 pixels a frame gained, not kept. Exact options left: untextured gouraud through a constant texel; textured gouraud only by rasterising off the GS (VU1/EE).
- **2x has only scaled dithering.**
  - The GS applies the 4x4 dither matrix per 2x pixel (DuckStation's "Scaled"); the PS1's coarse look at 2x ("Unscaled", 2x2-pixel cells) would need the pattern drawn through a texture.
- **2x 32-bit looks lighter than the PS1** (blacks lifted ~1 level): faint additive layers keep their fractions in true colour. DuckStation's TrueColor does the same, so it's expected, not a bug.
- **Exact blending (modes 0, 2, 3) not measured on hardware.**
  - Each such polygon costs three draws (opaque texels, the scratch area, the composite). Rare in the game frames captured so far (~70 subtract-mode polygons, no average or quarter-add); measure if a scene uses them heavily.
  - Not used with the 2x or 32-bit shadow buffer, for fog pairs, or past a 256x128 box: those keep the 1-2 level differences.
- **Filtered CLUT cost not measured on hardware.**
  - The flare's filtered CLUTs read the CLUT back from the GS once per CLUT, then refilter and upload 16 entries per new sprite colour, and each filtered polygon is drawn twice (the mask-bit pass).
  - map4_s02's effects use them on ~340 polygons a frame; since 2026-10-23 they're cached by filter thresholds in 256 slots (PS2 Optimizations.md), ~23 misses a frame left there.
- **2x prototype limits.**
  - Lines stay 1 pixel wide.
  - A clip area straddling the 2x area's edge is drawn at 1x (logged "hires: clip area ... straddles").
  - Feedback effects reading a 32-bit shadow buffer lose the mask (semi-transparency) bit.
  - The credits weren't covered by the screen-read inventory.

## Testing

- **The PCSX2 memory card's options have brightness 0** (the default is 3): the port auto-loads them at boot, so test runs from it are darker than a fresh DuckStation run. DuckStation's card 1 now holds the same PS1 card image (backup: shared_card_1.mcd.empty_backup).
- **Attract demos need the PS1's load timing to stay exactly in sync**: the port's chunk loads finish faster (demo 4: 17 vs 33 frames at step 881) and Harry's animation runs on meanwhile. `compare_frames.py --ps1-holds` reproduces the PS1's timing for comparisons only.

## Options / UI

- **Debug Options is opened with Triangle (temporary).**
  - Triangle is also one of the default Cancel buttons. On the main options screen it now only opens Debug Options, and Cancel there is Circle/Square.
  - Replace it with a better way in.
