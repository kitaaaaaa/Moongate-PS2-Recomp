# Native Model 2 renderer (60 fps work)

Goal (user, 2026-11-17): 60 fps minimum. The 3D and tilemap layers are drawn natively from the Model 2 state the
game builds each frame, following Model 2 behaviour (MAME `src/mame/sega/model2_v.cpp`, `model2rd.ipp` as the
structure reference); PS2-only screens stay on the exact GS renderer. Background facts: `docs/model2-state-map.md`,
`scratch/native_map/verified.txt` (checked mapping: no i960 code anywhere; game logic is native MIPS).

## Verified data sources (scratch/native/*.py)

- **Geo command list**: bufferRAM list at 0x342C40 decodes with MAME's geo opcode word counts, except op1, which the
  PS2 Geo API writes with a 5th word (type word: bits 4-7 model type, 0-3 model no, 8-11 sub). `geolist.py` on
  tr4000/ram_004002.bin: op16 LOD, op07 mode = 3 (no normals, specular -> MAME `geo_parse_nn_s`), op08 zsort,
  op03 window, op06 texture params (32), op09 focal, op0A light, op0B matrix, op01 objects (105), END 0x07800F0F.
  The list is rebuilt per tick (FUN_001f9bd8 fills it with END words), so dumps taken after the reset show only END.
- **Polygon data (PA)**: `pdtrom.bin` on the disc is the arcade polygon ROM: object data at byte
  `(PA & 0x3FFFFF) * 4`, little-endian, MAME nn format (2 start points, then 10-word records attr + 3 unused +
  2 points, attr & 3 == 0 ends). Parses for every object in the tick-4002 list; type-5 (stage) objects also match the
  resident pd50 copy in EE RAM; the per-model pd*.bin files are slices of pdtrom (`pdcheck.py`, `slices.py`).
- **Texture points (TP)**: MAME texture ROM (u16 index). `tdt0rom.bin` = u16 0..0x3FFFFF, `tdt1rom.bin` =
  0x400000..; type-5 object TP data equals tdt0rom at TP*2 (`tdcheck.py`); tp*.bin files are pieces of both.
- **Texture headers (TH)**: the port does not use the arcade headers. It translates TH<<1 per model through key
  lists (`FUN_001fcf48`: table 0x46D668[type][no] -> {key, offset} pairs, exact key match, -1 ends) into its own
  `th_hi<type><no>.bin` files (loaded by FUN_001fb768, model id = type<<4 | no). The converter FUN_00209c80
  (`scratch/native/conv.c`) reads them in Model 2 layout plus PS2 extension bits: th0 bits 0-2/3-5 width/height
  (32<<n), 6/7 wrap, 13 transparent, 14 textured, 15 checker; th1 low byte = luma row, th1 bit 8/9 = double
  x/y, bit 10 = pages 2/3 (GS TBP 0x2A10) instead of 0/1 (0x3800); th2 bits 0-5 *32 = x, bits 6-11 *32 = y,
  bit 12 = second sheet (+0x400); th3 >> 6 = colour base.
- **Colour tables**: 0x6BC270/0x6C0270/0x6C4270 = MAME colorxlat R/G/B (32 rows x 256 u16). Luma rows:
  W2+0x1457C0 (0x992440), 128 bytes per row, indexed by th1 & 0xFF (FUN_0020a548).

## Open (not yet established)

- (resolved) Texture atlases: A = pages 0/1 uploaded by FUN_00208A88 as 8x8 transfers of 128x128 PSMT4 (EE tile
  (x,y) at +y*0x10000 + x*0x2000) to TBP 0x3800 TBW 16; B = pages 2/3 by FUN_00208DA0 as two 512x1024 PSMT4 at
  DSAX 0/512, TBP 0x2A10. They hold the Model 2 sheets at half resolution: texture at Model 2 (texx, texy) is at
  ((texx << dbx) + 0x400*sheet) / 2, (texy << dby) / 2, texels scaled by 1/2 (or 1 with the double bits); from
  FUN_00209c80's TEX0/UV arithmetic and the block table 0x550C00. The UV nudges/fixups in FUN_002096e8 /
  FUN_002097e0 / FUN_00209bb0 are GS-specific and not used natively.
