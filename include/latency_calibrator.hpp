#pragma once

#include "types.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace looper {

struct CalibrationResult {
    bool success = false;
    uint32_t latency_samples = 0;
    float latency_ms = 0.0f;
    std::string message;
};

class LatencyCalibrator {
public:
    // Pure algorithmic pulse detector for unit testing (returns sample index of first peak >= threshold, or -1)
    static int32_t detectPulseOffset(const float* signal, size_t length, float threshold = 0.25f);

    // Calculate median latency from a vector of sample measurements
    static uint32_t calculateMedian(std::vector<uint32_t> samples);

    // Run live hardware loopback calibration on the configured ALSA audio devices
    static CalibrationResult run(const AudioConfig& audio_cfg, uint32_t num_pulses = 3);
};

} // namespace looper
