// Direct3D 8 (XDK 5849) entry points the title calls, implemented on the host renderer.
// Resource creation, locking and the XG helpers stay recompiled: they only manage memory.
// Xbox 360 build (see x360/tools/port_d3d8.py): guest floats and dwords read and written through the kernel's
// byte reversing accessors, the Xenos renderer drawing on the title's thread.
#include <algorithm>
#include <atomic>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "app/app.h"
#include "gpu/backend.h"
#include "host.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;

// config/game.toml has this function at 0, so the recompiled body ran: it pushed the factor to a push buffer nobody
// reads and the renderer kept 0. The floors take their alpha from it (sub_00025640: material alpha, 1.0 becoming 0.2),
// and at 0 they were opaque over the mirrored world drawn before them.
#undef SYM_D3DDevice_SetRenderState_TextureFactor
#define SYM_D3DDevice_SetRenderState_TextureFactor 000ED620

// configs written before the SDK knew the name
#ifndef SYM_D3DDevice_BeginPush
#define SYM_D3DDevice_BeginPush FFFFFE00
#define HAS_D3DDevice_BeginPush 0
#endif
#ifndef SYM_D3DDevice_EndPush
#define SYM_D3DDevice_EndPush FFFFFE01
#define HAS_D3DDevice_EndPush 0
#endif
#ifndef HAS_D3D_FvfShader
#define HAS_D3D_FvfShader 0
#endif
#ifndef SYM_D3DDevice_SetPalette
#define SYM_D3DDevice_SetPalette FFFFFE02
#define HAS_D3DDevice_SetPalette 0
#endif
#ifndef SYM_D3DDevice_GetVertexShaderFunction
#define SYM_D3DDevice_GetVertexShaderFunction FFFFFE03
#define HAS_D3DDevice_GetVertexShaderFunction 0
#endif
#ifndef SYM_D3DDevice_UpdateOverlay
#define SYM_D3DDevice_UpdateOverlay FFFFFE04
#define HAS_D3DDevice_UpdateOverlay 0
#endif

namespace d3d {

// false builds the renderer on the title's thread (the v15 way), for comparing
const bool kRenderThread = true;

namespace {


// D3D globals of this build
const uint32_t kDevice = SYM(D3D_Device);        // the static CDevice, g_pDevice points at it
const uint32_t kDevicePtr = SYM(D3D_pDevice);
const uint32_t kIndexData = SYM(D3D_IndexData);  // index data base the inline draws read
const uint32_t kDirtyFlags = SYM(D3D_DirtyFlags);
const uint32_t kTextureState = SYM(D3D_DeferredTextureState);  // [4][32]
const uint32_t kRenderState = SYM(D3D_RenderState);            // [166]
const uint32_t kStreams = SYM(D3D_Streams);                    // [16] stride, offset, buffer
const uint32_t kSimpleEncode = SYM(D3D_SimpleRenderStateEncode);

// CDevice fields mirrored so the recompiled getters and resource code see what the XDK would
const uint32_t kDevPush = XDK_CDevice_Push, kDevThreshold = XDK_CDevice_PushThreshold,
                   kDevBaseVertex = XDK_CDevice_BaseVertex, kDevIndexBuffer = XDK_CDevice_IndexBuffer,
                   kDevTextures = XDK_CDevice_Textures, kDevRenderTarget = XDK_CDevice_RenderTarget,
                   kDevDepth = XDK_CDevice_Depth, kDevSwapCount = XDK_CDevice_SwapCount,
                   kDevBackBuffer = XDK_CDevice_BackBuffer, kDevFrontBuffer = XDK_CDevice_FrontBuffer,
                   kDevZScale = XDK_CDevice_ZScale, kDevSuperX = XDK_CDevice_SuperSampleX,
                   kDevSuperY = XDK_CDevice_SuperSampleY, kDevTransforms = XDK_CDevice_Transforms,
                   kDevPalettes = XDK_CDevice_Palettes;
const uint32_t kStageStates = XDK_TextureState_Stage, kStreamSize = XDK_Stream_Size;

gpu::State g_state;
std::unique_ptr<gpu::Backend> g_backend;
std::recursive_mutex g_lock;

std::unordered_map<uint32_t, std::shared_ptr<gpu::VertexShader>> g_vs;
std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> g_ps;
std::array<uint32_t, 3> g_buffers;  // front, back surfaces (zeroed at CreateDevice)
uint32_t g_buffer_count = 2;
uint32_t g_depth = 0;
uint32_t g_scratch_push = 0, g_scratch_size = 0;
uint32_t g_walk = 0;  // first scratch command not read yet

uint32_t Dev(uint32_t off) { return R32(kDevice + off); }
void SetDev(uint32_t off, uint32_t v) { W32(kDevice + off, v); }

// The device's own references: a count in bits 19 to 22 of the resource's first dword. A surface's first one
// also holds its parent. The library's functions do it when the image has them, inlined ones are done here.
void AddRef(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalAddRef
    GUEST_CALL(D3D_InternalAddRef, ctx, res);
#else
    (void)ctx;
    uint32_t common = R32(res);
    if (!(common & 0x780000) && (common & 0x70000) == 0x50000)
        if (uint32_t parent = R32(res + 0x14)) W32(parent, R32(parent) + 0x80000);
    W32(res, R32(res) + 0x80000);
#endif
}
void Release(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalRelease
    GUEST_CALL(D3D_InternalRelease, ctx, res);
#else
    (void)ctx;
    // the library frees a resource nothing holds any more; without its function the memory stays
    auto drop = [](uint32_t r) {
        W32(r, R32(r) - 0x80000);
        if (!(R32(r) & 0x78FFFF)) LOG_FIRST(8, "d3d: resource %08X is not freed, the image has no D3D_InternalRelease", r);
    };
    uint32_t common = R32(res);
    if ((common & 0x780000) == 0x80000 && (common & 0x70000) == 0x50000)
        if (uint32_t parent = R32(res + 0x14)) drop(parent);
    drop(res);
#endif
}

void Walk(uint32_t end);

// Hardware commands a caller still pushes go to a scratch buffer, rewound at every request once Walk has read it
uint32_t ScratchPush(uint32_t bytes = 0) {
    Walk(Dev(kDevPush));
    uint32_t need = std::max<uint32_t>(0x200000, (bytes + 0x1FFF) & ~0xFFFu);
    // the host's own buffers come from the system pool: the guest's 64 MB stay the title's
    if (need > g_scratch_size) {
        if (g_scratch_push) mem::FreeSystem(g_scratch_push);
        g_scratch_push = mem::AllocSystem(need, 0x1000);
        g_scratch_size = g_scratch_push ? need : 0;
    }
    SetDev(kDevPush, g_scratch_push);
    SetDev(kDevThreshold, g_scratch_push + g_scratch_size - 0x10000);
    g_walk = g_scratch_push;
    return g_scratch_push;
}

gpu::Surface SurfaceOf(uint32_t header);

// Bring-up self test of the renderer through its own interface, on the device's back buffer before the title draws:
// a clear read back, then a white pretransformed quad read back. The log says which step gives the wrong pixels.
void SelfTest(uint32_t w, uint32_t h) {
    gpu::State s;
    s.rt = SurfaceOf(g_buffers[1]);
    s.viewport = gpu::Viewport(0, 0, w, h, 0.0f, 1.0f);
    uint32_t pitch = (w * 4 + 63) & ~63u;
    uint32_t data = s.rt.data;
    // 1: clear to red, written back to guest memory
    g_backend->Clear(s, 0, NULL, gpu::kClearTarget, 0xFFFF0000, 1.0f, 0);
    g_backend->Sync(data, 0);
    LOGI("selftest 1 clear: pixel %08X (want FFFF0000)", R32(data + 10 * pitch + 10 * 4));
    // 2: a white quad from (100,100) to (300,300), FVF XYZRHW | DIFFUSE, no depth, colour writes on
    uint32_t vbdata = mem::AllocSystem(4 * 20, 0x1000), vb = mem::AllocSystem(20);
    const float xy[4][2] = {{100, 100}, {300, 100}, {300, 300}, {100, 300}};
    for (uint32_t i = 0; i < 4; i++) {
        WF32(vbdata + i * 20 + 0, xy[i][0]);
        WF32(vbdata + i * 20 + 4, xy[i][1]);
        WF32(vbdata + i * 20 + 8, 0.5f);
        WF32(vbdata + i * 20 + 12, 1.0f);
        W32(vbdata + i * 20 + 16, 0xFFFFFFFF);
    }
    W32(vb, 1);
    W32(vb + 4, mem::PhysicalFromVirtual(vbdata));
    s.vs_handle = 0x44;
    s.streams[0] = gpu::State::Stream(vb, 20);
    s.simple[gpu::kMColorWriteEnable >> 2] = 0xF;
    s.tss[0][gpu::kTssColorOp] = 1;  // D3DTOP_DISABLE: the diffuse colour as it is
    g_backend->Draw(s, 8, 0, 4, NULL);
    g_backend->Sync(data, 0);
    LOGI("selftest 2 draw: inside %08X (want FFFFFFFF), outside %08X (want FFFF0000)", R32(data + 200 * pitch + 200 * 4),
         R32(data + 10 * pitch + 10 * 4));
    // 3: the same quad through a vertex program, as the title draws: "mov oPos, v0; mov oD0, v3", the vertices blue
    //    microcode fields as gpu/shaders.cpp reads them: word 1 mac op at 21, input at 9, A swizzle xyzw (0x1B);
    //    word 2 A from an input (mux 2 at 26); word 3 output mask at 12, output write at 11, output register at 3,
    //    last instruction at bit 0
    g_backend->Clear(s, 0, NULL, gpu::kClearTarget, 0xFF000000, 1.0f, 0);
    for (uint32_t i = 0; i < 4; i++) W32(vbdata + i * 20 + 16, 0xFF0000FF);
    std::shared_ptr<gpu::VertexShader> prog = std::make_shared<gpu::VertexShader>();
    const uint32_t code[8] = {0, 1u << 21 | 0u << 9 | 0x1B, 2u << 26, 0xFu << 12 | 1u << 11 | 0u << 3,
                              0, 1u << 21 | 3u << 9 | 0x1B, 2u << 26, 0xFu << 12 | 1u << 11 | 3u << 3 | 1u};
    prog->program.assign(code, code + 8);
    prog->program_type = 0x2078;
    prog->inputs[0] = gpu::VertexShader::Input(0, 0, 0x42);
    prog->inputs[3] = gpu::VertexShader::Input(0, 16, 0x40);
    prog->stream_stride[0] = 20;
    gpu::State p = s;
    p.vs_handle = 0x11;
    p.vs = prog;
    g_backend->Draw(p, 8, 0, 4, NULL);
    g_backend->Sync(data, 0);
    LOGI("selftest 3 vertex program: inside %08X (want FF0000FF), outside %08X (want FF000000)",
         R32(data + 200 * pitch + 200 * 4), R32(data + 10 * pitch + 10 * 4));
    // 4: a textured quad, fixed function: a 4x4 swizzled A8R8G8B8 texture of green, FVF XYZRHW | DIFFUSE | TEX1
    g_backend->Clear(s, 0, NULL, gpu::kClearTarget, 0xFF000000, 1.0f, 0);
    uint32_t tvb = mem::AllocSystem(4 * 28, 0x1000), tex = mem::AllocSystem(20), texels = mem::AllocSystem(64, 0x1000);
    const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (uint32_t i = 0; i < 4; i++) {
        WF32(tvb + i * 28 + 0, xy[i][0]);
        WF32(tvb + i * 28 + 4, xy[i][1]);
        WF32(tvb + i * 28 + 8, 0.5f);
        WF32(tvb + i * 28 + 12, 1.0f);
        W32(tvb + i * 28 + 16, 0xFFFFFFFF);
        WF32(tvb + i * 28 + 20, uv[i][0]);
        WF32(tvb + i * 28 + 24, uv[i][1]);
    }
    for (uint32_t i = 0; i < 16; i++) W32(texels + i * 4, 0xFF00FF00);
    W32(vb + 4, mem::PhysicalFromVirtual(tvb));
    W32(tex, 0x00040001);  // a texture, one reference
    W32(tex + 4, mem::PhysicalFromVirtual(texels));
    W32(tex + 8, 0);
    W32(tex + 12, 0x00010021 | 0x06 << 8 | 2 << 20 | 2 << 24);  // A8R8G8B8, 2D, one level, 4x4
    W32(tex + 16, 0);
    gpu::State t = s;
    t.vs_handle = 0x144;
    t.streams[0] = gpu::State::Stream(vb, 28);
    t.textures[0] = tex;
    t.tss[0][gpu::kTssColorOp] = 2;  // SELECTARG1
    t.tss[0][gpu::kTssColorArg1] = 2;  // TEXTURE
    t.tss[0][gpu::kTssAlphaOp] = 2;
    t.tss[0][gpu::kTssAlphaArg1] = 2;
    g_backend->Draw(t, 8, 0, 4, NULL);
    g_backend->Sync(data, 0);
    LOGI("selftest 4 texture: inside %08X (want FF00FF00), outside %08X (want FF000000)",
         R32(data + 200 * pitch + 200 * 4), R32(data + 10 * pitch + 10 * 4));
    // 5: selftest 2's white quad with the device's depth buffer: cleared to 1.0, LESSEQUAL (GL 0x203), writes on
    if (g_depth) {
        gpu::State d = s;
        d.zs = SurfaceOf(g_depth);
        d.vs_handle = 0x44;
        d.streams[0] = gpu::State::Stream(vb, 20);
        W32(vb + 4, mem::PhysicalFromVirtual(vbdata));
        for (uint32_t i = 0; i < 4; i++) W32(vbdata + i * 20 + 16, 0xFFFFFFFF);
        d.simple[gpu::kMDepthTestEnable >> 2] = 1;
        d.simple[gpu::kMZFunc >> 2] = 0x203;
        d.simple[gpu::kMZWriteEnable >> 2] = 1;
        d.rs[gpu::kRsZEnable] = 1;
        g_backend->Clear(d, 0, NULL, gpu::kClearTarget | gpu::kClearZ | gpu::kClearStencil, 0xFF000000, 1.0f, 0);
        g_backend->Draw(d, 8, 0, 4, NULL);
        g_backend->Sync(data, 0);
        LOGI("selftest 5 depth: inside %08X (want FFFFFFFF), outside %08X (want FF000000)", R32(data + 200 * pitch + 200 * 4),
             R32(data + 10 * pitch + 10 * 4));
    }
    // the title's own clear follows; the memory goes back to black meanwhile
    std::memset(HostPtr(data), 0, pitch * h);
    gpu::MarkDirty(g_buffers[1]);
}

gpu::Surface SurfaceOf(uint32_t header) {
    gpu::Surface s;
    s.header = header;
    if (header) {
        s.data = R32(header + 4);
        s.format = R32(header + 12);
        s.size = R32(header + 16);
    }
    return s;
}

uint32_t NewSurface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t bpp) {
    uint32_t pitch = (w * bpp + 63) & ~63u;
    uint32_t data = mem::AllocContiguous(pitch * h, 0, 0xFFFFFFFF, 0x1000);
    uint32_t hdr = mem::AllocSystem(24);
    W32(hdr + 0, 0x000D0001);  // surface, one reference, one internal reference held by the device
    W32(hdr + 4, mem::PhysicalFromVirtual(data));
    W32(hdr + 8, 0);
    W32(hdr + 12, 0x00010021 | (fmt << 8));  // dma A, 2D, one level
    W32(hdr + 16, (w - 1) | (h - 1) << 12 | (pitch / 64 - 1) << 24);
    W32(hdr + 20, 0);
    return hdr;
}

// gpu::MarkDirty from the title's threads: to the renderer in order with the draws recorded before it
void DirtyToBackend(uint32_t data) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->Dirty(data);
    else gpu::MarkDirtyData(data);
}

