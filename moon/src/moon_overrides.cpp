// Virtual-On (SLPM-62767) specific runtime overrides.
//
// Each override replaces one guest function by address. Addresses: docs/model2-state-map.md.

#include "game_overrides.h"
#include "moon_display.h"
#include "moon_native.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr uint32_t kMoonEntry = 0x00100134u;

    bool envOn(const char *name)
    {
        const char *v = std::getenv(name);
        return v && std::strcmp(v, "0") != 0;
    }

    // Returns to the caller without running the guest function.
    void skipFunction(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ctx->pc = GPR_U32(ctx, 31);
    }

    void applyMoonOverrides(PS2Runtime &runtime)
    {
        // PC resolution entry in the game's DISPLAY SETTING menu (moon_display.cpp); PS2X_MOON_DISPLAY_MENU=0 turns it off.
        if (const char *v = std::getenv("PS2X_MOON_DISPLAY_MENU"); !(v && std::strcmp(v, "0") == 0))
            moon_display::install(runtime);
        // Measurement aids: PS2X_MOON_NO3D=1 skips FUN_00207810 (the render-list op1 handler that adds
        // each object's VIF1 chain to the frame, so no Model 2 polygons reach VU1/GS);
        // PS2X_MOON_NOTILE=1 skips FUN_00207538 (tilemap layers -> GS sprites).
        if (envOn("PS2X_MOON_NO3D"))
            runtime.replaceFunction(0x00207810u, &skipFunction);
        if (envOn("PS2X_MOON_NOTILE"))
            runtime.replaceFunction(0x00207538u, &skipFunction);
        // PS2X_MOON_NOGEO=1 skips FUN_002093c0 (whole per-frame Model 2 polygon output: frame setup, colour
        // translation, render list -> VU1/GS, including the PS2 overlay hooks inside it).
        if (envOn("PS2X_MOON_NOGEO"))
            runtime.replaceFunction(0x002093c0u, &skipFunction);
        // Native Model 2 renderer instead of FUN_002093c0/FUN_00207538 (docs/native-model2.md): on by default
        // with the exact GS backend (the launcher's default; the only one that takes native pictures).
        // PS2X_MOON_NATIVE=0 keeps the port's own GS path.
        else
        {
            const char *native = std::getenv("PS2X_MOON_NATIVE");
            const char *backend = std::getenv("PS2X_GS_BACKEND");
            const bool exact = backend && std::strcmp(backend, "exact") == 0;
            const bool want = native ? std::strcmp(native, "0") != 0 : exact;
            if (want && exact)
                moon_native::install(runtime);
            else if (want)
                std::fprintf(stderr, "[moon-native] needs PS2X_GS_BACKEND=exact; using the port's GS path\n");
        }
    }
}

PS2_REGISTER_GAME_OVERRIDE("Virtual-On (Sega Ages 2500 Vol.31)", "SLPM_627.67", kMoonEntry, 0u, applyMoonOverrides);
