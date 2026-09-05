#include "looper_engine.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <iostream>

namespace looper {

LooperEngine::LooperEngine(CommandQueue& cmd_queue, BufferReturnQueue& return_queue, SaveQueue& save_queue, const LooperConfig& config)
    : cmd_queue_(cmd_queue), return_queue_(return_queue), save_queue_(save_queue), config_(config),
      latency_compensation_(config.latency_compensation),
      monitor_mode_(config.monitor_mode) {
    base_track_.reserve(MAX_LOOP_FRAMES);
    current_overdub_.reserve(MAX_LOOP_FRAMES);
    last_overdub_.reserve(MAX_LOOP_FRAMES);

    pre_roll_buffer_.assign(config_.pre_roll, 0.0f);
}

void LooperEngine::process(const float* in, float* out_left, float* out_right, size_t nframes) {
    // 1. Process all pending control commands at the start of audio block (block boundary)
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
            if (base_track_.size() < MAX_LOOP_FRAMES) {
                base_track_.push_back(in_sample);
            }
        } else if (current_state == LooperState::PLAYING || current_state == LooperState::OVERDUB) {
            if (loop_length_ > 0) {
                // Map transport playhead to memory index based on reverse flag
                size_t read_idx = is_reversed_.load(std::memory_order_relaxed) 
                                    ? (loop_length_ - 1 - playhead_) 
                                    : playhead_;

                float loop_sample = base_track_[read_idx];

                // FEATURE 6: In OVERDUB, immediately hear previously recorded overdub audio!
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
}

void LooperEngine::processPendingCommands() {
    Command cmd;
    while (cmd_queue_.pop(cmd)) {
        switch (cmd.type) {
            case CommandType::ACTION: {
                LooperState current = state_.load(std::memory_order_relaxed);
                switch (current) {
                    case LooperState::IDLE: {
                        // Start recording base loop with pre-roll
                        base_track_.clear();
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
                                base_track_.push_back(pre_roll_buffer_[(start_pos + i) % buf_len]);
                            }
                        }

                        state_.store(LooperState::RECORDING, std::memory_order_release);
                        break;
                    }
                    case LooperState::RECORDING: {
                        if (!base_track_.empty()) {
                            loop_length_ = base_track_.size();
                            applyLoopSeamCrossfade();
                            playhead_ = 0;
                            current_overdub_.assign(loop_length_, 0.0f);
                            state_.store(LooperState::PLAYING, std::memory_order_release);
                        } else {
                            state_.store(LooperState::IDLE, std::memory_order_release);
                        }
                        break;
                    }
                    case LooperState::PLAYING: {
                        // Start overdub
                        current_overdub_.assign(loop_length_, 0.0f);
                        state_.store(LooperState::OVERDUB, std::memory_order_release);
                        break;
                    }
                    case LooperState::OVERDUB: {
                        // Commit overdub: additive into base_track without destructive clamp!
                        for (size_t i = 0; i < loop_length_; ++i) {
                            base_track_[i] += current_overdub_[i];
                        }
                        last_overdub_ = std::move(current_overdub_);
                        current_overdub_.assign(loop_length_, 0.0f);
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
                }
                break;
            }
            case CommandType::STOP: {
                LooperState current = state_.load(std::memory_order_relaxed);
                if (current == LooperState::RECORDING) {
                    if (!base_track_.empty()) {
                        loop_length_ = base_track_.size();
                        applyLoopSeamCrossfade();
                        playhead_ = 0;
                        state_.store(LooperState::STOPPED, std::memory_order_release);
                    } else {
                        state_.store(LooperState::IDLE, std::memory_order_release);
                    }
                } else if (current == LooperState::OVERDUB) {
                    for (size_t i = 0; i < loop_length_; ++i) {
                        base_track_[i] += current_overdub_[i];
                    }
                    last_overdub_ = std::move(current_overdub_);
                    current_overdub_.assign(loop_length_, 0.0f);
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
            case CommandType::CLEAR: {
                state_.store(LooperState::IDLE, std::memory_order_release);
                base_track_.clear();
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
            case CommandType::UNDO_REDO: {
                if (!has_undo_layer_ || loop_length_ == 0 || last_overdub_.size() != loop_length_) break;

                if (!is_undone_) {
                    // Non-destructive exact float subtraction!
                    for (size_t i = 0; i < loop_length_; ++i) {
                        base_track_[i] -= last_overdub_[i];
                    }
                    is_undone_ = true;
                    undo_available_.store(false, std::memory_order_relaxed);
                    redo_available_.store(true, std::memory_order_relaxed);
                } else {
                    // Redo
                    for (size_t i = 0; i < loop_length_; ++i) {
                        base_track_[i] += last_overdub_[i];
                    }
                    is_undone_ = false;
                    undo_available_.store(true, std::memory_order_relaxed);
                    redo_available_.store(false, std::memory_order_relaxed);
                }
                break;
            }
            case CommandType::TOGGLE_REVERSE: {
                bool rev = is_reversed_.load(std::memory_order_relaxed);
                is_reversed_.store(!rev, std::memory_order_relaxed);
                break;
            }
            case CommandType::TRIGGER_FADE: {
                LooperState current = state_.load(std::memory_order_relaxed);
                if (current == LooperState::PLAYING || current == LooperState::OVERDUB) {
                    fade_out_total_frames_ = static_cast<size_t>(config_.fade_out_sec * config_.sample_rate);
                    fade_out_remaining_frames_ = fade_out_total_frames_;
                    is_fading_out_.store(true, std::memory_order_relaxed);
                }
                break;
            }
            case CommandType::SET_MONITOR_MODE: {
                auto new_mode = static_cast<MonitorMode>(cmd.int_param);
                monitor_mode_.store(new_mode, std::memory_order_relaxed);
                break;
            }
            case CommandType::ADJUST_LATENCY: {
                int current = static_cast<int>(latency_compensation_.load(std::memory_order_relaxed));
                int updated = std::clamp(current + cmd.int_param, 0, 48000);
                latency_compensation_.store(static_cast<uint32_t>(updated), std::memory_order_relaxed);
                break;
            }
            case CommandType::SET_LOOP_GAIN: {
                config_.loop_gain = std::max(0.0f, cmd.float_param);
                break;
            }
            case CommandType::SET_DRY_GAIN: {
                config_.dry_gain = std::clamp(cmd.float_param, 0.0f, 1.0f);
                break;
            }
            case CommandType::LOAD_LOOP_READY: {
                if (cmd.buffer_payload && !cmd.buffer_payload->empty()) {
                    // Safely swap buffers!
                    auto old_buf = std::make_shared<std::vector<float>>();
                    base_track_.swap(*old_buf);
                    // Pass old buffer back to worker thread for asynchronous deallocation
                    return_queue_.push(old_buf);

                    base_track_ = std::move(*cmd.buffer_payload);
                    loop_length_ = base_track_.size();
                    playhead_ = 0;
                    current_overdub_.assign(loop_length_, 0.0f);
                    last_overdub_.clear();
                    has_undo_layer_ = false;
                    is_undone_ = false;
                    undo_available_.store(false, std::memory_order_relaxed);
                    redo_available_.store(false, std::memory_order_relaxed);
                    state_.store(LooperState::STOPPED, std::memory_order_release);
                }
                break;
            }
            case CommandType::REQUEST_SAVE_WAV: {
                if (!base_track_.empty() && cmd.buffer_payload) {
                    if (cmd.buffer_payload->size() >= base_track_.size()) {
                        std::copy(base_track_.begin(), base_track_.end(), cmd.buffer_payload->begin());
                        cmd.buffer_payload->resize(base_track_.size());
                        SaveRequest req;
                        req.filepath = std::move(cmd.string_param);
                        req.buffer = std::move(cmd.buffer_payload);
                        req.sample_rate = config_.sample_rate;
                        save_queue_.push(std::move(req));
                    }
                }
                break;
            }
        }
    }
}

void LooperEngine::applyLoopSeamCrossfade() {
    size_t xfade_len = std::min(static_cast<size_t>(config_.crossfade_samples), loop_length_ / 2);
    if (xfade_len == 0) return;

    size_t end_start = loop_length_ - xfade_len;

    for (size_t i = 0; i < xfade_len; ++i) {
        float alpha = static_cast<float>(i) / static_cast<float>(xfade_len);
        float start_sample = base_track_[i];
        float end_sample = base_track_[end_start + i];

        base_track_[i] = (start_sample * alpha) + (end_sample * (1.0f - alpha));
    }
}

LooperStatus LooperEngine::getStatus() const {
    LooperStatus s;
    s.state = state_.load(std::memory_order_relaxed);
    s.monitor_mode = monitor_mode_.load(std::memory_order_relaxed);
    s.playhead_frames = playhead_;
    s.total_frames = loop_length_;
    s.current_sec = static_cast<float>(playhead_) / config_.sample_rate;
    s.total_sec = static_cast<float>(loop_length_) / config_.sample_rate;
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
