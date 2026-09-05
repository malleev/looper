#pragma once
#include <cmath>
#include <cstdint>
#include <limits>

namespace looper {
inline int32_t floatToPcm32(float value) {
    if (std::isnan(value)) return 0;
    if (value >= 1.0f) return std::numeric_limits<int32_t>::max();
    if (value <= -1.0f) return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(static_cast<double>(value) * 2147483648.0);
}

// Timestamp and unread frames must come from the same ALSA status snapshot.
inline uint64_t captureBlockStart(uint64_t status_ns, uint64_t unread_frames,
                                  uint64_t read_frames, uint32_t rate) {
    if (!rate) return 0;
    const uint64_t age = (unread_frames + read_frames) * 1000000000ULL / rate;
    return status_ns > age ? status_ns - age : 0;
}
}
