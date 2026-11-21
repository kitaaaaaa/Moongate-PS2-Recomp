// GS drawn by the GS User's Manual rules on the GPU. See gs_exact_renderer.h.
// Section numbers below refer to "GS User's Manual Version 6.0" (ref/manuals/gs_users_manual.pdf).

#include "gs_exact_renderer.h"

#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_memory.h"

#include "external/glad.h"
#include "raylib.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>

// Horizontal squeeze of the 2D drawn into the display buffers' shadow images (1 = none): set by a game module, e.g. to
// keep menus at 4:3 inside a widescreen picture. Only the high-resolution shadow images change; GS memory does not.
static std::atomic<float> g_hiResSqueeze{1.0f};
void ps2SetHiResSqueeze(float k)
{
    g_hiResSqueeze.store(k > 0.0f && k <= 1.0f ? k : 1.0f, std::memory_order_relaxed);
}

namespace
{
    // GS memory, swizzle tables and per-PSM layout constants, shared by the draw and PCRTC shaders.
    const char *kCommonGlsl = R"GLSL(
layout(std430, binding = 0) coherent buffer Mem { uint mem[]; };
layout(std430, binding = 1) readonly buffer Swz { uint swz[]; };
layout(std430, binding = 6) readonly buffer PsmInfo { uint psmInfo[]; };
layout(std430, binding = 5) buffer Counters { uint counters[]; };

// Chapter 8: bit address of pixel (x, y) of a buffer at block bp, width bw (64-pixel units).
// psmInfo per PSM code: table offset, page width, page height, bits per pixel slot, bit offset,
// stored bytes, nibble flag.
uint pixelBits(uint psm, uint bp, uint bw, uint x, uint y)
{
    uint b = psm * 8u;
    uint toff = psmInfo[b], pw = psmInfo[b + 1u], ph = psmInfo[b + 2u];
    uint ub = psmInfo[b + 3u], bo = psmInfo[b + 4u];
    uint bwv = max(bw, 1u); // manual silent on width 0 (BITBLTBUF: "1 to 32")
    uint page = bp / 32u + (y / ph) * ((bwv * 64u) / pw) + x / pw;
    uint offs = swz[toff + (x % pw) + ((bp % 32u) * ph + y % ph) * pw];
    return (page * pw * ph + offs) * ub + bo;
}

uint byteAddress(uint psm, uint bits)
{
    return (bits >> 3u) & (4194304u - psmInfo[psm * 8u + 5u]);
}

// Raw stored value: 32 bits (24-bit formats unmasked), 16 bits, 8 bits or a nibble.
uint readPixel(uint psm, uint bp, uint bw, uint x, uint y)
{
    uint bits = pixelBits(psm, bp, bw, x, y);
    uint addr = byteAddress(psm, bits);
    uint word = mem[addr >> 2u];
    uint pb = psmInfo[psm * 8u + 5u];
    if (pb == 4u)
        return word;
    uint shift = (addr & 3u) * 8u;
    if (pb == 2u)
        return (word >> shift) & 0xFFFFu;
    uint v = (word >> shift) & 0xFFu;
    return psmInfo[psm * 8u + 6u] != 0u ? ((v >> (bits & 7u)) & 0xFu) : v;
}

// Word index and bit shift of a 32- or 16-bit pixel (frame and Z buffers).
void pixelWord(uint psm, uint bp, uint bw, uint x, uint y, out uint word, out uint shift)
{
    uint addr = byteAddress(psm, pixelBits(psm, bp, bw, x, y));
    word = addr >> 2u;
    shift = (addr & 3u) * 8u;
}

// Only the masked bits of the word change; other pixels sharing the word (16-bit formats) are
// written by other invocations at the same time, so the update is two atomic operations.
void writeBits(uint word, uint value, uint mask)
{
    if (mask == 0u)
        return;
    atomicAnd(mem[word], ~mask);
    atomicOr(mem[word], value & mask);
}

void countSilent(uint which)
{
#ifndef HIRES
    atomicAdd(counters[which], 1u);
#endif
}
)GLSL";

    const char *kDrawVertex = R"GLSL(#version 430
layout(std430, binding = 2) readonly buffer Prims { uint prims[]; };
layout(std430, binding = 3) readonly buffer States { uint states[]; };
flat out int vPrim;

int wcoord(uint pk, int shiftBits, uint offset)
{
    return int((pk >> uint(shiftBits)) & 0xFFFFu) - int(offset);
}

void main()
{
    int prim = gl_VertexID / 6;
    int corner = gl_VertexID % 6;
    vPrim = prim;
    uint pb = uint(prim) * 32u;
    if (prims[pb + 26u] != 0u)
    {
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0); // run by the sequence shader instead
        return;
    }
    uint sb = prims[pb + 25u] * 48u;
    uint type = states[sb] & 7u;
    uint n = type == 0u ? 1u : (type == 1u || type == 2u || type == 6u) ? 2u : 3u;
    uint ofx = states[sb + 10u], ofy = states[sb + 11u];
    int minX = 1 << 30, minY = 1 << 30, maxX = -(1 << 30), maxY = -(1 << 30);
    for (uint i = 0u; i < n; ++i)
    {
        uint xy = prims[pb + i * 8u];
        int x = wcoord(xy, 0, ofx), y = wcoord(xy, 16, ofy);
        minX = min(minX, x); minY = min(minY, y);
        maxX = max(maxX, x); maxY = max(maxY, y);
    }
    // Pixels whose centre (i * 16) can be covered, one pixel of margin (the exact test is in the
    // fragment shader), clipped to the scissor rectangle (3.7.1).
    int x0 = (minX >> 4) - 1, y0 = (minY >> 4) - 1;
    int x1 = (maxX >> 4) + 1, y1 = (maxY >> 4) + 1;
    uint sx = states[sb + 8u], sy = states[sb + 9u];
    x0 = max(x0, int(sx & 0xFFFFu)); x1 = min(x1, int(sx >> 16));
    y0 = max(y0, int(sy & 0xFFFFu)); y1 = min(y1, int(sy >> 16));
    x0 = max(x0, 0); y0 = max(y0, 0); x1 = min(x1, 2047); y1 = min(y1, 2047);
    if (x0 > x1 || y0 > y1)
    {
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
        return;
    }
    // Two triangles covering pixel squares [x0, x1 + 1) x [y0, y1 + 1) of a 2048 x 2048 target.
    int cx = (corner == 1 || corner == 2 || corner == 4) ? x1 + 1 : x0;
    int cy = (corner == 2 || corner == 4 || corner == 5) ? y1 + 1 : y0;
    gl_Position = vec4(float(cx) / 1024.0 - 1.0, float(cy) / 1024.0 - 1.0, 0.0, 1.0);
}
)GLSL";

    const char *kDrawFragmentHead = R"GLSL(#version 430
#extension GL_ARB_fragment_shader_interlock : require
layout(pixel_interlock_ordered) in;
)GLSL";

    const char *kDrawFragmentBody = R"GLSL(
// REAL: double in the GS-exact passes; float in the high-resolution passes (#define HIRES), where exactness to the
// GS's 1x arithmetic does not apply and double precision is slow on most GPUs. Depth: exact for equal vertex Z, float otherwise.
#ifdef HIRES
#define REAL float
#define LF(x) x
#else
#define REAL double
#define LF(x) x##lf
#endif
layout(std430, binding = 2) readonly buffer Prims { uint prims[]; };
layout(std430, binding = 3) readonly buffer States { uint states[]; };
layout(std430, binding = 4) buffer Cluts { uint cluts[]; }; // the CLUT temporary buffer (snapshots)
uniform int uDebug; // PS2X_GS_EXACT_DEBUG=1: every covered pixel written magenta (pipeline bypassed)
#ifdef HIRES
// High-resolution shadow pass (see GSExactRenderer::HiDrawPasses): the same pipeline evaluated at the centre of a
// shadow-image pixel (a fractional GS position), reading and writing the shadow image of one frame buffer instead
// of GS memory. Role 1: the primitive's frame buffer is that buffer (its Z too when ZBP == FBP); role 2: only its
// Z buffer is (the frame is not shadowed in this pass).
uniform int uHiTarget, uHiRole;
uniform vec2 uHiScale;                                  // shadow pixels per GS pixel of the drawn buffer
layout(r32ui, binding = 1) coherent uniform uimage2D uHiImg;
// A Z buffer that is neither the drawn buffer nor shared with it (uHiZMode 1): a high-resolution copy made from GS
// memory before the batch, read and written in primitive order like the frame (16-bit Z in the low bits).
uniform int uHiZMode;
layout(r32ui, binding = 2) coherent uniform uimage2D uHiZ;
uniform uint uTgtTbp[3], uTgtValid[3];                  // shadowed buffers, as texture sources
uniform vec2 uTgtScale[3];
uniform sampler2D uTgtTex0, uTgtTex1, uTgtTex2;
vec2 gHiC;    // sample centre in 1/16 GS pixels
ivec2 gHiPix; // shadow pixel
uint gHiCur;  // the shadow pixel's current value (frame and aliased Z writes go to the same word)
#endif

// "manual silent" counters (GSExactRenderer::UnsupportedSummary names them).
const uint SILENT_Z_RANGE = 0u, SILENT_LINE_TIE = 1u, SILENT_BLEND_RESERVED = 2u, SILENT_DITHER32 = 3u,
           SILENT_ZTE0 = 4u, SILENT_TRI_DEGENERATE = 5u, SILENT_POINT_TIE = 6u, SILENT_NEG_TEXCOORD = 7u,
           SILENT_MMIN_UNDEFINED = 8u, SILENT_Q_ZERO = 9u, SILENT_AA1 = 10u;

uint sb;
uint S(uint i) { return states[sb + i]; }

int ofsX, ofsY;
uint pb;
uint vXY(uint i) { return prims[pb + i * 8u]; }
int vX(uint i) { return int(vXY(i) & 0xFFFFu) - ofsX; }
int vY(uint i) { return int(vXY(i) >> 16) - ofsY; }
uint vZ(uint i) { return prims[pb + i * 8u + 1u]; }
uint vRGBA(uint i) { return prims[pb + i * 8u + 2u]; }
// 3.4.4 / RGBAQ: "The GS performs processing by rounding down the lower 8 bits of the mantissa".
float vF32(uint i, uint k) { return uintBitsToFloat(prims[pb + i * 8u + k] & 0xFFFFFF00u); }
uint vUV(uint i) { return prims[pb + i * 8u + 6u]; }
uint vFog(uint i) { return prims[pb + i * 8u + 7u]; }

// ---- texture ------------------------------------------------------------------------------------

uint lvTbp[7], lvTbw[7];
uint gLastRaw = 0u; // debug: the last raw texel read

// 3.4.5 wrap modes on the integer texel coordinate of level `lvl`.
int wrap(int c, uint size, uint mode, uint lo, uint hi, uint lvl)
{
    if (mode == 0u) return c & int(size - 1u);                  // REPEAT: u % USIZE
    if (mode == 1u) return clamp(c, 0, int(size) - 1);          // CLAMP (manual silent: USIZE as the last texel)
    if (mode == 2u) return clamp(c, int(lo >> lvl), int(hi >> lvl)); // REGION_CLAMP, MINU >> n on level n
    return (c & int(lo)) | int(hi);                             // REGION_REPEAT: (uint & UMSK) | UFIX
}

// RGBA16 to 8 bits per channel with TEXA (3.4.6).
uvec4 expand16(uint raw, uint texa)
{
    uint r = (raw & 0x1Fu) << 3, g = ((raw >> 5) & 0x1Fu) << 3, b = ((raw >> 10) & 0x1Fu) << 3;
    uint ta0 = texa & 0xFFu, aem = (texa >> 8) & 1u, ta1 = (texa >> 16) & 0xFFu;
    uint a;
    if ((raw & 0x8000u) != 0u)
        a = ta1;
    else
        a = (aem != 0u && r == 0u && g == 0u && b == 0u) ? 0u : ta0;
    return uvec4(r, g, b, a);
}

uvec4 expand24(uint raw, uint texa)
{
    uint r = raw & 0xFFu, g = (raw >> 8) & 0xFFu, b = (raw >> 16) & 0xFFu;
    uint ta0 = texa & 0xFFu, aem = (texa >> 8) & 1u;
    uint a = (aem != 0u && r == 0u && g == 0u && b == 0u) ? 0u : ta0;
    return uvec4(r, g, b, a);
}

uvec4 expand32(uint raw)
{
    return uvec4(raw & 0xFFu, (raw >> 8) & 0xFFu, (raw >> 16) & 0xFFu, raw >> 24);
}

// One texel of level `lvl` after wrapping, format conversion and the CLUT (3.4.6, 3.4.7).
uvec4 texel(int u, int v, uint lvl)
{
    uint psm = S(14), texa = S(34);
    uint usz = max(1u << S(15) >> lvl, 1u), vsz = max(1u << S(16) >> lvl, 1u);
    uint clampLo = S(28), clampHi = S(29);
    uint wms = clampLo & 3u, wmt = (clampLo >> 2) & 3u;
    uint minu = (clampLo >> 4) & 0x3FFu, maxu = (clampLo >> 14) & 0x3FFu;
    uint minv = ((clampLo >> 24) | (clampHi << 8)) & 0x3FFu, maxv = (clampHi >> 2) & 0x3FFu;
    u = wrap(u, usz, wms, minu, maxu, lvl);
    v = wrap(v, vsz, wmt, minv, maxv, lvl);
    uint raw = readPixel(psm, lvTbp[lvl], lvTbw[lvl], uint(u) & 2047u, uint(v) & 2047u);
    gLastRaw = raw;
    if (psm == 19u || psm == 20u || psm == 27u || psm == 36u || psm == 44u)
    {
        // Index into the temporary buffer from entry CSA * 16 (3.4.7). Manual silent on running
        // past the end of the buffer: the index wraps.
        uint cpsm = S(19), csa = S(20), cb = S(21) * 512u;
        if (cpsm == 2u || cpsm == 10u)
            return expand16(cluts[cb + ((csa * 16u + raw) & 511u)], texa);
        uint e = (csa * 16u + raw) & 255u;
        uint c32 = cluts[cb + e] | (cluts[cb + 256u + e] << 16);
        return cpsm == 1u ? expand24(c32 & 0xFFFFFFu, texa) : expand32(c32);
    }
    if (psm == 1u || psm == 49u)
        return expand24(raw & 0xFFFFFFu, texa);
    if (psm == 2u || psm == 10u || psm == 50u || psm == 58u)
        return expand16(raw, texa);
    return expand32(raw);
}

// 3.4.8 sampling of one level at texel coordinates (u, v) of level 0.
uvec4 sampleLevel(float u0, float v0, uint lvl, bool bilinear)
{
    float scale = 1.0 / float(1u << lvl);
    float u = u0 * scale, v = v0 * scale;
    if (!bilinear)
        return texel(int(floor(u)), int(floor(v)), lvl); // "Integer Part"; manual silent for negatives: floor
    float su = u - 0.5, sv = v - 0.5;
    int iu = int(floor(su)), iv = int(floor(sv));
    float a = su - float(iu), b = sv - float(iv);
    vec4 ta = vec4(texel(iu, iv, lvl)), tb = vec4(texel(iu + 1, iv, lvl));
    vec4 tc = vec4(texel(iu, iv + 1, lvl)), td = vec4(texel(iu + 1, iv + 1, lvl));
    precise vec4 c = (1.0 - a) * (1.0 - b) * ta + a * (1.0 - b) * tb + (1.0 - a) * b * tc + a * b * td;
    return uvec4(floor(c)); // manual silent on weight precision and rounding: truncation
}

