#include "command_queue.hpp"
#include "types.hpp"
#include "wav_file.hpp"
#include "wav_worker.hpp"
#include "looper_engine.hpp"

#include <iostream>
#include <cassert>
#include <cmath>
#include <thread>
#include <vector>
#include <fstream>

using namespace looper;

void test_spsc_queue() {
    std::cout << "[TEST] SpscQueue concurrency... " << std::flush;
    SpscQueue<int, 128> queue;
    constexpr int COUNT = 50000;

    std::thread producer([&]() {
        for (int i = 0; i < COUNT; ++i) {
            while (!queue.push(i)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&]() {
        for (int i = 0; i < COUNT; ++i) {
            int val = -1;
            while (!queue.pop(val)) {
                std::this_thread::yield();
            }
            assert(val == i);
        }
    });

    producer.join();
    consumer.join();
    std::cout << "PASSED!" << std::endl;
}

void test_wav_sample_rate_validation() {
    std::cout << "[TEST] WAV Sample Rate Verification... " << std::flush;

    // 1. Create a mock 44.1kHz WAV file
    std::string bad_wav = "test_44k.wav";
    {
        std::vector<float> dummy(44100, 0.2f);
        bool ok = WavFile::save(bad_wav, dummy, 44100);
        assert(ok);
    }

    // Try loading with expected 48000 Hz
    std::vector<float> loaded;
    std::string err;
    bool loaded_ok = WavFile::load(bad_wav, loaded, 48000, err);
    assert(!loaded_ok);
    assert(!err.empty());
    assert(err.find("Sample rate mismatch") != std::string::npos);

    // 2. Create a mock 48.0kHz WAV file
    std::string good_wav = "test_48k.wav";
    {
        std::vector<float> dummy(48000, 0.35f);
        bool ok = WavFile::save(good_wav, dummy, 48000);
        assert(ok);
    }

    // Try loading with expected 48000 Hz
    loaded_ok = WavFile::load(good_wav, loaded, 48000, err);
    assert(loaded_ok);
    assert(loaded.size() == 48000);
    assert(std::abs(loaded[100] - 0.35f) < 0.01f);

    std::remove(bad_wav.c_str());
    std::remove(good_wav.c_str());
    std::cout << "PASSED!" << std::endl;
}

void test_engine_state_and_undo_redo() {
    std::cout << "[TEST] Engine State Machine, Headroom & Undo/Redo... " << std::flush;

    CommandQueue cmd_queue;
    BufferReturnQueue return_queue;
    SaveQueue save_queue;

    LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128;
    config.monitor_mode = MonitorMode::DIRECT_ANALOG;
    config.dry_gain = 0.0f;
    config.loop_gain = 1.0f;
    config.pre_roll = 0; // Disable pre-roll for exact sample checks
    config.crossfade_samples = 0; // Disable crossfade for exact sample checks
    config.latency_compensation = 0; // Test basic overdub mixing without shift

    LooperEngine engine(cmd_queue, return_queue, save_queue, config);

    constexpr size_t BLOCK = 128;
    std::vector<float> in(BLOCK, 0.5f);
    std::vector<float> out_l(BLOCK, 0.0f);
    std::vector<float> out_r(BLOCK, 0.0f);

    auto push_cmd = [&](CommandType type) {
        Command cmd;
        cmd.type = type;
        cmd_queue.push(std::move(cmd));
    };

    // 1. Trigger ACTION -> Start Recording
    push_cmd(CommandType::ACTION);
    engine.process(in.data(), out_l.data(), out_r.data(), BLOCK);
    assert(engine.getStatus().state == LooperState::RECORDING);

    // Record another block of 0.5f
    engine.process(in.data(), out_l.data(), out_r.data(), BLOCK);
    // Total recorded = 256 samples of 0.5f

    // 2. Trigger ACTION -> End Recording, Enter PLAYING
    push_cmd(CommandType::ACTION);
    std::fill(in.begin(), in.end(), 0.0f); // Zero input during playback
    engine.process(in.data(), out_l.data(), out_r.data(), BLOCK);
    assert(engine.getStatus().state == LooperState::PLAYING);
    assert(engine.getStatus().total_frames == 256);

    // Playback output should match recorded 0.5f
    for (size_t i = 0; i < BLOCK; ++i) {
        assert(std::abs(out_l[i] - 0.5f) < 0.001f);
    }

    // 3. Trigger ACTION -> Enter OVERDUB
    push_cmd(CommandType::ACTION);
    // Overdub input = 0.3f
    std::fill(in.begin(), in.end(), 0.3f);
    engine.process(in.data(), out_l.data(), out_r.data(), BLOCK);
    assert(engine.getStatus().state == LooperState::OVERDUB);

    // Process next block of overdub
    engine.process(in.data(), out_l.data(), out_r.data(), BLOCK);

    // 4. Trigger ACTION -> End Overdub, return to PLAYING
    push_cmd(CommandType::ACTION);
    std::fill(in.begin(), in.end(), 0.0f);
    engine.process(in.data(), out_l.data(), out_r.data(), BLOCK);
    assert(engine.getStatus().state == LooperState::PLAYING);
    assert(engine.getStatus().undo_available == true);

    // During playback, base_track should now contain base + overdub = 0.5 + 0.3 = 0.8f
    for (size_t i = 0; i < BLOCK; ++i) {
        assert(std::abs(out_l[i] - 0.8f) < 0.001f);
    }

    // 5. Test UNDO
    push_cmd(CommandType::UNDO_REDO);
    engine.process(in.data(), out_l.data(), out_r.data(), BLOCK);
    assert(engine.getStatus().undo_available == false);
    assert(engine.getStatus().redo_available == true);

    // Loop should be restored back to exact 0.5f!
    for (size_t i = 0; i < BLOCK; ++i) {
        assert(std::abs(out_l[i] - 0.5f) < 0.001f);
    }

    // 6. Test REDO
    push_cmd(CommandType::UNDO_REDO);
    engine.process(in.data(), out_l.data(), out_r.data(), BLOCK);
    assert(engine.getStatus().undo_available == true);
    assert(engine.getStatus().redo_available == false);

    // Loop should be restored back to 0.8f!
    for (size_t i = 0; i < BLOCK; ++i) {
        assert(std::abs(out_l[i] - 0.8f) < 0.001f);
    }

    std::cout << "PASSED!" << std::endl;
}

void test_monitor_mode_and_latency() {
    std::cout << "[TEST] MonitorMode & Latency Compensation Logic... " << std::flush;

    CommandQueue cmd_queue;
    BufferReturnQueue return_queue;
    SaveQueue save_queue;

    LooperConfig config;
    config.monitor_mode = MonitorMode::DIRECT_ANALOG;
    config.latency_compensation = 384;

    LooperEngine engine(cmd_queue, return_queue, save_queue, config);

    auto status = engine.getStatus();
    assert(status.monitor_mode == MonitorMode::DIRECT_ANALOG);
    assert(status.effective_latency_samples == 384);

    // Switch to SOFTWARE monitoring mode
    Command cmd;
    cmd.type = CommandType::SET_MONITOR_MODE;
    cmd.int_param = static_cast<int>(MonitorMode::SOFTWARE);
    cmd_queue.push(std::move(cmd));

    float in[128] = {0};
    float out_l[128] = {0};
    float out_r[128] = {0};
    engine.process(in, out_l, out_r, 128);

    status = engine.getStatus();
    assert(status.monitor_mode == MonitorMode::SOFTWARE);
    assert(status.effective_latency_samples == 0); // K = 0 in SOFTWARE mode!

    std::cout << "PASSED!" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  RUNNING LOOPER ENGINE UNIT TESTS      " << std::endl;
    std::cout << "========================================" << std::endl;

    test_spsc_queue();
    test_wav_sample_rate_validation();
    test_engine_state_and_undo_redo();
    test_monitor_mode_and_latency();

    std::cout << "\nALL UNIT TESTS PASSED SUCCESSFULLY! (4/4)" << std::endl;
    return 0;
}
