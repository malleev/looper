#ifdef NDEBUG
#error Regression checks must stay enabled
#endif
#include "looper_engine.hpp"
#include "audio_math.hpp"
#include "wav_file.hpp"
#include "wav_worker.hpp"
#include <cassert>
#include <algorithm>
#include <array>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <limits>
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace looper;
namespace fs = std::filesystem;

struct Rig {
    ControlQueue control; LoadQueue loads; BufferReturnQueue returns;
    SaveSlotQueue slots; SaveReadyQueue ready;
    LooperConfig config;
    LooperEngine engine;
    std::array<float, 128> in{}, left{}, right{};
    static LooperConfig settings(uint32_t seam = 0) {
        LooperConfig c; c.max_loop_frames = 1024; c.pre_roll = 0;
        c.crossfade_samples = seam; c.fade_out_sec = 8.0f / 48000;
        c.monitor_mode = MonitorMode::DIRECT_ANALOG; c.latency_compensation = 0;
        return c;
    }
    explicit Rig(uint32_t seam = 0) : config(settings(seam)), engine(control, loads, returns, slots, ready, config) {}
    void run(size_t frames = 128) { engine.process(in.data(), left.data(), right.data(), frames); }
    void cmd(ControlCommandType type, int param = 0) {
        ControlCommand c; c.type = type; c.int_param = param; assert(control.push(c));
    }
};

void audio_regressions() {
    assert(floatToPcm32(1) == INT32_MAX);
    assert(floatToPcm32(-1) == INT32_MIN);
    assert(floatToPcm32(2) == INT32_MAX);
    assert(floatToPcm32(-2) == INT32_MIN);
    assert(floatToPcm32(0.5f) == 1073741824);
    assert(floatToPcm32(std::numeric_limits<float>::quiet_NaN()) == 0);
    const uint64_t t = 1000000000;
    assert(captureBlockStart(t, 352, 128, 48000) == t - 10000000);
    // Same captured block after another 48 frames arrived / 1ms elapsed.
    assert(captureBlockStart(t + 1000000, 400, 128, 48000) == t - 10000000);
    Rig r; r.in.fill(.5f);
    r.cmd(ControlCommandType::SET_MONITOR_MODE, int(MonitorMode::SOFTWARE)); r.run();
    assert(r.left[0] == .5f);
    r.cmd(ControlCommandType::SET_MONITOR_MODE, int(MonitorMode::DIRECT_ANALOG)); r.run();
    assert(r.left[0] == 0);
    r.cmd(ControlCommandType::ACTION); r.run();
    r.cmd(ControlCommandType::ACTION); r.in.fill(0); r.run();
    r.cmd(ControlCommandType::TRIGGER_FADE); r.run();
    for (size_t i = 1; i < r.left.size(); ++i) assert(r.left[i] <= r.left[i-1]);
    for (size_t i = 8; i < r.left.size(); ++i) assert(r.left[i] == 0);
    assert(r.engine.getStatus().state == LooperState::STOPPED);
    r.engine.process(nullptr, nullptr, nullptr, 0);
    std::cout << "PCM boundaries, capture backlog timing, monitor and fade: PASS\n";
}

void seam_regressions() {
    Rig r(16);
    for (size_t i = 0; i < 128; ++i) r.in[i] = -.4f + .8f * i / 127;
    r.cmd(ControlCommandType::ACTION); r.run();
    r.cmd(ControlCommandType::ACTION); r.in.fill(0); r.run();
    assert(std::abs(r.left.front() - r.left.back()) < 1e-6f);
    const auto base = r.left;
    r.cmd(ControlCommandType::ACTION);
    for (size_t i = 0; i < 128; ++i) r.in[i] = .2f * i / 127;
    r.run(); r.cmd(ControlCommandType::ACTION); r.in.fill(0); r.run();
    assert(std::abs(r.left.front() - r.left.back()) < 1e-6f);
    r.cmd(ControlCommandType::UNDO_REDO); r.run();
    for (size_t i = 0; i < 128; ++i) assert(std::abs(r.left[i] - base[i]) < 1e-6f);
    r.cmd(ControlCommandType::UNDO_REDO); r.cmd(ControlCommandType::TOGGLE_REVERSE); r.run();
    // Reverse preserves physical position 0, then crosses to position 127.
    assert(std::abs(r.left[0] - r.left[1]) < 1e-6f);
    assert(r.engine.getStatus().total_frames == 128);
    std::cout << "Base/overdub seam, undo, reverse and unchanged length: PASS\n";
}

