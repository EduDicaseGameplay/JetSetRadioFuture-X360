// The title's Direct3D 8 calls drawn with the Xbox 360's Direct3D 9 (Xenos), following the PC recomp's D3D12
// renderer (pc-recomp/sdk/runtime/gpu/d3d12.cpp) for what each console state means.
//
// Targets: every guest colour or depth surface the title draws to gets an EDRAM surface of its size and a linear
// texture its EDRAM contents resolve into. Only one target pair is in EDRAM at a time: switching resolves the old
// one and draws the new one's texture back. The title's draws keep its own pixel centres (D3D9's, HALFPIXELOFFSET
// off), so nothing is shifted. The frame is shown at 640x480 scaled to the console's output, 4:3 in the middle.
//
// Textures: guest textures decoded on the CPU (gpu/texture.cpp) into little endian formats, uploaded again when
// the title locked them since (gpu::DirtyEpoch) or their sampled bytes changed.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "gpu/backend.h"

#include <algorithm>
#include <map>
#include <set>
#include <math.h>
#include <string.h>
#include <unordered_map>
#include <vector>

#include <xtl.h>
#include <xgraphics.h>
#include <d3dx9mesh.h>

#include "core/log.h"
#include "gpu/shaders.h"
#include "host.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace gpu {

namespace {

// little endian texture formats: the decoded or guest bytes as they are (the GPU reads little endian natively)
const D3DFORMAT kFmtLeBgra = D3DFORMAT(MAKELINFMT(MAKED3DFMT(GPUTEXTUREFORMAT_8_8_8_8, GPUENDIAN_NONE, TRUE,
                                                            GPUSIGN_ALL_UNSIGNED, GPUNUMFORMAT_FRACTION, GPUSWIZZLE_ARGB)));
const D3DFORMAT kFmtLeDxt1 = D3DFORMAT(MAKELINFMT(MAKED3DFMT(GPUTEXTUREFORMAT_DXT1, GPUENDIAN_NONE, TRUE,
                                                            GPUSIGN_ALL_UNSIGNED, GPUNUMFORMAT_FRACTION, GPUSWIZZLE_ABGR)));
const D3DFORMAT kFmtLeDxt3 = D3DFORMAT(MAKELINFMT(MAKED3DFMT(GPUTEXTUREFORMAT_DXT2_3, GPUENDIAN_NONE, TRUE,
                                                            GPUSIGN_ALL_UNSIGNED, GPUNUMFORMAT_FRACTION, GPUSWIZZLE_ABGR)));
const D3DFORMAT kFmtLeDxt5 = D3DFORMAT(MAKELINFMT(MAKED3DFMT(GPUTEXTUREFORMAT_DXT4_5, GPUENDIAN_NONE, TRUE,
                                                            GPUSIGN_ALL_UNSIGNED, GPUNUMFORMAT_FRACTION, GPUSWIZZLE_ABGR)));

// bring-up: a 32 bit BMP in game:\dump, top row first; px(x, y) gives B G R A bytes
template <typename F>
void DumpBmp(const char* name, uint32_t w, uint32_t h, F px) {
    CreateDirectoryA("game:\\dump", NULL);
    char path[128];
    _snprintf(path, sizeof(path), "game:\\dump\\%s.bmp", name);
    FILE* f = fopen(path, "wb");
    if (!f) {
        LOGW("gpu: dump %s could not be written", path);
        return;
    }
    uint8_t hd[54];
    memset(hd, 0, sizeof(hd));
    uint32_t size = 54 + w * h * 4;
    int32_t neg_h = -int32_t(h);
    uint32_t v[] = {size, 0, 54, 40, w, uint32_t(neg_h)};
    hd[0] = 'B';
    hd[1] = 'M';
    for (int i = 0; i < 6; i++)
        for (int b = 0; b < 4; b++) hd[2 + i * 4 + b] = uint8_t(v[i] >> (b * 8));
    hd[26] = 1;   // planes
    hd[28] = 32;  // bits
    fwrite(hd, 1, 54, f);
    std::vector<uint8_t> row(w * 4);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) px(x, y, &row[x * 4]);
        fwrite(&row[0], 1, row.size(), f);
    }
    fclose(f);
    LOGI("gpu: dumped %s (%ux%u)", path, w, h);
}

inline float Bits(uint32_t v) {
    union { uint32_t u; float f; } c;
    c.u = v;
    return c.f;
}
inline DWORD FloatRs(float f) {
    union { uint32_t u; float f; } c;
    c.f = f;
    return c.u;
}

// byte / 255 from a table: an integer to float conversion goes through memory on the Xbox 360 (load-hit-store)
struct UnitTable {
    float v[256];
    UnitTable() {
        for (int i = 0; i < 256; i++) v[i] = i / 255.0f;
    }
};
const UnitTable g_unit;
void Color(uint32_t argb, float* o) {
    o[0] = g_unit.v[(argb >> 16) & 255];
    o[1] = g_unit.v[(argb >> 8) & 255];
    o[2] = g_unit.v[argb & 255];
    o[3] = g_unit.v[argb >> 24];
}

// the fixed function constants in the device remade by group (State::gen): only the groups whose inputs moved (v24 A);
// 0: only the world matrix's rows, when nothing else moved (v23 A)
#ifndef FF_GROUPS
#define FF_GROUPS 0
#endif

// v25: a draw's converted vertices kept in a vertex buffer of the GPU while the guest's are unchanged (most of a
// fight's meshes are static: the vertex programs skin them), its indices written to a ring; 0: every draw converts
// its vertices and copies them into the command buffer (DrawIndexedPrimitiveUP), as up to v24
#ifndef VERTEX_CACHE
#define VERTEX_CACHE 1
#endif
const bool kVertexCache = VERTEX_CACHE != 0;
const size_t kVertexCacheBytes = 64u << 20;
int64_t g_asm_indices = 0;
const uint32_t kSystemPool = 0xB4000000;  // DrawUP's ring and other host memory (kernel/memory.h): never cached

// 16 samples of a range of guest bytes and its last 8
uint64_t QuickSig(const uint8_t* p, uint32_t bytes) {
    uint64_t h = 1469598103934665603ull;
    if (bytes < 8) return h;
    uint32_t step = bytes / 16 < 8 ? 8 : bytes / 16;
    for (uint32_t o = 0; o + 8 <= bytes; o += step) {
        uint64_t v;
        memcpy(&v, p + o, 8);
        h = (h ^ v) * 1099511628211ull;
    }
    uint64_t v;
    memcpy(&v, p + bytes - 8, 8);
    return (h ^ v) * 1099511628211ull;
}
const bool kFfGroups = FF_GROUPS != 0;

// RequestCapture: a capture starts at the next Present; the target is saved every kCaptureStep draws of a full size one
volatile bool g_capture_want = false;
// JSRF: a folha de pecas do HUD, 256x256 DXT3, pelo conteudo: pode carregar em qualquer endereco
const uint64_t kHudSheetHash = 0x3341314EF4D35C0Dull;

// JSRF: painel de texto. Fonte 5x7 embutida, ASCII 0x20 a 0x5A, por colunas.
char g_osd_text[768] = {0};
volatile uint32_t g_osd_seq = 0;
const uint8_t kFont5x7[59][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x56,0x20,0x50},{0x00,0x08,0x07,0x03,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x2A,0x1C,0x7F,0x1C,0x2A},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x80,0x70,0x30,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x00,0x60,0x60,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x72,0x49,0x49,0x49,0x46},{0x21,0x41,0x49,0x4D,0x33},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x31},{0x41,0x21,0x11,0x09,0x07},
    {0x36,0x49,0x49,0x49,0x36},{0x46,0x49,0x49,0x29,0x1E},{0x00,0x00,0x14,0x00,0x00},{0x00,0x40,0x34,0x00,0x00},
    {0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x59,0x09,0x06},
    {0x3E,0x41,0x5D,0x59,0x4E},{0x7C,0x12,0x11,0x12,0x7C},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x41,0x3E},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x41,0x51,0x73},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x1C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x26,0x49,0x49,0x49,0x32},
    {0x03,0x01,0x7F,0x01,0x03},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},{0x03,0x04,0x78,0x04,0x03},{0x61,0x59,0x49,0x4D,0x43}};
const uint32_t kCaptureStep = 40;

inline int64_t CpTicks() { return 0; }

// parts of the constants' time, logged apart: fixed function made whole, in part, vertex program, pixel, binds
enum { kCpFfWhole, kCpFfPart, kCpProgram, kCpPixel, kCpBinds, kCpCount };
int64_t g_cprof[kCpCount];
uint32_t g_ccount[kCpCount];

// D3DBLEND, the console uses GL values (the PC renderer's Blend)
DWORD Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return D3DBLEND_ZERO;
        case 1: return D3DBLEND_ONE;
        case 2: return D3DBLEND_ONE;
        case 3: return alpha ? D3DBLEND_SRCALPHA : D3DBLEND_SRCCOLOR;
        case 4: return alpha ? D3DBLEND_INVSRCALPHA : D3DBLEND_INVSRCCOLOR;
        case 5: return D3DBLEND_SRCALPHA;
        case 6: return D3DBLEND_INVSRCALPHA;
        case 7: return D3DBLEND_DESTALPHA;
        case 8: return D3DBLEND_INVDESTALPHA;
        case 9: return alpha ? D3DBLEND_DESTALPHA : D3DBLEND_DESTCOLOR;
        case 10: return alpha ? D3DBLEND_INVDESTALPHA : D3DBLEND_INVDESTCOLOR;
        case 11: return alpha ? D3DBLEND_ONE : D3DBLEND_SRCALPHASAT;
        case 14: return D3DBLEND_BLENDFACTOR;
        case 15: return D3DBLEND_INVBLENDFACTOR;
        case 0x300: return alpha ? D3DBLEND_SRCALPHA : D3DBLEND_SRCCOLOR;
        case 0x301: return alpha ? D3DBLEND_INVSRCALPHA : D3DBLEND_INVSRCCOLOR;
        case 0x302: return D3DBLEND_SRCALPHA;
        case 0x303: return D3DBLEND_INVSRCALPHA;
        case 0x304: return D3DBLEND_DESTALPHA;
        case 0x305: return D3DBLEND_INVDESTALPHA;
        case 0x306: return alpha ? D3DBLEND_DESTALPHA : D3DBLEND_DESTCOLOR;
        case 0x307: return alpha ? D3DBLEND_INVDESTALPHA : D3DBLEND_INVDESTCOLOR;
        case 0x308: return alpha ? D3DBLEND_ONE : D3DBLEND_SRCALPHASAT;
        case 0x8001: case 0x8003: return D3DBLEND_BLENDFACTOR;
        case 0x8002: case 0x8004: return D3DBLEND_INVBLENDFACTOR;
        default: return D3DBLEND_ONE;
    }
}

// D3DBLENDOP, D3D and GL values
DWORD BlendOp(uint32_t v) {
    switch (v) {
        case 2: case 0x800A: return D3DBLENDOP_SUBTRACT;
        case 3: case 0x800B: return D3DBLENDOP_REVSUBTRACT;
        case 4: case 0x8007: return D3DBLENDOP_MIN;
        case 5: case 0x8008: return D3DBLENDOP_MAX;
        default: return D3DBLENDOP_ADD;
    }
}

// D3D8's D3DCMP numbering, 1 to 8, from D3D and GL values (the shaders' alpha and shadow tests use it)
uint32_t Cmp(uint32_t v) {
    if (v >= 0x200 && v <= 0x207) return v - 0x200 + 1;
    if (v >= 1 && v <= 8) return v;
    return 8;
}

// the Xbox 360's D3DCMPFUNC (d3d9types.h: NEVER 0 to ALWAYS 7, not the PC's 1 to 8), from D3D and GL values
D3DCMPFUNC CmpState(uint32_t v) {
    static const D3DCMPFUNC k[8] = {D3DCMP_NEVER, D3DCMP_LESS, D3DCMP_EQUAL, D3DCMP_LESSEQUAL,
                                    D3DCMP_GREATER, D3DCMP_NOTEQUAL, D3DCMP_GREATEREQUAL, D3DCMP_ALWAYS};
    return k[Cmp(v) - 1];
}

// D3DSTENCILOP 1 to 8, from D3D and GL values
DWORD StencilOp(uint32_t v) {
    switch (v) {
        case 1: return D3DSTENCILOP_KEEP;
        case 0: case 2: return D3DSTENCILOP_ZERO;
        case 3: case 0x1E01: return D3DSTENCILOP_REPLACE;
        case 4: case 0x1E02: return D3DSTENCILOP_INCRSAT;
        case 5: case 0x1E03: return D3DSTENCILOP_DECRSAT;
        case 6: case 0x150A: return D3DSTENCILOP_INVERT;
        case 7: case 0x8507: return D3DSTENCILOP_INCR;
        case 8: case 0x8508: return D3DSTENCILOP_DECR;
        default: return D3DSTENCILOP_KEEP;
    }
}

// D3DTADDRESS: 1 wrap, 2 mirror, 3 clamp, 4 border, 5 clamp to edge, plus GL values
DWORD Address(uint32_t v) {
    switch (v) {
        case 2: case 0x8370: return D3DTADDRESS_MIRROR;
        case 3: case 5: case 0x2900: case 0x812F: return D3DTADDRESS_CLAMP;
        case 4: case 0x812D: return D3DTADDRESS_BORDER;
        default: return D3DTADDRESS_WRAP;
    }
}

// D3DTEXF: 1 point, 2 linear, 3 anisotropic and up
DWORD Filter(uint32_t v) { return v == 0 ? D3DTEXF_NONE : v == 1 ? D3DTEXF_POINT : v == 2 ? D3DTEXF_LINEAR : D3DTEXF_ANISOTROPIC; }

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++)
            o[r * 4 + col] =
                x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] + x[r * 4 + 3] * y[12 + col];
}

// HLSL mul(row vector, matrix) with column major packing wants the transpose
void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++)
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (col + 1) % 3, c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
}

// D3DTRANSFORMSTATETYPE indices into s.transforms
const size_t kTsView = 2, kTsProjection = 3, kTsTexture0 = 16, kTsWorld = 256;