void SyncDeferred() {
    // deferred states live in the title's memory, the inline SetRenderState and SetTextureStageState write them
    // on every draw: compared as they are read, and marked for the render thread only when one changed
    // JSRF: os mesmos bytes do ultimo desenho, comparados sem inverter. Na thread do jogo este laco rodava mil vezes por
    // quadro. Por quadro a comparacao completa volta uma vez, porque outras funcoes tambem escrevem em g_state.
    static uint64_t raw_rs[(gpu::kRsComplexFirst - gpu::kRsDeferredFirst) / 2 + 1];
    static uint64_t raw_ts[4 * kStageStates / 2 + 1];
    static uint32_t raw_swaps = 0xFFFFFFFFu;
    const uint32_t rs_bytes = (gpu::kRsComplexFirst - gpu::kRsDeferredFirst) * 4, ts_bytes = 4 * kStageStates * 4;
    const uint64_t* rs_now = reinterpret_cast<const uint64_t*>(HostPtr(kRenderState + gpu::kRsDeferredFirst * 4));
    const uint64_t* ts_now = reinterpret_cast<const uint64_t*>(HostPtr(kTextureState));
    uint64_t changed = raw_swaps != Dev(kDevSwapCount) ? 1 : 0;
    for (uint32_t k = 0; k < rs_bytes / 8; k++) changed |= rs_now[k] ^ raw_rs[k];
    for (uint32_t k = 0; k < ts_bytes / 8; k++) changed |= ts_now[k] ^ raw_ts[k];
    if (!changed) return;
    raw_swaps = Dev(kDevSwapCount);
    std::memcpy(raw_rs, rs_now, rs_bytes / 8 * 8);
    std::memcpy(raw_ts, ts_now, ts_bytes / 8 * 8);
    uint32_t diff = 0;
    for (uint32_t i = gpu::kRsDeferredFirst; i < gpu::kRsComplexFirst; i++) {
        const uint32_t v = R32(kRenderState + i * 4);
        if (g_state.rs[i] != v) {
            diff = 1;
            g_state.rs[i] = v;
        }
    }
    if (diff) g_state.Mark(&g_state.rs[gpu::kRsDeferredFirst], (gpu::kRsComplexFirst - gpu::kRsDeferredFirst) * 4);
    for (int s = 0; s < 4; s++) {
        diff = 0;
        for (uint32_t i = 0; i < gpu::kTssBumpEnvMat00; i++) {
            const uint32_t v = R32(kTextureState + (s * kStageStates + i) * 4);
            if (g_state.tss[s][i] != v) {
                diff = 1;
                g_state.tss[s][i] = v;
            }
        }
        if (diff) g_state.Mark(&g_state.tss[s][0], gpu::kTssBumpEnvMat00 * 4);
    }
}

// guest floats into the state as words: a word loaded into a float register goes through memory on the Xbox 360, a stall
// per float (load-hit-store), and nothing here does arithmetic on them
inline void LoadFloats(float* dst, uint32_t src, uint32_t n) {
    uint32_t* d = reinterpret_cast<uint32_t*>(dst);
    for (uint32_t k = 0; k < n; k++) d[k] = R32(src + k * 4);
}

// a field of g_state written: the render thread sends the 64 byte pieces it lies in (gpu::State::Mark)
#define MARK(field) g_state.Mark(&(field), sizeof(field))

float ZScale(const gpu::Surface& zs) {
    uint32_t f = zs.header ? (zs.format >> 8) & 0xFF : 0x2E;
    return f == 0x2C || f == 0x30 ? 65535.0f : 16777215.0f;
}

bool g_own_constants = false;  // D3DSCM_NORESERVEDCONSTANTS: c-38 and c-37 are the title's

// c-38 and c-37 hold the viewport scale and offset, as D3D_GetViewportOffsetAndScale computes them
void UpdateViewportConstants() {
    auto& v = g_state.viewport;
    g_state.zscale = ZScale(g_state.zs);
    MARK(g_state.viewport);
    MARK(g_state.zscale);
    MARK(g_state.ss_offset_x);
    MARK(g_state.ss_offset_y);
    float hw = v.w * 0.5f, hh = v.h * 0.5f;
    if (!g_own_constants) {
        float c58[4] = {hw, -hh, (v.max_z - v.min_z) * g_state.zscale, 0};
        float c59[4] = {v.x + g_state.ss_offset_x + hw, v.y + g_state.ss_offset_y + hh, v.min_z * g_state.zscale, 0};
        for (int k = 0; k < 4; k++) {
            g_state.vsc[58][k] = c58[k];
            g_state.vsc[59][k] = c59[k];
        }
        g_state.VscWritten(58, 2);
    }
    // the viewport as the library's SetViewport stores it, for inlined readers
    if (XDK_CDevice_Viewport != xdk::kUnknown) {
        uint32_t p = kDevice + XDK_CDevice_Viewport;
        W32(p, v.x);
        W32(p + 4, v.y);
        W32(p + 8, v.w);
        W32(p + 12, v.h);
        WF32(p + 16, v.min_z);
        WF32(p + 20, v.max_z);
    }
}

void SetComplex(uint32_t rs, uint32_t v) {
    g_state.rs[rs] = v;
    MARK(g_state.rs[rs]);
    W32(kRenderState + rs * 4, v);
}

