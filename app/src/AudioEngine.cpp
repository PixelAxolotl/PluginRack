#include "AudioEngine.h"

#include "AppPaths.h"
#include "CrashContext.h"
#include "DeviceBackend.h"
#include "DeviceNames.h"
#include "Theme.h"

#include <pluginrack/AsioDeviceMatch.h>
#include <pluginrack/AudioDeviceProbe.h>
#include <pluginrack/ChainOrder.h>
#include <pluginrack/ClockFailover.h>
#include <pluginrack/InputBufferPolicy.h>
#include <pluginrack/InputFollowPolicy.h>
#include <pluginrack/RenderSlicer.h>
#include <pluginrack/SampleScrub.h>
#include <pluginrack/SceneLimits.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#if JUCE_WINDOWS
 #include <objbase.h>
 // COM's headers drag in windows.h, whose min/max macros break
 // std::numeric_limits<int>::max() and juce::jmax below.
 #ifdef max
  #undef max
 #endif
 #ifdef min
  #undef min
 #endif
#endif

namespace {
constexpr std::uint32_t kLatencyMeasureTimeoutMs = 3000;

// A removed insert or strip is stopped after stateLock is released:
// closeFloatingEditor must run on the message thread and stopChildHost joins
// the sandbox process, so neither may happen while render() holds the lock.
void retireInsert(AudioEngine::InsertSlot& slot) {
    slot.pluginHost.closeFloatingEditor();
    slot.pluginHost.stopChildHost();
    slot.pluginHost.releaseResources();
}

template <typename Strip>
void retireStripInserts(Strip& strip) {
    for (auto& insert : strip.inserts) retireInsert(*insert);
}

// Text the audio thread publishes while it renders an insert, so a crash inside
// the plugin names the insert instead of just naming a module offset
// (CrashContext.h). Built once, on the message thread, when the insert is
// created; the registry keeps the text alive for the life of the process.
// When the child-process renderer lands, append its mode here ("child" vs
// "in-process") - see the parked plan, section 6.0.
const char* makeCrashLabel(const juce::String& stripName, std::uint32_t insertId,
                           const juce::String& pluginName, const juce::String& pluginPath) {
    const auto strip = stripName.isNotEmpty() ? stripName : juce::String("output");
    const auto text = strip.toStdString() + " insert " + std::to_string(insertId) + " \""
                      + pluginName.toStdString() + "\" ["
                      + juce::File(pluginPath).getFileName().toStdString() + "]";
    return crash_context::registerLabel(text);
}
// A detected impulse further away than this cannot be the loopback.
constexpr double kLatencyMeasureMaxSeconds = 5.0;
// Device probes can finish in either order. Reserve enough FIFO/ring history
// for a larger negotiated clock block so outputs and inputs can be opened in
// parallel without making startup wait for a second, serialized probe.
constexpr int kProbeRenderBlockFrames = 4096;
// A single-client ASIO driver (M-Audio M-Track) refuses a second client for a
// moment after the previous stream is closed, reporting the bare "No such
// device" from loadDriver(). The clock gets one short retry; a driver that
// still refuses is reopened on the message thread, which is the same recovery
// as pressing Force ASIO by hand.
constexpr int kClockOpenAttempts = 2;
constexpr int kClockRetryDelayMs = 150;
// Prefix of the error shown for an assigned device that is not enumerated.
const juce::String kNoSuchDevice = "No such device: ";
// A clock callback that has not run for this long is treated as dead. Large
// driver buffers still call back far more often than this; the grace only has
// to absorb a busy machine or a sleep/resume hiccup.
constexpr std::uint64_t kClockStallGraceMs = 2000;
// Auto-reclaim: the first output is the preferred clock. Reopening a returned
// endpoint is rate-limited so a dead device is not hammered every UI tick, and
// bounded so an endpoint that cannot open at the session rate stops trying.
constexpr std::uint32_t kReclaimRetryMs = 5000;
constexpr int kReclaimMaxAttempts = 3;

// A single-client driver reports its own bare wording ("No such device");
// the engine's own message adds the endpoint after a colon. Both mean the same
// refusal, so the retry predicate has to accept either spelling.
bool isNoSuchDeviceError(const juce::String& message) {
    return message.containsIgnoreCase("No such device");
}

// The persisted mode is the type the opener actually selected, not the
// direction: the next launch uses it to skip a mode this endpoint already
// refused while keeping the cache advisory.
juce::String deviceModeName(const juce::String& deviceType) {
    const auto id = pluginrack::deviceModeId(pluginrack::deviceBackendOf(deviceType.toRawUTF8()));
    return id.empty() ? juce::String("shared") : juce::String(id.data(), static_cast<int>(id.size()));
}

// The JUCE patch reads this on the opener thread. Writing it on every apply
// (including 0 when the toggle is off) is what makes turning the cap off take
// effect on the next open instead of surviving until restart.
void setWasapiProbeCapEnvironment(bool enabled, int channels) {
   #if JUCE_WINDOWS
    const auto value = pluginrack::wasapiProbeCapValue(enabled, channels);
    const auto name = juce::String("PLUGINRACK_WASAPI_MAX_PROBE_CHANNELS");
    _putenv_s(name.toRawUTF8(), juce::String(value).toRawUTF8());
    juce::Logger::writeToLog("device: wasapi probe cap " + juce::String(value)
                             + (value == 0 ? " (stock)" : " channels"));
   #else
    juce::ignoreUnused(enabled, channels);
   #endif
}

// Resolve a strip's chain and its unchained-effect pass into RenderStep lists.
// This is the only place that searches the chain for a row's effect or insert;
// it runs on the message thread under stateLock whenever one of those vectors
// can change, so render() only switches on a step's type and indexes a vector.
// The rules are the ones render() used inline:
//  - a built-in row resolves its effect by id inside its own family vector, an
//    Insert row resolves its slot by id inside `inserts`;
//  - a row's Item tap is owned by the first row carrying that id, even when a
//    later row of another family shares it (Scene.h firstChainItemIndex);
//  - the fallback pass runs the unchained families in the order the old
//    processNewMissing calls did, then the EQ when the chain has no Eq row, in
//    the same last position the offline mixer appends it.
template <typename StripType>
void buildRenderPlan(StripType& strip) {
    strip.renderSteps.clear();
    strip.fallbackSteps.clear();
    strip.renderSteps.reserve(strip.chain.size());
    strip.hasEqInChain = false;

    const auto findIndex = [](const auto& effects, std::uint32_t id) {
        for (std::size_t i = 0; i < effects.size(); ++i) {
            if (effects[i].id == id) return i;
        }
        return static_cast<std::size_t>(-1);
    };

    for (std::size_t row = 0; row < strip.chain.size(); ++row) {
        const auto& item = strip.chain[row];
        AudioEngine::RenderStep step;
        step.type = item.type;
        step.itemId = item.id;
        step.tapOwned =
            pluginrack::firstChainItemIndex(strip.chain, item.id) == static_cast<int>(row);
        switch (item.type) {
        case pluginrack::ChainItemType::Eq:
            strip.hasEqInChain = true;
            break;
        case pluginrack::ChainItemType::Dynamics:
            step.index = findIndex(strip.effects, item.id);
            break;
        case pluginrack::ChainItemType::Multiband:
            step.index = findIndex(strip.multibandEffects, item.id);
            break;
        case pluginrack::ChainItemType::DeEsser:
            step.index = findIndex(strip.deEsserEffects, item.id);
            break;
        case pluginrack::ChainItemType::Limiter:
            step.index = findIndex(strip.limiterEffects, item.id);
            break;
        case pluginrack::ChainItemType::FeedbackSuppressor:
            step.index = findIndex(strip.feedbackSuppressorEffects, item.id);
            break;
        case pluginrack::ChainItemType::SpectralDenoiser:
            step.index = findIndex(strip.spectralDenoiserEffects, item.id);
            break;
        case pluginrack::ChainItemType::Insert:
            for (const auto& insert : strip.inserts) {
                if (insert->id == item.id) {
                    step.insert = insert.get();
                    break;
                }
            }
            break;
        }
        strip.renderSteps.push_back(step);
    }

    // Coverage is per family: ids are unique inside a family but a default
    // Dynamics and a default Limiter both use id 1, so one id-keyed set would
    // let a row of one family cover an unchained effect of another. This is the
    // same ChainItem (type, id) comparison processNewMissing used.
    const auto chained = [&strip](pluginrack::ChainItemType type, std::uint32_t id) {
        return std::any_of(strip.chain.begin(), strip.chain.end(),
                           [type, id](const pluginrack::ChainItem& item) {
                               return item.type == type && item.id == id;
                           });
    };
    const auto appendUnchained = [&strip, &chained](pluginrack::ChainItemType type,
                                                     const auto& effects) {
        for (std::size_t i = 0; i < effects.size(); ++i) {
            if (chained(type, effects[i].id)) continue;
            AudioEngine::RenderStep step;
            step.type = type;
            step.index = i;
            step.itemId = effects[i].id;
            strip.fallbackSteps.push_back(step);
        }
    };
    appendUnchained(pluginrack::ChainItemType::Dynamics, strip.effects);
    appendUnchained(pluginrack::ChainItemType::Multiband, strip.multibandEffects);
    appendUnchained(pluginrack::ChainItemType::DeEsser, strip.deEsserEffects);
    appendUnchained(pluginrack::ChainItemType::Limiter, strip.limiterEffects);
    appendUnchained(pluginrack::ChainItemType::FeedbackSuppressor,
                    strip.feedbackSuppressorEffects);
    appendUnchained(pluginrack::ChainItemType::SpectralDenoiser,
                    strip.spectralDenoiserEffects);
    if (!strip.hasEqInChain) {
        AudioEngine::RenderStep step;
        step.type = pluginrack::ChainItemType::Eq;
        strip.fallbackSteps.push_back(step);
    }
}

// The unchained-effect pass, driven by the resolved fallbackSteps. Covers
// scenes saved before per-strip ordering existed; removal sets the effect's
// bypassed flag, so a removed effect stays silent here until picked again,
// which unbypasses it. Same order, same bypass rule and same taps as the old
// processNewMissing loop, with the tap written before and after the processor
// so an editor on such a scene still shows the signal. The EQ entry keeps the
// row path's tap order and stays gated by the bypass flag, so its taps and its
// process() remain coupled exactly as before.
template <typename StripType>
void runFallbackSteps(StripType& strip, const std::vector<AudioEngine::RenderStep>& steps,
                      float* left, float* right, int frames) noexcept {
    for (const auto& step : steps) {
        switch (step.type) {
        case pluginrack::ChainItemType::Eq:
            if (!strip.eqBypassed.get()) {
                strip.eqTap.writeTap(false, left, right, frames);
                strip.eq.process(left, right, frames);
                strip.eqTap.writeTap(true, left, right, frames);
            }
            break;
        case pluginrack::ChainItemType::Dynamics:
            if (step.index >= strip.effects.size()
                || step.index >= strip.effectProcessors.size()
                || strip.effects[step.index].bypassed)
                break;
            strip.dynamicsTap.writeTap(false, left, right, frames);
            strip.effectProcessors[step.index].process(left, right, frames);
            strip.dynamicsTap.writeTap(true, left, right, frames);
            break;
        case pluginrack::ChainItemType::Multiband:
            if (step.index >= strip.multibandEffects.size()
                || step.index >= strip.multibandProcessors.size()
                || strip.multibandEffects[step.index].bypassed)
                break;
            strip.multibandTap.writeTap(false, left, right, frames);
            strip.multibandProcessors[step.index].process(left, right, frames);
            strip.multibandTap.writeTap(true, left, right, frames);
            break;
        case pluginrack::ChainItemType::DeEsser:
            if (step.index >= strip.deEsserEffects.size()
                || step.index >= strip.deEsserProcessors.size()
                || strip.deEsserEffects[step.index].bypassed)
                break;
            strip.deEsserTap.writeTap(false, left, right, frames);
            strip.deEsserProcessors[step.index].process(left, right, frames);
            strip.deEsserTap.writeTap(true, left, right, frames);
            break;
        case pluginrack::ChainItemType::Limiter:
            if (step.index >= strip.limiterEffects.size()
                || step.index >= strip.limiterProcessors.size()
                || strip.limiterEffects[step.index].bypassed)
                break;
            strip.limiterTap.writeTap(false, left, right, frames);
            strip.limiterProcessors[step.index].process(left, right, frames);
            strip.limiterTap.writeTap(true, left, right, frames);
            break;
        case pluginrack::ChainItemType::FeedbackSuppressor:
            if (step.index >= strip.feedbackSuppressorEffects.size()
                || step.index >= strip.feedbackSuppressorProcessors.size()
                || strip.feedbackSuppressorEffects[step.index].bypassed)
                break;
            strip.feedbackTap.writeTap(false, left, right, frames);
            strip.feedbackSuppressorProcessors[step.index].process(left, right, frames);
            strip.feedbackTap.writeTap(true, left, right, frames);
            break;
        case pluginrack::ChainItemType::SpectralDenoiser:
            // The denoiser has no analyzer tap, matching the chain loop.
            if (step.index >= strip.spectralDenoiserEffects.size()
                || step.index >= strip.spectralDenoiserProcessors.size()
                || strip.spectralDenoiserEffects[step.index].bypassed)
                break;
            strip.spectralDenoiserProcessors[step.index].process(left, right, frames);
            break;
        case pluginrack::ChainItemType::Insert:
            // Insert rows are only ever chained, so a plan never puts one here.
            break;
        }
    }
}

// One built-in family re-prepared for the current clock. A size change
// rebuilds the vector so processors keep index alignment with effects (the
// chain looks processors up by effect index); a matched vector is prepared
// in place, which is what a sample-rate change needs: every processor's
// cached tables (limiter lookahead, multiband crossovers, denoiser FFT) are
// recomputed, and the following setEffect re-syncs the caches prepare()
// reads from the old effect copy. SpectralDenoiserProcessor::prepare keeps
// the learned profile: it lives in the effect struct and prepare()
// resamples it from profileSampleRate.
template <typename Processor, typename Effect>
void prepareProcessorFamily(std::vector<Processor>& processors,
                            const std::vector<Effect>& effects, double sampleRate) {
    if (processors.size() != effects.size()) {
        processors.clear();
        processors.reserve(effects.size());
        for (const auto& effect : effects) {
            Processor processor;
            processor.prepare(sampleRate);
            processor.setEffect(effect);
            processors.push_back(std::move(processor));
        }
        return;
    }
    for (std::size_t i = 0; i < processors.size(); ++i) {
        processors[i].prepare(sampleRate);
        processors[i].setEffect(effects[i]);
    }
}

// Every built-in family a strip owns, prepared together for a new clock.
// Called for each channel, output and bus from prepareToPlayImpl under
// stateLock. EqEngine::prepare only stores the rate and recomputes cached
// coefficients; bands set via setChannelEq and the delay set via
// setChannelEqDelay survive a rate change.
template <typename StripType>
void prepareStripProcessors(StripType& strip, double sampleRate) {
    strip.eq.prepare(sampleRate);
    prepareProcessorFamily(strip.effectProcessors, strip.effects, sampleRate);
    prepareProcessorFamily(strip.multibandProcessors, strip.multibandEffects, sampleRate);
    prepareProcessorFamily(strip.deEsserProcessors, strip.deEsserEffects, sampleRate);
    prepareProcessorFamily(strip.limiterProcessors, strip.limiterEffects, sampleRate);
    prepareProcessorFamily(strip.feedbackSuppressorProcessors, strip.feedbackSuppressorEffects, sampleRate);
    prepareProcessorFamily(strip.spectralDenoiserProcessors, strip.spectralDenoiserEffects, sampleRate);
}

// Picks the class a menu row stood for. Falls back to the bundle's first class
// when the id is unknown: a scene written before ids were kept, or a plugin
// updated to a new class id, must still load rather than fail.
juce::PluginDescription chooseDescription(const juce::Array<juce::PluginDescription>& descriptions,
                                          int pluginUniqueId) {
    if (pluginUniqueId != 0) {
        for (const auto& description : descriptions) {
            if (description.uniqueId == pluginUniqueId) return description;
        }
    }
    return descriptions.getReference(0);
}

// render() must not block indefinitely, but giving up on the first failed
// try turns an ordinary UI critical section (setters, EQ coefficient swaps)
// into a dropped block and a permanently lower FIFO fill. Yield a handful of
// times so short contention becomes a small render delay instead; a long
// message-thread stall still falls through to the silence path.
struct BriefTryLock {
    BriefTryLock(juce::CriticalSection& section, int maxAttempts)
        : lock(section), attempts(juce::jmax(1, maxAttempts)) {
        if (lock.tryEnter()) {
            held = true;
            return;
        }
        for (int attempt = 0; attempt < attempts && !held; ++attempt) {
            juce::Thread::yield();
            held = lock.tryEnter();
        }
    }
    ~BriefTryLock() {
        if (held) lock.exit();
    }
    bool isLocked() const noexcept { return held; }

    juce::CriticalSection& lock;
    int attempts = 1;
    bool held = false;
};

// Advisory capability cache. It never decides whether a device can open; it
// only remembers what a successful probe cost and negotiated.
juce::File deviceCacheFile() {
    return appPaths::file("devices/device-cache.json");
}
}

std::vector<pluginrack::ChainItem> AudioEngine::defaultChain() {
    // Channels load with no effects: the built-ins are added by picking them
    // from the plugin menu, which is also how a removed one comes back.
    return {};
}

void AudioEngine::rebuildRenderPlan(Channel& channel, double sampleRate) {
    buildRenderPlan(channel);
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    channel.cachedChainLatencySamples = chainLatencySamples(channel, rate);
    for (auto& point : channel.tapPoints) {
        point.cachedLatencySamples = tapLatencySamples(channel, point.tap, rate);
    }
}

void AudioEngine::rebuildRenderPlan(Output& output, double sampleRate) {
    buildRenderPlan(output);
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    output.cachedChainLatencySamples = chainLatencySamples(output, rate);
}

const juce::AudioBuffer<float>* AudioEngine::resolveSideChainSource(
    const InsertSlot& insert, bool& mono) const {
    mono = false;
    const bool preInsert =
        insert.sideChainTap == pluginrack::SideChainTap::PreInsert;
    if (insert.sideChainSource == pluginrack::SideChainSource::Channel) {
        for (const auto& channel : channels) {
            if (channel != nullptr && channel->id == insert.sideChainSourceId) {
                mono = channel->layout == pluginrack::ChannelLayout::Mono;
                return preInsert ? &channel->preInsertTap : &channel->postFaderTap;
            }
        }
    } else if (insert.sideChainSource == pluginrack::SideChainSource::Output) {
        for (const auto& output : outputs) {
            if (output != nullptr && output->id == insert.sideChainSourceId) {
                mono = output->layout == pluginrack::ChannelLayout::Mono;
                return preInsert ? &output->preInsertTap : &output->postFaderTap;
            }
        }
    } else if (insert.sideChainSource == pluginrack::SideChainSource::Bus) {
        for (const auto& bus : buses) {
            if (bus != nullptr && bus->id == insert.sideChainSourceId) {
                mono = bus->layout == pluginrack::ChannelLayout::Mono;
                return preInsert ? &bus->preInsertTap : &bus->postFaderTap;
            }
        }
    }
    return nullptr;
}

void AudioEngine::refreshAllSideChainCachesLocked() {
    const auto refresh = [this](auto& inserts) {
        for (auto& insert : inserts) {
            bool mono = false;
            insert->sideChainSourceBuffer = resolveSideChainSource(*insert, mono);
            insert->sideChainSourceMono = mono;
        }
    };
    for (auto& channel : channels) refresh(channel->inserts);
    for (auto& output : outputs) refresh(output->inserts);
    for (auto& bus : buses) refresh(bus->inserts);
}

AudioEngine::StereoMeterSample AudioEngine::measureStereoBlock(
    const juce::AudioBuffer<float>& buffer, int frames) noexcept {
    StereoMeterSample sample;
    const int numSamples = juce::jmin(frames, buffer.getNumSamples());
    if (numSamples <= 0 || buffer.getNumChannels() < 2) return sample;

    // Same arithmetic as the AudioBuffer calls this replaces:
    // getMagnitude() is max(max, -min) of the samples (findMinAndMax is exact
    // for finite values), and getRMSLevel() accumulates sample * sample as a
    // float product into a double sum and returns a float. Keeping that shape
    // makes the published meters bit-identical.
    const auto measure = [numSamples](const float* data, float& peak, float& rms) {
        float lo = data[0];
        float hi = data[0];
        double sum = 0.0;
        for (int i = 0; i < numSamples; ++i) {
            const float value = data[i];
            lo = value < lo ? value : lo;
            hi = value > hi ? value : hi;
            sum += value * value;
        }
        peak = juce::jmax(hi, -lo);
        rms = static_cast<float>(std::sqrt(sum / numSamples));
    };

    measure(buffer.getReadPointer(0), sample.peakL, sample.rmsL);
    measure(buffer.getReadPointer(1), sample.peakR, sample.rmsR);
    return sample;
}

// Free-running fallback clock. When the hardware clock disappears and no
// other output can take over, this timer keeps calling render() at the last
// live rate so the remaining output FIFOs drain gracefully instead of
// snapping. It is the only render driver while it runs; the device callback
// it replaced is gone.
class AudioEngine::InternalClock final : public juce::HighResolutionTimer {
public:
    explicit InternalClock(AudioEngine& engine) : engine_(engine) {}

    void configure(int blockFrames, double sampleRate) {
        blockFrames_ = juce::jmax(1, blockFrames);
        const double intervalMs =
            1000.0 * static_cast<double>(blockFrames_) / juce::jmax(1.0, sampleRate);
        startTimer(juce::jmax(1, static_cast<int>(std::lround(intervalMs))));
    }

    void hiResTimerCallback() override {
        // The fallback thread is a render thread too: keep the same FTZ/DAZ
        // guarantee as the device callbacks.
        juce::ScopedNoDenormals noDenormals;
        engine_.render(blockFrames_);
    }

private:
    AudioEngine& engine_;
    int blockFrames_ = 256;
};

AudioEngine::AudioEngine() {
    // Never inherit a cap from the launching shell: only Settings may set one.
    setWasapiProbeCapEnvironment(false, 8);
    for (auto& slot : renderOutputs_) {
        slot.store(nullptr, std::memory_order_relaxed);
    }
    const auto cacheFile = deviceCacheFile();
    if (cacheFile.existsAsFile()) {
        std::vector<std::string> errors;
        if (!deviceCache_.loadFromFile(cacheFile.getFullPathName().toStdString(), &errors)) {
            for (const auto& error : errors) {
                juce::Logger::writeToLog("device cache: " + juce::String(error));
            }
        }
    }
    for (int i = 0; i < 3; ++i) addOutput();
    addChannel();
}

AudioEngine::~AudioEngine() {
    abortSceneRestore();
    prepareAliveToken_->store(false, std::memory_order_release);
    stopInternalClock();
    stopDeviceOpenThread();
}

void AudioEngine::openDefaultDevicesAsync() {
    {
        const juce::ScopedLock lock(stateLock);
        if (!outputs.empty()) outputs.front()->outputDeviceName = systemDefaultDeviceName();
        if (!channels.empty()) channels.front()->inputDeviceName = systemDefaultDeviceName();
    }
    openStripsDevicesAsync();
}

void AudioEngine::prepareToPlay(int maxBlockFrames) {
    prepareToPlayImpl(maxBlockFrames, true);
}

void AudioEngine::prepareToPlayImpl(int maxBlockFrames, bool openInputs) {
    // Collect the plugins under the lock but prepare them outside it: VST3
    // prepareToPlay can take tens of milliseconds (and allocate), and holding
    // stateLock across it starves the audio callback for the whole device
    // reopen. The audio thread only skips a plugin whose own try-lock is busy
    // instead of dropping every output block.
    std::vector<PluginHost*> hosts;
    const int newBlock = juce::jmax(1, maxBlockFrames);
    double prepareRate = 48000.0;
    {
        const juce::ScopedLock lock(stateLock);
        // Lower the render ceiling before touching the plugins: a shrinking
        // block must not stay renderable at the old size while they are being
        // re-prepared for the new one.
        renderBlockFrames_ = juce::jmin(renderBlockFrames_, newBlock);
        maxBlockFrames_ = newBlock;
        tapScratch.setSize(2, maxBlockFrames_, false, true, true);
        tapScratch.clear();
        currentBufferSize = newBlock;
        prepareRate = currentSampleRate > 0.0 ? currentSampleRate : 48000.0;
        // The clock has negotiated. Settle every capture's read delay on the
        // real block now, so the value cannot depend on which device finished
        // probing first at load.
        for (auto& entry : inputCaptures_) {
            if (entry.second.capture != nullptr)
                entry.second.capture->applyRenderBlockFrames(maxBlockFrames_);
        }
        for (auto& channel : channels) {
            channel->buffer.setSize(2, maxBlockFrames_, false, true, true);
            channel->buffer.clear();
            channel->preInsertTap.setSize(2, maxBlockFrames_, false, true, true);
            channel->preInsertTap.clear();
            channel->postFaderTap.setSize(2, maxBlockFrames_, false, true, true);
            channel->postFaderTap.clear();
            channel->outputTapRing.prepare(pluginrack::kMaxTapDelaySamples + maxBlockFrames_ + 2);
            for (auto& point : channel->tapPoints) {
                point.ring.prepare(pluginrack::kMaxTapDelaySamples + maxBlockFrames_ + 2);
            }
            prepareStripProcessors(*channel, prepareRate);
            for (auto& insert : channel->inserts) {
                insert->sideChainBuffer.setSize(2, maxBlockFrames_, false, true, true);
                insert->sideChainBuffer.clear();
                hosts.push_back(&insert->pluginHost);
            }
            meters.observeChannel(channel->id, 0.0f);
        }
        for (auto& output : outputs) {
            output->buffer.setSize(2, maxBlockFrames_, false, true, true);
            output->buffer.clear();
            output->preInsertTap.setSize(2, maxBlockFrames_, false, true, true);
            output->preInsertTap.clear();
            output->postFaderTap.setSize(2, maxBlockFrames_, false, true, true);
            output->postFaderTap.clear();
            prepareStripProcessors(*output, prepareRate);
            for (auto& insert : output->inserts) {
                insert->sideChainBuffer.setSize(2, maxBlockFrames_, false, true, true);
                insert->sideChainBuffer.clear();
                hosts.push_back(&insert->pluginHost);
            }
            meters.observeOutput(output->id, 0.0f);
        }
        for (auto& bus : buses) {
            bus->buffer.setSize(2, maxBlockFrames_, false, true, true);
            bus->buffer.clear();
            bus->preInsertTap.setSize(2, maxBlockFrames_, false, true, true);
            bus->preInsertTap.clear();
            bus->postFaderTap.setSize(2, maxBlockFrames_, false, true, true);
            bus->postFaderTap.clear();
            bus->compensation.prepare(pluginrack::kMaxTapDelaySamples + maxBlockFrames_ + 2);
            prepareStripProcessors(*bus, prepareRate);
            for (auto& insert : bus->inserts) {
                insert->sideChainBuffer.setSize(2, maxBlockFrames_, false, true, true);
                insert->sideChainBuffer.clear();
                hosts.push_back(&insert->pluginHost);
            }
            meters.observeBus(bus->id, 0.0f);
        }
        // New rate: every plan's cached latency (insert reports, EQ delay,
        // limiter look-ahead) and every side-chain source is re-resolved while
        // the strips are already sized for the new block.
        for (auto& channel : channels) rebuildRenderPlan(*channel, prepareRate);
        for (auto& output : outputs) rebuildRenderPlan(*output, prepareRate);
        for (auto& bus : buses) rebuildRenderPlan(*bus, prepareRate);
        refreshAllSideChainCachesLocked();
    }
    // Prepare before raising the ceiling: until every host has allocated for
    // the new block the audio thread must keep rendering at the old, smaller
    // size. Handing a plugin more frames than it prepared for corrupts its
    // internal buffers and faults inside its own DSP.
    for (auto* host : hosts) {
        host->prepareToPlay(prepareRate, newBlock);
    }
    {
        const juce::ScopedLock lock(stateLock);
        renderBlockFrames_ = newBlock;
    }
    // The render block size may have changed (device reload); reopen any
    // capture whose ring was sized for a smaller block. Unchanged captures
    // are reused without touching the device.
    if (openInputs) prepareInputs();
}

void AudioEngine::pushSilenceToOutputs(int frames) {
    if (frames <= 0) return;
    // Lock-free on purpose: this is the path taken when render() lost the
    // state lock, which means the message thread is holding it. Trying to
    // acquire it here again would always fail and leave the output FIFO to
    // drain. Retired playbacks stay alive for the whole engine lifetime, and
    // a closed one ignores the push, so the raw pointers are safe.
    for (auto& slot : renderOutputs_) {
        if (auto* playback = slot.load(std::memory_order_acquire)) {
            playback->pushSilence(frames);
        }
    }
}

void AudioEngine::pushSilenceToOutputsLocked(int frames) {
    // The locked variant walks the live outputs so a just-removed output is
    // not fed. pushSilence() handles the declick.
    for (auto& output : outputs) {
        if (output->output) {
            output->output->pushSilence(frames);
        }
    }
}

void AudioEngine::publishRenderOutputsLocked() {
    std::size_t slot = 0;
    for (auto& output : outputs) {
        if (slot >= renderOutputs_.size()) break;
        renderOutputs_[slot].store(output->output.get(), std::memory_order_release);
        ++slot;
    }
    for (; slot < renderOutputs_.size(); ++slot) {
        renderOutputs_[slot].store(nullptr, std::memory_order_release);
    }
}

// AnalyzerTap ring writes stay allocation-free: the buffers are sized at
// strip construction (AnalyzerTap data members) and never resized, so the
// audio thread only memcpy-copies into them and the UI thread only
// memcpy-copies out. Mode/cursor atomics make the unlocked UI snapshot safe
// to race (worst case: one visually torn block).
void AudioEngine::AnalyzerTap::writeTap(bool postEq, const float* inL, const float* inR,
                                        int frames) noexcept {
    if (mode.get() == kOff) return;
    // Both rings stay current while the tap is on: the EQ reads Post and the
    // dynamics panel reads Pre, and they may be open at different times.
    const int total = pre.getNumSamples(); // == kSamples; rings are fixed
    if (total <= 0 || frames <= 0) return;
    const int skip = juce::jmax(0, frames - total); // ring keeps the newest window
    const int n = frames - skip;
    juce::AudioBuffer<float>& ring = postEq ? post : pre;
    juce::Atomic<std::uint32_t>& cursor = postEq ? postWrite : preWrite;
    const int pos = static_cast<int>(cursor.get() % static_cast<std::uint32_t>(total));
    const int first = juce::jmin(n, total - pos);
    const float* srcL = inL + skip;
    const float* srcR = inR + skip;
    float* dstL = ring.getWritePointer(0);
    float* dstR = ring.getWritePointer(1);
    std::memcpy(dstL + pos, srcL, sizeof(float) * static_cast<std::size_t>(first));
    std::memcpy(dstR + pos, srcR, sizeof(float) * static_cast<std::size_t>(first));
    if (n > first) { // wrapped segment
        std::memcpy(dstL, srcL + first, sizeof(float) * static_cast<std::size_t>(n - first));
        std::memcpy(dstR, srcR + first, sizeof(float) * static_cast<std::size_t>(n - first));
    }
    cursor.set(static_cast<std::uint32_t>((pos + n) % total));
}

void AudioEngine::AnalyzerTap::snapshot(bool postEq, float* destL, float* destR,
                                        int maxFrames) const noexcept {
    const juce::AudioBuffer<float>& ring = postEq ? post : pre;
    const std::uint32_t cursor = (postEq ? postWrite : preWrite).get();
    const int total = ring.getNumSamples();
    const int n = juce::jmin(maxFrames, total);
    if (n <= 0) return;
    // Newest sample sits at cursor - 1; read the newest window oldest-first
    // so the FFT sees a contiguous ramp-up ending at the freshest frame.
    const int pos = static_cast<int>(cursor % static_cast<std::uint32_t>(total));
    const int start = (pos + total - n) % total;
    const int first = juce::jmin(n, total - start);
    std::memcpy(destL, ring.getReadPointer(0) + start, sizeof(float) * static_cast<std::size_t>(first));
    std::memcpy(destR, ring.getReadPointer(1) + start, sizeof(float) * static_cast<std::size_t>(first));
    if (n > first) {
        std::memcpy(destL + first, ring.getReadPointer(0), sizeof(float) * static_cast<std::size_t>(n - first));
        std::memcpy(destR + first, ring.getReadPointer(1), sizeof(float) * static_cast<std::size_t>(n - first));
    }
}

