#include "command_queue.hpp"
#include "types.hpp"
#include "wav_file.hpp"
#include "wav_worker.hpp"
#include "looper_engine.hpp"
#include "latency_calibrator.hpp"

#include <iostream>
#include <cassert>
#include <cmath>
#include <thread>
#include <vector>
#include <fstream>
#include <atomic>

using namespace looper;

void test_spsc_queue() {
    std::cout << "[TEST] SpscQueue concurrency (50k items)... " << std::flush;
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
        (void)ok;
    }

    // Try loading with expected 48000 Hz
    std::vector<float> loaded;
    std::string err;
    bool loaded_ok = WavFile::load(bad_wav, loaded, 48000, err);
    assert(!loaded_ok);
    assert(!err.empty());
    assert(err.find("Sample rate mismatch") != std::string::npos);
    (void)loaded_ok;

    // 2. Create a mock 48.0kHz WAV file
    std::string good_wav = "test_48k.wav";
    {
        std::vector<float> dummy(48000, 0.35f);
        bool ok = WavFile::save(good_wav, dummy, 48000);
        assert(ok);
        (void)ok;
    }

    // Try loading with expected 48000 Hz
    loaded_ok = WavFile::load(good_wav, loaded, 48000, err);
    assert(loaded_ok);
    (void)loaded_ok;
    assert(loaded.size() == 48000);
    assert(std::abs(loaded[100] - 0.35f) < 0.01f);

    std::remove(bad_wav.c_str());
    std::remove(good_wav.c_str());
    std::cout << "PASSED!" << std::endl;
}

void test_engine_state_and_undo_redo() {
    std::cout << "[TEST] Engine State Machine, Headroom & Undo/Redo... " << std::flush;

    ControlQueue ctrl_queue;
    LoadQueue load_queue;
    BufferReturnQueue return_queue;
    SaveSlotQueue save_slot_queue;
    SaveReadyQueue save_ready_queue;

    LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128;
    config.monitor_mode = MonitorMode::DIRECT_ANALOG;
    config.dry_gain = 0.0f;
    config.loop_gain = 1.0f;
    config.pre_roll = 0; // Disable pre-roll for exact sample checks
    config.crossfade_samples = 0; // Disable crossfade for exact sample checks
    config.latency_compensation = 0; // Test basic overdub mixing without shift

    LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    constexpr size_t BLOCK = 128;
    std::vector<float> in(BLOCK, 0.5f);
    std::vector<float> out_l(BLOCK, 0.0f);
    std::vector<float> out_r(BLOCK, 0.0f);

    auto push_cmd = [&](ControlCommandType type) {
        ControlCommand cmd;
        cmd.type = type;
        ctrl_queue.push(cmd);
    };

    // 1. Trigger ACTION -> Start Recording
    push_cmd(ControlCommandType::ACTION);
    engine.process(in.data(), out_l.data(), out_r.data(), BLOCK);
    assert(engine.getStatus().state == LooperState::RECORDING);

    // Record another block of 0.5f
    engine.process(in.data(), out_l.data(), out_r.data(), BLOCK);
    // Total recorded = 256 samples of 0.5f

    // 2. Trigger ACTION -> End Recording, Enter PLAYING
    push_cmd(ControlCommandType::ACTION);
    std::fill(in.begin(), in.end(), 0.0f); // Zero input during playback
    engine.process(in.data(), out_l.data(), out_r.data(), BLOCK);
    assert(engine.getStatus().state == LooperState::PLAYING);
    assert(engine.getStatus().total_frames == 256);

    // Playback output should match recorded 0.5f
    for (size_t i = 0; i < BLOCK; ++i) {
        assert(std::abs(out_l[i] - 0.5f) < 0.001f);
    }

    // 3. Trigger ACTION -> Enter OVERDUB
    push_cmd(ControlCommandType::ACTION);
    // Overdub input = 0.3f
    std::fill(in.begin(), in.end(), 0.3f);
    engine.process(in.data(), out_l.data(), out_r.data(), BLOCK);
    assert(engine.getStatus().state == LooperState::OVERDUB);

    // Process next block of overdub
    engine.process(in.data(), out_l.data(), out_r.data(), BLOCK);

    // 4. Trigger ACTION -> End Overdub, return to PLAYING
    push_cmd(ControlCommandType::ACTION);
    std::fill(in.begin(), in.end(), 0.0f);
    engine.process(in.data(), out_l.data(), out_r.data(), BLOCK);
    assert(engine.getStatus().state == LooperState::PLAYING);
    assert(engine.getStatus().undo_available == true);

    // During playback, base_track should now contain base + overdub = 0.5 + 0.3 = 0.8f
    for (size_t i = 0; i < BLOCK; ++i) {
        assert(std::abs(out_l[i] - 0.8f) < 0.001f);
    }

    // 5. Test UNDO
    push_cmd(ControlCommandType::UNDO_REDO);
    engine.process(in.data(), out_l.data(), out_r.data(), BLOCK);
    assert(engine.getStatus().undo_available == false);
    assert(engine.getStatus().redo_available == true);

    // Loop should be restored back to exact 0.5f!
    for (size_t i = 0; i < BLOCK; ++i) {
        assert(std::abs(out_l[i] - 0.5f) < 0.001f);
    }

    // 6. Test REDO
    push_cmd(ControlCommandType::UNDO_REDO);
    engine.process(in.data(), out_l.data(), out_r.data(), BLOCK);
    assert(engine.getStatus().undo_available == true);
    assert(engine.getStatus().redo_available == false);

    // Loop should be restored back to 0.8f!
    for (size_t i = 0; i < BLOCK; ++i) {
        assert(std::abs(out_l[i] - 0.8f) < 0.001f);
    }

    std::cout << "PASSED!" << std::endl;
}

void test_concurrent_get_status() {
    std::cout << "[TEST] Concurrent getStatus() thread-safety... " << std::flush;

    ControlQueue ctrl_queue;
    LoadQueue load_queue;
    BufferReturnQueue return_queue;
    SaveSlotQueue save_slot_queue;
    SaveReadyQueue save_ready_queue;

    LooperConfig config;
    LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    std::atomic<bool> done{false};
    constexpr int BLOCKS = 2000;

    // Audio thread: process blocks and update playhead
    std::thread audio_thread([&]() {
        float in[128] = {0.1f};
        float out_l[128] = {0};
        float out_r[128] = {0};

        ControlCommand cmd;
        cmd.type = ControlCommandType::ACTION;
        ctrl_queue.push(cmd);

        for (int i = 0; i < BLOCKS; ++i) {
            engine.process(in, out_l, out_r, 128);
        }
        done.store(true);
    });

    // UI thread: hammer getStatus() concurrently
    std::thread ui_thread([&]() {
        while (!done.load(std::memory_order_relaxed)) {
            LooperStatus s = engine.getStatus();
            (void)s.playhead_frames;
            (void)s.total_frames;
            (void)s.state;
        }
    });

    audio_thread.join();
    ui_thread.join();
    std::cout << "PASSED!" << std::endl;
}

