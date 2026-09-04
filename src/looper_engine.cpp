#include "looper_engine.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace looper {

LooperEngine::LooperEngine(const LooperConfig& config)
    : config_(config) {
    // Pre-allocate memory to guarantee no reallocation or syscalls in audio callback
    base_track_.reserve(MAX_LOOP_FRAMES);
    overdub_layer_.reserve(MAX_LOOP_FRAMES);
    undo_backup_.reserve(MAX_LOOP_FRAMES);
}

void LooperEngine::process(const float* in, float* out_left, float* out_right, size_t nframes) {
    float dry_gain = config_.dry_gain;
    float loop_gain = config_.loop_gain;
    LooperState current_state = state_.load(std::memory_order_relaxed);

    for (size_t i = 0; i < nframes; ++i) {
        float in_sample = in[i];

        // Dry pass-through (attenuated or full)
        float left = in_sample * dry_gain;
        float right = in_sample * dry_gain;

        if (current_state == LooperState::RECORDING) {
            if (base_track_.size() < MAX_LOOP_FRAMES) {
                base_track_.push_back(in_sample);
            }
        } else if (current_state == LooperState::PLAYING || current_state == LooperState::OVERDUB) {
            if (loop_length_ > 0) {
                size_t read_idx = is_reversed_.load(std::memory_order_relaxed) 
                                    ? (loop_length_ - 1 - playhead_) 
                                    : playhead_;

                float loop_sample = base_track_[read_idx];

                // Handle fade-out calculation
                if (is_fading_out_.load(std::memory_order_relaxed)) {
                    if (fade_out_remaining_frames_ > 0) {
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

                // Handle overdubbing: record directly into overdub layer
                if (current_state == LooperState::OVERDUB) {
                    if (read_idx < overdub_layer_.size()) {
                        overdub_layer_[read_idx] += in_sample;
                    }
                }

                // Advance playhead
                playhead_ = (playhead_ + 1) % loop_length_;
            }
        }

        // Soft limit / clamp to avoid digital wrapping
        out_left[i] = std::clamp(left, -1.0f, 1.0f);
        out_right[i] = std::clamp(right, -1.0f, 1.0f);
    }
}

void LooperEngine::triggerAction() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    LooperState current = state_.load(std::memory_order_relaxed);

    switch (current) {
        case LooperState::IDLE: {
            // Start recording base loop
            base_track_.clear();
            overdub_layer_.clear();
            undo_backup_.clear();
            loop_length_ = 0;
            playhead_ = 0;
            is_fading_out_.store(false, std::memory_order_relaxed);
            undo_available_.store(false, std::memory_order_relaxed);
            redo_available_.store(false, std::memory_order_relaxed);
            state_.store(LooperState::RECORDING, std::memory_order_release);
            break;
        }
        case LooperState::RECORDING: {
            // Finish base loop -> Transition to PLAYING
            if (!base_track_.empty()) {
                loop_length_ = base_track_.size();
                applyLoopSeamCrossfade();
                playhead_ = 0;
                overdub_layer_.assign(loop_length_, 0.0f);
                state_.store(LooperState::PLAYING, std::memory_order_release);
            } else {
                state_.store(LooperState::IDLE, std::memory_order_release);
            }
            break;
        }
        case LooperState::PLAYING: {
            // Start overdubbing
            // Save backup for undo
            undo_backup_ = base_track_;
            undo_available_.store(true, std::memory_order_relaxed);
            redo_available_.store(false, std::memory_order_relaxed);

            overdub_layer_.assign(loop_length_, 0.0f);
            state_.store(LooperState::OVERDUB, std::memory_order_release);
            break;
        }
        case LooperState::OVERDUB: {
            // Finish overdubbing -> Mix overdub into base track -> Return to PLAYING
            for (size_t i = 0; i < loop_length_; ++i) {
                base_track_[i] = std::clamp(base_track_[i] + overdub_layer_[i], -1.0f, 1.0f);
            }
            state_.store(LooperState::PLAYING, std::memory_order_release);
            break;
        }
        case LooperState::STOPPED: {
            // Resume playback
            if (loop_length_ > 0) {
                is_fading_out_.store(false, std::memory_order_relaxed);
                state_.store(LooperState::PLAYING, std::memory_order_release);
            }
            break;
        }
    }
}

void LooperEngine::triggerStop() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    LooperState current = state_.load(std::memory_order_relaxed);

    if (current == LooperState::RECORDING) {
        // If stopped during initial record, close loop and stay stopped
        if (!base_track_.empty()) {
            loop_length_ = base_track_.size();
            applyLoopSeamCrossfade();
            playhead_ = 0;
            state_.store(LooperState::STOPPED, std::memory_order_release);
        } else {
            state_.store(LooperState::IDLE, std::memory_order_release);
        }
    } else if (current == LooperState::OVERDUB) {
        // Commit active overdub before stopping
        for (size_t i = 0; i < loop_length_; ++i) {
            base_track_[i] = std::clamp(base_track_[i] + overdub_layer_[i], -1.0f, 1.0f);
        }
        state_.store(LooperState::STOPPED, std::memory_order_release);
    } else if (current == LooperState::PLAYING) {
        state_.store(LooperState::STOPPED, std::memory_order_release);
    }
    is_fading_out_.store(false, std::memory_order_relaxed);
}

