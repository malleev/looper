#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace looper {

constexpr uint32_t DEFAULT_SAMPLE_RATE = 48000;
constexpr uint32_t DEFAULT_PERIOD_SIZE = 128;   // ~2.6 ms buffer at 48kHz
constexpr uint32_t DEFAULT_CHANNELS = 4;      // MiniFuse 1 requires 4 HW channels
constexpr uint32_t CROSSFADE_SAMPLES = 240;   // 5 ms crossfade at 48kHz to prevent clicks

enum class LooperState {
    IDLE,       // No loop recorded, passthrough live audio
    RECORDING,  // Recording base layer
    PLAYING,    // Playing loop (+ passthrough live audio)
    OVERDUB,    // Recording new layer on top of loop (+ playing loop + passthrough)
    STOPPED     // Loop exists in memory, but playback is paused
};

inline const char* stateToString(LooperState state) {
    switch (state) {
        case LooperState::IDLE: return "IDLE";
        case LooperState::RECORDING: return "RECORDING";
        case LooperState::PLAYING: return "PLAYING";
        case LooperState::OVERDUB: return "OVERDUB";
        case LooperState::STOPPED: return "STOPPED";
        default: return "UNKNOWN";
    }
}

struct LooperConfig {
    uint32_t sample_rate = DEFAULT_SAMPLE_RATE;
    uint32_t period_size = DEFAULT_PERIOD_SIZE;
    float dry_gain = 0.0f;       // 0.0 if using MiniFuse Direct Monitor hardware button, 1.0 if software monitor
    float loop_gain = 1.0f;      // Main loop playback volume
    float fade_out_sec = 3.0f;   // Fade-out duration in seconds
    uint32_t crossfade_samples = CROSSFADE_SAMPLES;
};

struct LooperStatus {
    LooperState state = LooperState::IDLE;
    size_t playhead_frames = 0;
    size_t total_frames = 0;
    float current_sec = 0.0f;
    float total_sec = 0.0f;
    float in_peak = 0.0f;        // 0.0 to 1.0 peak input level for VU-meter
    bool is_reversed = false;
    bool is_fading_out = false;
    bool undo_available = false;
    bool redo_available = false;
};

} // namespace looper
