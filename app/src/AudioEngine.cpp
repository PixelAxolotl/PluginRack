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
// A loopback either answers inside this window or the hunt is abandoned.
constexpr std::uint32_t kLatencyMeasureTimeoutMs = 3000;

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

// Effects whose chain entry is missing run here. That covers scenes saved
// before per-strip ordering existed. Removal sets the effect's bypassed flag,
// so a removed effect stays silent through this path until picked again,
// which unbypasses it.
template <typename Effect, typename Processor>
void processNewMissing(const std::vector<pluginrack::ChainItem>& chain,
    const std::vector<Effect>& effects, std::vector<Processor>& processors,
    pluginrack::ChainItemType type, float* left, float* right, int frames) noexcept {
    const std::size_t count = std::min(effects.size(), processors.size());
    for (std::size_t i = 0; i < count; ++i) {
        const pluginrack::ChainItem item{type, effects[i].id};
        if (effects[i].bypassed) continue;
        if (std::find(chain.begin(), chain.end(), item) == chain.end())
            processors[i].process(left, right, frames);
    }
}

void processEffectsMissingFromChain(
    const std::vector<pluginrack::ChainItem>& chain,
    const std::vector<pluginrack::DynamicsEffect>& effects,
    std::vector<pluginrack::DynamicsProcessor>& processors, float* left, float* right,
    int frames) noexcept {
    const std::size_t count = std::min(effects.size(), processors.size());
    for (std::size_t i = 0; i < count; ++i) {
        const pluginrack::ChainItem item{pluginrack::ChainItemType::Dynamics, effects[i].id};
        if (effects[i].bypassed) continue;
        if (std::find(chain.begin(), chain.end(), item) == chain.end())
            processors[i].process(left, right, frames);
    }
}


// Per-built-in output taps: one mode gate per tap, set by the owning
// window (and cleared on close). setPluginTap() below flips the whole set.
enum class PluginTapKind { Eq, Dynamics, Multiband, DeEsser, Limiter };

