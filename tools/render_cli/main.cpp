#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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
    try {
        for (int i = 3; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--tone" && i + 1 < argc) {
                toneHz = std::stod(argv[++i]);
            } else if (arg == "--seconds" && i + 1 < argc) {
                seconds = std::stod(argv[++i]);
            } else if (arg.rfind("--tone=", 0) == 0) {
                toneHz = std::stod(arg.substr(7));
            } else if (arg.rfind("--seconds=", 0) == 0) {
                seconds = std::stod(arg.substr(10));
            } else {
                std::cerr << "error: unknown argument '" << arg << "'\n";
                return 1;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "error: bad number: " << e.what() << "\n";
        return 1;
    }
    if (toneHz <= 0.0 || seconds <= 0.0) {
        std::cerr << "error: --tone and --seconds must be positive\n";
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
    const int numFrames = static_cast<int>(seconds * scene.sampleRate);
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
    for (std::size_t o = 0; o < scene.outputs.size(); ++o) {
        const std::string fileName =
            (scene.outputs[o].name.empty()
                 ? "output-" + std::to_string(scene.outputs[o].id)
                 : scene.outputs[o].name) +
            ".wav";
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
