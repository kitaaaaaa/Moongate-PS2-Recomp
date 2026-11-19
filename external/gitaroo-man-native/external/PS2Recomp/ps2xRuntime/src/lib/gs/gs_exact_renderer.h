#pragma once

#include "gs_host_renderer.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

class GSCpuBackend;

// GS drawn by the rules of the GS User's Manual (ref/manuals/gs_users_manual.pdf), on the GPU.
//
// All 4 MiB of GS local memory live in one GPU buffer, addressed with the chapter 8 page/block/
// column layouts. Every primitive is drawn by a fragment shader that decides coverage itself
// (3.2.9: pixel centres at integer window coordinates, left/top edges drawn, line diamonds, nearest
// pixel for points) and runs the 3.4-3.9 pipeline in integer arithmetic: texture coordinates, LOD
// and MIPMAP, wrap modes, format conversion, the CLUT temporary buffer, the texture function, fog,
// alpha / destination alpha / depth tests, blending, dithering, colour clamp, FBA, FBMSK and the
// format conversion of the frame and Z buffers. Fragment shader interlock keeps each pixel's
// read-modify-write in primitive order, as the GS pixel pipeline does. The display is the 5.2 PCRTC:
// two read circuits (DISPFB/DISPLAY) and the merge circuit (PMODE/BGCOLOR).
//
// Where the manual does not say (rounding of the DDA, tie rules on line diamonds, Z beyond the Z
// buffer format, ...) the code says "manual silent" and counts how often the game hits that case
// (PS2X_GS_GPU_STATS / shutdown summary), so it is known whether the choice matters.
//
// GS memory on the CPU side (the m_vram the transfers and CLUT loads use) is kept in step by 8 KiB
// page: pages drawn on the GPU are read back before the CPU uses them, pages the CPU wrote are sent
// before the next draw.
//
// PS2X_GS_BACKEND=exact selects it. Internal resolution is always 1x (the GS has no other).
class GSExactRenderer final : public GSHostRenderer, public GSNativeSink
{
public:
    explicit GSExactRenderer(GSCpuBackend &cpu);
    ~GSExactRenderer() override;

    void SetVram(uint8_t *vram) override { m_vram = vram; }
    void BeginHostWork() override;
    void EndHostWork() override;
    void Release() override;
    void Reset() override;

    void Draw(const GSPrimitiveBatch &batch) override;
    void FlushDraws() override;

    void DownloadOverlapping(uint32_t first, uint32_t count, const char *why = "other") override;
    void DownloadAll(const char *why = "other") override;
    void DownloadForPresent(const GSPresentationRequest &request) override;
    void InvalidatePages(uint32_t first, uint32_t count) override;
    void InvalidateAll() override;

    bool PresentFromGpu(const GSPresentationRequest &request) override;
    bool HasPresentation() const override { return m_presentValid; }
    void InvalidatePresentation() override { m_presentValid = false; }
    void DrawPresentation(float x, float y, float w, float h) override;

    std::string UnsupportedSummary() const override;
    std::string StatsSummary() const override;

