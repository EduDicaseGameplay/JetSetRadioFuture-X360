// Xbox 360 build of pc-recomp/sdk/runtime/gpu/vertex.cpp, the converted path: guest vertices (little endian) to
// float4 per input register, primitives the GPU lacks (fans, quads, loops, polygons) rebuilt as lists.
#include "gpu/vertex.h"

#include <algorithm>
#include <string.h>

#include "core/log.h"
#include "kernel/kernel.h"

namespace gpu {

namespace {

const uint32_t kTypeFloat2H = 0x72;  // FLOAT2H
// D3DVSDT_NONE: a FLOAT with no components, the register reads no stream data
const uint32_t kTypeNone = 0x02;
const uint32_t kResData = 4;

inline uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline float LeF(const uint8_t* p) {
    union { uint32_t u; float f; } c;
    c.u = Le32(p);
    return c.f;
}

// D3DVSDT: low nibble the kind, high nibble the component count
void Convert(uint32_t type, const uint8_t* p, float* o) {
    o[0] = 0;
    o[1] = 0;
    o[2] = 0;
    o[3] = 1;
    uint32_t kind = type & 15;
    uint32_t n = std::min<uint32_t>((type >> 4) & 15, 4);
    if (type == kTypeFloat2H) {
        o[0] = LeF(p);
        o[1] = LeF(p + 4);
        o[3] = LeF(p + 8);
        return;
    }
    switch (kind) {
        case 0:  // D3DCOLOR, bytes B G R A
            o[0] = p[2] / 255.0f;
            o[1] = p[1] / 255.0f;
            o[2] = p[0] / 255.0f;
            o[3] = p[3] / 255.0f;
            break;
        case 1:  // NORMSHORT
            for (uint32_t i = 0; i < n; i++) o[i] = std::max(int16_t(Le16(p + i * 2)) / 32767.0f, -1.0f);
            break;
        case 2:  // FLOAT
            for (uint32_t i = 0; i < n; i++) o[i] = LeF(p + i * 4);
            break;
        case 4:  // PBYTE
            for (uint32_t i = 0; i < n; i++) o[i] = p[i] / 255.0f;
            break;
        case 5:  // SHORT
            for (uint32_t i = 0; i < n; i++) o[i] = int16_t(Le16(p + i * 2));
            break;
        case 6: {  // NORMPACKED3, 11 11 10 signed
            uint32_t v = Le32(p);
            int32_t x = int32_t(v << 21) >> 21;
            int32_t y = int32_t(v << 10) >> 21;
            int32_t z = int32_t(v) >> 22;
            o[0] = std::max(x / 1023.0f, -1.0f);
            o[1] = std::max(y / 1023.0f, -1.0f);
            o[2] = std::max(z / 511.0f, -1.0f);
            break;
        }
        default:
            break;
    }
}

uint32_t TypeBytes(uint32_t t) {
    if (t == kTypeFloat2H) return 12;
    uint32_t kind = t & 15;
    uint32_t n = (t >> 4) & 15;
    switch (kind) {
        case 0: return 4;
        case 1: case 5: return 2 * n;
        case 4: return n;
        case 6: return 4;
        default: return 4 * n;
    }
}

inline void Push2(std::vector<uint32_t>& ix, uint32_t a, uint32_t b) {
    ix.push_back(a);
    ix.push_back(b);
}
inline void Push3(std::vector<uint32_t>& ix, uint32_t a, uint32_t b, uint32_t c) {
    ix.push_back(a);
    ix.push_back(b);
    ix.push_back(c);
}

// the index list for any primitive, local maps a draw position to the vertex in the converted buffer
template <typename F>
void ListIndices(uint32_t prim, uint32_t count, F local, std::vector<uint32_t>& ix, Topology& topology) {
    ix.reserve(size_t(count) * 3);
    topology = kTopTriangles;
    switch (prim) {
        case 1:  // points
            topology = kTopPoints;
            for (uint32_t i = 0; i < count; i++) ix.push_back(local(i));
            break;
        case 2:  // line list
            topology = kTopLines;
            for (uint32_t i = 0; i + 1 < count; i += 2) Push2(ix, local(i), local(i + 1));
            break;
        case 3:  // line loop
        case 4:  // line strip
            topology = kTopLines;
            for (uint32_t i = 0; i + 1 < count; i++) Push2(ix, local(i), local(i + 1));
            if (prim == 3 && count > 2) Push2(ix, local(count - 1), local(0));
            break;
        case 5:  // triangle list
            for (uint32_t i = 0; i + 2 < count; i += 3) Push3(ix, local(i), local(i + 1), local(i + 2));
            break;
        case 6:  // triangle strip
            for (uint32_t i = 0; i + 2 < count; i++) {
                if (i & 1) Push3(ix, local(i + 1), local(i), local(i + 2));
                else Push3(ix, local(i), local(i + 1), local(i + 2));
            }
            break;
        case 7:   // fan
        case 10:  // polygon
            for (uint32_t i = 1; i + 1 < count; i++) Push3(ix, local(0), local(i), local(i + 1));
            break;
        case 8:  // quad list
            for (uint32_t i = 0; i + 3 < count; i += 4) {
                Push3(ix, local(i), local(i + 1), local(i + 2));
                Push3(ix, local(i), local(i + 2), local(i + 3));
            }
            break;
        case 9:  // quad strip
            for (uint32_t i = 0; i + 3 < count; i += 2) {
                Push3(ix, local(i), local(i + 1), local(i + 2));
                Push3(ix, local(i + 1), local(i + 3), local(i + 2));
            }
            break;
    }
}

// the declaration's register layout, or the one an FVF handle stands for, built into fvf_layout
const VertexShader* PickLayout(const State& s, VertexShader& fvf_layout) {
    if (!(s.vs_handle & 1)) {
        FvfInputs(s.vs_handle, fvf_layout);
        return &fvf_layout;
    }
    return s.vs.get();
}

// a vertex range at most this far over the draw's count is converted whole, in range order
const uint64_t kRangedSpanPerVertex = 2;
const uint64_t kRangedSpanSlack = 64;

struct Local {
    const std::vector<uint32_t>* seq;
    uint32_t first;
    bool ranged;  // indexed and in range order: the draw position's vertex; otherwise the position itself
    uint32_t operator()(uint32_t i) const { return ranged ? (*seq)[i] - first : i; }
};

}  // namespace

void FvfInputs(uint32_t fvf, VertexShader& vs) {
    for (int i = 0; i < 16; i++) vs.inputs[i] = VertexShader::Input();
    uint32_t off = 0;
#define FVF_ADD(reg, type)                                     \
    do {                                                       \
        vs.inputs[size_t(reg)] = VertexShader::Input(0, off, (type)); \
        off += TypeBytes(type);                                \
    } while (0)
    // D3DFVF: position 0xE, normal 0x10, point size 0x20, diffuse 0x40, specular 0x80, texture count bits 8 to 11,
    // sizes from bit 16
    switch (fvf & 0xE) {
        case 0x2: FVF_ADD(0, 0x32); break;
        case 0x4: FVF_ADD(0, 0x42); break;
        case 0x6: case 0x8: case 0xA: case 0xC: {
            FVF_ADD(0, 0x32);
            uint32_t weights = ((fvf & 0xE) - 0x4) / 2;
            FVF_ADD(1, 0x02 | (weights << 4));
            break;
        }
        default: break;
    }
    if (fvf & 0x10) FVF_ADD(2, 0x32);
    if (fvf & 0x20) FVF_ADD(6, 0x12);
    if (fvf & 0x40) FVF_ADD(3, 0x40);
    if (fvf & 0x80) FVF_ADD(4, 0x40);
    uint32_t tex = (fvf >> 8) & 15;
    static const uint32_t kTexType[4] = {0x22, 0x32, 0x42, 0x12};
    for (uint32_t i = 0; i < tex && i < 4; i++) FVF_ADD(int(9 + i), kTexType[(fvf >> (16 + i * 2)) & 3]);
#undef FVF_ADD
    vs.stream_stride.fill(0);
    vs.stream_stride[0] = off;
}

// vertex numbers in draw order (kept: a fight draws thousands of times a frame), from AssembleIndices to AssembleVertices
static std::vector<uint32_t> g_seq;

bool Assemble(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* ip, Assembled& out) {
    if (!AssembleIndices(s, prim, start, count, ip, out)) return false;
    AssembleVertices(out);
    return true;
}

bool AssembleIndices(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* ip, Assembled& out) {
    using namespace kernel;
    // (the vertices are not cleared: AssembleVertices would zero the whole buffer again before writing it, every draw)
    out.indices.clear();
    out.direct_prim = 0;
    out.direct_count = 0;
    out.regs = 0;
    out.nregs = 0;
    out.vertex_count = 0;
    out.topology = kTopTriangles;
    if (!count || prim < 1 || prim > 10) return false;

    VertexShader fvf_layout;
    const VertexShader* layout = PickLayout(s, fvf_layout);
    out.pretransformed = !(s.vs_handle & 1) && (s.vs_handle & 0xE) == 0x4;
    if (!layout) return false;

    // the input registers fed by a bound stream
    int regs[16], nregs = 0;
    for (int r = 0; r < 16; r++) {
        const VertexShader::Input& in = layout->inputs[size_t(r)];
        if (in.stream < 0 || !in.type || in.type == kTypeNone) continue;
        if (!s.streams[size_t(in.stream)].vb) continue;
        regs[nregs++] = r;
        out.regs |= uint16_t(1u << r);
    }
    if (!nregs) return false;
    out.nregs = uint32_t(nregs);

    // vertex numbers in draw order and the range they span
    std::vector<uint32_t>& seq = g_seq;
    uint32_t lo = start, n = count;
    out.span = count;
    bool ranged = true;
    if (ip) {
        seq.resize(count);
        uint32_t hi = 0;
        lo = 0xFFFFFFFF;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t v = Le16(ip + i * 2) + s.base_vertex;
            seq[i] = v;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        uint32_t span = hi - lo + 1;
        out.span = span;
        ranged = uint64_t(span) <= uint64_t(count) * kRangedSpanPerVertex + kRangedSpanSlack;
        n = ranged ? span : count;
    }
    out.vertex_count = n;
    out.ranged = ranged;
    out.first = lo;
    for (int k = 0; k < nregs; k++) {
        const VertexShader::Input& in = layout->inputs[size_t(regs[k])];
        const State::Stream& st = s.streams[size_t(in.stream)];
        out.src_base[k] = mem::VirtualFromPhysical(R32(st.vb + kResData));
        out.src_off[k] = in.offset;
        out.src_stride[k] = st.stride ? st.stride : layout->stream_stride[size_t(in.stream)];
        out.src_type[k] = in.type;
    }
    // not with flat shading: a list made from a strip keeps the first vertex of each triangle as it is now
        if (!ip && (prim == 5 || prim == 6) && count <= 65535 && s.simple[kMShadeMode >> 2] != 0x1D00) {
        out.direct_prim = prim;
        out.direct_count = prim == 6 ? (count >= 3 ? count - 2 : 0) : count / 3;
        return out.direct_count != 0;
    }
    Local local = {&seq, lo, ranged && ip != NULL};
    ListIndices(prim, count, local, out.indices, out.topology);
    return !out.indices.empty();
}

void AssembleVertices(Assembled& out) {
    using namespace kernel;
    const std::vector<uint32_t>& seq = g_seq;
    const int nregs = int(out.nregs);
    const uint32_t n = out.vertex_count, lo = out.first;
    const bool ranged = out.ranged;
    if (out.vertices.size() < size_t(n) * nregs * 4) out.vertices.resize(size_t(n) * nregs * 4);
    for (int k = 0; k < nregs; k++) {
        uint32_t stride = out.src_stride[k];
        const uint8_t* src = static_cast<const uint8_t*>(HostPtr(out.src_base[k] + out.src_off[k]));
        float* dst = &out.vertices[size_t(k) * 4];
        const size_t vstep = size_t(nregs) * 4;
        struct { uint32_t type; } in = {out.src_type[k]};
        uint32_t kind = in.type & 15, comps = (in.type >> 4) & 15;
        // the common kinds in integer registers only: a float's bits are moved as a word (on this in-order CPU an
        // integer stored and read back as a float stalls tens of cycles), colours through a table; the rest via Convert
        if (in.type != kTypeFloat2H && kind == 2 && comps >= 1 && comps <= 4) {
            uint32_t* d = reinterpret_cast<uint32_t*>(dst);
            // in range order, by component count: this loop is most of a draw's conversion
            if (ranged && comps == 4) {
                const uint8_t* p = src + size_t(lo) * stride;
                for (uint32_t v = 0; v < n; v++, d += vstep, p += stride) {
                    __dcbt(384, p);
                    d[0] = __loadwordbytereverse(0, p);
                    d[1] = __loadwordbytereverse(4, p);
                    d[2] = __loadwordbytereverse(8, p);
                    d[3] = __loadwordbytereverse(12, p);
                }
            } else if (ranged && comps == 3) {
                const uint8_t* p = src + size_t(lo) * stride;
                for (uint32_t v = 0; v < n; v++, d += vstep, p += stride) {
                    __dcbt(384, p);
                    d[0] = __loadwordbytereverse(0, p);
                    d[1] = __loadwordbytereverse(4, p);
                    d[2] = __loadwordbytereverse(8, p);
                    d[3] = 0x3F800000u;
                }
            } else if (ranged && comps == 2) {
                const uint8_t* p = src + size_t(lo) * stride;
                for (uint32_t v = 0; v < n; v++, d += vstep, p += stride) {
                    __dcbt(384, p);
                    d[0] = __loadwordbytereverse(0, p);
                    d[1] = __loadwordbytereverse(4, p);
                    d[2] = 0;
                    d[3] = 0x3F800000u;
                }
            } else for (uint32_t v = 0; v < n; v++, d += vstep) {
                const uint8_t* p = src + size_t(ranged ? lo + v : seq[v]) * stride;
                d[0] = __loadwordbytereverse(0, p);
                d[1] = comps > 1 ? __loadwordbytereverse(4, p) : 0u;
                d[2] = comps > 2 ? __loadwordbytereverse(8, p) : 0u;
                d[3] = comps > 3 ? __loadwordbytereverse(12, p) : 0x3F800000u;  // 1.0f
            }
        } else if (kind == 0 && in.type != kTypeFloat2H) {
            static float unit[256];
            static bool unit_made = false;
            if (!unit_made) {
                for (int i = 0; i < 256; i++) unit[i] = i / 255.0f;
                unit_made = true;
            }
            for (uint32_t v = 0; v < n; v++, dst += vstep) {
                const uint8_t* p = src + size_t(ranged ? lo + v : seq[v]) * stride;
                dst[0] = unit[p[2]];
                dst[1] = unit[p[1]];
                dst[2] = unit[p[0]];
                dst[3] = unit[p[3]];
            }
        } else {
            for (uint32_t v = 0; v < n; v++, dst += vstep) Convert(in.type, src + size_t(ranged ? lo + v : seq[v]) * stride, dst);
        }
    }
}

}  // namespace gpu