// D3DLIGHT8 as floats: Type, Diffuse[4], Specular[4], Ambient[4], Position[3], Direction[3], Range, Falloff,
// Attenuation0 to 2, Theta, Phi
void LightToView(const std::array<uint32_t, 26>& l, const float* vm, float* o /* 7 x 4 */) {
    float f[26];
    for (int i = 0; i < 26; i++) f[i] = Bits(l[i]);
    for (int j = 0; j < 4; j++) {
        o[0 * 4 + j] = f[1 + j];
        o[1 * 4 + j] = f[5 + j];
        o[2 * 4 + j] = f[9 + j];
    }
    const float* pos = f + 13;
    const float* dir = f + 16;
    for (int j = 0; j < 3; j++) {
        o[3 * 4 + j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4 * 4 + j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3 * 4 + 3] = f[19];
    o[4 * 4 + 3] = f[20];
    o[5 * 4 + 0] = f[21];
    o[5 * 4 + 1] = f[22];
    o[5 * 4 + 2] = f[23];
    o[6 * 4 + 0] = cosf(f[24] * 0.5f);
    o[6 * 4 + 1] = cosf(f[25] * 0.5f);
}

// a sampled signature of guest bytes: catches writes made without a lock
uint64_t SampleSig(const uint8_t* p, uint32_t bytes) {
    uint64_t h = 1469598103934665603ull;
    uint32_t step = bytes > 4096 ? bytes / 64 : 64;
    for (uint32_t o = 0; o + 8 <= bytes; o += step) {
        uint64_t v;
        memcpy(&v, p + o, 8);
        h = (h ^ v) * 1099511628211ull;
    }
    return h;
}

const char* kBlitVs =
    "struct O { float4 p : POSITION; float2 uv : TEXCOORD0; };\n"
    "O main(float2 p : POSITION, float2 uv : TEXCOORD0) { O o; o.p = float4(p, 0, 1); o.uv = uv; return o; }\n";
const char* kBlitPs =
    "sampler s : register(s0);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR0 { return tex2D(s, uv); }\n";
// The picture as shown: TVs show the console's output darker than the title was made to look (v33: frame buffer against
// the video, mid tones at about x^1.3: 80 -> 54, 112 -> 85, 144 -> 129; the Killer Instinct port lifts them the same way).
const char* kPresentPs =
    "sampler s : register(s0);\n"
    "float4 g : register(c0);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR0 { float4 c = tex2D(s, uv); return float4(pow(saturate(c.rgb), g.x), c.a); }\n";
// o modo original escalado para a tela, com um pouco de nitidez
const char* kPresentSharpPs =
    "sampler s : register(s0);\n"
    "float4 g : register(c0);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "    float4 c = tex2D(s, uv);\n"
    "    float3 n = tex2D(s, uv + float2(g.y, 0)).rgb + tex2D(s, uv - float2(g.y, 0)).rgb + tex2D(s, uv + float2(0, g.z)).rgb + tex2D(s, uv - float2(0, g.z)).rgb;\n"
    "    float3 d = c.rgb + (c.rgb - n * 0.25) * 0.45;\n"
    "    return float4(pow(saturate(d), g.x), c.a);\n"
    "}\n";
const int kGammas[] = {145, 100, 115, 130};  // in hundredths; the first is the default
volatile int g_gamma_i = 0;

// one guest surface the title draws to
struct Target {
    uint32_t data, w, h;  // w, h: o tamanho real
    uint32_t lw, lh;      // o tamanho que o jogo pediu: quadros de 640x480 sao desenhados em 1280x720 numa TV HD
    bool depth;
    D3DSurface* surf;    // EDRAM
    D3DTexture* tex;     // what the EDRAM resolves into
    bool has_content;    // tex holds the target's pixels
    bool edram_dirty;    // drawn since the last resolve
    uint64_t cpu_epoch;  // the guest memory's DirtyEpoch when it was last read or written back
    uint64_t cpu_sig;
    Target() : data(0), w(0), h(0), lw(0), lh(0), depth(false), surf(NULL), tex(NULL), has_content(false), edram_dirty(false),
               cpu_epoch(0), cpu_sig(0) {}
};

struct Tex {
    D3DBaseTexture* tex;
    TexInfo info;
    uint64_t epoch, sig;
    uint32_t pal;
    uint64_t pal_hash;
    uint32_t frame;  // the frame its guest memory was last checked in, plus one
    Tex() : tex(NULL), epoch(0), sig(0), pal(0), pal_hash(0), frame(0) { memset(&info, 0, sizeof(info)); }
};

// where a frame's time goes: performance counter ticks per part, logged with the frame rate
enum {
    kPfAssemble, kPfTexture, kPfUpload, kPfBind, kPfShader, kPfState, kPfSampler, kPfConst, kPfDraw, kPfPresent, kPfCount
};
int64_t g_prof[kPfCount];
uint32_t g_uploads;
// the time base register: one instruction, where QueryPerformanceCounter is a call
inline int64_t Ticks() { return int64_t(__mftb()); }
// time base ticks a second, measured against QueryPerformanceCounter once
double TicksPerSecond() {
    static double f = 0;
    if (f == 0) {
        LARGE_INTEGER q0, q1, qf;
        QueryPerformanceFrequency(&qf);
        QueryPerformanceCounter(&q0);
        int64_t t0 = Ticks();
        Sleep(50);
        QueryPerformanceCounter(&q1);
        int64_t t1 = Ticks();
        f = double(t1 - t0) * double(qf.QuadPart) / double(q1.QuadPart - q0.QuadPart);
        LOGI("gpu: time base %.0f ticks a second", f);
    }
    return f;
}

class XenosBackend : public Backend {
public:
    XenosBackend()
        : d3d_(NULL), dev_(NULL), host_bb_(NULL), rt_(NULL), ds_(NULL), owner_(0), out_w_(1280), out_h_(720), hd_targets_(false), hiz_on_(false), msaa_(false), aniso_(false), hidef_(false), wide_tv_(false), mode_now_(-1), present_sharp_ps_(NULL), wide_shift_now_(1e9f), wide_scale_now_(1e9f), hud_next_(0xFFFFFFFFu), hud_side_(0),
          guest_w_(640), guest_h_(480), osd_tex_(NULL), osd_seq_(0), blit_vs_(NULL), blit_ps_(NULL), present_ps_(NULL), blit_shown_(false), movie_tex_(NULL), movie_last_(NULL), blit_decl_(NULL), vs_fixed_last_(true),
          vsc_epoch_(0), gc_hit_(0), gc_miss_(0), gc_store_(0), last_vs_(NULL), cur_decl_(NULL), cur_vs_(NULL), cur_ps_(NULL), pk_ps_(NULL), pk_gen_(0), pk_handle_(0), cur_vp_w_(0), cur_vp_h_(0), vs_memo_prog_(NULL), vs_memo_regs_(0), vs_memo_(NULL), surf_w_(0), surf_h_(0), surf_z_(0), cap_(NULL), cap_n_(0), cap_draw_(0), vcache_bytes_(0), vc_hit_(0), vc_miss_(0), vc_none_(0), px_gen_(0), ff_w_(0), ff_h_(0), frames_(0), draws_(0), skip_assemble_(0), skip_bind_(0), skip_shader_(0),
          skip_prims_(0), memo_ok_(false), ps_known_(false), vs_known_(false), ff_loaded_(false), tail_known_(false) {
        memset(memo_, 0, sizeof(memo_));
        memset(surf_f_, 0, sizeof(surf_f_));
        memset(&cur_scissor_, 0, sizeof(cur_scissor_));
        for (int i = 0; i < 4; i++) cur_tex_[i] = reinterpret_cast<D3DBaseTexture*>(1);
        memset(rs_known_, 0, sizeof(rs_known_));
        memset(ss_known_, 0, sizeof(ss_known_));
    }

    bool Init(uint32_t width, uint32_t height);
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices);
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points);
    void Present(const Surface& front);
    uint32_t CountNot(Target* t, uint32_t bg);
    bool ReadPixels(Target* t, std::vector<uint32_t>& px);
    void WritePixels(Target* t, const std::vector<uint32_t>& px);
    void Ablate(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices);
    void Sync(uint32_t data, uint32_t size);
    void SyncLate(uint32_t data) { Sync(data, 0); }
    void Flush() {}
    void SetGamma(const uint8_t* ramp);

private:
    void Own() {
        DWORD me = GetCurrentThreadId();
        if (me != owner_) {
            dev_->AcquireThreadOwnership();
            owner_ = me;
        }
    }
    Target* FindTarget(uint32_t data, bool depth, uint32_t w, uint32_t h);
    Target* EnsureTarget(const Surface& s, bool depth, uint32_t w, uint32_t h);
    void Resolve(Target* t);
    void ResolveBound();
    bool Bind(const State& s);
    void UploadFromGuest(Target* t, const Surface& s);
    void Blit(D3DTexture* tex, const RECT& dst, uint32_t tw, uint32_t th);
    bool Compile(const std::string& src, const char* profile, ID3DXBuffer** out);
    D3DVertexShader* VertexShaderFor(const State& s, const Assembled& a, bool& fixed);
    D3DPixelShader* PixelShaderFor(const PixelKey& k);
    D3DVertexDeclaration* DeclFor(uint16_t regs);
    D3DBaseTexture* BindTexture(const State& s, int stage, TexInfo& info, TextureDesc& desc);
    D3DBaseTexture* BindTextureSlow(const State& s, int stage, TexInfo& info, TextureDesc& desc);
    void SetStates(const State& s, const Assembled& a);
    void SetSamplers(const State& s, int stage, bool rect);
    void SetConstants(const State& s, bool fixed, const TextureDesc* desc, const bool* bound);

    IDirect3D9* d3d_;
    D3DDevice* dev_;
    D3DSurface* host_bb_;
    Target* rt_;
    Target* ds_;
    DWORD owner_;
    uint32_t out_w_, out_h_, guest_w_, guest_h_;
    bool hd_targets_, hiz_on_, msaa_, aniso_, hidef_, wide_tv_;
    int mode_now_;
    void ApplyVideoMode(int mode);
    void EvictTextures();
    D3DPixelShader* present_sharp_ps_;
    // JSRF, widescreen: deslocamento em x, NDC, deste desenho; pecas do HUD indo para a borda
    float wide_shift_now_, wide_scale_now_;
    uint32_t hud_next_;
    int hud_side_;
    float WideOverlay(const State& s, Assembled& a, const float* verts, float& xscale);
    bool HudSheet(const State& s);
    std::unordered_map<uint64_t, uint64_t> hud_hash_;
    std::map<std::pair<uint64_t, uint32_t>, Target*> targets_;  // (data << 1 | depth, w << 16 | h)
    std::unordered_map<uint64_t, Tex> textures_;
    // a guest texture already checked this frame, by its header: what BindTexture found for it
    struct TexMemo {
        uint32_t header, words[4], palette, frame;
        D3DBaseTexture* tex;
        TexInfo info;
        TextureDesc desc;
    };
    TexMemo memo_[1024];
    // the render and sampler states last given to the device: the same value again is not sent
    DWORD rs_value_[512], ss_value_[8][128];
    bool rs_known_[512], ss_known_[8][128];
    void Rs(D3DRENDERSTATETYPE t, DWORD v) {
        if (unsigned(t) < 512) {
            if (rs_known_[t] && rs_value_[t] == v) return;
            rs_known_[t] = true;
            rs_value_[t] = v;
        }
        dev_->SetRenderState(t, v);
    }
    void Ss(DWORD i, D3DSAMPLERSTATETYPE t, DWORD v) {
        if (i < 8 && unsigned(t) < 128) {
            if (ss_known_[i][t] && ss_value_[i][t] == v) return;
            ss_known_[i][t] = true;
            ss_value_[i][t] = v;
        }
        dev_->SetSamplerState(i, t, v);
    }
    // forgotten whenever the device may have changed them itself
    void ForgetStates() {
        memset(rs_known_, 0, sizeof(rs_known_));
        memset(ss_known_, 0, sizeof(ss_known_));
        ps_known_ = false;
        vs_known_ = false;
        ff_loaded_ = false;
        tail_known_ = false;
        for (int i = 0; i < 4; i++) cur_tex_[i] = reinterpret_cast<D3DBaseTexture*>(1);
        cur_vp_w_ = 0;
        wide_shift_now_ = 1e9f;
        cur_decl_ = NULL;
        cur_vs_ = NULL;
        cur_ps_ = NULL;
    }
    // what the device has bound, while ForgetStates has not been called: the same again is not set
    D3DVertexDeclaration* cur_decl_;
    D3DVertexShader* cur_vs_;
    D3DPixelShader* cur_ps_;
    // what the device was last given, while ForgetStates has not been called: the same again is not given
    D3DBaseTexture* cur_tex_[4];
    uint32_t cur_vp_w_, cur_vp_h_;  // 0: not known
    RECT cur_scissor_;
    D3DPixelShader* pk_ps_;
    uint32_t pk_gen_, pk_handle_, pk_info_[4];
    std::shared_ptr<PixelShader> pk_keep_ps_;
    std::shared_ptr<VertexShader> pk_keep_vs_;
    // the shaders last found: a key made and hashed for every draw, most finding the same few again
    struct PsMemo {
        D3DPixelShader* ps;
        PixelKey key;
        PsMemo() : ps(NULL) {}
    };
    PsMemo ps_memo_[64];
    const VertexShader* vs_memo_prog_;
    std::shared_ptr<VertexShader> vs_memo_keep_;
    uint16_t vs_memo_regs_;
    D3DVertexShader* vs_memo_;
    // the target size and depth scale as floats (an integer to float conversion goes through memory)
    uint32_t surf_w_, surf_h_;
    float surf_z_, surf_f_[4];
    // frame capture (RequestCapture)
    FILE* cap_;
    uint32_t cap_n_, cap_draw_;
    float cap_vsc_[192 * 4];
    std::set<uint64_t> cap_seen_;
    void CaptureDraw(const State& s, const Assembled& a, const TexInfo* info, const TextureDesc* desc, const bool* bound, bool fixed);
    void CaptureTarget(Target* t, const char* when);
    // vertex cache (kVertexCache): converted vertices by where they came from
    // JSRF: guarda os vertices convertidos na memoria do jogo e desenha pelo mesmo caminho UP de sempre. Com vertex buffer
    // e DrawIndexedPrimitive a geometria saia errada no Xenia (v03, v05) e a causa nao foi achada.
    struct VCache {
        float* vb;
        uint32_t bytes, frame, misses;
        uint64_t epoch, sig;
        bool seen;
        VCache() : vb(NULL), bytes(0), frame(0), misses(0), epoch(0), sig(0), seen(false) {}
    };
    std::unordered_map<uint64_t, VCache> vcache_;
    // JSRF: desenhos indexados inteiros, listas e vertices prontos, pelo endereco dos indices no jogo. A montagem das listas
    // custava mais que a conversao: no Xenia, 8,5 de 10 ms por quadro em Skyscraper.
    struct GCache {
        float* verts;
        uint32_t* idx;
        uint32_t bytes, icount, vertex_count, frame, misses, nregs, nsrc, src[4], src_bytes[4];
        uint64_t sig;
        uint16_t regs;
        Topology topology;
        bool pretransformed, seen, skip;
        GCache() : verts(NULL), idx(NULL), bytes(0), icount(0), vertex_count(0), frame(0), misses(0), nregs(0), nsrc(0), sig(0), regs(0),
                   topology(kTopTriangles), pretransformed(false), seen(false), skip(false) {}
    };
    std::unordered_map<uint64_t, GCache> gcache_;
    uint32_t gc_hit_, gc_miss_, gc_store_;
    GCache* LookupGeometry(const State& s, uint32_t prim, uint32_t index_va, uint32_t count, const uint8_t* ip, uint64_t& index_sig);
    void StoreGeometry(GCache& g, const Assembled& a, uint64_t index_sig, uint32_t index_bytes);
    uint64_t GeometrySig(const GCache& g, uint64_t index_sig);
    void DropGeometry(GCache& g);
    size_t vcache_bytes_;
    uint32_t vc_hit_, vc_miss_, vc_none_;
    VCache* CachedVertices(Assembled& a);
    void TrimVertexCache();
    // SetPixelConstants' inputs for ps_last_: State::gen[kGenPixel], the target and texture sizes
    uint32_t px_gen_, px_key_[11];
    bool FfPart(const State& s, uint32_t W, uint32_t H, const float* surf, const float* fog);
    // the constants last given to the device
    float ps_last_[kPsCount * 4], vs_last_[192 * 4];
    bool ps_known_, vs_known_;
    // the fixed function constants' inputs last made, while they are in the device
    struct FfInputs {
        float surf[4], fog[4], world[16], view[16], proj[16], tex[4][16], material[17];
        uint32_t ambient, light_enable, lights[8][26], vp[6], wh[2];
    };
    FfInputs ff_last_;
    bool ff_loaded_;
    // what the fixed function constants in the device were made from: State::ff_gen and world_gen, the target size,
    // and view * projection * viewport for a world matrix alone
    uint32_t ff_g_[State::kGenFog + 1], ff_w_, ff_h_;
    float ff_vpf_[16];
    float tail_last_[8];
    bool tail_known_;
    void SetPixelConstants(const State& s, const TextureDesc* desc, const bool* bound, const float* surf);
    bool memo_ok_;  // BindTextureSlow's answer may be kept (a guest texture, not a target)
    std::unordered_map<uint64_t, D3DVertexShader*> vs_;
    std::unordered_map<uint64_t, D3DPixelShader*> ps_;
    std::unordered_map<uint32_t, D3DVertexDeclaration*> decls_;
    D3DVertexShader* blit_vs_;
    D3DPixelShader* blit_ps_;
    D3DPixelShader* present_ps_;
    bool blit_shown_;  // the next Blit is the picture shown
    D3DTexture* movie_tex_;
    D3DTexture* osd_tex_;
    uint32_t osd_seq_;
    void DrawOsd();
    const uint8_t* movie_last_;
    D3DVertexDeclaration* blit_decl_;
    bool vs_fixed_last_;
    uint64_t vsc_epoch_;
    D3DVertexShader* last_vs_;
    uint32_t frames_, draws_;
    // bring-up diagnostics: draws dropped, by reason
    uint32_t skip_assemble_, skip_bind_, skip_shader_, skip_prims_;
};

