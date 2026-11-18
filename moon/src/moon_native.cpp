// Native Model 2 polygon renderer for Virtual-On (SLPM-62767). docs/native-model2.md.
//
// Replaces FUN_002093c0 (the port's per-frame Model 2 polygon output: geo list -> VIF1/VU1/GS, colour
// tables on the GS, final 496x384 -> 512x448 copy into the back buffer). Each frame the hook snapshots
// the game's Model 2 state from EE RAM and queues a call on the GS host thread (GS::queueNativeHostCall,
// in order with the guest's GS work); that call runs the geometrizer (MAME model2_v.cpp semantics),
// draws the polygons with OpenGL and writes the picture into the back buffer (FRAME_1).
//
// Sources (all verified, docs/native-model2.md): geo command list in bufferRAM 0x332C40 (list at
// 0x342C40); polygon ROM pdtrom.bin; texture point ROM tdt0rom.bin/tdt1rom.bin; the port's texture
// headers th_hi<type><no>.bin through its key tables at 0x46D668; texture atlases (W2 pages), luma
// RAM (W2+0x1457C0), polygon palette (g_geo 0x6C8300), colour tables (0x6BC270/0x6C0270/0x6C4270).
//
// Not done yet: tilemap layers, mipmaps/microtexture, geo ops 2/0xC (direct data/translate), the PS2
// overlay hooks inside FUN_002093c0 (FUN_0020bc10, render-list ops 0xF/0x1C/0x1D/0x1E).

#include "moon_native.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_native_layer.h"

#include "external/glad.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

void ps2WaitForVif1Chain();

namespace moon_native
{
namespace
{
    // ---- EE addresses -------------------------------------------------------------------------------
    constexpr uint32_t kBufferRam = 0x00332C40u;    // geo command buffer (0xC000 words)
    constexpr uint32_t kListStart = 0x00342C40u;    // word 0x4000
    constexpr uint32_t kBufferRamWords = 0xC000u;
    constexpr uint32_t kW2Ptr = 0x0056852Cu;        // gp-0x30C4: render-state work area W2
    constexpr uint32_t kGeoState = 0x006C8300u;     // g_geo: polygon palette (1024 x RGB555) at +0
    constexpr uint32_t kColorXlat[3] = {0x006BC270u, 0x006C0270u, 0x006C4270u}; // R, G, B: 32 rows x 256 u16
    constexpr uint32_t kThKeyTables = 0x0046D668u; // [type] -> [no] -> {key = TH<<1, offset} pairs, -1 ends
    constexpr uint32_t kW2PagesA = 0x457C0u;       // pages 0/1: 8x8 tiles of 128x128 PSMT4 (FUN_00208A88)
    constexpr uint32_t kW2PagesB = 0xC57C0u;       // pages 2/3: two 512x1024 PSMT4 side by side (FUN_00208DA0)
    constexpr uint32_t kW2Luma = 0x1457C0u;        // luma rows: 256 x 128 bytes
    // Tilemap (System 24 tile chip as the port keeps it, docs/model2-state-map.md section 2)
    constexpr uint32_t kTileBlock = 0x0062FA00u;   // names, line scroll, registers, masks (MAME tile_ram, compacted)
    constexpr uint32_t kTileBlockBytes = 0xA810u;
    constexpr uint32_t kCharSheet = 0x0063A210u;   // char RAM as 8x8 tiles of 128x128 PSMT4 (like atlas A)
    constexpr uint32_t kTilePalette = 0x006BA210u; // palette RAM, 4096 x RGB555