// The library's default render and texture stage states; its pushes land in the scratch buffer
void ApplyDefaultStates(X86Context& ctx) {
#if HAS_D3D_InitDefaultState
    ::kernel::CallGuest(ctx, SYM(D3D_InitDefaultState));
#else
    (void)ctx;
    LOG_FIRST(1, "d3d: setting standard Direct3D default states");
    g_state.simple[gpu::kMDepthTestEnable >> 2] = 1;
    g_state.simple[gpu::kMZWriteEnable >> 2] = 1;
    g_state.simple[gpu::kMZFunc >> 2] = 4;
    g_state.simple[gpu::kMColorWriteEnable >> 2] = 0xF;
    g_state.rs[gpu::kRsZEnable] = 1;
    MARK(g_state.simple);
    MARK(g_state.rs);
#endif
}

const bool kVBlank = XDK_CDevice_VBlankCount != xdk::kUnknown && XDK_CDevice_VBlankEvent != xdk::kUnknown &&
                         XDK_CDevice_VBlankCallback != xdk::kUnknown;

// what the library's vertical blank interrupt leaves for the title: count, event, callback
void VerticalBlank(X86Context& ctx) {
    uint32_t info = mem::AllocSystem(12), shown = 0;
    int64_t next = host::NowNs();
    for (;;) {
        next = std::max<int64_t>(next + 16666667, host::NowNs());
        host::SleepNs(next - host::NowNs());
        uint32_t count = Dev(XDK_CDevice_VBlankCount) + 1, swaps = Dev(kDevSwapCount);
        SetDev(XDK_CDevice_VBlankCount, count);
        ObjectRef event = FromPointer(kDevice + XDK_CDevice_VBlankEvent);
        SetEvent(*event);
        if (uint32_t callback = Dev(XDK_CDevice_VBlankCallback)) {
            // blank count, swaps shown, 1 when this blank showed one
            W32(info, count);
            W32(info + 4, swaps);
            W32(info + 8, swaps != shown);
            CallGuest(ctx, callback, info);
        }
        shown = swaps;
        // the title clears the event in its own memory before a wait, which the host object does not see
        host::SleepNs(1000000);
        ResetEvent(*event);
    }
}

}  // namespace

gpu::State& State() { return g_state; }
std::recursive_mutex& Lock() { return g_lock; }
void MoviePresent() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend && g_buffers[1]) g_backend->Present(SurfaceOf(g_buffers[1]));
}

}  // namespace d3d

using namespace d3d;

// device

HLE_STDCALL(Direct3D_CreateDevice, 6) {
    XDK_REQUIRE("d3d", XDK_DATA(D3D_Device), XDK_DATA(D3D_pDevice), XDK_DATA(D3D_DirtyFlags),
                XDK_DATA(D3D_DeferredTextureState), XDK_DATA(D3D_RenderState), XDK_DATA(D3D_Streams),
                XDK_DATA(D3D_SimpleRenderStateEncode));
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Push), XDK_KEY(CDevice_PushThreshold), XDK_KEY(CDevice_BaseVertex),
                XDK_KEY(CDevice_IndexBuffer), XDK_KEY(CDevice_ZScale), XDK_KEY(CDevice_SuperSampleX),
                XDK_KEY(CDevice_SuperSampleY), XDK_KEY(CDevice_Transforms), XDK_KEY(CDevice_Textures),
                XDK_KEY(CDevice_RenderTarget), XDK_KEY(CDevice_Depth), XDK_KEY(CDevice_BackBuffer),
                XDK_KEY(CDevice_FrontBuffer), XDK_KEY(CDevice_SwapCount), XDK_KEY(CDevice_Size),
                XDK_KEY(DirtyFlags_Texture), XDK_KEY(TextureState_Stage), XDK_KEY(Stream_Size),
                XDK_KEY(Stream_Stride), XDK_KEY(Stream_Buffer));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t pp = a[4];  // D3DPRESENT_PARAMETERS
    uint32_t w = R32(pp), h = R32(pp + 4), fmt = R32(pp + 8), count = std::max<uint32_t>(R32(pp + 12), 1);
    uint32_t auto_depth = R32(pp + 0x20), depth_fmt = R32(pp + 0x24), interval = R32(pp + 0x30);
    if (!w) w = 640;
    if (!h) h = 480;
    LOGI("d3d: CreateDevice %ux%u format %X buffers %u depth %u format %X interval %X flags %X", w, h, fmt, count,
         auto_depth, depth_fmt, interval, R32(pp + 0x28));
    std::memset(HostPtr(kDevice), 0, XDK_CDevice_Size);
    g_buffers.fill(0);
    W32(kDevicePtr, kDevice);
    ScratchPush();

    uint32_t color_fmt = gpu::LinearColorFormat(fmt);
    g_buffer_count = std::min<uint32_t>(count + 1, 3);
    for (uint32_t i = 0; i < g_buffer_count; i++) g_buffers[i] = NewSurface(w, h, color_fmt, 4);
    g_depth = 0;
    if (auto_depth) {
        uint32_t zf = gpu::LinearDepthFormat(depth_fmt);
        g_depth = NewSurface(w, h, zf, zf == 0x30 || zf == 0x31 ? 2 : 4);  // 0x30 LIN_D16, 0x31 LIN_F16
    }
    if (!g_backend) {
        // the renderer on a hardware thread of its own: the title's code and the drawing overlap
        g_backend = kRenderThread ? gpu::CreateThreadedBackend(gpu::CreateXenosBackend()) : gpu::CreateXenosBackend();
        gpu::SetDirtyHook(DirtyToBackend);
        if (!g_backend->Init(w, h)) LOGE("d3d: the Xenos renderer did not start");
        else SelfTest(w, h);
    }
    g_state.Reset();
    g_state.viewport = gpu::Viewport(0, 0, w, h, 0.0f, 1.0f);
    SetDev(kDevRenderTarget, g_buffers[1]);
    SetDev(kDevDepth, g_depth);
    SetDev(kDevBackBuffer, g_buffers[1]);
    SetDev(kDevFrontBuffer, g_buffers[1]);
    float one = 1.0f, zs = g_depth && (R32(g_depth + 12) >> 8 & 0xFF) == 0x30 ? 65535.0f : 16777215.0f;
    WF32(kDevice + kDevSuperX, one);
    WF32(kDevice + kDevSuperY, one);
    WF32(kDevice + kDevZScale, zs);
    // fields the library's init sets that inlined library code and the default states read
    if (XDK_CDevice_AutoDepth != xdk::kUnknown) SetDev(XDK_CDevice_AutoDepth, g_depth);
    if (XDK_CDevice_BackBufferScale != xdk::kUnknown) {
        WF32(kDevice + XDK_CDevice_BackBufferScale, one);
        WF32(kDevice + XDK_CDevice_BackBufferScale + 4, one);
    }
    // 60 Hz, widescreen
    if (XDK_CDevice_DisplayFlags != xdk::kUnknown) SetDev(XDK_CDevice_DisplayFlags, 0x400000 | (app::Widescreen() ? 0x10000 : 0));
#if HAS_D3D_FvfShader
    if (XDK_CDevice_VertexShader != xdk::kUnknown) SetDev(XDK_CDevice_VertexShader, SYM(D3D_FvfShader));
#endif
    if (kVBlank) {
        // notification event, signalled
        SetDev(XDK_CDevice_VBlankEvent, 0x00040000);
        SetDev(XDK_CDevice_VBlankEvent + 4, 1);
        static bool started = false;
        if (!started) StartHostThread("vertical blank", VerticalBlank);
        started = true;
    }
    g_state.rt = SurfaceOf(g_buffers[1]);
    g_state.zs = SurfaceOf(g_depth);
    UpdateViewportConstants();
    ApplyDefaultStates(ctx);
    if (a[5]) W32(a[5], kDevice);
    return 0;
}

HLE_STDCALL(D3DDevice_Release, 0) { return 1; }

// synchronisation, the renderer never runs behind

HLE_STDCALL(D3D_BlockOnResource, 1) {
    uint32_t res = a[0];
    if (res && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        uint32_t type = R32(res) & 0x70000;  // 0x40000 texture, 0x50000 surface
        if (type == 0x40000 || type == 0x50000) {
            uint32_t data = mem::VirtualFromPhysical(R32(res + 4));
            g_backend->Sync(data, 0);
        } else {
            g_backend->WaitResource(res);
        }
    }
    gpu::MarkDirty(res);
    return 0;
}
// lock(resource, face, level, locked, rect, flags): D3DLOCK_NOOVERWRITE skips the block above, yet a
// title may read what the GPU drew
HLE_WRAP(Lock2DSurface, 6) {
    uint32_t res = a[0];
    if ((a[5] & 0x20) && res && g_backend) {  // D3DLOCK_NOOVERWRITE
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->SyncLate(mem::VirtualFromPhysical(R32(res + 4)));
    }
}
HLE_STDCALL(D3DResource_BlockUntilNotBusy, 1) {
    if (a[0] && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->WaitResource(a[0]);
    }
    gpu::MarkDirty(a[0]);
    return 0;
}
HLE_STDCALL(D3DResource_IsBusy, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t res = a[0];
    if (!res) return 0;
    // set on the device, a surface also through its texture
    uint32_t common = R32(res), parent = (common & 0x70000) == 0x50000 ? R32(res + 0x14) : 0;
    if ((common & 0x780000) || (parent && (R32(parent) & 0x780000))) return 1;  // internal reference count
    return g_backend && g_backend->Busy(res) ? 1 : 0;
}
HLE_STDCALL(D3D_BlockOnTime, 2) { return 0; }
HLE_STDCALL(D3DDevice_BlockOnFence, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->WaitFence(a[0]);
    return 0;
}
HLE_STDCALL(D3DDevice_InsertFence, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3DDevice_IsFencePending, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend && g_backend->FencePending(a[0]) ? 1 : 0;
}
HLE_STDCALL(D3D_SetFence, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3D_KickOffAndWaitForIdle, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->Flush();
    return 0;
}
HLE_FASTCALL(CDevice_KickOff, 0) { return 0; }

HLE_STDCALL(D3DDevice_MakeSpace, 0) {
    LOG_FIRST(16, "d3d: MakeSpace from %08X, some caller still pushes hardware commands", R32(ctx.esp));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return ScratchPush();
}
HLE_STDCALL(D3D_MakeRequestedSpace, 2) {
    LOG_FIRST(16, "d3d: MakeRequestedSpace from %08X", R32(ctx.esp));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return ScratchPush();
}

