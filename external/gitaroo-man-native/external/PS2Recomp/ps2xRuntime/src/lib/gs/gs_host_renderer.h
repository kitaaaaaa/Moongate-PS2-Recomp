#pragma once

#include "runtime/gs/gs_types.h"
#include "runtime/gs/gs_native_layer.h"

#include <cstdint>
#include <string>

// What GSGpuBackend's host thread needs from a renderer: GSGpuRenderer (GL render targets synced
// with GS memory) or GSExactRenderer (GS memory itself on the GPU, drawn by the GS rules).
class GSHostRenderer
{
public:
    virtual ~GSHostRenderer() = default;

    virtual void SetVram(uint8_t *vram) = 0;
    virtual void BeginHostWork() = 0;
    virtual void EndHostWork() = 0;
    virtual void Release() = 0;
    virtual void Reset() = 0;

    virtual void Draw(const GSPrimitiveBatch &batch) = 0;
    virtual void FlushDraws() = 0;

    // Make GS memory current for pages [first, first + count) (or all of it).
    virtual void DownloadOverlapping(uint32_t first, uint32_t count, const char *why = "other") = 0;
    virtual void DownloadAll(const char *why = "other") = 0;
    virtual void DownloadForPresent(const GSPresentationRequest &request) = 0;
    // GS memory in [first, first + count) was written by someone else.
    virtual void InvalidatePages(uint32_t first, uint32_t count) = 0;
    virtual void InvalidateAll() = 0;

    virtual bool PresentFromGpu(const GSPresentationRequest &request) = 0;
    virtual bool HasPresentation() const = 0;
    virtual void InvalidatePresentation() = 0;
    virtual void DrawPresentation(float x, float y, float w, float h) = 0;

    virtual std::string UnsupportedSummary() const = 0;
    virtual std::string StatsSummary() const = 0;

    // A renderer that keeps its own CLUT buffer handles TEX0/TEX2 loads itself (returns true).
    virtual bool LoadClut(const GSTex0Reg &, const GSTexClutReg &) { return false; }
    // A renderer that keeps GS memory itself carries out host->local transfers (returns true from
    // BeginTransfer for those); the data then comes through UploadImage.
    virtual bool BeginTransfer(const GSTransferCommand &) { return false; }
    virtual void UploadImage(const uint8_t *, uint32_t) {}
    // Whether the backend's HLE frame clear may be used (it writes GS memory outside the GS rules).
    virtual bool AllowsHleClear() const { return true; }
    // Where native pictures go into GS memory (gs_native_layer.h); nullptr when not supported.
    virtual GSNativeSink *NativeSink() { return nullptr; }
};
