// JSRF no Xbox 360. Uma definicao forte substitui a funcao recompilada naquele endereco. Os ganchos do BRE (hooks.cpp,
// movie.cpp) ficaram de fora: os enderecos sao de outro jogo.
#include "cpu/x86_ops.h"
#include "core/log.h"
#include "host.h"
#include "core/input.h"
#include "gpu/backend.h"
#include "kernel/kernel.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

namespace game {
void CycleFrameSkip() {}
}  // namespace game

// Ritmo do jogo. Cada passo de CActMan::IdleSub avanca 1/60 s de jogo: abaixo de 60 fps tudo ficava em camera lenta (no
// console, 7 s do cronometro da fase em 15 s de video). Quando o relogio passa de um passo de atraso, o passo seguinte
// roda a logica e nao desenha, com o m_bSkipDraw (+0x74) do proprio jogo, que tambem dispensa o Present. Mesma ideia do
// BRE-03. Sem mais de 2 seguidos: com 3, o console ficava em 15 fps; com 2, cerca de 18 fps e 90% da velocidade, que
// quase nao se percebe. game:\sem_pulo.txt desliga, para comparar.
namespace {
struct Pace {
    int64_t next, exec_ns, draw_ns, t_start;
    uint32_t steps, drawn, skips_row;
    bool skip, off, read_off;
    // JSRF: modo de ritmo. 0 automatico: 60 fps enquanto cabe, 30 fps com passo regular quando a fase pesa. 1: sempre 30
    // regulares. 2: livre, como ate a v35 (pula o que precisar, o quadro oscila). game:\fps_modo.txt guarda a escolha.
    int mode;
    bool lock30, parity;
    uint32_t calm;  // janelas seguidas com folga, no 30 do automatico
    uint32_t w_steps, w_skips, w_drawn;
    int64_t w_exec, w_draw;
} g_pace;
volatile int g_fps_mode_req = -1;  // pedido do painel
const int64_t kStepNs = 16666667;
}  // namespace

extern "C" void __imp__sub_0014C800(X86Context& ctx, uint8_t* base);

// Medida do que pesa na CPU do console, uma vez na abertura (cerca de 50 ms). Decide o proximo passo de otimizacao.
static void CpuBench(X86Context& ctx, uint8_t* base) {
    __declspec(align(16)) static uint8_t buf[4096];
    for (int i = 0; i < 4096; i += 4) *reinterpret_cast<uint32_t*>(buf + i) = 0x0000803Fu;
    const int n = 256;
    const int rounds = 2000;
    volatile uint32_t sink_i = 0;
    volatile float sink_f = 0;
    int64_t t0 = host::NowNs();
    uint32_t si = 0;
    for (int r = 0; r < rounds; r++)
        for (int i = 0; i < n; i++) si += x86_le32(buf + i * 4);
    sink_i = si;
    int64_t t1 = host::NowNs();
    double sf = 0;
    for (int r = 0; r < rounds; r++)
        for (int i = 0; i < n; i++) sf += double(x86_lef32(buf + i * 4));
    sink_f = float(sf);
    int64_t t2 = host::NowNs();
    uint32_t saved_top = ctx.top, ecx = ctx.ecx, edx = ctx.edx, esp = ctx.esp;
    uint32_t data = (esp - 0x100u) & ~15u;
    for (int i = 0; i < 4; i++) X86_MEM32(data + i * 4) = 0u;
    const int calls = 100000;
    for (int i = 0; i < calls; i++) {
        ctx.ecx = data;
        ctx.edx = data;
        ctx.esp = esp - 4;
        __imp__sub_0014C800(ctx, base);
        ctx.top = saved_top;
    }
    ctx.ecx = ecx;
    ctx.edx = edx;
    ctx.esp = esp;
    int64_t t3 = host::NowNs();
    double total = double(rounds) * n;
    LOGI("jsrf: medida da CPU: leitura inteira %.2f ns, leitura de float %.2f ns, vDot com chamada %.1f ns", double(t1 - t0) / total,
         double(t2 - t1) / total, double(t3 - t2) / calls);
    (void)sink_i;
    (void)sink_f;
}

