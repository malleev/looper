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
                } else if (task.loop_length > MAX_LOOP_FRAMES) {
                    if (callback_) callback_("Cannot save: loop exceeds max frames", true);
                } else {
                    // Allocate staging buffer on worker thread
                    auto* save_buf = new std::vector<float>();
                    save_buf->reserve(task.loop_length);
                    save_buf->resize(task.loop_length);

                    SaveSlotCommand slot;
                    slot.buffer = save_buf;
                    if (!save_slot_queue_.push(slot)) {
                        delete save_buf; // Clean up immediately if queue is full
                        if (callback_) callback_("Save failed: engine queue full", true);
                    } else {
                        // Ownership transferred to engine.
                        // Strict ownership: worker MUST wait for engine to return buffer via save_ready_queue_!
                        // Worker NEVER deletes save_buf directly on timeout to prevent UAF.
                        std::vector<float>* ready_buf = nullptr;
                        while (running_.load()) {
                            if (save_ready_queue_.pop(ready_buf)) {
                                break;
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        }

                        if (ready_buf && !ready_buf->empty()) {
                            if (WavFile::save(task.filepath, *ready_buf, task.sample_rate)) {
                                if (callback_) callback_("Saved: " + task.filepath, false);
                            } else {
                                if (callback_) callback_("Failed to save: " + task.filepath, true);
                            }
                            delete ready_buf;
                        } else {
                            if (callback_) callback_("Save failed (playback active or buffer empty)", true);
                            if (ready_buf) {
                                delete ready_buf;
                            }
                        }
                    }
                }
            } else {
                // Load task
                std::vector<float> loaded;
                std::string err;
                if (WavFile::load(task.filepath, loaded, task.sample_rate, err)) {
                    if (loaded.size() > MAX_LOOP_FRAMES) {
                        if (callback_) callback_("Load error: WAV file exceeds 5 min max", true);
                    } else {
                        size_t file_frames = loaded.size();
                        auto* new_base = new std::vector<float>(MAX_LOOP_FRAMES, 0.0f);
                        std::copy(loaded.begin(), loaded.end(), new_base->begin());

                        auto* new_layer = new std::vector<float>(MAX_LOOP_FRAMES, 0.0f);
                        auto* new_record = new std::vector<float>(MAX_LOOP_FRAMES, 0.0f);

                        LoadCommand cmd;
                        cmd.base_buffer = new_base;
                        cmd.layer_buffer = new_layer;
                        cmd.record_buffer = new_record;
                        cmd.loop_length = file_frames;
                        if (!load_queue_.push(cmd)) {
                            delete new_base; // Clean up on push failure
                            delete new_layer;
                            delete new_record;
                            if (callback_) callback_("Load error: engine load queue full", true);
                        } else {
                            if (callback_) callback_("Loaded: " + task.filepath, false);
                        }
                    }
                } else {
                    if (callback_) callback_("Load error: " + err, true);
                }
            }
        }
    }

    // Drain and delete any leftover buffers on shutdown
    std::vector<float>* leftover = nullptr;
    while (return_queue_.pop(leftover)) {
        delete leftover;
    }
    while (save_ready_queue_.pop(leftover)) {
        delete leftover;
    }
}

} // namespace looper
