// XAPI functions that talk to hardware: controller ports and the hard disk cache partition

#include "core/input.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;

namespace game {
void MapPickerPad(input::Pad& p);  // game/jsrf.cpp
}

namespace {

// XPP device type records: CurrentConnected, ChangeConnected, PreviousConnected
const uint32_t kGamepadType = SYM(XPP_GamepadType);
const uint32_t kMemoryUnitType = SYM(XPP_MemoryUnitType);

std::mutex g_ports_lock;
bool g_poller = false;

void UpdatePorts() {
    std::lock_guard<std::mutex> lock(g_ports_lock);
    uint32_t cur = 0;
    // P1 sempre presente, como no BRE. As outras portas so com controle de verdade: o Unleashed no PC abre so a porta 0.
    cur |= 1u << 0;
    for (int p = 1; p < 4; p++)
        if (input::Poll(p).connected) cur |= 1u << p;
    uint32_t old = R32(kGamepadType + XDK_XPP_Current);
    if (cur != old) {
        W32(kGamepadType + XDK_XPP_Changed, R32(kGamepadType + XDK_XPP_Changed) | (cur ^ old));
        W32(kGamepadType + XDK_XPP_Current, cur);
        LOGI("xapi: controller ports now %X", cur);
    }
}

struct Handle {
    uint32_t port;
    uint32_t packet;
    input::Pad last;
};
std::unordered_map<uint32_t, Handle> g_handles;

}  // namespace

HLE_STDCALL(XMountUtilityDrive, 1) {
    CreateSymbolicLink("\\??\\Z:", "\\Device\\Harddisk0\\Partition5");
    return 1;
}

HLE_STDCALL(XInitDevices, 2) {
    XDK_REQUIRE("xapi", XDK_KEY(XPP_Current), XDK_KEY(XPP_Changed), XDK_KEY(XPP_Previous), XDK_DATA(XPP_GamepadType));
    W32(kGamepadType + XDK_XPP_Current, 0);
    W32(kGamepadType + XDK_XPP_Changed, 0);
    W32(kGamepadType + XDK_XPP_Previous, 0);
    if (HAS_XPP_MemoryUnitType != 0) {  // titles without memory unit support have no record
        W32(kMemoryUnitType + XDK_XPP_Current, 0);
        W32(kMemoryUnitType + XDK_XPP_Changed, 0);
        W32(kMemoryUnitType + XDK_XPP_Previous, 0);
    }
    // JSRF: o controle ja esta presente no boot. Atual e anterior comecam iguais, sem mudanca pendente: a enumeracao do
    // jogo (sub_001669A0) trata XGetDeviceChanges != 0 como E_FAIL, e cai toda a inicializacao de entrada, fontes e
    // interface (g_unknownCOM nulo). Mesma conclusao do Unleashed, port/recomp_manual.c, sub_001BD621.
    UpdatePorts();
    W32(kGamepadType + XDK_XPP_Previous, R32(kGamepadType + XDK_XPP_Current));
    W32(kGamepadType + XDK_XPP_Changed, 0);
    if (!g_poller) {
        g_poller = true;
        host::Spawn([] {
            for (;;) {
                host::SleepNs(250000000);
                UpdatePorts();
            }
        }, "xinput poll");
    }
    return 0;
}

HLE_STDCALL(XInputOpen, 4) {
    uint32_t type = a[0], port = a[1];
    LOGI("xapi: XInputOpen type=%08X (expected %08X) port=%u slot=%u params=%08X", type, kGamepadType, port, a[2], a[3]);
    if (type != kGamepadType || port > 3) {
        LOGE("xapi: XInputOpen failed type=%08X expected=%08X port=%u", type, kGamepadType, port);
        return 0;
    }
    uint32_t h = mem::AllocSystem(16);
    std::lock_guard<std::mutex> lock(g_ports_lock);
    Handle hd;
    hd.port = port;
    hd.packet = 0;
    g_handles[h] = hd;
    LOGI("xapi: XInputOpen succeeded -> handle=%08X", h);
    return h;
}

HLE_STDCALL(XInputClose, 1) {
    LOGI("xapi: XInputClose handle=%08X", a[0]);
    std::lock_guard<std::mutex> lock(g_ports_lock);
    g_handles.erase(a[0]);
    mem::FreeSystem(a[0]);
    return 0;
}