extern "C" void __imp__sub_00013A80(X86Context& ctx, uint8_t* base);
X86_FUNC(sub_00013A80) {
    Pace& p = g_pace;
    static bool benched = false;
    if (!benched) {
        benched = true;
        CpuBench(ctx, base);
    }
    if (!p.read_off) {
        p.read_off = true;
        kernel::GuestSpinHere();
        p.mode = 1;  // 30 fixo: o melhor no console (console_boot17 e 18); o painel troca
        FILE* mf = fopen("game:\\fps_modo.txt", "rb");
        if (mf) {
            if (fscanf(mf, "%d", &p.mode) != 1 || p.mode < 0 || p.mode > 2) p.mode = 1;
            fclose(mf);
        }
        p.lock30 = p.mode == 1;
        p.off = GetFileAttributesA("game:\\sem_pulo.txt") != DWORD(-1);
        LOGI("jsrf: pulo de desenho %s", p.off ? "desligado" : "ligado");
    }
    int64_t now = host::NowNs();
    if (!p.next) {
        p.next = now;
        p.t_start = now;
    }
    // No 30 regular, o passo que nao desenha nao tem Present, e e o Present que segura o ritmo no vsync. Sem esperar a hora
    // dele, o passo pulado rodava logo depois do desenhado e o jogo andava a 120 passos por segundo (xenia_v36_30fixo). O
    // desenhado tambem espera: com o Present anterior pulado, a fila do vsync tem folga e ele nao segura (xenia_v37_30fixo).
    if (p.lock30 && p.mode != 2 && !p.off && g_fps_mode_req < 0) {
        int64_t left = p.next - now;
        if (left > 0 && left < 2 * kStepNs) {
            if (left > 3000000) {
                kernel::GuestUnlock unlocked;
                Sleep(DWORD((left - 2000000) / 1000000));
            }
            while (host::NowNs() < p.next) YieldProcessor();
            now = host::NowNs();
        }
    }
    int64_t lag = now - p.next;
    // adiantado, ou parado por carregamento: a divida nao fica para depois
    if (lag < 0 || lag > 250000000ll) {
        p.next = now;
        lag = 0;
    }
    if (g_fps_mode_req >= 0) {
        p.mode = g_fps_mode_req;
        g_fps_mode_req = -1;
        p.lock30 = p.mode == 1;
        p.w_steps = p.w_skips = p.w_drawn = 0;
        p.w_exec = p.w_draw = 0;
    }
    if (p.mode == 2 || p.off) {
        p.skip = !p.off && lag > kStepNs && p.skips_row < 2;
    } else if (p.lock30) {
        // um passo desenha, o seguinte nao: 30 quadros regulares por segundo. Atrasado alem de um passo, pula mais um.
        p.parity = !p.parity;
        p.skip = p.parity || (lag > 2 * kStepNs && p.skips_row < 2);
    } else {
        p.skip = lag > kStepNs && p.skips_row < 2;
    }
    p.skips_row = p.skip ? p.skips_row + 1 : 0;
    p.w_steps++;
    if (p.skip) p.w_skips++;
    else p.w_drawn++;
    // a cada 2 s decide o automatico, com folga para nao ficar trocando
    if (p.mode == 0 && p.w_steps >= 120) {
        double l = double(p.w_exec) / (double(p.w_steps) * 1e6), c = p.w_drawn ? double(p.w_draw) / (double(p.w_drawn) * 1e6) : 0;
        if (!p.lock30 && p.w_skips * 10 > p.w_steps) {
            p.lock30 = true;
            LOGI("jsrf: ritmo automatico vai para 30 fps (logica %.1f ms, cena %.1f ms)", l, c);
        } else if (p.lock30) {
            // no console ia e voltava a cada 2 s com 9 a 10 ms (console_boot17): so volta depois de 6 s com bastante folga
            p.calm = l + c < 9.0 ? p.calm + 1 : 0;
            if (p.calm >= 3) {
                p.lock30 = false;
                p.calm = 0;
                LOGI("jsrf: ritmo automatico volta para 60 fps (logica %.1f ms, cena %.1f ms)", l, c);
            }
        }
        p.w_steps = p.w_skips = p.w_drawn = 0;
        p.w_exec = p.w_draw = 0;
    }
    p.next += kStepNs;
    gpu::g_skip_frame = p.skip;
    p.steps++;
    if (!p.skip) p.drawn++;
    __imp__sub_00013A80(ctx, base);
    gpu::g_skip_frame = false;
    if (now - p.t_start >= 5000000000ll) {
        double n = p.steps ? double(p.steps) * 1e6 : 1;
        LOGI("jsrf: ritmo em %.1f s: %u passos, %u desenhados; por passo, logica %.2f ms; por quadro desenhado, cena %.2f ms; esperando a vez %.0f ms; modo %d%s",
             double(now - p.t_start) / 1e9, p.steps, p.drawn, double(p.exec_ns) / n, p.drawn ? double(p.draw_ns) / (double(p.drawn) * 1e6) : 0,
             double(kernel::GuestWaitNs()) / 1e6, p.mode, p.lock30 ? " (30)" : " (60)");
        p.t_start = now;
        p.steps = p.drawn = 0;
        p.exec_ns = p.draw_ns = 0;
    }
}