#ifdef HIRES
// (b - a) x (p - a), computed from the lower of a, b so that the reversed edge gives exactly the negated value.
float edgeFn(float ax, float ay, float bx, float by, float px, float py)
{
    if (ay > by || (ay == by && ax > bx))
        return -((ax - bx) * (py - by) - (ay - by) * (px - bx));
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

// A texture that is a shadowed frame buffer (same base, width, 32-bit) is read from its shadow image, so a copy of
// one buffer into another keeps the resolution. Never the buffer this pass draws into.
int hiSource()
{
    uint psm = S(14);
    if ((psm != 0u && psm != 1u) || S(13) != 8u)
        return -1;
    for (int i = 0; i < 3; ++i)
        if (i != uHiTarget && uTgtValid[i] != 0u && S(12) == uTgtTbp[i])
            return i;
    return -1;
}

vec4 shadowFetch(int i, vec2 p, bool bilinear)
{
    // p in shadow pixels. Bilinear: the GS's (u - 0.5) weights are the GL texel-centre convention.
    if (i == 0)
        return bilinear ? textureLod(uTgtTex0, p / vec2(textureSize(uTgtTex0, 0)), 0.0)
                        : texelFetch(uTgtTex0, clamp(ivec2(floor(p)), ivec2(0), textureSize(uTgtTex0, 0) - 1), 0);
    if (i == 1)
        return bilinear ? textureLod(uTgtTex1, p / vec2(textureSize(uTgtTex1, 0)), 0.0)
                        : texelFetch(uTgtTex1, clamp(ivec2(floor(p)), ivec2(0), textureSize(uTgtTex1, 0) - 1), 0);
    return bilinear ? textureLod(uTgtTex2, p / vec2(textureSize(uTgtTex2, 0)), 0.0)
                    : texelFetch(uTgtTex2, clamp(ivec2(floor(p)), ivec2(0), textureSize(uTgtTex2, 0) - 1), 0);
}

uvec4 sampleShadow(int i, float u, float v, bool bilinear)
{
    vec4 c = shadowFetch(i, vec2(u, v) * uTgtScale[i], bilinear);
    uvec4 t = uvec4(clamp(floor(c * 255.0 + 0.5), 0.0, 255.0));
    uint texa = S(34);
    if (S(14) == 1u) // RGB24 texture: alpha from TEXA
        return expand24(t.r | (t.g << 8) | (t.b << 16), texa);
    return t;
}
#endif

// floor(num / den) for integer-valued doubles below 2^53, den > 0, exact whatever the division rounds to.
REAL floorDiv(REAL num, REAL den)
{
    REAL q = floor(num / den);
    if (q * den > num)
        q -= LF(1.0);
    else if ((q + LF(1.0)) * den <= num)
        q += LF(1.0);
    return q;
}

// a0 + (a1 - a0) * n / d truncated, exactly (lines and sprites: manual silent on the DDA's precision;
// the exact value truncated, UV kept to the UV register's 1/16 texel).
REAL lerpFloor(REAL a0, REAL a1, REAL n, REAL d)
{
    return a0 + floorDiv((a1 - a0) * n, d);
}

// ---- the per-pixel GS pipeline, shared by the draw (fragment) and sequence (compute) shaders ------
// gsShade: coverage, DDA values, texture, fog, tests' inputs; false when the pixel is not drawn.
// gsWrite: the read-modify-write of the frame and Z buffers (inside the interlock in the fragment shader).

ivec2 pix;
bool gAbe, gTme;
uint fword, fshift, zword, zshift, z, test, fpsm, fbmsk, fmask16, zpsm, zmsk, ztst;
uint selA, selB, selC, selD, fix, colclamp, dthe, fba;
bool fb16, pabe, writeRgb, writeA, writeZ;
uvec4 cv;

bool gsShade(int prim, ivec2 pixIn)
{
    pix = pixIn;
    pb = uint(prim) * 32u;
    sb = prims[pb + 25u] * 48u;
    uint primBits = S(0);
    uint type = primBits & 7u;
    bool iip = ((primBits >> 3) & 1u) != 0u, tme = ((primBits >> 4) & 1u) != 0u, fge = ((primBits >> 5) & 1u) != 0u;
    bool abe = ((primBits >> 6) & 1u) != 0u, aa1 = ((primBits >> 7) & 1u) != 0u, fst = ((primBits >> 8) & 1u) != 0u;
    ofsX = int(S(10));
    ofsY = int(S(11));
    gAbe = abe;
    gTme = tme;
    // 3.7.1 scissoring (the draw's quad is already clipped to it; the sequence shader is not).
    uint scx = S(8), scy = S(9);
    if (pix.x < int(scx & 0xFFFFu) || pix.x > int(scx >> 16) || pix.y < int(scy & 0xFFFFu) || pix.y > int(scy >> 16))
        return false;
    // Pixel centre in 1/16 window units (2.4.4); the shadow pass samples at its own pixel's centre.
#ifdef HIRES
    REAL cxd = gHiC.x, cyd = gHiC.y;
#else
    REAL cxd = REAL(pix.x * 16), cyd = REAL(pix.y * 16);
#endif

    // SCANMSK
    uint scanmsk = S(36) & 3u;
    if ((scanmsk == 2u && (pix.y & 1) == 0) || (scanmsk == 3u && (pix.y & 1) == 1))
        return false;

    // ---- 3.2.9 coverage and interpolation weights ----
    REAL w0 = LF(1.0), w1 = LF(0.0), w2 = LF(0.0); // attribute = w0*a0 + w1*a1 + w2*a2
    REAL sx = LF(0.0), sy = LF(0.0);            // sprite: fraction across x and y
    REAL ln = LF(0.0), ld = LF(1.0);            // line: position along the major axis as ln / ld
    REAL snx = LF(0.0), sdx = LF(1.0), sny = LF(0.0), sdy = LF(1.0); // sprite: sx = snx / sdx, sy = sny / sdy
    uint last = 0u;                           // vertex supplying flat colour (the one kicked last)
    if (type == 0u)
    {
        // Point: the closest pixel. Manual silent on exact halves: they go to the larger coordinate.
        int x = vX(0u), y = vY(0u);
        if (((x + 8) >> 4) != pix.x || ((y + 8) >> 4) != pix.y)
            return false;
        if ((x & 15) == 8 || (y & 15) == 8)
            countSilent(SILENT_POINT_TIE);
    }
    else if (type == 1u || type == 2u)
    {
        // Line: each pixel owns the diamond |dx| + |dy| < 0.5 around its centre (Figure 3-5); the
        // pixels whose diamond the line passes through are drawn, except the one holding the end point.
        REAL ax = REAL(vX(0u)), ay = REAL(vY(0u)), bx = REAL(vX(1u)), by = REAL(vY(1u));
        REAL dx = bx - ax, dy = by - ay;
        REAL pcx = cxd, pcy = cyd;
        REAL best = abs(ax - pcx) + abs(ay - pcy);
        best = min(best, abs(bx - pcx) + abs(by - pcy));
        if (dx != LF(0.0))
        {
            REAL t = (pcx - ax) / dx;
            if (t > LF(0.0) && t < LF(1.0))
                best = min(best, abs(ay + t * dy - pcy));
        }
        if (dy != LF(0.0))
        {
            REAL t = (pcy - ay) / dy;
            if (t > LF(0.0) && t < LF(1.0))
                best = min(best, abs(ax + t * dx - pcx));
        }
        if (best == LF(8.0))
            countSilent(SILENT_LINE_TIE); // touches the diamond's edge: manual silent, not drawn
        if (best >= LF(8.0))
            return false;
        if (abs(bx - pcx) + abs(by - pcy) < LF(8.0))
            return false; // "a pixel located at the endpoint of a line is not drawn"
        // The DDA steps along the major axis (supplement 1.1); position along it.
        bool xMajor = abs(dx) >= abs(dy);
        ln = xMajor ? pcx - ax : pcy - ay;
        ld = xMajor ? dx : dy;
        if (ld < LF(0.0))
        {
            ln = -ln;
            ld = -ld;
        }
        ln = clamp(ln, LF(0.0), ld);
        REAL t = ln / ld;
        w0 = LF(1.0) - t;
        w1 = t;
        last = 1u;
    }
    else if (type == 6u)
    {
        // Sprite: top/left sides drawn, bottom/right not (Figure 3-9).
        int x0 = vX(0u), y0 = vY(0u), x1 = vX(1u), y1 = vY(1u);
        if (cxd < REAL(min(x0, x1)) || cxd >= REAL(max(x0, x1)) || cyd < REAL(min(y0, y1)) || cyd >= REAL(max(y0, y1)))
            return false;
        snx = cxd - REAL(x0);
        sdx = REAL(x1 - x0);
        sny = cyd - REAL(y0);
        sdy = REAL(y1 - y0);
        if (sdx < LF(0.0)) { snx = -snx; sdx = -sdx; }
        if (sdy < LF(0.0)) { sny = -sny; sdy = -sdy; }
        sx = (cxd - REAL(x0)) / REAL(x1 - x0);
        sy = (cyd - REAL(y0)) / REAL(y1 - y0);
        last = 1u;
    }
    else
    {
        // Triangle: a centre on a side is drawn on left sides and on horizontal top sides only.
        REAL px0 = REAL(vX(0u)), py0 = REAL(vY(0u));
        REAL px1 = REAL(vX(1u)), py1 = REAL(vY(1u));
        REAL px2 = REAL(vX(2u)), py2 = REAL(vY(2u));
        REAL area = (px1 - px0) * (py2 - py0) - (py1 - py0) * (px2 - px0);
        if (area == LF(0.0))
        {
            countSilent(SILENT_TRI_DEGENERATE);
            return false;
        }
        REAL sgn = area > LF(0.0) ? LF(1.0) : -LF(1.0);
        REAL pcx = cxd, pcy = cyd;
        // Edge k is opposite vertex k; inside when sgn * e > 0.
#ifdef HIRES
        // Each edge evaluated from its lower endpoint (by y, then x), so two triangles sharing an edge get exactly
        // opposite values in float: no gaps or double hits along it.
        REAL e0 = sgn * edgeFn(px1, py1, px2, py2, pcx, pcy);
        REAL e1 = sgn * edgeFn(px2, py2, px0, py0, pcx, pcy);
        REAL e2 = sgn * edgeFn(px0, py0, px1, py1, pcx, pcy);
#else
        REAL e0 = sgn * ((px2 - px1) * (pcy - py1) - (py2 - py1) * (pcx - px1));
        REAL e1 = sgn * ((px0 - px2) * (pcy - py2) - (py0 - py2) * (pcx - px2));
        REAL e2 = sgn * ((px1 - px0) * (pcy - py0) - (py1 - py0) * (pcx - px0));
#endif
        // Edge direction as seen with the inside on the left of (dx, dy) * sgn, in y-down window
        // coordinates the inside normal is (-dy, dx): a left side has the inside to its right
        // (normal x > 0, so dy < 0), a top side is horizontal with the inside below (dx > 0).
        REAL d0x = sgn * (px2 - px1), d0y = sgn * (py2 - py1);
        REAL d1x = sgn * (px0 - px2), d1y = sgn * (py0 - py2);
        REAL d2x = sgn * (px1 - px0), d2y = sgn * (py1 - py0);
        bool in0 = e0 > LF(0.0) || (e0 == LF(0.0) && (d0y < LF(0.0) || (d0y == LF(0.0) && d0x > LF(0.0))));
        bool in1 = e1 > LF(0.0) || (e1 == LF(0.0) && (d1y < LF(0.0) || (d1y == LF(0.0) && d1x > LF(0.0))));
        bool in2 = e2 > LF(0.0) || (e2 == LF(0.0) && (d2y < LF(0.0) || (d2y == LF(0.0) && d2x > LF(0.0))));
        if (!(in0 && in1 && in2))
            return false;
        REAL inv = LF(1.0) / (sgn * area);
        w0 = e0 * inv;
        w1 = e1 * inv;
        w2 = e2 * inv;
        last = 2u;
    }

    if (aa1 && (type == 1u || type == 2u || type == 3u || type == 4u || type == 5u))
        countSilent(SILENT_AA1); // antialiasing band not drawn (see UnsupportedSummary)

    // ---- DDA values (manual silent on precision: exact plane values, truncated) ----
    uint rgbaF;
    if (iip && (type == 1u || type == 2u))
    {
        uint r0 = vRGBA(0u), r1 = vRGBA(1u);
        uint ci[4];
        for (uint k = 0u; k < 4u; ++k)
            ci[k] = uint(clamp(lerpFloor(REAL((r0 >> (8u * k)) & 0xFFu), REAL((r1 >> (8u * k)) & 0xFFu), ln, ld), LF(0.0), LF(255.0)));
        rgbaF = ci[0] | (ci[1] << 8) | (ci[2] << 16) | (ci[3] << 24);
    }
    else if (iip && type != 0u && type != 6u)
    {
        precise vec4 c = vec4(0.0); // precise: the CPU port (CpuGs::shade) computes the same, unfused
        for (uint i = 0u; i < 3u; ++i)
        {
            REAL w = i == 0u ? w0 : i == 1u ? w1 : w2;
            if (w == LF(0.0)) continue;
            uint rgba = vRGBA(i);
            c += vec4(float(w)) * vec4(float(rgba & 0xFFu), float((rgba >> 8) & 0xFFu), float((rgba >> 16) & 0xFFu), float(rgba >> 24));
        }
        uvec4 ci = uvec4(clamp(floor(c + vec4(1.0e-4)), 0.0, 255.0));
        rgbaF = ci.r | (ci.g << 8) | (ci.b << 16) | (ci.a << 24);
    }
    else
        rgbaF = vRGBA(last); // flat: the colour set before the drawing kick (3.2.8)
    uvec4 cf = uvec4(rgbaF & 0xFFu, (rgbaF >> 8) & 0xFFu, (rgbaF >> 16) & 0xFFu, rgbaF >> 24);

#ifdef HIRES
    // Depth in the high-resolution passes without double precision (slow on most GPUs): exact when the vertices
    // share one Z (all 2D), float interpolation otherwise.
    uint fogF;
    if (type == 6u)
    {
        z = vZ(1u);
        fogF = vFog(1u);
    }
    else if (type == 0u)
    {
        z = vZ(0u);
        fogF = vFog(0u);
    }
    else if (type == 1u || type == 2u)
    {
        uint z0 = vZ(0u), z1 = vZ(1u);
        z = z0 == z1 ? z0 : uint(clamp(floor(float(z0) + (float(z1) - float(z0)) * (ln / ld)), 0.0, 4294967040.0));
        fogF = uint(clamp(lerpFloor(REAL(vFog(0u)), REAL(vFog(1u)), ln, ld), LF(0.0), LF(255.0)));
    }
    else
    {
        uint z0 = vZ(0u), z1 = vZ(1u), z2 = vZ(2u);
        z = (z0 == z1 && z1 == z2) ? z0
                                   : uint(clamp(floor(w0 * float(z0) + w1 * float(z1) + w2 * float(z2) + 1.0e-6), 0.0, 4294967040.0));
        REAL f = w0 * REAL(vFog(0u)) + w1 * REAL(vFog(1u)) + w2 * REAL(vFog(2u));
        fogF = uint(clamp(floor(f + LF(1.0e-6)), LF(0.0), LF(255.0)));
    }
#else
    double zd;
    uint fogF;
    if (type == 6u)
    {
        zd = double(vZ(1u)); // sprite: Z and fog of the second vertex (3.2.8)
        fogF = vFog(1u);
    }
    else if (type == 0u)
    {
        zd = double(vZ(0u));
        fogF = vFog(0u);
    }
    else if (type == 1u || type == 2u)
    {
        zd = lerpFloor(double(vZ(0u)), double(vZ(1u)), ln, ld);
        fogF = uint(clamp(lerpFloor(REAL(vFog(0u)), REAL(vFog(1u)), ln, ld), LF(0.0), LF(255.0)));
    }
    else
    {
        zd = w0 * double(vZ(0u)) + w1 * double(vZ(1u)) + w2 * double(vZ(2u));
        REAL f = w0 * REAL(vFog(0u)) + w1 * REAL(vFog(1u)) + w2 * REAL(vFog(2u));
        fogF = uint(clamp(floor(f + LF(1.0e-6)), LF(0.0), LF(255.0)));
    }
    zd = clamp(floor(zd + LF(1.0e-6)), LF(0.0), LF(4294967295.0));
    z = uint(zd);
#endif

    // ---- 3.4 texture mapping ----
    cv = cf;
    if (tme)
    {
        float u, v, q;
        if (fst)
        {
            // UV in 1/16 texels.
            REAL uu, vv;
            if (type == 6u)
            {
                uu = lerpFloor(REAL(vUV(0u) & 0xFFFFu), REAL(vUV(1u) & 0xFFFFu), snx, sdx);
                vv = lerpFloor(REAL(vUV(0u) >> 16), REAL(vUV(1u) >> 16), sny, sdy);
            }
            else if (type == 1u || type == 2u)
            {
                uu = lerpFloor(REAL(vUV(0u) & 0xFFFFu), REAL(vUV(1u) & 0xFFFFu), ln, ld);
                vv = lerpFloor(REAL(vUV(0u) >> 16), REAL(vUV(1u) >> 16), ln, ld);
            }
            else
            {
                uu = w0 * REAL(vUV(0u) & 0xFFFFu) + w1 * REAL(vUV(1u) & 0xFFFFu) + w2 * REAL(vUV(2u) & 0xFFFFu);
                vv = w0 * REAL(vUV(0u) >> 16) + w1 * REAL(vUV(1u) >> 16) + w2 * REAL(vUV(2u) >> 16);
            }
            u = float(uu / LF(16.0));
            v = float(vv / LF(16.0));
            q = type == 6u ? vF32(1u, 5u) : float(w0 * REAL(vF32(0u, 5u)) + w1 * REAL(vF32(1u, 5u)) + w2 * REAL(vF32(2u, 5u)));
        }
        else
        {
            REAL s, t, qs, qt;
            if (type == 6u)
            {
                // Sprite STQ: manual silent on how Q varies across the rectangle; S and Q along x,
                // T and Q along y.
                s = REAL(vF32(0u, 3u)) + sx * (REAL(vF32(1u, 3u)) - REAL(vF32(0u, 3u)));
                t = REAL(vF32(0u, 4u)) + sy * (REAL(vF32(1u, 4u)) - REAL(vF32(0u, 4u)));
                qs = REAL(vF32(0u, 5u)) + sx * (REAL(vF32(1u, 5u)) - REAL(vF32(0u, 5u)));
                qt = REAL(vF32(0u, 5u)) + sy * (REAL(vF32(1u, 5u)) - REAL(vF32(0u, 5u)));
            }
            else
            {
                s = w0 * REAL(vF32(0u, 3u)) + w1 * REAL(vF32(1u, 3u)) + w2 * REAL(vF32(2u, 3u));
                t = w0 * REAL(vF32(0u, 4u)) + w1 * REAL(vF32(1u, 4u)) + w2 * REAL(vF32(2u, 4u));
                qs = w0 * REAL(vF32(0u, 5u)) + w1 * REAL(vF32(1u, 5u)) + w2 * REAL(vF32(2u, 5u));
                qt = qs;
            }
            if (qs == LF(0.0) || qt == LF(0.0))
            {
                countSilent(SILENT_Q_ZERO);
                qs = qs == LF(0.0) ? LF(1.0) : qs;
                qt = qt == LF(0.0) ? LF(1.0) : qt;
            }
            // 3.4.4: u = USIZE x S/Q, v = VSIZE x T/Q
            u = float(s / qs * REAL(1u << S(15)));
            v = float(t / qt * REAL(1u << S(16)));
            q = float(qs);
        }
        // "u and v values that the GS can process are in the range of -2047 to +2047"
        u = clamp(u, -2047.0, 2047.0);
        v = clamp(v, -2047.0, 2047.0);
        if (u < 0.0 || v < 0.0)
            countSilent(SILENT_NEG_TEXCOORD);

        // 3.4.11 / 3.4.12 levels and filters.
        uint tex1Lo = S(22), tex1Hi = S(23);
        uint lcm = tex1Lo & 1u, mxl = (tex1Lo >> 2) & 7u, mmag = (tex1Lo >> 5) & 1u, mmin = (tex1Lo >> 6) & 7u;
        uint lshift = (tex1Lo >> 19) & 3u;
        int kfix = int(tex1Hi << 20) >> 20; // K: 12-bit signed, 1:7:4
        float k = float(kfix) / 16.0;
        float lod = lcm == 1u ? k : (log2(1.0 / abs(q)) * float(1u << lshift) + k);
        lvTbp[0] = S(12); lvTbw[0] = S(13);
        uint m1lo = S(24), m1hi = S(25), m2lo = S(26), m2hi = S(27);
        lvTbp[1] = m1lo & 0x3FFFu; lvTbw[1] = (m1lo >> 14) & 0x3Fu;
        lvTbp[2] = (m1lo >> 20) | ((m1hi & 0x3u) << 12); lvTbw[2] = (m1hi >> 2) & 0x3Fu;
        lvTbp[3] = (m1hi >> 8) & 0x3FFFu; lvTbw[3] = (m1hi >> 22) & 0x3Fu;
        lvTbp[4] = m2lo & 0x3FFFu; lvTbw[4] = (m2lo >> 14) & 0x3Fu;
        lvTbp[5] = (m2lo >> 20) | ((m2hi & 0x3u) << 12); lvTbw[5] = (m2hi >> 2) & 0x3Fu;
        lvTbp[6] = (m2hi >> 8) & 0x3FFFu; lvTbw[6] = (m2hi >> 22) & 0x3Fu;
        uvec4 ct;
#ifdef HIRES
        int hs = hiSource();
        if (hs >= 0)
            ct = sampleShadow(hs, u, v, (lod <= 0.0 ? mmag : (mmin & 1u)) == 1u);
        else
#endif
        if (lod <= 0.0)
            ct = sampleLevel(u, v, 0u, mmag == 1u); // "0.0 or less": level 0 with MMAG
        else if (mmin <= 1u)
            ct = sampleLevel(u, v, 0u, mmin == 1u);
        else if (mmin == 2u || mmin == 4u)
        {
            uint lvl = min(uint(floor(lod + 0.5)), mxl); // "Selection of m or m+1 by LOD (round-off)"
            ct = sampleLevel(u, v, lvl, mmin == 4u);
        }
        else if (mmin == 3u || mmin == 5u)
        {
            uint m = min(uint(floor(lod)), mxl), m1 = min(m + 1u, mxl);
            float f = m == mxl ? 0.0 : lod - floor(lod);
            vec4 c0 = vec4(sampleLevel(u, v, m, mmin == 5u)), c1 = vec4(sampleLevel(u, v, m1, mmin == 5u));
            ct = uvec4(floor(c0 + (c1 - c0) * f)); // manual silent on the blend precision
        }
        else
        {
            countSilent(SILENT_MMIN_UNDEFINED);
            ct = sampleLevel(u, v, 0u, false);
        }

        // 3.4.9 texture function: A*B = (A x B) >> 7, clamped to 0..0xff.
        uint tfx = S(18), tcc = S(17);
        uvec3 mod3 = min((ct.rgb * cf.rgb) >> 7u, uvec3(255u));
        if (tfx == 0u)
            cv = uvec4(mod3, tcc != 0u ? min((ct.a * cf.a) >> 7u, 255u) : cf.a);
        else if (tfx == 1u)
            cv = uvec4(ct.rgb, tcc != 0u ? ct.a : cf.a);
        else if (tfx == 2u)
            cv = uvec4(min(mod3 + uvec3(cf.a), uvec3(255u)), tcc != 0u ? min(ct.a + cf.a, 255u) : cf.a);
        else
            cv = uvec4(min(mod3 + uvec3(cf.a), uvec3(255u)), tcc != 0u ? ct.a : cf.a);
    }

    // ---- 3.5 fog: A*B = (A x B) >> 8 ----
    if (fge)
    {
        uint fc = S(41);
        uvec3 fcol = uvec3(fc & 0xFFu, (fc >> 8) & 0xFFu, (fc >> 16) & 0xFFu);
        cv.rgb = ((uvec3(fogF) * cv.rgb) >> 8u) + ((uvec3(255u - fogF) * fcol) >> 8u);
    }

    // ---- frame / Z buffer addresses ----
    uint fbp = S(1), fbw = S(2);
    fpsm = S(3);
    fbmsk = S(4);
    uint zbp = S(5);
    zpsm = S(6);
    zmsk = S(7);
    test = S(32);
    fb16 = fpsm == 2u || fpsm == 10u;
    pixelWord(fpsm, fbp * 32u, fbw, uint(pix.x), uint(pix.y), fword, fshift);
    pixelWord(zpsm, zbp * 32u, fbw, uint(pix.x), uint(pix.y), zword, zshift);
    ztst = (test >> 17) & 3u;
    if (((test >> 16) & 1u) == 0u)
        countSilent(SILENT_ZTE0); // "prohibited"; treated as the depth test as set

    // Z beyond the Z buffer format (manual silent): compared and stored saturated.
    uint zmax = zpsm == 49u ? 0xFFFFFFu : (zpsm == 50u || zpsm == 58u) ? 0xFFFFu : 0xFFFFFFFFu;
    if (z > zmax)
    {
        countSilent(SILENT_Z_RANGE);
        z = zmax;
    }

    // ---- 3.7.2 alpha test ----
    writeRgb = true;
    writeA = true;
    writeZ = true;
    if ((test & 1u) != 0u)
    {
        uint atst = (test >> 1) & 7u, aref = (test >> 4) & 0xFFu, a = cv.a;
        bool pass = atst == 1u || (atst == 2u && a < aref) || (atst == 3u && a <= aref) || (atst == 4u && a == aref) ||
                    (atst == 5u && a >= aref) || (atst == 6u && a > aref) || (atst == 7u && a != aref);
        if (!pass)
        {
            uint afail = (test >> 12) & 3u;
            if (afail == 3u && fpsm != 0u)
                afail = 1u; // RGB_ONLY is FB_ONLY except for RGBA32
            writeRgb = afail == 1u || afail == 3u;
            writeA = afail == 1u;
            writeZ = afail == 2u;
        }
    }

    // ---- 3.8 blending inputs ----
    uint alpha = S(30);
    fix = S(31) & 0xFFu;
    selA = alpha & 3u;
    selB = (alpha >> 2) & 3u;
    selC = (alpha >> 4) & 3u;
    selD = (alpha >> 6) & 3u;
    pabe = S(35) != 0u;
    colclamp = S(40) & 1u;
    dthe = S(39) & 1u;
    fba = S(33) & 1u;
    // FBMSK in the frame buffer's own bits (3.9.5): for RGBA16, FBMSK bits 7:3, 15:11, 23:19, 31.
    fmask16 = ((fbmsk >> 3) & 0x1Fu) | (((fbmsk >> 11) & 0x1Fu) << 5) | (((fbmsk >> 19) & 0x1Fu) << 10) | ((fbmsk >> 31) << 15);

    return true;
}

void gsWrite()
{
    bool pass = true;

#ifdef HIRES
    // Z shares the shadow word when ZBP == FBP (role 1) or is the shadowed buffer (role 2); otherwise it is read
    // from GS memory (the main pass keeps it) and not written here.
    bool zHi = uHiRole == 2 || S(5) == S(1);
    bool zImg = !zHi && uHiZMode != 0;
    gHiCur = imageLoad(uHiImg, gHiPix).r;
    uint fold = uHiRole == 1 ? gHiCur : mem[fword];
    uint zold = zHi ? gHiCur : zImg ? imageLoad(uHiZ, gHiPix).r : mem[zword];
    uint zsh = zImg ? 0u : zshift;
#else
    uint fold = mem[fword];
    uint zold = mem[zword];
#endif
#ifndef HIRES
    if (uDebug == 1)
        writeBits(fword, 0xFFFF00FFu, 0xFFFFFFFFu);
    else if (uDebug == 2)
        writeBits(fword, cv.r | (cv.g << 8) | (cv.b << 16) | (cv.a << 24), 0xFFFFFFFFu);
    else if (uDebug == 3 && gTme)
        writeBits(fword, (gLastRaw & 0xFFu) * 0x10101u | 0xFF000000u, 0xFFFFFFFFu);
    else if (uDebug == 4)
        writeBits(fword, cluts[S(21) * 512u + uint(pix.x & 511)] | 0xFF000000u, 0xFFFFFFFFu);
    if (uDebug != 0)
        pass = false;
#endif

    // Destination colour as the blend and the destination alpha test see it.
    uvec3 cd;
    uint ad;
    if (fb16)
    {
        uint p = (fold >> fshift) & 0xFFFFu;
        cd = uvec3((p & 0x1Fu) << 3, ((p >> 5) & 0x1Fu) << 3, ((p >> 10) & 0x1Fu) << 3); // manual silent: 5 bits << 3
        ad = (p & 0x8000u) != 0u ? 0x80u : 0u; // 3.8.1: "Alpha is treated as 0 when A is 0 and as 0x80 when A is 1"
    }
    else
    {
        cd = uvec3(fold & 0xFFu, (fold >> 8) & 0xFFu, (fold >> 16) & 0xFFu);
        ad = fpsm == 1u ? 0x80u : (fold >> 24); // RGB24: 0x80
    }

    // ---- 3.7.3 destination alpha test ----
    if (pass && ((test >> 14) & 1u) != 0u)
    {
        uint datm = (test >> 15) & 1u;
        if (fpsm == 0u)
            pass = ((fold >> 31) & 1u) == datm;
        else if (fb16)
            pass = (((fold >> fshift) >> 15) & 1u) == datm;
    }

    // ---- 3.7.4 depth test ----
    if (pass)
    {
#ifdef HIRES
        uint zs = zpsm == 49u ? (zold & 0xFFFFFFu) : (zpsm == 50u || zpsm == 58u) ? ((zold >> zsh) & 0xFFFFu) : zold;
#else
        uint zs = zpsm == 49u ? (zold & 0xFFFFFFu) : (zpsm == 50u || zpsm == 58u) ? ((zold >> zshift) & 0xFFFFu) : zold;
#endif
        if (ztst == 0u) pass = false;
        else if (ztst == 2u) pass = z >= zs;
        else if (ztst == 3u) pass = z > zs;
    }

    if (pass)
    {
        // ---- 3.8 alpha blending: (A - B) * C >> 7 + D, not clamped ----
        ivec3 res = ivec3(cv.rgb);
        bool blend = gAbe && !(pabe && (cv.a & 0x80u) == 0u);
        if (blend)
        {
            if (selA == 3u || selB == 3u || selC == 3u || selD == 3u)
                countSilent(SILENT_BLEND_RESERVED);
            ivec3 cs = ivec3(cv.rgb), cdi = ivec3(cd);
            ivec3 A = selA == 0u ? cs : selA == 1u ? cdi : ivec3(0);
            ivec3 B = selB == 0u ? cs : selB == 1u ? cdi : ivec3(0);
            int C = selC == 0u ? int(cv.a) : selC == 1u ? int(ad) : int(fix);
            ivec3 D = selD == 0u ? cs : selD == 1u ? cdi : ivec3(0);
            res = (((A - B) * C) >> 7) + D;
        }
        // 3.9.1 dithering (RGBA16), 3.9.2 colour clamp
        if (dthe != 0u)
        {
            if (fb16)
            {
                uint dimx = (pix.y & 2) == 0 ? S(37) : S(38);
                uint sh = uint(((pix.y & 1) * 4 + (pix.x & 3)) * 4);
                int dm = int(((dimx >> sh) & 7u) << 29) >> 29; // 1:2:0
                res += ivec3(dm);
            }
            else
                countSilent(SILENT_DITHER32); // "the result of dithering is not guaranteed": not applied
        }
        res = colclamp != 0u ? clamp(res, ivec3(0), ivec3(255)) : (res & ivec3(0xFF));
        uvec3 rgb = uvec3(res);

        // 3.9.3 alpha correction, 3.9.4 format conversion, 3.9.5 masking
        if (fb16)
        {
            uint abit = (cv.a >> 7) | fba;
            uint p = (rgb.r >> 3) | ((rgb.g >> 3) << 5) | ((rgb.b >> 3) << 10) | ((abit & 1u) << 15);
            uint m = ((writeRgb ? 0x7FFFu : 0u) | (writeA ? 0x8000u : 0u)) & ~fmask16;
#ifndef HIRES
            writeBits(fword, p << fshift, m << fshift);
#endif
        }
        else
        {
            uint a = fpsm == 0u ? (cv.a | (fba << 7)) : 0u;
            uint p = rgb.r | (rgb.g << 8) | (rgb.b << 16) | (a << 24);
            uint m = (writeRgb ? 0x00FFFFFFu : 0u) | ((writeA && fpsm == 0u) ? 0xFF000000u : 0u);
#ifdef HIRES
            if (uHiRole == 1)
            {
                uint mm = m & ~fbmsk;
                gHiCur = (gHiCur & ~mm) | (p & mm);
            }
#else
            writeBits(fword, p, m & ~fbmsk); // PSMCT24: bits 31:24 "Not used", left as they are
#endif
        }
        if (writeZ && zmsk == 0u)
        {
#ifdef HIRES
            // 32/24-bit Z only (16-bit Z on a shadowed buffer makes it stale instead, see HiClassify).
            if (zHi && zpsm != 50u && zpsm != 58u)
            {
                uint zm = zpsm == 49u ? 0xFFFFFFu : 0xFFFFFFFFu;
                gHiCur = (gHiCur & ~zm) | (z & zm);
            }
            else if (zImg)
            {
                uint zm = zpsm == 49u ? 0xFFFFFFu : (zpsm == 50u || zpsm == 58u) ? 0xFFFFu : 0xFFFFFFFFu;
                imageStore(uHiZ, gHiPix, uvec4((zold & ~zm) | (z & zm)));
            }
#else
            if (zpsm == 50u || zpsm == 58u)
                writeBits(zword, (z & 0xFFFFu) << zshift, 0xFFFFu << zshift);
            else if (zpsm == 49u)
                writeBits(zword, z, 0xFFFFFFu); // bits 31:24 "Not used"
            else
                writeBits(zword, z, 0xFFFFFFFFu);
#endif
        }
#ifdef HIRES
        imageStore(uHiImg, gHiPix, uvec4(gHiCur));
#endif
    }

}
)GLSL";

    const char *kDrawFragmentMain = R"GLSL(
flat in int vPrim;
void main()
{
    if (!gsShade(vPrim, ivec2(gl_FragCoord.xy)))
        discard;
    beginInvocationInterlockARB();
    gsWrite();
    endInvocationInterlockARB();
}
)GLSL";

    // High-resolution shadow pass: the primitives of the batch that draw into shadow buffer uHiTarget in role
    // uHiRole (prim word 27: frame-role mask, 28: Z-role mask), each over its bounding box in shadow pixels.
    const char *kHiVertex = R"GLSL(#version 430
layout(std430, binding = 2) readonly buffer Prims { uint prims[]; };
layout(std430, binding = 3) readonly buffer States { uint states[]; };
uniform int uHiTarget, uHiRole;
uniform vec2 uHiScale;
uniform ivec2 uHiSize;
uniform int uHiZMode; // 0: primitives that only write Z (prim word 29) have nothing to do in this pass
uniform float uHiSqueeze, uHiCenter; // prim word 30 set: drawn squeezed horizontally by uHiSqueeze about uHiCenter
flat out int vPrim;
flat out int vSq;

int wcoord(uint pk, int shiftBits, uint offset)
{
    return int((pk >> uint(shiftBits)) & 0xFFFFu) - int(offset);
}

void main()
{
    int prim = gl_VertexID / 6;
    int corner = gl_VertexID % 6;
    vPrim = prim;
    uint pb = uint(prim) * 32u;
    vSq = int(prims[pb + 30u]);
    uint roles = prims[pb + (uHiRole == 1 ? 27u : 28u)];
    if (prims[pb + 26u] != 0u || ((roles >> uint(uHiTarget)) & 1u) == 0u || (uHiZMode == 0 && prims[pb + 29u] != 0u))
    {
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
        return;
    }
    uint sb = prims[pb + 25u] * 48u;
    uint type = states[sb] & 7u;
    uint n = type == 0u ? 1u : (type == 1u || type == 2u || type == 6u) ? 2u : 3u;
    uint ofx = states[sb + 10u], ofy = states[sb + 11u];
    int minX = 1 << 30, minY = 1 << 30, maxX = -(1 << 30), maxY = -(1 << 30);
    for (uint i = 0u; i < n; ++i)
    {
        uint xy = prims[pb + i * 8u];
        int x = wcoord(xy, 0, ofx), y = wcoord(xy, 16, ofy);
        minX = min(minX, x); minY = min(minY, y);
        maxX = max(maxX, x); maxY = max(maxY, y);
    }
    int x0 = (minX >> 4) - 1, y0 = (minY >> 4) - 1;
    int x1 = (maxX >> 4) + 1, y1 = (maxY >> 4) + 1;
    uint sx = states[sb + 8u], sy = states[sb + 9u];
    x0 = max(x0, int(sx & 0xFFFFu)); x1 = min(x1, int(sx >> 16));
    y0 = max(y0, int(sy & 0xFFFFu)); y1 = min(y1, int(sy >> 16));
    x0 = max(x0, 0); y0 = max(y0, 0);
    if (x0 > x1 || y0 > y1)
    {
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
        return;
    }
    // GS pixels x0..x1 cover GS positions [x0 - 0.5, x1 + 0.5); shadow pixel X samples (X + 0.5) / scale - 0.5.
    float fx0 = float(x0), fx1 = float(x1 + 1);
    if (vSq != 0)
    {
        fx0 = uHiCenter + (fx0 - 0.5 - uHiCenter) * uHiSqueeze + 0.5;
        fx1 = uHiCenter + (fx1 - 0.5 - uHiCenter) * uHiSqueeze + 0.5;
    }
    int hx0 = max(int(floor(fx0 * uHiScale.x)) - 1, 0), hy0 = max(int(floor(float(y0) * uHiScale.y)) - 1, 0);
    int hx1 = min(int(ceil(fx1 * uHiScale.x)) + 1, uHiSize.x), hy1 = min(int(ceil(float(y1 + 1) * uHiScale.y)) + 1, uHiSize.y);
    if (hx0 >= hx1 || hy0 >= hy1)
    {
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
        return;
    }
    int cx = (corner == 1 || corner == 2 || corner == 4) ? hx1 : hx0;
    int cy = (corner == 2 || corner == 4 || corner == 5) ? hy1 : hy0;
    gl_Position = vec4(float(cx) / float(uHiSize.x) * 2.0 - 1.0, float(cy) / float(uHiSize.y) * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

    const char *kHiFragmentMain = R"GLSL(
flat in int vPrim;
flat in int vSq;
uniform float uHiSqueeze, uHiCenter;
void main()
{
    gHiPix = ivec2(gl_FragCoord.xy);
    // GS position of this shadow pixel's centre, in 1/16 pixels, and the GS pixel whose square holds it. A squeezed
    // primitive (vSq) is drawn as at 1x, then pulled towards the centre: its GS position is the inverse of that.
    vec2 g = gl_FragCoord.xy / uHiScale - 0.5;
    if (vSq != 0)
        g.x = uHiCenter + (g.x - uHiCenter) / uHiSqueeze;
    gHiC = g * 16.0;
    ivec2 gp = ivec2(floor((gHiC + 8.0) / 16.0));
    if (!gsShade(vPrim, gp))
        discard;
    beginInvocationInterlockARB();
    gsWrite();
    endInvocationInterlockARB();
}
)GLSL";

    // Shadow image rectangle from GS memory (PSMCT32, nearest): after writes the shadow passes do not mirror.
    const char *kHiResyncBody = R"GLSL(
layout(local_size_x = 16, local_size_y = 16) in;
layout(r32ui, binding = 1) writeonly uniform uimage2D uHiImg;
uniform uint uFbp, uFbw, uPsm; // buffer base (2048-word units), width, PSM (0 for a frame buffer)
uniform vec2 uScale;
uniform ivec4 uRect; // shadow pixels x, y, w, h
void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= uRect.z || p.y >= uRect.w)
        return;
    ivec2 X = uRect.xy + p;
    uvec2 g = uvec2(floor((vec2(X) + 0.5) / uScale));
    imageStore(uHiImg, X, uvec4(readPixel(uPsm, uFbp * 32u, uFbw, g.x & 2047u, g.y & 2047u)));
}
)GLSL";

    // Small-target primitives and CLUT loads in command order, before the batch's draw: one workgroup,
    // one invocation per pixel of the rectangle uRect (x, y, w, h); every item ends with a workgroup
    // barrier, so each item sees all writes of the items before it.
    const char *kSequenceMain = R"GLSL(
layout(local_size_x = 64) in; // most items are a few pixels: a small group keeps each barrier cheap
layout(std430, binding = 8) readonly buffer Seq { uint seq[]; };
uniform uint uSeqCount;
void main()
{
    int t = int(gl_LocalInvocationID.x);
    for (uint i = 0u; i < uSeqCount; ++i)
    {
        uint b = i * 12u;
        if (seq[b] == 0u)
        {
            // A primitive: the pixels of its bounding box (x, y, w, h in words 2-5), 1024 at a time.
            int bx = int(seq[b + 2u]), by = int(seq[b + 3u]), bw = int(seq[b + 4u]), bh = int(seq[b + 5u]);
            for (int k = t; k < bw * bh; k += 64)
            {
                if (gsShade(int(seq[b + 1u]), ivec2(bx + k % bw, by + k / bw)))
                    gsWrite();
            }
        }
        else
        for (int idxLoop = t; idxLoop < 512; idxLoop += 64)
        {
            // 3.4.7 CLUT load: snapshot dst = snapshot src with the loaded entries.
            uint cbp = seq[b + 1u], cpsm = seq[b + 2u], csm = seq[b + 3u], csa = seq[b + 4u], entries = seq[b + 5u];
            uint cou = seq[b + 6u], cov = seq[b + 7u], cbw = seq[b + 8u], src = seq[b + 9u], dst = seq[b + 10u];
            uint idx = uint(idxLoop);
            uint value = cluts[src * 512u + idx];
            bool sixteen = cpsm == 2u || cpsm == 10u;
            bool high = !sixteen && idx >= 256u;
            uint e = sixteen ? ((idx - csa * 16u) & 511u) : (((idx & 255u) - csa * 16u) & 255u);
            if (e < entries)
            {
                uint x, y, bw;
                if (csm == 0u)
                {
                    uint sw = (e & ~0x18u) | ((e & 0x08u) << 1) | ((e & 0x10u) >> 1);
                    x = sw & 0x0Fu;
                    y = sw >> 4;
                    bw = 1u;
                }
                else
                {
                    x = cou * 16u + e;
                    y = cov;
                    bw = max(cbw, 1u);
                }
                uint raw = readPixel(cpsm, cbp, bw, x, y);
                value = sixteen ? (raw & 0xFFFFu) : high ? (raw >> 16) : (raw & 0xFFFFu);
            }
            cluts[dst * 512u + idx] = value;
        }
        memoryBarrierBuffer();
        barrier();
    }
}
)GLSL";

    // 3.4.7 load from the CLUT buffer in local memory into the temporary buffer, on the GPU in
    // command order (after the draws before it, before the draws after it).
    const char *kClutLoadBody = R"GLSL(
layout(local_size_x = 512) in;
layout(std430, binding = 4) buffer Cluts { uint cluts[]; };
uniform uint uSrc, uDst, uCbp, uCpsm, uCsm, uCsa, uEntries, uCou, uCov, uCbw;
// The temporary buffer is kept as snapshots (512 x 16-bit entries each) so draws queued before and
// after a load each read the buffer as it was for them. Snapshot uDst = uSrc with this load applied.
void main()
{
    uint i = gl_LocalInvocationID.x;
    uint value = cluts[uSrc * 512u + i];
    bool sixteen = uCpsm == 2u || uCpsm == 10u;
    bool high = !sixteen && i >= 256u;
    uint e = sixteen ? ((i - uCsa * 16u) & 511u) : (((i & 255u) - uCsa * 16u) & 255u);
    if (e < uEntries)
    {
        uint x, y, bw;
        if (uCsm == 0u)
        {
            // CSM1: at the start of the CLUT buffer, 8 x 2 (IDTEX4) or 16 x 16 with entries 8-15 and
            // 16-23 swapped (IDTEX8) (2.7).
            uint sw = (e & ~0x18u) | ((e & 0x08u) << 1) | ((e & 0x10u) >> 1);
            x = sw & 0x0Fu;
            y = sw >> 4;
            bw = 1u;
        }
        else
        {
            // CSM2: a row at (COU * 16, COV) of a buffer of width CBW (manual silent on CBW 0: width 1).
            x = uCou * 16u + e;
            y = uCov;
            bw = max(uCbw, 1u);
        }
        uint raw = readPixel(uCpsm, uCbp, bw, x, y);
        // "loaded to the entry address (CSA multiplied by 16)"; manual silent past the end: wraps.
        value = sixteen ? (raw & 0xFFFFu) : high ? (raw >> 16) : (raw & 0xFFFFu);
    }
    cluts[uDst * 512u + i] = value;
}
)GLSL";

    // 4.3 host->local transmission: pixel uStart + i of the transfer rectangle (left to right, top to
    // bottom from (DSAX, DSAY), RRW wide) takes uBits bits of the transmitted data (RGBA32 4 bytes,
    // RGB24 3 bytes, RGBA16 2 bytes, IDTEX8 1 byte, IDTEX4 a nibble, low nibble first), written into GS
    // memory with the destination PSM's layout.
    const char *kXferBody = R"GLSL(
