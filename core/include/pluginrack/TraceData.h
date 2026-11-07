#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "DeEsserEffect.h"
#include "MultibandEffect.h"

namespace pluginrack {

std::vector<float> makeMultibandTargetTrace(const MultibandEffect& effect,
                                            std::size_t binCount,
                                            float minHz = 20.0f,
                                            float maxHz = 20000.0f);

std::vector<float> makeMultibandGainChangeTrace(const MultibandEffect& effect,
                                                const std::vector<float>& reductionsDb,
                                                std::size_t binCount,
                                                float minHz = 20.0f,
                                                float maxHz = 20000.0f);

std::vector<float> makeDeEsserGainReductionTrace(const DeEsserEffect& effect,
                                                 float reductionDb,
                                                 std::size_t binCount,
                                                 float minHz = 20.0f,
                                                 float maxHz = 20000.0f);

class LevelHistory final {
public:
    explicit LevelHistory(std::size_t capacity);

    void push(float inputDb, float reductionDb, float outputDb) noexcept;
    std::size_t size() const noexcept { return size_; }
    void copyOrderedTo(std::span<float> inputDb,
                       std::span<float> reductionDb,
                       std::span<float> outputDb) const noexcept;

private:
    std::vector<float> inputDb_;
    std::vector<float> reductionDb_;
    std::vector<float> outputDb_;
    std::size_t next_ = 0;
    std::size_t size_ = 0;
};

} // namespace pluginrack