    bool LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    bool BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;
    bool AllowsHleClear() const override { return false; }
    GSNativeSink *NativeSink() override { return this; }
    bool WriteFrameFromTexture(unsigned glTexture, const GSNativeTarget &target) override;
    bool ReadFrameToTexture(unsigned glTexture, const GSNativeTarget &target) override;
    void SetPresentHook(std::function<void(GSNativeSink *sink, const GSNativePresent &present)> hook) override { m_presentHook = std::move(hook); }
    bool HiResSize(uint32_t fbp, int &w, int &h) override;
    bool WriteHiResFromTexture(unsigned glTexture, int texW, int texH, const GSNativeTarget &target) override;

private:
    // ---- high-resolution shadow buffers (PS2X_GS_HIRES, on by default) ----
    // The work buffer (FBP 224, 496 x 384) and the two display buffers (FBP 0 / 0x70, 512 x 448, all FBW 8, 32-bit)
    // each have a shadow image at the window's resolution. Every primitive the batch draws into one of them (or
    // whose Z buffer is one of them) is drawn a second time into its shadow image by the same pipeline, sampled at
    // the shadow pixel's centre; textures that are a shadowed buffer are read from its shadow image; the PCRTC
    // picture is made from the shadow images. GS memory stays the GS's own (1x); writes the shadow passes do not
    // repeat (transfers, CPU writes, other formats over those pages) copy GS memory into the shadow (resync).
    struct HiTarget
    {
        uint32_t fbp = 0, gsW = 0, gsH = 0;
        uint32_t firstPage = 0, pageCount = 0;
        float sx = 1.0f, sy = 1.0f; // shadow pixels per GS pixel
        int w = 0, h = 0;
        unsigned tex = 0;
        bool valid = false;
        // GS-pixel rectangles to copy from GS memory: before the next use (stale) or after this batch (staleAfter).
        bool stale = false, staleAfter = false;
        int staleRect[4]{}, staleAfterRect[4]{};
        bool batchFrame = false, batchZ = false; // the batch draws into it (role 1 / 2)
        bool batchUse = false;                   // the batch draws into it or reads it as a texture
        // The batch's frame-role primitives use a Z buffer of their own (not this buffer): its high-resolution copy.
        bool batchZImg = false;
        bool batchColor = false; // a frame-role primitive that writes colour (else the pass only matters with batchZImg)
        int batchBox[4]{};       // GS-pixel box (x0, y0, x1, y1 inclusive) of the batch's frame-role primitives
        uint32_t zbp = 0, zpsm = 0, zfbw = 0;
    };
    static constexpr int kHiTargets = 3;
    std::array<HiTarget, kHiTargets> m_hi{};
    bool m_hiOn = false;
    unsigned m_hiProgram = 0, m_hiResyncProgram = 0, m_hiFbo = 0, m_hiBlitFbo[2] = {0, 0}, m_hiZTex = 0;
    int m_hiZW = 0, m_hiZH = 0;
    int m_outW = 0, m_outH = 0;              // window rectangle of the picture (DrawPresentation)
    uint32_t m_hiCanvasW = 0, m_hiCanvasH = 0; // PS2 canvas size of the last present
    void HiUpdateSizes();
    void HiMarkStale(int i, uint32_t first, uint32_t count, bool after);
    void HiResync(int i, const int rect[4]);
    void HiResyncPending(bool after, bool useOnly = false);
    void HiDrawPasses();
    void HiTouchPages(uint32_t first, uint32_t count); // GS memory written outside the shadow passes
    static constexpr uint32_t kPageCount = 512u;
    static constexpr uint32_t kPageBytes = 8192u;
    static constexpr uint32_t kPrimWords = 32u;
    static constexpr uint32_t kStateWords = 48u;
    static constexpr uint32_t kClutWords = 512u;
    static constexpr uint32_t kMaxPrims = 65536u;

    void EnsureGl();
    void UploadDirtyPages();
    void DownloadPages(uint32_t first, uint32_t count);
    uint32_t StateIndex(const GSPrimitiveBatch &batch);
    void BuildState(const GSPrimitiveBatch &batch, std::array<uint32_t, 48> &s) const;
    static void BuildPrimWords(const GSPrimitiveBatch &batch, uint32_t *p);
    void Note(const std::string &what, uint64_t n = 1) { m_counts[what] += n; }
    // Why a flush with work pending happened (PS2X_GS_GPU_STATS).
    void NoteFlush(const char *why)
    {
        if (!m_prims.empty() || !m_seq.empty())
            ++m_counts[std::string("flush:") + why];
        (void)0;
    }
    void ReadShaderCounters() const;
    void DumpCanvas(uint64_t tick);

    GSCpuBackend &m_cpu;
    uint8_t *m_vram = nullptr;
    bool m_glReady = false;
    bool m_glFailed = false;

    // GL objects.
    unsigned m_memBuf = 0, m_swzBuf = 0, m_primBuf = 0, m_stateBuf = 0, m_clutBuf = 0, m_counterBuf = 0, m_psmBuf = 0;
    unsigned m_xferProgram = 0, m_imgBuf = 0, m_texWriteProgram = 0;
    unsigned m_texReadProgram = 0;
    std::function<void(GSNativeSink *sink, const GSNativePresent &present)> m_presentHook;
    // The host->local transfer in progress (4.1 / 4.3): destination, pixels done, bytes not yet used.
    GSTransferCommand m_xfer{};
    bool m_xferActive = false;
    uint32_t m_xferDone = 0, m_xferTotal = 0, m_xferBits = 32;
    std::vector<uint8_t> m_xferCarry;
    unsigned m_drawProgram = 0, m_crtcProgram = 0, m_clutProgram = 0, m_vao = 0, m_drawFbo = 0;
    unsigned m_canvasTex = 0, m_canvasFbo = 0;
    int m_savedViewport[4]{};
    bool m_cullWasEnabled = false;

