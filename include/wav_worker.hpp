#pragma once

#include "command_queue.hpp"
#include "types.hpp"
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>

namespace looper {

class WavWorker {
public:
    using StatusCallback = std::function<void(const std::string&, bool is_error)>;

    WavWorker(LoadQueue& load_queue,
              BufferReturnQueue& return_queue,
              SaveSlotQueue& save_slot_queue,
              SaveReadyQueue& save_ready_queue,
              StatusCallback callback, size_t max_loop_frames = MAX_LOOP_FRAMES);
    ~WavWorker();

    void start();
    void stop();

    // Enqueue background file operations
    void requestSave(const std::string& filepath, size_t loop_length, uint32_t sample_rate);
    void requestLoad(const std::string& filepath, uint32_t expected_sample_rate);

private:
    struct Task {
        bool is_save = false;
        std::string filepath;
        size_t loop_length = 0;
        uint32_t sample_rate = 48000;
    };

    void workerLoop();

    LoadQueue& load_queue_;
    BufferReturnQueue& return_queue_;
    SaveSlotQueue& save_slot_queue_;
    SaveReadyQueue& save_ready_queue_;
    StatusCallback callback_;
    const size_t max_loop_frames_;

    std::thread worker_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Task> tasks_;
    std::atomic<bool> running_{false};
};

} // namespace looper