layout(local_size_x = 256) in;
layout(std430, binding = 7) readonly buffer Img { uint img[]; };
uniform uint uDbp, uDbw, uDpsm, uDsax, uDsay, uRrw, uStart, uCount, uBits;
void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= uCount)
        return;
    uint p = uStart + i;
    uint x = (uDsax + p % uRrw) & 2047u; // transfer coordinates wrap at 2048
    uint y = (uDsay + p / uRrw) & 2047u;
    uint bit = i * uBits;
    uint w = bit >> 5u, o = bit & 31u;
    uint v = img[w] >> o;
    if (o + uBits > 32u)
        v |= img[w + 1u] << (32u - o);
    if (uBits < 32u)
        v &= (1u << uBits) - 1u;
    uint psm = uDpsm;
    uint bits = pixelBits(psm, uDbp, uDbw, x, y);
    uint addr = byteAddress(psm, bits);
    uint pb = psmInfo[psm * 8u + 5u];
    uint word = addr >> 2u, sh = (addr & 3u) * 8u;
    if (pb == 4u)
        writeBits(word, v, (psm == 1u || psm == 49u) ? 0xFFFFFFu : 0xFFFFFFFFu); // 24-bit: bits 31:24 kept
    else if (pb == 2u)
        writeBits(word, (v & 0xFFFFu) << sh, 0xFFFFu << sh);
    else if (psmInfo[psm * 8u + 6u] != 0u)
    {
        uint nsh = sh + (bits & 4u);
        writeBits(word, (v & 15u) << nsh, 15u << nsh);
    }
    else
        writeBits(word, (v & 255u) << sh, 255u << sh);
}
)GLSL";

    // Native pictures (gs_native_layer.h): one invocation per pixel of the target rectangle writes the
    // texture's texel as a PSMCT32 pixel (all 32 bits).
    const char *kTexWriteBody = R"GLSL(
layout(local_size_x = 16, local_size_y = 16) in;
uniform sampler2D uTex;
uniform uint uFbp, uFbw, uX, uY, uW, uH;
void main()
{
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= uW || p.y >= uH)
        return;
    vec4 c = texelFetch(uTex, ivec2(p), 0);
    uvec4 b = uvec4(round(clamp(c, 0.0, 1.0) * 255.0));
    uint word, shift;
    pixelWord(0u, uFbp * 32u, uFbw, (uX + p.x) & 2047u, (uY + p.y) & 2047u, word, shift);
    writeBits(word, b.r | (b.g << 8u) | (b.b << 16u) | (b.a << 24u), 0xFFFFFFFFu);
}
)GLSL";

    const char *kTexReadBody = R"GLSL(
layout(local_size_x = 16, local_size_y = 16) in;
layout(rgba8, binding = 0) uniform writeonly image2D uImg;
uniform uint uFbp, uFbw, uX, uY, uW, uH;
void main()
{
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= uW || p.y >= uH)
        return;
    uint word, shift;
    pixelWord(0u, uFbp * 32u, uFbw, (uX + p.x) & 2047u, (uY + p.y) & 2047u, word, shift);
    uint v = mem[word];
    imageStore(uImg, ivec2(p), vec4(float(v & 255u), float((v >> 8u) & 255u), float((v >> 16u) & 255u), float(v >> 24u)) / 255.0);
}
)GLSL";

    const char *kCrtcVertex = R"GLSL(#version 430
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

    // 5.2 PCRTC: canvas pixel (x, y) is VCK x0 + x * uVckStep and output line y0 + y. Interlaced
    // modes refresh only the lines of the current field (the TV keeps the other field's lines).
    const char *kCrtcFragmentBody = R"GLSL(
uniform ivec2 uOrigin;      // VCK, line of canvas (0, 0)
uniform int uVckStep;
uniform int uCanvasH;
uniform int uInterlace, uFfmd, uField;
uniform int uAllLines; // 1: every line each present (interlace display switched off), not only this field's
uniform int uEnable[2];
uniform ivec4 uDisplay[2];  // DX, DY, DW, DH
uniform ivec2 uMag[2];      // MAGH + 1, MAGV + 1
uniform uvec4 uDispFb[2];   // FBP, FBW, PSM, 0
uniform ivec2 uDb[2];       // DBX, DBY
uniform int uMmod, uAmod, uSlbg, uAlp;
uniform uvec3 uBg;
// High-resolution canvas: uHiCanvas canvas pixels per PS2 canvas pixel ((1, 1): the PS2's own). A circuit whose
// buffer has a shadow image (uHiSrc >= 0, scale uHiSrcScale) is read from it at the fractional buffer position.
uniform vec2 uHiCanvas;
uniform int uHiSrc[2];
uniform vec2 uHiSrcScale[2];
uniform sampler2D uHiSrc0, uHiSrc1;
out vec4 oColor;

// One read circuit at output position (vck, line) (fvck, fline: the same, fractional): colour, alpha, inside its
// rectangle.
bool circuit(int c, int vck, int line, float fvck, float fline, out uvec3 rgb, out uint a)
{
    rgb = uvec3(0u);
    a = 0u;
    ivec4 d = uDisplay[c];
    if (uEnable[c] == 0 || vck < d.x || vck > d.x + d.z || line < d.y || line > d.y + d.w)
        return false;
    int k = line - d.y;
    int by;
    if (uInterlace != 0)
    {
        // Manual silent on DH in interlaced modes: DH + 1 counts output lines of both fields.
        int fl = k >> 1;
        by = uFfmd != 0 ? fl / uMag[c].y                  // FRAME mode: every line from the start
                        : 2 * (fl / uMag[c].y) + (k & 1);  // FIELD mode: every other line
    }
    else
        by = k / uMag[c].y;
    int bx = (vck - d.x) / uMag[c].x;
    uint psm = uDispFb[c].z;
    if (uHiSrc[c] >= 0)
    {
        float fk = fline - float(d.y);
        float fby = (uInterlace != 0 && uFfmd != 0) ? fk * 0.5 / float(uMag[c].y) : fk / float(uMag[c].y);
        float fbx = (fvck - float(d.x)) / float(uMag[c].x);
        vec2 p = (vec2(float(uDb[c].x), float(uDb[c].y)) + vec2(fbx, fby)) * uHiSrcScale[c];
        vec4 t = c == 0 ? texelFetch(uHiSrc0, clamp(ivec2(floor(p)), ivec2(0), textureSize(uHiSrc0, 0) - 1), 0)
                        : texelFetch(uHiSrc1, clamp(ivec2(floor(p)), ivec2(0), textureSize(uHiSrc1, 0) - 1), 0);
        uvec4 v = uvec4(clamp(floor(t * 255.0 + 0.5), 0.0, 255.0));
        rgb = v.rgb;
        a = psm == 1u ? 0x80u : v.a;
        return true;
    }
    uint raw = readPixel(psm, uDispFb[c].x * 32u, uDispFb[c].y, uint(uDb[c].x + bx) & 2047u, uint(uDb[c].y + by) & 2047u);
    if (psm == 2u || psm == 10u)
    {
        rgb = uvec3((raw & 0x1Fu) << 3, ((raw >> 5) & 0x1Fu) << 3, ((raw >> 10) & 0x1Fu) << 3); // EDCBA000
        a = (raw & 0x8000u) != 0u ? 0x80u : 0u;
    }
    else
    {
        rgb = uvec3(raw & 0xFFu, (raw >> 8) & 0xFFu, (raw >> 16) & 0xFFu);
        a = psm == 1u ? 0x80u : (raw >> 24);
    }
    return true;
}

void main()
{
    vec2 f = gl_FragCoord.xy / uHiCanvas; // PS2 canvas position
    int x = int(floor(f.x)), y = int(floor(f.y));
    int line = uOrigin.y + y;
    if (uInterlace != 0 && uAllLines == 0 && ((line - uOrigin.y) & 1) != uField)
        discard;
    int vck = uOrigin.x + x * uVckStep;
    float fvck = float(uOrigin.x) + f.x * float(uVckStep), fline = float(uOrigin.y) + f.y;
    uvec3 c1, c2;
    uint a1, a2;
    bool in1 = circuit(0, vck, line, fvck, fline, c1, a1);
    bool in2 = circuit(1, vck, line, fvck, fline, c2, a2);
    // OUT2: circuit 2 inside its rectangle, else BGCOLOR.
    uvec3 out2 = in2 ? c2 : uBg;
    uvec3 rd = uSlbg != 0 ? uBg : out2;
    uvec3 o = rd;
    if (in1)
    {
        uint al = uMmod != 0 ? uint(uAlp) : min(a1 * 2u, 255u); // circuit alpha: doubled, clamped
        // (Rs x A + Rd x (0xff - A)) x 256 / 255, then 8 bits. Manual silent on rounding: truncation.
        uvec3 sum = c1 * al + rd * (255u - al);
        o = (sum * 256u / 255u) >> 8u;
    }
    // Manual silent on OUT1 outside circuit 1: OUT2 / BGCOLOR as selected.
    oColor = vec4(vec3(o) / 255.0, 1.0);
}
)GLSL";

    unsigned compile(GLenum type, const std::string &source)
    {
        const GLuint shader = glCreateShader(type);
        const char *src = source.c_str();
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[8192];
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            std::cerr << "[gs-exact] shader compile failed: " << log << std::endl;
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    unsigned link(unsigned vs, unsigned fs)
    {
        if (!vs || !fs)
            return 0;
        const GLuint program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            char log[8192];
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            std::cerr << "[gs-exact] program link failed: " << log << std::endl;
            glDeleteProgram(program);
            return 0;
        }
        return program;
    }

    bool isIndexed(uint32_t psm)
    {
        return psm == GS_PSM_T8 || psm == GS_PSM_T4 || psm == GS_PSM_T8H || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }

    void pageSize(uint32_t psm, uint32_t &w, uint32_t &h)
    {
        uint32_t pw = 64, ph = 32;
        if (!GSMem::PageTableData(psm, pw, ph))
            pw = 64, ph = 32;
        w = pw;
        h = ph;
    }

    // Pages a w x h rectangle at (x, y) of a buffer at block bp can touch (GSGpuRenderer's pageSpan).
    void pageSpan(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t &first,
                  uint32_t &count)
    {
        uint32_t pw, ph;
        pageSize(psm, pw, ph);
        const uint32_t perRow = (std::max<uint32_t>(bw, 1u) * 64u) / pw;
        auto page = [&](uint32_t px, uint32_t py) { return bp / 32u + (py / ph) * std::max<uint32_t>(perRow, 1u) + px / pw; };
        first = page(x, y);
        const uint32_t last = page(x + std::max<uint32_t>(w, 1u) - 1u, y + std::max<uint32_t>(h, 1u) - 1u) + ((bp % 32u) ? 1u : 0u);
        count = std::min<uint32_t>(last - first + 1u, 512u);
    }

    const char *kSilentNames[] = {"z-beyond-zbuf-format", "line-diamond-edge", "blend-reserved-selector", "dither-on-32bit-frame",
                                  "zte-0", "degenerate-triangle", "point-halfway", "negative-texcoord",
                                  "mmin-6-7", "q-zero", "aa1-band-not-drawn"};

    void exportPng(std::vector<uint8_t> &&pixels, int width, int height, std::string path)
    {
        std::thread([pixels = std::move(pixels), width, height, path = std::move(path)]() mutable
                    {
                        Image image{};
                        image.data = pixels.data();
                        image.width = width;
                        image.height = height;
                        image.mipmaps = 1;
                        image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                        ExportImage(image, path.c_str()); })
            .detach();
    }
}

GSExactRenderer::GSExactRenderer(GSCpuBackend &cpu) : m_cpu(cpu)
{
    m_cpuNewer.fill(true); // the first draw sends all of GS memory
    std::cerr << "[gs-exact] GS by the GS User's Manual rules (PS2X_GS_BACKEND=exact), 1x" << std::endl;
}

GSExactRenderer::~GSExactRenderer() = default;

void GSExactRenderer::EnsureGl()
{
    if (m_glReady || m_glFailed)
        return;
    GLint major = 0, minor = 0, count = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    bool interlock = false;
    for (GLint i = 0; i < count; ++i)
    {
        const char *ext = reinterpret_cast<const char *>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        interlock |= ext && std::strcmp(ext, "GL_ARB_fragment_shader_interlock") == 0;
    }
    if (!interlock || major < 4 || (major == 4 && minor < 3))
    {
        std::cerr << "[gs-exact] needs GL 4.3 and GL_ARB_fragment_shader_interlock (have GL " << major << "." << minor
                  << ", interlock " << (interlock ? "yes" : "no") << "): nothing will be drawn" << std::endl;
        m_glFailed = true;
        return;
    }
    m_drawProgram = link(compile(GL_VERTEX_SHADER, kDrawVertex),
                         compile(GL_FRAGMENT_SHADER, std::string(kDrawFragmentHead) + kCommonGlsl + kDrawFragmentBody + kDrawFragmentMain));
    {
        const unsigned cs = compile(GL_COMPUTE_SHADER, std::string("#version 430\n") + kCommonGlsl + kDrawFragmentBody + kSequenceMain);
        if (cs)
        {
            m_seqProgram = glCreateProgram();
            glAttachShader(m_seqProgram, cs);
            glLinkProgram(m_seqProgram);
            glDeleteShader(cs);
            GLint ok = 0;
            glGetProgramiv(m_seqProgram, GL_LINK_STATUS, &ok);
            if (!ok)
            {
                char log[4096];
                glGetProgramInfoLog(m_seqProgram, sizeof(log), nullptr, log);
                std::cerr << "[gs-exact] sequence program link failed: " << log << std::endl;
                glDeleteProgram(m_seqProgram);
                m_seqProgram = 0;
            }
        }
        glGenBuffers(1, &m_seqBuf);
    }
    m_crtcProgram = link(compile(GL_VERTEX_SHADER, kCrtcVertex),
                         compile(GL_FRAGMENT_SHADER, std::string("#version 430\n") + kCommonGlsl + kCrtcFragmentBody));
    {
        const unsigned cs = compile(GL_COMPUTE_SHADER, std::string("#version 430\n") + kCommonGlsl + kClutLoadBody);
        if (cs)
        {
            m_clutProgram = glCreateProgram();
            glAttachShader(m_clutProgram, cs);
            glLinkProgram(m_clutProgram);
            glDeleteShader(cs);
            GLint ok = 0;
            glGetProgramiv(m_clutProgram, GL_LINK_STATUS, &ok);
            if (!ok)
            {
                glDeleteProgram(m_clutProgram);
                m_clutProgram = 0;
            }
        }
    }
    {
        const unsigned cs = compile(GL_COMPUTE_SHADER, std::string("#version 430\n") + kCommonGlsl + kXferBody);
        if (cs)
        {
            m_xferProgram = glCreateProgram();
            glAttachShader(m_xferProgram, cs);
            glLinkProgram(m_xferProgram);
            glDeleteShader(cs);
            GLint ok = 0;
            glGetProgramiv(m_xferProgram, GL_LINK_STATUS, &ok);
            if (!ok)
            {
                glDeleteProgram(m_xferProgram);
                m_xferProgram = 0;
            }
        }
        glGenBuffers(1, &m_imgBuf);
    }
    {
        const unsigned cs = compile(GL_COMPUTE_SHADER, std::string("#version 430\n") + kCommonGlsl + kTexWriteBody);
        if (cs)
        {
            m_texWriteProgram = glCreateProgram();
            glAttachShader(m_texWriteProgram, cs);
            glLinkProgram(m_texWriteProgram);
            glDeleteShader(cs);
            GLint ok = 0;
            glGetProgramiv(m_texWriteProgram, GL_LINK_STATUS, &ok);
            if (!ok)
            {
                glDeleteProgram(m_texWriteProgram);
                m_texWriteProgram = 0; // native pictures unsupported; the rest works
            }
        }
    }
    {
        const unsigned cs = compile(GL_COMPUTE_SHADER, std::string("#version 430\n") + kCommonGlsl + kTexReadBody);
        if (cs)
        {
            m_texReadProgram = glCreateProgram();
            glAttachShader(m_texReadProgram, cs);
            glLinkProgram(m_texReadProgram);
            glDeleteShader(cs);
            GLint ok = 0;
            glGetProgramiv(m_texReadProgram, GL_LINK_STATUS, &ok);
            if (!ok)
            {
                glDeleteProgram(m_texReadProgram);
                m_texReadProgram = 0;
            }
        }
    }
    if (!m_drawProgram || !m_crtcProgram || !m_clutProgram || !m_xferProgram || !m_seqProgram)
    {
        m_glFailed = true;
        return;
    }

    // Swizzle tables of every PSM code, and the per-code layout constants (8 words each).
    std::vector<uint32_t> swz;
    std::vector<uint32_t> info(64u * 8u, 0u);
    std::map<const uint16_t *, uint32_t> tableOffset;
    for (uint32_t psm = 0; psm < 64u; ++psm)
    {
        uint32_t pw = 0, ph = 0;
        const uint16_t *table = GSMem::PageTableData(psm, pw, ph);
        if (!table)
            table = GSMem::PageTableData(GS_PSM_CT32, pw, ph); // undefined PSM codes (manual: "undefined")
        auto it = tableOffset.find(table);
        if (it == tableOffset.end())
        {
            it = tableOffset.emplace(table, static_cast<uint32_t>(swz.size())).first;
            const size_t n = static_cast<size_t>(pw) * ph * 32u;
            for (size_t i = 0; i < n; ++i)
                swz.push_back(table[i]);
        }
        const bool nibble = psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
        const bool sixteen = psm == GS_PSM_CT16 || psm == GS_PSM_CT16S || psm == GS_PSM_Z16 || psm == GS_PSM_Z16S;
        const uint32_t unpacked = psm == GS_PSM_T4 ? 4u : psm == GS_PSM_T8 ? 8u : sixteen ? 16u : 32u;
        const uint32_t bitOffset = (psm == GS_PSM_T8H || psm == GS_PSM_T4HL) ? 24u : psm == GS_PSM_T4HH ? 28u : 0u;
        const uint32_t packed = (psm == GS_PSM_T4 || psm == GS_PSM_T8 || psm == GS_PSM_T8H || nibble) ? 1u : sixteen ? 2u : 4u;
        uint32_t *e = &info[psm * 8u];
        e[0] = it->second;
        e[1] = pw;
        e[2] = ph;
        e[3] = unpacked;
        e[4] = bitOffset;
        e[5] = packed;
        e[6] = nibble ? 1u : 0u;
    }

    auto makeBuffer = [](unsigned &buf, size_t bytes, const void *data)
    {
        glGenBuffers(1, &buf);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
        glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(bytes), data, GL_DYNAMIC_DRAW);
    };
    makeBuffer(m_memBuf, 4u * 1024u * 1024u, nullptr);
    makeBuffer(m_swzBuf, swz.size() * 4u, swz.data());
    makeBuffer(m_psmBuf, info.size() * 4u, info.data());
    m_swzCpu = swz;
    m_psmInfoCpu = info;
    makeBuffer(m_primBuf, 4u, nullptr);
    makeBuffer(m_stateBuf, 4u, nullptr);
    const std::vector<uint32_t> clutZero(static_cast<size_t>(kClutWords) * kClutSnapshots, 0u);
    makeBuffer(m_clutBuf, clutZero.size() * 4u, clutZero.data());
    const std::array<uint32_t, 16> zeros{};
    makeBuffer(m_counterBuf, sizeof(zeros), zeros.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    // Upload ring (see StreamTo): persistently mapped, coherent, written by the CPU, copied by the GPU.
    if (glBufferStorage && glMapBufferRange && glFenceSync && glClientWaitSync && glCopyBufferSubData &&
        std::getenv("PS2X_GS_EXACT_NO_RING") == nullptr)
    {
        glGenBuffers(1, &m_ringBuf);
        glBindBuffer(GL_COPY_READ_BUFFER, m_ringBuf);
        const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        glBufferStorage(GL_COPY_READ_BUFFER, static_cast<GLsizeiptr>(kRingBytes), nullptr, flags);
        m_ringPtr = static_cast<uint8_t *>(glMapBufferRange(GL_COPY_READ_BUFFER, 0, static_cast<GLsizeiptr>(kRingBytes), flags));
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
        if (!m_ringPtr)
        {
            glDeleteBuffers(1, &m_ringBuf);
            m_ringBuf = 0;
        }
        m_ringHead = 0;
        m_ringSeg = 0;
    }
    std::cerr << "[gs-exact] upload ring " << (m_ringPtr ? "on" : "off (glBufferSubData)") << std::endl;
    // CLUT readback buffer (PollClutReadbacks): GPU copies of GPU-made snapshots, read once their fence signalled.
    if (glBufferStorage && glMapBufferRange && glFenceSync && glClientWaitSync && glCopyBufferSubData)
    {
        glGenBuffers(1, &m_rbBuf);
        glBindBuffer(GL_COPY_WRITE_BUFFER, m_rbBuf);
        const GLbitfield flags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        glBufferStorage(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(kRbSlots * kClutWords * 4u), nullptr, flags);
        m_rbPtr = static_cast<uint8_t *>(glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, static_cast<GLsizeiptr>(kRbSlots * kClutWords * 4u), flags));
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        if (!m_rbPtr)
        {
            glDeleteBuffers(1, &m_rbBuf);
            m_rbBuf = 0;
        }
    }

    glGenVertexArrays(1, &m_vao);
    glGenFramebuffers(1, &m_drawFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_drawFbo);
    glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_WIDTH, 2048);
    glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_HEIGHT, 2048);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // High-resolution shadow buffers (PS2X_GS_HIRES=0: off, everything at the GS's 1x as before).
    {
        const char *v = std::getenv("PS2X_GS_HIRES");
        if (!(v && v[0] == '0'))
        {
            m_hiProgram = link(compile(GL_VERTEX_SHADER, kHiVertex),
                               compile(GL_FRAGMENT_SHADER, std::string(kDrawFragmentHead) + "#define HIRES\n" + kCommonGlsl + kDrawFragmentBody +
                                                               kHiFragmentMain));
            const unsigned cs = compile(GL_COMPUTE_SHADER, std::string("#version 430\n") + kCommonGlsl + kHiResyncBody);
            if (cs)
            {
                m_hiResyncProgram = glCreateProgram();
                glAttachShader(m_hiResyncProgram, cs);
                glLinkProgram(m_hiResyncProgram);
                glDeleteShader(cs);
                GLint ok = 0;
                glGetProgramiv(m_hiResyncProgram, GL_LINK_STATUS, &ok);
                if (!ok)
                {
                    glDeleteProgram(m_hiResyncProgram);
                    m_hiResyncProgram = 0;
                }
            }
            if (m_hiProgram && m_hiResyncProgram)
            {
                glGenFramebuffers(1, &m_hiFbo);
                glBindFramebuffer(GL_FRAMEBUFFER, m_hiFbo);
                glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_WIDTH, 4096);
                glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_HEIGHT, 4096);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glGenFramebuffers(2, m_hiBlitFbo);
                const uint32_t fbps[kHiTargets] = {224u, 0u, 112u};
                const uint32_t gw[kHiTargets] = {496u, 512u, 512u}, gh[kHiTargets] = {384u, 448u, 448u};
                for (int i = 0; i < kHiTargets; ++i)
                {
                    HiTarget &t = m_hi[i];
                    t.fbp = fbps[i];
                    t.gsW = gw[i];
                    t.gsH = gh[i];
                    pageSpan(GS_PSM_CT32, t.fbp * 32u, 8u, 0u, 0u, t.gsW, t.gsH, t.firstPage, t.pageCount);
                }
                m_hiOn = true;
                std::cerr << "[gs-exact] high-resolution shadow buffers on (PS2X_GS_HIRES=0: off)" << std::endl;
            }
            else
                std::cerr << "[gs-exact] high-resolution shadow programs failed: off" << std::endl;
        }
    }
    m_cpuNewer.fill(true);
    m_gpuNewer.fill(false);
    ++m_batchGen;
    m_glReady = true;
    std::cerr << "[gs-exact] GL " << major << "." << minor << " on " << reinterpret_cast<const char *>(glGetString(GL_RENDERER))
              << std::endl;
}

void GSExactRenderer::Release()
{
    if (!m_glReady)
        return;
    for (void *&f : m_ringFence)
        if (f)
            glDeleteSync(static_cast<GLsync>(f)), f = nullptr;
    for (ClutReadback &rb : m_clutRbInFlight)
        if (rb.fence)
            glDeleteSync(static_cast<GLsync>(rb.fence));
    m_clutRbInFlight.clear();
    if (m_rbBuf)
    {
        glBindBuffer(GL_COPY_WRITE_BUFFER, m_rbBuf);
        glUnmapBuffer(GL_COPY_WRITE_BUFFER);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        glDeleteBuffers(1, &m_rbBuf);
    }
    m_rbBuf = 0;
    m_rbPtr = nullptr;
    if (m_ringBuf)
    {
        glBindBuffer(GL_COPY_READ_BUFFER, m_ringBuf);
        glUnmapBuffer(GL_COPY_READ_BUFFER);
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
        glDeleteBuffers(1, &m_ringBuf);
    }
    m_ringBuf = 0;
    m_ringPtr = nullptr;
    m_primCap = m_stateCap = m_seqCap = m_imgCap = 0;
    for (unsigned *b : {&m_memBuf, &m_swzBuf, &m_primBuf, &m_stateBuf, &m_clutBuf, &m_counterBuf, &m_psmBuf})
        if (*b)
            glDeleteBuffers(1, b), *b = 0;
    glDeleteProgram(m_drawProgram);
    glDeleteProgram(m_crtcProgram);
    glDeleteProgram(m_clutProgram);
    glDeleteProgram(m_seqProgram);
    if (m_seqBuf)
        glDeleteBuffers(1, &m_seqBuf);
    m_seqBuf = 0;
    glDeleteProgram(m_xferProgram);
    if (m_imgBuf)
        glDeleteBuffers(1, &m_imgBuf);
    m_imgBuf = 0;
    glDeleteVertexArrays(1, &m_vao);
    glDeleteFramebuffers(1, &m_drawFbo);
    if (m_hiProgram)
        glDeleteProgram(m_hiProgram);
    if (m_hiResyncProgram)
        glDeleteProgram(m_hiResyncProgram);
    m_hiProgram = m_hiResyncProgram = 0;
    if (m_hiFbo)
        glDeleteFramebuffers(1, &m_hiFbo);
    if (m_hiZTex)
        glDeleteTextures(1, &m_hiZTex);
    m_hiZTex = 0;
    m_hiZW = m_hiZH = 0;
    if (m_hiBlitFbo[0])
        glDeleteFramebuffers(2, m_hiBlitFbo);
    m_hiFbo = m_hiBlitFbo[0] = m_hiBlitFbo[1] = 0;
    for (HiTarget &t : m_hi)
    {
        if (t.tex)
            glDeleteTextures(1, &t.tex);
        t.tex = 0;
        t.w = t.h = 0;
        t.valid = false;
    }
    m_hiOn = false;
    if (m_canvasTex)
        glDeleteTextures(1, &m_canvasTex);
    if (m_canvasFbo)
        glDeleteFramebuffers(1, &m_canvasFbo);
    m_canvasTex = m_canvasFbo = 0;
    m_presentValid = false;
    m_glReady = false;
}

void GSExactRenderer::Reset()
{
    m_prims.clear();
    m_states.clear();
    m_haveLastState = false;
    m_batchHasFrame = false;
    m_batchWritten.fill(false);
        m_batchRead.fill(false);
        ++m_batchGen;
    m_cbp0 = m_cbp1 = 0; // manual silent on reset values
    m_pendingLoads.clear();
    m_seq.clear();
    m_seqHasRect = false;
    m_mainPrims = 0u;
    m_snapOps.clear();
    m_snapData.clear();
    m_clutMirror.fill(0u);
    m_clutUnknownGen.fill(0u); // snapshot m_clutSnap is zero on both sides (zeroed below when GL is up)
    m_clutUnknownCount = 0u;
    m_clutRbPending.clear();
    for (ClutReadback &rb : m_clutRbInFlight)
        if (rb.fence)
            glDeleteSync(static_cast<GLsync>(rb.fence));
    m_clutRbInFlight.clear();
    m_haveLastLoad = false;
    if (m_glReady)
    {
        const std::vector<uint32_t> zero(kClutWords, 0u);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_clutBuf);
        StreamTo(m_clutBuf, static_cast<size_t>(m_clutSnap) * kClutWords * 4u, zero.data(), zero.size() * 4u);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }
    m_cpuNewer.fill(true);
    m_gpuNewer.fill(false);
    ++m_batchGen;
    m_presentValid = false;
}

void GSExactRenderer::BeginHostWork()
{
    EnsureGl();
    glGetIntegerv(GL_VIEWPORT, m_savedViewport);
    m_cullWasEnabled = glIsEnabled(GL_CULL_FACE);
}

// Leave GL the way raylib expects it (raylib caches its own state).
void GSExactRenderer::EndHostWork()
{
    NoteFlush("endhost");
    FlushDraws();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(m_savedViewport[0], m_savedViewport[1], m_savedViewport[2], m_savedViewport[3]);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(0);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (m_cullWasEnabled)
        glEnable(GL_CULL_FACE);
    else
        glDisable(GL_CULL_FACE);
}