template <typename Strip>
void setPluginTap(Strip& strip, PluginTapKind kind, std::uint32_t mode) noexcept {
    switch (kind) {
        case PluginTapKind::Eq: strip.eqTap.setMode(mode); break;
        case PluginTapKind::Dynamics: strip.dynamicsTap.setMode(mode); break;
        case PluginTapKind::Multiband: strip.multibandTap.setMode(mode); break;
        case PluginTapKind::DeEsser: strip.deEsserTap.setMode(mode); break;
        case PluginTapKind::Limiter: strip.limiterTap.setMode(mode); break;
    }
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
            // EqEngine::prepare only stores the rate/size and recomputes cached
            // coefficients; bands set via setChannelEq and the delay set via
            // setChannelEqDelay survive a rate change.
            channel->eq.prepare(currentSampleRate, maxBlockFrames_);
            if (channel->effectProcessors.size() != channel->effects.size()) {
                channel->effectProcessors.clear();
                channel->effectProcessors.reserve(channel->effects.size());
                for (const auto& effect : channel->effects) {
                    pluginrack::DynamicsProcessor processor;
                    processor.prepare(currentSampleRate);
                    processor.setEffect(effect);
                    channel->effectProcessors.push_back(std::move(processor));
                }
            }
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
            output->eq.prepare(currentSampleRate, maxBlockFrames_);
            if (output->effectProcessors.size() != output->effects.size()) {
                output->effectProcessors.clear();
                output->effectProcessors.reserve(output->effects.size());
                for (const auto& effect : output->effects) {
                    pluginrack::DynamicsProcessor processor;
                    processor.prepare(currentSampleRate);
                    processor.setEffect(effect);
                    output->effectProcessors.push_back(std::move(processor));
                }
            }
            for (auto& insert : output->inserts) {
                insert->sideChainBuffer.setSize(2, maxBlockFrames_, false, true, true);
                insert->sideChainBuffer.clear();
                hosts.push_back(&insert->pluginHost);
            }
            meters.observeOutput(output->id, 0.0f);
        }
        for (auto& bus : auxBuses) {
            bus->buffer.setSize(2, maxBlockFrames_, false, true, true);
            bus->buffer.clear();
        }
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
        oversizedBlockCount_.fetch_add(1, std::memory_order_relaxed);
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

    // Side-chain aux buses are summed from each contributing channel's
    // post-fader tap. That tap is the previous block's final strip signal, so
    // a bus never depends on where in this block its sources are rendered.
    for (auto& bus : auxBuses) {
        if (bus == nullptr) continue;
        bus->buffer.clear(0, frames);
        const float busGain = decibelsToGain(bus->gainDb.get());
        for (const auto sourceId : bus->sourceChannelIds) {
            const auto source = std::find_if(channels.begin(), channels.end(), [sourceId](const auto& c) {
                return c->id == sourceId;
            });
            if (source == channels.end() || (*source)->postFaderTap.getNumSamples() < frames) continue;
            const auto& tap = (*source)->postFaderTap;
            bus->buffer.addFrom(0, 0, tap, 0, 0, frames, busGain);
            const int rightTap = (*source)->layout == pluginrack::ChannelLayout::Mono ? 0 : 1;
            bus->buffer.addFrom(1, 0, tap, rightTap, 0, frames, busGain);
        }
    }

    // Resolve one insert's side-chain source into its preallocated
    // sideChainBuffer slice. Called once per insert per render slice, before
    // that insert runs, so the source buffers are all stable snapshots.
    const auto fillSideChain = [&](InsertSlot& insert, int start, int count) {
        if (insert.sideChainBuffer.getNumSamples() < start + count) return;
        insert.sideChainBuffer.clear(start, count);
        if (insert.sideChainSource == pluginrack::SideChainSource::None
            || insert.sideChainSourceId == 0) {
            return;
        }
        const juce::AudioBuffer<float>* source = nullptr;
        bool mono = false;
        if (insert.sideChainSource == pluginrack::SideChainSource::Channel) {
            const auto found = std::find_if(channels.begin(), channels.end(), [&insert](const auto& c) {
                return c->id == insert.sideChainSourceId;
            });
            if (found != channels.end() && *found != nullptr) {
                source = insert.sideChainTap == pluginrack::SideChainTap::PreInsert
                             ? &(*found)->preInsertTap
                             : &(*found)->postFaderTap;
                mono = (*found)->layout == pluginrack::ChannelLayout::Mono;
            }
        } else if (insert.sideChainSource == pluginrack::SideChainSource::Output) {
            const auto found = std::find_if(outputs.begin(), outputs.end(), [&insert](const auto& o) {
                return o->id == insert.sideChainSourceId;
            });
            if (found != outputs.end() && *found != nullptr) {
                source = insert.sideChainTap == pluginrack::SideChainTap::PreInsert
                             ? &(*found)->preInsertTap
                             : &(*found)->postFaderTap;
                mono = (*found)->layout == pluginrack::ChannelLayout::Mono;
            }
        } else if (insert.sideChainSource == pluginrack::SideChainSource::Bus) {
            const auto found = std::find_if(auxBuses.begin(), auxBuses.end(), [&insert](const auto& b) {
                return b != nullptr && b->id == insert.sideChainSourceId;
            });
            if (found != auxBuses.end()) source = &(*found)->buffer;
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
                channel->analyzerTap.writeTap(false, block.getReadPointer(0), block.getReadPointer(1), count);
                for (const auto& item : channel->chain) {
                    if (item.type == pluginrack::ChainItemType::Eq) {
                        if (!channel->eqBypassed.get())
                            channel->eq.process(block.getWritePointer(0), block.getWritePointer(1), count);
                        channel->eqTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
                    } else if (item.type == pluginrack::ChainItemType::Dynamics) {
                        auto effect = std::find_if(channel->effects.begin(), channel->effects.end(),
                                                   [id = item.id](const auto& value) { return value.id == id; });
                        if (effect != channel->effects.end()) {
                            const auto index = static_cast<std::size_t>(effect - channel->effects.begin());
                            if (index < channel->effectProcessors.size())
                                channel->effectProcessors[index].process(
                                    block.getWritePointer(0), block.getWritePointer(1), count);
                        }
                        channel->dynamicsTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
                    } else if (item.type == pluginrack::ChainItemType::Multiband) {
                        auto f = std::find_if(channel->multibandEffects.begin(), channel->multibandEffects.end(), [id=item.id](const auto& v){return v.id==id;});
                        if (f != channel->multibandEffects.end()) { const auto i = static_cast<std::size_t>(f-channel->multibandEffects.begin()); if (i < channel->multibandProcessors.size()) channel->multibandProcessors[i].process(block.getWritePointer(0), block.getWritePointer(1), count); }
                        channel->multibandTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
                    } else if (item.type == pluginrack::ChainItemType::DeEsser) {
                        auto f = std::find_if(channel->deEsserEffects.begin(), channel->deEsserEffects.end(), [id=item.id](const auto& v){return v.id==id;});
                        if (f != channel->deEsserEffects.end()) { const auto i = static_cast<std::size_t>(f-channel->deEsserEffects.begin()); if (i < channel->deEsserProcessors.size()) channel->deEsserProcessors[i].process(block.getWritePointer(0), block.getWritePointer(1), count); }
                        channel->deEsserTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
                    } else if (item.type == pluginrack::ChainItemType::Limiter) {
                        auto f = std::find_if(channel->limiterEffects.begin(), channel->limiterEffects.end(), [id=item.id](const auto& v){return v.id==id;});
                        if (f != channel->limiterEffects.end()) { const auto i = static_cast<std::size_t>(f-channel->limiterEffects.begin()); if (i < channel->limiterProcessors.size()) channel->limiterProcessors[i].process(block.getWritePointer(0), block.getWritePointer(1), count); }
                        channel->limiterTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
                    } else {
                        auto insert = std::find_if(channel->inserts.begin(), channel->inserts.end(),
                                                   [id = item.id](const auto& value) { return value->id == id; });
                        if (insert != channel->inserts.end()) {
                            fillSideChain(**insert, start, count);
                            crash_context::setCurrentRender((*insert)->crashLabel);
                            (*insert)->pluginHost.processBlock(
                                block, (*insert)->bypassed.get(),
                                (*insert)->sideChainBuffer.getReadPointer(0) + start,
                                (*insert)->sideChainBuffer.getReadPointer(1) + start, count);
                            crash_context::clearCurrentRender();
                            // Fault containment: mirror the child's death/wedge
                            // into the slot so the UI and reload path can see it.
                            const auto status = (*insert)->pluginHost.childHostStatus();
                            if (status.enabled && status.failed) {
                                (*insert)->failed = true;
                                (*insert)->failExitCode = status.exitCode;
                                if (!(*insert)->faultRecorded.get()) {
                                    (*insert)->faultRecorded = true;
                                    // faults.json is disk I/O. Never do it on
                                    // the audio thread, still less under
                                    // stateLock: one dead child would stall
                                    // every strip's render.
                                    const juce::String faultPath = (*insert)->pluginPath;
                                    const int faultExit = status.exitCode;
                                    const auto faultId = (*insert)->id;
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
                }
                // Older scenes can have effects without their dynamics chain
                // entries. Keep those effects live instead of showing a working
                // but inaudible UI.
                processEffectsMissingFromChain(channel->chain, channel->effects,
                                               channel->effectProcessors,
                                               block.getWritePointer(0), block.getWritePointer(1), count);
                processNewMissing(channel->chain, channel->multibandEffects, channel->multibandProcessors, pluginrack::ChainItemType::Multiband, block.getWritePointer(0), block.getWritePointer(1), count);
                processNewMissing(channel->chain, channel->deEsserEffects, channel->deEsserProcessors, pluginrack::ChainItemType::DeEsser, block.getWritePointer(0), block.getWritePointer(1), count);
                processNewMissing(channel->chain, channel->limiterEffects, channel->limiterProcessors, pluginrack::ChainItemType::Limiter, block.getWritePointer(0), block.getWritePointer(1), count);
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
        }

        const float peakL = channel->buffer.getMagnitude(0, 0, frames);
        const float peakR = channel->buffer.getMagnitude(1, 0, frames);
        meters.observeChannelRealtime(channel->id, juce::jmax(peakL, peakR));
        channel->peak = meters.channelPeak(channel->id);
        channel->peakL = peakL;
        channel->peakR = peakR;
        channel->rmsL = channel->buffer.getRMSLevel(0, 0, frames);
        channel->rmsR = channel->buffer.getRMSLevel(1, 0, frames);
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
        if (sendGainL != 0.0f) {
            (*outputIt)->buffer.addFrom(0, 0, (*channelIt)->buffer, 0, 0, frames, sendGainL);
        }
        if (sendGainR != 0.0f) {
            (*outputIt)->buffer.addFrom(1, 0, (*channelIt)->buffer, 1, 0, frames, sendGainR);
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
                juce::AudioBuffer<float> block(
                    output->buffer.getArrayOfWritePointers(), 2, start, count);
                // Same per-plugin taps as Channel: each built-in's RTA reads
                // its own output, so the EQ's curve never leaks into a later
                // plugin's RTA and each preview is the plugin's own signal.
                if (output->preInsertTap.getNumSamples() >= start + count) {
                    output->preInsertTap.copyFrom(0, start, output->buffer, 0, start, count);
                    output->preInsertTap.copyFrom(1, start, output->buffer, 1, start, count);
                }
                output->analyzerTap.writeTap(false, block.getReadPointer(0), block.getReadPointer(1), count);
                for (const auto& item : output->chain) {
                    if (item.type == pluginrack::ChainItemType::Eq) {
                        if (!output->eqBypassed.get())
                            output->eq.process(block.getWritePointer(0), block.getWritePointer(1), count);
                        output->eqTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
                    } else if (item.type == pluginrack::ChainItemType::Dynamics) {
                        auto effect = std::find_if(output->effects.begin(), output->effects.end(),
                                                   [id = item.id](const auto& value) { return value.id == id; });
                        if (effect != output->effects.end()) {
                            const auto index = static_cast<std::size_t>(effect - output->effects.begin());
                            if (index < output->effectProcessors.size())
                                output->effectProcessors[index].process(
                                    block.getWritePointer(0), block.getWritePointer(1), count);
                        }
                        output->dynamicsTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
                    } else if (item.type == pluginrack::ChainItemType::Multiband) {
                        auto f = std::find_if(output->multibandEffects.begin(), output->multibandEffects.end(), [id=item.id](const auto& v){return v.id==id;});
                        if (f != output->multibandEffects.end()) { const auto i = static_cast<std::size_t>(f-output->multibandEffects.begin()); if (i < output->multibandProcessors.size()) output->multibandProcessors[i].process(block.getWritePointer(0), block.getWritePointer(1), count); }
                        output->multibandTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
                    } else if (item.type == pluginrack::ChainItemType::DeEsser) {
                        auto f = std::find_if(output->deEsserEffects.begin(), output->deEsserEffects.end(), [id=item.id](const auto& v){return v.id==id;});
                        if (f != output->deEsserEffects.end()) { const auto i = static_cast<std::size_t>(f-output->deEsserEffects.begin()); if (i < output->deEsserProcessors.size()) output->deEsserProcessors[i].process(block.getWritePointer(0), block.getWritePointer(1), count); }
                        output->deEsserTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
                    } else if (item.type == pluginrack::ChainItemType::Limiter) {
                        auto f = std::find_if(output->limiterEffects.begin(), output->limiterEffects.end(), [id=item.id](const auto& v){return v.id==id;});
                        if (f != output->limiterEffects.end()) { const auto i = static_cast<std::size_t>(f-output->limiterEffects.begin()); if (i < output->limiterProcessors.size()) output->limiterProcessors[i].process(block.getWritePointer(0), block.getWritePointer(1), count); }
                        output->limiterTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
                    } else {
                        auto insert = std::find_if(output->inserts.begin(), output->inserts.end(),
                                                   [id = item.id](const auto& value) { return value->id == id; });
                        if (insert != output->inserts.end()) {
                            fillSideChain(**insert, start, count);
                            crash_context::setCurrentRender((*insert)->crashLabel);
                            (*insert)->pluginHost.processBlock(
                                block, (*insert)->bypassed.get(),
                                (*insert)->sideChainBuffer.getReadPointer(0) + start,
                                (*insert)->sideChainBuffer.getReadPointer(1) + start, count);
                            crash_context::clearCurrentRender();
                            const auto status = (*insert)->pluginHost.childHostStatus();
                            if (status.enabled && status.failed) {
                                (*insert)->failed = true;
                                (*insert)->failExitCode = status.exitCode;
                                if (!(*insert)->faultRecorded.get()) {
                                    (*insert)->faultRecorded = true;
                                    // faults.json is disk I/O. Never do it on
                                    // the audio thread, still less under
                                    // stateLock: one dead child would stall
                                    // every strip's render.
                                    const juce::String faultPath = (*insert)->pluginPath;
                                    const int faultExit = status.exitCode;
                                    const auto faultId = (*insert)->id;
                                    juce::MessageManager::callAsync([faultPath, faultExit, faultId] {
                                        PluginFaultGuard::record(faultPath, faultExit, faultId);
                                    });
                                }
                            }
                        }
                    }
                    scrubRange(output->buffer, start, count);
                }
                // Older scenes can have effects without their dynamics chain
                // entries.
                processEffectsMissingFromChain(output->chain, output->effects,
                                               output->effectProcessors,
                                               block.getWritePointer(0), block.getWritePointer(1), count);
                processNewMissing(output->chain, output->multibandEffects, output->multibandProcessors, pluginrack::ChainItemType::Multiband, block.getWritePointer(0), block.getWritePointer(1), count);
                processNewMissing(output->chain, output->deEsserEffects, output->deEsserProcessors, pluginrack::ChainItemType::DeEsser, block.getWritePointer(0), block.getWritePointer(1), count);
                processNewMissing(output->chain, output->limiterEffects, output->limiterProcessors, pluginrack::ChainItemType::Limiter, block.getWritePointer(0), block.getWritePointer(1), count);
                output->analyzerTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), count);
                scrubRange(output->buffer, start, count);
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
        if (output->muted.get()) {
            output->buffer.clear(0, frames);
        }
        if (output->postFaderTap.getNumSamples() >= frames) {
            output->postFaderTap.copyFrom(0, 0, output->buffer, 0, 0, frames);
            output->postFaderTap.copyFrom(1, 0, output->buffer, 1, 0, frames);
        }
        const float peakL = output->buffer.getMagnitude(0, 0, frames);
        const float peakR = output->buffer.getMagnitude(1, 0, frames);
        meters.observeOutputRealtime(output->id, juce::jmax(peakL, peakR));
        output->peak = meters.outputPeak(output->id);
        output->peakL = peakL;
        output->peakR = peakR;
        output->rmsL = output->buffer.getRMSLevel(0, 0, frames);
        output->rmsR = output->buffer.getRMSLevel(1, 0, frames);
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
        partialRenders_.fetch_add(1, std::memory_order_relaxed);
    }
    if (scrubbedThisBlock > 0) {
        renderFlags |= pluginrack::AudioTelemetry::flagNonFiniteScrub;
        scrubbedSamples_.fetch_add(static_cast<std::uint64_t>(scrubbedThisBlock),
                                   std::memory_order_relaxed);
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
    channels.erase(it);
    routes.erase(std::remove_if(routes.begin(), routes.end(),
                               [channelId](const auto& route) { return route->channelId == channelId; }),
                  routes.end());
    pruneUnusedCaptures();
    return {};
}

void AudioEngine::moveChannel(std::uint32_t channelId, int targetIndex) {
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
}

void AudioEngine::moveOutput(std::uint32_t outputId, int targetIndex) {
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
    // Preallocate the EQ so internal bands match the member mirror: a strip
    // created mid-session is usable the moment the UI un-bypasses it.
    channel->eq.prepare(currentSampleRate, currentBufferSize);
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
    output->eq.prepare(currentSampleRate, currentBufferSize);
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
    for (const auto& channel : channels) {
        auto route = std::make_unique<Route>();
        route->channelId = channel->id;
        route->outputId = output->id;
        route->sendGainDbL = kSendOffDb;
        route->sendGainDbR = kSendOffDb;
        routes.push_back(std::move(route));
    }
    outputs.push_back(std::move(output));
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
        for (auto it = outputs.begin(); it != outputs.end(); ++it) {
            if ((*it)->id == outputId) {
                it = outputs.erase(it);
                break;
            }
        }
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
                    o2->output->setRenderHook([this](int n) { this->render(n); });
                    break;
                }
            }
        }
        publishRenderOutputsLocked();
    }
    if (retiredPlayback != nullptr) {
        retiredPlayback->close();
    }
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
                insert->latencySamples = insert->pluginHost.getLatencySamples();

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
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) {
        return;
    }
    auto& inserts = channel->inserts;
    for (auto it = inserts.begin(); it != inserts.end(); ++it) {
        if ((*it)->id == insertId) {
            retiredInserts_.push_back(std::move(*it));
            inserts.erase(it);
            channel->chain.erase(std::remove_if(channel->chain.begin(), channel->chain.end(),
                                                [insertId](const auto& item) {
                                                    return item.type == pluginrack::ChainItemType::Insert &&
                                                           item.id == insertId;
                                                }),
                                 channel->chain.end());
            return;
        }

    }
}

