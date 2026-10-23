# PS2 decomps: graphics pipelines compared with the port

2026-10-23. Two PS2 decompilations, cloned to `~/Documents/PS2 Decomps/`:

- **Chronicle** (github.com/TheMoonPeople/Chronicle): Dark Cloud, fully decompiled (no assembly left), with a PC port. The rendering code was read directly, the VU1 microcode included.
- **ICO-decomp** (github.com/RossyDoubleUnderscore/ICO-decomp): ICO, early. One C file is decompiled (the `ios` thread system). Its pipeline below is inferred from the ~5,800 names in `config/symbol_addrs.txt`, the source layout in `config/ICO-PAL.yaml` and the 12 VU microcode overlays in the ELF's section headers: a well-founded guess, not a reading of the code.

The performance figures are estimates from the code's structure and from the port's own measurements, not measurements of those games.

## Dark Cloud (Chronicle)

### Original PS2 pipeline
- **Models converted at load.** `CVisualVu1::CreateVUdataFromMDT` (ps2/src/visualvu1.cpp) turns each MDT model into a VU1-ready block: vertices, normals, UVs, colours and the GS primitive setup.
- **Per draw: a small header and a pointer.** `CVisualVu1::DrawVu1` writes ~30 quadwords per object: a VIF unpack header, the screen, clip and viewport matrices, flags (lit, fog, shadow pass), fog parameters and the eye position in model space. It then adds a DMA *ref* tag (`vu_size | 0x30000000`, `vu_data`) pointing at the model's prebuilt block. The EE never touches individual vertices.
- **VU1 does the per-vertex work**: transform, lighting, fog and clipping, then GS packets straight to the GS. Programs in ps2/src/vutext.cpp: `Vu_prog0`, `Vu_prog0f`, `Vu_prog1`, `Vu_progg`, `Vu_progmain`.
- **Whole frames double-buffered.** One VIF1 packet per frame (`MGBeginFrame` resets it). `MGEndFrame` waits for the previous frame's path (`sceGsSyncPath`), swaps the display buffers and sends the new frame in one DMA, so the EE builds frame N+1 while VU1 and the GS draw frame N. Retail prints "CPU %" and "FREE %" from timer 0.
- **Effects** through render-to-texture and frame grabs: shadows (`MGBeginDrawShadow`/`MGEndDrawShadow`, `shadow_buf`), water and depth of field (`MGMoveImage`, `MGStretchMoveImage`, `frame_image`). Depth picking reads an 8x8 block of the Z buffer back (`mgPickZBuff`).
- **Textures:** TIM2 in a GS memory allocator (`CTextureManager`, texture animation by copies).
- **Skinned models** are rebuilt on the EE (`CreateVUdataFromMDTRemake`): the one per-vertex job the EE keeps.

### PC port
The PS2 renderer is replaced by Vulkan through a shared engine, Kazarov (docs/PC_PORT_PLAN.md 4.2 and 4.6, docs/PC.md "Rendering"):
- Models become GPU meshes. The `vu_data` pointer becomes a mesh handle, and `DrawVu1` becomes a `DrawMesh` with the constants the VU1 header carried. Frame culling and hierarchy logic are kept.
- GS semantics in shaders: the blend `((A - B) * C >> 7) + D`, alpha test, fog, MODULATE at 0x80 = 1.0. Reverse-Z depth matches the game's convention.
- Bindless textures, 8-bit TIM2 as an index texture plus a palette (cheap CLUT swaps), every pipeline compiled at start-up, an optional render thread, render scaling and widescreen (HUD anchoring, depth of field and water reworked).
- It aims at the look, not the PS2's exact pixels.

## ICO (inferred)

The same native model as Dark Cloud, with more GS work:
- **Load-time packet building** (`pac_*`: `makeStrip`, `MakeClusterStrip`, `makeMaterialTable`, `openDmaTag`, `SetVifCode`, `SetGifTag`, `MakePacket`): DMA, VIF and GIF chains built when a model loads. Weight-clustered strips suggest skinning on VU1.
- **Microcode per object** (`reg_chooseMicroCode`, `reg_chooseSpecularMicroCode`, `reg_chooseReflectionMicroCode`, `reg_transMicroCode`) and matrix sets per object type (`reg_set{N,M,C,E}MatrixPacket`). There are 12 VU microcode overlays in the ELF (`.DVP.overlay`).
- **GS-heavy effects:**
  - Shadow volumes (`shadow_RenderVolume`, `shadow_RenderVolumeMulti`).
  - Depth fog (`seki/src/ZFog`).
  - Full-screen post-processing (`gsb_Reduction`, `gsb_KeepFrameBuffer`, `gsb_SetMotionBlur`, `gsb_filmNoise`, `gsb_antiAlias`, `gsb_controlBrightness`, `gsb_PostEffect`).
- **Textures:** a GS memory manager (`tex_AllocVramAuto`) with TIM2, mipmaps (`tex_UpdateMipMapLevel`), CLUT animation (`tex_SetClutAnimation`, `tex_scrollClut`), UV scrolling and a CSM2→CSM1 conversion.
- **Level of detail** (`sugipon/src/lodManager`). 2D through `gif_*` helpers (sprites, strips, lines; path 1 and path 3 packets).
- **Multithreaded** game framework (`fumi/ios`: threads, message queues, memory partitions per subsystem).

## Compared with the port

| | Dark Cloud | ICO | Silent Hill port |
|---|---|---|---|
| Geometry built | once, at load | once, at load | every frame, by the game's PS1 code |
| Per-vertex work | VU1 (the EE for skinning) | VU1, probably skinning too | the EE: the GTE in C (gte.c), with batched paths |
| Per-primitive work | VU1 | VU1 | the EE: each PS1 GP0 packet translated to GS packets (DrawOTag, gpu_gs.c) |
| EE cost per object | ~30 quadwords plus a DMA pointer | about the same | ~1,300 cycles per primitive for drawing alone (map4_s02: 737k cycles, ~560 primitives) |
| Textures | native TIM2, mipmaps | the same | PS1 VRAM kept in GS memory; page, CLUT and filtered-CLUT caches |
| Accuracy target | the game's look | the game's look | the PS1's pixels (dither, blending, fog pairs, filtered CLUTs) |
| Main limit | GS fill rate, VU1 | GS fill rate (post effects, shadow volumes) | the EE |

Those games were authored for VU1 and hand it the vertex and primitive work. The port runs the PS1 game's own logic, which builds a new list of 2D primitives every frame, so the EE translates each one and VU1 is unused.

## Performance estimates
- **Dark Cloud:** ~1-2k EE cycles per object for the matrix maths and the header. VU1 handles thousands of vertices a frame. Limited by GS fill (frame grabs, depth of field, shadow textures), VU1 time and EE skinning. It targets 30 fps with spare EE time.
- **ICO:** EE cost as low as Dark Cloud's; GS fill heavier (shadow volumes, several full-screen passes, large transparent areas). About 30 fps with known slowdowns: a GS fill or VU1 limit, not an EE one.
- **Chronicle PC port:** rendering is trivial on a modern GPU; it's limited by the game logic on the CPU, which the render thread helps with.
- **The port:** PS1 scenes are small (~500-2,000 primitives a frame), so 60 fps is reachable. In map4_s02, after the filtered CLUT fix: ~3.5M of the ~4.9M EE cycles a 60 fps frame allows (in PCSX2), with VU1 idle and the GS lightly loaded. Per triangle, the native games are roughly 10-30x cheaper on the EE.
