#include "types.hpp"
#include "command_queue.hpp"
#include "looper_engine.hpp"
#include "audio_device.hpp"
#include "input_manager.hpp"
#include "wav_worker.hpp"
#include "latency_calibrator.hpp"

#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>
#include <cmath>
#include <ctime>
#include <mutex>
#include <filesystem>

namespace fs = std::filesystem;

std::atomic<bool> g_running{true};
std::mutex g_info_mutex;
std::string g_info_message = "";
std::chrono::steady_clock::time_point g_info_expire;

void setInfoMessage(const std::string& msg, int display_sec = 3) {
    std::lock_guard<std::mutex> lock(g_info_mutex);
    g_info_message = msg;
    g_info_expire = std::chrono::steady_clock::now() + std::chrono::seconds(display_sec);
}

void signalHandler(int) {
    g_running.store(false);
}

void printStatus(const looper::LooperStatus& status, float loop_gain, const looper::AudioTelemetrySnapshot& tele) {
    std::cout << "\r\033[2K"; // Clear line

    // If there's an active notification message, show it
    std::string info_to_show = "";
    {
        std::lock_guard<std::mutex> lock(g_info_mutex);
        if (!g_info_message.empty()) {
            if (std::chrono::steady_clock::now() < g_info_expire) {
                info_to_show = g_info_message;
            } else {
                g_info_message.clear();
            }
        }
    }

    if (!info_to_show.empty()) {
        std::cout << "\033[1;36m>>> " << info_to_show << " <<<\033[0m" << std::flush;
        return;
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

    // Monitor Mode & Effective Latency
    if (status.monitor_mode == looper::MonitorMode::DIRECT_ANALOG) {
        std::cout << "MON:\033[1;33mANALOG\033[0m(K=" << status.effective_latency_samples << ") | ";
    } else {
        std::cout << "MON:\033[1;32mSOFT\033[0m(K=0) | ";
    }

    // Flags
    std::cout << "R:" << (status.is_reversed ? "\033[1;35mON \033[0m" : "OFF ")
              << "U:" << (status.undo_available ? "\033[1;32mYES\033[0m" : (status.redo_available ? "\033[1;36mREDO\033[0m" : "NO "))
              << " | ";

    // Volume
    std::cout << "VOL:" << std::setw(3) << static_cast<int>(loop_gain * 100) << "%";

    // Telemetry (XRUNs and audio callback execution time)
    std::cout << " | XRUN:C:" << tele.capture_xruns << "/P:" << tele.playback_xruns
              << " | RT:" << tele.process_avg_us << "us(max:" << tele.process_max_us << "us)"
              << std::flush;
}

void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options] [alsa_device]\n\n"
              << "Options:\n"
              << "  -c, --capture <dev>        ALSA capture device (default: hw:CARD=M1,DEV=0)\n"
              << "  -p, --playback <dev>       ALSA playback device (default: matches capture)\n"
              << "  --rate <hz>                Sample rate (default: 48000)\n"
              << "  --period <frames>          Period buffer size (default: 128)\n"
              << "  --periods <n>              Number of periods (default: 4)\n"
              << "  -i, --in-channel <idx>     Capture channel index (default: 0)\n"
              << "  --capture-channels <n>     Hardware capture channels (default: 4)\n"
              << "  --playback-channels <n>    Hardware playback channels (default: 4)\n"
              << "  -m, --monitor <mode>       Monitor mode: software (default) or analog\n"
              << "  --latency <samples>        Latency compensation in samples (default: 384)\n"
              << "  --pre-roll <samples>       Pre-roll buffer samples (default: 256)\n"
              << "  --calibrate                Run automatic loopback RTL measurement (OUT 1 -> IN 1) and exit\n"
              << "  -h, --help                 Display this help message\n";
}