// ---- CPU <-> GPU page sync ---------------------------------------------------------------------

void GSExactRenderer::UploadDirtyPages()
{
    if (!m_vram)
        return;
    bool barrier = false;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_memBuf);
    for (uint32_t p = 0; p < kPageCount;)
    {
        if (!m_cpuNewer[p])
        {
            ++p;
            continue;
        }
        if (!barrier)
        {
            // GS memory is written by shaders too: the copies must come after those writes (once per call).
            glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
            barrier = true;
        }
        uint32_t end = p;
        while (end < kPageCount && m_cpuNewer[end])
            m_cpuNewer[end++] = false;
        StreamTo(m_memBuf, static_cast<size_t>(p) * kPageBytes, m_vram + static_cast<size_t>(p) * kPageBytes,
                 static_cast<size_t>(end - p) * kPageBytes);
        m_statUploadPages += end - p;
        p = end;
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void GSExactRenderer::DownloadPages(uint32_t first, uint32_t count)
{
    if (!m_glReady || !m_vram)
        return;
    NoteFlush("download");
    FlushDraws();
    bool barrier = false;
    for (uint32_t i = 0; i < count;)
    {
        const uint32_t p = (first + i) % kPageCount;
        if (!m_gpuNewer[p])
        {
            ++i;
            continue;
        }
        if (!barrier)
        {
            glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_memBuf);
            barrier = true;
        }
        uint32_t n = 0;
        while (i + n < count && (first + i + n) % kPageCount >= p && m_gpuNewer[(first + i + n) % kPageCount])
        {
            m_gpuNewer[(first + i + n) % kPageCount] = false;
            ++m_batchGen;
            ++n;
        }
        const auto rb0 = std::chrono::steady_clock::now();
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(p) * kPageBytes, static_cast<GLsizeiptr>(n) * kPageBytes,
                           m_vram + static_cast<size_t>(p) * kPageBytes);
        m_statReadbackSec += std::chrono::duration<double>(std::chrono::steady_clock::now() - rb0).count();
        ++m_statReadbacks;
        m_statDownloadPages += n;
        i += n;
    }
    if (barrier)
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void GSExactRenderer::DownloadOverlapping(uint32_t first, uint32_t count, const char *why)
{
    const uint64_t before = m_statDownloadPages;
    DownloadPages(first, count);
    if (m_statDownloadPages != before)
        m_counts[std::string("download:") + (why ? why : "?")] += m_statDownloadPages - before;
}

void GSExactRenderer::DownloadAll(const char *why)
{
    const uint64_t before = m_statDownloadPages;
    DownloadPages(0, kPageCount);
    if (m_statDownloadPages != before)
        m_counts[std::string("download-all:") + (why ? why : "?")] += m_statDownloadPages - before;
}

void GSExactRenderer::DownloadForPresent(const GSPresentationRequest &)
{
    DownloadPages(0, kPageCount);
}

void GSExactRenderer::InvalidatePages(uint32_t first, uint32_t count)
{
    FlushPendingEpoch();
    ++m_epochCounter;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t p = (first + i) % kPageCount;
        m_pageEpoch[p] = m_epochCounter;
        m_cpuNewer[p] = true;
        m_gpuNewer[p] = false;
    }
    HiTouchPages(first, count);
    ++m_batchGen;
}

void GSExactRenderer::InvalidateAll()
{
    InvalidatePages(0, kPageCount);
}

// ---- CPU execution of small-target primitives -------------------------------------------------------
// A line-for-line port of gsShade/gsWrite (kDrawFragmentBody) for the primitives the sequence would run:
// tiny draws into 64-pixel-wide buffers (the robot palette lines into FBP 411, ~1600 per frame). On the
// GPU each of them is a dependent step (~8 us); here ~1 us. Same rules, same arithmetic: doubles where the
// shader uses doubles, floats where it uses floats, no FP contraction (the shader marks those expressions
// `precise`). Only cases whose result cannot depend on a GPU-only approximation are taken (no log2 LOD
// choice, no CLUT lookup); PS2X_GS_EXACT_CPU_CHECK=1 compares every result with the GPU's.
#pragma clang fp contract(off)

namespace
{
    struct CpuPixelState
    {
        int pixX = 0, pixY = 0;
        bool abe = false, tme = false;
        uint32_t fword = 0, fshift = 0, zword = 0, zshift = 0, z = 0, test = 0, fpsm = 0, fbmsk = 0, fmask16 = 0;
        uint32_t zpsm = 0, zmsk = 0, ztst = 0, selA = 0, selB = 0, selC = 0, selD = 0, fix = 0, colclamp = 0, dthe = 0, fba = 0;
        bool fb16 = false, pabe = false, writeRgb = true, writeA = true, writeZ = true;
        uint32_t cv[4] = {0, 0, 0, 0};
    };

    double cpuFloorDiv(double num, double den)
    {
        double q = std::floor(num / den);
        if (q * den > num)
            q -= 1.0;
        else if ((q + 1.0) * den <= num)
            q += 1.0;
        return q;
    }
    double cpuLerpFloor(double a0, double a1, double n, double d) { return a0 + cpuFloorDiv((a1 - a0) * n, d); }

    struct CpuGs
    {
        uint32_t *mem = nullptr;            // 4 MiB of GS memory as words
        const uint32_t *swz = nullptr;      // m_swzCpu
        const uint32_t *info = nullptr;     // m_psmInfoCpu
        const uint32_t *pr = nullptr;       // primitive words (kPrimWords)
        const uint32_t *st = nullptr;       // state words (kStateWords)
        int ofsX = 0, ofsY = 0;
        uint32_t lvTbp[7]{}, lvTbw[7]{};
        const uint32_t *clut = nullptr;     // the CLUT snapshot S(21) (CPU mirror; all entries known when used)

        uint32_t S(uint32_t i) const { return st[i]; }
        uint32_t vXY(uint32_t i) const { return pr[i * 8u]; }
        int vX(uint32_t i) const { return static_cast<int>(vXY(i) & 0xFFFFu) - ofsX; }
        int vY(uint32_t i) const { return static_cast<int>(vXY(i) >> 16) - ofsY; }
        uint32_t vZ(uint32_t i) const { return pr[i * 8u + 1u]; }
        uint32_t vRGBA(uint32_t i) const { return pr[i * 8u + 2u]; }
        float vF32(uint32_t i, uint32_t k) const
        {
            const uint32_t bits = pr[i * 8u + k] & 0xFFFFFF00u;
            float f;
            std::memcpy(&f, &bits, 4);
            return f;
        }
        uint32_t vUV(uint32_t i) const { return pr[i * 8u + 6u]; }
        uint32_t vFog(uint32_t i) const { return pr[i * 8u + 7u]; }

        uint32_t pixelBits(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const
        {
            const uint32_t b = psm * 8u;
            const uint32_t toff = info[b], pw = info[b + 1u], ph = info[b + 2u], ub = info[b + 3u], bo = info[b + 4u];
            const uint32_t bwv = std::max(bw, 1u);
            const uint32_t page = bp / 32u + (y / ph) * ((bwv * 64u) / pw) + x / pw;
            const uint32_t offs = swz[toff + (x % pw) + ((bp % 32u) * ph + y % ph) * pw];
            return (page * pw * ph + offs) * ub + bo;
        }
        uint32_t byteAddress(uint32_t psm, uint32_t bits) const { return (bits >> 3u) & (4194304u - info[psm * 8u + 5u]); }
        uint32_t readPixel(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const
        {
            const uint32_t bits = pixelBits(psm, bp, bw, x, y);
            const uint32_t addr = byteAddress(psm, bits);
            const uint32_t word = mem[addr >> 2u];
            const uint32_t pb = info[psm * 8u + 5u];
            if (pb == 4u)
                return word;
            const uint32_t shift = (addr & 3u) * 8u;
            if (pb == 2u)
                return (word >> shift) & 0xFFFFu;
            const uint32_t v = (word >> shift) & 0xFFu;
            return info[psm * 8u + 6u] != 0u ? ((v >> (bits & 7u)) & 0xFu) : v;
        }
        void pixelWord(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t &word, uint32_t &shift) const
        {
            const uint32_t addr = byteAddress(psm, pixelBits(psm, bp, bw, x, y));
            word = addr >> 2u;
            shift = (addr & 3u) * 8u;
        }
        void writeBits(uint32_t word, uint32_t value, uint32_t mask)
        {
            if (mask == 0u)
                return;
            mem[word] = (mem[word] & ~mask) | (value & mask);
        }

        static int wrap(int c, uint32_t size, uint32_t mode, uint32_t lo, uint32_t hi, uint32_t lvl)
        {
            if (mode == 0u) return c & static_cast<int>(size - 1u);
            if (mode == 1u) return std::clamp(c, 0, static_cast<int>(size) - 1);
            if (mode == 2u) return std::clamp(c, static_cast<int>(lo >> lvl), static_cast<int>(hi >> lvl));
            return (c & static_cast<int>(lo)) | static_cast<int>(hi);
        }
        static void expand16(uint32_t raw, uint32_t texa, uint32_t o[4])
        {
            const uint32_t r = (raw & 0x1Fu) << 3, g = ((raw >> 5) & 0x1Fu) << 3, b = ((raw >> 10) & 0x1Fu) << 3;
            const uint32_t ta0 = texa & 0xFFu, aem = (texa >> 8) & 1u, ta1 = (texa >> 16) & 0xFFu;
            uint32_t a;
            if ((raw & 0x8000u) != 0u)
                a = ta1;
            else
                a = (aem != 0u && r == 0u && g == 0u && b == 0u) ? 0u : ta0;
            o[0] = r, o[1] = g, o[2] = b, o[3] = a;
        }
        static void expand24(uint32_t raw, uint32_t texa, uint32_t o[4])
        {
            const uint32_t r = raw & 0xFFu, g = (raw >> 8) & 0xFFu, b = (raw >> 16) & 0xFFu;
            const uint32_t ta0 = texa & 0xFFu, aem = (texa >> 8) & 1u;
            o[0] = r, o[1] = g, o[2] = b, o[3] = (aem != 0u && r == 0u && g == 0u && b == 0u) ? 0u : ta0;
        }
        static void expand32(uint32_t raw, uint32_t o[4])
        {
            o[0] = raw & 0xFFu, o[1] = (raw >> 8) & 0xFFu, o[2] = (raw >> 16) & 0xFFu, o[3] = raw >> 24;
        }
        // As the shader's texel(): wrapping, format conversion and the CLUT (3.4.6, 3.4.7).
        void texel(int u, int v, uint32_t lvl, uint32_t o[4]) const
        {
            const uint32_t psm = S(14), texa = S(34);
            const uint32_t usz = std::max((1u << S(15)) >> lvl, 1u), vsz = std::max((1u << S(16)) >> lvl, 1u);
            const uint32_t clampLo = S(28), clampHi = S(29);
            const uint32_t wms = clampLo & 3u, wmt = (clampLo >> 2) & 3u;
            const uint32_t minu = (clampLo >> 4) & 0x3FFu, maxu = (clampLo >> 14) & 0x3FFu;
            const uint32_t minv = ((clampLo >> 24) | (clampHi << 8)) & 0x3FFu, maxv = (clampHi >> 2) & 0x3FFu;
            u = wrap(u, usz, wms, minu, maxu, lvl);
            v = wrap(v, vsz, wmt, minv, maxv, lvl);
            const uint32_t raw = readPixel(psm, lvTbp[lvl], lvTbw[lvl], static_cast<uint32_t>(u) & 2047u, static_cast<uint32_t>(v) & 2047u);
            if (psm == 19u || psm == 20u || psm == 27u || psm == 36u || psm == 44u)
            {
                // Index into the temporary buffer from entry CSA * 16; past the end the index wraps (manual silent).
                const uint32_t cpsm = S(19), csa = S(20);
                if (cpsm == 2u || cpsm == 10u)
                {
                    expand16(clut[(csa * 16u + raw) & 511u], texa, o);
                    return;
                }
                const uint32_t e = (csa * 16u + raw) & 255u;
                const uint32_t c32 = clut[e] | (clut[256u + e] << 16);
                if (cpsm == 1u)
                    expand24(c32 & 0xFFFFFFu, texa, o);
                else
                    expand32(c32, o);
                return;
            }
            if (psm == 1u || psm == 49u)
                expand24(raw & 0xFFFFFFu, texa, o);
            else if (psm == 2u || psm == 10u || psm == 50u || psm == 58u)
                expand16(raw, texa, o);
            else
                expand32(raw, o);
        }
        void sampleLevel(float u0, float v0, uint32_t lvl, bool bilinear, uint32_t o[4]) const
        {
            const float scale = 1.0f / static_cast<float>(1u << lvl);
            const float u = u0 * scale, v = v0 * scale;
            if (!bilinear)
            {
                texel(static_cast<int>(std::floor(u)), static_cast<int>(std::floor(v)), lvl, o);
                return;
            }
            const float su = u - 0.5f, sv = v - 0.5f;
            const int iu = static_cast<int>(std::floor(su)), iv = static_cast<int>(std::floor(sv));
            const float a = su - static_cast<float>(iu), b = sv - static_cast<float>(iv);
            uint32_t ta[4], tb[4], tc[4], td[4];
            texel(iu, iv, lvl, ta);
            texel(iu + 1, iv, lvl, tb);
            texel(iu, iv + 1, lvl, tc);
            texel(iu + 1, iv + 1, lvl, td);
            for (int k = 0; k < 4; ++k)
            {
                const float c = (1.0f - a) * (1.0f - b) * static_cast<float>(ta[k]) + a * (1.0f - b) * static_cast<float>(tb[k]) +
                                (1.0f - a) * b * static_cast<float>(tc[k]) + a * b * static_cast<float>(td[k]);
                o[k] = static_cast<uint32_t>(std::floor(c));
            }
        }

        // gsShade: false when the pixel is not drawn.
        bool shade(int px, int py, CpuPixelState &ps)
        {
            ps.pixX = px, ps.pixY = py;
            const uint32_t primBits = S(0);
            const uint32_t type = primBits & 7u;
            const bool iip = ((primBits >> 3) & 1u) != 0u, tme = ((primBits >> 4) & 1u) != 0u, fge = ((primBits >> 5) & 1u) != 0u;
            const bool abe = ((primBits >> 6) & 1u) != 0u, fst = ((primBits >> 8) & 1u) != 0u;
            ofsX = static_cast<int>(S(10));
            ofsY = static_cast<int>(S(11));
            ps.abe = abe;
            ps.tme = tme;
            const uint32_t scx = S(8), scy = S(9);
            if (px < static_cast<int>(scx & 0xFFFFu) || px > static_cast<int>(scx >> 16) || py < static_cast<int>(scy & 0xFFFFu) ||
                py > static_cast<int>(scy >> 16))
                return false;
            const int cx = px * 16, cy = py * 16;
            const uint32_t scanmsk = S(36) & 3u;
            if ((scanmsk == 2u && (py & 1) == 0) || (scanmsk == 3u && (py & 1) == 1))
                return false;

            double w0 = 1.0, w1 = 0.0, w2 = 0.0;
            double sx = 0.0, sy = 0.0;
            double ln = 0.0, ld = 1.0;
            double snx = 0.0, sdx = 1.0, sny = 0.0, sdy = 1.0;
            uint32_t last = 0u;
            if (type == 0u)
            {
                const int x = vX(0u), y = vY(0u);
                if (((x + 8) >> 4) != px || ((y + 8) >> 4) != py)
                    return false;
            }
            else if (type == 1u || type == 2u)
            {
                const double ax = vX(0u), ay = vY(0u), bx = vX(1u), by = vY(1u);
                const double dx = bx - ax, dy = by - ay;
                const double pcx = cx, pcy = cy;
                double best = std::abs(ax - pcx) + std::abs(ay - pcy);
                best = std::min(best, std::abs(bx - pcx) + std::abs(by - pcy));
                if (dx != 0.0)
                {
                    const double t = (pcx - ax) / dx;
                    if (t > 0.0 && t < 1.0)
                        best = std::min(best, std::abs(ay + t * dy - pcy));
                }
                if (dy != 0.0)
                {
                    const double t = (pcy - ay) / dy;
                    if (t > 0.0 && t < 1.0)
                        best = std::min(best, std::abs(ax + t * dx - pcx));
                }
                if (best >= 8.0)
                    return false;
                if (std::abs(bx - pcx) + std::abs(by - pcy) < 8.0)
                    return false;
                const bool xMajor = std::abs(dx) >= std::abs(dy);
                ln = xMajor ? pcx - ax : pcy - ay;
                ld = xMajor ? dx : dy;
                if (ld < 0.0)
                {
                    ln = -ln;
                    ld = -ld;
                }
                ln = std::clamp(ln, 0.0, ld);
                const double t = ln / ld;
                w0 = 1.0 - t;
                w1 = t;
                last = 1u;
            }
            else if (type == 6u)
            {
                const int x0 = vX(0u), y0 = vY(0u), x1 = vX(1u), y1 = vY(1u);
                if (cx < std::min(x0, x1) || cx >= std::max(x0, x1) || cy < std::min(y0, y1) || cy >= std::max(y0, y1))
                    return false;
                snx = static_cast<double>(cx - x0);
                sdx = static_cast<double>(x1 - x0);
                sny = static_cast<double>(cy - y0);
                sdy = static_cast<double>(y1 - y0);
                if (sdx < 0.0) { snx = -snx; sdx = -sdx; }
                if (sdy < 0.0) { sny = -sny; sdy = -sdy; }
                sx = static_cast<double>(cx - x0) / static_cast<double>(x1 - x0);
                sy = static_cast<double>(cy - y0) / static_cast<double>(y1 - y0);
                last = 1u;
            }
            else
            {
                const double px0 = vX(0u), py0 = vY(0u), px1 = vX(1u), py1 = vY(1u), px2 = vX(2u), py2 = vY(2u);
                const double area = (px1 - px0) * (py2 - py0) - (py1 - py0) * (px2 - px0);
                if (area == 0.0)
                    return false;
                const double sgn = area > 0.0 ? 1.0 : -1.0;
                const double pcx = cx, pcy = cy;
                const double e0 = sgn * ((px2 - px1) * (pcy - py1) - (py2 - py1) * (pcx - px1));
                const double e1 = sgn * ((px0 - px2) * (pcy - py2) - (py0 - py2) * (pcx - px2));
                const double e2 = sgn * ((px1 - px0) * (pcy - py0) - (py1 - py0) * (pcx - px0));
                const double d0x = sgn * (px2 - px1), d0y = sgn * (py2 - py1);
                const double d1x = sgn * (px0 - px2), d1y = sgn * (py0 - py2);
                const double d2x = sgn * (px1 - px0), d2y = sgn * (py1 - py0);
                const bool in0 = e0 > 0.0 || (e0 == 0.0 && (d0y < 0.0 || (d0y == 0.0 && d0x > 0.0)));
                const bool in1 = e1 > 0.0 || (e1 == 0.0 && (d1y < 0.0 || (d1y == 0.0 && d1x > 0.0)));
                const bool in2 = e2 > 0.0 || (e2 == 0.0 && (d2y < 0.0 || (d2y == 0.0 && d2x > 0.0)));
                if (!(in0 && in1 && in2))
                    return false;
                const double inv = 1.0 / (sgn * area);
                w0 = e0 * inv;
                w1 = e1 * inv;
                w2 = e2 * inv;
                last = 2u;
            }

            uint32_t rgbaF;
            if (iip && (type == 1u || type == 2u))
            {
                const uint32_t r0 = vRGBA(0u), r1 = vRGBA(1u);
                uint32_t ci[4];
                for (uint32_t k = 0u; k < 4u; ++k)
                    ci[k] = static_cast<uint32_t>(std::clamp(cpuLerpFloor(static_cast<double>((r0 >> (8u * k)) & 0xFFu),
                                                                          static_cast<double>((r1 >> (8u * k)) & 0xFFu), ln, ld), 0.0, 255.0));
                rgbaF = ci[0] | (ci[1] << 8) | (ci[2] << 16) | (ci[3] << 24);
            }
            else if (iip && type != 0u && type != 6u)
            {
                float c[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                for (uint32_t i = 0u; i < 3u; ++i)
                {
                    const double w = i == 0u ? w0 : i == 1u ? w1 : w2;
                    if (w == 0.0)
                        continue;
                    const uint32_t rgba = vRGBA(i);
                    const float fw = static_cast<float>(w);
                    c[0] += fw * static_cast<float>(rgba & 0xFFu);
                    c[1] += fw * static_cast<float>((rgba >> 8) & 0xFFu);
                    c[2] += fw * static_cast<float>((rgba >> 16) & 0xFFu);
                    c[3] += fw * static_cast<float>(rgba >> 24);
                }
                uint32_t ci[4];
                for (int k = 0; k < 4; ++k)
                    ci[k] = static_cast<uint32_t>(std::clamp(std::floor(c[k] + 1.0e-4f), 0.0f, 255.0f));
                rgbaF = ci[0] | (ci[1] << 8) | (ci[2] << 16) | (ci[3] << 24);
            }
            else
                rgbaF = vRGBA(last);
            const uint32_t cf[4] = {rgbaF & 0xFFu, (rgbaF >> 8) & 0xFFu, (rgbaF >> 16) & 0xFFu, rgbaF >> 24};

            double zd;
            uint32_t fogF;
            if (type == 6u)
            {
                zd = static_cast<double>(vZ(1u));
                fogF = vFog(1u);
            }
            else if (type == 0u)
            {
                zd = static_cast<double>(vZ(0u));
                fogF = vFog(0u);
            }
            else if (type == 1u || type == 2u)
            {
                zd = cpuLerpFloor(static_cast<double>(vZ(0u)), static_cast<double>(vZ(1u)), ln, ld);
                fogF = static_cast<uint32_t>(std::clamp(cpuLerpFloor(static_cast<double>(vFog(0u)), static_cast<double>(vFog(1u)), ln, ld), 0.0, 255.0));
            }
            else
            {
                zd = w0 * static_cast<double>(vZ(0u)) + w1 * static_cast<double>(vZ(1u)) + w2 * static_cast<double>(vZ(2u));
                const double f = w0 * static_cast<double>(vFog(0u)) + w1 * static_cast<double>(vFog(1u)) + w2 * static_cast<double>(vFog(2u));
                fogF = static_cast<uint32_t>(std::clamp(std::floor(f + 1.0e-6), 0.0, 255.0));
            }
            zd = std::clamp(std::floor(zd + 1.0e-6), 0.0, 4294967295.0);
            ps.z = static_cast<uint32_t>(zd);

            for (int k = 0; k < 4; ++k)
                ps.cv[k] = cf[k];
            if (tme)
            {
                float u, v;
                if (fst)
                {
                    double uu, vv;
                    if (type == 6u)
                    {
                        uu = cpuLerpFloor(static_cast<double>(vUV(0u) & 0xFFFFu), static_cast<double>(vUV(1u) & 0xFFFFu), snx, sdx);
                        vv = cpuLerpFloor(static_cast<double>(vUV(0u) >> 16), static_cast<double>(vUV(1u) >> 16), sny, sdy);
                    }
                    else if (type == 1u || type == 2u)
                    {
                        uu = cpuLerpFloor(static_cast<double>(vUV(0u) & 0xFFFFu), static_cast<double>(vUV(1u) & 0xFFFFu), ln, ld);
                        vv = cpuLerpFloor(static_cast<double>(vUV(0u) >> 16), static_cast<double>(vUV(1u) >> 16), ln, ld);
                    }
                    else
                    {
                        uu = w0 * static_cast<double>(vUV(0u) & 0xFFFFu) + w1 * static_cast<double>(vUV(1u) & 0xFFFFu) + w2 * static_cast<double>(vUV(2u) & 0xFFFFu);
                        vv = w0 * static_cast<double>(vUV(0u) >> 16) + w1 * static_cast<double>(vUV(1u) >> 16) + w2 * static_cast<double>(vUV(2u) >> 16);
                    }
                    u = static_cast<float>(uu / 16.0);
                    v = static_cast<float>(vv / 16.0);
                }
                else
                {
                    double s, t, qs, qt;
                    if (type == 6u)
                    {
                        s = static_cast<double>(vF32(0u, 3u)) + sx * (static_cast<double>(vF32(1u, 3u)) - static_cast<double>(vF32(0u, 3u)));
                        t = static_cast<double>(vF32(0u, 4u)) + sy * (static_cast<double>(vF32(1u, 4u)) - static_cast<double>(vF32(0u, 4u)));
                        qs = static_cast<double>(vF32(0u, 5u)) + sx * (static_cast<double>(vF32(1u, 5u)) - static_cast<double>(vF32(0u, 5u)));
                        qt = static_cast<double>(vF32(0u, 5u)) + sy * (static_cast<double>(vF32(1u, 5u)) - static_cast<double>(vF32(0u, 5u)));
                    }
                    else
                    {
                        s = w0 * static_cast<double>(vF32(0u, 3u)) + w1 * static_cast<double>(vF32(1u, 3u)) + w2 * static_cast<double>(vF32(2u, 3u));
                        t = w0 * static_cast<double>(vF32(0u, 4u)) + w1 * static_cast<double>(vF32(1u, 4u)) + w2 * static_cast<double>(vF32(2u, 4u));
                        qs = w0 * static_cast<double>(vF32(0u, 5u)) + w1 * static_cast<double>(vF32(1u, 5u)) + w2 * static_cast<double>(vF32(2u, 5u));
                        qt = qs;
                    }
                    if (qs == 0.0 || qt == 0.0)
                    {
                        qs = qs == 0.0 ? 1.0 : qs;
                        qt = qt == 0.0 ? 1.0 : qt;
                    }
                    u = static_cast<float>(s / qs * static_cast<double>(1u << S(15)));
                    v = static_cast<float>(t / qt * static_cast<double>(1u << S(16)));
                }
                u = std::clamp(u, -2047.0f, 2047.0f);
                v = std::clamp(v, -2047.0f, 2047.0f);
                // Only level 0 with one filter (eligibility: MMAG == MMIN <= 1), so the LOD does not matter.
                const uint32_t tex1Lo = S(22);
                const uint32_t mmag = (tex1Lo >> 5) & 1u;
                lvTbp[0] = S(12);
                lvTbw[0] = S(13);
                uint32_t ct[4];
                sampleLevel(u, v, 0u, mmag == 1u, ct);
                const uint32_t tfx = S(18), tcc = S(17);
                uint32_t mod3[3];
                for (int k = 0; k < 3; ++k)
                    mod3[k] = std::min((ct[k] * cf[k]) >> 7u, 255u);
                if (tfx == 0u)
                    ps.cv[0] = mod3[0], ps.cv[1] = mod3[1], ps.cv[2] = mod3[2], ps.cv[3] = tcc != 0u ? std::min((ct[3] * cf[3]) >> 7u, 255u) : cf[3];
                else if (tfx == 1u)
                    ps.cv[0] = ct[0], ps.cv[1] = ct[1], ps.cv[2] = ct[2], ps.cv[3] = tcc != 0u ? ct[3] : cf[3];
                else if (tfx == 2u)
                {
                    for (int k = 0; k < 3; ++k)
                        ps.cv[k] = std::min(mod3[k] + cf[3], 255u);
                    ps.cv[3] = tcc != 0u ? std::min(ct[3] + cf[3], 255u) : cf[3];
                }
                else
                {
                    for (int k = 0; k < 3; ++k)
                        ps.cv[k] = std::min(mod3[k] + cf[3], 255u);
                    ps.cv[3] = tcc != 0u ? ct[3] : cf[3];
                }
            }
            if (fge)
            {
                const uint32_t fc = S(41);
                const uint32_t fcol[3] = {fc & 0xFFu, (fc >> 8) & 0xFFu, (fc >> 16) & 0xFFu};
                for (int k = 0; k < 3; ++k)
                    ps.cv[k] = ((fogF * ps.cv[k]) >> 8u) + (((255u - fogF) * fcol[k]) >> 8u);
            }

            const uint32_t fbp = S(1), fbw = S(2);
            ps.fpsm = S(3);
            ps.fbmsk = S(4);
            const uint32_t zbp = S(5);
            ps.zpsm = S(6);
            ps.zmsk = S(7);
            ps.test = S(32);
            ps.fb16 = ps.fpsm == 2u || ps.fpsm == 10u;
            pixelWord(ps.fpsm, fbp * 32u, fbw, static_cast<uint32_t>(px), static_cast<uint32_t>(py), ps.fword, ps.fshift);
            pixelWord(ps.zpsm, zbp * 32u, fbw, static_cast<uint32_t>(px), static_cast<uint32_t>(py), ps.zword, ps.zshift);
            ps.ztst = (ps.test >> 17) & 3u;
            const uint32_t zmax = ps.zpsm == 49u ? 0xFFFFFFu : (ps.zpsm == 50u || ps.zpsm == 58u) ? 0xFFFFu : 0xFFFFFFFFu;
            if (ps.z > zmax)
                ps.z = zmax;
            ps.writeRgb = true, ps.writeA = true, ps.writeZ = true;
            if ((ps.test & 1u) != 0u)
            {
                const uint32_t atst = (ps.test >> 1) & 7u, aref = (ps.test >> 4) & 0xFFu, a = ps.cv[3];
                const bool pass = atst == 1u || (atst == 2u && a < aref) || (atst == 3u && a <= aref) || (atst == 4u && a == aref) ||
                                  (atst == 5u && a >= aref) || (atst == 6u && a > aref) || (atst == 7u && a != aref);
                if (!pass)
                {
                    uint32_t afail = (ps.test >> 12) & 3u;
                    if (afail == 3u && ps.fpsm != 0u)
                        afail = 1u;
                    ps.writeRgb = afail == 1u || afail == 3u;
                    ps.writeA = afail == 1u;
                    ps.writeZ = afail == 2u;
                }
            }
            const uint32_t alpha = S(30);
            ps.fix = S(31) & 0xFFu;
            ps.selA = alpha & 3u, ps.selB = (alpha >> 2) & 3u, ps.selC = (alpha >> 4) & 3u, ps.selD = (alpha >> 6) & 3u;
            ps.pabe = S(35) != 0u;
            ps.colclamp = S(40) & 1u;
            ps.dthe = S(39) & 1u;
            ps.fba = S(33) & 1u;
            ps.fmask16 = ((ps.fbmsk >> 3) & 0x1Fu) | (((ps.fbmsk >> 11) & 0x1Fu) << 5) | (((ps.fbmsk >> 19) & 0x1Fu) << 10) | ((ps.fbmsk >> 31) << 15);
            return true;
        }

        // gsWrite.
        void write(const CpuPixelState &ps)
        {
            bool pass = true;
            const uint32_t fold = mem[ps.fword];
            const uint32_t zold = mem[ps.zword];
            uint32_t cd[3], ad;
            if (ps.fb16)
            {
                const uint32_t p = (fold >> ps.fshift) & 0xFFFFu;
                cd[0] = (p & 0x1Fu) << 3, cd[1] = ((p >> 5) & 0x1Fu) << 3, cd[2] = ((p >> 10) & 0x1Fu) << 3;
                ad = (p & 0x8000u) != 0u ? 0x80u : 0u;
            }
            else
            {
                cd[0] = fold & 0xFFu, cd[1] = (fold >> 8) & 0xFFu, cd[2] = (fold >> 16) & 0xFFu;
                ad = ps.fpsm == 1u ? 0x80u : (fold >> 24);
            }
            if (pass && ((ps.test >> 14) & 1u) != 0u)
            {
                const uint32_t datm = (ps.test >> 15) & 1u;
                if (ps.fpsm == 0u)
                    pass = ((fold >> 31) & 1u) == datm;
                else if (ps.fb16)
                    pass = (((fold >> ps.fshift) >> 15) & 1u) == datm;
            }
            if (pass)
            {
                const uint32_t zs = ps.zpsm == 49u ? (zold & 0xFFFFFFu) : (ps.zpsm == 50u || ps.zpsm == 58u) ? ((zold >> ps.zshift) & 0xFFFFu) : zold;
                if (ps.ztst == 0u)
                    pass = false;
                else if (ps.ztst == 2u)
                    pass = ps.z >= zs;
                else if (ps.ztst == 3u)
                    pass = ps.z > zs;
            }
            if (!pass)
                return;
            int res[3] = {static_cast<int>(ps.cv[0]), static_cast<int>(ps.cv[1]), static_cast<int>(ps.cv[2])};
            const bool blend = ps.abe && !(ps.pabe && (ps.cv[3] & 0x80u) == 0u);
            if (blend)
            {
                const int C = ps.selC == 0u ? static_cast<int>(ps.cv[3]) : ps.selC == 1u ? static_cast<int>(ad) : static_cast<int>(ps.fix);
                for (int k = 0; k < 3; ++k)
                {
                    const int cs = static_cast<int>(ps.cv[k]), cdi = static_cast<int>(cd[k]);
                    const int A = ps.selA == 0u ? cs : ps.selA == 1u ? cdi : 0;
                    const int B = ps.selB == 0u ? cs : ps.selB == 1u ? cdi : 0;
                    const int D = ps.selD == 0u ? cs : ps.selD == 1u ? cdi : 0;
                    res[k] = (((A - B) * C) >> 7) + D;
                }
            }
            if (ps.dthe != 0u && ps.fb16)
            {
                const uint32_t dimx = (ps.pixY & 2) == 0 ? S(37) : S(38);
                const uint32_t sh = static_cast<uint32_t>(((ps.pixY & 1) * 4 + (ps.pixX & 3)) * 4);
                const int dm = static_cast<int>(((dimx >> sh) & 7u) << 29) >> 29;
                for (int k = 0; k < 3; ++k)
                    res[k] += dm;
            }
            uint32_t rgb[3];
            for (int k = 0; k < 3; ++k)
                rgb[k] = static_cast<uint32_t>(ps.colclamp != 0u ? std::clamp(res[k], 0, 255) : (res[k] & 0xFF));
            if (ps.fb16)
            {
                const uint32_t abit = (ps.cv[3] >> 7) | ps.fba;
                const uint32_t p = (rgb[0] >> 3) | ((rgb[1] >> 3) << 5) | ((rgb[2] >> 3) << 10) | ((abit & 1u) << 15);
                const uint32_t m = ((ps.writeRgb ? 0x7FFFu : 0u) | (ps.writeA ? 0x8000u : 0u)) & ~ps.fmask16;
                writeBits(ps.fword, p << ps.fshift, m << ps.fshift);
            }
            else
            {
                const uint32_t a = ps.fpsm == 0u ? (ps.cv[3] | (ps.fba << 7)) : 0u;
                const uint32_t p = rgb[0] | (rgb[1] << 8) | (rgb[2] << 16) | (a << 24);
                const uint32_t m = (ps.writeRgb ? 0x00FFFFFFu : 0u) | ((ps.writeA && ps.fpsm == 0u) ? 0xFF000000u : 0u);
                writeBits(ps.fword, p, m & ~ps.fbmsk);
            }
            if (ps.writeZ && ps.zmsk == 0u)
            {
                if (ps.zpsm == 50u || ps.zpsm == 58u)
                    writeBits(ps.zword, (ps.z & 0xFFFFu) << ps.zshift, 0xFFFFu << ps.zshift);
                else if (ps.zpsm == 49u)
                    writeBits(ps.zword, ps.z, 0xFFFFFFu);
                else
                    writeBits(ps.zword, ps.z, 0xFFFFFFFFu);
            }
        }
    };
}

#pragma clang fp contract(on)

// ---- 4. host->local transmission ------------------------------------------------------------------

bool GSExactRenderer::BeginTransfer(const GSTransferCommand &command)
{
    // Local->host and local->local stay on the CPU path (GS memory downloaded first).
    if (!m_glReady || command.direction != 0u)
    {
        m_xferActive = false;
        return false;
    }
    const uint32_t psm = command.bitbltbuf.dpsm;
    uint32_t bits;
    switch (psm)
    {
    case GS_PSM_CT32:
    case GS_PSM_Z32:
        bits = 32u;
        break;
    case GS_PSM_CT24:
    case GS_PSM_Z24:
        bits = 24u;
        break;
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        bits = 16u;
        break;
    case GS_PSM_T8:
    case GS_PSM_T8H:
        bits = 8u;
        break;
    case GS_PSM_T4:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
        bits = 4u;
        break;
    default:
        Note("transfer-psm-" + std::to_string(psm)); // undefined PSM: CPU path
        m_xferActive = false;
        return false;
    }
    NoteFlush("transfer");
    FlushDraws(); // drawing before the transfer comes first (3.2.7)
    m_xfer = command;
    m_xferBits = bits;
    m_xferDone = 0;
    m_xferTotal = static_cast<uint32_t>(command.trxreg.rrw) * command.trxreg.rrh;
    m_xferCarry.clear();
    m_xferActive = m_xferTotal != 0u;
    m_xferMirror = false;
    // The transfer is applied to m_vram as well (UploadImage), so pages current on the CPU stay current
    // on both sides; pages already newer on the GPU stay so.
    uint32_t first, count;
    pageSpan(psm, command.bitbltbuf.dbp, command.bitbltbuf.dbw, command.trxpos.dsax, command.trxpos.dsay, command.trxreg.rrw,
             command.trxreg.rrh, first, count);
    FlushPendingEpoch();
    ++m_epochCounter;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t p = (first + i) % kPageCount;
        m_pageEpoch[p] = m_epochCounter;
        // Only pages CPU work uses are mirrored (UploadImage); the rest become GPU-only (pulled if needed).
        if (m_swzCpu.empty() || !m_vram || !m_cpuUse[p])
            m_gpuNewer[p] = true;
        else
            m_xferMirror = true;
    }
    HiTouchPages(first, count);
    return true;
}

bool GSExactRenderer::WriteFrameFromTexture(unsigned glTexture, const GSNativeTarget &t)
{
    if (!m_glReady || !m_texWriteProgram || t.w <= 0 || t.h <= 0)
        return false;
    NoteFlush("native");
    FlushDraws(); // drawing queued before the picture comes first
    uint32_t first, count;
    pageSpan(GS_PSM_CT32, t.fbp * 32u, t.fbw, static_cast<uint32_t>(t.x), static_cast<uint32_t>(t.y), static_cast<uint32_t>(t.w),
             static_cast<uint32_t>(t.h), first, count);
    FlushPendingEpoch();
    ++m_epochCounter;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t p = (first + i) % kPageCount;
        m_pageEpoch[p] = m_epochCounter;
        m_gpuNewer[p] = true; // written on the GPU only; CPU users pull it
    }
    HiTouchPages(first, count); // WriteHiResFromTexture, when it follows, replaces the shadow copy
    ++m_batchGen;
    UploadDirtyPages();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT |
                    GL_FRAMEBUFFER_BARRIER_BIT);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_memBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_swzBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_counterBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, m_psmBuf);
    glUseProgram(m_texWriteProgram);
    static GLint u[7] = {-2};
    if (u[0] == -2)
    {
        const char *names[7] = {"uTex", "uFbp", "uFbw", "uX", "uY", "uW", "uH"};
        for (int i = 0; i < 7; ++i)
            u[i] = glGetUniformLocation(m_texWriteProgram, names[i]);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, glTexture);
    glUniform1i(u[0], 0);
    glUniform1ui(u[1], t.fbp);
    glUniform1ui(u[2], t.fbw);
    glUniform1ui(u[3], static_cast<uint32_t>(t.x));
    glUniform1ui(u[4], static_cast<uint32_t>(t.y));
    glUniform1ui(u[5], static_cast<uint32_t>(t.w));
    glUniform1ui(u[6], static_cast<uint32_t>(t.h));
    glDispatchCompute((static_cast<uint32_t>(t.w) + 15u) / 16u, (static_cast<uint32_t>(t.h) + 15u) / 16u, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    Note("native-picture");
    return true;
}

