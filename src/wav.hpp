#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mbot {

struct Pcm {
    std::vector<int16_t> samples;  // mono
    uint32_t rate = 0;
};

// Parse a 16-bit PCM WAV (mono or stereo -> downmixed to mono).
bool loadWav(const std::string& bytes, Pcm& out, std::string* err = nullptr);

// A 16-bit mono PCM WAV file (bytes) from samples.
std::string makeWav(const Pcm& pcm);

// Linear resample — good enough for speech into Vector's little speaker.
Pcm resample(const Pcm& in, uint32_t rate);

}  // namespace mbot
