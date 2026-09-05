#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>

namespace looper {

enum class ActionKey {
    NONE,
    ACTION,          // Space
    STOP,            // S
    CLEAR,           // C
    UNDO,            // U
    REVERSE,         // R
    FADE,            // F
    TOGGLE_MONITOR,  // M: Toggle between DIRECT_ANALOG and SOFTWARE
    VOL_UP,          // +
    VOL_DOWN,        // -
    SAVE_WAV,        // W
    LOAD_WAV,        // L
    LATENCY_UP,      // > or .
    LATENCY_DOWN,    // < or ,
    QUIT             // Q
};

class InputManager {
public:
    using KeyCallback = std::function<void(ActionKey, uint64_t)>;

    explicit InputManager(KeyCallback callback);
    ~InputManager();

    bool start();
    void stop();

private:
    void workerLoop();
    void scanEvdevDevices();

    KeyCallback callback_;
    std::thread worker_thread_;
    std::atomic<bool> running_{false};
    std::vector<int> evdev_fds_;
};

} // namespace looper