void test_monitor_mode_and_latency() {
    std::cout << "[TEST] MonitorMode & Latency Compensation Logic... " << std::flush;

    ControlQueue ctrl_queue;
    LoadQueue load_queue;
    BufferReturnQueue return_queue;
    SaveSlotQueue save_slot_queue;
    SaveReadyQueue save_ready_queue;

    LooperConfig config;
    config.monitor_mode = MonitorMode::DIRECT_ANALOG;
    config.latency_compensation = 384;

    LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    auto status = engine.getStatus();
    assert(status.monitor_mode == MonitorMode::DIRECT_ANALOG);
    assert(status.effective_latency_samples == 384);

    // Switch to SOFTWARE monitoring mode
    ControlCommand cmd;
    cmd.type = ControlCommandType::SET_MONITOR_MODE;
    cmd.int_param = static_cast<int>(MonitorMode::SOFTWARE);
    ctrl_queue.push(cmd);

    float in[128] = {0};
    float out_l[128] = {0};
    float out_r[128] = {0};
    engine.process(in, out_l, out_r, 128);

    status = engine.getStatus();
    assert(status.monitor_mode == MonitorMode::SOFTWARE);
    assert(status.effective_latency_samples == 0); // K = 0 in SOFTWARE mode!

    std::cout << "PASSED!" << std::endl;
}

void test_preroll_length_compensation() {
    std::cout << "[TEST] Pre-Roll Musical Length Compensation... " << std::flush;

    ControlQueue ctrl_queue;
    LoadQueue load_queue;
    BufferReturnQueue return_queue;
    SaveSlotQueue save_slot_queue;
    SaveReadyQueue save_ready_queue;

    LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128;
    config.pre_roll = 256; // 256 samples pre-roll
    config.crossfade_samples = 0;

    LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    std::vector<float> in(128, 0.2f);
    std::vector<float> out_l(128, 0.0f);
    std::vector<float> out_r(128, 0.0f);

    // 1. Start Recording
    ControlCommand cmd;
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128); // Block 1

    // Record 15 more blocks = 16 blocks total = 16 * 128 = 2048 frames played
    for (int b = 1; b < 16; ++b) {
        engine.process(in.data(), out_l.data(), out_r.data(), 128);
    }

    // 2. Stop Recording / Enter PLAYING
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);

    auto status = engine.getStatus();
    assert(status.state == LooperState::PLAYING);
    // Despite 256 frames of pre-roll at start, loop length should match the exact 2048 musical frames recorded!
    assert(status.total_frames == 2048);
    (void)status;

    std::cout << "PASSED!" << std::endl;
}

void test_zero_on_transition_timing() {
    std::cout << "[TEST] Zero O(N) Transition Execution Timing... " << std::flush;

    ControlQueue ctrl_queue;
    LoadQueue load_queue;
    BufferReturnQueue return_queue;
    SaveSlotQueue save_slot_queue;
    SaveReadyQueue save_ready_queue;

    LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128;
    config.pre_roll = 0;
    config.crossfade_samples = 0;
    config.latency_compensation = 0;

    LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    std::vector<float> in(128, 0.1f);
    std::vector<float> out_l(128, 0.0f);
    std::vector<float> out_r(128, 0.0f);

    ControlCommand cmd;
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);

    // Record a 500,000 samples loop (~10.4 seconds)
    constexpr size_t TOTAL_BLOCKS = 500000 / 128;
    for (size_t b = 0; b < TOTAL_BLOCKS; ++b) {
        engine.process(in.data(), out_l.data(), out_r.data(), 128);
    }

    // Enter PLAYING
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::PLAYING);

    // Enter OVERDUB
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::OVERDUB);

    // Measure time to transition OVERDUB -> PLAYING (must be O(1), < 2000 microseconds)
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);

    auto t0 = std::chrono::high_resolution_clock::now();
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    auto t1 = std::chrono::high_resolution_clock::now();

    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    assert(engine.getStatus().state == LooperState::PLAYING);
    assert(elapsed_us < 2000);
    (void)elapsed_us;

    // Measure UNDO time (must be O(1))
    cmd.type = ControlCommandType::UNDO_REDO;
    ctrl_queue.push(cmd);

    t0 = std::chrono::high_resolution_clock::now();
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    t1 = std::chrono::high_resolution_clock::now();

    elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    assert(engine.getStatus().undo_available == false);
    assert(elapsed_us < 2000);

    std::cout << "PASSED!" << std::endl;
}

void test_partial_overdub_with_latency() {
    std::cout << "[TEST] Partial Overdub with Latency Compensation (Transport Domain)... " << std::flush;

    ControlQueue ctrl_queue;
    LoadQueue load_queue;
    BufferReturnQueue return_queue;
    SaveSlotQueue save_slot_queue;
    SaveReadyQueue save_ready_queue;

    LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128;
    config.pre_roll = 0;
    config.crossfade_samples = 0;
    config.monitor_mode = MonitorMode::DIRECT_ANALOG;
    config.dry_gain = 0.0f;
    config.latency_compensation = 32; // K = 32 samples

    LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    std::vector<float> in(128, 0.5f);
    std::vector<float> out_l(128, 0.0f);
    std::vector<float> out_r(128, 0.0f);

    ControlCommand cmd;
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    // Record 2 blocks = 256 samples of 0.5f
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);

    // End REC -> PLAYING
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    std::fill(in.begin(), in.end(), 0.0f);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::PLAYING);
    assert(engine.getStatus().total_frames == 256);

    // Enter OVERDUB
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    // Overdub 1 block (128 samples) with 0.3f (partial: 128 of 256 frames)
    std::fill(in.begin(), in.end(), 0.3f);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::OVERDUB);

    // End OVERDUB -> PLAYING
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    std::fill(in.begin(), in.end(), 0.0f);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::PLAYING);

    // Process another block to ensure background transport merge runs
    engine.process(in.data(), out_l.data(), out_r.data(), 128);

    // Playback should have 0.8f in overdubbed region and 0.5f in untouched region
    // Over 256 samples, test output matches either 0.8f or 0.5f exactly
    for (int b = 0; b < 2; ++b) {
        engine.process(in.data(), out_l.data(), out_r.data(), 128);
        for (int i = 0; i < 128; ++i) {
            float val = out_l[i];
            bool is_08 = std::abs(val - 0.8f) < 0.01f;
            bool is_05 = std::abs(val - 0.5f) < 0.01f;
            assert(is_08 || is_05);
            (void)is_08;
            (void)is_05;
        }
    }

    std::cout << "PASSED!" << std::endl;
}

