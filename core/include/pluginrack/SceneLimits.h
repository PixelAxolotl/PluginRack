#pragma once

#include <cstdint>

namespace pluginrack {

// Largest scene file the loader accepts. One constant so the GUI, the
// headless renderer and the tests cannot drift apart.
inline constexpr std::int64_t kMaxSceneFileBytes = 8 * 1024 * 1024;

} // namespace pluginrack
