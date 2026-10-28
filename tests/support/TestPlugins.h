#pragma once

#include <memory>
#include <string>

#include "pluginrack/PluginHost.h"

namespace pluginrack::test {

class HalvePlugin : public IPluginInstance {
public:
    void prepare(double /*sampleRate*/, int /*maxBlockFrames*/) override {}
    ProcessResult process(AudioBlock& block) noexcept override {
        if (block.channels == nullptr || block.numFrames <= 0) {
            return ProcessResult::Ok;
        }
        for (int c = 0; c < block.numChannels; ++c) {
            float* channel = block.channels[c];
            if (channel == nullptr) {
                continue;
            }
            for (int i = 0; i < block.numFrames; ++i) {
                channel[i] *= 0.5f;
            }
        }
        return ProcessResult::Ok;
    }
    void reset() noexcept override {}
};

class AddOnePlugin : public IPluginInstance {
public:
    void prepare(double /*sampleRate*/, int /*maxBlockFrames*/) override {}
    ProcessResult process(AudioBlock& block) noexcept override {
        if (block.channels == nullptr || block.numFrames <= 0) {
            return ProcessResult::Ok;
        }
        for (int c = 0; c < block.numChannels; ++c) {
            float* channel = block.channels[c];
            if (channel == nullptr) {
                continue;
            }
            for (int i = 0; i < block.numFrames; ++i) {
                channel[i] += 0.25f;
            }
        }
        return ProcessResult::Ok;
    }
    void reset() noexcept override {}
};

class HalfFactory : public IPluginFactory {
public:
    std::unique_ptr<IPluginInstance> create(const std::string& /*pluginPath*/,
                                            std::string* error) override {
        if (error != nullptr) {
            error->clear();
        }
        return std::make_unique<HalvePlugin>();
    }
};

class FailingFactory : public IPluginFactory {
public:
    std::unique_ptr<IPluginInstance> create(const std::string& /*pluginPath*/,
                                            std::string* error) override {
        if (error != nullptr) {
            *error = "boom";
        }
        return nullptr;
    }
};

} // namespace pluginrack::test