void test_undo_then_partial_new_overdub() {
    std::cout << "[TEST] Undo -> Partial New Overdub (No Ghost Resurrection)... " << std::flush;

    ControlQueue ctrl_queue;
    LoadQueue load_queue;
    BufferReturnQueue return_queue;
    SaveSlotQueue save_slot_queue;
    SaveReadyQueue save_ready_queue;

    LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128;
    config.pre_roll = 0;
    config.crossfade_samples = 0;
    config.latency_compensation = 0;
    config.dry_gain = 0.0f;

    LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    std::vector<float> in(128, 0.5f);
    std::vector<float> out_l(128, 0.0f);
    std::vector<float> out_r(128, 0.0f);

    ControlCommand cmd;
    // 1. Record 2 blocks (256 samples) of Base = 0.5f
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);

    // End REC -> PLAYING
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    std::fill(in.begin(), in.end(), 0.0f);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().total_frames == 256);

    // 2. Overdub Layer A = 0.4f for full loop (2 blocks)
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    std::fill(in.begin(), in.end(), 0.4f);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);

    // End Overdub A -> PLAYING
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    std::fill(in.begin(), in.end(), 0.0f);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().undo_available == true);

    // 3. Trigger UNDO on Layer A
    cmd.type = ControlCommandType::UNDO_REDO;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().undo_available == false);
    // Sound is back to 0.5f
    for (int i = 0; i < 128; ++i) {
        assert(std::abs(out_l[i] - 0.5f) < 0.001f);
    }

    // 4. Start Overdub Layer B (Partial: 1 block of 128 samples with 0.1f)
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    std::fill(in.begin(), in.end(), 0.1f);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);

    // End Overdub B -> PLAYING
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    std::fill(in.begin(), in.end(), 0.0f);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);

    // Advance 2 more blocks to complete background chunked merge
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);

    // Verify:
    // Block 1 (Layer B recorded): 0.5f + 0.1f = 0.6f
    // Block 2 (Layer B untouched): MUST BE 0.5f! Layer A (0.4f) must NOT be resurrected!
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    for (int i = 0; i < 128; ++i) {
        assert(std::abs(out_l[i] - 0.6f) < 0.01f);
    }
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    for (int i = 0; i < 128; ++i) {
        assert(std::abs(out_l[i] - 0.5f) < 0.01f); // Untouched tail stays 0.5f, NOT 0.9f!
    }

    std::cout << "PASSED!" << std::endl;
}

void test_save_interrupted_by_clear_and_load() {
    std::cout << "[TEST] Active WAV Save Protection (Interrupted by Clear & Load)... " << std::flush;

    ControlQueue ctrl_queue;
    LoadQueue load_queue;
    BufferReturnQueue return_queue;
    SaveSlotQueue save_slot_queue;
    SaveReadyQueue save_ready_queue;

    LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128;
    config.pre_roll = 0;
    config.crossfade_samples = 0;

    LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    std::vector<float> in(128, 0.4f);
    std::vector<float> out_l(128, 0.0f);
    std::vector<float> out_r(128, 0.0f);

    // 1. Record loop of 38400 samples (> SAVE_CHUNK_SIZE 16384) and STOP
    ControlCommand cmd;
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    for (int i = 0; i < 300; ++i) {
        engine.process(in.data(), out_l.data(), out_r.data(), 128); // REC 300 * 128 = 38400 samples
    }
    cmd.type = ControlCommandType::STOP;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128); // STOPPED
    assert(engine.getStatus().state == LooperState::STOPPED);
    assert(engine.getStatus().total_frames == 38400);

    // 2. Initiate SaveSlotCommand for 38400 samples
    auto* save_buf = new std::vector<float>(38400);
    SaveSlotCommand slot;
    slot.buffer = save_buf;
    save_slot_queue.push(slot);

    // Engine begins chunked copy on next block (copies 16384 samples, 22016 remaining)
    engine.process(in.data(), out_l.data(), out_r.data(), 128);

    // 3. User interrupts with CLEAR
    cmd.type = ControlCommandType::CLEAR;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);

    // Verify engine aborted save, pushed buffer to save_ready_queue, and state is IDLE
    assert(engine.getStatus().state == LooperState::IDLE);
    std::vector<float>* ready_buf = nullptr;
    bool got_buf = save_ready_queue.pop(ready_buf);
    assert(got_buf);
    (void)got_buf;
    assert(ready_buf == save_buf);
    assert(ready_buf->empty()); // Empty marks aborted save
    delete save_buf;

    // 4. Test LoadCommand 3-buffer pointer swap
    auto* new_base = new std::vector<float>(256, 0.7f);
    auto* new_layer = new std::vector<float>(256, 0.0f);
    auto* new_record = new std::vector<float>(256, 0.0f);
    LoadCommand lcmd;
    lcmd.base_buffer = new_base;
    lcmd.layer_buffer = new_layer;
    lcmd.record_buffer = new_record;
    load_queue.push(lcmd);

    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::STOPPED);
    assert(engine.getStatus().total_frames == 256);

    // Check that all 3 old buffers were pushed to return queue for worker
    std::vector<float>* returned_buf = nullptr;
    assert(return_queue.pop(returned_buf));
    delete returned_buf;
    assert(return_queue.pop(returned_buf));
    delete returned_buf;
    assert(return_queue.pop(returned_buf));
    delete returned_buf;

    std::cout << "PASSED!" << std::endl;
}

