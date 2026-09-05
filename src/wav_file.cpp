#include "wav_file.hpp"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cstring>

namespace looper {

#pragma pack(push, 1)
struct WavHeader {
    char riff_tag[4] = {'R', 'I', 'F', 'F'};
    uint32_t riff_size = 0;
    char wave_tag[4] = {'W', 'A', 'V', 'E'};
    char fmt_tag[4] = {'f', 'm', 't', ' '};
    uint32_t fmt_size = 16;
    uint16_t audio_format = 1; // PCM
    uint16_t num_channels = 1; // Mono
    uint32_t sample_rate = 48000;
    uint32_t byte_rate = 48000 * 2;
    uint16_t block_align = 2;
    uint16_t bits_per_sample = 16;
    char data_tag[4] = {'d', 'a', 't', 'a'};
    uint32_t data_size = 0;
};
#pragma pack(pop)

bool WavFile::save(const std::string& filepath, const std::vector<float>& samples, uint32_t sample_rate) {
    if (samples.empty()) {
        std::cerr << "[WAV] Cannot save empty audio buffer." << std::endl;
        return false;
    }

    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[WAV] Failed to open file for writing: " << filepath << std::endl;
        return false;
    }

    uint32_t data_bytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    WavHeader header;
    header.sample_rate = sample_rate;
    header.num_channels = 1;
    header.bits_per_sample = 16;
    header.byte_rate = sample_rate * header.num_channels * (header.bits_per_sample / 8);
    header.block_align = header.num_channels * (header.bits_per_sample / 8);
    header.data_size = data_bytes;
    header.riff_size = sizeof(WavHeader) - 8 + data_bytes;

    file.write(reinterpret_cast<const char*>(&header), sizeof(WavHeader));

    std::vector<int16_t> pcm16(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        float s = std::clamp(samples[i], -1.0f, 1.0f);
        pcm16[i] = static_cast<int16_t>(s * 32767.0f);
    }

    file.write(reinterpret_cast<const char*>(pcm16.data()), data_bytes);
    file.close();

    std::cout << "[WAV] Saved " << samples.size() << " samples ("
              << (float)samples.size() / sample_rate << "s) to " << filepath << std::endl;
    return true;
}

bool WavFile::load(const std::string& filepath, std::vector<float>& out_samples, uint32_t expected_sample_rate, std::string& error_msg) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        error_msg = "Cannot open file: " + filepath;
        return false;
    }

    char chunk_id[4];
    uint32_t chunk_size = 0;

    file.read(chunk_id, 4);
    if (std::memcmp(chunk_id, "RIFF", 4) != 0) {
        error_msg = "Not a valid RIFF file: " + filepath;
        return false;
    }

    file.read(reinterpret_cast<char*>(&chunk_size), 4);
    char wave_id[4];
    file.read(wave_id, 4);
    if (std::memcmp(wave_id, "WAVE", 4) != 0) {
        error_msg = "Not a valid WAVE file: " + filepath;
        return false;
    }

    uint16_t channels = 1;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 16;
    bool fmt_found = false;

    while (file.read(chunk_id, 4) && file.read(reinterpret_cast<char*>(&chunk_size), 4)) {
        if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
            uint16_t format_tag = 0;
            file.read(reinterpret_cast<char*>(&format_tag), 2);
            file.read(reinterpret_cast<char*>(&channels), 2);
            file.read(reinterpret_cast<char*>(&sample_rate), 4);
            file.seekg(6, std::ios::cur); // skip byte_rate & block_align
            file.read(reinterpret_cast<char*>(&bits_per_sample), 2);

            // Strict sample rate check!
            if (expected_sample_rate > 0 && sample_rate != expected_sample_rate) {
                error_msg = "Sample rate mismatch: expected " + std::to_string(expected_sample_rate) +
                            " Hz, got " + std::to_string(sample_rate) + " Hz";
                return false;
            }

            if (chunk_size > 16) {
                file.seekg(chunk_size - 16, std::ios::cur);
            }
            fmt_found = true;
        } else if (std::memcmp(chunk_id, "data", 4) == 0) {
            if (!fmt_found) {
                error_msg = "Corrupted WAV: data chunk found before fmt chunk";
                return false;
            }

            if (bits_per_sample == 16) {
                size_t num_samples = chunk_size / (channels * sizeof(int16_t));
                out_samples.resize(num_samples);
                std::vector<int16_t> raw_buf(channels);
                for (size_t i = 0; i < num_samples; ++i) {
                    file.read(reinterpret_cast<char*>(raw_buf.data()), channels * sizeof(int16_t));
                    out_samples[i] = static_cast<float>(raw_buf[0]) / 32768.0f;
                }
            } else if (bits_per_sample == 24) {
                size_t num_samples = chunk_size / (channels * 3);
                out_samples.resize(num_samples);
                std::vector<uint8_t> raw_buf(channels * 3);
                for (size_t i = 0; i < num_samples; ++i) {
                    file.read(reinterpret_cast<char*>(raw_buf.data()), channels * 3);
                    int32_t val = (raw_buf[0]) | (raw_buf[1] << 8) | (raw_buf[2] << 16);
                    if (val & 0x800000) val |= 0xFF000000;
                    out_samples[i] = static_cast<float>(val) / 8388608.0f;
                }
            } else {
                error_msg = "Unsupported bits per sample: " + std::to_string(bits_per_sample);
                return false;
            }

            return true;
        } else {
            file.seekg(chunk_size, std::ios::cur);
        }
    }

    error_msg = "No data chunk found in WAV";
    return false;
}

} // namespace looper