void capacity_and_commands() {
    Rig r;
    r.cmd(ControlCommandType::ACTION);
    for (int i = 0; i < 8; ++i) r.run();
    assert(r.engine.getStatus().state == LooperState::PLAYING);
    assert(r.engine.getStatus().total_frames == 1024);
    Rig deferred;
    for (int i = 0; i < 33; ++i) {
        ControlCommand c; c.type = ControlCommandType::TOGGLE_REVERSE; c.timestamp_ns = 2000000000;
        assert(deferred.control.push(c));
    }
    deferred.engine.process(deferred.in.data(), deferred.left.data(), deferred.right.data(), 128, 1000000000, 2666666);
    assert(!deferred.engine.getStatus().is_reversed);
    deferred.engine.process(deferred.in.data(), deferred.left.data(), deferred.right.data(), 128, 2000000000, 2666666);
    assert(deferred.engine.getStatus().is_reversed);
    std::cout << "Capacity auto-finish and 33 future commands without loss: PASS\n";
}

using Bytes = std::vector<unsigned char>;
void put(Bytes& b, size_t pos, uint32_t v, int n = 4) {
    for (int i = 0; i < n; ++i) b[pos+i] = static_cast<unsigned char>(v >> (8*i));
}
Bytes pcm(unsigned bits, unsigned channels = 1, bool odd_chunk = false) {
    const unsigned align = channels * bits / 8;
    Bytes b(44 + 2 * align, 0);
    std::copy_n("RIFF", 4, b.begin()); std::copy_n("WAVEfmt ", 8, b.begin()+8);
    put(b, 16, 16); put(b, 20, 1, 2); put(b, 22, channels, 2);
    put(b, 24, 48000); put(b, 28, 48000 * align); put(b, 32, align, 2); put(b, 34, bits, 2);
    std::copy_n("data", 4, b.begin()+36); put(b, 40, 2*align);
    put(b, 44, uint32_t(1) << (bits-2), bits/8);
    put(b, 44+align, uint32_t(1) << (bits-1), bits/8);
    if (odd_chunk) {
        Bytes junk{'J','U','N','K',1,0,0,0,42,0};
        b.insert(b.begin()+36, junk.begin(), junk.end());
    }
    put(b, 4, b.size()-8); return b;
}
void writeFixture(const fs::path& p, const Bytes& b) {
    std::ofstream f(p, std::ios::binary); f.write(reinterpret_cast<const char*>(b.data()), b.size());
    assert(bool(f));
}
Bytes readBytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary); return Bytes(std::istreambuf_iterator<char>(f), {});
}
void wav_regressions(const fs::path& dir) {
    const auto path = dir / "test.wav";
    std::string error; std::vector<float> decoded;
    for (unsigned bits : {16,24,32}) {
        writeFixture(path, pcm(bits, 2, true));
        assert(WavFile::load(path, decoded, 48000, error));
        assert(decoded.size() == 2 && decoded[0] == .5f && decoded[1] == -1);
    }
    auto bad = [&](Bytes b) {
        writeFixture(path, b); decoded = {7};
        assert(!WavFile::load(path, decoded, 48000, error));
        assert(decoded == std::vector<float>{7}); assert(!error.empty());
    };
    auto b = pcm(16); put(b,22,0,2); bad(b);
    b = pcm(16); put(b,20,6,2); bad(b);
    b = pcm(16); b.pop_back(); bad(b);
    b = pcm(16); put(b,16,4); bad(b);
    b = pcm(16); put(b,40,0xffffffff); bad(b);
    b = pcm(16); put(b,32,1,2); bad(b);
    bad(Bytes{0,1,2});
    const std::vector<float> original{1.75f,-2.5f,.125f};
    assert(WavFile::save(path, original, 48000));
    assert(WavFile::load(path, decoded, 48000, error)); assert(decoded == original);
    assert(!WavFile::load(path, decoded, 48000, error, 2));
    const auto good = readBytes(path);
    assert(!WavFile::save(path, {std::numeric_limits<float>::infinity()}, 48000));
    assert(readBytes(path) == good);
    // Real short-write failure, in a child so the parent's resource limits stay intact.
    pid_t child = fork(); assert(child >= 0);
    if (child == 0) {
        signal(SIGXFSZ, SIG_IGN);
        struct rlimit limit{128,128};
        if (setrlimit(RLIMIT_FSIZE, &limit)) _exit(2);
        _exit(WavFile::save(path, std::vector<float>(4096,.5f),48000) ? 3 : 0);
    }
    int status = 0; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(readBytes(path) == good);
    for (const auto& entry : fs::directory_iterator(dir)) assert(entry.path().extension() == ".wav");
    b = good; put(b,56,0x7fc00000); bad(b);
    std::cout << "WAV encodings, corrupt files, float headroom and atomic short-write failure: PASS\n";
}

int main() {
    char pattern[] = "/tmp/looper-regression-XXXXXX";
    char* dir = mkdtemp(pattern); assert(dir);
    audio_regressions(); seam_regressions(); capacity_and_commands(); wav_regressions(dir);
    fs::remove_all(dir);
    std::cout << "All MVP regression checks passed\n";
}
