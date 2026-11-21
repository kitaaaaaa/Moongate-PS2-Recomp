// PC display options in the game's own DISPLAY SETTING menu (docs/display-menu.md).
//
// The menu (class MenuOptionDisplay) builds a generic list menu from a 7-entry table at 0x2B8BD0
// (FUN_001114d8(obj, parent, owner, table, count, line height, label width)). This module passes it a copy of
// that table with one more row, RESOLUTION, whose getter/change handlers are native functions installed at two
// guest function addresses nothing references (FUN_00103c60 / FUN_00103ca0). The choice is kept in a spare byte
// of the game's display settings record (0x18 bytes at offset 0x198 of the system file buffer), so it is saved
// and loaded with the game's other settings; INITIALIZE zeroes the record, and 0 means the default size.
// The picture's shape inside the window follows the game's SCREEN MODE (NORMAL 4:3, WIDE 16:9).
// A second new row, HUD ASPECT (WIDE / 4:3), keeps the HUD and menus at 4:3 inside the WIDE picture: the native renderer
// squeezes its HUD (moon_native.cpp) and the GS renderer the 2D drawn into the display buffers (ps2SetHiResSqueeze).
// It is kept in byte 0x0F of the record, which the game never reads (0 = WIDE, 1 = 4:3).

#include "moon_display.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <atomic>
#include <cstdio>
#include <cstring>

void ps2RequestWindowSize(int width, int height); // ps2_runtime.cpp
void ps2SetHiResSqueeze(float k);                  // gs_exact_renderer.cpp
extern "C" int GetScreenWidth(void);  // raylib: the window's client size
extern "C" int GetScreenHeight(void);
extern "C" bool IsWindowReady(void);

namespace moon_display
{
namespace
{
    constexpr uint32_t kMenuCreate = 0x001114D8u; // FUN_001114d8
    constexpr uint32_t kMenuStep = 0x00112730u;   // FUN_00112730 (list menu step, called through the vtable)
    constexpr uint32_t kLoadCallback = 0x00105418u; // system file loaded: a0 = descriptor, buffer at *(a0 + 0x10C)
    constexpr uint32_t kGetterAddr = 0x00103C60u, kChangeAddr = 0x00103CA0u; // unreferenced guest functions
    constexpr uint32_t kGameTable = 0x002B8BD0u, kEntrySize = 0x34u, kGameEntries = 7u;
    constexpr uint32_t kScreenTypeCount = 0x002B8C50u; // entry 2 count, written by absolute address by the game
    constexpr uint32_t kWideFlag = 0x00564918u;        // SCREEN MODE: 0 NORMAL, 1 WIDE
    constexpr uint32_t kRecordOffset = 0x198u, kRecordResolution = 0x14u; // spare byte: 0 default, else index + 1
    constexpr uint32_t kRecordWide = 0x11u;                              // SCREEN MODE in the record
    constexpr uint32_t kRecordHud = 0x0Fu;                               // HUD ASPECT: 0 WIDE, 1 4:3 (unused by the game)
    constexpr uint32_t kNewRow = 5u; // RESOLUTION, before INITIALIZE and EXIT
    constexpr uint32_t kHudRow = 6u; // HUD ASPECT, after it
    constexpr uint32_t kNewRows = 2u;
    // Guest memory for the new table and strings: the runtime's MPEG audio scratch (0xF0000..0x100000, MPEG.cpp),
    // used only by movie playback; this game has no movies.
    constexpr uint32_t kTable = 0x000F8000u, kStringPtrs = 0x000F8200u, kStrings = 0x000F8300u, kLabel = 0x000F82C0u;
    constexpr uint32_t kHudLabel = 0x000F82D0u, kHudPtrs = 0x000F82E0u, kHudStrings = 0x000F8600u;

    struct Size
    {
        int w, h;
    };
    // Typical PC sizes from 480p to 4K in 4:3, 16:10 and 16:9, by width then height.
    constexpr Size kSizes[] = {{640, 480},   {800, 600},   {1024, 768},  {1280, 720},  {1280, 800},  {1280, 960},  {1366, 768},
                               {1440, 900},  {1600, 900},  {1600, 1200}, {1680, 1050}, {1920, 1080}, {1920, 1200}, {1920, 1440},
                               {2560, 1440}, {2560, 1600}, {2880, 2160}, {3840, 2160}, {3840, 2400}};
    constexpr uint32_t kSizeCount = sizeof(kSizes) / sizeof(kSizes[0]);
    constexpr uint32_t kDefaultIndex = 5u; // 1280x960, the size the window opened at before this option