- (resolved) Polygon palette = g_geo 0x6C8300, 1024 x RGB555 (FUN_00208380 reads 0x800 bytes from there to build
  the port's 64x1024 colour LUT; bit 15 selects its blend mode there).

## Headroom (measured 2026-11-17, exact backend, attract mode, game frames per 10 s window)

- Normal: ~30/s (cumulative 30.1 at 110 s).
- `PS2X_MOON_NO3D=1 PS2X_MOON_NOTILE=1`: ~43/s. Still left: colour-translation pass with a GS page readback
  (~1 per game frame, ~6.5 ms each), mask layers, ~135k CPU-executed small prims per game frame.
- `PS2X_MOON_NOGEO=1 PS2X_MOON_NOTILE=1` (no FUN_002093c0, no FUN_00207538): 600 per 10 s = 60.0 locked
  (screen black). So the native renderer must replace both functions entirely; everything else fits in 60.

## Native renderer (working, 2026-11-17)

`moon/src/moon_native.cpp`. On by default with `PS2X_GS_BACKEND=exact` (the launcher's default);
`PS2X_MOON_NATIVE=0` keeps the port's own GS path. Replaces FUN_002093c0 (polygons) and FUN_00207538 (tilemap).
`PS2X_MOON_NATIVE_STATS=1` prints polygons/frame and ms/frame every 600 frames; `PS2X_MOON_NATIVE_TRACE=1` logs
each marker emitted and reached.

**Ordering (2026-11-18, fixed the black title menu):** FUN_002093c0 does not draw; it adds DMA CALLs to the
frame's VIF1 chain, which runs later. Queuing the picture at hook time put it before the frame's own GS work
(clear, menus), and FRAME_1 then was not yet the frame's buffer (always FBP 0). Now the hook links a CALL node
(FUN_00244340 written out in C++) to a one-qword packet in the runtime's guest heap: a DMA `ret` tag whose VIF codes
are NOP(NUM 0x7E, IMM id) and NOP. The runtime's VIF1 interpreter turns that NOP into a GIF A+D write of GS
register 0x7E (undefined on the GS), and the GS frontend calls the game's marker callback for it, in stream order,
with FRAME_1 at that point (alternates FBP 0 / 112 as the port's own copy does). The callback queues the native
render/write. Tried and dropped (each hung the game at the boot memory card check, markers stopped being reached):
calling the recompiled FUN_00246860 from the hook; a DIRECT/DIRECTHL GIF packet of our own (the runtime continues an
unfinished DIRECT image transfer with the next DIRECT data); VIF MARK (changes VIF1_MARK/STAT). Also tried and
dropped: writing into FBP 224 and calling the port's FUN_00205200 (all black).

Measured (game frames per 10 s, exact backend): attract locked 600; a 4-minute scripted match
(`start@950,circle@1250..3050`: title menu -> arcade -> machine select -> stage 1 -> loss -> continue -> game over)
600 in all gameplay windows, ~590 polygons/frame, native cost 0.40 ms/frame on the EE thread and 0.30 ms on the
host. Lower windows at boot (523-539) and two transitions (568, 516). The same input with no Model 2 output at all
(`PS2X_MOON_NOGEO=1 PS2X_MOON_NOTILE=1`) gives the same pattern (516-540 at boot, 566 and 520 at the transitions),
so those dips are the game's own, not the renderer's.

- Geo list: ops 1 (objects), 2 (the port's direct data, see below), 3, 6, 7, 8, 9, A, B, 0x10, 0x16, 0x1B (port,
  2 words, ignored), 0x1C (port overlay hook, 0 words, counted and logged, not drawn), END. Any other op stops the
  list (the port's parser spins on them) and is logged.
- Op 2 (port format, FUN_001f9c58 case 2): TP, TH, 6 words, records of 9 words {attr, luma, distance, point, point}
  until attr 0. The port makes them object data {attr, normal (0, 0, *0x5671A0), point, point}, translates TP/TH
  as model type 5 no 0 and draws them as an object (current matrix, lighting); luma/distance are dropped. HUD bars,
  timer box, reticle and weapon gauges are op 2.
- FUN_002093c0's branches are kept: list head 0x7FFFC = repeat (only FUN_00205200: the native path re-writes the
  last picture), head < 0x20000 with a list = draw, else nothing. W2+0x18DD2C (double-buffer index) advances every
  call, W2+0x18DD30 per drawn frame, and the overlay module word *(*0x568518 + 0x35C0) toggles as FUN_00205200 does
  (FUN_0014dbd8 reads it as a gate).
- Tilemap: MAME segaic24 draw_common/draw_rect per pixel (scroll, line scroll, split modes 1-3, window masks) on
  the port's block 0x62FA00 (names +0, line scroll +0x8000, registers +0x9000, masks +0x9010/+0x9C10), char sheet
  0x63A210 (atlas-A layout), palette 0x6BA210 with colorxlat entry 0x40; MAME screen_update order: low tiles
  (3, 2 opaque; 1, 0), polygons, high tiles (category bit 15).
- Not drawn natively: FUN_0020bc10's PS2 overlay quads (0x4C1CA0; none active in the runs, logged when active) and
  op 0x1C hooks (one seen in a match frame; what it draws is not traced).
- Uploads: atlases and char sheet only when their EE bytes changed.

- EE side (hook): waits for the running VIF1 chain (so all earlier GS packets are in the backend queue), copies
  bufferRAM, W2 texture pages A/B, luma rows, polygon palette, colour tables; runs the geometrizer (MAME geo_*,
  nn/np parsers, lighting, culling, 4-plane clip, zsort value) with pdtrom/tdt ROMs and the port's th_hi headers;
  reads FRAME_1 (back buffer); queues `GS::queueNativeHostCall`.
- Host side: deswizzles the atlases (A: 8x8 tiles of 128x128 at y*0x10000 + x*0x2000; B: two 512x1024 pages side
  by side), draws the polygons in MAME's priority (painter's order reversed: window ascending, zval descending,
  oldest first) with a shader doing MAME's bilinear-on-index, luma lookup, colorxlat; writes the 512x448 picture
  to FRAME_1 with `GSNativeSink::WriteFrameFromTexture` (compute write into GS memory, exact renderer).
- Screen mapping: Model 2 window coordinates; the frame's first (full-screen) window's top-left is the screen
  origin (window 0,128..498,514 in attract = the port's 496x384 scissor), scaled to 512x448 like the port's copy.
- Deviations from MAME, by choice: no gamma table (the port uses colorxlat values directly; `PS2X_MOON_GAMMA=1`
  applies MAME's), no mipmaps/microtexture (level 0 of the port atlas), checker pattern on target pixels.
- Runtime additions: `include/runtime/gs/gs_native_layer.h` (sink, host call, marker register 0x7E),
  `GS::queueNativeHostCall`, `GS::setNativeMarker` (frontend `writeRegisterUnlocked`), `GSGpuBackend::EnqueueHostCall`
  (never waits on queue backpressure), `GSExactRenderer::WriteFrameFromTexture` (kTexWriteBody), VIF1 NOP marker
  (`ps2_vif1_interpreter.cpp`), `PS2X_TRACE_DRAWS_TICKS`.

## Hooks

`moon/src/moon_overrides.cpp` (game override module, entry 0x100134). Measurement switches:
`PS2X_MOON_NO3D=1` (skip FUN_00207810, no Model 2 polygons sent to VU1/GS), `PS2X_MOON_NOTILE=1` (skip FUN_00207538),
`PS2X_MOON_NOGEO=1` (skip FUN_002093c0). Game input for tests: `PS2X_INPUT_SCRIPT` (circle = decide in the
Sega Ages menus; start@950 reaches the title menu).