    // ---- small helpers ---------------------------------------------------------------------------------
    inline uint32_t rd32(const uint8_t *ram, uint32_t a)
    {
        uint32_t v;
        std::memcpy(&v, ram + (a & 0x01FFFFFFu), 4);
        return v;
    }
    inline float u2f(uint32_t v)
    {
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    inline uint32_t f2u(float f)
    {
        uint32_t v;
        std::memcpy(&v, &f, 4);
        return v;
    }
    bool envOn(const char *name)
    {
        const char *v = std::getenv(name);
        return v && std::strcmp(v, "0") != 0;
    }

    // ---- disc data (BIN.PAK) -------------------------------------------------------------------------------
    struct Pak
    {
        std::ifstream file;
        std::map<std::string, std::pair<uint32_t, uint32_t>> entries; // name -> (sector, size)

        bool open(const std::string &path)
        {
            file.open(path, std::ios::binary);
            if (!file)
                return false;
            std::vector<uint8_t> head(0x8000);
            file.read(reinterpret_cast<char *>(head.data()), head.size());
            if (!file || std::memcmp(head.data(), "PAKFILE", 7) != 0)
                return false;
            const auto be = [&](size_t o) { return (uint32_t(head[o]) << 24) | (uint32_t(head[o + 1]) << 16) | (uint32_t(head[o + 2]) << 8) | head[o + 3]; };
            const uint32_t count = be(8);
            for (uint32_t i = 0; i < count && 0x50u + i * 64u <= head.size(); ++i)
            {
                const size_t e = 0x10u + i * 64u;
                std::string name(reinterpret_cast<const char *>(&head[e]), strnlen(reinterpret_cast<const char *>(&head[e]), 56));
                entries[name] = {be(e + 56), be(e + 60)};
            }
            return true;
        }
        bool read(const std::string &name, std::vector<uint8_t> &out)
        {
            auto it = entries.find(name);
            if (it == entries.end())
                return false;
            out.resize(it->second.second);
            file.clear();
            file.seekg(static_cast<std::streamoff>(it->second.first) * 2048);
            file.read(reinterpret_cast<char *>(out.data()), out.size());
            return static_cast<bool>(file);
        }
    };

    struct Roms
    {
        bool tried = false, ok = false;
        Pak pak;
        std::vector<uint32_t> pdt; // polygon ROM, words
        std::vector<uint16_t> tdt; // texture point ROM: tdt0 at 0, tdt1 at 0x400000 (u16 units)
        std::map<uint32_t, std::vector<uint8_t>> thFiles; // model id -> th_hi file

        bool load(const std::filesystem::path &dir)
        {
            tried = true;
            if (!pak.open((dir / "BIN.PAK").string()))
            {
                std::fprintf(stderr, "[moon-native] cannot open %s\n", (dir / "BIN.PAK").string().c_str());
                return false;
            }
            std::vector<uint8_t> b;
            if (!pak.read("pdtrom.bin", b))
                return false;
            pdt.assign((b.size() + 3) / 4, 0u);
            std::memcpy(pdt.data(), b.data(), b.size());
            tdt.assign(0x800000u, 0u);
            if (!pak.read("tdt0rom.bin", b))
                return false;
            std::memcpy(tdt.data(), b.data(), std::min<size_t>(b.size(), 0x800000u));
            if (!pak.read("tdt1rom.bin", b))
                return false;
            std::memcpy(tdt.data() + 0x400000u, b.data(), std::min<size_t>(b.size(), 0x800000u));
            ok = true;
            std::fprintf(stderr, "[moon-native] ROMs loaded: pdtrom %zu words\n", pdt.size());
            return true;
        }
        const std::vector<uint8_t> *thFile(uint32_t id)
        {
            auto it = thFiles.find(id);
            if (it != thFiles.end())
                return it->second.empty() ? nullptr : &it->second;
            char name[32];
            std::snprintf(name, sizeof(name), "th_hi%02x.bin", id);
            std::vector<uint8_t> b;
            if (!pak.read(name, b))
                b.clear();
            auto &slot = thFiles[id];
            slot = std::move(b);
            return slot.empty() ? nullptr : &slot;
        }
    };
    Roms g_roms;

    // ---- frame snapshot (EE thread) -> host thread ---------------------------------------------------------
    struct Polygon
    {
        float x[8], y[8], z[8], u[8], v[8]; // camera space after clipping; u/v in Model 2 texels
        int n = 0;
        uint16_t th[4]{};
        uint32_t luma = 0;
        uint32_t zval = 0;
        int window = 0;
        int viewport[4]{};
        int center[2]{};
        uint32_t order = 0;
    };

    struct Frame
    {
        std::vector<uint32_t> geo;    // bufferRAM copy
        std::vector<uint8_t> pagesA;  // 0x80000
        std::vector<uint8_t> pagesB;  // 0x80000
        std::vector<uint8_t> luma;    // 0x8000
        std::vector<uint16_t> pal;    // 1024
        std::vector<uint8_t> xlat;    // 3 x 32 x 128 (low bytes, luma 0..127)
        std::vector<uint16_t> tileWords; // kTileBlockBytes / 2
        std::vector<uint8_t> charSheet;  // 0x80000
        std::vector<uint16_t> tilePal;   // 4096
        bool haveTiles = false;
        std::vector<uint32_t> thTables; // flattened (type,no) -> list address
        GSNativeTarget target;
        // Geometry result
        std::vector<Polygon> polys;
        // Screen origin: the first (full-screen) window's top-left in Model 2 window coordinates.
        int originX = 0, originY = 0;
        bool haveOrigin = false;
        uint32_t skippedObjects = 0, unknownOps = 0, overlayOps = 0, lastUnknownOp = 0;
        uint32_t directNormalZ = 0; // *0x5671A0, the normal the port gives op 2 polygons
    };

    // ---- geometrizer (MAME model2_v.cpp: geo_* and model2_3d_process_polygon) --------------------------
    struct Vec
    {
        float x = 0, y = 0, z = 0;
    };
    struct TexParam
    {
        float diffuse = 0, ambient = 0, specScale = 0;
        uint32_t specControl = 0;
    };
    struct GeoState
    {
        float matrix[12]{};
        Vec light, focus{1, 1, 1};
        float lod = 0;
        uint32_t mode = 0;
        TexParam tex[32];
        float coef[32]{};
        // raster side
        int viewport[4]{};
        int center[4][2]{};
        struct Plane
        {
            Vec n;
            float d = 0;
        } clip[4][4];
        int window = 0;
        float polygonZ = 1e10f;
        int32_t zAdjust = 0;
    };

    inline Vec xformPoint(const Vec &p, const float *m)
    {
        return {p.x * m[0] + p.y * m[3] + p.z * m[6] + m[9], p.x * m[1] + p.y * m[4] + p.z * m[7] + m[10],
                p.x * m[2] + p.y * m[5] + p.z * m[8] + m[11]};
    }
    inline Vec xformVec(const Vec &p, const float *m)
    {
        return {p.x * m[0] + p.y * m[3] + p.z * m[6], p.x * m[1] + p.y * m[4] + p.z * m[7], p.x * m[2] + p.y * m[5] + p.z * m[8]};
    }
    inline float dot(const Vec &a, const Vec &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline Vec cross3(const Vec &v0, const Vec &v1, const Vec &v2)
    {
        const Vec p1{v1.x - v0.x, v1.y - v0.y, v1.z - v0.z}, p2{v2.x - v0.x, v2.y - v0.y, v2.z - v0.z};
        return {p1.y * p2.z - p1.z * p2.y, p1.z * p2.x - p1.x * p2.z, p1.x * p2.y - p1.y * p2.x};
    }
    inline void normalize(Vec &v)
    {
        const float n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (n != 0.0f)
        {
            const float o = 1.0f / n;
            v.x *= o, v.y *= o, v.z *= o;
        }
    }

    // MAME float_to_zval (1.8.23 float -> 4.12 float).
    uint16_t floatToZval(float f, int32_t zAdjust)
    {
        const int32_t fpint = static_cast<int32_t>(f2u(f));
        int32_t exponent = ((fpint >> 23) & 0xff) - ((zAdjust >> 23) & 0xff);
        uint32_t mantissa = fpint & 0x7fffff;
        mantissa += 0x400;
        if (mantissa > 0x7fffff)
        {
            exponent++;
            mantissa = (mantissa & 0x7fffff) >> 1;
        }
        mantissa >>= 11;
        if (fpint < 0)
            return 0;
        if (exponent < -12)
            return 0;
        if (exponent < 0)
            return static_cast<uint16_t>((mantissa | 0x1000) >> -exponent);
        if (exponent < 15)
            return static_cast<uint16_t>(((exponent + 1) << 12) | mantissa);
        return 0xffff;
    }

    struct CVert
    {
        float x, y, z, u, v;
    };
    int clipPolygon(const CVert *in, int n, CVert *out, const GeoState::Plane &pl)
    {
        int count = 0;
        float curdot = in[0].x * pl.n.x + in[0].y * pl.n.y + in[0].z * pl.n.z;
        bool curin = curdot >= pl.d;
        for (int i = 0; i < n; ++i)
        {
            const CVert &cur = in[i];
            const CVert &nxt = in[(i + 1) % n];
            if (curin)
                out[count++] = cur;
            const float nextdot = nxt.x * pl.n.x + nxt.y * pl.n.y + nxt.z * pl.n.z;
            const bool nextin = nextdot >= pl.d;
            if (curin != nextin && !std::isnan(curdot) && !std::isnan(nextdot))
            {
                const float s = (pl.d - curdot) / (nextdot - curdot);
                out[count++] = {cur.x + (nxt.x - cur.x) * s, cur.y + (nxt.y - cur.y) * s, cur.z + (nxt.z - cur.z) * s,
                                cur.u + (nxt.u - cur.u) * s, cur.v + (nxt.v - cur.v) * s};
            }
            curdot = nextdot;
            curin = nextin;
        }
        return count;
    }

    inline int sext12(uint32_t v)
    {
        v &= 0xfff;
        return (v & 0x800) ? -int(0x800 - (v & 0x7ff)) : int(v);
    }

    void setWindow(GeoState &g, const uint32_t *w)
    {
        // geo_window_data converts each word to 00XXXYYY; model2_3d_push case 0x03 decodes it.
        uint32_t c[6];
        for (int i = 0; i < 6; ++i)
            c[i] = ((w[i] & 0x0fff0000u) >> 4) | (w[i] & 0xfffu);
        g.window++;
        g.viewport[0] = sext12(c[0] >> 12);
        g.viewport[1] = sext12(c[0]);
        g.viewport[2] = sext12(c[1] >> 12);
        g.viewport[3] = sext12(c[1]);
        for (int i = 0; i < 4; ++i)
        {
            g.center[i][0] = sext12(c[2 + i] >> 12);
            g.center[i][1] = sext12(c[2 + i]);
            const float left = float(g.center[i][0] - g.viewport[0]);
            const float right = float(g.viewport[2] - g.center[i][0]);
            const float top = float(g.viewport[3] - g.center[i][1]);
            const float bottom = float(g.center[i][1] - g.viewport[1]);
            g.clip[i][0].n = {1.0f / std::hypot(1.0f, left), 0.0f, left / std::hypot(1.0f, left)};
            g.clip[i][1].n = {-1.0f / std::hypot(-1.0f, right), 0.0f, right / std::hypot(-1.0f, right)};
            g.clip[i][2].n = {0.0f, -1.0f / std::hypot(-1.0f, top), top / std::hypot(-1.0f, top)};
            g.clip[i][3].n = {0.0f, 1.0f / std::hypot(1.0f, bottom), bottom / std::hypot(1.0f, bottom)};
        }
    }

    // The port's texture header for TH of a model (FUN_001fcf48): exact key match on TH<<1 in the list of
    // (type, no); the header lives in th_hi<type><no>.bin at the listed offset.
    const uint8_t *translateTh(const uint8_t *ram, uint32_t type, uint32_t no, uint32_t th, size_t &avail)
    {
        if (type >= 13)
            return nullptr;
        const uint32_t sub = rd32(ram, kThKeyTables + type * 4u);
        if (!sub)
            return nullptr;
        uint32_t list = rd32(ram, sub + no * 4u);
        if (!list)
            return nullptr;
        const uint32_t key = th << 1;
        for (int guard = 0; guard < 65536; ++guard, list += 8u)
        {
            const uint32_t k = rd32(ram, list);
            if (k == 0xFFFFFFFFu)
                return nullptr;
            if (k == key)
            {
                const uint32_t off = rd32(ram, list + 4u);
                const std::vector<uint8_t> *f = g_roms.thFile((type << 4) | no);
                if (!f || off + 8u > f->size())
                    return nullptr;
                avail = f->size() - off;
                return f->data() + off;
            }
        }
        return nullptr;
    }

    struct Builder
    {
        Frame &fr;
        const uint8_t *ram;
        GeoState g;
        uint32_t order = 0;

        // One object (geo_object_data -> geo_parse_n?_?? -> model2_3d_push -> model2_3d_process_polygon).
        void object(uint32_t tp, uint32_t th, uint32_t pa, uint32_t pn, uint32_t typeWord, int centerSel)
        {
            if (!(pa & 0x00800000u) || (pa & 0x01000000u))
            {
                ++fr.skippedObjects; // polygon RAM (op 5 data): not used by the port's lists seen so far
                return;
            }
            objectData(tp, th, g_roms.pdt, pa & 0x3FFFFFu, pn, typeWord, centerSel);
        }

        // The port's op 2 (FUN_001f9c58 case 2): TP, TH, 6 words (two points), then records of 9 words
        // {attr, luma, distance, point, point} until attr 0. It turns them into object data {attr, normal
        // (0, 0, *0x5671A0), point, point}, translates TP/TH as model type 5 no 0 and draws it as an object.
        void directData(const std::vector<uint32_t> &buf, uint32_t &idx, int centerSel)
        {
            auto at = [&](uint32_t i) { return i < buf.size() ? buf[i] : 0u; };
            const uint32_t tp = at(idx), th = at(idx + 1);
            std::vector<uint32_t> data;
            uint32_t i = idx + 2;
            for (int k = 0; k < 6; ++k)
                data.push_back(at(i++));
            while (at(i) != 0u && i < buf.size())
            {
                data.push_back(at(i));
                data.insert(data.end(), {0u, 0u, fr.directNormalZ});
                i += 3;
                for (int k = 0; k < 6; ++k)
                    data.push_back(at(i++));
            }
            data.push_back(0u);
            idx = i + 1;
            objectData(tp, th, data, 0u, 0u, (5u << 4) | 0u, centerSel);
        }

        void objectData(uint32_t tp, uint32_t th, const std::vector<uint32_t> &rom, uint32_t base, uint32_t pn, uint32_t typeWord,
                        int centerSel)
        {
            size_t avail = 0;
            const uint8_t *thp = translateTh(ram, (typeWord >> 4) & 0xFu, typeWord & 0xFu, th, avail);
            if (!thp)
            {
                ++fr.skippedObjects;
                return;
            }
            size_t thOff = 0;
            uint32_t tpa = tp & 0x7FFFFFu;
            uint32_t count = pn ? pn : 0xfffffu;
            uint32_t k = base;
            auto word = [&](uint32_t i) -> uint32_t { return i < rom.size() ? rom[i] : 0u; };
            auto point = [&](uint32_t i) { return Vec{u2f(word(i)), u2f(word(i + 1)), u2f(word(i + 2))}; };
            const bool normals = (g.mode & 2u) == 0u;
            const bool specularOn = (g.mode & 1u) != 0u;
            // P0(n-1), P1(n-1): transformed, before focus (lighting) and after focus (projection)
            Vec q0 = xformPoint(point(k), g.matrix), q1 = xformPoint(point(k + 3), g.matrix);
            k += 6;
            Vec r0 = q0, r1 = q1; // with focus
            r0.x *= g.focus.x, r0.y *= g.focus.y, r1.x *= g.focus.x, r1.y *= g.focus.y;
            for (uint32_t i = 0; i < count && i < 0x20000u; ++i)
            {
                const uint32_t attr = word(k++);
                if ((attr & 3u) == 0u)
                    break;
                Vec normal;
                if (normals)
                    normal = xformVec(point(k), g.matrix);
                k += 3;
                const Vec p2 = xformPoint(point(k), g.matrix);
                k += 3;
                if (!normals)
                {
                    normal = cross3(q0, q1, p2);
                    normalize(normal);
                }
                const float dotl = dot(normal, g.light);
                const float dotp = dot(normal, p2);
                const uint32_t face = dotp >= 0 ? 0u : 0x100u;
                const TexParam &tpar = g.tex[(attr >> 18) & 0x1f];
                float lum = (dotl * dotp) < 0 ? 0.0f : std::fabs(dotl);
                if (specularOn)
                {
                    float spec = ((2 * dotl) * normal.z) - g.light.z;
                    if (spec < 0)
                        spec = 0;
                    if (tpar.specControl == 0)
                        spec = 0;
                    if ((tpar.specControl >> 1) != 0)
                        spec *= spec;
                    if ((tpar.specControl >> 2) != 0)
                        spec *= spec;
                    if (((tpar.specControl + 1) >> 3) != 0)
                        spec *= spec;
                    lum = lum * tpar.diffuse + tpar.ambient + spec * tpar.specScale;
                }
                else
                    lum = lum * tpar.diffuse + tpar.ambient;
                lum = std::clamp(lum, 0.0f, 255.0f);
                const uint32_t luma = static_cast<uint32_t>(static_cast<int32_t>(lum)) + face;
                Vec p3 = p2;
                const bool quad = (attr & 1u) != 0u;
                if (quad)
                    p3 = xformPoint(point(k), g.matrix);
                k += 3;
                Vec r2 = p2, r3 = p3;
                r2.x *= g.focus.x, r2.y *= g.focus.y, r3.x *= g.focus.x, r3.y *= g.focus.y;

                // model2_3d_process_polygon: v[1] = P0(n-1), v[0] = P1(n-1), v[2] = P0(n), v[3] = P1(n)
                CVert v[4] = {{r1.x, r1.y, r1.z, 0, 0}, {r0.x, r0.y, r0.z, 0, 0}, {r2.x, r2.y, r2.z, 0, 0}, {r3.x, r3.y, r3.z, 0, 0}};
                const int nv = quad ? 4 : 3;
                for (int j = 0; j < nv; ++j)
                {
                    const uint32_t a = (tpa + 2u * j) & 0x7FFFFFu;
                    v[j].v = float(g_roms.tdt[a]) * 0.125f;
                    v[j].u = float(g_roms.tdt[(a + 1u) & 0x7FFFFFu]) * 0.125f;
                }
                tpa += nv * 2u;
                uint16_t hdr[4] = {0, 0, 0, 0};
                if (thOff + 8u <= avail)
                    std::memcpy(hdr, thp + thOff, 8);
                {
                    int32_t tho = static_cast<int32_t>((attr >> 12) & 0x1f);
                    if (tho & 0x10)
                        tho -= 0x20;
                    thOff = static_cast<size_t>(static_cast<int64_t>(thOff) + tho * 8);
                }
                float minz = std::min({v[0].z, v[1].z, v[2].z}), maxz = std::max({v[0].z, v[1].z, v[2].z});
                if (nv == 4)
                    minz = std::min(minz, v[3].z), maxz = std::max(maxz, v[3].z);
                bool cull = false;
                if (((attr >> 17) & 1u) == 0u && (luma & 0x100u))
                    cull = true;
                if (((attr >> 8) & 3u) == 0u)
                    cull = true;
                if (maxz < 0)
                    cull = true;
                float zvalue;
                switch ((attr >> 10) & 3u)
                {
                case 0: zvalue = g.polygonZ; break;
                case 1: zvalue = minz; break;
                case 2: zvalue = maxz; break;
                default: zvalue = 1e10f; break;
                }
                g.polygonZ = zvalue;
                if (!cull)
                {
                    CVert a[8], b[8];
                    int n = nv;
                    for (int j = 0; j < nv; ++j)
                        a[j] = v[j];
                    for (int pl = 0; pl < 4 && n > 0; ++pl)
                    {
                        n = clipPolygon(a, n, b, g.clip[centerSel][pl]);
                        for (int j = 0; j < n; ++j)
                            a[j] = b[j];
                    }
                    if (n > 2)
                    {
                        Polygon P;
                        P.n = n;
                        for (int j = 0; j < n; ++j)
                            P.x[j] = a[j].x, P.y[j] = a[j].y, P.z[j] = a[j].z, P.u[j] = a[j].u, P.v[j] = a[j].v;
                        std::memcpy(P.th, hdr, 8);
                        P.luma = (luma & 0xffu);
                        P.zval = floatToZval(zvalue, g.zAdjust);
                        P.window = g.window;
                        std::memcpy(P.viewport, g.viewport, sizeof(P.viewport));
                        P.center[0] = g.center[centerSel][0];
                        P.center[1] = g.center[centerSel][1];
                        P.order = order++;
                        fr.polys.push_back(P);
                    }
                }
                // linking
                switch ((attr >> 8) & 3u)
                {
                case 0:
                case 2:
                    q0 = p2, q1 = p3, r0 = r2, r1 = r3;
                    break;
                case 1:
                    q1 = p2, r1 = r2; // P0(n) -> P1(n-1)
                    break;
                case 3:
                    q0 = p3, r0 = r3; // P1(n) -> P0(n-1)
                    break;
                }
                (void)q0;
            }
        }

        void run()
        {
            const std::vector<uint32_t> &buf = fr.geo;
            uint32_t idx = (kListStart - kBufferRam) / 4u;
            for (int ops = 0; ops < 0x8000 && idx < buf.size(); ++ops)
            {
                const uint32_t opw = buf[idx++];
                if (opw & 0x80000000u)
                {
                    idx = (opw & 0x1ffffu) / 4u;
                    continue;
                }
                const uint32_t op = (opw >> 23) & 0x1fu;
                auto arg = [&](uint32_t i) { return idx + i < buf.size() ? buf[idx + i] : 0u; };
                switch (op)
                {
                case 0x00: break;
                case 0x02:
                    directData(buf, idx, static_cast<int>((opw >> 23 >> 6) & 3u));
                    break;
                case 0x01:
                    object(arg(0), arg(1), arg(2), arg(3), arg(4), static_cast<int>((opw >> 23 >> 6) & 3u));
                    idx += 5;
                    break;
                case 0x03:
                {
                    uint32_t w[6];
                    for (int i = 0; i < 6; ++i)
                        w[i] = arg(i);
                    setWindow(g, w);
                    if (!fr.haveOrigin)
                    {
                        fr.haveOrigin = true;
                        fr.originX = g.viewport[0];
                        fr.originY = 384 - g.viewport[3];
                    }
                    idx += 6;
                    break;
                }
                case 0x06:
                {
                    uint32_t index = arg(0) >> 2;
                    const uint32_t count = arg(1);
                    idx += 2;
                    for (uint32_t i = 0; i < count && i < 64u; ++i)
                    {
                        const uint32_t p = arg(0);
                        g.tex[index].diffuse = float(p & 0xff);
                        g.tex[index].ambient = float((p >> 8) & 0xff);
                        g.tex[index].specControl = (p >> 24) & 0xff;
                        g.tex[index].specScale = float((p >> 16) & 0xff);
                        g.coef[index] = u2f(arg(1));
                        idx += 2;
                        index = (index + 1) & 0x1f;
                    }
                    break;
                }
                case 0x07: g.mode = arg(0); idx += 1; break;
                case 0x08: g.zAdjust = static_cast<int32_t>((arg(0) >> 8) << 8); idx += 1; break;
                case 0x09: g.focus.x = u2f(arg(0)); g.focus.y = u2f(arg(1)); idx += 2; break;
                case 0x0A: g.light = {u2f(arg(0)), u2f(arg(1)), u2f(arg(2))}; idx += 3; break;
                case 0x0B:
                    for (int i = 0; i < 12; ++i)
                        g.matrix[i] = u2f(arg(i));
                    idx += 12;
                    break;
                case 0x10: idx += 1; break;
                case 0x16: g.lod = u2f(arg(0)); idx += 1; break;
                case 0x1B: idx += 2; break; // port op: VU1 0x136 (2 words), no Model 2 meaning used here
                case 0x1C: ++fr.overlayOps; break; // port op: FUN_0020C1E8 (PS2 overlay), no words; not drawn yet
                case 0x0F:
                case 0x1F:
                    return;
                default:
                    ++fr.unknownOps; // the port's parser spins on these ('Undefined Command'); stop here
                    fr.lastUnknownOp = opw;
                    return;
                }
            }
        }
    };

    // ---- GL side (host thread) ---------------------------------------------------------------------------
    const char *kVs = R"GLSL(#version 430
layout(location = 0) in vec4 aPos;   // clip-space position (w = camera z)
layout(location = 1) in vec2 aUv;    // Model 2 texels
layout(location = 2) in float aPoly;
out vec2 vUv;
flat out int vPoly;
void main()
{
    gl_Position = aPos;
    vUv = aUv;
    vPoly = int(aPoly);
}
)GLSL";

    const char *kFs = R"GLSL(#version 430
in vec2 vUv;
flat in int vPoly;
out vec4 oColor;
layout(std430, binding = 3) readonly buffer Polys { uvec4 polys[]; }; // 2 per polygon
uniform usampler2D uAtlasA, uAtlasB, uLuma, uXlat, uPal;
uniform vec2 uScale;    // target pixels per Model 2 pixel
uniform bool uGamma;

uint lerp2(uint x, uint y, uint a)
{
    return (x + (((y - x) * a) >> 8u)) & 0x00ff00ffu;
}

uint texel(bool b, int x, int y)
{
    return b ? texelFetch(uAtlasB, ivec2(x, y), 0).r : texelFetch(uAtlasA, ivec2(x, y), 0).r;
}

// MAME fetch_bilinear_texel on the port's atlas (half resolution unless the port's double bits are set).
uint bilinear(uvec4 h0, uvec4 h1, bool translucent, out bool keep)
{
    uint th0 = h0.x, th1 = h0.y, th2 = h0.z;
    int w = 32 << int(th0 & 7u), h = 32 << int((th0 >> 3u) & 7u);
    bool wrapx = ((th0 >> 6u) & 1u) != 0u, wrapy = ((th0 >> 7u) & 1u) != 0u;
    bool mirx = ((th0 >> 8u) & 1u) != 0u, miry = ((th0 >> 9u) & 1u) != 0u;
    wrapx = wrapx && !mirx;
    wrapy = wrapy && !miry;
    int dbx = int((th1 >> 8u) & 1u), dby = int((th1 >> 9u) & 1u);
    bool atlasB = ((th1 >> 10u) & 1u) != 0u;
    int texx = int(th2 & 0x3fu) * 32, texy = int((th2 >> 6u) & 0x3fu) * 32;
    if (texx < 0x400) { if (texy > 0x3ff) texy -= 0x400; } else texx -= 0x400;
    int ox = ((texx << dbx) + (((th2 & 0x1000u) != 0u) ? 0x400 : 0)) >> 1;
    int oy = (texy << dby) >> 1;
    // Model 2 texel coordinates with 8 fractional bits
    int u = int(floor(vUv.x * 256.0)), v = int(floor(vUv.y * 256.0));
    if (mirx && (u & (w << 8)) != 0) u = ~u;
    if (miry && (v & (h << 8)) != 0) v = ~v;
    // to atlas units
    int aw = dbx != 0 ? w : w >> 1, ah = dby != 0 ? h : h >> 1;
    if (dbx == 0) u >>= 1;
    if (dby == 0) v >>= 1;
    u -= 0x80; v -= 0x80;
    int uf = u & 0xff, vf = v & 0xff;
    int u0 = (u >> 8) & (aw - 1), u1 = (u0 + 1) & (aw - 1);
    int v0 = (v >> 8) & (ah - 1), v1 = (v0 + 1) & (ah - 1);
    if (!wrapx && u1 == 0) { if (uf >= 0x80) { u0 = u1; u1++; uf = 0; } else { u1 = u0; u0--; uf = 0x100; } }
    if (!wrapy && v1 == 0) { if (vf >= 0x80) { v0 = 0; v1++; vf = 0; } else { v1 = v0; v0--; vf = 0x100; } }
    uint t00 = texel(atlasB, ox + u0, oy + v0) << 4u, t01 = texel(atlasB, ox + u1, oy + v0) << 4u;
    uint t10 = texel(atlasB, ox + u0, oy + v1) << 4u, t11 = texel(atlasB, ox + u1, oy + v1) << 4u;
    // From here MAME's code as is: two lanes (texel low, alpha 0x80 at bit 16 when translucent) in a u32.
    if (translucent)
    {
        if (t00 != 0xf0u) t00 |= 0x00800000u;
        if (t01 != 0xf0u) t01 |= 0x00800000u;
        if (t10 != 0xf0u) t10 |= 0x00800000u;
        if (t11 != 0xf0u) t11 |= 0x00800000u;
        if (t00 == 0x000000f0u) t00 = t01 & 0xffu;
        if (t01 == 0x000000f0u) t01 = t00 & 0xffu;
        if (t10 == 0x000000f0u) t10 = t11 & 0xffu;
        if (t11 == 0x000000f0u) t11 = t10 & 0xffu;
    }
    uint t0x = lerp2(t00, t01, uint(uf)), t1x = lerp2(t10, t11, uint(uf));
    if (translucent)
    {
        if (t0x == 0x000000f0u) t0x = t1x & 0xffu;
        if (t1x == 0x000000f0u) t1x = t0x & 0xffu;
    }
    uint t = lerp2(t0x, t1x, uint(vf));
    keep = true;
    if (translucent)
    {
        keep = t >= 0x00400000u; // alpha below 50%: discarded
        t &= 0xffu;
    }
    return t & 0xffu;
}

uint xlat(int ch, uint color, uint luma)
{
    return texelFetch(uXlat, ivec2(int(luma), ch * 32 + int((color >> uint(ch * 5)) & 31u)), 0).r;
}

void main()
{
    uvec4 h0 = polys[vPoly * 2], h1 = polys[vPoly * 2 + 1];
    // h0: th0..th3; h1: luma, viewport (x0,y0) packed, viewport (x1,y1) packed, unused
    ivec2 p = ivec2(gl_FragCoord.xy / uScale); // Model 2 pixel
    int vx0 = int(h1.y & 0xffffu) - 32768, vy0 = int(h1.y >> 16u) - 32768;
    int vx1 = int(h1.z & 0xffffu) - 32768, vy1 = int(h1.z >> 16u) - 32768;
    if (p.x < vx0 || p.x > vx1 || p.y < vy0 || p.y > vy1)
        discard;
    uint th0 = h0.x;
    bool checker = ((th0 >> 15u) & 1u) != 0u;
    ivec2 tp = ivec2(gl_FragCoord.xy);
    if (checker && ((tp.x ^ tp.y) & 1) == 0)
        discard;
    uint renderer = (th0 >> 13u) & 3u; // bit 1 textured, bit 0 translucent
    uint colorbase = (h0.w >> 6u) & 0x3ffu;
    uint pal = texelFetch(uPal, ivec2(int(colorbase), 0), 0).r;
    uint luma;
    if ((renderer & 2u) == 0u)
    {
        if ((renderer & 1u) != 0u)
            discard;
        luma = h1.x >> 2u;
    }
    else
    {
        bool keep;
        uint t = bilinear(h0, h1, (renderer & 1u) != 0u, keep);
        if (!keep)
            discard;
        uint lumabase = (h0.y & 0xffu);
        uint lr = texelFetch(uLuma, ivec2(int(t >> 1u), int(lumabase)), 0).r;
        luma = min((lr * h1.x) / 256u, 0x3fu);
        pal &= 0x7fffu;
    }
    vec3 c = vec3(xlat(0, pal, luma), xlat(1, pal, luma), xlat(2, pal, luma));
    if (uGamma)
        c = max((c - 64.0) * 255.0 / 191.0, 0.0);
    oColor = vec4(c / 255.0, 128.0 / 255.0);
}
)GLSL";

