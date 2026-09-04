#pragma once

#include "types.hpp"
#include <vector>
#include <mutex>
#include <atomic>
#include <cstddef>

namespace looper {

class LooperEngine {
public:
    explicit LooperEngine(const LooperConfig& config = LooperConfig());
    ~LooperEngine() = default;

    // Real-time audio processing (called from real-time audio thread)
    // in: pointer to nframes of input audio (mono channel 0)
    // out_left, out_right: pointers to nframes for stereo output
    void process(const float* in, float* out_left, float* out_right, size_t nframes);

    // Control triggers (can be called from main/GPIO thread)
    void triggerAction();      // Single footswitch logic: Rec -> Play -> Dub -> Play ...
    void triggerStop();        // Dedicated Stop
    void triggerClear();       // Clear all loops & reset to IDLE
    void triggerUndoRedo();    // Undo/Redo last overdub
    void toggleReverse();      // Toggle reverse playback
    void triggerFadeOut();     // Smooth fade-out to stop

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

    // Pre-allocated buffers to prevent dynamic allocation in audio thread
    static constexpr size_t MAX_LOOP_FRAMES = 48000 * 60 * 10; // 10 minutes max loop
    std::vector<float> base_track_;
    std::vector<float> overdub_layer_;
    std::vector<float> undo_backup_;

    size_t loop_length_{0};
    size_t playhead_{0};

    // Fade-out counters
    size_t fade_out_total_frames_{0};
    size_t fade_out_remaining_frames_{0};

    // Thread synchronization for buffer modifications
    mutable std::mutex control_mutex_;
};

} // namespace looper