// JSRF: duas threads do CRI (0x13B1C0, 0x13B230) tem aqui seu relogio de 60 Hz. Retornar na hora as deixava em laco.
HLE_STDCALL(D3DDevice_BlockUntilVerticalBlank, 0) {
    const int64_t kBlank = 1000000000ll / 60;
    int64_t now = host::NowNs();
    GuestUnlock unlocked;
    host::SleepNs((now / kBlank + 1) * kBlank - now);
    return 0;
}
HLE_STDCALL(D3DDevice_SetFlickerFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_SetSoftDisplayFilter, 1) { return 0; }
static uint32_t g_overlay_surface = 0;
static bool g_overlay_enabled = false;
static int64_t g_last_swap_time = 0;

static inline int ClampInt(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void DrawOverlay(uint32_t s, uint32_t dst_hdr) {
    if (!s || !dst_hdr) return;
    uint32_t src_phys = R32(s + 4);
    uint32_t src_va = mem::VirtualFromPhysical(src_phys);
    gpu::TextureDesc sd, dd;
    uint32_t w = 640, h = 480, src_pitch = 1280;
    if (gpu::Describe(s, sd)) {
        w = sd.width;
        h = sd.height;
        src_pitch = sd.pitch;
    }
    uint32_t dst_w = 640, dst_h = 480, dst_pitch = 2560;
    if (gpu::Describe(dst_hdr, dd)) {
        dst_w = dd.width;
        dst_h = dd.height;
        dst_pitch = dd.pitch;
    }

    uint32_t dst_phys = R32(dst_hdr + 4);
    uint32_t dst_va = mem::VirtualFromPhysical(dst_phys);

    const uint8_t* src_bytes = static_cast<const uint8_t*>(HostPtr(src_va));
    uint8_t* dst_bytes = static_cast<uint8_t*>(HostPtr(dst_va));
    if (!src_bytes || !dst_bytes) {
        LOG_FIRST(10, "d3d: DrawOverlay null buffer src=%p dst=%p s=%08X", src_bytes, dst_bytes, s);
        return;
    }

    uint32_t copy_w = std::min(w, dst_w);
    uint32_t copy_h = std::min(h, dst_h);

    static uint32_t s_last_s = 0;
    static uint32_t s_last_dst = 0;
    static uint64_t s_last_sig = 0;

    uint64_t current_sig = *reinterpret_cast<const uint64_t*>(src_bytes);
    if (copy_h > 10) {
        current_sig ^= *reinterpret_cast<const uint64_t*>(src_bytes + (copy_h / 2) * src_pitch + (copy_w & ~7u));
    }

    if (s == s_last_s && dst_hdr == s_last_dst && current_sig == s_last_sig) {
        return;
    }
    s_last_s = s;
    s_last_dst = dst_hdr;
    s_last_sig = current_sig;

    for (uint32_t y = 0; y < copy_h; y++) {
        const uint8_t* srow = src_bytes + y * src_pitch;
        uint8_t* drow = dst_bytes + y * dst_pitch;
        for (uint32_t x = 0; x < copy_w; x += 2) {
            int Y0 = srow[x * 2 + 0];
            int U  = srow[x * 2 + 1];
            int Y1 = srow[x * 2 + 2];
            int V  = srow[x * 2 + 3];

            int C0 = Y0 - 16;
            int C1 = Y1 - 16;
            int D  = U - 128;
            int E  = V - 128;

            int r_uv = 409 * E + 128;
            int g_uv = -100 * D - 208 * E + 128;
            int b_uv = 516 * D + 128;

            // Pixel 0 (BGRA)
            drow[(x + 0) * 4 + 0] = uint8_t(ClampInt((298 * C0 + b_uv) >> 8, 0, 255));
            drow[(x + 0) * 4 + 1] = uint8_t(ClampInt((298 * C0 + g_uv) >> 8, 0, 255));
            drow[(x + 0) * 4 + 2] = uint8_t(ClampInt((298 * C0 + r_uv) >> 8, 0, 255));
            drow[(x + 0) * 4 + 3] = 0xFF;

            // Pixel 1 (BGRA)
            if (x + 1 < copy_w) {
                drow[(x + 1) * 4 + 0] = uint8_t(ClampInt((298 * C1 + b_uv) >> 8, 0, 255));
                drow[(x + 1) * 4 + 1] = uint8_t(ClampInt((298 * C1 + g_uv) >> 8, 0, 255));
                drow[(x + 1) * 4 + 2] = uint8_t(ClampInt((298 * C1 + r_uv) >> 8, 0, 255));
                drow[(x + 1) * 4 + 3] = 0xFF;
            }
        }
    }
}

static void SyncOverlayBuffers() {
    if (!g_buffers[1]) return;
    uint32_t src_phys = R32(g_buffers[1] + 4);
    if (!src_phys) return;
    const uint8_t* psrc = static_cast<const uint8_t*>(HostPtr(mem::VirtualFromPhysical(src_phys)));
    gpu::TextureDesc dd;
    if (!psrc || !gpu::Describe(g_buffers[1], dd)) return;
    size_t bytes = size_t(dd.pitch) * dd.height;
    for (uint32_t i = 0; i < g_buffer_count; i++) {
        if (i == 1 || !g_buffers[i]) continue;
        uint32_t dst_phys = R32(g_buffers[i] + 4);
        if (!dst_phys || dst_phys == src_phys) continue;
        uint8_t* pdst = static_cast<uint8_t*>(HostPtr(mem::VirtualFromPhysical(dst_phys)));
        if (pdst) {
            std::memcpy(pdst, psrc, bytes);
            gpu::MarkDirty(g_buffers[i]);
        }
    }
}

static void PaceMovie30() {
    static int64_t next = host::NowNs();
    int64_t now_t = host::NowNs(), period = 1000000000ll / 30;
    next += period;
    if (next < now_t - period || next > now_t + period * 2) next = now_t;
    host::SleepNs(next - host::NowNs());
}

HLE_STDCALL(D3DDevice_EnableOverlay, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_overlay_enabled = (a[0] != 0);
    if (!g_overlay_enabled) g_overlay_surface = 0;
    LOGI("d3d: EnableOverlay %u", a[0]);
    return 0;
}
HLE_STDCALL(D3DDevice_UpdateOverlay, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_overlay_surface = a[0];
    g_overlay_enabled = (a[0] != 0);
    if (a[0] && g_backend && g_buffers[1]) {
        DrawOverlay(a[0], g_buffers[1]);
        gpu::MarkDirty(g_buffers[1]);
        SyncOverlayBuffers();
        if (host::NowNs() - g_last_swap_time > 100000000ll) {
            g_backend->Present(SurfaceOf(g_buffers[1]));
            PaceMovie30();
        }
    }
    return 0;
}
// a kept display only matters to a relaunched instance
HLE_STDCALL(D3DDevice_PersistDisplay, 0) { return 0; }
HLE_STDCALL(D3DDevice_FlushVertexCache, 0) { return 0; }

HLE_STDCALL(D3DDevice_SetGammaRamp, 2) {
    uint8_t ramp[768];
    std::memcpy(ramp, HostPtr(a[1]), 768);
    if (g_backend) g_backend->SetGamma(ramp);
    return 0;
}

// targets and presentation

HLE_STDCALL(D3DDevice_SetRenderTarget, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t rt = a[0] ? a[0] : Dev(kDevRenderTarget), zs = a[1];
    AddRef(ctx, rt);
    uint32_t old_rt = Dev(kDevRenderTarget), old_zs = Dev(kDevDepth);
    SetDev(kDevRenderTarget, rt);
    SetDev(kDevDepth, zs);
    AddRef(ctx, zs);
    Release(ctx, old_rt);
    Release(ctx, old_zs);
    g_state.rt = SurfaceOf(rt);
    g_state.zs = SurfaceOf(zs);
    // SetRenderTarget resets the viewport to the whole target
    uint32_t w, h;
    gpu::SurfaceSize(g_state.rt, w, h);
    g_state.viewport = gpu::Viewport(0, 0, w, h, 0.0f, 1.0f);
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_GetBackBuffer2, 1) {
    int32_t i = int32_t(a[0]);
    uint32_t s = i < 0 ? g_buffers[0] : g_buffers[std::min<uint32_t>(uint32_t(i) + 1, g_buffer_count - 1)];
    W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_GetRenderTarget2, 0) {
    uint32_t s = Dev(kDevRenderTarget);
    if (s) W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_GetDepthStencilSurface2, 0) {
    uint32_t s = Dev(kDevDepth);
    if (s) W32(s, R32(s) + 1);
    return s;
}

namespace {
std::atomic<uint32_t> g_presents(0);
}  // namespace

HLE_STDCALL(D3DDevice_Swap, 1) {
    g_last_swap_time = host::NowNs();
    uint32_t n;
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_presents++;
        if (g_presents <= 10 || (g_presents % 60) == 0) {
            LOGI("d3d: presenting frame %u, push=%08X walk=%08X", unsigned(g_presents.load()), Dev(kDevPush), g_walk);
        }
        // the kept display is shown until the title presents its own
        kernel::SetSavedDataAddress(0);
        // the ramp an inlined SetGammaRamp left for the vertical blank that shows this swap
        if (g_backend && XDK_CDevice_GammaPending != xdk::kUnknown && XDK_CDevice_GammaRamps != xdk::kUnknown) {
            uint32_t i = Dev(kDevSwapCount) & 1;
            if (Dev(XDK_CDevice_GammaPending + i * 4) == 1) {
                SetDev(XDK_CDevice_GammaPending + i * 4, 0);
                g_backend->SetGamma(static_cast<const uint8_t*>(HostPtr(kDevice + XDK_CDevice_GammaRamps + i * 0x300)));
            }
        }
        Walk(Dev(kDevPush));  // reports asked for at the end of the frame
        if (g_overlay_enabled && g_overlay_surface) {
            DrawOverlay(g_overlay_surface, g_buffers[1]);
            gpu::MarkDirty(g_buffers[1]);
        }
        if (g_backend) g_backend->Present(SurfaceOf(g_buffers[1]));

        // like the XDK, the surfaces stay put and their memory rotates: titles keep back buffer pointers and copy its data
        uint32_t last = g_buffers[g_buffer_count - 1], data = R32(last + 4), lk = R32(last + 8);
        for (uint32_t i = g_buffer_count - 1; i > 0; i--) {
            W32(g_buffers[i] + 4, R32(g_buffers[i - 1] + 4));
            W32(g_buffers[i] + 8, R32(g_buffers[i - 1] + 8));
        }
        W32(g_buffers[0] + 4, data);
        W32(g_buffers[0] + 8, lk);
        g_state.rt = SurfaceOf(Dev(kDevRenderTarget));
        n = Dev(kDevSwapCount) + 1;
        SetDev(kDevSwapCount, n);
    }
    if (g_overlay_enabled) PaceMovie30();  // otherwise the renderer's vertical sync paces
    return n;
}