void AudioEngine::removeChannelChainItem(std::uint32_t channelId, pluginrack::ChainItem item) {
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) return;
    channel->chain.erase(std::remove(channel->chain.begin(), channel->chain.end(), item),
                         channel->chain.end());
    // A removed effect must stay silent through processEffectsMissingFromChain
    // until the user picks it again (which unbypasses it).
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
    }
}

void AudioEngine::ensureChannelChainItem(std::uint32_t channelId, pluginrack::ChainItem item) {
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) return;
    if (std::none_of(channel->chain.begin(), channel->chain.end(),
                     [&item](const pluginrack::ChainItem& entry) { return entry == item; }))
        channel->chain.push_back(item);
}

void AudioEngine::ensureOutputChainItem(std::uint32_t outputId, pluginrack::ChainItem item) {
    const juce::ScopedLock lock(stateLock);
    auto* output = findOutput(outputId);
    if (output == nullptr) return;
    if (std::none_of(output->chain.begin(), output->chain.end(),
                     [&item](const pluginrack::ChainItem& entry) { return entry == item; }))
        output->chain.push_back(item);
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
    }
}

void AudioEngine::setChannelChain(std::uint32_t channelId,
                                  const std::vector<pluginrack::ChainItem>& chain) {
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) channel->chain = chain;
}

