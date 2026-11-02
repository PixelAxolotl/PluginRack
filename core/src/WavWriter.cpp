#include "pluginrack/WavWriter.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>

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
    const float finite = std::isfinite(sample) ? sample : 0.0f;
    const float clamped = finite < -1.0f ? -1.0f : (finite > 1.0f ? 1.0f : finite);
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
    const auto channels = static_cast<std::uint64_t>(numChannels);
    if (channels > std::numeric_limits<std::uint16_t>::max() ||
        interleaved.size() % channels != 0) {
        return false;
    }
    constexpr auto kMaxDataBytes =
        std::numeric_limits<std::uint32_t>::max() - 36u;
    if (interleaved.size() >
        static_cast<std::size_t>(kMaxDataBytes / 2u)) {
        return false;
    }
    const auto dataBytes = static_cast<std::uint64_t>(interleaved.size()) * 2u;
    const auto byteRate = static_cast<std::uint64_t>(sampleRate) * channels * 2u;
    if (channels * 2u > std::numeric_limits<std::uint16_t>::max() ||
        byteRate > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    const auto dataBytes32 = static_cast<std::uint32_t>(dataBytes);
    const auto channels16 = static_cast<std::uint16_t>(channels);
    const auto rate = static_cast<std::uint32_t>(sampleRate);
    const auto byteRate32 = static_cast<std::uint32_t>(byteRate);
    const auto blockAlign = static_cast<std::uint16_t>(channels * 2u);

    out.write("RIFF", 4);
    writeU32(out, 36u + dataBytes32);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeU32(out, 16);
    writeU16(out, 1);
    writeU16(out, channels16);
    writeU32(out, rate);
    writeU32(out, byteRate32);
    writeU16(out, blockAlign);
    writeU16(out, 16);
    out.write("data", 4);
    writeU32(out, dataBytes32);
    for (const float sample : interleaved) {
        writeSample(out, sample);
    }
    out.close();
    return static_cast<bool>(out);
}

} // namespace pluginrack
