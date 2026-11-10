// Xbox 360 build of pc-recomp/sdk/runtime/gpu/backend.h: the renderer interface the Direct3D 8 layer draws through.
// The Xbox 360 renderer (xenos.cpp) draws at once on the calling thread, so fences and busy checks never wait.
#pragma once
#include <memory>
#include <stdint.h>

#include "gpu/state.h"

namespace gpu {

// D3DCLEAR flags as the XDK defines them
enum : uint32_t {
    kClearTarget = 0xF0,  // ARGB channels
    kClearZ = 0x01,
    kClearStencil = 0x02,
};

// a frame of the title's the display will not show (game/hooks.cpp): its draws and clears are dropped when asked for
extern bool g_skip_frame;
// JSRF: widescreen, como no Unleashed. g_wide_on: 16:9 em HD, decidido em Init. g_wide_scene: a cena atual e alargada (o jogo
// avisa pelo objeto de fluxo; titulo e logos ficam 4:3).
extern volatile bool g_wide_on, g_wide_scene;
// JSRF: modo de video, trocado no painel do seletor. 0: 1280x720 em 16:9. 1: original, 640x480 em 4:3, com 4x MSAA, filtro
// anisotropico e nitidez na saida. O renderizador aplica no fim do Present.
extern volatile int g_video_mode;
extern const uint8_t* volatile g_movie_frame;  // game/movie.cpp: a 640x480 DXT1 picture shown instead of the target
extern uint32_t g_draws_asked;  // draws the title asked for, skipped or not

// JSRF: painel de texto por cima da imagem (xenos.cpp). Maiusculas e numeros; string vazia esconde.
void SetOsdText(const char* text);

// the next output gamma, for the picture shown only (xenos.cpp)
void CycleGamma();

// a frame capture from the next Present on: every draw's state in game:\dump\capN.txt and the target every few draws
void RequestCapture();

class Backend {
public:
    Backend() : fence_(0) {}
    virtual ~Backend() {}
    // the title's back buffer size
    virtual bool Init(uint32_t width, uint32_t height) = 0;
    // rects: count D3DRECTs (4 x int32, guest memory), null for the whole viewport
    virtual void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                       uint32_t stencil) = 0;
    // indices: the title's 16 bit indices (guest memory), null for non indexed draws, then start is the first vertex
    virtual void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) = 0;
    virtual void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va,
                      uint32_t dst_points_va) = 0;
    virtual void Present(const Surface& front) = 0;
    // the CPU is about to touch this resource's memory: write GPU results back first
    virtual void Sync(uint32_t data_va, uint32_t size) = 0;
    virtual void SyncLate(uint32_t data_va) = 0;
    virtual void Flush() = 0;
    virtual void SetGamma(const uint8_t* ramp /*3 x 256*/) = 0;
    virtual void ClearReport() {}
    virtual void GetReport(uint32_t /*report*/) {}

    virtual uint32_t Fence() { return ++fence_; }
    virtual bool FencePending(uint32_t) { return false; }
    virtual void WaitFence(uint32_t) {}
    virtual bool Busy(uint32_t /*resource*/) { return false; }
    // the title wrote a resource's memory with the CPU (gpu::MarkDirty): in order with what it recorded
    virtual void Dirty(uint32_t data_va);
    virtual void WaitResource(uint32_t /*resource*/) {}

private:
    uint32_t fence_;
};

std::unique_ptr<Backend> CreateXenosBackend();
// the same backend driven from a hardware thread of its own: calls return once recorded (render_thread.cpp)
std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner);

}  // namespace gpu
