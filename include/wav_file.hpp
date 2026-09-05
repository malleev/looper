#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "types.hpp"

namespace looper {

class WavFile {
public:
    // Atomically save mono IEEE float32 WAV, preserving finite headroom.
    static bool save(const std::string& filepath, const std::vector<float>& samples, uint32_t sample_rate);

    // PCM16/24/32 or IEEE float32, channel 1, strict bounds and rate validation.
    static bool load(const std::string& filepath, std::vector<float>& out_samples, uint32_t expected_sample_rate,
                     std::string& error_msg, size_t max_frames = MAX_LOOP_FRAMES);
};

} // namespace looper
