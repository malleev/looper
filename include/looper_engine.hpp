#pragma once

#include "types.hpp"
#include "command_queue.hpp"
#include <vector>
#include <atomic>
#include <cstddef>
#include <memory>
#include <functional>

namespace looper {

class LooperEngine {
public:
    LooperEngine(CommandQueue& cmd_queue, BufferReturnQueue& return_queue, SaveQueue& save_queue, const LooperConfig& config = LooperConfig());
    ~LooperEngine() = default;

    // REAL-TIME AUDIO PROCESSING (Runs strictly on real-time audio thread)
    void process(const float* in, float* out_left, float* out_right, size_t nframes);

    // Thread-safe query for UI
    LooperStatus getStatus() const;

private:
    // Process pending commands on audio block boundaries
    void processPendingCommands();
    void applyLoopSeamCrossfade();

    CommandQueue& cmd_queue_;
    BufferReturnQueue& return_queue_;
    SaveQueue& save_queue_;

    LooperConfig config_;

    std::atomic<LooperState> state_{LooperState::IDLE};
    std::atomic<bool> is_reversed_{false};
    std::atomic<bool> is_fading_out_{false};
    std::atomic<bool> undo_available_{false};
    std::atomic<bool> redo_available_{false};
    std::atomic<float> in_peak_{0.0f};
    std::atomic<uint32_t> latency_compensation_{DEFAULT_DIRECT_LATENCY};
    std::atomic<MonitorMode> monitor_mode_{MonitorMode::DIRECT_ANALOG};

    // Pre-allocated loop buffers (owned and accessed exclusively by real-time audio thread)
    static constexpr size_t MAX_LOOP_FRAMES = 48000 * 60 * 10; // 10 minutes max loop
    std::vector<float> base_track_;
    std::vector<float> current_overdub_;
    std::vector<float> last_overdub_;
    bool has_undo_layer_{false};
    bool is_undone_{false};

    // Rolling circular pre-roll buffer
    std::vector<float> pre_roll_buffer_;
    size_t pre_roll_idx_{0};

    size_t loop_length_{0};
    size_t playhead_{0};

    // Fade-out counters
    size_t fade_out_total_frames_{0};
    size_t fade_out_remaining_frames_{0};
};

} // namespace looper