void test_wav_save_includes_active_overdub() {
    std::cout << "[TEST] WAV Save Snapshot Includes Active Overdub (0.5 + 0.3 = 0.8)... " << std::flush;

    ControlQueue ctrl_queue;
    LoadQueue load_queue;
    BufferReturnQueue return_queue;
    SaveSlotQueue save_slot_queue;
    SaveReadyQueue save_ready_queue;

    LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128;
    config.pre_roll = 0;
    config.crossfade_samples = 0;
    config.latency_compensation = 0;

    LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    std::vector<float> in(128, 0.5f);
    std::vector<float> out_l(128, 0.0f);
    std::vector<float> out_r(128, 0.0f);

    // 1. Record 2 blocks (256 samples) of 0.5f base loop
    ControlCommand cmd;
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);

    // 2. Close recording -> PLAYING
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    std::fill(in.begin(), in.end(), 0.0f);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::PLAYING);
    assert(engine.getStatus().total_frames == 256);

    // 3. Enter OVERDUB, record 2 blocks (256 samples) of 0.3f
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    std::fill(in.begin(), in.end(), 0.3f);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);

    // 4. End OVERDUB -> PLAYING
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    std::fill(in.begin(), in.end(), 0.0f);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::PLAYING);

    // Verify audible playback is 0.5f + 0.3f = 0.8f
    for (int i = 0; i < 128; ++i) {
        assert(std::abs(out_l[i] - 0.8f) < 0.01f);
    }

    // 5. STOP
    cmd.type = ControlCommandType::STOP;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::STOPPED);

    // 6. Request save snapshot
    auto* save_buf = new std::vector<float>(256);
    SaveSlotCommand slot;
    slot.buffer = save_buf;
    save_slot_queue.push(slot);

    // Process blocks until save is ready
    std::vector<float>* ready_buf = nullptr;
    while (!save_ready_queue.pop(ready_buf)) {
        engine.process(in.data(), out_l.data(), out_r.data(), 128);
    }
    assert(ready_buf == save_buf);
    assert(ready_buf->size() == 256);
    // Verify saved WAV contains 0.8f (0.5 base + 0.3 overdub)
    for (size_t i = 0; i < ready_buf->size(); ++i) {
        assert(std::abs((*ready_buf)[i] - 0.8f) < 0.01f);
    }
    delete ready_buf;

    // 7. Test UNDO then save: should contain only 0.5f
    cmd.type = ControlCommandType::UNDO_REDO;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().undo_available == false);
    assert(engine.getStatus().redo_available == true);

    save_buf = new std::vector<float>(256);
    slot.buffer = save_buf;
    save_slot_queue.push(slot);

    ready_buf = nullptr;
    while (!save_ready_queue.pop(ready_buf)) {
        engine.process(in.data(), out_l.data(), out_r.data(), 128);
    }
    assert(ready_buf == save_buf);
    for (size_t i = 0; i < ready_buf->size(); ++i) {
        assert(std::abs((*ready_buf)[i] - 0.5f) < 0.01f);
    }
    delete ready_buf;

    std::cout << "PASSED!" << std::endl;
}

void test_large_loop_partial_overdub_and_undo() {
    std::cout << "[TEST] Large Loop (16384 samples) Partial Overdub, Undo/Redo & Ghost Prevention... " << std::flush;

    ControlQueue ctrl_queue;
    LoadQueue load_queue;
    BufferReturnQueue return_queue;
    SaveSlotQueue save_slot_queue;
    SaveReadyQueue save_ready_queue;

    LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128;
    config.pre_roll = 0;
    config.crossfade_samples = 0;
    config.latency_compensation = 0;

    LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    std::vector<float> in(128, 0.4f);
    std::vector<float> out_l(128, 0.0f);
    std::vector<float> out_r(128, 0.0f);

    // 1. Record 128 blocks * 128 = 16384 samples base loop with 0.4f
    ControlCommand cmd;
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    for (int i = 0; i < 128; ++i) {
        engine.process(in.data(), out_l.data(), out_r.data(), 128);
    }
    // End recording -> PLAYING
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    std::fill(in.begin(), in.end(), 0.0f);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::PLAYING);
    assert(engine.getStatus().total_frames == 16384);

    // Wrap around playhead to 0 by running remaining 127 blocks
    for (int i = 0; i < 127; ++i) {
        engine.process(in.data(), out_l.data(), out_r.data(), 128);
    }
    assert(engine.getStatus().playhead_frames == 0);

    // 2. Overdub 1 block (128 samples) with 0.2f at beginning of loop
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    std::fill(in.begin(), in.end(), 0.2f);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);

    // End overdub -> PLAYING
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    std::fill(in.begin(), in.end(), 0.0f);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::PLAYING);

    // Advance through the entire loop to verify:
    // Run remaining 126 blocks: all should be 0.4f
    for (int i = 0; i < 126; ++i) {
        engine.process(in.data(), out_l.data(), out_r.data(), 128);
        assert(std::abs(out_l[0] - 0.4f) < 0.01f);
    }

    // Now playhead reaches block 0 again: should be 0.4f + 0.2f = 0.6f
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(std::abs(out_l[0] - 0.6f) < 0.01f);

    // 3. Test UNDO
    cmd.type = ControlCommandType::UNDO_REDO;
    ctrl_queue.push(cmd);
    // Next block is block 1 (0.4f)
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().undo_available == false);

    // Advance until block 0 again
    for (int i = 0; i < 126; ++i) {
        engine.process(in.data(), out_l.data(), out_r.data(), 128);
    }
    // Block 0 with UNDO should be 0.4f
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(std::abs(out_l[0] - 0.4f) < 0.01f);

    // 4. Test REDO
    cmd.type = ControlCommandType::UNDO_REDO;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().undo_available == true);

    // Advance until block 0 again
    for (int i = 0; i < 126; ++i) {
        engine.process(in.data(), out_l.data(), out_r.data(), 128);
    }
    // Block 0 with REDO should be 0.6f
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(std::abs(out_l[0] - 0.6f) < 0.01f);

    std::cout << "PASSED!" << std::endl;
}

void test_overdub_mode_locks() {
    std::cout << "[TEST] Mode / Transport Switches Disallowed in OVERDUB... " << std::flush;

    ControlQueue ctrl_queue;
    LoadQueue load_queue;
    BufferReturnQueue return_queue;
    SaveSlotQueue save_slot_queue;
    SaveReadyQueue save_ready_queue;

    LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128;
    config.pre_roll = 0;
    config.crossfade_samples = 0;
    config.latency_compensation = 384;
    config.monitor_mode = MonitorMode::DIRECT_ANALOG;

    LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    std::vector<float> in(128, 0.5f);
    std::vector<float> out_l(128, 0.0f);
    std::vector<float> out_r(128, 0.0f);

    // Record base loop & enter PLAYING
    ControlCommand cmd;
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::PLAYING);

    // Enter OVERDUB
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::OVERDUB);

    // Try TOGGLE_REVERSE in OVERDUB -> must be ignored!
    cmd.type = ControlCommandType::TOGGLE_REVERSE;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().is_reversed == false);

    // Try SET_MONITOR_MODE in OVERDUB -> must be ignored!
    cmd.type = ControlCommandType::SET_MONITOR_MODE;
    cmd.int_param = static_cast<int>(MonitorMode::SOFTWARE);
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().monitor_mode == MonitorMode::DIRECT_ANALOG);

    // Try ADJUST_LATENCY in OVERDUB -> must be ignored!
    cmd.type = ControlCommandType::ADJUST_LATENCY;
    cmd.int_param = 50;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().configured_latency_samples == 384);

    // End overdub -> PLAYING
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::PLAYING);

    // Now in PLAYING, TOGGLE_REVERSE should work!
    cmd.type = ControlCommandType::TOGGLE_REVERSE;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().is_reversed == true);

    std::cout << "PASSED!" << std::endl;
}