// CActMan::ActionExec: a logica do passo
extern "C" void __imp__sub_000123E0(X86Context& ctx, uint8_t* base);
X86_FUNC(sub_000123E0) {
    int64_t t = host::NowNs();
    __imp__sub_000123E0(ctx, base);
    int64_t dt = host::NowNs() - t;
    g_pace.exec_ns += dt;
    g_pace.w_exec += dt;
}

// CActMan::draw: a cena inteira
extern "C" void __imp__sub_00013930(X86Context& ctx, uint8_t* base);
X86_FUNC(sub_00013930) {
    if (g_pace.skip) {
        X86_MEM32(ctx.ecx + 0x74u) = 1u;  // m_bSkipDraw
        ctx.esp += 4;                     // o ret da funcao original, sem argumentos
        return;
    }
    int64_t t = host::NowNs();
    __imp__sub_00013930(ctx, base);
    int64_t dt = host::NowNs() - t;
    g_pace.draw_ns += dt;
    g_pace.w_draw += dt;
}

// sub_0013B180: `while (!g_exit) g_counter++` em prioridade alta, sem nunca bloquear. Ocuparia uma thread de hardware
// inteira. Mesma correcao do Unleashed (port/recomp_manual.c): o laco fica aqui, com uma pausa. Nada le o ritmo do
// contador. Com a saida marcada, a funcao original segue para ExitThread.
extern "C" void __imp__sub_0013B180(X86Context& ctx, uint8_t* base);
X86_FUNC(sub_0013B180) {
    while (X86_MEM32(0x25EFC0u) == 0) {
        X86_MEM32(0x25EFA8u) = uint32_t(X86_MEM32(0x25EFA8u)) + 1;
        kernel::GuestUnlock unlocked;
        Sleep(2);
    }
    __imp__sub_0013B180(ctx, base);
}

// Diagnostico da inicializacao do CMGameGL: quem falha antes de g_unknownCOM (0x251D74) ficar nulo.
#define JSRF_LOG_RET(addr, label)                                                                    \
    extern "C" void __imp__sub_##addr(X86Context& ctx, uint8_t* base);                               \
    X86_FUNC(sub_##addr) {                                                                           \
        uint32_t ret = X86_MEM32(ctx.esp);                                                           \
        __imp__sub_##addr(ctx, base);                                                                \
        LOGI("jsrf: %s retornou %08X (chamada de %08X) gl=%08X ucom=%08X", label, ctx.eax, ret,      \
             uint32_t(X86_MEM32(0x251D6Cu)), uint32_t(X86_MEM32(0x251D74u)));                        \
    }
JSRF_LOG_RET(0005F2C0, "MGameGL_Initialize")
JSRF_LOG_RET(0005F250, "createControllerCOM")
JSRF_LOG_RET(00156DF0, "createUnknownCOM")
JSRF_LOG_RET(0014CFB0, "sub_14CFB0")

// Rastro do erro fatal e da tela de erro de disco, como port/recomp_manual.c do Unleashed
extern "C" void __imp__sub_00012770(X86Context& ctx, uint8_t* base);
X86_FUNC(sub_00012770) {
    LOGE("jsrf: erro fatal (sub_00012770), ecx=%08X", ctx.ecx);
    for (uint32_t i = 0; i < 200; i++) {
        uint32_t v = X86_MEM32(ctx.esp + i * 4);
        if (v > 0x11000 && v < 0x1C3F60) LOGE("jsrf:   [esp+%03X] %08X", i * 4, v);
    }
    __imp__sub_00012770(ctx, base);
}
extern "C" void __imp__sub_0006F730(X86Context& ctx, uint8_t* base);
X86_FUNC(sub_0006F730) {
    LOGE("jsrf: tela de erro de disco, codigo ADXT %d, chamada de %08X", int(int16_t(ctx.eax)), uint32_t(X86_MEM32(ctx.esp)));
    __imp__sub_0006F730(ctx, base);
}

// Mensagem de erro do CRI ADX (ADXERR), como port/recomp_manual.c do Unleashed
extern "C" void __imp__sub_00143240(X86Context& ctx, uint8_t* base);
X86_FUNC(sub_00143240) {
    char a[96] = {0}, b[96] = {0};
    uint32_t pa = X86_MEM32(ctx.esp + 4), pb = X86_MEM32(ctx.esp + 8);
    for (int i = 0; pa && i < 95; i++) { a[i] = char(X86_MEM8(pa + i)); if (!a[i]) break; }
    for (int i = 0; pb && i < 95; i++) { b[i] = char(X86_MEM8(pb + i)); if (!b[i]) break; }
    LOGE("jsrf: ADXERR '%s' '%s' (chamada de %08X)", a, b, uint32_t(X86_MEM32(ctx.esp)));
    __imp__sub_00143240(ctx, base);
}

