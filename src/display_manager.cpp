#include "display_manager.hpp"

#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <gpiod.h>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace looper {

// Simple, crisp 8x8 bitmap font for ASCII 32..126
// Each byte is one row of 8 horizontal pixels (MSB to LSB)
static const uint8_t FONT8X8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 32 ' '
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // 33 '!'
    {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, // 34 '"'
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, // 35 '#'
    {0x18,0x7E,0xC0,0x7C,0x06,0xFC,0x18,0x00}, // 36 '$'
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, // 37 '%'
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, // 38 '&'
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // 39 '''
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, // 40 '('
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, // 41 ')'
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // 42 '*'
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, // 43 '+'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, // 44 ','
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, // 45 '-'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // 46 '.'
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, // 47 '/'
    {0x7C,0xC6,0xCE,0xD6,0xE6,0xC6,0x7C,0x00}, // 48 '0'
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, // 49 '1'
    {0x7C,0xC6,0x06,0x1C,0x30,0x60,0xFE,0x00}, // 50 '2'
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, // 51 '3'
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, // 52 '4'
    {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00}, // 53 '5'
    {0x78,0x0C,0x18,0xFC,0xC6,0xC6,0x7C,0x00}, // 54 '6'
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00}, // 55 '7'
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, // 56 '8'
    {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, // 57 '9'
    {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00}, // 58 ':'
    {0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00}, // 59 ';'
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // 60 '<'
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, // 61 '='
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, // 62 '>'
    {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00}, // 63 '?'
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00}, // 64 '@'
    {0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 65 'A'
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, // 66 'B'
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, // 67 'C'
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, // 68 'D'
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, // 69 'E'
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, // 70 'F'
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3A,0x00}, // 71 'G'
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 72 'H'
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 73 'I'
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, // 74 'J'
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, // 75 'K'
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, // 76 'L'
    {0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0x00}, // 77 'M'
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, // 78 'N'
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 79 'O'
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, // 80 'P'
    {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x06}, // 81 'Q'
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, // 82 'R'
    {0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00}, // 83 'S'
    {0x7E,0x7E,0x5A,0x18,0x18,0x18,0x3C,0x00}, // 84 'T'
    {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 85 'U'
    {0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00}, // 86 'V'
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, // 87 'W'
    {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00}, // 88 'X'
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00}, // 89 'Y'
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00}, // 90 'Z'
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, // 91 '['
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, // 92 '\'
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, // 93 ']'
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, // 94 '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00}, // 95 '_'
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, // 96 '`'
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00}, // 97 'a'
    {0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00}, // 98 'b'
    {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00}, // 99 'c'
    {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00}, // 100 'd'
    {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00}, // 101 'e'
    {0x3C,0x66,0x60,0xF8,0x60,0x60,0xF0,0x00}, // 102 'f'
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8}, // 103 'g'
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00}, // 104 'h'
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, // 105 'i'
    {0x06,0x00,0x06,0x06,0x06,0x06,0xC6,0x78}, // 106 'j'
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00}, // 107 'k'
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 108 'l'
    {0x00,0x00,0xE6,0xFF,0xDB,0xC9,0xC9,0x00}, // 109 'm'
    {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00}, // 110 'n'
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00}, // 111 'o'
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0}, // 112 'p'
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E}, // 113 'q'
    {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00}, // 114 'r'
    {0x00,0x00,0x7E,0xC0,0x7C,0x06,0xFC,0x00}, // 115 's'
    {0x30,0x30,0xFC,0x30,0x30,0x34,0x18,0x00}, // 116 't'
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00}, // 117 'u'
    {0x00,0x00,0xC6,0xC6,0x6C,0x38,0x10,0x00}, // 118 'v'
    {0x00,0x00,0xC6,0xD6,0xFE,0xEE,0x6C,0x00}, // 119 'w'
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, // 120 'x'
    {0x00,0x00,0xC6,0xCC,0x78,0x30,0x60,0xC0}, // 121 'y'
    {0x00,0x00,0xFE,0x4C,0x18,0x32,0xFE,0x00}, // 122 'z'
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, // 123 '{'
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // 124 '|'
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, // 125 '}'
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}  // 126 '~'
};

