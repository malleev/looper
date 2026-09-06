#include "gpio_manager.hpp"
#include <iostream>
#include <chrono>
#include <vector>
#include <array>
#include <thread>
#include <atomic>

#if __has_include(<gpiod.h>)
#define LOOPER_HAS_GPIOD 1
#include <gpiod.h>
#endif

namespace looper {

#ifdef LOOPER_HAS_GPIOD

struct ButtonDef {
    unsigned int offset;
    ActionKey action;
    const char* name;
};

static const std::array<ButtonDef, 6> HARDWARE_BUTTONS = {{
    {3,  ActionKey::ACTION,  "PA3 (ACTION)"},
    {7,  ActionKey::STOP,    "PA7 (STOP)"},
    {8,  ActionKey::CLEAR,   "PA8 (CLEAR)"},
    {9,  ActionKey::UNDO,    "PA9 (UNDO/REDO)"},
    {10, ActionKey::FADE,    "PA10 (FADE 3s)"},
    {2,  ActionKey::REVERSE, "PA2 (REVERSE)"}
}};

constexpr unsigned int LED_RED_OFFSET   = 6; // PA6
constexpr unsigned int LED_GREEN_OFFSET = 1; // PA1
constexpr unsigned int LED_BLUE_OFFSET  = 0; // PA0

constexpr unsigned int CLIP_LED_RED_OFFSET   = 13; // PA13 (Pin 8)  - Clip / Overload LED
constexpr unsigned int CLIP_LED_GREEN_OFFSET = 14; // PA14 (Pin 10) - Signal Present LED

struct GpioManager::Impl {
    explicit Impl(KeyCallback cb) : callback(std::move(cb)) {}

    KeyCallback callback;
    std::thread button_thread;
    std::atomic<bool> running{false};

    struct gpiod_chip* chip{nullptr};
    struct gpiod_line_bulk button_lines{};
    bool buttons_active{false};

    struct gpiod_line* led_r{nullptr};
    struct gpiod_line* led_g{nullptr};
    struct gpiod_line* led_b{nullptr};
    bool leds_active{false};

    struct gpiod_line* clip_led_r{nullptr};
    struct gpiod_line* clip_led_g{nullptr};
    bool clip_leds_active{false};

    int current_r{-1};
    int current_g{-1};
    int current_b{-1};
    int current_clip_r{-1};
    int current_clip_g{-1};

    std::array<uint64_t, 32> last_press_ts_ns{};

