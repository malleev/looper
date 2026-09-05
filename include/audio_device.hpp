#pragma once

#include "types.hpp"
#include "looper_engine.hpp"
#include <alsa/asoundlib.h>
#include <thread>
#include <atomic>
#include <string>
#include <vector>

namespace looper {

class AudioDevice {
public:
    AudioDevice(const AudioConfig& audio_config, LooperEngine& engine, const LooperConfig& config = LooperConfig());
    AudioDevice(const std::string& device_name, LooperEngine& engine, const LooperConfig& config = LooperConfig());
    ~AudioDevice();

    bool start();
    void stop();
    bool isRunning() const { return is_running_.load(); }

    const PcmParams& getCaptureParams() const { return capture_params_; }
    const PcmParams& getPlaybackParams() const { return playback_params_; }
    const AudioTelemetry& getTelemetry() const { return telemetry_; }
    AudioTelemetrySnapshot getTelemetrySnapshot() const;

private:
    void audioLoop();
    bool initAlsaDevice(snd_pcm_t** handle, const std::string& dev_name, snd_pcm_stream_t stream, PcmParams& negotiated);
    bool initSoftwareParams(snd_pcm_t* handle, const PcmParams& params, snd_pcm_stream_t stream);
    bool recoverDuplex(int err, snd_pcm_stream_t stream);
    snd_pcm_sframes_t writeAll(const int32_t* data, snd_pcm_uframes_t frames);

    AudioConfig audio_config_;
    LooperEngine& engine_;
    LooperConfig config_;

    snd_pcm_t* capture_handle_{nullptr};
    snd_pcm_t* playback_handle_{nullptr};

    PcmParams capture_params_;
    PcmParams playback_params_;

    AudioTelemetry telemetry_;

    std::vector<int32_t> in_buffer_;
    std::vector<int32_t> out_buffer_;
    std::vector<float> mono_in_;
    std::vector<float> stereo_out_left_;
    std::vector<float> stereo_out_right_;
    std::vector<int32_t> silence_buffer_;

    std::thread audio_thread_;
    std::atomic<bool> is_running_{false};
};

} // namespace looper