void test_second_overdub_immediate_exit_timing() {
    std::cout << "[TEST] Large Loop (500k samples) 2nd Overdub Immediate Exit Timing (O(1))... " << std::flush;

    ControlQueue ctrl_queue;
    LoadQueue load_queue;
    BufferReturnQueue return_queue;
    SaveSlotQueue save_slot_queue;
    SaveReadyQueue save_ready_queue;

    LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128;
    config.pre_roll = 0;
    config.crossfade_samples = 0;
    config.latency_compensation = 0;

    LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    std::vector<float> in(128, 0.1f);
    std::vector<float> out_l(128, 0.0f);
    std::vector<float> out_r(128, 0.0f);

    ControlCommand cmd;
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);

    // Record a 500,000 samples base loop (~10.4 seconds)
    constexpr size_t TOTAL_BLOCKS = 500000 / 128;
    for (size_t b = 0; b < TOTAL_BLOCKS; ++b) {
        engine.process(in.data(), out_l.data(), out_r.data(), 128);
    }

    // Enter PLAYING
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::PLAYING);

    // 1. First Overdub: record 1 block and exit
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::OVERDUB);

    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::PLAYING);
    assert(engine.getStatus().undo_available == true);

    // 2. Second Overdub: starts while previous layer exists!
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::OVERDUB);

    // Now IMMEDIATELY exit second overdub (after only 1 block, leaving 499,872 samples unmerged!)
    // If the engine did while (pending_merge_active_), this would block and fail the timing check!
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);

    auto t0 = std::chrono::high_resolution_clock::now();
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    auto t1 = std::chrono::high_resolution_clock::now();

    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    std::cout << "(elapsed: " << elapsed_us << " us) " << std::flush;
    assert(engine.getStatus().state == LooperState::PLAYING);
    assert(elapsed_us < 10000); // Strictly O(1): bounded in microseconds under ASan, NOT 500,000 samples!

    std::cout << "PASSED! (Elapsed: " << elapsed_us << " us)" << std::endl;
}

void test_undo_disallowed_during_overdub() {
    std::cout << "[TEST] UNDO / REDO Ignored During Active OVERDUB... " << std::flush;

    ControlQueue ctrl_queue;
    LoadQueue load_queue;
    BufferReturnQueue return_queue;
    SaveSlotQueue save_slot_queue;
    SaveReadyQueue save_ready_queue;

    LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128;
    config.pre_roll = 0;
    config.crossfade_samples = 0;

    LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    std::vector<float> in(128, 0.5f);
    std::vector<float> out_l(128, 0.0f);
    std::vector<float> out_r(128, 0.0f);

    // Record base loop
    ControlCommand cmd;
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::PLAYING);

    // Complete Layer 1
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().undo_available == true);

    // Start Layer 2 overdub
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::OVERDUB);

    // Attempt UNDO during OVERDUB -> must be ignored!
    cmd.type = ControlCommandType::UNDO_REDO;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::OVERDUB);

    // Exit overdub -> PLAYING
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::PLAYING);
    assert(engine.getStatus().undo_available == true);

    // In PLAYING, UNDO now works!
    cmd.type = ControlCommandType::UNDO_REDO;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().undo_available == false);
    assert(engine.getStatus().redo_available == true);

    std::cout << "PASSED!" << std::endl;
}

void test_fade_during_overdub_preserves_take() {
    std::cout << "[TEST] Fade Triggered in OVERDUB Preserves Overdub Take... " << std::flush;

    ControlQueue ctrl_queue;
    LoadQueue load_queue;
    BufferReturnQueue return_queue;
    SaveSlotQueue save_slot_queue;
    SaveReadyQueue save_ready_queue;

    LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128;
    config.pre_roll = 0;
    config.crossfade_samples = 0;
    config.fade_out_sec = 0.05f; // Fast fade for testing (~2400 samples = ~19 blocks)

    LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    std::vector<float> in(128, 0.5f);
    std::vector<float> out_l(128, 0.0f);
    std::vector<float> out_r(128, 0.0f);

    // Record base loop of 2 blocks = 256 samples
    ControlCommand cmd;
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);

    // Enter PLAYING
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    std::fill(in.begin(), in.end(), 0.0f);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::PLAYING);

    // Enter OVERDUB, record 2 blocks of 0.3f (full 256 samples loop)
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    std::fill(in.begin(), in.end(), 0.3f);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::OVERDUB);

    // Trigger FADE directly while in OVERDUB!
    cmd.type = ControlCommandType::TRIGGER_FADE;
    ctrl_queue.push(cmd);
    std::fill(in.begin(), in.end(), 0.0f);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().is_fading_out == true);

    // Process blocks until fade completes and state becomes STOPPED
    while (engine.getStatus().state != LooperState::STOPPED) {
        engine.process(in.data(), out_l.data(), out_r.data(), 128);
    }
    assert(engine.getStatus().state == LooperState::STOPPED);
    assert(engine.getStatus().total_frames == 256);
    // Overdub take must be committed as undo_available layer!
    assert(engine.getStatus().undo_available == true);

    // Resume playback with ACTION: audio should contain the recorded overdub (0.5f + 0.3f = 0.8f)
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::PLAYING);

    for (int i = 0; i < 128; ++i) {
        assert(std::abs(out_l[i] - 0.8f) < 0.01f);
    }

    std::cout << "PASSED!" << std::endl;
}

void test_action_aborts_active_save() {
    std::cout << "[TEST] ACTION Aborts Active WAV Save to Prevent Source Buffer Mutation... " << std::flush;

    ControlQueue ctrl_queue;
    LoadQueue load_queue;
    BufferReturnQueue return_queue;
    SaveSlotQueue save_slot_queue;
    SaveReadyQueue save_ready_queue;

    LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128;
    config.pre_roll = 0;
    config.crossfade_samples = 0;

    LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    std::vector<float> in(128, 0.4f);
    std::vector<float> out_l(128, 0.0f);
    std::vector<float> out_r(128, 0.0f);

    // Record loop and stop
    ControlCommand cmd;
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    for (int i = 0; i < 300; ++i) {
        engine.process(in.data(), out_l.data(), out_r.data(), 128); // 38400 samples
    }
    cmd.type = ControlCommandType::STOP;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::STOPPED);

    // Request save
    auto* save_buf = new std::vector<float>(38400);
    SaveSlotCommand slot;
    slot.buffer = save_buf;
    save_slot_queue.push(slot);
    engine.process(in.data(), out_l.data(), out_r.data(), 128); // begins copying

    // Press ACTION -> should safely abort save and enter PLAYING
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::PLAYING);

    // Verify aborted buffer was returned
    std::vector<float>* ready_buf = nullptr;
    assert(save_ready_queue.pop(ready_buf));
    assert(ready_buf == save_buf);
    assert(ready_buf->empty()); // marked empty = aborted
    delete ready_buf;

    std::cout << "PASSED!" << std::endl;
}

