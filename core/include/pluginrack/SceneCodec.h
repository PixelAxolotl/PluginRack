#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pluginrack/Scene.h"

namespace pluginrack {

struct SceneLoadResult {
    std::optional<Scene> scene; // engaged only when errors is empty
    std::vector<std::string> errors;
};

std::string toJson(const Scene& scene);
SceneLoadResult fromJson(std::string_view text);

} // namespace pluginrack