// JSRF chama estas duas; o codigo original leria o handle como estrutura do XPP. Valores do Unleashed
// (port/recomp_manual.c, sub_001C3C22 e sub_001C3EBD).
HLE_STDCALL(XInputGetCapabilities, 2) {
    uint32_t caps = a[1];
    {
        std::lock_guard<std::mutex> lock(g_ports_lock);
        if (!caps || g_handles.find(a[0]) == g_handles.end()) return 0x57;  // ERROR_INVALID_PARAMETER
    }
    std::memset(HostPtr(caps), 0, 28);
    *static_cast<uint8_t*>(HostPtr(caps)) = 1;   // XINPUT_DEVSUBTYPE_GC_GAMEPAD
    W16(caps + 4, 0x00FF);                       // In.wButtons: todos os digitais
    std::memset(HostPtr(caps + 6), 0xFF, 8);     // In.bAnalogButtons
    for (int k = 0; k < 4; k++) W16(caps + 14 + 2 * k, 0x7FFF);
    W16(caps + 22, 0xFFFF);                      // vibracao
    W16(caps + 24, 0xFFFF);
    return 0;
}

HLE_STDCALL(XInputPoll, 1) { return 0; }

HLE_STDCALL(XInputGetState, 2) {
    uint32_t port;
    {
        std::lock_guard<std::mutex> lock(g_ports_lock);
        auto it = g_handles.find(a[0]);
        if (it == g_handles.end()) return 6;  // ERROR_INVALID_HANDLE
        port = it->second.port;
    }
    input::Pad p = input::Poll(int(port));
    // Permitir que el puerto 0 reporte conectado aunque no haya mando fisico (teclado)
    if (port == 0) {
        p.connected = true;
        game::MapPickerPad(p);
    }
    if (!p.connected) return 1167;  // ERROR_DEVICE_NOT_CONNECTED
    std::lock_guard<std::mutex> lock(g_ports_lock);
    Handle& h = g_handles[a[0]];
    if (std::memcmp(&p, &h.last, sizeof(p))) h.packet++;
    h.last = p;
    static uint32_t g_poll_count = 0;
    if (++g_poll_count <= 5) {
        LOGI("xapi: XInputGetState count=%u handle=%08X port=%u packet=%u buttons=%04X", g_poll_count, a[0], port, h.packet, p.buttons);
    }
    uint32_t s = a[1];
    W32(s, h.packet);  // XINPUT_STATE: packet, buttons, 8 analog buttons, thumbs
    W16(s + 4, p.buttons);
    std::memcpy(HostPtr(s + 6), p.analog, 8);
    W16(s + 14, uint16_t(p.lx));
    W16(s + 16, uint16_t(p.ly));
    W16(s + 18, uint16_t(p.rx));
    W16(s + 20, uint16_t(p.ry));
    return 0;
}

HLE_STDCALL(XInputSetState, 2) {
    uint32_t port;
    {
        std::lock_guard<std::mutex> lock(g_ports_lock);
        auto it = g_handles.find(a[0]);
        if (it == g_handles.end()) return 6;
        port = it->second.port;
    }
    uint32_t fb = a[1];
    // XINPUT_FEEDBACK_HEADER is dwStatus, hEvent and 58 reserved bytes, the rumble follows
    input::Rumble(int(port), R16(fb + 66), R16(fb + 68));
    W32(fb, 0);
    if (uint32_t ev = R32(fb + 4))
        if (ObjectRef o = FromHandle(ev)) SetEvent(*o);
    return 997;  // ERROR_IO_PENDING, already complete
}

// A recompiled image launches through the XAPI, then a process restart into its executable; any other
// (the dashboard, updates) is refused and the title stays in its menu.
X86_EXTERN_FUNC(GUEST_ORIG(XLaunchNewImageA));
X86_FUNC(GUEST(XLaunchNewImageA)) {
    std::string path = ReadCString(R32(ctx.esp + 4));
    LOGW("xapi: launch of %s refused, it is not recompiled", path.c_str());
    Return(ctx, 2, 2);  // ERROR_FILE_NOT_FOUND
}
