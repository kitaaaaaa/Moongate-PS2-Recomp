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

class GSNativeSink
{
public:
    virtual ~GSNativeSink() = default;
    // Host thread. Writes the RGBA8 texture (w x h of the target, texel (0,0) = top-left pixel) as PSMCT32
    // pixels into the target rectangle, after all drawing queued before it.
    virtual bool WriteFrameFromTexture(unsigned glTexture, const GSNativeTarget &target) = 0;
};

using GSNativeHostCall = std::function<void(GSNativeSink *sink)>;

// A write of GS register address kGSNativeMarkerReg (undefined on the GS) is not applied; the frontend calls the
// marker callback instead, with the written value and FRAME_1 (fbp, fbw, psm) at that point of the GS stream.
// Game code puts such a write into its own DMA chain to run native work exactly where that chain is processed.
constexpr uint8_t kGSNativeMarkerReg = 0x7E;
using GSNativeMarker = std::function<void(uint64_t value, uint32_t fbp, uint32_t fbw, uint32_t psm)>;
