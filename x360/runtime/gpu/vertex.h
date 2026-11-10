// Xbox 360 build of pc-recomp/sdk/runtime/gpu/vertex.h. Every element is converted on the CPU to a float4 (host
// order, the GPU's default 8-in-32 swap reads it), so a draw has one buffer of nregs float4 per vertex and a 32 bit
// index list. A raw path reading the guest's little endian vertices in place is a later optimisation.
#pragma once
#include <stdint.h>
#include <vector>

#include "gpu/state.h"

namespace gpu {

enum Topology { kTopPoints, kTopLines, kTopTriangles };

// how a vertex shader finishes an element read raw (kept for the shader generator; the converted path reads none)
enum VertexDecode { kDecodeNone, kDecodeW1, kDecodeInt, kDecodeIntW1, kDecodePacked, kDecodeFloat2H };

// one draw's vertices and indices as the renderer takes them
struct Assembled {
    std::vector<float> vertices;    // vertex_count * nregs * 4
    std::vector<uint32_t> indices;  // the draw's primitives as a list (or a strip, see strip)
    uint16_t regs;                  // input registers present, bit per v register
    uint32_t nregs;                 // float4 per vertex, in register order
    uint32_t vertex_count;
    Topology topology;
    bool pretransformed;  // FVF XYZRHW
    // where the vertices come from: when ranged, vertices first to first + vertex_count - 1 of the streams; per
    // converted register, its vertex buffer's data, offset in a vertex, stride and type (the renderer's vertex cache)
    bool ranged;
    uint32_t first;
    uint32_t span;  // vertices from first to the highest the draw reads
    // an unindexed strip (prim 6) or list (prim 5) drawn as it is, its vertices in draw order with no index list made
    // for it (xenos.cpp has a fixed table 0, 1, 2...); 0: use indices. The title draws nearly everything this way, and
    // a list made of a strip cost more than its conversion. direct_count: primitives.
    uint32_t direct_prim, direct_count;
    uint32_t src_base[16], src_off[16], src_stride[16], src_type[16];
    Assembled() : regs(0), nregs(0), vertex_count(0), topology(kTopTriangles), pretransformed(false), ranged(false), first(0), direct_prim(0), direct_count(0) {}
};

// the FVF bits translated to the register layout the XDK's declarations use
void FvfInputs(uint32_t fvf, VertexShader& out);

// false when there is nothing to draw. indices: the title's 16 bit indices (guest memory), null for an unindexed draw
bool Assemble(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices, Assembled& out);
// the same in two steps: everything but the vertices, then the vertices (on the same thread, right after: the order of
// the vertices is kept between the two)
bool AssembleIndices(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices, Assembled& out);
void AssembleVertices(Assembled& out);

}  // namespace gpu
