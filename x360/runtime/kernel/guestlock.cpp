// JSRF: so uma thread do jogo roda por vez, como no Xbox original, que tem um nucleo. O CRI ADX protege suas estruturas
// com SuspendThread e prioridades (ADXM_Lock, 0x13B0E0), o que nao protege nada com threads rodando em paralelo: erro
// ADXT -2 e congelamento intermitentes (2026-11-04). Mesma solucao do Unleashed (xbox_guest_lock em kernel_bridge.c):
// bloqueio unico, fila em ordem de chegada e fatia de 4 ms. A troca acontece em esperas do kernel, chamadas indiretas e
// saltos para tras do codigo gerado (X86_BACKEDGE). Quem pede SuspendThread agora para mesmo: a thread alvo so volta a
// rodar quando pega o bloqueio, e e ai que o pedido e cumprido.
#include "kernel/kernel.h"

#include <deque>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "host.h"

volatile long g_guest_preempt = 0;

namespace kernel {

namespace {

const int64_t kSliceNs = 4000000;

CRITICAL_SECTION* Cs() {
    static CRITICAL_SECTION cs;
    static volatile long state = 0;
    if (InterlockedCompareExchange(&state, 1, 0) == 0) {
        InitializeCriticalSection(&cs);
        state = 2;
    }
    while (state != 2) Sleep(0);
    return &cs;
}

bool g_busy = false;
std::deque<HANDLE>* g_queue = NULL;
volatile int64_t g_handed = 0;
__declspec(thread) HANDLE t_event = NULL;
__declspec(thread) bool t_held = false;
__declspec(thread) int64_t t_wait_ns = 0;
// JSRF: a thread do jogo tem um nucleo so dela (render_thread.cpp). Dormir no evento e ser acordada pelo sistema custava cada
// troca de vez com as threads do CRI: no console, 12 a 20% do tempo dela na corrida de Dogenzaka (console_boot15). Ela espera
// a vez girando por ate 2 ms antes de dormir. As outras threads seguem dormindo, porque dividem o nucleo 0 com o audio.
__declspec(thread) bool t_spin = false;

void Take() {
    if (!t_event) t_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    CRITICAL_SECTION* cs = Cs();
    EnterCriticalSection(cs);
    if (!g_queue) g_queue = new std::deque<HANDLE>();
    if (!g_busy) {
        g_busy = true;
        g_handed = host::NowNs();
        LeaveCriticalSection(cs);
    } else {
        g_queue->push_back(t_event);
        LeaveCriticalSection(cs);
        int64_t t0 = host::NowNs();
        bool got = false;
        if (t_spin) {
            while (host::NowNs() - t0 < 2000000) {
                if (WaitForSingleObject(t_event, 0) == WAIT_OBJECT_0) {
                    got = true;
                    break;
                }
                YieldProcessor();
            }
        }
        if (!got)
            while (WaitForSingleObject(t_event, 4) == WAIT_TIMEOUT)
                if (host::NowNs() - g_handed >= kSliceNs) g_guest_preempt = 1;
        t_wait_ns += host::NowNs() - t0;
    }
    t_held = true;
}

void Give() {
    t_held = false;
    CRITICAL_SECTION* cs = Cs();
    EnterCriticalSection(cs);
    g_guest_preempt = 0;
    if (g_queue->empty()) {
        g_busy = false;
    } else {
        HANDLE next = g_queue->front();
        g_queue->pop_front();
        g_handed = host::NowNs();
        ::SetEvent(next);
    }
    LeaveCriticalSection(cs);
}

}  // namespace

bool GuestHeld() { return t_held; }
void GuestSpinHere() { t_spin = true; }
int64_t GuestWaitNs() {
    int64_t v = t_wait_ns;
    t_wait_ns = 0;
    return v;
}

void GuestAcquire() {
    if (t_held) return;
    for (;;) {
        Take();
        Thread* t = Current();
        if (!t || t->suspend.load() <= 0) return;
        Give();
        std::unique_lock<std::mutex> lock(t->apc_lock);
        while (t->suspend.load() > 0) t->resume_cv.wait(lock);
    }
}

void GuestRelease() {
    if (t_held) Give();
}

void GuestYield() {
    if (!t_held) return;
    Give();
    GuestAcquire();
}

}  // namespace kernel

extern "C" void x86_guest_yield() { kernel::GuestYield(); }