    const char *kTileVs = R"GLSL(#version 430
void main()
{
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

    // Copies the persistent polygon layer over the low tiles: only pixels some frame's polygons have written
    // (alpha != 0; the polygon shader writes alpha 128/255).
    const char *kPolyCopyFs = R"GLSL(#version 430
uniform sampler2D uPoly;
out vec4 oColor;
void main()
{
    vec4 c = texelFetch(uPoly, ivec2(gl_FragCoord.xy), 0);
    if (c.a == 0.0)
        discard;
    oColor = c;
}
)GLSL";

    // MAME segaic24 draw_common/draw_rect and model2 screen_update's layer order, per output pixel.
    const char *kTileFs = R"GLSL(#version 430
out vec4 oColor;
layout(std430, binding = 4) readonly buffer Tiles { uint tw[]; };
uniform usampler2D uSheet, uTilePal, uXlat;
uniform vec2 uScale;
uniform bool uHigh, uGamma;
const uint NONE = 0xffffffffu;
uint rd16(int i)
{
    uint w = tw[i >> 1];
    return (i & 1) != 0 ? (w >> 16u) : (w & 0xffffu);
}
uint tilePixel(int L, int sx, int sy, uint tpri, bool opaque)
{
    uint name = rd16(L * 0x1000 + ((sy >> 3) & 63) * 64 + ((sx >> 3) & 63));
    uint c = name & 0x3fffu;
    uint pen = texelFetch(uSheet, ivec2(int(c >> 7u) * 8 + (sx & 7), int(c & 127u) * 8 + (sy & 7)), 0).r;
    if (!opaque && (pen == 0u || (name >> 15u) != tpri))
        return NONE;
    return ((name >> 7u) & 0xffu) * 16u + pen;
}
uint layerPixel(int L, int x, int y, uint tpri, bool opaque)
{
    uint hscr = rd16(0x4800 + L), vscr = rd16(0x4804 + L), ctrl = rd16(0x4804 + (L & 2));
    if ((vscr & 0x8000u) != 0u)
        return NONE;
    if ((ctrl & 0x6000u) != 0u)
    {
        if ((L & 1) != 0)
            return NONE;
        uint mode = (ctrl & 0x6000u) >> 13u;
        int vs = int(vscr & 0x1ffu);
        uint hv = ((hscr & 0x8000u) != 0u) ? rd16(0x4000 + 0x200 * L + y) : hscr;
        int h = int(hv & 0x1ffu);
        int l = L;
        if (mode == 1u)
        {
            uint nv = (0u - vscr) & 0x1ffu;
            if (((0u - vscr) & 0x200u) == 0u)
                l ^= 1;
            if (uint(y) >= nv)
                l ^= 1;
        }
        else
        {
            if ((hv & 0x200u) == 0u)
                l ^= 1;
            if (x >= h)
                l ^= 1;
        }
        return tilePixel(l, (x - h) & 511, (y + vs) & 511, tpri, opaque);
    }
    int sx;
    if ((hscr & 0x8000u) != 0u)
        sx = int((0u - rd16(0x4000 + 0x200 * L + y)) & 0x1ffu);
    else
        sx = int((0u - hscr) & 0x1ffu);
    int sy = int(vscr & 0x1ffu) + y;
    uint m = rd16((L >= 2 ? 0x4E08 : 0x4808) + y * 4 + (x >> 7));
    if ((L & 1) != 0)
        m = ~m & 0xffffu;
    if ((m & (0x8000u >> uint((x & 127) >> 3))) != 0u)
        return NONE;
    return tilePixel(L, (x + sx) & 511, sy & 511, tpri, opaque);
}
uint xl(int ch, uint c)
{
    return texelFetch(uXlat, ivec2(0x40, ch * 32 + int((c >> uint(ch * 5)) & 31u)), 0).r;
}
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy / uScale);
    if (p.x >= 496 || p.y >= 384)
        discard;
    uint v = 0u;
    if (!uHigh)
    {
        for (int L = 3; L >= 2; --L) { uint r = layerPixel(L, p.x, p.y, 0u, true); if (r != NONE) v = r; }
        for (int L = 1; L >= 0; --L) { uint r = layerPixel(L, p.x, p.y, 0u, false); if (r != NONE) v = r; }
    }
    else
    {
        for (int L = 3; L >= 0; --L) { uint r = layerPixel(L, p.x, p.y, 1u, false); if (r != NONE) v = r; }
        if (v == 0u)
            discard;
    }
    uint pal = texelFetch(uTilePal, ivec2(int(v), 0), 0).r; // v == 0: the background (pen 0)
    vec3 c = vec3(xl(0, pal), xl(1, pal), xl(2, pal));
    if (uGamma)
        c = max((c - 64.0) * 255.0 / 191.0, 0.0);
    oColor = vec4(c / 255.0, 128.0 / 255.0);
}
)GLSL";

    struct Gl
    {
        GLuint tileProg = 0, tileVao = 0, tileBuf = 0, sheet = 0, tilePal = 0;
        GLint tSheet = -1, tPal = -1, tXlat = -1, tScale = -1, tHigh = -1, tGamma = -1;
        std::vector<uint8_t> linSheet;
        bool ready = false, failed = false;
        GLuint prog = 0, vao = 0, vbo = 0, polyBuf = 0, fbo = 0, target = 0;
        GLuint polyFbo = 0, polyTex = 0, copyProg = 0; // persistent polygon layer (see render)
        GLint cPoly = -1;
        GLuint atlasA = 0, atlasB = 0, luma = 0, xlat = 0, pal = 0;
        int tw = 0, th = 0;
        GLint uAtlasA = -1, uAtlasB = -1, uLuma = -1, uXlat = -1, uPal = -1, uScale = -1, uGamma = -1;
        std::vector<uint8_t> linA, linB;
        std::vector<uint8_t> lastA, lastB, lastSheet; // EE bytes of the last upload (skip unchanged)
    };
    Gl g_gl;

    GLuint compileShader(GLenum type, const char *src)
    {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[4096];
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            std::fprintf(stderr, "[moon-native] shader error: %s\n", log);
            glDeleteShader(s);
            return 0;
        }
        return s;
    }

    GLuint makeTex(GLenum internal, int w, int h, GLenum format, GLenum type)
    {
        GLuint t;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        return t;
    }

    bool ensureGl(int w, int h)
    {
        Gl &G = g_gl;
        if (G.failed)
            return false;
        if (!G.ready)
        {
            GLuint vs = compileShader(GL_VERTEX_SHADER, kVs), fs = compileShader(GL_FRAGMENT_SHADER, kFs);
            if (!vs || !fs)
            {
                G.failed = true;
                return false;
            }
            G.prog = glCreateProgram();
            glAttachShader(G.prog, vs);
            glAttachShader(G.prog, fs);
            glLinkProgram(G.prog);
            glDeleteShader(vs);
            glDeleteShader(fs);
            GLint ok = 0;
            glGetProgramiv(G.prog, GL_LINK_STATUS, &ok);
            if (!ok)
            {
                std::fprintf(stderr, "[moon-native] program link failed\n");
                G.failed = true;
                return false;
            }
            G.uAtlasA = glGetUniformLocation(G.prog, "uAtlasA");
            G.uAtlasB = glGetUniformLocation(G.prog, "uAtlasB");
            G.uLuma = glGetUniformLocation(G.prog, "uLuma");
            G.uXlat = glGetUniformLocation(G.prog, "uXlat");
            G.uPal = glGetUniformLocation(G.prog, "uPal");
            G.uScale = glGetUniformLocation(G.prog, "uScale");
            G.uGamma = glGetUniformLocation(G.prog, "uGamma");
            {
                GLuint tvs = compileShader(GL_VERTEX_SHADER, kTileVs), tfs = compileShader(GL_FRAGMENT_SHADER, kTileFs);
                if (!tvs || !tfs)
                {
                    G.failed = true;
                    return false;
                }
                G.tileProg = glCreateProgram();
                glAttachShader(G.tileProg, tvs);
                glAttachShader(G.tileProg, tfs);
                glLinkProgram(G.tileProg);
                glDeleteShader(tvs);
                glDeleteShader(tfs);
                GLint tok = 0;
                glGetProgramiv(G.tileProg, GL_LINK_STATUS, &tok);
                if (!tok)
                {
                    std::fprintf(stderr, "[moon-native] tile program link failed\n");
                    G.failed = true;
                    return false;
                }
                G.tSheet = glGetUniformLocation(G.tileProg, "uSheet");
                G.tPal = glGetUniformLocation(G.tileProg, "uTilePal");
                G.tXlat = glGetUniformLocation(G.tileProg, "uXlat");
                G.tScale = glGetUniformLocation(G.tileProg, "uScale");
                G.tHigh = glGetUniformLocation(G.tileProg, "uHigh");
                G.tGamma = glGetUniformLocation(G.tileProg, "uGamma");
                glGenVertexArrays(1, &G.tileVao);
                glGenBuffers(1, &G.tileBuf);
            }
            glGenVertexArrays(1, &G.vao);
            glGenBuffers(1, &G.vbo);
            glGenBuffers(1, &G.polyBuf);
            glBindVertexArray(G.vao);
            glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 28, reinterpret_cast<void *>(0));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 28, reinterpret_cast<void *>(16));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 28, reinterpret_cast<void *>(24));
            glBindVertexArray(0);
            G.atlasA = makeTex(GL_R8UI, 1024, 1024, GL_RED_INTEGER, GL_UNSIGNED_BYTE);
            G.atlasB = makeTex(GL_R8UI, 1024, 1024, GL_RED_INTEGER, GL_UNSIGNED_BYTE);
            G.luma = makeTex(GL_R8UI, 128, 256, GL_RED_INTEGER, GL_UNSIGNED_BYTE);
            G.xlat = makeTex(GL_R8UI, 128, 96, GL_RED_INTEGER, GL_UNSIGNED_BYTE);
            G.pal = makeTex(GL_R16UI, 1024, 1, GL_RED_INTEGER, GL_UNSIGNED_SHORT);
            G.sheet = makeTex(GL_R8UI, 1024, 1024, GL_RED_INTEGER, GL_UNSIGNED_BYTE);
            G.tilePal = makeTex(GL_R16UI, 4096, 1, GL_RED_INTEGER, GL_UNSIGNED_SHORT);
            glGenFramebuffers(1, &G.fbo);
            glGenFramebuffers(1, &G.polyFbo);
            {
                GLuint cvs = compileShader(GL_VERTEX_SHADER, kTileVs), cfs = compileShader(GL_FRAGMENT_SHADER, kPolyCopyFs);
                if (!cvs || !cfs)
                {
                    G.failed = true;
                    return false;
                }
                G.copyProg = glCreateProgram();
                glAttachShader(G.copyProg, cvs);
                glAttachShader(G.copyProg, cfs);
                glLinkProgram(G.copyProg);
                glDeleteShader(cvs);
                glDeleteShader(cfs);
                GLint cok = 0;
                glGetProgramiv(G.copyProg, GL_LINK_STATUS, &cok);
                if (!cok)
                {
                    std::fprintf(stderr, "[moon-native] polygon copy program link failed\n");
                    G.failed = true;
                    return false;
                }
                G.cPoly = glGetUniformLocation(G.copyProg, "uPoly");
            }
            G.ready = true;
        }
        if (G.tw != w || G.th != h)
        {
            if (G.target)
                glDeleteTextures(1, &G.target);
            G.target = makeTex(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
            glBindFramebuffer(GL_FRAMEBUFFER, G.fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, G.target, 0);
            if (G.polyTex)
                glDeleteTextures(1, &G.polyTex);
            G.polyTex = makeTex(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
            glBindFramebuffer(GL_FRAMEBUFFER, G.polyFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, G.polyTex, 0);
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT); // empty until polygons are drawn
            G.tw = w, G.th = h;
        }
        return true;
    }

    void upload(GLuint tex, int w, int h, GLenum format, GLenum type, const void *data)
    {
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, format, type, data);
    }

    void tilePass(bool high, float sx, float sy)
    {
        Gl &G = g_gl;
        glUseProgram(G.tileProg);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, G.tileBuf);
        const GLuint texs[3] = {G.sheet, G.tilePal, G.xlat};
        const GLint locs[3] = {G.tSheet, G.tPal, G.tXlat};
        for (int t = 0; t < 3; ++t)
        {
            glActiveTexture(GL_TEXTURE1 + t);
            glBindTexture(GL_TEXTURE_2D, texs[t]);
            glUniform1i(locs[t], 1 + t);
        }
        glUniform2f(G.tScale, sx, sy);
        glUniform1i(G.tHigh, high ? 1 : 0);
        static const bool s_gamma = envOn("PS2X_MOON_GAMMA");
        glUniform1i(G.tGamma, s_gamma ? 1 : 0);
        glBindVertexArray(G.tileVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        for (int t = 0; t < 3; ++t)
        {
            glActiveTexture(GL_TEXTURE1 + t);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }

    std::atomic<uint64_t> g_hostNs{0}, g_eeNs{0}; // time in render() (host) and in frameHook (EE)
    std::atomic<uint64_t> g_waitNs{0};             // part of g_eeNs spent in ps2WaitForVif1Chain
    inline uint64_t nsSince(std::chrono::steady_clock::time_point t0)
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    }

    // Model 2 screen (496 x 384) -> target pixels.
    constexpr float kM2W = 496.0f, kM2H = 384.0f;

    void render(Frame &fr, GSNativeSink *sink)
    {
        if (!sink)
            return;
        const auto t0 = std::chrono::steady_clock::now();
        const int W = fr.target.w, H = fr.target.h;
        // Save the GL state we touch (raylib caches its own).
        GLint prevFbo, prevProg, prevVao, prevVp[4], prevTex, prevActive, prevArray;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
        glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
        glGetIntegerv(GL_VIEWPORT, prevVp);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArray);
        const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cullf = glIsEnabled(GL_CULL_FACE),
                        scis = glIsEnabled(GL_SCISSOR_TEST);
        if (!ensureGl(W, H))
            return;
        Gl &G = g_gl;
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);

        // Atlases: the port's EE layouts -> linear 1024 x 1024 4-bit indices (one byte each); only when changed.
        const bool newA = G.lastA != fr.pagesA, newB = G.lastB != fr.pagesB;
        G.linA.resize(1024u * 1024u);
        G.linB.resize(1024u * 1024u);
        for (uint32_t y = 0; y < 1024u && (newA || newB); ++y)
        {
            const uint8_t *rowA = fr.pagesA.data() + (y >> 7) * 0x10000u + (y & 127u) * 64u;
            const uint8_t *rowB0 = fr.pagesB.data() + y * 256u, *rowB1 = fr.pagesB.data() + 0x40000u + y * 256u;
            uint8_t *dA = G.linA.data() + y * 1024u, *dB = G.linB.data() + y * 1024u;
            for (uint32_t x = 0; x < 1024u; x += 2)
            {
                const uint8_t a = rowA[(x >> 7) * 0x2000u + ((x & 127u) >> 1)];
                dA[x] = a & 15u, dA[x + 1] = a >> 4;
                const uint8_t b = (x < 512u) ? rowB0[x >> 1] : rowB1[(x - 512u) >> 1];
                dB[x] = b & 15u, dB[x + 1] = b >> 4;
            }
        }
        if (newA)
        {
            upload(G.atlasA, 1024, 1024, GL_RED_INTEGER, GL_UNSIGNED_BYTE, G.linA.data());
            G.lastA = fr.pagesA;
        }
        if (newB)
        {
            upload(G.atlasB, 1024, 1024, GL_RED_INTEGER, GL_UNSIGNED_BYTE, G.linB.data());
            G.lastB = fr.pagesB;
        }
        upload(G.luma, 128, 256, GL_RED_INTEGER, GL_UNSIGNED_BYTE, fr.luma.data());
        upload(G.xlat, 128, 96, GL_RED_INTEGER, GL_UNSIGNED_BYTE, fr.xlat.data());
        upload(G.pal, 1024, 1, GL_RED_INTEGER, GL_UNSIGNED_SHORT, fr.pal.data());
        const bool newSheet = fr.haveTiles && G.lastSheet != fr.charSheet;
        if (fr.haveTiles)
        {
            G.linSheet.resize(1024u * 1024u);
            for (uint32_t y = 0; y < 1024u && newSheet; ++y)
            {
                const uint8_t *row = fr.charSheet.data() + (y >> 7) * 0x10000u + (y & 127u) * 64u;
                uint8_t *d = G.linSheet.data() + y * 1024u;
                for (uint32_t x = 0; x < 1024u; x += 2)
                {
                    const uint8_t a = row[(x >> 7) * 0x2000u + ((x & 127u) >> 1)];
                    d[x] = a & 15u, d[x + 1] = a >> 4;
                }
            }
            if (newSheet)
            {
                upload(G.sheet, 1024, 1024, GL_RED_INTEGER, GL_UNSIGNED_BYTE, G.linSheet.data());
                G.lastSheet = fr.charSheet;
            }
            upload(G.tilePal, 4096, 1, GL_RED_INTEGER, GL_UNSIGNED_SHORT, fr.tilePal.data());
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, G.tileBuf);
            glBufferData(GL_SHADER_STORAGE_BUFFER, fr.tileWords.size() * 2, fr.tileWords.data(), GL_STREAM_DRAW);
        }

        // Draw order: MAME renders windows from the last to 0, z buckets ascending, newest first in a
        // bucket, the first write to a pixel winning. Here the last write wins, so the order is reversed.
        std::vector<uint32_t> order(fr.polys.size());
        for (uint32_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            const Polygon &A = fr.polys[a], &B = fr.polys[b];
            if (A.window != B.window)
                return A.window < B.window;
            if (A.zval != B.zval)
                return A.zval > B.zval;
            return A.order < B.order;
        });
        const float sx = float(W) / kM2W, sy = float(H) / kM2H;
        std::vector<float> verts;
        std::vector<uint32_t> pdata;
        verts.reserve(fr.polys.size() * 6 * 3 * 7);
        pdata.reserve(fr.polys.size() * 8);
        uint32_t pi = 0;
        for (uint32_t idx : order)
        {
            const Polygon &P = fr.polys[idx];
            const int ox = fr.originX, oy = fr.originY;
            pdata.insert(pdata.end(), {P.th[0], P.th[1], P.th[2], P.th[3], P.luma,
                                       uint32_t(P.viewport[0] - ox + 32768) | (uint32_t(384 - P.viewport[3] - oy + 32768) << 16),
                                       uint32_t(P.viewport[2] - ox + 32768) | (uint32_t(384 - P.viewport[1] - oy + 32768) << 16), 0u});
            float cx[8], cy[8], cw[8];
            for (int j = 0; j < P.n; ++j)
            {
                const float z = P.z[j] + 1.17549435e-38f;
                const float px = float(P.center[0] - ox) + P.x[j] / z;          // Model 2 screen x
                const float py = (384.0f - float(P.center[1]) - float(oy)) - P.y[j] / z; // Model 2 screen y (0 = top)
                cw[j] = std::max(P.z[j], 1e-6f);
                cx[j] = (px * sx / float(W) * 2.0f - 1.0f) * cw[j];
                cy[j] = (py * sy / float(H) * 2.0f - 1.0f) * cw[j];
            }
            for (int j = 1; j + 1 < P.n; ++j)
                for (int k : {0, j, j + 1})
                    verts.insert(verts.end(), {cx[k], cy[k], 0.0f, cw[k], P.u[k], P.v[k], float(pi)});
            ++pi;
        }

        glViewport(0, 0, W, H);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, G.polyFbo);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!verts.empty())
        {
            glUseProgram(G.prog);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, G.polyBuf);
            glBufferData(GL_SHADER_STORAGE_BUFFER, pdata.size() * 4, pdata.data(), GL_STREAM_DRAW);
            const GLuint texs[5] = {G.atlasA, G.atlasB, G.luma, G.xlat, G.pal};
            const GLint locs[5] = {G.uAtlasA, G.uAtlasB, G.uLuma, G.uXlat, G.uPal};
            for (int t = 0; t < 5; ++t)
            {
                glActiveTexture(GL_TEXTURE1 + t);
                glBindTexture(GL_TEXTURE_2D, texs[t]);
                glUniform1i(locs[t], 1 + t);
            }
            glUniform2f(G.uScale, sx, sy);
            static const bool s_gamma = envOn("PS2X_MOON_GAMMA");
            glUniform1i(G.uGamma, s_gamma ? 1 : 0);
            glBindVertexArray(G.vao);
            glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
            glBufferData(GL_ARRAY_BUFFER, verts.size() * 4, verts.data(), GL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size() / 7));
            for (int t = 0; t < 5; ++t)
            {
                glActiveTexture(GL_TEXTURE1 + t);
                glBindTexture(GL_TEXTURE_2D, 0);
            }
        }
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
        // The picture: low tiles, the polygon layer, high tiles (MAME screen_update order).
        glBindFramebuffer(GL_FRAMEBUFFER, G.fbo);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        if (fr.haveTiles)
            tilePass(false, sx, sy);
        glUseProgram(G.copyProg);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, G.polyTex);
        glUniform1i(G.cPoly, 1);
        glBindVertexArray(G.tileVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (fr.haveTiles)
            tilePass(true, sx, sy);
        // Alpha 0x40 on every pixel, as the port's own pictures have: before its composite the port draws sprites
        // over FBP 224 that use FBP 224 as their Z buffer with alpha test NOTEQUAL 0x40 (TEST 0x3040f), zeroing
        // every pixel whose alpha is not 0x40.
        {
            GLfloat prevClear[4];
            glGetFloatv(GL_COLOR_CLEAR_VALUE, prevClear);
            glDisable(GL_SCISSOR_TEST);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
            glClearColor(0.0f, 0.0f, 0.0f, 64.0f / 255.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);
        }
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
        // Restore before handing over (the sink binds what it needs).
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
        glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
        glBindVertexArray(static_cast<GLuint>(prevVao));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prevArray));
        glUseProgram(static_cast<GLuint>(prevProg));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
        glActiveTexture(static_cast<GLenum>(prevActive));
        if (blend) glEnable(GL_BLEND);
        if (depth) glEnable(GL_DEPTH_TEST);
        if (cullf) glEnable(GL_CULL_FACE);
        if (scis) glEnable(GL_SCISSOR_TEST);

        sink->WriteFrameFromTexture(G.target, fr.target);
        g_hostNs += nsSince(t0);
        glUseProgram(static_cast<GLuint>(prevProg));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
        glActiveTexture(static_cast<GLenum>(prevActive));
    }

    std::atomic<uint64_t> g_frames{0}, g_polys{0}, g_skipped{0};

    struct TileSnap
    {
        std::vector<uint16_t> tileWords;
        std::vector<uint8_t> charSheet;
        std::vector<uint16_t> tilePal;
    };
    std::unique_ptr<TileSnap> g_pendingTiles; // EE thread only

    // FUN_00207538(a0 = mode, a1 = tile block) replacement: the port's tilemap -> GS sprites pass. Its inputs
    // are kept for the next native frame (FUN_002093c0 follows it in the same draw).
    void tileHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = GPR_U32(ctx, 31);
        static const bool s_trace = envOn("PS2X_MOON_NATIVE_TRACE");
        if (s_trace)
            std::fprintf(stderr, "[moon-native] tiles a1 0x%x tick %llu\n", GPR_U32(ctx, 5),
                         static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load()));
        if (GPR_U32(ctx, 5) != kTileBlock)
            return; // only the block the docs describe
        auto t = std::make_unique<TileSnap>();
        t->tileWords.resize(kTileBlockBytes / 2u);
        std::memcpy(t->tileWords.data(), rdram + kTileBlock, kTileBlockBytes);
        t->charSheet.assign(rdram + kCharSheet, rdram + kCharSheet + 0x80000u);
        t->tilePal.resize(4096);
        std::memcpy(t->tilePal.data(), rdram + kTilePalette, 8192);
        g_pendingTiles = std::move(t);
    }

    // Where the picture goes: the port draws each frame into its work buffer FBP 224 (FRAME_1 packet 0x550240,
    // scissor 0..495 x 0..383 from FUN_002488f8), then FUN_00205200 draws the PS2 sprites over it (the SEGA logo
    // 0x4C1B40 via FUN_0020b620, 0x4C2A90 via FUN_0020c790) and copies it, scaled to 512x448, into the display buffer
    // (module packet at +0x35C0's index, TEX0 TBP 7168 = FBP 224), then adds the border bars (0x54CC30/0x54CCF0).
    // The native picture replaces only the port's Model 2 drawing in that buffer; everything after it is the port's.
    constexpr GSNativeTarget kWorkBuffer{224u, 8u, 0u, 0u, 496u, 384u};

    // Marker ids -> frames. The marker sits in the frame's VIF1 chain where the port's Model 2 packets were
    // (chainHook); when the GS stream reaches it, the callback queues the native draw in stream order.
    std::mutex g_pendingMutex;
    std::map<uint32_t, std::shared_ptr<Frame>> g_pending;
    uint32_t g_nextMarker = 1;

    void onMarker(PS2Runtime *runtime, uint64_t value, uint32_t fbp, uint32_t fbw, uint32_t psm)
    {
        std::shared_ptr<Frame> fr;
        {
            std::lock_guard<std::mutex> lock(g_pendingMutex);
            if (!(value & 0x10000u))
                return;
            auto it = g_pending.find(static_cast<uint32_t>(value & 0xFFFFu));
            if (it == g_pending.end())
                return;
            fr = std::move(it->second);
            g_pending.erase(it);
        }
        static const bool s_trace = envOn("PS2X_MOON_NATIVE_TRACE");
        if (s_trace)
            std::fprintf(stderr, "[moon-native] marker %u reached, FRAME_1 fbp %u fbw %u psm %u\n", static_cast<uint32_t>(value), fbp,
                         fbw, psm);
        if (!fr)
            return;
        fr->target = kWorkBuffer;
        runtime->gs().queueNativeHostCall([fr](GSNativeSink *sink) { render(*fr, sink); });
    }

    // Held slow-motion frames (see saveHook/restoreHook below).
    std::vector<uint32_t> g_heldGeo[2];
    std::vector<uint32_t> g_frameGeo;   // the geo list the last native frame was built from
    std::vector<uint32_t> g_restoreGeo; // set by restoreHook for the frame being drawn

    // The frame prepared by frameHook for the chainHook call inside the original FUN_002093c0 (EE thread only).
    std::shared_ptr<Frame> g_chainFrame;

    // FUN_002092d8(a0 = the frame's W2 packet, W2 + 0x3000 + index*0x21380) replacement. That packet is the port's
    // colour-translation pass for its own polygon textures (CLUT upload + GS draws), CALLed by FUN_002093c0 right
    // after this; the native path has no use for it. Its first tag is rewritten (below) with VIF codes
    // NOP(NUM 0x7E, IMM id) and NOP, which the runtime turns into a GS-stream marker (ps2_vif1_interpreter.cpp).
    // So the marker is CALLed by the game's own code, first in the frame's Model 2 work, and no chain node or
    // memory of our own is needed. (Earlier versions linked their own CALL node to a marker in a separate buffer;
    // a ring from guestMalloc shifted the game's malloc blocks, and one of them has a field the game never
    // initialises: the callback at +0x78 that FUN_0022c448 calls when the system file loads, which then pointed
    // into stale data.)
    void chainHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = GPR_U32(ctx, 31);
        const uint32_t pkt = GPR_U32(ctx, 4);
        const uint32_t w2 = rd32(rdram, kW2Ptr);
        if (w2 && rd32(rdram, w2 + 0x18DD34u) != 0u) // FUN_002092d8 clears this flag (it picked its tag with it)
        {
            const uint32_t zero = 0u;
            std::memcpy(rdram + w2 + 0x18DD34u, &zero, 4);
        }
        uint32_t id = 0u;
        if (std::shared_ptr<Frame> fr = std::move(g_chainFrame))
        {
            std::lock_guard<std::mutex> lock(g_pendingMutex);
            id = g_nextMarker++;
            if (g_nextMarker > 0xFFFFu) // ids travel in the 16-bit NOP immediate
                g_nextMarker = 1u;
            g_pending[id] = std::move(fr);
            while (g_pending.size() > 8u) // never reached by the stream (chain dropped): forget the oldest
                g_pending.erase(g_pending.begin());
        }
        // The packet's top level: +0x00 CALL colour pass, +0x10 the tag FUN_002092d8 writes (cnt, or CALL when the
        // flag is set), +0x20 CALL the objects' subchain, +0x30 ret with FRAME_1 = FBP 224 (alpha masked), ZBUF_1 and
        // SCISSOR_1 0..495 x 0..383, the state the overlays and FUN_00205200 then draw with. The marker tag is a
        // NEXT to +0x30, so only the two Model 2 subchains are skipped.
        const uint32_t words[4] = {0x20000000u, pkt + 0x30u, id ? (0x007E0000u | id) : 0u, 0u};
        std::memcpy(rdram + (pkt & 0x01FFFFF0u), words, sizeof(words));
        static const bool s_trace = envOn("PS2X_MOON_NATIVE_TRACE");
        if (s_trace && id)
            std::fprintf(stderr, "[moon-native] marker %u at 0x%x tick %llu\n", id, pkt,
                         static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load()));
    }

    PS2Runtime::RecompiledFunction g_frameOriginal = nullptr;

    // FUN_002093c0(a0 = render list header 0x712300, a1 = g_geo) wrapper: snapshots the Model 2 state for the native
    // picture, then runs the original, whose Model 2 polygon parts are replaced (chainHook for its colour pass,
    // skipObject for op1 objects). Everything else in it is the port's own: the double-buffer and frame counters,
    // the PS2 overlays (FUN_0020bc10, render-list ops 0x1C/0x1D/0x1E), and at the list's END FUN_00205200 (sprites,
    // copy to the display buffer, border bars). The repeat case (head 0x7FFFC) only re-composites the work buffer.
    void frameHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const auto t0 = std::chrono::steady_clock::now();
        if (!g_roms.tried)
            g_roms.load(PS2Runtime::getIoPaths().elfDirectory);
        const uint32_t w2 = rd32(rdram, kW2Ptr);
        const uint32_t list = GPR_U32(ctx, 4), geoState = GPR_U32(ctx, 5);
        g_chainFrame.reset();
        // PS2X_MOON_DUMP_TICK=<vsync>: writes EE RAM once at the first call from that tick on (moon_ram_<tick>.bin in
        // the working directory); the runtime's own RAM dump does not run with the exact backend.
        static const long s_dumpTick = [] { const char *v = std::getenv("PS2X_MOON_DUMP_TICK"); return v ? std::strtol(v, nullptr, 0) : -1L; }();
        if (s_dumpTick >= 0)
        {
            static bool s_dumped = false;
            const uint64_t tick = runtime->memory().gs().vsyncTick.load();
            if (!s_dumped && tick >= static_cast<uint64_t>(s_dumpTick))
            {
                s_dumped = true;
                char name[64];
                std::snprintf(name, sizeof(name), "moon_ram_%06llu.bin", static_cast<unsigned long long>(tick));
                if (FILE *f = std::fopen(name, "wb"))
                {
                    std::fwrite(rdram, 1, 32u * 1024u * 1024u, f);
                    std::fclose(f);
                }
                std::fprintf(stderr, "[moon-native] wrote %s\n", name);
            }
        }
        // FUN_002093c0's conditions for drawing a render list.
        const bool valid = w2 && list && !(list & 0x3fu) && geoState && !(geoState & 0x3fu);
        const uint32_t head = valid ? (rd32(rdram, list) & 0x7fffcu) : 0x7fffcu;
        static const bool s_traceCalls = envOn("PS2X_MOON_NATIVE_TRACE");
        if (s_traceCalls)
            std::fprintf(stderr, "[moon-native] call tick %llu valid %d head 0x%x n %u idx %u mode %u scene %u sub %d vscr %04x %04x %04x %04x\n",
                         static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load()), valid ? 1 : 0, head,
                         valid ? rd32(rdram, list + 4u) : 0u, w2 ? rd32(rdram, w2 + 0x18DD2Cu) : 0u, rd32(rdram, 0x00513104u),
                         rd32(rdram, 0x00513108u), static_cast<int32_t>(rd32(rdram, 0x00566F98u)),
                         rd32(rdram, 0x00638A08u) & 0xFFFFu, rd32(rdram, 0x00638A08u) >> 16, rd32(rdram, 0x00638A0Cu) & 0xFFFFu,
                         rd32(rdram, 0x00638A0Cu) >> 16);
        if (g_roms.ok && head != 0x7fffcu && head < 0x20000u && rd32(rdram, list + 4u) != 0u)
        {
            {
                const auto w0 = std::chrono::steady_clock::now();
                ps2WaitForVif1Chain(); // kept from the tested version (the markers alone keep the order)
                g_waitNs += nsSince(w0);
            }
            auto fr = std::make_shared<Frame>();
            if (!g_restoreGeo.empty())
                fr->geo = std::move(g_restoreGeo); // a held frame: the list the port also redraws (restoreHook)
            else
            {
                fr->geo.resize(kBufferRamWords);
                std::memcpy(fr->geo.data(), rdram + kBufferRam, kBufferRamWords * 4u);
            }
            g_restoreGeo.clear();
            g_frameGeo = fr->geo;
            fr->pagesA.assign(rdram + w2 + kW2PagesA, rdram + w2 + kW2PagesA + 0x80000u);
            fr->pagesB.assign(rdram + w2 + kW2PagesB, rdram + w2 + kW2PagesB + 0x80000u);
            fr->luma.assign(rdram + w2 + kW2Luma, rdram + w2 + kW2Luma + 0x8000u);
            fr->pal.resize(1024);
            std::memcpy(fr->pal.data(), rdram + kGeoState, 2048);
            fr->xlat.resize(3u * 32u * 128u);
            for (int ch = 0; ch < 3; ++ch)
                for (uint32_t row = 0; row < 32u; ++row)
                    for (uint32_t l = 0; l < 128u; ++l)
                        fr->xlat[(ch * 32u + row) * 128u + l] = rdram[kColorXlat[ch] + (row * 256u + l) * 2u];
            if (g_pendingTiles)
            {
                fr->tileWords = std::move(g_pendingTiles->tileWords);
                fr->charSheet = std::move(g_pendingTiles->charSheet);
                fr->tilePal = std::move(g_pendingTiles->tilePal);
                fr->haveTiles = true;
                g_pendingTiles.reset();
            }
            fr->directNormalZ = rd32(rdram, 0x005671A0u);
            // Geometry on the EE thread (it needs the key tables in EE RAM; cheap next to the frame).
            Builder b{*fr, rdram};
            b.run();
            // PS2X_MOON_DUMP_DROP=<vsync>: from that tick on, when a frame has no polygons right after one with over 100, writes EE RAM
            // (moon_drop_<tick>.bin) and the previous frame's bufferRAM (moon_drop_<tick>_prev.bin), up to 3 times.
            {
                static const long s_dropFrom = [] { const char *v = std::getenv("PS2X_MOON_DUMP_DROP"); return v ? std::strtol(v, nullptr, 0) : -1L; }();
                const bool s_dumpDrop = s_dropFrom >= 0 && runtime->memory().gs().vsyncTick.load() >= static_cast<uint64_t>(s_dropFrom);
                static size_t s_prevPolys = 0;
                static std::vector<uint32_t> s_prevGeo;
                static int s_drops = 0;
                if (s_dumpDrop)
                {
                    if (fr->polys.empty() && s_prevPolys > 100u && s_drops < 3)
                    {
                        ++s_drops;
                        const unsigned long long tick = runtime->memory().gs().vsyncTick.load();
                        char name[64];
                        std::snprintf(name, sizeof(name), "moon_drop_%06llu.bin", tick);
                        if (FILE *f = std::fopen(name, "wb"))
                        {
                            std::fwrite(rdram, 1, 32u * 1024u * 1024u, f);
                            std::fclose(f);
                        }
                        std::snprintf(name, sizeof(name), "moon_drop_%06llu_prev.bin", tick);
                        if (FILE *f = std::fopen(name, "wb"))
                        {
                            std::fwrite(s_prevGeo.data(), 4, s_prevGeo.size(), f);
                            std::fclose(f);
                        }
                        std::fprintf(stderr, "[moon-native] polygon drop at tick %llu (previous frame %zu polygons)\n", tick, s_prevPolys);
                    }
                    s_prevPolys = fr->polys.size();
                    s_prevGeo = fr->geo;
                }
            }
            g_polys += fr->polys.size();
            g_skipped += fr->skippedObjects;
            static std::atomic<uint64_t> s_unknownFrames{0};
            if (fr->unknownOps && (s_unknownFrames++ % 600u) == 0u)
                std::fprintf(stderr, "[moon-native] geo list stopped at unknown op word 0x%08x\n", fr->lastUnknownOp);
            const uint64_t f = ++g_frames;
            static const bool s_stats = envOn("PS2X_MOON_NATIVE_STATS");
            if (s_stats && (f % 600u) == 0u)
                std::fprintf(stderr, "[moon-native] frames %llu polys/frame %.0f skipped objects %llu ms/frame EE %.2f host %.2f\n",
                             static_cast<unsigned long long>(f), double(g_polys.load()) / double(f),
                             static_cast<unsigned long long>(g_skipped.load()), double(g_eeNs.load()) / 1e6 / double(f),
                             double(g_hostNs.load()) / 1e6 / double(f));
            g_chainFrame = std::move(fr);
        }
        g_eeNs += nsSince(t0);
        static const bool s_traceThreads = envOn("PS2X_MOON_TRACE_THREADS");
        if (s_traceThreads && (runtime->memory().gs().vsyncTick.load() % 30u) == 0u)
            std::fprintf(stderr, "[moon-native] frame thread %d tick %llu\n", runtime->eeScheduler().currentThreadId(),
                         static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load()));
        // PS2X_MOON_NATIVE_STATS: per 10 s of wall time, the game's frame interval (calls of this hook), the
        // frames over 17 ms, and the time this hook spent waiting for the VIF1 chain.
        {
            static const bool s_stats = envOn("PS2X_MOON_NATIVE_STATS");
            if (s_stats)
            {
                static auto s_last = std::chrono::steady_clock::now(), s_window = s_last;
                static uint64_t s_frames = 0, s_slow = 0, s_maxNs = 0, s_waitAtWindow = 0;
                const auto now = std::chrono::steady_clock::now();
                const uint64_t dt = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - s_last).count());
                s_last = now;
                if (dt > 100000000u)
                    std::fprintf(stderr, "[moon-native] stall %.1f ms ending at %lld ms tick %llu mode %u scene %u sub %d\n", double(dt) / 1e6,
                                 static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                            std::chrono::system_clock::now().time_since_epoch())
                                                            .count()),
                                 static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load()), rd32(rdram, 0x00513104u),
                                 rd32(rdram, 0x00513108u), static_cast<int32_t>(rd32(rdram, 0x00566F98u)));
                ++s_frames;
                s_slow += dt > 17000000u ? 1u : 0u;
                s_maxNs = std::max(s_maxNs, dt);
                if (now - s_window >= std::chrono::seconds(10))
                {
                    const uint64_t wait = g_waitNs.load();
                    std::fprintf(stderr, "[moon-native] window: calls %llu over17ms %llu max %.1f ms vif1-wait %.1f ms tick %llu\n",
                                 static_cast<unsigned long long>(s_frames), static_cast<unsigned long long>(s_slow), double(s_maxNs) / 1e6,
                                 double(wait - s_waitAtWindow) / 1e6,
                                 static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load()));
                    s_window = now, s_frames = 0, s_slow = 0, s_maxNs = 0, s_waitAtWindow = wait;
                }
            }
        }
        g_frameOriginal(rdram, ctx, runtime);
        g_chainFrame.reset(); // not consumed: the original took another branch
        g_restoreGeo.clear();
    }

    // Slow motion (KO replay, VR viewer) holds frames: FUN_001f8018 sets the hold flag 0x5684CC (with 0x564910 set),
    // the game logic then does not rebuild the geo list, and the port draws a saved converted list instead of parsing:
    // FUN_001f8060 (after each draw, hold flag clear) saves the GeoBuffer to slot 0x5684C8 of two at 0x75B000 and
    // advances the slot (0x567128 = next slot; flag 0x5684D0 also saves to the other slot), and FUN_001f8118
    // (instead of the parser, hold flag set) copies slot 0x567128 back and advances it unless 0x5684D0 is set.
    // The native path keeps the same two slots of the raw geo list, under the same conditions.
    constexpr uint32_t kHoldEnable = 0x00564910u, kHoldFlag = 0x005684CCu, kSaveSlot = 0x005684C8u, kSaveBoth = 0x005684D0u,
                       kRestoreSlot = 0x00567128u;
    PS2Runtime::RecompiledFunction g_saveOriginal = nullptr, g_restoreOriginal = nullptr;

    void saveHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_001f8060
    {
        if (rd32(rdram, kHoldEnable) != 0u && rd32(rdram, kHoldFlag) == 0u)
        {
            const uint32_t slot = rd32(rdram, kSaveSlot) & 1u;
            g_heldGeo[slot] = g_frameGeo;
            if (rd32(rdram, kSaveBoth) != 0u)
                g_heldGeo[slot ^ 1u] = g_frameGeo; // the original saves to the advanced slot too
        }
        g_saveOriginal(rdram, ctx, runtime);
    }

    void restoreHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_001f8118
    {
        if (rd32(rdram, kHoldEnable) != 0u && rd32(rdram, kHoldFlag) != 0u)
            g_restoreGeo = g_heldGeo[rd32(rdram, kRestoreSlot) & 1u];
        g_restoreOriginal(rdram, ctx, runtime);
    }

    // PS2X_MOON_TRACE_THREADS=1: logs the guest thread (and its priority-free id) running the load step FUN_001fdf48
    // and the frame FUN_002093c0, to see whether loads share the drawing thread.
    PS2Runtime::RecompiledFunction g_loadStepOriginal = nullptr;
    void loadStepTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::fprintf(stderr, "[moon-native] load step thread %d tick %llu cmd %u\n", runtime->eeScheduler().currentThreadId(),
                     static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load()), rd32(rdram, GPR_U32(ctx, 4)));
        g_loadStepOriginal(rdram, ctx, runtime);
    }

    // FUN_00207810 (render-list op1: one object's packets for VU1/GS) replacement: the native picture has them.
    void skipObject(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ctx->pc = GPR_U32(ctx, 31);
    }
}

