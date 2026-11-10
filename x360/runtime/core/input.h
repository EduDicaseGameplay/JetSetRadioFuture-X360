// Xbox 360 build of pc-recomp/sdk/runtime/core/input.h: the original Xbox controller report from XInput
#pragma once
#include <stdint.h>
#include <string.h>

namespace input {

// the console controller's report, analog buttons are 0-255
struct Pad {
    bool connected;
    uint16_t buttons;   // dpad up/down/left/right, start, back, left thumb, right thumb
    uint8_t analog[8];  // A B X Y black white left trigger right trigger
    int16_t lx, ly, rx, ry;
    Pad() : connected(false), buttons(0), lx(0), ly(0), rx(0), ry(0) { memset(analog, 0, sizeof(analog)); }
};

void Init();
Pad Poll(int port);
void Rumble(int port, uint16_t left, uint16_t right);

}  // namespace input
