#include "wav_worker.hpp"
#include "wav_file.hpp"
#include <iostream>
#include <chrono>

namespace looper {

WavWorker::WavWorker(LoadQueue& load_queue,
                     BufferReturnQueue& return_queue,
                     SaveSlotQueue& save_slot_queue,
                     SaveReadyQueue& save_ready_queue,
                     StatusCallback callback)
    : load_queue_(load_queue),
      return_queue_(return_queue),
      save_slot_queue_(save_slot_queue),
      save_ready_queue_(save_ready_queue),
      callback_(std::move(callback)) {
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

void WavWorker::requestSave(const std::string& filepath, size_t loop_length, uint32_t sample_rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    Task t;
    t.is_save = true;
    t.filepath = filepath;
    t.loop_length = loop_length;
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
        // 1. Drain return queue (deallocate old audio buffers returned from audio thread)
        std::vector<float>* old_buf = nullptr;
        while (return_queue_.pop(old_buf)) {
            delete old_buf; // Deleted safely in background worker thread!
        }

        // 2. Process tasks from UI thread
        Task task;
        bool has_task = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(50), [this]() {
                return !running_.load() || !tasks_.empty() || !return_queue_.empty();
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
                if (task.loop_length == 0) {
                    if (callback_) callback_("Cannot save: loop is empty", true);
                } else {
                    // Allocate staging buffer on worker thread
                    auto* save_buf = new std::vector<float>(task.loop_length);
                    SaveSlotCommand slot;
                    slot.buffer = save_buf;
                    save_slot_queue_.push(slot);

                    // Wait for audio thread to copy snapshot (with 2-second timeout)
                    auto start_wait = std::chrono::steady_clock::now();
                    std::vector<float>* ready_buf = nullptr;
                    while (running_.load()) {
                        if (save_ready_queue_.pop(ready_buf)) {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        if (std::chrono::steady_clock::now() - start_wait > std::chrono::seconds(2)) {
                            break;
                        }
                    }

                    if (ready_buf && !ready_buf->empty()) {
                        if (WavFile::save(task.filepath, *ready_buf, task.sample_rate)) {
                            if (callback_) callback_("Saved: " + task.filepath, false);
                        } else {
                            if (callback_) callback_("Failed to save: " + task.filepath, true);
                        }
                        delete ready_buf;
                    } else {
                        if (callback_) callback_("Save failed (playback not stopped?)", true);
                        if (ready_buf) delete ready_buf;
                        else delete save_buf;
                    }
                }
            } else {
                // Load task
                std::vector<float> loaded;
                std::string err;
                if (WavFile::load(task.filepath, loaded, task.sample_rate, err)) {
                    auto* new_buf = new std::vector<float>();
                    constexpr size_t MAX_LOOP_FRAMES = 48000 * 60 * 10;
                    new_buf->reserve(MAX_LOOP_FRAMES);
                    new_buf->assign(loaded.begin(), loaded.end());

                    LoadCommand cmd;
                    cmd.buffer = new_buf;
                    load_queue_.push(cmd);
                    if (callback_) callback_("Loaded: " + task.filepath, false);
                } else {
                    if (callback_) callback_("Load error: " + err, true);
                }
            }
        }
    }
}

} // namespace looper
