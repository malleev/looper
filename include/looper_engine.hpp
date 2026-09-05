#pragma once

#include "types.hpp"
#include <vector>
#include <mutex>
#include <atomic>
#include <cstddef>
#include <string>

namespace looper {

class LooperEngine {
public:
    explicit LooperEngine(const LooperConfig& config = LooperConfig());
    ~LooperEngine() = default;

    // Real-time audio processing (called from real-time audio thread)
    void process(const float* in, float* out_left, float* out_right, size_t nframes);

    // Control triggers
    void triggerAction();      // Rec -> Play -> Dub -> Play ...
    void triggerStop();        // Dedicated Stop
    void triggerClear();       // Clear all loops & reset to IDLE
    void triggerUndoRedo();    // Undo/Redo last overdub
    void toggleReverse();      // Toggle reverse playback
    void triggerFadeOut();     // Smooth fade-out to stop

    // WAV Storage
    bool saveToWav(const std::string& filepath);
    bool loadFromWav(const std::string& filepath);

    // Latency compensation
    void adjustLatency(int delta_samples);
    uint32_t getLatency() const { return latency_compensation_.load(); }

    // Status query
    LooperStatus getStatus() const;
    void setDryGain(float gain);
    void setLoopGain(float gain);
    void setFadeOutSec(float sec);

private:
    void applyLoopSeamCrossfade();

    LooperConfig config_;

    std::atomic<LooperState> state_{LooperState::IDLE};
    std::atomic<bool> is_reversed_{false};
    std::atomic<bool> is_fading_out_{false};
    std::atomic<bool> undo_available_{false};
    std::atomic<bool> redo_available_{false};
    std::atomic<float> in_peak_{0.0f};
    std::atomic<uint32_t> latency_compensation_{DEFAULT_LATENCY_COMPENSATION};

    // Pre-allocated buffers
    static constexpr size_t MAX_LOOP_FRAMES = 48000 * 60 * 10; // 10 minutes max loop
    std::vector<float> base_track_;
    std::vector<float> overdub_layer_;
    std::vector<float> undo_backup_;

    // Pre-roll rolling history buffer to catch initial note attack
    std::vector<float> pre_roll_buffer_;
    size_t pre_roll_idx_{0};

    size_t loop_length_{0};
    size_t playhead_{0};

    // Fade-out counters
    size_t fade_out_total_frames_{0};
    size_t fade_out_remaining_frames_{0};

    // Thread synchronization
    mutable std::mutex control_mutex_;
};

} // namespace looper