void LooperEngine::triggerClear() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    state_.store(LooperState::IDLE, std::memory_order_release);
    base_track_.clear();
    overdub_layer_.clear();
    undo_backup_.clear();
    loop_length_ = 0;
    playhead_ = 0;
    is_fading_out_.store(false, std::memory_order_relaxed);
    undo_available_.store(false, std::memory_order_relaxed);
    redo_available_.store(false, std::memory_order_relaxed);
}

void LooperEngine::triggerUndoRedo() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (undo_backup_.empty() || loop_length_ == 0) return;

    // Swap active base track with undo backup
    base_track_.swap(undo_backup_);
    
    // Toggle undo / redo flags
    bool was_undo = undo_available_.load(std::memory_order_relaxed);
    undo_available_.store(!was_undo, std::memory_order_relaxed);
    redo_available_.store(was_undo, std::memory_order_relaxed);
}

void LooperEngine::toggleReverse() {
    bool rev = is_reversed_.load(std::memory_order_relaxed);
    is_reversed_.store(!rev, std::memory_order_relaxed);
}

void LooperEngine::triggerFadeOut() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    LooperState current = state_.load(std::memory_order_relaxed);
    if (current != LooperState::PLAYING && current != LooperState::OVERDUB) return;

    fade_out_total_frames_ = static_cast<size_t>(config_.fade_out_sec * config_.sample_rate);
    fade_out_remaining_frames_ = fade_out_total_frames_;
    is_fading_out_.store(true, std::memory_order_relaxed);
}

void LooperEngine::applyLoopSeamCrossfade() {
    // Crossfade loop seam to eliminate clicks/pops
    size_t xfade_len = std::min(static_cast<size_t>(config_.crossfade_samples), loop_length_ / 2);
    if (xfade_len == 0) return;

    size_t end_start = loop_length_ - xfade_len;

    for (size_t i = 0; i < xfade_len; ++i) {
        float alpha = static_cast<float>(i) / static_cast<float>(xfade_len); // 0.0 -> 1.0
        // Crossfade beginning with end
        float start_sample = base_track_[i];
        float end_sample = base_track_[end_start + i];

        base_track_[i] = (start_sample * alpha) + (end_sample * (1.0f - alpha));
    }
}

LooperStatus LooperEngine::getStatus() const {
    LooperStatus s;
    s.state = state_.load(std::memory_order_relaxed);
    s.playhead_frames = playhead_;
    s.total_frames = loop_length_;
    s.current_sec = static_cast<float>(playhead_) / config_.sample_rate;
    s.total_sec = static_cast<float>(loop_length_) / config_.sample_rate;
    s.is_reversed = is_reversed_.load(std::memory_order_relaxed);
    s.is_fading_out = is_fading_out_.load(std::memory_order_relaxed);
    s.undo_available = undo_available_.load(std::memory_order_relaxed);
    s.redo_available = redo_available_.load(std::memory_order_relaxed);
    return s;
}

void LooperEngine::setDryGain(float gain) {
    config_.dry_gain = std::clamp(gain, 0.0f, 1.0f);
}

void LooperEngine::setLoopGain(float gain) {
    config_.loop_gain = std::max(0.0f, gain);
}

void LooperEngine::setFadeOutSec(float sec) {
    config_.fade_out_sec = std::max(0.5f, sec);
}

} // namespace looper
