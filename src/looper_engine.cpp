#include "looper_engine.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <iostream>

namespace looper {

LooperEngine::LooperEngine(ControlQueue& ctrl_queue,
                           LoadQueue& load_queue,
                           BufferReturnQueue& return_queue,
                           SaveSlotQueue& save_slot_queue,
                           SaveReadyQueue& save_ready_queue,
                           const LooperConfig& config)
    : ctrl_queue_(ctrl_queue),
      load_queue_(load_queue),
      return_queue_(return_queue),
      save_slot_queue_(save_slot_queue),
      save_ready_queue_(save_ready_queue),
      config_(config),
      latency_compensation_(config.latency_compensation),
      monitor_mode_(config.monitor_mode) {
    base_track_ptr_ = new std::vector<float>();
    base_track_ptr_->reserve(MAX_LOOP_FRAMES);
    current_overdub_.reserve(MAX_LOOP_FRAMES);
    last_overdub_.reserve(MAX_LOOP_FRAMES);

    if (config_.pre_roll > 0) {
        pre_roll_buffer_.assign(config_.pre_roll, 0.0f);
    }
}

LooperEngine::~LooperEngine() {
    delete base_track_ptr_;
    base_track_ptr_ = nullptr;
}

void LooperEngine::process(const float* in, float* out_left, float* out_right, size_t nframes) {
    // 1. Process all pending control, load, and save-slot commands at the start of audio block (block boundary)
    processPendingCommands();

    MonitorMode mode = monitor_mode_.load(std::memory_order_relaxed);
    float dry_gain = (mode == MonitorMode::SOFTWARE) ? config_.dry_gain : 0.0f;
    float loop_gain = config_.loop_gain;
    LooperState current_state = state_.load(std::memory_order_relaxed);

    // Overdub latency compensation:
    // DIRECT_ANALOG mode: musician plays in time with dry sound, so overdub must be shifted back by K samples.
    // SOFTWARE mode: musician already hears delayed monitor and plays to it, so compensation is 0!
    uint32_t K = (mode == MonitorMode::DIRECT_ANALOG) ? latency_compensation_.load(std::memory_order_relaxed) : 0;

    float max_peak = 0.0f;

    for (size_t i = 0; i < nframes; ++i) {
        float in_sample = in[i];
        float abs_in = std::abs(in_sample);
        if (abs_in > max_peak) max_peak = abs_in;

        // Circular pre-roll rolling buffer
        if (!pre_roll_buffer_.empty()) {
            pre_roll_buffer_[pre_roll_idx_] = in_sample;
            pre_roll_idx_ = (pre_roll_idx_ + 1) % pre_roll_buffer_.size();
        }

        // Dry audio
        float left = in_sample * dry_gain;
        float right = in_sample * dry_gain;

        if (current_state == LooperState::RECORDING) {
            if (base_track_ptr_->size() < MAX_LOOP_FRAMES) {
                base_track_ptr_->push_back(in_sample);
            }
        } else if (current_state == LooperState::PLAYING || current_state == LooperState::OVERDUB) {
            if (loop_length_ > 0) {
                // Map transport playhead to memory index based on reverse flag
                size_t read_idx = is_reversed_.load(std::memory_order_relaxed) 
                                    ? (loop_length_ - 1 - playhead_) 
                                    : playhead_;

                float loop_sample = (*base_track_ptr_)[read_idx];

                // In OVERDUB, immediately hear previously recorded overdub audio!
                if (current_state == LooperState::OVERDUB && read_idx < current_overdub_.size()) {
                    loop_sample += current_overdub_[read_idx];
                }

                // Handle fade-out
                if (is_fading_out_.load(std::memory_order_relaxed)) {
                    if (fade_out_remaining_frames_ > 0 && fade_out_total_frames_ > 0) {
                        float fade_mult = static_cast<float>(fade_out_remaining_frames_) / static_cast<float>(fade_out_total_frames_);
                        loop_sample *= fade_mult;
                        --fade_out_remaining_frames_;
                    } else {
                        is_fading_out_.store(false, std::memory_order_relaxed);
                        state_.store(LooperState::STOPPED, std::memory_order_release);
                        current_state = LooperState::STOPPED;
                    }
                }

                loop_sample *= loop_gain;

                left += loop_sample;
                right += loop_sample;

                // Handle Overdub record with Transport-Domain Latency Compensation
                if (current_state == LooperState::OVERDUB) {
                    // 1. Shift backwards in transport time
                    size_t comp_playhead = 0;
                    if (playhead_ >= (K % loop_length_)) {
                        comp_playhead = playhead_ - (K % loop_length_);
                    } else {
                        comp_playhead = loop_length_ + playhead_ - (K % loop_length_);
                    }
                    comp_playhead %= loop_length_;

                    // 2. Map compensated transport time to memory index (handles reverse cleanly!)
                    size_t target_idx = is_reversed_.load(std::memory_order_relaxed)
                                          ? (loop_length_ - 1 - comp_playhead)
                                          : comp_playhead;

                    if (target_idx < current_overdub_.size()) {
                        current_overdub_[target_idx] += in_sample;
                    }
                }

                // Advance transport
                playhead_ = (playhead_ + 1) % loop_length_;
            }
        }

        // Final output stage limiter (Internal bus has full float headroom; clamp only at DAC)
        out_left[i] = std::clamp(left, -1.0f, 1.0f);
        out_right[i] = std::clamp(right, -1.0f, 1.0f);
    }

    in_peak_.store(max_peak, std::memory_order_relaxed);

    // Atomically publish status for UI thread (eliminates getStatus data race)
    status_playhead_.store(playhead_, std::memory_order_relaxed);
    status_loop_length_.store(loop_length_, std::memory_order_relaxed);
}

void LooperEngine::processPendingCommands() {
    // 1. Process control commands from UI / InputManager
    ControlCommand cmd;
    while (ctrl_queue_.pop(cmd)) {
        switch (cmd.type) {
            case ControlCommandType::ACTION: {
                LooperState current = state_.load(std::memory_order_relaxed);
                switch (current) {
                    case LooperState::IDLE: {
                        // Start recording base loop with pre-roll
                        base_track_ptr_->clear();
                        current_overdub_.clear();
                        last_overdub_.clear();
                        loop_length_ = 0;
                        playhead_ = 0;
                        has_undo_layer_ = false;
                        is_undone_ = false;
                        is_fading_out_.store(false, std::memory_order_relaxed);
                        undo_available_.store(false, std::memory_order_relaxed);
                        redo_available_.store(false, std::memory_order_relaxed);

                        // Copy pre-roll samples to catch initial transient attack
                        if (!pre_roll_buffer_.empty() && config_.pre_roll > 0) {
                            size_t buf_len = pre_roll_buffer_.size();
                            size_t n = std::min(static_cast<size_t>(config_.pre_roll), buf_len);
                            size_t start_pos = (pre_roll_idx_ + buf_len - n) % buf_len;
                            for (size_t i = 0; i < n; ++i) {
                                base_track_ptr_->push_back(pre_roll_buffer_[(start_pos + i) % buf_len]);
                            }
                        }

                        state_.store(LooperState::RECORDING, std::memory_order_release);
                        break;
                    }
                    case LooperState::RECORDING: {
                        if (!base_track_ptr_->empty()) {
                            loop_length_ = base_track_ptr_->size();
                            applyLoopSeamCrossfade();
                            playhead_ = 0;
                            current_overdub_.resize(loop_length_);
                            std::fill(current_overdub_.begin(), current_overdub_.end(), 0.0f);
                            state_.store(LooperState::PLAYING, std::memory_order_release);
                        } else {
                            state_.store(LooperState::IDLE, std::memory_order_release);
                        }
                        break;
                    }
                    case LooperState::PLAYING: {
                        // Start overdub
                        current_overdub_.resize(loop_length_);
                        std::fill(current_overdub_.begin(), current_overdub_.end(), 0.0f);
                        state_.store(LooperState::OVERDUB, std::memory_order_release);
                        break;
                    }
                    case LooperState::OVERDUB: {
                        // Commit overdub: additive into base_track without destructive clamp!
                        for (size_t i = 0; i < loop_length_; ++i) {
                            (*base_track_ptr_)[i] += current_overdub_[i];
                        }
                        // Zero-allocation swap preserving capacity
                        last_overdub_.swap(current_overdub_);
                        current_overdub_.resize(loop_length_);
                        std::fill(current_overdub_.begin(), current_overdub_.end(), 0.0f);

                        has_undo_layer_ = true;
                        is_undone_ = false;
                        undo_available_.store(true, std::memory_order_relaxed);
                        redo_available_.store(false, std::memory_order_relaxed);
                        state_.store(LooperState::PLAYING, std::memory_order_release);
                        break;
                    }
                    case LooperState::STOPPED: {
                        if (loop_length_ > 0) {
                            is_fading_out_.store(false, std::memory_order_relaxed);
                            state_.store(LooperState::PLAYING, std::memory_order_release);
                        }
                        break;
                    }
                    default:
                        break;
                }
                break;
            }
            case ControlCommandType::STOP: {
                LooperState current = state_.load(std::memory_order_relaxed);
                if (current == LooperState::RECORDING) {
                    if (!base_track_ptr_->empty()) {
                        loop_length_ = base_track_ptr_->size();
                        applyLoopSeamCrossfade();
                        playhead_ = 0;
                        state_.store(LooperState::STOPPED, std::memory_order_release);
                    } else {
                        state_.store(LooperState::IDLE, std::memory_order_release);
                    }
                } else if (current == LooperState::OVERDUB) {
                    for (size_t i = 0; i < loop_length_; ++i) {
                        (*base_track_ptr_)[i] += current_overdub_[i];
                    }
                    last_overdub_.swap(current_overdub_);
                    current_overdub_.resize(loop_length_);
                    std::fill(current_overdub_.begin(), current_overdub_.end(), 0.0f);

                    has_undo_layer_ = true;
                    is_undone_ = false;
                    undo_available_.store(true, std::memory_order_relaxed);
                    redo_available_.store(false, std::memory_order_relaxed);
                    state_.store(LooperState::STOPPED, std::memory_order_release);
                } else if (current == LooperState::PLAYING) {
                    state_.store(LooperState::STOPPED, std::memory_order_release);
                }
                is_fading_out_.store(false, std::memory_order_relaxed);
                break;
            }
            case ControlCommandType::CLEAR: {
                state_.store(LooperState::IDLE, std::memory_order_release);
                base_track_ptr_->clear();
                current_overdub_.clear();
                last_overdub_.clear();
                loop_length_ = 0;
                playhead_ = 0;
                has_undo_layer_ = false;
                is_undone_ = false;
                is_fading_out_.store(false, std::memory_order_relaxed);
                undo_available_.store(false, std::memory_order_relaxed);
                redo_available_.store(false, std::memory_order_relaxed);
                break;
            }
            case ControlCommandType::UNDO_REDO: {
                if (!has_undo_layer_ || loop_length_ == 0 || last_overdub_.size() != loop_length_) break;

                if (!is_undone_) {
                    // Non-destructive exact float subtraction!
                    for (size_t i = 0; i < loop_length_; ++i) {
                        (*base_track_ptr_)[i] -= last_overdub_[i];
                    }
                    is_undone_ = true;
                    undo_available_.store(false, std::memory_order_relaxed);
                    redo_available_.store(true, std::memory_order_relaxed);
                } else {
                    // Redo
                    for (size_t i = 0; i < loop_length_; ++i) {
                        (*base_track_ptr_)[i] += last_overdub_[i];
                    }
                    is_undone_ = false;
                    undo_available_.store(true, std::memory_order_relaxed);
                    redo_available_.store(false, std::memory_order_relaxed);
                }
                break;
            }
            case ControlCommandType::TOGGLE_REVERSE: {
                bool rev = is_reversed_.load(std::memory_order_relaxed);
                is_reversed_.store(!rev, std::memory_order_relaxed);
                break;
            }
            case ControlCommandType::TRIGGER_FADE: {
                LooperState current = state_.load(std::memory_order_relaxed);
                if (current == LooperState::PLAYING || current == LooperState::OVERDUB) {
                    fade_out_total_frames_ = static_cast<size_t>(config_.fade_out_sec * config_.sample_rate);
                    fade_out_remaining_frames_ = fade_out_total_frames_;
                    is_fading_out_.store(true, std::memory_order_relaxed);
                }
                break;
            }
            case ControlCommandType::SET_MONITOR_MODE: {
                auto new_mode = static_cast<MonitorMode>(cmd.int_param);
                monitor_mode_.store(new_mode, std::memory_order_relaxed);
                break;
            }
            case ControlCommandType::ADJUST_LATENCY: {
                int current = static_cast<int>(latency_compensation_.load(std::memory_order_relaxed));
                int updated = std::clamp(current + cmd.int_param, 0, 48000);
                latency_compensation_.store(static_cast<uint32_t>(updated), std::memory_order_relaxed);
                break;
            }
            case ControlCommandType::SET_LOOP_GAIN: {
                config_.loop_gain = std::max(0.0f, cmd.float_param);
                break;
            }
            case ControlCommandType::SET_DRY_GAIN: {
                config_.dry_gain = std::clamp(cmd.float_param, 0.0f, 1.0f);
                break;
            }
            default:
                break;
        }
    }

    // 2. Process Load commands from WavWorker (pointer swap, 0 malloc/free in audio thread)
    LoadCommand load_cmd;
    while (load_queue_.pop(load_cmd)) {
        if (load_cmd.buffer) {
            std::vector<float>* old_buf = base_track_ptr_;
            base_track_ptr_ = load_cmd.buffer;
            return_queue_.push(old_buf); // Old buffer reclaimed by WavWorker

            loop_length_ = base_track_ptr_->size();
            playhead_ = 0;
            current_overdub_.resize(loop_length_);
            std::fill(current_overdub_.begin(), current_overdub_.end(), 0.0f);
            last_overdub_.clear();
            has_undo_layer_ = false;
            is_undone_ = false;
            undo_available_.store(false, std::memory_order_relaxed);
            redo_available_.store(false, std::memory_order_relaxed);
            state_.store(LooperState::STOPPED, std::memory_order_release);
        }
    }

    // 3. Process Save snapshot slot commands from WavWorker
    SaveSlotCommand slot_cmd;
    while (save_slot_queue_.pop(slot_cmd)) {
        if (slot_cmd.buffer) {
            LooperState current = state_.load(std::memory_order_relaxed);
            // Allow save snapshot only when STOPPED and loop exists
            if (current == LooperState::STOPPED && base_track_ptr_ && !base_track_ptr_->empty()) {
                slot_cmd.buffer->resize(base_track_ptr_->size());
                std::copy(base_track_ptr_->begin(), base_track_ptr_->end(), slot_cmd.buffer->begin());
                save_ready_queue_.push(slot_cmd.buffer);
            } else {
                // Not stopped or empty loop: signal failure with empty buffer
                slot_cmd.buffer->clear();
                save_ready_queue_.push(slot_cmd.buffer);
            }
        }
    }
}

void LooperEngine::applyLoopSeamCrossfade() {
    if (!base_track_ptr_ || loop_length_ < 2) return;
    size_t xfade_len = std::min(static_cast<size_t>(config_.crossfade_samples), loop_length_ / 2);
    if (xfade_len == 0) return;

    size_t end_start = loop_length_ - xfade_len;

    for (size_t i = 0; i < xfade_len; ++i) {
        float alpha = static_cast<float>(i) / static_cast<float>(xfade_len);
        float start_sample = (*base_track_ptr_)[i];
        float end_sample = (*base_track_ptr_)[end_start + i];

        (*base_track_ptr_)[i] = (start_sample * alpha) + (end_sample * (1.0f - alpha));
    }
}

LooperStatus LooperEngine::getStatus() const {
    LooperStatus s;
    s.state = state_.load(std::memory_order_relaxed);
    s.monitor_mode = monitor_mode_.load(std::memory_order_relaxed);
    s.playhead_frames = status_playhead_.load(std::memory_order_relaxed);
    s.total_frames = status_loop_length_.load(std::memory_order_relaxed);
    s.current_sec = static_cast<float>(s.playhead_frames) / config_.sample_rate;
    s.total_sec = static_cast<float>(s.total_frames) / config_.sample_rate;
    s.in_peak = in_peak_.load(std::memory_order_relaxed);

    // Report effective latency based on monitor mode
    uint32_t configured = latency_compensation_.load(std::memory_order_relaxed);
    if (s.monitor_mode == MonitorMode::DIRECT_ANALOG) {
        s.effective_latency_samples = configured;
    } else {
        s.effective_latency_samples = 0; // Software monitoring mode has zero overdub compensation
    }
    s.effective_latency_ms = (static_cast<float>(s.effective_latency_samples) / config_.sample_rate) * 1000.0f;

    s.is_reversed = is_reversed_.load(std::memory_order_relaxed);
    s.is_fading_out = is_fading_out_.load(std::memory_order_relaxed);
    s.undo_available = undo_available_.load(std::memory_order_relaxed);
    s.redo_available = redo_available_.load(std::memory_order_relaxed);
    return s;
}

} // namespace looper