void AudioEngine::setOutputChain(std::uint32_t outputId,
                                 const std::vector<pluginrack::ChainItem>& chain) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) output->chain = chain;
}

void AudioEngine::moveChannelChainItem(std::uint32_t channelId, pluginrack::ChainItem item,
                                       int targetIndex) {
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) return;
    auto& chain = channel->chain;
    pluginrack::moveChainItem(chain, item, targetIndex);
}

void AudioEngine::setPluginBypassed(std::uint32_t channelId, std::uint32_t insertId, bool bypassed) {
    if (auto* insert = findInsert(channelId, insertId)) {
        insert->bypassed = bypassed;
        insert->pluginHost.refreshLatencyCache();
        insert->latencySamples = insert->pluginHost.getLatencySamples();
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
    }
}

std::uint32_t AudioEngine::addAuxBus(const juce::String& name,
                                     const std::vector<std::uint32_t>& sourceChannelIds) {
    const juce::ScopedLock lock(stateLock);
    auto bus = std::make_unique<AuxBus>();
    bus->id = nextAuxBusId++;
    bus->name = name.isNotEmpty() ? name : ("Bus " + juce::String(bus->id));
    bus->sourceChannelIds = sourceChannelIds;
    bus->buffer.setSize(2, maxBlockFrames_, false, true, true);
    bus->buffer.clear();
    const std::uint32_t id = bus->id;
    auxBuses.push_back(std::move(bus));
    return id;
}

void AudioEngine::removeAuxBus(std::uint32_t busId) {
    const juce::ScopedLock lock(stateLock);
    auxBuses.erase(std::remove_if(auxBuses.begin(), auxBuses.end(),
                                  [busId](const auto& bus) { return bus->id == busId; }),
                   auxBuses.end());
    // Inserts naming the removed bus fall back to no side chain instead of
    // pointing at an id that no longer exists.
    const auto clearBusSource = [busId](auto& inserts) {
        for (auto& insert : inserts) {
            if (insert->sideChainSource == pluginrack::SideChainSource::Bus &&
                insert->sideChainSourceId == busId) {
                insert->sideChainSource = pluginrack::SideChainSource::None;
                insert->sideChainSourceId = 0;
            }
        }
    };
    for (auto& channel : channels) clearBusSource(channel->inserts);
    for (auto& output : outputs) clearBusSource(output->inserts);
}

void AudioEngine::setAuxBus(std::uint32_t busId, const juce::String& name, float gainDb,
                            const std::vector<std::uint32_t>& sourceChannelIds) {
    const juce::ScopedLock lock(stateLock);
    for (auto& bus : auxBuses) {
        if (bus->id != busId) continue;
        if (name.isNotEmpty()) bus->name = name;
        bus->gainDb = juce::jlimit(-60.0f, 12.0f, gainDb);
        bus->sourceChannelIds = sourceChannelIds;
        return;
    }
}

bool AudioEngine::reloadInsert(std::uint32_t channelId, std::uint32_t insertId) {
    InsertSlot* insert = findInsert(channelId, insertId);
    bool isOutput = false;
    if (insert == nullptr) {
        insert = findOutputInsert(channelId, insertId);
        isOutput = (insert != nullptr);
    }
    if (insert == nullptr) return false;
    if (!getChildProcessDsp()) return false;

    insert->pluginHost.stopChildHost();
    juce::String error;
    const bool ok = insert->pluginHost.loadChildHost(
        juce::File(insert->pluginPath), insert->pluginUniqueId,
        insert->pluginHost.getStateAsBase64(), currentSampleRate, currentBufferSize, 20000,
        error);
    if (ok) {
        insert->failed = false;
        insert->failExitCode = 0;
        insert->faultRecorded = false;
        insert->latencySamples = insert->pluginHost.getLatencySamples();
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
                insert->latencySamples = insert->pluginHost.getLatencySamples();

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
    const juce::ScopedLock lock(stateLock);
    auto* output = findOutput(outputId);
    if (output == nullptr) {
        return;
    }
    auto& inserts = output->inserts;
    for (auto it = inserts.begin(); it != inserts.end(); ++it) {
        if ((*it)->id == insertId) {
            retiredInserts_.push_back(std::move(*it));
            inserts.erase(it);
            output->chain.erase(std::remove_if(output->chain.begin(), output->chain.end(),
                                               [insertId](const auto& item) {
                                                   return item.type == pluginrack::ChainItemType::Insert &&
                                                          item.id == insertId;
                                               }),
                                output->chain.end());
            return;
        }

    }
}

void AudioEngine::moveOutputChainItem(std::uint32_t outputId, pluginrack::ChainItem item,
                                      int targetIndex) {
    const juce::ScopedLock lock(stateLock);
    auto* output = findOutput(outputId);
    if (output == nullptr) return;
    auto& chain = output->chain;
    pluginrack::moveChainItem(chain, item, targetIndex);
}

void AudioEngine::setOutputPluginBypassed(std::uint32_t outputId, std::uint32_t insertId, bool bypassed) {
    if (auto* insert = findOutputInsert(outputId, insertId)) {
        insert->bypassed = bypassed;
        insert->pluginHost.refreshLatencyCache();
        insert->latencySamples = insert->pluginHost.getLatencySamples();
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
    }
}

void AudioEngine::setChannelGain(std::uint32_t channelId, float gainDb) {
    if (auto* channel = findChannel(channelId)) {
        channel->gainDb = gainDb;
    }
}

void AudioEngine::setChannelTrim(std::uint32_t channelId, float trimDb) {
    if (auto* channel = findChannel(channelId)) {
        channel->trimDb = trimDb;
    }
}

void AudioEngine::setChannelPan(std::uint32_t channelId, float pan) {
    if (auto* channel = findChannel(channelId)) {
        channel->pan = pan;
    }
}

void AudioEngine::setChannelName(std::uint32_t channelId, const juce::String& name) {
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) channel->name = name;
}