int main(int argc, char* argv[]) {
    std::string capture_dev = "hw:CARD=M1,DEV=0";
    std::string playback_dev = "";
    uint32_t sample_rate = looper::DEFAULT_SAMPLE_RATE;
    uint32_t period_size = looper::DEFAULT_PERIOD_SIZE;
    uint32_t periods = looper::DEFAULT_PERIODS;
    uint32_t cap_channels = looper::DEFAULT_CHANNELS;
    uint32_t play_channels = looper::DEFAULT_CHANNELS;
    uint32_t cap_index = 0;
    looper::MonitorMode monitor_mode = looper::MonitorMode::SOFTWARE;
    uint32_t latency_compensation = looper::DEFAULT_DIRECT_LATENCY;
    uint32_t pre_roll = looper::PRE_ROLL_SAMPLES;
    bool do_calibrate = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--calibrate") {
            do_calibrate = true;
        } else if ((arg == "-c" || arg == "--capture") && i + 1 < argc) {
            capture_dev = argv[++i];
        } else if ((arg == "-p" || arg == "--playback") && i + 1 < argc) {
            playback_dev = argv[++i];
        } else if (arg == "--rate" && i + 1 < argc) {
            sample_rate = std::stoul(argv[++i]);
        } else if (arg == "--period" && i + 1 < argc) {
            period_size = std::stoul(argv[++i]);
        } else if (arg == "--periods" && i + 1 < argc) {
            periods = std::stoul(argv[++i]);
        } else if ((arg == "-i" || arg == "--in-channel") && i + 1 < argc) {
            cap_index = std::stoul(argv[++i]);
        } else if (arg == "--capture-channels" && i + 1 < argc) {
            cap_channels = std::stoul(argv[++i]);
        } else if (arg == "--playback-channels" && i + 1 < argc) {
            play_channels = std::stoul(argv[++i]);
        } else if ((arg == "-m" || arg == "--monitor") && i + 1 < argc) {
            std::string m = argv[++i];
            if (m == "analog" || m == "direct" || m == "DIRECT_ANALOG") {
                monitor_mode = looper::MonitorMode::DIRECT_ANALOG;
            } else {
                monitor_mode = looper::MonitorMode::SOFTWARE;
            }
        } else if (arg == "--latency" && i + 1 < argc) {
            latency_compensation = std::stoul(argv[++i]);
        } else if (arg == "--pre-roll" && i + 1 < argc) {
            pre_roll = std::stoul(argv[++i]);
        } else if (!arg.empty() && arg[0] != '-') {
            // Positional argument sets both capture and playback (backwards compatibility)
            capture_dev = arg;
        }
    }

    if (playback_dev.empty()) {
        playback_dev = capture_dev;
    }

    // Automated loopback latency calibration mode
    if (do_calibrate) {
        looper::AudioConfig cal_cfg;
        cal_cfg.capture_device = capture_dev;
        cal_cfg.playback_device = playback_dev;
        cal_cfg.sample_rate = sample_rate;
        cal_cfg.period_size = period_size;
        cal_cfg.periods = periods;
        cal_cfg.capture_channels = cap_channels;
        cal_cfg.playback_channels = play_channels;
        cal_cfg.capture_channel_index = cap_index;

        std::cout << "=====================================================" << std::endl;
        std::cout << "      AUTOMATED LOOPBACK LATENCY CALIBRATION         " << std::endl;
        std::cout << "=====================================================" << std::endl;
        std::cout << "Connect: Audio Output 1 -> Audio Input 1" << std::endl;
        std::cout << "Capture:  " << cal_cfg.capture_device << " (ch " << cap_index << ")" << std::endl;
        std::cout << "Playback: " << cal_cfg.playback_device << std::endl;
        std::cout << "Rate:     " << sample_rate << " Hz, Period: " << period_size << " (" << periods << " periods)\n" << std::endl;

        std::cout << "[CALIBRATING] Transmitting test impulses..." << std::endl;
        auto res = looper::LatencyCalibrator::run(cal_cfg, 3);
        if (res.success) {
            std::cout << "\n[SUCCESS] " << res.message << std::endl;
            std::cout << "  Measured RTL: " << res.latency_samples << " samples ("
                      << std::fixed << std::setprecision(2) << res.latency_ms << " ms)" << std::endl;
            std::cout << "  Recommended launch flag: --latency " << res.latency_samples << "\n" << std::endl;
            return 0;
        } else {
            std::cerr << "\n[FAILED] " << res.message << "\n" << std::endl;
            return 1;
        }
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    fs::create_directories("recordings");

    std::cout << "=====================================================" << std::endl;
    std::cout << "   ORANGE PI GUITAR LOOPER (Version C - Full MVP)    " << std::endl;
    std::cout << "=====================================================" << std::endl;
    std::cout << "Capture:  " << capture_dev << " (channel " << cap_index << ")" << std::endl;
    std::cout << "Playback: " << playback_dev << std::endl;
    std::cout << "\nControls:" << std::endl;
    std::cout << "  [SPACE]     : Rec -> Play -> Overdub -> Play" << std::endl;
    std::cout << "  [S]         : Stop playback" << std::endl;
    std::cout << "  [C]         : Clear loop & reset to IDLE" << std::endl;
    std::cout << "  [U]         : Undo / Redo last overdub layer" << std::endl;
    std::cout << "  [R]         : Toggle Reverse playback" << std::endl;
    std::cout << "  [F]         : Trigger smooth Fade-Out to stop" << std::endl;
    std::cout << "  [M]         : Toggle Monitor Mode (Direct Analog vs Software Dry)" << std::endl;
    std::cout << "  [W]         : Save loop to WAV (in recordings/)" << std::endl;
    std::cout << "  [L]         : Load latest WAV from recordings/" << std::endl;
    std::cout << "  [>] / [<]   : Adjust Latency compensation (+/- 32 samples)" << std::endl;
    std::cout << "  [+] / [-]   : Adjust loop volume" << std::endl;
    std::cout << "  [Q]         : Quit" << std::endl;
    std::cout << "=====================================================\n" << std::endl;

    looper::LooperConfig config;
    config.sample_rate = sample_rate;
    config.period_size = period_size;
    config.monitor_mode = monitor_mode;
    config.dry_gain = (monitor_mode == looper::MonitorMode::SOFTWARE) ? 1.0f : 0.0f;
    config.loop_gain = 1.0f;
    config.fade_out_sec = 3.0f;
    config.latency_compensation = latency_compensation;
    config.pre_roll = pre_roll;

    // Pure SPSC queues for thread-safe decoupled communication
    looper::ControlQueue ctrl_queue;
    looper::LoadQueue load_queue;
    looper::BufferReturnQueue return_queue;
    looper::SaveSlotQueue save_slot_queue;
    looper::SaveReadyQueue save_ready_queue;

    // Background worker for non-blocking file I/O
    looper::WavWorker wav_worker(load_queue, return_queue, save_slot_queue, save_ready_queue, [](const std::string& msg, bool is_error) {
        setInfoMessage(msg, is_error ? 3 : 2);
    });
    wav_worker.start();

    // Audio Engine and ALSA device configuration
    looper::LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    looper::AudioConfig audio_cfg;
    audio_cfg.capture_device = capture_dev;
    audio_cfg.playback_device = playback_dev;
    audio_cfg.sample_rate = config.sample_rate;
    audio_cfg.period_size = config.period_size;
    audio_cfg.periods = periods;
    audio_cfg.capture_channels = cap_channels;
    audio_cfg.playback_channels = play_channels;
    audio_cfg.capture_channel_index = cap_index;

    looper::AudioDevice audio_device(audio_cfg, engine, config);

    std::cout << "[SYSTEM] Initializing audio device..." << std::endl;
    if (!audio_device.start()) {
        std::cerr << "[ERROR] Failed to start audio device (capture: " << capture_dev 
                  << ", playback: " << playback_dev << ")" << std::endl;
        wav_worker.stop();
        return 1;
    }
    std::cout << "[SYSTEM] Audio engine started successfully!" << std::endl;

    const auto& cap = audio_device.getCaptureParams();
    const auto& play = audio_device.getPlaybackParams();

    std::cout << "\nCapture:\n"
              << "  device    " << audio_cfg.capture_device << "\n"
              << "  rate      " << cap.sample_rate << "\n"
              << "  period    " << cap.period_size << "\n"
              << "  periods   " << cap.periods << "\n"
              << "  buffer    " << cap.buffer_size << "\n"
              << "  channels  " << cap.channels << "\n" << std::endl;

    std::cout << "Playback:\n"
              << "  device    " << audio_cfg.playback_device << "\n"
              << "  rate      " << play.sample_rate << "\n"
              << "  period    " << play.period_size << "\n"
              << "  periods   " << play.periods << "\n"
              << "  buffer    " << play.buffer_size << "\n"
              << "  channels  " << play.channels << "\n" << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto init_tele = audio_device.getTelemetrySnapshot();
    if (init_tele.rt_priority_acquired) {
        std::cout << "[AUDIO] Real-time thread priority set to SCHED_FIFO 80." << std::endl;
    } else {
        std::cout << "[AUDIO] Warning: Real-time SCHED_FIFO priority not acquired (running normal priority)." << std::endl;
    }

    std::atomic<float> current_loop_gain{config.loop_gain};

    looper::InputManager input_manager([&](looper::ActionKey key, uint64_t ts_ns) {
        switch (key) {
            case looper::ActionKey::ACTION: {
                looper::ControlCommand cmd;
                cmd.type = looper::ControlCommandType::ACTION;
                cmd.timestamp_ns = ts_ns;
                if (!ctrl_queue.push(cmd)) {
                    setInfoMessage("Error: command queue full", 2);
                }
                break;
            }
            case looper::ActionKey::STOP: {
                looper::ControlCommand cmd;
                cmd.type = looper::ControlCommandType::STOP;
                cmd.timestamp_ns = ts_ns;
                if (!ctrl_queue.push(cmd)) {
                    setInfoMessage("Error: command queue full", 2);
                }
                break;
            }
            case looper::ActionKey::CLEAR: {
                looper::ControlCommand cmd;
                cmd.type = looper::ControlCommandType::CLEAR;
                cmd.timestamp_ns = ts_ns;
                if (ctrl_queue.push(cmd)) {
                    setInfoMessage("Loop cleared", 2);
                }
                break;
            }
            case looper::ActionKey::UNDO: {
                looper::ControlCommand cmd;
                cmd.type = looper::ControlCommandType::UNDO_REDO;
                cmd.timestamp_ns = ts_ns;
                if (ctrl_queue.push(cmd)) {
                    setInfoMessage("Undo / Redo triggered", 2);
                }
                break;
            }
            case looper::ActionKey::REVERSE: {
                looper::ControlCommand cmd;
                cmd.type = looper::ControlCommandType::TOGGLE_REVERSE;
                cmd.timestamp_ns = ts_ns;
                ctrl_queue.push(cmd);
                break;
            }
            case looper::ActionKey::FADE: {
                looper::ControlCommand cmd;
                cmd.type = looper::ControlCommandType::TRIGGER_FADE;
                cmd.timestamp_ns = ts_ns;
                ctrl_queue.push(cmd);
                break;
            }
            case looper::ActionKey::TOGGLE_MONITOR: {
                auto s = engine.getStatus();
                auto next_mode = (s.monitor_mode == looper::MonitorMode::DIRECT_ANALOG)
                                 ? looper::MonitorMode::SOFTWARE
                                 : looper::MonitorMode::DIRECT_ANALOG;
                looper::ControlCommand cmd;
                cmd.type = looper::ControlCommandType::SET_MONITOR_MODE;
                cmd.int_param = static_cast<int>(next_mode);
                cmd.timestamp_ns = ts_ns;
                if (ctrl_queue.push(cmd)) {
                    if (next_mode == looper::MonitorMode::DIRECT_ANALOG) {
                        setInfoMessage("Monitor: DIRECT ANALOG (K=" + std::to_string(s.configured_latency_samples) + " smp, Dry: OFF)", 2);
                    } else {
                        setInfoMessage("Monitor: SOFTWARE DRY (K=0 smp, Dry: ON)", 2);
                    }
                }
                break;
            }
            case looper::ActionKey::SAVE_WAV: {
                auto s = engine.getStatus();
                if (s.state != looper::LooperState::STOPPED) {
                    setInfoMessage("Stop playback [S] before saving WAV", 3);
                } else if (s.total_frames == 0) {
                    setInfoMessage("Cannot save: loop buffer is empty", 2);
                } else {
                    auto now = std::time(nullptr);
                    char buf[64];
                    std::strftime(buf, sizeof(buf), "loop-%Y%m%d-%H%M%S.wav", std::localtime(&now));
                    std::string path = "recordings/" + std::string(buf);
                    wav_worker.requestSave(path, s.total_frames, config.sample_rate);
                    setInfoMessage("Saving snapshot: " + path, 2);
                }
                break;
            }
            case looper::ActionKey::LOAD_WAV: {
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
                if (!newest_file.empty()) {
                    setInfoMessage("Loading: " + newest_file + " ...", 2);
                    wav_worker.requestLoad(newest_file, config.sample_rate);
                } else {
                    setInfoMessage("No WAV file found in recordings/", 2);
                }
                break;
            }
            case looper::ActionKey::LATENCY_UP: {
                looper::ControlCommand cmd;
                cmd.type = looper::ControlCommandType::ADJUST_LATENCY;
                cmd.int_param = +32;
                cmd.timestamp_ns = ts_ns;
                if (ctrl_queue.push(cmd)) {
                    setInfoMessage("Latency +32 smp", 1);
                }
                break;
            }
            case looper::ActionKey::LATENCY_DOWN: {
                looper::ControlCommand cmd;
                cmd.type = looper::ControlCommandType::ADJUST_LATENCY;
                cmd.int_param = -32;
                cmd.timestamp_ns = ts_ns;
                if (ctrl_queue.push(cmd)) {
                    setInfoMessage("Latency -32 smp", 1);
                }
                break;
            }
            case looper::ActionKey::VOL_UP: {
                float cur = current_loop_gain.load(std::memory_order_relaxed);
                float updated = std::min(2.0f, cur + 0.05f);
                current_loop_gain.store(updated, std::memory_order_relaxed);
                looper::ControlCommand cmd;
                cmd.type = looper::ControlCommandType::SET_LOOP_GAIN;
                cmd.float_param = updated;
                cmd.timestamp_ns = ts_ns;
                ctrl_queue.push(cmd);
                break;
            }
            case looper::ActionKey::VOL_DOWN: {
                float cur = current_loop_gain.load(std::memory_order_relaxed);
                float updated = std::max(0.0f, cur - 0.05f);
                current_loop_gain.store(updated, std::memory_order_relaxed);
                looper::ControlCommand cmd;
                cmd.type = looper::ControlCommandType::SET_LOOP_GAIN;
                cmd.float_param = updated;
                cmd.timestamp_ns = ts_ns;
                ctrl_queue.push(cmd);
                break;
            }
            case looper::ActionKey::QUIT: {
                g_running.store(false);
                break;
            }
            default:
                break;
        }
    });

    input_manager.start();
    std::cout << "[SYSTEM] Input manager ready. Waiting for triggers...\n" << std::endl;

    while (g_running.load()) {
        printStatus(engine.getStatus(), current_loop_gain.load(std::memory_order_relaxed), audio_device.getTelemetrySnapshot());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << "\n\n[SYSTEM] Stopping audio engine..." << std::endl;
    input_manager.stop();
    audio_device.stop();
    wav_worker.stop();
    std::cout << "[SYSTEM] Clean shutdown complete. Goodbye!" << std::endl;

    return 0;
}