    // Page sync between m_vram and the GPU copy.
    std::array<bool, kPageCount> m_gpuNewer{};
    std::array<bool, kPageCount> m_cpuNewer{};

    // The batch being collected.
    std::vector<uint32_t> m_prims;
    std::vector<uint32_t> m_states;
    std::array<uint32_t, kStateWords> m_lastState{};
    bool m_haveLastState = false;
    uint32_t m_batchFrameKey[6]{};
    bool m_batchHasFrame = false;
    // Sequence run before the batch's draw: small-target primitives and CLUT loads, 12 words each.
    std::vector<uint32_t> m_seq;
    // CPU execution of small-target primitives and CLUT loads (CpuGs in the .cpp).
    std::vector<uint32_t> m_swzCpu, m_psmInfoCpu;
    std::array<bool, kPageCount> m_deferRead{}; // pages queued GPU sequence work reads
    std::array<bool, kPageCount> m_cpuUse{};    // pages CPU work reads or writes (transfers mirrored there)
    bool m_xferMirror = false;                  // the current transfer also goes into m_vram
    std::array<uint32_t, 512> m_clutMirror{};   // CPU copy of snapshot m_clutSnap: all entries m_clutUnknown does not mark
    // Entries GPU loads wrote, not yet known on the CPU: the generation of that load (0 = known), and how many.
    std::array<uint32_t, 512> m_clutUnknownGen{};
    uint32_t m_clutUnknownCount = 0, m_clutGpuLoadGen = 0;
    struct ClutReadback
    {
        uint32_t gen, snap, rbSlot;
        void *fence; // on the last copy of a flush's group
    };
    std::vector<ClutReadback> m_clutRbPending, m_clutRbInFlight;
    static constexpr uint32_t kRbSlots = 256u;
    unsigned m_rbBuf = 0;
    uint8_t *m_rbPtr = nullptr;
    uint32_t m_rbHead = 0;
    uint64_t m_statClutRbTaken = 0;
    void PollClutReadbacks();
    void SetClutKnown(uint32_t idx);
    // CPU-made snapshots to upload at the next flush: ring slots and their 512 words each, back to back.
    struct SnapOp
    {
        uint32_t slot;      // ring slot written
        uint32_t copyFrom;  // ~0u: whole snapshot uploaded; else GPU copy of this slot, then the loaded entries
        uint64_t loaded[8]; // entries the load wrote (copyFrom != ~0u)
    };
    std::vector<SnapOp> m_snapOps;
    std::vector<uint32_t> m_snapData; // 512 words per op, the CPU mirror after the load
    void UploadStream(unsigned buf, size_t &cap, const void *data, size_t bytes);
    void StreamTo(unsigned buf, size_t dstOffset, const void *data, size_t bytes);
    // Main-batch span cache (Draw): the last primitive's frame/Z spans, valid while m_batchGen is unchanged.
    struct SpanCache
    {
        uint64_t key = 0;
        uint32_t ff = 0, fc = 0, zf = 0, zc = 0;
        uint64_t gen = 0;
        bool ok = false;
    } m_spanCache;
    uint64_t m_batchGen = 1;  // bumped on every change to m_batchWritten/Read/Owner or a m_gpuNewer reset
    uint64_t m_spanEpoch = 0; // epoch not yet written to m_spanCache's pages (FlushPendingEpoch)
    void FlushPendingEpoch();
    // Upload ring (StreamTo): persistently mapped, GPU copies out of it, fenced per segment.
    static constexpr size_t kRingBytes = 64u * 1024u * 1024u;
    static constexpr uint32_t kRingSegments = 4u;
    unsigned m_ringBuf = 0;
    uint8_t *m_ringPtr = nullptr;
    size_t m_ringHead = 0;
    uint32_t m_ringSeg = 0;
    void *m_ringFence[kRingSegments] = {};
    uint64_t m_statRingWaits = 0;
    uint64_t m_statStreamCopies = 0, m_statSnapCopies = 0;
    // Bytes allocated in each streamed buffer: re-specified (glBufferData) only to grow, otherwise glBufferSubData.
    uint64_t m_statMtba = 0;
    // Wall time in synchronous GPU readbacks (each waits for all queued GPU work).
    double m_statReadbackSec = 0.0, m_statClutReadbackSec = 0.0;
    uint64_t m_statReadbacks = 0;
    size_t m_primCap = 0, m_stateCap = 0, m_seqCap = 0, m_imgCap = 0;
    std::vector<uint32_t> m_checkMem;
    std::array<bool, kPageCount> m_checkHas{}, m_checkTouched{}, m_checkTainted{};
    uint64_t m_statCpuPulls = 0, m_statCpuPrims = 0, m_statCpuLoads = 0, m_statClutMirrorReads = 0, m_statCpuChecked = 0, m_statCheckPages = 0,
             m_statCheckBad = 0;
    int m_seqRect[4]{};
    bool m_seqHasRect = false;
    uint32_t m_mainPrims = 0;
    std::array<bool, kPageCount> m_deferWritten{};
    unsigned m_seqProgram = 0, m_seqBuf = 0;
    uint64_t m_statDeferred = 0, m_statSeqItems = 0, m_statSeqDispatches = 0;
    std::array<bool, kPageCount> m_batchWritten{}; // pages queued draws write
    std::array<bool, kPageCount> m_batchRead{};    // pages queued draws read as texture
    std::array<uint64_t, kPageCount> m_batchOwner{}; // frame/Z configuration that writes the page

