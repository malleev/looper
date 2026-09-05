#pragma once

#include "types.hpp"
#include "input_manager.hpp"
#include <functional>
#include <memory>

namespace looper {

class GpioManager {
public:
    using KeyCallback = std::function<void(ActionKey, uint64_t)>;

    explicit GpioManager(KeyCallback callback);
    ~GpioManager();

    bool start();
    void stop();

    void updateStatus(const LooperStatus& status);
    void setOff();

private:
    KeyCallback callback_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace looper
