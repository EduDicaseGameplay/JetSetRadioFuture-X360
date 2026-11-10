// The Xenos renderer on a hardware thread of its own (pc-recomp/sdk/runtime/gpu/render_thread.cpp, ported to the
// VS2010 Xbox 360 compiler). The title's thread records each call with what the console takes at call time (state
// changes, resource headers, indices, rects) and goes on; vertex, texture and surface memory are read when the draw
// runs, as the console's GPU reads them after the call returns. The XDK's fences, busy checks and locks wait for that.
// The title records one frame while the one before it draws: its own code and the drawing overlap.
#include "gpu/backend.h"
#include "gpu/texture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <xtl.h>

#include "core/log.h"
#include "host.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

#ifndef DIRTY_MARKS
#define DIRTY_MARKS 1
#endif

namespace gpu {

namespace {

// commands handed over at once, so the render thread keeps close behind and a wait is short
const uint32_t kBatchCommands = 256;

// resource headers the render thread reads: common, data, lock, format, size
const uint32_t kHeaderBytes = 20, kHeaderSlot = 32;
const uint32_t kHeaderRing = 1u << 20;  // guest memory for header copies; a copy is renewed after half a ring

// Xenon: three cores of two hardware threads each (0-1, 2-3, 4-5), the two threads of a core share its in-order
// pipeline. The renderer gets core 2 and the title's thread core 1, each to itself; audio and everything light share
// core 0 (host::Spawn). Until v21 the renderer sat on thread 1; in v22 audio on thread 5 slowed it by a fifth.
const int kRenderCore = 4, kTitleCore = 2;

// a draw sends the pieces of State its writers marked (State::Mark) instead of comparing all of it; once a frame the
// whole State is compared too, and what that finds is counted in the log (a writer that does not mark)
const bool kDirtyMarks = DIRTY_MARKS;

enum Op { kOpState, kOpDraw, kOpClear, kOpCopy, kOpPresent, kOpSync, kOpFlush, kOpGamma, kOpClearReport, kOpGetReport, kOpInit, kOpDirty };

struct Cmd {
    uint8_t op;
    uint32_t a, b, c, d;
    float f;
    uint32_t data, size;  // bytes in Batch::bytes
    int32_t vs, ps;       // index in Batch::vs / ps: the shader changed
    explicit Cmd(uint8_t o) : op(o), a(0), b(0), c(0), d(0), f(0), data(0), size(0), vs(-1), ps(-1) {}
};

// bytes that grow without being zeroed first (vector::resize writes them twice, for every change of every draw)
struct Bytes {
    uint8_t* p;
    size_t n, cap;
    Bytes() : p(NULL), n(0), cap(0) {}
    size_t size() const { return n; }
    bool empty() const { return n == 0; }
    void clear() { n = 0; }
    uint8_t& operator[](size_t i) { return p[i]; }
    const uint8_t& operator[](size_t i) const { return p[i]; }
    uint8_t* grow(size_t k) {
        if (n + k > cap) {
            cap = std::max<size_t>(n + k, cap ? cap * 2 : 1u << 16);
            p = static_cast<uint8_t*>(realloc(p, cap));
        }
        uint8_t* r = p + n;
        n += k;
        return r;
    }
};

struct Batch {
    uint64_t id;
    std::vector<Cmd> cmds;
    std::vector<Surface> surfaces;  // Copy's and Present's, by Cmd::d
    Bytes bytes;
    std::vector<std::shared_ptr<VertexShader> > vs;
    std::vector<std::shared_ptr<PixelShader> > ps;
    Batch() : id(0) {}
};

// the plain data in State, around its shader pointers and the resource headers (sent as copies); of the transforms only
// view, projection, texture and world are drawn with
struct Range {
    size_t off;
    size_t len;
    Range(size_t o, size_t l) : off(o), len(l) {}
};
size_t At(const State& s, const void* p) { return size_t(static_cast<const uint8_t*>(p) - reinterpret_cast<const uint8_t*>(&s)); }
// bytes of the simple render states from method first to method last
size_t SimpleBytes(uint32_t first, uint32_t last) { return size_t(last - first) + sizeof(uint32_t); }
std::vector<Range> StateRanges() {
    static const State s;
    std::vector<Range> r;
    r.push_back(Range(At(s, &s.simple[kMAlphaTestEnable >> 2]), SimpleBytes(kMAlphaTestEnable, kMDepthOffset)));
    r.push_back(Range(At(s, &s.simple[kMPsConstant0 >> 2]), SimpleBytes(kMPsConstant0, kMPsAlphaOutputs - 4)));
    r.push_back(Range(At(s, &s.simple[kMPsFinalConstant0 >> 2]), SimpleBytes(kMPsFinalConstant0, kMPsFinalConstant1)));
    r.push_back(Range(At(s, &s.rs), At(s, &s.textures) - At(s, &s.rs)));
    r.push_back(Range(At(s, &s.index_buffer), At(s, &s.vs) - At(s, &s.index_buffer)));
    // the vertex program constants go apart (kVscRange): compared only when vsc_epoch moved
    r.push_back(Range(At(s, &s.vsc_epoch), At(s, &s.rt) - At(s, &s.vsc_epoch)));
    r.push_back(Range(At(s, &s.scissor_count), At(s, &s.transforms) - At(s, &s.scissor_count)));
    r.push_back(Range(At(s, &s.transforms[2]), 2 * sizeof(s.transforms[0])));
    r.push_back(Range(At(s, &s.transforms[16]), 4 * sizeof(s.transforms[0])));
    r.push_back(Range(At(s, &s.transforms[256]), sizeof(s.transforms[0])));
    r.push_back(Range(At(s, &s.material), sizeof(State) - At(s, &s.material)));
    return r;
}

const size_t kPiece = 64;  // bytes compared and sent at once
static_assert(sizeof(State) <= State::kDirtyWords * 32 * kPiece, "State::dirty has too few bits");

// the 192 vertex program constants (3 KB): every writer moves vsc_epoch, so equal epochs mean equal constants
Range VscRange() {
    static const State s;
    return Range(At(s, &s.vsc), sizeof(s.vsc));
}

// equal 64 byte pieces: the scan over the state on every draw. In 4 byte words: State's arrays are only 4 byte
// aligned in places, and an 8 byte load that crosses 32 bytes is microcoded on the Xbox 360 (hundreds of cycles; the
// v19 log had 9 us per draw in here)
inline bool SamePiece(const uint8_t* a, const uint8_t* b) {
    const uint32_t* x = reinterpret_cast<const uint32_t*>(a);
    const uint32_t* y = reinterpret_cast<const uint32_t*>(b);
    uint32_t d = 0;
    for (int i = 0; i < 16; i += 4) d |= (x[i] ^ y[i]) | (x[i + 1] ^ y[i + 1]) | (x[i + 2] ^ y[i + 2]) | (x[i + 3] ^ y[i + 3]);
    return d == 0;
}

inline int64_t Tb() { return int64_t(__mftb()); }

class Threaded : public Backend {
public:
    explicit Threaded(std::unique_ptr<Backend> inner)
        : inner_(std::move(inner)), ranges_(StateRanges()), vsc_range_(VscRange()), last_written_(0), ring_(0), ring_pos_(0), cur_(NULL), next_id_(0),
          last_present_(0), done_(0), stop_(false), init_ok_(false), record_tb_(0), wait_tb_(0), state_tb_(0), ranges_tb_(0), warm_tb_(0),
          headers_tb_(0), t_state0_(0), last_state_(NULL), reconcile_(false), targets_read_(false), rt_in_(0), zs_in_(0), rt_out_(0), zs_out_(0), marked_(0), missed_(0), missed_bytes_(0), frames_(0),
          since_tb_(0) {
        memset(recent_headers_, 0, sizeof(recent_headers_));
        memset(recent_uses_, 0, sizeof(recent_uses_));
        memset(slots_, 0, sizeof(slots_));
        // the ranges cut at the 64 byte pieces of State: what a marked piece sends
        seg_first_.assign(State::kDirtyWords * 32 + 1, 0);
        for (size_t piece = 0; piece < State::kDirtyWords * 32; piece++) {
            seg_first_[piece] = uint32_t(segs_.size());
            size_t lo = piece * kPiece, hi = lo + kPiece;
            for (size_t i = 0; i < ranges_.size(); i++) {
                size_t a = std::max(lo, ranges_[i].off), b = std::min(hi, ranges_[i].off + ranges_[i].len);
                if (a < b) segs_.push_back(Range(a, b - a));
            }
        }
        seg_first_[State::kDirtyWords * 32] = uint32_t(segs_.size());
        // what each group of the renderer's constants is made of (xenos.cpp SetConstants, SetPixelConstants)
        static const State z;
        Group(State::kGenWorld, At(z, &z.transforms[256]), sizeof(z.transforms[0]));
        Group(State::kGenView, At(z, &z.transforms[2]), sizeof(z.transforms[0]));
        Group(State::kGenProj, At(z, &z.transforms[3]), sizeof(z.transforms[0]));
        Group(State::kGenProj, At(z, &z.viewport), sizeof(z.viewport));
        Group(State::kGenTexMat, At(z, &z.transforms[16]), 4 * sizeof(z.transforms[0]));
        Group(State::kGenMaterial, At(z, &z.material), sizeof(z.material));
        Group(State::kGenMaterial, At(z, &z.rs[kRsAmbient]), 4);
        Group(State::kGenLights, At(z, &z.lights), At(z, &z.ss_offset_x) - At(z, &z.lights));
        Group(State::kGenFog, At(z, &z.rs[kRsFogEnable]), (kRsFogDensity + 1 - kRsFogEnable) * 4);
        Group(State::kGenFog, At(z, &z.zscale), sizeof(z.zscale));
        Group(State::kGenPixel, At(z, &z.simple[kMAlphaTestEnable >> 2]), SimpleBytes(kMAlphaTestEnable, kMDepthOffset));
        Group(State::kGenPixel, At(z, &z.simple[kMPsConstant0 >> 2]), kMPsAlphaOutputs - kMPsConstant0);
        Group(State::kGenPixel, At(z, &z.simple[kMPsFinalConstant0 >> 2]), 8);
        Group(State::kGenPixel, At(z, &z.rs[kRsFogColor]), 4);
        Group(State::kGenPixel, At(z, &z.rs[kRsTextureFactor]), 4);
        Group(State::kGenPixel, At(z, &z.rs[kRsShadowFunc]), 4);
        Group(State::kGenPixel, At(z, &z.rs[kRsSpecularEnable]), 4);
        Group(State::kGenPixel, At(z, &z.tss), sizeof(z.tss));
        Group(State::kGenShader, At(z, &z.tss), sizeof(z.tss));
        Group(State::kGenShader, At(z, &z.rs[kRsPsTextureModes]), 4);
        Group(State::kGenShader, At(z, &z.simple[kMAlphaTestEnable >> 2]), 4);
        Group(State::kGenShader, At(z, &z.rs[kRsFogEnable]), 4);
        Group(State::kGenShader, At(z, &z.rs[kRsSpecularEnable]), 4);
        Group(State::kGenShader, At(z, &z.rs[kRsTwoSidedLighting]), 4);
        Group(State::kGenShader, At(z, &z.simple[kMShadeMode >> 2]), 4);
        state_.gens_valid = true;
        // the title's thread is the one that makes the device
        XSetThreadProcessor(GetCurrentThread(), kTitleCore);
        LOGI("render thread: the title's thread moves to hardware thread %d", kTitleCore);
        ring_ = mem::AllocSystem(kHeaderRing, 64);
        if (!ring_) LOGE("render thread: no guest memory for header copies, headers are read late");
        cur_ = NewBatch();
        host::Spawn([this] { Run(); }, "render", kRenderCore);
        LOGI("render thread: started on hardware thread %d", kRenderCore);
    }