bool XenosBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    XVIDEO_MODE mode;
    memset(&mode, 0, sizeof(mode));
    XGetVideoMode(&mode);
    out_w_ = mode.fIsHiDef ? 1280 : 640;
    out_h_ = mode.fIsHiDef ? 720 : 480;
    hidef_ = mode.fIsHiDef != 0;
    wide_tv_ = mode.fIsHiDef && mode.fIsWideScreen && GetFileAttributesA("game:\\sem_widescreen.txt") == DWORD(-1);
    {
        int saved = 0;
        FILE* vf = fopen("game:\\video_modo.txt", "rb");
        if (vf) {
            if (fscanf(vf, "%d", &saved) != 1) saved = 0;
            fclose(vf);
        }
        if (GetFileAttributesA("game:\\sem_720p.txt") != DWORD(-1)) saved = 1;
        g_video_mode = saved == 1 ? 1 : 0;
    }
    ApplyVideoMode(g_video_mode);
    // JSRF: desligado por padrao ate haver medida no console; game:\\com_hiz.txt liga
    hiz_on_ = GetFileAttributesA("game:\\com_hiz.txt") != DWORD(-1);
    d3d_ = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d_) return false;
    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth = out_w_;
    pp.BackBufferHeight = out_h_;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    pp.EnableAutoDepthStencil = FALSE;
    // vertices and indices of the title's draws pass through the ring buffer
    pp.RingBufferParameters.SecondarySize = 16u << 20;
    if (FAILED(d3d_->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev_))) {
        LOGE("gpu: CreateDevice failed");
        return false;
    }
    owner_ = GetCurrentThreadId();
    dev_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &host_bb_);
    ID3DXBuffer* code = NULL;
    if (Compile(kBlitVs, "vs_3_0", &code)) {
        dev_->CreateVertexShader(static_cast<const DWORD*>(code->GetBufferPointer()), &blit_vs_);
        code->Release();
    }
    if (Compile(kBlitPs, "ps_3_0", &code)) {
        dev_->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &blit_ps_);
        code->Release();
    }
    if (Compile(kPresentPs, "ps_3_0", &code)) {
        dev_->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &present_ps_);
        code->Release();
    }
    if (Compile(kPresentSharpPs, "ps_3_0", &code)) {
        dev_->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &present_sharp_ps_);
        code->Release();
    }
    static const D3DVERTEXELEMENT9 el[] = {
        {0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()};
    dev_->CreateVertexDeclaration(el, &blit_decl_);
    LOGI("gpu: vertex cache %s", kVertexCache ? "on" : "off");
    LOGI("gpu: Xenos renderer (v41, built " __DATE__ " " __TIME__ "), guest %ux%u shown at %ux%u", width, height, out_w_, out_h_);
    // bring-up: does a resolve reach a linear texture, and a tiled one? EDRAM cleared to red, both read back
    {
        D3DSurface* rt = NULL;
        D3DTexture *lin = NULL, *til = NULL;
        D3DSURFACE_PARAMETERS sp;
        memset(&sp, 0, sizeof(sp));
        dev_->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, &sp);
        dev_->CreateTexture(64, 64, 1, 0, D3DFMT_LIN_A8R8G8B8, 0, &lin, NULL);
        dev_->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, 0, &til, NULL);
        if (rt && lin && til) {
            dev_->SetRenderTarget(0, rt);
            dev_->Clear(0, NULL, D3DCLEAR_TARGET, 0xFFFF0000, 1.0f, 0);
            dev_->Resolve(D3DRESOLVE_RENDERTARGET0, NULL, lin, NULL, 0, 0, NULL, 0, 0, NULL);
            dev_->Resolve(D3DRESOLVE_RENDERTARGET0, NULL, til, NULL, 0, 0, NULL, 0, 0, NULL);
            dev_->BlockUntilIdle();
            D3DLOCKED_RECT lr;
            uint32_t a = 0, b = 0;
            if (SUCCEEDED(lin->LockRect(0, &lr, NULL, D3DLOCK_READONLY))) {
                a = static_cast<const uint32_t*>(lr.pBits)[0];
                lin->UnlockRect(0);
            }
            if (SUCCEEDED(til->LockRect(0, &lr, NULL, D3DLOCK_READONLY))) {
                b = static_cast<const uint32_t*>(lr.pBits)[0];
                til->UnlockRect(0);
            }
            LOGI("selftest 0 resolve: linear %08X, tiled %08X (want FFFF0000)", a, b);
            dev_->SetRenderTarget(0, host_bb_);
        } else {
            LOGE("selftest 0 resolve: could not make its surfaces");
        }
        if (rt) rt->Release();
        if (lin) lin->Release();
        if (til) til->Release();
    }
    return blit_vs_ && blit_ps_ && blit_decl_;
}

// compiled shaders kept in game:\shadercache by their source's hash: the console's compiler takes 0.2 to 0.4 s a shader,
// a stutter whenever a fight shows something new. All of them are read at the first compile (the device's start).
std::map<uint64_t, std::vector<uint8_t> >& ShaderDisk() {
    static std::map<uint64_t, std::vector<uint8_t> > disk;
    static bool loaded = false;
    if (loaded) return disk;
    loaded = true;
    CreateDirectoryA("game:\\shadercache", NULL);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("game:\\shadercache\\*.bin", &fd);
    if (h == INVALID_HANDLE_VALUE) return disk;
    do {
        unsigned long long key = 0;
        if (sscanf(fd.cFileName, "%16llX", &key) != 1) continue;
        char path[MAX_PATH];
        _snprintf(path, sizeof(path), "game:\\shadercache\\%s", fd.cFileName);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        std::vector<uint8_t> bytes(fd.nFileSizeLow);
        if (!bytes.empty() && fread(&bytes[0], 1, bytes.size(), f) == bytes.size()) disk[key].swap(bytes);
        fclose(f);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    LOGI("gpu: %u compiled shaders read from game:\\shadercache", unsigned(disk.size()));
    return disk;
}

bool XenosBackend::Compile(const std::string& src, const char* profile, ID3DXBuffer** out) {
    std::map<uint64_t, std::vector<uint8_t> >& disk = ShaderDisk();
    const uint64_t key = Hash(src.data(), src.size(), Hash(profile, strlen(profile)));
    std::map<uint64_t, std::vector<uint8_t> >::iterator it = disk.find(key);
    if (it != disk.end() && SUCCEEDED(D3DXCreateBuffer(DWORD(it->second.size()), out)) && *out) {
        memcpy((*out)->GetBufferPointer(), &it->second[0], it->second.size());
        return true;
    }
    ID3DXBuffer* err = NULL;
    HRESULT hr = D3DXCompileShader(src.c_str(), UINT(src.size()), NULL, NULL, "main", profile, 0, out, &err, NULL);
    if (SUCCEEDED(hr) && *out) {
        const uint8_t* p = static_cast<const uint8_t*>((*out)->GetBufferPointer());
        std::vector<uint8_t>& kept = disk[key];
        kept.assign(p, p + (*out)->GetBufferSize());
        char path[MAX_PATH];
        _snprintf(path, sizeof(path), "game:\\shadercache\\%016llX.bin", (unsigned long long)key);
        FILE* f = fopen(path, "wb");
        if (f) {
            fwrite(&kept[0], 1, kept.size(), f);
            fclose(f);
        }
    }
    if (FAILED(hr)) {
        static int failures = 0;
        if (failures++ < 8) {
            LOGE("gpu: %s compile failed: %s", profile, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            // the source, a line at a time, for the first ones
            size_t p = 0;
            int line = 1;
            while (p < src.size() && line < 400) {
                size_t e = src.find('\n', p);
                if (e == std::string::npos) e = src.size();
                LOGE("  %3d %s", line++, src.substr(p, e - p).c_str());
                p = e + 1;
            }
        }
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();
    return true;
}

Target* XenosBackend::FindTarget(uint32_t data, bool depth, uint32_t w, uint32_t h) {
    std::map<std::pair<uint64_t, uint32_t>, Target*>::iterator it =
        targets_.find(std::make_pair(uint64_t(data) << 1 | (depth ? 1 : 0), w << 16 | h));
    return it == targets_.end() ? NULL : it->second;
}

Target* XenosBackend::EnsureTarget(const Surface& s, bool depth, uint32_t w, uint32_t h) {
    Target* t = FindTarget(s.data, depth, w, h);
    if (t) return t;
    t = new Target;
    t->data = s.data;
    t->lw = w;
    t->lh = h;
    if (hd_targets_ && w == 640 && h == 480) {
        w = 1280;
        h = 720;
    }
    const D3DMULTISAMPLE_TYPE aa = msaa_ && w == 640 && h == 480 ? D3DMULTISAMPLE_4_SAMPLES : D3DMULTISAMPLE_NONE;
    t->w = w;
    t->h = h;
    t->depth = depth;
    D3DSURFACE_PARAMETERS sp;
    memset(&sp, 0, sizeof(sp));
    if (depth) {
        // after the largest colour surface's tiles: 1280x720 is the most a guest target here takes
        sp.Base = msaa_ ? XGSurfaceSize(640, 480, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES) : XGSurfaceSize(1280, 720, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE);
        // Hi-Z memory from its start, as every XDK sample has it (0xFFFFFFFF pointed Hi-Z at nothing, and every pixel
        // failed); Hi-Z itself is off in SetStates, as the targets share that memory
        sp.HierarchicalZBase = 0;
        sp.HiZFunc = D3DHIZFUNC_DEFAULT;
        dev_->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, aa, 0, FALSE, &t->surf, &sp);
    } else {
        sp.Base = 0;
        dev_->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, aa, 0, FALSE, &t->surf, &sp);
        // tiled, as Resolve writes it (v12's dumps: a LIN_ texture got tiled pixels and was sampled as linear, the
        // picture in streaks); the CPU goes through ReadPixels and WritePixels
        dev_->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, 0, &t->tex, NULL);
    }
    if (!t->surf || (!depth && !t->tex)) LOGE("gpu: target %08X %ux%u%s could not be made", s.data, w, h, depth ? " depth" : "");
    targets_[std::make_pair(uint64_t(s.data) << 1 | (depth ? 1 : 0), t->lw << 16 | t->lh)] = t;
    LOGI("gpu: target %08X %ux%u%s", s.data, w, h, depth ? " depth" : "");
    return t;
}

// JSRF: o cache de texturas nunca soltava nada: cada fase somava as suas as das anteriores (62 texturas no inicio, 1646
// depois das 14 fases, e o console desligou na 15a troca de fase, console_boot18). A cada 300 quadros, solta as que nao
// foram usadas nos ultimos 900 quadros (30 s a 30 fps); a GPU terminou com elas ha muito. Antes, tira todas do
// dispositivo e esquece o que estava ligado, para um ponteiro reaproveitado nao passar pelo teste de "mesma textura".
void XenosBackend::EvictTextures() {
    if (frames_ < 900) return;
    std::vector<uint64_t> old;
    for (std::unordered_map<uint64_t, Tex>::iterator it = textures_.begin(); it != textures_.end(); ++it)
        if (it->second.frame + 900 < frames_) old.push_back(it->first);
    if (old.empty()) return;
    for (int i = 0; i < 4; i++) {
        dev_->SetTexture(i, NULL);
        cur_tex_[i] = reinterpret_cast<D3DBaseTexture*>(1);
    }
    memset(memo_, 0, sizeof(memo_));
    for (size_t k = 0; k < old.size(); k++) {
        Tex& tx = textures_[old[k]];
        if (tx.tex) tx.tex->Release();
        textures_.erase(old[k]);
    }
    LOGI("gpu: %u texturas sem uso soltas, %u ficam", unsigned(old.size()), unsigned(textures_.size()));
}

// JSRF: troca do modo de video. Os alvos sao refeitos no proximo desenho; o conteudo deles nao precisa sobreviver a um quadro.
void XenosBackend::ApplyVideoMode(int mode) {
    if (mode == mode_now_) return;
    bool first = mode_now_ < 0;
    mode_now_ = mode;
    if (!first) {
        dev_->BlockUntilIdle();
        for (int i = 0; i < 4; i++) dev_->SetTexture(i, NULL);
        dev_->SetRenderTarget(0, host_bb_);
        dev_->SetDepthStencilSurface(NULL);
        for (std::map<std::pair<uint64_t, uint32_t>, Target*>::iterator it = targets_.begin(); it != targets_.end(); ++it) {
            Target* t = it->second;
            if (t->surf) t->surf->Release();
            if (t->tex) t->tex->Release();
            delete t;
        }
        targets_.clear();
        rt_ = NULL;
        ds_ = NULL;
        ForgetStates();
    }
    hd_targets_ = hidef_ && mode == 0;
    msaa_ = mode == 1;
    aniso_ = mode == 1;
    g_wide_on = wide_tv_ && mode == 0;
    LOGI("gpu: modo %d: quadros em %s, widescreen %s, 4x MSAA %s", mode, hd_targets_ ? "1280x720" : "640x480", g_wide_on ? "ligado" : "desligado",
         msaa_ ? "ligado" : "desligado");
}

void XenosBackend::Resolve(Target* t) {
    if (!t || t->depth || !t->edram_dirty || t != rt_) return;
    dev_->Resolve(D3DRESOLVE_RENDERTARGET0, NULL, t->tex, NULL, 0, 0, NULL, 0, 0, NULL);
    ForgetStates();
    t->edram_dirty = false;
    t->has_content = true;
}

void XenosBackend::ResolveBound() {
    if (rt_) Resolve(rt_);
    if (ds_) ds_->edram_dirty = false;
}

// guest pixels (the CPU drew them, or the title copied into the surface) into the target's texture
void XenosBackend::UploadFromGuest(Target* t, const Surface& s) {
    TextureDesc d;
    if (!t->tex || !DescribeSurface(s, d)) return;
    // locked and written back by Sync, but not changed by the CPU since: nothing to read
    uint64_t sig = SampleSig(static_cast<const uint8_t*>(kernel::HostPtr(d.data)), d.pitch * d.height);
    if (t->has_content && sig == t->cpu_sig) {
        t->cpu_epoch = DirtyEpoch(t->data);
        return;
    }
    std::vector<uint8_t> px;
    uint32_t pitch = 0;
    DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), NULL, px, pitch);
    if (px.empty()) return;
    uint32_t w = std::min(t->lw, d.width), h = std::min(t->lh, d.height);
    std::vector<uint32_t> lin(size_t(t->w) * t->h, 0);
    for (uint32_t y = 0; y < t->h; y++) {
        uint32_t sy = y * t->lh / t->h;
        if (sy >= h) break;
        const uint8_t* row = &px[size_t(sy) * pitch];
        uint32_t* dst = &lin[size_t(y) * t->w];
        for (uint32_t x = 0; x < t->w; x++) {
            uint32_t sx = x * t->lw / t->w;
            if (sx >= w) break;
            const uint8_t* src = row + sx * 4;
            dst[x] = uint32_t(src[3]) << 24 | uint32_t(src[2]) << 16 | uint32_t(src[1]) << 8 | src[0];
        }
    }
    WritePixels(t, lin);
    t->has_content = true;
    t->cpu_epoch = DirtyEpoch(t->data);
    t->cpu_sig = SampleSig(static_cast<const uint8_t*>(kernel::HostPtr(d.data)), d.pitch * d.height);
}

