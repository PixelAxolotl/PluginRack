#pragma once

#include <JuceHeader.h>

#include <functional>
#include <vector>

#include "MiniButton.h"
#include "Theme.h"

constexpr float kTraceRtaDefaultOffsetDb = 24.0f;
constexpr float kTraceRtaMinOffsetDb = -60.0f;
constexpr float kTraceRtaMaxOffsetDb = 60.0f;
constexpr float kTraceRtaStepDb = 3.0f;

// Matches the EQ spectrum floor: bins at or below this measured level are
// silence for display purposes and must stay pinned to the bottom of the
// graph regardless of the user's display offset.
inline constexpr float kTraceRtaSilenceFloorDb = -99.5f;

enum class SpectrumWeighting { Z = 0, A = 1, C = 2 };

inline SpectrumWeighting weightingFromIndex(int index) {
    return static_cast<SpectrumWeighting>(juce::jlimit(0, 2, index));
}

float weightingDb(SpectrumWeighting weighting, double hz);

// Applies EQ's display-range compression to a measured bin: truthful above
// the knee (a -6 dBFS tone stays at -6 dB), 1:2 below it, and the silence
// floor is pinned to the bottom of the trace graph. The graph maps values
// to Y through clamping, so the floor is returned as -60 dB.
float displayRtaLevelDb(float measuredDb, float displayOffsetDb, SpectrumWeighting weighting, double hz);

enum class TraceGraphMode { Frequency, Time };

struct FrequencyTraceData {
    std::vector<float> targetDb;
    std::vector<float> activityDb;
    std::vector<float> reductionDb;
    double spectrumSampleRateHz = 48000.0;
    float spectrumDisplayOffsetDb = 0.0f;
    SpectrumWeighting spectrumWeighting = SpectrumWeighting::Z;
};

struct TimeTraceData {
    std::vector<float> inputDb;
    std::vector<float> reductionDb;
    std::vector<float> outputDb;
};

class TraceGraphComponent final : public juce::Component {
public:
    TraceGraphComponent();

    void setFrequencyData(FrequencyTraceData data);
    void setTimeData(TimeTraceData data);
    void setMode(TraceGraphMode newMode);
    void setOverlayPainter(std::function<void(juce::Graphics&, juce::Rectangle<float>)> painter);

    static juce::Rectangle<float> graphBounds(juce::Rectangle<int> bounds);
    static juce::Rectangle<float> miniGraphBounds(juce::Rectangle<int> bounds);
    static float miniTraceDb(float db, juce::Rectangle<float> graph);
    static juce::Colour miniTraceColour(float inputDb, float gainDb);
    static void drawMiniTrace(juce::Graphics& g,
                              juce::Rectangle<float> graph,
                              const std::vector<float>& values,
                              juce::Colour colour,
                              bool fill);
    static void drawSpectrum(juce::Graphics& g,
                              juce::Rectangle<float> graph,
                              const std::vector<float>& values,
                              double sampleRateHz,
                              float displayOffsetDb,
                              SpectrumWeighting weighting);
    static double spectrumHzAtBin(std::size_t bin, std::size_t binCount, double sampleRateHz);
    static float spectrumLevelAtDb(const std::vector<float>& values,
                                   double frequencyHz,
                                   double sampleRateHz);
    static void drawFrequency(juce::Graphics& g,
                              juce::Rectangle<float> bounds,
                              const FrequencyTraceData& data,
                              const std::function<void(juce::Graphics&, juce::Rectangle<float>)>& overlay = {});
    static void drawTime(juce::Graphics& g,
                         juce::Rectangle<float> bounds,
                         const TimeTraceData& data,
                         const std::function<void(juce::Graphics&, juce::Rectangle<float>)>& overlay = {});

    void paint(juce::Graphics& g) override;

private:
    TraceGraphMode mode_ = TraceGraphMode::Frequency;
    FrequencyTraceData frequencyData_;
    TimeTraceData timeData_;
    std::function<void(juce::Graphics&, juce::Rectangle<float>)> overlayPainter_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TraceGraphComponent)
};