    // the device is made on the render thread, which then owns it
    bool Init(uint32_t width, uint32_t height) {
        Cmd c(kOpInit);
        c.a = width;
        c.b = height;
        cur_->cmds.push_back(c);
        Wait(Submit());
        return init_ok_;
    }

    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
        g_draws_asked++;
        if (g_skip_frame) return;
        int64_t t = Tb();
        SendState(s);
        state_tb_ += Tb() - t;
        Cmd c(kOpDraw);
        c.a = prim;
        c.b = start;
        c.c = count;
        if (indices) {
            c.d = 1;
            c.data = Put(indices, size_t(count) * 2);
        }
        cur_->cmds.push_back(c);
        for (int i = 0; i < 4; i++) UseAt(kSlotTexture + i, s.textures[size_t(i)]);
        for (int i = 0; i < 4; i++) UseAt(kSlotPalette + i, s.palettes[size_t(i)]);
        for (int i = 0; i < 16; i++) UseAt(kSlotStream + i, s.streams[size_t(i)].vb);
        UseAt(kSlotRt, s.rt.header);
        UseAt(kSlotZs, s.zs.header);
        Written(s.rt);
        if (cur_->cmds.size() >= kBatchCommands) Submit();
        record_tb_ += Tb() - t;
    }

    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) {
        if (g_skip_frame) return;
        int64_t t = Tb();
        SendState(s);
        Cmd c(kOpClear);
        c.a = count;
        c.b = flags;
        c.c = color;
        c.d = stencil;
        c.f = z;
        if (count && rects) {
            c.data = Put(rects, size_t(count) * 16);
            c.size = count * 16;
        }
        cur_->cmds.push_back(c);
        Use(s.rt.header);
        Use(s.zs.header);
        if (flags & kClearTarget) Written(s.rt);
        record_tb_ += Tb() - t;
    }

