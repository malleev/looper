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
    AudioDevice(const std::string& device_name, LooperEngine& engine, const LooperConfig& config = LooperConfig());
    ~AudioDevice();

    bool start();
    void stop();
    bool isRunning() const { return is_running_.load(); }

private:
    void audioLoop();
    bool initAlsaDevice(snd_pcm_t** handle, snd_pcm_stream_t stream);
    void handleXrun(snd_pcm_t* handle, snd_pcm_stream_t stream);

    std::string device_name_;
    LooperEngine& engine_;
    LooperConfig config_;

    snd_pcm_t* capture_handle_{nullptr};
    snd_pcm_t* playback_handle_{nullptr};

    std::thread audio_thread_;
    std::atomic<bool> is_running_{false};
};

} // namespace looper