    PS2Runtime::RecompiledFunction g_createOriginal = nullptr, g_stepOriginal = nullptr, g_loadOriginal = nullptr, g_tickOriginal = nullptr,
                                   g_focalOriginal = nullptr;
    int g_appliedIndex = -1;
    bool g_loaded = false; // the system file (or its defaults) has set the game's flags
    int g_appliedWide = -1;
    bool g_hud43 = false;                  // HUD ASPECT 4:3
    std::atomic<float> g_hudSqueeze{1.0f}; // read by the native renderer

    uint32_t rd32(const uint8_t *rdram, uint32_t a)
    {
        uint32_t v;
        std::memcpy(&v, rdram + (a & 0x01FFFFFFu), 4);
        return v;
    }
    void wr32(uint8_t *rdram, uint32_t a, uint32_t v)
    {
        std::memcpy(rdram + (a & 0x01FFFFFFu), &v, 4);
    }

    uint32_t indexFromStored(uint8_t stored)
    {
        return (stored >= 1u && stored <= kSizeCount) ? stored - 1u : kDefaultIndex;
    }

    void applyResolution(uint32_t index)
    {
        if (static_cast<int>(index) == g_appliedIndex)
            return;
        g_appliedIndex = static_cast<int>(index);
        ps2RequestWindowSize(kSizes[index].w, kSizes[index].h);
        std::fprintf(stderr, "[moon-display] window %dx%d\n", kSizes[index].w, kSizes[index].h);
    }

    // The picture's shape. SCREEN MODE NORMAL: 4:3, with bars at the sides of a wider window. WIDE: 16:10 in a 16:10
    // window, else 16:9 (both fill a window of their shape). In WIDE mode the
    // game divides its horizontal focal length by the constant at 0x564934 (1.3333 = 16:9 over 4:3, FUN_00121780)
    // and scales two 2D placements by 0x564358 (0.75, FUN_00118370) and 0x566128 (1.3333, FUN_001a26c0), each
    // used only there; they are set to the picture's shape over 4:3, so the view widens by exactly the amount the
    // picture does and pixels stay square.
    constexpr uint32_t kWideFocalDivisor = 0x00564934u, kWideScaleA = 0x00564358u, kWideScaleB = 0x00566128u;
    float g_appliedAspect = 0.0f;

    void applyAspect(uint8_t *rdram, PS2Runtime *runtime, bool wideMode)
    {
        float aspect = 4.0f / 3.0f;
        if (wideMode)
        {
            aspect = 16.0f / 9.0f;
            if (IsWindowReady() && GetScreenHeight() > 0)
            {
                const float window = float(GetScreenWidth()) / float(GetScreenHeight());
                // 16:10 windows are filled as 16:10; everything else gets 16:9 (a window the desktop had to
                // shrink, e.g. 1600x1200 on a 1080-line monitor, must not produce an odd shape of its own).
                if (window > 1.6f * 0.98f && window < 1.6f * 1.02f)
                    aspect = 1.6f;
            }
        }
        g_appliedWide = wideMode ? 1 : 0;
        // HUD ASPECT 4:3: 2D placed for the 4:3 screen is drawn at 4:3 in the middle of the wide picture.
        const float squeeze = (wideMode && g_hud43) ? (4.0f / 3.0f) / aspect : 1.0f;
        if (squeeze != g_hudSqueeze.load())
        {
            g_hudSqueeze.store(squeeze);
            ps2SetHiResSqueeze(squeeze);
        }
        if (aspect == g_appliedAspect)
            return;
        g_appliedAspect = aspect;
        if (wideMode)
        {
            const float ratio = aspect / (4.0f / 3.0f), inverse = 1.0f / ratio;
            std::memcpy(rdram + kWideFocalDivisor, &ratio, 4);
            std::memcpy(rdram + kWideScaleA, &inverse, 4);
            std::memcpy(rdram + kWideScaleB, &ratio, 4);
        }
        runtime->setDisplayAspect(aspect);
        std::fprintf(stderr, "[moon-display] picture %.4f:1 (%s)\n", aspect, wideMode ? "WIDE" : "NORMAL");
    }

    // The display settings record of the menu's owner: *(*(menu + 0x1C) + 0x4C).
    uint32_t recordOf(const uint8_t *rdram, uint32_t menu)
    {
        const uint32_t owner = rd32(rdram, menu + 0x1Cu);
        return owner ? rd32(rdram, owner + 0x4Cu) : 0u;
    }

