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
      monitor_mode_(config.monitor_mode),
      status_loop_gain_(config.loop_gain) {
    base_track_ptr_ = new std::vector<float>(MAX_LOOP_FRAMES, 0.0f);
    last_layer_ptr_ = new std::vector<float>(MAX_LOOP_FRAMES, 0.0f);
    record_layer_ptr_ = new std::vector<float>(MAX_LOOP_FRAMES, 0.0f);

    if (config_.pre_roll > 0) {
        pre_roll_buffer_.assign(config_.pre_roll, 0.0f);
    }
}

LooperEngine::~LooperEngine() {
    delete base_track_ptr_;
    base_track_ptr_ = nullptr;
    delete last_layer_ptr_;
    last_layer_ptr_ = nullptr;
    delete record_layer_ptr_;
    record_layer_ptr_ = nullptr;
    delete merge_layer_ptr_;
    merge_layer_ptr_ = nullptr;
    delete overflow_return_buf_;
    overflow_return_buf_ = nullptr;
    delete overflow_return_buf2_;
    overflow_return_buf2_ = nullptr;
    delete overflow_return_buf3_;
    overflow_return_buf3_ = nullptr;
}

void LooperEngine::process(const float* in, float* out_left, float* out_right, size_t nframes, uint64_t block_start_ns) {
    // 1. Process pending Load and SaveSlot commands at block boundary
    processLoadAndSaveCommands();

    // 2. Advance background chunked WAV save if active (strictly bounded: 16384 samples / ~40us)
    if (active_save_buf_) {
        if (save_copy_total_ == 0) {
            if (save_ready_queue_.push(active_save_buf_)) {
                active_save_buf_ = nullptr;
            }
        } else {
            size_t chunk = std::min(SAVE_CHUNK_SIZE, save_copy_total_ - save_copy_progress_);
            bool include_last = (!is_undone_ && has_undo_layer_ && last_layer_ptr_ && loop_length_ >= save_copy_total_);
            bool include_merge = (merge_layer_ptr_ && merge_last_to_base_ && loop_length_ >= save_copy_total_);
            for (size_t c = 0; c < chunk; ++c) {
                size_t idx = save_copy_progress_ + c;
                float s = (*base_track_ptr_)[idx];
                if (include_last) {
                    s += (*last_layer_ptr_)[idx];
                }
                if (include_merge) {
                    s += (*merge_layer_ptr_)[idx];
                }
                (*active_save_buf_)[idx] = s;
            }
            save_copy_progress_ += chunk;
            if (save_copy_progress_ >= save_copy_total_) {
                if (save_ready_queue_.push(active_save_buf_)) {
                    active_save_buf_ = nullptr;
                }
            }
        }
    }

    // 3. Advance background chunked layer merge (strictly linear index 0..loop_length, 8192 samples / ~15us)
    if (pending_merge_active_ && loop_length_ > 0 && merge_layer_ptr_) {
        size_t chunk = std::min(MERGE_CHUNK_SIZE, pending_merge_frames_);
        for (size_t c = 0; c < chunk; ++c) {
            size_t idx = pending_merge_idx_++;
            if (idx < loop_length_) {
                if (merge_last_to_base_) {
                    (*base_track_ptr_)[idx] += (*merge_layer_ptr_)[idx];
                }
                (*merge_layer_ptr_)[idx] = 0.0f;
            }
        }
        pending_merge_frames_ -= chunk;
        if (pending_merge_frames_ == 0) {
            pending_merge_active_ = false;
            // Only assign to record_layer_ptr_ if not currently in OVERDUB!
            // (While in OVERDUB, record_layer_ptr_ is actively recording new take)
            if (state_.load(std::memory_order_relaxed) != LooperState::OVERDUB) {
                record_layer_ptr_ = merge_layer_ptr_; // 100% pre-zeroed scratchpad ready for next overdub!
                merge_layer_ptr_ = nullptr;
            }
        }
    }

    // 4. Collect and map pending control commands to sample offsets (Sample-Accurate Footswitch)
    ControlCommand pending_cmds[8];
    size_t num_cmds = 0;
    while (num_cmds < 8 && ctrl_queue_.pop(pending_cmds[num_cmds])) {
        auto& cmd = pending_cmds[num_cmds];
        if (cmd.timestamp_ns > 0 && cmd.sample_offset == 0 && block_start_ns > 0) {
            if (cmd.timestamp_ns <= block_start_ns) {
                cmd.sample_offset = 0;
            } else {
                uint64_t diff_ns = cmd.timestamp_ns - block_start_ns;
                uint64_t offset = (diff_ns * config_.sample_rate) / 1000000000ULL;
                if (offset >= nframes) offset = nframes - 1;
                cmd.sample_offset = static_cast<uint32_t>(offset);
            }
        }
        if (cmd.sample_offset >= nframes) {
            cmd.sample_offset = static_cast<uint32_t>(nframes - 1);
        }
        num_cmds++;
    }

    // Sort commands by intra-block sample offset
    for (size_t i = 1; i < num_cmds; ++i) {
        ControlCommand key = pending_cmds[i];
        int j = static_cast<int>(i) - 1;
        while (j >= 0 && pending_cmds[j].sample_offset > key.sample_offset) {
            pending_cmds[j + 1] = pending_cmds[j];
            --j;
        }
        pending_cmds[j + 1] = key;
    }

    // 5. Process audio in sub-slices, triggering commands sample-accurately mid-block
    float max_peak = 0.0f;
    size_t cur_frame = 0;
    for (size_t c = 0; c < num_cmds; ++c) {
        size_t target_offset = pending_cmds[c].sample_offset;
        if (target_offset > cur_frame && target_offset <= nframes) {
            processAudioSlice(in, out_left, out_right, cur_frame, target_offset, max_peak);
            cur_frame = target_offset;
        }
        executeControlCommand(pending_cmds[c]);
    }
    if (cur_frame < nframes) {
        processAudioSlice(in, out_left, out_right, cur_frame, nframes, max_peak);
    }

    in_peak_.store(max_peak, std::memory_order_relaxed);

    // Atomically publish status for UI thread (eliminates getStatus data race)
    status_playhead_.store(playhead_, std::memory_order_relaxed);
    status_loop_length_.store(loop_length_, std::memory_order_relaxed);
}

