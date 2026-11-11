#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pluginrack/Scene.h"

namespace pluginrack {

// PR-SEC-009: unbounded nesting before validation exhausts the stack during
// parse/teardown. fromJson enforces these in core so every consumer
// (SceneBridge, render_cli) gets the same guard.
constexpr std::size_t kMaxSceneJsonBytes = 8 * 1024 * 1024;
constexpr int kMaxSceneJsonDepth = 32;
constexpr std::size_t kMaxSceneJsonKeys = 65536;

struct SceneLoadResult {
    std::optional<Scene> scene; // engaged only when errors is empty
    std::vector<std::string> errors;
};

std::string toJson(const Scene& scene);
SceneLoadResult fromJson(std::string_view text);

} // namespace pluginrack