bool GSExactRenderer::ReadFrameToTexture(unsigned glTexture, const GSNativeTarget &t)
{
    if (!m_glReady || !m_texReadProgram || t.w <= 0 || t.h <= 0)
        return false;
    NoteFlush("native");
    FlushDraws(); // drawing queued before the read comes first
    UploadDirtyPages();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_memBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_swzBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_counterBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, m_psmBuf);
    glUseProgram(m_texReadProgram);
    static GLint u[6] = {-2};
    if (u[0] == -2)
    {
        const char *names[6] = {"uFbp", "uFbw", "uX", "uY", "uW", "uH"};
        for (int i = 0; i < 6; ++i)
            u[i] = glGetUniformLocation(m_texReadProgram, names[i]);
    }
    glBindImageTexture(0, glTexture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glUniform1ui(u[0], t.fbp);
    glUniform1ui(u[1], t.fbw);
    glUniform1ui(u[2], static_cast<uint32_t>(t.x));
    glUniform1ui(u[3], static_cast<uint32_t>(t.y));
    glUniform1ui(u[4], static_cast<uint32_t>(t.w));
    glUniform1ui(u[5], static_cast<uint32_t>(t.h));
    glDispatchCompute((static_cast<uint32_t>(t.w) + 15u) / 16u, (static_cast<uint32_t>(t.h) + 15u) / 16u, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glUseProgram(0);
    return true;
}

void GSExactRenderer::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    if (!m_xferActive || !data || sizeBytes == 0u)
        return;
    // Pixels may be split across uploads (RGB24): keep the bytes of an unfinished pixel.
    m_xferCarry.insert(m_xferCarry.end(), data, data + sizeBytes);
    const uint64_t available = static_cast<uint64_t>(m_xferCarry.size()) * 8u / m_xferBits;
    const uint32_t pixels = static_cast<uint32_t>(std::min<uint64_t>(available, m_xferTotal - m_xferDone));
    if (pixels == 0u)
        return;
    const size_t usedBytes = (static_cast<size_t>(pixels) * m_xferBits + 7u) / 8u;
    std::vector<uint8_t> padded(m_xferCarry.begin(), m_xferCarry.begin() + static_cast<std::ptrdiff_t>(usedBytes));
    padded.resize((usedBytes + 7u) & ~size_t(3u), 0u);
    UploadDirtyPages();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    UploadStream(m_imgBuf, m_imgCap, padded.data(), padded.size());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_memBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_swzBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_counterBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, m_psmBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, m_imgBuf);
    glUseProgram(m_xferProgram);
    static GLint u[9] = {-2};
    if (u[0] == -2)
    {
        const char *names[9] = {"uDbp", "uDbw", "uDpsm", "uDsax", "uDsay", "uRrw", "uStart", "uCount", "uBits"};
        for (int i = 0; i < 9; ++i)
            u[i] = glGetUniformLocation(m_xferProgram, names[i]);
    }
    glUniform1ui(u[0], m_xfer.bitbltbuf.dbp);
    glUniform1ui(u[1], m_xfer.bitbltbuf.dbw);
    glUniform1ui(u[2], m_xfer.bitbltbuf.dpsm);
    glUniform1ui(u[3], m_xfer.trxpos.dsax);
    glUniform1ui(u[4], m_xfer.trxpos.dsay);
    glUniform1ui(u[5], std::max<uint32_t>(m_xfer.trxreg.rrw, 1u));
    glUniform1ui(u[6], m_xferDone);
    glUniform1ui(u[7], pixels);
    glUniform1ui(u[8], m_xferBits);
    glDispatchCompute((pixels + 255u) / 256u, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    glUseProgram(0);
    if (m_xferMirror && !m_swzCpu.empty() && m_vram)
    {
        // The same pixels into m_vram (kXferBody's arithmetic), for pages CPU work uses.
        CpuGs gs;
        gs.mem = reinterpret_cast<uint32_t *>(m_vram);
        gs.swz = m_swzCpu.data();
        gs.info = m_psmInfoCpu.data();
        const uint32_t *img = reinterpret_cast<const uint32_t *>(padded.data());
        const uint32_t rrw = std::max<uint32_t>(m_xfer.trxreg.rrw, 1u), dpsm = m_xfer.bitbltbuf.dpsm, bitsPer = m_xferBits;
        const uint32_t pb = m_psmInfoCpu[dpsm * 8u + 5u], nib = m_psmInfoCpu[dpsm * 8u + 6u];
        const size_t words = padded.size() / 4u;
        for (uint32_t i = 0; i < pixels; ++i)
        {
            const uint32_t pi = m_xferDone + i;
            const uint32_t x = (m_xfer.trxpos.dsax + pi % rrw) & 2047u, y = (m_xfer.trxpos.dsay + pi / rrw) & 2047u;
            const uint32_t bit = i * bitsPer, w = bit >> 5u, o = bit & 31u;
            uint32_t v = img[w] >> o;
            if (o + bitsPer > 32u && w + 1u < words)
                v |= img[w + 1u] << (32u - o);
            if (bitsPer < 32u)
                v &= (1u << bitsPer) - 1u;
            const uint32_t bits = gs.pixelBits(dpsm, m_xfer.bitbltbuf.dbp, m_xfer.bitbltbuf.dbw, x, y);
            const uint32_t addr = gs.byteAddress(dpsm, bits);
            if (m_gpuNewer[(addr >> 13u) % kPageCount])
                continue; // the GPU has the page; it applies the transfer itself
            const uint32_t word = addr >> 2u, sh = (addr & 3u) * 8u;
            if (pb == 4u)
                gs.writeBits(word, v, (dpsm == 1u || dpsm == 49u) ? 0xFFFFFFu : 0xFFFFFFFFu);
            else if (pb == 2u)
                gs.writeBits(word, (v & 0xFFFFu) << sh, 0xFFFFu << sh);
            else if (nib != 0u)
            {
                const uint32_t nsh = sh + (bits & 4u);
                gs.writeBits(word, (v & 15u) << nsh, 15u << nsh);
            }
            else
                gs.writeBits(word, (v & 255u) << sh, 255u << sh);
        }
    }
    m_xferDone += pixels;
    // Bytes of whole pixels are used up; a half-used byte only occurs at the end of an IDTEX4 transfer.
    m_xferCarry.erase(m_xferCarry.begin(), m_xferCarry.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(pixels) * m_xferBits / 8u));
    ++m_statTransfers;
    if (m_xferDone >= m_xferTotal)
    {
        m_xferActive = false;
        m_xferCarry.clear();
    }
}

// ---- 3.4.7 CLUT temporary buffer -----------------------------------------------------------------

bool GSExactRenderer::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    // Manual silent on loads for non-indexed PSMs (entry counts exist only for IDTEX4/IDTEX8).
    if (!isIndexed(tex0.psm))
        return true;
    switch (tex0.cld)
    {
    case 1u:
        break;
    case 2u:
        m_cbp0 = tex0.cbp;
        break;
    case 3u:
        m_cbp1 = tex0.cbp;
        break;
    case 4u:
        if (m_cbp0 == tex0.cbp)
            return true;
        m_cbp0 = tex0.cbp;
        break;
    case 5u:
        if (m_cbp1 == tex0.cbp)
            return true;
        m_cbp1 = tex0.cbp;
        break;
    default:
        return true; // 000: not loaded (110/111 undefined: manual silent)
    }
    const bool four = tex0.psm == GS_PSM_T4 || tex0.psm == GS_PSM_T4HL || tex0.psm == GS_PSM_T4HH;
    const bool sixteen = tex0.cpsm == GS_PSM_CT16 || tex0.cpsm == GS_PSM_CT16S;
    if (!sixteen && tex0.cpsm != GS_PSM_CT32)
        Note("clut-cpsm-" + std::to_string(tex0.cpsm)); // manual: PSMCT32/PSMCT16/PSMCT16S only
    if (tex0.csm != 0u && tex0.csa != 0u)
        Note("clut-csm2-csa"); // "In CSM2 mode, CSA=0 must be set"
    if (!m_glReady)
        return true;

    // Pages the load reads. A load reading pages queued draws write must wait for them; otherwise it
    // runs ahead of them at the next flush, into a new snapshot that only later draws use.
    uint32_t sf, sc;
    if (tex0.csm == 0u)
        pageSpan(tex0.cpsm, tex0.cbp, 1u, 0u, 0u, 16u, four ? 2u : 16u, sf, sc);
    else
        pageSpan(tex0.cpsm, tex0.cbp, std::max<uint32_t>(texclut.cbw, 1u), texclut.cou * 16u, texclut.cov, four ? 16u : 256u, 1u, sf, sc);
    FlushPendingEpoch();
    bool dependsOnBatch = false;
    uint64_t newest = 0;
    for (uint32_t i = 0; i < sc; ++i)
    {
        dependsOnBatch |= m_batchWritten[(sf + i) % kPageCount];
        newest = std::max(newest, m_pageEpoch[(sf + i) % kPageCount]);
    }
    const ClutLoad load{tex0.cbp, tex0.cpsm, tex0.csm, static_cast<uint32_t>(tex0.csa & (sixteen ? 0x1Fu : 0x0Fu)), four ? 16u : 256u,
                        texclut.cou, texclut.cov, texclut.cbw, 0u, 0u};
    // The same entries from unchanged memory into the same place leave the buffer as it is.
    if (m_haveLastLoad && load.Same(m_lastLoad) && newest < m_lastLoadEpoch)
        return true;
    // CPU load when its pages are current on the CPU and not written by queued GPU work.
    static const bool s_noCpuLoad = std::getenv("PS2X_GS_EXACT_NO_CPU") != nullptr;
    bool cpuOk = !s_noCpuLoad && !m_swzCpu.empty() && m_vram && !CpuCheckMode();
    for (uint32_t i = 0; i < sc && cpuOk; ++i)
    {
        const uint32_t pg = (sf + i) % kPageCount;
        cpuOk = !m_gpuNewer[pg] && !m_deferWritten[pg] && !m_batchWritten[pg];
    }
    if (cpuOk)
    {
        // Entries of the newest snapshot that GPU loads wrote are not known on the CPU. The new snapshot is then
        // made on the GPU side: a copy of the newest one plus the entries this load writes (no readback, which
        // waited for all queued GPU work: ~3 ms each). Its copy source must exist first, so a GPU load still
        // queued is flushed (it runs in that flush's sequence, before the next flush's snapshot uploads).
        PollClutReadbacks();
        const bool allKnown = m_clutUnknownCount == 0u;
        if (!allKnown && !m_pendingLoads.empty())
        {
            NoteFlush("clut-mirror");
            FlushDraws();
            ++m_statClutMirrorReads; // now counts these flushes (no readback)
        }
        const uint32_t nextSnap = (m_clutSnap + 1u) % kClutSnapshots;
        if (nextSnap == m_batchFirstSnap && (!m_prims.empty() || !m_seq.empty() || !m_snapOps.empty()))
        {
            NoteFlush("clut-ring");
            FlushDraws();
        }
        for (uint32_t i = 0; i < sc; ++i)
            m_cpuUse[(sf + i) % kPageCount] = true;
        // In place: CpuClutLoad reads each entry before writing it. One copy goes to the upload list.
        CpuClutLoad(load, m_clutMirror.data(), m_clutMirror.data(), reinterpret_cast<const uint32_t *>(m_vram));
        SnapOp op{nextSnap, allKnown ? ~0u : m_clutSnap, {}};
        ForEachClutEntry(load, [&](uint32_t idx) {
            SetClutKnown(idx);
            op.loaded[idx >> 6] |= 1ull << (idx & 63u);
        });
        m_snapOps.push_back(op);
        m_snapData.insert(m_snapData.end(), m_clutMirror.begin(), m_clutMirror.end());
        m_clutSnap = nextSnap;
        m_haveLastState = false;
        m_lastLoad = load;
        m_haveLastLoad = true;
        m_lastLoadEpoch = ++m_epochCounter;
        ++m_statCpuLoads;
        return true;
    }
    // This load runs on the GPU: its entries are unknown on the CPU, except in check mode when the CPU copy
    // (m_checkMem) holds every page it reads, untainted; then the mirror follows it there.
    uint32_t rbGen = 0; // GPU load whose snapshot is read back asynchronously
    bool mirrorFollows = CpuCheckMode() && !m_swzCpu.empty() && m_vram;
    for (uint32_t i = 0; i < sc && mirrorFollows; ++i)
    {
        const uint32_t pg = (sf + i) % kPageCount;
        mirrorFollows = !m_checkTainted[pg] && (m_checkHas[pg] || (!m_gpuNewer[pg] && !m_deferWritten[pg] && !m_batchWritten[pg]));
    }
    if (mirrorFollows)
    {
        for (uint32_t i = 0; i < sc; ++i)
            CheckPage(sf + i, false);
        CpuClutLoad(load, m_clutMirror.data(), m_clutMirror.data(), m_checkMem.data());
        ForEachClutEntry(load, [&](uint32_t idx) { SetClutKnown(idx); });
    }
    else
    {
        // Unknown until the GPU's copy of this snapshot is read back (asynchronously, after the flush that runs it).
        const uint32_t gen = ++m_clutGpuLoadGen;
        ForEachClutEntry(load, [&](uint32_t idx) {
            if (m_clutUnknownGen[idx] == 0u)
                ++m_clutUnknownCount;
            m_clutUnknownGen[idx] = gen;
        });
        rbGen = gen;
    }
    for (uint32_t i = 0; i < sc; ++i)
        m_deferRead[(sf + i) % kPageCount] = true;
    if (dependsOnBatch)
    {
        if (!m_prims.empty() || !m_pendingLoads.empty())
            ++m_counts["flush:clut-reads-batch:cbp" + std::to_string(tex0.cbp) + ":psm" + std::to_string(tex0.psm)];
        FlushDraws();
    }
    const uint32_t next = (m_clutSnap + 1u) % kClutSnapshots;
    if (next == m_batchFirstSnap && (!m_prims.empty() || !m_pendingLoads.empty() || !m_snapOps.empty()))
    {
        NoteFlush("clut-ring");
        FlushDraws();
    } // the ring would overwrite a snapshot queued draws still read
    ClutLoad queued = load;
    queued.src = m_clutSnap;
    queued.dst = next;
    m_pendingLoads.push_back(queued);
    if (rbGen != 0u)
        m_clutRbPending.push_back({rbGen, next, 0u, nullptr});
    const uint32_t item[12] = {1u, queued.cbp, queued.cpsm, queued.csm, queued.csa, queued.entries, queued.cou, queued.cov, queued.cbw,
                               queued.src, queued.dst, 0u};
    m_seq.insert(m_seq.end(), item, item + 12);
    m_clutSnap = next;
    m_haveLastState = false;
    m_lastLoad = load;
    m_haveLastLoad = true;
    m_lastLoadEpoch = ++m_epochCounter;

    // PS2X_GS_EXACT_CHECK_CLUT=N: compare the first N loads with the entries read on the CPU.
    static long s_check = []()
    {
        const char *v = std::getenv("PS2X_GS_EXACT_CHECK_CLUT");
        return v ? std::strtol(v, nullptr, 0) : 0L;
    }();
    if (s_check > 0)
    {
        --s_check;
        FlushDraws();
        DownloadAll();
        std::vector<uint32_t> gpu(kClutWords);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_clutBuf);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(m_clutSnap) * kClutWords * 4u,
                           static_cast<GLsizeiptr>(kClutWords * 4u), gpu.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        uint32_t bad = 0, first = 0, expect = 0, got = 0;
        const uint32_t n = four ? 16u : 256u;
        for (uint32_t e = 0; e < n; ++e)
        {
            uint32_t x, y, bw;
            if (tex0.csm == 0u)
            {
                const uint32_t sw = (e & ~0x18u) | ((e & 0x08u) << 1) | ((e & 0x10u) >> 1);
                x = sw & 0x0Fu, y = sw >> 4, bw = 1u;
            }
            else
                x = texclut.cou * 16u + e, y = texclut.cov, bw = std::max<uint32_t>(texclut.cbw, 1u);
            const uint32_t raw = m_cpu.ReadVram(tex0.cpsm, tex0.cbp, bw, x, y);
            const uint32_t csa = tex0.csa & (sixteen ? 0x1Fu : 0x0Fu);
            const uint32_t g = sixteen ? gpu[(csa * 16u + e) & 511u]
                                       : (gpu[(csa * 16u + e) & 255u] | (gpu[256u + ((csa * 16u + e) & 255u)] << 16));
            const uint32_t want = sixteen ? (raw & 0xFFFFu) : raw;
            if (e == 1u)
                first = e, expect = want, got = g;
            if (g != want)
                ++bad;
        }
        std::cerr << "[gs-exact] clut check cbp=" << tex0.cbp << " cpsm=" << int(tex0.cpsm) << " csm=" << int(tex0.csm) << " csa="
                  << int(tex0.csa) << " psm=" << int(tex0.psm) << ": " << bad << " of " << n << " differ" << std::hex
                  << " entry " << first << " cpu 0x" << expect << " gpu 0x" << got
                  << std::dec << std::endl;
    }
    return true;
}


// ---- drawing -------------------------------------------------------------------------------------

uint32_t GSExactRenderer::StateIndex(const GSPrimitiveBatch &batch)
{
    std::array<uint32_t, kStateWords> s;
    BuildState(batch, s);
    if (m_haveLastState && s == m_lastState)
        return static_cast<uint32_t>(m_states.size() / kStateWords) - 1u;
    m_lastState = s;
    m_haveLastState = true;
    m_states.insert(m_states.end(), s.begin(), s.end());
    return static_cast<uint32_t>(m_states.size() / kStateWords) - 1u;
}

void GSExactRenderer::BuildState(const GSPrimitiveBatch &batch, std::array<uint32_t, kStateWords> &s) const
{
    const GSDrawState &st = batch.state;
    const GSContext &c = st.context;
    s.fill(0u);
    s[0] = static_cast<uint32_t>(st.prim.type) | (st.prim.iip << 3) | (st.prim.tme << 4) | (st.prim.fge << 5) | (st.prim.abe << 6) |
           (st.prim.aa1 << 7) | (st.prim.fst << 8) | (st.prim.ctxt << 9) | (st.prim.fix << 10);
    s[1] = c.frame.fbp;
    s[2] = c.frame.fbw;
    s[3] = c.frame.psm;
    s[4] = c.frame.fbmsk;
    s[5] = c.zbuf.zbp;
    s[6] = c.zbuf.psm;
    s[7] = c.zbuf.zmask ? 1u : 0u;
    s[8] = static_cast<uint32_t>(c.scissor.x0) | (static_cast<uint32_t>(c.scissor.x1) << 16);
    s[9] = static_cast<uint32_t>(c.scissor.y0) | (static_cast<uint32_t>(c.scissor.y1) << 16);
    s[10] = c.xyoffset.ofx;
    s[11] = c.xyoffset.ofy;
    s[12] = c.tex0.tbp0;
    s[13] = c.tex0.tbw;
    s[14] = c.tex0.psm;
    s[15] = std::min<uint32_t>(c.tex0.tw, 10u); // TW/TH above 10: manual silent ("max 2^10")
    s[16] = std::min<uint32_t>(c.tex0.th, 10u);
    s[17] = c.tex0.tcc;
    s[18] = c.tex0.tfx;
    s[19] = c.tex0.cpsm;
    s[20] = c.tex0.csa & ((c.tex0.cpsm == GS_PSM_CT16 || c.tex0.cpsm == GS_PSM_CT16S) ? 0x1Fu : 0x0Fu);
    s[21] = m_clutSnap;
    s[22] = static_cast<uint32_t>(c.tex1);
    s[23] = static_cast<uint32_t>(c.tex1 >> 32);
    s[24] = static_cast<uint32_t>(c.miptbp1);
    s[25] = static_cast<uint32_t>(c.miptbp1 >> 32);
    s[26] = static_cast<uint32_t>(c.miptbp2);
    s[27] = static_cast<uint32_t>(c.miptbp2 >> 32);
    s[28] = static_cast<uint32_t>(c.clamp);
    s[29] = static_cast<uint32_t>(c.clamp >> 32);
    s[30] = static_cast<uint32_t>(c.alpha & 0xFFu);
    s[31] = static_cast<uint32_t>((c.alpha >> 32) & 0xFFu);
    s[32] = static_cast<uint32_t>(c.test);
    s[33] = static_cast<uint32_t>(c.fba & 1u);
    s[34] = static_cast<uint32_t>(st.texa.ta0) | (st.texa.aem ? 0x100u : 0u) | (static_cast<uint32_t>(st.texa.ta1) << 16);
    s[35] = st.pabe ? 1u : 0u;
    s[36] = static_cast<uint32_t>(st.scanmsk & 3u);
    s[37] = static_cast<uint32_t>(st.dimx);
    s[38] = static_cast<uint32_t>(st.dimx >> 32);
    s[39] = static_cast<uint32_t>(st.dthe & 1u);
    s[40] = static_cast<uint32_t>(st.colclamp & 1u);
    s[41] = static_cast<uint32_t>(st.fogR) | (static_cast<uint32_t>(st.fogG) << 8) | (static_cast<uint32_t>(st.fogB) << 16);
}