void AudioEngine::render(int frames) {
    if (frames <= 0) return;

    const auto renderStart = std::chrono::steady_clock::now();
    const auto elapsedNanos = [&renderStart] {
        const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - renderStart)
                               .count();
        return static_cast<std::uint32_t>(juce::jlimit<std::int64_t>(0, 4'000'000'000, nanos));
    };

    // Yield proportionally to the block budget: a 32-sample block cannot
    // afford as many scheduler yields as a 512-sample one.
    const BriefTryLock lock(stateLock, juce::jlimit(1, 16, frames / 16));
    if (!lock.isLocked()) {
        renderMisses_.fetch_add(1, std::memory_order_relaxed);
        // Keep every published output FIFO at its target fill. Previously this
        // path tried to lock again and silently did nothing, so a UI-thread
        // stall drained the FIFO, tripped the low-watermark counter, and then
        // repeated the last block (an audible stutter). A clean faded dropout
        // is the correct fallback; the next real block fades back in.
        pushSilenceToOutputs(frames);
        recordRenderTelemetry(frames, elapsedNanos(),
                              pluginrack::AudioTelemetry::flagRenderMiss, 0);
        return;
    }

    // Oversized block: silence it, count once, skip processing. The ceiling is
    // renderBlockFrames_, which only rises once the plugins are prepared for
    // the new device block.
    if (renderBlockFrames_ > 0 && frames > renderBlockFrames_) {
        pushSilenceToOutputsLocked(frames);
        recordRenderTelemetry(frames, elapsedNanos(),
                              pluginrack::AudioTelemetry::flagOversized, 0);
        return;
    }

    const std::uint64_t epoch = ++renderEpoch_;
    // Deadline-aware rendering: plan equal slices up front and check a
    // monotonic deadline before each one. A plugin that overruns now costs at
    // most one slice of audio; the rest of the block is silenced and flagged
    // instead of surrendering the whole driver callback.
    const bool slicingEnabled = slicedRender_.load(std::memory_order_relaxed);
    const bool scrubEnabled = nonFiniteScrub_.load(std::memory_order_relaxed);
    const pluginrack::SliceLayout slices =
        pluginrack::planSlices(frames, slicingEnabled ? 4 : 1);
    const double budgetMs = slicingEnabled
                                ? pluginrack::deadlineBudgetMs(frames, currentSampleRate, 0.8)
                                : 0.0;
    bool partial = false;
    int scrubbedThisBlock = 0;
    const auto budgetSpent = [&] {
        return pluginrack::deadlineExceededMs(
            static_cast<double>(elapsedNanos()) / 1.0e6, budgetMs);
    };
    const auto scrubRange = [&](juce::AudioBuffer<float>& buffer, int start, int count) {
        if (!scrubEnabled) return;
        scrubbedThisBlock += pluginrack::scrubNonFinite(
            buffer.getWritePointer(0) + start, buffer.getWritePointer(1) + start, count);
    };

    // buffers are preallocated in prepareToPlay; clear used range only.
    for (auto& channel : channels) {
        channel->buffer.clear(0, frames);
        if (channel->duplexInput.get()) {
            pullDuplexInput(*channel, frames);
        } else if (channel->input) {
            channel->input->pull(channel->buffer, frames, channel->inputChannels, epoch);
        }
    }
    for (auto& output : outputs) {
        output->buffer.clear(0, frames);
    }

    // Bus sends are summed after the channels so a tap can read this block's
    // signal at any chain point; the sum is also the bus's pre-insert key,
    // which lets a bus insert side-chain off its own bus.
    for (auto& bus : buses) {
        bus->buffer.clear(0, frames);
    }

    // Resolve one insert's side-chain source into its preallocated
    // sideChainBuffer slice. Called once per insert per render slice, before
    // that insert runs, so the source buffers are all stable snapshots.
    const auto fillSideChain = [&](InsertSlot& insert, int start, int count) {
        if (insert.sideChainBuffer.getNumSamples() < start + count) return;
        insert.sideChainBuffer.clear(start, count);
        if (insert.sideChainSource == pluginrack::SideChainSource::None
            || insert.sideChainSourceId == 0) {
            insert.sideChainKeyPeak.set(insert.sideChainKeyPeak.get() * 0.85f);
            return;
        }
        const juce::AudioBuffer<float>* source = insert.sideChainSourceBuffer;
        bool mono = insert.sideChainSourceMono;
        if (source == nullptr) {
            // The cache is refreshed under stateLock on every routing or strip
            // change, so a miss here means the plan was not rebuilt yet (or the
            // source was just removed). Resolve once and keep the answer: this
            // is the only lookup left in the render path.
            mono = false;
            source = resolveSideChainSource(insert, mono);
            insert.sideChainSourceBuffer = source;
            insert.sideChainSourceMono = mono;
        }
        if (source == nullptr || source->getNumSamples() < start + count) return;
        const float gain = decibelsToGain(insert.sideChainGainDb.get());
        insert.sideChainBuffer.copyFrom(0, start, *source, 0, start, count);
        // A mono source feeds both side-chain legs; that is the documented
        // behaviour for mono strips and mono aux sources.
        insert.sideChainBuffer.copyFrom(1, start, *source, mono ? 0 : 1, start, count);
        if (gain != 1.0f) {
            insert.sideChainBuffer.applyGain(0, start, count, gain);
            insert.sideChainBuffer.applyGain(1, start, count, gain);
        }
        const float peak = insert.sideChainBuffer.getMagnitude(0, start, count);
        const float previous = insert.sideChainKeyPeak.get();
        insert.sideChainKeyPeak.set(peak > previous ? peak : previous * 0.85f);
    };

    meters.advance(frames, currentSampleRate);
    bool anySolo = false;
    for (const auto& channel : channels) {
        anySolo = anySolo || channel->soloed.get();
    }

    for (auto& channel : channels) {
        if (partial) {
            // The deadline was already spent by an earlier strip: write
            // silence for the rest of this block, without touching stateful
            // processors (which would only widen the overrun).
            channel->buffer.clear(0, frames);
        } else {
            for (int slice = 0; slice < slices.count; ++slice) {
                const int start = slices.offsetAt(slice);
                const int count = slices.framesAt(slice);
                if (budgetSpent()) {
                    channel->buffer.clear(start, frames - start);
                    partial = true;
                    break;
                }
                juce::AudioBuffer<float> block(
                    channel->buffer.getArrayOfWritePointers(), 2, start, count);
                // One tap per built-in, written right after that plugin runs,
                // so each RTA sees *that plugin's* output — not the strip end,
                // and not a later plugin's effect. The order below only
                // decides WHERE each snapshot falls; the chain loop decides
                // the signal flow.
                const float trim = decibelsToGain(channel->trimDb.get());
                channel->buffer.applyGain(0, start, count, trim);
                channel->buffer.applyGain(1, start, count, trim);
                if (channel->preInsertTap.getNumSamples() >= start + count) {
                    channel->preInsertTap.copyFrom(0, start, channel->buffer, 0, start, count);
                    channel->preInsertTap.copyFrom(1, start, channel->buffer, 1, start, count);
                }
                if (auto* point = findTapPoint(
                        *channel, pluginrack::Tap{pluginrack::Tap::Kind::Input, 0})) {
                    point->ring.push(block.getReadPointer(0), block.getReadPointer(1), count);
                }
                channel->analyzerTap.writeTap(false, block.getReadPointer(0), block.getReadPointer(1), count);
                for (const auto& step : channel->renderSteps) {
                    if (step.type == pluginrack::ChainItemType::Eq) {
                        // EQWindow's Pre mode reads the input side, so the pre
                        // ring is written before process() and the post ring
                        // after - the same rule as every other built-in tap.
                        channel->eqTap.writeTap(false, block.getReadPointer(0), block.getReadPointer(1), count);
                        if (!channel->eqBypassed.get())
                            channel->eq.process(block.getWritePointer(0), block.getWritePointer(1), count);
                        channel->eqTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
                    } else if (step.type == pluginrack::ChainItemType::Dynamics) {
                        // Detector taps: the pre ring is written immediately
                        // before the effect runs so the panel's dot and GR
                        // meter model the true input level; the post ring
                        // keeps the output for anything that wants it.
                        channel->dynamicsTap.writeTap(false, block.getReadPointer(0), block.getReadPointer(1), count);
                        if (step.index < channel->effectProcessors.size())
                            channel->effectProcessors[step.index].process(
                                block.getWritePointer(0), block.getWritePointer(1), count);
                        channel->dynamicsTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
                    } else if (step.type == pluginrack::ChainItemType::Multiband) {
                        channel->multibandTap.writeTap(false, block.getReadPointer(0), block.getReadPointer(1), count);
                        if (step.index < channel->multibandProcessors.size()) channel->multibandProcessors[step.index].process(block.getWritePointer(0), block.getWritePointer(1), count);
                        channel->multibandTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
                    } else if (step.type == pluginrack::ChainItemType::DeEsser) {
                        channel->deEsserTap.writeTap(false, block.getReadPointer(0), block.getReadPointer(1), count);
                        if (step.index < channel->deEsserProcessors.size()) channel->deEsserProcessors[step.index].process(block.getWritePointer(0), block.getWritePointer(1), count);
                        channel->deEsserTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
                    } else if (step.type == pluginrack::ChainItemType::Limiter) {
                        // LimiterWindow derives its GR trace from inputDb minus
                        // outputDb across both rings: without the pre write the
                        // input side is stale and GR reads zero forever.
                        channel->limiterTap.writeTap(false, block.getReadPointer(0), block.getReadPointer(1), count);
                        if (step.index < channel->limiterProcessors.size()) channel->limiterProcessors[step.index].process(block.getWritePointer(0), block.getWritePointer(1), count);
                        channel->limiterTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
                    } else if (step.type == pluginrack::ChainItemType::FeedbackSuppressor) {
                        // The feedback editor's IN/OUT meters must show this
                        // plugin, not the whole strip. Snapshot immediately
                        // before and after the suppressor runs.
                        channel->feedbackTap.writeTap(false, block.getReadPointer(0), block.getReadPointer(1), count);
                        if (step.index < channel->feedbackSuppressorProcessors.size()) channel->feedbackSuppressorProcessors[step.index].process(block.getWritePointer(0), block.getWritePointer(1), count);
                        channel->feedbackTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
                    } else if (step.type == pluginrack::ChainItemType::SpectralDenoiser) {
                        if (step.index < channel->spectralDenoiserProcessors.size()) channel->spectralDenoiserProcessors[step.index].process(block.getWritePointer(0), block.getWritePointer(1), count);
                    } else if (step.type == pluginrack::ChainItemType::Insert) {
                        auto* insert = step.insert;
                        if (insert != nullptr) {
                            fillSideChain(*insert, start, count);
                            crash_context::setCurrentRender(insert->crashLabel);
                            insert->pluginHost.processBlock(
                                block, insert->bypassed.get(),
                                insert->sideChainBuffer.getReadPointer(0) + start,
                                insert->sideChainBuffer.getReadPointer(1) + start, count);
                            crash_context::clearCurrentRender();
                            // Fault containment: mirror the child's death/wedge
                            // into the slot so the UI and reload path can see it.
                            const auto status = insert->pluginHost.childHostStatus();
                            if (status.enabled && status.failed) {
                                insert->failed = true;
                                insert->failExitCode = status.exitCode;
                                if (!insert->faultRecorded.get()) {
                                    insert->faultRecorded = true;
                                    // faults.json is disk I/O. Never do it on
                                    // the audio thread, still less under
                                    // stateLock: one dead child would stall
                                    // every strip's render.
                                    const juce::String faultPath = insert->pluginPath;
                                    const int faultExit = status.exitCode;
                                    const auto faultId = insert->id;
                                    juce::MessageManager::callAsync([faultPath, faultExit, faultId] {
                                        PluginFaultGuard::record(faultPath, faultExit, faultId);
                                    });
                                }
                            }
                        }
                    }
                    // Every insert boundary is a quarantine point: a NaN that
                    // escaped one plugin must not reach the next one's state or
                    // the mix bus.
                    scrubRange(channel->buffer, start, count);
                    // The plan put the Item tap on the first chain row with its
                    // id (Scene.h firstChainItemIndex), the same row
                    // tapLatencySamples measures. Built-in families mint ids
                    // independently, so a later row of another family can share
                    // the id; pushing there too would leave the ring holding
                    // that row's signal while the latency came from the first.
                    if (step.tapOwned) {
                        if (auto* point = findTapPoint(
                                *channel, pluginrack::Tap{pluginrack::Tap::Kind::Item, step.itemId})) {
                            point->ring.push(block.getReadPointer(0), block.getReadPointer(1), count);
                        }
                    }
                }
                // Older scenes can have effects without their dynamics chain
                // entries. Keep those effects live instead of showing a working
                // but inaudible UI; the unchained EQ runs last in the same pass.
                runFallbackSteps(*channel, channel->fallbackSteps,
                                 block.getWritePointer(0), block.getWritePointer(1), count);
                channel->analyzerTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
                scrubRange(channel->buffer, start, count);
            }
        }

        const bool silent = channel->muted.get() || (anySolo && !channel->soloed.get());
        const float gain = silent ? 0.0f : decibelsToGain(channel->gainDb.get());
        const float theta = (channel->pan.get() + 1.0f) * juce::MathConstants<float>::pi / 4.0f;
        channel->buffer.applyGain(0, 0, frames, gain * std::cos(theta));
        channel->buffer.applyGain(1, 0, frames, gain * std::sin(theta));
        if (channel->postFaderTap.getNumSamples() >= frames) {
            channel->postFaderTap.copyFrom(0, 0, channel->buffer, 0, 0, frames);
            channel->postFaderTap.copyFrom(1, 0, channel->buffer, 1, 0, frames);
            channel->outputTapRing.push(channel->postFaderTap.getReadPointer(0),
                                        channel->postFaderTap.getReadPointer(1), frames);
        }

        // One read pass for both peaks and both RMS levels. The numbers are
        // the ones the four AudioBuffer queries produced.
        const auto meter = measureStereoBlock(channel->buffer, frames);
        meters.observeChannelRealtime(channel->id, juce::jmax(meter.peakL, meter.peakR));
        channel->peak = meters.channelPeak(channel->id);
        channel->peakL = meter.peakL;
        channel->peakR = meter.peakR;
        channel->rmsL = meter.rmsL;
        channel->rmsR = meter.rmsR;
    }


    // Tap compensation. Every path into an output is measured against the
    // longest one: channel output plus a bus chain. A send reads its tap
    // delayed by the chain latency after the tap, a route adds the bus
    // overhead, and a bus route reads back by the difference to the longest
    // bus. With compensation off every send reads the channel output.
    const bool compensate = latencyCompensation_;
    int maxBusLatency = 0;
    if (compensate) {
        for (const auto& route : busRoutes) {
            const auto bus = std::find_if(buses.begin(), buses.end(),
                                          [&route](const auto& b) { return b != nullptr && b->id == route->busId; });
            if (bus != buses.end() && *bus != nullptr) {
                // Cached by rebuildRenderPlan: the chain was already resolved
                // for rendering, so the block does not walk it again.
                maxBusLatency = juce::jmax(maxBusLatency, (*bus)->cachedChainLatencySamples);
            }
        }
        maxBusLatency = juce::jmin(maxBusLatency, pluginrack::kMaxTapDelaySamples);
    }

    const auto tapSource = [&](Channel& channel, const pluginrack::Tap& tap,
                               int delay) -> const juce::AudioBuffer<float>* {
        if (tap.kind == pluginrack::Tap::Kind::Output) {
            if (delay <= 0) return &channel.postFaderTap;
            channel.outputTapRing.read(delay, tapScratch.getWritePointer(0),
                                       tapScratch.getWritePointer(1), frames);
            return &tapScratch;
        }
        if (const auto* point = findTapPoint(channel, tap)) {
            point->ring.read(delay, tapScratch.getWritePointer(0),
                             tapScratch.getWritePointer(1), frames);
            return &tapScratch;
        }
        return &channel.postFaderTap;
    };

    // How much of the strip's chain latency sits at or before this tap. The
    // plan cached each TapPoint's figure, so neither send nor route re-walks
    // the chain: 0 at the input, the point's cached value for an Item tap, and
    // the strip total for the output tap (an Item that left the chain falls
    // back to the post-fader signal, whose delay is 0).
    const auto tapLatencyFor = [this](const Channel& channel, const pluginrack::Tap& tap) {
        if (tap.kind == pluginrack::Tap::Kind::Input) return 0;
        if (tap.kind == pluginrack::Tap::Kind::Item) {
            const auto* point = findTapPoint(channel, tap);
            return point != nullptr ? point->cachedLatencySamples : 0;
        }
        return channel.cachedChainLatencySamples;
    };

    for (const auto& send : busSends) {
        const auto channel = std::find_if(channels.begin(), channels.end(),
                                          [&send](const auto& c) { return c != nullptr && c->id == send->channelId; });
        const auto bus = std::find_if(buses.begin(), buses.end(),
                                      [&send](const auto& b) { return b != nullptr && b->id == send->busId; });
        if (channel == channels.end() || bus == buses.end()) continue;
        auto& sourceChannel = **channel;
        if (sourceChannel.postFaderTap.getNumSamples() < frames) continue;
        const float sendGainL = decibelsToGain(send->sendGainDbL.get());
        const float sendGainR = decibelsToGain(send->sendGainDbR.get());
        if (sendGainL == 0.0f && sendGainR == 0.0f) continue;
        int delay = 0;
        if (compensate) {
            delay = juce::jmax(0, sourceChannel.cachedChainLatencySamples
                                       - tapLatencyFor(sourceChannel, send->tap));
        }
        const auto* source = compensate ? tapSource(sourceChannel, send->tap, delay)
                                        : &sourceChannel.postFaderTap;
        if (source->getNumSamples() < frames) continue;
        const int rightTap = sourceChannel.layout == pluginrack::ChannelLayout::Mono ? 0 : 1;
        if (sendGainL != 0.0f) (*bus)->buffer.addFrom(0, 0, *source, 0, 0, frames, sendGainL);
        if (sendGainR != 0.0f) (*bus)->buffer.addFrom(1, 0, *source, rightTap, 0, frames, sendGainR);
    }
    for (auto& bus : buses) {
        if (bus->preInsertTap.getNumSamples() >= frames) {
            bus->preInsertTap.copyFrom(0, 0, bus->buffer, 0, 0, frames);
            bus->preInsertTap.copyFrom(1, 0, bus->buffer, 1, 0, frames);
        }
    }

    // One strip chain, shared by outputs and buses so the two cannot drift.
    const auto processChain = [&](Output* strip, int start, int count) {
        juce::AudioBuffer<float> block(
            strip->buffer.getArrayOfWritePointers(), 2, start, count);
        // Same per-plugin taps as Channel: each built-in's RTA reads
        // its own output, so the EQ's curve never leaks into a later
        // plugin's RTA and each preview is the plugin's own signal.
        if (strip->preInsertTap.getNumSamples() >= start + count) {
            strip->preInsertTap.copyFrom(0, start, strip->buffer, 0, start, count);
            strip->preInsertTap.copyFrom(1, start, strip->buffer, 1, start, count);
        }
        strip->analyzerTap.writeTap(false, block.getReadPointer(0), block.getReadPointer(1), count);
        for (const auto& step : strip->renderSteps) {
            if (step.type == pluginrack::ChainItemType::Eq) {
                strip->eqTap.writeTap(false, block.getReadPointer(0), block.getReadPointer(1), count);
                if (!strip->eqBypassed.get())
                    strip->eq.process(block.getWritePointer(0), block.getWritePointer(1), count);
                strip->eqTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
            } else if (step.type == pluginrack::ChainItemType::Dynamics) {
                // Detector taps: pre ring written immediately before the
                // effect (panel dot/GR model the true input), post ring
                // after. Same rule as the channel chain above.
                strip->dynamicsTap.writeTap(false, block.getReadPointer(0), block.getReadPointer(1), count);
                if (step.index < strip->effectProcessors.size())
                    strip->effectProcessors[step.index].process(
                        block.getWritePointer(0), block.getWritePointer(1), count);
                strip->dynamicsTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
            } else if (step.type == pluginrack::ChainItemType::Multiband) {
                strip->multibandTap.writeTap(false, block.getReadPointer(0), block.getReadPointer(1), count);
                if (step.index < strip->multibandProcessors.size()) strip->multibandProcessors[step.index].process(block.getWritePointer(0), block.getWritePointer(1), count);
                strip->multibandTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
            } else if (step.type == pluginrack::ChainItemType::DeEsser) {
                strip->deEsserTap.writeTap(false, block.getReadPointer(0), block.getReadPointer(1), count);
                if (step.index < strip->deEsserProcessors.size()) strip->deEsserProcessors[step.index].process(block.getWritePointer(0), block.getWritePointer(1), count);
                strip->deEsserTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
            } else if (step.type == pluginrack::ChainItemType::Limiter) {
                strip->limiterTap.writeTap(false, block.getReadPointer(0), block.getReadPointer(1), count);
                if (step.index < strip->limiterProcessors.size()) strip->limiterProcessors[step.index].process(block.getWritePointer(0), block.getWritePointer(1), count);
                strip->limiterTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
            } else if (step.type == pluginrack::ChainItemType::FeedbackSuppressor) {
                strip->feedbackTap.writeTap(false, block.getReadPointer(0), block.getReadPointer(1), count);
                if (step.index < strip->feedbackSuppressorProcessors.size()) strip->feedbackSuppressorProcessors[step.index].process(block.getWritePointer(0), block.getWritePointer(1), count);
                strip->feedbackTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
            } else if (step.type == pluginrack::ChainItemType::SpectralDenoiser) {
                if (step.index < strip->spectralDenoiserProcessors.size()) strip->spectralDenoiserProcessors[step.index].process(block.getWritePointer(0), block.getWritePointer(1), count);
            } else if (step.type == pluginrack::ChainItemType::Insert) {
                auto* insert = step.insert;
                if (insert != nullptr) {
                    fillSideChain(*insert, start, count);
                    crash_context::setCurrentRender(insert->crashLabel);
                    insert->pluginHost.processBlock(
                        block, insert->bypassed.get(),
                        insert->sideChainBuffer.getReadPointer(0) + start,
                        insert->sideChainBuffer.getReadPointer(1) + start, count);
                    crash_context::clearCurrentRender();
                    const auto status = insert->pluginHost.childHostStatus();
                    if (status.enabled && status.failed) {
                        insert->failed = true;
                        insert->failExitCode = status.exitCode;
                        if (!insert->faultRecorded.get()) {
                            insert->faultRecorded = true;
                            // faults.json is disk I/O. Never do it on
                            // the audio thread, still less under
                            // stateLock: one dead child would stall
                            // every strip's render.
                            const juce::String faultPath = insert->pluginPath;
                            const int faultExit = status.exitCode;
                            const auto faultId = insert->id;
                            juce::MessageManager::callAsync([faultPath, faultExit, faultId] {
                                PluginFaultGuard::record(faultPath, faultExit, faultId);
                            });
                        }
                    }
                }
            }
            scrubRange(strip->buffer, start, count);
        }
        // Older scenes can have effects without their dynamics chain entries.
        // Same plan-driven fallback as the channel pass, with the unchained EQ
        // last so both engines apply and bill it at the same position.
        runFallbackSteps(*strip, strip->fallbackSteps,
                         block.getWritePointer(0), block.getWritePointer(1), count);
        strip->analyzerTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
        scrubRange(strip->buffer, start, count);
    };


    // Buses run after the channels so their chain input is settled. The bus's
    // own post-fader tap is seeded with this block's send sum, so a bus insert
    // keyed off its own bus reads the current block; every other bus key reads
    // the previous block, the same one-block rule the old aux bus had.
    for (auto& bus : buses) {
        // Trim the send sum before the chain, like a channel trim on its
        // incoming signal. Applied here so preInsertTap sees the trimmed input.
        const float busTrim = decibelsToGain(bus->trimDb.get());
        if (busTrim != 1.0f) {
            bus->buffer.applyGain(0, 0, frames, busTrim);
            bus->buffer.applyGain(1, 0, frames, busTrim);
        }
        if (bus->postFaderTap.getNumSamples() >= frames) {
            bus->postFaderTap.copyFrom(0, 0, bus->buffer, 0, 0, frames);
            bus->postFaderTap.copyFrom(1, 0, bus->buffer, 1, 0, frames);
        }
        if (partial) {
            bus->buffer.clear(0, frames);
        } else {
            for (int slice = 0; slice < slices.count; ++slice) {
                const int start = slices.offsetAt(slice);
                const int count = slices.framesAt(slice);
                if (budgetSpent()) {
                    bus->buffer.clear(start, frames - start);
                    partial = true;
                    break;
                }
                processChain(bus.get(), start, count);
            }
        }
        const float busGain = bus->muted.get() ? 0.0f : decibelsToGain(bus->gainDb.get());
        if (busGain != 1.0f) {
            bus->buffer.applyGain(0, 0, frames, busGain);
            bus->buffer.applyGain(1, 0, frames, busGain);
        }
        if (bus->postFaderTap.getNumSamples() >= frames) {
            bus->postFaderTap.copyFrom(0, 0, bus->buffer, 0, 0, frames);
            bus->postFaderTap.copyFrom(1, 0, bus->buffer, 1, 0, frames);
            bus->compensation.push(bus->postFaderTap.getReadPointer(0),
                                   bus->postFaderTap.getReadPointer(1), frames);
        }
        const auto meter = measureStereoBlock(bus->buffer, frames);
        meters.observeBusRealtime(bus->id, juce::jmax(meter.peakL, meter.peakR));
        bus->peak = meters.busPeak(bus->id);
        bus->peakL = meter.peakL;
        bus->peakR = meter.peakR;
        bus->rmsL = meter.rmsL;
        bus->rmsR = meter.rmsR;
    }

    // Bus routes add a finished bus into its output, in parallel with any
    // direct channel route. Outputs run their chain after this.
    for (const auto& route : busRoutes) {
        const auto bus = std::find_if(buses.begin(), buses.end(),
                                      [&route](const auto& b) { return b != nullptr && b->id == route->busId; });
        const auto output = std::find_if(outputs.begin(), outputs.end(),
                                         [&route](const auto& o) { return o != nullptr && o->id == route->outputId; });
        if (bus == buses.end() || output == outputs.end()) continue;
        const float sendGainL = decibelsToGain(route->sendGainDbL.get());
        const float sendGainR = decibelsToGain(route->sendGainDbR.get());
        if (sendGainL == 0.0f && sendGainR == 0.0f) continue;
        const int busDelay = compensate
            ? juce::jmax(0, maxBusLatency - (*bus)->cachedChainLatencySamples)
            : 0;
        const juce::AudioBuffer<float>* source = &(*bus)->buffer;
        if (busDelay > 0) {
            (*bus)->compensation.read(busDelay, tapScratch.getWritePointer(0),
                                      tapScratch.getWritePointer(1), frames);
            source = &tapScratch;
        }
        if (sendGainL != 0.0f) (*output)->buffer.addFrom(0, 0, *source, 0, 0, frames, sendGainL);
        if (sendGainR != 0.0f) (*output)->buffer.addFrom(1, 0, *source, 1, 0, frames, sendGainR);
    }

    for (const auto& route : routes) {
        const auto channelIt = std::find_if(channels.begin(), channels.end(), [&route](const auto& c) {
            return c->id == route->channelId;
        });
        const auto outputIt = std::find_if(outputs.begin(), outputs.end(), [&route](const auto& o) {
            return o->id == route->outputId;
        });
        if (channelIt == channels.end() || outputIt == outputs.end()) {
            continue;
        }

        const float sendGainL = decibelsToGain(route->sendGainDbL.get());
        const float sendGainR = decibelsToGain(route->sendGainDbR.get());
        if (sendGainL == 0.0f && sendGainR == 0.0f) {
            continue;
        }
        auto& sourceChannel = **channelIt;
        const int delay = compensate
            ? juce::jmax(0, sourceChannel.cachedChainLatencySamples
                                  - tapLatencyFor(sourceChannel, route->tap)
                                  + maxBusLatency)
            : 0;
        const auto* source = compensate ? tapSource(sourceChannel, route->tap, delay)
                                        : &sourceChannel.buffer;
        const int rightTap = sourceChannel.layout == pluginrack::ChannelLayout::Mono ? 0 : 1;
        if (sendGainL != 0.0f) {
            (*outputIt)->buffer.addFrom(0, 0, *source, 0, 0, frames, sendGainL);
        }
        if (sendGainR != 0.0f) {
            (*outputIt)->buffer.addFrom(1, 0, *source, rightTap, 0, frames, sendGainR);
        }
    }

    for (auto& output : outputs) {
        if (partial) {
            output->buffer.clear(0, frames);
        } else {
            for (int slice = 0; slice < slices.count; ++slice) {
                const int start = slices.offsetAt(slice);
                const int count = slices.framesAt(slice);
                if (budgetSpent()) {
                    output->buffer.clear(start, frames - start);
                    partial = true;
                    break;
                }
                processChain(output.get(), start, count);
            }
        }
        if (output->flipped.get()) {
            for (int i = 0; i < frames; ++i) {
                const float l = output->buffer.getSample(0, i);
                const float r = output->buffer.getSample(1, i);
                output->buffer.setSample(0, i, r);
                output->buffer.setSample(1, i, l);
            }
        }
        if (output->layout == pluginrack::ChannelLayout::Mono) {
            for (int i = 0; i < frames; ++i) {
                const float v = output->buffer.getSample(0, i);
                output->buffer.setSample(1, i, v);
            }
        }
    }

    for (auto& output : outputs) {
        const float outGain = output->muted.get() ? 0.0f : decibelsToGain(output->gainDb.get());
        if (outGain != 1.0f) {
            output->buffer.applyGain(0, 0, frames, outGain);
            output->buffer.applyGain(1, 0, frames, outGain);
        }
        if (output->postFaderTap.getNumSamples() >= frames) {
            output->postFaderTap.copyFrom(0, 0, output->buffer, 0, 0, frames);
            output->postFaderTap.copyFrom(1, 0, output->buffer, 1, 0, frames);
        }
        const auto meter = measureStereoBlock(output->buffer, frames);
        meters.observeOutputRealtime(output->id, juce::jmax(meter.peakL, meter.peakR));
        output->peak = meters.outputPeak(output->id);
        output->peakL = meter.peakL;
        output->peakR = meter.peakR;
        output->rmsL = meter.rmsL;
        output->rmsR = meter.rmsR;
    }

    // Round-trip latency measurement: inject a one-sample impulse on the
    // clock output after all processing, then detect it on input.
    if (latencyMeasureActive_.load(std::memory_order_relaxed) &&
        !latencyMeasureEmitted_.load(std::memory_order_relaxed)) {
        bool emitted = false;
        for (auto& output : outputs) {
            if (output->id == clockOutputId && output->output != nullptr) {
                output->buffer.setSample(0, 0, 1.0f);
                output->buffer.setSample(1, 0, 1.0f);
                emitted = true;
            }
        }
        if (emitted) {
            latencyMeasureStartMs_.store(juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
            latencyMeasureEmitted_.store(true, std::memory_order_relaxed);
            // Snapshot the capture clock in the same callback that places the
            // impulse: the hit index then measures the real round trip in
            // captured samples instead of a whole-render-block estimate. Only
            // the selected input listens, so an unrelated device cannot answer.
            for (auto& entry : inputCaptures_) {
                if (entry.second.capture == nullptr) continue;
                if (latencyListenDevice_.isNotEmpty() && entry.first != latencyListenDevice_)
                    continue;
                entry.second.capture->beginLatencyMeasurement();
            }
        }
        // No clock output renders nothing, so nothing is emitted; the
        // message-thread watchdog reports that case as a failure.
    }

    for (auto& output : outputs) {
        if (output->output) {
            output->output->push(output->buffer, frames);
        }
    }

    if (latencyMeasureActive_.load(std::memory_order_relaxed)) {
        InputCapture* hit = nullptr;
        for (auto& entry : inputCaptures_) {
            if (entry.second.capture != nullptr &&
                entry.second.capture->isLatencyImpulseDetected()) {
                hit = entry.second.capture.get();
                break;
            }
        }
        const bool timedOut = latencyMeasureEmitted_.load(std::memory_order_relaxed)
            && juce::Time::getMillisecondCounter()
                    - latencyMeasureStartMs_.load(std::memory_order_relaxed)
                > kLatencyMeasureTimeoutMs;
        if (hit != nullptr) {
            const auto samples = hit->getLatencyRoundTripSamples();
            const double rate = hit->getSampleRate() > 0.0 ? hit->getSampleRate()
                                                           : currentSampleRate;
            // A loud input can trip the amplitude test without carrying the
            // impulse. Reject an answer that lands before the impulse left or
            // beyond any plausible cable, so the readout never shows a figure
            // the chain cannot have.
            const auto maxSamples = static_cast<std::uint64_t>(
                juce::jmax(1.0, rate * kLatencyMeasureMaxSeconds));
            const bool plausible = samples > 0 && samples <= maxSamples;
            if (plausible) {
                latencyMeasuredSamples_.store(
                    static_cast<int>(juce::jmin<std::uint64_t>(
                        samples, static_cast<std::uint64_t>(std::numeric_limits<int>::max()))),
                    std::memory_order_relaxed);
                latencyMeasuredRate_.store(rate, std::memory_order_relaxed);
            }
            latencyMeasuredValid_.store(plausible, std::memory_order_relaxed);
            latencyMeasureFailed_.store(!plausible, std::memory_order_relaxed);
            latencyMeasureActive_.store(false, std::memory_order_relaxed);
            for (auto& entry : inputCaptures_) {
                if (entry.second.capture != nullptr)
                    entry.second.capture->endLatencyMeasurement();
            }
        } else if (timedOut) {
            latencyMeasureFailed_.store(true, std::memory_order_relaxed);
            latencyMeasureActive_.store(false, std::memory_order_relaxed);
            for (auto& entry : inputCaptures_) {
                if (entry.second.capture != nullptr)
                    entry.second.capture->endLatencyMeasurement();
            }
        }
    }

    std::int32_t clockFillFrames = 0;
    for (const auto& output : outputs) {
        if (output->id == clockOutputId && output->output != nullptr) {
            clockFillFrames = static_cast<std::int32_t>(output->output->getFillFrames());
        }
    }
    std::uint16_t renderFlags = 0;
    if (partial) {
        renderFlags |= pluginrack::AudioTelemetry::flagPartialRender;
    }
    if (scrubbedThisBlock > 0) {
        renderFlags |= pluginrack::AudioTelemetry::flagNonFiniteScrub;
    }
    recordRenderTelemetry(frames, elapsedNanos(), renderFlags, clockFillFrames);
}

void AudioEngine::pullDuplexInput(Channel& channel, int frames) const {
    if (duplexInputBuffers_ == nullptr || duplexInputBufferCount_ <= 0 || frames <= 0) return;
    const int numDest = juce::jmin(2, static_cast<int>(channel.inputChannels.size()));
    for (int i = 0; i < numDest; ++i) {
        const int source = channel.inputChannels[static_cast<std::size_t>(i)];
        if (source < 0 || source >= duplexInputBufferCount_) continue;
        const float* in = duplexInputBuffers_[source];
        if (in == nullptr) continue;
        std::memcpy(channel.buffer.getWritePointer(i), in,
                    sizeof(float) * static_cast<std::size_t>(frames));
    }
}

void AudioEngine::recordRenderTelemetry(int frames, std::uint32_t renderNanos,
                                        std::uint16_t flags,
                                        std::int32_t outputFill) const {
    pluginrack::AudioTelemetry::Sample sample;
    sample.blockFrames = static_cast<std::uint32_t>(juce::jmax(0, frames));
    sample.renderNanos = renderNanos;
    sample.outputFillFrames = outputFill;
    sample.flags = flags;
    renderTelemetry_.push(sample);
}

pluginrack::AudioTelemetry::Summary AudioEngine::getRenderTelemetry() const {
    const int blockFrames = maxBlockFrames_ > 0 ? maxBlockFrames_ : currentBufferSize;
    return renderTelemetry_.summarise(static_cast<std::uint32_t>(juce::jmax(0, blockFrames)),
                                      currentSampleRate > 0.0 ? currentSampleRate : 48000.0);
}

void AudioEngine::startLatencyProfile() {
    const auto diagnostics = getFifoDiagnostics();
    latencyProfileLastEvents_ = diagnostics.inputUnderruns + diagnostics.inputOverruns
                              + diagnostics.outputUnderruns + diagnostics.outputLowWatermarks
                              + diagnostics.renderMisses;
    const int startBuffer = currentBufferSize > 0 ? currentBufferSize : 480;
    latencyProfile_.start(startBuffer, pluginrack::LatencyProfile::Config{});
    latencyProfileLastMs_ = juce::Time::getMillisecondCounter();
    latencyProfileAwaitingOpen_ = false;
    (void) takeDeviceOpenErrors();
    tickLatencyProfile();
}

void AudioEngine::cancelLatencyProfile() {
    if (!latencyProfile_.status().active) return;
    const int startBuffer = latencyProfile_.status().startBuffer;
    latencyProfile_.cancel();
    latencyProfileAwaitingOpen_ = false;
    if (startBuffer > 0) {
        auto settings = getDeviceSettings();
        settings.inputBuffer = startBuffer;
        settings.outputBuffer = startBuffer;
        applyDeviceSettings(settings);
    }
}

AudioEngine::LatencyProfileState AudioEngine::tickLatencyProfile() {
    if (!latencyProfile_.status().active) return getLatencyProfileState();

    const auto now = juce::Time::getMillisecondCounter();
    const double elapsedMs = latencyProfileLastMs_ == 0
                                 ? 0.0
                                 : static_cast<double>(now - latencyProfileLastMs_);
    latencyProfileLastMs_ = now;

    const auto diagnostics = getFifoDiagnostics();
    const auto events = diagnostics.inputUnderruns + diagnostics.inputOverruns
                      + diagnostics.outputUnderruns + diagnostics.outputLowWatermarks
                      + diagnostics.renderMisses;
    // A device reload can reset counters; never treat that as negative input.
    const auto delta = events >= latencyProfileLastEvents_ ? events - latencyProfileLastEvents_ : 0;
    latencyProfileLastEvents_ = events;

    // The step we applied last tick has finished opening: that is the only
    // point where a refusal is attributable to this step.
    if (latencyProfileAwaitingOpen_ && !isOpeningDevices()) {
        latencyProfileAwaitingOpen_ = false;
        const bool refused = !takeDeviceOpenErrors().isEmpty();
        latencyProfile_.onApplied(latencyProfileRequested_, refused);
    }

    const auto action = latencyProfile_.tick(
        static_cast<int>(juce::jmin<std::uint64_t>(delta, 1000)), elapsedMs);
    if (action == pluginrack::LatencyProfile::Action::Apply) {
        const int target = latencyProfile_.status().currentBuffer;
        auto settings = getDeviceSettings();
        settings.inputBuffer = target;
        settings.outputBuffer = target;
        latencyProfileRequested_ = target;
        (void) takeDeviceOpenErrors();
        applyDeviceSettings(settings);
        latencyProfileAwaitingOpen_ = true;
    } else if (action == pluginrack::LatencyProfile::Action::Revert) {
        auto settings = getDeviceSettings();
        const int target = latencyProfile_.status().startBuffer;
        settings.inputBuffer = target;
        settings.outputBuffer = target;
        applyDeviceSettings(settings);
    }
    return getLatencyProfileState();
}

AudioEngine::LatencyProfileState AudioEngine::getLatencyProfileState() const {
    const auto status = latencyProfile_.status();
    LatencyProfileState state;
    state.active = status.active;
    state.finished = status.finished;
    state.failed = status.failed;
    state.stepIndex = status.stepIndex;
    state.totalSteps = status.totalSteps;
    state.startBuffer = status.startBuffer;
    state.currentBuffer = status.currentBuffer;
    state.remainingMs = status.remainingMs;
    return state;
}

void AudioEngine::releaseResources() {
    std::vector<PluginHost*> hosts;
    {
        const juce::ScopedLock lock(stateLock);
        for (auto& channel : channels) {
            for (auto& insert : channel->inserts) {
                hosts.push_back(&insert->pluginHost);
            }
        }
        for (auto& output : outputs) {
            for (auto& insert : output->inserts) {
                hosts.push_back(&insert->pluginHost);
            }
        }
    }
    for (auto* host : hosts) {
        host->releaseResources();
    }
}

juce::String AudioEngine::removeChannel(std::uint32_t channelId) {
    // The startup opener holds Channel* across a device probe; refuse the
    // structural change until it is done instead of invalidating it.
    if (isOpeningDevices()) return "Audio devices are still starting up. Try again in a moment.";
    std::unique_ptr<Channel> removed;
    {
        const juce::ScopedLock lock(stateLock);
        const auto it = std::find_if(channels.begin(), channels.end(),
                                     [channelId](const auto& channel) {
                                         return channel->id == channelId;
                                     });
        if (it == channels.end()) return "The channel no longer exists.";
        // validate() rejects a scene with no channels, so the exit autosave would
        // silently keep the previous session. Mirror removeOutput's floor.
        if (channels.size() <= 1) return "At least one channel is required.";
        (*it)->input.reset();
        removed = std::move(*it);
        channels.erase(it);
        routes.erase(std::remove_if(routes.begin(), routes.end(),
                                   [channelId](const auto& route) { return route->channelId == channelId; }),
                      routes.end());
        busSends.erase(std::remove_if(busSends.begin(), busSends.end(),
                                      [channelId](const auto& send) { return send->channelId == channelId; }),
                       busSends.end());
        clearSideChainRefsLocked(pluginrack::SideChainSource::Channel, channelId);
        pruneUnusedCaptures();
    }
    // Released the lock first: render() can no longer reach the strip, and
    // stopping its plugins joins sandbox processes and closes editors on this
    // message thread.
    retireStripInserts(*removed);
    bumpStructureRevision();
    return {};
}

void AudioEngine::moveChannel(std::uint32_t channelId, int targetIndex) {
    // The startup opener holds Channel* across a device probe; reordering the
    // vector would invalidate it mid-probe. Same guard as addChannel.
    if (isOpeningDevices()) return;
    const juce::ScopedLock lock(stateLock);
    const auto it = std::find_if(channels.begin(), channels.end(),
                                 [channelId](const auto& channel) {
                                     return channel->id == channelId;
                                 });
    if (it == channels.end()) return;
    const int currentIndex = static_cast<int>(std::distance(channels.begin(), it));
    const int index = juce::jlimit(0, static_cast<int>(channels.size()) - 1, targetIndex);
    if (index == currentIndex) return;
    auto moved = std::move(*it);
    channels.erase(it);
    channels.insert(channels.begin() + index, std::move(moved));
    bumpStructureRevision();
}

void AudioEngine::moveOutput(std::uint32_t outputId, int targetIndex) {
    // moveOutput swaps unique_ptrs inside `outputs` while background opener
    // threads may hold Output* across a device probe. Same guard as addOutput.
    if (isOpeningDevices()) return;
    const juce::ScopedLock lock(stateLock);
    const auto it = std::find_if(outputs.begin(), outputs.end(),
                                 [outputId](const auto& output) {
                                     return output->id == outputId;
                                 });
    if (it == outputs.end()) return;
    const int currentIndex = static_cast<int>(std::distance(outputs.begin(), it));
    const int index = juce::jlimit(0, static_cast<int>(outputs.size()) - 1, targetIndex);
    if (index == currentIndex) return;
    auto moved = std::move(*it);
    outputs.erase(it);
    outputs.insert(outputs.begin() + index, std::move(moved));
    bumpStructureRevision();
}

void AudioEngine::addChannel() {
    if (isOpeningDevices()) return;
    const juce::ScopedLock lock(stateLock);
    auto channel = std::make_unique<Channel>();
    channel->id = nextChannelId++;
    channel->name = "Channel " + juce::String(channel->id);
    channel->sourceKind = pluginrack::SourceKind::Hardware;
    channel->layout = pluginrack::ChannelLayout::Stereo;
    channel->inputChannels = {0, 1};
    channel->chain = defaultChain();
    // Identity colour: a random hue from the curated palette so every strip
    // is distinguishable from birth; editable later from the rename dialog.
    {
        juce::Random rng(juce::Time::currentTimeMillis()
                         ^ static_cast<juce::int64>(channel->id * 2654435761u));
        channel->colourHue = theme::randomStripHue(rng);
    }
    channel->buffer.setSize(2, currentBufferSize);
    channel->buffer.clear();
    channel->preInsertTap.setSize(2, currentBufferSize);
    channel->preInsertTap.clear();
    channel->postFaderTap.setSize(2, currentBufferSize);
    channel->postFaderTap.clear();
    channel->outputTapRing.prepare(pluginrack::kMaxTapDelaySamples + currentBufferSize + 2);
    // Preallocate the EQ so internal bands match the member mirror: a strip
    // created mid-session is usable the moment the UI un-bypasses it.
    channel->eq.prepare(currentSampleRate);
    channel->eq.setBands(channel->eqBands);
    channel->eq.setDelayMs(channel->eqDelayMs);
    channel->effectProcessors.reserve(channel->effects.size());
    for (const auto& effect : channel->effects) {
        pluginrack::DynamicsProcessor processor;
        processor.prepare(currentSampleRate);
        processor.setEffect(effect);
        channel->effectProcessors.push_back(std::move(processor));
    }
    meters.observeChannel(channel->id, 0.0f);
    for (const auto& e : channel->multibandEffects) { pluginrack::MultibandProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); channel->multibandProcessors.push_back(std::move(pr)); }
    for (const auto& e : channel->deEsserEffects) { pluginrack::DeEsserProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); channel->deEsserProcessors.push_back(std::move(pr)); }
    for (const auto& e : channel->limiterEffects) { pluginrack::LimiterProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); channel->limiterProcessors.push_back(std::move(pr)); }