    // rects and points are taken now; the copy itself runs in order like the console's
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) {
        Cmd c(kOpCopy);
        c.a = count;
        c.b = count && src_rects ? Scratch(kernel::HostPtr(src_rects), count * 16) : 0;
        c.c = count && dst_points ? Scratch(kernel::HostPtr(dst_points), count * 8) : 0;
        c.d = uint32_t(cur_->surfaces.size());
        cur_->surfaces.push_back(src);
        cur_->surfaces.back().header = Header(src.header);
        cur_->surfaces.push_back(dst);
        cur_->surfaces.back().header = Header(dst.header);
        cur_->cmds.push_back(c);
        Use(src.header);
        Use(dst.header);
        Written(dst);
    }

    void Present(const Surface& front) {
        Cmd c(kOpPresent);
        c.d = uint32_t(cur_->surfaces.size());
        cur_->surfaces.push_back(front);
        cur_->surfaces.back().header = Header(front.header);
        cur_->cmds.push_back(c);
        Use(front.header);
        reconcile_ = true;
        targets_read_ = false;
        uint64_t id = Submit();
        // the title records one frame while the one before it draws
        Wait(last_present_);
        last_present_ = id;
        // every 5 s: the title's thread recording, and waiting for the render thread
        frames_++;
        int64_t now = Tb();
        if (!since_tb_) since_tb_ = now;
        static double tps = 0;
        if (tps == 0) {
            LARGE_INTEGER q0, q1, qf;
            QueryPerformanceFrequency(&qf);
            QueryPerformanceCounter(&q0);
            int64_t t0 = Tb();
            Sleep(20);
            QueryPerformanceCounter(&q1);
            tps = double(Tb() - t0) * double(qf.QuadPart) / double(q1.QuadPart - q0.QuadPart);
        }
        if (now - since_tb_ >= int64_t(tps * 5)) {
            double per = 1000.0 / (tps * frames_);
            LOGI("render thread: per frame, the title recorded %.2f ms (state %.2f: %s %.2f, the whole state once %.2f, headers "
                 "and shaders %.2f; %.0f pieces marked) and waited %.2f ms for the renderer; unmarked changes %u (%u bytes)",
                 double(record_tb_) * per, double(state_tb_) * per, kDirtyMarks ? "marked" : "scan", double(ranges_tb_) * per,
                 double(warm_tb_) * per, double(headers_tb_) * per, double(marked_) / frames_, double(wait_tb_) * per,
                 missed_, missed_bytes_);
            record_tb_ = wait_tb_ = state_tb_ = ranges_tb_ = warm_tb_ = headers_tb_ = 0;
            marked_ = missed_ = missed_bytes_ = 0;
            frames_ = 0;
            since_tb_ = now;
        }
    }

    void Sync(uint32_t data, uint32_t) { ReadBack(data); }

    // a lock that does not wait for the GPU: this renderer has no copy made ahead, so it reads back now
    void SyncLate(uint32_t data) {
        written_.insert(data);
        ReadBack(data);
    }

    void Flush() {
        cur_->cmds.push_back(Cmd(kOpFlush));
        Wait(Submit());
    }

    void SetGamma(const uint8_t* ramp) {
        Cmd c(kOpGamma);
        c.data = Put(ramp, 768);
        cur_->cmds.push_back(c);
    }

    void ClearReport() { cur_->cmds.push_back(Cmd(kOpClearReport)); }
    void Dirty(uint32_t data) {
        Cmd c(kOpDirty);
        c.a = data;
        cur_->cmds.push_back(c);
    }
    void GetReport(uint32_t report) {
        Cmd c(kOpGetReport);
        c.a = report;
        cur_->cmds.push_back(c);
    }

    uint32_t Fence() { return uint32_t(cur_->id); }
    bool FencePending(uint32_t fence) { return fence > done_.load(); }
    void WaitFence(uint32_t fence) { Wait(fence); }
    bool Busy(uint32_t resource) {
        std::unordered_map<uint32_t, uint64_t>::iterator it = last_use_.find(resource);
        return it != last_use_.end() && it->second > done_.load();
    }
    void WaitResource(uint32_t resource) {
        std::unordered_map<uint32_t, uint64_t>::iterator it = last_use_.find(resource);
        if (it != last_use_.end()) Wait(it->second);
    }