void test_prefaulted_buffers_and_telemetry() {
    std::cout << "[TEST] Prefaulted Fixed Buffers & AudioTelemetry Lock-Free Invariants... " << std::flush;

    // Verify AudioTelemetry lock-free properties
    AudioTelemetry tele;
    assert(tele.capture_xruns.is_lock_free());
    assert(tele.playback_xruns.is_lock_free());
    assert(tele.suspends.is_lock_free());
    assert(tele.disconnects.is_lock_free());
    assert(tele.short_writes.is_lock_free());
    assert(tele.recoveries.is_lock_free());
    assert(tele.fatal_audio_errors.is_lock_free());
    assert(tele.process_max_us.is_lock_free());
    assert(tele.process_avg_us.is_lock_free());
    assert(tele.rt_priority_acquired.is_lock_free());

    // Verify AudioConfig defaults
    AudioConfig cfg;
    assert(cfg.sample_rate == 48000);
    assert(cfg.period_size == 128);
    assert(cfg.periods == 4);
    assert(cfg.capture_channels == 4);
    assert(cfg.playback_channels == 4);

    // Verify engine fixed buffers
    ControlQueue ctrl_queue;
    LoadQueue load_queue;
    BufferReturnQueue return_queue;
    SaveSlotQueue save_slot_queue;
    SaveReadyQueue save_ready_queue;

    LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128;
    config.pre_roll = 0;
    config.crossfade_samples = 0;

    LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    std::vector<float> in(128, 0.7f);
    std::vector<float> out_l(128, 0.0f);
    std::vector<float> out_r(128, 0.0f);

    // Record 1 block without push_back reallocation
    ControlCommand cmd;
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::RECORDING);

    // Complete recording
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::PLAYING);
    assert(engine.getStatus().total_frames == 128);

    // Clear loop -> logical size reset, memory remains pre-allocated
    cmd.type = ControlCommandType::CLEAR;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::IDLE);
    assert(engine.getStatus().total_frames == 0);

    std::cout << "PASSED!" << std::endl;
}

void test_clear_o1_timing_on_large_loop() {
    std::cout << "[TEST] CLEAR O(1) Execution Timing on Large Loop (500k samples)... " << std::flush;

    ControlQueue ctrl_queue;
    LoadQueue load_queue;
    BufferReturnQueue return_queue;
    SaveSlotQueue save_slot_queue;
    SaveReadyQueue save_ready_queue;

    LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128;
    config.pre_roll = 0;
    config.crossfade_samples = 0;

    LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    std::vector<float> in(128, 0.5f);
    std::vector<float> out_l(128, 0.0f);
    std::vector<float> out_r(128, 0.0f);

    // Record ~500k samples (~3906 blocks)
    ControlCommand cmd;
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    for (int i = 0; i < 3906; ++i) {
        engine.process(in.data(), out_l.data(), out_r.data(), 128);
    }
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::PLAYING);
    assert(engine.getStatus().total_frames >= 499968);

    // Timing CLEAR on 500k loop
    cmd.type = ControlCommandType::CLEAR;
    ctrl_queue.push(cmd);
    auto t0 = std::chrono::steady_clock::now();
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    auto t1 = std::chrono::steady_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    assert(engine.getStatus().state == LooperState::IDLE);
    assert(engine.getStatus().total_frames == 0);

    // Must be strictly O(1) < 1500 us (even with ASan overhead)
    assert(elapsed_us < 1500);

    std::cout << "PASSED! (Elapsed: " << elapsed_us << " us)" << std::endl;
}

void test_reverse_continuity_no_position_jump() {
    std::cout << "[TEST] Reverse Continuity (No Position Jump Across Seam)... " << std::flush;

    ControlQueue ctrl_queue;
    LoadQueue load_queue;
    BufferReturnQueue return_queue;
    SaveSlotQueue save_slot_queue;
    SaveReadyQueue save_ready_queue;

    LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128;
    config.pre_roll = 0;
    config.crossfade_samples = 0;
    config.dry_gain = 0.0f; // Only loop playback

    LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    // Record a linear ramp loop: sample i has value i * 0.001f
    // 4 periods = 512 samples
    ControlCommand cmd;
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);

    std::vector<float> in(128);
    std::vector<float> out_l(128, 0.0f);
    std::vector<float> out_r(128, 0.0f);

    for (int p = 0; p < 4; ++p) {
        for (int i = 0; i < 128; ++i) {
            in[i] = static_cast<float>(p * 128 + i) * 0.001f;
        }
        engine.process(in.data(), out_l.data(), out_r.data(), 128);
    }
    // Stop record -> playing
    cmd.type = ControlCommandType::ACTION;
    ctrl_queue.push(cmd);
    std::fill(in.begin(), in.end(), 0.0f);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::PLAYING);
    assert(engine.getStatus().total_frames == 512);

    // Play 1 more period forward: out_l[127] is sample at playhead ~255
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    float last_fwd_sample = out_l[127];

    // Toggle reverse!
    cmd.type = ControlCommandType::TOGGLE_REVERSE;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);

    // With reverse continuity, the first sample in the new period must continue from last_fwd_sample!
    // Ramp slope is 0.001f per sample. Jump must NOT be large (without continuity it would jump by ~0.25f)!
    float first_rev_sample = out_l[0];
    float jump = std::abs(first_rev_sample - last_fwd_sample);
    assert(jump < 0.005f); // Max 5 samples delta, continuous!
    (void)jump;

    // Play backwards and verify decreasing slope
    assert(out_l[10] > out_l[20]);

    // Toggle back to forward!
    float last_rev_sample = out_l[127];
    cmd.type = ControlCommandType::TOGGLE_REVERSE;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    float first_fwd_sample = out_l[0];
    float jump_back = std::abs(first_fwd_sample - last_rev_sample);
    assert(jump_back < 0.005f); // Smooth continuity in both directions!
    (void)jump_back;

    std::cout << "PASSED!" << std::endl;
}

