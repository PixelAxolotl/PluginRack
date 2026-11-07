#include <cctype>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "limits.h"

#include "pluginrack/DeviceRegistry.h"
#include "pluginrack/OfflineRenderer.h"
#include "pluginrack/SceneAvailability.h"
#include "pluginrack/SceneCodec.h"
#include "pluginrack/SceneValidator.h"
#include "pluginrack/WavWriter.h"

namespace {

class AllDevicesPresent : public pluginrack::DeviceRegistry {
public:
    bool hasInput(const std::string&) const override { return true; }
    bool hasOutput(const std::string&) const override { return true; }
};

bool parsePositiveFinite(const std::string& text, double* value) {
    try {
        std::size_t consumed = 0;
        const double parsed = std::stod(text, &consumed);
        if (consumed != text.size() || !std::isfinite(parsed) || parsed <= 0.0) {
            return false;
        }
        *value = parsed;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string outputFileStem(const pluginrack::Output& output) {
    std::string stem = output.name;
    for (char& ch : stem) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) == 0 && ch != '.' && ch != '-' && ch != '_' && ch != ' ') {
            ch = '_';
        }
    }
    while (!stem.empty() && (stem.back() == '.' || stem.back() == ' ')) {
        stem.pop_back();
    }
    if (stem.empty()) {
        stem = "output-" + std::to_string(output.id);
    }
    return stem;
}

std::string fileKey(const std::string& fileName) {
    std::string key = fileName;
    for (char& ch : key) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return key;
}

void usage() {
    std::cerr << "usage: pluginrack-render <scene.json> <outDir>"
                 " [--tone <hz>] [--seconds <n>]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        usage();
        return 1;
    }
    const std::string scenePath = argv[1];
    const std::string outDir = argv[2];
    double toneHz = 440.0;
    double seconds = 2.0;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        double* destination = nullptr;
        std::string value;
        if (arg == "--tone" && i + 1 < argc) {
            destination = &toneHz;
            value = argv[++i];
        } else if (arg == "--seconds" && i + 1 < argc) {
            destination = &seconds;
            value = argv[++i];
        } else if (arg.rfind("--tone=", 0) == 0) {
            destination = &toneHz;
            value = arg.substr(7);
        } else if (arg.rfind("--seconds=", 0) == 0) {
            destination = &seconds;
            value = arg.substr(10);
        } else {
            std::cerr << "error: unknown argument '" << arg << "'\n";
            return 1;
        }
        if (!parsePositiveFinite(value, destination)) {
            std::cerr << "error: --tone and --seconds must be finite positive numbers\n";
            return 1;
        }
    }

    std::error_code sizeEc;
    const std::uintmax_t sceneSize = std::filesystem::file_size(scenePath, sizeEc);
    if (sizeEc || !render_limits::sceneWithinLimit(static_cast<std::int64_t>(sceneSize))) {
        std::cerr << "error: scene file '" << scenePath
                  << "' is too large (limit "
                  << render_limits::kMaxSceneFileBytes / (1024 * 1024) << " MiB)\n";
        return 1;
    }

    std::ifstream sceneFile(scenePath, std::ios::binary);
    if (!sceneFile) {
        std::cerr << "error: cannot open '" << scenePath << "'\n";
        return 1;
    }
    std::ostringstream text;
    text << sceneFile.rdbuf();
    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(text.str());
    if (!loaded.scene.has_value()) {
        for (const std::string& loadError : loaded.errors) {
            std::cerr << "error: " << loadError << "\n";
        }
        if (loaded.errors.empty()) {
            std::cerr << "error: failed to load scene\n";
        }
        return 1;
    }
    const pluginrack::Scene& scene = *loaded.scene;
    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    if (!errors.empty()) {
        std::cerr << "error: " << errors.front().message << "\n";
        return 1;
    }

    const AllDevicesPresent registry;
    const pluginrack::SceneAvailability availability =
        pluginrack::resolveAvailability(scene, registry);
    if (!render_limits::secondsWithinLimit(seconds, scene.sampleRate)) {
        std::cerr << "error: --seconds must be between 0 and "
                  << render_limits::kMaxSeconds << " and fit in the frame count\n";
        return 1;
    }
    const double frameCount = seconds * scene.sampleRate;
    if (!render_limits::outputWithinLimit(frameCount,
                                           static_cast<std::int64_t>(scene.outputs.size()))) {
        std::cerr << "error: render is too large (output sample budget exceeded)\n";
        return 1;
    }
    const int numFrames = static_cast<int>(frameCount);
    std::vector<std::vector<float>> buffers;
    try {
        buffers = pluginrack::renderSine(scene, availability,
                                         static_cast<float>(toneHz), 1.0f, numFrames);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) {
        std::cerr << "error: cannot create '" << outDir << "'\n";
        return 1;
    }
    std::unordered_set<std::string> usedFileNames;
    usedFileNames.reserve(scene.outputs.size() * 2 + 1);
    for (std::size_t o = 0; o < scene.outputs.size(); ++o) {
        const std::string stem = outputFileStem(scene.outputs[o]);
        std::string fileName = stem + ".wav";
        if (!usedFileNames.insert(fileKey(fileName)).second) {
            fileName = stem + "-" + std::to_string(scene.outputs[o].id) + ".wav";
            int suffix = 2;
            while (!usedFileNames.insert(fileKey(fileName)).second) {
                fileName = stem + "-" + std::to_string(scene.outputs[o].id) + "-" +
                           std::to_string(suffix++) + ".wav";
            }
        }
        // outputFileStem produces one filename component, so this cannot escape outDir.
        const std::string outPath = (std::filesystem::path(outDir) / fileName).string();
        if (o >= buffers.size() ||
            !pluginrack::writeWav(outPath, buffers[o], 2,
                                  static_cast<int>(scene.sampleRate))) {
            std::cerr << "error: cannot write '" << outPath << "'\n";
            return 1;
        }
        float peak = 0.0f;
        for (const float sample : buffers[o]) {
            const float abs = std::fabs(sample);
            if (abs > peak) {
                peak = abs;
            }
        }
        std::cout << "output \"" << scene.outputs[o].name << "\": peak " << peak << "\n";
    }
    return 0;
}
