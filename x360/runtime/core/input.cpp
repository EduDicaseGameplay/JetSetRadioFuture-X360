// Xbox 360 controllers as original Xbox controllers. The 360 pad has the same layout: its shoulder buttons stand in
// for the original's black (RB) and white (LB) buttons, which the game reads as analog, so a press reads 255.
#include "core/input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "core/log.h"

#include "host.h"

namespace input {

// Botoes programados para testes, como JSRF_PAD_SCRIPT do Unleashed: game:\pad_script.txt, porta 0.
// "20 START;25 A;70 LY+@5;76 RX+50@2": segundos desde a primeira leitura, botao, @duracao (padrao 0,25 s).
// Sem o arquivo, nada muda. So para testes: nao deixe esse arquivo na pasta para jogar.
namespace {
struct Press {
    double t, dur;
    uint16_t buttons;
    int analog;       // indice em Pad::analog, -1 nenhum
    int axis;         // 0 lx 1 ly 2 rx 3 ry, -1 nenhum
    int16_t value;
};
std::vector<Press> g_script;
bool g_script_read = false;
int64_t g_script_t0 = 0;

void ReadScript() {
    g_script_read = true;
    FILE* f = fopen("game:\\pad_script.txt", "rb");
    if (!f) return;
    std::string s;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ';' || s[i] == ' ' || s[i] == '\r' || s[i] == '\n')) i++;
        if (i >= s.size()) break;
        size_t e = s.find(';', i);
        std::string item = s.substr(i, e == std::string::npos ? std::string::npos : e - i);
        i = e == std::string::npos ? s.size() : e + 1;
        char name[16] = {0};
        double t = 0, dur = 0.25;
        if (sscanf(item.c_str(), "%lf %15s", &t, name) != 2) continue;
        if (char* at = strchr(name, '@')) {
            dur = atof(at + 1);
            *at = 0;
        }
        Press p;
        p.t = t;
        p.dur = dur;
        p.buttons = 0;
        p.analog = -1;
        p.axis = -1;
        p.value = 0;
        static const char* const kB[] = {"UP", "DOWN", "LEFT", "RIGHT", "START", "BACK", "LS", "RS"};
        for (int b = 0; b < 8; b++) if (!strcmp(name, kB[b])) p.buttons = uint16_t(1u << b);
        static const char* const kA[] = {"A", "B", "X", "Y", "BLACK", "WHITE", "LT", "RT"};
        for (int b = 0; b < 8; b++) if (!strcmp(name, kA[b])) p.analog = b;
        if ((name[0] == 'L' || name[0] == 'R') && (name[1] == 'X' || name[1] == 'Y') && (name[2] == '+' || name[2] == '-')) {
            p.axis = (name[0] == 'R' ? 2 : 0) + (name[1] == 'Y' ? 1 : 0);
            int pct = name[3] ? atoi(name + 3) : 100;
            p.value = int16_t((name[2] == '-' ? -1 : 1) * pct * 32767 / 100);
        }
        g_script.push_back(p);
    }
    LOGI("input: pad_script.txt com %u comandos", unsigned(g_script.size()));
}

void ApplyScript(Pad& p) {
    if (!g_script_read) ReadScript();
    if (g_script.empty()) return;
    p.connected = true;
    if (!g_script_t0) g_script_t0 = host::NowNs();
    double now = double(host::NowNs() - g_script_t0) / 1e9;
    for (size_t i = 0; i < g_script.size(); i++) {
        const Press& s = g_script[i];
        if (now < s.t || now >= s.t + s.dur) continue;
        p.buttons |= s.buttons;
        if (s.analog >= 0) p.analog[s.analog] = 255;
        if (s.axis == 0) p.lx = s.value;
        if (s.axis == 1) p.ly = s.value;
        if (s.axis == 2) p.rx = s.value;
        if (s.axis == 3) p.ry = s.value;
    }
}
}  // namespace

void Init() {}

Pad Poll(int port) {
    Pad p;
    XINPUT_STATE s;
    if (port < 0 || port > 3 || XInputGetState(DWORD(port), &s) != ERROR_SUCCESS) {
        if (port == 0) ApplyScript(p);
        return p;
    }
    const XINPUT_GAMEPAD& g = s.Gamepad;
    p.connected = true;
    // original order: up down left right start back left thumb right thumb, the same bits as XINPUT_GAMEPAD
    p.buttons = uint16_t(g.wButtons & 0x00FF);
    p.analog[0] = (g.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
    p.analog[1] = (g.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
    p.analog[2] = (g.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
    p.analog[3] = (g.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
    p.analog[4] = (g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;  // black
    p.analog[5] = (g.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;   // white
    p.analog[6] = g.bLeftTrigger;
    p.analog[7] = g.bRightTrigger;
    p.lx = g.sThumbLX;
    p.ly = g.sThumbLY;
    p.rx = g.sThumbRX;
    p.ry = g.sThumbRY;
    if (port == 0) ApplyScript(p);
    return p;
}

void Rumble(int port, uint16_t left, uint16_t right) {
    if (port < 0 || port > 3) return;
    XINPUT_VIBRATION v;
    v.wLeftMotorSpeed = left;
    v.wRightMotorSpeed = right;
    XInputSetState(DWORD(port), &v);
}

}  // namespace input
