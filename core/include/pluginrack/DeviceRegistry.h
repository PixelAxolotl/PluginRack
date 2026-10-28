#pragma once

#include <string>

namespace pluginrack {

class DeviceRegistry {
public:
    virtual ~DeviceRegistry() = default;
    virtual bool hasInput(const std::string& deviceId) const = 0;
    virtual bool hasOutput(const std::string& deviceId) const = 0;
};

} // namespace pluginrack
