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
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

std::atomic<bool> g_running{true};
std::string g_info_message = "";
std::chrono::steady_clock::time_point g_info_expire;

void setInfoMessage(const std::string& msg, int display_sec = 3) {
    g_info_message = msg;
    g_info_expire = std::chrono::steady_clock::now() + std::chrono::seconds(display_sec);
}

void signalHandler(int) {
    g_running.store(false);
}

void printStatus(const looper::LooperStatus& status, float loop_gain, float dry_gain) {
    std::cout << "\r\033[2K"; // Clear line

    // If there's an active notification message, show it
    if (!g_info_message.empty()) {
        if (std::chrono::steady_clock::now() < g_info_expire) {
            std::cout << "\033[1;36m>>> " << g_info_message << " <<<\033[0m" << std::flush;
            return;
        } else {
            g_info_message.clear();
        }
    }

    // State Badge (7 chars)
    std::string state_str = " IDLE ";
    std::string color = "\033[1;37m";
    if (status.state == looper::LooperState::RECORDING) { state_str = " REC  "; color = "\033[1;31m"; }
    else if (status.state == looper::LooperState::PLAYING) { state_str = " PLAY "; color = "\033[1;32m"; }
    else if (status.state == looper::LooperState::OVERDUB) { state_str = " DUB  "; color = "\033[1;33m"; }
    else if (status.state == looper::LooperState::STOPPED) { state_str = " STOP "; color = "\033[1;34m"; }

    std::cout << color << "[" << state_str << "]\033[0m ";

    // Input Meter (6 chars)
    constexpr int METER_WIDTH = 4;
    int meter_fill = std::clamp(static_cast<int>(status.in_peak * 10 * METER_WIDTH), 0, METER_WIDTH);
    std::string meter_col = (status.in_peak > 0.8f) ? "\033[1;31m" : "\033[1;32m";
    std::cout << "IN:[" << meter_col;
    for (int i = 0; i < METER_WIDTH; ++i) {
        if (i < meter_fill) std::cout << "#";
        else std::cout << "-";
    }
    std::cout << "\033[0m] ";

    // Progress Bar (10 chars)
    constexpr int BAR_WIDTH = 8;
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

    // Time (11 chars)
    std::cout << std::fixed << std::setprecision(1)
              << std::setw(4) << status.current_sec << "/" 
              << std::setw(4) << status.total_sec << "s | ";

    // Latency compensation (12 chars)
    std::cout << "LAT:" << std::setw(3) << status.latency_samples << "smp | ";

    // Flags (13 chars)
    std::cout << "R:" << (status.is_reversed ? "\033[1;35mON \033[0m" : "OFF ")
              << "U:" << (status.undo_available ? "\033[1;32mYES \033[0m" : "NO  ")
              << "| ";

    // Volume & Dry (15 chars)
    std::cout << "V:" << std::setw(3) << static_cast<int>(loop_gain * 100) << "% "
              << "DRY:" << (dry_gain > 0.05f ? "\033[1;32mON\033[0m" : "OFF")
              << std::flush;
}

int main(int argc, char* argv[]) {
    std::string alsa_device = "hw:CARD=M1,DEV=0";
    if (argc > 1) {
        alsa_device = argv[1];
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    fs::create_directories("recordings");

    std::cout << "=====================================================" << std::endl;
    std::cout << "   ORANGE PI GUITAR LOOPER (Version C - Full MVP)    " << std::endl;
    std::cout << "=====================================================" << std::endl;
    std::cout << "Audio device: " << alsa_device << std::endl;
    std::cout << "\nControls:" << std::endl;
    std::cout << "  [SPACE]     : Rec -> Play -> Overdub -> Play" << std::endl;
    std::cout << "  [S]         : Stop playback" << std::endl;
    std::cout << "  [C]         : Clear loop & reset to IDLE" << std::endl;
    std::cout << "  [U]         : Undo / Redo last overdub layer" << std::endl;
    std::cout << "  [R]         : Toggle Reverse playback" << std::endl;
    std::cout << "  [F]         : Trigger smooth Fade-Out to stop" << std::endl;
    std::cout << "  [M]         : Toggle software Dry Monitor (on/off)" << std::endl;
    std::cout << "  [W]         : Save loop to WAV (in recordings/)" << std::endl;
    std::cout << "  [L]         : Load latest WAV from recordings/" << std::endl;
    std::cout << "  [>] / [<]   : Adjust Latency compensation (+/- 32 samples)" << std::endl;
    std::cout << "  [+] / [-]   : Adjust loop volume" << std::endl;
    std::cout << "  [Q]         : Quit" << std::endl;
    std::cout << "=====================================================\n" << std::endl;

    looper::LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128; // ~2.67 ms buffer
    config.dry_gain = 1.0f;   // Direct Monitor ON by default
    config.loop_gain = 1.0f;
    config.fade_out_sec = 3.0f;
    config.latency_compensation = 384; // ~8.0 ms (1 capture period + 2 playback cushion periods)
    config.pre_roll = 256;             // ~5.3 ms pre-roll to catch note attack

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
                setInfoMessage("Loop cleared", 2);
                break;
            case looper::ActionKey::UNDO:
                engine.triggerUndoRedo();
                setInfoMessage("Undo / Redo triggered", 2);
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
            case looper::ActionKey::SAVE_WAV: {
                auto now = std::time(nullptr);
                char buf[64];
                std::strftime(buf, sizeof(buf), "loop-%Y%m%d-%H%M%S.wav", std::localtime(&now));
                std::string path = "recordings/" + std::string(buf);
                if (engine.saveToWav(path)) {
                    setInfoMessage("Saved: " + path, 3);
                } else {
                    setInfoMessage("Save failed (empty loop?)", 2);
                }
                break;
            }
            case looper::ActionKey::LOAD_WAV: {
                // Find newest .wav file in recordings/
                std::string newest_file = "";
                fs::file_time_type newest_time;
                if (fs::exists("recordings")) {
                    for (const auto& entry : fs::directory_iterator("recordings")) {
                        if (entry.is_regular_file() && entry.path().extension() == ".wav") {
                            auto t = fs::last_write_time(entry);
                            if (newest_file.empty() || t > newest_time) {
                                newest_file = entry.path().string();
                                newest_time = t;
                            }
                        }
                    }
                }
                if (!newest_file.empty() && engine.loadFromWav(newest_file)) {
                    setInfoMessage("Loaded: " + newest_file, 3);
                } else {
                    setInfoMessage("No WAV file found to load", 2);
                }
                break;
            }
            case looper::ActionKey::LATENCY_UP:
                engine.adjustLatency(+32);
                setInfoMessage("Latency: " + std::to_string(engine.getLatency()) + " smp", 1);
                break;
            case looper::ActionKey::LATENCY_DOWN:
                engine.adjustLatency(-32);
                setInfoMessage("Latency: " + std::to_string(engine.getLatency()) + " smp", 1);
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
    std::cout << "[SYSTEM] Input manager ready. Waiting for triggers...\n" << std::endl;

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
