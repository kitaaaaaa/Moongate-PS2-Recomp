# Exact GS renderer (`PS2X_GS_BACKEND=exact`)

Working notes for the GS-manual renderer of SLPM-62767 (Virtual-On, Model 2 port). Kept current so work can
resume after a context reset. Code: `external/gitaroo-man-native/external/PS2Recomp/ps2xRuntime/src/lib/gs/`
`gs_exact_renderer.{h,cpp}`, interface `gs_host_renderer.h`, hosted by `GSGpuBackend` (`gs_gpu_backend.cpp`).
References: `ref/manuals/gs_users_manual.pdf` (+ `.txt`, page images `ref/manuals/pages/`),
`ref/manuals/ee_users_manual.pdf`, audit of the old renderers `ref/gs_manual_audit.json`,
Model 2 state map `docs/model2-state-map.md`.

## Design

- All 4 MiB of GS memory is one GPU SSBO (`m_memBuf`), addressed with the chapter 8 swizzle tables
  (`GSMem::PageTableData`, uploaded as `m_swzBuf` + per-PSM constants `m_psmBuf`).
- The CPU copy (`m_vram`, shared with `GSCpuBackend`) is kept in step per 8 KiB page: `m_gpuNewer` (download
  before the CPU reads), `m_cpuNewer` (upload before the next GPU work).
- Drawing: each primitive becomes a screen quad over its pixel bounding box; the fragment shader does coverage
  itself (3.2.9 rules) and the whole 3.4-3.9 pipeline, `gsShade()` (coverage, DDA values, texture, fog, test
  inputs) then `gsWrite()` (read-modify-write) inside `GL_ARB_fragment_shader_interlock` (ordered per pixel).
- Batching: one GL draw per flush. A primitive joins the batch unless (a) its texture pages are written by the
  batch (`texture-reads-batch`), (b) it writes pages written by another frame/Z configuration or read as
  texture by the batch (`write-overlap`). See `Draw()`.
- CLUT temporary buffer (3.4.7): a ring of 2048 snapshots (512 x 16-bit entries) in `m_clutBuf`; each load makes
  a new snapshot (copy of the previous + loaded entries), draws reference their snapshot index (state word 21).
  CBP0/CBP1 + CLD logic on the CPU (`LoadClut`). Identical consecutive loads from unchanged pages are skipped.
