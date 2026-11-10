// Xbox 360 front end settings the kernel reports to the game (main360.cpp)
#pragma once
#include <stdint.h>

namespace app {

uint32_t Language();  // XC_LANGUAGE value, 1 English
bool Widescreen();

}  // namespace app
