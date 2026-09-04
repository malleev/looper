#include "input_manager.hpp"
#include <iostream>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <termios.h>
#include <linux/input.h>
#include <cstring>
#include <dirent.h>

namespace looper {

InputManager::InputManager(KeyCallback callback)
    : callback_(std::move(callback)) {
}

InputManager::~InputManager() {
    stop();
}

void InputManager::scanEvdevDevices() {
    for (int i = 0; i < 16; ++i) {
        std::string path = "/dev/input/event" + std::to_string(i);
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        // Check if device supports keys
        unsigned long ev_bits = 0;
        if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), &ev_bits) >= 0) {
            if (ev_bits & (1 << EV_KEY)) {
                // Check if device has SPACE key
                uint8_t key_bits[(KEY_MAX + 7) / 8];
                memset(key_bits, 0, sizeof(key_bits));
                if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0) {
                    if (key_bits[KEY_SPACE / 8] & (1 << (KEY_SPACE % 8))) {
                        char name[256] = "Unknown";
                        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
                        std::cout << "[INPUT] Found physical keyboard: " << name 
                                  << " (" << path << ")" << std::endl;
                        evdev_fds_.push_back(fd);
                        continue;
                    }
                }
            }
        }
        close(fd);
    }
}

bool InputManager::start() {
    if (running_.load()) return true;

    scanEvdevDevices();

    running_.store(true);
    worker_thread_ = std::thread(&InputManager::workerLoop, this);
    return true;
}

void InputManager::stop() {
    if (!running_.load()) return;

    running_.store(false);
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    for (int fd : evdev_fds_) {
        close(fd);
    }
    evdev_fds_.clear();
}

void InputManager::workerLoop() {
    // Configure STDIN for non-canonical non-echo if available
    struct termios orig_termios;
    bool has_tty = isatty(STDIN_FILENO);
    if (has_tty) {
        tcgetattr(STDIN_FILENO, &orig_termios);
        struct termios raw = orig_termios;
        raw.c_lflag &= ~(ECHO | ICANON);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }

    while (running_.load(std::memory_order_relaxed)) {
        std::vector<struct pollfd> pfd;
        
        if (has_tty) {
            struct pollfd p;
            p.fd = STDIN_FILENO;
            p.events = POLLIN;
            p.revents = 0;
            p.revents = 0;
            pfd.push_back(p);
        }

        for (int fd : evdev_fds_) {
            struct pollfd p;
            p.fd = fd;
            p.events = POLLIN;
            p.revents = 0;
            pfd.push_back(p);
        }

        int ret = poll(pfd.data(), pfd.size(), 50); // 50ms poll
        if (ret <= 0) continue;

        // 1. Check STDIN
        size_t idx = 0;
        if (has_tty) {
            if (pfd[0].revents & POLLIN) {
                char ch = 0;
                while (read(STDIN_FILENO, &ch, 1) > 0) {
                    ActionKey ak = ActionKey::NONE;
                    switch (ch) {
                        case ' ': ak = ActionKey::ACTION; break;
                        case 's': case 'S': ak = ActionKey::STOP; break;
                        case 'c': case 'C': ak = ActionKey::CLEAR; break;
                        case 'u': case 'U': ak = ActionKey::UNDO; break;
                        case 'r': case 'R': ak = ActionKey::REVERSE; break;
                        case 'f': case 'F': ak = ActionKey::FADE; break;
                        case 'm': case 'M': ak = ActionKey::DRY_TOGGLE; break;
                        case '+': case '=': ak = ActionKey::VOL_UP; break;
                        case '-': case '_': ak = ActionKey::VOL_DOWN; break;
                        case 'q': case 'Q': ak = ActionKey::QUIT; break;
                        default: break;
                    }
                    if (ak != ActionKey::NONE && callback_) callback_(ak);
                }
            }
            idx = 1;
        }

        // 2. Check evdev (physical USB keyboard)
        for (; idx < pfd.size(); ++idx) {
            if (pfd[idx].revents & POLLIN) {
                struct input_event ev;
                while (read(pfd[idx].fd, &ev, sizeof(ev)) == sizeof(ev)) {
                    if (ev.type == EV_KEY && ev.value == 1) { // Key down event
                        ActionKey ak = ActionKey::NONE;
                        switch (ev.code) {
                            case KEY_SPACE: ak = ActionKey::ACTION; break;
                            case KEY_S: ak = ActionKey::STOP; break;
                            case KEY_C: ak = ActionKey::CLEAR; break;
                            case KEY_U: ak = ActionKey::UNDO; break;
                            case KEY_R: ak = ActionKey::REVERSE; break;
                            case KEY_F: ak = ActionKey::FADE; break;
                            case KEY_M: ak = ActionKey::DRY_TOGGLE; break;
                            case KEY_EQUAL: case KEY_KPPLUS: ak = ActionKey::VOL_UP; break;
                            case KEY_MINUS: case KEY_KPMINUS: ak = ActionKey::VOL_DOWN; break;
                            case KEY_Q: case KEY_ESC: ak = ActionKey::QUIT; break;
                            default: break;
                        }
                        if (ak != ActionKey::NONE && callback_) callback_(ak);
                    }
                }
            }
        }
    }

    if (has_tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    }
}

} // namespace looper