void XenosBackend::Blit(D3DTexture* tex, const RECT& dst, uint32_t tw, uint32_t th) {
    D3DSURFACE_DESC sd;
    (void)tw;
    (void)th;
    float W, H;
    D3DSurface* cur = NULL;
    dev_->GetRenderTarget(0, &cur);
    if (!cur) return;
    cur->GetDesc(&sd);
    cur->Release();
    W = float(sd.Width);
    H = float(sd.Height);
    float x0 = dst.left / W * 2 - 1, x1 = dst.right / W * 2 - 1, y0 = 1 - dst.top / H * 2, y1 = 1 - dst.bottom / H * 2;
    const float quad[4][4] = {{x0, y0, 0, 0}, {x1, y0, 1, 0}, {x0, y1, 0, 1}, {x1, y1, 1, 1}};
    Rs(D3DRS_HALFPIXELOFFSET, TRUE);
    Rs(D3DRS_ZENABLE, FALSE);
    Rs(D3DRS_STENCILENABLE, FALSE);
    Rs(D3DRS_ALPHABLENDENABLE, FALSE);
    Rs(D3DRS_CULLMODE, D3DCULL_NONE);
    Rs(D3DRS_SCISSORTESTENABLE, FALSE);
    Rs(D3DRS_COLORWRITEENABLE, 0xF);
    Rs(D3DRS_FILLMODE, D3DFILL_SOLID);
    D3DVIEWPORT9 vp = {0, 0, DWORD(W), DWORD(H), 0, 1};
    dev_->SetViewport(&vp);
    dev_->SetVertexDeclaration(blit_decl_);
    dev_->SetVertexShader(blit_vs_);
    if (blit_shown_ && present_ps_) {
        const float g[4] = {100.0f / float(kGammas[g_gamma_i]), 1.0f / float(tw ? tw : 640), 1.0f / float(th ? th : 480), 0};
        dev_->SetPixelShaderConstantF(0, g, 1);
        dev_->SetPixelShader(aniso_ && present_sharp_ps_ ? present_sharp_ps_ : present_ps_);
        ps_known_ = false;
    } else {
        dev_->SetPixelShader(blit_ps_);
    }
    dev_->SetTexture(0, tex);
    Ss(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    Ss(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    Ss(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    Ss(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    Ss(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
    Rs(D3DRS_HALFPIXELOFFSET, FALSE);
    cur_decl_ = NULL;
    cur_vs_ = NULL;
    cur_ps_ = NULL;
    last_vs_ = NULL;
    vs_fixed_last_ = true;  // the blit shader's constants are not the title's
}

// the draw's target pair in EDRAM; false when there is nothing to draw to
bool XenosBackend::Bind(const State& s) {
    uint32_t w = 0, h = 0;
    if (s.rt.header) SurfaceSize(s.rt, w, h);
    else if (s.zs.header) SurfaceSize(s.zs, w, h);
    if (!w || !h) return false;
    w = std::min<uint32_t>(w, 1280);
    h = std::min<uint32_t>(h, 720);
    // a depth only pass (no colour target) is not drawn yet: EDRAM wants a colour surface bound
    if (!s.rt.header) {
        LOG_FIRST(4, "gpu: a draw without a colour target is skipped");
        return false;
    }
    Target* rt = EnsureTarget(s.rt, false, w, h);
    Target* ds = s.zs.header ? EnsureTarget(s.zs, true, w, h) : NULL;
    // pixels the CPU wrote since: the target reads them first
    if (rt && rt->tex) {
        uint64_t e = DirtyEpoch(rt->data);
        if (e > rt->cpu_epoch) {
            if (rt == rt_ && rt->edram_dirty) Resolve(rt);
            UploadFromGuest(rt, s.rt);
            if (rt == rt_) rt_ = NULL;  // EDRAM is older than the texture now
        }
    }
    if (rt == rt_ && ds == ds_ && (rt || ds)) return true;
    ResolveBound();
    ForgetStates();
    dev_->SetRenderTarget(0, rt ? rt->surf : NULL);
    dev_->SetDepthStencilSurface(ds ? ds->surf : NULL);
    rt_ = rt;
    ds_ = ds;
    // EDRAM holds another target's pixels: put this one's back
    if (rt && rt->has_content) {
        // JSRF: o tamanho real do alvo. w e h aqui sao os que o jogo pediu: em 720p a imagem voltava encolhida no canto da tela
        // a cada troca de alvo no meio do quadro, como no calor do boost (o glitch rapido da v22 no console).
        RECT r = {0, 0, LONG(rt->w), LONG(rt->h)};
        Blit(rt->tex, r, rt->w, rt->h);
    }
    return true;
}

void XenosBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                         uint32_t stencil) {
    Own();
    if (!Bind(s)) return;
    DWORD f = 0;
    if ((flags & kClearTarget) && rt_) f |= D3DCLEAR_TARGET;
    if ((flags & kClearZ) && ds_) f |= D3DCLEAR_ZBUFFER;
    if ((flags & kClearStencil) && ds_) f |= D3DCLEAR_STENCIL;
    if (!f) return;
    std::vector<D3DRECT> rs;
    for (uint32_t i = 0; rects && i < count; i++) {
        D3DRECT r;
        r.x1 = LONG(__loadwordbytereverse(i * 16 + 0, rects));
        r.y1 = LONG(__loadwordbytereverse(i * 16 + 4, rects));
        r.x2 = LONG(__loadwordbytereverse(i * 16 + 8, rects));
        r.y2 = LONG(__loadwordbytereverse(i * 16 + 12, rects));
        {
            const Target* cl = rt_ ? rt_ : ds_;
            r.x1 = r.x1 * LONG(cl->w) / LONG(cl->lw);
            r.x2 = r.x2 * LONG(cl->w) / LONG(cl->lw);
            r.y1 = r.y1 * LONG(cl->h) / LONG(cl->lh);
            r.y2 = r.y2 * LONG(cl->h) / LONG(cl->lh);
        }
        rs.push_back(r);
    }
    Rs(D3DRS_SCISSORTESTENABLE, FALSE);
    Rs(D3DRS_COLORWRITEENABLE, 0xF);
    Rs(D3DRS_ZWRITEENABLE, TRUE);
    Rs(D3DRS_HIZWRITEENABLE, hiz_on_ && ds_ && ds_->w == 1280 && ds_->h == 720 ? D3DHIZ_AUTOMATIC : D3DHIZ_DISABLE);
    Rs(D3DRS_STENCILWRITEMASK, 0xFF);
    dev_->Clear(DWORD(rs.size()), rs.empty() ? NULL : &rs[0], f, color, z, stencil);
    // the console's Clear draws with shaders, a declaration and constants of its own
    ForgetStates();
    if (rt_ && (f & D3DCLEAR_TARGET)) rt_->edram_dirty = true;
    if (ds_) ds_->edram_dirty = true;
}

D3DVertexDeclaration* XenosBackend::DeclFor(uint16_t regs) {
    std::unordered_map<uint32_t, D3DVertexDeclaration*>::iterator it = decls_.find(regs);
    if (it != decls_.end()) return it->second;
    D3DVERTEXELEMENT9 el[17];
    int n = 0;
    for (int r = 0; r < 16; r++) {
        if (!((regs >> r) & 1)) continue;
        D3DVERTEXELEMENT9 e = {0, WORD(n * 16), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, BYTE(r)};
        el[n++] = e;
    }
    D3DVERTEXELEMENT9 end = D3DDECL_END();
    el[n] = end;
    D3DVertexDeclaration* d = NULL;
    dev_->CreateVertexDeclaration(el, &d);
    decls_[regs] = d;
    return d;
}

D3DVertexShader* XenosBackend::VertexShaderFor(const State& s, const Assembled& a, bool& fixed) {
    bool program = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    fixed = !program;
    if (program && s.vs.get() == vs_memo_prog_ && a.regs == vs_memo_regs_) return vs_memo_;
    uint64_t key;
    FixedKey fk;
    if (program) {
        if (!s.vs->hash) s.vs->hash = Hash(&s.vs->program[0], s.vs->program.size() * 4);
        key = Hash(&a.regs, sizeof(a.regs), s.vs->hash) ^ 0x1;
    } else {
        fk = MakeFixedKey(s);
        uint32_t extra[2] = {a.regs, a.pretransformed ? 1u : 0u};
        key = Hash(extra, sizeof(extra), Hash(&fk, sizeof(fk))) & ~1ull;
    }
    std::unordered_map<uint64_t, D3DVertexShader*>::iterator it = vs_.find(key);
    if (it != vs_.end()) {
        if (program && it->second) {
            vs_memo_prog_ = s.vs.get();
            vs_memo_keep_ = s.vs;
            vs_memo_regs_ = a.regs;
            vs_memo_ = it->second;
        }
        return it->second;
    }
    std::string src = program ? VertexProgramHlsl(s.vs->program, a.regs, 0) : FixedVertexHlsl(a.regs, a.pretransformed, 0, fk);
    // bring-up: the title's first two vertex programs as HLSL, with the constants they read
    static int dumped = 0;
    if (program && s.vs_handle != 0x11 && dumped < 2) {
        dumped++;
        LOGI("gpu: vertex program %08X as HLSL:", s.vs_handle);
        size_t p = src.find("VOut main"), e;
        for (int line = 0; p != std::string::npos && p < src.size() && line < 120; line++, p = e + 1) {
            e = src.find('\n', p);
            if (e == std::string::npos) e = src.size();
            LOGI("  | %s", src.substr(p, e - p).c_str());
        }
        for (int r = 0; r < 192; r++)
            if (s.vsc[r][0] || s.vsc[r][1] || s.vsc[r][2] || s.vsc[r][3])
                LOGI("  c[%d] = %g %g %g %g", r, s.vsc[r][0], s.vsc[r][1], s.vsc[r][2], s.vsc[r][3]);
    }
    D3DVertexShader* vs = NULL;
    ID3DXBuffer* code = NULL;
    if (Compile(src, "vs_3_0", &code)) {
        dev_->CreateVertexShader(static_cast<const DWORD*>(code->GetBufferPointer()), &vs);
        code->Release();
    }
    vs_[key] = vs;
    LOG_FIRST(256, "gpu: vertex shader %016llX (%s, regs %04X)%s", (unsigned long long)key, program ? "program" : "fixed",
              a.regs, vs ? "" : " FAILED");
    return vs;
}

D3DPixelShader* XenosBackend::PixelShaderFor(const PixelKey& k) {
    // a slot from the key's words without the 64 bit multiplies of Hash (tens of cycles each on this CPU)
    const uint32_t* w = reinterpret_cast<const uint32_t*>(&k);
    uint32_t x = 0;
    for (size_t i = 0; i < sizeof(k) / 4; i++) x = _rotl(x, 5) ^ w[i];
    PsMemo& m = ps_memo_[(x ^ (x >> 6) ^ (x >> 12) ^ (x >> 18) ^ (x >> 24)) & 63];
    if (m.ps && !memcmp(&m.key, &k, sizeof(k))) return m.ps;
    uint64_t key = Hash(&k, sizeof(k));
    std::unordered_map<uint64_t, D3DPixelShader*>::iterator it = ps_.find(key);
    if (it != ps_.end()) {
        if (it->second) {
            m.ps = it->second;
            m.key = k;
        }
        return it->second;
    }
    D3DPixelShader* ps = NULL;
    ID3DXBuffer* code = NULL;
    if (Compile(PixelHlsl(k), "ps_3_0", &code)) {
        dev_->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &ps);
        code->Release();
    }
    ps_[key] = ps;
    LOG_FIRST(256, "gpu: pixel shader %016llX%s", (unsigned long long)key, ps ? "" : " FAILED");
    return ps;
}

TexKind KindOf(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

// the stage's texture: a target's resolved pixels, or the guest texture decoded and uploaded
// the stage's texture, from the frame's memo when its header, its palette and their words are the same
D3DBaseTexture* XenosBackend::BindTexture(const State& s, int stage, TexInfo& info, TextureDesc& d) {
    uint32_t header = s.textures[size_t(stage)];
    if (!header) {
        memset(&info, 0, sizeof(info));
        return NULL;
    }
    uint32_t w[4] = {kernel::R32(header), kernel::R32(header + 4), kernel::R32(header + 12), kernel::R32(header + 16)};
    uint32_t pal = s.palettes[size_t(stage)];
    uint32_t pal_data = pal ? kernel::R32(pal + 4) : 0;
    TexMemo& m = memo_[(header >> 3) & 1023];
    if (m.frame == frames_ + 1 && m.header == header && m.words[0] == w[0] && m.words[1] == w[1] && m.words[2] == w[2] &&
        m.words[3] == w[3] && m.palette == (pal ^ pal_data)) {
        info = m.info;
        d = m.desc;
        return m.tex;
    }
    D3DBaseTexture* t = BindTextureSlow(s, stage, info, d);
    if (t && memo_ok_) {
        m.header = header;
        memcpy(m.words, w, sizeof(w));
        m.palette = pal ^ pal_data;
        m.frame = frames_ + 1;
        m.tex = t;
        m.info = info;
        m.desc = d;
    }
    return t;
}

D3DBaseTexture* XenosBackend::BindTextureSlow(const State& s, int stage, TexInfo& info, TextureDesc& d) {
    memo_ok_ = false;
    memset(&info, 0, sizeof(info));
    uint32_t header = s.textures[size_t(stage)];
    if (!header || !Describe(header, d)) return NULL;
    const FormatInfo& fi = Format(d.format);
    info.kind = uint8_t(KindOf(d));
    info.rect = d.linear ? 1 : 0;
    if (fi.depth) info.shadow = (d.format == 0x2C || d.format == 0x30) ? 2 : 1;
    // drawn by the title: its target. A target goes by the surface's physical address, a texture's data is virtual
    // (v28: compared as they were, this never matched, and the beast effect that reads the screen around the joints got
    // guest memory no draw had reached: the black smoke). A draw reading the target it draws to gets it as it is now.
    if (!fi.depth && info.kind == kTex2D) {
        for (std::map<std::pair<uint64_t, uint32_t>, Target*>::iterator it = targets_.begin(); it != targets_.end(); ++it) {
            Target* t = it->second;
            if (t->depth || mem::VirtualFromPhysical(t->data) != d.data || !t->tex) continue;
            if (t == rt_ && t->edram_dirty) Resolve(t);
            if (t->has_content && DirtyEpoch(t->data) <= t->cpu_epoch) return t->tex;
        }
    }
    if (fi.host == kHostNone || info.kind == kTex3D) {
        LOG_FIRST(16, "gpu: texture format %02X (%s) %s is not supported", d.format, fi.name, info.kind == kTex3D ? "3D" : "");
        return NULL;
    }
    // P8: the stage's palette, 256 colours
    uint32_t pal = 0;
    uint32_t colours[256];
    uint64_t pal_hash = 0;
    if (d.format == 0x0B && s.palettes[size_t(stage)]) {
        pal = mem::VirtualFromPhysical(kernel::R32(s.palettes[size_t(stage)] + 4));
        for (int i = 0; i < 256; i++) colours[i] = kernel::R32(pal + i * 4);
        pal_hash = Hash(colours, sizeof(colours));
    }
    // the key from the fields only: TextureDesc ends in two bools and its padding bytes are whatever the stack held,
    // which made every draw a new texture (memory ran out within seconds on the console)
    uint32_t kf[10] = {d.data, d.format, d.width, d.height, d.depth, d.levels, d.pitch, d.cube ? 1u : 0u,
                       d.linear ? 1u : 0u, pal};
    uint64_t key = Hash(kf, sizeof(kf));
    Tex& tx = textures_[key];
    // checked once a frame: sampling a texture's memory costs cache misses, and a fight draws thousands of times
    if (tx.tex && tx.frame == frames_ + 1 && pal_hash == tx.pal_hash) {
        memo_ok_ = true;
        return tx.tex;
    }
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
    uint32_t total = TotalSize(d);
    uint64_t epoch = DirtyEpoch(d.data);
    uint64_t sig = SampleSig(guest, std::min<uint32_t>(total, 1u << 22));
    if (tx.tex && epoch == tx.epoch && sig == tx.sig && pal_hash == tx.pal_hash) {
        tx.frame = frames_ + 1;
        memo_ok_ = true;
        return tx.tex;
    }
    int64_t up0 = Ticks();
    g_uploads++;
    // written again: bound again, not left to the same pointer test in Draw
    for (int i = 0; i < 4; i++) cur_tex_[i] = reinterpret_cast<D3DBaseTexture*>(1);
    // made again when missing, re-uploaded otherwise
    D3DFORMAT fmt = fi.host == kHostBC1 ? kFmtLeDxt1 : fi.host == kHostBC2 ? kFmtLeDxt3 : fi.host == kHostBC3 ? kFmtLeDxt5
                    : fi.host == kHostR32F ? D3DFMT_LIN_R32F : kFmtLeBgra;
    uint32_t levels = d.linear ? 1 : d.levels;
    if (!tx.tex) {
        HRESULT hr;
        if (d.cube) {
            D3DCubeTexture* c = NULL;
            hr = dev_->CreateCubeTexture(d.width, levels, 0, fmt, D3DPOOL_DEFAULT, &c, NULL);
            tx.tex = c;
        } else {
            D3DTexture* t2 = NULL;
            hr = dev_->CreateTexture(d.width, d.height, levels, 0, fmt, D3DPOOL_DEFAULT, &t2, NULL);
            tx.tex = t2;
        }
        LOG_FIRST(48, "gpu: texture %08X %ux%u levels %u format %s%s%s", d.data, d.width, d.height, levels, fi.name,
                  d.cube ? " cube" : "", d.linear ? " linear" : "");
        if (textures_.size() == 2048) LOGW("gpu: 2048 textures cached, more than the title should have");
        if (FAILED(hr) || !tx.tex) {
            LOG_FIRST(16, "gpu: texture %08X %ux%u format %s could not be made", d.data, d.width, d.height, fi.name);
            textures_.erase(key);
            return NULL;
        }
    }
    std::vector<uint8_t> out;
    uint32_t faces = d.cube ? 6 : 1;
    for (uint32_t face = 0; face < faces; face++)
        for (uint32_t level = 0; level < levels; level++) {
            uint32_t row_pitch = 0;
            DecodeLevel(d, level, face, guest, pal ? colours : NULL, out, row_pitch);
            // bring-up: the first big decoded textures as uploaded, to tell decoding from sampling
            static int tex_dumped = 0;
            if (level == 0 && face == 0 && !fi.compressed && fi.host == kHostBGRA8 && d.width >= 128 && tex_dumped < 0 &&
                out.size() >= size_t(row_pitch) * d.height) {
                tex_dumped++;
                char name[64];
                _snprintf(name, sizeof(name), "tex_%08X_%ux%u_%s", d.data, d.width, d.height, fi.name);
                const uint8_t* o = &out[0];
                uint32_t rp = row_pitch;
                DumpBmp(name, d.width, d.height, [o, rp](uint32_t x, uint32_t y, uint8_t* b) { memcpy(b, o + y * rp + x * 4, 4); });
            }
            D3DLOCKED_RECT lr;
            HRESULT hr = d.cube ? static_cast<D3DCubeTexture*>(tx.tex)->LockRect(D3DCUBEMAP_FACES(face), level, &lr, NULL, 0)
                                : static_cast<D3DTexture*>(tx.tex)->LockRect(level, &lr, NULL, 0);
            if (FAILED(hr)) continue;
            uint32_t h = d.linear ? d.height : std::max(d.height >> level, 1u);
            uint32_t rows = fi.compressed ? std::max(1u, (h + 3) / 4) : h;
            uint32_t n = std::min<uint32_t>(row_pitch, uint32_t(lr.Pitch));
            for (uint32_t y = 0; y < rows && size_t(y) * row_pitch < out.size(); y++)
                memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, &out[size_t(y) * row_pitch], n);
            if (d.cube) static_cast<D3DCubeTexture*>(tx.tex)->UnlockRect(D3DCUBEMAP_FACES(face), level);
            else static_cast<D3DTexture*>(tx.tex)->UnlockRect(level);
        }
    tx.info = info;
    tx.epoch = epoch;
    tx.sig = sig;
    tx.pal = pal;
    tx.pal_hash = pal_hash;
    tx.frame = frames_ + 1;
    g_prof[kPfUpload] += Ticks() - up0;
    memo_ok_ = true;
    return tx.tex;
}

void XenosBackend::SetSamplers(const State& s, int i, bool rect) {
    const std::array<uint32_t, kTssCount>& t = s.tss[size_t(i)];
    Ss(i, D3DSAMP_ADDRESSU, Address(t[kTssAddressU]));
    Ss(i, D3DSAMP_ADDRESSV, Address(t[kTssAddressV]));
    Ss(i, D3DSAMP_ADDRESSW, Address(t[kTssAddressW]));
    Ss(i, D3DSAMP_MAGFILTER, Filter(t[kTssMagFilter] ? t[kTssMagFilter] : 1));
    // modo original: anisotropico onde o jogo filtra de forma linear com mipmaps
    Ss(i, D3DSAMP_MINFILTER, aniso_ && !rect && t[kTssMinFilter] == 2 && t[kTssMipFilter] >= 2 ? D3DTEXF_ANISOTROPIC : Filter(t[kTssMinFilter] ? t[kTssMinFilter] : 1));
    Ss(i, D3DSAMP_MIPFILTER, rect ? D3DTEXF_NONE : Filter(t[kTssMipFilter]));
    Ss(i, D3DSAMP_MAXANISOTROPY, std::max<uint32_t>(1, std::min<uint32_t>(t[kTssMaxAnisotropy], 16)));
    Ss(i, D3DSAMP_BORDERCOLOR, t[kTssBorderColor]);
    Ss(i, D3DSAMP_MIPMAPLODBIAS, t[kTssMipLodBias]);
    Ss(i, D3DSAMP_MAXMIPLEVEL, t[kTssMaxMipLevel]);
}

void XenosBackend::SetStates(const State& s, const Assembled& a) {
    // blend
    bool blend = SimpleState(s, kMBlendEnable) != 0;
    Rs(D3DRS_ALPHABLENDENABLE, blend);
    if (blend) {
        uint32_t src = SimpleState(s, kMSrcBlend), dst = SimpleState(s, kMDestBlend), op = SimpleState(s, kMBlendOp);
        Rs(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        Rs(D3DRS_SRCBLEND, Blend(src, false));
        Rs(D3DRS_DESTBLEND, Blend(dst, false));
        Rs(D3DRS_BLENDOP, BlendOp(op));
        Rs(D3DRS_SRCBLENDALPHA, Blend(src, true));
        Rs(D3DRS_DESTBLENDALPHA, Blend(dst, true));
        Rs(D3DRS_BLENDOPALPHA, BlendOp(op));
        float bf[4];
        Color(SimpleState(s, kMBlendColor), bf);
        if (src == 0x8003 || dst == 0x8003 || src == 0x8004 || dst == 0x8004 || src == 14 || dst == 14 || src == 0x8001 ||
            dst == 0x8001 || (bf[0] == 0.0f && bf[1] == 0.0f && bf[2] == 0.0f && bf[3] > 0.0f))
            bf[0] = bf[1] = bf[2] = bf[3];
        D3DCOLOR c = D3DCOLOR_COLORVALUE(bf[0], bf[1], bf[2], bf[3]);
        Rs(D3DRS_BLENDFACTOR, c);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    uint32_t wmask = cw <= 15 ? cw : (((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3);
    Rs(D3DRS_COLORWRITEENABLE, rt_ ? wmask : 0);
    // depth and stencil
    bool zen = ds_ && (SimpleState(s, kMDepthTestEnable) != 0 || s.rs[kRsZEnable] != 0);
    Rs(D3DRS_ZENABLE, zen ? D3DZB_TRUE : D3DZB_FALSE);
    // JSRF: Hi-Z no alvo de profundidade principal, que e o dono da memoria de Hi-Z. O texto de EnsureTarget explica a base 0.
    const bool hiz = hiz_on_ && zen && ds_ && ds_->w == 1280 && ds_->h == 720;
    Rs(D3DRS_HIZENABLE, hiz ? D3DHIZ_AUTOMATIC : D3DHIZ_DISABLE);
    Rs(D3DRS_HIZWRITEENABLE, hiz ? D3DHIZ_AUTOMATIC : D3DHIZ_DISABLE);
    if (zen) {
        uint32_t zf = SimpleState(s, kMZFunc);
        Rs(D3DRS_ZWRITEENABLE, SimpleState(s, kMZWriteEnable) != 0);
        Rs(D3DRS_ZFUNC, CmpState(zf ? zf : 4));
    }
    bool sten = ds_ && (SimpleState(s, kMStencilTestEnable) != 0 || s.rs[kRsStencilEnable] != 0);
    Rs(D3DRS_STENCILENABLE, sten);
    if (sten) {
        Rs(D3DRS_STENCILFUNC, CmpState(SimpleState(s, kMStencilFunc)));
        Rs(D3DRS_STENCILREF, SimpleState(s, kMStencilRef) & 0xFF);
        Rs(D3DRS_STENCILMASK, SimpleState(s, kMStencilMask) & 0xFF);
        Rs(D3DRS_STENCILWRITEMASK, SimpleState(s, kMStencilWriteMask) & 0xFF);
        Rs(D3DRS_STENCILFAIL, StencilOp(s.rs[kRsStencilFail]));
        Rs(D3DRS_STENCILZFAIL, StencilOp(SimpleState(s, kMStencilZFail)));
        Rs(D3DRS_STENCILPASS, StencilOp(SimpleState(s, kMStencilPass)));
    }
    // polygon offset: bias in 24 bit depth buffer units, slope per pixel
    if (a.topology == kTopTriangles && SimpleState(s, kMSolidOffsetEnable)) {
        Rs(D3DRS_DEPTHBIAS, FloatRs(Bits(SimpleState(s, kMDepthOffset)) / s.zscale));
        Rs(D3DRS_SLOPESCALEDEPTHBIAS, FloatRs(Bits(SimpleState(s, kMSlopeScale))));
    } else {
        Rs(D3DRS_DEPTHBIAS, 0);
        Rs(D3DRS_SLOPESCALEDEPTHBIAS, 0);
    }
    // culling: the console's mode names the culled winding
    uint32_t cull = s.rs[kRsCullMode];
    bool cull_en = (cull == 0x900 || cull == 0x901 || cull == 2 || cull == 3) && a.topology == kTopTriangles;
    bool cull_cw = cull == 0x900 || cull == 2;
    Rs(D3DRS_CULLMODE, !cull_en ? D3DCULL_NONE : cull_cw ? D3DCULL_CW : D3DCULL_CCW);
    uint32_t fill = s.rs[kRsFillMode];
    Rs(D3DRS_FILLMODE, fill == 0x1B00 || fill == 1 ? D3DFILL_POINT : fill == 0x1B01 || fill == 2 ? D3DFILL_WIREFRAME
                                                                                                                : D3DFILL_SOLID);
    Rs(D3DRS_POINTSIZE, FloatRs(1.0f));
    // the whole target is the viewport, the title's viewport is in the shaders; the scissor clips to it
    uint32_t W = rt_ ? rt_->w : ds_->w, H = rt_ ? rt_->h : ds_->h;
    const uint32_t LW = rt_ ? rt_->lw : ds_->lw, LH = rt_ ? rt_->lh : ds_->lh;
    if (W != cur_vp_w_ || H != cur_vp_h_) {
        D3DVIEWPORT9 vp = {0, 0, W, H, 0, 1};
        dev_->SetViewport(&vp);
        cur_vp_w_ = W;
        cur_vp_h_ = H;
        cur_scissor_.right = -1;
    }
    LONG x0 = LONG(s.viewport.x), y0 = LONG(s.viewport.y), x1 = LONG(s.viewport.x + s.viewport.w),
         y1 = LONG(s.viewport.y + s.viewport.h);
    if (s.scissor_count) {
        x0 = std::max(x0, LONG(s.scissors[0]));
        y0 = std::max(y0, LONG(s.scissors[1]));
        x1 = std::min(x1, LONG(s.scissors[2]));
        y1 = std::min(y1, LONG(s.scissors[3]));
    }
    // JSRF, widescreen: uma janela menor que a tela (selecao de personagem) acompanha a moldura espremida; Unleashed, nv2a_gl.c
    if (g_wide_on && g_wide_scene && LW == 640 && LH == 480 && x1 - x0 < 600) {
        x0 = 320 + (x0 - 320) * 3 / 4;
        x1 = 320 + (x1 - 320) * 3 / 4;
    }
    x0 = x0 * LONG(W) / LONG(LW);
    x1 = x1 * LONG(W) / LONG(LW);
    y0 = y0 * LONG(H) / LONG(LH);
    y1 = y1 * LONG(H) / LONG(LH);
    RECT r = {std::max<LONG>(0, std::min<LONG>(x0, W)), std::max<LONG>(0, std::min<LONG>(y0, H)),
              std::max<LONG>(0, std::min<LONG>(x1, W)), std::max<LONG>(0, std::min<LONG>(y1, H))};
    if (r.left != cur_scissor_.left || r.top != cur_scissor_.top || r.right != cur_scissor_.right || r.bottom != cur_scissor_.bottom) {
        dev_->SetScissorRect(&r);
        cur_scissor_ = r;
    }
    Rs(D3DRS_SCISSORTESTENABLE, TRUE);
    Rs(D3DRS_ALPHATESTENABLE, FALSE);  // in the pixel shader
    Rs(D3DRS_HALFPIXELOFFSET, FALSE);
}

void XenosBackend::SetConstants(const State& s, bool fixed, const TextureDesc* desc, const bool* bound) {
    // o tamanho que o jogo conhece: os programas dividem por ele e dao a posicao na tela, qualquer que seja o alvo real
    uint32_t W = rt_ ? rt_->lw : ds_->lw, H = rt_ ? rt_->lh : ds_->lh;
    if (W != surf_w_ || H != surf_h_ || s.zscale != surf_z_) {
        surf_w_ = W;
        surf_h_ = H;
        surf_z_ = s.zscale;
        surf_f_[0] = float(W);
        surf_f_[1] = float(H);
        surf_f_[2] = 1.0f / s.zscale;
    }
    const float* surf = surf_f_;
    float fog[4] = {0, 0, 0, 0};
    if (s.rs[kRsFogEnable]) {
        if (!fixed && s.rs[kRsFogTableMode] == 0) {
            fog[0] = 1;
            fog[3] = 3;
        } else {
            fog[0] = Bits(s.rs[kRsFogStart]);
            fog[1] = Bits(s.rs[kRsFogEnd]);
            fog[2] = Bits(s.rs[kRsFogDensity]);
            fog[3] = float(s.rs[kRsFogTableMode]);
        }
    }
    if (fixed) {
        // the constants in the device made from the same state but for some groups: those groups' rows again
        int64_t tf = CpTicks();
        if (s.gens_valid && ff_loaded_ && W == ff_w_ && H == ff_h_ && FfPart(s, W, H, surf, fog)) {
            g_cprof[kCpFfPart] += CpTicks() - tf;
            g_ccount[kCpFfPart]++;
            vs_fixed_last_ = true;
            vs_known_ = false;
            return SetPixelConstants(s, desc, bound, surf);
        }
        g_ccount[kCpFfWhole]++;
        // what the fixed function constants are made of: the same again, with them still in the device, is nothing
        // to do (the matrix products, the inverse and the lights cost more than the draw itself)
        FfInputs in;
        memset(&in, 0, sizeof(in));
        memcpy(in.surf, surf, 16);
        memcpy(in.fog, fog, 16);
        memcpy(in.world, s.transforms[kTsWorld].data(), 64);
        memcpy(in.view, s.transforms[kTsView].data(), 64);
        memcpy(in.proj, s.transforms[kTsProjection].data(), 64);
        for (int t = 0; t < 4; t++) memcpy(in.tex[t], s.transforms[kTsTexture0 + size_t(t)].data(), 64);
        memcpy(in.material, s.material.data(), sizeof(in.material));
        in.ambient = s.rs[kRsAmbient];
        in.light_enable = s.light_enable;
        for (int i = 0; i < 8; i++)
            if ((s.light_enable >> i) & 1) memcpy(in.lights[i], s.lights[size_t(i)].data(), sizeof(in.lights[i]));
        in.vp[0] = s.viewport.x;
        in.vp[1] = s.viewport.y;
        in.vp[2] = s.viewport.w;
        in.vp[3] = s.viewport.h;
        memcpy(&in.vp[4], &s.viewport.min_z, 4);
        memcpy(&in.vp[5], &s.viewport.max_z, 4);
        in.wh[0] = W;
        in.wh[1] = H;
        memcpy(ff_g_, s.gen, sizeof(ff_g_));
        ff_w_ = W;
        ff_h_ = H;
        if (ff_loaded_ && memcmp(&in, &ff_last_, sizeof(in)) == 0) {
            vs_fixed_last_ = true;
            vs_known_ = false;
            g_cprof[kCpFfWhole] += CpTicks() - tf;
            return SetPixelConstants(s, desc, bound, surf);
        }
        ff_last_ = in;
        ff_loaded_ = true;
        float c[kVfCount * 4];
        memset(c, 0, sizeof(c));
        memcpy(&c[kVfSurf * 4], surf, 16);
        memcpy(&c[kVfFog * 4], fog, 16);
        // world, view, projection, then the viewport inside the target
        float wv[16], wvp[16], full[16];
        MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
        MatMul(wv, s.transforms[kTsProjection].data(), wvp);
        const Viewport& v = s.viewport;
        float sx = float(v.w) / W, sy = float(v.h) / H;
        float ox = (2.0f * v.x + v.w) / W - 1, oy = 1 - (2.0f * v.y + v.h) / H;
        float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
        MatMul(wvp, vpm, full);
        float vp[16];
        MatMul(s.transforms[kTsView].data(), s.transforms[kTsProjection].data(), vp);
        MatMul(vp, vpm, ff_vpf_);
        Transpose(full, &c[kVfWvp * 4]);
        Transpose(wv, &c[kVfWv * 4]);
        float n[16];
        memset(n, 0, sizeof(n));
        InverseTranspose3x3(wv, n);
        Transpose(n, &c[kVfWvn * 4]);
        for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), &c[(kVfTm + t * 4) * 4]);
        memcpy(&c[kVfMat * 4], s.material.data(), 16 * sizeof(float));
        c[(kVfMat + 4) * 4] = s.material[16];
        Color(s.rs[kRsAmbient], &c[kVfAmb * 4]);
        const float* vm = s.transforms[kTsView].data();
        for (int i = 0; i < 8; i++)
            if ((s.light_enable >> i) & 1) LightToView(s.lights[size_t(i)], vm, &c[(kVfLt + i * 7) * 4]);
        dev_->SetVertexShaderConstantF(0, c, kVfCount);
        g_cprof[kCpFfWhole] += CpTicks() - tf;
        vs_fixed_last_ = true;
        vs_known_ = false;
        tail_known_ = false;
    } else {
        int64_t tp = CpTicks();
        g_ccount[kCpProgram]++;
        if (vs_fixed_last_ || vsc_epoch_ != s.vsc_epoch) {
            // the rows that differ from what the device holds, as one range: of those sent changes reached since the
            // last time (render_thread.cpp), or of all 192
            const float* now = &s.vsc[0][0];
            int first = 0, last = 191;
            if (vs_known_ && !vs_fixed_last_) {
                if (s.gens_valid) {
                    first = int(s.vsc_lo);
                    last = int(std::min<uint32_t>(s.vsc_hi, 192)) - 1;
                }
                while (first <= last && memcmp(now + first * 4, vs_last_ + first * 4, 16) == 0) first++;
                while (last >= first && memcmp(now + last * 4, vs_last_ + last * 4, 16) == 0) last--;
            }
            s.vsc_lo = 192;
            s.vsc_hi = 0;
            if (first <= last) {
                ff_loaded_ = false;
                dev_->SetVertexShaderConstantF(first, now + first * 4, last - first + 1);
                memcpy(vs_last_ + first * 4, now + first * 4, size_t(last - first + 1) * 16);
            }
            vs_known_ = true;
            vsc_epoch_ = s.vsc_epoch;
            vs_fixed_last_ = false;
        }
        float tail[8];
        memcpy(tail, surf, 16);
        memcpy(tail + 4, fog, 16);
        if (!tail_known_ || memcmp(tail, tail_last_, sizeof(tail)) != 0) {
            dev_->SetVertexShaderConstantF(kVsSurf, tail, 2);
            memcpy(tail_last_, tail, sizeof(tail));
            tail_known_ = true;
        }
        g_cprof[kCpProgram] += CpTicks() - tp;
    }
    SetPixelConstants(s, desc, bound, surf);
}

// the groups of fixed function constants whose inputs moved since they were made (State::gen), the rest kept in the
// device. False when that is not this draw's case (v23 A: anything but the world matrix moved)
bool XenosBackend::FfPart(const State& s, uint32_t W, uint32_t H, const float* surf, const float* fog) {
    const uint32_t* g = s.gen;
    if (!kFfGroups)
        for (int i = State::kGenView; i <= State::kGenFog; i++)
            if (g[i] != ff_g_[i]) return false;
    const bool view = g[State::kGenView] != ff_g_[State::kGenView];
    const bool vp = view || g[State::kGenProj] != ff_g_[State::kGenProj];
    if (g[State::kGenFog] != ff_g_[State::kGenFog]) {
        float r[8];
        memcpy(r, surf, 16);
        memcpy(r + 4, fog, 16);
        dev_->SetVertexShaderConstantF(kVfSurf, r, 2);
    }
    if (vp) {
        const Viewport& v = s.viewport;
        float sx = float(v.w) / W, sy = float(v.h) / H;
        float ox = (2.0f * v.x + v.w) / W - 1, oy = 1 - (2.0f * v.y + v.h) / H;
        float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
        float m[16];
        MatMul(s.transforms[kTsView].data(), s.transforms[kTsProjection].data(), m);
        MatMul(m, vpm, ff_vpf_);
    }
    if (vp || g[State::kGenWorld] != ff_g_[State::kGenWorld]) {
        // world * view, world * (view * projection * viewport) and the normal matrix
        const float* world = s.transforms[kTsWorld].data();
        float rows[12 * 4], wv[16], full[16], n[16];
        MatMul(world, s.transforms[kTsView].data(), wv);
        MatMul(world, ff_vpf_, full);
        memset(n, 0, sizeof(n));
        InverseTranspose3x3(wv, n);
        Transpose(full, &rows[(kVfWvp - kVfWvp) * 4]);
        Transpose(wv, &rows[(kVfWv - kVfWvp) * 4]);
        Transpose(n, &rows[(kVfWvn - kVfWvp) * 4]);
        dev_->SetVertexShaderConstantF(kVfWvp, rows, 12);
    }
    if (g[State::kGenTexMat] != ff_g_[State::kGenTexMat]) {
        float rows[16 * 4];
        for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), &rows[t * 16]);
        dev_->SetVertexShaderConstantF(kVfTm, rows, 16);
    }
    if (g[State::kGenMaterial] != ff_g_[State::kGenMaterial]) {
        float rows[6 * 4];
        memset(rows, 0, sizeof(rows));
        memcpy(rows, s.material.data(), 16 * sizeof(float));
        rows[4 * 4] = s.material[16];
        Color(s.rs[kRsAmbient], &rows[(kVfAmb - kVfMat) * 4]);
        dev_->SetVertexShaderConstantF(kVfMat, rows, 6);
    }
    if (view || g[State::kGenLights] != ff_g_[State::kGenLights]) {
        float rows[56 * 4];
        memset(rows, 0, sizeof(rows));
        const float* vm = s.transforms[kTsView].data();
        for (int i = 0; i < 8; i++)
            if ((s.light_enable >> i) & 1) LightToView(s.lights[size_t(i)], vm, &rows[i * 7 * 4]);
        dev_->SetVertexShaderConstantF(kVfLt, rows, 56);
    }
    memcpy(ff_g_, g, sizeof(ff_g_));
    // the whole set's inputs are not kept up to date here: SetConstants' compare of them must not match
    memset(&ff_last_, 0xFF, sizeof(ff_last_));
    return true;
}

void XenosBackend::SetPixelConstants(const State& s, const TextureDesc* desc, const bool* bound, const float* surf) {
    int64_t t0 = CpTicks();
    g_ccount[kCpPixel]++;
    // made from the same states, target and texture sizes as those in the device: nothing to do
    uint32_t key[11];
    memcpy(key, surf, 12);
    for (int i = 0; i < 4; i++) {
        key[3 + i * 2] = bound[i] ? desc[i].width : 0xFFFFFFFFu;
        key[4 + i * 2] = bound[i] ? desc[i].height : 0xFFFFFFFFu;
    }
    if (s.gens_valid && ps_known_ && s.gen[State::kGenPixel] == px_gen_ && !memcmp(key, px_key_, sizeof(key))) {
        g_cprof[kCpPixel] += CpTicks() - t0;
        return;
    }
    px_gen_ = s.gen[State::kGenPixel];
    memcpy(px_key_, key, sizeof(key));
    float p[kPsCount * 4];
    memset(p, 0, sizeof(p));
    memcpy(&p[kPsSurf * 4], surf, 16);
    Color(s.rs[kRsFogColor], &p[kPsFogColor * 4]);
    p[kPsMisc * 4 + 0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    p[kPsMisc * 4 + 1] = SimpleState(s, kMAlphaTestEnable) ? float(Cmp(SimpleState(s, kMAlphaFunc)) - 1) : 7.0f;
    p[kPsMisc * 4 + 2] = float(Cmp(s.rs[kRsShadowFunc]));
    p[kPsMisc * 4 + 3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), &p[(kPsPc + i * 2) * 4]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), &p[(kPsPc + i * 2 + 1) * 4]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), &p[(kPsPc + 16) * 4]);
    Color(SimpleState(s, kMPsFinalConstant1), &p[(kPsPc + 17) * 4]);
    Color(s.rs[kRsTextureFactor], &p[kPsTfactor * 4]);
    for (int i = 0; i < 4; i++) {
        if (bound[i]) {
            p[(kPsTexsize + i) * 4 + 0] = float(desc[i].width);
            p[(kPsTexsize + i) * 4 + 1] = float(desc[i].height);
        }
        const std::array<uint32_t, kTssCount>& t = s.tss[size_t(i)];
        p[(kPsBump + i) * 4 + 0] = Bits(t[kTssBumpEnvMat00]);
        p[(kPsBump + i) * 4 + 1] = Bits(t[kTssBumpEnvMat01]);
        p[(kPsBump + i) * 4 + 2] = Bits(t[kTssBumpEnvMat10]);
        p[(kPsBump + i) * 4 + 3] = Bits(t[kTssBumpEnvMat11]);
        p[(kPsBumpl + i) * 4 + 0] = Bits(t[kTssBumpEnvLScale]);
        p[(kPsBumpl + i) * 4 + 1] = Bits(t[kTssBumpEnvLOffset]);
    }
    if (!ps_known_ || memcmp(p, ps_last_, sizeof(p)) != 0) {
        dev_->SetPixelShaderConstantF(0, p, kPsCount);
        memcpy(ps_last_, p, sizeof(p));
    }
    ps_known_ = true;
    g_cprof[kCpPixel] += CpTicks() - t0;
}

// the converted vertices of a draw from the same guest bytes as before, in a vertex buffer of the GPU; made on the second
// draw that finds them unchanged. NULL: convert them for this draw
XenosBackend::VCache* XenosBackend::CachedVertices(Assembled& a) {
    if (!a.ranged || a.vertex_count < 3) {
        vc_none_++;
        return NULL;
    }
    // where the vertices come from: the key; the guest bytes' lock epochs and samples: whether they are still the same
    uint64_t key = 1469598103934665603ull;
    const uint64_t kPrime = 1099511628211ull;
    key = (key ^ (uint64_t(a.regs) << 32 | a.nregs)) * kPrime;
    key = (key ^ (uint64_t(a.first) << 32 | a.vertex_count)) * kPrime;
    uint64_t epoch = 0, sig = 0;
    uint32_t seen[16];
    int nseen = 0;
    for (uint32_t k = 0; k < a.nregs; k++) {
        if (a.src_base[k] >= kSystemPool) {
            vc_none_++;
            return NULL;
        }
        key = (key ^ (uint64_t(a.src_base[k]) << 32 | a.src_off[k])) * kPrime;
        key = (key ^ (uint64_t(a.src_stride[k]) << 32 | a.src_type[k])) * kPrime;
        bool again = false;
        for (int i = 0; i < nseen; i++) again |= seen[i] == a.src_base[k];
        if (again) continue;
        seen[nseen++] = a.src_base[k];
        epoch += DirtyEpoch(a.src_base[k]);
        uint32_t bytes = a.vertex_count * a.src_stride[k];
        sig = (sig ^ QuickSig(static_cast<const uint8_t*>(kernel::HostPtr(a.src_base[k] + a.first * a.src_stride[k])), bytes)) * kPrime;
    }
    VCache& c = vcache_[key];
    c.frame = frames_;
    if (c.vb && c.epoch == epoch && c.sig == sig) {
        vc_hit_++;
        return &c;
    }
    vc_miss_++;
    if (c.vb) {
        // made before and changed since, written by the title; DrawIndexedPrimitiveUP copied what it read
        free(c.vb);
        vcache_bytes_ -= c.bytes;
        c.vb = NULL;
        c.bytes = 0;
        c.misses++;
    }
    bool same = c.seen && c.epoch == epoch && c.sig == sig;
    c.seen = true;
    c.epoch = epoch;
    c.sig = sig;
    // the first sighting only remembers it; a buffer the title rewrote twice is not kept
    if (!same || c.misses >= 2) return NULL;
    uint32_t bytes = a.vertex_count * a.nregs * 16;
    if (vcache_bytes_ + bytes > kVertexCacheBytes) TrimVertexCache();
    if (vcache_bytes_ + bytes > kVertexCacheBytes) return NULL;
    c.vb = static_cast<float*>(malloc(bytes));
    if (!c.vb) return NULL;
    c.bytes = bytes;
    vcache_bytes_ += bytes;
    AssembleVertices(a);
    memcpy(c.vb, &a.vertices[0], bytes);
    return &c;
}

bool XenosBackend::HudSheet(const State& s) {
    uint32_t header = s.textures[0];
    TextureDesc d;
    if (!header || !Describe(header, d) || d.width != 256 || d.height != 256 || d.format != 0x0E) return false;
    uint64_t& h = hud_hash_[uint64_t(d.data) << 16 | 1];
    if (!h) {
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        uint64_t f = 1469598103934665603ull;
        for (uint32_t i = 0; i < 65536; i += 8) {
            uint64_t v;
            memcpy(&v, p + i, 8);
            f = (f ^ v) * 1099511628211ull;
        }
        h = f | 1;
        LOGI("gpu: folha 256x256 DXT3 em %08X, conteudo %016llX%s", d.data, (unsigned long long)h, h == kHudSheetHash ? ", a do HUD" : "");
    }
    return h == kHudSheetHash;
}

// JSRF, widescreen, como nv2a_gl.c do Unleashed: o quadro guarda a vista 16:9 espremida em 640x480 e a saida a estica. O 2D em
// pixels (HUD, texto, menus) e espremido por 3/4 em torno do centro para manter a forma; o que ocupa a tela toda (fundo, fade)
// fica como esta. Pecas da folha do HUD vao para a borda da tela. Devolve o deslocamento em NDC deste desenho.
float XenosBackend::WideOverlay(const State& s, Assembled& a, const float* verts, float& xscale) {
    const Target* t = rt_ ? rt_ : ds_;
    xscale = 1.0f;
    if (!g_wide_on || !g_wide_scene || !t || t->lw != 640 || t->lh != 480) return 0;
    if (!a.pretransformed) {
        // o calor do boost: quadrado de frente para a camera, medido para os 4:3 do jogo. Pela camera aberta, ele cobria so o
        // meio da tela, com as bordas a vista. Se os cantos caem no meio 4:3, abre ate a tela toda.
        if (!(s.vs_handle & 1) && a.vertex_count >= 3 && a.vertex_count <= 8 && (a.regs & 1)) {
            const Viewport& v = s.viewport;
            float wv[16], wvp[16], f[16];
            MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
            MatMul(wv, s.transforms[kTsProjection].data(), wvp);
            float sx = float(v.w) / t->lw, sy = float(v.h) / t->lh;
            float ox = (2.0f * v.x + v.w) / t->lw - 1, oy = 1 - (2.0f * v.y + v.h) / t->lh;
            float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
            MatMul(wvp, vpm, f);
            float lo = 1e30f, hi = -1e30f, ylo = 1e30f, yhi = -1e30f;
            bool ok = true;
            for (uint32_t k = 0; k < a.vertex_count && ok; k++) {
                const float* p = verts + k * a.nregs * 4;
                float W = p[0] * f[3] + p[1] * f[7] + p[2] * f[11] + f[15];
                if (!(W > 1e-6f)) {
                    ok = false;
                    break;
                }
                float X = (p[0] * f[0] + p[1] * f[4] + p[2] * f[8] + f[12]) / W;
                float Y = (p[0] * f[1] + p[1] * f[5] + p[2] * f[9] + f[13]) / W;
                lo = std::min(lo, X);
                hi = std::max(hi, X);
                ylo = std::min(ylo, Y);
                yhi = std::max(yhi, Y);
            }
            if (ok && fabs(lo + 0.75f) < 0.03f && fabs(hi - 0.75f) < 0.03f && fabs(ylo + 1.0f) < 0.05f && fabs(yhi - 1.0f) < 0.05f) {
                LOG_FIRST(6, "gpu: quadrado de tela cheia alargado, x %.2f a %.2f", lo, hi);
                xscale = 4.0f / 3.0f;
                return 0;
            }
        }
        // uma janela 3D dentro de moldura 2D acompanha a moldura espremida
        float shift = 0;
        const Viewport& v = s.viewport;
        LONG x0 = LONG(v.x), x1 = LONG(v.x + v.w);
        if (s.scissor_count) {
            x0 = std::max(x0, LONG(s.scissors[0]));
            x1 = std::min(x1, LONG(s.scissors[2]));
        }
        if (x1 - x0 < 600) shift = -0.25f * ((x0 + x1) * 0.5f - 320.0f) / 320.0f;
        // a lata e outras pecas 3D do HUD vem logo depois das planas da mesma folha, e vao com elas
        if (hud_side_ && draws_ == hud_next_ && a.vertex_count <= 32 && HudSheet(s)) {
            shift += float(hud_side_) * 0.25f;
            hud_next_ = draws_ + 1;
        }
        return shift;
    }
    if (a.vertices.empty() || !(a.regs & 1)) return 0;
    const uint32_t step = a.nregs * 4;
    float lo = 1e30f, hi = -1e30f, ylo = 1e30f, yhi = -1e30f;
    float* v = &a.vertices[0];
    for (uint32_t k = 0; k < a.vertex_count; k++) {
        float x = v[k * step], y = v[k * step + 1];
        if (x < lo) lo = x;
        if (x > hi) hi = x;
        if (y < ylo) ylo = y;
        if (y > yhi) yhi = y;
    }
    if (hi - lo >= 576.0f || yhi - ylo >= 432.0f) return 0;
    float move = 0;
    if (HudSheet(s)) {
        hud_side_ = lo >= 352.0f ? 1 : hi <= 288.0f ? -1 : 0;
        move = float(hud_side_) * 80.0f;
        hud_next_ = hud_side_ ? draws_ + 1 : 0xFFFFFFFFu;
    }
    for (uint32_t k = 0; k < a.vertex_count; k++) v[k * step] = 320.0f + (v[k * step] - 320.0f) * 0.75f + move;
    return 0;
}

uint64_t XenosBackend::GeometrySig(const GCache& g, uint64_t index_sig) {
    uint64_t sig = index_sig;
    for (uint32_t k = 0; k < g.nsrc; k++)
        sig = (sig ^ QuickSig(static_cast<const uint8_t*>(kernel::HostPtr(g.src[k])), g.src_bytes[k])) * 1099511628211ull;
    return sig;
}

void XenosBackend::DropGeometry(GCache& g) {
    if (g.verts) {
        free(g.verts);
        vcache_bytes_ -= g.bytes;
    }
    g.verts = NULL;
    g.idx = NULL;
    g.bytes = 0;
}

XenosBackend::GCache* XenosBackend::LookupGeometry(const State& s, uint32_t prim, uint32_t index_va, uint32_t count, const uint8_t* ip,
                                                   uint64_t& index_sig) {
    const uint64_t kPrime = 1099511628211ull;
    uint64_t key = 1469598103934665603ull;
    key = (key ^ (uint64_t(index_va) << 32 | count)) * kPrime;
    key = (key ^ (uint64_t(prim) << 32 | s.base_vertex)) * kPrime;
    key = (key ^ (uint64_t(s.vs_handle) << 32 | uint32_t(uintptr_t(s.vs.get())))) * kPrime;
    for (int i = 0; i < 16; i++) {
        uint32_t vb = s.streams[size_t(i)].vb;
        if (vb) key = (key ^ (uint64_t(kernel::R32(vb + 4)) << 32 | (uint32_t(i) << 16) | s.streams[size_t(i)].stride)) * kPrime;
    }
    index_sig = QuickSig(ip, count * 2);
    GCache& g = gcache_[key];
    g.frame = frames_;
    return &g;
}

void XenosBackend::StoreGeometry(GCache& g, const Assembled& a, uint64_t index_sig, uint32_t index_bytes) {
    // onde estao os vertices lidos; um desenho que le o anel do DrawUP muda a cada quadro
    g.nsrc = 0;
    for (uint32_t k = 0; k < a.nregs && !g.skip; k++) {
        if (a.src_base[k] >= kSystemPool) {
            g.skip = true;
            break;
        }
        uint32_t addr = a.src_base[k] + a.first * a.src_stride[k];
        bool again = false;
        for (uint32_t i = 0; i < g.nsrc; i++) again |= g.src[i] == addr;
        if (again) continue;
        if (g.nsrc == 4) {
            g.skip = true;
            break;
        }
        g.src[g.nsrc] = addr;
        g.src_bytes[g.nsrc] = a.span * a.src_stride[k];
        g.nsrc++;
    }
    if (g.skip) return;
    uint64_t sig = GeometrySig(g, index_sig);
    bool same = g.seen && g.sig == sig;
    g.seen = true;
    g.sig = sig;
    if (!same || g.misses >= 2) return;
    uint32_t vbytes = a.vertex_count * a.nregs * 16;
    uint32_t bytes = vbytes + index_bytes;
    if (vcache_bytes_ + bytes > kVertexCacheBytes) TrimVertexCache();
    if (vcache_bytes_ + bytes > kVertexCacheBytes) return;
    float* mem = static_cast<float*>(malloc(bytes));
    if (!mem) return;
    memcpy(mem, &a.vertices[0], vbytes);
    memcpy(reinterpret_cast<uint8_t*>(mem) + vbytes, &a.indices[0], index_bytes);
    g.verts = mem;
    g.idx = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(mem) + vbytes);
    g.bytes = bytes;
    g.icount = uint32_t(a.indices.size());
    g.vertex_count = a.vertex_count;
    g.nregs = a.nregs;
    g.regs = a.regs;
    g.topology = a.topology;
    g.pretransformed = a.pretransformed;
    vcache_bytes_ += bytes;
    gc_store_++;
}