    bool init() {
        chip = gpiod_chip_open_by_name("gpiochip0");
        if (!chip) {
            std::cerr << "[GPIO] Warning: Cannot open /dev/gpiochip0. Hardware buttons and LEDs disabled.\n";
            return false;
        }

        // 1. Initialize RGB LED lines as outputs
        led_r = gpiod_chip_get_line(chip, LED_RED_OFFSET);
        led_g = gpiod_chip_get_line(chip, LED_GREEN_OFFSET);
        led_b = gpiod_chip_get_line(chip, LED_BLUE_OFFSET);

        if (led_r && led_g && led_b) {
            int ret_r = gpiod_line_request_output(led_r, "looper_led_r", 0);
            int ret_g = gpiod_line_request_output(led_g, "looper_led_g", 0);
            int ret_b = gpiod_line_request_output(led_b, "looper_led_b", 0);
            if (ret_r == 0 && ret_g == 0 && ret_b == 0) {
                leds_active = true;
                current_r = 0; current_g = 0; current_b = 0;
                std::cout << "[GPIO] RGB Status LED initialized on PA6(R), PA1(G), PA0(B).\n";
            } else {
                std::cerr << "[GPIO] Warning: Failed to configure RGB LED output lines.\n";
            }
        }

        // 2. Initialize Clip / Overload & Signal LED lines
        clip_led_r = gpiod_chip_get_line(chip, CLIP_LED_RED_OFFSET);
        clip_led_g = gpiod_chip_get_line(chip, CLIP_LED_GREEN_OFFSET);
        if (clip_led_r && clip_led_g) {
            int ret_cr = gpiod_line_request_output(clip_led_r, "looper_clip_r", 0);
            int ret_cg = gpiod_line_request_output(clip_led_g, "looper_sig_g", 0);
            if (ret_cr == 0 && ret_cg == 0) {
                clip_leds_active = true;
                current_clip_r = 0;
                current_clip_g = 0;
                std::cout << "[GPIO] Overload/Signal LED initialized on PA13 (Red/Clip) & PA14 (Green/Signal).\n";
            } else {
                std::cerr << "[GPIO] Warning: Failed to configure Overload/Signal LED output lines.\n";
            }
        }

        // 2. Initialize Hardware Button lines as edge-triggered inputs
        gpiod_line_bulk_init(&button_lines);
        for (const auto& btn : HARDWARE_BUTTONS) {
            struct gpiod_line* line = gpiod_chip_get_line(chip, btn.offset);
            if (line) {
                gpiod_line_bulk_add(&button_lines, line);
            } else {
                std::cerr << "[GPIO] Warning: Could not get line for " << btn.name << "\n";
            }
        }

        struct gpiod_line_request_config req_cfg;
        req_cfg.consumer = "looper_buttons";
        req_cfg.request_type = GPIOD_LINE_REQUEST_EVENT_BOTH_EDGES;
        req_cfg.flags = GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP;

        if (gpiod_line_request_bulk(&button_lines, &req_cfg, nullptr) < 0) {
            // Fallback without BIAS_PULL_UP flag if kernel reports error
            req_cfg.flags = 0;
            if (gpiod_line_request_bulk(&button_lines, &req_cfg, nullptr) < 0) {
                std::cerr << "[GPIO] Warning: Failed to request button event lines.\n";
            } else {
                buttons_active = true;
            }
        } else {
            buttons_active = true;
        }

        if (buttons_active) {
            std::cout << "[GPIO] 6 Hardware buttons registered with kernel event timestamps.\n";
        }

        return (buttons_active || leds_active);
    }

    void start() {
        if (!buttons_active || running.load()) return;
        running.store(true);
        button_thread = std::thread(&Impl::buttonLoop, this);
    }

    void stop() {
        if (running.load()) {
            running.store(false);
            if (button_thread.joinable()) {
                button_thread.join();
            }
        }
        setLedColor(0, 0, 0);
        if (clip_leds_active) {
            if (clip_led_r) {
                gpiod_line_set_value(clip_led_r, 0);
                gpiod_line_release(clip_led_r);
            }
            if (clip_led_g) {
                gpiod_line_set_value(clip_led_g, 0);
                gpiod_line_release(clip_led_g);
            }
            clip_leds_active = false;
        }

        if (buttons_active) {
            gpiod_line_release_bulk(&button_lines);
            buttons_active = false;
        }

        if (leds_active) {
            if (led_r) gpiod_line_release(led_r);
            if (led_g) gpiod_line_release(led_g);
            if (led_b) gpiod_line_release(led_b);
            leds_active = false;
        }

        if (chip) {
            gpiod_chip_close(chip);
            chip = nullptr;
        }
    }

    void setLedColor(int r, int g, int b) {
        if (!leds_active) return;
        if (r != current_r) {
            gpiod_line_set_value(led_r, r);
            current_r = r;
        }
        if (g != current_g) {
            gpiod_line_set_value(led_g, g);
            current_g = g;
        }
        if (b != current_b) {
            gpiod_line_set_value(led_b, b);
            current_b = b;
        }
    }

    void setOff() {
        setLedColor(0, 0, 0);
        if (clip_leds_active) {
            if (clip_led_r) gpiod_line_set_value(clip_led_r, 0);
            if (clip_led_g) gpiod_line_set_value(clip_led_g, 0);
            current_clip_r = 0;
            current_clip_g = 0;
        }
    }

    void updateStatus(const LooperStatus& status) {
        if (leds_active) {
            int r = 0, g = 0, b = 0;

            switch (status.state) {
                case LooperState::IDLE:
                    r = 0; g = 0; b = 0;
                    break;

                case LooperState::RECORDING:
                    r = 1; g = 0; b = 0; // Solid RED
                    break;

                case LooperState::PLAYING:
                    if (status.is_fading_out) {
                        // Blink Green at 4 Hz during fade-out
                        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()
                        ).count();
                        g = ((now_ms / 125) % 2 == 0) ? 1 : 0;
                    } else {
                        g = 1; // Solid GREEN
                    }
                    r = 0; b = 0;
                    break;

                case LooperState::OVERDUB:
                    r = 1; g = 1; b = 0; // Solid YELLOW (Red + Green)
                    break;

                case LooperState::STOPPED:
                    r = 0; g = 0; b = 1; // Solid BLUE
                    break;
            }

            setLedColor(r, g, b);
        }