void GSExactRenderer::BuildPrimWords(const GSPrimitiveBatch &batch, uint32_t *p)
{
    for (uint32_t i = 0; i < 3u && i < batch.vertexCount; ++i)
    {
        const GSVertex &v = batch.vertices[i];
        uint32_t *o = p + i * 8u;
        o[0] = (static_cast<uint32_t>(std::lround(v.x * 16.0f)) & 0xFFFFu) | ((static_cast<uint32_t>(std::lround(v.y * 16.0f)) & 0xFFFFu) << 16);
        o[1] = static_cast<uint32_t>(std::min(std::max(v.z, 0.0), 4294967295.0));
        o[2] = static_cast<uint32_t>(v.r) | (static_cast<uint32_t>(v.g) << 8) | (static_cast<uint32_t>(v.b) << 16) | (static_cast<uint32_t>(v.a) << 24);
        std::memcpy(&o[3], &v.s, 4);
        std::memcpy(&o[4], &v.t, 4);
        std::memcpy(&o[5], &v.q, 4);
        o[6] = static_cast<uint32_t>(v.u) | (static_cast<uint32_t>(v.v) << 16);
        o[7] = v.fog;
    }
    p[24] = batch.vertexCount;
}

// Runs one primitive on the CPU over GS memory `mem` (bounding box bx0..bx1, by0..by1).
void GSExactRenderer::CpuRunPrim(const GSPrimitiveBatch &batch, int bx0, int by0, int bx1, int by1, uint32_t *mem)
{
    uint32_t prim[kPrimWords] = {};
    BuildPrimWords(batch, prim);
    std::array<uint32_t, kStateWords> state;
    BuildState(batch, state);
    CpuGs gs;
    gs.mem = mem;
    gs.swz = m_swzCpu.data();
    gs.info = m_psmInfoCpu.data();
    gs.pr = prim;
    gs.st = state.data();
    gs.clut = m_clutMirror.data(); // snapshot m_clutSnap = state word 21 (BuildState above)
    CpuPixelState ps;
    for (int y = by0; y <= by1; ++y)
        for (int x = bx0; x <= bx1; ++x)
            if (gs.shade(x, y, ps))
                gs.write(ps);
}

void GSExactRenderer::SetClutKnown(uint32_t idx)
{
    if (m_clutUnknownGen[idx] != 0u)
    {
        m_clutUnknownGen[idx] = 0u;
        --m_clutUnknownCount;
    }
}

// Takes in the GPU-made snapshots whose copies have landed (fence signalled; never waits). An entry is filled only
// if the load that copy belongs to is still the last writer of it (generation), so later loads are never undone.
void GSExactRenderer::PollClutReadbacks()
{
    while (!m_clutRbInFlight.empty() && m_clutUnknownCount != 0u)
    {
        // Copies are fenced per flush; the fence sits on the last copy of its group.
        size_t end = 0;
        while (end < m_clutRbInFlight.size() && !m_clutRbInFlight[end].fence)
            ++end;
        if (end == m_clutRbInFlight.size())
            return;
        const GLenum r = glClientWaitSync(static_cast<GLsync>(m_clutRbInFlight[end].fence), GL_SYNC_FLUSH_COMMANDS_BIT, 0);
        if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED)
            return;
        glDeleteSync(static_cast<GLsync>(m_clutRbInFlight[end].fence));
        for (size_t k = 0; k <= end; ++k)
        {
            const ClutReadback &rb = m_clutRbInFlight[k];
            const uint32_t *src = reinterpret_cast<const uint32_t *>(m_rbPtr) + static_cast<size_t>(rb.rbSlot) * kClutWords;
            for (uint32_t idx = 0; idx < kClutWords; ++idx)
                if (m_clutUnknownGen[idx] == rb.gen)
                {
                    m_clutMirror[idx] = src[idx];
                    m_clutUnknownGen[idx] = 0u;
                    --m_clutUnknownCount;
                }
            ++m_statClutRbTaken;
        }
        m_clutRbInFlight.erase(m_clutRbInFlight.begin(), m_clutRbInFlight.begin() + static_cast<std::ptrdiff_t>(end + 1u));
    }
    if (m_clutUnknownCount == 0u)
    {
        // Nothing unknown: the rest are not needed (their entries were overwritten since).
        for (ClutReadback &rb : m_clutRbInFlight)
            if (rb.fence)
                glDeleteSync(static_cast<GLsync>(rb.fence));
        m_clutRbInFlight.clear();
    }
}

// Calls fn(idx) for every snapshot entry (0..511) a load writes: the indices CpuClutLoad loads.
template <typename Fn>
void GSExactRenderer::ForEachClutEntry(const ClutLoad &l, Fn &&fn)
{
    const bool sixteen = l.cpsm == 2u || l.cpsm == 10u;
    for (uint32_t idx = 0; idx < kClutWords; ++idx)
    {
        const uint32_t e = sixteen ? ((idx - l.csa * 16u) & 511u) : (((idx & 255u) - l.csa * 16u) & 255u);
        if (e < l.entries)
            fn(idx);
    }
}

// One CLUT load (3.4.7) on the CPU: snapshot `dst` = `src` with the loaded entries, from GS memory `mem`.
void GSExactRenderer::CpuClutLoad(const ClutLoad &l, const uint32_t *src, uint32_t *dst, const uint32_t *mem) const
{
    CpuGs gs;
    gs.mem = const_cast<uint32_t *>(mem);
    gs.swz = m_swzCpu.data();
    gs.info = m_psmInfoCpu.data();
    const bool sixteen = l.cpsm == 2u || l.cpsm == 10u;
    for (uint32_t idx = 0; idx < kClutWords; ++idx)
    {
        uint32_t value = src[idx];
        const bool high = !sixteen && idx >= 256u;
        const uint32_t e = sixteen ? ((idx - l.csa * 16u) & 511u) : (((idx & 255u) - l.csa * 16u) & 255u);
        if (e < l.entries)
        {
            uint32_t x, y, bw;
            if (l.csm == 0u)
            {
                const uint32_t sw = (e & ~0x18u) | ((e & 0x08u) << 1) | ((e & 0x10u) >> 1);
                x = sw & 0x0Fu, y = sw >> 4, bw = 1u;
            }
            else
                x = l.cou * 16u + e, y = l.cov, bw = std::max(l.cbw, 1u);
            const uint32_t raw = gs.readPixel(l.cpsm, l.cbp, bw, x, y);
            value = sixteen ? (raw & 0xFFFFu) : high ? (raw >> 16) : (raw & 0xFFFFu);
        }
        dst[idx] = value;
    }
}

// ---- high-resolution shadow buffers (see HiTarget in the header) ---------------------------------------