HLE_STDCALL(D3DDevice_Clear, 6) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const uint8_t* rects = a[0] && a[1] ? static_cast<const uint8_t*>(HostPtr(a[1])) : nullptr;
    static uint32_t clear_count = 0;
    if (++clear_count <= 20) {
        LOGI("d3d: Clear count=%u flags=%X color=%08X caller=%08X", a[0], a[2], a[3], R32(ctx.esp));
    }
    uint32_t flags = a[2];
    if (g_overlay_enabled) flags &= ~0xF0u;
    if (flags && g_backend) g_backend->Clear(g_state, a[0], rects, flags, a[3], ArgF(a, 4), a[5]);
    return 0;
}

HLE_STDCALL(D3DDevice_CopyRects, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->Copy(SurfaceOf(a[0]), SurfaceOf(a[3]), a[2], a[1], a[4]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetViewport, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t v = a[0];
    g_state.viewport = gpu::Viewport(R32(v), R32(v + 4), R32(v + 8), R32(v + 12), GF(v + 16), GF(v + 20));
    g_state.scissor_count = 0;
    MARK(g_state.scissor_count);
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_SetScissors, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.scissor_count = std::min<uint32_t>(a[0], 8);
    for (uint32_t i = 0; i < g_state.scissor_count * 4; i++) g_state.scissors[i] = R32(a[2] + i * 4);
    MARK(g_state.scissor_count);
    MARK(g_state.scissors);
    return 0;
}

HLE_STDCALL(D3DDevice_SetScreenSpaceOffset, 2) {
    g_state.ss_offset_x = ArgF(a, 0);
    g_state.ss_offset_y = ArgF(a, 1);
    UpdateViewportConstants();
    return 0;
}

// render states

HLE_FASTCALL(D3DDevice_SetRenderState_Simple, 0) {
    uint32_t method = a[0] & 0x1FFC, value = a[1];
    g_state.simple[method >> 2] = value;
    MARK(g_state.simple[method >> 2]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetRenderStateNotInline, 2) {
    uint32_t rs = a[0], v = a[1];
    if (rs == 7) { // D3DRS_ZENABLE
        SetComplex(gpu::kRsZEnable, v);
        g_state.simple[gpu::kMDepthTestEnable >> 2] = v;
        MARK(g_state.simple[gpu::kMDepthTestEnable >> 2]);
    } else if (rs == 14) { // D3DRS_ZWRITEENABLE
        g_state.simple[gpu::kMZWriteEnable >> 2] = v;
        MARK(g_state.simple[gpu::kMZWriteEnable >> 2]);
    } else if (rs == 23) { // D3DRS_ZFUNC
        g_state.simple[gpu::kMZFunc >> 2] = v;
        MARK(g_state.simple[gpu::kMZFunc >> 2]);
    } else if (rs == 52) { // D3DRS_STENCILENABLE
        SetComplex(gpu::kRsStencilEnable, v);
        g_state.simple[gpu::kMStencilTestEnable >> 2] = v;
        MARK(g_state.simple[gpu::kMStencilTestEnable >> 2]);
    }
    if (rs < gpu::kRsDeferredFirst) {
        uint32_t method = R32(kSimpleEncode + rs * 4) & 0x1FFC;
        g_state.simple[method >> 2] = v;
        MARK(g_state.simple[method >> 2]);
        W32(kRenderState + rs * 4, v);  // titles read their states back from the library's table
    } else if (rs < gpu::kRsMax) {
        SetComplex(rs, v);
    }
    return 0;
}

#define COMPLEX_STATE(name, rs)                       \
    HLE_STDCALL(D3DDevice_SetRenderState_##name, 1) { \
        SetComplex(rs, a[0]);                         \
        return 0;                                     \
    }
COMPLEX_STATE(PSTextureModes, gpu::kRsPsTextureModes)
COMPLEX_STATE(VertexBlend, gpu::kRsVertexBlend)
COMPLEX_STATE(FogColor, gpu::kRsFogColor)
COMPLEX_STATE(FillMode, gpu::kRsFillMode)
COMPLEX_STATE(BackFillMode, gpu::kRsBackFillMode)
COMPLEX_STATE(TwoSidedLighting, gpu::kRsTwoSidedLighting)
COMPLEX_STATE(NormalizeNormals, gpu::kRsNormalizeNormals)
COMPLEX_STATE(ZEnable, gpu::kRsZEnable)
COMPLEX_STATE(StencilEnable, gpu::kRsStencilEnable)
COMPLEX_STATE(StencilFail, gpu::kRsStencilFail)
COMPLEX_STATE(FrontFace, gpu::kRsFrontFace)
COMPLEX_STATE(CullMode, gpu::kRsCullMode)
COMPLEX_STATE(TextureFactor, gpu::kRsTextureFactor)
COMPLEX_STATE(LogicOp, gpu::kRsLogicOp)
COMPLEX_STATE(EdgeAntiAlias, gpu::kRsEdgeAntiAlias)
COMPLEX_STATE(MultiSampleAntiAlias, gpu::kRsMultiSampleAntiAlias)
COMPLEX_STATE(MultiSampleMask, gpu::kRsMultiSampleMask)
COMPLEX_STATE(MultiSampleMode, gpu::kRsMultiSampleMode)
COMPLEX_STATE(MultiSampleRenderTargetMode, gpu::kRsMultiSampleRenderTargetMode)
COMPLEX_STATE(ShadowFunc, gpu::kRsShadowFunc)
COMPLEX_STATE(LineWidth, gpu::kRsLineWidth)
COMPLEX_STATE(SampleAlpha, gpu::kRsSampleAlpha)
COMPLEX_STATE(Dxt1NoiseEnable, gpu::kRsDxt1NoiseEnable)
COMPLEX_STATE(YuvEnable, gpu::kRsYuvEnable)
COMPLEX_STATE(OcclusionCullEnable, gpu::kRsOcclusionCullEnable)
COMPLEX_STATE(StencilCullEnable, gpu::kRsStencilCullEnable)
COMPLEX_STATE(RopZCmpAlwaysRead, gpu::kRsRopZCmpAlwaysRead)
COMPLEX_STATE(RopZRead, gpu::kRsRopZRead)
COMPLEX_STATE(DoNotCullUncompressed, gpu::kRsDoNotCullUncompressed)

HLE_STDCALL(D3DDevice_SetTextureState_TexCoordIndex, 2) {
    g_state.tss[a[0] & 3][gpu::kTssTexCoordIndex] = a[1];
    MARK(g_state.tss[a[0] & 3][gpu::kTssTexCoordIndex]);
    W32(kTextureState + ((a[0] & 3) * kStageStates + gpu::kTssTexCoordIndex) * 4, a[1]);
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BumpEnv, 3) {
    if (a[1] >= gpu::kTssBumpEnvMat00 && a[1] <= gpu::kTssBumpEnvLOffset) {
        g_state.tss[a[0] & 3][a[1]] = a[2];
        MARK(g_state.tss[a[0] & 3][a[1]]);
    }
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BorderColor, 2) {
    g_state.tss[a[0] & 3][gpu::kTssBorderColor] = a[1];
    MARK(g_state.tss[a[0] & 3][gpu::kTssBorderColor]);
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_ColorKeyColor, 2) {
    g_state.tss[a[0] & 3][gpu::kTssColorKeyColor] = a[1];
    MARK(g_state.tss[a[0] & 3][gpu::kTssColorKeyColor]);
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureStageStateNotInline, 3) {
    uint32_t stage = a[0] & 3, type = a[1], v = a[2];
    if (type < gpu::kTssCount) {
        g_state.tss[stage][type] = v;
        MARK(g_state.tss[stage][type]);
        W32(kTextureState + (stage * kStageStates + type) * 4, v);
    }
    return 0;
}

// the XDK's transform types: view 0, projection 1, texture 2-5, world 6-9; the renderer keeps Direct3D 8's numbers
// (view 2, projection 3, texture 16, world 256). The device copy is what GetTransform reads.
HLE_STDCALL(D3DDevice_SetTransform, 2) {
    uint32_t t = a[0];
    if (t >= 10) return 0;
    std::memcpy(HostPtr(kDevice + kDevTransforms + t * 64), HostPtr(a[1]), 64);
    uint32_t pc = t < 2 ? t + 2 : t < 6 ? t + 14 : t + 250;
    LoadFloats(&g_state.transforms[pc][0], a[1], 16);
    MARK(g_state.transforms[pc]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetMaterial, 1) {
    LoadFloats(&g_state.material[0], a[0], 17);
    MARK(g_state.material);
    return 0;
}

// the XDK keeps its own light lists, the renderer gets a copy
HLE_WRAP(D3DDevice_SetLight, 2) {
    uint32_t i = a[0], l = a[1];
    if (i < 8 && l) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        for (uint32_t k = 0; k < 26; k++) g_state.lights[i][k] = R32(l + k * 4);
        MARK(g_state.lights[i]);
    }
}
HLE_WRAP(D3DDevice_LightEnable, 2) {
    uint32_t i = a[0], on = a[1];
    if (i < 8) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_state.light_enable = on ? g_state.light_enable | 1u << i : g_state.light_enable & ~(1u << i);
        MARK(g_state.light_enable);
    }
}

// resources in use

HLE_STDCALL(D3DDevice_SetTexture, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t stage = a[0] & 3, tex = a[1];
    uint32_t old = Dev(kDevTextures + stage * 4);
    if (tex != old) AddRef(ctx, tex);
    SetDev(kDevTextures + stage * 4, tex);
    if (tex != old) Release(ctx, old);
    g_state.textures[stage] = tex;
    W32(kDirtyFlags, R32(kDirtyFlags) | XDK_DirtyFlags_Texture);
    return 0;
}

// SetPalette(stage, palette): the colours a P8 texture of the stage looks up
HLE_STDCALL(D3DDevice_SetPalette, 2) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Palettes));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t stage = a[0] & 3, pal = a[1];
    uint32_t old = Dev(kDevPalettes + stage * 4);
    AddRef(ctx, pal);
    SetDev(kDevPalettes + stage * 4, pal);
    Release(ctx, old);
    g_state.palettes[stage] = pal;
    return 0;
}

