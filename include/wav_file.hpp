#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace looper {

class WavFile {
public:
    // Save mono float [-1.0, 1.0] buffer as a standard 16-bit PCM WAV
    static bool save(const std::string& filepath, const std::vector<float>& samples, uint32_t sample_rate);

    // Load a 16-bit or 24-bit PCM WAV into float [-1.0, 1.0] mono buffer
    static bool load(const std::string& filepath, std::vector<float>& out_samples, uint32_t& out_sample_rate);
};

} // namespace looper
