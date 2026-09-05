#pragma once

#include "types.hpp"
#include "command_queue.hpp"
#include <vector>
#include <array>
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
    void process(const float* in, float* out_left, float* out_right, size_t nframes,
                 uint64_t block_start_ns = 0, uint64_t block_duration_ns = 0);

    // Thread-safe query for UI
    LooperStatus getStatus() const;

private:
    // Process pending commands on audio block boundaries
    void processPendingCommands();
    void processLoadAndSaveCommands();
    void executeControlCommand(const ControlCommand& cmd);
    void processAudioSlice(const float* in, float* out_left, float* out_right,
                           size_t start_frame, size_t end_frame, float& max_peak);
    void applyLoopSeamCrossfade(std::vector<float>* track);
    void abortActiveSave();
    void commitOverdubTake();

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
    std::atomic<float> status_loop_gain_{1.0f};

    // Atomic status mirrors for UI to avoid data races
    std::atomic<size_t> status_playhead_{0};
    std::atomic<size_t> status_loop_length_{0};

    // Pre-allocated loop buffers (owned and accessed exclusively by real-time audio thread)
    std::vector<float>* base_track_ptr_{nullptr};
    std::vector<float>* last_layer_ptr_{nullptr};
    std::vector<float>* record_layer_ptr_{nullptr};
    std::vector<float>* merge_layer_ptr_{nullptr};
    bool has_undo_layer_{false};
    bool is_undone_{false};

    // Live overdub session state
    size_t overdub_frames_recorded_{0};

    // Background chunked merge of previous last_layer into base_track
    static constexpr size_t MERGE_CHUNK_SIZE = 8192;
    bool pending_merge_active_{false};
    size_t pending_merge_idx_{0};
    size_t pending_merge_frames_{0};
    bool merge_last_to_base_{false};

    // Chunked non-blocking WAV save snapshot state
    static constexpr size_t SAVE_CHUNK_SIZE = 16384;
    std::vector<float>* active_save_buf_{nullptr};
    size_t save_copy_progress_{0};
    size_t save_copy_total_{0};

    // Overflow return buffer fallbacks in case return queue is momentarily full
    std::vector<float>* overflow_return_buf_{nullptr};
    std::vector<float>* overflow_return_buf2_{nullptr};
    std::vector<float>* overflow_return_buf3_{nullptr};

    // Rolling circular pre-roll buffer
    std::vector<float> pre_roll_buffer_;
    size_t pre_roll_idx_{0};

    size_t loop_length_{0};
    size_t playhead_{0};

    // Fade-out counters
    size_t fade_out_total_frames_{0};
    size_t fade_out_remaining_frames_{0};

    // Deferred future command storage for commands with timestamps in future blocks
    static constexpr size_t MAX_DEFERRED_CMDS = 64;
    std::array<ControlCommand, MAX_DEFERRED_CMDS> deferred_cmds_{};
    size_t deferred_count_{0};
};

} // namespace looper
