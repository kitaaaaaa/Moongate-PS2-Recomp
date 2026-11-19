#pragma once

#include <cstdint>
#include <functional>

// Lets game code draw a picture natively (OpenGL, on the GS host thread) and place it into GS memory in
// order with the guest's own GS work. GS::queueNativeHostCall runs the function on the host thread after
// everything the guest has sent to the GS so far and before anything it sends later.

// A rectangle of a frame buffer in GS memory.
struct GSNativeTarget
{
    uint32_t fbp = 0; // frame buffer base pointer (2048-word units)
    uint32_t fbw = 0; // width in 64-pixel units
    int x = 0, y = 0, w = 0, h = 0;
};

// The picture just presented: the canvas (what the window shows, one pixel per frame-buffer pixel) and where the
// top read circuit's frame buffer sits in it.
struct GSNativePresent
{
    uint32_t fbp = 0, fbw = 0; // the frame buffer shown by the top (opaque) read circuit
    int canvasW = 0, canvasH = 0;
    int originX = 0, originY = 0; // canvas pixel of frame-buffer pixel (dbx, dby) of that circuit
    int dbx = 0, dby = 0;
    bool plain = false; // true: every canvas pixel of that circuit's rectangle is that buffer's pixel, unblended
};

class GSNativeSink
{
public:
    virtual ~GSNativeSink() = default;
    // Host thread. Writes the RGBA8 texture (w x h of the target, texel (0,0) = top-left pixel) as PSMCT32
    // pixels into the target rectangle, after all drawing queued before it.
    virtual bool WriteFrameFromTexture(unsigned glTexture, const GSNativeTarget &target) = 0;
    // Host thread. The reverse: copies the target rectangle (read as PSMCT32), after all drawing queued before
    // it, into the RGBA8 texture (which must be w x h; texel (0,0) = top-left pixel).
    virtual bool ReadFrameToTexture(unsigned glTexture, const GSNativeTarget &target) = 0;
    // Called on the host thread each time a picture is presented (in GS stream order, right after the PCRTC
    // picture is made), so native code can prepare a layer of its own for that picture.
    virtual void SetPresentHook(std::function<void(GSNativeSink *sink, const GSNativePresent &present)> hook) = 0;
    // Host thread. The size of the high-resolution image the renderer keeps for frame buffer fbp (what the window
    // shows of it); false when it keeps none.
    virtual bool HiResSize(uint32_t, int &, int &) { return false; }
    // Host thread. Writes the RGBA8 texture (texW x texH, texel (0,0) = top-left) into the high-resolution image
    // of the target's frame buffer, over the target rectangle, after all drawing queued before it. GS memory is not
    // changed (write it with WriteFrameFromTexture first). False when there is no such image.
    virtual bool WriteHiResFromTexture(unsigned, int, int, const GSNativeTarget &) { return false; }
};


using GSNativeHostCall = std::function<void(GSNativeSink *sink)>;

// A write of GS register address kGSNativeMarkerReg (undefined on the GS) is not applied; the frontend calls the
// marker callback instead, with the written value and FRAME_1 (fbp, fbw, psm) at that point of the GS stream.
// Game code puts such a write into its own DMA chain to run native work exactly where that chain is processed.
constexpr uint8_t kGSNativeMarkerReg = 0x7E;
using GSNativeMarker = std::function<void(uint64_t value, uint32_t fbp, uint32_t fbw, uint32_t psm)>;