// Seletor de mapas, como port/recomp_manual.c do Unleashed. LB + RB por 1 s abre; D-pad ou analogico escolhe; A vai; B fecha.
// O mapa e pedido do jeito que uma missao termina: objeto de fluxo +0x4C capitulo, +0x54 numero, estado +0x5C = 93, em jogo
// (estados 15 e 31) ha pelo menos 1 s e nunca na propria abertura (missao 0x5A). Sao as missoes livres do capitulo 9.
// game:\map_tour.txt, para testes: "45:0x9006E,75:0x90073" pede cada missao no segundo indicado.
namespace {
struct MapEntry { const char* name; uint32_t mission; };
const MapEntry kMaps[] = {
    {"Shibuya Terminal", 0x9006E},    {"Chuo Street", 0x90073},         {"Dogenzaka Hill", 0x90078},
    {"Hikage Street", 0x8007D},       {"Rokkaku-dai Heights", 0x90082}, {"Sewage Facility", 0x90087},
    {"Kibogaoka Hill", 0x9008C},      {"Sewage Bottom Point", 0x90091}, {"Fortified Zone", 0x90096},
    {"99th Street", 0x9009B},         {"Sky Dinosaurian Sq", 0x900A0},  {"Rokkaku Expo Stadium", 0x900A5},
    {"Highway Zero", 0x900AA},        {"Skyscraper District", 0x900AF}};
const int kMapCount = sizeof(kMaps) / sizeof(kMaps[0]);

struct Picker {
    bool open, fresh, go_was, close_was, x_was, y_was;
    int sel, dir_was;
    int64_t held_since, next_move;
    volatile uint32_t pick;
} g_picker;

double Secs() {
    static int64_t t0 = host::NowNs();
    return double(host::NowNs() - t0) / 1e9;
}

void PickerShow() {
    char text[640];
    int n = _snprintf(text, sizeof(text), "GO TO A MAP\n");
    for (int k = 0; k < kMapCount; k++)
        n += _snprintf(text + n, sizeof(text) - n, "%s %s\n", k == g_picker.sel ? ">" : " ", kMaps[k].name);
    n += _snprintf(text + n, sizeof(text) - n, "VIDEO: %s\n", gpu::g_video_mode == 1 ? "ORIGINAL 4:3 + EFFECTS" : "720P WIDESCREEN");
    {
        int fm = g_fps_mode_req >= 0 ? g_fps_mode_req : g_pace.mode;
        n += _snprintf(text + n, sizeof(text) - n, "FPS: %s\n", fm == 1 ? "30 STEADY" : fm == 2 ? "FREE (OLD)" : "AUTO 60/30");
    }
    _snprintf(text + n, sizeof(text) - n, "UP DOWN CHOOSE  A GO  B CLOSE  X VIDEO  Y FPS");
    gpu::SetOsdText(text);
}

uint32_t TourRequest() {
    static bool read = false;
    static double at[32];
    static uint32_t id[32];
    static int n = 0, i = 0;
    if (!read) {
        read = true;
        FILE* f = fopen("game:\\map_tour.txt", "rb");
        if (f) {
            char buf[1024] = {0};
            fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            for (char* p = buf; p && *p && n < 32;) {
                char* e;
                double t = strtod(p, &e);
                if (e == p || *e != ':') break;
                at[n] = t;
                id[n] = uint32_t(strtoul(e + 1, &e, 0));
                n++;
                p = *e == ',' ? e + 1 : NULL;
            }
            LOGI("jsrf: map_tour.txt com %d mapas", n);
        }
    }
    return i < n && Secs() >= at[i] ? id[i++] : 0;
}
}  // namespace

