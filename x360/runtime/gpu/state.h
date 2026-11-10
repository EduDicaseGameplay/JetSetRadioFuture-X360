#pragma once
#include <array>
#include <stdint.h>
#include <string.h>
#include <memory>
#include <vector>

// Everything the title handed to the XDK Direct3D API, as the API defines it. The renderer translates
// this to D3D12; nothing here models the console's GPU.
namespace gpu {

// D3DRS indices of the XDK 5849 build this title links, taken from its own tables
enum : uint32_t {
    kRsDeferredFirst = 92,
    kRsFogEnable = 92,
    kRsFogTableMode = 93,
    kRsFogStart = 94,
    kRsFogEnd = 95,
    kRsFogDensity = 96,
    kRsRangeFogEnable = 97,
    kRsWrap0 = 98,
    kRsLighting = 102,
    kRsSpecularEnable = 103,
    kRsLocalViewer = 104,
    kRsColorVertex = 105,
    kRsSpecularMaterialSource = 110,
    kRsDiffuseMaterialSource = 111,
    kRsAmbientMaterialSource = 112,
    kRsEmissiveMaterialSource = 113,
    kRsAmbient = 115,
    kRsPointSize = 116,
    kRsPointSizeMin = 117,
    kRsPointSpriteEnable = 118,
    kRsPointScaleEnable = 119,
    kRsPointScaleA = 120,
    kRsPointScaleB = 121,
    kRsPointScaleC = 122,
    kRsPointSizeMax = 123,
    kRsPresentationInterval = 127,
    kRsComplexFirst = 136,
    kRsPsTextureModes = 136,
    kRsVertexBlend = 137,
    kRsFogColor = 138,
    kRsFillMode = 139,
    kRsBackFillMode = 140,
    kRsTwoSidedLighting = 141,
    kRsNormalizeNormals = 142,
    kRsZEnable = 143,
    kRsStencilEnable = 144,
    kRsStencilFail = 145,
    kRsFrontFace = 146,
    kRsCullMode = 147,
    kRsTextureFactor = 148,
    kRsZBias = 149,
    kRsLogicOp = 150,
    kRsEdgeAntiAlias = 151,
    kRsMultiSampleAntiAlias = 152,
    kRsMultiSampleMask = 153,
    kRsMultiSampleMode = 154,
    kRsMultiSampleRenderTargetMode = 155,
    kRsShadowFunc = 156,
    kRsLineWidth = 157,
    kRsSampleAlpha = 158,
    kRsDxt1NoiseEnable = 159,
    kRsYuvEnable = 160,
    kRsOcclusionCullEnable = 161,
    kRsStencilCullEnable = 162,
    kRsRopZCmpAlwaysRead = 163,
    kRsRopZRead = 164,
    kRsDoNotCullUncompressed = 165,
    kRsMax = 166,
};

// Simple render states reach the API as (method, value) pairs; the method is the key the XDK's own
// D3DSIMPLERENDERSTATEENCODE table assigns to each state.
enum : uint32_t {
    kMAlphaTestEnable = 0x300,
    kMBlendEnable = 0x304,
    kMDepthTestEnable = 0x30C,
    kMDitherEnable = 0x310,
    kMAlphaFunc = 0x33C,
    kMAlphaRef = 0x340,
    kMSrcBlend = 0x344,
    kMDestBlend = 0x348,
    kMBlendColor = 0x34C,
    kMBlendOp = 0x350,
    kMZFunc = 0x354,
    kMColorWriteEnable = 0x358,
    kMZWriteEnable = 0x35C,
    kMStencilWriteMask = 0x360,
    kMStencilFunc = 0x364,
    kMStencilRef = 0x368,
    kMStencilMask = 0x36C,
    kMStencilTestEnable = 0x370,
    kMStencilZFail = 0x374,
    kMStencilPass = 0x378,
    kMShadeMode = 0x37C,
    kMSolidOffsetEnable = 0x338,
    kMSlopeScale = 0x384,
    kMDepthOffset = 0x388,
    kMPsAlphaInputs = 0x260,     // 8
    kMPsFinalAbcd = 0x288,
    kMPsFinalEfgh = 0x28C,
    kMPsConstant0 = 0xA60,       // 8
    kMPsConstant1 = 0xA80,       // 8
    kMPsAlphaOutputs = 0xAA0,    // 8
    kMPsRgbInputs = 0xAC0,       // 8
    kMPsCompareMode = 0x17F8,
    kMPsFinalConstant0 = 0x1E20,
    kMPsFinalConstant1 = 0x1E24,
    kMPsRgbOutputs = 0x1E40,     // 8
    kMPsCombinerCount = 0x1E60,
    kMPsDotMapping = 0x1E74,
    kMPsInputTexture = 0x1E78,
    kMDepthClipControl = 0x1D78,
    // visibility tests, sent as raw commands rather than render states
    kMClearReport = 0x17C8,
    kMCountEnable = 0x17CC,
    kMGetReport = 0x17D0,
};

// texture stage state indices
enum : uint32_t {
    kTssAddressU = 0, kTssAddressV, kTssAddressW, kTssMagFilter, kTssMinFilter, kTssMipFilter, kTssMipLodBias,
    kTssMaxMipLevel, kTssMaxAnisotropy, kTssColorKeyOp, kTssColorSign, kTssAlphaKill, kTssColorOp, kTssColorArg0,
    kTssColorArg1, kTssColorArg2, kTssAlphaOp, kTssAlphaArg0, kTssAlphaArg1, kTssAlphaArg2, kTssResultArg,
    kTssTextureTransformFlags, kTssBumpEnvMat00, kTssBumpEnvMat01, kTssBumpEnvMat11, kTssBumpEnvMat10,
    kTssBumpEnvLScale, kTssBumpEnvLOffset, kTssTexCoordIndex, kTssBorderColor, kTssColorKeyColor, kTssCount = 32
};

struct VertexShader {
    uint32_t handle;  // guest handle, record address | 1
    std::vector<uint32_t> decl;
    std::vector<uint32_t> program;  // 4 dwords per instruction
    uint32_t program_type;          // 0x2078 vertex, 0x7378 state, 0x7778 read/write
    uint64_t hash;
    // stream layout parsed from the declaration: register -> stream, offset, type
    struct Input {
        int stream;
        uint32_t offset;
        uint32_t type;  // D3DVSDT
        Input() : stream(-1), offset(0), type(0) {}
        Input(int s, uint32_t o, uint32_t t) : stream(s), offset(o), type(t) {}
    };
    std::array<Input, 16> inputs;
    std::array<uint32_t, 16> stream_stride;  // bytes consumed per stream by the declaration
    typedef std::array<float, 4> Vec4;
    std::vector<std::pair<uint32_t, Vec4> > constants;  // D3DVSD_CONST tokens
    VertexShader() : handle(0), program_type(0), hash(0) { stream_stride.fill(0); }
};

struct PixelShader {
    uint32_t handle;
    uint32_t def[0xF0 / 4];  // D3DPIXELSHADERDEF
    PixelShader() : handle(0) { memset(def, 0, sizeof(def)); }
};

struct Viewport {
    uint32_t x, y, w, h;
    float min_z, max_z;
    Viewport() : x(0), y(0), w(640), h(480), min_z(0), max_z(1) {}
    Viewport(uint32_t x_, uint32_t y_, uint32_t w_, uint32_t h_, float a, float b)
        : x(x_), y(y_), w(w_), h(h_), min_z(a), max_z(b) {}
};

struct Surface {
    uint32_t header;  // guest D3DSurface
    uint32_t data, format, size;
    Surface() : header(0), data(0), format(0), size(0) {}
};

struct State {
    std::array<uint32_t, 0x800> simple;  // simple render states indexed by method / 4
    std::array<uint32_t, kRsMax> rs;     // deferred and complex render states by D3DRS
    std::array<std::array<uint32_t, kTssCount>, 4> tss;
    std::array<uint32_t, 4> textures;  // guest D3DBaseTexture pointers
    std::array<uint32_t, 4> palettes;  // guest D3DPalette pointers, the colours of the stage's P8 texture
    struct Stream {
        uint32_t vb;  // guest D3DVertexBuffer
        uint32_t stride;
        Stream() : vb(0), stride(0) {}
        Stream(uint32_t v, uint32_t s) : vb(v), stride(s) {}
    };
    std::array<Stream, 16> streams;
    uint32_t index_buffer, index_data, base_vertex;
    uint32_t vs_handle;  // programmable when bit 0 set, otherwise an FVF
    std::shared_ptr<VertexShader> vs;
    std::shared_ptr<PixelShader> ps;
    // vertex program constants written since the render thread last took them, [vsc_lo, vsc_hi) (render_thread.cpp)
    mutable uint32_t vsc_lo, vsc_hi;
    void VscWritten(uint32_t first, uint32_t count) {
        if (first < vsc_lo) vsc_lo = first;
        if (first + count > vsc_hi) vsc_hi = first + count > 192 ? 192 : first + count;
        vsc_epoch++;
        Mark(&vsc_epoch, sizeof(vsc_epoch));  // v22 A did not: the renderer kept old vertex program constants
    }
    // 64 byte pieces of this State written since the render thread last took them (render_thread.cpp): the writers
    // of the plain data mark what they write, so a draw sends those pieces instead of comparing the whole State
    static const uint32_t kDirtyWords = 24;
    // the render thread's copy only (render_thread.cpp): each moves when a sent change reaches its group (kGen*), so
    // the renderer remakes only the constants made from a group that moved
    enum { kGenWorld, kGenView, kGenProj, kGenTexMat, kGenMaterial, kGenLights, kGenFog, kGenPixel, kGenShader, kGens };
    uint32_t gen[kGens];
    bool gens_valid;
    mutable uint32_t dirty[kDirtyWords];
    void Mark(const void* p, size_t n) {
        size_t off = size_t(static_cast<const uint8_t*>(p) - reinterpret_cast<const uint8_t*>(this));
        for (size_t i = off >> 6, last = (off + n - 1) >> 6; i <= last; i++) dirty[i >> 5] |= 1u << (i & 31);
    }
    std::array<std::array<float, 4>, 192> vsc;  // hardware order: D3D register + 96
    uint64_t vsc_epoch;
    std::array<std::array<float, 4>, 16> psc;
    Viewport viewport;
    Surface rt, zs;
    uint32_t scissor_count;
    std::array<uint32_t, 4 * 8> scissors;
    // fixed function
    std::array<std::array<float, 16>, 512> transforms;  // by D3DTRANSFORMSTATETYPE
    std::array<float, 17> material;
    std::array<std::array<uint32_t, 26>, 8> lights;  // D3DLIGHT8
    uint32_t light_enable;                           // bit per light
    float ss_offset_x, ss_offset_y;
    float zscale;  // depth buffer units per 1.0
    State() { Reset(); }
    // everything back to zero, as a value initialised State
    void Reset() {
        memset(&simple, 0, sizeof(simple));
        memset(&rs, 0, sizeof(rs));
        memset(&tss, 0, sizeof(tss));
        memset(&textures, 0, sizeof(textures));
        memset(&palettes, 0, sizeof(palettes));
        for (int i = 0; i < 16; i++) streams[i] = Stream();
        index_buffer = index_data = base_vertex = 0;
        vs_handle = 0;
        vs.reset();
        ps.reset();
        memset(&vsc, 0, sizeof(vsc));
        vsc_epoch = 1;
        vsc_lo = 0;
        vsc_hi = 192;
        memset(dirty, 0xFF, sizeof(dirty));
        memset(gen, 0, sizeof(gen));
        gens_valid = false;
        memset(&psc, 0, sizeof(psc));
        viewport = Viewport();
        rt = Surface();
        zs = Surface();
        scissor_count = 0;
        memset(&scissors, 0, sizeof(scissors));
        memset(&transforms, 0, sizeof(transforms));
        memset(&material, 0, sizeof(material));
        memset(&lights, 0, sizeof(lights));
        light_enable = 0;
        ss_offset_x = ss_offset_y = 0;
        zscale = 16777215.0f;
    }
};

inline uint32_t SimpleState(const State& s, uint32_t method) {
    return s.simple[method >> 2];
}

}  // namespace gpu