for (const auto& e : channel->feedbackSuppressorEffects) { pluginrack::FeedbackSuppressorProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); channel->feedbackSuppressorProcessors.push_back(std::move(pr)); }
    for (const auto& e : channel->spectralDenoiserEffects) { pluginrack::SpectralDenoiserProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); channel->spectralDenoiserProcessors.push_back(std::move(pr)); }
    for (const auto& output : outputs) {
        auto route = std::make_unique<Route>();
        route->channelId = channel->id;
        route->outputId = output->id;
        const float g = output == outputs.front() ? 0.0f : kSendOffDb;
        route->sendGainDbL = g;
        route->sendGainDbR = g;
        routes.push_back(std::move(route));
    }
    channels.push_back(std::move(channel));
    // The new strip gets its plan before render can see it: an empty chain
    // still has the default Dynamics effect and the unchained EQ to run.
    rebuildRenderPlan(*channels.back(), currentSampleRate);
    refreshAllSideChainCachesLocked();
    bumpStructureRevision();
}

void AudioEngine::addOutput() {
    if (isOpeningDevices()) return;
    const juce::ScopedLock lock(stateLock);
    auto output = std::make_unique<Output>();
    output->id = nextOutputId++;
    if (output->id <= 5) output->name = "A" + juce::String(output->id);
    else if (output->id <= 8) output->name = "B" + juce::String(output->id - 5);
    else output->name = "Output " + juce::String(output->id);
    output->buffer.setSize(2, currentBufferSize);
    output->buffer.clear();
    {
        juce::Random rng(juce::Time::currentTimeMillis()
                         ^ static_cast<juce::int64>(output->id * 40503u));
        output->colourHue = theme::randomStripHue(rng);
    }
    output->chain = defaultChain();
    output->eq.prepare(currentSampleRate);
    output->eq.setBands(output->eqBands);
    output->eq.setDelayMs(output->eqDelayMs);
    output->effectProcessors.reserve(output->effects.size());
    for (const auto& effect : output->effects) {
        pluginrack::DynamicsProcessor processor;
        processor.prepare(currentSampleRate);
        processor.setEffect(effect);
        output->effectProcessors.push_back(std::move(processor));
    }
    meters.observeOutput(output->id, 0.0f);
    for (const auto& e : output->multibandEffects) { pluginrack::MultibandProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); output->multibandProcessors.push_back(std::move(pr)); }
    for (const auto& e : output->deEsserEffects) { pluginrack::DeEsserProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); output->deEsserProcessors.push_back(std::move(pr)); }
    for (const auto& e : output->limiterEffects) { pluginrack::LimiterProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); output->limiterProcessors.push_back(std::move(pr)); }
for (const auto& e : output->feedbackSuppressorEffects) { pluginrack::FeedbackSuppressorProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); output->feedbackSuppressorProcessors.push_back(std::move(pr)); }
    for (const auto& e : output->spectralDenoiserEffects) { pluginrack::SpectralDenoiserProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); output->spectralDenoiserProcessors.push_back(std::move(pr)); }
    for (const auto& channel : channels) {
        auto route = std::make_unique<Route>();
        route->channelId = channel->id;
        route->outputId = output->id;
        route->sendGainDbL = kSendOffDb;
        route->sendGainDbR = kSendOffDb;
        routes.push_back(std::move(route));
    }
    outputs.push_back(std::move(output));
    rebuildRenderPlan(*outputs.back(), currentSampleRate);
    refreshAllSideChainCachesLocked();
    bumpStructureRevision();
}

juce::String AudioEngine::removeOutput(std::uint32_t outputId) {
    if (isOpeningDevices()) {
        return "Audio devices are still starting up. Try again in a moment.";
    }
    AudioEngine::Output* out = nullptr;
    for (auto& o : outputs) {
        if (o->id == outputId) { out = o.get(); break; }
    }
    if (out == nullptr) {
        return "The output no longer exists.";
    }
    if (outputs.size() <= 1) {
        return "At least one output is required.";
    }

    const bool wasClock = (clockOutputId == outputId);
    OutputPlayback* retiredPlayback = nullptr;
    std::unique_ptr<Output> removed;
    {
        const juce::ScopedLock sl(stateLock);
        if (wasClock && out->output != nullptr) {
            out->output->setRenderActive(false);
        }
        if (out->output != nullptr) {
            retiredPlayback = out->output.get();
            retiredOutputs_.push_back(std::move(out->output));
        }
        routes.erase(std::remove_if(routes.begin(), routes.end(),
                                   [outputId](const auto& route) { return route->outputId == outputId; }),
                     routes.end());
        busRoutes.erase(std::remove_if(busRoutes.begin(), busRoutes.end(),
                                       [outputId](const auto& route) { return route->outputId == outputId; }),
                        busRoutes.end());
        for (auto it = outputs.begin(); it != outputs.end(); ++it) {
            if ((*it)->id == outputId) {
                removed = std::move(*it);
                outputs.erase(it);
                break;
            }
        }
        clearSideChainRefsLocked(pluginrack::SideChainSource::Output, outputId);
        if (wasClock) {
            // The duplex callback left with this output: stop the channels from
            // reading capture pointers that no longer update.
            duplexActive_.store(false, std::memory_order_release);
            duplexDeviceName_.clear();
            duplexInputBuffers_ = nullptr;
            duplexInputBufferCount_ = 0;
            for (auto& c : channels) {
                c->duplexInput = false;
                c->duplexInputChannels = 0;
            }
            clockOutputId = 0;
            for (auto& o2 : outputs) {
                if (o2->output != nullptr) {
                    clockOutputId = o2->id;
                    // The hook is installed before every stream is opened; only
                    // the role moves here. A clock renders and drains in the
                    // same callback, so drop the independent-output headroom.
                    o2->output->setTargetFillBlocks(0);
                    o2->output->setRenderActive(true);
                    break;
                }
            }
        }
        publishRenderOutputsLocked();
    }
    if (removed != nullptr) {
        // Same rule as removeChannel: the strip is unlinked from the render
        // set, so its plugins can be stopped without the lock.
        retireStripInserts(*removed);
    }
    if (retiredPlayback != nullptr) {
        retiredPlayback->close();
    }
    bumpStructureRevision();
    return {};
}

void AudioEngine::addPluginAsync(std::uint32_t channelId, const juce::File& file,
                                 int pluginUniqueId, std::function<void(juce::String)> done) {
    {
        const juce::ScopedLock lock(stateLock);
        auto* channel = findChannel(channelId);
        if (channel == nullptr) {
            done("The channel no longer exists.");
            return;
        }
        if (channel->inserts.size() >= kMaxInserts) {
            done("This strip already holds " + juce::String(kMaxInserts)
                 + " VST inserts.");
            return;
        }
    }

    // Scanning happens in a child process (PluginHost::scanFile), so it is safe
    // on a worker and keeps the window responsive through the seconds a large
    // VST3 takes to enumerate its classes. Creating the instance has to run on
    // the message thread, and the strip is left untouched until it exists.
    const double rate = currentSampleRate;
    const int block = currentBufferSize;
    auto alive = prepareAliveToken_;
    juce::Logger::writeToLog("plugin: loading " + file.getFileName());
    juce::Thread::launch([this, alive, channelId, file, pluginUniqueId, rate, block,
                          done = std::move(done)]() mutable {
        juce::String scanError;
        const auto descriptions = PluginHost::scanFile(file, scanError);
        juce::MessageManager::callAsync(
            [this, alive, channelId, file, pluginUniqueId, rate, block, descriptions, scanError,
             done = std::move(done)]() mutable {
                if (!alive->load(std::memory_order_acquire)) return;

                auto loaded = PluginHost::LoadedPlugin{};
                loaded.path = file.getFullPathName();
                int resolvedUniqueId = 0;
                if (descriptions.isEmpty()) {
                    loaded.error =
                        scanError.isNotEmpty()
                            ? scanError
                            : juce::String("No VST3 plugin was found in the selected file.");
                } else {
                    const auto chosen = chooseDescription(descriptions, pluginUniqueId);
                    resolvedUniqueId = chosen.uniqueId;
                    loaded = PluginHost::instantiate(chosen, file, rate, block);
                }
                if (loaded.error.isNotEmpty()) {
                    juce::Logger::writeToLog("plugin: failed - " + loaded.error);
                    done(loaded.error);
                    return;
                }

                auto insert = std::make_unique<InsertSlot>();
                // Size the side-chain scratch here too: prepareToPlayImpl only
                // sizes inserts that exist when the engine is prepared, and a
                // freshly added insert renders before the next prepare. An
                // unsized buffer means null read pointers in the render loop.
                insert->sideChainBuffer.setSize(2, juce::jmax(1, currentBufferSize), false, true,
                                                true);
                insert->pluginHost.prepareToPlay(currentSampleRate, currentBufferSize);
                insert->pluginHost.adopt(loaded);
                insert->pluginPath = loaded.path;
                insert->pluginName = loaded.name;
                insert->pluginUniqueId = resolvedUniqueId;
                insert->latencySamples.store(insert->pluginHost.getLatencySamples());

                if (getChildProcessDsp()) {
                    juce::String childError;
                    const bool ok = insert->pluginHost.loadChildHost(
                        juce::File(loaded.path), resolvedUniqueId,
                        insert->pluginHost.getStateAsBase64(), currentSampleRate,
                        currentBufferSize, 20000, childError);
                    juce::Logger::writeToLog(ok
                                                 ? "plugin: child host ready for " + loaded.name
                                                 : "plugin: child host unavailable for " + loaded.name
                                                       + " (" + childError + ")");
                }

                juce::String error;
                {
                    const juce::ScopedLock lock(stateLock);
                    auto* channel = findChannel(channelId);
                    if (channel == nullptr) {
                        error = "The channel no longer exists.";
                    } else if (channel->inserts.size() >= kMaxInserts) {
                        error = "This strip already holds " + juce::String(kMaxInserts)
                                + " VST inserts.";
                    } else {
                        insert->id = nextInsertId++;
                        const auto insertId = insert->id;
                        insert->crashLabel = makeCrashLabel(channel->name, insertId,
                                                            insert->pluginName,
                                                            insert->pluginPath);
                        channel->inserts.push_back(std::move(insert));
                        channel->chain.push_back({pluginrack::ChainItemType::Insert, insertId});
                        rebuildRenderPlan(*channel, currentSampleRate);
                        refreshAllSideChainCachesLocked();
                        // The insert (and its chain row) is now part of the
                        // saved scene.
                        bumpStructureRevision();
                    }
                }
                juce::Logger::writeToLog(error.isEmpty()
                                             ? "plugin: loaded " + loaded.name
                                             : "plugin: install failed - " + error);
                done(error);
            });
    });
}

void AudioEngine::removePlugin(std::uint32_t channelId, std::uint32_t insertId) {
    std::unique_ptr<InsertSlot> removed;
    {
        const juce::ScopedLock lock(stateLock);
        auto* channel = findChannel(channelId);
        if (channel == nullptr) {
            return;
        }
        auto& inserts = channel->inserts;
        for (auto it = inserts.begin(); it != inserts.end(); ++it) {
            if ((*it)->id == insertId) {
                removed = std::move(*it);
                inserts.erase(it);
                channel->chain.erase(std::remove_if(channel->chain.begin(), channel->chain.end(),
                                                    [insertId](const auto& item) {
                                                        return item.type == pluginrack::ChainItemType::Insert &&
                                                               item.id == insertId;
                                                    }),
                                     channel->chain.end());
                break;
            }
        }
        if (removed != nullptr) {
            // The plan held a raw pointer to this slot; it must not outlive the
            // erase, and the chain latency drops by the insert's report.
            rebuildRenderPlan(*channel, currentSampleRate);
        }
    }
    if (removed == nullptr) {
        return;
    }
    // stateLock is released and render() can no longer reach this insert.
    // Stopping it here reaps the VST3 instance and its child process instead of
    // keeping both alive until shutdown.
    retireInsert(*removed);
    bumpStructureRevision();
}

void AudioEngine::removeChannelChainItem(std::uint32_t channelId, pluginrack::ChainItem item) {
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) return;
    channel->chain.erase(std::remove(channel->chain.begin(), channel->chain.end(), item),
                         channel->chain.end());
    // A removed effect must stay silent through the fallback pass until the
    // user picks it again (which unbypasses it).
    if (item.type == pluginrack::ChainItemType::Eq) {
        channel->eqBypassed = true;
    } else if (item.type == pluginrack::ChainItemType::Dynamics) {
        for (auto& effect : channel->effects)
            if (effect.id == item.id) effect.bypassed = true;
        for (std::size_t i = 0; i < channel->effects.size()
                                && i < channel->effectProcessors.size();
             ++i)
            if (channel->effects[i].id == item.id)
                channel->effectProcessors[i].setEffect(channel->effects[i]);
    } else if (item.type == pluginrack::ChainItemType::Multiband) {
        for (auto& effect : channel->multibandEffects)
            if (effect.id == item.id) effect.bypassed = true;
        for (std::size_t i = 0; i < channel->multibandEffects.size()
                                && i < channel->multibandProcessors.size();
             ++i)
            if (channel->multibandEffects[i].id == item.id)
                channel->multibandProcessors[i].setEffect(
                    channel->multibandEffects[i]);
    } else if (item.type == pluginrack::ChainItemType::DeEsser) {
        for (auto& effect : channel->deEsserEffects)
            if (effect.id == item.id) effect.bypassed = true;
        for (std::size_t i = 0; i < channel->deEsserEffects.size()
                                && i < channel->deEsserProcessors.size();
             ++i)
            if (channel->deEsserEffects[i].id == item.id)
                channel->deEsserProcessors[i].setEffect(
                    channel->deEsserEffects[i]);
    } else if (item.type == pluginrack::ChainItemType::Limiter) {
        for (auto& effect : channel->limiterEffects)
            if (effect.id == item.id) effect.bypassed = true;
        for (std::size_t i = 0; i < channel->limiterEffects.size()
                                && i < channel->limiterProcessors.size();
             ++i)
            if (channel->limiterEffects[i].id == item.id)
                channel->limiterProcessors[i].setEffect(
                    channel->limiterEffects[i]);
    } else if (item.type == pluginrack::ChainItemType::FeedbackSuppressor) {
        for (auto& effect : channel->feedbackSuppressorEffects)
            if (effect.id == item.id) effect.bypassed = true;
        for (std::size_t i = 0; i < channel->feedbackSuppressorEffects.size()
                                && i < channel->feedbackSuppressorProcessors.size();
             ++i)
            if (channel->feedbackSuppressorEffects[i].id == item.id)
                channel->feedbackSuppressorProcessors[i].setEffect(
                    channel->feedbackSuppressorEffects[i]);
    } else if (item.type == pluginrack::ChainItemType::SpectralDenoiser) {
        for (auto& effect : channel->spectralDenoiserEffects)
            if (effect.id == item.id) effect.bypassed = true;
        for (std::size_t i = 0; i < channel->spectralDenoiserEffects.size()
                                && i < channel->spectralDenoiserProcessors.size();
             ++i)
            if (channel->spectralDenoiserEffects[i].id == item.id)
                channel->spectralDenoiserProcessors[i].setEffect(
                    channel->spectralDenoiserEffects[i]);
    }
    // The row left the chain: the plan and the latency it bills both change.
    rebuildRenderPlan(*channel, currentSampleRate);
    bumpStructureRevision();
}

void AudioEngine::ensureChannelChainItem(std::uint32_t channelId, pluginrack::ChainItem item) {
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) return;
    if (std::none_of(channel->chain.begin(), channel->chain.end(),
                     [&item](const pluginrack::ChainItem& entry) { return entry == item; })) {
        channel->chain.push_back(item);
        bumpStructureRevision();
    }
    rebuildRenderPlan(*channel, currentSampleRate);
}

void AudioEngine::ensureOutputChainItem(std::uint32_t outputId, pluginrack::ChainItem item) {
    const juce::ScopedLock lock(stateLock);
    auto* output = findOutput(outputId);
    if (output == nullptr) return;
    if (std::none_of(output->chain.begin(), output->chain.end(),
                     [&item](const pluginrack::ChainItem& entry) { return entry == item; })) {
        output->chain.push_back(item);
        bumpStructureRevision();
    }
    rebuildRenderPlan(*output, currentSampleRate);
}

void AudioEngine::removeOutputChainItem(std::uint32_t outputId, pluginrack::ChainItem item) {
    const juce::ScopedLock lock(stateLock);
    auto* output = findOutput(outputId);
    if (output == nullptr) return;
    output->chain.erase(std::remove(output->chain.begin(), output->chain.end(), item),
                        output->chain.end());
    if (item.type == pluginrack::ChainItemType::Eq) {
        output->eqBypassed = true;
    } else if (item.type == pluginrack::ChainItemType::Dynamics) {
        for (auto& effect : output->effects)
            if (effect.id == item.id) effect.bypassed = true;
        for (std::size_t i = 0; i < output->effects.size()
                                && i < output->effectProcessors.size();
             ++i)
            if (output->effects[i].id == item.id)
                output->effectProcessors[i].setEffect(output->effects[i]);
    } else if (item.type == pluginrack::ChainItemType::Multiband) {
        for (auto& effect : output->multibandEffects)
            if (effect.id == item.id) effect.bypassed = true;
        for (std::size_t i = 0; i < output->multibandEffects.size()
                                && i < output->multibandProcessors.size();
             ++i)
            if (output->multibandEffects[i].id == item.id)
                output->multibandProcessors[i].setEffect(
                    output->multibandEffects[i]);
    } else if (item.type == pluginrack::ChainItemType::DeEsser) {
        for (auto& effect : output->deEsserEffects)
            if (effect.id == item.id) effect.bypassed = true;
        for (std::size_t i = 0; i < output->deEsserEffects.size()
                                && i < output->deEsserProcessors.size();
             ++i)
            if (output->deEsserEffects[i].id == item.id)
                output->deEsserProcessors[i].setEffect(
                    output->deEsserEffects[i]);
    } else if (item.type == pluginrack::ChainItemType::Limiter) {
        for (auto& effect : output->limiterEffects)
            if (effect.id == item.id) effect.bypassed = true;
        for (std::size_t i = 0; i < output->limiterEffects.size()
                                && i < output->limiterProcessors.size();
             ++i)
            if (output->limiterEffects[i].id == item.id)
                output->limiterProcessors[i].setEffect(
                    output->limiterEffects[i]);
    } else if (item.type == pluginrack::ChainItemType::FeedbackSuppressor) {
        for (auto& effect : output->feedbackSuppressorEffects)
            if (effect.id == item.id) effect.bypassed = true;
        for (std::size_t i = 0; i < output->feedbackSuppressorEffects.size()
                                && i < output->feedbackSuppressorProcessors.size();
             ++i)
            if (output->feedbackSuppressorEffects[i].id == item.id)
                output->feedbackSuppressorProcessors[i].setEffect(
                    output->feedbackSuppressorEffects[i]);
    } else if (item.type == pluginrack::ChainItemType::SpectralDenoiser) {
        for (auto& effect : output->spectralDenoiserEffects)
            if (effect.id == item.id) effect.bypassed = true;
        for (std::size_t i = 0; i < output->spectralDenoiserEffects.size()
                                && i < output->spectralDenoiserProcessors.size();
             ++i)
            if (output->spectralDenoiserEffects[i].id == item.id)
                output->spectralDenoiserProcessors[i].setEffect(
                    output->spectralDenoiserEffects[i]);
    }
    rebuildRenderPlan(*output, currentSampleRate);
    bumpStructureRevision();
}

void AudioEngine::setChannelChain(std::uint32_t channelId,
                                  const std::vector<pluginrack::ChainItem>& chain) {
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) {
        channel->chain = chain;
        rebuildRenderPlan(*channel, currentSampleRate);
        bumpStructureRevision();
    }
}

void AudioEngine::setOutputChain(std::uint32_t outputId,
                                 const std::vector<pluginrack::ChainItem>& chain) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) {
        output->chain = chain;
        rebuildRenderPlan(*output, currentSampleRate);
        bumpStructureRevision();
    }
}

void AudioEngine::moveChannelChainItem(std::uint32_t channelId, pluginrack::ChainItem item,
                                       int targetIndex) {
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) return;
    auto& chain = channel->chain;
    pluginrack::moveChainItem(chain, item, targetIndex);
    // A move changes which row owns an Item tap and the order the latency is
    // billed in.
    rebuildRenderPlan(*channel, currentSampleRate);
    bumpStructureRevision();
}

void AudioEngine::setPluginBypassed(std::uint32_t channelId, std::uint32_t insertId, bool bypassed) {
    // The latency query stays outside stateLock: it can talk to the insert's
    // child process, and render() must never wait behind that. No render path
    // reads the report any more - it reads the strip's cached total, which the
    // locked rebuild below refreshes.
    auto* insert = findInsert(channelId, insertId);
    if (insert == nullptr) return;
    insert->bypassed = bypassed;
    insert->pluginHost.refreshLatencyCache();
    insert->latencySamples.store(insert->pluginHost.getLatencySamples());
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) {
        rebuildRenderPlan(*channel, currentSampleRate);
        // The bypass flag is part of the saved insert.
        bumpStructureRevision();
    }
}

void AudioEngine::setPluginSideChain(std::uint32_t channelId, std::uint32_t insertId,
                                     pluginrack::SideChainSource source, std::uint32_t sourceId,
                                     pluginrack::SideChainTap tap, float gainDb) {
    const juce::ScopedLock lock(stateLock);
    if (auto* insert = findInsert(channelId, insertId)) {
        insert->sideChainSource = source;
        insert->sideChainSourceId = source == pluginrack::SideChainSource::None ? 0 : sourceId;
        insert->sideChainTap = tap;
        insert->sideChainGainDb = juce::jlimit(-60.0f, 12.0f, gainDb);
        // Re-resolve now: render() reads the cached source without searching.
        bool mono = false;
        insert->sideChainSourceBuffer = resolveSideChainSource(*insert, mono);
        insert->sideChainSourceMono = mono;
        // Side-chain routing is persisted with the insert (schema 8).
        bumpStructureRevision();
    }
}

std::uint32_t AudioEngine::addBus(const juce::String& name) {
    const juce::ScopedLock lock(stateLock);
    if (buses.size() >= pluginrack::kMaxBuses) return 0;
    auto bus = std::make_unique<Bus>();
    bus->id = nextOutputId++;
    bus->name = name.isNotEmpty() ? name : ("Bus " + juce::String(bus->id));
    bus->buffer.setSize(2, currentBufferSize);
    bus->preInsertTap.setSize(2, currentBufferSize);
    bus->postFaderTap.setSize(2, currentBufferSize);
    bus->buffer.clear();
    bus->preInsertTap.clear();
    bus->postFaderTap.clear();
    bus->compensation.prepare(pluginrack::kMaxTapDelaySamples + currentBufferSize + 2);
    {
        juce::Random rng(juce::Time::currentTimeMillis()
                         ^ static_cast<juce::int64>(bus->id * 40503u));
        bus->colourHue = theme::randomStripHue(rng);
    }
    bus->chain = defaultChain();
    bus->eq.prepare(currentSampleRate);
    bus->eq.setBands(bus->eqBands);
    bus->eq.setDelayMs(bus->eqDelayMs);
    bus->effectProcessors.reserve(bus->effects.size());
    for (const auto& effect : bus->effects) {
        pluginrack::DynamicsProcessor processor;
        processor.prepare(currentSampleRate);
        processor.setEffect(effect);
        bus->effectProcessors.push_back(std::move(processor));
    }
    meters.observeBus(bus->id, 0.0f);
    for (const auto& e : bus->multibandEffects) { pluginrack::MultibandProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); bus->multibandProcessors.push_back(std::move(pr)); }
    for (const auto& e : bus->deEsserEffects) { pluginrack::DeEsserProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); bus->deEsserProcessors.push_back(std::move(pr)); }
    for (const auto& e : bus->limiterEffects) { pluginrack::LimiterProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); bus->limiterProcessors.push_back(std::move(pr)); }
    for (const auto& e : bus->feedbackSuppressorEffects) { pluginrack::FeedbackSuppressorProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); bus->feedbackSuppressorProcessors.push_back(std::move(pr)); }
    for (const auto& e : bus->spectralDenoiserEffects) { pluginrack::SpectralDenoiserProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); bus->spectralDenoiserProcessors.push_back(std::move(pr)); }
    const std::uint32_t id = bus->id;
    buses.push_back(std::move(bus));
    rebuildRenderPlan(*buses.back(), currentSampleRate);
    refreshAllSideChainCachesLocked();
    bumpStructureRevision();
    return id;
}

void AudioEngine::clearSideChainRefsLocked(pluginrack::SideChainSource source,
                                           std::uint32_t id) {
    // Inserts naming a removed strip fall back to no side chain instead of
    // pointing at an id that no longer exists. Requires stateLock.
    const auto clear = [source, id](auto& inserts) {
        for (auto& insert : inserts) {
            if (insert->sideChainSource == source && insert->sideChainSourceId == id) {
                insert->sideChainSource = pluginrack::SideChainSource::None;
                insert->sideChainSourceId = 0;
            }
        }
    };
    for (auto& channel : channels) clear(channel->inserts);
    for (auto& output : outputs) clear(output->inserts);
    for (auto& bus : buses) clear(bus->inserts);
    // The strips are gone (or one of them is): no cached side-chain pointer may
    // survive pointing at them.
    refreshAllSideChainCachesLocked();
}

void AudioEngine::removeBus(std::uint32_t busId) {
    std::vector<std::unique_ptr<Bus>> removed;
    {
        const juce::ScopedLock lock(stateLock);
        for (auto it = buses.begin(); it != buses.end();) {
            if ((*it)->id == busId) {
                removed.push_back(std::move(*it));
                it = buses.erase(it);
            } else {
                ++it;
            }
        }
        busSends.erase(std::remove_if(busSends.begin(), busSends.end(),
                                      [busId](const auto& send) { return send->busId == busId; }),
                       busSends.end());
        busRoutes.erase(std::remove_if(busRoutes.begin(), busRoutes.end(),
                                       [busId](const auto& route) { return route->busId == busId; }),
                        busRoutes.end());
        clearSideChainRefsLocked(pluginrack::SideChainSource::Bus, busId);
    }
    for (auto& bus : removed) retireStripInserts(*bus);
    if (!removed.empty()) bumpStructureRevision();
}

void AudioEngine::setBusSend(std::uint32_t channelId, std::uint32_t busId,
                             float gainDbL, float gainDbR) {
    const juce::ScopedLock lock(stateLock);
    const bool off = gainDbL <= kSendOffDb && gainDbR <= kSendOffDb;
    for (auto it = busSends.begin(); it != busSends.end(); ++it) {
        if ((*it)->channelId != channelId || (*it)->busId != busId) continue;
        if (off) {
            busSends.erase(it);
            bumpStructureRevision();
            return;
        }
        (*it)->sendGainDbL = gainDbL;
        (*it)->sendGainDbR = gainDbR;
        bumpStructureRevision();
        return;
    }
    if (off || busSends.size() >= pluginrack::kMaxBusSends) return;
    if (findChannel(channelId) == nullptr || getBusById(busId) == nullptr) return;
    auto send = std::make_unique<BusSend>();
    send->channelId = channelId;
    send->busId = busId;
    send->sendGainDbL = gainDbL;
    send->sendGainDbR = gainDbR;
    busSends.push_back(std::move(send));
    bumpStructureRevision();
}

void AudioEngine::setBusTrim(std::uint32_t busId, float trimDb) {
    const juce::ScopedLock lock(stateLock);
    if (auto* bus = getBusById(busId)) {
        bus->trimDb = juce::jlimit(-12.0f, 12.0f, trimDb);
        bumpStructureRevision();
    }
}

void AudioEngine::setBusRoute(std::uint32_t busId, std::uint32_t outputId,
                              float gainDbL, float gainDbR) {
    const juce::ScopedLock lock(stateLock);
    const bool off = gainDbL <= kSendOffDb && gainDbR <= kSendOffDb;
    for (auto it = busRoutes.begin(); it != busRoutes.end(); ++it) {
        if ((*it)->busId != busId || (*it)->outputId != outputId) continue;
        if (off) {
            busRoutes.erase(it);
            bumpStructureRevision();
            return;
        }
        (*it)->sendGainDbL = gainDbL;
        (*it)->sendGainDbR = gainDbR;
        bumpStructureRevision();
        return;
    }
    if (off || busRoutes.size() >= pluginrack::kMaxBusRoutes) return;
    if (getBusById(busId) == nullptr || getOutputById(outputId) == nullptr) return;
    auto route = std::make_unique<BusRoute>();
    route->busId = busId;
    route->outputId = outputId;
    route->sendGainDbL = gainDbL;
    route->sendGainDbR = gainDbR;
    busRoutes.push_back(std::move(route));
    bumpStructureRevision();
}

int AudioEngine::tapLatencySamples(const Channel& channel, const pluginrack::Tap& tap,
                                   double sampleRate) {
    using Kind = pluginrack::Tap::Kind;
    if (tap.kind == Kind::Input) {
        return 0;
    }
    // Same per-item rules as chainLatencySamples (see chainItemLatencySamples
    // in AudioEngine.h); only the items up to and including the tapped one
    // have run by the time the tap fires. The tapped item resolves through the
    // shared identity rule (Scene.h firstChainItemIndex), exactly as the render
    // push does: ids are minted per family, so a later row in another family
    // can share the id and must not be the one this latency is measured at.
    if (tap.kind == Kind::Item) {
        const int index = pluginrack::firstChainItemIndex(channel.chain, tap.itemId);
        if (index >= 0) {
            int total = 0;
            for (int i = 0; i <= index; ++i) {
                total += chainItemLatencySamples(
                    channel, channel.chain[static_cast<std::size_t>(i)], sampleRate);
            }
            return total;
        }
    }
    // Output, or an item that left the chain: the send falls back to the end,
    // where the unchained-fallback effects have run too.
    return chainLatencySamples(channel, sampleRate);
}

int AudioEngine::stripLatencySamples(const Channel& strip) const {
    const juce::ScopedLock lock(stateLock);
    return chainLatencySamples(strip, currentSampleRate > 0.0 ? currentSampleRate : 48000.0);
}

int AudioEngine::stripLatencySamples(const Output& strip) const {
    const juce::ScopedLock lock(stateLock);
    return chainLatencySamples(strip, currentSampleRate > 0.0 ? currentSampleRate : 48000.0);
}

int AudioEngine::stripTapLatencySamples(const Channel& channel,
                                        const pluginrack::Tap& tap) const {
    const juce::ScopedLock lock(stateLock);
    return tapLatencySamples(
        channel, tap, currentSampleRate > 0.0 ? currentSampleRate : 48000.0);
}