namespace game {
void MapPickerPad(input::Pad& p) {
    int64_t now = host::NowNs();
    if (!g_picker.open) {
        if (p.analog[4] > 64 && p.analog[5] > 64) {  // RB e LB, que o jogo le como preto e branco
            if (!g_picker.held_since) g_picker.held_since = now;
            if (now - g_picker.held_since >= 1000000000ll) {
                g_picker.open = true;
                g_picker.fresh = true;
                g_picker.held_since = 0;
                PickerShow();
                LOGI("jsrf: seletor de mapas aberto");
            }
        } else {
            g_picker.held_since = 0;
        }
        if (!g_picker.open) return;
    }
    bool up = (p.buttons & 1) || p.ly > 16000, down = (p.buttons & 2) || p.ly < -16000;
    int dir = up ? -1 : down ? 1 : 0;
    bool go = p.analog[0] > 64 || (p.buttons & 0x10), close = p.analog[1] > 64 || (p.buttons & 0x20);
    if (g_picker.fresh) {  // o que estava apertado ao abrir nao conta
        g_picker.fresh = false;
        g_picker.go_was = go;
        g_picker.close_was = close;
        g_picker.x_was = p.analog[2] > 64;
        g_picker.y_was = p.analog[3] > 64;
        g_picker.dir_was = dir;
        g_picker.next_move = now + 400000000ll;
    }
    if (dir && (dir != g_picker.dir_was || now >= g_picker.next_move)) {
        g_picker.sel = (g_picker.sel + dir + kMapCount) % kMapCount;
        g_picker.next_move = now + (dir != g_picker.dir_was ? 400000000ll : 120000000ll);
        PickerShow();
    }
    g_picker.dir_was = dir;
    {
        bool x = p.analog[2] > 64;
        if (x && !g_picker.x_was) {
            gpu::g_video_mode = gpu::g_video_mode == 1 ? 0 : 1;
            LOGI("jsrf: modo de video trocado para %d", gpu::g_video_mode);
            FILE* f = fopen("game:\\video_modo.txt", "wb");
            if (f) {
                fprintf(f, "%d", gpu::g_video_mode);
                fclose(f);
            }
            PickerShow();
        }
        g_picker.x_was = x;
        bool y = p.analog[3] > 64;
        if (y && !g_picker.y_was) {
            int fm = ((g_fps_mode_req >= 0 ? g_fps_mode_req : g_pace.mode) + 1) % 3;
            g_fps_mode_req = fm;
            LOGI("jsrf: modo de fps trocado para %d", fm);
            FILE* f = fopen("game:\\fps_modo.txt", "wb");
            if (f) {
                fprintf(f, "%d", fm);
                fclose(f);
            }
            PickerShow();
        }
        g_picker.y_was = y;
    }
    if (go && !g_picker.go_was) {
        g_picker.pick = kMaps[g_picker.sel].mission;
        LOGI("jsrf: seletor escolheu %s", kMaps[g_picker.sel].name);
        g_picker.open = false;
        gpu::SetOsdText("");
    } else if (close && !g_picker.close_was) {
        g_picker.open = false;
        gpu::SetOsdText("");
    }
    g_picker.go_was = go;
    g_picker.close_was = close;
    // o jogo ve o controle parado
    uint16_t keep = 0;
    p.buttons = keep;
    memset(p.analog, 0, sizeof(p.analog));
    p.lx = p.ly = p.rx = p.ry = 0;
}
}  // namespace game

// Widescreen, de port/recomp_manual.c do Unleashed (licenca MIT, secao "widescreen"). O jogo desenha 4:3: a vertical fica igual, a
// horizontal abre para 16:9 (m00 = 3/4) e o recorte lateral acompanha.
static inline bool WideNow() { return gpu::g_wide_on && gpu::g_wide_scene; }

// CMGameGL: a projecao da camera, matriz fixa em 0x22E6B8 com m00 = 1
extern "C" void __imp__sub_00153EC0(X86Context& ctx, uint8_t* base);
static uint32_t g_cam_dev = 0;
static int g_cam_wide = -1;
X86_FUNC(sub_00153EC0) {
    g_cam_dev = ctx.ecx;
    g_cam_wide = WideNow() ? 1 : 0;
    float want = WideNow() ? 0.75f : 1.0f, m00 = X86_MEMF32(0x22E6B8u);
    if ((m00 == 1.0f || m00 == 0.75f) && m00 != want) X86_MEMF32(0x22E6B8u) = want;
    __imp__sub_00153EC0(ctx, base);
}

// CMGameGL: o recorte da camera. 0x264E80 sec, 0x264E84 -tan, 0x264E88 sec, 0x264E8C +tan dos planos laterais
extern "C" void __imp__sub_00153A90(X86Context& ctx, uint8_t* base);
X86_FUNC(sub_00153A90) {
    __imp__sub_00153A90(ctx, base);
    if (!WideNow()) return;
    float t = float(X86_MEMF32(0x264E8Cu)) * (4.0f / 3.0f), sec = sqrtf(1.0f + t * t);
    X86_MEMF32(0x264E80u) = sec;
    X86_MEMF32(0x264E84u) = -t;
    X86_MEMF32(0x264E88u) = sec;
    X86_MEMF32(0x264E8Cu) = t;
}

