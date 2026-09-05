#include "audio_device.hpp"
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <cmath>
#include <algorithm>
#include <chrono>

namespace looper {

AudioDevice::AudioDevice(const AudioConfig& audio_config, LooperEngine& engine, const LooperConfig& config)
    : audio_config_(audio_config), engine_(engine), config_(config) {
}

AudioDevice::AudioDevice(const std::string& device_name, LooperEngine& engine, const LooperConfig& config)
    : engine_(engine), config_(config) {
    audio_config_.capture_device = device_name;
    audio_config_.playback_device = device_name;
    audio_config_.sample_rate = config.sample_rate;
    audio_config_.period_size = config.period_size;
}

AudioDevice::~AudioDevice() {
    stop();
}

AudioTelemetrySnapshot AudioDevice::getTelemetrySnapshot() const {
    AudioTelemetrySnapshot snap;
    snap.capture_xruns = telemetry_.capture_xruns.load(std::memory_order_relaxed);
    snap.playback_xruns = telemetry_.playback_xruns.load(std::memory_order_relaxed);
    snap.suspends = telemetry_.suspends.load(std::memory_order_relaxed);
    snap.disconnects = telemetry_.disconnects.load(std::memory_order_relaxed);
    snap.short_writes = telemetry_.short_writes.load(std::memory_order_relaxed);
    snap.recoveries = telemetry_.recoveries.load(std::memory_order_relaxed);
    snap.fatal_audio_errors = telemetry_.fatal_audio_errors.load(std::memory_order_relaxed);
    snap.process_max_us = telemetry_.process_max_us.load(std::memory_order_relaxed);
    snap.process_avg_us = telemetry_.process_avg_us.load(std::memory_order_relaxed);
    snap.rt_priority_acquired = telemetry_.rt_priority_acquired.load(std::memory_order_relaxed);
    return snap;
}

bool AudioDevice::initAlsaDevice(snd_pcm_t** handle, const std::string& dev_name, snd_pcm_stream_t stream, PcmParams& negotiated) {
    int err = snd_pcm_open(handle, dev_name.c_str(), stream, 0);
    if (err < 0) {
        std::cerr << "[ALSA] Cannot open audio device " << dev_name 
                  << " for " << (stream == SND_PCM_STREAM_PLAYBACK ? "playback" : "capture") 
                  << ": " << snd_strerror(err) << std::endl;
        return false;
    }

    snd_pcm_hw_params_t* hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    err = snd_pcm_hw_params_any(*handle, hw_params);
    if (err < 0) {
        std::cerr << "[ALSA] Cannot initialize hw params on " << dev_name << ": " << snd_strerror(err) << std::endl;
        snd_pcm_close(*handle);
        *handle = nullptr;
        return false;
    }

    err = snd_pcm_hw_params_set_access(*handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        std::cerr << "[ALSA] Cannot set interleaved access on " << dev_name << ": " << snd_strerror(err) << std::endl;
        snd_pcm_close(*handle);
        *handle = nullptr;
        return false;
    }

    err = snd_pcm_hw_params_set_format(*handle, hw_params, SND_PCM_FORMAT_S32_LE);
    if (err < 0) {
        std::cerr << "[ALSA] Cannot set format S32_LE on " << dev_name << ": " << snd_strerror(err) << std::endl;
        snd_pcm_close(*handle);
        *handle = nullptr;
        return false;
    }

    unsigned int ch = (stream == SND_PCM_STREAM_CAPTURE) ? audio_config_.capture_channels : audio_config_.playback_channels;
    err = snd_pcm_hw_params_set_channels(*handle, hw_params, ch);
    if (err < 0) {
        std::cerr << "[ALSA] Cannot set channels (" << ch << ") on " << dev_name << ": " << snd_strerror(err) << std::endl;
        snd_pcm_close(*handle);
        *handle = nullptr;
        return false;
    }

    unsigned int rate = audio_config_.sample_rate;
    err = snd_pcm_hw_params_set_rate_near(*handle, hw_params, &rate, 0);
    if (err < 0) {
        std::cerr << "[ALSA] Cannot set rate (" << rate << ") on " << dev_name << ": " << snd_strerror(err) << std::endl;
        snd_pcm_close(*handle);
        *handle = nullptr;
        return false;
    }

    snd_pcm_uframes_t period_size = audio_config_.period_size;
    err = snd_pcm_hw_params_set_period_size_near(*handle, hw_params, &period_size, 0);
    if (err < 0) {
        std::cerr << "[ALSA] Cannot set period size (" << period_size << ") on " << dev_name << ": " << snd_strerror(err) << std::endl;
        snd_pcm_close(*handle);
        *handle = nullptr;
        return false;
    }

    unsigned int periods = audio_config_.periods;
    err = snd_pcm_hw_params_set_periods_near(*handle, hw_params, &periods, 0);
    if (err < 0) {
        std::cerr << "[ALSA] Cannot set periods (" << periods << ") on " << dev_name << ": " << snd_strerror(err) << std::endl;
        snd_pcm_close(*handle);
        *handle = nullptr;
        return false;
    }

    err = snd_pcm_hw_params(*handle, hw_params);
    if (err < 0) {
        std::cerr << "[ALSA] Cannot commit hw parameters on " << dev_name << ": " << snd_strerror(err) << std::endl;
        snd_pcm_close(*handle);
        *handle = nullptr;
        return false;
    }

    // Read back negotiated parameters
    unsigned int neg_rate = 0;
    snd_pcm_hw_params_get_rate(hw_params, &neg_rate, 0);
    negotiated.sample_rate = neg_rate;

    snd_pcm_uframes_t neg_period = 0;
    snd_pcm_hw_params_get_period_size(hw_params, &neg_period, 0);
    negotiated.period_size = static_cast<uint32_t>(neg_period);

    unsigned int neg_periods = 0;
    snd_pcm_hw_params_get_periods(hw_params, &neg_periods, 0);
    negotiated.periods = neg_periods;

    snd_pcm_uframes_t neg_buf = 0;
    snd_pcm_hw_params_get_buffer_size(hw_params, &neg_buf);
    negotiated.buffer_size = static_cast<uint32_t>(neg_buf);

    unsigned int neg_ch = 0;
    snd_pcm_hw_params_get_channels(hw_params, &neg_ch);
    negotiated.channels = neg_ch;

    // Strict validation: engine and DSP algorithms require exact sample rate
    if (negotiated.sample_rate != audio_config_.sample_rate) {
        std::cerr << "[ALSA] ERROR: Device negotiated sample rate " << negotiated.sample_rate 
                  << " Hz, but exactly " << audio_config_.sample_rate << " Hz is required!" << std::endl;
        snd_pcm_close(*handle);
        *handle = nullptr;
        return false;
    }

    if (negotiated.period_size != audio_config_.period_size) {
        std::cerr << "[ALSA] ERROR: Device negotiated period size " << negotiated.period_size 
                  << ", but exactly " << audio_config_.period_size << " is required!" << std::endl;
        snd_pcm_close(*handle);
        *handle = nullptr;
        return false;
    }

    err = snd_pcm_prepare(*handle);
    if (err < 0) {
        std::cerr << "[ALSA] Cannot prepare device " << dev_name << ": " << snd_strerror(err) << std::endl;
        snd_pcm_close(*handle);
        *handle = nullptr;
        return false;
    }

    return true;
}

bool AudioDevice::initSoftwareParams(snd_pcm_t* handle, const PcmParams& params, snd_pcm_stream_t stream) {
    snd_pcm_sw_params_t* sw_params;
    snd_pcm_sw_params_alloca(&sw_params);
    int err = snd_pcm_sw_params_current(handle, sw_params);
    if (err < 0) {
        std::cerr << "[ALSA] Cannot get current sw params: " << snd_strerror(err) << std::endl;
        return false;
    }

    // Wake up as soon as 1 full period is ready
    err = snd_pcm_sw_params_set_avail_min(handle, sw_params, params.period_size);
    if (err < 0) {
        std::cerr << "[ALSA] Cannot set avail_min: " << snd_strerror(err) << std::endl;
        return false;
    }

    if (stream == SND_PCM_STREAM_PLAYBACK) {
        // Automatic playback start threshold = 2 periods of cushion
        err = snd_pcm_sw_params_set_start_threshold(handle, sw_params, 2 * params.period_size);
        if (err < 0) {
            std::cerr << "[ALSA] Cannot set playback start_threshold: " << snd_strerror(err) << std::endl;
            return false;
        }
    } else {
        // Capture starts on first sample
        err = snd_pcm_sw_params_set_start_threshold(handle, sw_params, 1);
        if (err < 0) {
            std::cerr << "[ALSA] Cannot set capture start_threshold: " << snd_strerror(err) << std::endl;
            return false;
        }
    }

    err = snd_pcm_sw_params(handle, sw_params);
    if (err < 0) {
        std::cerr << "[ALSA] Cannot commit sw params: " << snd_strerror(err) << std::endl;
        return false;
    }

    return true;
}

bool AudioDevice::start() {
    if (is_running_.load()) return true;

    if (!initAlsaDevice(&capture_handle_, audio_config_.capture_device, SND_PCM_STREAM_CAPTURE, capture_params_)) {
        return false;
    }
    if (!initSoftwareParams(capture_handle_, capture_params_, SND_PCM_STREAM_CAPTURE)) {
        snd_pcm_close(capture_handle_);
        capture_handle_ = nullptr;
        return false;
    }

    if (!initAlsaDevice(&playback_handle_, audio_config_.playback_device, SND_PCM_STREAM_PLAYBACK, playback_params_)) {
        snd_pcm_close(capture_handle_);
        capture_handle_ = nullptr;
        return false;
    }
    if (!initSoftwareParams(playback_handle_, playback_params_, SND_PCM_STREAM_PLAYBACK)) {
        snd_pcm_close(capture_handle_);
        capture_handle_ = nullptr;
        snd_pcm_close(playback_handle_);
        playback_handle_ = nullptr;
        return false;
    }

    // Hardware and Duplex consistency validation
    if (audio_config_.capture_channel_index >= capture_params_.channels) {
        std::cerr << "[ALSA] ERROR: capture_channel_index (" << audio_config_.capture_channel_index 
                  << ") exceeds available channels (" << capture_params_.channels << ")!" << std::endl;
        stop();
        return false;
    }
    if (capture_params_.channels != audio_config_.capture_channels) {
        std::cerr << "[ALSA] ERROR: capture channels (" << capture_params_.channels 
                  << ") != requested (" << audio_config_.capture_channels << ")!" << std::endl;
        stop();
        return false;
    }
    if (playback_params_.channels != audio_config_.playback_channels) {
        std::cerr << "[ALSA] ERROR: playback channels (" << playback_params_.channels 
                  << ") != requested (" << audio_config_.playback_channels << ")!" << std::endl;
        stop();
        return false;
    }
    if (capture_params_.sample_rate != playback_params_.sample_rate) {
        std::cerr << "[ALSA] ERROR: Duplex sample rate mismatch: capture=" << capture_params_.sample_rate 
                  << " != playback=" << playback_params_.sample_rate << std::endl;
        stop();
        return false;
    }
    if (capture_params_.period_size != playback_params_.period_size) {
        std::cerr << "[ALSA] ERROR: Duplex period size mismatch: capture=" << capture_params_.period_size 
                  << " != playback=" << playback_params_.period_size << std::endl;
        stop();
        return false;
    }
    if (playback_params_.buffer_size < 2 * playback_params_.period_size) {
        std::cerr << "[ALSA] ERROR: Playback buffer size (" << playback_params_.buffer_size 
                  << ") is too small for 2 periods (" << 2 * playback_params_.period_size << ") cushion!" << std::endl;
        stop();
        return false;
    }

    // Pre-allocate & prefault all member audio buffers before starting real-time thread (ZERO allocations in RT loop)
    in_buffer_.assign(capture_params_.period_size * capture_params_.channels, 0);
    out_buffer_.assign(playback_params_.period_size * playback_params_.channels, 0);
    mono_in_.assign(capture_params_.period_size, 0.0f);
    stereo_out_left_.assign(playback_params_.period_size, 0.0f);
    stereo_out_right_.assign(playback_params_.period_size, 0.0f);
    silence_buffer_.assign(playback_params_.period_size * playback_params_.channels, 0);

    is_running_.store(true);
    audio_thread_ = std::thread(&AudioDevice::audioLoop, this);
    return true;
}

void AudioDevice::stop() {
    if (!is_running_.load()) return;

    is_running_.store(false);

    // Unblock any blocking snd_pcm_readi / snd_pcm_writei calls immediately
    if (capture_handle_) {
        snd_pcm_drop(capture_handle_);
    }
    if (playback_handle_) {
        snd_pcm_drop(playback_handle_);
    }

    if (audio_thread_.joinable()) {
        audio_thread_.join();
    }

    if (capture_handle_) {
        snd_pcm_close(capture_handle_);
        capture_handle_ = nullptr;
    }
    if (playback_handle_) {
        snd_pcm_close(playback_handle_);
        playback_handle_ = nullptr;
    }
}

snd_pcm_sframes_t AudioDevice::writeAll(const int32_t* data, snd_pcm_uframes_t total_frames) {
    snd_pcm_uframes_t frames_left = total_frames;
    const int32_t* ptr = data;
    const size_t channels = playback_params_.channels;
    int zero_write_retries = 0;

    while (frames_left > 0 && is_running_.load(std::memory_order_relaxed)) {
        snd_pcm_sframes_t written = snd_pcm_writei(playback_handle_, ptr, frames_left);
        if (written > 0) {
            zero_write_retries = 0;
            if (static_cast<snd_pcm_uframes_t>(written) < frames_left) {
                telemetry_.short_writes.fetch_add(1, std::memory_order_relaxed);
            }
            frames_left -= written;
            ptr += written * channels;
        } else if (written == 0) {
            // Guard against driver busy-spin: abort if repeated 0 returns
            if (++zero_write_retries > 10) {
                return -EIO;
            }
        } else {
            return written; // Return error code to caller for duplex recovery
        }
    }
    return total_frames;
}

bool AudioDevice::recoverDuplex(int err, snd_pcm_stream_t stream) {
    if (err == -EPIPE) {
        if (stream == SND_PCM_STREAM_CAPTURE) {
            telemetry_.capture_xruns.fetch_add(1, std::memory_order_relaxed);
        } else {
            telemetry_.playback_xruns.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (err == -ESTRPIPE) {
        telemetry_.suspends.fetch_add(1, std::memory_order_relaxed);
    } else if (err == -ENODEV) {
        telemetry_.disconnects.fetch_add(1, std::memory_order_relaxed);
    }
    telemetry_.recoveries.fetch_add(1, std::memory_order_relaxed);

    // Check for fatal unrecoverable errors (device unplugged, broken descriptor)
    if (err == -ENODEV || err == -EBADFD || err == -EIO) {
        telemetry_.fatal_audio_errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Attempt ALSA silent recovery (0 = silent, no iostream in RT thread)
    snd_pcm_t* failed_handle = (stream == SND_PCM_STREAM_CAPTURE) ? capture_handle_ : playback_handle_;
    int rec_err = snd_pcm_recover(failed_handle, err, 0);
    if (rec_err < 0) {
        telemetry_.fatal_audio_errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Re-align duplex pair with checked return codes
    if (snd_pcm_drop(capture_handle_) < 0) return false;
    if (snd_pcm_drop(playback_handle_) < 0) return false;
    if (snd_pcm_prepare(capture_handle_) < 0) return false;
    if (snd_pcm_prepare(playback_handle_) < 0) return false;

    // Re-prime playback with 2 periods of silence cushion
    if (writeAll(silence_buffer_.data(), playback_params_.period_size) < 0) return false;
    if (writeAll(silence_buffer_.data(), playback_params_.period_size) < 0) return false;

    if (snd_pcm_start(capture_handle_) < 0) return false;
    return true;
}

void AudioDevice::audioLoop() {
    // Set real-time thread priority (SCHED_FIFO) - record status via atomic telemetry
    struct sched_param param;
    param.sched_priority = 80;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0) {
        telemetry_.rt_priority_acquired.store(true, std::memory_order_relaxed);
    }

    const size_t period = capture_params_.period_size;
    const size_t cap_channels = capture_params_.channels;
    const size_t play_channels = playback_params_.channels;
    const size_t cap_idx = audio_config_.capture_channel_index;

    constexpr float INT32_TO_FLOAT = 1.0f / 2147483648.0f;
    constexpr float FLOAT_TO_INT32 = 2147483647.0f;

    // Prime playback buffer with 2 periods of silence cushion
    if (writeAll(silence_buffer_.data(), period) < 0 || writeAll(silence_buffer_.data(), period) < 0) {
        return;
    }
    if (snd_pcm_start(capture_handle_) < 0) {
        return;
    }

    while (is_running_.load(std::memory_order_relaxed)) {
        // Read from ALSA capture into pre-allocated member buffer (blocking for 1 period)
        snd_pcm_sframes_t frames_read = snd_pcm_readi(capture_handle_, in_buffer_.data(), period);
        if (frames_read < 0) {
            if (!recoverDuplex(static_cast<int>(frames_read), SND_PCM_STREAM_CAPTURE)) {
                break;
            }
            continue;
        }

        // De-interleave and convert input Channel to float [-1.0, 1.0]
        for (size_t i = 0; i < static_cast<size_t>(frames_read); ++i) {
            mono_in_[i] = static_cast<float>(in_buffer_[i * cap_channels + cap_idx]) * INT32_TO_FLOAT;
        }

        // Process audio in LooperEngine with execution timing
        auto t_start = std::chrono::steady_clock::now();
        engine_.process(mono_in_.data(), stereo_out_left_.data(), stereo_out_right_.data(), frames_read);
        auto t_end = std::chrono::steady_clock::now();

        uint32_t elapsed_us = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count()
        );

        // Update atomic process_max_us
        uint32_t cur_max = telemetry_.process_max_us.load(std::memory_order_relaxed);
        while (elapsed_us > cur_max && !telemetry_.process_max_us.compare_exchange_weak(cur_max, elapsed_us, std::memory_order_relaxed)) {}

        // Update atomic process_avg_us with exponential moving average (alpha ~ 1/32)
        uint32_t cur_avg = telemetry_.process_avg_us.load(std::memory_order_relaxed);
        uint32_t new_avg = (cur_avg == 0) ? elapsed_us : ((cur_avg * 31 + elapsed_us) >> 5);
        telemetry_.process_avg_us.store(new_avg, std::memory_order_relaxed);

        // Convert and interleave stereo outputs into playback hardware buffer
        // Ch0 = Left, Ch1 = Right, remaining = 0
        for (size_t i = 0; i < static_cast<size_t>(frames_read); ++i) {
            float l = std::clamp(stereo_out_left_[i], -1.0f, 1.0f);
            float r = std::clamp(stereo_out_right_[i], -1.0f, 1.0f);
            out_buffer_[i * play_channels + 0] = static_cast<int32_t>(l * FLOAT_TO_INT32);
            if (play_channels > 1) {
                out_buffer_[i * play_channels + 1] = static_cast<int32_t>(r * FLOAT_TO_INT32);
            }
            for (size_t ch = 2; ch < play_channels; ++ch) {
                out_buffer_[i * play_channels + ch] = 0;
            }
        }

        // Write all frames to ALSA playback (handles short writes internally)
        snd_pcm_sframes_t written = writeAll(out_buffer_.data(), frames_read);
        if (written < 0) {
            if (!recoverDuplex(static_cast<int>(written), SND_PCM_STREAM_PLAYBACK)) {
                break;
            }
        }
    }
}

} // namespace looper