void AudioEngine::ensureTapPoint(Channel& channel, const pluginrack::Tap& tap) {
    if (tap.kind == pluginrack::Tap::Kind::Output) {
        return;
    }
    if (findTapPoint(channel, tap) != nullptr) {
        return;
    }
    TapPoint point;
    point.tap = tap;
    point.ring.prepare(pluginrack::kMaxTapDelaySamples
                       + juce::jmax(currentBufferSize, maxBlockFrames_) + 2);
    // Cache the delay this point bills so sends/routes never re-walk the chain.
    point.cachedLatencySamples = tapLatencySamples(
        channel, tap, currentSampleRate > 0.0 ? currentSampleRate : 48000.0);
    channel.tapPoints.push_back(std::move(point));
}

AudioEngine::TapPoint* AudioEngine::findTapPoint(Channel& channel, const pluginrack::Tap& tap) {
    for (auto& point : channel.tapPoints) {
        if (point.tap == tap) {
            return &point;
        }
    }
    return nullptr;
}

const AudioEngine::TapPoint* AudioEngine::findTapPoint(const Channel& channel,
                                                       const pluginrack::Tap& tap) const {
    for (const auto& point : channel.tapPoints) {
        if (point.tap == tap) {
            return &point;
        }
    }
    return nullptr;
}

void AudioEngine::setBusSendTap(std::uint32_t channelId, std::uint32_t busId,
                                pluginrack::Tap tap) {
    const juce::ScopedLock lock(stateLock);
    for (auto& send : busSends) {
        if (send->channelId != channelId || send->busId != busId) continue;
        send->tap = tap;
        if (auto* channel = findChannel(channelId)) {
            ensureTapPoint(*channel, tap);
        }
        bumpStructureRevision();
        return;
    }
}

pluginrack::Tap AudioEngine::getBusSendTap(std::uint32_t channelId, std::uint32_t busId) const {
    const juce::ScopedLock lock(stateLock);
    for (const auto& send : busSends) {
        if (send->channelId == channelId && send->busId == busId) {
            return send->tap;
        }
    }
    return {};
}

pluginrack::Tap AudioEngine::getSendTap(std::uint32_t channelId, std::uint32_t outputId) const {
    const juce::ScopedLock lock(stateLock);
    for (const auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            return route->tap;
        }
    }
    return {};
}

void AudioEngine::setLatencyCompensationEnabled(bool enabled) {
    const juce::ScopedLock lock(stateLock);
    latencyCompensation_ = enabled;
}

bool AudioEngine::isLatencyCompensationEnabled() const {
    const juce::ScopedLock lock(stateLock);
    return latencyCompensation_;
}

bool AudioEngine::reloadInsert(std::uint32_t channelId, std::uint32_t insertId) {
    if (!getChildProcessDsp()) return false;

    // The old child is detached under stateLock and reaped only after the lock
    // is gone. render() holds stateLock across processBlock - which is where
    // submitBlock/fetchBlock run - so entering and leaving this section proves
    // the audio thread is no longer inside that host. Only then is it safe to
    // kill it: killAndReap() closes the Win32 events and unmaps the shared
    // views the block was using. The reload used to call stopChildHost() here,
    // which did both while the insert was still reachable by render().
    InsertSlot* insert = nullptr;
    juce::File pluginFile;
    int pluginUniqueId = 0;
    double sampleRate = 0.0;
    int bufferSize = 0;
    std::shared_ptr<PluginProcessHost> oldHost;
    {
        const juce::ScopedLock lock(stateLock);
        insert = findInsert(channelId, insertId);
        if (insert == nullptr) insert = findOutputInsert(channelId, insertId);
        if (insert == nullptr) return false;
        pluginFile = juce::File(insert->pluginPath);
        pluginUniqueId = insert->pluginUniqueId;
        sampleRate = currentSampleRate;
        bufferSize = currentBufferSize;
        oldHost = insert->pluginHost.detachChildHost();
    }

    // Outside the lock, before reaping: the blob has to come from the in-process
    // instance (detachChildHost left no child to ask, exactly as the old
    // stopChildHost did) and capturing state runs plugin code, which must not
    // stall the audio thread behind stateLock. reloadInsert runs on the message
    // thread, the only thread that removes inserts, so `insert` stays valid.
    const auto stateBase64 = insert->pluginHost.getStateAsBase64();
    if (oldHost != nullptr) oldHost->killAndReap();

    // The relaunch waits for a child process to load a plugin, so it stays
    // outside the lock too; the new child is published atomically by
    // loadChildHost, which is why no lock is needed around it.
    juce::String error;
    const bool ok = insert->pluginHost.loadChildHost(pluginFile, pluginUniqueId, stateBase64,
                                                     sampleRate, bufferSize, 20000, error);
    if (ok) {
        insert->failed = false;
        insert->failExitCode = 0;
        insert->faultRecorded = false;
        insert->latencySamples.store(insert->pluginHost.getLatencySamples());
        // The strip's plan caches this number: rebuild under stateLock so
        // render() never pairs the new report with the old total.
        const juce::ScopedLock lock(stateLock);
        if (auto* channel = findChannel(channelId)) {
            rebuildRenderPlan(*channel, currentSampleRate);
        } else if (auto* output = findOutput(channelId)) {
            rebuildRenderPlan(*output, currentSampleRate);
        }
    }
    return ok;
}

void AudioEngine::addOutputPluginAsync(std::uint32_t outputId, const juce::File& file,
                                       int pluginUniqueId, std::function<void(juce::String)> done) {
    {
        const juce::ScopedLock lock(stateLock);
        auto* output = findOutput(outputId);
        if (output == nullptr) {
            done("The output no longer exists.");
            return;
        }
        if (output->inserts.size() >= kMaxInserts) {
            done("This strip already holds " + juce::String(kMaxInserts)
                 + " VST inserts.");
            return;
        }
    }

    const double rate = currentSampleRate;
    const int block = currentBufferSize;
    auto alive = prepareAliveToken_;
    juce::Logger::writeToLog("plugin: loading " + file.getFileName());
    juce::Thread::launch([this, alive, outputId, file, pluginUniqueId, rate, block,
                          done = std::move(done)]() mutable {
        juce::String scanError;
        const auto descriptions = PluginHost::scanFile(file, scanError);
        juce::MessageManager::callAsync(
            [this, alive, outputId, file, pluginUniqueId, rate, block, descriptions, scanError,
             done = std::move(done)]() mutable {
                if (!alive->load(std::memory_order_acquire)) return;

                auto loaded = PluginHost::LoadedPlugin{};
                loaded.path = file.getFullPathName();
                int resolvedUniqueId = 0;
                if (descriptions.isEmpty()) {
                    loaded.error =
                        scanError.isNotEmpty()
                            ? scanError
                            : juce::String("No VST3 plugin was found in the selected file.");
                } else {
                    const auto chosen = chooseDescription(descriptions, pluginUniqueId);
                    resolvedUniqueId = chosen.uniqueId;
                    loaded = PluginHost::instantiate(chosen, file, rate, block);
                }
                if (loaded.error.isNotEmpty()) {
                    juce::Logger::writeToLog("plugin: failed - " + loaded.error);
                    done(loaded.error);
                    return;
                }

                auto insert = std::make_unique<InsertSlot>();
                // Size the side-chain scratch here too: prepareToPlayImpl only
                // sizes inserts that exist when the engine is prepared, and a
                // freshly added insert renders before the next prepare. An
                // unsized buffer means null read pointers in the render loop.
                insert->sideChainBuffer.setSize(2, juce::jmax(1, currentBufferSize), false, true,
                                                true);
                insert->pluginHost.prepareToPlay(currentSampleRate, currentBufferSize);
                insert->pluginHost.adopt(loaded);
                insert->pluginPath = loaded.path;
                insert->pluginName = loaded.name;
                insert->pluginUniqueId = resolvedUniqueId;
                insert->latencySamples.store(insert->pluginHost.getLatencySamples());

                if (getChildProcessDsp()) {
                    juce::String childError;
                    const bool ok = insert->pluginHost.loadChildHost(
                        juce::File(loaded.path), resolvedUniqueId,
                        insert->pluginHost.getStateAsBase64(), currentSampleRate,
                        currentBufferSize, 20000, childError);
                    juce::Logger::writeToLog(ok
                                                 ? "plugin: child host ready for " + loaded.name
                                                 : "plugin: child host unavailable for " + loaded.name
                                                       + " (" + childError + ")");
                }

                juce::String error;
                {
                    const juce::ScopedLock lock(stateLock);
                    auto* output = findOutput(outputId);
                    if (output == nullptr) {
                        error = "The output no longer exists.";
                    } else if (output->inserts.size() >= kMaxInserts) {
                        error = "This strip already holds " + juce::String(kMaxInserts)
                                + " VST inserts.";
                    } else {
                        insert->id = nextInsertId++;
                        const auto insertId = insert->id;
                        insert->crashLabel = makeCrashLabel(output->name, insertId,
                                                            insert->pluginName,
                                                            insert->pluginPath);
                        output->inserts.push_back(std::move(insert));
                        output->chain.push_back({pluginrack::ChainItemType::Insert, insertId});
                        rebuildRenderPlan(*output, currentSampleRate);
                        refreshAllSideChainCachesLocked();
                        // The insert (and its chain row) is now part of the
                        // saved scene.
                        bumpStructureRevision();
                    }
                }
                juce::Logger::writeToLog(error.isEmpty()
                                             ? "plugin: loaded " + loaded.name
                                             : "plugin: install failed - " + error);
                done(error);
            });
    });
}

void AudioEngine::removeOutputPlugin(std::uint32_t outputId, std::uint32_t insertId) {
    std::unique_ptr<InsertSlot> removed;
    {
        const juce::ScopedLock lock(stateLock);
        auto* output = findOutput(outputId);
        if (output == nullptr) {
            return;
        }
        auto& inserts = output->inserts;
        for (auto it = inserts.begin(); it != inserts.end(); ++it) {
            if ((*it)->id == insertId) {
                removed = std::move(*it);
                inserts.erase(it);
                output->chain.erase(std::remove_if(output->chain.begin(), output->chain.end(),
                                                   [insertId](const auto& item) {
                                                       return item.type == pluginrack::ChainItemType::Insert &&
                                                              item.id == insertId;
                                                   }),
                                    output->chain.end());
                break;
            }
        }
        if (removed != nullptr) {
            // The plan held a raw pointer to this slot; it must not outlive the
            // erase.
            rebuildRenderPlan(*output, currentSampleRate);
        }
    }
    if (removed == nullptr) {
        return;
    }
    // Reaped outside stateLock exactly like the channel path: unlinking the
    // insert under the lock is what makes it safe to stop here.
    removed->pluginHost.closeFloatingEditor();
    removed->pluginHost.stopChildHost();
    removed->pluginHost.releaseResources();
    bumpStructureRevision();
}

void AudioEngine::moveOutputChainItem(std::uint32_t outputId, pluginrack::ChainItem item,
                                      int targetIndex) {
    const juce::ScopedLock lock(stateLock);
    auto* output = findOutput(outputId);
    if (output == nullptr) return;
    auto& chain = output->chain;
    pluginrack::moveChainItem(chain, item, targetIndex);
    rebuildRenderPlan(*output, currentSampleRate);
    bumpStructureRevision();
}

void AudioEngine::setOutputPluginBypassed(std::uint32_t outputId, std::uint32_t insertId, bool bypassed) {
    // Same split as setPluginBypassed: the child-process latency query runs
    // outside stateLock, only the cached total's refresh holds it.
    auto* insert = findOutputInsert(outputId, insertId);
    if (insert == nullptr) return;
    insert->bypassed = bypassed;
    insert->pluginHost.refreshLatencyCache();
    insert->latencySamples.store(insert->pluginHost.getLatencySamples());
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) {
        rebuildRenderPlan(*output, currentSampleRate);
        // The bypass flag is part of the saved insert.
        bumpStructureRevision();
    }
}

void AudioEngine::setOutputPluginSideChain(std::uint32_t outputId, std::uint32_t insertId,
                                           pluginrack::SideChainSource source, std::uint32_t sourceId,
                                           pluginrack::SideChainTap tap, float gainDb) {
    const juce::ScopedLock lock(stateLock);
    if (auto* insert = findOutputInsert(outputId, insertId)) {
        insert->sideChainSource = source;
        insert->sideChainSourceId = source == pluginrack::SideChainSource::None ? 0 : sourceId;
        insert->sideChainTap = tap;
        insert->sideChainGainDb = juce::jlimit(-60.0f, 12.0f, gainDb);
        bool mono = false;
        insert->sideChainSourceBuffer = resolveSideChainSource(*insert, mono);
        insert->sideChainSourceMono = mono;
        // Side-chain routing is persisted with the insert (schema 8).
        bumpStructureRevision();
    }
}

void AudioEngine::setChannelGain(std::uint32_t channelId, float gainDb) {
    if (auto* channel = findChannel(channelId)) {
        channel->gainDb = gainDb;
        bumpStructureRevision();
    }
}

void AudioEngine::setChannelTrim(std::uint32_t channelId, float trimDb) {
    if (auto* channel = findChannel(channelId)) {
        channel->trimDb = trimDb;
        bumpStructureRevision();
    }
}

void AudioEngine::setChannelPan(std::uint32_t channelId, float pan) {
    if (auto* channel = findChannel(channelId)) {
        channel->pan = pan;
        bumpStructureRevision();
    }
}

void AudioEngine::setChannelName(std::uint32_t channelId, const juce::String& name) {
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) {
        channel->name = name;
        bumpStructureRevision();
    }
}

void AudioEngine::setChannelMuted(std::uint32_t channelId, bool muted) {
    if (auto* channel = findChannel(channelId)) {
        channel->muted = muted;
        bumpStructureRevision();
    }
}

void AudioEngine::setChannelSoloed(std::uint32_t channelId, bool soloed) {
    if (auto* channel = findChannel(channelId)) {
        channel->soloed = soloed;
        bumpStructureRevision();
    }
}

void AudioEngine::setChannelLayout(std::uint32_t channelId, pluginrack::ChannelLayout layout) {
    juce::String deviceName;
    {
        const juce::ScopedLock lock(stateLock);
        auto* channel = findChannel(channelId);
        if (channel == nullptr) {
            return;
        }
        channel->layout = layout;
        deviceName = channel->inputDeviceName;
        // A mono/stereo change alters what every side chain keyed off this
        // strip feeds (the mono flag decides whether channel 0 is duplicated).
        refreshAllSideChainCachesLocked();
    }
    // Re-assign the device so the shared capture is re-opened (or widened)
    // for the new layout; silent channels keep nothing to reopen.
    if (!isSilentDeviceName(deviceName)) {
        // No bump inside: the whole edit is counted once, below.
        reassignChannelInputDevice(channelId, deviceName);
    }
    bumpStructureRevision();
}

void AudioEngine::setChannelInputChannels(std::uint32_t channelId, const std::vector<int>& inputChannels) {
    juce::String deviceName;
    {
        const juce::ScopedLock lock(stateLock);
        auto* channel = findChannel(channelId);
        if (channel == nullptr) {
            return;
        }
        channel->inputChannels = inputChannels;
        deviceName = channel->inputDeviceName;
    }
    if (!isSilentDeviceName(deviceName)) {
        // No bump inside: the whole edit is counted once, below.
        reassignChannelInputDevice(channelId, deviceName);
    }
    bumpStructureRevision();
}

void AudioEngine::setChannelMapping(std::uint32_t channelId, pluginrack::ChannelLayout layout, const std::vector<int>& inputChannels) {
    juce::String deviceName;
    {
        const juce::ScopedLock lock(stateLock);
        auto* channel = findChannel(channelId);
        if (channel == nullptr) return;
        channel->layout = layout;
        channel->inputChannels = inputChannels;
        deviceName = channel->inputDeviceName;
        refreshAllSideChainCachesLocked();
    }
    if (!isSilentDeviceName(deviceName)) {
        // No bump inside: the whole edit is counted once, below.
        reassignChannelInputDevice(channelId, deviceName);
    }
    bumpStructureRevision();
}

void AudioEngine::setSendGain(std::uint32_t channelId, std::uint32_t outputId, float gainDb) {
    for (auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            route->sendGainDbL = gainDb;
            route->sendGainDbR = gainDb;
            bumpStructureRevision();
            return;
        }
    }
}

void AudioEngine::setSendGainL(std::uint32_t channelId, std::uint32_t outputId, float gainDb) {
    for (auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            route->sendGainDbL = gainDb;
            bumpStructureRevision();
            return;
        }
    }
}

void AudioEngine::setSendGainR(std::uint32_t channelId, std::uint32_t outputId, float gainDb) {
    for (auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            route->sendGainDbR = gainDb;
            bumpStructureRevision();
            return;
        }
    }
}

void AudioEngine::connectSend(std::uint32_t channelId, std::uint32_t outputId,
                              const pluginrack::Tap& tap) {
    const juce::ScopedLock lock(stateLock);
    if (findChannel(channelId) == nullptr || getOutputById(outputId) == nullptr) {
        return;
    }
    for (auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            route->sendGainDbL = 0.0f;
            route->sendGainDbR = 0.0f;
            route->tap = tap;
            ensureTapPoint(*findChannel(channelId), tap);
            bumpStructureRevision();
            return;
        }
    }
    if (routes.size() >= static_cast<std::size_t>(pluginrack::kMaxRoutes)) {
        return;
    }
    auto route = std::make_unique<Route>();
    route->channelId = channelId;
    route->outputId = outputId;
    route->sendGainDbL = 0.0f;
    route->sendGainDbR = 0.0f;
    route->tap = tap;
    ensureTapPoint(*findChannel(channelId), tap);
    routes.push_back(std::move(route));
    bumpStructureRevision();
}

void AudioEngine::removeSend(std::uint32_t channelId, std::uint32_t outputId) {
    const juce::ScopedLock lock(stateLock);
    const auto before = routes.size();
    routes.erase(std::remove_if(routes.begin(), routes.end(),
                                [channelId, outputId](const auto& route) {
                                    return route->channelId == channelId
                                           && route->outputId == outputId;
                                }),
                 routes.end());
    if (routes.size() != before) bumpStructureRevision();
}

void AudioEngine::setOutputGain(std::uint32_t outputId, float gainDb) {
    if (auto* output = findOutput(outputId)) {
        output->gainDb = gainDb;
        bumpStructureRevision();
    }
}

void AudioEngine::setOutputMuted(std::uint32_t outputId, bool muted) {
    if (auto* output = findOutput(outputId)) {
        output->muted = muted;
        bumpStructureRevision();
    }
}

void AudioEngine::setOutputName(std::uint32_t outputId, const juce::String& name) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) {
        output->name = name;
        bumpStructureRevision();
    }
}

void AudioEngine::setOutputLayout(std::uint32_t outputId, pluginrack::ChannelLayout layout) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) {
        output->layout = layout;
        // Side chains keyed off this strip read the mono flag from the cache.
        refreshAllSideChainCachesLocked();
        bumpStructureRevision();
    }
}

void AudioEngine::setOutputFlipped(std::uint32_t outputId, bool flipped) {
    if (auto* output = findOutput(outputId)) {
        output->flipped = flipped;
        bumpStructureRevision();
    }
}

void AudioEngine::setChannelEq(std::uint32_t channelId, const std::vector<pluginrack::EqBand>& bands) {
    // Message thread: stateLock serializes the band copy and the coefficient
    // rebuild against render()'s taps (mirrors core's EqState mutex). Bypass
    // state is kept, like core's setChannelEq.
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) {
        channel->eqBands = bands;
        channel->eq.setBands(channel->eqBands);
        bumpStructureRevision();
    }
}

void AudioEngine::setChannelEqBypassed(std::uint32_t channelId, bool bypassed) {
    // Live: no re-prepare needed. The reset under stateLock cannot race the
    // render tap, so re-enabling starts from clean filter state, like core.
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) {
        channel->eqBypassed = bypassed;
        if (bypassed) {
            channel->eq.reset();
        }
        // The EQ's delay line is part of the strip latency the plan caches.
        rebuildRenderPlan(*channel, currentSampleRate);
        bumpStructureRevision();
    }
}

void AudioEngine::setChannelEqDelay(std::uint32_t channelId, float delayMs) {
    // Message thread: same lock as setChannelEq so the allocation the first
    // non-zero delay performs cannot race render()'s EQ tap.
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) {
        channel->eqDelayMs = delayMs;
        channel->eq.setDelayMs(delayMs);
        rebuildRenderPlan(*channel, currentSampleRate);
        bumpStructureRevision();
    }
}

void AudioEngine::setChannelEqPhaseFlip(std::uint32_t channelId, bool flipped) {
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) {
        channel->eqPhaseFlip = flipped;
        channel->eq.setPhaseFlip(flipped);
        bumpStructureRevision();
    }
}

void AudioEngine::setChannelColourHue(std::uint32_t channelId, float hueDegrees) {
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) {
        channel->colourHue = hueDegrees;
        bumpStructureRevision();
    }
}

void AudioEngine::setChannelEqRtaOffset(std::uint32_t channelId, float offsetDb) {
    // Display only, but it is saved with the scene, so it follows the same
    // message-thread lock rule as the bands.
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) {
        channel->eqRtaOffsetDb = offsetDb;
        bumpStructureRevision();
    }
}

void AudioEngine::setChannelEqRtaWeighting(std::uint32_t channelId, int weighting) {
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) {
        channel->eqRtaWeighting = juce::jlimit(0, 2, weighting);
        bumpStructureRevision();
    }
}

void AudioEngine::setChannelMultibandRtaOffset(std::uint32_t channelId, float offsetDb) {
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) {
        channel->multibandRtaOffsetDb = offsetDb;
        bumpStructureRevision();
    }
}

void AudioEngine::setChannelMultibandRtaWeighting(std::uint32_t channelId, int weighting) {
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) {
        channel->multibandRtaWeighting = juce::jlimit(0, 2, weighting);
        bumpStructureRevision();
    }
}

void AudioEngine::setChannelDeEsserRtaOffset(std::uint32_t channelId, float offsetDb) {
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) {
        channel->deEsserRtaOffsetDb = offsetDb;
        bumpStructureRevision();
    }
}

void AudioEngine::setChannelDeEsserRtaWeighting(std::uint32_t channelId, int weighting) {
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) {
        channel->deEsserRtaWeighting = juce::jlimit(0, 2, weighting);
        bumpStructureRevision();
    }
}

void AudioEngine::setChannelFeedbackSuppressorRtaOffset(std::uint32_t channelId, float offsetDb) {
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) {
        channel->feedbackSuppressorRtaOffsetDb = offsetDb;
        bumpStructureRevision();
    }
}

void AudioEngine::setChannelFeedbackSuppressorRtaWeighting(std::uint32_t channelId, int weighting) {
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) {
        channel->feedbackSuppressorRtaWeighting = juce::jlimit(0, 2, weighting);
        bumpStructureRevision();
    }
}

void AudioEngine::setOutputEq(std::uint32_t outputId,
                              const std::vector<pluginrack::EqBand>& bands) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) {
        output->eqBands = bands;
        output->eq.setBands(output->eqBands);
        bumpStructureRevision();
    }
}

void AudioEngine::setOutputEqBypassed(std::uint32_t outputId, bool bypassed) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) {
        output->eqBypassed = bypassed;
        if (bypassed) {
            output->eq.reset();
        }
        rebuildRenderPlan(*output, currentSampleRate);
        bumpStructureRevision();
    }
}

void AudioEngine::setOutputEqDelay(std::uint32_t outputId, float delayMs) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) {
        output->eqDelayMs = delayMs;
        output->eq.setDelayMs(delayMs);
        rebuildRenderPlan(*output, currentSampleRate);
        bumpStructureRevision();
    }
}

void AudioEngine::setOutputEqPhaseFlip(std::uint32_t outputId, bool flipped) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) {
        output->eqPhaseFlip = flipped;
        output->eq.setPhaseFlip(flipped);
        bumpStructureRevision();
    }
}

void AudioEngine::setOutputColourHue(std::uint32_t outputId, float hueDegrees) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) {
        output->colourHue = hueDegrees;
        bumpStructureRevision();
    }
}

void AudioEngine::setOutputEqRtaOffset(std::uint32_t outputId, float offsetDb) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) {
        output->eqRtaOffsetDb = offsetDb;
        bumpStructureRevision();
    }
}

void AudioEngine::setOutputEqRtaWeighting(std::uint32_t outputId, int weighting) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) {
        output->eqRtaWeighting = juce::jlimit(0, 2, weighting);
        bumpStructureRevision();
    }
}

void AudioEngine::setOutputMultibandRtaOffset(std::uint32_t outputId, float offsetDb) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) {
        output->multibandRtaOffsetDb = offsetDb;
        bumpStructureRevision();
    }
}

void AudioEngine::setOutputMultibandRtaWeighting(std::uint32_t outputId, int weighting) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) {
        output->multibandRtaWeighting = juce::jlimit(0, 2, weighting);
        bumpStructureRevision();
    }
}

void AudioEngine::setOutputDeEsserRtaOffset(std::uint32_t outputId, float offsetDb) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) {
        output->deEsserRtaOffsetDb = offsetDb;
        bumpStructureRevision();
    }
}

void AudioEngine::setOutputDeEsserRtaWeighting(std::uint32_t outputId, int weighting) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) {
        output->deEsserRtaWeighting = juce::jlimit(0, 2, weighting);
        bumpStructureRevision();
    }
}

void AudioEngine::setOutputFeedbackSuppressorRtaOffset(std::uint32_t outputId, float offsetDb) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) {
        output->feedbackSuppressorRtaOffsetDb = offsetDb;
        bumpStructureRevision();
    }
}

void AudioEngine::setOutputFeedbackSuppressorRtaWeighting(std::uint32_t outputId, int weighting) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) {
        output->feedbackSuppressorRtaWeighting = juce::jlimit(0, 2, weighting);
        bumpStructureRevision();
    }
}

void AudioEngine::setChannelEffects(
    std::uint32_t channelId, const std::vector<pluginrack::DynamicsEffect>& effects) {
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) return;
    channel->effects = effects;
    // Rebuild only when the count changes: recreating the processors resets
    // their envelopes, which clicks on every knob move.
    if (channel->effectProcessors.size() == effects.size()) {
        for (std::size_t i = 0; i < effects.size(); ++i)
            channel->effectProcessors[i].setEffect(effects[i]);
        // Bypass flags and look-ahead settings changed: the plan's step indices
        // and the strip latency billing must follow.
        rebuildRenderPlan(*channel, currentSampleRate);
        bumpStructureRevision();
        return;
    }
    channel->effectProcessors.clear();
    channel->effectProcessors.reserve(effects.size());
    for (const auto& effect : effects) {
        pluginrack::DynamicsProcessor processor;
        processor.prepare(currentSampleRate);
        processor.setEffect(effect);
        channel->effectProcessors.push_back(std::move(processor));
    }
    rebuildRenderPlan(*channel, currentSampleRate);
    bumpStructureRevision();
}

void AudioEngine::setOutputEffects(
    std::uint32_t outputId, const std::vector<pluginrack::DynamicsEffect>& effects) {
    const juce::ScopedLock lock(stateLock);
    auto* output = findOutput(outputId);
    if (output == nullptr) return;
    output->effects = effects;
    if (output->effectProcessors.size() == effects.size()) {
        for (std::size_t i = 0; i < effects.size(); ++i)
            output->effectProcessors[i].setEffect(effects[i]);
        rebuildRenderPlan(*output, currentSampleRate);
        bumpStructureRevision();
        return;
    }
    output->effectProcessors.clear();
    output->effectProcessors.reserve(effects.size());
    for (const auto& effect : effects) {
        pluginrack::DynamicsProcessor processor;
        processor.prepare(currentSampleRate);
        processor.setEffect(effect);
        output->effectProcessors.push_back(std::move(processor));
    }
    rebuildRenderPlan(*output, currentSampleRate);
    bumpStructureRevision();
}

template <typename Effect, typename Processor>
void syncNewEffects(const std::vector<Effect>& effects,
                    std::vector<Effect>& stored, std::vector<Processor>& processors,
                    double rate) {
    stored = effects;
    if (processors.size() == effects.size()) {
        for (std::size_t i = 0; i < effects.size(); ++i) processors[i].setEffect(effects[i]);
        return;
    }
    processors.clear();
    processors.reserve(effects.size());
    for (const auto& effect : effects) {
        Processor processor;
        processor.prepare(rate);
        processor.setEffect(effect);
        processors.push_back(std::move(processor));
    }
}

void AudioEngine::setChannelMultiband(std::uint32_t channelId, const std::vector<pluginrack::MultibandEffect>& effects) {
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) return;
    syncNewEffects(effects, channel->multibandEffects, channel->multibandProcessors, currentSampleRate);
    rebuildRenderPlan(*channel, currentSampleRate);
    bumpStructureRevision();
}
void AudioEngine::setOutputMultiband(std::uint32_t outputId, const std::vector<pluginrack::MultibandEffect>& effects) {
    const juce::ScopedLock lock(stateLock);
    auto* output = findOutput(outputId);
    if (output == nullptr) return;
    syncNewEffects(effects, output->multibandEffects, output->multibandProcessors, currentSampleRate);
    rebuildRenderPlan(*output, currentSampleRate);
    bumpStructureRevision();
}
void AudioEngine::setChannelDeEssers(std::uint32_t channelId, const std::vector<pluginrack::DeEsserEffect>& effects) {
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) return;
    syncNewEffects(effects, channel->deEsserEffects, channel->deEsserProcessors, currentSampleRate);
    rebuildRenderPlan(*channel, currentSampleRate);
    bumpStructureRevision();
}
void AudioEngine::setOutputDeEssers(std::uint32_t outputId, const std::vector<pluginrack::DeEsserEffect>& effects) {
    const juce::ScopedLock lock(stateLock);
    auto* output = findOutput(outputId);
    if (output == nullptr) return;
    syncNewEffects(effects, output->deEsserEffects, output->deEsserProcessors, currentSampleRate);
    rebuildRenderPlan(*output, currentSampleRate);
    bumpStructureRevision();
}
void AudioEngine::setChannelLimiters(std::uint32_t channelId, const std::vector<pluginrack::LimiterEffect>& effects) {
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) return;
    syncNewEffects(effects, channel->limiterEffects, channel->limiterProcessors, currentSampleRate);
    rebuildRenderPlan(*channel, currentSampleRate);
    bumpStructureRevision();
}
void AudioEngine::setOutputLimiters(std::uint32_t outputId, const std::vector<pluginrack::LimiterEffect>& effects) {
    const juce::ScopedLock lock(stateLock);
    auto* output = findOutput(outputId);
    if (output == nullptr) return;
    syncNewEffects(effects, output->limiterEffects, output->limiterProcessors, currentSampleRate);
    rebuildRenderPlan(*output, currentSampleRate);
    bumpStructureRevision();
}
void AudioEngine::setChannelFeedbackSuppressors(std::uint32_t channelId, const std::vector<pluginrack::FeedbackSuppressorEffect>& effects) {
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) return;
    syncNewEffects(effects, channel->feedbackSuppressorEffects, channel->feedbackSuppressorProcessors, currentSampleRate);
    rebuildRenderPlan(*channel, currentSampleRate);
    bumpStructureRevision();
}
void AudioEngine::setOutputFeedbackSuppressors(std::uint32_t outputId, const std::vector<pluginrack::FeedbackSuppressorEffect>& effects) {
    const juce::ScopedLock lock(stateLock);
    auto* output = findOutput(outputId);
    if (output == nullptr) return;
    syncNewEffects(effects, output->feedbackSuppressorEffects, output->feedbackSuppressorProcessors, currentSampleRate);
    rebuildRenderPlan(*output, currentSampleRate);
    bumpStructureRevision();
}
void AudioEngine::setChannelSpectralDenoisers(std::uint32_t channelId, const std::vector<pluginrack::SpectralDenoiserEffect>& effects) {
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) return;
    syncNewEffects(effects, channel->spectralDenoiserEffects, channel->spectralDenoiserProcessors, currentSampleRate);
    rebuildRenderPlan(*channel, currentSampleRate);
    bumpStructureRevision();
}
void AudioEngine::setOutputSpectralDenoisers(std::uint32_t outputId, const std::vector<pluginrack::SpectralDenoiserEffect>& effects) {
    const juce::ScopedLock lock(stateLock);
    auto* output = findOutput(outputId);
    if (output == nullptr) return;
    syncNewEffects(effects, output->spectralDenoiserEffects, output->spectralDenoiserProcessors, currentSampleRate);
    rebuildRenderPlan(*output, currentSampleRate);
    bumpStructureRevision();
}

namespace {
template <typename Effects, typename Processors>
std::size_t denoiserIndex(const Effects& effects, const Processors& processors, std::uint32_t effectId) {
    const std::size_t count = std::min(effects.size(), processors.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (effects[i].id == effectId) return i;
    }
    return count;
}
} // namespace

void AudioEngine::startChannelDenoiserLearn(std::uint32_t channelId, std::uint32_t effectId) {
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) return;
    const auto index = denoiserIndex(channel->spectralDenoiserEffects,
                                     channel->spectralDenoiserProcessors, effectId);
    if (index < channel->spectralDenoiserProcessors.size())
        channel->spectralDenoiserProcessors[index].startLearning();
}
void AudioEngine::startOutputDenoiserLearn(std::uint32_t outputId, std::uint32_t effectId) {
    const juce::ScopedLock lock(stateLock);
    auto* output = findOutput(outputId);
    if (output == nullptr) return;
    const auto index = denoiserIndex(output->spectralDenoiserEffects,
                                     output->spectralDenoiserProcessors, effectId);
    if (index < output->spectralDenoiserProcessors.size())
        output->spectralDenoiserProcessors[index].startLearning();
}
float AudioEngine::getChannelDenoiserLearnProgress(std::uint32_t channelId, std::uint32_t effectId) {
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) return 0.0f;
    const auto index = denoiserIndex(channel->spectralDenoiserEffects,
                                     channel->spectralDenoiserProcessors, effectId);
    return index < channel->spectralDenoiserProcessors.size()
        ? channel->spectralDenoiserProcessors[index].learnProgress() : 0.0f;
}
float AudioEngine::getOutputDenoiserLearnProgress(std::uint32_t outputId, std::uint32_t effectId) {
    const juce::ScopedLock lock(stateLock);
    auto* output = findOutput(outputId);
    if (output == nullptr) return 0.0f;
    const auto index = denoiserIndex(output->spectralDenoiserEffects,
                                     output->spectralDenoiserProcessors, effectId);
    return index < output->spectralDenoiserProcessors.size()
        ? output->spectralDenoiserProcessors[index].learnProgress() : 0.0f;
}
bool AudioEngine::takeChannelDenoiserLearnedProfile(std::uint32_t channelId, std::uint32_t effectId,
                                                    std::vector<float>& profile, double& sampleRate) {
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) return false;
    const auto index = denoiserIndex(channel->spectralDenoiserEffects,
                                     channel->spectralDenoiserProcessors, effectId);
    return index < channel->spectralDenoiserProcessors.size()
        && channel->spectralDenoiserProcessors[index].takeLearnedProfile(profile, sampleRate);
}
bool AudioEngine::takeOutputDenoiserLearnedProfile(std::uint32_t outputId, std::uint32_t effectId,
                                                   std::vector<float>& profile, double& sampleRate) {
    const juce::ScopedLock lock(stateLock);
    auto* output = findOutput(outputId);
    if (output == nullptr) return false;
    const auto index = denoiserIndex(output->spectralDenoiserEffects,
                                     output->spectralDenoiserProcessors, effectId);
    return index < output->spectralDenoiserProcessors.size()
        && output->spectralDenoiserProcessors[index].takeLearnedProfile(profile, sampleRate);
}

std::array<float, pluginrack::kSpectralDenoiserTrimBandCount>
AudioEngine::getChannelDenoiserBandReduction(std::uint32_t channelId, std::uint32_t effectId) {
    std::array<float, pluginrack::kSpectralDenoiserTrimBandCount> bands{};
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) return bands;
    const auto index = denoiserIndex(channel->spectralDenoiserEffects,
                                     channel->spectralDenoiserProcessors, effectId);
    if (index >= channel->spectralDenoiserProcessors.size()) return bands;
    for (int band = 0; band < pluginrack::kSpectralDenoiserTrimBandCount; ++band) {
        bands[static_cast<std::size_t>(band)] =
            channel->spectralDenoiserProcessors[index].bandReductionDb(band);
    }
    return bands;
}