// drops the cached vertices no frame of the last three drew, until the cache holds under three quarters of its room
void XenosBackend::TrimVertexCache() {
    for (std::unordered_map<uint64_t, GCache>::iterator it = gcache_.begin(); it != gcache_.end();) {
        if (vcache_bytes_ <= kVertexCacheBytes / 4 * 3) break;
        if (it->second.frame + 3 < frames_) {
            DropGeometry(it->second);
            it = gcache_.erase(it);
        } else {
            ++it;
        }
    }
    for (std::unordered_map<uint64_t, VCache>::iterator it = vcache_.begin(); it != vcache_.end();) {
        if (vcache_bytes_ <= kVertexCacheBytes / 4 * 3) break;
        if (it->second.frame + 3 < frames_) {
            if (it->second.vb) {
                vcache_bytes_ -= it->second.bytes;
                free(it->second.vb);
            }
            it = vcache_.erase(it);
        } else {
            ++it;
        }
    }
}

// the target as it is now, in game:\dump\capN_<when>.bmp
void XenosBackend::CaptureTarget(Target* t, const char* when) {
    if (!t) return;
    if (t == rt_ && t->edram_dirty) Resolve(t);
    if (!t->has_content) return;
    std::vector<uint32_t> pxs;
    if (!ReadPixels(t, pxs)) return;
    char name[64];
    _snprintf(name, sizeof(name), "cap%u_%s", cap_n_, when);
    const uint32_t* bits = &pxs[0];
    uint32_t w = t->w;
    DumpBmp(name, t->w, t->h, [bits, w](uint32_t x, uint32_t y, uint8_t* b) {
        uint32_t px = bits[size_t(y) * w + x];  // A R G B
        b[0] = uint8_t(px);
        b[1] = uint8_t(px >> 8);
        b[2] = uint8_t(px >> 16);
        b[3] = 255;
    });
}