// SwitchTexture(method, data, format), fastcall: a swizzled texture of the bound one's shape replaces it. It is
// bound through a header of the stage's own holding the bound texture's fields with the new data and format.
HLE_FASTCALL(D3DDevice_SwitchTexture, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    static uint32_t headers[4];
    // texture methods from 0x1B00, 0x40 per stage
    uint32_t stage = (((a[0] & 0x1FFC) - 0x1B00) >> 6) & 3, cur = g_state.textures[stage];
    uint32_t& h = headers[stage];
    if (!h) h = mem::AllocSystem(20);
    if (cur && cur != h) std::memcpy(HostPtr(h), HostPtr(cur), 20);
    W32(h + 4, a[1]);
    W32(h + 12, a[2]);
    g_state.textures[stage] = h;
    return 0;
}

HLE_STDCALL(D3DDevice_SetStreamSource, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t n = a[0] & 15, vb = a[1], stride = a[2];
    uint32_t old = R32(kStreams + n * kStreamSize + XDK_Stream_Buffer);
    // AddRef and Release run the title's own code: for the same buffer again they cancel out
    if (vb != old) AddRef(ctx, vb);
    W32(kStreams + n * kStreamSize + XDK_Stream_Stride, stride);
    W32(kStreams + n * kStreamSize + XDK_Stream_Buffer, vb);
    if (vb != old) Release(ctx, old);
    g_state.streams[n] = gpu::State::Stream(vb, stride);
    return 0;
}

HLE_STDCALL(D3DDevice_SetIndices, 2) {
    XDK_REQUIRE("d3d", XDK_DATA(D3D_IndexData));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t ib = a[0];
    uint32_t old = Dev(kDevIndexBuffer);
    AddRef(ctx, ib);
    W32(kIndexData, ib ? R32(ib + 4) : 0);
    SetDev(kDevIndexBuffer, ib);
    SetDev(kDevBaseVertex, a[1]);
    Release(ctx, old);
    g_state.index_buffer = ib;
    g_state.index_data = ib ? R32(ib + 4) : 0;
    g_state.base_vertex = a[1];
    MARK(g_state.index_buffer);
    MARK(g_state.index_data);
    MARK(g_state.base_vertex);
    return 0;
}

// shaders

HLE_STDCALL(D3DDevice_CreateVertexShader, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t decl = a[0], func = a[1], out = a[2];
    auto vs = std::make_shared<gpu::VertexShader>();
    if (decl) {
        for (uint32_t i = 0; i < 1024; i++) {
            uint32_t t = R32(decl + i * 4);
            vs->decl.push_back(t);
            if (t == 0xFFFFFFFF) break;
        }
    }
    if (func) {
        uint32_t hdr = R32(func);
        vs->program_type = hdr & 0xFFFF;
        uint32_t n = hdr >> 16;
        for (uint32_t i = 0; i < n * 4; i++) vs->program.push_back(R32(func + 4 + i * 4));
    }
    gpu::ParseDeclaration(*vs);
    uint32_t rec = mem::AllocSystem(16, 16);
    std::memset(HostPtr(rec), 0, 16);
    vs->handle = rec | 1;
    g_vs[vs->handle] = vs;
    W32(out, vs->handle);
    LOG_FIRST(64, "d3d: vertex shader %08X, %u decl tokens, %u instructions, type %X", vs->handle, vs->decl.size(),
              vs->program.size() / 4, vs->program_type);
    return 0;
}

HLE_STDCALL(D3DDevice_DeleteVertexShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_vs.find(a[0]);
    if (it != g_vs.end()) {
        mem::FreeSystem(a[0] & ~1u);
        g_vs.erase(it);
    }
    return 0;
}