// CMGameGL: SetTransform(self, estado, matriz). Uma projecao em perspectiva de outra origem tambem abre, so durante a chamada
extern "C" void __imp__sub_00154420(X86Context& ctx, uint8_t* base);
X86_FUNC(sub_00154420) {
    uint32_t self = X86_MEM32(ctx.esp + 4), state = X86_MEM32(ctx.esp + 8), mat = X86_MEM32(ctx.esp + 12);
    bool widened = false;
    float keep[4];
    if (state == 1 && WideNow()) {
        if (mat == 0xFFFFFFFFu && self >= 0x10000u) {
            uint32_t top = X86_MEM32(self + 0x64u);
            if (top >= 0x10000u) mat = X86_MEM32(top);
        }
        if (mat >= 0x10000u && mat != 0xFFFFFFFFu && float(X86_MEMF32(mat + 44u)) != 0.0f && float(X86_MEMF32(mat + 60u)) == 0.0f) {
            widened = true;
            for (uint32_t i = 0; i < 4; i++) {
                keep[i] = X86_MEMF32(mat + i * 16u);
                X86_MEMF32(mat + i * 16u) = keep[i] * 0.75f;
            }
        }
    }
    __imp__sub_00154420(ctx, base);
    if (widened)
        for (uint32_t i = 0; i < 4; i++) X86_MEMF32(mat + i * 16u) = keep[i];
}

// O jogo define a camera uma vez: ao trocar o modo, refaz o recorte e a projecao, as mesmas duas chamadas do SetCamera, no mesmo
// dispositivo. Como wide_reapply_step do Unleashed.
static void WideReapply(X86Context& ctx, uint8_t* base) {
    int now = WideNow() ? 1 : 0;
    if (!g_cam_dev || g_cam_wide < 0 || g_cam_wide == now) return;
    if (g_cam_dev < 0x10000u || uint32_t(X86_MEM32(g_cam_dev)) != 0x001E0F00u) {
        g_cam_dev = 0;
        return;
    }
    uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx, dev = g_cam_dev;
    ctx.esp -= 4;
    X86_MEM32(ctx.esp) = 0u;
    ctx.ecx = dev;
    sub_00153A90(ctx, base);
    ctx.esp -= 4;
    X86_MEM32(ctx.esp) = 0u;
    ctx.ecx = dev;
    sub_00153EC0(ctx, base);
    ctx.eax = eax;
    ctx.ecx = ecx;
    ctx.edx = edx;
    LOGI("jsrf: camera refeita, widescreen %d", now);
}

// game:\\video_tour.txt, so para teste: tempo:modo,tempo:modo
static void VideoTour() {
    static bool read = false;
    static double at[8];
    static int mode[8], n = 0, i = 0;
    if (!read) {
        read = true;
        FILE* f = fopen("game:\\video_tour.txt", "rb");
        if (f) {
            char buf[256] = {0};
            fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            for (char* p = buf; p && *p && n < 8;) {
                char* e;
                at[n] = strtod(p, &e);
                if (e == p || *e != ':') break;
                mode[n++] = int(strtol(e + 1, &e, 10));
                p = *e == ',' ? e + 1 : NULL;
            }
        }
    }
    if (i < n && Secs() >= at[i]) {
        gpu::g_video_mode = mode[i++];
        LOGI("jsrf: video_tour, modo %d", gpu::g_video_mode);
    }
}

// game:\\latas.txt, so para teste: "tempo:quantidade" da latas ao jogador, para acionar o boost. Como JSRF_CANS do Unleashed: camera
// +0x24 e o personagem, personagem +0xFA0 as latas.
extern "C" void __imp__sub_000A5070(X86Context& ctx, uint8_t* base);
X86_FUNC(sub_000A5070) {
    static int state = -1, cans = 0;
    static double when = 0;
    if (state < 0) {
        state = 0;
        FILE* f = fopen("game:\\latas.txt", "rb");
        if (f) {
            if (fscanf(f, "%lf:%d", &when, &cans) == 2) state = 1;
            fclose(f);
        }
    }
    if (state == 1 && Secs() >= when && ctx.ecx >= 0x10000u) {
        uint32_t who = X86_MEM32(ctx.ecx + 0x24u);
        if (who >= 0x10000u) {
            X86_MEM32(who + 0xFA0u) = uint32_t(cans);
            LOGI("jsrf: teste, %d latas para o jogador", cans);
            state = 2;
        }
    }
    __imp__sub_000A5070(ctx, base);
}