- Sequence (`kSequenceMain`, compute): small-target primitives (FBW 1, no Z, bbox <= 4096 px, pages untouched by
  the batch's draw) and all CLUT loads run in command order in one workgroup before the batch's draw.
- Host->local transfers (4.3) run on the GPU (`BeginTransfer`/`UploadImage`, compute `kXferBody`), carrying
  split RGB24 pixels across uploads. Local->host / local->local still use the CPU path (download first).
- PCRTC (5.2): `PresentFromGpu` composes both read circuits + merge circuit into a canvas (union of DISPLAY
  rects, interlaced modes refresh only the current field's lines). Both circuits off -> BGCOLOR.
- "Manual silent" choices are marked in the code and counted (shader counters + `Note`), printed by
  `PS2X_GS_GPU_STATS=1` / at shutdown.

## Switches

`PS2X_GS_BACKEND=exact`, `PS2X_GS_GPU_STATS=1` (every 10 s: flush reasons, downloads, game frames/s, sequence
counts), `PS2X_GS_EXACT_TIMING=1` (GPU timer queries; they stall, timing only), `PS2X_GS_EXACT_DUMP=<present#>`
(+ `PS2X_GS_EXACT_DUMP_TEX="psm:bp:bw:w:h;..."`), `PS2X_GS_EXACT_LOG_DISPLAY=<tick>`,
`PS2X_GS_EXACT_CHECK_CLUT=<n>` (GPU CLUT loads vs CPU reads), `PS2X_GS_EXACT_DEBUG=1..4`,
`PS2X_GS_EXACT_NO_SEQUENCE=1` (old path: small-target prims in the main batch).

## Bugs found on the way (runtime, not the renderer)

- `sceGszbufaddr` runtime stub read (w,h) from a2/a3 -> display buffers overlapped (FBP 7): forced native.
- Scratchpad DMA interleave mode (CHCR MOD=2) was dropped -> stale Model 2 texture pages: implemented
  (`PS2Memory::tryProcessScratchpadDma`, EE manual 5.2.5).

## CPU execution of small-target work (robot palettes)

- Per robot polygon the game draws two 8-pixel textured lines + a point into FBP 411 (64 wide), loads them as a
  CLUT (CBP 13152, T4, CSM1) and draws the polygon: ~1600 per game frame. On the GPU each is a dependent step.
- `CpuGs` (gs_exact_renderer.cpp) is a line-for-line C++ port of `gsShade`/`gsWrite`. Sequence-eligible points,
  lines and sprites (UV texturing only, non-indexed textures, MMAG == MMIN <= 1 so the LOD cannot matter) run on the
  CPU when their pages are current on the CPU and no queued GPU work touches them; GPU-newer pages are pulled
  once (`cpu-pull`). Their CLUT loads then run on the CPU too (`CpuClutLoad`, snapshot mirror `m_clutMirror`,
  snapshots uploaded at the next flush). Host->local transfers are applied to `m_vram` as well.
- Lines and sprites interpolate exactly (`floorDiv`/`lerpFloor`, both GLSL and C++): the GPU's double division
  is not exact, which picked neighbouring texels (found by the check). UV kept to 1/16 texel (manual silent).
- `PS2X_GS_EXACT_CPU_CHECK=1`: the GPU does the real work, the CPU runs the same primitives on a copy and every
  flush compares the pages: 2,478,351 primitives, 180,854 page compares, 0 differing words (2026-11-17).
  `PS2X_GS_EXACT_NO_CPU=1` turns the CPU path off.

## Speed (3D attract mode, game frames/s = DISPFB swaps/s)

Old GPU renderer ~2.7. Exact: 5 -> 7.8 (timer queries off) -> 8.5 (GPU transfers, no page downloads) -> 9.5
(page-overlap batching) -> ~10 (sequence: CLUT round trips gone, flushes 1.6M -> 0.1M per 100 s) -> ~12.5
(CPU execution of the palette work; GPU sequence was ~8 us per dependent item) -> ~13.5 (transfers mirrored on
the CPU only for pages CPU work uses, `m_cpuUse`). Profile in heavy 3D (scratch/prof/exact4.txt): VU1Interpreter
thread 78% busy = the limit now; GS host thread ~73% (CpuRunPrim 12%, driver 18%); EE thread ~42%.
Now GPU-bound in the sequence: ~5M dependent tiny items per 100 s at ~8 us each (barrier width does not matter:
64- and 1024-thread groups measured the same). Robot palettes: per polygon the game draws two 8-pixel textured
lines + a point into FBP 411 (64 wide), loads them as a CLUT (CBP 13152, T4, CSM1), then draws the polygon.
Next step (planned): run the sequence items on the CPU (C++ port of gsShade/gsWrite, same arithmetic,
`precise`/no FP contraction) when their input pages are current on the CPU, with a check mode comparing CPU and
GPU results; transfers then also update `m_vram`. Profile at ~9.5 fps: VU1Interpreter thread ~60% busy (VU1 is
interpreted in this game) -> needs lifting next; then the display switches (interlace off, PCRTC two-circuit
flicker filter off like Marz `PS2X_GS_FLICKER_FILTER=0`).

## VU1 lift (2026-11-17)

- Census: `PS2X_VU1_CENSUS=scratch/vu1census1` over 130 s of attract mode (exact backend). The game has one
  VU1 image (crc f99ddaa0a234d1ca, 0x3A88 bytes code, MSCAL 0 then MSCNT at 0x1AE8; E bits at 0x03A8/0x1AD8).
  1.3 G instructions per 130 s, 46 JR targets, census hazards all `disagree 0`.
- Lifted with the Marz tool, census mode (the ELF is stripped, so no `--elf`):
  `python J:/Marz-decomp/marz/tools/vu1lift_marz.py --out scratch/vu1_moongate.inc scratch/vu1census1`
  -> 144 blocks, 1014/1873 pairs reachable. Copied over the runtime's
  `src/lib/vu/generated/ps2_vu1_lifted_marz.inc` (the include name is hard-wired; the Marz file that was there is
  kept in `scratch/ps2_vu1_lifted_marz.inc.marz_orig`). Generated from the game: never commit it.
- Speed (heavy 3D, game frames/s per 10 s window): ~14 -> ~17.5-18.4 with the lift. `PS2X_VU1_LIFT=0` = interpreter.
- `PS2X_VU1_VERIFY=1`: ~20% of runs differ, all in leftover registers only (vf22-24, ACC); memory and GS packets
  equal. Cause (traced with the new `PS2X_VU1_VERIFY_TRACE=1`, which replays the first mismatching run in the
  interpreter, logging per pair pc/vf22-24/acc/cycle/mac/vi1/vi4/vi5, `ps2_vu1_core.cpp` `g_vu1TracePairs`):
  back-face test at 0x25C0 OPMULA / 0x25C8 OPMSUB / 0x25E8 FMAND 0x20 / 0x25F0 IBNE. For a zero-area triangle the
  two edge vectors are equal, the float result is exactly 0. Lifted flags from the result: Z set, S clear -> culled
  (jump 0x2600). The interpreter (`normalizeFmacResult` -> `calculateFmacExactResult`) recomputes acc - product in
  double (exact product, about +-2^28 here), keeps the stored 0 but sets S from that value and no Z -> not culled
  (0x2610 path, loads zeros, kicks the same packet). `PS2X_VU1_FLAGS_SLOW=1` does not change it.
  Open question for the user: which flag rule the VU hardware follows (VU manual not in ref/ yet).
- VU User's Manual fetched (archive.org ps2-development-docs, `ref/manuals/vu_users_manual.pdf/.txt`). 3.3.1 MAC
  Flags: Z "set to 1 when the calculation results are 0", S "set to 0 when the calculation results are positive or
  0"; for MADD/MSUB the MAC Z/S are "according to the calculation results of addition", multiplication and
  addition being separate exception stages (tables after 3.3.2, manual pages 41-43). So hardware: result 0 -> Z=1,
  S=0, as the lifted code does. The interpreter's `calculateFmacExactResult` uses the unrounded product for
  product-sum ops and sets S without Z here: interpreter deviates from the manual (not changed yet, needs the
  user's go-ahead; it is shared with the verifier's reference).

## Speed after the VU1 lift (profile scratch/prof/exact5.txt, 2026-11-17)

VU1 is no longer the limit. GS host thread: 32% own code + glBufferData 27% + stalls in FlushDraws 15%
(`scratch/prof/callers.py <file> <tid>` attributes driver/ntdll samples to the nearest exe caller). GS packet
thread blocked 54% in GSGpuBackend::Enqueue (host queue full), VU1 thread waits on the packet thread, EE thread
64% waiting (EeScheduler::run 21%). Chain: host GL submission -> queue backpressure -> VU1 -> EE.

## Host-thread fixes (2026-11-17, after the VU1 lift)

Facts (scratch/wf_*.md = verified investigation reports; PS2X_TRACE_VBLANK): game logic runs at 60 ticks/s of real
time (semaphore per vblank, main loop at 0x100590 draws once per pending tick, skips draws when ticks pile up), so
low fps = dropped pictures, not slow game. The limit is the GS host/GL thread (~42-45 ms of work per game frame).
- `FlushDraws`: buffers re-specified with glBufferData every flush (driver allocation each time) -> grow-only
  buffers (`UploadStream`); CLUT snapshots uploaded per contiguous run (was one call per 2 KiB snapshot,
  ~40k/s); `uDebug` location cached; glGetError only with PS2X_GS_EXACT_DEBUG. flush cpu 22 s -> 2 s per ~110 s.
- Upload ring (`StreamTo`): every upload into a buffer the GPU may still use (GS memory pages, prims, states,
  sequence, image data, CLUT snapshots) goes through a 64 MiB persistently mapped ring (4 fenced segments) and a
  GPU-side glCopyBufferSubData, so the host thread never waits on the GPU for uploads. Buffer-update barrier
  before copies into m_memBuf/m_clutBuf. `PS2X_GS_EXACT_NO_RING=1` = old glBufferSubData path. ~+10%.
  CPU check after the change: 2,144,911 prims, 103,098 page compares, 0 bad words.
- Draw(): DrawTimer only with PS2X_GS_GPU_STATS, texSpans fixed array, mtba counter without a string map.
- Diagnostics added: `PS2X_GS_CMD_TIMING=1` (host time per command type), readback timing in the stats line,
  `PS2X_GS_MAX_READY_BATCHES` (queue depth; 1024 did not help).
- Still left (measured per game frame on the host thread): Submit ~25 ms (24k prims; ~7k are CPU palette prims),
  LoadClut ~12 ms (~2k CPU CLUT loads), synchronous readbacks ~12% of wall (cpu-pull ~44/s, clut-mirror ~55/s).
  Game frames/s in heavy 3D: ~18-20.

## Host CPU work cut (2026-11-17, second round) -> ~29 game fps in heavy 3D

- Draw main path: span cache (`m_spanCache`, `m_batchGen`, `FlushPendingEpoch`): a primitive with the previous
  one's frame/Z spans and no page-array change since skips the five ~140-page walks; page epochs written lazily.
- CLUT: no more synchronous mirror readback. Per-entry state `m_clutUnknownGen` (GPU load generation, 0 = known).
  GPU loads mark their entries unknown and, after the flush that runs them, the GPU copies that snapshot into a
  mapped readback buffer behind a fence (`m_rbBuf`, `PollClutReadbacks`, never waits); once signalled, entries
  whose last writer is still that load are filled in. While entries are unknown, CPU loads make their snapshot as a
  GPU copy of the previous one plus the loaded entries (`SnapOp.copyFrom`). CPU snapshots go up in place, one copy.
- CPU executor handles indexed textures (CLUT lookup ported from the shader's texel()), only when every CLUT
  entry is known; texture format decided before any page pull (the indexed sprite into FBP 409 used to pull pages
  and then go to the GPU anyway, ~8 ms per pull). Check mode keeps the mirror by running GPU loads on m_checkMem.
- Barriers: one GL_BUFFER_UPDATE barrier per upload group (UploadDirtyPages, snapshot block), not per copy
  (thousands of barriers per frame had made the GPU fall ~20 ms behind).
- Stats: stream/snapshot copies, clut readbacks taken, page readback time.
- CPU check after all of it: 2,168,214 prims, 128,752 page compares, 0 bad words.
- Left: page-409 pulls ~1 per game frame (~6 ms each); per-primitive Draw cost.

## Display switches (exact renderer, PresentFromGpu / kCrtcFragmentBody)

Both off by default (user request; afterimages): interlace -> every line from the current buffer each present
(`uAllLines`; same line mapping); the PS2X_GS_INTERLACE switch was removed (the game has its own 480i/480p option). Anti-flicker -> when
both circuits show the same buffer with MMOD, ALP forced to 255 = circuit 1 exactly; PS2X_GS_FLICKER_FILTER=1 =
the blend (as the GS). (The old GPU renderer's PS2X_GS_FLICKER_FILTER=0 switch is the Marz one.)

## High-resolution shadow buffers (`PS2X_GS_HIRES`, on by default)

Everything the window shows is drawn at the window's resolution; GS memory stays the GS's own 1x memory.

- Three shadow images (`HiTarget`): the work buffer FBP 224 (496 x 384) and the display buffers FBP 0 / 0x70
  (512 x 448), FBW 8, 32-bit. Size = the window rectangle of the picture (`DrawPresentation`) per PS2 canvas pixel,
  so the display buffers map 1:1 to the window and FBP 224 is stretched like the TYPE-C copy (all three the same size).
- Shadow pass (`kHiVertex`, `kDrawFragmentBody` with `#define HIRES`, `kHiFragmentMain`, `HiDrawPasses`): every
  primitive of a batch whose frame buffer is a shadowed buffer is drawn again into its image by the same pipeline,
  sampled at the image pixel's centre (a fractional GS position; `cxd/cyd` doubles in gsShade), before the batch's 1x
  draw. Frame (and Z when ZBP = FBP) read/write the image (r32ui view of RGBA8) inside the interlock; a depth test
  against another Z buffer uses a high-resolution copy of that Z buffer loaded from GS memory at pass start
  (`m_hiZTex`); textures that are a shadowed buffer (TBP0 = its base, TBW 8, 32/24-bit) are read from its image
  (`hiSource`/`sampleShadow`, the port's copy FBP 224 -> display buffer), other textures from GS memory.
- Z written into a shadowed buffer by a primitive drawing elsewhere is a pass of its own (role 2): the port paints
  its weapon gauges that way (sprites over FBP 224 with an alpha test on its alpha, "Z" = the colour, into the
  display buffer). Primitives that only write Z are part of a frame pass only when that pass keeps a depth copy
  (prim word 29, culled otherwise): the port clears Z before its depth-tested small text.
- Precision: the high-resolution passes run the body with `REAL` = float (double in the 1x passes; `LF()`
  literals), depth values stay double; triangle edges are evaluated from their lower endpoint so shared edges get
  exactly opposite values. The 1x path is unchanged (CPU check after the change: 1,985,402 prims, 0 bad words).
- Writes the passes do not repeat (transfers, CPU work, other layouts over those pages) mark the image stale (`HiMarkStale`, by the primitive's own box); it is
  copied from GS memory (`kHiResyncBody`) only when next used (drawn into, read as texture, shown). The native
  picture replaces the work buffer image entirely each frame, so those copies are rare in play.
- PCRTC: `PresentFromGpu` makes the canvas at the window's size; a circuit whose buffer has an image reads it at the
  fractional buffer position (`uHiCanvas`, `uHiSrc`), others read GS memory as before.
- Native picture (moon_native.cpp): drawn once at `HiResSize(224)`, written with `WriteHiResFromTexture`; GS memory
  gets the key colour with alpha 0x40 (the port's alpha tests need it). The old key-colour overlay is gone.
- Switches: `PS2X_GS_HIRES=0` (renderer) / `PS2X_MOON_HIRES=0` (native picture at 1x): the previous 1x path.
  `PS2X_GS_HIRES_TRACE=<n>` logs the first n primitives with a shadow role or stale write, `PS2X_GS_HIRES_TIMING=1`
  GPU time of the passes (waits for the GPU), counters `hires-*` in `PS2X_GS_GPU_STATS`.
- Measured (scripted 270 s run: menus, stage intro, match, YOU LOSE, continue, game over, attract; 1920 x 1080 and
  1600 x 900 WIDE, RTX 2080): 600 presents per 10 s after boot; 81 presents dropped, all during boot (1x path: 61-72).
  No copies from GS memory after boot. The first run after a shader change drops more while the driver compiles.