void AudioEngine::setChannelMuted(std::uint32_t channelId, bool muted) {
    if (auto* channel = findChannel(channelId)) {
        channel->muted = muted;
    }
}

void AudioEngine::setChannelSoloed(std::uint32_t channelId, bool soloed) {
    if (auto* channel = findChannel(channelId)) {
        channel->soloed = soloed;
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
    }
    // Re-assign the device so the shared capture is re-opened (or widened)
    // for the new layout; silent channels keep nothing to reopen.
    if (!isSilentDeviceName(deviceName)) {
        setChannelInputDevice(channelId, deviceName);
    }
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
        setChannelInputDevice(channelId, deviceName);
    }
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
    }
    if (!isSilentDeviceName(deviceName)) {
        setChannelInputDevice(channelId, deviceName);
    }
}

void AudioEngine::setSendGain(std::uint32_t channelId, std::uint32_t outputId, float gainDb) {
    for (auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            route->sendGainDbL = gainDb;
            route->sendGainDbR = gainDb;
            return;
        }
    }
}

void AudioEngine::setSendGainL(std::uint32_t channelId, std::uint32_t outputId, float gainDb) {
    for (auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            route->sendGainDbL = gainDb;
            return;
        }
    }
}

void AudioEngine::setSendGainR(std::uint32_t channelId, std::uint32_t outputId, float gainDb) {
    for (auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            route->sendGainDbR = gainDb;
            return;
        }
    }
}

void AudioEngine::setOutputGain(std::uint32_t outputId, float gainDb) {
    if (auto* output = findOutput(outputId)) output->gainDb = gainDb;
}

void AudioEngine::setOutputMuted(std::uint32_t outputId, bool muted) {
    if (auto* output = findOutput(outputId)) output->muted = muted;
}

void AudioEngine::setOutputName(std::uint32_t outputId, const juce::String& name) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) output->name = name;
}

void AudioEngine::setOutputLayout(std::uint32_t outputId, pluginrack::ChannelLayout layout) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) output->layout = layout;
}

void AudioEngine::setOutputFlipped(std::uint32_t outputId, bool flipped) {
    if (auto* output = findOutput(outputId)) output->flipped = flipped;
}

void AudioEngine::setChannelEq(std::uint32_t channelId, const std::vector<pluginrack::EqBand>& bands) {
    // Message thread: stateLock serializes the band copy and the coefficient
    // rebuild against render()'s taps (mirrors core's EqState mutex). Bypass
    // state is kept, like core's setChannelEq.
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) {
        channel->eqBands = bands;
        channel->eq.setBands(channel->eqBands);
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
    }
}

void AudioEngine::setChannelEqDelay(std::uint32_t channelId, float delayMs) {
    // Message thread: same lock as setChannelEq so the allocation the first
    // non-zero delay performs cannot race render()'s EQ tap.
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) {
        channel->eqDelayMs = delayMs;
        channel->eq.setDelayMs(delayMs);
    }
}

void AudioEngine::setChannelEqPhaseFlip(std::uint32_t channelId, bool flipped) {
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) {
        channel->eqPhaseFlip = flipped;
        channel->eq.setPhaseFlip(flipped);
    }
}

void AudioEngine::setChannelColourHue(std::uint32_t channelId, float hueDegrees) {
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) channel->colourHue = hueDegrees;
}

void AudioEngine::setChannelEqRtaOffset(std::uint32_t channelId, float offsetDb) {
    // Display only, but it is saved with the scene, so it follows the same
    // message-thread lock rule as the bands.
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) channel->eqRtaOffsetDb = offsetDb;
}

void AudioEngine::setChannelEqRtaWeighting(std::uint32_t channelId, int weighting) {
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId))
        channel->eqRtaWeighting = juce::jlimit(0, 2, weighting);
}

void AudioEngine::setChannelMultibandRtaOffset(std::uint32_t channelId, float offsetDb) {
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) channel->multibandRtaOffsetDb = offsetDb;
}

void AudioEngine::setChannelMultibandRtaWeighting(std::uint32_t channelId, int weighting) {
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId))
        channel->multibandRtaWeighting = juce::jlimit(0, 2, weighting);
}

void AudioEngine::setChannelDeEsserRtaOffset(std::uint32_t channelId, float offsetDb) {
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) channel->deEsserRtaOffsetDb = offsetDb;
}

void AudioEngine::setChannelDeEsserRtaWeighting(std::uint32_t channelId, int weighting) {
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId))
        channel->deEsserRtaWeighting = juce::jlimit(0, 2, weighting);
}

void AudioEngine::setOutputEq(std::uint32_t outputId,
                              const std::vector<pluginrack::EqBand>& bands) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) {
        output->eqBands = bands;
        output->eq.setBands(output->eqBands);
    }
}

void AudioEngine::setOutputEqBypassed(std::uint32_t outputId, bool bypassed) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) {
        output->eqBypassed = bypassed;
        if (bypassed) {
            output->eq.reset();
        }
    }
}

void AudioEngine::setOutputEqDelay(std::uint32_t outputId, float delayMs) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) {
        output->eqDelayMs = delayMs;
        output->eq.setDelayMs(delayMs);
    }
}

void AudioEngine::setOutputEqPhaseFlip(std::uint32_t outputId, bool flipped) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) {
        output->eqPhaseFlip = flipped;
        output->eq.setPhaseFlip(flipped);
    }
}

void AudioEngine::setOutputColourHue(std::uint32_t outputId, float hueDegrees) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) output->colourHue = hueDegrees;
}

void AudioEngine::setOutputEqRtaOffset(std::uint32_t outputId, float offsetDb) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) output->eqRtaOffsetDb = offsetDb;
}

void AudioEngine::setOutputEqRtaWeighting(std::uint32_t outputId, int weighting) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId))
        output->eqRtaWeighting = juce::jlimit(0, 2, weighting);
}

void AudioEngine::setOutputMultibandRtaOffset(std::uint32_t outputId, float offsetDb) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) output->multibandRtaOffsetDb = offsetDb;
}

void AudioEngine::setOutputMultibandRtaWeighting(std::uint32_t outputId, int weighting) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId))
        output->multibandRtaWeighting = juce::jlimit(0, 2, weighting);
}

void AudioEngine::setOutputDeEsserRtaOffset(std::uint32_t outputId, float offsetDb) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId)) output->deEsserRtaOffsetDb = offsetDb;
}