void test_sample_accurate_action_trigger() {
    std::cout << "[TEST] Sample-Accurate Action Trigger (Sub-Block Timing)... " << std::flush;

    ControlQueue ctrl_queue;
    LoadQueue load_queue;
    BufferReturnQueue return_queue;
    SaveSlotQueue save_slot_queue;
    SaveReadyQueue save_ready_queue;

    LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128;
    config.pre_roll = 0;
    config.crossfade_samples = 0;
    config.dry_gain = 0.0f;

    LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    std::vector<float> in(128, 0.5f);
    std::vector<float> out_l(128, 0.0f);
    std::vector<float> out_r(128, 0.0f);

    // 1. Start recording at block boundary
    ControlCommand cmd;
    cmd.type = ControlCommandType::ACTION;
    cmd.sample_offset = 0;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128);
    assert(engine.getStatus().state == LooperState::RECORDING);
    assert(engine.getStatus().total_frames == 128);

    // 2. In second block, press ACTION mid-block at exactly sample 48
    cmd.type = ControlCommandType::ACTION;
    cmd.sample_offset = 48;
    ctrl_queue.push(cmd);
    std::fill(in.begin(), in.end(), 0.0f); // Live input becomes silent
    engine.process(in.data(), out_l.data(), out_r.data(), 128);

    // State must now be PLAYING!
    assert(engine.getStatus().state == LooperState::PLAYING);
    // Total frames must be exactly 128 + 48 = 176 samples! Not quantized to 128 or 256!
    assert(engine.getStatus().total_frames == 176);

    // From sample 48 to 127 in this block, the engine immediately played back the recorded 0.5f loop!
    // Frame 0..47 was silent live input (0.0f), while 48..127 must have loop playback (0.5f)!
    for (size_t i = 0; i < 48; ++i) {
        assert(std::abs(out_l[i]) < 1e-4f);
    }
    for (size_t i = 48; i < 128; ++i) {
        assert(std::abs(out_l[i] - 0.5f) < 1e-4f);
    }

    std::cout << "PASSED! (Length: " << engine.getStatus().total_frames << " frames, exact 176)" << std::endl;
}

void test_latency_calibrator_pulse_detection() {
    std::cout << "[TEST] Latency Calibrator Pulse Detection & Median Filter... " << std::flush;

    // 1. Synthetic pulse detection with background noise
    std::vector<float> buffer(256, 0.02f); // Noise floor 0.02
    buffer[73] = 0.85f; // Sharp pulse at index 73

    int32_t detected = LatencyCalibrator::detectPulseOffset(buffer.data(), buffer.size(), 0.25f);
    assert(detected == 73);
    (void)detected;

    // 2. Negative case: no signal above threshold
    std::vector<float> silence(256, 0.05f);
    assert(LatencyCalibrator::detectPulseOffset(silence.data(), silence.size(), 0.25f) == -1);

    // 3. Null / empty safety
    assert(LatencyCalibrator::detectPulseOffset(nullptr, 0, 0.25f) == -1);

    // 4. Median filter test
    std::vector<uint32_t> measurements = {390, 384, 382};
    uint32_t med = LatencyCalibrator::calculateMedian(measurements);
    assert(med == 384);
    (void)med;

    std::vector<uint32_t> even_measurements = {380, 384, 386, 390};
    uint32_t med_even = LatencyCalibrator::calculateMedian(even_measurements);
    assert(med_even == 385);
    (void)med_even;

    std::cout << "PASSED!" << std::endl;
}

void test_timestamp_to_sample_offset_and_deferral() {
    std::cout << "[TEST] Timestamp-to-Sample-Offset & Future Command Deferral... " << std::flush;

    ControlQueue ctrl_queue;
    LoadQueue load_queue;
    BufferReturnQueue return_queue;
    SaveSlotQueue save_slot_queue;
    SaveReadyQueue save_ready_queue;

    LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128;
    config.pre_roll = 0;
    config.crossfade_samples = 0;

    LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    std::vector<float> in(128, 0.5f);
    std::vector<float> out_l(128, 0.0f);
    std::vector<float> out_r(128, 0.0f);

    // 1. Start Recording at Block 0
    constexpr uint64_t B_DUR = (128ULL * 1000000000ULL) / 48000ULL; // 2666666 ns
    uint64_t b0_start = 1000000000ULL;

    ControlCommand cmd;
    cmd.type = ControlCommandType::ACTION;
    cmd.timestamp_ns = b0_start;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128, b0_start, B_DUR);
    assert(engine.getStatus().state == LooperState::RECORDING);
    assert(engine.getStatus().total_frames == 128);

    // 2. Intra-block timestamp test:
    // In Block 1, send ACTION command with timestamp exactly 1.0 ms (1,000,000 ns) after b1_start.
    // At 48 kHz, 1 ms is exactly 48 samples.
    // Command executes at sample 48 of this block, ending REC -> PLAYING.
    // Total frames recorded must be exactly 128 + 48 = 176 frames.
    uint64_t b1_start = b0_start + B_DUR;
    cmd.type = ControlCommandType::ACTION;
    cmd.timestamp_ns = b1_start + 1000000ULL; // 1.0 ms intra-block offset
    cmd.sample_offset = 0;
    ctrl_queue.push(cmd);
    std::fill(in.begin(), in.end(), 0.0f);
    engine.process(in.data(), out_l.data(), out_r.data(), 128, b1_start, B_DUR);

    assert(engine.getStatus().state == LooperState::PLAYING);
    assert(engine.getStatus().total_frames == 176);

    for (size_t i = 0; i < 48; ++i) {
        assert(std::abs(out_l[i]) < 1e-4f);
    }
    for (size_t i = 48; i < 128; ++i) {
        assert(std::abs(out_l[i] - 0.5f) < 1e-4f);
    }

    // Clear and reset for Future Command Deferral test
    cmd.type = ControlCommandType::CLEAR;
    cmd.timestamp_ns = 0;
    cmd.sample_offset = 0;
    ctrl_queue.push(cmd);
    engine.process(in.data(), out_l.data(), out_r.data(), 128, b1_start + B_DUR, B_DUR);
    assert(engine.getStatus().state == LooperState::IDLE);

    // 3. Future Command Deferral Test (Multiple future commands: ACTION @ sample 20, STOP @ sample 80):
    // Start Recording in Block A
    uint64_t ba_start = 2000000000ULL;
    cmd.type = ControlCommandType::ACTION;
    cmd.timestamp_ns = ba_start;
    cmd.sample_offset = 0;
    ctrl_queue.push(cmd);
    std::fill(in.begin(), in.end(), 0.7f);
    engine.process(in.data(), out_l.data(), out_r.data(), 128, ba_start, B_DUR);
    assert(engine.getStatus().state == LooperState::RECORDING);
    assert(engine.getStatus().total_frames == 128);

    // In Block B, queue TWO future commands destined for Block C:
    // 1) ACTION @ Block C + 20 samples (ends REC -> PLAYING at sample 20)
    //    At 48 kHz, 20 samples = (20 * 1e9) / 48000 = 416666 ns.
    // 2) STOP @ Block C + 80 samples (stops playback at sample 80)
    //    At 48 kHz, 80 samples = (80 * 1e9) / 48000 = 1666666 ns.
    uint64_t bb_start = ba_start + B_DUR;
    uint64_t bc_start = bb_start + B_DUR;
    uint64_t ts_action = bc_start + (20ULL * 1000000000ULL) / 48000ULL;
    uint64_t ts_stop = bc_start + (80ULL * 1000000000ULL) / 48000ULL;

    ControlCommand cmd_action;
    cmd_action.type = ControlCommandType::ACTION;
    cmd_action.timestamp_ns = ts_action;
    cmd_action.sample_offset = 0;
    ctrl_queue.push(cmd_action);

    ControlCommand cmd_stop;
    cmd_stop.type = ControlCommandType::STOP;
    cmd_stop.timestamp_ns = ts_stop;
    cmd_stop.sample_offset = 0;
    ctrl_queue.push(cmd_stop);

    // Process Block B: BOTH future commands must be deferred without dropping any!
    // Engine must stay in RECORDING through all 128 samples of Block B.
    engine.process(in.data(), out_l.data(), out_r.data(), 128, bb_start, B_DUR);
    assert(engine.getStatus().state == LooperState::RECORDING);
    assert(engine.getStatus().total_frames == 256); // 128 + 128

    // Process Block C: BOTH deferred commands must execute at their respective offsets (20 and 80)!
    std::fill(in.begin(), in.end(), 0.0f); // Live dry input becomes 0
    engine.process(in.data(), out_l.data(), out_r.data(), 128, bc_start, B_DUR);

    // Loop recording ended at sample 20: total_frames must be 256 + 20 = 276 samples!
    assert(engine.getStatus().total_frames == 276);
    // At sample 80, STOP executed: final state must be STOPPED!
    assert(engine.getStatus().state == LooperState::STOPPED);

    // Verify audio sub-slices in Block C:
    // 0..19: Live input (0.0f) while recording
    for (size_t i = 0; i < 20; ++i) {
        assert(std::abs(out_l[i]) < 1e-4f);
    }
    // 20..79: Loop playback (0.7f) while PLAYING
    for (size_t i = 20; i < 80; ++i) {
        assert(std::abs(out_l[i] - 0.7f) < 1e-4f);
    }
    // 80..127: Muted (0.0f) after STOP
    for (size_t i = 80; i < 128; ++i) {
        assert(std::abs(out_l[i]) < 1e-4f);
    }

    std::cout << "PASSED!" << std::endl;
}