// one draw of a capture: all it was drawn with, the shaders' HLSL the first time they are seen
void XenosBackend::CaptureDraw(const State& s, const Assembled& a, const TexInfo* info, const TextureDesc* desc, const bool* bound, bool fixed) {
    const uint32_t n = cap_draw_++;
    PixelKey pk = MakePixelKey(s, info);
    uint64_t pkey = Hash(&pk, sizeof(pk));
    uint64_t vkey = fixed || !s.vs ? 0 : s.vs->hash;
    fprintf(cap_, "D %u rt %08X %ux%u zs %08X top %d verts %u idx %u regs %04X vs %08X vk %016llX ps %016llX %s\n", n, s.rt.data,
            rt_ ? rt_->w : 0, rt_ ? rt_->h : 0, s.zs.data, int(a.topology), a.vertex_count, unsigned(a.indices.size()), a.regs,
            s.vs_handle, (unsigned long long)vkey, (unsigned long long)pkey, fixed ? "fixed" : "program");
    fprintf(cap_, "  blend %u src %X dst %X op %X color %08X cw %X | atest %u func %X ref %X | z %u/%u func %X write %u | sten %u func %X ref %X "
            "mask %X wmask %X fail %X zfail %X pass %X | cull %X fill %X | offset %u %08X %08X\n",
            SimpleState(s, kMBlendEnable), SimpleState(s, kMSrcBlend), SimpleState(s, kMDestBlend), SimpleState(s, kMBlendOp),
            SimpleState(s, kMBlendColor), SimpleState(s, kMColorWriteEnable), SimpleState(s, kMAlphaTestEnable), SimpleState(s, kMAlphaFunc),
            SimpleState(s, kMAlphaRef), SimpleState(s, kMDepthTestEnable), s.rs[kRsZEnable], SimpleState(s, kMZFunc), SimpleState(s, kMZWriteEnable),
            SimpleState(s, kMStencilTestEnable), SimpleState(s, kMStencilFunc), SimpleState(s, kMStencilRef), SimpleState(s, kMStencilMask),
            SimpleState(s, kMStencilWriteMask), s.rs[kRsStencilFail], SimpleState(s, kMStencilZFail), SimpleState(s, kMStencilPass), s.rs[kRsCullMode],
            s.rs[kRsFillMode], SimpleState(s, kMSolidOffsetEnable), SimpleState(s, kMDepthOffset), SimpleState(s, kMSlopeScale));
    fprintf(cap_, "  fog en %u mode %u start %g end %g dens %g color %08X | tfactor %08X ambient %08X spec %u | vp %u,%u %ux%u z %g..%g zscale %g\n",
            s.rs[kRsFogEnable], s.rs[kRsFogTableMode], Bits(s.rs[kRsFogStart]), Bits(s.rs[kRsFogEnd]), Bits(s.rs[kRsFogDensity]), s.rs[kRsFogColor],
            s.rs[kRsTextureFactor], s.rs[kRsAmbient], s.rs[kRsSpecularEnable], s.viewport.x, s.viewport.y, s.viewport.w, s.viewport.h,
            s.viewport.min_z, s.viewport.max_z, s.zscale);
    for (int i = 0; i < 4; i++)
        if (s.textures[size_t(i)]) {
            const std::array<uint32_t, kTssCount>& t = s.tss[size_t(i)];
            fprintf(cap_, "  t%d hdr %08X data %08X fmt %02X %ux%u bound %u rect %u shadow %u | addr %u %u filt %u %u %u akill %u\n", i, s.textures[size_t(i)],
                    bound[i] ? desc[i].data : 0, bound[i] ? desc[i].format : 0, bound[i] ? desc[i].width : 0, bound[i] ? desc[i].height : 0, bound[i] ? 1 : 0,
                    info[i].rect, info[i].shadow, t[kTssAddressU], t[kTssAddressV], t[kTssMagFilter], t[kTssMinFilter], t[kTssMipFilter], t[kTssAlphaKill]);
        }
    // the vertex program constants that moved since the draw before
    const float* now = &s.vsc[0][0];
    for (int r = 0; r < 192; r++)
        if (memcmp(now + r * 4, cap_vsc_ + r * 4, 16)) fprintf(cap_, "  c%d %g %g %g %g\n", r - 96, now[r * 4], now[r * 4 + 1], now[r * 4 + 2], now[r * 4 + 3]);
    memcpy(cap_vsc_, now, sizeof(cap_vsc_));
    // the combiners' constants
    if (s.ps) {
        fprintf(cap_, "  pc");
        for (int i = 0; i < 8; i++) fprintf(cap_, " %08X/%08X", SimpleState(s, kMPsConstant0 + i * 4), SimpleState(s, kMPsConstant1 + i * 4));
        fprintf(cap_, " final %08X %08X\n", SimpleState(s, kMPsFinalConstant0), SimpleState(s, kMPsFinalConstant1));
    }
    if (!a.vertices.empty()) {
        fprintf(cap_, "  v0");
        for (uint32_t k = 0; k < a.nregs * 4 && k < 40; k++) fprintf(cap_, " %g", a.vertices[k]);
        fprintf(cap_, "\n");
    }
    if (cap_seen_.insert(pkey).second) fprintf(cap_, "--- PS %016llX\n%s\n---\n", (unsigned long long)pkey, PixelHlsl(pk).c_str());
    if (!fixed && s.vs && cap_seen_.insert(vkey).second)
        fprintf(cap_, "--- VS %016llX\n%s\n---\n", (unsigned long long)vkey, VertexProgramHlsl(s.vs->program, a.regs, 0).c_str());
    if (rt_ && rt_->w >= 640 && n % kCaptureStep == 0) {
        char when[32];
        _snprintf(when, sizeof(when), "d%05u", n);
        fprintf(cap_, "  saved %s\n", when);
        CaptureTarget(rt_, when);
    }
}

void XenosBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    Own();
    static Assembled a;
    int64_t t0 = Ticks();
    // start, num desenho indexado: o endereco dos indices no jogo (d3d8.cpp), ou 0 se nao ha um que dure
    uint64_t index_sig = 0;
    const bool rhw_now = !(s.vs_handle & 1) && (s.vs_handle & 0xE) == 0x4;  // o 2D e ajustado a cada desenho, nao vai para o cache
    GCache* geo = kVertexCache && indices && start && draws_ >= 64 && !rhw_now ? LookupGeometry(s, prim, start, count, indices, index_sig) : NULL;
    const float* verts = NULL;
    const uint32_t* idx = NULL;
    uint32_t idx_count = 0;
    if (geo && geo->verts) {
        if (GeometrySig(*geo, index_sig) == geo->sig) {
            gc_hit_++;
            a.direct_prim = 0;
            a.direct_count = 0;
            a.regs = geo->regs;
            a.nregs = geo->nregs;
            a.vertex_count = geo->vertex_count;
            a.topology = geo->topology;
            a.pretransformed = geo->pretransformed;
            verts = geo->verts;
            idx = geo->idx;
            idx_count = geo->icount;
        } else {
            gc_miss_++;
            DropGeometry(*geo);
            geo->misses++;
        }
    }
    if (!verts) {
        if (!AssembleIndices(s, prim, start, count, indices, a)) {
            skip_assemble_++;
            return;
        }
        g_asm_indices += Ticks() - t0;
        VCache* cached = kVertexCache && !geo && !rhw_now && draws_ >= 64 ? CachedVertices(a) : NULL;
        if (!cached) AssembleVertices(a);
        verts = cached ? cached->vb : &a.vertices[0];
        idx = a.indices.empty() ? NULL : &a.indices[0];
        idx_count = uint32_t(a.indices.size());
        if (geo && !geo->skip && !a.direct_prim && idx_count) StoreGeometry(*geo, a, index_sig, idx_count * 4);
    }
    int64_t t1 = Ticks();
    g_prof[kPfAssemble] += t1 - t0;
    TexInfo info[4];
    TextureDesc desc[4];
    D3DBaseTexture* tex[4];
    bool bound[4];
    int64_t up_before = g_prof[kPfUpload];
    for (int i = 0; i < 4; i++) {
        tex[i] = BindTexture(s, i, info[i], desc[i]);
        bound[i] = tex[i] != NULL;
    }
    int64_t t2 = Ticks();
    g_prof[kPfTexture] += (t2 - t1) - (g_prof[kPfUpload] - up_before);
    if (!Bind(s)) {
        skip_bind_++;
        return;
    }
    {
        float xscale = 1.0f;
        float shift = WideOverlay(s, a, verts, xscale);
        if (shift != wide_shift_now_ || xscale != wide_scale_now_) {
            float c[4] = {shift, xscale, 0, 0};
            dev_->SetVertexShaderConstantF(kVsWide, c, 1);
            wide_shift_now_ = shift;
            wide_scale_now_ = xscale;
        }
    }
    int64_t t3 = Ticks();
    g_prof[kPfBind] += t3 - t2;
    bool fixed;
    D3DVertexShader* vs = VertexShaderFor(s, a, fixed);
    // the last pixel shader while nothing its key is made of moved (State::gen[kGenShader], shaders, stage kinds)
    uint32_t pk_info[4];
    memcpy(pk_info, info, sizeof(pk_info));
    D3DPixelShader* ps;
    if (s.gens_valid && pk_ps_ && s.gen[State::kGenShader] == pk_gen_ && s.ps.get() == pk_keep_ps_.get() && s.vs.get() == pk_keep_vs_.get() &&
        s.vs_handle == pk_handle_ && pk_info[0] == pk_info_[0] && pk_info[1] == pk_info_[1] && pk_info[2] == pk_info_[2] && pk_info[3] == pk_info_[3]) {
        ps = pk_ps_;
    } else {
        ps = PixelShaderFor(MakePixelKey(s, info));
        pk_ps_ = ps;
        pk_gen_ = s.gen[State::kGenShader];
        pk_keep_ps_ = s.ps;
        pk_keep_vs_ = s.vs;
        pk_handle_ = s.vs_handle;
        memcpy(pk_info_, pk_info, sizeof(pk_info_));
    }
    int64_t t4 = Ticks();
    g_prof[kPfShader] += t4 - t3;
    if (!vs || !ps) {
        skip_shader_++;
        return;
    }
    SetStates(s, a);
    int64_t t4b = Ticks();
    g_prof[kPfState] += t4b - t4;
    // bring-up: the states of the title's first draws, and where its first vertices land
    if (draws_ < 12 && !a.vertices.empty()) {
        const float* v = &a.vertices[0];
        LOGI("gpu: draw %u: vs %s %08X regs %04X, ps %s, cw %X blend %u %X/%X zen %u/%u zfunc %X zw %u cull %X fill %X "
             "alpha %u/%X ref %u, vp %u,%u %ux%u, rt %08X zs %08X, tex %08X %08X, v0 %.1f %.1f %.1f %.1f",
             draws_, fixed ? "fixed" : "program", s.vs_handle, a.regs, s.ps ? "combiners" : "fixed",
             SimpleState(s, kMColorWriteEnable), SimpleState(s, kMBlendEnable), SimpleState(s, kMSrcBlend),
             SimpleState(s, kMDestBlend), SimpleState(s, kMDepthTestEnable), s.rs[kRsZEnable], SimpleState(s, kMZFunc),
             SimpleState(s, kMZWriteEnable), s.rs[kRsCullMode], s.rs[kRsFillMode], SimpleState(s, kMAlphaTestEnable),
             SimpleState(s, kMAlphaFunc), SimpleState(s, kMAlphaRef), s.viewport.x, s.viewport.y, s.viewport.w,
             s.viewport.h, s.rt.data, s.zs.data, s.textures[0], s.textures[1], v[0], v[1], v[2], v[3]);
    }
    // bring-up: every input register of the first vertices of the title's first textured draws
    static int vdumped = 0;
    if (tex[0] && vdumped < 8 && !a.vertices.empty()) {
        vdumped++;
        LOGI("gpu: textured draw %u: tex0 %08X %ux%u %s rect %u, %u vertices, regs %04X, tss0 colorop %u arg1 %u arg2 %u, "
             "addr %u/%u, filter %u/%u",
             draws_, s.textures[0], desc[0].width, desc[0].height, Format(desc[0].format).name, info[0].rect, a.vertex_count,
             a.regs, s.tss[0][kTssColorOp], s.tss[0][kTssColorArg1], s.tss[0][kTssColorArg2], s.tss[0][kTssAddressU],
             s.tss[0][kTssAddressV], s.tss[0][kTssMinFilter], s.tss[0][kTssMagFilter]);
        for (uint32_t v = 0; v < std::min<uint32_t>(a.vertex_count, 4); v++) {
            std::string line;
            int k = 0;
            for (int r = 0; r < 16; r++) {
                if (!((a.regs >> r) & 1)) continue;
                const float* f = &a.vertices[(size_t(v) * a.nregs + k++) * 4];
                char b[96];
                _snprintf(b, sizeof(b), " v%d(%g %g %g %g)", r, f[0], f[1], f[2], f[3]);
                line += b;
            }
            LOGI("  vertex %u:%s", v, line.c_str());
        }
        if (vdumped <= 2) {
            std::string src = PixelHlsl(MakePixelKey(s, info));
            LOGI("  pixel shader as HLSL:");
            size_t p = src.find("main"), e;
            if (p != std::string::npos) p = src.rfind('\n', p) + 1;
            for (int line = 0; p != std::string::npos && p < src.size() && line < 60; line++, p = e + 1) {
                e = src.find('\n', p);
                if (e == std::string::npos) e = src.size();
                LOGI("  | %s", src.substr(p, e - p).c_str());
            }
        }
    }
    int64_t t4c = Ticks();
    for (int i = 0; i < 4; i++) {
        if (tex[i] != cur_tex_[i]) {
            dev_->SetTexture(i, tex[i]);
            cur_tex_[i] = tex[i];
        }
        if (tex[i]) SetSamplers(s, i, info[i].rect != 0);
    }
    int64_t t4d = Ticks();
    g_prof[kPfSampler] += t4d - t4c;
    SetConstants(s, fixed, desc, bound);
    // the same shaders and declaration again are not set again (ForgetStates after anything that binds its own)
    int64_t tb = CpTicks();
    D3DVertexDeclaration* decl = DeclFor(a.regs);
    if (decl != cur_decl_) dev_->SetVertexDeclaration(decl);
    if (vs != cur_vs_) dev_->SetVertexShader(vs);
    if (ps != cur_ps_) dev_->SetPixelShader(ps);
    cur_decl_ = decl;
    cur_vs_ = vs;
    cur_ps_ = ps;
    last_vs_ = vs;
    g_cprof[kCpBinds] += CpTicks() - tb;
    int64_t t5 = Ticks();
    g_prof[kPfConst] += t5 - t4d;
    if (a.direct_prim) {
        // through the indexed call with a fixed table (DrawPrimitiveUP here took Xenia down within seconds, unexplained)
        static uint16_t table[65536];
        static bool table_made = false;
        if (!table_made) {
            for (uint32_t i = 0; i < 65536; i++) table[i] = uint16_t(i);
            table_made = true;
        }
        dev_->DrawIndexedPrimitiveUP(a.direct_prim == 6 ? D3DPT_TRIANGLESTRIP : D3DPT_TRIANGLELIST, 0, a.vertex_count, a.direct_count, table,
                                     D3DFMT_INDEX16, verts, a.nregs * 16);
        g_prof[kPfDraw] += Ticks() - t5;
        if (rt_) rt_->edram_dirty = true;
        if (ds_) ds_->edram_dirty = true;
        draws_++;
        if (cap_) CaptureDraw(s, a, info, desc, bound, fixed);
        return;
    }
    D3DPRIMITIVETYPE pt = a.topology == kTopPoints ? D3DPT_POINTLIST : a.topology == kTopLines ? D3DPT_LINELIST : D3DPT_TRIANGLELIST;
    uint32_t n = idx_count;
    uint32_t prims = a.topology == kTopPoints ? n : a.topology == kTopLines ? n / 2 : n / 3;
    if (!prims) {
        skip_prims_++;
        return;
    }
    dev_->DrawIndexedPrimitiveUP(pt, 0, a.vertex_count, prims, idx, D3DFMT_INDEX32, verts, a.nregs * 16);
    g_prof[kPfDraw] += Ticks() - t5;
    if (rt_) rt_->edram_dirty = true;
    if (ds_) ds_->edram_dirty = true;
    draws_++;
    if (cap_) CaptureDraw(s, a, info, desc, bound, fixed);
    // bring-up ablation: three of the title's first textured draws again (the boot self tests, before the first frame,
    // are not), one piece of state off at a time
    static int ablated = 0;
    static bool in_ablation = false;
    if (!in_ablation && ablated < 3 && s.textures[0] && rt_ && frames_ > 0) {
        in_ablation = true;
        ablated++;
        Ablate(s, prim, start, count, indices);
        in_ablation = false;
    }
}

