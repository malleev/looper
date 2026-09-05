#include "latency_calibrator.hpp"
#include <alsa/asoundlib.h>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <thread>
#include <chrono>

namespace looper {

int32_t LatencyCalibrator::detectPulseOffset(const float* signal, size_t length, float threshold) {
    if (!signal || length == 0) return -1;
    for (size_t i = 0; i < length; ++i) {
        if (std::abs(signal[i]) >= threshold) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

uint32_t LatencyCalibrator::calculateMedian(std::vector<uint32_t> samples) {
    if (samples.empty()) return 0;
    std::sort(samples.begin(), samples.end());
    size_t mid = samples.size() / 2;
    if (samples.size() % 2 == 0) {
        return (samples[mid - 1] + samples[mid]) / 2;
    } else {
        return samples[mid];
    }
}

namespace {

static bool setupAlsa(snd_pcm_t** handle, const std::string& dev, snd_pcm_stream_t stream,
                      uint32_t rate, uint32_t period_size, uint32_t periods, uint32_t channels) {
    int err = snd_pcm_open(handle, dev.c_str(), stream, 0);
    if (err < 0) {
        std::cerr << "[CALIBRATOR] Cannot open " << dev << ": " << snd_strerror(err) << std::endl;
        return false;
    }

    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    if (snd_pcm_hw_params_any(*handle, hw) < 0) return false;
    if (snd_pcm_hw_params_set_access(*handle, hw, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) return false;
    if (snd_pcm_hw_params_set_format(*handle, hw, SND_PCM_FORMAT_S32_LE) < 0) return false;
    if (snd_pcm_hw_params_set_channels(*handle, hw, channels) < 0) return false;

    unsigned int r = rate;
    if (snd_pcm_hw_params_set_rate_near(*handle, hw, &r, 0) < 0 || r != rate) return false;

    snd_pcm_uframes_t p = period_size;
    if (snd_pcm_hw_params_set_period_size_near(*handle, hw, &p, 0) < 0 || p != period_size) return false;

    unsigned int pds = periods;
    if (snd_pcm_hw_params_set_periods_near(*handle, hw, &pds, 0) < 0) return false;

    if (snd_pcm_hw_params(*handle, hw) < 0) return false;

    snd_pcm_sw_params_t* sw;
    snd_pcm_sw_params_alloca(&sw);
    if (snd_pcm_sw_params_current(*handle, sw) < 0) return false;
    if (snd_pcm_sw_params_set_avail_min(*handle, sw, period_size) < 0) return false;
    if (stream == SND_PCM_STREAM_PLAYBACK) {
        if (snd_pcm_sw_params_set_start_threshold(*handle, sw, 2 * period_size) < 0) return false;
    } else {
        if (snd_pcm_sw_params_set_start_threshold(*handle, sw, 1) < 0) return false;
    }
    if (snd_pcm_sw_params(*handle, sw) < 0) return false;
    if (snd_pcm_prepare(*handle) < 0) return false;
    return true;
}

static bool writeAll(snd_pcm_t* handle, const int32_t* data, size_t frames, size_t channels) {
    size_t left = frames;
    const int32_t* ptr = data;
    while (left > 0) {
        snd_pcm_sframes_t written = snd_pcm_writei(handle, ptr, left);
        if (written > 0) {
            left -= written;
            ptr += written * channels;
        } else if (written == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

CalibrationResult LatencyCalibrator::run(const AudioConfig& audio_cfg, uint32_t num_pulses) {
    CalibrationResult res;
    snd_pcm_t* cap_handle = nullptr;
    snd_pcm_t* play_handle = nullptr;

    const uint32_t rate = audio_cfg.sample_rate;
    const uint32_t period = audio_cfg.period_size;
    const uint32_t periods = audio_cfg.periods;
    const uint32_t cap_ch = audio_cfg.capture_channels;
    const uint32_t play_ch = audio_cfg.playback_channels;
    const uint32_t cap_idx = audio_cfg.capture_channel_index;

    // Validate capture channel index
    if (cap_idx >= cap_ch) {
        res.message = "Capture channel index (" + std::to_string(cap_idx) + 
                      ") exceeds available capture channels (" + std::to_string(cap_ch) + ")";
        return res;
    }

    if (!setupAlsa(&cap_handle, audio_cfg.capture_device, SND_PCM_STREAM_CAPTURE, rate, period, periods, cap_ch)) {
        res.message = "Failed to open capture device: " + audio_cfg.capture_device;
        if (cap_handle) snd_pcm_close(cap_handle);
        return res;
    }
    if (!setupAlsa(&play_handle, audio_cfg.playback_device, SND_PCM_STREAM_PLAYBACK, rate, period, periods, play_ch)) {
        res.message = "Failed to open playback device: " + audio_cfg.playback_device;
        snd_pcm_close(cap_handle);
        if (play_handle) snd_pcm_close(play_handle);
        return res;
    }

    // Attempt hardware-synchronized linking if driver/device supports it
    bool is_linked = (snd_pcm_link(cap_handle, play_handle) == 0);
    if (is_linked) {
        std::cout << "[CALIBRATOR] Linked capture and playback streams via snd_pcm_link." << std::endl;
    } else {
        std::cout << "[CALIBRATOR] Note: snd_pcm_link unsupported by device; using synchronized duplex start." << std::endl;
    }

    std::vector<int32_t> in_buf(period * cap_ch, 0);
    std::vector<int32_t> pulse_buf(period * play_ch, 0);
    std::vector<int32_t> silence_buf(period * play_ch, 0);
    std::vector<float> mono_in(period, 0.0f);

    constexpr float INT32_TO_FLOAT = 1.0f / 2147483648.0f;
    constexpr int32_t PULSE_AMPLITUDE = static_cast<int32_t>(0.80f * 2147483647.0f);

    // Prepare pulse buffer: impulse on Left/ch0 (and ch1 if stereo)
    pulse_buf[0] = PULSE_AMPLITUDE;
    if (play_ch > 1) {
        pulse_buf[1] = PULSE_AMPLITUDE;
    }

    // Prime playback buffer with 2 periods of silence cushion (lockstep duplex base)
    if (!writeAll(play_handle, silence_buf.data(), period, play_ch) ||
        !writeAll(play_handle, silence_buf.data(), period, play_ch)) {
        res.message = "Failed to prime playback buffer with silence cushion";
        if (is_linked) snd_pcm_unlink(cap_handle);
        snd_pcm_close(cap_handle);
        snd_pcm_close(play_handle);
        return res;
    }

    uint64_t total_played_samples = 2 * period;
    uint64_t total_recorded_samples = 0;

    int err = snd_pcm_start(cap_handle);
    if (err < 0) {
        res.message = "Failed to start capture PCM stream: " + std::string(snd_strerror(err));
        if (is_linked) snd_pcm_unlink(cap_handle);
        snd_pcm_close(cap_handle);
        snd_pcm_close(play_handle);
        return res;
    }

    std::vector<uint32_t> measurements;
    measurements.reserve(num_pulses);

    enum class CalState {
        WARMUP,
        WAITING_FOR_RESPONSE,
        COOLDOWN
    };

    CalState state = CalState::WARMUP;
    int state_counter = 0;
    constexpr int WARMUP_PERIODS = 20;   // ~53 ms at 48kHz / 128
    constexpr int COOLDOWN_PERIODS = 20; // ~53 ms between pulses
    constexpr int TIMEOUT_PERIODS = 120; // ~320 ms timeout per pulse
    constexpr uint32_t MAX_ATTEMPTS = 10;
    uint32_t attempts = 0;

    uint64_t pulse_playback_sample = 0;

    // Strict continuous lockstep streaming loop (exactly 1 read + 1 write per iteration)
    while (measurements.size() < num_pulses) {
        // 1. Read one period from capture
        snd_pcm_sframes_t r = snd_pcm_readi(cap_handle, in_buf.data(), period);
        if (r < 0) {
            res.message = "ALSA read error during calibration: " + std::string(snd_strerror(static_cast<int>(r)));
            if (is_linked) snd_pcm_unlink(cap_handle);
            snd_pcm_drop(cap_handle);
            snd_pcm_drop(play_handle);
            snd_pcm_close(cap_handle);
            snd_pcm_close(play_handle);
            return res;
        }

        // Strict validation: short reads would corrupt timeline alignment
        if (r != static_cast<snd_pcm_sframes_t>(period)) {
            res.success = false;
            res.message = "ALSA short read during calibration: expected " + std::to_string(period) +
                          " frames, got " + std::to_string(r);
            if (is_linked) snd_pcm_unlink(cap_handle);
            snd_pcm_drop(cap_handle);
            snd_pcm_drop(play_handle);
            snd_pcm_close(cap_handle);
            snd_pcm_close(play_handle);
            return res;
        }

        // 2. State machine update & response detection
        bool emit_pulse_now = false;

        if (state == CalState::WARMUP) {
            if (++state_counter >= WARMUP_PERIODS) {
                emit_pulse_now = true;
                state = CalState::WAITING_FOR_RESPONSE;
                state_counter = 0;
            }
        } else if (state == CalState::WAITING_FOR_RESPONSE) {
            // Check incoming audio for impulse
            for (size_t s = 0; s < static_cast<size_t>(r); ++s) {
                mono_in[s] = static_cast<float>(in_buf[s * cap_ch + cap_idx]) * INT32_TO_FLOAT;
            }

            int32_t offset = detectPulseOffset(mono_in.data(), r, 0.20f);
            if (offset >= 0) {
                uint64_t pulse_capture_sample = total_recorded_samples + static_cast<uint64_t>(offset);
                if (pulse_capture_sample >= pulse_playback_sample) {
                    uint32_t rtl = static_cast<uint32_t>(pulse_capture_sample - pulse_playback_sample);
                    measurements.push_back(rtl);
                    state = CalState::COOLDOWN;
                    state_counter = 0;
                } else {
                    res.success = false;
                    res.message = "Capture/playback timelines are not synchronized: pulse captured before emission.";
                    if (is_linked) snd_pcm_unlink(cap_handle);
                    snd_pcm_drop(cap_handle);
                    snd_pcm_drop(play_handle);
                    snd_pcm_close(cap_handle);
                    snd_pcm_close(play_handle);
                    return res;
                }
            } else if (++state_counter >= TIMEOUT_PERIODS) {
                res.success = false;
                res.message = "No loopback signal detected. Connect Audio Output 1 to Audio Input 1 and verify cable / gain.";
                if (is_linked) snd_pcm_unlink(cap_handle);
                snd_pcm_drop(cap_handle);
                snd_pcm_drop(play_handle);
                snd_pcm_close(cap_handle);
                snd_pcm_close(play_handle);
                return res;
            }
        } else if (state == CalState::COOLDOWN) {
            if (++state_counter >= COOLDOWN_PERIODS) {
                if (measurements.size() < num_pulses) {
                    emit_pulse_now = true;
                    state = CalState::WAITING_FOR_RESPONSE;
                    state_counter = 0;
                }
            }
        }

        total_recorded_samples += r;

        // 3. Write one period to playback (lockstep pairing)
        if (emit_pulse_now) {
            if (++attempts > MAX_ATTEMPTS) {
                res.success = false;
                res.message = "Calibration failed: exceeded maximum pulse attempts (" + std::to_string(MAX_ATTEMPTS) +
                              "). Timelines may not be synchronized.";
                if (is_linked) snd_pcm_unlink(cap_handle);
                snd_pcm_drop(cap_handle);
                snd_pcm_drop(play_handle);
                snd_pcm_close(cap_handle);
                snd_pcm_close(play_handle);
                return res;
            }
            pulse_playback_sample = total_played_samples;
        }

        const int32_t* out_data = emit_pulse_now ? pulse_buf.data() : silence_buf.data();
        if (!writeAll(play_handle, out_data, period, play_ch)) {
            res.message = "ALSA write error during calibration";
            if (is_linked) snd_pcm_unlink(cap_handle);
            snd_pcm_drop(cap_handle);
            snd_pcm_drop(play_handle);
            snd_pcm_close(cap_handle);
            snd_pcm_close(play_handle);
            return res;
        }

        total_played_samples += period;
    }

    if (is_linked) snd_pcm_unlink(cap_handle);
    snd_pcm_drop(cap_handle);
    snd_pcm_drop(play_handle);
    snd_pcm_close(cap_handle);
    snd_pcm_close(play_handle);

    res.success = true;
    res.latency_samples = calculateMedian(measurements);
    res.latency_ms = (static_cast<float>(res.latency_samples) / static_cast<float>(rate)) * 1000.0f;
    res.message = "Calibration successful across " + std::to_string(measurements.size()) + " impulses.";
    return res;
}

} // namespace looper
