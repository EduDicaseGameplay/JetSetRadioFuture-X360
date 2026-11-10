// Xbox 360 build of pc-recomp/sdk/runtime/gpu/shaders.h: Shader Model 3 HLSL for the title's shaders and its fixed
// function state, per the XDK's definitions of both. Constants sit at fixed registers per stage (below).
#pragma once
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

#include "gpu/state.h"

namespace gpu {

// vertex shader constants of a vertex program: the title's c[0..191], then
enum { kVsSurf = 192, kVsFog = 193, kVsWide = 255 };
// of the fixed function pipeline
enum {
    kVfSurf = 0, kVfFog = 1, kVfWvp = 2, kVfWv = 6, kVfWvn = 10, kVfTm = 14, kVfMat = 30, kVfAmb = 35, kVfLt = 36,
    kVfCount = 93
};
// pixel shader constants
enum {
    kPsSurf = 0, kPsFogColor = 1, kPsMisc = 2, kPsPc = 3, kPsTfactor = 21, kPsTexsize = 22, kPsBump = 26, kPsBumpl = 30,
    kPsCount = 34
};

enum TexKind { kTexNone, kTex2D, kTex3D, kTexCube };

struct TexInfo {
    uint8_t kind;
    uint8_t rect;    // linear texture, coordinates in texels
    uint8_t shadow;  // depth texture: 1 D24, 2 D16
    uint8_t flags;
};

struct PixelKey {
    uint32_t programmable;
    // combiners, straight from D3DPIXELSHADERDEF
    uint32_t count, rgb_in[8], rgb_out[8], a_in[8], a_out[8], final_abcd, final_efg;
    uint32_t tex_modes, dot_mapping, input_texture, compare_mode;
    // fixed function stages: colorop, arg1, arg2, arg0, alphaop, arg1, arg2, arg0, result
    uint32_t ff[4][9];
    TexInfo tex[4];
    uint32_t alphakill;  // bit per stage
    uint32_t flat, two_sided;
    uint32_t proj;  // fixed function: bit per stage sampled at coordinates divided by w
    uint32_t quad;
    uint32_t atest, fog, spec;  // JSRF: teste de alfa, neblina e especular ligados
    PixelKey() { memset(this, 0, sizeof(*this)); }
    bool operator==(const PixelKey& o) const;
};

uint64_t Hash(const void* p, size_t n, uint64_t h = 1469598103934665603ull);

PixelKey MakePixelKey(const State& s, const TexInfo tex[4]);

// vertex program microcode to HLSL, regs is the bit mask of v registers the draw supplies
std::string VertexProgramHlsl(const std::vector<uint32_t>& program, uint16_t regs, uint64_t decode = 0);
// fixed function vertex state a shader is built for
struct FixedKey {
    uint32_t light;  // bit 0 lighting, 1 specular, 2 local viewer, 3 normalize, then D3DMCS 2 bits each from bit 8
    uint32_t types;  // D3DLIGHTTYPE per light, 2 bits each, 0 off
    uint32_t tex[4];  // per stage: coordinate set, texgen at bit 4, D3DTTFF count at bit 8, projected bit 11, set size at bit 12
    FixedKey() { memset(this, 0, sizeof(*this)); }
};
FixedKey MakeFixedKey(const State& s);
std::string FixedVertexHlsl(uint16_t regs, bool pretransformed, uint64_t decode, const FixedKey& f);
std::string PixelHlsl(const PixelKey& k);

}  // namespace gpu
