#pragma once

#include <memory>
#include <string>

#include "pluginrack/AudioBlock.h"

namespace pluginrack {

enum class ProcessResult { Ok, Failed };

// Host seam for VST/host plugin instances. The seam must NOT assume
// in-process hosting: factories hand out instances behind this interface,
// so out-of-process bridges stay source-compatible.
class IPluginInstance {
public:
    virtual ~IPluginInstance() = default;
    virtual void prepare(double sampleRate, int maxBlockFrames) = 0;
    virtual ProcessResult process(AudioBlock& block) noexcept = 0;
    virtual void reset() noexcept = 0;
};

class IPluginFactory {
public:
    virtual ~IPluginFactory() = default;
    // Returns nullptr and fills *error on failure. Must not throw.
    virtual std::unique_ptr<IPluginInstance> create(const std::string& pluginPath,
                                                    std::string* error) = 0;
};

} // namespace pluginrack