        // Update Overload (Clip) and Audio Signal LEDs
        if (clip_leds_active) {
            int clip_val = status.in_clipped ? 1 : 0;
            int sig_val = (status.in_peak > 0.02f) ? 1 : 0;

            if (clip_val != current_clip_r) {
                gpiod_line_set_value(clip_led_r, clip_val);
                current_clip_r = clip_val;
            }
            if (sig_val != current_clip_g) {
                gpiod_line_set_value(clip_led_g, sig_val);
                current_clip_g = sig_val;
            }
        }
    }

    void buttonLoop() {
        constexpr uint64_t DEBOUNCE_NS = 20'000'000ULL; // 20 ms debounce

        while (running.load(std::memory_order_relaxed)) {
            struct gpiod_line_bulk event_lines;
            gpiod_line_bulk_init(&event_lines);

            struct timespec ts = {0, 50000000}; // 50 ms timeout
            int ret = gpiod_line_event_wait_bulk(&button_lines, &ts, &event_lines);
            if (ret <= 0) continue;

            unsigned int num_events = gpiod_line_bulk_num_lines(&event_lines);
            for (unsigned int i = 0; i < num_events; ++i) {
                struct gpiod_line* line = gpiod_line_bulk_get_line(&event_lines, i);
                unsigned int offset = gpiod_line_offset(line);

                struct gpiod_line_event event;
                if (gpiod_line_event_read(line, &event) < 0) continue;

                // With pull-up bias: falling edge corresponds to contact closure to GND (button pressed)
                if (event.event_type == GPIOD_LINE_EVENT_FALLING_EDGE) {
                    uint64_t ts_ns = static_cast<uint64_t>(event.ts.tv_sec) * 1000000000ULL +
                                     static_cast<uint64_t>(event.ts.tv_nsec);
                    if (ts_ns == 0) {
                        ts_ns = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()
                            ).count()
                        );
                    }

                    // Debounce filter
                    if (offset < last_press_ts_ns.size()) {
                        if (ts_ns > last_press_ts_ns[offset] &&
                            (ts_ns - last_press_ts_ns[offset]) < DEBOUNCE_NS) {
                            continue; // Ignore bounce
                        }
                        last_press_ts_ns[offset] = ts_ns;
                    }

                    // Map pin to ActionKey
                    ActionKey action = ActionKey::NONE;
                    for (const auto& btn : HARDWARE_BUTTONS) {
                        if (btn.offset == offset) {
                            action = btn.action;
                            break;
                        }
                    }

                    if (action != ActionKey::NONE && callback) {
                        callback(action, ts_ns);
                    }
                }
            }
        }
    }
};

#else // !LOOPER_HAS_GPIOD

struct GpioManager::Impl {
    explicit Impl(KeyCallback cb) : callback(std::move(cb)) {}
    KeyCallback callback;
    bool init() { return true; }
    void start() {}
    void stop() {}
    void updateStatus(const LooperStatus&) {}
    void setLedColor(int, int, int) {}
    void setOff() {}
};

#endif // LOOPER_HAS_GPIOD

GpioManager::GpioManager(KeyCallback callback)
    : callback_(std::move(callback)), impl_(std::make_unique<Impl>(callback_)) {
}

GpioManager::~GpioManager() {
    stop();
}

bool GpioManager::start() {
    if (!impl_->init()) {
        return false;
    }
    impl_->start();
    return true;
}

void GpioManager::stop() {
    if (impl_) {
        impl_->stop();
    }
}

void GpioManager::updateStatus(const LooperStatus& status) {
    if (impl_) {
        impl_->updateStatus(status);
    }
}

void GpioManager::setOff() {
    if (impl_) {
        impl_->setOff();
    }
}

} // namespace looper
