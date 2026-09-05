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
#include <memory>

namespace looper {

class WavWorker {
public:
    using StatusCallback = std::function<void(const std::string&, bool is_error)>;

    WavWorker(CommandQueue& to_engine_queue, BufferReturnQueue& from_engine_queue, SaveQueue& save_queue, StatusCallback callback);
    ~WavWorker();

    void start();
    void stop();

    // Enqueue background file operations
    void requestSave(const std::string& filepath, std::shared_ptr<std::vector<float>> data, uint32_t sample_rate);
    void requestLoad(const std::string& filepath, uint32_t expected_sample_rate);

private:
    struct Task {
        bool is_save = false;
        std::string filepath;
        std::shared_ptr<std::vector<float>> data;
        uint32_t sample_rate = 48000;
    };

    void workerLoop();

    CommandQueue& to_engine_queue_;
    BufferReturnQueue& from_engine_queue_;
    SaveQueue& save_queue_;
    StatusCallback callback_;

    std::thread worker_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Task> tasks_;
    std::atomic<bool> running_{false};
};

} // namespace looper
