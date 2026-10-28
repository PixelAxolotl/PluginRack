#include "pluginrack/WavWriter.h"

#include <cmath>
#include <cstdint>
#include <fstream>

namespace pluginrack {

namespace {

void writeU16(std::ofstream& out, std::uint16_t value) {
    const char bytes[2] = {
        static_cast<char>(value & 0xFFu),
        static_cast<char>((value >> 8) & 0xFFu),
    };
    out.write(bytes, 2);
}

void writeU32(std::ofstream& out, std::uint32_t value) {
    const char bytes[4] = {
        static_cast<char>(value & 0xFFu),
        static_cast<char>((value >> 8) & 0xFFu),
        static_cast<char>((value >> 16) & 0xFFu),
        static_cast<char>((value >> 24) & 0xFFu),
    };
    out.write(bytes, 4);
}

void writeSample(std::ofstream& out, float sample) {
    const float clamped = sample < -1.0f ? -1.0f : (sample > 1.0f ? 1.0f : sample);
    const auto pcm =
        static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lrintf(clamped * 32767.0f)));
    writeU16(out, pcm);
}

} // namespace

bool writeWav(const std::string& path, std::span<const float> interleaved,
              int numChannels, int sampleRate) {
    if (numChannels <= 0 || sampleRate <= 0) {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    const std::uint32_t dataBytes =
        static_cast<std::uint32_t>(interleaved.size() * 2);
    const std::uint32_t channels = static_cast<std::uint32_t>(numChannels);
    const std::uint32_t rate = static_cast<std::uint32_t>(sampleRate);

    out.write("RIFF", 4);
    writeU32(out, 36 + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeU32(out, 16);
    writeU16(out, 1);
    writeU16(out, static_cast<std::uint16_t>(channels));
    writeU32(out, rate);
    writeU32(out, rate * channels * 2u);
    writeU16(out, static_cast<std::uint16_t>(channels * 2u));
    writeU16(out, 16);
    out.write("data", 4);
    writeU32(out, dataBytes);
    for (const float sample : interleaved) {
        writeSample(out, sample);
    }
    out.close();
    return static_cast<bool>(out);
}

} // namespace pluginrack
