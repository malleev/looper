#pragma once

#include "types.hpp"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>

struct gpiod_chip;
struct gpiod_line;

namespace looper {

class DisplayManager {
public:
    static constexpr int WIDTH = 160;
    static constexpr int HEIGHT = 128;

    DisplayManager();
    ~DisplayManager();

    // Disable copy
    DisplayManager(const DisplayManager&) = delete;
    DisplayManager& operator=(const DisplayManager&) = delete;

    // Start background render worker thread (returns false if hardware unavailable, looper continues gracefully)
    bool start();
    void stop();

    // Thread-safe update from main loop
    void updateStatus(const LooperStatus& status, float loop_gain, const std::string& info_message = "");

private:
    void renderLoop();
    bool initHardware();
    void closeHardware();
    void initDisplay();

    // Low-level SPI / GPIO
    void writeCommand(uint8_t cmd);
    void writeData(const uint8_t* data, size_t len);
    void writeDataByte(uint8_t byte);
    void setWindow(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1);
    void resetHardware();

    // Drawing primitives (writes directly to m_framebuffer)
    void clear(uint16_t color);
    void drawPixel(int x, int y, uint16_t color);
    void drawLine(int x0, int y0, int x1, int y1, uint16_t color);
    void drawRect(int x0, int y0, int x1, int y1, uint16_t color);
    void drawFilledRect(int x0, int y0, int x1, int y1, uint16_t color);
    void drawChar(int x, int y, char c, uint16_t color, int scale = 1);
    void drawText(int x, int y, const std::string& text, uint16_t color, int scale = 1);

    // Frame rendering
    void renderFrame();
    void flushBuffer();

    // RGB565 color helper
    static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
        return ((static_cast<uint16_t>(r) & 0xF8) << 8) |
               ((static_cast<uint16_t>(g) & 0xFC) << 3) |
               (static_cast<uint16_t>(b) >> 3);
    }

    // Hardware handles
    int m_spi_fd{-1};
    gpiod_chip* m_chip{nullptr};
    gpiod_line* m_line_res{nullptr};
    gpiod_line* m_line_dc{nullptr};
    bool m_hardware_ready{false};

    // Thread management
    std::atomic<bool> m_running{false};
    std::thread m_render_thread;

    // Synchronized status snapshot
    mutable std::mutex m_status_mutex;
    LooperStatus m_status{};
    float m_loop_gain{1.0f};
    std::string m_info_message{""};

    // Framebuffer: 160 x 128 x 2 bytes
    std::vector<uint8_t> m_framebuffer;

    // Smoothed visual meters
    float m_meter_level{0.0f};
    int m_clip_hold{0};
};

} // namespace looper
