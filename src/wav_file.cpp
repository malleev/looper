#include "wav_file.hpp"
#include "types.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace looper {
namespace {
uint16_t u16(const unsigned char* p) { return p[0] | (uint16_t(p[1]) << 8); }
uint32_t u32(const unsigned char* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void put32(unsigned char* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(v >> (i * 8));
}
bool writeAll(int fd, const void* data, size_t size) {
    auto p = static_cast<const unsigned char*>(data);
    while (size) {
        const ssize_t n = ::write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; size -= static_cast<size_t>(n);
    }
    return true;
}
}

bool WavFile::save(const std::string& filepath, const std::vector<float>& samples, uint32_t rate) {
    if (samples.empty() || !rate || rate > UINT32_MAX / 4 || samples.size() > (UINT32_MAX - 48) / 4) return false;
    for (float s : samples) if (!std::isfinite(s)) return false;
    const auto target = std::filesystem::path(filepath);
    const auto directory = target.has_parent_path() ? target.parent_path() : std::filesystem::path(".");
    std::string pattern = filepath + ".tmp-XXXXXX";
    std::vector<char> temp(pattern.begin(), pattern.end()); temp.push_back(0);
    const int fd = ::mkstemp(temp.data());
    if (fd < 0) return false;
    // IEEE float mono with fact chunk; preserve overdub headroom.
    std::array<unsigned char, 56> header{};
    std::memcpy(header.data(), "RIFF", 4);
    put32(header.data() + 4, 48 + static_cast<uint32_t>(samples.size() * 4));
    std::memcpy(header.data() + 8, "WAVEfmt ", 8);
    put32(header.data() + 16, 16); header[20] = 3; header[22] = 1;
    put32(header.data() + 24, rate); put32(header.data() + 28, rate * 4);
    header[32] = 4; header[34] = 32;
    std::memcpy(header.data() + 36, "fact", 4); put32(header.data() + 40, 4);
    put32(header.data() + 44, static_cast<uint32_t>(samples.size()));
    std::memcpy(header.data() + 48, "data", 4);
    put32(header.data() + 52, static_cast<uint32_t>(samples.size() * 4));
    bool ok = writeAll(fd, header.data(), header.size());
    std::array<unsigned char, 4096> block{};
    for (size_t pos = 0; ok && pos < samples.size();) {
        const size_t count = std::min(block.size() / 4, samples.size() - pos);
        for (size_t i = 0; i < count; ++i) {
            uint32_t bits; std::memcpy(&bits, &samples[pos + i], 4);
            put32(block.data() + i * 4, bits);
        }
        ok = writeAll(fd, block.data(), count * 4); pos += count;
    }
    if (ok) ok = (::fsync(fd) == 0);
    if (::close(fd) != 0) ok = false;
    if (ok) ok = (::rename(temp.data(), filepath.c_str()) == 0);
    if (!ok) { ::unlink(temp.data()); return false; }
    const int dirfd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) return false;
    ok = (::fsync(dirfd) == 0);
    if (::close(dirfd) != 0) ok = false;
    return ok;
}

bool WavFile::load(const std::string& filepath, std::vector<float>& out, uint32_t expected_rate,
                   std::string& error, size_t max_frames) {
    error.clear();
    auto fail = [&](const char* why) { error = why; return false; };
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) return fail("Cannot open WAV");
    const auto file_end = file.tellg();
    if (file_end < 12) return fail("Truncated RIFF header");
    const uint64_t file_size = static_cast<uint64_t>(file_end);
    file.seekg(0);
    std::array<unsigned char, 16> h{};
    if (!file.read(reinterpret_cast<char*>(h.data()), 12)) return fail("Truncated RIFF header");
    if (std::memcmp(h.data(), "RIFF", 4) || std::memcmp(h.data() + 8, "WAVE", 4)) return fail("Not a RIFF/WAVE file");
    const uint64_t riff_end = uint64_t(u32(h.data() + 4)) + 8;
    if (riff_end < 12 || riff_end > file_size) return fail("Invalid RIFF size");
    bool have_fmt = false, have_data = false;
    uint16_t format = 0, channels = 0, bits = 0, align = 0;
    uint32_t rate = 0, data_size = 0;
    uint64_t data_pos = 0;
    for (uint64_t pos = 12; pos < riff_end;) {
        if (riff_end - pos < 8) return fail("Truncated chunk header");
        file.seekg(static_cast<std::streamoff>(pos));
        if (!file.read(reinterpret_cast<char*>(h.data()), 8)) return fail("Truncated chunk header");
        const uint32_t size = u32(h.data() + 4);
        const uint64_t next = pos + 8 + uint64_t(size) + (size & 1);
        if (next > riff_end) return fail("Chunk exceeds RIFF bounds");
        if (!std::memcmp(h.data(), "fmt ", 4)) {
            if (have_fmt || size < 16) return fail("Invalid fmt chunk");
            if (!file.read(reinterpret_cast<char*>(h.data()), 16)) return fail("Truncated fmt");
            format = u16(h.data()); channels = u16(h.data() + 2); rate = u32(h.data() + 4);
            align = u16(h.data() + 12); bits = u16(h.data() + 14);
            if (!channels || channels > 32 || !rate) return fail("Invalid channels or sample rate");
            if (!((format == 1 && (bits == 16 || bits == 24 || bits == 32)) || (format == 3 && bits == 32)))
                return fail("Unsupported WAV encoding");
            if (align != channels * (bits / 8) || uint64_t(rate) * align != u32(h.data() + 8))
                return fail("Invalid WAV block alignment / byte rate");
            if (expected_rate && rate != expected_rate) return fail("Sample rate mismatch");
            have_fmt = true;
        } else if (!std::memcmp(h.data(), "data", 4)) {
            if (have_data) return fail("Multiple data chunks unsupported");
            have_data = true; data_pos = pos + 8; data_size = size;
        }
        pos = next;
    }
    if (!have_fmt || !have_data || !data_size || data_size % align) return fail("Missing / empty / incomplete WAV data");
    const size_t frames = data_size / align;
    if (frames > std::min(max_frames, MAX_LOOP_FRAMES)) return fail("WAV exceeds configured loop capacity");
    std::vector<float> decoded(frames);
    std::array<unsigned char, 128> frame{};
    file.seekg(static_cast<std::streamoff>(data_pos));
    for (size_t i = 0; i < frames; ++i) {
        if (!file.read(reinterpret_cast<char*>(frame.data()), align)) return fail("Truncated sample data");
        // MVP explicitly imports channel 1 of multichannel WAVs.
        float sample;
        if (format == 3) {
            const uint32_t raw = u32(frame.data()); std::memcpy(&sample, &raw, 4);
        } else {
            uint32_t raw = 0;
            for (unsigned b = 0; b < bits / 8; ++b) raw |= uint32_t(frame[b]) << (8 * b);
            const int64_t signed_sample = (raw & (uint32_t(1) << (bits - 1)))
                ? int64_t(raw) - (int64_t(1) << bits) : int64_t(raw);
            sample = static_cast<float>(double(signed_sample) / double(int64_t(1) << (bits - 1)));
        }
        if (!std::isfinite(sample)) return fail("Non-finite WAV sample");
        decoded[i] = sample;
    }
    out.swap(decoded);
    return true;
}
} // namespace looper