void AudioEngine::setOutputDeEsserRtaWeighting(std::uint32_t outputId, int weighting) {
    const juce::ScopedLock lock(stateLock);
    if (auto* output = findOutput(outputId))
        output->deEsserRtaWeighting = juce::jlimit(0, 2, weighting);
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
}

template <typename Effect, typename Processor, typename Strip>
void syncNewEffects(Strip* strip, const std::vector<Effect>& effects,
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
    (void)strip;
}

void AudioEngine::setChannelMultiband(std::uint32_t channelId, const std::vector<pluginrack::MultibandEffect>& effects) {
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) return;
    syncNewEffects(channel, effects, channel->multibandEffects, channel->multibandProcessors, currentSampleRate);
}
void AudioEngine::setOutputMultiband(std::uint32_t outputId, const std::vector<pluginrack::MultibandEffect>& effects) {
    const juce::ScopedLock lock(stateLock);
    auto* output = findOutput(outputId);
    if (output == nullptr) return;
    syncNewEffects(output, effects, output->multibandEffects, output->multibandProcessors, currentSampleRate);
}
void AudioEngine::setChannelDeEssers(std::uint32_t channelId, const std::vector<pluginrack::DeEsserEffect>& effects) {
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) return;
    syncNewEffects(channel, effects, channel->deEsserEffects, channel->deEsserProcessors, currentSampleRate);
}
void AudioEngine::setOutputDeEssers(std::uint32_t outputId, const std::vector<pluginrack::DeEsserEffect>& effects) {
    const juce::ScopedLock lock(stateLock);
    auto* output = findOutput(outputId);
    if (output == nullptr) return;
    syncNewEffects(output, effects, output->deEsserEffects, output->deEsserProcessors, currentSampleRate);
}
void AudioEngine::setChannelLimiters(std::uint32_t channelId, const std::vector<pluginrack::LimiterEffect>& effects) {
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) return;
    syncNewEffects(channel, effects, channel->limiterEffects, channel->limiterProcessors, currentSampleRate);
}
void AudioEngine::setOutputLimiters(std::uint32_t outputId, const std::vector<pluginrack::LimiterEffect>& effects) {
    const juce::ScopedLock lock(stateLock);
    auto* output = findOutput(outputId);
    if (output == nullptr) return;
    syncNewEffects(output, effects, output->limiterEffects, output->limiterProcessors, currentSampleRate);
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

const std::vector<std::unique_ptr<AudioEngine::Output>>& AudioEngine::getOutputs() const {
    return outputs;
}

const std::vector<std::unique_ptr<AudioEngine::AuxBus>>& AudioEngine::getAuxBuses() const {
    return auxBuses;
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
    to.effects = from.effects;
    to.multibandEffects = from.multibandEffects;
    to.deEsserEffects = from.deEsserEffects;
    to.limiterEffects = from.limiterEffects;
    to.chain = from.chain;
}

// The host pointers stay live until capture finishes; the slot copy carries
// only what the scene file stores.
template <typename LiveInserts>
std::vector<const PluginHost*> copyInserts(const LiveInserts& inserts,
                                           std::vector<pluginrack::InsertSlot>& saved) {
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
        // the blob the scene supplied.
        slot.pluginState = insert->savedPluginState.toStdString();
        saved.push_back(std::move(slot));
        hosts.push_back(&insert->pluginHost);
    }
    return hosts;
}

} // namespace