std::array<float, pluginrack::kSpectralDenoiserTrimBandCount>
AudioEngine::getOutputDenoiserBandReduction(std::uint32_t outputId, std::uint32_t effectId) {
    std::array<float, pluginrack::kSpectralDenoiserTrimBandCount> bands{};
    const juce::ScopedLock lock(stateLock);
    auto* output = findOutput(outputId);
    if (output == nullptr) return bands;
    const auto index = denoiserIndex(output->spectralDenoiserEffects,
                                     output->spectralDenoiserProcessors, effectId);
    if (index >= output->spectralDenoiserProcessors.size()) return bands;
    for (int band = 0; band < pluginrack::kSpectralDenoiserTrimBandCount; ++band) {
        bands[static_cast<std::size_t>(band)] =
            output->spectralDenoiserProcessors[index].bandReductionDb(band);
    }
    return bands;
}

float AudioEngine::getSendGain(std::uint32_t channelId, std::uint32_t outputId) const {
    for (const auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            return route->sendGainDbL.get();
        }
    }
    return kSendOffDb;
}

float AudioEngine::getSendGainL(std::uint32_t channelId, std::uint32_t outputId) const {
    for (const auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            return route->sendGainDbL.get();
        }
    }
    return kSendOffDb;
}

float AudioEngine::getSendGainR(std::uint32_t channelId, std::uint32_t outputId) const {
    for (const auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            return route->sendGainDbR.get();
        }
    }
    return kSendOffDb;
}

const std::vector<std::unique_ptr<AudioEngine::Channel>>& AudioEngine::getChannels() const {
    return channels;
}

double AudioEngine::getSampleRate() const {
    const juce::ScopedLock lock(stateLock);
    return currentSampleRate;
}

const std::vector<std::unique_ptr<AudioEngine::Output>>& AudioEngine::getOutputs() const {
    return outputs;
}

const std::vector<std::unique_ptr<AudioEngine::Bus>>& AudioEngine::getBuses() const {
    return buses;
}

const std::vector<std::unique_ptr<AudioEngine::BusSend>>& AudioEngine::getBusSends() const {
    return busSends;
}

const std::vector<std::unique_ptr<AudioEngine::BusRoute>>& AudioEngine::getBusRoutes() const {
    return busRoutes;
}

float AudioEngine::getSideChainKeyPeak(std::uint32_t ownerId, std::uint32_t insertId,
                                       bool isOutput) const {
    const juce::ScopedLock lock(stateLock);
    const auto findPeak = [insertId](const std::vector<std::unique_ptr<InsertSlot>>& inserts) -> float {
        for (const auto& insert : inserts) {
            if (insert != nullptr && insert->id == insertId) return insert->sideChainKeyPeak.get();
        }
        return 0.0f;
    };
    if (isOutput) {
        for (const auto& output : outputs) {
            if (output != nullptr && output->id == ownerId) return findPeak(output->inserts);
        }
        for (const auto& bus : buses) {
            if (bus != nullptr && bus->id == ownerId) return findPeak(bus->inserts);
        }
    } else {
        for (const auto& channel : channels) {
            if (channel != nullptr && channel->id == ownerId) return findPeak(channel->inserts);
        }
    }
    return 0.0f;
}

const std::vector<std::unique_ptr<AudioEngine::Route>>& AudioEngine::getRoutes() const {
    return routes;
}

namespace {

// The live strip and the saved strip keep the same names for everything they
// share, but the live one guards the audio-thread fields with atomics. Copying
// the strip's own half in one place keeps a new field from being forgotten on
// one side (the output side had already lost its OutputKind).
template <typename LiveStrip, typename SavedStrip>
void copyStripFields(const LiveStrip& from, SavedStrip& to) {
    to.id = from.id;
    to.name = from.name.toStdString();
    to.layout = from.layout;
    to.gainDb = from.gainDb.get();
    to.muted = from.muted.get();
    to.eqEnabled = !from.eqBypassed.get();
    to.eqBands = from.eqBands;
    to.eqDelayMs = from.eqDelayMs;
    to.eqPhaseFlip = from.eqPhaseFlip.get();
    to.colourHue = from.colourHue;
    to.eqRtaOffsetDb = from.eqRtaOffsetDb;
    to.eqRtaWeighting = from.eqRtaWeighting;
    to.multibandRtaOffsetDb = from.multibandRtaOffsetDb;
    to.multibandRtaWeighting = from.multibandRtaWeighting;
    to.deEsserRtaOffsetDb = from.deEsserRtaOffsetDb;
    to.deEsserRtaWeighting = from.deEsserRtaWeighting;
    to.feedbackSuppressorRtaOffsetDb = from.feedbackSuppressorRtaOffsetDb;
    to.feedbackSuppressorRtaWeighting = from.feedbackSuppressorRtaWeighting;
    to.effects = from.effects;
    to.multibandEffects = from.multibandEffects;
    to.deEsserEffects = from.deEsserEffects;
    to.limiterEffects = from.limiterEffects;
    to.feedbackSuppressorEffects = from.feedbackSuppressorEffects;
    to.spectralDenoiserEffects = from.spectralDenoiserEffects;
    to.chain = from.chain;
}

// The host pointers stay live until capture finishes; the slot copy carries
// only what the scene file stores.
template <typename LiveInserts>
std::vector<const PluginHost*> copyInserts(const LiveInserts& inserts,
                                           std::vector<pluginrack::InsertSlot>& saved,
                                           bool includePluginState = true) {
    std::vector<const PluginHost*> hosts;
    hosts.reserve(inserts.size());
    for (const auto& insert : inserts) {
        pluginrack::InsertSlot slot;
        slot.id = insert->id;
        slot.pluginPath = insert->pluginPath.toStdString();
        slot.pluginName = insert->pluginName.toStdString();
        slot.pluginUniqueId = insert->pluginUniqueId;
        slot.bypassed = insert->bypassed.get();
        slot.sideChainSource = insert->sideChainSource;
        slot.sideChainSourceId = insert->sideChainSourceId;
        slot.sideChainTap = insert->sideChainTap;
        slot.sideChainGainDb = insert->sideChainGainDb.get();
        // A failed insert has no live instance to ask for state; write back
        // the blob the scene supplied. The structural token used for change
        // detection skips it: a missing plugin's blob can be large and is
        // already covered by the full capture taken when a save is due.
        if (includePluginState) slot.pluginState = insert->savedPluginState.toStdString();
        saved.push_back(std::move(slot));
        hosts.push_back(&insert->pluginHost);
    }
    return hosts;
}

} // namespace

pluginrack::Scene AudioEngine::makeScene(bool includePluginState) const {
    pluginrack::Scene scene;
    // Plugin state capture is plugin code of unbounded length, and render()
    // only waits a few yields for stateLock before it pushes silence. Record
    // the hosts under the lock and ask them for their state after it is
    // released. Safe because makeScene and every structural edit run on the
    // message thread, so no slot can be reaped between the record and the
    // capture: removePlugin frees the removed slot once it has released the
    // lock, but it cannot start until this message-thread call returns.
    std::vector<std::vector<const PluginHost*>> channelHosts;
    std::vector<std::vector<const PluginHost*>> outputHosts;
    std::vector<std::vector<const PluginHost*>> busHosts;
    {
        const juce::ScopedLock lock(stateLock);
        scene.name = "Default";
        scene.sampleRate = currentSampleRate;
        scene.bufferFrames = currentBufferSize;
        for (const auto& channel : channels) {
            pluginrack::Channel savedChannel;
            copyStripFields(*channel, savedChannel);
            savedChannel.sourceKind = channel->sourceKind;
            savedChannel.sourceDeviceId = channel->sourceDeviceId;
            savedChannel.inputChannels = channel->inputChannels;
            savedChannel.trimDb = channel->trimDb.get();
            savedChannel.pan = channel->pan.get();
            savedChannel.soloed = channel->soloed.get();
            channelHosts.push_back(
                copyInserts(channel->inserts, savedChannel.inserts, includePluginState));
            scene.channels.push_back(std::move(savedChannel));
        }
        for (const auto& output : outputs) {
            pluginrack::Output savedOutput;
            copyStripFields(*output, savedOutput);
            // The live engine models physical endpoints only; a saved output
            // therefore stays OutputKind::Physical.
            savedOutput.deviceId = output->outputDeviceName.toStdString();
            savedOutput.flipped = output->flipped.get();
            outputHosts.push_back(
                copyInserts(output->inserts, savedOutput.inserts, includePluginState));
            scene.outputs.push_back(std::move(savedOutput));
        }
        for (const auto& route : routes) {
            pluginrack::Route savedRoute;
            savedRoute.channelId = route->channelId;
            savedRoute.outputId = route->outputId;
            savedRoute.sendGainDb = route->sendGainDbL.get();
            const float rightDb = route->sendGainDbR.get();
            // Only a split pair needs the extra key; a matched send stores once.
            if (rightDb != savedRoute.sendGainDb) {
                savedRoute.sendGainDbR = rightDb;
            }
            savedRoute.tap = route->tap;
            scene.routes.push_back(std::move(savedRoute));
        }
        for (const auto& bus : buses) {
            pluginrack::Bus savedBus;
            copyStripFields(*bus, savedBus);
            savedBus.trimDb = bus->trimDb.get();
            busHosts.push_back(
                copyInserts(bus->inserts, savedBus.inserts, includePluginState));
            scene.buses.push_back(std::move(savedBus));
        }
        for (const auto& send : busSends) {
            pluginrack::BusSend savedSend;
            savedSend.channelId = send->channelId;
            savedSend.busId = send->busId;
            savedSend.sendGainDb = send->sendGainDbL.get();
            const float rightDb = send->sendGainDbR.get();
            if (rightDb != savedSend.sendGainDb) savedSend.sendGainDbR = rightDb;
            savedSend.tap = send->tap;
            scene.busSends.push_back(std::move(savedSend));
        }
        for (const auto& route : busRoutes) {
            pluginrack::BusRoute savedRoute;
            savedRoute.busId = route->busId;
            savedRoute.outputId = route->outputId;
            savedRoute.sendGainDb = route->sendGainDbL.get();
            const float rightDb = route->sendGainDbR.get();
            if (rightDb != savedRoute.sendGainDb) savedRoute.sendGainDbR = rightDb;
            scene.busRoutes.push_back(std::move(savedRoute));
        }

        // The tuning the session runs with travels with the save, so loading it
        // restores the same buffers, stream modes and device watchdog.
        scene.audio = getDeviceSettings();
    }

    if (!includePluginState) return scene;

    // Capture the live parameter/program state so reopening the scene does not
    // reset every insert to its defaults. Runs unlocked: this is the part that
    // enters plugin code.
    for (std::size_t c = 0; c < channelHosts.size(); ++c) {
        for (std::size_t i = 0; i < channelHosts[c].size(); ++i) {
            // Only a live instance can describe its state. For a failed insert
            // keep the saved blob (already copied by copyInserts).
            if (channelHosts[c][i]->hasPlugin())
                scene.channels[c].inserts[i].pluginState =
                    channelHosts[c][i]->getStateAsBase64().toStdString();
        }
    }
    for (std::size_t o = 0; o < outputHosts.size(); ++o) {
        for (std::size_t i = 0; i < outputHosts[o].size(); ++i) {
            if (outputHosts[o][i]->hasPlugin())
                scene.outputs[o].inserts[i].pluginState =
                    outputHosts[o][i]->getStateAsBase64().toStdString();
        }
    }
    for (std::size_t b = 0; b < busHosts.size(); ++b) {
        for (std::size_t i = 0; i < busHosts[b].size(); ++i) {
            if (busHosts[b][i]->hasPlugin())
                scene.buses[b].inserts[i].pluginState =
                    busHosts[b][i]->getStateAsBase64().toStdString();
        }
    }
    return scene;
}

std::uint64_t AudioEngine::getPluginEditRevision() const {
    // stateLock keeps the live insert list stable while the atomics are read.
    const juce::ScopedLock lock(stateLock);
    std::uint64_t revision = 0;
    for (const auto& channel : channels)
        for (const auto& insert : channel->inserts)
            revision += insert->pluginHost.getParameterRevision();
    for (const auto& output : outputs)
        for (const auto& insert : output->inserts)
            revision += insert->pluginHost.getParameterRevision();
    // Buses host inserts too; leaving them out made the autosave trigger blind
    // to edits on bus plugins.
    for (const auto& bus : buses)
        for (const auto& insert : bus->inserts)
            revision += insert->pluginHost.getParameterRevision();
    return revision;
}

juce::String AudioEngine::restoreScene(const pluginrack::Scene& scene, bool openDevicesNow) {
    // Diagnostics and tests keep the blocking contract: the same staged
    // machinery, drained without returning to the message loop.
    if (restoreActive_.load(std::memory_order_acquire)) {
        return "A scene is already loading. Please try again in a moment.";
    }
    auto error = prepareSceneRestore(scene, openDevicesNow);
    if (error.isNotEmpty()) return error;
    while (loadNextPendingInsert()) {
    }
    return commitSceneRestore();
}

juce::String AudioEngine::beginSceneRestore(const pluginrack::Scene& scene, bool openDevicesNow,
                                            std::function<void(juce::String)> onFinished) {
    if (restoreActive_.load(std::memory_order_acquire)) {
        return "A scene is already loading. Please try again in a moment.";
    }
    auto error = prepareSceneRestore(scene, openDevicesNow);
    if (error.isNotEmpty()) return error;
    restoreJob_->onFinished = std::move(onFinished);
    restoreActive_.store(true, std::memory_order_release);
    startTimer(1);
    return {};
}

juce::String AudioEngine::prepareSceneRestore(const pluginrack::Scene& scene, bool openDevicesNow) {
    if (isOpeningDevices()) {
        return "Audio devices are still starting up. Please try again in a moment.";
    }
    restoreJob_ = std::make_unique<SceneRestoreJob>();
    auto& job = *restoreJob_;
    job.openDevicesNow = openDevicesNow;
    // Tuning first: the stream open below must already use the saved buffer
    // sizes and modes. Scenes written before schema 4 carry no tuning block,
    // and loading one leaves the live settings alone.
    if (scene.audio.has_value()) {
        applySettingsValues(*scene.audio);
        setWatchedDevices(scene.audio->watchedDevices);
    }
    double sampleRate;
    int bufferSize;
    std::map<std::uint32_t, juce::String> savedInputDevices;
    std::map<std::uint32_t, juce::String> savedOutputDevices;
    {
        const juce::ScopedLock lock(stateLock);
        // A restore can run before the clock device has reported its format
        // (startup restores the last scene). Preparing a VST3 at 0 Hz / 0
        // frames leaves it with zero-length internal buffers and the first
        // real processBlock faults inside the plugin; fall back to the scene's
        // saved format, then to a sane default.
        sampleRate = currentSampleRate > 0.0
                         ? currentSampleRate
                         : (scene.sampleRate > 0.0 ? scene.sampleRate : 48000.0);
        bufferSize = currentBufferSize > 0 ? currentBufferSize : 512;
        // A live scene load can happen while the engine is already rendering
        // at the device block; never prepare below the render ceiling.
        bufferSize = juce::jmax(bufferSize, maxBlockFrames_);
        for (const auto& c : channels) savedInputDevices[c->id] = c->inputDeviceName;
        for (const auto& o : outputs) savedOutputDevices[o->id] = o->outputDeviceName;
    }

    auto& restoredChannels = job.channels;
    auto& restoredOutputs = job.outputs;
    auto& restoredRoutes = job.routes;
    auto& restoredChannelIds = job.channelIds;
    auto& restoredOutputIds = job.outputIds;
    // Highest ids seen while building: applied to the next-id counters under
    // stateLock together with the vector swap below, so the counters and
    // the strips they number advance as one committed step.
    auto& maxInsertId = job.maxInsertId;
    auto& maxChannelId = job.maxChannelId;
    auto& maxOutputId = job.maxOutputId;
    job.sampleRate = sampleRate;
    job.bufferSize = bufferSize;

    for (const auto& savedChannel : scene.channels) {
        auto channel = std::make_unique<Channel>();
        channel->id = savedChannel.id;
        channel->name = savedChannel.name;
        channel->gainDb = savedChannel.gainDb;
        channel->trimDb = savedChannel.trimDb;
        channel->pan = savedChannel.pan;
        channel->muted = savedChannel.muted;
        channel->soloed = savedChannel.soloed;
        channel->sourceKind = savedChannel.sourceKind;
        channel->sourceDeviceId = savedChannel.sourceDeviceId;
        channel->layout = savedChannel.layout;
        channel->inputChannels = savedChannel.inputChannels;
        if (channel->inputChannels.empty()) {
            channel->inputChannels = channel->layout == pluginrack::ChannelLayout::Mono
                                        ? std::vector<int>{0}
                                        : std::vector<int>{0, 1};
        }
        channel->buffer.setSize(2, bufferSize);
        channel->buffer.clear();
        channel->preInsertTap.setSize(2, bufferSize);
        channel->preInsertTap.clear();
        channel->postFaderTap.setSize(2, bufferSize);
        channel->postFaderTap.clear();
        // EQ bands are scene state; rebuild coefficients here on the message
        // thread before the strip goes live (never in render). The delay line
        // is sized here for the same reason.
        channel->eqBands = savedChannel.eqBands;
        channel->eqDelayMs = savedChannel.eqDelayMs;
        // Old scenes carry no hue: assign one now so every strip has colour.
        if (savedChannel.colourHue > 0.0f) {
            channel->colourHue = savedChannel.colourHue;
        } else {
            juce::Random rng(juce::Time::currentTimeMillis()
                             ^ static_cast<juce::int64>(channel->id * 747796405u));
            channel->colourHue = theme::randomStripHue(rng);
        }
        channel->eqPhaseFlip = savedChannel.eqPhaseFlip;
        channel->eqRtaOffsetDb = savedChannel.eqRtaOffsetDb;
        channel->eqRtaWeighting = savedChannel.eqRtaWeighting;
        channel->multibandRtaOffsetDb = savedChannel.multibandRtaOffsetDb;
        channel->multibandRtaWeighting = savedChannel.multibandRtaWeighting;
        channel->deEsserRtaOffsetDb = savedChannel.deEsserRtaOffsetDb;
        channel->deEsserRtaWeighting = savedChannel.deEsserRtaWeighting;
        channel->feedbackSuppressorRtaOffsetDb = savedChannel.feedbackSuppressorRtaOffsetDb;
        channel->feedbackSuppressorRtaWeighting = savedChannel.feedbackSuppressorRtaWeighting;
        channel->eqBypassed = !savedChannel.eqEnabled;
        channel->eq.prepare(sampleRate);
        channel->eq.setBands(channel->eqBands);
        channel->eq.setDelayMs(channel->eqDelayMs);
        channel->eq.setPhaseFlip(channel->eqPhaseFlip.get());
        channel->effects = savedChannel.effects;
        channel->multibandEffects = savedChannel.multibandEffects;
        channel->deEsserEffects = savedChannel.deEsserEffects;
        channel->limiterEffects = savedChannel.limiterEffects;
        channel->feedbackSuppressorEffects = savedChannel.feedbackSuppressorEffects;
        channel->spectralDenoiserEffects = savedChannel.spectralDenoiserEffects;
        for (const auto& e : channel->multibandEffects) { pluginrack::MultibandProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); channel->multibandProcessors.push_back(std::move(pr)); }
        for (const auto& e : channel->deEsserEffects) { pluginrack::DeEsserProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); channel->deEsserProcessors.push_back(std::move(pr)); }
        for (const auto& e : channel->limiterEffects) { pluginrack::LimiterProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); channel->limiterProcessors.push_back(std::move(pr)); }
        for (const auto& e : channel->feedbackSuppressorEffects) { pluginrack::FeedbackSuppressorProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); channel->feedbackSuppressorProcessors.push_back(std::move(pr)); }
        for (const auto& e : channel->spectralDenoiserEffects) { pluginrack::SpectralDenoiserProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); channel->spectralDenoiserProcessors.push_back(std::move(pr)); }
        // An empty saved chain is a real empty state (no effects loaded).
        // Scenes without per-strip ordering predate this field; their effects
        // still run through the plan's fallback pass, so a missing chain
        // degrades to the same audible behaviour without synthesising rows.
        channel->chain = savedChannel.chain;
        channel->effectProcessors.clear();
        channel->effectProcessors.reserve(channel->effects.size());
        for (const auto& effect : channel->effects) {
            pluginrack::DynamicsProcessor processor;
            processor.prepare(sampleRate);
            processor.setEffect(effect);
            channel->effectProcessors.push_back(std::move(processor));
        }
        for (const auto& savedInsert : savedChannel.inserts) {
            auto insert = std::make_unique<InsertSlot>();
            insert->id = savedInsert.id;
            insert->pluginPath = savedInsert.pluginPath;
            insert->pluginName = savedInsert.pluginName;
            insert->pluginUniqueId = savedInsert.pluginUniqueId;
            insert->bypassed = savedInsert.bypassed;
            insert->sideChainSource = savedInsert.sideChainSource;
            insert->sideChainSourceId = savedInsert.sideChainSourceId;
            insert->sideChainTap = savedInsert.sideChainTap;
            insert->sideChainGainDb = savedInsert.sideChainGainDb;
            insert->sideChainBuffer.setSize(2, bufferSize);
            insert->sideChainBuffer.clear();
            // Keep the blob until a live instance can speak for itself. If the
            // plugin is missing, a later save writes this back instead of an
            // empty state that would erase the user's settings for good.
            insert->savedPluginState = juce::String(savedInsert.pluginState);
            insert->crashLabel = makeCrashLabel(channel->name, insert->id, insert->pluginName,
                                                insert->pluginPath);
            insert->pluginHost.prepareToPlay(sampleRate, bufferSize);
            // Instantiation is staged: the timer loads one insert per turn, so
            // a slow plugin stalls one short turn instead of the whole restore.
            job.pending.push_back({insert.get(), channel->name,
                                   juce::String(savedInsert.pluginState),
                                   savedInsert.pluginUniqueId});
            maxInsertId = juce::jmax(maxInsertId, insert->id + 1);
            channel->inserts.push_back(std::move(insert));
            if (std::none_of(channel->chain.begin(), channel->chain.end(),
                             [id = savedInsert.id](const auto& item) {
                                 return item.type == pluginrack::ChainItemType::Insert &&
                                        item.id == id;
                             })) {
                channel->chain.push_back({pluginrack::ChainItemType::Insert, savedInsert.id});
            }
        }
        maxChannelId = juce::jmax(maxChannelId, channel->id + 1);
        restoredChannelIds.push_back(channel->id);
        restoredChannels.push_back(std::move(channel));
    }
    for (const auto& savedOutput : scene.outputs) {
        auto output = std::make_unique<Output>();
        output->id = savedOutput.id;
        output->name = savedOutput.name;
        output->layout = savedOutput.layout;
        output->flipped = savedOutput.flipped;
        output->gainDb = savedOutput.gainDb;
        output->muted = savedOutput.muted;
        output->buffer.setSize(2, bufferSize);
        output->buffer.clear();
        output->preInsertTap.setSize(2, bufferSize);
        output->preInsertTap.clear();
        output->postFaderTap.setSize(2, bufferSize);
        output->postFaderTap.clear();
        output->eqBands = savedOutput.eqBands;
        output->eqDelayMs = savedOutput.eqDelayMs;
        if (savedOutput.colourHue > 0.0f) {
            output->colourHue = savedOutput.colourHue;
        } else {
            juce::Random rng(juce::Time::currentTimeMillis()
                             ^ static_cast<juce::int64>(output->id * 2891336453u));
            output->colourHue = theme::randomStripHue(rng);
        }
        output->eqPhaseFlip = savedOutput.eqPhaseFlip;
        output->eqRtaOffsetDb = savedOutput.eqRtaOffsetDb;
        output->eqRtaWeighting = savedOutput.eqRtaWeighting;
        output->multibandRtaOffsetDb = savedOutput.multibandRtaOffsetDb;
        output->multibandRtaWeighting = savedOutput.multibandRtaWeighting;
        output->deEsserRtaOffsetDb = savedOutput.deEsserRtaOffsetDb;
        output->deEsserRtaWeighting = savedOutput.deEsserRtaWeighting;
        output->feedbackSuppressorRtaOffsetDb = savedOutput.feedbackSuppressorRtaOffsetDb;
        output->feedbackSuppressorRtaWeighting = savedOutput.feedbackSuppressorRtaWeighting;
        output->eqBypassed = !savedOutput.eqEnabled;
        output->eq.prepare(sampleRate);
        output->eq.setBands(output->eqBands);
        output->eq.setDelayMs(output->eqDelayMs);
        output->eq.setPhaseFlip(output->eqPhaseFlip.get());
        output->effects = savedOutput.effects;
        output->multibandEffects = savedOutput.multibandEffects;
        output->deEsserEffects = savedOutput.deEsserEffects;
        output->limiterEffects = savedOutput.limiterEffects;
        output->feedbackSuppressorEffects = savedOutput.feedbackSuppressorEffects;
        output->spectralDenoiserEffects = savedOutput.spectralDenoiserEffects;
        for (const auto& e : output->multibandEffects) { pluginrack::MultibandProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); output->multibandProcessors.push_back(std::move(pr)); }
        for (const auto& e : output->deEsserEffects) { pluginrack::DeEsserProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); output->deEsserProcessors.push_back(std::move(pr)); }
        for (const auto& e : output->limiterEffects) { pluginrack::LimiterProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); output->limiterProcessors.push_back(std::move(pr)); }
        for (const auto& e : output->feedbackSuppressorEffects) { pluginrack::FeedbackSuppressorProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); output->feedbackSuppressorProcessors.push_back(std::move(pr)); }
        for (const auto& e : output->spectralDenoiserEffects) { pluginrack::SpectralDenoiserProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); output->spectralDenoiserProcessors.push_back(std::move(pr)); }
        output->chain = savedOutput.chain;
        output->effectProcessors.clear();
        output->effectProcessors.reserve(output->effects.size());
        for (const auto& effect : output->effects) {
            pluginrack::DynamicsProcessor processor;
            processor.prepare(sampleRate);
            processor.setEffect(effect);
            output->effectProcessors.push_back(std::move(processor));
        }
        for (const auto& savedInsert : savedOutput.inserts) {
            auto insert = std::make_unique<InsertSlot>();
            insert->id = savedInsert.id;
            insert->pluginPath = savedInsert.pluginPath;
            insert->pluginName = savedInsert.pluginName;
            insert->pluginUniqueId = savedInsert.pluginUniqueId;
            insert->bypassed = savedInsert.bypassed;
            insert->sideChainSource = savedInsert.sideChainSource;
            insert->sideChainSourceId = savedInsert.sideChainSourceId;
            insert->sideChainTap = savedInsert.sideChainTap;
            insert->sideChainGainDb = savedInsert.sideChainGainDb;
            insert->sideChainBuffer.setSize(2, bufferSize);
            insert->sideChainBuffer.clear();
            // Same rule as channels: hold the saved blob while the plugin is
            // not loaded, so a save cannot blank it.
            insert->savedPluginState = juce::String(savedInsert.pluginState);
            insert->crashLabel = makeCrashLabel(output->name, insert->id, insert->pluginName,
                                                insert->pluginPath);
            insert->pluginHost.prepareToPlay(sampleRate, bufferSize);
            // Instantiation is staged: the timer loads one insert per turn, so
            // a slow plugin stalls one short turn instead of the whole restore.
            job.pending.push_back({insert.get(), output->name,
                                   juce::String(savedInsert.pluginState),
                                   savedInsert.pluginUniqueId});
            maxInsertId = juce::jmax(maxInsertId, insert->id + 1);
            output->inserts.push_back(std::move(insert));
            if (std::none_of(output->chain.begin(), output->chain.end(),
                             [id = savedInsert.id](const auto& item) {
                                 return item.type == pluginrack::ChainItemType::Insert &&
                                        item.id == id;
                             })) {
                output->chain.push_back({pluginrack::ChainItemType::Insert, savedInsert.id});
            }
        }
        maxOutputId = juce::jmax(maxOutputId, output->id + 1);
        restoredOutputIds.push_back(output->id);
        restoredOutputs.push_back(std::move(output));
    }
    for (const auto& savedRoute : scene.routes) {
        auto route = std::make_unique<Route>();
        route->channelId = savedRoute.channelId;
        route->outputId = savedRoute.outputId;
        route->sendGainDbL = savedRoute.sendGainDb;
        route->sendGainDbR = savedRoute.sendGainDbR.value_or(savedRoute.sendGainDb);
        route->tap = savedRoute.tap;
        restoredRoutes.push_back(std::move(route));
    }
    auto& restoredBusIds = job.busIds;
    for (const auto& savedBus : scene.buses) {
        auto bus = std::make_unique<Bus>();
        bus->id = savedBus.id;
        bus->name = savedBus.name;
        bus->layout = savedBus.layout;
        bus->gainDb = savedBus.gainDb;
        bus->trimDb = savedBus.trimDb;
        bus->muted = savedBus.muted;
        bus->buffer.setSize(2, bufferSize);
        bus->buffer.clear();
        bus->preInsertTap.setSize(2, bufferSize);
        bus->preInsertTap.clear();
        bus->postFaderTap.setSize(2, bufferSize);
        bus->postFaderTap.clear();
        bus->eqBands = savedBus.eqBands;
        bus->eqDelayMs = savedBus.eqDelayMs;
        if (savedBus.colourHue > 0.0f) {
            bus->colourHue = savedBus.colourHue;
        } else {
            juce::Random rng(juce::Time::currentTimeMillis()
                             ^ static_cast<juce::int64>(bus->id * 2891336453u));
            bus->colourHue = theme::randomStripHue(rng);
        }
        bus->eqPhaseFlip = savedBus.eqPhaseFlip;
        bus->eqRtaOffsetDb = savedBus.eqRtaOffsetDb;
        bus->eqRtaWeighting = savedBus.eqRtaWeighting;
        bus->multibandRtaOffsetDb = savedBus.multibandRtaOffsetDb;
        bus->multibandRtaWeighting = savedBus.multibandRtaWeighting;
        bus->deEsserRtaOffsetDb = savedBus.deEsserRtaOffsetDb;
        bus->deEsserRtaWeighting = savedBus.deEsserRtaWeighting;
        bus->feedbackSuppressorRtaOffsetDb = savedBus.feedbackSuppressorRtaOffsetDb;
        bus->feedbackSuppressorRtaWeighting = savedBus.feedbackSuppressorRtaWeighting;
        bus->eqBypassed = !savedBus.eqEnabled;
        bus->eq.prepare(sampleRate);
        bus->eq.setBands(bus->eqBands);
        bus->eq.setDelayMs(bus->eqDelayMs);
        bus->eq.setPhaseFlip(bus->eqPhaseFlip.get());
        bus->effects = savedBus.effects;
        bus->multibandEffects = savedBus.multibandEffects;
        bus->deEsserEffects = savedBus.deEsserEffects;
        bus->limiterEffects = savedBus.limiterEffects;
        bus->feedbackSuppressorEffects = savedBus.feedbackSuppressorEffects;
        bus->spectralDenoiserEffects = savedBus.spectralDenoiserEffects;
        for (const auto& e : bus->multibandEffects) { pluginrack::MultibandProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); bus->multibandProcessors.push_back(std::move(pr)); }
        for (const auto& e : bus->deEsserEffects) { pluginrack::DeEsserProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); bus->deEsserProcessors.push_back(std::move(pr)); }
        for (const auto& e : bus->limiterEffects) { pluginrack::LimiterProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); bus->limiterProcessors.push_back(std::move(pr)); }
        for (const auto& e : bus->feedbackSuppressorEffects) { pluginrack::FeedbackSuppressorProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); bus->feedbackSuppressorProcessors.push_back(std::move(pr)); }
        for (const auto& e : bus->spectralDenoiserEffects) { pluginrack::SpectralDenoiserProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); bus->spectralDenoiserProcessors.push_back(std::move(pr)); }
        bus->chain = savedBus.chain;
        bus->effectProcessors.clear();
        bus->effectProcessors.reserve(bus->effects.size());
        for (const auto& effect : bus->effects) {
            pluginrack::DynamicsProcessor processor;
            processor.prepare(sampleRate);
            processor.setEffect(effect);
            bus->effectProcessors.push_back(std::move(processor));
        }
        for (const auto& savedInsert : savedBus.inserts) {
            auto insert = std::make_unique<InsertSlot>();
            insert->id = savedInsert.id;
            insert->pluginPath = savedInsert.pluginPath;
            insert->pluginName = savedInsert.pluginName;
            insert->pluginUniqueId = savedInsert.pluginUniqueId;
            insert->bypassed = savedInsert.bypassed;
            insert->sideChainSource = savedInsert.sideChainSource;
            insert->sideChainSourceId = savedInsert.sideChainSourceId;
            insert->sideChainTap = savedInsert.sideChainTap;
            insert->sideChainGainDb = savedInsert.sideChainGainDb;
            insert->sideChainBuffer.setSize(2, bufferSize);
            insert->sideChainBuffer.clear();
            insert->savedPluginState = juce::String(savedInsert.pluginState);
            insert->crashLabel = makeCrashLabel(bus->name, insert->id, insert->pluginName,
                                                insert->pluginPath);
            insert->pluginHost.prepareToPlay(sampleRate, bufferSize);
            job.pending.push_back({insert.get(), bus->name,
                                   juce::String(savedInsert.pluginState),
                                   savedInsert.pluginUniqueId});
            maxInsertId = juce::jmax(maxInsertId, insert->id + 1);
            bus->inserts.push_back(std::move(insert));
            if (std::none_of(bus->chain.begin(), bus->chain.end(),
                             [id = savedInsert.id](const auto& item) {
                                 return item.type == pluginrack::ChainItemType::Insert &&
                                        item.id == id;
                             })) {
                bus->chain.push_back({pluginrack::ChainItemType::Insert, savedInsert.id});
            }
        }
        maxOutputId = juce::jmax(maxOutputId, bus->id + 1);
        restoredBusIds.push_back(bus->id);
        job.buses.push_back(std::move(bus));
    }
    for (const auto& savedSend : scene.busSends) {
        auto send = std::make_unique<BusSend>();
        send->channelId = savedSend.channelId;
        send->busId = savedSend.busId;
        send->sendGainDbL = savedSend.sendGainDb;
        send->sendGainDbR = savedSend.sendGainDbR.value_or(savedSend.sendGainDb);
        send->tap = savedSend.tap;
        job.busSends.push_back(std::move(send));
    }
    for (const auto& savedRoute : scene.busRoutes) {
        auto route = std::make_unique<BusRoute>();
        route->busId = savedRoute.busId;
        route->outputId = savedRoute.outputId;
        route->sendGainDbL = savedRoute.sendGainDb;
        route->sendGainDbR = savedRoute.sendGainDbR.value_or(savedRoute.sendGainDb);
        job.busRoutes.push_back(std::move(route));
    }

    // Resolve the device assignments now, while the scene is in hand: the
    // commit runs after several message-loop turns, and the caller's scene may
    // be gone by then.
    for (const auto& savedChannel : scene.channels) {
        const juce::String sceneDevice(savedChannel.sourceDeviceId);
        if (sceneDevice.isNotEmpty()) {
            job.desiredInputDevices[savedChannel.id] = sceneDevice;
            continue;
        }
        const auto live = savedInputDevices.find(savedChannel.id);
        if (live != savedInputDevices.end() && live->second.isNotEmpty()) {
            job.desiredInputDevices[savedChannel.id] = live->second;
        }
    }
    for (const auto& savedOutput : scene.outputs) {
        const juce::String sceneDevice(savedOutput.deviceId);
        if (sceneDevice.isNotEmpty()) {
            job.desiredOutputDevices[savedOutput.id] = sceneDevice;
            continue;
        }
        const auto live = savedOutputDevices.find(savedOutput.id);
        if (live != savedOutputDevices.end() && live->second.isNotEmpty()) {
            job.desiredOutputDevices[savedOutput.id] = live->second;
        }
    }
    // Progress is published only once every insert is queued.
    restoreDone_.store(0, std::memory_order_relaxed);
    restoreTotal_.store(static_cast<int>(job.pending.size()), std::memory_order_relaxed);
    return {};
}