void install(PS2Runtime &runtime)
{
    PS2Runtime *rt = &runtime;
    runtime.gs().setNativeMarker([rt](uint64_t value, uint32_t fbp, uint32_t fbw, uint32_t psm) { onMarker(rt, value, fbp, fbw, psm); });
    g_frameOriginal = runtime.lookupFunction(0x002093c0u);
    if (!g_frameOriginal)
    {
        std::fprintf(stderr, "[moon-native] FUN_002093c0 not found; native renderer off\n");
        return;
    }
    runtime.replaceFunction(0x002093c0u, &frameHook);
    runtime.replaceFunction(0x002092d8u, &chainHook);
    runtime.replaceFunction(0x00207810u, &skipObject);
    g_saveOriginal = runtime.lookupFunction(0x001f8060u);
    g_restoreOriginal = runtime.lookupFunction(0x001f8118u);
    if (g_saveOriginal && g_restoreOriginal)
    {
        runtime.replaceFunction(0x001f8060u, &saveHook);
        runtime.replaceFunction(0x001f8118u, &restoreHook);
    }
    if (envOn("PS2X_MOON_TRACE_THREADS"))
    {
        g_loadStepOriginal = runtime.lookupFunction(0x001fdf48u);
        if (g_loadStepOriginal)
            runtime.replaceFunction(0x001fdf48u, &loadStepTrace);
    }
    if (!envOn("PS2X_MOON_NOTILE"))
        runtime.replaceFunction(0x00207538u, &tileHook);
    std::fprintf(stderr, "[moon-native] native Model 2 polygon renderer on (FUN_002093c0 wrapped)\n");
}
}