void GSExactRenderer::HiUpdateSizes()
{
    if (!m_hiOn || m_outW <= 0 || m_outH <= 0 || m_hiCanvasW == 0u || m_hiCanvasH == 0u)
        return;
    // Window pixels per PS2 canvas pixel; a display buffer pixel is a canvas pixel (MAGH = the canvas step).
    const float sx = static_cast<float>(m_outW) / static_cast<float>(m_hiCanvasW);
    const float sy = static_cast<float>(m_outH) / static_cast<float>(m_hiCanvasH);
    const int W = std::clamp(static_cast<int>(std::lround(512.0f * sx)), 512, 4096);
    const int H = std::clamp(static_cast<int>(std::lround(448.0f * sy)), 448, 4096);
    for (int i = 0; i < kHiTargets; ++i)
    {
        HiTarget &t = m_hi[i];
        if (t.tex && t.w == W && t.h == H)
            continue;
        if (!t.tex)
            glGenTextures(1, &t.tex);
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        t.w = W;
        t.h = H;
        // Every image has the display's size: the work buffer is shown stretched (496 x 384 -> 512 x 448, the
        // TYPE-C copy), so its copy into a display buffer maps shadow pixels 1:1.
        t.sx = static_cast<float>(W) / static_cast<float>(t.gsW);
        t.sy = static_cast<float>(H) / static_cast<float>(t.gsH);
        t.valid = true;
        t.stale = true;
        t.staleRect[0] = 0, t.staleRect[1] = 0, t.staleRect[2] = static_cast<int>(t.gsW), t.staleRect[3] = static_cast<int>(t.gsH);
        t.staleAfter = false;
        Note("hires-resize");
    }
    if (!m_hiZTex || m_hiZW != W || m_hiZH != H)
    {
        if (!m_hiZTex)
            glGenTextures(1, &m_hiZTex);
        glBindTexture(GL_TEXTURE_2D, m_hiZTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, W, H, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        m_hiZW = W;
        m_hiZH = H;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void GSExactRenderer::HiMarkStale(int i, uint32_t first, uint32_t count, bool after)
{
    HiTarget &t = m_hi[i];
    if (!t.valid || count == 0u)
        return;
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    for (uint32_t k = 0; k < count && k < kPageCount; ++k)
    {
        const uint32_t pg = (first + k) % kPageCount;
        if (pg < t.firstPage || pg >= t.firstPage + t.pageCount)
            continue;
        const uint32_t rel = pg - t.firstPage; // PSMCT32 pages of 64 x 32, 8 per row (FBW 8)
        const int px = static_cast<int>((rel % 8u) * 64u), py = static_cast<int>((rel / 8u) * 32u);
        x0 = std::min(x0, px), y0 = std::min(y0, py), x1 = std::max(x1, px + 64), y1 = std::max(y1, py + 32);
    }
    x1 = std::min(x1, static_cast<int>(t.gsW));
    y1 = std::min(y1, static_cast<int>(t.gsH));
    if (x0 >= x1 || y0 >= y1)
        return;
    bool &flag = after ? t.staleAfter : t.stale;
    int *r = after ? t.staleAfterRect : t.staleRect;
    if (!flag)
        r[0] = x0, r[1] = y0, r[2] = x1, r[3] = y1;
    else
        r[0] = std::min(r[0], x0), r[1] = std::min(r[1], y0), r[2] = std::max(r[2], x1), r[3] = std::max(r[3], y1);
    flag = true;
}

void GSExactRenderer::HiTouchPages(uint32_t first, uint32_t count)
{
    if (!m_hiOn)
        return;
    for (int i = 0; i < kHiTargets; ++i)
        HiMarkStale(i, first, count, false);
}

void GSExactRenderer::HiResync(int i, const int rect[4])
{
    HiTarget &t = m_hi[i];
    if (!t.valid || !t.tex || !m_hiResyncProgram)
        return;
    const int x0 = std::max(static_cast<int>(std::floor(static_cast<float>(rect[0]) * t.sx)), 0);
    const int y0 = std::max(static_cast<int>(std::floor(static_cast<float>(rect[1]) * t.sy)), 0);
    const int x1 = std::min(static_cast<int>(std::ceil(static_cast<float>(rect[2]) * t.sx)), t.w);
    const int y1 = std::min(static_cast<int>(std::ceil(static_cast<float>(rect[3]) * t.sy)), t.h);
    if (x0 >= x1 || y0 >= y1)
        return;
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                    GL_TEXTURE_FETCH_BARRIER_BIT);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_memBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_swzBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_counterBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, m_psmBuf);
    glUseProgram(m_hiResyncProgram);
    glBindImageTexture(1, t.tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
    glUniform1ui(glGetUniformLocation(m_hiResyncProgram, "uFbp"), t.fbp);
    glUniform1ui(glGetUniformLocation(m_hiResyncProgram, "uFbw"), 8u);
    glUniform1ui(glGetUniformLocation(m_hiResyncProgram, "uPsm"), 0u);
    glUniform2f(glGetUniformLocation(m_hiResyncProgram, "uScale"), t.sx, t.sy);
    glUniform4i(glGetUniformLocation(m_hiResyncProgram, "uRect"), x0, y0, x1 - x0, y1 - y0);
    glDispatchCompute(static_cast<GLuint>((x1 - x0 + 15) / 16), static_cast<GLuint>((y1 - y0 + 15) / 16), 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
    glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
    glUseProgram(0);
    Note("hires-resync-fbp" + std::to_string(t.fbp) + (rect[2] - rect[0] >= static_cast<int>(t.gsW) && rect[3] - rect[1] >= static_cast<int>(t.gsH) ? "-full" : "-part"));
}

// after: the batch has run, GS memory holds its writes: those over shadowed buffers become stale (copied when
// the buffer is next used, skipped if a native picture replaces it first). Otherwise: copy the stale buffers this
// batch uses (useOnly) or all of them.
void GSExactRenderer::HiResyncPending(bool after, bool useOnly)
{
    if (!m_hiOn)
        return;
    for (int i = 0; i < kHiTargets; ++i)
    {
        HiTarget &t = m_hi[i];
        if (after)
        {
            if (!t.staleAfter)
            {
                t.batchUse = false;
                continue;
            }
            t.batchUse = false;
            t.staleAfter = false;
            const int *r = t.staleAfterRect;
            if (!t.stale)
                std::copy(r, r + 4, t.staleRect);
            else
                t.staleRect[0] = std::min(t.staleRect[0], r[0]), t.staleRect[1] = std::min(t.staleRect[1], r[1]),
                t.staleRect[2] = std::max(t.staleRect[2], r[2]), t.staleRect[3] = std::max(t.staleRect[3], r[3]);
            t.stale = true;
            continue;
        }
        if (!t.stale || (useOnly && !t.batchUse))
            continue;
        t.stale = false;
        HiResync(i, t.staleRect);
    }
}

void GSExactRenderer::HiDrawPasses()
{
    bool any = false;
    for (const HiTarget &t : m_hi)
        any |= t.batchFrame || t.batchZ;
    if (!any || !m_hiProgram)
        return;
    // (FlushDraws put an SSBO barrier before this; images are covered by the barrier at the end of the last passes.)
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_memBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_swzBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_primBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_stateBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, m_clutBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_counterBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, m_psmBuf);
    glBindFramebuffer(GL_FRAMEBUFFER, m_hiFbo);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glUseProgram(m_hiProgram);
    // Uniform locations looked up once per program, and the per-buffer values sent only when they change (this
    // runs on most flushes: ~6000 per second in the menus).
    struct Loc
    {
        unsigned prog = 0;
        GLint debug, zMode, target, role, scale, size, squeeze, center, tbp[kHiTargets], valid[kHiTargets], tscale[kHiTargets], tex[kHiTargets];
        uint32_t lastTbp[kHiTargets], lastValid[kHiTargets];
        float lastSx[kHiTargets], lastSy[kHiTargets];
    };
    static Loc L;
    if (L.prog != m_hiProgram)
    {
        L.prog = m_hiProgram;
        auto G = [&](const std::string &n) { return glGetUniformLocation(m_hiProgram, n.c_str()); };
        L.debug = G("uDebug"), L.zMode = G("uHiZMode"), L.target = G("uHiTarget"), L.role = G("uHiRole");
        L.scale = G("uHiScale"), L.size = G("uHiSize"), L.squeeze = G("uHiSqueeze"), L.center = G("uHiCenter");
        for (int i = 0; i < kHiTargets; ++i)
        {
            const std::string idx = "[" + std::to_string(i) + "]";
            L.tbp[i] = G("uTgtTbp" + idx), L.valid[i] = G("uTgtValid" + idx), L.tscale[i] = G("uTgtScale" + idx);
            L.tex[i] = G("uTgtTex" + std::to_string(i));
            glUniform1i(L.tex[i], 4 + i);
            L.lastTbp[i] = L.lastValid[i] = ~0u;
            L.lastSx[i] = L.lastSy[i] = -1.0f;
        }
        glUniform1i(L.debug, 0);
    }
    for (int i = 0; i < kHiTargets; ++i)
    {
        const HiTarget &t = m_hi[i];
        const uint32_t tbp = t.fbp * 32u, valid = (t.valid && t.tex) ? 1u : 0u;
        if (tbp != L.lastTbp[i])
            glUniform1ui(L.tbp[i], tbp), L.lastTbp[i] = tbp;
        if (valid != L.lastValid[i])
            glUniform1ui(L.valid[i], valid), L.lastValid[i] = valid;
        if (t.sx != L.lastSx[i] || t.sy != L.lastSy[i])
            glUniform2f(L.tscale[i], t.sx, t.sy), L.lastSx[i] = t.sx, L.lastSy[i] = t.sy;
        glActiveTexture(GL_TEXTURE4 + i);
        glBindTexture(GL_TEXTURE_2D, t.tex);
    }
    glBindVertexArray(m_vao);
    const GLsizei verts = static_cast<GLsizei>(m_prims.size() / kPrimWords * 6u);
    bool drew = false;
    for (int i = 0; i < kHiTargets; ++i)
    {
        HiTarget &t = m_hi[i];
        for (int role = 1; role <= 2; ++role)
        {
            if (!(role == 1 ? t.batchFrame : t.batchZ) || !t.valid || !t.tex)
                continue;
            if (role == 1 && !t.batchColor && !(t.batchZImg && t.batchZWrite))
                continue; // only Z writes, and nothing in the batch tests them
            // The passes run before the batch's 1x draw, so GS memory holds the Z buffer as it was before the batch:
            // a copy is needed only when the batch also writes it.
            const bool zImg = role == 1 && t.batchZImg && t.batchZWrite && m_hiZTex && m_hiZW >= t.w && m_hiZH >= t.h;
            if (zImg)
            {
                // The Z buffer before this batch, at the shadow's resolution (the resync program, Z layout).
                glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
                glUseProgram(m_hiResyncProgram);
                glBindImageTexture(1, m_hiZTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
                glUniform1ui(glGetUniformLocation(m_hiResyncProgram, "uFbp"), t.zbp);
                glUniform1ui(glGetUniformLocation(m_hiResyncProgram, "uFbw"), t.zfbw);
                glUniform1ui(glGetUniformLocation(m_hiResyncProgram, "uPsm"), t.zpsm);
                glUniform2f(glGetUniformLocation(m_hiResyncProgram, "uScale"), t.sx, t.sy);
                const int zx0 = std::clamp(static_cast<int>(std::floor(static_cast<float>(t.batchBox[0]) * t.sx)) - 2, 0, t.w);
                const int zy0 = std::clamp(static_cast<int>(std::floor(static_cast<float>(t.batchBox[1]) * t.sy)) - 2, 0, t.h);
                const int zx1 = std::clamp(static_cast<int>(std::ceil(static_cast<float>(t.batchBox[2] + 1) * t.sx)) + 2, 0, t.w);
                const int zy1 = std::clamp(static_cast<int>(std::ceil(static_cast<float>(t.batchBox[3] + 1) * t.sy)) + 2, 0, t.h);
                glUniform4i(glGetUniformLocation(m_hiResyncProgram, "uRect"), zx0, zy0, std::max(zx1 - zx0, 0), std::max(zy1 - zy0, 0));
                glDispatchCompute(static_cast<GLuint>((std::max(zx1 - zx0, 0) + 15) / 16), static_cast<GLuint>((std::max(zy1 - zy0, 0) + 15) / 16), 1);
                glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
                glUseProgram(m_hiProgram);
                glBindImageTexture(2, m_hiZTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
                Note("hires-zimage");
            }
            glUniform1i(L.zMode, zImg ? 1 : 0);
            glBindImageTexture(1, t.tex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
            glUniform1i(L.target, i);
            glUniform1i(L.role, role);
            glUniform2f(L.scale, t.sx, t.sy);
            glUniform2i(L.size, t.w, t.h);
            glUniform1f(L.squeeze, std::max(g_hiResSqueeze.load(std::memory_order_relaxed), 0.25f));
            glUniform1f(L.center, static_cast<float>(t.gsW) * 0.5f - 0.5f);
            glViewport(0, 0, t.w, t.h);
            // PS2X_GS_HIRES_TIMING=1: GPU time of the shadow passes (each query waits for the GPU: measuring only).
            static const bool s_hiTiming = std::getenv("PS2X_GS_HIRES_TIMING") != nullptr;
            static unsigned s_q = 0;
            static double s_ns[2] = {0.0, 0.0};
            static uint64_t s_n[2] = {0, 0};
            if (s_hiTiming)
            {
                if (!s_q)
                    glGenQueries(1, &s_q);
                glBeginQuery(GL_TIME_ELAPSED, s_q);
            }
            glDrawArrays(GL_TRIANGLES, 0, verts);
            drew = true;
            if (s_hiTiming)
            {
                glEndQuery(GL_TIME_ELAPSED);
                GLuint64 ns = 0;
                glGetQueryObjectui64v(s_q, GL_QUERY_RESULT, &ns);
                s_ns[role - 1] += static_cast<double>(ns);
                if ((++s_n[role - 1] % 2000u) == 0u)
                    std::fprintf(stderr, "[hires] timing: frame passes %llu %.1f ms total, z passes %llu %.1f ms total\n",
                                 static_cast<unsigned long long>(s_n[0]), s_ns[0] * 1e-6, static_cast<unsigned long long>(s_n[1]), s_ns[1] * 1e-6);
            }
            // No barrier between passes: within one batch no pass reads what another writes (batch rules), and a
            // depth copy is written by compute (its own barrier above). One barrier after all passes.
            Note(role == 1 ? "hires-pass-frame" : "hires-pass-z");
        }
        t.batchFrame = t.batchZ = t.batchZImg = t.batchColor = t.batchZWrite = false;
    }
    if (drew)
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
    glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    glBindImageTexture(2, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    for (int i = 0; i < kHiTargets; ++i)
    {
        glActiveTexture(GL_TEXTURE4 + i);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool GSExactRenderer::HiResSize(uint32_t fbp, int &w, int &h)
{
    for (const HiTarget &t : m_hi)
        if (m_hiOn && t.valid && t.tex && t.fbp == fbp)
        {
            w = t.w;
            h = t.h;
            return true;
        }
    return false;
}

bool GSExactRenderer::WriteHiResFromTexture(unsigned glTexture, int texW, int texH, const GSNativeTarget &target)
{
    if (!m_glReady || !m_hiOn || texW <= 0 || texH <= 0)
        return false;
    HiTarget *tp = nullptr;
    for (HiTarget &t : m_hi)
        if (t.valid && t.tex && t.fbp == target.fbp && target.fbw == 8u)
            tp = &t;
    if (!tp)
        return false;
    NoteFlush("native");
    FlushDraws(); // drawing queued before the picture comes first
    HiTarget &t = *tp;
    const int dx0 = static_cast<int>(std::lround(static_cast<float>(target.x) * t.sx));
    const int dy0 = static_cast<int>(std::lround(static_cast<float>(target.y) * t.sy));
    const int dx1 = std::min(static_cast<int>(std::lround(static_cast<float>(target.x + target.w) * t.sx)), t.w);
    const int dy1 = std::min(static_cast<int>(std::lround(static_cast<float>(target.y + target.h) * t.sy)), t.h);
    if (dx0 >= dx1 || dy0 >= dy1)
        return false;
    glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_hiBlitFbo[0]);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTexture, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_hiBlitFbo[1]);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    glDisable(GL_SCISSOR_TEST);
    const bool same = texW == dx1 - dx0 && texH == dy1 - dy0;
    glBlitFramebuffer(0, 0, texW, texH, dx0, dy0, dx1, dy1, GL_COLOR_BUFFER_BIT, same ? GL_NEAREST : GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
    // The picture replaces what WriteFrameFromTexture just put into GS memory there (HiTouchPages marked it).
    if (target.x <= 0 && target.y <= 0 && target.x + target.w >= static_cast<int>(t.gsW) && target.y + target.h >= static_cast<int>(t.gsH))
        t.stale = false;
    Note("hires-native");
    return true;
}

void GSExactRenderer::Draw(const GSPrimitiveBatch &batch)
{
    // draw() CPU time for PS2X_GS_GPU_STATS only (two clock reads per primitive otherwise wasted).
    static const bool s_drawTiming = std::getenv("PS2X_GS_GPU_STATS") != nullptr;
    struct DrawTimer
    {
        double &acc;
        bool on;
        std::chrono::steady_clock::time_point t = on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        ~DrawTimer()
        {
            if (on)
                acc += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
        }
    } drawTimer{m_statDrawCpuSec, s_drawTiming};
    if (m_glFailed)
        return;
    const GSDrawState &st = batch.state;
    const GSContext &c = st.context;
    ++m_statDraws;
    if (st.prim.aa1)
        Note("aa1-primitives");
    if (st.prim.tme && (((c.tex1 >> 9) & 1u) != 0u))
        ++m_statMtba; // automatic MIPMAP base pointers: not applied (MIPTBP1 used as set); printed as "mtba"

    // Pages this draw writes (frame, Z) and reads as a texture.
    const uint32_t fbw = std::max<uint32_t>(c.frame.fbw, 1u);
    const uint32_t sx0 = c.scissor.x0, sy0 = c.scissor.y0;
    const uint32_t sw = c.scissor.x1 >= c.scissor.x0 ? c.scissor.x1 - c.scissor.x0 + 1u : 1u;
    const uint32_t sh = c.scissor.y1 >= c.scissor.y0 ? c.scissor.y1 - c.scissor.y0 + 1u : 1u;
    uint32_t ff, fc, zf = 0, zc = 0;
    pageSpan(c.frame.psm, c.frame.fbp * 32u, fbw, sx0, sy0, sw, sh, ff, fc);
    const uint32_t ztst = static_cast<uint32_t>((c.test >> 17) & 3u);
    const bool zUsed = ztst >= 2u || !c.zbuf.zmask;
    if (zUsed)
        pageSpan(c.zbuf.psm, c.zbuf.zbp * 32u, fbw, sx0, sy0, sw, sh, zf, zc);

    bool readsBatchPages = false;
    // At most 7 mip levels (MXL <= 6): a fixed array, not a heap vector per primitive.
    struct
    {
        std::pair<uint32_t, uint32_t> v[7];
        uint32_t n = 0;
        void emplace_back(uint32_t a, uint32_t b) { v[n++] = {a, b}; }
        const std::pair<uint32_t, uint32_t> *begin() const { return v; }
        const std::pair<uint32_t, uint32_t> *end() const { return v + n; }
    } texSpans;
    if (st.prim.tme)
    {
        const uint32_t mxl = static_cast<uint32_t>((c.tex1 >> 2) & 7u);
        for (uint32_t lvl = 0; lvl <= std::min<uint32_t>(mxl, 6u); ++lvl)
        {
            uint32_t tbp = c.tex0.tbp0, tbw = c.tex0.tbw;
            if (lvl >= 1u && lvl <= 3u)
            {
                tbp = static_cast<uint32_t>((c.miptbp1 >> (20u * (lvl - 1u))) & 0x3FFFu);
                tbw = static_cast<uint32_t>((c.miptbp1 >> (20u * (lvl - 1u) + 14u)) & 0x3Fu);
            }
            else if (lvl >= 4u)
            {
                tbp = static_cast<uint32_t>((c.miptbp2 >> (20u * (lvl - 4u))) & 0x3FFFu);
                tbw = static_cast<uint32_t>((c.miptbp2 >> (20u * (lvl - 4u) + 14u)) & 0x3Fu);
            }
            uint32_t tf, tc;
            pageSpan(c.tex0.psm, tbp, tbw, 0u, 0u, std::max<uint32_t>(st.textureWidth >> lvl, 1u),
                     std::max<uint32_t>(st.textureHeight >> lvl, 1u), tf, tc);
            texSpans.emplace_back(tf, tc);
            for (uint32_t i = 0; i < tc && !readsBatchPages; ++i)
                readsBatchPages = m_batchWritten[(tf + i) % kPageCount];
        }
    }

    // Within one GL draw, fragments are ordered per window pixel only. Primitives of one frame/Z buffer
    // configuration share a pixel->address mapping, so different pixels never share a word except
    // through the 16-bit packing (atomics). A primitive may join the batch unless it would write pages
    // another configuration in the batch writes or that the batch read as texture, or read as texture
    // pages the batch writes: those need the earlier draws finished first.
    const uint64_t frameKey = (static_cast<uint64_t>(c.frame.fbp) << 0) ^ (static_cast<uint64_t>(fbw) << 9) ^
                              (static_cast<uint64_t>(c.frame.psm) << 15) ^ (static_cast<uint64_t>(c.zbuf.zbp) << 21) ^
                              (static_cast<uint64_t>(c.zbuf.psm) << 30) ^ (static_cast<uint64_t>(zUsed ? 1u : 0u) << 36) ^ (1ull << 40);
    // Small targets (64 pixels wide, no Z, at most 1024 pixels touched) go into the sequence when
    // nothing queued for the batch's draw writes or reads their pages (that draw runs after the
    // sequence) and their textures are not written by it.
    static const bool s_noSequence = std::getenv("PS2X_GS_EXACT_NO_SEQUENCE") != nullptr;
    if (!s_noSequence && fbw == 1u && !zUsed && !readsBatchPages && batch.vertexCount >= 1u)
    {
        const uint32_t nv = st.prim.type == GS_PRIM_POINT ? 1u
                            : (st.prim.type == GS_PRIM_LINE || st.prim.type == GS_PRIM_LINESTRIP || st.prim.type == GS_PRIM_SPRITE) ? 2u : 3u;
        int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -(1 << 30), by1 = -(1 << 30);
        for (uint32_t i = 0; i < nv && i < batch.vertexCount; ++i)
        {
            const int x = static_cast<int>(std::lround(batch.vertices[i].x * 16.0f)) - static_cast<int>(c.xyoffset.ofx);
            const int y = static_cast<int>(std::lround(batch.vertices[i].y * 16.0f)) - static_cast<int>(c.xyoffset.ofy);
            bx0 = std::min(bx0, x), by0 = std::min(by0, y), bx1 = std::max(bx1, x), by1 = std::max(by1, y);
        }
        bx0 = std::max({(bx0 >> 4) - 1, static_cast<int>(c.scissor.x0), 0});
        by0 = std::max({(by0 >> 4) - 1, static_cast<int>(c.scissor.y0), 0});
        bx1 = std::min({(bx1 >> 4) + 1, static_cast<int>(c.scissor.x1), 2047});
        by1 = std::min({(by1 >> 4) + 1, static_cast<int>(c.scissor.y1), 2047});
        bool pagesFree = true;
        for (uint32_t i = 0; i < fc && pagesFree; ++i)
            pagesFree = !m_batchWritten[(ff + i) % kPageCount] && !m_batchRead[(ff + i) % kPageCount];
        if (pagesFree && bx0 <= bx1 && by0 <= by1 && (bx1 - bx0 + 1) * (by1 - by0 + 1) <= 4096)
        {
            // CPU execution when every page it reads or writes is current on the CPU and no GPU work
            // still queued touches them (PS2X_GS_EXACT_NO_CPU=1: always the GPU sequence).
            static const bool s_noCpu = std::getenv("PS2X_GS_EXACT_NO_CPU") != nullptr;
            const bool check = CpuCheckMode();
            auto cpuPage = [&](uint32_t pg, bool written) {
                pg %= kPageCount;
                if (m_deferWritten[pg] && !(check && m_checkHas[pg]))
                    return false;
                if (written && m_deferRead[pg])
                    return false;
                if (check && m_checkTainted[pg])
                    return false;
                return check ? (m_checkHas[pg] || !m_gpuNewer[pg]) : !m_gpuNewer[pg];
            };
            // Points, lines and sprites (exact DDA on both sides); textured only with UV coordinates.
            bool cpuOk = !s_noCpu && !m_swzCpu.empty() && m_vram &&
                         (st.prim.type == GS_PRIM_POINT || st.prim.type == GS_PRIM_LINE || st.prim.type == GS_PRIM_LINESTRIP ||
                          st.prim.type == GS_PRIM_SPRITE) &&
                         (!st.prim.tme || st.prim.fst);
            // Texture format first, so no page is pulled for a primitive that then goes to the GPU anyway:
            // result independent of the LOD; an indexed texture needs every CLUT entry known on the CPU.
            if (cpuOk && st.prim.tme)
            {
                const uint32_t mmag = static_cast<uint32_t>((c.tex1 >> 5) & 1u), mmin = static_cast<uint32_t>((c.tex1 >> 6) & 7u);
                cpuOk = mmin <= 1u && mmag == mmin;
                if (cpuOk && isIndexed(c.tex0.psm))
                {
                    PollClutReadbacks();
                    cpuOk = m_clutUnknownCount == 0u;
                }
            }
            // Pages newer on the GPU (from earlier GPU work) are read back once; after that the CPU keeps
            // them current as long as only CPU work touches them.
            if (cpuOk)
            {
                auto pull = [&](uint32_t first, uint32_t count)
                {
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        const uint32_t pg = (first + i) % kPageCount;
                        if (m_gpuNewer[pg] && !m_deferWritten[pg] && !m_batchWritten[pg] && !(check && m_checkHas[pg]))
                        {
                            DownloadOverlapping(pg, 1u, "cpu-pull");
                            ++m_statCpuPulls;
                        }
                    }
                };
                pull(ff, fc);
                if (st.prim.tme)
                    for (const auto &[tf, tc] : texSpans)
                        pull(tf, tc);
            }
            for (uint32_t i = 0; i < fc && cpuOk; ++i)
                cpuOk = cpuPage(ff + i, true);
            if (cpuOk && st.prim.tme)
            {
                for (const auto &[tf, tc] : texSpans)
                    for (uint32_t i = 0; i < tc && cpuOk; ++i)
                        cpuOk = cpuPage(tf + i, false);
            }
            if (cpuOk && check)
            {
                // Check mode: the GPU sequence does the real work below; the CPU runs the same primitive
                // on a copy and FlushDraws compares the pages.
                for (uint32_t i = 0; i < fc; ++i)
                    CheckPage(ff + i, true);
                for (const auto &[tf, tc] : texSpans)
                    for (uint32_t i = 0; i < tc; ++i)
                        CheckPage(tf + i, false);
                CpuRunPrim(batch, bx0, by0, bx1, by1, m_checkMem.data());
                ++m_statCpuChecked;
            }
            else if (cpuOk)
            {
                for (uint32_t i = 0; i < fc; ++i)
                    m_cpuUse[(ff + i) % kPageCount] = true;
                for (const auto &[tf, tc] : texSpans)
                    for (uint32_t i = 0; i < tc; ++i)
                        m_cpuUse[(tf + i) % kPageCount] = true;
                CpuRunPrim(batch, bx0, by0, bx1, by1, reinterpret_cast<uint32_t *>(m_vram));
                if (m_hiOn)
                {
                    uint32_t bf, bc;
                    pageSpan(c.frame.psm, c.frame.fbp * 32u, fbw, static_cast<uint32_t>(bx0), static_cast<uint32_t>(by0),
                             static_cast<uint32_t>(bx1 - bx0 + 1), static_cast<uint32_t>(by1 - by0 + 1), bf, bc);
                    HiTouchPages(bf, bc);
                }
                FlushPendingEpoch();
                ++m_epochCounter;
                for (uint32_t i = 0; i < fc; ++i)
                {
                    const uint32_t pg = (ff + i) % kPageCount;
                    m_pageEpoch[pg] = m_epochCounter;
                    m_cpuNewer[pg] = true;
                }
                ++m_statCpuPrims;
                return;
            }
            const uint32_t stateIndexD = StateIndex(batch);
            const size_t baseD = m_prims.size();
            m_prims.resize(baseD + kPrimWords, 0u);
            uint32_t *pd = &m_prims[baseD];
            for (uint32_t i = 0; i < 3u && i < batch.vertexCount; ++i)
            {
                const GSVertex &v = batch.vertices[i];
                uint32_t *o = pd + i * 8u;
                o[0] = (static_cast<uint32_t>(std::lround(v.x * 16.0f)) & 0xFFFFu) | ((static_cast<uint32_t>(std::lround(v.y * 16.0f)) & 0xFFFFu) << 16);
                o[1] = static_cast<uint32_t>(std::min(std::max(v.z, 0.0), 4294967295.0));
                o[2] = static_cast<uint32_t>(v.r) | (static_cast<uint32_t>(v.g) << 8) | (static_cast<uint32_t>(v.b) << 16) |
                       (static_cast<uint32_t>(v.a) << 24);
                std::memcpy(&o[3], &v.s, 4);
                std::memcpy(&o[4], &v.t, 4);
                std::memcpy(&o[5], &v.q, 4);
                o[6] = static_cast<uint32_t>(v.u) | (static_cast<uint32_t>(v.v) << 16);
                o[7] = v.fog;
            }
            pd[24] = batch.vertexCount;
            pd[25] = stateIndexD;
            pd[26] = 1u;
            const uint32_t item[12] = {0u, static_cast<uint32_t>(baseD / kPrimWords), static_cast<uint32_t>(bx0), static_cast<uint32_t>(by0),
                                       static_cast<uint32_t>(bx1 - bx0 + 1), static_cast<uint32_t>(by1 - by0 + 1), 0u, 0u, 0u, 0u, 0u, 0u};
            m_seq.insert(m_seq.end(), item, item + 12);
            if (m_hiOn)
            {
                uint32_t bf, bc;
                pageSpan(c.frame.psm, c.frame.fbp * 32u, fbw, static_cast<uint32_t>(bx0), static_cast<uint32_t>(by0),
                         static_cast<uint32_t>(bx1 - bx0 + 1), static_cast<uint32_t>(by1 - by0 + 1), bf, bc);
                for (int i = 0; i < kHiTargets; ++i)
                    HiMarkStale(i, bf, bc, true);
            }
            FlushPendingEpoch();
            ++m_epochCounter;
            for (uint32_t i = 0; i < fc; ++i)
            {
                const uint32_t pg = (ff + i) % kPageCount;
                m_pageEpoch[pg] = m_epochCounter;
                m_deferWritten[pg] = true;
                m_gpuNewer[pg] = true;
            }
            for (const auto &[tf, tc] : texSpans)
                for (uint32_t i = 0; i < tc; ++i)
                    m_deferRead[(tf + i) % kPageCount] = true;
            if (check && !cpuOk)
                for (uint32_t i = 0; i < fc; ++i)
                    m_checkTainted[(ff + i) % kPageCount] = true; // GPU-only work: the CPU copy no longer follows
            ++m_statDeferred;
            return;
        }
    }
    // High-resolution shadow roles of this primitive (see HiTarget): frame role (its frame buffer is a shadowed
    // buffer), Z role (only its Z buffer is), or a write over a shadowed buffer's pages in another layout (that
    // buffer is copied from GS memory after the batch). Ordering: a primitive that uses a buffer with such a copy
    // pending, or such a write after shadow work on the buffer in this batch, starts a new batch.
    uint32_t hiFrameMask = 0u, hiZMask = 0u, hiStaleFrame = 0u, hiStaleZ = 0u;
    uint32_t hiPf = 0u, hiPc = 0u, hiPzf = 0u, hiPzc = 0u;
    bool hiZOnly = false;
    int hiBox[4] = {0, 0, -1, -1};
    // ps2SetHiResSqueeze: primitives into the display buffers that are not full width (clears, fades, backgrounds)
    // and not the copy of the work buffer (FBP 224) are drawn squeezed towards the centre in the shadow images.
    bool squeezeBox = false, hiSqueeze = false;
    if (m_hiOn)
    {
        // What the primitive can write at all: ZTST NEVER writes nothing; an alpha test that always fails (ATST
        // NEVER) writes only what AFAIL keeps; FBMSK all ones writes no colour.
        const uint32_t ate = static_cast<uint32_t>(c.test & 1u), atst = static_cast<uint32_t>((c.test >> 1) & 7u);
        const uint32_t afail = static_cast<uint32_t>((c.test >> 12) & 3u);
        const bool alwaysFail = ate != 0u && atst == 0u;
        const bool ztstNever = ((c.test >> 16) & 1u) != 0u && ztst == 0u;
        const bool frameWrites = !ztstNever && static_cast<uint32_t>(c.frame.fbmsk) != 0xFFFFFFFFu &&
                                 (!alwaysFail || afail == 1u || afail == 3u);
        const bool zWrite = !ztstNever && !c.zbuf.zmask && zc != 0u && (!alwaysFail || afail == 2u);
        // The primitive's own box (its vertices, clipped to the scissor), for writes over other buffers' pages.
        uint32_t pf = 0, pc = 0, pzf = 0, pzc = 0;
        int box[4] = {0, 0, -1, -1};
        {
            const uint32_t nv = st.prim.type == GS_PRIM_POINT ? 1u
                                : (st.prim.type == GS_PRIM_LINE || st.prim.type == GS_PRIM_LINESTRIP || st.prim.type == GS_PRIM_SPRITE) ? 2u : 3u;
            int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -(1 << 30), by1 = -(1 << 30);
            for (uint32_t i = 0; i < nv && i < batch.vertexCount; ++i)
            {
                const int x = static_cast<int>(std::lround(batch.vertices[i].x * 16.0f)) - static_cast<int>(c.xyoffset.ofx);
                const int y = static_cast<int>(std::lround(batch.vertices[i].y * 16.0f)) - static_cast<int>(c.xyoffset.ofy);
                bx0 = std::min(bx0, x), by0 = std::min(by0, y), bx1 = std::max(bx1, x), by1 = std::max(by1, y);
            }
            bx0 = std::max({(bx0 >> 4) - 1, static_cast<int>(c.scissor.x0), 0});
            by0 = std::max({(by0 >> 4) - 1, static_cast<int>(c.scissor.y0), 0});
            bx1 = std::min({(bx1 >> 4) + 1, static_cast<int>(c.scissor.x1), 2047});
            by1 = std::min({(by1 >> 4) + 1, static_cast<int>(c.scissor.y1), 2047});
            if (bx0 <= bx1 && by0 <= by1)
            {
                box[0] = bx0, box[1] = by0, box[2] = bx1, box[3] = by1;
                const uint32_t bw = static_cast<uint32_t>(bx1 - bx0 + 1), bh = static_cast<uint32_t>(by1 - by0 + 1);
                if (frameWrites)
                    pageSpan(c.frame.psm, c.frame.fbp * 32u, fbw, static_cast<uint32_t>(bx0), static_cast<uint32_t>(by0), bw, bh, pf, pc);
                if (zWrite)
                    pageSpan(c.zbuf.psm, c.zbuf.zbp * 32u, fbw, static_cast<uint32_t>(bx0), static_cast<uint32_t>(by0), bw, bh, pzf, pzc);
            }
        }
        hiPf = pf, hiPc = pc, hiPzf = pzf, hiPzc = pzc;
        hiZOnly = !frameWrites;
        std::copy(box, box + 4, hiBox);
        squeezeBox = box[0] <= box[2] && (box[2] - box[0] + 1) * 10 < (static_cast<int>(c.scissor.x1) - static_cast<int>(c.scissor.x0) + 1) * 9 &&
                     !(st.prim.tme && c.tex0.tbp0 == 224u * 32u);
        bool hiFlush = false;
        uint32_t hiUse = 0u;
        for (int i = 0; i < kHiTargets; ++i)
        {
            const HiTarget &t = m_hi[i];
            if (!t.valid)
                continue;
            auto overlaps = [&](uint32_t first, uint32_t count) {
                for (uint32_t k = 0; k < count && k < kPageCount; ++k)
                {
                    const uint32_t pg = (first + k) % kPageCount;
                    if (pg >= t.firstPage && pg < t.firstPage + t.pageCount)
                        return true;
                }
                return false;
            };
            const bool frameMatch = c.frame.fbp == t.fbp && fbw == 8u && (c.frame.psm == GS_PSM_CT32 || c.frame.psm == GS_PSM_CT24);
            const bool zAlias = frameMatch && c.zbuf.zbp == c.frame.fbp && (c.zbuf.psm == GS_PSM_Z32 || c.zbuf.psm == GS_PSM_Z24);
            // Primitives that only write Z belong to the pass too: a later primitive of the batch may test that Z
            // (the depth copy of that pass, uHiZMode), e.g. the port's Z clear before its depth-tested text.
            const bool frameRole = frameMatch && (frameWrites || zWrite);
            // Z written into a shadowed buffer by a primitive drawing elsewhere (role 2): the port paints its weapon
            // gauges that way (sprites over the work buffer, alpha test on its alpha, "Z" = the colour, written into
            // the display buffer).
            const bool zRole = !frameMatch && zWrite && c.zbuf.zbp == t.fbp && (c.zbuf.psm == GS_PSM_Z32 || c.zbuf.psm == GS_PSM_Z24);
            const bool frameStale = !frameMatch && overlaps(pf, pc);
            const bool zStale = zWrite && !zRole && !zAlias && overlaps(pzf, pzc);
            const bool samples = st.prim.tme && c.tex0.tbp0 == t.fbp * 32u && c.tex0.tbw == 8u &&
                                 (c.tex0.psm == GS_PSM_CT32 || c.tex0.psm == GS_PSM_CT24);
            if (frameRole)
                hiFrameMask |= 1u << i;
            if (zRole)
                hiZMask |= 1u << i;
            if (frameStale)
                hiStaleFrame |= 1u << i;
            if (zStale)
                hiStaleZ |= 1u << i;
            if ((frameRole || zRole || samples) && t.staleAfter)
                hiFlush = true;
            if (frameRole || zRole || samples)
                hiUse |= 1u << i;
            if ((frameStale || zStale) && (t.batchFrame || t.batchZ))
                hiFlush = true;
        }
        const float squeeze = g_hiResSqueeze.load(std::memory_order_relaxed);
        if (squeeze < 0.999f && squeezeBox)
            for (int i = 0; i < kHiTargets; ++i)
            {
                const HiTarget &t = m_hi[i];
                if (!t.valid || t.gsW != 512u || ((hiFrameMask >> i) & 1u) == 0u)
                    continue;
                hiSqueeze = true;
                // The batch box (depth copy) covers both the 1x box and where the primitive lands.
                const float cx = static_cast<float>(t.gsW) * 0.5f - 0.5f;
                const int sx0 = static_cast<int>(std::floor(cx + (static_cast<float>(box[0]) - 0.5f - cx) * squeeze)) - 1;
                const int sx1 = static_cast<int>(std::ceil(cx + (static_cast<float>(box[2]) + 0.5f - cx) * squeeze)) + 1;
                hiBox[0] = std::max(std::min(hiBox[0], sx0), 0);
                hiBox[2] = std::max(hiBox[2], sx1);
            }
        if (hiFlush && !m_prims.empty())
        {
            NoteFlush("hires-order");
            FlushDraws();
        }
        for (int i = 0; i < kHiTargets; ++i)
        {
            if ((hiUse >> i) & 1u)
                m_hi[i].batchUse = true;
            HiTarget &t = m_hi[i];
            // Only a depth test needs the Z buffer's values (Z writes alone go to the copy when a later primitive
            // of the batch tests them: its pass loads the copy, and every primitive of that pass writes into it).
            if (((hiFrameMask >> i) & 1u) && !c.zbuf.zmask && c.zbuf.zbp != c.frame.fbp)
                t.batchZWrite = true;
            if (((hiFrameMask >> i) & 1u) && ztst >= 2u && c.zbuf.zbp != c.frame.fbp)
            {
                t.batchZImg = true;
                t.zbp = c.zbuf.zbp;
                t.zpsm = c.zbuf.psm;
                t.zfbw = fbw;
            }
        }
        // PS2X_GS_HIRES_TRACE=<n>: the first n primitives with a shadow role or a stale write.
        static long s_hiTrace = []() { const char *v = std::getenv("PS2X_GS_HIRES_TRACE"); return v ? std::strtol(v, nullptr, 0) : 0L; }();
        static const long s_hiTraceFrom = []() { const char *v = std::getenv("PS2X_GS_HIRES_TRACE_FROM"); return v ? std::strtol(v, nullptr, 0) : 0L; }();
        if (s_hiTrace > 0 && m_presentCount >= static_cast<uint64_t>(s_hiTraceFrom) && (hiFrameMask | hiZMask | hiStaleFrame | hiStaleZ) != 0u)
        {
            --s_hiTrace;
            std::fprintf(stderr,
                         "[hires] prim %u roles f%x z%x stale f%x z%x frame fbp %u fbw %u psm %u fbmsk %08x zbuf %u psm %u zmsk %u test %05x "
                         "tme %u tbp %u tbw %u tpsm %u v0 %.1f,%.1f v1 %.1f,%.1f scissor %u,%u-%u,%u\n",
                         static_cast<unsigned>(st.prim.type), hiFrameMask, hiZMask, hiStaleFrame, hiStaleZ, c.frame.fbp, c.frame.fbw,
                         c.frame.psm, static_cast<unsigned>(c.frame.fbmsk), c.zbuf.zbp, c.zbuf.psm, c.zbuf.zmask ? 1u : 0u,
                         static_cast<unsigned>(c.test), st.prim.tme ? 1u : 0u, c.tex0.tbp0, c.tex0.tbw, c.tex0.psm, batch.vertices[0].x,
                         batch.vertices[0].y, batch.vertices[1].x, batch.vertices[1].y, c.scissor.x0, c.scissor.y0, c.scissor.x1,
                         c.scissor.y1);
        }
    }
    bool writeConflict = false;
    auto checkWrites = [&](uint32_t first, uint32_t count)
    {
        for (uint32_t i = 0; i < count && !writeConflict; ++i)
        {
            const uint32_t pg = (first + i) % kPageCount;
            writeConflict = (m_batchWritten[pg] && m_batchOwner[pg] != frameKey) || m_batchRead[pg];
        }
    };
    // Consecutive primitives nearly always have the frame/Z spans of the one before (a 640x448 32-bit frame is
    // 140 pages, walked five times per primitive). If nothing in the page arrays changed since that primitive was
    // queued (m_batchGen) and its spans were conflict-free then (m_spanCache.ok), they still are, and its marks
    // (owner, written, GPU-newer) are still in place: the walks are skipped, the page epochs written lazily
    // (FlushPendingEpoch). Every decision is the one the full walks make.
    const uint64_t genAtStart = m_batchGen;
    const bool sameSpans = m_spanCache.ok && m_spanCache.gen == m_batchGen && m_spanCache.key == frameKey && m_spanCache.ff == ff &&
                           m_spanCache.fc == fc && m_spanCache.zf == zf && m_spanCache.zc == zc;
    if (!sameSpans)
    {
        checkWrites(ff, fc);
        checkWrites(zf, zc);
    }
    if (!m_prims.empty() && (readsBatchPages || writeConflict || m_prims.size() / kPrimWords >= kMaxPrims))
    {
        NoteFlush(readsBatchPages ? "texture-reads-batch" : writeConflict ? "write-overlap" : "full");
        FlushDraws();
    }
    for (const auto &[tf, tc] : texSpans)
        for (uint32_t i = 0; i < tc; ++i)
            if (!m_batchRead[(tf + i) % kPageCount])
            {
                m_batchRead[(tf + i) % kPageCount] = true;
                ++m_batchGen;
            }
    ++m_mainPrims;

    const uint32_t stateIndex = StateIndex(batch);
    const size_t base = m_prims.size();
    m_prims.resize(base + kPrimWords, 0u);
    uint32_t *p = &m_prims[base];
    for (uint32_t i = 0; i < 3u && i < batch.vertexCount; ++i)
    {
        const GSVertex &v = batch.vertices[i];
        const uint32_t x = static_cast<uint32_t>(std::lround(v.x * 16.0f)) & 0xFFFFu; // the 12.4 register value
        const uint32_t y = static_cast<uint32_t>(std::lround(v.y * 16.0f)) & 0xFFFFu;
        uint32_t *o = p + i * 8u;
        o[0] = x | (y << 16);
        o[1] = static_cast<uint32_t>(std::min(std::max(v.z, 0.0), 4294967295.0));
        o[2] = static_cast<uint32_t>(v.r) | (static_cast<uint32_t>(v.g) << 8) | (static_cast<uint32_t>(v.b) << 16) |
               (static_cast<uint32_t>(v.a) << 24);
        std::memcpy(&o[3], &v.s, 4);
        std::memcpy(&o[4], &v.t, 4);
        std::memcpy(&o[5], &v.q, 4);
        o[6] = static_cast<uint32_t>(v.u) | (static_cast<uint32_t>(v.v) << 16);
        o[7] = v.fog;
    }
    p[24] = batch.vertexCount;
    p[25] = stateIndex;
    p[27] = hiFrameMask;
    p[28] = hiZMask;
    p[29] = hiZOnly ? 1u : 0u;
    p[30] = hiSqueeze ? 1u : 0u;
    if (m_hiOn)
        for (int i = 0; i < kHiTargets; ++i)
        {
            HiTarget &t = m_hi[i];
            if ((hiFrameMask >> i) & 1u)
            {
                if (!t.batchFrame)
                    std::copy(hiBox, hiBox + 4, t.batchBox);
                else
                    t.batchBox[0] = std::min(t.batchBox[0], hiBox[0]), t.batchBox[1] = std::min(t.batchBox[1], hiBox[1]),
                    t.batchBox[2] = std::max(t.batchBox[2], hiBox[2]), t.batchBox[3] = std::max(t.batchBox[3], hiBox[3]);
                t.batchFrame = true;
                t.batchColor |= !hiZOnly;
                if (!hiZOnly && hiBox[2] >= hiBox[0])
                    m_statHiArea += static_cast<double>(hiBox[2] - hiBox[0] + 1) * (hiBox[3] - hiBox[1] + 1) * t.sx * t.sy;
            }
            t.batchZ |= ((hiZMask >> i) & 1u) != 0u;
            if ((hiStaleFrame >> i) & 1u)
                HiMarkStale(i, hiPf, hiPc, true);
            if ((hiStaleZ >> i) & 1u)
                HiMarkStale(i, hiPzf, hiPzc, true);
        }

    if (sameSpans && m_batchGen == genAtStart)
    {
        // Marks already in place (see above); only the epoch moves on.
        m_spanEpoch = ++m_epochCounter;
        return;
    }
    FlushPendingEpoch();
    ++m_epochCounter;
    for (uint32_t i = 0; i < fc; ++i)
    {
        m_pageEpoch[(ff + i) % kPageCount] = m_epochCounter;
        m_batchOwner[(ff + i) % kPageCount] = frameKey;
        m_batchWritten[(ff + i) % kPageCount] = true;
        m_gpuNewer[(ff + i) % kPageCount] = true;
    }
    for (uint32_t i = 0; i < zc; ++i)
    {
        m_pageEpoch[(zf + i) % kPageCount] = m_epochCounter;
        m_batchOwner[(zf + i) % kPageCount] = frameKey;
        m_batchWritten[(zf + i) % kPageCount] = true;
        m_gpuNewer[(zf + i) % kPageCount] = true;
    }
    ++m_batchGen;
    // Cache these spans if the next primitive's full check would find no conflict in them now.
    writeConflict = false;
    checkWrites(ff, fc);
    checkWrites(zf, zc);
    m_spanCache = {frameKey, ff, fc, zf, zc, m_batchGen, !writeConflict};
    m_spanEpoch = 0;
}

// The epoch of the last cached-span primitive, written to its frame/Z pages (they all get the newest one, as the
// per-primitive writes would have left them).
void GSExactRenderer::FlushPendingEpoch()
{
    if (m_spanEpoch == 0)
        return;
    for (uint32_t i = 0; i < m_spanCache.fc; ++i)
        m_pageEpoch[(m_spanCache.ff + i) % kPageCount] = m_spanEpoch;
    for (uint32_t i = 0; i < m_spanCache.zc; ++i)
        m_pageEpoch[(m_spanCache.zf + i) % kPageCount] = m_spanEpoch;
    m_spanEpoch = 0;
}

// Streams `bytes` into the start of `buf`, leaving it bound to GL_SHADER_STORAGE_BUFFER. The store is re-specified
// only when it has to grow (doubling): glBufferData with a new size on every flush made the driver allocate a new
// store each time (~27% of the host thread). No shader reads past what was written or asks for the buffer length.
void GSExactRenderer::UploadStream(unsigned buf, size_t &cap, const void *data, size_t bytes)
{
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
    if (bytes > cap)
    {
        cap = std::max<size_t>(std::max<size_t>(bytes, cap * 2u), 64u * 1024u);
        glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(cap), nullptr, GL_DYNAMIC_DRAW);
    }
    StreamTo(buf, 0u, data, bytes);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
}

// Writes `bytes` at `dstOffset` of `buf` without making the CPU wait for the GPU. glBufferSubData into a buffer
// that queued GPU work still uses (GS memory, prims, CLUT snapshots: every flush) made the driver hold the host
// thread until the GPU was done with it. Here the bytes go into a persistently mapped ring and the GPU copies them
// (glCopyBufferSubData), in command order like glBufferSubData, so every buffer gets the same contents at the
// same point of the command stream. The ring has kRingSegments parts; leaving one fences it, and a part is
// reused only after its fence (one lap earlier) has signalled. PS2X_GS_EXACT_NO_RING=1: plain glBufferSubData.
void GSExactRenderer::StreamTo(unsigned buf, size_t dstOffset, const void *data, size_t bytes)
{
    if (bytes == 0u)
        return;
    constexpr size_t seg = kRingBytes / kRingSegments;
    if (!m_ringPtr || bytes > seg)
    {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(dstOffset), static_cast<GLsizeiptr>(bytes), data);
        return;
    }
    size_t off = (m_ringHead + 15u) & ~size_t(15u);
    if (off + bytes > (static_cast<size_t>(m_ringSeg) + 1u) * seg)
    {
        m_ringFence[m_ringSeg] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        m_ringSeg = (m_ringSeg + 1u) % kRingSegments;
        if (GLsync f = static_cast<GLsync>(m_ringFence[m_ringSeg]))
        {
            while (glClientWaitSync(f, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull) == GL_TIMEOUT_EXPIRED)
            {
            }
            glDeleteSync(f);
            m_ringFence[m_ringSeg] = nullptr;
            ++m_statRingWaits;
        }
        off = static_cast<size_t>(m_ringSeg) * seg;
    }
    std::memcpy(m_ringPtr + off, data, bytes);
    glBindBuffer(GL_COPY_READ_BUFFER, m_ringBuf);
    glBindBuffer(GL_COPY_WRITE_BUFFER, buf);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(off), static_cast<GLintptr>(dstOffset),
                        static_cast<GLsizeiptr>(bytes));
    ++m_statStreamCopies;
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    m_ringHead = off + bytes;
}

void GSExactRenderer::FlushDraws()
{
    // PS2X_GS_GPU_STATS timing: CPU time here, and GPU time (timer queries) of every 64th flush.
    const auto flushStart = std::chrono::steady_clock::now();
    struct FlushTimer
    {
        GSExactRenderer &r;
        std::chrono::steady_clock::time_point t;
        ~FlushTimer() { r.m_statFlushCpuSec += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(); }
    } flushTimer{*this, flushStart};
    // PS2X_GS_EXACT_TIMING=1: GPU timer queries (each one waits for the GPU, so off by default).
    static const bool s_timing = std::getenv("PS2X_GS_EXACT_TIMING") != nullptr;
    static uint64_t s_flushCalls = 0;
    const bool timeGpu = s_timing && m_glReady && ((++s_flushCalls) & 63u) == 0u && (!m_prims.empty() || !m_seq.empty());
    if (timeGpu && !m_timerQuery[0])
        glGenQueries(2, m_timerQuery);
    if (!m_snapOps.empty() && m_glReady)
    {
        // Snapshots are also written by shaders (sequence loads): one barrier so the copies below come after
        // those writes. Copies among themselves are ordered by GL (no shader writes in between).
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        // Snapshots made by CPU loads, in order, before anything that reads them. Whole snapshots: one upload per
        // run of consecutive ring slots, straight from m_snapData. Partly known ones (see LoadClut): a GPU copy of
        // the snapshot before, then the entries the load wrote.
        for (size_t a = 0; a < m_snapOps.size();)
        {
            const SnapOp &op = m_snapOps[a];
            const uint32_t *data = m_snapData.data() + a * kClutWords;
            if (op.copyFrom == ~0u)
            {
                size_t b = a + 1u;
                while (b < m_snapOps.size() && m_snapOps[b].copyFrom == ~0u && m_snapOps[b].slot == m_snapOps[b - 1u].slot + 1u)
                    ++b;
                StreamTo(m_clutBuf, static_cast<size_t>(op.slot) * kClutWords * 4u, data, (b - a) * kClutWords * 4u);
                a = b;
                continue;
            }
            glBindBuffer(GL_COPY_READ_BUFFER, m_clutBuf);
            glBindBuffer(GL_COPY_WRITE_BUFFER, m_clutBuf);
            ++m_statSnapCopies;
            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(op.copyFrom) * kClutWords * 4u,
                                static_cast<GLintptr>(op.slot) * kClutWords * 4u, static_cast<GLsizeiptr>(kClutWords * 4u));
            glBindBuffer(GL_COPY_READ_BUFFER, 0);
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            for (uint32_t e = 0; e < kClutWords;)
            {
                if (!((op.loaded[e >> 6] >> (e & 63u)) & 1u))
                {
                    ++e;
                    continue;
                }
                uint32_t f = e + 1u;
                while (f < kClutWords && ((op.loaded[f >> 6] >> (f & 63u)) & 1u))
                    ++f;
                StreamTo(m_clutBuf, (static_cast<size_t>(op.slot) * kClutWords + e) * 4u, data + e, (f - e) * 4u);
                e = f;
            }
            ++a;
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        m_snapOps.clear();
        m_snapData.clear();
    }
    // Shadow buffers written outside the shadow passes since (transfers, CPU work): from GS memory, before this
    // batch draws over them.
    if (m_hiOn && m_glReady)
    {
        bool anyStale = false;
        for (const HiTarget &t : m_hi)
            anyStale |= t.stale && t.batchUse;
        if (anyStale)
        {
            UploadDirtyPages();
            HiResyncPending(false, true);
        }
    }
    if ((m_prims.empty() && m_seq.empty()) || !m_glReady)
    {
        m_prims.clear();
        m_states.clear();
        m_haveLastState = false;
        m_batchWritten.fill(false);
        m_batchRead.fill(false);
        ++m_batchGen;
        return;
    }
    // m_gpuNewer for this batch's pages was set when they were queued; send what the CPU wrote.
    // Pages the batch writes are never also CPU-newer: the CPU side downloads before writing.
    UploadDirtyPages();
    if (!m_prims.empty())
    {
        UploadStream(m_primBuf, m_primCap, m_prims.data(), m_prims.size() * 4u);
        UploadStream(m_stateBuf, m_stateCap, m_states.data(), m_states.size() * 4u);
    }
    if (!m_seq.empty())
    {
        // Small-target primitives and CLUT loads, in command order (see kSequenceMain).
        UploadStream(m_seqBuf, m_seqCap, m_seq.data(), m_seq.size() * 4u);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_memBuf);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_swzBuf);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_primBuf);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_stateBuf);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, m_clutBuf);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_counterBuf);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, m_psmBuf);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, m_seqBuf);
        glUseProgram(m_seqProgram);
        static GLint uCount = -2, uDebugSeq = -2;
        if (uCount == -2)
        {
            uCount = glGetUniformLocation(m_seqProgram, "uSeqCount");
            uDebugSeq = glGetUniformLocation(m_seqProgram, "uDebug");
        }
        glUniform1ui(uCount, static_cast<GLuint>(m_seq.size() / 12u));
        glUniform1i(uDebugSeq, 0);
        if (timeGpu)
            glBeginQuery(GL_TIME_ELAPSED, m_timerQuery[0]);
        glDispatchCompute(1, 1, 1);
        if (timeGpu)
        {
            glEndQuery(GL_TIME_ELAPSED);
            GLuint64 ns = 0;
            glGetQueryObjectui64v(m_timerQuery[0], GL_QUERY_RESULT, &ns);
            m_statGpuClutSec += static_cast<double>(ns) * 1e-9 * 64.0; // sequence time (sampled)
        }
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        glUseProgram(0);
        if (!m_clutRbPending.empty() && m_rbPtr)
        {
            glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT); // the snapshots were written by the sequence shader
            glBindBuffer(GL_COPY_READ_BUFFER, m_clutBuf);
            glBindBuffer(GL_COPY_WRITE_BUFFER, m_rbBuf);
            size_t pushed = 0;
            for (ClutReadback rb : m_clutRbPending)
            {
                if (m_clutRbInFlight.size() >= kRbSlots)
                    break; // no room: these entries stay unknown (copy-and-patch path), never wrong
                ++pushed;
                rb.rbSlot = m_rbHead;
                m_rbHead = (m_rbHead + 1u) % kRbSlots;
                glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(rb.snap) * kClutWords * 4u,
                                    static_cast<GLintptr>(rb.rbSlot) * kClutWords * 4u, static_cast<GLsizeiptr>(kClutWords * 4u));
                m_clutRbInFlight.push_back(rb);
            }
            glBindBuffer(GL_COPY_READ_BUFFER, 0);
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            // One fence for this flush's copies, on the last of them.
            if (pushed)
                m_clutRbInFlight.back().fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            m_clutRbPending.clear();
        }
        m_statClutLoads += m_pendingLoads.size();
        m_statSeqItems += m_seq.size() / 12u;
        ++m_statSeqDispatches;
        m_pendingLoads.clear();
        m_seq.clear();
        m_seqHasRect = false;
    }
    m_batchFirstSnap = m_clutSnap;
    m_deferWritten.fill(false);
    m_deferRead.fill(false);
    if (CpuCheckMode())
        CheckCompare();
    if (m_mainPrims == 0u)
    {
        HiResyncPending(true);
        m_prims.clear();
        m_states.clear();
        m_haveLastState = false;
        m_batchWritten.fill(false);
        m_batchRead.fill(false);
        ++m_batchGen;
        return;
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    // The batch into the shadow buffers first: GS memory (Z buffers, textures) is then as it was before the batch.
    if (m_hiOn)
        HiDrawPasses();

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_memBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_swzBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_primBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_stateBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, m_clutBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_counterBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, m_psmBuf);
    glBindFramebuffer(GL_FRAMEBUFFER, m_drawFbo);
    glViewport(0, 0, 2048, 2048);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glUseProgram(m_drawProgram);
    static const int s_debug = std::getenv("PS2X_GS_EXACT_DEBUG") ? std::atoi(std::getenv("PS2X_GS_EXACT_DEBUG")) : 0;
    static GLint s_uDebugDraw = -2;
    if (s_uDebugDraw == -2)
        s_uDebugDraw = glGetUniformLocation(m_drawProgram, "uDebug");
    glUniform1i(s_uDebugDraw, s_debug);
    glBindVertexArray(m_vao);
    if (timeGpu)
        glBeginQuery(GL_TIME_ELAPSED, m_timerQuery[1]);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_prims.size() / kPrimWords * 6u));
    if (timeGpu)
    {
        glEndQuery(GL_TIME_ELAPSED);
        GLuint64 ns = 0;
        glGetQueryObjectui64v(m_timerQuery[1], GL_QUERY_RESULT, &ns);
        m_statGpuDrawSec += static_cast<double>(ns) * 1e-9 * 64.0;
        ++m_statTimedFlushes;
    }
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    if (s_debug != 0) // glGetError waits for NVIDIA's driver thread: only with PS2X_GS_EXACT_DEBUG
    {
        static int s_errors = 0;
        const GLenum err = glGetError();
        if (err != GL_NO_ERROR && s_errors++ < 8)
            std::cerr << "[gs-exact] GL error 0x" << std::hex << err << std::dec << " after draw (fbo status 0x" << std::hex
                      << glCheckFramebufferStatus(GL_FRAMEBUFFER) << std::dec << ")" << std::endl;
    }
    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // The same primitives into the shadow buffers, then the copies the batch's other writes need.
    if (m_hiOn)
        HiResyncPending(true);
    ++m_statFlushes;

    m_prims.clear();
    m_states.clear();
    m_haveLastState = false;
    m_mainPrims = 0u;
    m_batchWritten.fill(false);
        m_batchRead.fill(false);
        ++m_batchGen;
}