    // getter(menu, idx): stores the current choice index at menu + 0x2C + idx*4. Runs when the menu opens and on
    // every refresh (after INITIALIZE too), so it also applies a value that changed under it.
    // getter for the HUD ASPECT row, which shares the RESOLUTION row's handlers (they get the row index).
    void hudGetter(uint8_t *rdram, uint32_t menu, uint32_t idx)
    {
        const uint32_t record = recordOf(rdram, menu);
        const uint32_t value = (record && rdram[(record + kRecordHud) & 0x01FFFFFFu] == 1u) ? 1u : 0u;
        wr32(rdram, menu + 0x2Cu + idx * 4u, value);
        g_hud43 = value == 1u;
    }

    void resolutionGetter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = GPR_U32(ctx, 31);
        const uint32_t menu = GPR_U32(ctx, 4), idx = GPR_U32(ctx, 5);
        if (idx == kHudRow)
        {
            hudGetter(rdram, menu, idx);
            applyAspect(rdram, runtime, rd32(rdram, kWideFlag) != 0u);
            return;
        }
        const uint32_t record = recordOf(rdram, menu);
        const uint32_t index = record ? indexFromStored(rdram[(record + kRecordResolution) & 0x01FFFFFFu]) : kDefaultIndex;
        wr32(rdram, menu + 0x2Cu + idx * 4u, index);
        applyResolution(index);
    }

    // change(menu, idx, value): value = the new choice index (the entry has no value map).
    void resolutionChange(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = GPR_U32(ctx, 31);
        const uint32_t menu = GPR_U32(ctx, 4);
        if (GPR_U32(ctx, 5) == kHudRow)
        {
            g_hud43 = GPR_U32(ctx, 6) == 1u;
            if (const uint32_t record = recordOf(rdram, menu))
                rdram[(record + kRecordHud) & 0x01FFFFFFu] = g_hud43 ? 1u : 0u;
            applyAspect(rdram, runtime, rd32(rdram, kWideFlag) != 0u);
            return;
        }
        uint32_t index = GPR_U32(ctx, 6);
        if (index >= kSizeCount)
            index = kDefaultIndex;
        if (const uint32_t record = recordOf(rdram, menu))
            rdram[(record + kRecordResolution) & 0x01FFFFFFu] = static_cast<uint8_t>(index + 1u);
        applyResolution(index);
    }

    void writeString(uint8_t *rdram, uint32_t addr, const char *text)
    {
        std::memcpy(rdram + addr, text, std::strlen(text) + 1u);
    }

    void buildTable(uint8_t *rdram)
    {
        // Rows 0..4 as the game has them, the new rows, then INITIALIZE and EXIT.
        std::memcpy(rdram + kTable, rdram + kGameTable, kNewRow * kEntrySize);
        std::memcpy(rdram + kTable + (kNewRow + kNewRows) * kEntrySize, rdram + kGameTable + kNewRow * kEntrySize,
                    (kGameEntries - kNewRow) * kEntrySize);
        writeString(rdram, kLabel, "RESOLUTION");
        for (uint32_t i = 0; i < kSizeCount; ++i)
        {
            char text[16];
            std::snprintf(text, sizeof(text), "%dx%d", kSizes[i].w, kSizes[i].h);
            writeString(rdram, kStrings + i * 16u, text);
            wr32(rdram, kStringPtrs + i * 4u, kStrings + i * 16u);
        }
        const uint32_t e = kTable + kNewRow * kEntrySize;
        std::memset(rdram + e, 0, kEntrySize);
        wr32(rdram, e + 0x00u, kLabel);
        wr32(rdram, e + 0x04u, 8u);          // id: the game's handlers use 0..4, 6, 7
        wr32(rdram, e + 0x08u, 3u);          // type 3: left/right value row (button guide)
        wr32(rdram, e + 0x0Cu, kGetterAddr); // getter
        wr32(rdram, e + 0x14u, kChangeAddr); // change
        wr32(rdram, e + 0x18u, kSizeCount);
        wr32(rdram, e + 0x1Cu, kStringPtrs);

        writeString(rdram, kHudLabel, "HUD ASPECT");
        writeString(rdram, kHudStrings, "WIDE");
        writeString(rdram, kHudStrings + 16u, "4:3");
        wr32(rdram, kHudPtrs, kHudStrings);
        wr32(rdram, kHudPtrs + 4u, kHudStrings + 16u);
        const uint32_t h = kTable + kHudRow * kEntrySize;
        std::memset(rdram + h, 0, kEntrySize);
        wr32(rdram, h + 0x00u, kHudLabel);
        wr32(rdram, h + 0x04u, 9u);          // id
        wr32(rdram, h + 0x08u, 3u);          // left/right value row
        wr32(rdram, h + 0x0Cu, kGetterAddr); // getter
        wr32(rdram, h + 0x14u, kChangeAddr); // change
        wr32(rdram, h + 0x18u, 2u);
        wr32(rdram, h + 0x1Cu, kHudPtrs);
    }

