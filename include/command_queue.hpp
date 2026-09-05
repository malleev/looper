#pragma once

#include <atomic>
#include <vector>
#include <memory>
#include <string>

namespace looper {

enum class CommandType {
    ACTION,             // Rec -> Play -> Overdub -> Play
    STOP,               // Stop playback
    CLEAR,              // Clear loop
    UNDO_REDO,          // Toggle Undo/Redo
    TOGGLE_REVERSE,     // Reverse playback
    TRIGGER_FADE,       // Start fade-out
    SET_MONITOR_MODE,   // Toggle or set MonitorMode (DIRECT_ANALOG vs SOFTWARE)
    ADJUST_LATENCY,     // Adjust latency compensation (+/- delta)
    SET_LOOP_GAIN,      // Adjust playback volume
    SET_DRY_GAIN,       // Adjust software dry volume
    LOAD_LOOP_READY,    // New loop loaded from disk by background worker
    REQUEST_SAVE_WAV    // Request snapshot save to disk
};

struct Command {
    CommandType type = CommandType::ACTION;
    int int_param = 0;
    float float_param = 0.0f;
    std::shared_ptr<std::vector<float>> buffer_payload;
    std::string string_param;
};

// Fixed-size wait-free Single-Producer Single-Consumer queue
template <typename T, size_t Capacity = 64>
class SpscQueue {
public:
    SpscQueue() : head_(0), tail_(0) {}

    bool push(const T& item) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next_head = (head + 1) % Capacity;
        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false; // Queue full
        }
        ring_[head] = item;
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    bool push(T&& item) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next_head = (head + 1) % Capacity;
        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false; // Queue full
        }
        ring_[head] = std::move(item);
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false; // Queue empty
        }
        item = std::move(ring_[tail]);
        tail_.store((tail + 1) % Capacity, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

private:
    T ring_[Capacity];
    std::atomic<size_t> head_;
    std::atomic<size_t> tail_;
};

struct SaveRequest {
    std::string filepath;
    std::shared_ptr<std::vector<float>> buffer;
    uint32_t sample_rate = 48000;
};

using CommandQueue = SpscQueue<Command, 64>;
using BufferReturnQueue = SpscQueue<std::shared_ptr<std::vector<float>>, 64>;
using SaveQueue = SpscQueue<SaveRequest, 16>;

} // namespace looper
