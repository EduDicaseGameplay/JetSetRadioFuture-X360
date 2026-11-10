#pragma once
#include <mutex>

#include "gpu/state.h"

namespace d3d {

gpu::State& State();
std::recursive_mutex& Lock();
void MoviePresent();  // shows the native movie's picture (game/movie.cpp): the title does not swap during a movie

}  // namespace d3d