bool AudioEngine::loadNextPendingInsert() {
    if (restoreJob_ == nullptr || restoreJob_->pending.empty()) return false;
    auto& job = *restoreJob_;
    const PendingInsertLoad load = std::move(job.pending.front());
    job.pending.erase(job.pending.begin());
    const auto total = restoreTotal_.load(std::memory_order_relaxed);
    const auto done = total - static_cast<int>(job.pending.size());
    // The run log still names each plugin, so a slow one stays visible.
    juce::Logger::writeToLog("scene: instantiating "
                             + juce::File(load.insert->pluginPath).getFileName() + " ("
                             + juce::String(done) + " of " + juce::String(total) + ")");
    const auto error = load.insert->pluginHost.loadPlugin(
        juce::File(load.insert->pluginPath), load.pluginState, load.pluginUniqueId);
    if (error.isNotEmpty()) {
        job.errors += load.ownerName + ": " + error + "\n";
    } else {
        // The live instance now owns the state; savedPluginState is only the
        // fallback for an insert that never loaded.
        load.insert->savedPluginState.clear();
    }
    load.insert->latencySamples.store(load.insert->pluginHost.getLatencySamples());
    if (getChildProcessDsp()) {
        juce::String childError;
        const bool ok = load.insert->pluginHost.loadChildHost(
            juce::File(load.insert->pluginPath), load.pluginUniqueId, load.pluginState,
            job.sampleRate, job.bufferSize, 20000, childError);
        juce::Logger::writeToLog(ok
                                     ? "plugin: child host ready for " + load.insert->pluginName
                                     : "plugin: child host unavailable for "
                                           + load.insert->pluginName + " (" + childError + ")");
    }
    restoreDone_.store(done, std::memory_order_relaxed);
    return true;
}

juce::String AudioEngine::commitSceneRestore() {
    if (restoreJob_ == nullptr) return {};
    auto& job = *restoreJob_;

    // Park the clock render hook before the swap so no device callback can
    // enter render() while the strips below change hands, and so the old
    // strips' device-close destructors (which wait for in-flight callbacks)
    // never block on a live render. It is re-armed by the setOutputDevice
    // hand-offs during device re-apply below, and force-resumed at the end.
    {
        const juce::ScopedLock lock(stateLock);
        for (auto& o : outputs) {
            if (o->id == clockOutputId && o->output != nullptr) {
                o->output->setRenderActive(false);
                break;
            }
        }
    }

    // Swap under the lock, then destroy the old strips after releasing it:
    // OutputPlayback/InputCapture destructors close devices
    // (removeAudioCallback + closeAudioDevice) and must not run under
    // stateLock while render() falls back to the silence path.
    std::vector<std::unique_ptr<Channel>> oldChannels;
    std::vector<std::unique_ptr<Output>> oldOutputs;
    std::vector<std::unique_ptr<Bus>> oldBuses;
    std::vector<std::unique_ptr<BusSend>> oldBusSends;
    std::vector<std::unique_ptr<BusRoute>> oldBusRoutes;
    std::vector<std::unique_ptr<Route>> oldRoutes;
    std::map<juce::String, SharedInputCapture> retiredCaptures;
    std::vector<OutputPlayback*> oldPlaybacks;
    {
        const juce::ScopedLock lock(stateLock);
        // The staged window can outlive a clock reopen (for example a settings
        // Apply while the inserts load), so the strips may have been sized for
        // the old block. render() indexes them by the live block; size them to
        // the current maximum before they become visible.
        const int restoreFrames = juce::jmax(1, maxBlockFrames_);
        const double restoreRate = currentSampleRate > 0.0 ? currentSampleRate : job.sampleRate;
        // Mirror prepareToPlayImpl's sizing exactly: render() indexes the tap
        // and side-chain buffers of every strip type by the live block size,
        // and only the bus loop used to re-size them here.
        for (auto& channel : job.channels) {
            channel->buffer.setSize(2, restoreFrames, false, false, false);
            channel->preInsertTap.setSize(2, restoreFrames, false, false, false);
            channel->postFaderTap.setSize(2, restoreFrames, false, false, false);
            // Same re-prepare as prepareToPlayImpl: a clock reopen changes the
            // rate, and every built-in family caches tables for it.
            prepareStripProcessors(*channel, restoreRate);
            for (auto& insert : channel->inserts) {
                insert->sideChainBuffer.setSize(2, restoreFrames, false, false, false);
            }
        }
        for (auto& output : job.outputs) {
            output->buffer.setSize(2, restoreFrames, false, false, false);
            output->preInsertTap.setSize(2, restoreFrames, false, false, false);
            output->postFaderTap.setSize(2, restoreFrames, false, false, false);
            prepareStripProcessors(*output, restoreRate);
            for (auto& insert : output->inserts) {
                insert->sideChainBuffer.setSize(2, restoreFrames, false, false, false);
            }
        }
        for (auto& bus : job.buses) {
            bus->buffer.setSize(2, restoreFrames, false, false, false);
            bus->preInsertTap.setSize(2, restoreFrames, false, false, false);
            bus->postFaderTap.setSize(2, restoreFrames, false, false, false);
            bus->compensation.prepare(pluginrack::kMaxTapDelaySamples + restoreFrames + 2);
            prepareStripProcessors(*bus, restoreRate);
            for (auto& insert : bus->inserts) {
                insert->sideChainBuffer.setSize(2, restoreFrames, false, false, false);
            }
        }
        oldChannels = std::move(channels);
        oldOutputs = std::move(outputs);
        oldRoutes = std::move(routes);
        oldBuses = std::move(buses);
        oldBusSends = std::move(busSends);
        oldBusRoutes = std::move(busRoutes);
        channels = std::move(job.channels);
        outputs = std::move(job.outputs);
        buses = std::move(job.buses);
        routes = std::move(job.routes);
        busSends = std::move(job.busSends);
        busRoutes = std::move(job.busRoutes);
        // Taps survive a save/load, so rebuild their rings for the current
        // block size before render can read them.
        for (auto& channel : channels) {
            channel->outputTapRing.prepare(pluginrack::kMaxTapDelaySamples + restoreFrames + 2);
            for (const auto& route : routes) {
                if (route->channelId == channel->id) ensureTapPoint(*channel, route->tap);
            }
            for (const auto& send : busSends) {
                if (send->channelId == channel->id) ensureTapPoint(*channel, send->tap);
            }
            for (auto& point : channel->tapPoints) {
                point.ring.prepare(pluginrack::kMaxTapDelaySamples + restoreFrames + 2);
            }
        }
        // The staged strips were built without a plan: resolve every chain and
        // side-chain source now, before the clock hook resumes and render() can
        // walk them. This also recomputes each tap point's billed latency.
        for (auto& channel : channels) rebuildRenderPlan(*channel, restoreRate);
        for (auto& output : outputs) rebuildRenderPlan(*output, restoreRate);
        for (auto& bus : buses) rebuildRenderPlan(*bus, restoreRate);
        refreshAllSideChainCachesLocked();
        meters.setTargets(job.channelIds.data(), job.channelIds.size(),
                          job.outputIds.data(), job.outputIds.size(),
                          job.busIds.data(), job.busIds.size());
        // Keep the old playbacks alive for the lock-free silence snapshot,
        // but close their devices once the lock is released.
        for (auto& output : oldOutputs) {
            if (output->output != nullptr) {
                oldPlaybacks.push_back(output->output.get());
                retiredOutputs_.push_back(std::move(output->output));
            }
        }
        publishRenderOutputsLocked();
        nextInsertId = juce::jmax(nextInsertId, job.maxInsertId);
        nextChannelId = juce::jmax(nextChannelId, job.maxChannelId);
        nextOutputId = juce::jmax(nextOutputId, job.maxOutputId);
        // Collect captures no live channel references so their device close
        // runs after the lock is released (see retiredCaptures.clear()).
        for (auto it = inputCaptures_.begin(); it != inputCaptures_.end();) {
            bool used = false;
            for (const auto& c : channels) {
                if (c->input != nullptr && it->second.capture != nullptr
                    && c->input == it->second.capture) {
                    used = true;
                    break;
                }
            }
            if (!used) {
                retiredCaptures.emplace(it->first, std::move(it->second));
                it = inputCaptures_.erase(it);
            } else {
                ++it;
            }
        }
    }
    oldRoutes.clear();
    oldBuses.clear();
    oldBusSends.clear();
    oldBusRoutes.clear();
    oldOutputs.clear();
    oldChannels.clear();
    retiredCaptures.clear();
    for (auto* playback : oldPlaybacks) {
        playback->close();
    }

    // Re-apply device assignments where possible: the device stored in the
    // scene wins; channels from scenes saved before sourceDeviceId was
    // persisted keep whatever device was live for the same id. The maps were
    // resolved in prepareSceneRestore.

    // Record the assignments only. Opening them inline would run one blocking
    // driver probe per endpoint on the message thread, which is what made
    // loading a scene look like a freeze; the background opener below (or the
    // caller at startup) opens them in parallel and reports any refusal.
    {
        const juce::ScopedLock lock(stateLock);
        clockOutputId = 0;
        for (const auto& [id, name] : job.desiredInputDevices) {
            if (auto* channel = findChannel(id)) {
                channel->inputDeviceName = name;
                channel->sourceDeviceId = isSilentDeviceName(name) ? std::string{}
                                                                 : name.toStdString();
                channel->inputError.clear();
            }
        }
        for (const auto& [id, name] : job.desiredOutputDevices) {
            if (auto* output = findOutput(id)) {
                // setOutputDevice treats the silent choice as "no device".
                output->outputDeviceName = isSilentDeviceName(name) ? juce::String{} : name;
                output->outputError.clear();
            }
        }
    }

    if (job.openDevicesNow)
        openStripsDevicesAsync();

    const juce::String errors = job.errors;
    restoreJob_.reset();
    bumpStructureRevision();
    return errors;
}

void AudioEngine::timerCallback() {
    if (!restoreActive_.load(std::memory_order_acquire) || restoreJob_ == nullptr) {
        stopTimer();
        return;
    }
    // One plugin per turn: the message loop repaints between slow plugins.
    if (loadNextPendingInsert()) return;
    // A device job can start while the inserts load. Device workers hold bare
    // Channel*/Output* across blocking driver probes, so swapping the strips
    // under them is a use-after-free. Wait for the job, then commit; opening
    // the new rig would also be dropped while that job is still running.
    if (isOpeningDevices()) {
        startTimer(50);
        return;
    }
    stopTimer();
    auto finished = std::move(restoreJob_->onFinished);
    const auto errors = commitSceneRestore();
    restoreActive_.store(false, std::memory_order_release);
    if (finished) finished(errors);
}

void AudioEngine::abortSceneRestore() {
    stopTimer();
    restoreJob_.reset();
    restoreActive_.store(false, std::memory_order_release);
    restoreDone_.store(0, std::memory_order_relaxed);
    restoreTotal_.store(0, std::memory_order_relaxed);
}

juce::String AudioEngine::reassignChannelInputDevice(std::uint32_t channelId,
                                                     const juce::String& deviceName) {
    if (isOpeningDevices()) {
        return "Audio devices are still starting up. Try again in a moment.";
    }
    const auto error = setChannelInputDeviceImpl(channelId, deviceName);
    // The previous figure described the previous capture chain.
    if (error.isEmpty()) resetLatencyMeasurement();
    return error;
}

juce::String AudioEngine::setChannelInputDevice(std::uint32_t channelId, const juce::String& deviceName) {
    const auto error = reassignChannelInputDevice(channelId, deviceName);
    if (error.isEmpty()) bumpStructureRevision();
    return error;
}

juce::String AudioEngine::setChannelInputDeviceImpl(std::uint32_t channelId, const juce::String& deviceName,
                                                     bool forceReopen, double engineRate) {
    auto* channel = findChannel(channelId);
    if (channel == nullptr) {
        return "The channel no longer exists.";
    }

    if (isSilentDeviceName(deviceName)) {
        const juce::ScopedLock sl(stateLock);
        channel->input.reset();
        channel->duplexInput = false;
        channel->duplexInputChannels = 0;
        channel->inputDeviceName = silentDeviceName();
        channel->sourceDeviceId.clear();
        channel->inputError.clear();
        pruneUnusedCaptures();
        return {};
    }

    {
        // The clock's duplex callback already owns this endpoint for capture.
        // Opening a second client on it is exactly what ASIO refuses, so the
        // channel is fed from that callback instead of a capture ring.
        const juce::ScopedLock sl(stateLock);
        if (duplexActive_.load(std::memory_order_acquire) && deviceName == duplexDeviceName_) {
            int available = 0;
            for (const auto& o : outputs) {
                if (o->id == clockOutputId && o->output != nullptr) {
                    available = o->output->getDuplexInputChannels();
                }
            }
            for (auto& c : channels) {
                if (c->inputDeviceName != deviceName) continue;
                c->input.reset();
                c->duplexInput = true;
                c->duplexInputChannels = available;
            }
            channel->inputDeviceName = deviceName;
            channel->sourceDeviceId = deviceName.toStdString();
            channel->inputError.clear();
            pruneUnusedCaptures();
            return {};
        }
        channel->duplexInput = false;
        channel->duplexInputChannels = 0;
    }

    // The shared capture for this device must cover the highest physical index
    // used by any channel on it, including this channel's new mapping.
    int needed = 2;
    {
        const juce::ScopedLock sl(stateLock);
        for (const auto& c : channels) {
            if (c.get() == channel || c->inputDeviceName == deviceName) {
                for (const int index : c->inputChannels) {
                    needed = juce::jmax(needed, index + 1);
                }
            }
        }
    }

    int renderBlockFrames = 0;
    int capacityReserveFrames = 0;
    {
        const juce::ScopedLock sl(stateLock);
        // The read delay must follow the real render block; only the ring
        // reserves room for a block the clock may still be negotiating.
        renderBlockFrames = currentBufferSize;
        capacityReserveFrames = juce::jmax(currentBufferSize, kProbeRenderBlockFrames);
    }

    std::shared_ptr<InputCapture> reuse;
    if (!forceReopen) {
        const juce::ScopedLock sl(stateLock);
        const auto it = inputCaptures_.find(deviceName);
        // Compare against the ring the capture was actually sized for, not the
        // render block: the clock has not negotiated when inputs start, so an
        // exact block comparison rejects every capture on the next pass and
        // reopens a device this process already holds open.
        if (it != inputCaptures_.end() && it->second.requestedChannels >= needed
            && it->second.requestedBufferSize == requestedInputBufferSize_.load()
            && it->second.renderBlockFrames >= capacityReserveFrames) {
            reuse = it->second.capture;
        }
    }

    if (reuse != nullptr) {
        // One device stays open for every logical channel mapped into it.
        const juce::ScopedLock sl(stateLock);
        channel->input = reuse;
        channel->inputDeviceName = deviceName;
        channel->sourceDeviceId = deviceName.toStdString();
        channel->inputError = reuse->getInputError();
        pruneUnusedCaptures();
        return {};
    }

    // No usable capture yet (or it is too narrow): open one sized for all
    // channels on this device. On failure every channel keeps its previous
    // capture untouched.
    //
    // The capture being replaced has to be closed first: a driver hands an
    // endpoint out once, so opening the replacement while the old stream still
    // runs fails and leaves the user with an error next to working audio. The
    // cost is one device-open of silence on an explicit reload.
    {
        std::shared_ptr<InputCapture> previous;
        {
            const juce::ScopedLock sl(stateLock);
            const auto it = inputCaptures_.find(deviceName);
            if (it != inputCaptures_.end()) previous = it->second.capture;
        }
        if (previous != nullptr) {
            {
                const juce::ScopedLock sl(stateLock);
                inputCaptures_.erase(deviceName);
                for (auto& c : channels) {
                    if (c->input == previous) c->input.reset();
                }
            }
            previous->close();
        }
    }

    auto capture = std::make_shared<InputCapture>();
    juce::String err;
    // Same-device input usually follows the same hardware clock, so the
    // optional one-block delay is a reasonable starting point. The capture
    // promotes itself to a safer delay after a real starvation.
    const auto clockDevice = getClockOutputDeviceName();
    const bool inputOnClockDevice = fullDuplexInput_.load(std::memory_order_relaxed)
                                    && clockDevice.isNotEmpty() && clockDevice == deviceName;
    const bool useLowLatencyInput = lowLatencyInput_.load(std::memory_order_relaxed)
                                    || inputOnClockDevice;
    // Do not create an exclusive input client for an endpoint already used by
    // the clock output. Two independent exclusive clients cannot share the
    // endpoint; shared WASAPI is the stable fallback for same-device capture.
    const bool inputPreferExclusive = preferExclusive_.load(std::memory_order_relaxed)
                                   && !(clockDevice.isNotEmpty() && clockDevice == deviceName);
    const double requestedRate = engineRate > 0.0 ? engineRate : currentSampleRate;
    // Auto (0) follows the render block instead of handing the driver a "you
    // decide": the driver picks a different shared-mode period between launches
    // (480 one time, 600 the next) and the read delay moved with it.
    const auto cachedCap = cachedCapabilities(deviceName);
    int bufferRequest =
        pluginrack::inputBufferRequest(requestedInputBufferSize_.load(), renderBlockFrames);
    // Cache-first hint: with the size on Auto, reuse the buffer this endpoint
    // negotiated last time. A refused size still falls through to the live
    // probe, so a stale entry can never wedge a device closed.
    if (requestedInputBufferSize_.load() == 0) {
        if (cachedCap.has_value() && cachedCap->negotiatedBuffer > 0) {
            bufferRequest = cachedCap->negotiatedBuffer;
        }
    }
    const auto openStartMs = juce::Time::getMillisecondCounterHiRes();
    auto openErr = capture->open(isSystemDefaultName(deviceName) ? juce::String() : deviceName,
                                 requestedRate, needed, err, bufferRequest,
                                 renderBlockFrames,
                                 useLowLatencyInput ? 1 : 2,
                                 inputPreferExclusive,
                                 capacityReserveFrames);
    if (openErr.isNotEmpty() && requestedInputBufferSize_.load() == 0 && bufferRequest > 0) {
        // This device will not follow the engine's period. Its own default is
        // still valid, just less predictable; better than a silent strip.
        openErr = capture->open(isSystemDefaultName(deviceName) ? juce::String() : deviceName,
                                requestedRate, needed, err, 0,
                                renderBlockFrames,
                                useLowLatencyInput ? 1 : 2,
                                inputPreferExclusive,
                                capacityReserveFrames);
    }
    const int probeMs = static_cast<int>(std::lround(
        juce::Time::getMillisecondCounterHiRes() - openStartMs));
    juce::Logger::writeToLog("device: open endpoint=\"" + deviceName
        + "\" role=input cached=" + juce::String(cachedCap.has_value() ? 1 : 0)
        + " probe_ms=" + juce::String(probeMs)
        + " err=" + (openErr.isEmpty() ? juce::String("none") : openErr));
    if (openErr.isNotEmpty()) {
        if (!forceReopen) {
            const juce::ScopedLock sl(stateLock);
            channel->inputError = openErr;
        }
        return openErr;
    }

    std::shared_ptr<InputCapture> retired;
    int settledBlockFrames = 0;
    {
        const juce::ScopedLock sl(stateLock);
        settledBlockFrames = maxBlockFrames_;
        const auto old = inputCaptures_.find(deviceName);
        if (old != inputCaptures_.end()) retired = old->second.capture;
        for (auto& c : channels) {
            if (c.get() == channel || c->inputDeviceName == deviceName) {
                c->input = capture;
                c->inputDeviceName = deviceName;
                c->sourceDeviceId = deviceName.toStdString();
                c->inputError = capture->getInputError();
            }
        }
        inputCaptures_[deviceName] = SharedInputCapture{capture, needed,
                                                        requestedInputBufferSize_.load(),
                                                        capacityReserveFrames};
        pruneUnusedCaptures();
        // Configure before releasing stateLock. The channels above already
        // point at this capture, and render() may be pulling it the moment the
        // lock opens; the delay target and health window are not atomics.
        capture->setHealthThresholds(getHealthThresholds());
        // The clock may have negotiated while this input was probing. Settle
        // the read delay on the real block here as well, so a capture that
        // commits after prepareToPlayImpl's sweep cannot keep its provisional
        // delay.
        capture->applyRenderBlockFrames(settledBlockFrames);
    }
    recordDeviceCapabilities(deviceName, true, capture->availableInputCount(), 0,
                             capture->getSampleRate(), capture->getDeviceBufferSize(),
                             probeMs, deviceModeName(capture->getActiveDeviceType()));
    // The old capture closes here, outside stateLock, only after every channel
    // has moved to the new one. On failure the old one was never detached.
    return {};
}

void AudioEngine::prepareInputs() {
    std::vector<std::pair<std::uint32_t, juce::String>> wanted;
    {
        const juce::ScopedLock lock(stateLock);
        for (const auto& channel : channels) {
            if (channel->inputDeviceName.isNotEmpty()
                && !isSilentDeviceName(channel->inputDeviceName)) {
                wanted.emplace_back(channel->id, channel->inputDeviceName);
            }
        }
    }
    // setChannelInputDevice reuses an existing capture when the requested
    // width still covers the mappings and reopens wider when they grew.
    for (const auto& [id, name] : wanted) {
        setChannelInputDeviceImpl(id, name);
    }
}

void AudioEngine::pruneUnusedCaptures() {
    for (auto it = inputCaptures_.begin(); it != inputCaptures_.end();) {
        // Only the map itself holds the capture: no channel uses it anymore.
        if (it->second.capture == nullptr || it->second.capture.use_count() == 1) {
            it = inputCaptures_.erase(it);  // destructor closes the device
        } else {
            ++it;
        }
    }
}

juce::String AudioEngine::setOutputDevice(std::uint32_t outputId, const juce::String& deviceName) {
    if (isOpeningDevices()) {
        return "Audio devices are still starting up. Try again in a moment.";
    }
    // Moving the clock to an ASIO interface must take its captures with it,
    // otherwise the duplex match never fires, the clock opens output-only and
    // the input keeps its own read delay. Works from the device menu too, not
    // just the Force ASIO button.
    bool isClock = false;
    {
        const juce::ScopedLock lock(stateLock);
        isClock = clockOutputId == outputId;
    }
    int retargeted = 0;
    std::vector<std::pair<std::uint32_t, juce::String>> previous;
    if (isClock && fullDuplexInput_.load(std::memory_order_relaxed))
        retargeted = retargetInputsToClockDevice(deviceName, &previous);

    const auto error = setOutputDeviceImpl(outputId, deviceName, currentSampleRate);
    if (!error.isEmpty() && retargeted > 0) {
        // The clock refused: put the captures back, or those strips stay
        // flagged as duplex with nothing feeding them and go silent.
        {
            const juce::ScopedLock lock(stateLock);
            for (auto& c : channels) {
                for (const auto& [id, name] : previous) {
                    if (c->id != id) continue;
                    c->input.reset();
                    c->duplexInput = false;
                    c->duplexInputChannels = 0;
                    c->inputDeviceName = name;
                    c->sourceDeviceId = name.toStdString();
                }
            }
        }
        prepareInputs();
    }
    if (!error.isEmpty() && isClock) {
        // Opening a replacement clock closes the previous stream first, so a
        // refusal would otherwise leave every other output with no render
        // driver at all. Promote one of them or fall back to the internal clock.
        checkClockHealth();
    }
    // The previous figure described the previous output chain.
    if (error.isEmpty()) {
        resetLatencyMeasurement();
        // The strip's saved device id changed (the output assignment is what a
        // scene stores), so the autosave poll must see this edit.
        bumpStructureRevision();
    }
    return error;
}

int AudioEngine::retargetInputsToClockDevice(
    const juce::String& clockDevice,
    std::vector<std::pair<std::uint32_t, juce::String>>* previous) {
    const juce::StringArray asio = DeviceEnumeration::asioDevices();
    if (asio.isEmpty() || clockDevice.isEmpty() || isSilentDeviceName(clockDevice)) return 0;

    std::vector<std::string> names;
    names.reserve(static_cast<std::size_t>(asio.size()));
    for (const auto& name : asio) names.push_back(name.toStdString());

    std::vector<std::pair<std::uint32_t, std::string>> assignments;
    {
        const juce::ScopedLock lock(stateLock);
        for (const auto& c : channels)
            assignments.emplace_back(c->id, c->inputDeviceName.toStdString());
    }

    int retargeted = 0;
    for (const auto id : pluginrack::inputsToRetargetForClock(assignments, clockDevice.toStdString(), names)) {
        const juce::ScopedLock lock(stateLock);
        for (auto& c : channels) {
            if (c->id != id) continue;
            if (previous != nullptr) previous->emplace_back(c->id, c->inputDeviceName);
            // Release the endpoint now: the clock's duplex callback will feed
            // this strip, and a single-client driver refuses a second client.
            c->input.reset();
            c->inputDeviceName = clockDevice;
            c->sourceDeviceId = clockDevice.toStdString();
            c->duplexInput = true;
            c->inputError.clear();
            ++retargeted;
        }
    }
    if (retargeted > 0) {
        pruneUnusedCaptures();
        bumpStructureRevision();
    }
    return retargeted;
}

juce::String AudioEngine::setOutputDeviceImpl(std::uint32_t outputId, const juce::String& deviceName,
                                              double engineRate) {
    AudioEngine::Output* out = findOutput(outputId);
    // findOutput also resolves buses; only a physical output can take a device.
    // The physical-existence scan reads `outputs`, which moveOutput reorders on
    // the message thread, so it has to run under stateLock like everything
    // else this background opener touches.
    bool missing = (out == nullptr);
    bool isClock = false;
    bool sharesClockInput = false;
    int renderBlockFrames = 0;
    {
        const juce::ScopedLock sl(stateLock);
        if (!missing) {
            missing = std::none_of(outputs.begin(), outputs.end(),
                                   [outputId](const auto& o) { return o->id == outputId; });
        }
        if (!missing) {
            // The startup opener can open several outputs at once. Read the clock
            // role under the same lock that the clock hand-off updates it with;
            // otherwise the first output's negotiation can race another worker's
            // role check.
            isClock = (clockOutputId == outputId) || (clockOutputId == 0);
            renderBlockFrames = juce::jmax(currentBufferSize, kProbeRenderBlockFrames);
            if (isClock) {
                sharesClockInput = std::any_of(
                    channels.begin(), channels.end(), [&deviceName](const auto& channel) {
                        return channel->inputDeviceName == deviceName;
                    });
            }
        }
    }
    if (missing) {
        return "The output no longer exists.";
    }

    if (isSilentDeviceName(deviceName)) {
        bool wasClock = false;
        OutputPlayback* retiredPlayback = nullptr;
        {
            const juce::ScopedLock sl(stateLock);
            wasClock = (clockOutputId == outputId);
            if (wasClock) {
                // Dropping the clock also drops the duplex capture path.
                duplexActive_.store(false, std::memory_order_release);
                duplexDeviceName_.clear();
                duplexInputBuffers_ = nullptr;
                duplexInputBufferCount_ = 0;
                for (auto& c : channels) {
                    c->duplexInput = false;
                    c->duplexInputChannels = 0;
                }
            }
            if (out->output != nullptr) {
                // Keep the object alive: the lock-free render-miss path may
                // still hold its raw pointer until the snapshot is rebuilt.
                retiredOutputs_.push_back(std::move(out->output));
                retiredPlayback = retiredOutputs_.back().get();
            }
            out->outputDeviceName.clear();
            out->outputDeviceType.clear();
            out->outputError.clear();
            if (wasClock) {
                clockOutputId = 0;
                for (auto& o2 : outputs) {
                    if (o2->output != nullptr) {
                        clockOutputId = o2->id;
                        o2->output->setTargetFillBlocks(0);
                        o2->output->setRenderActive(true);
                        break;
                    }
                }
            }
            publishRenderOutputsLocked();
        }
        // Close outside stateLock: removing the callback can wait for an
        // in-flight callback that itself takes stateLock.
        if (retiredPlayback != nullptr) retiredPlayback->close();
        return {};
    }

    std::unique_ptr<OutputPlayback> prevOutput;
    {
        // Move the live playback under stateLock: render() dereferences
        // out->output while holding it, so handing it off outside the lock
        // races the audio thread.
        const juce::ScopedLock sl(stateLock);
        prevOutput = std::move(out->output);
    }
    // Keep the old clock rendering while the new device opens: stopping it
    // here would leave every other output's FIFO undrained for the whole
    // open() (plus prepareToPlay) window. It is stopped at the hand-off
    // below, inside stateLock, so a second render hook never runs.

    auto newPlayback = std::make_unique<OutputPlayback>();
    // Every stream gets the same hook before open() starts its callback. Only
    // the stream holding the clock role is active, so a non-clock callback
    // just drains its FIFO. Keeping the hook immutable means the audio thread
    // never observes a torn std::function, and a clock hand-off is one atomic
    // store instead of a live closure assignment.
    newPlayback->setRenderHook([this](int n) { this->render(n); });
    newPlayback->setRenderActive(false);

    juce::String err;
    const auto openName = isSystemDefaultName(deviceName) ? juce::String() : deviceName;
    // The endpoint's last successful open is advisory: the mode below can skip
    // a probe this endpoint already refused, and the buffer hint below starts
    // from the negotiated size. A refusal always falls through to the live
    // probe inside open(), so a stale entry cannot wedge a device closed.
    const auto cachedCap = cachedCapabilities(openName);
    // Always offer the engine rate, including for the clock: exclusive-mode
    // WASAPI can only negotiate when a concrete sample rate is requested.
    // The clock renders and drains in the same callback, so during the async
    // startup open it needs almost no FIFO depth. A synchronous clock change
    // (prepareToPlay before the hand-off) still keeps the full margin, since
    // the device callbacks run while the FIFO is not being refilled.
    int targetBlocks = 4;
    int primeBlocks = 4;
    if (isClock && isOpeningDevices()) {
        targetBlocks = 1;
        primeBlocks = minimalClockFifo_.load(std::memory_order_relaxed) ? 0 : 1;
    } else if (!isClock) {
        targetBlocks = lowLatencyOutputs_.load(std::memory_order_relaxed) ? 2 : 4;
        primeBlocks = targetBlocks;
    }
    // Exclusive mode is limited to a standalone clock output. It is not safe
    // for non-clock outputs (which may share an endpoint) or for a clock whose
    // endpoint is also used by an input capture.
    bool preferExclusive = preferExclusive_.load(std::memory_order_relaxed)
                        && isClock && !sharesClockInput;
    // A warm start does not re-probe a mode this endpoint already refused: if
    // its last successful open was shared, exclusive is not attempted again.
    // Only an output-side record is evidence here: an input record says the
    // capture opened shared, which says nothing about the output's exclusive
    // support (recordDeviceCapabilities writes outputChannels = 0 for inputs).
    // Any explicit Apply/Reload clears modeCacheBypass_, so the user's toggle
    // still forces a fresh probe instead of being pinned to the old answer.
    const bool cachedSharedMode = cachedCap.has_value()
        && cachedCap->outputChannels > 0
        && (cachedCap->mode == pluginrack::deviceModeId(pluginrack::DeviceBackend::Shared)
            || cachedCap->mode == pluginrack::deviceModeId(pluginrack::DeviceBackend::SharedLowLatency));
    if (preferExclusive && cachedSharedMode
        && !modeCacheBypass_.load(std::memory_order_relaxed)) {
        preferExclusive = false;
        juce::Logger::writeToLog("device: exclusive skipped for \"" + openName
                                 + "\" (cached mode " + cachedCap->mode.c_str() + ")");
    }

    // Duplex: when a capture is assigned to this same endpoint and the user
    // opted in, open both directions in one driver client. ASIO refuses a
    // second client on the same interface, and for any driver this removes the
    // capture ring and the clock FIFO from the path.
    int duplexInputs = 0;
    if (isClock && fullDuplexInput_.load(std::memory_order_relaxed)) {
        const juce::ScopedLock sl(stateLock);
        for (const auto& c : channels) {
            if (c->inputDeviceName != deviceName) continue;
            duplexInputs = juce::jmax(1, duplexInputs);
            for (const int index : c->inputChannels) {
                duplexInputs = juce::jmax(duplexInputs, index + 1);
            }
        }
        if (duplexInputs > 0) {
            // A separate capture that already holds this endpoint would make
            // the duplex open fail on any single-client driver (ASIO), and the
            // fallback would silently keep the input on its own clock. Release
            // it first: prepareToPlayImpl re-marks these strips as duplex.
            for (auto& c : channels) {
                if (c->inputDeviceName == deviceName) c->input.reset();
            }
            const auto it = inputCaptures_.find(deviceName);
            if (it != inputCaptures_.end()) inputCaptures_.erase(it);
        }
    }

    // ASIO is single-client: reopening the same endpoint while the prior
    // playback still holds it makes the driver refuse the second open
    // ("No such device" / "(the previous stream is still running)"). Release
    // it first. In duplex mode nothing is queued, so closing costs no drain;
    // otherwise the push-silence fallback keeps other outputs fed.
    if (prevOutput != nullptr) {
        prevOutput->close();
    }
    // Cache-first hint: reuse the buffer this endpoint negotiated last time
    // when the user left the size on Auto. A refusal falls through to the live
    // probe, so the cache remains advisory.
    int outputBufferRequest = requestedOutputBufferSize_;
    if (outputBufferRequest == 0) {
        if (cachedCap.has_value() && cachedCap->negotiatedBuffer > 0) {
            outputBufferRequest = cachedCap->negotiatedBuffer;
        }
    }
    const auto openStartMs = juce::Time::getMillisecondCounterHiRes();
    auto openErr = newPlayback->open(openName, engineRate, err, outputBufferRequest,
                                     targetBlocks, primeBlocks, preferExclusive,
                                     renderBlockFrames,
                                     isClock ? pluginrack::StreamRole::Clock
                                             : pluginrack::StreamRole::Output,
                                     duplexInputs);
    if (openErr.isNotEmpty() && duplexInputs > 0) {
        // This endpoint has no usable input side, or another client holds it.
        // Falling back keeps the clock alive; the split path still works.
        openErr = newPlayback->open(openName, engineRate, err, outputBufferRequest,
                                     targetBlocks, primeBlocks, preferExclusive,
                                     renderBlockFrames,
                                     isClock ? pluginrack::StreamRole::Clock
                                            : pluginrack::StreamRole::Output,
                                     0);
        duplexInputs = 0;
    }
    const int probeMs = static_cast<int>(std::lround(
        juce::Time::getMillisecondCounterHiRes() - openStartMs));
    juce::Logger::writeToLog("device: open endpoint=\"" + deviceName
        + "\" role=" + juce::String(isClock ? "clock" : "output")
        + " cached=" + juce::String(cachedCap.has_value() ? 1 : 0)
        + " probe_ms=" + juce::String(probeMs)
        + " err=" + (openErr.isEmpty() ? juce::String("none") : openErr));

    // Publish the duplex decision before the inputs are prepared below: that
    // pass decides whether a channel opens its own capture or is fed by this
    // callback.
    if (isClock) {
        duplexInputBuffers_ = nullptr;
        duplexInputBufferCount_ = 0;
        {
            const juce::ScopedLock sl(stateLock);
            duplexDeviceName_ = duplexInputs > 0 ? deviceName : juce::String();
        }
        duplexActive_.store(duplexInputs > 0, std::memory_order_release);
    }
    if (openErr.isNotEmpty()) {
        const juce::ScopedLock sl(stateLock);
        out->output = std::move(prevOutput);
        // A clock that failed to open cannot feed its duplex inputs; leaving
        // the flag set would make those channels wait forever for a callback
        // that no longer exists.
        if (isClock) {
            duplexDeviceName_.clear();
            duplexActive_.store(false, std::memory_order_release);
        }
        // A restored stream is closed, so this is mostly belt and braces. Do
        // not leave a non-clock stream active: every stream now carries a
        // render hook and would double-drive the session.
        if (out->output != nullptr) out->output->setRenderActive(isClock);
        // Nothing is playing: keep the name the user assigned so the strip and
        // the saved scene still say which device is missing, instead of
        // dropping back to "Choose device" with only an error line.
        if (out->output == nullptr) out->outputDeviceName = deviceName;
        // The prior playback was closed above, so its name no longer applies;
        // openErr itself is the reason (and a reopened candidate may already
        // have failed above for a different endpoint).
        auto message = openErr;
        out->outputError = message;
        return message;
    }

    if (isClock) {
        const double newRate = newPlayback->getSampleRate();
        int newBuf = newPlayback->getBufferSize();
        if (newBuf <= 0) newBuf = 256;
        {
            const juce::ScopedLock sl(stateLock);
            currentSampleRate = newRate;
            // Publish the same value lock-free for the UI timers.
            atomicSampleRate_.store(newRate, std::memory_order_relaxed);
            currentBufferSize = newBuf;
            clockOutputId = outputId;
            clockFailoverActive_ = false;
            // The cached chain/tap latency is rate-dependent (EQ delay,
            // limiter look-ahead). Recompute it here so the first block at the
            // new rate already compensates correctly; prepareToPlayImpl
            // refreshes it again once the processors are re-prepared.
            for (auto& channel : channels) rebuildRenderPlan(*channel, newRate);
            for (auto& output : outputs) rebuildRenderPlan(*output, newRate);
            for (auto& bus : buses) rebuildRenderPlan(*bus, newRate);
        }
        // A real device owns the clock again: the free-running fallback must
        // not keep rendering in parallel with it.
        stopInternalClock();
        // Plugins are prepared on the message thread only: some synchronise
        // with it inside prepareToPlay, and the device worker must never wait
        // on a message thread that may itself be waiting for the worker.
        // During startup the input captures are still closed; the opener
        // sizes them for this block when it starts them in parallel.
        if (isOpeningDevices()) {
            auto alive = prepareAliveToken_;
            const int block = newBuf;
            juce::MessageManager::callAsync([this, alive, block] {
                if (!alive->load(std::memory_order_acquire)) return;
                prepareToPlayImpl(block, false);
            });
        } else {
            prepareToPlayImpl(newBuf, true);
        }
    }

    if (openErr.isEmpty()) {
        // A stream opened after the settings were applied still needs the
        // scene's health thresholds, not the tracker's defaults.
        newPlayback->setHealthThresholds(getHealthThresholds());
        recordDeviceCapabilities(
            openName, false, duplexInputs,
            juce::jmax(2, newPlayback->getNumDeviceOutputChannels()),
            newPlayback->getSampleRate(), newPlayback->getBufferSize(), probeMs,
            deviceModeName(newPlayback->getActiveDeviceType()));
    }

    OutputPlayback* retiredPlayback = nullptr;
    {
        const juce::ScopedLock sl(stateLock);
        // render() holds stateLock for its whole body, so acquiring it here
        // excludes any in-flight render from prevOutput's thread; the flag
        // stops its next callback. Deactivate before activating the new
        // clock so exactly one hook feeds the outputs.
        if (isClock && prevOutput != nullptr) {
            prevOutput->setRenderActive(false);
        }
        if (prevOutput != nullptr) {
            retiredPlayback = prevOutput.get();
            retiredOutputs_.push_back(std::move(prevOutput));
        }
        out->output = std::move(newPlayback);
        if (isClock) {
            // Duplex: the callback now owns both directions, so hand render()
            // the capture pointers as they arrive.
            if (duplexInputs > 0) {
                out->output->setDuplexInputHook([this](const float* const* in, int count) {
                    duplexInputBuffers_ = in;
                    duplexInputBufferCount_ = count;
                });
            }
            // A synchronous open (Apply, scene load, device change) primes the
            // clock FIFO four blocks deep so the hand-off cannot starve. Once
            // the hook below is running the clock renders and drains in the
            // same callback, so that prime is pure latency: retarget it to the
            // running depth (zero — the block pushed this callback is the
            // block the device plays) and let the callback drop the backlog.
            out->output->setTargetFillBlocks(0);
            out->output->setRenderActive(true);
        }
        out->outputDeviceName = deviceName;
        out->outputDeviceType = out->output->getActiveDeviceType();
        out->outputError.clear();
        publishRenderOutputsLocked();
    }
    if (retiredPlayback != nullptr) {
        // Close outside stateLock: removing the callback and closing the
        // device can wait for an in-flight callback. The object itself stays
        // in retiredOutputs_ so a concurrent silence push is still safe.
        retiredPlayback->close();
    }
    return {};
}

