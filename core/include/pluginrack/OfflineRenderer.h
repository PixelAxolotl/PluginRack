#pragma once

#include <vector>

#include "pluginrack/Scene.h"
#include "pluginrack/SceneAvailability.h"

namespace pluginrack {

// Renders a mono sine (amplitude * sin(2*pi*toneHz*t)) through the engine
// block by block and returns one interleaved stereo buffer per output,
// each holding numFrames frames. Stopped channels and disabled outputs
// from availability render as silence. Throws std::runtime_error when the
// scene cannot be prepared.
std::vector<std::vector<float>> renderSine(const Scene& scene,
                                           const SceneAvailability& availability,
                                           float toneHz, float amplitude,
                                           int numFrames);

} // namespace pluginrack