    // FUN_001114d8(a0 obj, a1 parent, a2 owner, a3 table, t0 count, t1 line height, t2 label width)
    void menuCreate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (GPR_U32(ctx, 7) == kGameTable && GPR_U32(ctx, 8) == kGameEntries)
        {
            buildTable(rdram);
            SET_GPR_U32(ctx, 7, kTable);
            SET_GPR_U32(ctx, 8, kGameEntries + kNewRows);
        }
        g_createOriginal(rdram, ctx, runtime);
    }

    // FUN_00112730(a0 menu): the game writes SCREEN TYPE's choice count into its own table by absolute address
    // (3, or 1 in 480p); the copy follows it. Also the place where a SCREEN MODE change is picked up.
    void menuStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t menu = GPR_U32(ctx, 4);
        if (menu && rd32(rdram, menu + 0x20u) == kTable)
            wr32(rdram, kTable + 2u * kEntrySize + 0x18u, rd32(rdram, kScreenTypeCount));
        g_stepOriginal(rdram, ctx, runtime);
        applyAspect(rdram, runtime, rd32(rdram, kWideFlag) != 0u); // the game's own flag, set when a setting is applied
    }

    // FUN_0014dbd8 (the game's per-frame function): the window's shape can change at any time (Alt+Enter, a drag),
    // so the picture's shape follows it here.
    void frameTick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_loaded || g_appliedWide >= 0)
        {
            const float before = g_appliedAspect;
            applyAspect(rdram, runtime, g_loaded ? rd32(rdram, kWideFlag) != 0u : g_appliedWide == 1);
            if (g_appliedAspect != before && g_focalOriginal)
            {
                // FUN_00121780 recomputes the focal lengths from the constants (the game runs it when a setting
                // is applied); run it now so a window change takes effect at once. Registers are kept.
                const R5900Context saved = *ctx;
                g_focalOriginal(rdram, ctx, runtime);
                *ctx = saved;
            }
        }
        g_tickOriginal(rdram, ctx, runtime);
    }

    // FUN_00105418(a0 descriptor): the system file is already in the buffer *(a0 + 0x10C) when this runs.
    void loadCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t buffer = rd32(rdram, GPR_U32(ctx, 4) + 0x10Cu);
        const uint8_t stored = buffer ? rdram[(buffer + kRecordOffset + kRecordResolution) & 0x01FFFFFFu] : 0u;
        // The record's SCREEN MODE byte: the game's flag 0x564918 is only set from it later.
        const bool wide = buffer && rdram[(buffer + kRecordOffset + kRecordWide) & 0x01FFFFFFu] != 0u;
        g_hud43 = buffer && rdram[(buffer + kRecordOffset + kRecordHud) & 0x01FFFFFFu] == 1u;
        g_loadOriginal(rdram, ctx, runtime);
        if (stored != 0u)
            applyResolution(indexFromStored(stored));
        applyAspect(rdram, runtime, wide);
        g_loaded = true;
    }
}

float hudSqueeze()
{
    return g_hudSqueeze.load();
}

void install(PS2Runtime &runtime)
{
    g_createOriginal = runtime.lookupFunction(kMenuCreate);
    g_stepOriginal = runtime.lookupFunction(kMenuStep);
    g_loadOriginal = runtime.lookupFunction(kLoadCallback);
    if (!g_createOriginal || !g_stepOriginal || !g_loadOriginal || !runtime.hasFunction(kGetterAddr) || !runtime.hasFunction(kChangeAddr))
    {
        std::fprintf(stderr, "[moon-display] menu functions not found; display options off\n");
        return;
    }
    runtime.replaceFunction(kMenuCreate, &menuCreate);
    runtime.replaceFunction(kMenuStep, &menuStep);
    runtime.replaceFunction(kLoadCallback, &loadCallback);
    runtime.replaceFunction(kGetterAddr, &resolutionGetter);
    runtime.replaceFunction(kChangeAddr, &resolutionChange);
    g_tickOriginal = runtime.lookupFunction(0x0014DBD8u);
    g_focalOriginal = runtime.lookupFunction(0x00121780u);
    if (g_tickOriginal)
        runtime.replaceFunction(0x0014DBD8u, &frameTick);
}
}
