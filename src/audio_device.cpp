#include "audio_device.hpp"
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <cmath>
#include <algorithm>

namespace looper {

AudioDevice::AudioDevice(const std::string& device_name, LooperEngine& engine, const LooperConfig& config)
    : device_name_(device_name), engine_(engine), config_(config) {
}

AudioDevice::~AudioDevice() {
    stop();
}

bool AudioDevice::initAlsaDevice(snd_pcm_t** handle, snd_pcm_stream_t stream) {
    int err = snd_pcm_open(handle, device_name_.c_str(), stream, 0);
    if (err < 0) {
        std::cerr << "[ALSA] Cannot open audio device " << device_name_ 
                  << " for " << (stream == SND_PCM_STREAM_PLAYBACK ? "playback" : "capture") 
                  << ": " << snd_strerror(err) << std::endl;
        return false;
    }

    snd_pcm_hw_params_t* hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(*handle, hw_params);

    snd_pcm_hw_params_set_access(*handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(*handle, hw_params, SND_PCM_FORMAT_S32_LE);
    snd_pcm_hw_params_set_channels(*handle, hw_params, DEFAULT_CHANNELS);

    unsigned int rate = config_.sample_rate;
    snd_pcm_hw_params_set_rate_near(*handle, hw_params, &rate, 0);

    snd_pcm_uframes_t period_size = config_.period_size;
    snd_pcm_hw_params_set_period_size_near(*handle, hw_params, &period_size, 0);

    unsigned int periods = DEFAULT_PERIODS;
    snd_pcm_hw_params_set_periods_near(*handle, hw_params, &periods, 0);

    err = snd_pcm_hw_params(*handle, hw_params);
    if (err < 0) {
        std::cerr << "[ALSA] Cannot set hw parameters: " << snd_strerror(err) << std::endl;
        snd_pcm_close(*handle);
        *handle = nullptr;
        return false;
    }

    snd_pcm_prepare(*handle);
    return true;
}

void AudioDevice::handleXrun(snd_pcm_t* handle, snd_pcm_stream_t stream) {
    int err = snd_pcm_recover(handle, -EPIPE, 1);
    if (err < 0) {
        std::cerr << "[ALSA] Recovery failed for " 
                  << (stream == SND_PCM_STREAM_PLAYBACK ? "playback" : "capture") 
                  << ": " << snd_strerror(err) << std::endl;
    }
}

bool AudioDevice::start() {
    if (is_running_.load()) return true;

    if (!initAlsaDevice(&capture_handle_, SND_PCM_STREAM_CAPTURE)) {
        return false;
    }
    if (!initAlsaDevice(&playback_handle_, SND_PCM_STREAM_PLAYBACK)) {
        if (capture_handle_) {
            snd_pcm_close(capture_handle_);
            capture_handle_ = nullptr;
        }
        return false;
    }

    is_running_.store(true);
    audio_thread_ = std::thread(&AudioDevice::audioLoop, this);
    return true;
}

void AudioDevice::stop() {
    if (!is_running_.load()) return;

    is_running_.store(false);
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

void AudioDevice::audioLoop() {
    // Set real-time thread priority (SCHED_FIFO)
    struct sched_param param;
    param.sched_priority = 80;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
        std::cerr << "[AUDIO] Warning: Could not set SCHED_FIFO real-time priority." << std::endl;
    } else {
        std::cout << "[AUDIO] Real-time thread priority set to SCHED_FIFO 80." << std::endl;
    }

    const size_t period = config_.period_size;
    const size_t channels = DEFAULT_CHANNELS;

    std::vector<int32_t> in_buffer(period * channels, 0);
    std::vector<int32_t> out_buffer(period * channels, 0);

    std::vector<float> mono_in(period, 0.0f);
    std::vector<float> stereo_out_left(period, 0.0f);
    std::vector<float> stereo_out_right(period, 0.0f);

    constexpr float INT32_TO_FLOAT = 1.0f / 2147483648.0f;
    constexpr float FLOAT_TO_INT32 = 2147483647.0f;

    // CRITICAL: Pre-fill playback buffer with 2 periods of silence!
    // This creates a pipeline cushion so that when snd_pcm_readi blocks for capture,
    // playback does not starve and underrun (which causes bitcrusher-like buzzing distortion).
    std::vector<int32_t> silence(period * channels, 0);
    snd_pcm_writei(playback_handle_, silence.data(), period);
    snd_pcm_writei(playback_handle_, silence.data(), period);

    while (is_running_.load(std::memory_order_relaxed)) {
        // Read from ALSA capture (blocking for 1 period)
        snd_pcm_sframes_t frames_read = snd_pcm_readi(capture_handle_, in_buffer.data(), period);
        if (frames_read < 0) {
            if (frames_read == -EPIPE) {
                handleXrun(capture_handle_, SND_PCM_STREAM_CAPTURE);
                continue;
            } else {
                std::cerr << "[ALSA] Capture error: " << snd_strerror(frames_read) << std::endl;
                break;
            }
        }

        // De-interleave and convert input Channel 0 (Instrument / Mic) to float [-1.0, 1.0]
        for (size_t i = 0; i < static_cast<size_t>(frames_read); ++i) {
            mono_in[i] = static_cast<float>(in_buffer[i * channels + 0]) * INT32_TO_FLOAT;
        }

        // Process audio in LooperEngine
        engine_.process(mono_in.data(), stereo_out_left.data(), stereo_out_right.data(), frames_read);

        // Convert and interleave stereo outputs into 4-channel hardware buffer
        // Ch0 = Left, Ch1 = Right, Ch2 = 0, Ch3 = 0
        for (size_t i = 0; i < static_cast<size_t>(frames_read); ++i) {
            float l = std::clamp(stereo_out_left[i], -1.0f, 1.0f);
            float r = std::clamp(stereo_out_right[i], -1.0f, 1.0f);
            out_buffer[i * channels + 0] = static_cast<int32_t>(l * FLOAT_TO_INT32);
            out_buffer[i * channels + 1] = static_cast<int32_t>(r * FLOAT_TO_INT32);
            out_buffer[i * channels + 2] = 0;
            out_buffer[i * channels + 3] = 0;
        }

        // Write to ALSA playback
        snd_pcm_sframes_t frames_written = snd_pcm_writei(playback_handle_, out_buffer.data(), frames_read);
        if (frames_written < 0) {
            if (frames_written == -EPIPE) {
                handleXrun(playback_handle_, SND_PCM_STREAM_PLAYBACK);
                // Re-seed with a period of silence to restore cushion
                snd_pcm_writei(playback_handle_, silence.data(), period);
                continue;
            } else {
                std::cerr << "[ALSA] Playback error: " << snd_strerror(frames_written) << std::endl;
                break;
            }
        }
    }
}

} // namespace looper
