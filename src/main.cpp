#include "types.hpp"
#include "looper_engine.hpp"
#include "audio_device.hpp"
#include "input_manager.hpp"

#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>
#include <cmath>

std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running.store(false);
}

void printStatus(const looper::LooperStatus& status, float loop_gain, float dry_gain) {
    std::cout << "\r\033[K"; // Clear line

    // State badge
    std::string state_str = looper::stateToString(status.state);
    std::string color = "\033[1;37m";
    if (status.state == looper::LooperState::RECORDING) color = "\033[1;31m"; // Red
    else if (status.state == looper::LooperState::PLAYING) color = "\033[1;32m"; // Green
    else if (status.state == looper::LooperState::OVERDUB) color = "\033[1;33m"; // Yellow
    else if (status.state == looper::LooperState::STOPPED) color = "\033[1;34m"; // Blue

    std::cout << color << "[" << std::setw(9) << state_str << "]\033[0m ";

    // Live Input VU-Meter (shows if mic/guitar is receiving sound!)
    constexpr int METER_WIDTH = 8;
    int meter_fill = std::clamp(static_cast<int>(status.in_peak * 10 * METER_WIDTH), 0, METER_WIDTH);
    std::string meter_col = (status.in_peak > 0.8f) ? "\033[1;31m" : "\033[1;32m";
    std::cout << "IN:[" << meter_col;
    for (int i = 0; i < METER_WIDTH; ++i) {
        if (i < meter_fill) std::cout << "#";
        else std::cout << "-";
    }
    std::cout << "\033[0m] ";

    // Loop Progress Bar
    constexpr int BAR_WIDTH = 16;
    int progress = 0;
    if (status.total_frames > 0) {
        progress = static_cast<int>((static_cast<float>(status.playhead_frames) / status.total_frames) * BAR_WIDTH);
    }

    std::cout << "[";
    for (int i = 0; i < BAR_WIDTH; ++i) {
        if (i < progress) std::cout << "=";
        else if (i == progress) std::cout << ">";
        else std::cout << " ";
    }
    std::cout << "] ";

    // Time
    std::cout << std::fixed << std::setprecision(1)
              << status.current_sec << "s / " << status.total_sec << "s | ";

    // Flags
    std::cout << "REV: " << (status.is_reversed ? "\033[1;35mON\033[0m" : "OFF") << " | "
              << "FADE: " << (status.is_fading_out ? "\033[1;36mON\033[0m" : "OFF") << " | "
              << "UNDO: " << (status.undo_available ? "\033[1;32mYES\033[0m" : "NO") << " | "
              << "VOL: " << static_cast<int>(loop_gain * 100) << "% | "
              << "DRY: " << (dry_gain > 0.05f ? "\033[1;33mON\033[0m" : "OFF")
              << std::flush;
}

int main(int argc, char* argv[]) {
    std::string alsa_device = "hw:CARD=M1,DEV=0";
    if (argc > 1) {
        alsa_device = argv[1];
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::cout << "=====================================================" << std::endl;
    std::cout << "   ORANGE PI GUITAR LOOPER (Version C - Terminal UI) " << std::endl;
    std::cout << "=====================================================" << std::endl;
    std::cout << "Audio device: " << alsa_device << std::endl;
    std::cout << "\nControls (Works from both SSH and USB keyboard plugged into OPi!):" << std::endl;
    std::cout << "  [SPACE]     : Rec -> Play -> Overdub -> Play" << std::endl;
    std::cout << "  [S]         : Stop playback" << std::endl;
    std::cout << "  [C]         : Clear loop & reset to IDLE" << std::endl;
    std::cout << "  [U]         : Undo / Redo last overdub" << std::endl;
    std::cout << "  [R]         : Toggle Reverse playback" << std::endl;
    std::cout << "  [F]         : Trigger smooth Fade-Out to stop" << std::endl;
    std::cout << "  [M]         : Toggle software Dry Monitor (on/off)" << std::endl;
    std::cout << "  [+] / [-]   : Adjust loop volume" << std::endl;
    std::cout << "  [Q]         : Quit" << std::endl;
    std::cout << "=====================================================\n" << std::endl;

    looper::LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128; // ~2.6 ms buffer
    config.dry_gain = 0.0f;   // Direct Monitor recommended
    config.loop_gain = 1.0f;
    config.fade_out_sec = 3.0f;

    looper::LooperEngine engine(config);
    looper::AudioDevice audio_device(alsa_device, engine, config);

    std::cout << "[SYSTEM] Initializing audio device..." << std::endl;
    if (!audio_device.start()) {
        std::cerr << "[ERROR] Failed to start audio device on " << alsa_device << std::endl;
        return 1;
    }
    std::cout << "[SYSTEM] Audio engine started successfully!" << std::endl;

    float current_loop_gain = config.loop_gain;
    float current_dry_gain = config.dry_gain;

    // Initialize InputManager (handles both SSH stdin and physical USB keyboards via evdev)
    looper::InputManager input_manager([&](looper::ActionKey key) {
        switch (key) {
            case looper::ActionKey::ACTION:
                engine.triggerAction();
                break;
            case looper::ActionKey::STOP:
                engine.triggerStop();
                break;
            case looper::ActionKey::CLEAR:
                engine.triggerClear();
                break;
            case looper::ActionKey::UNDO:
                engine.triggerUndoRedo();
                break;
            case looper::ActionKey::REVERSE:
                engine.toggleReverse();
                break;
            case looper::ActionKey::FADE:
                engine.triggerFadeOut();
                break;
            case looper::ActionKey::DRY_TOGGLE:
                current_dry_gain = (current_dry_gain > 0.05f) ? 0.0f : 1.0f;
                engine.setDryGain(current_dry_gain);
                break;
            case looper::ActionKey::VOL_UP:
                current_loop_gain = std::min(2.0f, current_loop_gain + 0.05f);
                engine.setLoopGain(current_loop_gain);
                break;
            case looper::ActionKey::VOL_DOWN:
                current_loop_gain = std::max(0.0f, current_loop_gain - 0.05f);
                engine.setLoopGain(current_loop_gain);
                break;
            case looper::ActionKey::QUIT:
                g_running.store(false);
                break;
            default:
                break;
        }
    });

    input_manager.start();
    std::cout << "[SYSTEM] Input manager ready. Waiting for pedal / keyboard triggers...\n" << std::endl;

    while (g_running.load()) {
        printStatus(engine.getStatus(), current_loop_gain, current_dry_gain);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << "\n\n[SYSTEM] Stopping audio engine..." << std::endl;
    input_manager.stop();
    audio_device.stop();
    std::cout << "[SYSTEM] Clean shutdown complete. Goodbye!" << std::endl;

    return 0;
}