pluginrack::Scene AudioEngine::makeScene() const {
    pluginrack::Scene scene;
    // Plugin state capture is plugin code of unbounded length, and render()
    // only waits a few yields for stateLock before it pushes silence. Record
    // the hosts under the lock and ask them for their state after it is
    // released. Safe because makeScene and every structural edit run on the
    // message thread, and removePlugin parks retired slots instead of freeing
    // them, so the recorded hosts stay alive until the capture finishes.
    std::vector<std::vector<const PluginHost*>> channelHosts;
    std::vector<std::vector<const PluginHost*>> outputHosts;
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
            channelHosts.push_back(copyInserts(channel->inserts, savedChannel.inserts));
            scene.channels.push_back(std::move(savedChannel));
        }
        for (const auto& output : outputs) {
            pluginrack::Output savedOutput;
            copyStripFields(*output, savedOutput);
            // The live engine models physical endpoints only; a saved output
            // therefore stays OutputKind::Physical.
            savedOutput.deviceId = output->outputDeviceName.toStdString();
            savedOutput.flipped = output->flipped.get();
            outputHosts.push_back(copyInserts(output->inserts, savedOutput.inserts));
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
            scene.routes.push_back(std::move(savedRoute));
        }
        for (const auto& bus : auxBuses) {
            pluginrack::AuxBus savedBus;
            savedBus.id = bus->id;
            savedBus.name = bus->name.toStdString();
            savedBus.gainDb = bus->gainDb.get();
            savedBus.sourceChannelIds = bus->sourceChannelIds;
            scene.auxBuses.push_back(std::move(savedBus));
        }

        // The tuning the session runs with travels with the save, so loading it
        // restores the same buffers, stream modes and device watchdog.
        scene.audio = getDeviceSettings();
    }

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
    return scene;
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
        channel->eqBypassed = !savedChannel.eqEnabled;
        channel->eq.prepare(sampleRate, bufferSize);
        channel->eq.setBands(channel->eqBands);
        channel->eq.setDelayMs(channel->eqDelayMs);
        channel->eq.setPhaseFlip(channel->eqPhaseFlip.get());
        channel->effects = savedChannel.effects;
        channel->multibandEffects = savedChannel.multibandEffects;
        channel->deEsserEffects = savedChannel.deEsserEffects;
        channel->limiterEffects = savedChannel.limiterEffects;
        for (const auto& e : channel->multibandEffects) { pluginrack::MultibandProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); channel->multibandProcessors.push_back(std::move(pr)); }
        for (const auto& e : channel->deEsserEffects) { pluginrack::DeEsserProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); channel->deEsserProcessors.push_back(std::move(pr)); }
        for (const auto& e : channel->limiterEffects) { pluginrack::LimiterProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); channel->limiterProcessors.push_back(std::move(pr)); }
        // An empty saved chain is a real empty state (no effects loaded).
        // Scenes without per-strip ordering predate this field; their effects
        // still run via processEffectsMissingFromChain, so a missing chain
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
        output->eqBypassed = !savedOutput.eqEnabled;
        output->eq.prepare(sampleRate, bufferSize);
        output->eq.setBands(output->eqBands);
        output->eq.setDelayMs(output->eqDelayMs);
        output->eq.setPhaseFlip(output->eqPhaseFlip.get());
        output->effects = savedOutput.effects;
        output->multibandEffects = savedOutput.multibandEffects;
        output->deEsserEffects = savedOutput.deEsserEffects;
        output->limiterEffects = savedOutput.limiterEffects;
        for (const auto& e : output->multibandEffects) { pluginrack::MultibandProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); output->multibandProcessors.push_back(std::move(pr)); }
        for (const auto& e : output->deEsserEffects) { pluginrack::DeEsserProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); output->deEsserProcessors.push_back(std::move(pr)); }
        for (const auto& e : output->limiterEffects) { pluginrack::LimiterProcessor pr; pr.prepare(currentSampleRate); pr.setEffect(e); output->limiterProcessors.push_back(std::move(pr)); }
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
        restoredRoutes.push_back(std::move(route));
    }
    auto& restoredAuxBuses = job.auxBuses;
    std::uint32_t maxAuxBusId = 0;
    for (const auto& savedBus : scene.auxBuses) {
        auto bus = std::make_unique<AuxBus>();
        bus->id = savedBus.id;
        bus->name = savedBus.name;
        bus->gainDb = savedBus.gainDb;
        bus->sourceChannelIds = savedBus.sourceChannelIds;
        bus->buffer.setSize(2, bufferSize);
        bus->buffer.clear();
        maxAuxBusId = juce::jmax(maxAuxBusId, bus->id + 1);
        restoredAuxBuses.push_back(std::move(bus));
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
    load.insert->latencySamples = load.insert->pluginHost.getLatencySamples();
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
    std::vector<std::unique_ptr<AuxBus>> oldAuxBuses;
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
        for (auto& channel : job.channels) {
            channel->buffer.setSize(2, restoreFrames, false, false, false);
            channel->eq.prepare(restoreRate, restoreFrames);
        }
        for (auto& output : job.outputs) {
            output->buffer.setSize(2, restoreFrames, false, false, false);
            output->eq.prepare(restoreRate, restoreFrames);
        }
        oldChannels = std::move(channels);
        oldOutputs = std::move(outputs);
        oldRoutes = std::move(routes);
        oldAuxBuses = std::move(auxBuses);
        channels = std::move(job.channels);
        outputs = std::move(job.outputs);
        auxBuses = std::move(job.auxBuses);
        routes = std::move(job.routes);
        meters.setTargets(job.channelIds.data(), job.channelIds.size(),
                          job.outputIds.data(), job.outputIds.size());
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
        for (const auto& bus : auxBuses) {
            nextAuxBusId = juce::jmax(nextAuxBusId, bus->id + 1);
        }
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
    oldAuxBuses.clear();
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

juce::String AudioEngine::setChannelInputDevice(std::uint32_t channelId, const juce::String& deviceName) {
    if (isOpeningDevices()) {
        return "Audio devices are still starting up. Try again in a moment.";
    }
    const auto error = setChannelInputDeviceImpl(channelId, deviceName);
    // The previous figure described the previous capture chain.
    if (error.isEmpty()) resetLatencyMeasurement();
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
            && it->second.requestedBufferSize == requestedInputBufferSize_
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
        pluginrack::inputBufferRequest(requestedInputBufferSize_, renderBlockFrames);
    // Cache-first hint: with the size on Auto, reuse the buffer this endpoint
    // negotiated last time. A refused size still falls through to the live
    // probe, so a stale entry can never wedge a device closed.
    if (requestedInputBufferSize_ == 0) {
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
    if (openErr.isNotEmpty() && requestedInputBufferSize_ == 0 && bufferRequest > 0) {
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
                                                        requestedInputBufferSize_,
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
    // The previous figure described the previous output chain.
    if (error.isEmpty()) resetLatencyMeasurement();
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
    if (retargeted > 0) pruneUnusedCaptures();
    return retargeted;
}

juce::String AudioEngine::setOutputDeviceImpl(std::uint32_t outputId, const juce::String& deviceName,
                                              double engineRate) {
    AudioEngine::Output* out = findOutput(outputId);
    if (out == nullptr) {
        return "The output no longer exists.";
    }

    bool isClock = false;
    bool sharesClockInput = false;
    int renderBlockFrames = 0;
    {
        const juce::ScopedLock sl(stateLock);
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
                        o2->output->setRenderHook([this](int n) { this->render(n); });
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
    if (isClock) {
        newPlayback->setRenderHook([this](int n) { this->render(n); });
        // open() starts this device's callbacks; stay inactive until the
        // hand-off so no second render hook can run alongside prevOutput.
        newPlayback->setRenderActive(false);
    }

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
        if (out->output != nullptr) out->output->setRenderActive(true);
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
            currentBufferSize = newBuf;
            clockOutputId = outputId;
            clockFailoverActive_ = false;
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
    const int previous = requestedInputBufferSize_;
    requestedInputBufferSize_ = size;
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
        requestedInputBufferSize_ = previous;
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
    return firstError;
}

void AudioEngine::handleClockLoss() {
    std::vector<pluginrack::ClockCandidate> candidates;
    std::uint32_t lost = 0;
    int lastRate = 48000;
    pluginrack::ClockFailoverDecision decision;
    juce::String promotedName;
    {
        const juce::ScopedLock lock(stateLock);
        lastRate = static_cast<int>(std::lround(
            currentSampleRate > 0.0 ? currentSampleRate : 48000.0));
        lost = clockOutputId;
        for (const auto& output : outputs) {
            if (output->id == lost) continue;
            pluginrack::ClockCandidate candidate;
            candidate.outputId = output->id;
            candidate.clockCapable = output->output != nullptr;
            candidate.deviceOpen = output->output != nullptr;
            candidate.sampleRate =
                output->output != nullptr
                    ? static_cast<int>(std::lround(output->output->getSampleRate()))
                    : 0;
            candidates.push_back(candidate);
        }
        decision = pluginrack::chooseClockFailover(candidates, lost, lastRate);
        if (decision.action == pluginrack::ClockAction::Promote) {
            for (auto& output : outputs) {
                if (output->id != decision.outputId || output->output == nullptr) continue;
                for (auto& old : outputs) {
                    if (old->id == lost && old->output != nullptr) {
                        old->output->setRenderActive(false);
                    }
                }
                // The promoted stream already owns an open device; it only
                // needs to become the render driver. FIFOs are re-primed by
                // the next block, reusing the epoch path so no block repeats.
                output->output->setRenderHook([this](int n) { this->render(n); });
                output->output->setRenderActive(true);
                clockOutputId = output->id;
                promotedName = output->outputDeviceName;
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
        if (promotedName.isNotEmpty())
            juce::Logger::writeToLog("Clock promoted to " + promotedName);
    } else {
        startInternalClock(lastRate);
        juce::Logger::writeToLog(
            "Clock output was lost; rendering on the internal clock at "
            + juce::String(lastRate) + " Hz");
    }
}

void AudioEngine::startInternalClock(int sampleRate) {
    if (sampleRate <= 0) sampleRate = 48000;
    if (!internalClock_) {
        internalClock_ = std::make_unique<InternalClock>(*this);
    }
    const int block = maxBlockFrames_ > 0 ? maxBlockFrames_ : juce::jmax(1, currentBufferSize);
    internalClock_->configure(block, static_cast<double>(sampleRate));
}

void AudioEngine::stopInternalClock() {
    if (internalClock_ != nullptr) internalClock_->stopTimer();
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

juce::String AudioEngine::getDeviceCacheJson() const {
    const juce::ScopedLock lock(deviceCacheLock_);
    return deviceCache_.toJson();
}

juce::String AudioEngine::getClockDescription() const {
    // Called from the UI timer while the device opener may be rewriting these
    // strings, so every read here happens under the lock.
    const juce::ScopedLock lock(stateLock);
    if (clockOutputId == 0) {
        return "No output device - click a master strip";
    }
    for (const auto& o : outputs) {
        if (o->id == clockOutputId) {
            const auto device = isSilentDeviceName(o->outputDeviceName) ? systemDefaultDeviceName()
                                                              : o->outputDeviceName;
            const auto khzValue = currentSampleRate / 1000.0;
            const auto khz = std::abs(khzValue - std::round(khzValue)) < 0.001
                                 ? juce::String((int)std::round(khzValue))
                                 : juce::String(khzValue, 1);
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

    int pluginSamples = 0;
    int maxInsertSamples = 0;
    std::uint32_t maxInsertId = 0;
    juce::String maxInsertName;
    if (inputChannel != nullptr && inputChannel->input != nullptr) {
        for (const auto& insert : inputChannel->inserts) {
            pluginSamples += insert->latencySamples;
            if (insert->latencySamples > maxInsertSamples) {
                maxInsertSamples = insert->latencySamples;
                maxInsertId = insert->id;
                maxInsertName = insert->pluginName;
            }
        }
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
                    pluginSamples += insert->latencySamples;
                    if (insert->latencySamples > maxInsertSamples) {
                        maxInsertSamples = insert->latencySamples;
                        maxInsertId = insert->id;
                        maxInsertName = insert->pluginName;
                    }
                }
                valid = true;
                break;
            }
        }
    }

    const double sampleRate = currentSampleRate > 0.0 ? currentSampleRate : 48000.0;
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
    report.requestedInputBufferSize = requestedInputBufferSize_;
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
    report.approximate = true;
    report.measuredValid = latencyMeasuredValid_.load(std::memory_order_relaxed);
    report.measuredMs = report.measuredValid ? getMeasuredLatencyMs() : 0.0;
    report.measureFailed = latencyMeasureFailed_.load(std::memory_order_relaxed);
    report.measuredAttempted = report.measuredValid || report.measureFailed
        || latencyMeasureActive_.load(std::memory_order_relaxed);
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
            diagnostics.inputOccupancy = juce::jmax(diagnostics.inputOccupancy,
                                                    capture->getOccupancy());
        }
    }

    for (const auto& output : outputs) {
        if (output->output != nullptr) {
            diagnostics.outputUnderruns += output->output->getOutputUnderruns();
            diagnostics.outputDroppedFrames += output->output->getOutputDroppedFrames();
            diagnostics.outputLowWatermarks += output->output->getOutputLowWatermarks();
            diagnostics.outputOccupancy = juce::jmax(diagnostics.outputOccupancy,
                                                     output->output->getOccupancy());
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
        if (ins->latencySamples > maxLat) {
            maxLat = ins->latencySamples;
            result.insertId = ins->id;
            result.name = ins->pluginName;
            result.latencySamples = ins->latencySamples;
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

AudioEngine::DeviceSettings AudioEngine::getDeviceSettings() const {
    DeviceSettings settings;
    settings.inputBuffer = requestedInputBufferSize_;
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
    settings.ladderRecovery = ladderRecovery_.load();
    settings.healthYellowUnderrunsPerMinute =
        healthYellowUnderrunsPerMinute_.load();
    settings.healthRedUnderrunsPerMinute =
        healthRedUnderrunsPerMinute_.load();
    settings.healthPinnedToEdgeMs = healthPinnedToEdgeMs_.load();
    settings.rackThumbnailFps = rackThumbnailFps_.load();
    settings.rackPreviewMode = rackPreviewMode_;
    settings.watchedDevices = getWatchedDevices();
    return settings;
}

void AudioEngine::setAutoReloadDevices(bool enabled) {
    autoReloadDevices_.store(enabled);
}

void AudioEngine::setRackThumbnailFps(int fps) {
    rackThumbnailFps_.store(juce::jlimit(1, 60, fps));
}

void AudioEngine::setRackPreviewMode(pluginrack::RackPreviewMode mode) {
    rackPreviewMode_ = mode;
}

void AudioEngine::setWatchedDevices(const std::vector<pluginrack::WatchedDevice>& devices) {
    const juce::ScopedLock lock(watchedDevicesLock_);
    watchedDevices_ = devices;
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
    requestedInputBufferSize_ = settings.inputBuffer;
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
    ladderRecovery_.store(settings.ladderRecovery);
    healthYellowUnderrunsPerMinute_.store(settings.healthYellowUnderrunsPerMinute);
    healthRedUnderrunsPerMinute_.store(settings.healthRedUnderrunsPerMinute);
    healthPinnedToEdgeMs_.store(settings.healthPinnedToEdgeMs);
    applyHealthThresholdsToStreams();
    if (settings.rackThumbnailFps > 0) rackThumbnailFps_.store(settings.rackThumbnailFps);
    rackPreviewMode_ = settings.rackPreviewMode;
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

bool AudioEngine::getLatencyOption(LatencyOption option) const {
    switch (option) {
        case LatencyOption::LowLatencyInput:   return lowLatencyInput_.load();
        case LatencyOption::PreferExclusive:   return preferExclusive_.load();
        case LatencyOption::LowLatencyOutputs: return lowLatencyOutputs_.load();
        case LatencyOption::MinimalClockFifo:  return minimalClockFifo_.load();
        case LatencyOption::FullDuplexInput:   return fullDuplexInput_.load();
    }
    return false;
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
    // The stream that produced the last measurement is being replaced.
    resetLatencyMeasurement();
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
            beginDeviceOpen(status);
            // Single attempt: a device that refuses stays silent and is
            // reported, instead of being retried with a 400 ms pause.
            const auto error = device.isInput
                                   ? setChannelInputDeviceImpl(device.stripId, device.deviceName,
                                                               /*forceReopen=*/true)
                                   : setOutputDeviceImpl(device.stripId, device.deviceName,
                                                         currentSampleRate);
            endDeviceOpen(status);
            if (error.isNotEmpty()) failures.add(device.label + ": " + error);
            deviceOpenProgress_.fetch_add(1);
        }

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
            deviceOpenProgress_.fetch_add(1);
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
        launchInputs(openRate, forceInputs, inputResults, inputHelpers);
    }
    for (auto& helper : inputHelpers) helper.join();

    // Every stream was attempted exactly once. Report each refusal against the
    // strip it belongs to; the failing device is already closed and silent.
    for (std::size_t i = 0; i < outputResults.size(); ++i) {
        if (outputResults[i].isEmpty()) continue;
        const auto label = "Output \"" + outputsWanted[i].label + "\" ("
                         + outputsWanted[i].name + ")";
        if (i == 0)
            errors += label + " could not be opened as the clock, so nothing can play: "
                    + outputResults[i] + "\n";
        else
            errors += label + ": " + outputResults[i] + "\n";
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