// (handle, data, size in and out): the program as it was given, the size alone without data or room
HLE_STDCALL(D3DDevice_GetVertexShaderFunction, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_vs.find(a[0]);
    uint32_t words = it == g_vs.end() ? 0 : uint32_t(it->second->program.size());
    uint32_t need = words ? 4 + words * 4 : 0, room = R32(a[2]);
    W32(a[2], need);
    if (need > room) return a[1] ? 0x88760867 : 0;  // D3DERR_MOREDATA
    if (!a[1] || !need) return 0;
    W32(a[1], it->second->program_type | words / 4 << 16);
    for (uint32_t i = 0; i < words; i++) W32(a[1] + 4 + i * 4, it->second->program[i]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetVertexShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t h = a[0];
    g_state.vs_handle = h;
    MARK(g_state.vs_handle);
    if (XDK_CDevice_VertexShader != xdk::kUnknown) {
        if (h & 1) SetDev(XDK_CDevice_VertexShader, h & ~1u);
#if HAS_D3D_FvfShader
        else SetDev(XDK_CDevice_VertexShader, SYM(D3D_FvfShader));
#endif
    }
    if (h & 1) {
        auto it = g_vs.find(h);
        if (it != g_vs.end()) {
            // the same program again keeps its shared_ptr (two atomic operations less); a handle made anew is another one
            if (g_state.vs.get() != it->second.get()) g_state.vs = it->second;
            // constants declared with the shader load when it is selected
            const std::vector<std::pair<uint32_t, gpu::VertexShader::Vec4> >& cs = it->second->constants;
            for (size_t k = 0; k < cs.size(); k++)
                if (cs[k].first < 192) {
                    g_state.vsc[cs[k].first] = cs[k].second;
                    g_state.VscWritten(cs[k].first, 1);
                }
            g_state.vsc_epoch++;
        } else {
            g_state.vs = nullptr;
            LOG_FIRST(8, "d3d: SetVertexShader with unknown handle %08X", h);
        }
    } else if (g_state.vs) {
        g_state.vs = nullptr;
    }
    return 0;
}

// four guest floats into a constant register
static void LoadVec4(std::array<float, 4>& dst, uint32_t src) {
    LoadFloats(&dst[0], src, 4);
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1Fast, 0) {
    uint32_t reg = a[0];
    if (reg < 192) {
        LoadVec4(g_state.vsc[reg], a[1]);
        g_state.VscWritten(reg, 1);
    }
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1, 0) {
    uint32_t reg = a[0];
    if (reg < 192) {
        LoadVec4(g_state.vsc[reg], a[1]);
        g_state.VscWritten(reg, 1);
    }
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant4, 0) {
    uint32_t reg = a[0], src = a[1];
    for (uint32_t i = 0; i < 4 && reg + i < 192; i++) LoadVec4(g_state.vsc[reg + i], src + i * 16);
    g_state.VscWritten(reg, 4);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstantNotInlineFast, 1) {
    uint32_t reg = a[0], src = a[1], dwords = a[2];
    for (uint32_t i = 0; i < dwords / 4 && reg + i < 192; i++) LoadVec4(g_state.vsc[reg + i], src + i * 16);
    g_state.VscWritten(reg, dwords / 4);
    return 0;
}

// JSRF, XDK 4134: SetVertexShaderConstant(Register, data, count), 0x1905F0. Interno = Register + 0x60 (0x190604); o
// original guarda em CDevice + 0xB98 + interno * 16 e envia pelo pushbuffer, que aqui ninguem le. Sem isto, nenhum
// shader programavel recebia matrizes: personagens e veiculos sumiam.
HLE_STDCALL(D3DDevice_SetVertexShaderConstant, 3) {
    int32_t reg = int32_t(a[0]) + 0x60;
    uint32_t src = a[1], n = a[2];
    if (reg < 0 || reg >= 192) return 0;
    if (uint32_t(reg) + n > 192) n = 192 - reg;
    for (uint32_t i = 0; i < n; i++) {
        LoadVec4(g_state.vsc[reg + i], src + i * 16);
        for (uint32_t w = 0; w < 4; w++) W32(kDevice + 0xB98 + (reg + i) * 16 + w * 4, R32(src + i * 16 + w * 4));
    }
    if (n) g_state.VscWritten(reg, n);
    return 0;
}

HLE_STDCALL(D3DDevice_LoadVertexShader, 2) {
    LOG_FIRST(4, "d3d: LoadVertexShader %08X at %u is not supported", a[0], a[1]);
    return 0;
}
HLE_STDCALL(D3DDevice_SelectVertexShader, 2) {
    LOG_FIRST(4, "d3d: SelectVertexShader %08X at %u is not supported", a[0], a[1]);
    return 0;
}
HLE_STDCALL(D3DDevice_SetShaderConstantMode, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_own_constants = a[0] & 0x10;  // D3DSCM_NORESERVEDCONSTANTS
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_CreatePixelShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    std::shared_ptr<gpu::PixelShader> ps = std::make_shared<gpu::PixelShader>();
    for (uint32_t k = 0; k < sizeof(ps->def) / 4; k++) ps->def[k] = R32(a[0] + k * 4);
    uint32_t rec = mem::AllocSystem(16, 16);
    ps->handle = rec;
    g_ps[rec] = ps;
    W32(a[1], rec);
    return 0;
}

HLE_STDCALL(D3DDevice_DeletePixelShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_ps.erase(a[0])) mem::FreeSystem(a[0]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.ps = nullptr;
    if (!a[0]) return 0;
    auto it = g_ps.find(a[0]);
    if (it != g_ps.end()) {
        g_state.ps = it->second;
    } else if (XDK_PixelShader_Definition != xdk::kUnknown) {
        // made by library code the title inlined: the object points at its definition, handles are reused
        static std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> made;
        uint32_t def_va = R32(a[0] + XDK_PixelShader_Definition);
        uint32_t def[0xF0 / 4];
        for (uint32_t k = 0; k < 0xF0 / 4; k++) def[k] = R32(def_va + k * 4);
        std::shared_ptr<gpu::PixelShader>& ps = made[a[0]];
        if (!ps || std::memcmp(ps->def, def, sizeof(ps->def))) {
            ps = std::make_shared<gpu::PixelShader>();
            ps->handle = a[0];
            std::memcpy(ps->def, def, sizeof(ps->def));
        }
        g_state.ps = ps;
    } else {
        LOG_FIRST(8, "d3d: SetPixelShader with unknown handle %08X", a[0]);
        return 0;
    }
    // the definition loads the combiner state like the XDK pushes it
    const uint32_t* d = g_state.ps->def;
#define put(method, v) (g_state.simple[(method) >> 2] = (v), MARK(g_state.simple[(method) >> 2]))
    for (int i = 0; i < 8; i++) {
        put(gpu::kMPsAlphaInputs + i * 4, d[0 + i]);
        put(gpu::kMPsConstant0 + i * 4, d[10 + i]);
        put(gpu::kMPsConstant1 + i * 4, d[18 + i]);
        put(gpu::kMPsAlphaOutputs + i * 4, d[26 + i]);
        put(gpu::kMPsRgbInputs + i * 4, d[34 + i]);
        put(gpu::kMPsRgbOutputs + i * 4, d[45 + i]);
    }
    put(gpu::kMPsFinalAbcd, d[8]);
    put(gpu::kMPsFinalEfgh, d[9]);
    put(gpu::kMPsCompareMode, d[42]);
    put(gpu::kMPsFinalConstant0, d[43]);
    put(gpu::kMPsFinalConstant1, d[44]);
    put(gpu::kMPsCombinerCount, d[53]);
    put(gpu::kMPsDotMapping, d[55]);
    put(gpu::kMPsInputTexture, d[56]);
    SetComplex(gpu::kRsPsTextureModes, d[54]);
#undef put
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShaderConstant, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t reg = a[0], src = a[1], n = a[2];
    for (uint32_t i = 0; i < n && reg + i < 16; i++) LoadVec4(g_state.psc[reg + i], src + i * 16);
    MARK(g_state.psc);
    gpu::ApplyPixelConstants(g_state, reg, n);
    g_state.Mark(&g_state.simple[gpu::kMPsConstant0 >> 2], gpu::kMPsAlphaOutputs - gpu::kMPsConstant0);
    g_state.Mark(&g_state.simple[gpu::kMPsFinalConstant0 >> 2], 8);
    return 0;
}

// drawing

// JSRF: o texto e o HUD gravam quads num mesmo buffer, desenham e gravam de novo desde o inicio, para a pagina seguinte da
// fonte. A thread de render le o buffer depois, e via os quads da pagina 2 com a textura da pagina 1: "pizza" virava
// "pi$$a" e o inicio das frases sumia. Desenhos por FVF sao poucos e pequenos: os vertices vao para o anel, como no DrawUP.
namespace {
uint32_t UpVertices(const void* src, uint32_t bytes, bool host_words);
struct FvfSnapshot {
    gpu::State::Stream saved;
    bool active;
    explicit FvfSnapshot(uint32_t vertices) : active(false) {
        const gpu::State::Stream& st = g_state.streams[0];
        // so os pretransformados (texto, HUD): os outros FVF vem de buffers fixos e ficam no cache de geometria
        if (!g_backend || gpu::g_skip_frame || (g_state.vs_handle & 1) || (g_state.vs_handle & 0xE) != 0x4 || !st.vb || !st.stride) return;
        uint64_t bytes = uint64_t(vertices) * st.stride;
        if (!bytes || bytes > 0x10000) return;
        uint32_t data = mem::VirtualFromPhysical(R32(st.vb + 4) & 0x7FFFFFFF);
        uint32_t vb = UpVertices(HostPtr(data), uint32_t(bytes), false);
        if (!vb) return;
        saved = st;
        active = true;
        g_state.streams[0] = gpu::State::Stream(vb, st.stride);
    }
    ~FvfSnapshot() {
        if (active) g_state.streams[0] = saved;
    }
};
}  // namespace

HLE_STDCALL(D3DDevice_DrawVertices, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    static uint32_t dv_count = 0;
    if (++dv_count <= 20) {
        LOGI("d3d: DrawVertices #%u prim=%u start=%u count=%u", dv_count, a[0], a[1], a[2]);
    }
    Walk(Dev(kDevPush));  // a visibility test begun since the last draw counts this one
    SyncDeferred();
    FvfSnapshot snapshot(a[1] + a[2]);
    if (g_backend) g_backend->Draw(g_state, a[0], a[1], a[2], nullptr);
    return 0;
}

// JSRF: quanto custa cada parte do desenho na thread do jogo, a cada 5 s no log
static uint64_t g_td[6];
static uint32_t g_td_n = 0, g_td_idx = 0;
HLE_STDCALL(D3DDevice_DrawIndexedVertices, 3) {
    uint64_t c0 = __mftb();
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint64_t c1 = __mftb();
    static uint32_t div_count = 0;
    if (++div_count <= 20) {
        LOGI("d3d: DrawIndexedVertices #%u prim=%u count=%u indices=%08X", div_count, a[0], a[1], a[2]);
    }
    Walk(Dev(kDevPush));
    uint64_t c2 = __mftb();
    SyncDeferred();
    uint64_t c3 = __mftb();
    const uint8_t* indices = a[2] ? static_cast<const uint8_t*>(HostPtr(a[2])) : nullptr;
    uint32_t top = 0;
    // so o 2D pretransformado usa o maior indice (FvfSnapshot); nos outros FVF a varredura custava cerca de 1 ms por quadro
    if (indices && !(g_state.vs_handle & 1) && (g_state.vs_handle & 0xE) == 0x4 && a[1] <= 8192)
        for (uint32_t i = 0; i < a[1]; i++) top = std::max<uint32_t>(top, R16(a[2] + i * 2));
    FvfSnapshot snapshot(top ? top + 1 + g_state.base_vertex : 0);
    uint64_t c4 = __mftb();
    // JSRF: start leva o endereco dos indices, a chave do cache de geometria; num desenho indexado ele nao e usado
    if (g_backend) g_backend->Draw(g_state, a[0], snapshot.active ? 0 : a[2], a[1], indices);
    uint64_t c5 = __mftb();
    g_td[0] += c1 - c0;
    g_td[1] += c2 - c1;
    g_td[2] += c3 - c2;
    g_td[3] += c4 - c3;
    g_td[4] += c5 - c4;
    g_td_idx += a[1];
    g_td_n++;
    static uint64_t since = 0;
    if (!since) since = c5;
    if (c5 - since > 50000000ull * 5) {  // timebase de 50 MHz
        double per = 1.0 / 50000.0;      // ms
        LOGI("d3d: desenhos em 5 s %u (indices medios %u): trava %.1f ms, walk %.1f, estados %.1f, fvf %.1f, gravacao %.1f", g_td_n,
             g_td_n ? g_td_idx / g_td_n : 0, g_td[0] * per, g_td[1] * per, g_td[2] * per, g_td[3] * per, g_td[4] * per);
        memset(g_td, 0, sizeof(g_td));
        g_td_n = g_td_idx = 0;
        since = c5;
    }
    return 0;
}

// DrawVerticesUP(prim, count, vertices, stride): the vertices are copied to a ring the render thread reads, stream 0
// points there through a vertex buffer header for the draw. A half of the ring is written again once the draws
// that read it are done.
namespace {
const uint32_t kUpRing = 8u << 20, kUpHalf = kUpRing / 2;
uint32_t g_up_ring = 0, g_up_pos = 0, g_up_header = 0, g_up_fence[2] = {};

// host_words: src holds host 32 bit values (floats, dwords) that go to guest memory byte reversed; otherwise guest bytes
uint32_t UpVertices(const void* src, uint32_t bytes, bool host_words) {
    // the ring is the host's: from the system pool, never the title's 64 MB (an address there is its own
    // physical address, which the vertex reader maps back unchanged)
    if (!g_up_header) {
        g_up_header = mem::AllocSystem(20);
        W32(g_up_header, 1);  // a vertex buffer, one reference
    }
    if (!g_up_ring) g_up_ring = mem::AllocSystem(kUpRing, 0x1000);
    if (!g_up_ring || !g_up_header) {
        LOG_FIRST(4, "d3d: no memory for the DrawUP ring, the draw is dropped");
        return 0;
    }
    bytes = std::min(bytes, kUpHalf);
    uint32_t room = (bytes + 63) & ~63u;
    uint32_t pos = g_up_pos + room > kUpRing ? 0 : g_up_pos;
    uint32_t from = g_up_pos ? (g_up_pos - 1) / kUpHalf : 1, to = (pos + room - 1) / kUpHalf;
    if (to != from) {
        g_up_fence[from] = g_backend->Fence();
        g_backend->WaitFence(g_up_fence[to]);
    }
    if (host_words) {
        const uint32_t* w = static_cast<const uint32_t*>(src);
        for (uint32_t k = 0; k < bytes / 4; k++) W32(g_up_ring + pos + k * 4, w[k]);
    } else {
        std::memcpy(HostPtr(g_up_ring + pos), src, bytes);
    }
    g_up_pos = pos + room;
    W32(g_up_header + 4, mem::PhysicalFromVirtual(g_up_ring + pos));
    return g_up_header;
}

void DrawUp(uint32_t prim, uint32_t count, const void* vertices, uint32_t stride, bool host_words) {
    static uint32_t dup_count = 0;
    if (++dup_count <= 20) {
        LOGI("d3d: DrawUp #%u prim=%u count=%u stride=%u", dup_count, prim, count, stride);
    }
    if (!g_backend || !count || !stride) return;
    if (gpu::g_skip_frame) {  // the copy to the ring is for a draw that will not happen
        gpu::g_draws_asked++;
        return;
    }
    SyncDeferred();
    uint32_t vb = UpVertices(vertices, count * stride, host_words);
    if (!vb) return;
    gpu::State::Stream stream = g_state.streams[0];
    g_state.streams[0] = gpu::State::Stream(vb, stride);
    g_backend->Draw(g_state, prim, 0, count, nullptr);
    g_state.streams[0] = stream;
}

// bytes per vertex of the bound format: stream 0 of the FVF or of the shader's declaration
uint32_t VertexBytes() {
    if (g_state.vs_handle & 1) return g_state.vs ? g_state.vs->stream_stride[0] : 0;
    gpu::VertexShader fvf;
    gpu::FvfInputs(g_state.vs_handle, fvf);
    return fvf.stream_stride[0];
}

uint32_t g_push_begin = 0;
}  // namespace

// Begin/End immediate mode. Titles inline Begin and the vertex data calls, so their commands are read from the scratch buffer.
namespace d3d {
namespace {
uint32_t g_im_prim = 0, g_im_mask = 0;  // open primitive, slots it wrote
uint32_t g_vsc_load = 0, g_vsc_word = 0;  // JSRF: carga de constantes pelo pushbuffer
float g_im_attr[64];                    // 16 slots of x y z w
std::vector<float> g_im_vertices;       // every slot per vertex

// visibility tests: depth passing pixels are counted while enabled, a report gets the count since the clear,
// at a physical address in the first 16 MB
void ApplyVisibilityMethod(uint32_t m, uint32_t v) {
    const uint32_t kReportAddressMask = 0xFFFFFF;
    if (m == gpu::kMCountEnable) {
        g_state.simple[gpu::kMCountEnable >> 2] = v;
        MARK(g_state.simple[gpu::kMCountEnable >> 2]);
    }
    if (!g_backend) return;
    if (m == gpu::kMClearReport) g_backend->ClearReport();
    if (m == gpu::kMGetReport) g_backend->GetReport(mem::VirtualFromPhysical(v & kReportAddressMask));
}

// draws the open primitive: the slots it wrote are FLOAT4 inputs of the bound program, or of the fixed pipeline
void EndPrimitive() {
    struct Made {
        std::shared_ptr<gpu::VertexShader> from, vs;
    };
    // the render thread tells shaders apart by pointer: one object per program and slot set, the program kept alive
    static std::map<std::pair<gpu::VertexShader*, uint32_t>, Made> made;
    static std::vector<float> packed;
    static gpu::VertexShader* s_last_from = nullptr;
    static uint32_t s_last_mask = 0xFFFFFFFF;
    static std::shared_ptr<gpu::VertexShader> s_last_vs;

    uint32_t prim = g_im_prim, count = uint32_t(g_im_vertices.size() / 64), handle = g_state.vs_handle;
    g_im_prim = 0;
    if (!prim || !count) return;
    if (!(handle & 1) && (handle & 0xE) == 4) {
        // JSRF: logos e textos da interface. O renderizador sabe desenhar FVF XYZRHW: monta os vertices nesse formato,
        // cada entrada lida do registro que SetVertexData escreveu.
        gpu::VertexShader fvf;
        gpu::FvfInputs(handle, fvf);
        uint32_t stride = fvf.stream_stride[0];
        static std::vector<uint32_t> words;
        words.assign(count * stride / 4, 0);
        for (uint32_t v = 0; v < count; v++) {
            const float* attr = g_im_vertices.data() + v * 64;
            uint32_t* out = words.data() + v * stride / 4;
            for (uint32_t r = 0; r < 16; r++) {
                const gpu::VertexShader::Input& in = fvf.inputs[r];
                if (in.stream != 0) continue;
                const float* c = attr + r * 4;
                uint32_t* w = out + in.offset / 4;
                if (in.type == 0x40) {
                    uint32_t px = 0;
                    static const int kShift[4] = {16, 8, 0, 24};  // r g b a em D3DCOLOR
                    for (int k = 0; k < 4; k++) {
                        float f = c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k];
                        px |= uint32_t(f * 255.0f + 0.5f) << kShift[k];
                    }
                    w[0] = px;
                } else {
                    std::memcpy(w, c, (in.type >> 4) * 4);
                }
            }
        }
        DrawUp(prim, count, words.data(), stride, true);
        return;
    }
    std::shared_ptr<gpu::VertexShader> bound = g_state.vs, from = handle & 1 ? bound : std::shared_ptr<gpu::VertexShader>();
    gpu::VertexShader* from_ptr = from.get();

    std::shared_ptr<gpu::VertexShader> vs_to_use;
    if (from_ptr == s_last_from && g_im_mask == s_last_mask && s_last_vs) {
        vs_to_use = s_last_vs;
    } else {
        Made& m = made[std::make_pair(from_ptr, g_im_mask)];
        if (!m.vs) {
            m.from = from;
            m.vs = std::make_shared<gpu::VertexShader>();
            if (from) {
                m.vs->program = from->program;
                m.vs->program_type = from->program_type;
            }
            for (uint32_t s = 0; s < 16; s++) {
                if (g_im_mask >> s & 1) {
                    m.vs->inputs[s] = gpu::VertexShader::Input(0, m.vs->stream_stride[0], 0x42);
                    m.vs->stream_stride[0] += 16;
                }
            }
        }
        s_last_from = from_ptr;
        s_last_mask = g_im_mask;
        s_last_vs = m.vs;
        vs_to_use = m.vs;
    }

    uint32_t slot_offsets[16];
    uint32_t num_slots = 0;
    for (uint32_t s = 0; s < 16; s++) {
        if ((g_im_mask >> s) & 1) {
            slot_offsets[num_slots++] = s * 4;
        }
    }

    packed.resize(count * num_slots * 4);
    float* dst = packed.data();
    const float* src = g_im_vertices.data();
    for (uint32_t v = 0; v < count; v++) {
        const float* v_src = src + v * 64;
        for (uint32_t i = 0; i < num_slots; i++) {
            std::memcpy(dst, v_src + slot_offsets[i], 16);
            dst += 4;
        }
    }

    g_state.vs = vs_to_use;
    g_state.vs_handle = handle | 1;
    MARK(g_state.vs_handle);
    DrawUp(prim, count, packed.data(), vs_to_use->stream_stride[0], true);
    g_state.vs = bound;
    g_state.vs_handle = handle;
    MARK(g_state.vs_handle);
}

// reads the commands pushed since the last call: vertex data between a begin and an end is drawn, visibility tests
// are run, the rest is skipped
void Walk(uint32_t end) {
    if (!g_walk || end < g_walk || end > g_scratch_push + g_scratch_size) return;
    for (uint32_t p = g_walk; p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p), method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || n * 4 > end - p) break;  // anything but a method header
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4, v = R32(p);
            float f, *a = g_im_attr;
            bool last = true;  // the slot's last component
            std::memcpy(&f, &v, 4);
            if (m >= gpu::kMClearReport && m <= gpu::kMGetReport) {
                ApplyVisibilityMethod(m, v);
            }
            // JSRF: carga de constantes de vertex shader escrita direto no pushbuffer
            if (m == 0x1EA4) {
                g_vsc_load = v;
                g_vsc_word = 0;
                continue;
            }
            if (m >= 0x0B80 && m < 0x0C00) {
                if (g_vsc_load < 192) {
                    g_state.vsc[g_vsc_load][g_vsc_word] = f;
                    if (++g_vsc_word == 4) {
                        g_state.VscWritten(g_vsc_load, 1);
                        g_vsc_word = 0;
                        g_vsc_load++;
                    }
                }
                continue;
            }
            if (m == 0x17FC) {  // begin/end, 0 ends
                if (v) {
                    g_im_prim = v;
                    g_im_mask = 0;
                    g_im_vertices.clear();
                } else {
                    EndPrimitive();
                }
                continue;
            } else if (m >= 0x1880 && m < 0x1900) {  // 2 floats, z = 0 and w = 1
                a += (m - 0x1880) / 8 * 4;
                last = (m - 0x1880) / 4 & 1;
                a[last] = f;
                a[2] = 0;
                a[3] = 1;
            } else if (m >= 0x1940 && m < 0x1980) {  // 4 bytes
                a += (m - 0x1940) / 4 * 4;
                for (int c = 0; c < 4; c++) a[c] = (v >> c * 8 & 0xFF) / 255.0f;
            } else if (m >= 0x1A00 && m < 0x1B00) {  // 4 floats
                a += (m - 0x1A00) / 16 * 4;
                last = ((m - 0x1A00) / 4 & 3) == 3;
                a[(m - 0x1A00) / 4 & 3] = f;
            } else if (m >= 0x1500 && m < 0x150C) {  // position, 3 floats and w = 1
                last = m == 0x1508;
                a[(m - 0x1500) / 4] = f;
                a[3] = 1;
            } else if (m >= 0x1518 && m < 0x1528) {  // position, 4 floats
                last = m == 0x1524;
                a[(m - 0x1518) / 4] = f;
            } else {
                continue;
            }
            if (!g_im_prim) continue;
            g_im_mask |= 1u << (a - g_im_attr) / 4;
            if (a == g_im_attr && last) g_im_vertices.insert(g_im_vertices.end(), g_im_attr, g_im_attr + 64);
        }
    }
    g_walk = end;
}
}  // namespace
}  // namespace d3d