juce::String AudioEngine::setDeviceBufferSize(int size) {
    const auto inputError = setInputBufferSize(size);
    const auto outputError = setOutputBufferSize(size);
    return inputError.isNotEmpty() ? inputError : outputError;
}

juce::String AudioEngine::setInputBufferSize(int size) {
    if (isOpeningDevices()) return "Audio devices are still starting up. Try again in a moment.";
    const int previous = requestedInputBufferSize_.load();
    requestedInputBufferSize_.store(size);
    juce::String firstError;
    for (auto& channel : channels) {
        if (channel->inputDeviceName.isNotEmpty() &&
            !isSilentDeviceName(channel->inputDeviceName)) {
            const auto e = setChannelInputDeviceImpl(channel->id, channel->inputDeviceName,
                                                     /*forceReopen=*/true);
            if (firstError.isEmpty() && e.isNotEmpty()) firstError = e;
        }
    }
    if (firstError.isNotEmpty() && previous != size) {
        // The device refused: put every capture back on the old size so the
        // setting that was working stays in effect.
        requestedInputBufferSize_.store(previous);
        for (auto& channel : channels) {
            if (channel->inputDeviceName.isNotEmpty() &&
                !isSilentDeviceName(channel->inputDeviceName)) {
                setChannelInputDeviceImpl(channel->id, channel->inputDeviceName,
                                          /*forceReopen=*/true);
            }
        }
        return "The input device refused the new buffer size; kept the previous one.\n"
             + firstError;
    }
    bumpStructureRevision();
    return firstError;
}

juce::String AudioEngine::setOutputBufferSize(int size) {
    if (isOpeningDevices()) return "Audio devices are still starting up. Try again in a moment.";
    const int previous = requestedOutputBufferSize_;
    requestedOutputBufferSize_ = size;
    juce::String firstError;
    for (auto& output : outputs) {
        if (output->outputDeviceName.isNotEmpty() &&
            !isSilentDeviceName(output->outputDeviceName)) {
            const auto e = setOutputDeviceImpl(output->id, output->outputDeviceName,
                                                currentSampleRate);
            if (firstError.isEmpty() && e.isNotEmpty()) firstError = e;
        }
    }
    if (firstError.isNotEmpty() && previous != size) {
        requestedOutputBufferSize_ = previous;
        for (auto& output : outputs) {
            if (output->outputDeviceName.isNotEmpty() &&
                !isSilentDeviceName(output->outputDeviceName)) {
                setOutputDeviceImpl(output->id, output->outputDeviceName, currentSampleRate);
            }
        }
        return "The output device refused the new buffer size; kept the previous one.\n"
             + firstError;
    }
    bumpStructureRevision();
    return firstError;
}

void AudioEngine::handleClockLoss() {
    std::vector<pluginrack::ClockCandidate> candidates;
    std::uint32_t lost = 0;
    int lastRate = 48000;
    pluginrack::ClockFailoverDecision decision;
    juce::String promotedName;
    int promotedBuffer = 0;
    const auto nowMs = juce::Time::getMillisecondCounter();
    {
        const juce::ScopedLock lock(stateLock);
        lastRate = static_cast<int>(std::lround(
            currentSampleRate > 0.0 ? currentSampleRate : 48000.0));
        lost = clockOutputId;
        for (const auto& output : outputs) {
            if (output->id == lost) continue;
            pluginrack::ClockCandidate candidate;
            // Promoting a stream that already stopped (a second unplugged
            // interface, for example) would only move the silence: it has no
            // callback to drive the engine. Treat it as unavailable so the
            // free-running internal clock takes over instead.
            const auto* stream = output->output.get();
            const bool live = stream != nullptr && !stream->isDeviceStopped()
                              && !stream->isCallbackStalled(nowMs, kClockStallGraceMs);
            candidate.outputId = output->id;
            candidate.clockCapable = live;
            candidate.deviceOpen = live;
            candidate.sampleRate =
                live ? static_cast<int>(std::lround(stream->getSampleRate())) : 0;
            candidates.push_back(candidate);
        }
        decision = pluginrack::chooseClockFailover(candidates, lost, lastRate);
        // Whatever the decision, the lost stream must stop acting as a render
        // driver. If it is only stalled and later resumes, leaving it active
        // would let it render in parallel with the promoted stream or the
        // internal timer, doubling every push.
        for (auto& old : outputs) {
            if (old->id == lost && old->output != nullptr) {
                old->output->setRenderActive(false);
            }
        }
        // A duplex clock also owned the capture callback. Its buffers stop
        // updating (and may be freed with the device), so stop the channels
        // reading them; they fall back to their own capture.
        duplexActive_.store(false, std::memory_order_release);
        duplexDeviceName_.clear();
        duplexInputBuffers_ = nullptr;
        duplexInputBufferCount_ = 0;
        for (auto& c : channels) {
            c->duplexInput = false;
            c->duplexInputChannels = 0;
        }
        if (decision.action == pluginrack::ClockAction::Promote) {
            for (auto& output : outputs) {
                if (output->id != decision.outputId || output->output == nullptr) continue;
                // The promoted stream already owns an open device; it only
                // needs to become the render driver. Its FIFO backlog was
                // headroom for an independent output clock, so drop it to the
                // clock's zero target instead of paying that latency forever.
                output->output->setTargetFillBlocks(0);
                // The hook was installed before the stream was opened.
                output->output->setRenderActive(true);
                clockOutputId = output->id;
                promotedName = output->outputDeviceName;
                promotedBuffer = output->output->getBufferSize();
                break;
            }
        } else {
            clockOutputId = 0;
        }
        publishRenderOutputsLocked();
    }

    // Logging is file IO: never do it while holding stateLock, or a render
    // callback that misses the lock pays for the write with silence.
    if (decision.action == pluginrack::ClockAction::Promote) {
        stopInternalClock();
        // The promoted output can have a different driver buffer from the clock
        // it replaces. The render ceiling and plugin buffers must follow the
        // stream that now drives the session, or render() would reject its
        // blocks as oversized and push silence. A device job runs this on a
        // worker, and plugins must be prepared on the message thread only.
        if (promotedBuffer > 0 && promotedBuffer != currentBufferSize) {
            if (isOpeningDevices()) {
                auto alive = prepareAliveToken_;
                juce::MessageManager::callAsync([this, alive, promotedBuffer] {
                    if (!alive->load(std::memory_order_acquire)) return;
                    prepareToPlayImpl(promotedBuffer, true);
                });
            } else {
                prepareToPlayImpl(promotedBuffer, true);
            }
        }
        if (promotedName.isNotEmpty())
            juce::Logger::writeToLog("Clock promoted to " + promotedName);
    } else {
        startInternalClock(lastRate);
        juce::Logger::writeToLog(
            "Clock output was lost; rendering on the internal clock at "
            + juce::String(lastRate) + " Hz");
    }
}

bool AudioEngine::checkClockHealth() {
    const auto nowMs = juce::Time::getMillisecondCounter();
    bool clockLost = false;
    {
        const juce::ScopedLock lock(stateLock);
        bool otherOutputOpen = false;
        for (const auto& output : outputs) {
            if (output->id == clockOutputId) {
                const auto* stream = output->output.get();
                clockLost = stream == nullptr
                            || stream->isDeviceStopped()
                            || stream->isCallbackStalled(nowMs, kClockStallGraceMs);
                if (clockLost && output->outputError.isEmpty()) {
                    const auto name = isSilentDeviceName(output->outputDeviceName)
                                          ? systemDefaultDeviceName()
                                          : output->outputDeviceName;
                    output->outputError = "Clock device stopped: " + name;
                }
            } else if (output->output != nullptr) {
                otherOutputOpen = true;
            }
        }
        // A session with no other open output has nothing to keep alive; the
        // strips already report the missing clock. Do not start a timer that
        // renders into nothing.
        if (clockLost && !otherOutputOpen) clockLost = false;
    }
    if (!clockLost || clockFailoverActive_) return false;
    clockFailoverActive_ = true;
    handleClockLoss();
    clockFailoverActive_ = false;
    return true;
}

bool AudioEngine::maybeReclaimClock() {
    if (isOpeningDevices()) return false;

    std::uint32_t preferredId = 0;
    juce::String preferredName;
    juce::String preferredLabel;
    bool reclaimed = false;
    int reclaimedBuffer = 0;
    std::uint32_t oldClockId = 0;

    {
        const juce::ScopedLock lock(stateLock);
        if (outputs.empty()) return false;

        // Mirror the opener's clock choice: the first output by id that has a
        // real device assigned. An earlier strip set to "None - silent" does
        // not hold the clock role, so it must not block the wait either.
        Output* preferred = nullptr;
        for (auto& output : outputs) {
            if (isSilentDeviceName(output->outputDeviceName)) continue;
            if (preferred == nullptr || output->id < preferred->id) preferred = output.get();
        }
        if (preferred == nullptr) return false;
        preferredId = preferred->id;
        preferredName = preferred->outputDeviceName;
        preferredLabel = preferred->name;
        auto* stream = preferred->output.get();

        // System Default is a moving target: after a change it may name a
        // different endpoint, so it is not reclaimed by name. The OS default
        // path still covers it through the normal device selection.
        if (isSystemDefaultName(preferredName)) {
            reclaimGate_.reset();
            return false;
        }

        // Same 32-bit millisecond clock the callback stamps; HiRes has a
        // different epoch and cannot be compared with it.
        const auto nowHi = juce::Time::getMillisecondCounter();
        const bool live = stream != nullptr && !stream->isDeviceStopped()
                          && !stream->isCallbackStalled(nowHi, kClockStallGraceMs);
        // open() seeds the heartbeat, so a just-reopened stream can look live
        // before its driver has called back once. Do not promote it until a
        // real callback has arrived; the fallback keeps playing meanwhile.
        const bool seenCallback = live && stream->getCallbackCount() > 0;
        const int streamRate =
            stream != nullptr ? juce::roundToInt(stream->getSampleRate()) : 0;
        const int engineRate = juce::roundToInt(currentSampleRate);
        const bool liveSameRate = pluginrack::canReclaimClock(
            stream != nullptr, seenCallback, streamRate, engineRate);

        if (clockOutputId == preferredId && live) {
            // Already the clock; do not fight an open that is still starting
            // (its first callback may be a buffer period away).
            reclaimGate_.onSuccess();
            return false;
        }

        if (liveSameRate) {
            // Do not touch the stream here: the old driver still owns the
            // render role. Stop it, demote it and activate this stream in one
            // ordered hand-off below.
            oldClockId = clockOutputId;
            reclaimedBuffer = stream->getBufferSize();
            reclaimed = true;
        } else if (stream != nullptr && live) {
            // Open but not reclaimable yet: another rate class, or its first
            // callback has not arrived. Reclaim must never change the session
            // rate, and a still-starting clock must not be reopened.
            reclaimGate_.reset();
            return false;
        }
    }

    if (reclaimed) {
        const bool blockChange = reclaimedBuffer > 0
                                 && reclaimedBuffer != currentBufferSize;
        // A larger block can be prepared while the old driver still runs: the
        // ceiling stays at the old size until the hosts are ready, then rises,
        // and the old blocks remain valid. A smaller block must wait until
        // after the hand-off, because prepareToPlayImpl lowers the ceiling
        // immediately and would reject the old driver's oversized blocks.
        const bool grew = blockChange && reclaimedBuffer > currentBufferSize;
        if (grew) prepareToPlayImpl(reclaimedBuffer, true);

        // Stop the free-running timer before the new clock starts, so the two
        // drivers never render the same block. stopTimer() can wait for a
        // render that needs stateLock, so it cannot run under the lock.
        stopInternalClock();

        {
            const juce::ScopedLock lock(stateLock);
            for (auto& output : outputs) {
                if (output->output == nullptr) continue;
                if (output->id == oldClockId && output->id != preferredId) {
                    output->output->setRenderActive(false);
                    // Demoted outputs go back to the same headroom a normal
                    // non-clock open uses, so Low Latency Mode keeps its depth.
                    output->output->setTargetFillBlocks(
                        lowLatencyOutputs_.load(std::memory_order_relaxed) ? 2 : 4);
                } else if (output->id == preferredId) {
                    // The hook was installed before open(); this stream was
                    // inactive, so the depth and the role change together with
                    // no callback rendering between them.
                    output->output->setTargetFillBlocks(0);
                    output->output->setRenderActive(true);
                    output->outputError.clear();
                }
            }
            clockOutputId = preferredId;
            publishRenderOutputsLocked();
        }

        // The new stream now drives at its native block, which the old ceiling
        // still admits. Shrink the graph after the switch.
        if (blockChange && !grew)
            prepareToPlayImpl(reclaimedBuffer, true);
        else if (!blockChange)
            prepareInputs();  // a duplex clock loss released these captures

        reclaimGate_.onSuccess();
        juce::Logger::writeToLog("audio: Clock reclaimed by " + preferredName);
        return true;
    }

    // No usable stream yet: reopen the preferred endpoint once its name is
    // back in the cached enumeration. A missing device is never hammered.
    const auto present = getAvailableOutputDevices();
    if (present.isEmpty()) return false;
    if (present.indexOf(preferredName, true) < 0) {
        // Gone: the next appearance is a fresh episode with a fresh budget.
        reclaimGate_.onAbsent();
        return false;
    }

    const auto nowMs = juce::Time::getMillisecondCounter();
    if (!reclaimGate_.shouldAttempt(preferredName.toStdString(), nowMs,
                                    kReclaimRetryMs, kReclaimMaxAttempts))
        return false;

    ReloadableDevice device;
    device.label = preferredLabel;
    device.deviceName = preferredName;
    device.stripId = preferredId;
    device.isInput = false;
    // A missing endpoint can still be in the cached enumeration for a few
    // seconds. Do not interrupt playback with a dialog for every retry; the
    // strip already carries the driver error.
    device.quiet = true;
    reloadDevicesAsync({device});
    return false;
}

void AudioEngine::startInternalClock(int sampleRate) {
    if (sampleRate <= 0) sampleRate = 48000;
    if (!internalClock_) {
        internalClock_ = std::make_unique<InternalClock>(*this);
    }
    const int block = maxBlockFrames_ > 0 ? maxBlockFrames_ : juce::jmax(1, currentBufferSize);
    internalClock_->configure(block, static_cast<double>(sampleRate));
    internalClockActive_.store(true, std::memory_order_relaxed);
}

void AudioEngine::stopInternalClock() {
    if (internalClock_ != nullptr) internalClock_->stopTimer();
    internalClockActive_.store(false, std::memory_order_relaxed);
}

void AudioEngine::recordDeviceCapabilities(const juce::String& endpoint, bool input,
                                           int inputChannels, int outputChannels,
                                           double sampleRate, int bufferSize, int probeMs,
                                           const juce::String& mode) {
    if (endpoint.isEmpty()) return;
    pluginrack::EndpointCapabilities capabilities;
    capabilities.inputChannels = input ? juce::jmax(0, inputChannels) : 0;
    capabilities.outputChannels = input ? 0 : juce::jmax(0, outputChannels);
    if (sampleRate > 0.0) {
        capabilities.sampleRates.push_back(
            static_cast<int>(std::lround(sampleRate)));
    }
    capabilities.negotiatedRate = capabilities.sampleRates.empty()
                                      ? 0
                                      : capabilities.sampleRates.front();
    capabilities.negotiatedBuffer = bufferSize;
    capabilities.mode = mode.isEmpty() ? "shared" : mode.toStdString();
    capabilities.probeMs = probeMs;
    const auto now = juce::Time::getCurrentTime();
    capabilities.lastGood = now.toISO8601(true).toStdString();
    capabilities.lastGoodUnixMs = now.toMilliseconds();

    std::vector<std::string> errors;
    {
        const juce::ScopedLock lock(deviceCacheLock_);
        deviceCache_.put(endpoint.toStdString(), capabilities);
        const auto file = deviceCacheFile();
        file.getParentDirectory().createDirectory();
        if (!deviceCache_.saveToFile(file.getFullPathName().toStdString(), &errors)) {
            for (const auto& error : errors) {
                juce::Logger::writeToLog("device cache: " + juce::String(error));
            }
        }
    }
}

juce::String AudioEngine::getClockDescription() const {
    // Called from the UI timer while the device opener may be rewriting these
    // strings, so every read here happens under the lock.
    const juce::ScopedLock lock(stateLock);
    const auto khzValue = currentSampleRate / 1000.0;
    const auto khz = std::abs(khzValue - std::round(khzValue)) < 0.001
                         ? juce::String((int)std::round(khzValue))
                         : juce::String(khzValue, 1);
    if (clockOutputId == 0) {
        // The fallback timer is a real clock: keep reporting it, or the header
        // would claim there is no output while every strip is still playing.
        if (internalClockActive_.load(std::memory_order_relaxed)) {
            return "Internal clock  -  " + khz + " kHz  -  "
                 + juce::String(currentBufferSize) + " smp";
        }
        return "No output device - click a master strip";
    }
    for (const auto& o : outputs) {
        if (o->id == clockOutputId) {
            const auto device = isSilentDeviceName(o->outputDeviceName) ? systemDefaultDeviceName()
                                                              : o->outputDeviceName;
            auto desc = o->name + "  -  " + device + "  -  " + khz + " kHz  -  "
                      + juce::String(currentBufferSize) + " smp";
            if (o->outputDeviceType.isNotEmpty())
                desc += " [" + deviceBackendTag(o->outputDeviceType) + "]";
            return desc;
        }
    }
    return "No output device - click a master strip";
}

juce::StringArray AudioEngine::getAvailableInputDevices() const {
    return DeviceEnumeration::inputs();
}

juce::StringArray AudioEngine::getAvailableOutputDevices() const {
    return DeviceEnumeration::outputs();
}

juce::StringArray AudioEngine::getAsioDeviceNames() const {
    return DeviceEnumeration::asioDevices();
}

juce::String AudioEngine::forceAsio() {    const auto asio = DeviceEnumeration::asioDevices();
    if (asio.isEmpty()) {
        return "No ASIO driver found. Install the interface driver (or an ASIO "
               "bridge such as ASIO4ALL) and reopen Settings.";
    }

    std::uint32_t clockId = 0;
    juce::String currentDevice;
    {
        const juce::ScopedLock lock(stateLock);
        clockId = clockOutputId;
        for (const auto& o : outputs) {
            if (o->id == clockOutputId) {
                currentDevice = o->outputDeviceName;
                break;
            }
        }
    }
    if (clockId == 0) {
        return "Assign an output device first: the ASIO device becomes the clock.";
    }

    std::vector<std::string> names;
    names.reserve(static_cast<std::size_t>(asio.size()));
    for (const auto& name : asio) names.push_back(name.toStdString());
    std::string match = pluginrack::bestAsioDeviceMatch(currentDevice.toStdString(), names);
    bool movedClockToInput = false;
    if (match.empty()) {
        // The clock endpoint has no ASIO driver of its own (a virtual Windows
        // cable, for example) but an input strip is already on an interface.
        // Duplex is impossible across two devices, so move the clock onto that
        // interface instead: that is the only way to lose the read delay.
        std::vector<std::string> inputNames;
        {
            const juce::ScopedLock lock(stateLock);
            for (const auto& c : channels) {
                if (c->inputDeviceName.isNotEmpty() && !isSilentDeviceName(c->inputDeviceName))
                    inputNames.push_back(c->inputDeviceName.toStdString());
            }
        }
        match = pluginrack::asioClockFromInputs(inputNames, names);
        movedClockToInput = !match.empty();
        if (match.empty()) {
            return "Pick the ASIO device for the clock by hand: " + asio.joinIntoString(", ")
                 + ". The capture buffer cannot be removed while capture and playback "
                   "run on different devices.";
        }
    }
    const juce::String target(match);

    // 1. Arm duplex without a device job: applyDeviceSettings would start an
    //    async reopen, and setOutputDevice refuses to run while that is in
    //    flight ("devices are still starting up"), so the clock never moved.
    auto settings = getDeviceSettings();
    settings.fullDuplexInput = true;
    settings.latencyTuningOptIn = true;
    applySettingsValues(settings);

    // 2. Point the input strips on this interface at the ASIO device, before
    //    the clock moves: the duplex match is "capture device == clock device",
    //    so doing it afterwards leaves the clock on the split path. Keep the
    //    previous assignments: setOutputDevice only rolls back what *it*
    //    retargets, and these inputs are already on the clock name by then.
    std::vector<std::pair<std::uint32_t, juce::String>> previous;
    const int retargeted = retargetInputsToClockDevice(target, &previous);

    // 3. Move the clock. It now sees a duplex capture, so one driver client
    //    carries both directions and the read delay disappears.
    const auto error = setOutputDevice(clockId, target);
    if (error.isNotEmpty()) {
        // The clock refused: undo step 2, or these strips stay flagged duplex
        // with no capture behind them and stay silent.
        if (!previous.empty()) {
            {
                const juce::ScopedLock lock(stateLock);
                for (auto& c : channels) {
                    for (const auto& [id, name] : previous) {
                        if (c->id != id) continue;
                        c->input.reset();
                        c->duplexInput = false;
                        c->duplexInputChannels = 0;
                        c->inputDeviceName = name;
                        c->sourceDeviceId = name.toStdString();
                    }
                }
            }
            prepareInputs();
        }
        return "ASIO refused: " + error;
    }

    if (retargeted > 0) {
        return "Clock and " + juce::String(retargeted) + " input"
             + (retargeted == 1 ? "" : "s") + " are now on " + target
             + " - one callback, no capture delay.";
    }
    if (movedClockToInput) {
        return "Clock moved onto " + target + " to share the input's driver - "
               "the capture read delay is gone.";
    }
    return "Clock is now on " + target
         + ". Assign an input on that interface to use it for capture too.";
}

bool AudioEngine::refreshDeviceAvailability() {
    // Enumerate before taking stateLock: the scan creates a device manager and
    // must never run with the audio lock held.
    const auto outputsPresent = getAvailableOutputDevices();
    const auto inputsPresent = getAvailableInputDevices();
    // An empty list means the enumeration itself failed; do not report every
    // strip as missing in that case.
    const auto markMissing = [](juce::String& error, const juce::String& name,
                                const juce::StringArray& present, bool listUsable) {
        if (!listUsable || isSilentDeviceName(name) || isSystemDefaultName(name))
            return false;
        if (error.isNotEmpty()) return false; // a real open error already explains it
        if (present.indexOf(name, true) >= 0) return false;
        error = kNoSuchDevice + name;
        return true;
    };

    bool changed = false;
    bool clockLost = false;
    {
        const juce::ScopedLock lock(stateLock);
        for (auto& output : outputs) {
            const bool missing = markMissing(output->outputError, output->outputDeviceName,
                                             outputsPresent, !outputsPresent.isEmpty());
            changed |= missing;
            if (missing && output->id == clockOutputId) clockLost = true;
        }
        for (auto& channel : channels) {
            changed |= markMissing(channel->inputError, channel->inputDeviceName,
                                   inputsPresent, !inputsPresent.isEmpty());
        }
    }
    if (clockLost && !clockFailoverActive_) {
        clockFailoverActive_ = true;
        handleClockLoss();
        clockFailoverActive_ = false;
        changed = true;
    }
    return changed;
}

AudioEngine::Channel* AudioEngine::findChannel(std::uint32_t channelId) {
    const juce::ScopedLock lock(stateLock);
    for (auto& channel : channels) {
        if (channel->id == channelId) {
            return channel.get();
        }
    }
    return nullptr;
}

AudioEngine::InsertSlot* AudioEngine::findInsert(std::uint32_t channelId, std::uint32_t insertId) {
    auto* channel = findChannel(channelId);
    if (channel == nullptr) {
        return nullptr;
    }
    for (auto& insert : channel->inserts) {
        if (insert->id == insertId) {
            return insert.get();
        }
    }
    return nullptr;
}

AudioEngine::Output* AudioEngine::findOutput(std::uint32_t outputId) {
    const juce::ScopedLock lock(stateLock);
    for (auto& output : outputs) {
        if (output->id == outputId) {
            return output.get();
        }
    }
    // Buses are Output objects too; the strip setters address both by id.
    for (auto& bus : buses) {
        if (bus->id == outputId) {
            return bus.get();
        }
    }
    return nullptr;
}

AudioEngine::InsertSlot* AudioEngine::findOutputInsert(std::uint32_t outputId, std::uint32_t insertId) {
    auto* output = findOutput(outputId);
    if (output == nullptr) {
        return nullptr;
    }
    for (auto& insert : output->inserts) {
        if (insert->id == insertId) {
            return insert.get();
        }
    }
    return nullptr;
}

namespace {
// Built-in effects report their algorithmic delay here. Bypass preserves the
// delay, so every denoiser named by the chain counts. An effect removed from
// the chain is marked bypassed and no longer runs, so it must not count.
template <typename Strip>
int spectralDenoiserLatencySamples(const Strip& strip) {
    int samples = 0;
    for (const auto& effect : strip.spectralDenoiserEffects) {
        const pluginrack::ChainItem item{pluginrack::ChainItemType::SpectralDenoiser, effect.id};
        const bool inChain = std::find(strip.chain.begin(), strip.chain.end(), item) != strip.chain.end();
        if (inChain || !effect.bypassed) samples += pluginrack::kSpectralDenoiserLatencySamples;
    }
    return samples;
}
// Limiter and EQ contributions follow the same rules as
// AudioEngine::chainItemLatencySamples so the budget view and the render
// compensation can never disagree (plan Task 9).
template <typename Strip>
int stripLimiterLatencySamples(const Strip& strip, double sampleRate) {
    int samples = 0;
    for (const auto& effect : strip.limiterEffects) {
        if (!effect.bypassed) samples += pluginrack::limiterLatencySamples(effect, sampleRate);
    }
    return samples;
}
template <typename Strip>
int stripEqLatencySamples(const Strip& strip, double sampleRate) {
    // The EQ delay line runs from the Eq chain row, or - without one - from
    // render's fallback pass, so it always bills. eqLatencySamples already
    // reports 0 while the strip's EQ is bypassed or has no delay.
    return pluginrack::eqLatencySamples(true, strip.eqBypassed.get(), strip.eqDelayMs,
                                        sampleRate);
}
} // namespace

AudioEngine::LatencyReport AudioEngine::getLatencyReport() const {
    LatencyReport report;
    const juce::ScopedTryLock lock(stateLock);
    if (!lock.isLocked()) {
        report.valid = false;
        return report;
    }

    const Channel* inputChannel = nullptr;
    for (const auto& channel : channels) {
        if (channel->input != nullptr) {
            inputChannel = channel.get();
            break;
        }
    }

    int inputDeviceSamples = 0;
    int inputBufferSize = 0;
    int inputReadDelaySamples = 0;
    if (inputChannel != nullptr) {
        if (inputChannel->duplexInput.get()) {
            // Duplex: the capture latency is the same device's input latency,
            // and there is no read delay at all.
            for (const auto& output : outputs) {
                if (output->id == clockOutputId && output->output != nullptr) {
                    inputDeviceSamples = output->output->getDuplexInputLatencySamples();
                    break;
                }
            }
            inputBufferSize = inputChannel->duplexInputChannels > 0
                                  ? inputChannel->duplexInputChannels
                                  : 0;
        } else if (inputChannel->input != nullptr) {
            inputDeviceSamples = inputChannel->input->getLatencySamples();
            inputReadDelaySamples = inputChannel->input->getReadDelaySamples();
            inputBufferSize = inputChannel->input->getDeviceBufferSize();
        }
        if (inputBufferSize <= 0) inputBufferSize = currentBufferSize;
    }

    const double sampleRate = currentSampleRate > 0.0 ? currentSampleRate : 48000.0;
    int pluginSamples = 0;
    int maxInsertSamples = 0;
    std::uint32_t maxInsertId = 0;
    juce::String maxInsertName;
    if (inputChannel != nullptr && inputChannel->input != nullptr) {
        for (const auto& insert : inputChannel->inserts) {
            const int insertSamples = insert->latencySamples.load();
            pluginSamples += insertSamples;
            if (insertSamples > maxInsertSamples) {
                maxInsertSamples = insertSamples;
                maxInsertId = insert->id;
                maxInsertName = insert->pluginName;
            }
        }
        const int denoiserSamples = spectralDenoiserLatencySamples(*inputChannel);
        pluginSamples += denoiserSamples;
        if (denoiserSamples > maxInsertSamples) {
            maxInsertSamples = denoiserSamples;
            maxInsertId = 0xFFFFFFFFu;
            maxInsertName = "Spectral Denoiser";
        }
        pluginSamples += stripLimiterLatencySamples(*inputChannel, sampleRate)
                       + stripEqLatencySamples(*inputChannel, sampleRate);
    }

    int outputDeviceSamples = 0;
    int outputBufferSize = 0;
    int outputFillFrames = 0;
    int clockTargetFillFrames = 0;
    bool valid = false;
    if (clockOutputId != 0) {
        for (const auto& output : outputs) {
            if (output->id == clockOutputId && output->output != nullptr) {
                outputDeviceSamples = output->output->getLatencySamples();
                outputBufferSize = output->output->getBufferSize();
                if (outputBufferSize <= 0) outputBufferSize = currentBufferSize;
                outputFillFrames = output->output->getFillFrames();
                clockTargetFillFrames = output->output->getTargetFillFrames();
                report.clockDeviceType = output->outputDeviceType;
                for (const auto& insert : output->inserts) {
                    const int insertSamples = insert->latencySamples.load();
                    pluginSamples += insertSamples;
                    if (insertSamples > maxInsertSamples) {
                        maxInsertSamples = insertSamples;
                        maxInsertId = insert->id;
                        maxInsertName = insert->pluginName;
                    }
                }
                const int denoiserSamples = spectralDenoiserLatencySamples(*output);
                pluginSamples += denoiserSamples;
                if (denoiserSamples > maxInsertSamples) {
                    maxInsertSamples = denoiserSamples;
                    maxInsertId = 0xFFFFFFFFu;
                    maxInsertName = "Spectral Denoiser";
                }
                pluginSamples += stripLimiterLatencySamples(*output, sampleRate)
                               + stripEqLatencySamples(*output, sampleRate);
                valid = true;
                break;
            }
        }
    }

    // The capture read delay is part of the input device figure (see
    // DeviceStream); the output FIFO's actual fill is reported separately.
    const double fifoMs = outputFillFrames / sampleRate * 1000.0;
    // InputCapture::getLatencySamples() reports device latency + the capture
    // read delay; OutputPlayback::getLatencySamples() reports device latency
    // + one device buffer, with the FIFO fill reported by fifoMs.
    report.inputDeviceMs = inputDeviceSamples / sampleRate * 1000.0;
    report.outputDeviceMs = outputDeviceSamples / sampleRate * 1000.0;
    report.pluginMs = pluginSamples / sampleRate * 1000.0;
    report.maxInsertId = maxInsertId;
    report.maxInsertName = maxInsertName;
    report.maxInsertMs = maxInsertSamples / sampleRate * 1000.0;
    report.fifoMs = fifoMs;
    report.totalMs = report.inputDeviceMs + report.pluginMs + report.outputDeviceMs + report.fifoMs;
    // Split the two device totals so the budget can show what the app chose
    // (the read delay and the device buffer) separately from what the driver
    // reports. InputCapture::getLatencySamples() is device latency + read
    // delay; OutputPlayback::getLatencySamples() is device latency + one
    // device buffer.
    report.inputReadDelayFrames = inputReadDelaySamples;
    report.inputFifoDelayMs = inputReadDelaySamples / sampleRate * 1000.0;
    report.inputDeviceLatencyMs =
        juce::jmax(0, inputDeviceSamples - inputReadDelaySamples) / sampleRate * 1000.0;
    report.outputBufferMs = outputBufferSize / sampleRate * 1000.0;
    report.outputDeviceLatencyMs =
        juce::jmax(0, outputDeviceSamples - outputBufferSize) / sampleRate * 1000.0;
    report.clockFillFrames = outputFillFrames;
    report.clockTargetFillFrames = clockTargetFillFrames;
    if (inputChannel != nullptr && clockOutputId != 0) {
        for (const auto& output : outputs) {
            if (output->id != clockOutputId) continue;
            // Different endpoints mean two independent clocks; the capture
            // buffer cannot be removed without moving one of them.
            report.separateClocks = !inputChannel->duplexInput.get()
                                 && inputChannel->inputDeviceName.isNotEmpty()
                                 && inputChannel->inputDeviceName != output->outputDeviceName;
            break;
        }
    }
    report.requestedOutputBufferSize = requestedOutputBufferSize_;
    report.inputBufferSize = inputBufferSize;
    report.outputBufferSize = outputBufferSize;
    report.sampleRate = sampleRate;
    // Aggregate the FIFO counters directly: getFifoDiagnostics() takes its
    // own try-lock on stateLock, which is already held here, so calling it
    // would always fall through to zeros.
    report.underruns = 0;
    report.overruns = 0;
    for (const auto& entry : inputCaptures_) {
        if (entry.second.capture != nullptr) {
            report.overruns += entry.second.capture->getInputOverruns();
            report.underruns += entry.second.capture->getInputUnderruns();
        }
    }
    for (const auto& output : outputs) {
        if (output->output != nullptr) {
            report.underruns += output->output->getOutputUnderruns();
        }
    }
    report.valid = valid;
    report.measuredValid = latencyMeasuredValid_.load(std::memory_order_relaxed);
    report.measuredMs = report.measuredValid ? getMeasuredLatencyMs() : 0.0;
    report.measureFailed = latencyMeasureFailed_.load(std::memory_order_relaxed);
    // What the model cannot explain: the part of the physical round trip that
    // is neither our queues nor the plugin chain. A large positive value is
    // the signature of hidden buffering (rate conversion, a virtual cable, a
    // driver that reports a smaller period than it runs).
    report.unexpectedMs = report.measuredValid ? report.measuredMs - report.totalMs : 0.0;
    return report;
}