DisplayManager::DisplayManager()
    : m_framebuffer(WIDTH * HEIGHT * 2, 0) {
}

DisplayManager::~DisplayManager() {
    stop();
}

bool DisplayManager::start() {
    if (m_running.load()) return true;

    if (!initHardware()) {
        std::cerr << "[DISPLAY] Warning: ST7735 SPI display hardware initialization failed. Continuing without display." << std::endl;
        return false;
    }

    initDisplay();
    clear(rgb565(18, 18, 20));
    flushBuffer();

    m_running.store(true);
    m_render_thread = std::thread(&DisplayManager::renderLoop, this);
    std::cout << "[DISPLAY] ST7735 1.77\" TFT SPI display active (160x128 @ 30 FPS)" << std::endl;
    return true;
}

void DisplayManager::stop() {
    if (m_running.load()) {
        m_running.store(false);
        if (m_render_thread.joinable()) {
            m_render_thread.join();
        }
    }
    closeHardware();
}

bool DisplayManager::initHardware() {
    // 1. Open SPI0 device
    const char* spi_dev = "/dev/spidev0.0";
    m_spi_fd = open(spi_dev, O_WRONLY);
    if (m_spi_fd < 0) {
        std::cerr << "[DISPLAY] Cannot open " << spi_dev << std::endl;
        return false;
    }

    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed = 24000000; // 24 MHz

    if (ioctl(m_spi_fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(m_spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(m_spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        std::cerr << "[DISPLAY] Failed to configure SPI parameters" << std::endl;
        close(m_spi_fd);
        m_spi_fd = -1;
        return false;
    }

    // 2. Open GPIO chip for Reset and DC
    m_chip = gpiod_chip_open_by_name("gpiochip0");
    if (!m_chip) {
        std::cerr << "[DISPLAY] Cannot open gpiochip0" << std::endl;
        close(m_spi_fd);
        m_spi_fd = -1;
        return false;
    }

    // Pin 16: PC4 -> Line 68 (Reset)
    // Pin 18: PC7 -> Line 71 (Data / Command)
    m_line_res = gpiod_chip_get_line(m_chip, 68);
    m_line_dc = gpiod_chip_get_line(m_chip, 71);

    if (!m_line_res || !m_line_dc) {
        std::cerr << "[DISPLAY] Failed to get GPIO lines (68 or 71)" << std::endl;
        closeHardware();
        return false;
    }

    if (gpiod_line_request_output(m_line_res, "st7735_res", 1) < 0 ||
        gpiod_line_request_output(m_line_dc, "st7735_dc", 1) < 0) {
        std::cerr << "[DISPLAY] Failed to request GPIO lines as outputs" << std::endl;
        closeHardware();
        return false;
    }

    m_hardware_ready = true;
    return true;
}

void DisplayManager::closeHardware() {
    if (m_line_res) {
        gpiod_line_release(m_line_res);
        m_line_res = nullptr;
    }
    if (m_line_dc) {
        gpiod_line_release(m_line_dc);
        m_line_dc = nullptr;
    }
    if (m_chip) {
        gpiod_chip_close(m_chip);
        m_chip = nullptr;
    }
    if (m_spi_fd >= 0) {
        close(m_spi_fd);
        m_spi_fd = -1;
    }
    m_hardware_ready = false;
}

void DisplayManager::writeCommand(uint8_t cmd) {
    if (m_spi_fd < 0 || !m_line_dc) return;
    gpiod_line_set_value(m_line_dc, 0);
    write(m_spi_fd, &cmd, 1);
}

void DisplayManager::writeData(const uint8_t* data, size_t len) {
    if (m_spi_fd < 0 || !m_line_dc || !data || len == 0) return;
    gpiod_line_set_value(m_line_dc, 1);
    constexpr size_t CHUNK_SIZE = 4096;
    size_t offset = 0;
    while (offset < len) {
        size_t to_write = std::min(CHUNK_SIZE, len - offset);
        write(m_spi_fd, data + offset, to_write);
        offset += to_write;
    }
}

void DisplayManager::writeDataByte(uint8_t byte) {
    writeData(&byte, 1);
}

void DisplayManager::resetHardware() {
    if (!m_line_res) return;
    gpiod_line_set_value(m_line_res, 1);
    usleep(10000);
    gpiod_line_set_value(m_line_res, 0);
    usleep(50000);
    gpiod_line_set_value(m_line_res, 1);
    usleep(50000);
}

void DisplayManager::initDisplay() {
    resetHardware();

    writeCommand(0x01); // SWRESET
    usleep(120000);
    writeCommand(0x11); // SLPOUT
    usleep(120000);

    // Frame rate control
    writeCommand(0xB1);
    const uint8_t f1[] = {0x01, 0x2C, 0x2D};
    writeData(f1, sizeof(f1));
    writeCommand(0xB2);
    const uint8_t f2[] = {0x01, 0x2C, 0x2D};
    writeData(f2, sizeof(f2));
    writeCommand(0xB3);
    const uint8_t f3[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
    writeData(f3, sizeof(f3));

    writeCommand(0xB4); // Display inversion control
    writeDataByte(0x07);

    // Power sequence
    writeCommand(0xC0);
    const uint8_t p1[] = {0xA2, 0x02, 0x84};
    writeData(p1, sizeof(p1));
    writeCommand(0xC1);
    writeDataByte(0xC5);
    writeCommand(0xC2);
    const uint8_t p3[] = {0x0A, 0x00};
    writeData(p3, sizeof(p3));
    writeCommand(0xC3);
    const uint8_t p4[] = {0x8A, 0x2A};
    writeData(p4, sizeof(p4));
    writeCommand(0xC4);
    const uint8_t p5[] = {0x8A, 0xEE};
    writeData(p5, sizeof(p5));
    writeCommand(0xC5);
    writeDataByte(0x0E);

    writeCommand(0x20); // INVOFF

    // Memory Data Access Control (Landscape BGR)
    writeCommand(0x36);
    writeDataByte(0xA0);

    // Color mode: 16-bit RGB565
    writeCommand(0x3A);
    writeDataByte(0x05);

    // Gamma sequence
    writeCommand(0xE0);
    const uint8_t g1[] = {0x02, 0x1c, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2d,
                          0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10};
    writeData(g1, sizeof(g1));
    writeCommand(0xE1);
    const uint8_t g2[] = {0x03, 0x1d, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                          0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10};
    writeData(g2, sizeof(g2));

    writeCommand(0x13); // Normal display on
    usleep(10000);
    writeCommand(0x29); // Display on
    usleep(50000);
}

void DisplayManager::setWindow(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1) {
    writeCommand(0x2A); // CASET
    const uint8_t x_data[] = {0x00, x0, 0x00, x1};
    writeData(x_data, sizeof(x_data));

    writeCommand(0x2B); // RASET
    const uint8_t y_data[] = {0x00, y0, 0x00, y1};
    writeData(y_data, sizeof(y_data));

    writeCommand(0x2C); // RAMWR
}

void DisplayManager::flushBuffer() {
    setWindow(0, 0, WIDTH - 1, HEIGHT - 1);
    writeData(m_framebuffer.data(), m_framebuffer.size());
}

void DisplayManager::clear(uint16_t color) {
    uint8_t hi = color >> 8;
    uint8_t lo = color & 0xFF;
    for (size_t i = 0; i < m_framebuffer.size(); i += 2) {
        m_framebuffer[i] = hi;
        m_framebuffer[i + 1] = lo;
    }
}

void DisplayManager::drawPixel(int x, int y, uint16_t color) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    size_t idx = (y * WIDTH + x) * 2;
    m_framebuffer[idx] = color >> 8;
    m_framebuffer[idx + 1] = color & 0xFF;
}

void DisplayManager::drawLine(int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (true) {
        drawPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void DisplayManager::drawRect(int x0, int y0, int x1, int y1, uint16_t color) {
    drawLine(x0, y0, x1, y0, color);
    drawLine(x1, y0, x1, y1, color);
    drawLine(x1, y1, x0, y1, color);
    drawLine(x0, y1, x0, y0, color);
}

void DisplayManager::drawFilledRect(int x0, int y0, int x1, int y1, uint16_t color) {
    int min_x = std::max(0, std::min(x0, x1));
    int max_x = std::min(WIDTH - 1, std::max(x0, x1));
    int min_y = std::max(0, std::min(y0, y1));
    int max_y = std::min(HEIGHT - 1, std::max(y0, y1));

    uint8_t hi = color >> 8;
    uint8_t lo = color & 0xFF;

    for (int y = min_y; y <= max_y; ++y) {
        size_t row_start = (y * WIDTH + min_x) * 2;
        for (int x = min_x; x <= max_x; ++x) {
            m_framebuffer[row_start++] = hi;
            m_framebuffer[row_start++] = lo;
        }
    }
}

void DisplayManager::drawChar(int x, int y, char c, uint16_t color, int scale) {
    if (c < 32 || c > 126) c = ' ';
    const uint8_t* glyph = FONT8X8[c - 32];

    for (int row = 0; row < 8; ++row) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; ++col) {
            if (bits & (0x80 >> col)) {
                if (scale == 1) {
                    drawPixel(x + col, y + row, color);
                } else {
                    drawFilledRect(x + col * scale, y + row * scale,
                                   x + (col + 1) * scale - 1, y + (row + 1) * scale - 1, color);
                }
            }
        }
    }
}

void DisplayManager::drawText(int x, int y, const std::string& text, uint16_t color, int scale) {
    int cur_x = x;
    int step = 8 * scale;
    for (char c : text) {
        drawChar(cur_x, y, c, color, scale);
        cur_x += step;
    }
}

void DisplayManager::updateStatus(const LooperStatus& status, float loop_gain, const std::string& info_message) {
    std::lock_guard<std::mutex> lock(m_status_mutex);
    m_status = status;
    m_loop_gain = loop_gain;
    m_info_message = info_message;
}

void DisplayManager::renderLoop() {
    constexpr auto FRAME_TIME = std::chrono::milliseconds(33); // ~30 FPS

    while (m_running.load()) {
        auto t_start = std::chrono::steady_clock::now();

        renderFrame();
        flushBuffer();

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_start);
        if (elapsed < FRAME_TIME) {
            std::this_thread::sleep_for(FRAME_TIME - elapsed);
        }
    }
}

void DisplayManager::renderFrame() {
    LooperStatus s;
    float gain = 1.0f;
    std::string info = "";
    {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        s = m_status;
        gain = m_loop_gain;
        info = m_info_message;
    }

    // Color Palette (Minimalist)
    const uint16_t C_BG     = rgb565(18, 18, 20);     // Matte Obsidian Black
    const uint16_t C_WHITE  = rgb565(240, 242, 245);  // Crisp Stark White
    const uint16_t C_MUTED  = rgb565(85, 90, 100);    // Subtle Neutral Gray
    const uint16_t C_LINE   = rgb565(38, 40, 48);     // Subtle Rail Gray
    const uint16_t C_RED    = rgb565(255, 45, 55);    // Alert Red
    const uint16_t C_AMBER  = rgb565(255, 80, 0);     // Warm Amber
    const uint16_t C_CYAN   = rgb565(0, 220, 210);    // Cyan Accent

    clear(C_BG);

    // 1. TOP HEADER (Y: 6..22)
    drawText(10, 8, "LOOP 01", C_MUTED, 1);
    if (std::abs(gain - 1.0f) > 0.01f) {
        int pct = static_cast<int>(std::round(gain * 100.0f));
        drawText(68, 8, std::to_string(pct) + "%", C_MUTED, 1);
    }

    // State Badge (Clean, stark, unboxed)
    if (s.state == LooperState::RECORDING) {
        // Red solid dot + REC
        drawFilledRect(114, 10, 118, 14, C_RED);
        drawText(124, 8, "REC", C_RED, 1);
    } else if (s.state == LooperState::PLAYING) {
        // Play indicator + PLAY
        drawPixel(114, 9, C_WHITE);
        drawFilledRect(114, 9, 115, 15, C_WHITE);
        drawFilledRect(116, 10, 117, 14, C_WHITE);
        drawPixel(118, 12, C_WHITE);
        drawText(124, 8, "PLAY", C_WHITE, 1);
    } else if (s.state == LooperState::OVERDUB) {
        drawText(112, 8, "+DUB", C_AMBER, 1);
    } else {
        // STOP
        drawFilledRect(114, 10, 118, 14, C_MUTED);
        drawText(124, 8, "STOP", C_MUTED, 1);
    }

    // Divider Line
    drawLine(10, 22, WIDTH - 10, 22, C_LINE);

    // 2. HERO: Big confident time & beat (Y: 28..58)
    std::ostringstream ss_time;
    if (s.state == LooperState::RECORDING) {
        // Recording active time
        ss_time << std::fixed << std::setprecision(1) << std::setfill('0')
                << std::setw(4) << s.current_sec;
    } else if (s.total_frames > 0) {
        // Playback time / loop length
        ss_time << std::fixed << std::setprecision(1) << std::setfill('0')
                << std::setw(4) << s.current_sec;
    } else {
        ss_time << "--.-";
    }

    // Draw Big Time (Scale = 3 -> 24x24 px per char)
    drawText(10, 28, ss_time.str(), C_WHITE, 3);
    drawText(108, 42, "s", C_MUTED, 1);

    // Minimal Beat Counter: 4 small geometric boxes
    // Estimate beat from playhead progress or cycle
    int active_beat = 0;
    if (s.total_frames > 0 && s.state != LooperState::STOPPED) {
        float frac = static_cast<float>(s.playhead_frames) / static_cast<float>(s.total_frames);
        active_beat = static_cast<int>(frac * 4.0f) % 4;
    }

    drawText(122, 28, "BEAT", C_MUTED, 1);
    for (int b = 0; b < 4; ++b) {
        int bx = 122 + b * 7;
        int by = 40;
        if (b == active_beat && s.state != LooperState::STOPPED) {
            drawFilledRect(bx, by, bx + 4, by + 4, C_WHITE);
        } else {
            drawRect(bx, by, bx + 4, by + 4, C_LINE);
        }
    }

    // 3. PROGRESS TIMELINE (Y: 62..70)
    constexpr int RAIL_X0 = 10;
    constexpr int RAIL_X1 = WIDTH - 10;
    constexpr int RAIL_Y = 64;
    constexpr int RAIL_WIDTH = RAIL_X1 - RAIL_X0;

    // Subtle background rail (2px)
    drawLine(RAIL_X0, RAIL_Y, RAIL_X1, RAIL_Y, C_LINE);
    drawLine(RAIL_X0, RAIL_Y + 1, RAIL_X1, RAIL_Y + 1, C_LINE);

    if (s.total_frames > 0) {
        float progress = std::clamp(static_cast<float>(s.playhead_frames) / static_cast<float>(s.total_frames), 0.0f, 1.0f);
        int play_x = RAIL_X0 + static_cast<int>(progress * RAIL_WIDTH);

        // Solid progress fill
        drawLine(RAIL_X0, RAIL_Y, play_x, RAIL_Y, C_WHITE);
        drawLine(RAIL_X0, RAIL_Y + 1, play_x, RAIL_Y + 1, C_WHITE);

        // 6px vertical playhead notch
        drawLine(play_x, RAIL_Y - 3, play_x, RAIL_Y + 4, C_WHITE);
    }

    // 4. AUDIO LEVEL METER (Y: 76..88)
    drawText(10, 78, "IN", C_MUTED, 1);

    constexpr int METER_X0 = 28;
    constexpr int METER_X1 = WIDTH - 12;
    constexpr int METER_Y = 81;
    constexpr int METER_LEN = METER_X1 - METER_X0;

    // Ballistic smoothing for visual display
    if (s.in_peak > m_meter_level) {
        m_meter_level = s.in_peak; // Fast attack
    } else {
        m_meter_level = std::max(0.0f, m_meter_level - 0.035f); // Smooth exponential decay
    }

    if (s.in_clipped) {
        m_clip_hold = 8; // 8 frames ~250ms hold
    } else if (m_clip_hold > 0) {
        --m_clip_hold;
    }

    // 2px meter base line
    drawLine(METER_X0, METER_Y, METER_X1, METER_Y, C_LINE);
    drawLine(METER_X0, METER_Y + 1, METER_X1, METER_Y + 1, C_LINE);

    // Active level bar
    int fill_w = static_cast<int>(m_meter_level * METER_LEN);
    if (fill_w > 0) {
        int fill_x = std::min(METER_X1, METER_X0 + fill_w);
        drawLine(METER_X0, METER_Y, fill_x, METER_Y, C_WHITE);
        drawLine(METER_X0, METER_Y + 1, fill_x, METER_Y + 1, C_WHITE);
    }

    // Red clip tick at the far right
    if (m_clip_hold > 0 || m_meter_level >= 0.88f) {
        drawFilledRect(METER_X1 - 2, METER_Y - 2, METER_X1, METER_Y + 3, C_RED);
    }

    // 5. FOOTER: Status / Notifications (Y: 94..124)
    drawLine(10, 94, WIDTH - 10, 94, C_LINE);

    if (!info.empty()) {
        // Notification banner (e.g. "Saving: loop.wav", "Monitor: ANALOG")
        drawText(10, 104, info.substr(0, 18), C_CYAN, 1);
    } else {
        // Clean mode indicators: REV, DUB, FADE, UNDO
        int fx = 12;

        // REV
        if (s.is_reversed) {
            drawText(fx, 102, "REV", C_WHITE, 1);
            drawLine(fx, 114, fx + 22, 114, C_WHITE);
        } else {
            drawText(fx, 102, "REV", C_MUTED, 1);
        }
        fx += 36;

        // DUB
        if (s.state == LooperState::OVERDUB) {
            drawText(fx, 102, "DUB", C_AMBER, 1);
            drawLine(fx, 114, fx + 22, 114, C_AMBER);
        } else {
            drawText(fx, 102, "DUB", C_MUTED, 1);
        }
        fx += 36;

        // FADE
        if (s.is_fading_out) {
            drawText(fx, 102, "FADE", C_WHITE, 1);
            drawLine(fx, 114, fx + 30, 114, C_WHITE);
        } else {
            drawText(fx, 102, "FADE", C_MUTED, 1);
        }
        fx += 40;

        // UNDO
        if (s.undo_available) {
            drawText(fx, 102, "UNDO", C_WHITE, 1);
            drawLine(fx, 114, fx + 30, 114, C_WHITE);
        } else if (s.redo_available) {
            drawText(fx, 102, "REDO", C_CYAN, 1);
            drawLine(fx, 114, fx + 30, 114, C_CYAN);
        } else {
            drawText(fx, 102, "UNDO", C_MUTED, 1);
        }
    }
}

} // namespace looper
