#pragma once

#include "types.hpp"
#include "command_queue.hpp"
#include <vector>
#include <atomic>
#include <cstddef>
#include <memory>

namespace looper {

class LooperEngine {
public:
    LooperEngine(ControlQueue& ctrl_queue,
                 LoadQueue& load_queue,
                 BufferReturnQueue& return_queue,
                 SaveSlotQueue& save_slot_queue,
                 SaveReadyQueue& save_ready_queue,
                 const LooperConfig& config = LooperConfig());
    ~LooperEngine();

    // REAL-TIME AUDIO PROCESSING (Runs strictly on real-time audio thread)
    void process(const float* in, float* out_left, float* out_right, size_t nframes);

    // Thread-safe query for UI
    LooperStatus getStatus() const;

private:
    // Process pending commands on audio block boundaries
    void processPendingCommands();
    void applyLoopSeamCrossfade();

    ControlQueue& ctrl_queue_;
    LoadQueue& load_queue_;
    BufferReturnQueue& return_queue_;
    SaveSlotQueue& save_slot_queue_;
    SaveReadyQueue& save_ready_queue_;

    LooperConfig config_;

    std::atomic<LooperState> state_{LooperState::IDLE};
    std::atomic<bool> is_reversed_{false};
    std::atomic<bool> is_fading_out_{false};
    std::atomic<bool> undo_available_{false};
    std::atomic<bool> redo_available_{false};
    std::atomic<float> in_peak_{0.0f};
    std::atomic<uint32_t> latency_compensation_{DEFAULT_DIRECT_LATENCY};
    std::atomic<MonitorMode> monitor_mode_{MonitorMode::SOFTWARE};

    // Atomic status mirrors for UI to avoid data races
    std::atomic<size_t> status_playhead_{0};
    std::atomic<size_t> status_loop_length_{0};

    // Pre-allocated loop buffers (owned and accessed exclusively by real-time audio thread)
    std::vector<float>* base_track_ptr_{nullptr};
    std::vector<float> last_layer_;
    bool has_undo_layer_{false};
    bool is_undone_{false};

    // Incremental overdub tracking (eliminates O(N) operations)
    size_t overdub_frames_recorded_{0};
    bool pending_merge_active_{false};
    size_t pending_merge_idx_{0};
    size_t pending_merge_frames_{0};

    // Chunked non-blocking WAV save snapshot state
    static constexpr size_t SAVE_CHUNK_SIZE = 4096;
    std::vector<float>* active_save_buf_{nullptr};
    size_t save_copy_progress_{0};
    size_t save_copy_total_{0};

    // Overflow return buffer fallback in case return queue is momentarily full
    std::vector<float>* overflow_return_buf_{nullptr};

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