    // 3.4.7 CBP0/CBP1 internal registers. The temporary buffer is m_clutBuf on the GPU, as a ring of
    // snapshots: a load queued between draws makes the next snapshot, which later draws read.
    static constexpr uint32_t kClutSnapshots = 2048u;
    struct ClutLoad
    {
        uint32_t cbp, cpsm, csm, csa, entries, cou, cov, cbw, src, dst;
        bool Same(const ClutLoad &o) const
        {
            return cbp == o.cbp && cpsm == o.cpsm && csm == o.csm && csa == o.csa && entries == o.entries &&
                   (csm == 0u || (cou == o.cou && cov == o.cov && cbw == o.cbw));
        }
    };
    void CpuRunPrim(const GSPrimitiveBatch &batch, int bx0, int by0, int bx1, int by1, uint32_t *mem);
    void CpuClutLoad(const ClutLoad &l, const uint32_t *src, uint32_t *dst, const uint32_t *mem) const;
    template <typename Fn>
    static void ForEachClutEntry(const ClutLoad &l, Fn &&fn);
    static bool CpuCheckMode();
    void CheckPage(uint32_t pg, bool written);
    void CheckCompare();
    uint32_t m_cbp0 = 0, m_cbp1 = 0;
    uint32_t m_clutSnap = 0, m_batchFirstSnap = 0;
    std::vector<ClutLoad> m_pendingLoads;
    ClutLoad m_lastLoad{};
    bool m_haveLastLoad = false;
    uint64_t m_lastLoadEpoch = 0, m_epochCounter = 1;
    std::array<uint64_t, kPageCount> m_pageEpoch{};
    uint64_t m_statClutLoads = 0;

    // PCRTC output.
    uint32_t m_canvasW = 0, m_canvasH = 0;
    bool m_presentValid = false;
    uint64_t m_presentCount = 0;

    // Statistics and "manual silent" counters.
    mutable std::map<std::string, uint64_t> m_counts;
    mutable std::array<uint32_t, 16> m_shaderCounters{};
    uint64_t m_statDraws = 0, m_statFlushes = 0, m_statUploadPages = 0, m_statDownloadPages = 0;
    double m_statFlushCpuSec = 0.0, m_statGpuDrawSec = 0.0, m_statGpuClutSec = 0.0, m_statDrawCpuSec = 0.0;
    uint64_t m_statTransfers = 0;
    uint64_t m_statTimedFlushes = 0, m_statSwaps = 0, m_lastDispfb1 = ~0ull;
    unsigned m_timerQuery[2] = {0, 0};
};
