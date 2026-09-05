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

    std::vector<int32_t> in_buf(period * cap_ch, 0);
    std::vector<int32_t> out_buf(period * play_ch, 0);
    std::vector<int32_t> silence(period * play_ch, 0);
    std::vector<float> mono_in(period, 0.0f);

    constexpr float INT32_TO_FLOAT = 1.0f / 2147483648.0f;
    constexpr int32_t PULSE_AMPLITUDE = static_cast<int32_t>(0.80f * 2147483647.0f);

    // Prime playback buffer with 2 periods of silence cushion
    writeAll(play_handle, silence.data(), period, play_ch);
    writeAll(play_handle, silence.data(), period, play_ch);

    if (snd_pcm_start(cap_handle) < 0) {
        res.message = "Failed to start capture PCM stream";
        snd_pcm_close(cap_handle);
        snd_pcm_close(play_handle);
        return res;
    }

    std::vector<uint32_t> measurements;
    measurements.reserve(num_pulses);

    for (uint32_t pulse_idx = 0; pulse_idx < num_pulses; ++pulse_idx) {
        // 1. Drain & silence flush (20 periods = ~53 ms)
        for (int p = 0; p < 20; ++p) {
            snd_pcm_readi(cap_handle, in_buf.data(), period);
            writeAll(play_handle, silence.data(), period, play_ch);
        }

        // 2. Emit test impulse in playback buffer
        std::fill(out_buf.begin(), out_buf.end(), 0);
        // Put pulse on left/ch0 output
        out_buf[0] = PULSE_AMPLITUDE;
        if (play_ch > 1) out_buf[1] = PULSE_AMPLITUDE;

        writeAll(play_handle, out_buf.data(), period, play_ch);
        int64_t emit_period = 0;

        // 3. Listen for response on capture
        bool detected = false;
        constexpr int MAX_WAIT_PERIODS = 100; // ~267 ms timeout
        int wait_periods = 0;

        while (wait_periods < MAX_WAIT_PERIODS && !detected) {
            snd_pcm_sframes_t r = snd_pcm_readi(cap_handle, in_buf.data(), period);
            writeAll(play_handle, silence.data(), period, play_ch);
            if (r > 0) {
                for (size_t s = 0; s < static_cast<size_t>(r); ++s) {
                    float val = static_cast<float>(in_buf[s * cap_ch + cap_idx]) * INT32_TO_FLOAT;
                    mono_in[s] = val;
                }

                int32_t offset = detectPulseOffset(mono_in.data(), r, 0.20f);
                if (offset >= 0) {
                    uint32_t rtl = static_cast<uint32_t>((wait_periods - emit_period) * period + offset);
                    measurements.push_back(rtl);
                    detected = true;
                    break;
                }
            }
            wait_periods++;
        }

        if (!detected) {
            res.success = false;
            res.message = "No loopback signal detected. Connect Audio Output 1 to Audio Input 1 and verify cable / gain.";
            snd_pcm_drop(cap_handle);
            snd_pcm_drop(play_handle);
            snd_pcm_close(cap_handle);
            snd_pcm_close(play_handle);
            return res;
        }

        // Wait 10 periods before next pulse
        for (int p = 0; p < 10; ++p) {
            snd_pcm_readi(cap_handle, in_buf.data(), period);
            writeAll(play_handle, silence.data(), period, play_ch);
        }
    }

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