AudioEngine::FifoDiagnostics AudioEngine::getFifoDiagnostics() const {
    FifoDiagnostics diagnostics;
    const juce::ScopedTryLock lock(stateLock);
    if (!lock.isLocked()) {
        return diagnostics;
    }

    for (const auto& entry : inputCaptures_) {
        const auto& capture = entry.second.capture;
        if (capture != nullptr) {
            diagnostics.inputOverruns += capture->getInputOverruns();
            diagnostics.inputUnderruns += capture->getInputUnderruns();
            diagnostics.inputDroppedFrames += capture->getInputDroppedFrames();
        }
    }

    for (const auto& output : outputs) {
        if (output->output != nullptr) {
            diagnostics.outputUnderruns += output->output->getOutputUnderruns();
            diagnostics.outputDroppedFrames += output->output->getOutputDroppedFrames();
            diagnostics.outputLowWatermarks += output->output->getOutputLowWatermarks();
        }
    }
    diagnostics.renderMisses = renderMisses_.load(std::memory_order_relaxed);

    return diagnostics;
}

void AudioEngine::startLatencyMeasurement(const juce::String& listenDevice) {
    const juce::ScopedLock lock(stateLock);
    latencyListenDevice_ = listenDevice;
    latencyMeasureActive_.store(true, std::memory_order_relaxed);
    latencyMeasureEmitted_.store(false, std::memory_order_relaxed);
    latencyMeasuredValid_.store(false, std::memory_order_relaxed);
    latencyMeasuredSamples_.store(0, std::memory_order_relaxed);
    latencyMeasuredRate_.store(currentSampleRate, std::memory_order_relaxed);
    latencyMeasureFailed_.store(false, std::memory_order_relaxed);
    latencyMeasureStartMs_.store(juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
}

void AudioEngine::cancelLatencyMeasurementImpl() {
    latencyMeasureActive_.store(false, std::memory_order_relaxed);
    for (auto& entry : inputCaptures_) {
        if (entry.second.capture) entry.second.capture->endLatencyMeasurement();
    }
}

void AudioEngine::cancelLatencyMeasurement() {
    const juce::ScopedLock lock(stateLock);
    cancelLatencyMeasurementImpl();
}

void AudioEngine::resetLatencyMeasurementLocked() {
    cancelLatencyMeasurementImpl();
    latencyMeasureEmitted_.store(false, std::memory_order_relaxed);
    latencyMeasuredValid_.store(false, std::memory_order_relaxed);
    latencyMeasuredSamples_.store(0, std::memory_order_relaxed);
    latencyMeasureFailed_.store(false, std::memory_order_relaxed);
}

void AudioEngine::resetLatencyMeasurement() {
    const juce::ScopedLock lock(stateLock);
    resetLatencyMeasurementLocked();
}

void AudioEngine::pollLatencyMeasurement() {
    if (!latencyMeasureActive_.load(std::memory_order_relaxed)) return;
    const auto started = latencyMeasureStartMs_.load(std::memory_order_relaxed);
    if (juce::Time::getMillisecondCounter() - started <= kLatencyMeasureTimeoutMs) return;

    const juce::ScopedLock lock(stateLock);
    if (!latencyMeasureActive_.load(std::memory_order_relaxed)) return;
    // Nothing rendered the impulse in time: no clock output, no input
    // capture, or a stopped clock. Report it instead of leaving the button
    // stuck on "Cancel" forever.
    latencyMeasureActive_.store(false, std::memory_order_relaxed);
    latencyMeasureFailed_.store(true, std::memory_order_relaxed);
    for (auto& entry : inputCaptures_) {
        if (entry.second.capture) entry.second.capture->endLatencyMeasurement();
    }
}

double AudioEngine::getMeasuredLatencyMs() const {
    auto rate = latencyMeasuredRate_.load(std::memory_order_relaxed);
    if (rate <= 0.0) rate = currentSampleRate > 0.0 ? currentSampleRate : 48000.0;
    return latencyMeasuredSamples_.load(std::memory_order_relaxed) / rate * 1000.0;
}

AudioEngine::HighestLatencyInsert AudioEngine::getHighestLatencyInsert(std::uint32_t channelId) const {
    HighestLatencyInsert result;
    result.channelId = channelId;
    const juce::ScopedTryLock lock(stateLock);
    if (!lock.isLocked()) return result;
    const Channel* ch = nullptr;
    for (const auto& c : channels) if (c->id == channelId) { ch = c.get(); break; }
    if (ch == nullptr) return result;
    int maxLat = 0;
    for (const auto& ins : ch->inserts) {
        const int insLat = ins->latencySamples.load();
        if (insLat > maxLat) {
            maxLat = insLat;
            result.insertId = ins->id;
            result.name = ins->pluginName;
            result.latencySamples = insLat;
        }
    }
    return result;
}

float AudioEngine::decibelsToGain(float gainDb) {
    return gainDb <= AudioEngine::kSendOffDb ? 0.0f : std::pow(10.0f, gainDb / 20.0f);
}

//==============================================================================
// Background device opener, used on startup. Opening a WASAPI endpoint is a
// blocking driver call, and some drivers spend many seconds answering the
// format/channel probes JUCE runs before the stream opens (a USB endpoint on
// this machine needs ~12 s). Startup builds the strips on the message thread
// and then opens the streams here, in parallel, so the window stays usable.

class AudioEngine::DeviceOpenThread final : public juce::Thread {
public:
    explicit DeviceOpenThread(std::function<void(const std::function<bool()>&)> jobToRun)
        : juce::Thread("PluginRack device opener"), job(std::move(jobToRun)) {}

    void run() override {
       #if JUCE_WINDOWS
        const auto comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
       #endif
        job([this] { return threadShouldExit(); });
       #if JUCE_WINDOWS
        if (SUCCEEDED(comResult)) CoUninitialize();
       #endif
    }

private:
    std::function<void(const std::function<bool()>&)> job;
};

void AudioEngine::startDeviceJob(std::function<void(const std::function<bool()>&)> job) {
    if (openingDevices_.exchange(true)) return;
    {
        const juce::ScopedLock lock(deviceOpenErrorsLock_);
        deviceOpenErrors_.clear();
    }
    deviceOpenProgress_.store(0);
    deviceOpenTotal_.store(0);
    deviceOpenFinished_.store(false);
    clearDeviceOpens();

    if (deviceOpenThread_ != nullptr) {
        // The previous run has finished (openingDevices_ was false); join it
        // before handing the thread object to a fresh run.
        deviceOpenThread_->stopThread(2000);
        deviceOpenThread_.reset();
    }
    deviceOpenThread_ = std::make_unique<DeviceOpenThread>(std::move(job));
    deviceOpenThread_->startThread();
}

void AudioEngine::openStripsDevicesAsync() {
    startDeviceJob([this](const std::function<bool()>& shouldExit) {
        openStripsDevicesNow(shouldExit, /*forceInputs=*/false);
    });
}

void AudioEngine::openStripsDevicesNow(const std::function<bool()>& shouldExit, bool forceInputs) {
    // Exactly one pass. A device that refuses is reported and left silent
    // instead of being retried with another type, rate, or buffer size.
    juce::Logger::writeToLog("audio: opening devices");
    const auto errors = openAllDevicesPass(shouldExit, forceInputs);
    modeCacheBypass_.store(false, std::memory_order_relaxed);
    clearDeviceOpens();
    juce::Logger::writeToLog(errors.isNotEmpty() ? "audio: devices open with errors\n" + errors
                                                 : juce::String("audio: devices open"));

    {
        const juce::ScopedLock lock(deviceOpenErrorsLock_);
        deviceOpenErrors_ = errors;
    }
    // Publish completion before clearing the flag the UI uses to show progress.
    deviceOpenFinished_.store(true, std::memory_order_release);
    openingDevices_.store(false, std::memory_order_release);
}

void AudioEngine::stopDeviceOpenThread() {
    if (deviceOpenThread_ != nullptr) {
        deviceOpenThread_->signalThreadShouldExit();
        // Wait for an in-flight driver probe: the alternative is freeing the
        // strips this thread is still reading.
        deviceOpenThread_->stopThread(-1);
        deviceOpenThread_.reset();
    }
    openingDevices_.store(false);
}

juce::String AudioEngine::takeDeviceOpenErrors() {
    const juce::ScopedLock lock(deviceOpenErrorsLock_);
    auto errors = deviceOpenErrors_;
    deviceOpenErrors_.clear();
    return errors;
}

void AudioEngine::beginDeviceOpen(const juce::String& label) {
    const juce::ScopedLock lock(deviceOpenActiveLock_);
    deviceOpenActive_.push_back(label);
}

void AudioEngine::endDeviceOpen(const juce::String& label) {
    const juce::ScopedLock lock(deviceOpenActiveLock_);
    const auto it = std::find(deviceOpenActive_.begin(), deviceOpenActive_.end(), label);
    if (it != deviceOpenActive_.end()) deviceOpenActive_.erase(it);
}

void AudioEngine::clearDeviceOpens() {
    const juce::ScopedLock lock(deviceOpenActiveLock_);
    deviceOpenActive_.clear();
}

juce::String AudioEngine::getDeviceOpenStatus() const {
    const juce::ScopedLock lock(deviceOpenActiveLock_);
    if (deviceOpenActive_.empty()) return {};
    if (deviceOpenActive_.size() == 1) return "opening " + deviceOpenActive_.front();

    // The clock and every strip start together; say so instead of showing one.
    const auto count = (int) deviceOpenActive_.size();
    juce::String text = "opening " + juce::String(count) + " endpoints in parallel: "
                      + deviceOpenActive_[0] + ", " + deviceOpenActive_[1];
    if (count > 2) text << " (+" + juce::String(count - 2) + " more)";
    return text;
}

juce::String AudioEngine::getClockOutputDeviceName() const {
    const juce::ScopedLock lock(stateLock);
    for (const auto& output : outputs) {
        if (output->id == clockOutputId) return output->outputDeviceName;
    }
    return {};
}

bool AudioEngine::isClockOutput(std::uint32_t outputId) const {
    const juce::ScopedLock lock(stateLock);
    return outputId != 0 && clockOutputId == outputId;
}

AudioEngine::DeviceSettings AudioEngine::getDeviceSettings() const {
    DeviceSettings settings;
    settings.inputBuffer = requestedInputBufferSize_.load();
    settings.outputBuffer = requestedOutputBufferSize_;
    settings.latencyTuningOptIn = latencyTuningOptIn_.load();
    settings.lowLatencyInput = lowLatencyInput_.load();
    settings.preferExclusive = preferExclusive_.load();
    settings.lowLatencyOutputs = lowLatencyOutputs_.load();
    settings.minimalClockFifo = minimalClockFifo_.load();
    settings.fullDuplexInput = fullDuplexInput_.load();
    settings.wasapiProbeCapEnabled = wasapiProbeCapEnabled_.load();
    settings.wasapiProbeCapChannels = wasapiProbeCapChannels_.load();
    settings.autoReloadDevices = autoReloadDevices_.load();
    settings.slicedRender = slicedRender_.load();
    settings.nonFiniteScrub = nonFiniteScrub_.load();
    settings.healthReload = healthReload_.load();
    settings.healthYellowUnderrunsPerMinute =
        healthYellowUnderrunsPerMinute_.load();
    settings.healthRedUnderrunsPerMinute =
        healthRedUnderrunsPerMinute_.load();
    settings.healthPinnedToEdgeMs = healthPinnedToEdgeMs_.load();
    settings.rackThumbnailFps = rackThumbnailFps_.load();
    settings.watchedDevices = getWatchedDevices();
    return settings;
}

void AudioEngine::setAutoReloadDevices(bool enabled) {
    autoReloadDevices_.store(enabled);
    bumpStructureRevision();
}

void AudioEngine::setRackThumbnailFps(int fps) {
    rackThumbnailFps_.store(juce::jlimit(1, 60, fps));
    bumpStructureRevision();
}

void AudioEngine::setWatchedDevices(const std::vector<pluginrack::WatchedDevice>& devices) {
    const juce::ScopedLock lock(watchedDevicesLock_);
    watchedDevices_ = devices;
    bumpStructureRevision();
}

std::vector<pluginrack::WatchedDevice> AudioEngine::getWatchedDevices() const {
    const juce::ScopedLock lock(watchedDevicesLock_);
    return watchedDevices_;
}

pluginrack::StreamHealth AudioEngine::getChannelHealth(std::uint32_t channelId) const {
    const juce::ScopedLock lock(stateLock);
    for (const auto& channel : channels) {
        if (channel->id != channelId) continue;
        if (channel->input == nullptr) return pluginrack::StreamHealth::Green;
        return channel->input->healthState();
    }
    return pluginrack::StreamHealth::Green;
}

pluginrack::StreamHealth AudioEngine::getOutputHealth(std::uint32_t outputId) const {
    const juce::ScopedLock lock(stateLock);
    for (const auto& output : outputs) {
        if (output->id != outputId) continue;
        if (output->output == nullptr) return pluginrack::StreamHealth::Green;
        return output->output->healthState();
    }
    return pluginrack::StreamHealth::Green;
}

pluginrack::StreamHealthThresholds AudioEngine::getHealthThresholds() const {
    pluginrack::StreamHealthThresholds thresholds;
    thresholds.yellowUnderrunsPerMinute = healthYellowUnderrunsPerMinute_.load();
    thresholds.redUnderrunsPerMinute = healthRedUnderrunsPerMinute_.load();
    thresholds.pinnedToEdgeMs = healthPinnedToEdgeMs_.load();
    return thresholds;
}

std::optional<pluginrack::EndpointCapabilities> AudioEngine::cachedCapabilities(
    const juce::String& endpoint) const {
    if (endpoint.isEmpty()) return std::nullopt;
    const juce::ScopedLock lock(deviceCacheLock_);
    return deviceCache_.lookupUsable(endpoint.toStdString(),
                                     juce::Time::currentTimeMillis());
}

void AudioEngine::applyHealthThresholdsToStreams() {
    const auto thresholds = getHealthThresholds();
    const juce::ScopedLock lock(stateLock);
    for (const auto& channel : channels) {
        if (channel->input != nullptr) channel->input->setHealthThresholds(thresholds);
    }
    for (const auto& output : outputs) {
        if (output->output != nullptr) output->output->setHealthThresholds(thresholds);
    }
}

AudioEngine::DeviceState AudioEngine::getChannelDeviceState(std::uint32_t channelId) const {
    const juce::ScopedLock lock(stateLock);
    for (const auto& channel : channels) {
        if (channel->id == channelId) {
            juce::String type;
            if (channel->input != nullptr) {
                type = channel->input->getActiveDeviceType();
            } else if (channel->duplexInput.get()) {
                // Fed by the clock's callback: that is the device's backend.
                for (const auto& o : outputs) {
                    if (o->id == clockOutputId) {
                        type = o->outputDeviceType;
                        break;
                    }
                }
            }
            return {channel->inputDeviceName, channel->inputError,
                    channel->input != nullptr ? channel->input->availableInputCount()
                                              : channel->duplexInputChannels,
                    type};
        }
    }
    return {};
}

AudioEngine::DeviceState AudioEngine::getOutputDeviceState(std::uint32_t outputId) const {
    const juce::ScopedLock lock(stateLock);
    for (const auto& output : outputs) {
        if (output->id == outputId)
            return {output->outputDeviceName, output->outputError, 0, output->outputDeviceType};
    }
    return {};
}

int AudioEngine::getChannelFeedbackNotches(std::uint32_t channelId, float* freqs,
                                           int max) const {
    return getChannelFeedbackNotches(channelId, freqs, nullptr, max);
}

int AudioEngine::getChannelFeedbackNotches(std::uint32_t channelId, float* freqs,
                                           float* depthsDb, int max) const {
    const juce::ScopedLock lock(stateLock);
    for (const auto& channel : channels) {
        if (channel->id == channelId && !channel->feedbackSuppressorProcessors.empty())
            return channel->feedbackSuppressorProcessors.front().copyActiveNotches(
                freqs, depthsDb, max);
    }
    return 0;
}

int AudioEngine::getOutputFeedbackNotches(std::uint32_t outputId, float* freqs,
                                          int max) const {
    return getOutputFeedbackNotches(outputId, freqs, nullptr, max);
}

int AudioEngine::getOutputFeedbackNotches(std::uint32_t outputId, float* freqs,
                                          float* depthsDb, int max) const {
    const juce::ScopedLock lock(stateLock);
    for (const auto& output : outputs) {
        if (output->id == outputId && !output->feedbackSuppressorProcessors.empty())
            return output->feedbackSuppressorProcessors.front().copyActiveNotches(
                freqs, depthsDb, max);
    }
    return 0;
}

std::vector<std::pair<juce::String, juce::String>> AudioEngine::getDeviceErrors() const {
    std::vector<std::pair<juce::String, juce::String>> errors;
    const juce::ScopedLock lock(stateLock);
    for (const auto& channel : channels) {
        if (channel->inputError.isNotEmpty())
            errors.emplace_back(channel->name, channel->inputError);
    }
    for (const auto& output : outputs) {
        if (output->outputError.isNotEmpty())
            errors.emplace_back(output->name, output->outputError);
    }
    return errors;
}

std::vector<juce::String> AudioEngine::getAssignedInputDeviceNames() const {
    std::vector<juce::String> names;
    const juce::ScopedLock lock(stateLock);
    for (const auto& channel : channels) {
        if (channel->input == nullptr || channel->inputDeviceName.isEmpty())
            continue;
        if (std::find(names.begin(), names.end(), channel->inputDeviceName) == names.end())
            names.push_back(channel->inputDeviceName);
    }
    return names;
}

void AudioEngine::applySettingsValues(const DeviceSettings& settings) {
    requestedInputBufferSize_.store(settings.inputBuffer);
    requestedOutputBufferSize_ = settings.outputBuffer;
    latencyTuningOptIn_.store(settings.latencyTuningOptIn);
    lowLatencyInput_.store(settings.latencyTuningOptIn && settings.lowLatencyInput);
    preferExclusive_.store(settings.latencyTuningOptIn && settings.preferExclusive);
    lowLatencyOutputs_.store(settings.latencyTuningOptIn && settings.lowLatencyOutputs);
    minimalClockFifo_.store(settings.latencyTuningOptIn && settings.minimalClockFifo);
    fullDuplexInput_.store(settings.latencyTuningOptIn && settings.fullDuplexInput);
    wasapiProbeCapEnabled_.store(settings.wasapiProbeCapEnabled);
    wasapiProbeCapChannels_.store(settings.wasapiProbeCapChannels);
    setWasapiProbeCapEnvironment(settings.wasapiProbeCapEnabled,
                                 settings.wasapiProbeCapChannels);
    autoReloadDevices_.store(settings.autoReloadDevices);
    slicedRender_.store(settings.slicedRender);
    nonFiniteScrub_.store(settings.nonFiniteScrub);
    healthReload_.store(settings.healthReload);
    healthYellowUnderrunsPerMinute_.store(settings.healthYellowUnderrunsPerMinute);
    healthRedUnderrunsPerMinute_.store(settings.healthRedUnderrunsPerMinute);
    healthPinnedToEdgeMs_.store(settings.healthPinnedToEdgeMs);
    applyHealthThresholdsToStreams();
    if (settings.rackThumbnailFps > 0) rackThumbnailFps_.store(settings.rackThumbnailFps);
    // Buffer and stream-mode changes replace every device, so a measurement
    // taken against the old configuration is no longer true.
    resetLatencyMeasurement();
}

void AudioEngine::applyDeviceSettings(const DeviceSettings& settings) {
    if (isOpeningDevices()) return;
    applySettingsValues(settings);
    setWatchedDevices(settings.watchedDevices);
    // A user-initiated apply must re-probe the mode: the cached answer is only
    // trusted by the automatic startup pass.
    modeCacheBypass_.store(true, std::memory_order_relaxed);
    startDeviceJob([this](const std::function<bool()>& shouldExit) {
        openStripsDevicesNow(shouldExit, /*forceInputs=*/true);
    });
}

void AudioEngine::setLatencyOption(LatencyOption option, bool enabled) {
    auto settings = getDeviceSettings();
    switch (option) {
        case LatencyOption::LowLatencyInput:   settings.lowLatencyInput = enabled; break;
        case LatencyOption::PreferExclusive:   settings.preferExclusive = enabled; break;
        case LatencyOption::LowLatencyOutputs: settings.lowLatencyOutputs = enabled; break;
        case LatencyOption::MinimalClockFifo:  settings.minimalClockFifo = enabled; break;
        case LatencyOption::FullDuplexInput:   settings.fullDuplexInput = enabled; break;
    }
    settings.latencyTuningOptIn = settings.lowLatencyInput
                               || settings.preferExclusive
                               || settings.lowLatencyOutputs
                               || settings.minimalClockFifo
                               || settings.fullDuplexInput;
    applyDeviceSettings(settings);
}

void AudioEngine::reloadAllDevicesAsync() {
    applyDeviceSettings(getDeviceSettings());
}

std::vector<AudioEngine::ReloadableDevice> AudioEngine::getReloadableDevices() const {
    std::vector<ReloadableDevice> devices;
    const juce::ScopedLock lock(stateLock);
    for (const auto& output : outputs) {
        if (output->outputDeviceName.isNotEmpty()
            && !isSilentDeviceName(output->outputDeviceName)) {
            devices.push_back({"Output  " + output->name + "  -  " + output->outputDeviceName,
                               output->outputDeviceName, output->id, false});
        }
    }
    std::set<juce::String> seenInputs;
    for (const auto& channel : channels) {
        if (channel->inputDeviceName.isNotEmpty()
            && !isSilentDeviceName(channel->inputDeviceName)
            && seenInputs.insert(channel->inputDeviceName).second) {
            devices.push_back({"Input  " + channel->name + "  -  " + channel->inputDeviceName,
                               channel->inputDeviceName, channel->id, true});
        }
    }
    return devices;
}

void AudioEngine::reloadDeviceAsync(const ReloadableDevice& device) {
    reloadDevicesAsync({device});
}

void AudioEngine::reloadDevicesAsync(const std::vector<ReloadableDevice>& devices) {
    if (devices.empty()) return;
    // A background reclaim retry must not disturb the visible latency readout
    // or the opening banner; only user-visible reloads reset that state.
    bool anyVisible = false;
    for (const auto& device : devices) {
        if (!device.quiet) {
            anyVisible = true;
            break;
        }
    }
    if (anyVisible) resetLatencyMeasurement();
    startDeviceJob([this, devices](const std::function<bool()>& shouldExit) {
        // A single-device Reload is an explicit request too: re-probe the mode
        // instead of trusting the cached answer, like Reload All does.
        modeCacheBypass_.store(true, std::memory_order_relaxed);
        deviceOpenTotal_.store((int) devices.size());
        deviceOpenProgress_.store(0);
        juce::StringArray failures;

        for (const auto& device : devices) {
            if (shouldExit()) break;
            const auto status = "reloading " + device.label;
            if (!device.quiet) beginDeviceOpen(status);
            // Single attempt: a device that refuses stays silent and is
            // reported, instead of being retried with a 400 ms pause.
            const auto error = device.isInput
                                   ? setChannelInputDeviceImpl(device.stripId, device.deviceName,
                                                               /*forceReopen=*/true)
                                   : setOutputDeviceImpl(device.stripId, device.deviceName,
                                                         currentSampleRate);
            if (!device.quiet) endDeviceOpen(status);
            if (error.isNotEmpty() && !device.quiet)
                failures.add(device.label + ": " + error);
            deviceOpenProgress_.fetch_add(1);
        }

        // A failed clock reload closes the old stream before trying the new
        // endpoint. Hand the render role to a live output or the internal
        // clock now, so the other devices do not stay silent until the next UI
        // availability poll.
        checkClockHealth();

        clearDeviceOpens();
        modeCacheBypass_.store(false, std::memory_order_relaxed);
        if (failures.isEmpty())
            juce::Logger::writeToLog("audio: reloaded " + juce::String((int) devices.size())
                                     + " device(s)");
        else
            juce::Logger::writeToLog("audio: reload failed\n" + failures.joinIntoString("\n"));
        {
            const juce::ScopedLock lock(deviceOpenErrorsLock_);
            deviceOpenErrors_ = failures.joinIntoString("\n");
        }
        deviceOpenFinished_.store(true, std::memory_order_release);
        openingDevices_.store(false, std::memory_order_release);
    });
}

juce::String AudioEngine::openAllDevicesPass(const std::function<bool()>& shouldExit, bool forceInputs) {
    struct Request {
        std::uint32_t id = 0;
        juce::String name;
        // Strip name, used for the progress line and the error text so the
        // user can see which strip a device belongs to.
        juce::String label;
    };

    // Snapshot the assignments under stateLock. Structural mutators bail out
    // while openingDevices_ is set, so the strips stay valid for this run.
    std::vector<Request> outputsWanted, inputsWanted;
    double openRate = 48000.0;
    {
        const juce::ScopedLock lock(stateLock);
        for (const auto& output : outputs) {
            if (output->outputDeviceName.isNotEmpty()
                && !isSilentDeviceName(output->outputDeviceName)) {
                outputsWanted.push_back({output->id, output->outputDeviceName, output->name});
            }
        }
        std::sort(outputsWanted.begin(), outputsWanted.end(),
                  [](const Request& a, const Request& b) { return a.id < b.id; });
        std::set<juce::String> seenInputs;
        for (const auto& channel : channels) {
            if (isSilentDeviceName(channel->inputDeviceName)) {
                continue;
            }
            // Channels sharing one device share one capture: open it once.
            if (seenInputs.insert(channel->inputDeviceName).second) {
                inputsWanted.push_back({channel->id, channel->inputDeviceName, channel->name});
            }
        }
        openRate = currentSampleRate;
        // Claim the clock role before the parallel open: the first output by
        // id defines the engine rate, and no other stream may take over.
        if (!outputsWanted.empty()) clockOutputId = outputsWanted.front().id;
    }

    deviceOpenTotal_.store((int) (outputsWanted.size() + inputsWanted.size()));
    juce::String errors;

    // Open the clock (first output) first, on its own. ASIO is single-client:
    // an input probe that grabs the endpoint first makes the clock fail with
    // "No such device" while Force ASIO (which opens the clock synchronously
    // first) works. Releasing any separate capture on the clock device before
    // the clock reopens is what lets Apply do in one pass what Force ASIO did.
    if (!outputsWanted.empty())
        releaseDeviceInputsForClock(outputsWanted.front().name);

    // Only a single-client ASIO clock, or an input capturing from the clock
    // endpoint, needs the outputs fully open before any input starts. On WASAPI
    // the two groups are independent, so probe them together and let the
    // slowest endpoint — not their sum — set the startup time.
    bool serializeForClock = false;
    if (!outputsWanted.empty()) {
        const auto& clockName = outputsWanted.front().name;
        const auto asioNames = DeviceEnumeration::asioDevices();
        // Any ASIO output is single-client, so an input probe can block it —
        // not only the clock. "System Default" resolves to the first type that
        // carries a device, which is ASIO when a driver is installed: unknown
        // means serialize.
        const bool anyAsioOutput = std::any_of(
            outputsWanted.begin(), outputsWanted.end(),
            [&asioNames](const Request& request) {
                return asioNames.contains(request.name);
            });
        const bool defaultMayBeAsio =
            !asioNames.isEmpty() && isSystemDefaultName(clockName);
        const bool inputSharesClock = std::any_of(
            inputsWanted.begin(), inputsWanted.end(),
            [&clockName](const Request& request) { return request.name == clockName; });
        serializeForClock = anyAsioOutput || defaultMayBeAsio || inputSharesClock;
    }

    // Device probes are the expensive part of startup. Keep each probe on its
    // own COM thread and let output and input probes overlap. Every stream is
    // attempted exactly once: a refusal is reported against its strip and the
    // device stays closed and silent, never retried.
    auto launchOutputs = [this, &shouldExit, &outputsWanted](double rate,
                                                             std::vector<juce::String>& results,
                                                             std::vector<std::thread>& helpers) {
        for (std::size_t i = 0; i < outputsWanted.size(); ++i) {
            const auto request = outputsWanted[i];
            helpers.emplace_back([this, &shouldExit, &results, request, rate, i] {
                if (shouldExit()) return;
               #if JUCE_WINDOWS
                const auto comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
               #endif
                const auto status = "output \"" + request.label + "\" - " + request.name;
                beginDeviceOpen(status);
                results[i] = setOutputDeviceImpl(request.id, request.name, rate);
                endDeviceOpen(status);
                deviceOpenProgress_.fetch_add(1);
               #if JUCE_WINDOWS
                if (SUCCEEDED(comResult)) CoUninitialize();
               #endif
            });
        }
    };

    auto launchInputs = [this, &shouldExit, &inputsWanted](double rate,
                                                           bool force,
                                                           std::vector<juce::String>& results,
                                                           std::vector<std::thread>& helpers) {
        for (std::size_t i = 0; i < inputsWanted.size(); ++i) {
            const auto request = inputsWanted[i];
            helpers.emplace_back([this, &shouldExit, &results, request, rate, force, i] {
                if (shouldExit()) return;
               #if JUCE_WINDOWS
                const auto comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
               #endif
                const auto status = "input \"" + request.label + "\" - " + request.name;
                beginDeviceOpen(status);
                results[i] = setChannelInputDeviceImpl(request.id, request.name, force, rate);
                endDeviceOpen(status);
                deviceOpenProgress_.fetch_add(1);
               #if JUCE_WINDOWS
                if (SUCCEEDED(comResult)) CoUninitialize();
               #endif
            });
        }
    };

    std::vector<juce::String> outputResults(outputsWanted.size());
    std::vector<juce::String> inputResults(inputsWanted.size());
    std::vector<std::thread> outputHelpers;
    std::vector<std::thread> inputHelpers;
    outputHelpers.reserve(outputsWanted.size());
    inputHelpers.reserve(inputsWanted.size());

    // Outputs start first. On WASAPI the inputs join them immediately; only a
    // single-client ASIO output, or an input capturing from the clock endpoint,
    // waits for the outputs to finish — the old always-serial order kept for
    // that single-client race.
    if (!shouldExit()) {
        launchOutputs(openRate, outputResults, outputHelpers);
        if (!serializeForClock) {
            // WASAPI: inputs do not depend on the clock open, so they probe
            // while the outputs are still negotiating.
            launchInputs(openRate, forceInputs, inputResults, inputHelpers);
        }
    }
    for (auto& helper : outputHelpers) helper.join();

    // retryClockOpen: a single-client ASIO driver (M-Audio M-Track) keeps
    // refusing CoCreateInstance for a moment after the previous stream is
    // closed — JUCE only releases the driver when the old device object is
    // destroyed — and reports the bare "No such device" from loadDriver().
    // Retry the clock a bounded number of times before any input opens, which
    // is exactly the recovery pressing Force ASIO performs by hand.
    if (!shouldExit() && !outputResults.empty() && !outputsWanted.empty()) {
        const auto firstError = outputResults.front();
        for (int attempt = 2;
             attempt <= kClockOpenAttempts && outputResults.front().isNotEmpty();
             ++attempt) {
            // Only a driver-busy refusal is worth retrying; a rejected buffer
            // size or a missing endpoint would fail the same way again.
            if (!isNoSuchDeviceError(outputResults.front())) break;
            juce::Thread::sleep(kClockRetryDelayMs);
            const auto status = "output \"" + outputsWanted.front().label + "\" - "
                              + outputsWanted.front().name + " (retry "
                              + juce::String(attempt) + ")";
            beginDeviceOpen(status);
            const auto retryError = setOutputDeviceImpl(outputsWanted.front().id,
                                                        outputsWanted.front().name, openRate);
            endDeviceOpen(status);
            juce::Logger::writeToLog("audio: clock retry " + juce::String(attempt)
                                     + (retryError.isEmpty() ? juce::String(" ok")
                                                             : " failed: " + retryError));
            if (retryError.isEmpty()) outputResults.front().clear();
        }

        // A single-client ASIO driver can keep refusing while the open runs on
        // this worker thread's throwaway COM apartment — retrying forever makes
        // no difference. Force ASIO succeeds because it opens from the message
        // thread, so a clock that is still refused is reopened there, which is
        // the same call on the same thread the user's workaround uses. The wait
        // is interruptible: quitting must not leave the message thread blocked
        // in stopDeviceOpenThread() while this worker waits for it to dispatch
        // the reopen.
        if (!shouldExit() && outputResults.front().isNotEmpty()
            && isNoSuchDeviceError(outputResults.front())
            && !juce::MessageManager::existsAndIsCurrentThread()) {
            struct ClockReopen {
                std::uint32_t id = 0;
                juce::String name;
                double rate = 0.0;
                juce::String result;
                juce::WaitableEvent done{false};
            };
            auto state = std::make_shared<ClockReopen>();
            state->id = outputsWanted.front().id;
            state->name = outputsWanted.front().name;
            state->rate = openRate;
            auto alive = prepareAliveToken_;
            juce::MessageManager::callAsync([this, alive, state] {
                if (!alive->load(std::memory_order_acquire)) {
                    state->done.signal();
                    return;
                }
                state->result = setOutputDeviceImpl(state->id, state->name, state->rate);
                state->done.signal();
            });
            while (!state->done.wait(100)) {
                if (shouldExit()) break;
            }
            if (!shouldExit()) {
                outputResults.front() = state->result;
                juce::Logger::writeToLog("audio: clock reopened on the message thread: "
                                         + (outputResults.front().isEmpty()
                                                ? juce::String("ok")
                                                : outputResults.front()));
            }
        }
        if (outputResults.front().isNotEmpty()) outputResults.front() = firstError;
    }

    if (serializeForClock && !shouldExit()) {
        // The clock just opened and may have negotiated a new rate; inputs
        // joined after it must follow that rate, not the pre-open snapshot.
        {
            const juce::ScopedLock sl(stateLock);
            openRate = currentSampleRate;
        }
        launchInputs(openRate, forceInputs, inputResults, inputHelpers);
    }
    for (auto& helper : inputHelpers) helper.join();

    // The saved clock endpoint can be gone (unplugged or disabled since the
    // scene was saved). Its refusal must not silence every other output:
    // promote a live one or fall back to the internal clock before reporting.
    const bool clockRecovered = checkClockHealth();

    // Every stream was attempted exactly once. Report each refusal against the
    // strip it belongs to; the failing device is already closed and silent.
    for (std::size_t i = 0; i < outputResults.size(); ++i) {
        if (outputResults[i].isEmpty()) continue;
        const auto label = "Output \"" + outputsWanted[i].label + "\" ("
                         + outputsWanted[i].name + ")";
        if (i == 0) {
            errors += label + " could not be opened as the clock";
            if (clockRecovered)
                errors += "; playback continues on " + getClockDescription();
            else
                errors += ", so nothing can play";
            errors += ": " + outputResults[i] + "\n";
        } else {
            errors += label + ": " + outputResults[i] + "\n";
        }
    }
    for (std::size_t i = 0; i < inputResults.size(); ++i) {
        if (inputResults[i].isEmpty()) continue;
        errors += "Input \"" + inputsWanted[i].label + "\" (" + inputsWanted[i].name
               + "): " + inputResults[i] + "\n";
    }

    return errors;
}

void AudioEngine::releaseDeviceInputsForClock(const juce::String& deviceName) {
    if (isSilentDeviceName(deviceName)) return;
    std::shared_ptr<InputCapture> previous;
    {
        const juce::ScopedLock lock(stateLock);
        const auto it = inputCaptures_.find(deviceName);
        if (it != inputCaptures_.end()) previous = it->second.capture;
        for (auto& c : channels) {
            if (c->inputDeviceName == deviceName) c->input.reset();
        }
        if (it != inputCaptures_.end()) inputCaptures_.erase(it);
    }
    if (previous != nullptr) previous->close();
}