// Decodificador ADX estereo do CRI (sub_00144F60), em codigo nativo. As threads do CRI decodificam a musica dentro da vez do jogo,
// e nos picos da corrida de Dogenzaka a thread do jogo esperava ate 14% do tempo (console_boot16). Mesma conta do original,
// conferida no disassembly: blocos de 18 bytes, esquerdo e direito intercalados, escala de 13 bits em big endian (bit 15 = fim),
// s = ((c1*h1 + c2*h2) >> 12) + nibble*escala, saturado em 16 bits. Nas primeiras 300 chamadas os dois rodam sobre os mesmos dados
// e sao comparados; qualquer diferenca volta para o original de vez.
// cdecl (src, blocos, saidaE, histE, saidaD, histD, c1, c2) -> blocos, ou 2 * pares feitos ao achar o fim
extern "C" void __imp__sub_00144F60(X86Context& ctx, uint8_t* base);
static uint32_t AdxNative(uint8_t* base, uint32_t src, uint32_t count, uint32_t outL, uint32_t histL, uint32_t outR, uint32_t histR,
                          int32_t c1, int32_t c2) {
    int32_t l1 = int16_t(uint16_t(X86_MEM16(histL))), l2 = int16_t(uint16_t(X86_MEM16(histL + 2)));
    int32_t r1 = int16_t(uint16_t(X86_MEM16(histR))), r2 = int16_t(uint16_t(X86_MEM16(histR + 2)));
    int32_t pairs = int32_t(count) / 2;
    const uint8_t* p = base + (src & X86_GUEST_MASK);
    uint8_t* oL = base + (outL & X86_GUEST_MASK);
    uint8_t* oR = base + (outR & X86_GUEST_MASK);
    for (int32_t k = 0; k < pairs; k++, p += 36) {
        if ((p[0] & 0x80) || (p[18] & 0x80)) return uint32_t(k * 2);  // fim: as historias ficam como estavam, como no original
        int32_t sL = ((p[0] << 8) | p[1]) & 0x1FFF, sR = ((p[18] << 8) | p[19]) & 0x1FFF;
        sL++;
        sR++;
        for (int b = 0; b < 16; b++) {
            int32_t bl = int8_t(p[2 + b]), br = int8_t(p[20 + b]);
            int32_t nib[2][2] = {{bl >> 4, ((bl & 15) ^ 8) - 8}, {br >> 4, ((br & 15) ^ 8) - 8}};
            for (int h = 0; h < 2; h++) {
                int32_t v = ((c1 * l1 + c2 * l2) >> 12) + nib[0][h] * sL;
                v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
                l2 = l1;
                l1 = v;
                int32_t w = ((c1 * r1 + c2 * r2) >> 12) + nib[1][h] * sR;
                w = w > 32767 ? 32767 : w < -32768 ? -32768 : w;
                r2 = r1;
                r1 = w;
                __storeshortbytereverse(uint16_t(v), 0, oL);
                __storeshortbytereverse(uint16_t(w), 0, oR);
                oL += 2;
                oR += 2;
            }
        }
    }
    X86_MEM16(histL) = uint16_t(l1);
    X86_MEM16(histL + 2) = uint16_t(l2);
    X86_MEM16(histR) = uint16_t(r1);
    X86_MEM16(histR + 2) = uint16_t(r2);
    return count;
}

