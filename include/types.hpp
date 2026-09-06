#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <atomic>

namespace looper {

constexpr uint32_t DEFAULT_SAMPLE_RATE = 48000;
constexpr uint32_t DEFAULT_PERIOD_SIZE = 128;   // ~2.67 ms buffer at 48kHz
constexpr uint32_t DEFAULT_PERIODS = 4;       // 4 periods = 512 frames buffer (~10.67 ms) for rock-solid USB stability
constexpr uint32_t DEFAULT_CHANNELS = 4;      // MiniFuse 1 requires 4 HW channels
constexpr uint32_t CROSSFADE_SAMPLES = 240;   // 5 ms crossfade at 48kHz to prevent clicks
constexpr uint32_t DEFAULT_DIRECT_LATENCY = 384; // Configured test default for MiniFuse 1 Direct Analog Monitoring
constexpr uint32_t PRE_ROLL_SAMPLES = 256;    // Pre-record rolling buffer (~5.3 ms) to catch note attack
constexpr size_t MAX_LOOP_FRAMES = 48000 * 60 * 5; // 5 minutes max loop buffer (57.6 MB per layer, 172.8 MB for 3 layers)

enum class LooperState {
    IDLE,       // No loop recorded, passthrough live audio
    RECORDING,  // Recording base layer
    PLAYING,    // Playing loop (+ passthrough live audio)
    OVERDUB,    // Recording new layer on top of loop (+ playing loop + passthrough)
    STOPPED     // Loop exists in memory, but playback is paused
};

enum class MonitorMode {
    DIRECT_ANALOG, // Direct hardware monitoring on audio card. Software dry = 0, overdub compensation = K
    SOFTWARE       // Software live monitoring via ALSA. Software dry = 1.0, overdub compensation = 0
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

inline const char* monitorModeToString(MonitorMode mode) {
    switch (mode) {
        case MonitorMode::DIRECT_ANALOG: return "ANALOG_DIRECT";
        case MonitorMode::SOFTWARE: return "SOFTWARE_DRY";
        default: return "UNKNOWN";
    }
}

struct LooperConfig {
    size_t max_loop_frames = MAX_LOOP_FRAMES;
    uint32_t sample_rate = DEFAULT_SAMPLE_RATE;
    uint32_t period_size = DEFAULT_PERIOD_SIZE;
    MonitorMode monitor_mode = MonitorMode::DIRECT_ANALOG; // Default to direct analog for zero-latency playing
    float dry_gain = 0.0f;       // 0.0 for DIRECT_ANALOG, 1.0 for SOFTWARE
    float loop_gain = 1.0f;      // Main loop playback volume
    float fade_out_sec = 3.0f;   // Fade-out duration in seconds
    uint32_t crossfade_samples = CROSSFADE_SAMPLES;
    uint32_t latency_compensation = DEFAULT_DIRECT_LATENCY;
    uint32_t pre_roll = PRE_ROLL_SAMPLES;
};

struct LooperStatus {
    LooperState state = LooperState::IDLE;
    MonitorMode monitor_mode = MonitorMode::DIRECT_ANALOG;
    size_t playhead_frames = 0;
    size_t total_frames = 0;
    float current_sec = 0.0f;
    float total_sec = 0.0f;
    float in_peak = 0.0f;
    bool in_clipped = false;
    uint32_t configured_latency_samples = DEFAULT_DIRECT_LATENCY;
    uint32_t effective_latency_samples = DEFAULT_DIRECT_LATENCY;
    float effective_latency_ms = 8.0f;
    float loop_gain = 1.0f;
    bool is_reversed = false;
    bool is_fading_out = false;
    bool undo_available = false;
    bool redo_available = false;
};

struct AudioConfig {
    std::string capture_device = "hw:CARD=M1,DEV=0";
    std::string playback_device = "hw:CARD=M1,DEV=0";
    uint32_t sample_rate = DEFAULT_SAMPLE_RATE;
    uint32_t period_size = DEFAULT_PERIOD_SIZE;
    uint32_t periods = DEFAULT_PERIODS;
    uint32_t capture_channels = DEFAULT_CHANNELS;
    uint32_t playback_channels = DEFAULT_CHANNELS;
    uint32_t capture_channel_index = 0;
};

struct PcmParams {
    uint32_t sample_rate = 0;
    uint32_t period_size = 0;
    uint32_t buffer_size = 0;
    uint32_t periods = 0;
    uint32_t channels = 0;
};

struct AudioTelemetry {
    std::atomic<uint64_t> capture_xruns{0};
    std::atomic<uint64_t> playback_xruns{0};
    std::atomic<uint64_t> suspends{0};
    std::atomic<uint64_t> disconnects{0};
    std::atomic<uint64_t> short_writes{0};
    std::atomic<uint64_t> recoveries{0};
    std::atomic<uint64_t> fatal_audio_errors{0};
    std::atomic<uint32_t> process_max_us{0};
    std::atomic<uint32_t> process_avg_us{0};
    std::atomic<bool> rt_priority_acquired{false};
};

struct AudioTelemetrySnapshot {
    uint64_t capture_xruns = 0;
    uint64_t playback_xruns = 0;
    uint64_t suspends = 0;
    uint64_t disconnects = 0;
    uint64_t short_writes = 0;
    uint64_t recoveries = 0;
    uint64_t fatal_audio_errors = 0;
    uint32_t process_max_us = 0;
    uint32_t process_avg_us = 0;
    bool rt_priority_acquired = false;
};

} // namespace looper
