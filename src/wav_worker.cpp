#include "wav_worker.hpp"
#include "wav_file.hpp"
#include <iostream>
#include <chrono>

namespace looper {

WavWorker::WavWorker(CommandQueue& to_engine_queue, BufferReturnQueue& from_engine_queue, SaveQueue& save_queue, StatusCallback callback)
    : to_engine_queue_(to_engine_queue), from_engine_queue_(from_engine_queue), save_queue_(save_queue), callback_(std::move(callback)) {
}

WavWorker::~WavWorker() {
    stop();
}

void WavWorker::start() {
    if (running_.load()) return;
    running_.store(true);
    worker_thread_ = std::thread(&WavWorker::workerLoop, this);
}

void WavWorker::stop() {
    if (!running_.load()) return;
    running_.store(false);
    cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void WavWorker::requestSave(const std::string& filepath, std::shared_ptr<std::vector<float>> data, uint32_t sample_rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    Task t;
    t.is_save = true;
    t.filepath = filepath;
    t.data = std::move(data);
    t.sample_rate = sample_rate;
    tasks_.push(std::move(t));
    cv_.notify_one();
}

void WavWorker::requestLoad(const std::string& filepath, uint32_t expected_sample_rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    Task t;
    t.is_save = false;
    t.filepath = filepath;
    t.sample_rate = expected_sample_rate;
    tasks_.push(std::move(t));
    cv_.notify_one();
}

void WavWorker::workerLoop() {
    while (running_.load()) {
        // 1. Drain save snapshot queue from real-time audio thread
        SaveRequest save_req;
        while (save_queue_.pop(save_req)) {
            if (save_req.buffer && !save_req.buffer->empty()) {
                if (WavFile::save(save_req.filepath, *save_req.buffer, save_req.sample_rate)) {
                    if (callback_) callback_("Saved: " + save_req.filepath, false);
                } else {
                    if (callback_) callback_("Failed to save: " + save_req.filepath, true);
                }
            } else {
                if (callback_) callback_("Cannot save: buffer is empty", true);
            }
        }

        // 2. Clean up any recycled buffers returned from audio thread safely on worker thread
        std::shared_ptr<std::vector<float>> old_buf;
        while (from_engine_queue_.pop(old_buf)) {
            // Buffer destructs here in background worker thread!
        }

        // 3. Process any UI tasks (e.g. requestLoad)
        Task task;
        bool has_task = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(50), [this]() {
                return !running_.load() || !tasks_.empty() || !save_queue_.empty() || !from_engine_queue_.empty();
            });

            if (!running_.load() && tasks_.empty()) break;

            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop();
                has_task = true;
            }
        }

        if (has_task) {
            if (task.is_save) {
                if (task.data && !task.data->empty()) {
                    if (WavFile::save(task.filepath, *task.data, task.sample_rate)) {
                        if (callback_) callback_("Saved: " + task.filepath, false);
                    } else {
                        if (callback_) callback_("Failed to save: " + task.filepath, true);
                    }
                } else {
                    if (callback_) callback_("Cannot save: loop buffer is empty", true);
                }
            } else {
                // Load operation
                auto loaded = std::make_shared<std::vector<float>>();
                std::string err;
                if (WavFile::load(task.filepath, *loaded, task.sample_rate, err)) {
                    Command cmd;
                    cmd.type = CommandType::LOAD_LOOP_READY;
                    cmd.buffer_payload = loaded;
                    cmd.string_param = task.filepath;
                    to_engine_queue_.push(std::move(cmd));
                    if (callback_) callback_("Loaded: " + task.filepath, false);
                } else {
                    if (callback_) callback_("Load error: " + err, true);
                }
            }
        }
    }
}

} // namespace looper
