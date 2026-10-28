#pragma once

#include <span>
#include <string>

namespace pluginrack {

// Writes interleaved float samples as 16-bit PCM behind a canonical
// 44-byte header. Samples are clamped to [-1, 1] before conversion.
// Returns false when the file cannot be written.
bool writeWav(const std::string& path, std::span<const float> interleaved,
              int numChannels, int sampleRate);

} // namespace pluginrack