// ---- PS2X_GS_EXACT_CPU_CHECK=1: CPU results compared with the GPU's ----------------------------------

bool GSExactRenderer::CpuCheckMode()
{
    static const bool s_check = std::getenv("PS2X_GS_EXACT_CPU_CHECK") != nullptr;
    return s_check;
}

// Makes page `pg` of the check copy hold what GS memory holds before the queued GPU work runs.
void GSExactRenderer::CheckPage(uint32_t pg, bool written)
{
    pg %= kPageCount;
    if (m_checkMem.empty())
        m_checkMem.assign(4u * 1024u * 1024u / 4u, 0u);
    if (!m_checkHas[pg])
    {
        std::memcpy(&m_checkMem[pg * (kPageBytes / 4u)], m_vram + static_cast<size_t>(pg) * kPageBytes, kPageBytes);
        m_checkHas[pg] = true;
    }
    if (written)
        m_checkTouched[pg] = true;
}

void GSExactRenderer::CheckCompare()
{
    std::vector<uint32_t> gpu(kPageBytes / 4u);
    bool bound = false;
    for (uint32_t pg = 0; pg < kPageCount; ++pg)
    {
        if (m_checkHas[pg] && !m_checkTainted[pg]) // written pages compare the CPU's results, read pages its inputs
        {
            if (!bound)
            {
                glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_memBuf);
                bound = true;
            }
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(pg) * kPageBytes, kPageBytes, gpu.data());
            ++m_statCheckPages;
            for (uint32_t w = 0; w < kPageBytes / 4u; ++w)
            {
                const uint32_t cpu = m_checkMem[pg * (kPageBytes / 4u) + w];
                if (cpu != gpu[w])
                {
                    if (m_statCheckBad < 40u)
                        std::cerr << "[gs-exact] cpu check: " << (m_checkTouched[pg] ? "written" : "read") << " page " << pg << " word " << w << std::hex << " cpu 0x" << cpu << " gpu 0x" << gpu[w]
                                  << std::dec << std::endl;
                    ++m_statCheckBad;
                }
            }
        }
        m_checkHas[pg] = false;
        m_checkTouched[pg] = false;
        m_checkTainted[pg] = false;
    }
    if (bound)
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

// ---- 5.2 PCRTC ----------------------------------------------------------------------------------

bool GSExactRenderer::PresentFromGpu(const GSPresentationRequest &request)
{
    if (!m_glReady)
        return false;
    NoteFlush("present");
    FlushDraws();
    // Game frames: the display buffer changed (double buffering swaps DISPFB each frame).
    if (request.dispfb1 != m_lastDispfb1)
    {
        ++m_statSwaps;
        m_lastDispfb1 = request.dispfb1;
    }
    UploadDirtyPages();

    struct Circuit
    {
        bool on;
        int dx, dy, dw, dh, magh, magv;
        uint32_t fbp, fbw, psm;
        int dbx, dby;
    } cir[2];
    const uint64_t dispfb[2] = {request.dispfb1, request.dispfb2};
    const uint64_t display[2] = {request.display1, request.display2};
    for (int i = 0; i < 2; ++i)
    {
        Circuit &c = cir[i];
        c.on = ((request.pmode >> i) & 1u) != 0u;
        c.fbp = static_cast<uint32_t>(dispfb[i] & 0x1FFu);
        c.fbw = static_cast<uint32_t>((dispfb[i] >> 9) & 0x3Fu);
        c.psm = static_cast<uint32_t>((dispfb[i] >> 15) & 0x1Fu);
        c.dbx = static_cast<int>((dispfb[i] >> 32) & 0x7FFu);
        c.dby = static_cast<int>((dispfb[i] >> 43) & 0x7FFu);
        c.dx = static_cast<int>(display[i] & 0xFFFu);
        c.dy = static_cast<int>((display[i] >> 12) & 0x7FFu);
        c.magh = static_cast<int>((display[i] >> 23) & 0xFu) + 1;
        c.magv = static_cast<int>((display[i] >> 27) & 0x3u) + 1;
        c.dw = static_cast<int>((display[i] >> 32) & 0xFFFu);
        c.dh = static_cast<int>((display[i] >> 44) & 0x7FFu);
        if (c.on && c.psm == 0x12u)
            Note("display-ps-gpu24"); // read as PSMCT32 (not converted)
    }
    static const long s_logFrom = []()
    {
        const char *v = std::getenv("PS2X_GS_EXACT_LOG_DISPLAY");
        return v ? std::strtol(v, nullptr, 0) : -1L;
    }();
    if (s_logFrom >= 0 && request.vsyncTick >= static_cast<uint64_t>(s_logFrom) && request.vsyncTick < static_cast<uint64_t>(s_logFrom) + 40u)
        std::cerr << "[gs-exact] present tick " << request.vsyncTick << std::hex << " pmode=0x" << request.pmode << " smode2=0x"
                  << request.smode2 << " dispfb1=0x" << request.dispfb1 << " display1=0x" << request.display1 << " dispfb2=0x"
                  << request.dispfb2 << " display2=0x" << request.display2 << std::dec << std::endl;
    if (!cir[0].on && !cir[1].on)
    {
        // 5.2.3: OUT2 is BGCOLOR while read circuit 2 is off; with circuit 1 off too (manual silent on
        // OUT1 then) the output is that background colour.
        if (!m_canvasTex)
            return false;
        glBindFramebuffer(GL_FRAMEBUFFER, m_canvasFbo);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(static_cast<float>(request.bgcolor & 0xFFu) / 255.0f, static_cast<float>((request.bgcolor >> 8) & 0xFFu) / 255.0f,
                     static_cast<float>((request.bgcolor >> 16) & 0xFFu) / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        m_presentValid = true;
        ++m_presentCount;
        Note("display-both-circuits-off");
        return true;
    }
    // Canvas: the union of the enabled circuits' rectangles, one canvas pixel per (smallest MAGH)
    // VCK. Manual silent on the visible raster area of NTSC/PAL (overscan), so nothing else is shown.
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1, step = 16;
    for (const Circuit &c : cir)
        if (c.on)
        {
            x0 = std::min(x0, c.dx);
            y0 = std::min(y0, c.dy);
            x1 = std::max(x1, c.dx + c.dw);
            y1 = std::max(y1, c.dy + c.dh);
            step = std::min(step, c.magh);
        }
    const uint32_t w = static_cast<uint32_t>((x1 - x0 + 1 + step - 1) / step);
    const uint32_t h = static_cast<uint32_t>(y1 - y0 + 1);
    if (w == 0u || h == 0u || w > 4096u || h > 4096u)
        return false;
    // Shadow buffers: sized from this canvas and the window, brought up to date, and read by the circuits that
    // show them; the canvas is then made at the window's size.
    int hiSrc[2] = {-1, -1};
    uint32_t cw = w, ch = h;
    if (m_hiOn)
    {
        m_hiCanvasW = w;
        m_hiCanvasH = h;
        HiUpdateSizes();
        for (int c = 0; c < 2; ++c)
            if (cir[c].on && cir[c].fbw == 8u && (cir[c].psm == 0u || cir[c].psm == 1u))
                for (int i = 0; i < kHiTargets; ++i)
                    if (m_hi[i].valid && m_hi[i].tex && m_hi[i].fbp == cir[c].fbp)
                    {
                        hiSrc[c] = i;
                        if (m_hi[i].stale)
                        {
                            m_hi[i].stale = false;
                            HiResync(i, m_hi[i].staleRect);
                        }
                    }
        if ((hiSrc[0] >= 0 || hiSrc[1] >= 0) && m_outW > 0 && m_outH > 0)
        {
            cw = static_cast<uint32_t>(std::clamp(m_outW, static_cast<int>(w), 4096));
            ch = static_cast<uint32_t>(std::clamp(m_outH, static_cast<int>(h), 4096));
        }
        else
            hiSrc[0] = hiSrc[1] = -1;
    }
    if (cw != m_canvasW || ch != m_canvasH || !m_canvasTex)
    {
        if (!m_canvasTex)
            glGenTextures(1, &m_canvasTex);
        glBindTexture(GL_TEXTURE_2D, m_canvasTex);
        std::vector<uint8_t> black(static_cast<size_t>(cw) * ch * 4u, 0u);
        for (size_t i = 3; i < black.size(); i += 4)
            black[i] = 255u;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(cw), static_cast<GLsizei>(ch), 0, GL_RGBA, GL_UNSIGNED_BYTE, black.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (!m_canvasFbo)
            glGenFramebuffers(1, &m_canvasFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_canvasFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_canvasTex, 0);
        m_canvasW = cw;
        m_canvasH = ch;
    }

    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_memBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_swzBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_counterBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, m_psmBuf);
    glBindFramebuffer(GL_FRAMEBUFFER, m_canvasFbo);
    glViewport(0, 0, static_cast<GLsizei>(cw), static_cast<GLsizei>(ch));
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glUseProgram(m_crtcProgram);
    auto U = [&](const char *n) { return glGetUniformLocation(m_crtcProgram, n); };
    glUniform2f(U("uHiCanvas"), static_cast<float>(cw) / static_cast<float>(w), static_cast<float>(ch) / static_cast<float>(h));
    for (int c = 0; c < 2; ++c)
    {
        const std::string idx = "[" + std::to_string(c) + "]";
        glUniform1i(U(("uHiSrc" + idx).c_str()), hiSrc[c]);
        const HiTarget *t = hiSrc[c] >= 0 ? &m_hi[hiSrc[c]] : nullptr;
        glUniform2f(U(("uHiSrcScale" + idx).c_str()), t ? t->sx : 1.0f, t ? t->sy : 1.0f);
        glActiveTexture(GL_TEXTURE2 + c);
        glBindTexture(GL_TEXTURE_2D, t ? t->tex : 0u);
        glUniform1i(U(c == 0 ? "uHiSrc0" : "uHiSrc1"), 2 + c);
    }
    glActiveTexture(GL_TEXTURE0);
    const bool interlace = (request.smode2 & 1u) != 0u;
    glUniform2i(U("uOrigin"), x0, y0);
    glUniform1i(U("uVckStep"), step);
    glUniform1i(U("uCanvasH"), static_cast<GLint>(h));
    glUniform1i(U("uInterlace"), interlace ? 1 : 0);
    glUniform1i(U("uFfmd"), static_cast<GLint>((request.smode2 >> 1) & 1u));
    glUniform1i(U("uField"), static_cast<GLint>(request.vsyncTick & 1u)); // field parity of this vsync
    // Every line comes from the current buffer each present (the line mapping is unchanged), not only the current
    // field's lines as on the GS, which leaves afterimages on a progressive monitor. The game's own DISPLAY MODE
    // option (480i / 480p) chooses the video mode.
    glUniform1i(U("uAllLines"), 1);
    // Display switch (like Marz). Anti-flicker: both read circuits show the same buffer one line apart and the merge circuit blends them
    // (ALP). Off by default: circuit 1 is shown opaque. PS2X_GS_FLICKER_FILTER=1: the blend, as the GS does.
    static const bool s_flicker = []() { const char *v = std::getenv("PS2X_GS_FLICKER_FILTER"); return v && v[0] == '1'; }();
    const bool mmod = ((request.pmode >> 5) & 1u) != 0u;
    const bool sameBuffer = cir[0].on && cir[1].on && cir[0].fbp == cir[1].fbp && cir[0].fbw == cir[1].fbw && cir[0].psm == cir[1].psm &&
                            cir[0].dbx == cir[1].dbx && cir[0].dby == cir[1].dby;
    const bool opaque = !s_flicker && mmod && sameBuffer;
    for (int i = 0; i < 2; ++i)
    {
        const Circuit &c = cir[i];
        const std::string idx = "[" + std::to_string(i) + "]";
        glUniform1i(U(("uEnable" + idx).c_str()), c.on ? 1 : 0);
        glUniform4i(U(("uDisplay" + idx).c_str()), c.dx, c.dy, c.dw, c.dh);
        glUniform2i(U(("uMag" + idx).c_str()), c.magh, c.magv);
        glUniform4ui(U(("uDispFb" + idx).c_str()), c.fbp, c.fbw, c.psm, 0u);
        glUniform2i(U(("uDb" + idx).c_str()), c.dbx, c.dby);
    }
    glUniform1i(U("uMmod"), static_cast<GLint>((request.pmode >> 5) & 1u));
    glUniform1i(U("uAmod"), static_cast<GLint>((request.pmode >> 6) & 1u));
    glUniform1i(U("uSlbg"), static_cast<GLint>((request.pmode >> 7) & 1u));
    glUniform1i(U("uAlp"), opaque ? 255 : static_cast<GLint>((request.pmode >> 8) & 0xFFu));
    glUniform3ui(U("uBg"), static_cast<GLuint>(request.bgcolor & 0xFFu), static_cast<GLuint>((request.bgcolor >> 8) & 0xFFu),
                 static_cast<GLuint>((request.bgcolor >> 16) & 0xFFu));
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);
    for (int c = 0; c < 2; ++c)
    {
        glActiveTexture(GL_TEXTURE2 + c);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m_presentValid = true;
    ++m_presentCount;
    if (m_presentHook)
    {
        // The top circuit: read circuit 1 where it is on (it is blended over circuit 2), else circuit 2.
        const Circuit &top = cir[0].on ? cir[0] : cir[1];
        GSNativePresent present;
        present.fbp = top.fbp;
        present.fbw = top.fbw;
        present.canvasW = static_cast<int>(w);
        present.canvasH = static_cast<int>(h);
        present.originX = (top.dx - x0) / step;
        present.originY = top.dy - y0;
        present.dbx = top.dbx;
        present.dby = top.dby;
        // Unblended: both circuits show one buffer and circuit 1 is opaque, or a single circuit is on.
        present.plain = top.psm == 0u && top.magh == step && top.magv == 1 && (opaque || (cir[0].on != cir[1].on));
        m_presentHook(this, present);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    DumpCanvas(request.vsyncTick);
    // PS2X_GS_EXACT_DUMP=<present number>: GS memory (after this present) and the display registers.
    static const long s_dumpAt = []()
    {
        const char *v = std::getenv("PS2X_GS_EXACT_DUMP");
        return v ? std::strtol(v, nullptr, 0) : -1L;
    }();
    if (s_dumpAt >= 0 && m_presentCount == static_cast<uint64_t>(s_dumpAt) && m_vram)
    {
        DownloadAll();
        const char *dir = std::getenv("PS2X_DUMP_FRAMES");
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/exact_gsmem_%06llu.bin", (dir && *dir) ? dir : ".",
                      static_cast<unsigned long long>(request.vsyncTick));
        if (FILE *f = std::fopen(path, "wb"))
        {
            std::fwrite(m_vram, 1, 4u * 1024u * 1024u, f);
            std::fclose(f);
        }
        // PS2X_GS_EXACT_DUMP_TEX="psm:bp:bw:w:h;..." (bp in blocks): raw values of those regions as PNGs,
        // read with the chapter 8 layouts (GSMem). Indexed formats as grey (index * 17 / index).
        if (const char *spec = std::getenv("PS2X_GS_EXACT_DUMP_TEX"))
        {
            std::string all(spec);
            size_t start = 0;
            while (start < all.size())
            {
                const size_t end = std::min(all.find(';', start), all.size());
                unsigned psm = 0, bp = 0, bw = 0, tw = 0, th = 0;
                if (std::sscanf(all.substr(start, end - start).c_str(), "%u:%u:%u:%u:%u", &psm, &bp, &bw, &tw, &th) == 5 && tw && th)
                {
                    std::vector<uint8_t> px(static_cast<size_t>(tw) * th * 4u);
                    for (unsigned y = 0; y < th; ++y)
                        for (unsigned x = 0; x < tw; ++x)
                        {
                            const uint32_t v = m_cpu.ReadVram(psm, bp, bw, x, y);
                            uint8_t *o = &px[(static_cast<size_t>(y) * tw + x) * 4u];
                            if (psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH)
                                o[0] = o[1] = o[2] = static_cast<uint8_t>((v & 15u) * 17u);
                            else if (psm == GS_PSM_T8 || psm == GS_PSM_T8H)
                                o[0] = o[1] = o[2] = static_cast<uint8_t>(v);
                            else if (psm == GS_PSM_CT16 || psm == GS_PSM_CT16S)
                                o[0] = static_cast<uint8_t>((v & 31u) << 3), o[1] = static_cast<uint8_t>(((v >> 5) & 31u) << 3),
                                o[2] = static_cast<uint8_t>(((v >> 10) & 31u) << 3);
                            else
                                o[0] = static_cast<uint8_t>(v), o[1] = static_cast<uint8_t>(v >> 8), o[2] = static_cast<uint8_t>(v >> 16);
                            o[3] = 255u;
                        }
                    char tpath[1024];
                    std::snprintf(tpath, sizeof(tpath), "%s/tex_%llu_psm%u_bp%u_bw%u.png", (dir && *dir) ? dir : ".",
                                  static_cast<unsigned long long>(request.vsyncTick), psm, bp, bw);
                    Image image{};
                    image.data = px.data();
                    image.width = static_cast<int>(tw);
                    image.height = static_cast<int>(th);
                    image.mipmaps = 1;
                    image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                    ExportImage(image, tpath);
                }
                start = end + 1;
            }
        }
        std::cerr << "[gs-exact] dump " << path << std::hex << " pmode=0x" << request.pmode << " smode2=0x" << request.smode2
                  << " dispfb1=0x" << request.dispfb1 << " display1=0x" << request.display1 << " dispfb2=0x" << request.dispfb2
                  << " display2=0x" << request.display2 << " bg=0x" << request.bgcolor << std::dec << " canvas " << w << "x" << h
                  << " origin " << x0 << "," << y0 << " step " << step << std::endl;
    }
    return true;
}

void GSExactRenderer::DumpCanvas(uint64_t tick)
{
    static const char *dumpDir = std::getenv("PS2X_DUMP_FRAMES");
    if (!dumpDir || !*dumpDir)
        return;
    static const uint64_t every = []()
    {
        const char *e = std::getenv("PS2X_DUMP_EVERY");
        const long v = e ? std::strtol(e, nullptr, 10) : 60;
        return static_cast<uint64_t>(v > 0 ? v : 60);
    }();
    if (m_presentCount % every != 0u)
        return;
    std::vector<uint8_t> pixels(static_cast<size_t>(m_canvasW) * m_canvasH * 4u);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_canvasFbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, static_cast<GLsizei>(m_canvasW), static_cast<GLsizei>(m_canvasH), GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/frame_%06llu.png", dumpDir, static_cast<unsigned long long>(tick));
    exportPng(std::move(pixels), static_cast<int>(m_canvasW), static_cast<int>(m_canvasH), path);
}

void GSExactRenderer::DrawPresentation(float x, float y, float w, float h)
{
    // The window rectangle of the picture: the size the shadow buffers and the canvas are made at.
    m_outW = static_cast<int>(std::lround(w));
    m_outH = static_cast<int>(std::lround(h));
    if (!m_presentValid || !m_canvasTex)
        return;
    Texture2D tex{};
    tex.id = m_canvasTex;
    tex.width = static_cast<int>(m_canvasW);
    tex.height = static_cast<int>(m_canvasH);
    tex.mipmaps = 1;
    tex.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    // Canvas row 0 is the first output line, drawn at the top.
    DrawTexturePro(tex, Rectangle{0.0f, 0.0f, static_cast<float>(m_canvasW), static_cast<float>(m_canvasH)}, Rectangle{x, y, w, h},
                   Vector2{0.0f, 0.0f}, 0.0f, WHITE);
}

// ---- statistics ----------------------------------------------------------------------------------

void GSExactRenderer::ReadShaderCounters() const
{
    if (!m_glReady)
        return;
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_counterBuf);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(uint32_t) * m_shaderCounters.size(), m_shaderCounters.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

std::string GSExactRenderer::UnsupportedSummary() const
{
    ReadShaderCounters();
    std::ostringstream out;
    for (const auto &[k, n] : m_counts)
        out << " " << k << "=" << n;
    if (m_statMtba)
        out << " mtba=" << m_statMtba;
    for (size_t i = 0; i < sizeof(kSilentNames) / sizeof(kSilentNames[0]); ++i)
        if (m_shaderCounters[i])
            out << " silent:" << kSilentNames[i] << "=" << m_shaderCounters[i];
    return out.str();
}

std::string GSExactRenderer::StatsSummary() const
{
    std::ostringstream out;
    static const auto s_start = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_start).count();
    out << "hires Mpx " << m_statHiArea * 1e-6 << " ";
    out << "game frames " << m_statSwaps << " (" << (secs > 0.0 ? m_statSwaps / secs : 0.0) << "/s avg) ";
    out << "stream copies " << m_statStreamCopies << " snapshot copies " << m_statSnapCopies << " clut readbacks taken "
        << m_statClutRbTaken << " ";
    out << "page readbacks " << m_statReadbacks << " " << m_statReadbackSec << "s clut readbacks " << m_statClutReadbackSec << "s ";
    out << "cpu pulls " << m_statCpuPulls << " cpu prims " << m_statCpuPrims << " cpu loads " << m_statCpuLoads << " clut mirror reads " << m_statClutMirrorReads
        << " cpu-checked " << m_statCpuChecked << " check pages " << m_statCheckPages << " check bad words " << m_statCheckBad << " ";
    out << "deferred " << m_statDeferred << " seq items " << m_statSeqItems << " seq dispatches " << m_statSeqDispatches << " ";
    out << "flush cpu " << m_statFlushCpuSec << "s (incl. waits on sampled GPU timers) gpu draw ~" << m_statGpuDrawSec << "s gpu clut ~"
        << m_statGpuClutSec << "s draw() cpu " << m_statDrawCpuSec << "s | draws " << m_statDraws << " flushes " << m_statFlushes << " clut loads " << m_statClutLoads << " pages up " << m_statUploadPages << " down "
        << m_statDownloadPages << " presents " << m_presentCount;
    return out.str();
}