// the share of the target's pixels (16 x 16 samples) whose colour is not bg, after resolving it
uint32_t XenosBackend::CountNot(Target* t, uint32_t bg) {
    if (!t || !t->tex) return 0;
    if (t == rt_ && t->edram_dirty) Resolve(t);
    std::vector<uint32_t> px;
    if (!ReadPixels(t, px)) return 0;
    uint32_t n = 0;
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 16; x++)
            if ((px[size_t(y * t->h / 16) * t->w + x * t->w / 16] & 0x00FFFFFF) != (bg & 0x00FFFFFF)) n++;
    return n;
}

// a target's pixels for the CPU, linear, one A R G B word each: its texture is tiled, and the resolve that wrote it
// must be done first
bool XenosBackend::ReadPixels(Target* t, std::vector<uint32_t>& px) {
    dev_->BlockUntilIdle();
    D3DLOCKED_RECT lr;
    if (FAILED(t->tex->LockRect(0, &lr, NULL, D3DLOCK_READONLY))) return false;
    px.resize(size_t(t->w) * t->h);
    XGUntileSurface(&px[0], t->w * 4, NULL, lr.pBits, t->w, t->h, NULL, 4);
    t->tex->UnlockRect(0);
    return true;
}

// linear A R G B words into a target's tiled texture
void XenosBackend::WritePixels(Target* t, const std::vector<uint32_t>& px) {
    D3DLOCKED_RECT lr;
    if (FAILED(t->tex->LockRect(0, &lr, NULL, 0))) return;
    XGTileSurface(lr.pBits, t->w, t->h, NULL, &px[0], t->w * 4, NULL, 4);
    t->tex->UnlockRect(0);
}

void XenosBackend::Ablate(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    static const char* names[] = {"as the title asked", "no depth test", "no blend", "no texture", "no fog", "all of those off"};
    std::unique_ptr<State> t(new State(s));
    for (int v = 0; v < 6; v++) {
        *t = s;
        if (v == 1 || v == 5) {
            t->simple[kMDepthTestEnable >> 2] = 0;
            t->rs[kRsZEnable] = 0;
        }
        if (v == 2 || v == 5) t->simple[kMBlendEnable >> 2] = 0;
        if (v == 3 || v == 5)
            for (int i = 0; i < 4; i++) t->textures[size_t(i)] = 0;
        if (v == 4 || v == 5) t->rs[kRsFogEnable] = 0;
        // magenta behind, the title's depth left as it is
        Clear(*t, 0, NULL, kClearTarget, 0xFFFF00FF, 1.0f, 0);
        Draw(*t, prim, start, count, indices);
        LOGI("ablation draw %u (prim %u count %u, tex %08X fmt %X, fog %u table %u, zs %08X): %-18s -> %u of 256 pixels drawn",
             draws_, prim, count, s.textures[0], kernel::R32(s.textures[0] + 12) >> 8 & 0xFF, s.rs[kRsFogEnable],
             s.rs[kRsFogTableMode], s.zs.data, names[v], CountNot(rt_, 0xFFFF00FF));
    }
}

void XenosBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) {
    Own();
    // through guest memory: what the GPU drew is written back first, the copy marks the destination for re-upload
    Sync(src.data, 0);
    Sync(dst.data, 0);
    if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points))
        LOG_FIRST(8, "gpu: CopyRects %08X -> %08X formats %X %X not copied", src.data, dst.data, src.format, dst.format);
}

// the GPU's pixels of a colour target written to its guest memory, in the surface's format when it is 32 bit
void XenosBackend::Sync(uint32_t data, uint32_t) {
    Own();
    for (std::map<std::pair<uint64_t, uint32_t>, Target*>::iterator it = targets_.begin(); it != targets_.end(); ++it) {
        Target* t = it->second;
        if (t->depth || t->data != data || !t->tex) continue;
        if (t == rt_ && t->edram_dirty) Resolve(t);
        if (!t->has_content) continue;
        std::vector<uint32_t> px;
        if (!ReadPixels(t, px)) continue;
        // guest surfaces the title draws to are LIN_A8R8G8B8 or LIN_X8R8G8B8 here: pitch rounded to 64 bytes
        uint32_t pitch = (t->lw * 4 + 63) & ~63u;
        for (uint32_t y = 0; y < t->lh; y++) {
            const uint32_t* src = &px[size_t(y * t->h / t->lh) * t->w];
            uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data + y * pitch));
            for (uint32_t x = 0; x < t->lw; x++) __storewordbytereverse(src[x * t->w / t->lw], x * 4, dst);
        }
        t->cpu_epoch = DirtyEpoch(data);
        t->cpu_sig = SampleSig(static_cast<const uint8_t*>(kernel::HostPtr(data)), pitch * t->lh);
    }
}

// JSRF: o painel de texto, numa textura desenhada por software so quando muda
void XenosBackend::DrawOsd() {
    const uint32_t kW = 512, kH = 256;
    if (!g_osd_text[0]) return;
    if (!osd_tex_) {
        dev_->CreateTexture(kW, kH, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &osd_tex_, NULL);
        if (!osd_tex_) return;
        osd_seq_ = g_osd_seq - 1;
    }
    if (osd_seq_ != g_osd_seq) {
        osd_seq_ = g_osd_seq;
        dev_->SetTexture(0, NULL);
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(osd_tex_->LockRect(0, &lr, NULL, 0))) {
            for (uint32_t y = 0; y < kH; y++) {
                uint32_t* line = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch);
                for (uint32_t x = 0; x < kW; x++) line[x] = 0xFF101810;
            }
            uint32_t cx = 8, cy = 6, row = 0;
            for (const char* c = g_osd_text; *c; c++) {
                if (*c == '\n') {
                    cx = 8;
                    cy += 14;
                    row++;
                    continue;
                }
                uint32_t ch = uint32_t(uint8_t(*c));
                if (ch >= 0x61 && ch <= 0x7A) ch -= 0x20;
                if (ch < 0x20 || ch > 0x5A) ch = 0x20;
                const uint8_t* g = kFont5x7[ch - 0x20];
                uint32_t color = row == 0 ? 0xFF60F060 : (c[0] == '>' ? 0xFFFFFF60 : 0xFFF0F0F0);
                for (uint32_t col = 0; col < 5 && cx + col * 2 + 1 < kW; col++)
                    for (uint32_t bit = 0; bit < 8 && cy + bit * 1 < kH; bit++)
                        if (g[col] >> bit & 1) {
                            for (uint32_t dy = 0; dy < 1; dy++)
                                for (uint32_t dx = 0; dx < 2; dx++) {
                                    uint32_t* line = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + (cy + bit + dy) * lr.Pitch);
                                    line[cx + col * 2 + dx] = color;
                                }
                        }
                cx += 12;
            }
            osd_tex_->UnlockRect(0);
        }
    }
    LONG pw = LONG(out_h_) * 4 / 5, ph = pw / 2;
    RECT r = {LONG(out_w_) / 2 - pw / 2, LONG(out_h_) / 2 - ph / 2, LONG(out_w_) / 2 + pw / 2, LONG(out_h_) / 2 + ph / 2};
    Blit(osd_tex_, r, kW, kH);
}

void XenosBackend::Present(const Surface& front) {
    Own();
    int64_t present0 = Ticks();
    uint32_t w = 640, h = 480;
    SurfaceSize(front, w, h);
    w = std::min<uint32_t>(w, 1280);
    h = std::min<uint32_t>(h, 720);
    Target* t = EnsureTarget(front, false, w, h);
    if (t == rt_ && t->edram_dirty) Resolve(t);
    // pixels the CPU wrote (movies, copies) are what is shown when they are newer
    uint64_t e = DirtyEpoch(t->data);
    if (e > t->cpu_epoch || !t->has_content) UploadFromGuest(t, front);
    // the host back buffer takes EDRAM: the guest pair is put back at the next draw
    ResolveBound();
    ForgetStates();
    dev_->SetRenderTarget(0, host_bb_);
    dev_->SetDepthStencilSurface(NULL);
    rt_ = NULL;
    ds_ = NULL;
    dev_->Clear(0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    // a frame capture ends with the picture shown, and the next one starts here
    if (cap_) {
        CaptureTarget(t, "final");
        fprintf(cap_, "END %u draws\n", cap_draw_);
        fclose(cap_);
        cap_ = NULL;
        LOGI("gpu: capture %u: %u draws", cap_n_, cap_draw_);
    } else if (g_capture_want) {
        g_capture_want = false;
        CreateDirectoryA("game:\\dump", NULL);
        // the first of a run: older runs' captures, same names, would pass for this one's
        if (!cap_n_) {
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA("game:\\dump\\cap*", &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    char old[MAX_PATH];
                    _snprintf(old, sizeof(old), "game:\\dump\\%s", fd.cFileName);
                    DeleteFileA(old);
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
        }
        char path[64];
        _snprintf(path, sizeof(path), "game:\\dump\\cap%u.txt", ++cap_n_);
        cap_ = fopen(path, "w");
        cap_draw_ = 0;
        memset(cap_vsc_, 0, sizeof(cap_vsc_));
        cap_seen_.clear();
        LOGI("gpu: capture %u starts, %s", cap_n_, cap_ ? "the next frame is saved" : "the file could not be made");
    }
    // 4:3 in the middle of the output; JSRF: uma cena alargada ocupa a tela toda
    uint32_t dw = g_wide_on && g_wide_scene ? out_w_ : out_h_ * 4 / 3, dx = (out_w_ - std::min(dw, out_w_)) / 2;
    RECT r = {LONG(dx), 0, LONG(dx + std::min(dw, out_w_)), LONG(out_h_)};
    D3DTexture* shown = t->has_content ? t->tex : NULL;
    // a native movie's picture (game/movie.cpp), copied once per picture; lines of 4x4 blocks, 1280 bytes each
    if (const uint8_t* movie = g_movie_frame) {
        if (!movie_tex_) dev_->CreateTexture(640, 480, 1, 0, kFmtLeDxt1, D3DPOOL_DEFAULT, &movie_tex_, NULL);
        if (movie_tex_) {
            if (movie != movie_last_) {
                movie_last_ = movie;
                dev_->SetTexture(0, NULL);
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(movie_tex_->LockRect(0, &lr, NULL, 0))) {
                    for (uint32_t y = 0; y < 120; y++) memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, movie + y * 1280, 1280);
                    movie_tex_->UnlockRect(0);
                }
            }
            shown = movie_tex_;
        }
    } else {
        movie_last_ = NULL;
    }
    if (shown) {
        blit_shown_ = true;
        Blit(shown, r, w, h);
        blit_shown_ = false;
    }
    DrawOsd();
    dev_->Present(NULL, NULL, NULL, NULL);
    frames_++;
    if (g_video_mode != mode_now_) ApplyVideoMode(g_video_mode);
    if (frames_ % 300 == 0) EvictTextures();
    // frame rate every 5 seconds: frames shown and draws per frame
    static int64_t since = host::NowNs();
    static uint32_t frames_then = 0, draws_then = 0;
    g_prof[kPfPresent] += Ticks() - present0;
    int64_t now = host::NowNs();
    if (now - since >= 5000000000ll) {
        uint32_t f = frames_ - frames_then;
        // milliseconds per frame in each part; the rest is the title's own code and the other HLE calls
        double ms[kPfCount], sum = 0, per = f ? 1000.0 / (TicksPerSecond() * f) : 0;
        for (int i = 0; i < kPfCount; i++) {
            ms[i] = double(g_prof[i]) * per;
            sum += ms[i];
            g_prof[i] = 0;
        }
        double frame_ms = f ? double(now - since) / 1e6 / f : 0;
        LOGI("gpu: ms per frame %.2f: assemble %.2f, textures %.2f, uploads %.2f (%u), targets %.2f, shaders %.2f, "
             "states %.2f, samplers %.2f, constants %.2f, draw calls %.2f, present %.2f; title and the rest %.2f",
             frame_ms, ms[kPfAssemble], ms[kPfTexture], ms[kPfUpload], g_uploads, ms[kPfBind], ms[kPfShader], ms[kPfState],
             ms[kPfSampler], ms[kPfConst], ms[kPfDraw], ms[kPfPresent], frame_ms - sum);
        g_uploads = 0;
        double cms[kCpCount];
        for (int i = 0; i < kCpCount; i++) {
            cms[i] = double(g_cprof[i]) * per;
            g_cprof[i] = 0;
        }
        memset(g_ccount, 0, sizeof(g_ccount));
        LOGI("gpu: geometry cache %u hits, %u changed, %u stored; vertices %u hits, %u misses, %u none; %u MB, indices %.2f ms per frame",
             gc_hit_, gc_miss_, gc_store_, vc_hit_, vc_miss_, vc_none_, unsigned(vcache_bytes_ >> 20), double(g_asm_indices) * per);
        gc_hit_ = gc_miss_ = gc_store_ = 0;
        g_asm_indices = 0;
        vc_hit_ = vc_miss_ = vc_none_ = 0;
        // entries no frame drew for a minute
        for (std::unordered_map<uint64_t, GCache>::iterator it = gcache_.begin(); it != gcache_.end();) {
            if (it->second.frame + 3600 < frames_) {
                DropGeometry(it->second);
                it = gcache_.erase(it);
            } else {
                ++it;
            }
        }
        for (std::unordered_map<uint64_t, VCache>::iterator it = vcache_.begin(); it != vcache_.end();) {
            if (it->second.frame + 3600 < frames_) {
                if (it->second.vb) {
                    vcache_bytes_ -= it->second.bytes;
                    free(it->second.vb);
                }
                it = vcache_.erase(it);
            } else {
                ++it;
            }
        }
        LOGI("gpu: %.1f fps, %u draws per frame, %u targets, %u textures, %u+%u shaders; dropped: assemble %u, target %u, "
             "shader %u, empty %u",
             f * 1e9 / double(now - since), f ? (draws_ - draws_then) / f : 0, unsigned(targets_.size()),
             unsigned(textures_.size()), unsigned(vs_.size()), unsigned(ps_.size()), skip_assemble_, skip_bind_, skip_shader_,
             skip_prims_);
        since = now;
        frames_then = frames_;
        draws_then = draws_;
    }
    if (frames_ <= 5 || frames_ % 600 == 0) LOGI("gpu: frame %u, %u draws so far, %u targets, %u textures, %u+%u shaders", frames_,
                                                draws_, unsigned(targets_.size()), unsigned(textures_.size()),
                                                unsigned(vs_.size()), unsigned(ps_.size()));
}

void XenosBackend::SetGamma(const uint8_t* ramp) {
    D3DGAMMARAMP g;
    for (int i = 0; i < 256; i++) {
        g.red[i] = WORD(ramp[i] * 257);
        g.green[i] = WORD(ramp[256 + i] * 257);
        g.blue[i] = WORD(ramp[512 + i] * 257);
    }
    dev_->SetGammaRamp(0, 0, &g);
}

}  // namespace

void RequestCapture() { g_capture_want = true; }
void SetOsdText(const char* text) {
    _snprintf(g_osd_text, sizeof(g_osd_text) - 1, "%s", text ? text : "");
    g_osd_seq++;
}

void CycleGamma() {
    g_gamma_i = (g_gamma_i + 1) % (sizeof(kGammas) / sizeof(kGammas[0]));
    LOGI("gpu: output gamma now %d.%02d", kGammas[g_gamma_i] / 100, kGammas[g_gamma_i] % 100);
}

std::unique_ptr<Backend> CreateXenosBackend() { return std::unique_ptr<Backend>(new XenosBackend); }

}  // namespace gpu