X86_FUNC(sub_00144F60) {
    static int checked = 0, bad = 0;
    uint32_t sp = ctx.esp;
    uint32_t src = X86_MEM32(sp + 4), count = X86_MEM32(sp + 8), outL = X86_MEM32(sp + 12), histL = X86_MEM32(sp + 16);
    uint32_t outR = X86_MEM32(sp + 20), histR = X86_MEM32(sp + 24);
    int32_t c1 = int16_t(uint16_t(X86_MEM32(sp + 28))), c2 = int16_t(uint16_t(X86_MEM32(sp + 32)));
    int32_t pairs = int32_t(count) / 2;
    if (bad || pairs < 0 || pairs > 4096) {
        __imp__sub_00144F60(ctx, base);
        return;
    }
    if (checked < 300) {
        // os dois sobre os mesmos dados: guarda as saidas e historias, roda o original, restaura, roda o nativo e compara
        const uint32_t bytes = uint32_t(pairs) * 64;
        static std::vector<uint8_t> keepL, keepR, origL, origR;
        keepL.assign(base + (outL & X86_GUEST_MASK), base + (outL & X86_GUEST_MASK) + bytes);
        keepR.assign(base + (outR & X86_GUEST_MASK), base + (outR & X86_GUEST_MASK) + bytes);
        uint32_t hl = X86_MEM32(histL), hr = X86_MEM32(histR);
        __imp__sub_00144F60(ctx, base);
        uint32_t want = ctx.eax, ohl = X86_MEM32(histL), ohr = X86_MEM32(histR);
        origL.assign(base + (outL & X86_GUEST_MASK), base + (outL & X86_GUEST_MASK) + bytes);
        origR.assign(base + (outR & X86_GUEST_MASK), base + (outR & X86_GUEST_MASK) + bytes);
        memcpy(base + (outL & X86_GUEST_MASK), &keepL[0], bytes);
        memcpy(base + (outR & X86_GUEST_MASK), &keepR[0], bytes);
        X86_MEM32(histL) = hl;
        X86_MEM32(histR) = hr;
        uint32_t got = AdxNative(base, src, count, outL, histL, outR, histR, c1, c2);
        bool same = got == want && X86_MEM32(histL) == ohl && X86_MEM32(histR) == ohr &&
                    !memcmp(base + (outL & X86_GUEST_MASK), &origL[0], bytes) && !memcmp(base + (outR & X86_GUEST_MASK), &origR[0], bytes);
        if (!same) {
            bad = 1;
            LOGE("jsrf: ADX nativo diferente do original na chamada %d (retorno %u contra %u): volta ao original", checked, got, want);
            memcpy(base + (outL & X86_GUEST_MASK), &origL[0], bytes);
            memcpy(base + (outR & X86_GUEST_MASK), &origR[0], bytes);
            X86_MEM32(histL) = ohl;
            X86_MEM32(histR) = ohr;
        }
        ctx.eax = want;  // o original ja fez o ret
        if (++checked == 300 && !bad) LOGI("jsrf: ADX nativo conferido em 300 chamadas, igual ao original; dai em diante so o nativo");
        return;
    }
    ctx.eax = AdxNative(base, src, count, outL, histL, outR, histR, c1, c2);
    ctx.esp += 4;  // ret do cdecl
}

// sub_00051FC0: passo do objeto de fluxo do jogo a cada quadro; +0x58 missao, +0x5C estado
extern "C" void __imp__sub_00051FC0(X86Context& ctx, uint8_t* base);
X86_FUNC(sub_00051FC0) {
    VideoTour();
    WideReapply(ctx, base);
    static uint32_t want = 0, seen_flow = 0, seen_st = 0xFFFFFFFFu;
    static double seen_t = 0;
    uint32_t flow = ctx.ecx;
    if (flow >= 0x10000u) {
        {
            // titulo e demo (missao 0x5A) em 4:3; as outras missoes alargadas. Um fluxo passando a vez (93, 94) nao decide.
            uint32_t mi0 = X86_MEM32(flow + 0x58), st0 = X86_MEM32(flow + 0x5C);
            if (st0 != 93 && st0 != 94) gpu::g_wide_scene = mi0 != 0x5Au;
        }
        uint32_t r = TourRequest();
        if (!r && g_picker.pick) {
            r = g_picker.pick;
            g_picker.pick = 0;
        }
        if (r) want = r;
        uint32_t st = X86_MEM32(flow + 0x5C), mi = X86_MEM32(flow + 0x58);
        if (flow != seen_flow || st != seen_st) {
            seen_flow = flow;
            seen_st = st;
            seen_t = Secs();
        }
        if (want && (st == 15 || st == 31) && mi != 0x5Au && Secs() - seen_t >= 1.0) {
            LOGI("jsrf: t=%.1f estado %u missao %08X -> missao %08X", Secs(), st, mi, want);
            {
                // A missao 0x3AD (Kibogaoka, historia) e outras deixam eGLOBAL_SPAWNPOSINDEX_BASE (77) em -1. O fim normal dela leva a
                // uma cena que nao precisa de ponto de partida; pelo seletor, o jogador nascia fora do mapa, sem cenario nem
                // personagem, e isso seguia nas proximas trocas. O Unleashed tem a mesma falha (tests/evidence/pc_evento3).
                uint32_t am = X86_MEM32(0x22FCE0u);
                if (am >= 0x10000u && uint32_t(X86_MEM32(am + 0x7868u + 77u * 4u)) == 0xFFFFFFFFu) {
                    LOGI("jsrf: seletor corrigiu o ponto de partida, que estava em -1");
                    X86_MEM32(am + 0x7868u + 77u * 4u) = 1u;
                }
            }
            X86_MEM32(flow + 0x4C) = want >> 16;
            X86_MEM32(flow + 0x54) = want & 0xFFFF;
            X86_MEM32(flow + 0x5C) = 93;
            want = 0;
        }
    }
    __imp__sub_00051FC0(ctx, base);
}
