#pragma once

#include <cstddef>
#include <cstdint>

namespace pluginrack {

// Largest scene file the loader accepts. One constant so the GUI, the
// headless renderer and the tests cannot drift apart.
inline constexpr std::int64_t kMaxSceneFileBytes = 8 * 1024 * 1024;

// Bus caps shared by the validator and both engines, so the engine refuses to
// create what the validator would reject on the next load.
inline constexpr std::size_t kMaxBuses = 16;
inline constexpr std::size_t kMaxBusSends = 1024;
inline constexpr std::size_t kMaxBusRoutes = 512;
// Channel -> output routes; lent to the live engine so connectSend cannot
// exceed what the validator accepts.
inline constexpr std::size_t kMaxRoutes = 1024;

} // namespace pluginrack