HLE_STDCALL(D3DDevice_DrawVerticesUP, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    DrawUp(a[0], a[1], HostPtr(a[2]), a[3], false);
    return 0;
}

// JSRF: DrawIndexedVerticesUP(prim, count, indices, vertices, stride), 0x1991C0
HLE_STDCALL(D3DDevice_DrawIndexedVerticesUP, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    uint32_t count = a[1], stride = a[4];
    if (!g_backend || !count || !stride || !a[2] || !a[3]) return 0;
    if (gpu::g_skip_frame) {
        gpu::g_draws_asked++;
        return 0;
    }
    uint32_t top = 0;
    for (uint32_t i = 0; i < count; i++) top = std::max<uint32_t>(top, R16(a[2] + i * 2));
    SyncDeferred();
    uint32_t vb = UpVertices(HostPtr(a[3]), (top + 1) * stride, false);
    if (!vb) return 0;
    gpu::State::Stream stream = g_state.streams[0];
    g_state.streams[0] = gpu::State::Stream(vb, stride);
    g_backend->Draw(g_state, a[0], 0, count, static_cast<const uint8_t*>(HostPtr(a[2])));
    g_state.streams[0] = stream;
    return 0;
}

// BeginPush(dwords) hands out room for raw hardware commands, EndPush(end) runs what the title wrote there:
// a primitive between two begin/end writes with its vertices inline, any other method as a simple state.
HLE_STDCALL(D3DDevice_BeginPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_push_begin = ScratchPush(a[0] * 4);
    return g_push_begin;
}

HLE_STDCALL(D3DDevice_EndPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const uint32_t kBeginEnd = 0x17FC, kInlineArray = 0x1818;
    static std::vector<uint32_t> vertices;
    uint32_t prim = 0;
    for (uint32_t p = g_push_begin, end = a[0]; g_push_begin && p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p), method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || n * 4 > end - p) {  // anything but a method header
            LOG_FIRST(8, "d3d: EndPush stops at command %08X, %u bytes before the end", head, end - p);
            break;
        }
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4, v = R32(p);
            ApplyVisibilityMethod(m, v);
            if (m == kInlineArray) {
                if (prim) vertices.push_back(v);
            } else if (m != kBeginEnd) {
                if (m >> 2 < g_state.simple.size()) {
                    g_state.simple[m >> 2] = v;
                    MARK(g_state.simple[m >> 2]);
                }
            } else if (v) {
                prim = v;
                vertices.clear();
            } else if (prim) {
                uint32_t stride = VertexBytes(), bytes = uint32_t(vertices.size()) * 4;
                if (stride && bytes % stride == 0) DrawUp(prim, bytes / stride, vertices.data(), stride, true);
                else LOG_FIRST(8, "d3d: EndPush primitive %u dropped, %u bytes of vertices of %u", prim, bytes, stride);
                prim = 0;
            }
        }
    }
    g_push_begin = 0;
    ScratchPush();
    return a[0];
}

// frame buffer compression tiles are the console's memory controller's business
HLE_STDCALL(D3DDevice_SetTile, 2) { return 0; }

// the library's Begin pushes the start of the primitive, here it is opened
HLE_STDCALL(D3DDevice_Begin, 1) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    g_im_prim = a[0];
    g_im_mask = 0;
    g_im_vertices.clear();
    SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) | 0x800);
    return 0;
}
// the library's End pushes the end of the primitive, here it is drawn
HLE_STDCALL(D3DDevice_End, 0) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    EndPrimitive();
    SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) & ~0x1800u);
    return 0;
}