private:
    Batch* NewBatch() {
        Batch* b;
        {
            std::lock_guard<std::mutex> lock(m_);
            if (free_.empty()) {
                b = new Batch;
            } else {
                b = free_.back();
                free_.pop_back();
            }
        }
        b->id = ++next_id_;
        return b;
    }

    uint32_t Put(const void* p, size_t n) {
        size_t at = (cur_->bytes.size() + 3) & ~size_t(3);
        cur_->bytes.grow(at + n - cur_->bytes.size());
        memcpy(&cur_->bytes[0] + at, p, n);
        return uint32_t(at);
    }

    void ReadBack(uint32_t data) {
        // only surfaces the GPU wrote since the last read-back hold anything newer than guest memory
        if (!written_.erase(data)) return;
        if (data == last_written_) last_written_ = 0;
        Cmd c(kOpSync);
        c.a = data;
        cur_->cmds.push_back(c);
        Wait(Submit());
    }

    // bytes the render thread reads through a guest address, in the header ring
    uint32_t Scratch(const void* p, uint32_t n) {
        if (!ring_) return 0;
        uint32_t size = (n + kHeaderSlot - 1) & ~(kHeaderSlot - 1);
        if (ring_pos_ % kHeaderRing + size > kHeaderRing) ring_pos_ += kHeaderRing - ring_pos_ % kHeaderRing;
        uint32_t at = ring_ + uint32_t(ring_pos_ % kHeaderRing);
        ring_pos_ += size;
        memcpy(kernel::HostPtr(at), p, n);
        return at;
    }

    struct HeaderCopy {
        uint32_t at;
        uint64_t made;  // ring_pos_ after it was written
        __declspec(align(4)) uint8_t bytes[kHeaderBytes];
        HeaderCopy() : at(0), made(0) { memset(bytes, 0, sizeof(bytes)); }
    };

    // A header as it is now. The console's push buffer takes a texture's format and address, and a surface's or vertex
    // buffer's address, when they are set; the title points the same header elsewhere before the GPU gets there.
    uint32_t Header(uint32_t header) {
        if (!header || !ring_) return header;
        RecentHeader& r = recent_headers_[Slot(header)];
        HeaderCopy* c = r.key == header && r.value ? r.value : &headers_[header];
        r.key = header;
        r.value = c;
        return Renew(*c, header);
    }

    // the same through what a binding (texture stage, stream, target) held at the last draw: most draws keep them
    enum { kSlotTexture = 0, kSlotPalette = 4, kSlotStream = 8, kSlotRt = 24, kSlotZs = 25, kSlots = 26 };
    struct BindSlot {
        uint32_t header;
        HeaderCopy* copy;
        uint64_t* use;
    };
    uint32_t HeaderAt(int slot, uint32_t header) {
        if (!header || !ring_) return header;
        BindSlot& b = slots_[slot];
        if (b.header != header || !b.copy) {
            b.header = header;
            b.copy = &headers_[header];
            b.use = NULL;
        }
        return Renew(*b.copy, header);
    }

    uint32_t Renew(HeaderCopy& c, uint32_t header) {
        // vertex buffer, index buffer and palette headers are 12 bytes: reading 20 can run off the end of the heap
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(header));
        const uint32_t type = kernel::R32(header) & 0x70000;  // 0 vertex, 0x10000 index, 0x30000 palette
        uint32_t now[kHeaderBytes / 4];
        memcpy(now, p, 12);
        if (type < 0x20000 || type == 0x30000) {
            now[3] = now[4] = 0;
        } else {
            memcpy(now + 3, p + 12, 8);
        }
        const uint32_t* had = reinterpret_cast<const uint32_t*>(c.bytes);
        if (c.made && !((had[0] ^ now[0]) | (had[1] ^ now[1]) | (had[2] ^ now[2]) | (had[3] ^ now[3]) | (had[4] ^ now[4])) &&
            ring_pos_ - c.made < kHeaderRing / 2)
            return c.at;
        c.at = Scratch(now, kHeaderBytes);
        c.made = ring_pos_;
        memcpy(c.bytes, now, kHeaderBytes);
        return c.at;
    }

    // a field of the render thread's State, when it differs from what was sent
    void Delta(const void* field, const void* value, size_t n) {
        uint8_t* sent = reinterpret_cast<uint8_t*>(&sent_) + At(sent_, field);
        // everything in State goes in 4 byte words
        const uint32_t* x = reinterpret_cast<const uint32_t*>(sent);
        const uint32_t* y = static_cast<const uint32_t*>(value);
        uint32_t d = 0;
        for (size_t i = 0; i < n / 4; i++) d |= x[i] ^ y[i];
        if (d) Send(sent, value, n);
    }

    // a field that differs from what was sent
    void Send(uint8_t* sent, const void* value, size_t n) {
        memcpy(sent, value, n);
        uint8_t* o = cur_->bytes.grow(8 + n);
        reinterpret_cast<uint32_t*>(o)[0] = uint32_t(sent - reinterpret_cast<const uint8_t*>(&sent_));
        reinterpret_cast<uint32_t*>(o)[1] = uint32_t(n);
        memcpy(o + 8, value, n);
    }

    // the pieces of a range that changed, the last one shorter when the range ends inside it
    void SendRange(const uint8_t* now, const Range& r) {
        const uint8_t* sent = reinterpret_cast<const uint8_t*>(&sent_);
        size_t off = r.off;
        size_t end = r.off + r.len;
        for (; off + kPiece <= end; off += kPiece)
            if (!SamePiece(sent + off, now + off)) Send(const_cast<uint8_t*>(sent + off), now + off, kPiece);
        if (off < end) Delta(sent + off, now + off, end - off);
    }

    // the marked pieces: each sends its part of the ranges, when it differs from what was sent
    void SendMarked(const State& s) {
        const uint8_t* now = reinterpret_cast<const uint8_t*>(&s);
        const uint8_t* sent = reinterpret_cast<const uint8_t*>(&sent_);
        for (uint32_t w = 0; w < State::kDirtyWords; w++) {
            uint32_t bits = s.dirty[w];
            if (!bits) continue;
            s.dirty[w] = 0;
            for (uint32_t piece = w * 32; bits; piece++, bits >>= 1) {
                if (!(bits & 1)) continue;
                marked_++;
                for (uint32_t k = seg_first_[piece]; k < seg_first_[piece + 1]; k++)
                    Delta(sent + segs_[k].off, now + segs_[k].off, segs_[k].len);
            }
        }
    }

    // what changed since the last command, in 64 byte pieces
    void SendState(const State& s) {
        const uint8_t* now = reinterpret_cast<const uint8_t*>(&s);
        t_state0_ = Tb();
        int32_t new_vs = -1, new_ps = -1;
        size_t first = cur_->bytes.size();
        // another State than the last one (the self tests' own): its marks say nothing about what was sent
        const bool other = &s != last_state_;
        last_state_ = &s;
        if (other) {
            SendRange(now, vsc_range_);
        } else if (s.vsc_epoch != sent_.vsc_epoch && s.vsc_lo < s.vsc_hi) {
            // only the registers written since the last command (State::VscWritten), not all 192
            SendRange(now, Range(vsc_range_.off + s.vsc_lo * 16, (s.vsc_hi - s.vsc_lo) * 16));
        }
        s.vsc_lo = 192;
        s.vsc_hi = 0;
        if (kDirtyMarks && !other) {
            SendMarked(s);
        } else {
            memset(s.dirty, 0, sizeof(s.dirty));
            for (size_t i = 0; i < ranges_.size(); i++) SendRange(now, ranges_[i]);
        }
        int64_t t_ranges = Tb();
        ranges_tb_ += t_ranges - t_state0_;
        // the first command of a frame compares all of it: a change no writer marked is sent here and counted
        if (kDirtyMarks && reconcile_ && !other) {
            reconcile_ = false;
            size_t before = cur_->bytes.size();
            SendRange(now, vsc_range_);
            for (size_t i = 0; i < ranges_.size(); i++) SendRange(now, ranges_[i]);
            if (cur_->bytes.size() != before) {
                missed_++;
                missed_bytes_ += uint32_t(cur_->bytes.size() - before);
                LOG_FIRST(16, "render thread: an unmarked change of the state, %u bytes at %u", unsigned(cur_->bytes.size() - before - 8),
                          *reinterpret_cast<const uint32_t*>(&cur_->bytes[before]));
            }
            int64_t t_whole = Tb();
            warm_tb_ += t_whole - t_ranges;
            t_ranges = t_whole;
        }
        // the bindings as the render thread will see them; most draws change none, so the words are compared here and
        // a Delta only made for what differs
        uint32_t words[4], differ = 0;
        for (size_t i = 0; i < 4; i++) {
            words[i] = HeaderAt(kSlotTexture + int(i), s.textures[i]);
            differ |= words[i] ^ sent_.textures[i];
        }
        if (differ) Delta(&sent_.textures, words, sizeof(words));
        differ = 0;
        for (size_t i = 0; i < 4; i++) {
            words[i] = s.palettes[i] ? HeaderAt(kSlotPalette + int(i), s.palettes[i]) : 0;
            differ |= words[i] ^ sent_.palettes[i];
        }
        if (differ) Delta(&sent_.palettes, words, sizeof(words));
        uint32_t stream_words[32];
        differ = 0;
        for (size_t i = 0; i < 16; i++) {
            const State::Stream& st = s.streams[i];
            stream_words[i * 2] = st.vb ? HeaderAt(kSlotStream + int(i), st.vb) : 0;
            stream_words[i * 2 + 1] = st.stride;
            differ |= (stream_words[i * 2] ^ sent_.streams[i].vb) | (st.stride ^ sent_.streams[i].stride);
        }
        if (differ) Delta(&sent_.streams, stream_words, sizeof(stream_words));
        // a target's header changes when the swap rotates it, and only then (d3d8.cpp): read once a frame, not per draw
        Surface rt = s.rt, zs = s.zs;
        if (!targets_read_ || rt.header != rt_in_ || zs.header != zs_in_) {
            rt_in_ = rt.header;
            zs_in_ = zs.header;
            rt_out_ = HeaderAt(kSlotRt, rt.header);
            zs_out_ = HeaderAt(kSlotZs, zs.header);
            targets_read_ = true;
        }
        rt.header = rt_out_;
        zs.header = zs_out_;
        if ((rt.header ^ sent_.rt.header) | (rt.data ^ sent_.rt.data) | (rt.format ^ sent_.rt.format) | (rt.size ^ sent_.rt.size))
            Delta(&sent_.rt, &rt, sizeof(rt));
        if ((zs.header ^ sent_.zs.header) | (zs.data ^ sent_.zs.data) | (zs.format ^ sent_.zs.format) | (zs.size ^ sent_.zs.size))
            Delta(&sent_.zs, &zs, sizeof(zs));
        if (s.vs.get() != sent_.vs.get()) {
            sent_.vs = s.vs;
            new_vs = int32_t(cur_->vs.size());
            cur_->vs.push_back(s.vs);
        }
        if (s.ps.get() != sent_.ps.get()) {
            sent_.ps = s.ps;
            new_ps = int32_t(cur_->ps.size());
            cur_->ps.push_back(s.ps);
        }
        headers_tb_ += Tb() - t_ranges;
        if (cur_->bytes.size() == first && new_vs < 0 && new_ps < 0) return;
        Cmd c(kOpState);
        c.vs = new_vs;
        c.ps = new_ps;
        c.data = uint32_t(first);
        c.size = uint32_t(cur_->bytes.size() - first);
        cur_->cmds.push_back(c);
    }

    // resources by guest header: the batch that last read them
    void Use(uint32_t header) {
        if (!header) return;
        RecentUse& r = recent_uses_[Slot(header)];
        if (r.key != header || !r.value) {
            r.key = header;
            r.value = &last_use_[header];
        }
        *r.value = cur_->id;
    }
    void UseAt(int slot, uint32_t header) {
        if (!header) return;
        BindSlot& b = slots_[slot];
        if (b.header != header || !b.use) {
            if (b.header != header) {
                b.header = header;
                b.copy = NULL;
            }
            b.use = &last_use_[header];
        }
        *b.use = cur_->id;
    }
    void Written(const Surface& s) {
        if (!s.header) return;
        uint32_t d = mem::VirtualFromPhysical(s.data);
        if (d == last_written_) return;  // a draw after a draw to the same target: already there
        written_.insert(d);
        last_written_ = d;
    }

    uint64_t Submit() {
        uint64_t id = cur_->id;
        {
            std::lock_guard<std::mutex> lock(m_);
            queue_.push_back(cur_);
        }
        cv_.notify_one();
        cur_ = NewBatch();
        return id;
    }

    void Wait(uint64_t id) {
        if (!id || done_.load() >= id) return;
        if (id >= cur_->id) id = Submit();
        int64_t t = Tb();
        {
            std::unique_lock<std::mutex> lock(m_);
            while (done_.load() < id) done_cv_.WaitNs(lock, 5000000);
        }
        wait_tb_ += Tb() - t;
    }

    void Run() {
        for (;;) {
            Batch* b;
            {
                std::unique_lock<std::mutex> lock(m_);
                while (!stop_ && queue_.empty()) cv_.WaitNs(lock, 50000000);
                if (queue_.empty()) break;
                b = queue_.front();
                queue_.pop_front();
            }
            for (size_t i = 0; i < b->cmds.size(); i++) Execute(*b, b->cmds[i]);
            uint64_t id = b->id;
            b->cmds.clear();
            b->bytes.clear();
            b->surfaces.clear();
            b->vs.clear();
            b->ps.clear();
            {
                std::lock_guard<std::mutex> lock(m_);
                free_.push_back(b);
                done_.store(id);
            }
            done_cv_.notify_all();
        }
    }

    void Execute(const Batch& b, const Cmd& c) {
        const uint8_t* data = b.bytes.empty() ? NULL : &b.bytes[0] + c.data;
        switch (c.op) {
            case kOpState: {
                uint8_t* s = reinterpret_cast<uint8_t*>(&state_);
                for (const uint8_t* p = data; p < data + c.size;) {
                    uint32_t head[2];
                    memcpy(head, p, 8);
                    memcpy(s + head[0], p + 8, head[1]);
                    p += 8 + head[1];
                    // the vertex program constant rows a change reached since the renderer last sent them
                    // (xenos.cpp SetConstants): what it compares and sends instead of all 192
                    if (Overlaps(vsc_range_, head[0], head[1])) {
                        uint32_t a = uint32_t((std::max(size_t(head[0]), vsc_range_.off) - vsc_range_.off) / 16);
                        uint32_t b = uint32_t((std::min(size_t(head[0]) + head[1], vsc_range_.off + vsc_range_.len) - vsc_range_.off + 15) / 16);
                        if (a < state_.vsc_lo) state_.vsc_lo = a;
                        if (b > state_.vsc_hi) state_.vsc_hi = b;
                    }
                    uint32_t moved = 0;
                    for (size_t i = 0; i < groups_.size(); i++)
                        if (Overlaps(groups_[i].r, head[0], head[1])) moved |= 1u << groups_[i].gen;
                    for (int g = 0; moved; g++, moved >>= 1)
                        if (moved & 1) state_.gen[g]++;
                }
                if (c.vs >= 0) state_.vs = b.vs[size_t(c.vs)];
                if (c.ps >= 0) state_.ps = b.ps[size_t(c.ps)];
                break;
            }
            case kOpDraw: inner_->Draw(state_, c.a, c.b, c.c, c.d ? data : NULL); break;
            case kOpClear: inner_->Clear(state_, c.a, c.size ? data : NULL, c.b, c.c, c.f, c.d); break;
            case kOpCopy: inner_->Copy(b.surfaces[c.d], b.surfaces[c.d + 1], c.a, c.b, c.c); break;
            case kOpPresent: inner_->Present(b.surfaces[c.d]); break;
            case kOpSync: inner_->Sync(c.a, 0); break;
            case kOpFlush: inner_->Flush(); break;
            case kOpGamma: inner_->SetGamma(data); break;
            case kOpClearReport: inner_->ClearReport(); break;
            case kOpGetReport: inner_->GetReport(c.a); break;
            case kOpInit: init_ok_ = inner_->Init(c.a, c.b); break;
            case kOpDirty: MarkDirtyData(c.a); break;
        }
    }

    static bool Overlaps(const Range& r, size_t off, size_t len) { return off < r.off + r.len && r.off < off + len; }

    struct GenRange {
        Range r;
        int gen;
        GenRange(const Range& r_, int g) : r(r_), gen(g) {}
    };
    void Group(int gen, size_t off, size_t len) { groups_.push_back(GenRange(Range(off, len), gen)); }

    std::unique_ptr<Backend> inner_;
    std::vector<Range> ranges_;
    std::vector<GenRange> groups_;  // State::gen's parts of State
    Range vsc_range_;
    uint32_t last_written_;  // the surface Written last put in written_, while it is still there

    // the title's thread
    std::unordered_map<uint32_t, HeaderCopy> headers_;
    // the last few lookups, in front of the maps (their elements stay put)
    struct RecentHeader {
        uint32_t key;
        HeaderCopy* value;
    };
    struct RecentUse {
        uint32_t key;
        uint64_t* value;
    };
    static const uint32_t kRecent = 1024;
    static uint32_t Slot(uint32_t header) { return ((header >> 2) * 2654435761u) >> 22; }
    RecentHeader recent_headers_[kRecent];
    RecentUse recent_uses_[kRecent];
    BindSlot slots_[kSlots];
    std::vector<Range> segs_;          // the ranges cut at 64 byte pieces of State
    std::vector<uint32_t> seg_first_;  // a piece's first entry in segs_
    uint32_t ring_;
    uint64_t ring_pos_;
    Batch* cur_;
    State sent_;  // the state as the render thread will have it
    std::unordered_map<uint32_t, uint64_t> last_use_;
    std::unordered_set<uint32_t> written_;  // surfaces drawn since their last read-back, by data address
    uint64_t next_id_, last_present_;

    // the render thread
    State state_;

    std::mutex m_;
    std::condition_variable cv_, done_cv_;
    std::deque<Batch*> queue_;
    std::vector<Batch*> free_;
    std::atomic<uint64_t> done_;
    bool stop_;
    volatile bool init_ok_;

    // per frame timing of the title's thread
    int64_t record_tb_, wait_tb_, state_tb_, ranges_tb_, warm_tb_, headers_tb_, t_state0_;
    const State* last_state_;  // what the last command sent from
    bool reconcile_;           // the next command compares the whole State
    bool targets_read_;        // rt_out_ and zs_out_ are this frame's
    uint32_t rt_in_, zs_in_, rt_out_, zs_out_;
    uint32_t marked_, missed_, missed_bytes_;
    uint32_t frames_;
    int64_t since_tb_;
};

}  // namespace

void Backend::Dirty(uint32_t data_va) { MarkDirtyData(data_va); }

bool g_skip_frame = false;
volatile bool g_wide_on = false, g_wide_scene = false;
volatile int g_video_mode = 0;
const uint8_t* volatile g_movie_frame = NULL;
uint32_t g_draws_asked = 0;

std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner) {
    return std::unique_ptr<Backend>(new Threaded(std::move(inner)));
}

}  // namespace gpu