void LooperEngine::processAudioSlice(const float* in, float* out_left, float* out_right,
                                    size_t start_frame, size_t end_frame, float& max_peak) {
    MonitorMode mode = monitor_mode_.load(std::memory_order_relaxed);
    float dry_gain = (mode == MonitorMode::SOFTWARE) ? config_.dry_gain : 0.0f;
    float loop_gain = config_.loop_gain;
    LooperState current_state = state_.load(std::memory_order_relaxed);

    // Overdub latency compensation:
    // DIRECT_ANALOG mode: musician plays in time with dry sound, so overdub is shifted back by K samples.
    // SOFTWARE mode: musician already hears delayed software monitor and plays to it, so compensation is 0!
    uint32_t K = (mode == MonitorMode::DIRECT_ANALOG) ? latency_compensation_.load(std::memory_order_relaxed) : 0;

    for (size_t i = start_frame; i < end_frame; ++i) {
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
            if (loop_length_ < MAX_LOOP_FRAMES) {
                (*base_track_ptr_)[loop_length_] = in_sample;
                (*last_layer_ptr_)[loop_length_] = 0.0f;
                (*record_layer_ptr_)[loop_length_] = 0.0f;
                loop_length_++;
            }
        } else if (current_state == LooperState::PLAYING || current_state == LooperState::OVERDUB) {
            if (loop_length_ > 0) {
                // Map transport playhead to memory index based on reverse flag
                size_t read_idx = is_reversed_.load(std::memory_order_relaxed) 
                                    ? (loop_length_ - 1 - playhead_) 
                                    : playhead_;

                float loop_sample = 0.0f;
                if (read_idx < loop_length_) {
                    loop_sample = (*base_track_ptr_)[read_idx];
                }
                if (last_layer_ptr_ && !is_undone_ && read_idx < loop_length_) {
                    loop_sample += (*last_layer_ptr_)[read_idx];
                }
                if (merge_layer_ptr_ && merge_last_to_base_ && read_idx < loop_length_) {
                    loop_sample += (*merge_layer_ptr_)[read_idx];
                }
                if (current_state == LooperState::OVERDUB && record_layer_ptr_ && read_idx < loop_length_) {
                    loop_sample += (*record_layer_ptr_)[read_idx];
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

                // Handle Overdub with Transport-Domain Latency Compensation (strictly incremental O(1))
                if (current_state == LooperState::OVERDUB && record_layer_ptr_) {
                    size_t comp_playhead = 0;
                    if (playhead_ >= (K % loop_length_)) {
                        comp_playhead = playhead_ - (K % loop_length_);
                    } else {
                        comp_playhead = loop_length_ + playhead_ - (K % loop_length_);
                    }
                    comp_playhead %= loop_length_;

                    bool is_rev = is_reversed_.load(std::memory_order_relaxed);
                    size_t target_idx = is_rev ? (loop_length_ - 1 - comp_playhead) : comp_playhead;

                    if (target_idx < loop_length_) {
                        (*record_layer_ptr_)[target_idx] += in_sample;
                        overdub_frames_recorded_++;
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
}

void LooperEngine::abortActiveSave() {
    if (active_save_buf_) {
        active_save_buf_->clear();
        save_copy_total_ = 0;
        save_copy_progress_ = 0;
        if (save_ready_queue_.push(active_save_buf_)) {
            active_save_buf_ = nullptr;
        }
    }
}

void LooperEngine::commitOverdubTake() {
    if (merge_layer_ptr_) {
        if (!pending_merge_active_) {
            last_layer_ptr_ = record_layer_ptr_;
            record_layer_ptr_ = merge_layer_ptr_;
            merge_layer_ptr_ = nullptr;
        } else {
            last_layer_ptr_ = record_layer_ptr_;
            record_layer_ptr_ = nullptr;
        }
    } else {
        std::swap(last_layer_ptr_, record_layer_ptr_);
    }

    has_undo_layer_ = true;
    is_undone_ = false;
    undo_available_.store(true, std::memory_order_relaxed);
    redo_available_.store(false, std::memory_order_relaxed);
}

void LooperEngine::processPendingCommands() {
    processLoadAndSaveCommands();
    ControlCommand cmd;
    while (ctrl_queue_.pop(cmd)) {
        executeControlCommand(cmd);
    }
}

void LooperEngine::executeControlCommand(const ControlCommand& cmd) {
    switch (cmd.type) {
        case ControlCommandType::ACTION: {
            LooperState current = state_.load(std::memory_order_relaxed);
            switch (current) {
                case LooperState::IDLE: {
                    // Start recording base loop with pre-roll
                    loop_length_ = 0;
                    playhead_ = 0;
                    has_undo_layer_ = false;
                    is_undone_ = false;
                    overdub_frames_recorded_ = 0;
                    pending_merge_active_ = false;
                    pending_merge_frames_ = 0;
                    is_fading_out_.store(false, std::memory_order_relaxed);
                    undo_available_.store(false, std::memory_order_relaxed);
                    redo_available_.store(false, std::memory_order_relaxed);

                    // Pre-roll rolling buffer: capture note attack transient
                    if (!pre_roll_buffer_.empty() && config_.pre_roll > 0) {
                        size_t buf_len = pre_roll_buffer_.size();
                        size_t n = std::min(static_cast<size_t>(config_.pre_roll), buf_len);
                        size_t start_pos = (pre_roll_idx_ + buf_len - n) % buf_len;
                        for (size_t i = 0; i < n && loop_length_ < MAX_LOOP_FRAMES; ++i) {
                            (*base_track_ptr_)[loop_length_] = pre_roll_buffer_[(start_pos + i) % buf_len];
                            (*last_layer_ptr_)[loop_length_] = 0.0f;
                            (*record_layer_ptr_)[loop_length_] = 0.0f;
                            loop_length_++;
                        }
                    }

                    state_.store(LooperState::RECORDING, std::memory_order_release);
                    break;
                }
                case LooperState::RECORDING: {
                    if (loop_length_ > 0) {
                        // Musical length compensation: trim pre_roll from end so loop duration
                        // matches the exact musical rhythm between button presses!
                        if (config_.pre_roll > 0 && loop_length_ > config_.pre_roll) {
                            loop_length_ -= config_.pre_roll;
                        }
                        applyLoopSeamCrossfade();
                        playhead_ = 0;
                        overdub_frames_recorded_ = 0;
                        pending_merge_active_ = false;
                        pending_merge_frames_ = 0;
                        state_.store(LooperState::PLAYING, std::memory_order_release);
                    } else {
                        state_.store(LooperState::IDLE, std::memory_order_release);
                    }
                    break;
                }
                case LooperState::PLAYING: {
                    // Cannot start new overdub while scratchpad is still merging in background
                    if (record_layer_ptr_ == nullptr) {
                        break;
                    }
                    // Move previous overdub to merge_layer_ptr_ to begin background linear merge
                    if (has_undo_layer_ && last_layer_ptr_) {
                        merge_layer_ptr_ = last_layer_ptr_;
                        last_layer_ptr_ = nullptr;
                        merge_last_to_base_ = !is_undone_;
                        pending_merge_active_ = true;
                        pending_merge_idx_ = 0;
                        pending_merge_frames_ = loop_length_;
                    } else {
                        pending_merge_active_ = false;
                        pending_merge_frames_ = 0;
                    }
                    overdub_frames_recorded_ = 0;
                    state_.store(LooperState::OVERDUB, std::memory_order_release);
                    break;
                }
                case LooperState::OVERDUB: {
                    commitOverdubTake();
                    state_.store(LooperState::PLAYING, std::memory_order_release);
                    break;
                }
                case LooperState::STOPPED: {
                    if (loop_length_ > 0) {
                        abortActiveSave();
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
                if (loop_length_ > 0) {
                    if (config_.pre_roll > 0 && loop_length_ > config_.pre_roll) {
                        loop_length_ -= config_.pre_roll;
                    }
                    applyLoopSeamCrossfade();
                    playhead_ = 0;
                    overdub_frames_recorded_ = 0;
                    pending_merge_active_ = false;
                    pending_merge_frames_ = 0;
                    state_.store(LooperState::STOPPED, std::memory_order_release);
                } else {
                    state_.store(LooperState::IDLE, std::memory_order_release);
                }
            } else if (current == LooperState::OVERDUB) {
                commitOverdubTake();
                state_.store(LooperState::STOPPED, std::memory_order_release);
            } else if (current == LooperState::PLAYING) {
                state_.store(LooperState::STOPPED, std::memory_order_release);
            }
            is_fading_out_.store(false, std::memory_order_relaxed);
            break;
        }
        case ControlCommandType::CLEAR: {
            abortActiveSave();
            pending_merge_active_ = false;
            pending_merge_frames_ = 0;
            if (merge_layer_ptr_) {
                if (!record_layer_ptr_) {
                    record_layer_ptr_ = merge_layer_ptr_;
                } else if (!last_layer_ptr_) {
                    last_layer_ptr_ = merge_layer_ptr_;
                }
                merge_layer_ptr_ = nullptr;
            }

            state_.store(LooperState::IDLE, std::memory_order_release);
            loop_length_ = 0;
            playhead_ = 0;
            has_undo_layer_ = false;
            is_undone_ = false;
            overdub_frames_recorded_ = 0;
            is_fading_out_.store(false, std::memory_order_relaxed);
            undo_available_.store(false, std::memory_order_relaxed);
            redo_available_.store(false, std::memory_order_relaxed);
            break;
        }
        case ControlCommandType::UNDO_REDO: {
            // Disallow UNDO/REDO during active OVERDUB recording
            LooperState current = state_.load(std::memory_order_relaxed);
            if (current == LooperState::OVERDUB) break;

            abortActiveSave();
            if (!has_undo_layer_ || !last_layer_ptr_ || loop_length_ == 0) break;

            if (!is_undone_) {
                is_undone_ = true;
                undo_available_.store(false, std::memory_order_relaxed);
                redo_available_.store(true, std::memory_order_relaxed);
            } else {
                is_undone_ = false;
                undo_available_.store(true, std::memory_order_relaxed);
                redo_available_.store(false, std::memory_order_relaxed);
            }
            break;
        }
        case ControlCommandType::TOGGLE_REVERSE: {
            // Disallow reversing transport during active overdub to avoid discontinuous jumps
            LooperState current = state_.load(std::memory_order_relaxed);
            if (current == LooperState::OVERDUB) break;

            bool rev = is_reversed_.load(std::memory_order_relaxed);
            if (loop_length_ > 0) {
                // Reverse continuity: preserve physical track position (mem_pos) across direction flip
                size_t mem_pos = rev ? (loop_length_ - 1 - playhead_) : playhead_;
                bool new_rev = !rev;
                playhead_ = new_rev ? (loop_length_ - 1 - mem_pos) : mem_pos;
                is_reversed_.store(new_rev, std::memory_order_relaxed);
            } else {
                is_reversed_.store(!rev, std::memory_order_relaxed);
            }
            break;
        }
        case ControlCommandType::TRIGGER_FADE: {
            LooperState current = state_.load(std::memory_order_relaxed);
            if (current == LooperState::OVERDUB) {
                commitOverdubTake();
                state_.store(LooperState::PLAYING, std::memory_order_release);
                current = LooperState::PLAYING;
            }
            if (current == LooperState::PLAYING) {
                fade_out_total_frames_ = static_cast<size_t>(config_.fade_out_sec * config_.sample_rate);
                fade_out_remaining_frames_ = fade_out_total_frames_;
                is_fading_out_.store(true, std::memory_order_relaxed);
            }
            break;
        }
        case ControlCommandType::SET_MONITOR_MODE: {
            // Disallow monitor mode switch during active overdub
            LooperState current = state_.load(std::memory_order_relaxed);
            if (current == LooperState::OVERDUB) break;

            auto new_mode = static_cast<MonitorMode>(cmd.int_param);
            monitor_mode_.store(new_mode, std::memory_order_relaxed);
            break;
        }
        case ControlCommandType::ADJUST_LATENCY: {
            // Disallow adjusting latency during active overdub
            LooperState current = state_.load(std::memory_order_relaxed);
            if (current == LooperState::OVERDUB) break;

            int current_lat = static_cast<int>(latency_compensation_.load(std::memory_order_relaxed));
            int updated = std::clamp(current_lat + cmd.int_param, 0, 48000);
            latency_compensation_.store(static_cast<uint32_t>(updated), std::memory_order_relaxed);
            break;
        }
        case ControlCommandType::SET_LOOP_GAIN: {
            config_.loop_gain = std::max(0.0f, cmd.float_param);
            status_loop_gain_.store(config_.loop_gain, std::memory_order_relaxed);
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

void LooperEngine::processLoadAndSaveCommands() {
    // 1. Process Load commands from WavWorker (pure pointer swap: 0 malloc/free, 0 array loops in audio thread)
    LoadCommand load_cmd;
    while (load_queue_.pop(load_cmd)) {
        if (load_cmd.base_buffer && load_cmd.layer_buffer && load_cmd.record_buffer) {
            abortActiveSave();
            pending_merge_active_ = false;
            pending_merge_frames_ = 0;

            std::vector<float>* old_base = base_track_ptr_;
            std::vector<float>* old_layer = last_layer_ptr_;
            std::vector<float>* old_record = record_layer_ptr_;
            if (merge_layer_ptr_) {
                if (!old_layer) old_layer = merge_layer_ptr_;
                else if (!old_record) old_record = merge_layer_ptr_;
                merge_layer_ptr_ = nullptr;
            }

            base_track_ptr_ = load_cmd.base_buffer;
            last_layer_ptr_ = load_cmd.layer_buffer;
            record_layer_ptr_ = load_cmd.record_buffer;

            if (!return_queue_.push(old_base)) {
                if (!overflow_return_buf_) overflow_return_buf_ = old_base;
            }
            if (!return_queue_.push(old_layer)) {
                if (!overflow_return_buf2_) overflow_return_buf2_ = old_layer;
            }
            if (!return_queue_.push(old_record)) {
                if (!overflow_return_buf3_) overflow_return_buf3_ = old_record;
            }

            loop_length_ = std::min((load_cmd.loop_length > 0) ? load_cmd.loop_length : base_track_ptr_->size(), MAX_LOOP_FRAMES);
            playhead_ = 0;
            has_undo_layer_ = false;
            is_undone_ = false;
            overdub_frames_recorded_ = 0;
            undo_available_.store(false, std::memory_order_relaxed);
            redo_available_.store(false, std::memory_order_relaxed);
            state_.store(LooperState::STOPPED, std::memory_order_release);
        }
    }

    // Retry draining overflow buffers to worker (ZERO delete in audio thread!)
    if (overflow_return_buf_) {
        if (return_queue_.push(overflow_return_buf_)) {
            overflow_return_buf_ = nullptr;
        }
    }
    if (overflow_return_buf2_) {
        if (return_queue_.push(overflow_return_buf2_)) {
            overflow_return_buf2_ = nullptr;
        }
    }
    if (overflow_return_buf3_) {
        if (return_queue_.push(overflow_return_buf3_)) {
            overflow_return_buf3_ = nullptr;
        }
    }

    // 3. Process Save snapshot slot commands from WavWorker
    SaveSlotCommand slot_cmd;
    while (save_slot_queue_.pop(slot_cmd)) {
        if (slot_cmd.buffer) {
            LooperState current = state_.load(std::memory_order_relaxed);
            // Allow save snapshot only when STOPPED and loop exists
            if (current == LooperState::STOPPED && base_track_ptr_ && loop_length_ > 0) {
                active_save_buf_ = slot_cmd.buffer;
                save_copy_total_ = std::min(loop_length_, active_save_buf_->capacity());
                active_save_buf_->resize(save_copy_total_);
                save_copy_progress_ = 0;
            } else {
                // Not stopped or empty loop: signal failure with empty buffer
                slot_cmd.buffer->clear();
                if (!save_ready_queue_.push(slot_cmd.buffer)) {
                    active_save_buf_ = slot_cmd.buffer;
                    save_copy_total_ = 0;
                    save_copy_progress_ = 0;
                }
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

    s.configured_latency_samples = latency_compensation_.load(std::memory_order_relaxed);
    if (s.monitor_mode == MonitorMode::DIRECT_ANALOG) {
        s.effective_latency_samples = s.configured_latency_samples;
    } else {
        s.effective_latency_samples = 0; // Software monitoring mode has zero overdub compensation
    }
    s.effective_latency_ms = (static_cast<float>(s.effective_latency_samples) / config_.sample_rate) * 1000.0f;
    s.loop_gain = status_loop_gain_.load(std::memory_order_relaxed);

    s.is_reversed = is_reversed_.load(std::memory_order_relaxed);
    s.is_fading_out = is_fading_out_.load(std::memory_order_relaxed);
    s.undo_available = undo_available_.load(std::memory_order_relaxed);
    s.redo_available = redo_available_.load(std::memory_order_relaxed);
    return s;
}

} // namespace looper
