#pragma once

#include <atomic>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <utility>

namespace looper {

enum class ControlCommandType : uint8_t {
    NONE = 0,
    ACTION,
    STOP,
    CLEAR,
    UNDO_REDO,
    TOGGLE_REVERSE,
    TRIGGER_FADE,
    SET_MONITOR_MODE,
    ADJUST_LATENCY,
    SET_LOOP_GAIN,
    SET_DRY_GAIN
};

struct ControlCommand {
    ControlCommandType type = ControlCommandType::NONE;
    int32_t int_param = 0;
    float float_param = 0.0f;
};

struct LoadCommand {
    std::vector<float>* buffer = nullptr;
};

struct SaveSlotCommand {
    std::vector<float>* buffer = nullptr;
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

using ControlQueue = SpscQueue<ControlCommand, 64>;
using LoadQueue = SpscQueue<LoadCommand, 4>;
using BufferReturnQueue = SpscQueue<std::vector<float>*, 16>;
using SaveSlotQueue = SpscQueue<SaveSlotCommand, 8>;
using SaveReadyQueue = SpscQueue<std::vector<float>*, 8>;

} // namespace looper