void test_peak_hold_and_clip_detection() {
    std::cout << "[TEST] Peak Hold & Clip Detection Ballistics... " << std::flush;

    ControlQueue ctrl_queue;
    LoadQueue load_queue;
    BufferReturnQueue return_queue;
    SaveSlotQueue save_slot_queue;
    SaveReadyQueue save_ready_queue;

    LooperConfig config;
    config.sample_rate = 48000;
    config.period_size = 128;

    LooperEngine engine(ctrl_queue, load_queue, return_queue, save_slot_queue, save_ready_queue, config);

    std::vector<float> normal_in(128, 0.2f);
    std::vector<float> clip_in(128, 0.95f);
    std::vector<float> silence_in(128, 0.0f);
    std::vector<float> out_l(128, 0.0f);
    std::vector<float> out_r(128, 0.0f);

    // 1. Process normal signal (0.2f) -> no clip
    engine.process(normal_in.data(), out_l.data(), out_r.data(), 128);
    auto status = engine.getStatus();
    assert(!status.in_clipped);
    assert(status.in_peak >= 0.19f && status.in_peak <= 0.21f);

    // 2. Feed a clipping block (0.90f) -> moderate clip must trigger
    std::vector<float> mod_clip_in(128, 0.90f);
    engine.process(mod_clip_in.data(), out_l.data(), out_r.data(), 128);
    status = engine.getStatus();
    assert(status.in_clipped);
    assert(!status.in_severe_clipped);

    // 2b. Feed a severe clipping block (0.98f) -> severe clip must trigger
    std::vector<float> severe_clip_in(128, 0.98f);
    engine.process(severe_clip_in.data(), out_l.data(), out_r.data(), 128);
    status = engine.getStatus();
    assert(status.in_clipped);
    assert(status.in_severe_clipped);

    // 3. Feed silence for 10 blocks (1280 frames = ~26.6ms) -> clip MUST hold!
    for (int i = 0; i < 10; ++i) {
        engine.process(silence_in.data(), out_l.data(), out_r.data(), 128);
    }
    status = engine.getStatus();
    assert(status.in_clipped); // Peak hold active!
    assert(status.in_severe_clipped);

    // 4. Feed silence for > 500ms (> 3.5 time constants at tau=150ms)
    for (int i = 0; i < 200; ++i) {
        engine.process(silence_in.data(), out_l.data(), out_r.data(), 128);
    }
    status = engine.getStatus();
    assert(!status.in_clipped); // Clip hold expired!
    assert(!status.in_severe_clipped);
    assert(status.in_peak < 0.05f); // Peak smoothly decayed to < 5%

    std::cout << "PASSED!" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  RUNNING LOOPER ENGINE UNIT TESTS      " << std::endl;
    std::cout << "========================================" << std::endl;

    test_spsc_queue();
    test_wav_sample_rate_validation();
    test_engine_state_and_undo_redo();
    test_concurrent_get_status();
    test_monitor_mode_and_latency();
    test_preroll_length_compensation();
    test_zero_on_transition_timing();
    test_partial_overdub_with_latency();
    test_undo_then_partial_new_overdub();
    test_save_interrupted_by_clear_and_load();
    test_wav_save_includes_active_overdub();
    test_large_loop_partial_overdub_and_undo();
    test_overdub_mode_locks();
    test_second_overdub_immediate_exit_timing();
    test_undo_disallowed_during_overdub();
    test_fade_during_overdub_preserves_take();
    test_action_aborts_active_save();
    test_prefaulted_buffers_and_telemetry();
    test_clear_o1_timing_on_large_loop();
    test_reverse_continuity_no_position_jump();
    test_sample_accurate_action_trigger();
    test_latency_calibrator_pulse_detection();
    test_timestamp_to_sample_offset_and_deferral();
    test_peak_hold_and_clip_detection();

    std::cout << "\nALL UNIT TESTS PASSED SUCCESSFULLY! (24/24)" << std::endl;
    return 0;
}

