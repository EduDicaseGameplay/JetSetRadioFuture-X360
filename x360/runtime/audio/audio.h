// Xbox 360 build of pc-recomp/sdk/runtime/audio/audio.h: the DirectSound mixer's output
#pragma once
#include <functional>
#include <stdint.h>

namespace audio {

static const uint32_t kRate = 48000;

// the platform output pulls interleaved stereo float frames from render
void StartOutput(std::function<void(float* out, uint32_t frames)> render);

// a movie's own sound (game/movie.cpp): 16 bit stereo in the CPU's byte order, the buffers kept until played
bool MovieOpen(uint32_t rate);
void MovieSubmit(const void* samples, uint32_t bytes);
void MoviePlay(bool play);
uint64_t MoviePlayed();  // samples played since MovieOpen
uint32_t MovieQueued();  // buffers not finished
void MovieStop();

}  // namespace audio
