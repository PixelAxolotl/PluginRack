#include "DeviceStream.h"

#include "RealtimeThread.h"

#include <pluginrack/DeviceTypePolicy.h>
#include <pluginrack/OutputBufferPolicy.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

namespace {
juce::String formatKhz(double rate) {
    double k = rate / 1000.0;
    if (std::abs(k - std::round(k)) < 0.001) {
        return juce::String((int)std::round(k)) + " kHz";
    }
    return juce::String(k, 1) + " kHz";
}

// Device type names registered by JUCE for the WASAPI modes. Normal shared
// WASAPI is deliberately tried first: the low-latency and exclusive modes are
// useful fallbacks, but forcing either one on every endpoint made the beta
// tuning path driver-dependent and prone to dropouts.
// ASIO (only when this build has JUCE_ASIO) is tried last. "Windows Audio
// (Exclusive Mode)" is JUCE's exact registered name.
const juce::String kTypeLowLatency = "Windows Audio (Low Latency Mode)";
const juce::String kTypeExclusive = "Windows Audio (Exclusive Mode)";
const juce::String kTypeShared = "Windows Audio";
const juce::String kTypeAsio = "ASIO";

// Whether this build can reach an ASIO driver at all. Asking the policy to
// order a type we cannot create would just spend a failed probe.
bool asioAvailable() {
#if JUCE_ASIO
    return true;
#else
    return false;
#endif
}

// Endpoints remember the buffer size they settled on, so a later launch asks for
// it directly. Without this "Auto" let the driver hand back 960 on one launch
// and 480 on the next for the same device, which moved every latency derived
// from the clock period.
juce::CriticalSection bufferCacheLock;
std::map<juce::String, int> bufferCache;

int cachedBufferFor(const juce::String& deviceName) {
    const juce::ScopedLock sl(bufferCacheLock);
    const auto it = bufferCache.find(deviceName);
    return it != bufferCache.end() ? it->second : 0;
}

void rememberBufferFor(const juce::String& deviceName, int bufferSize) {
    if (deviceName.isEmpty() || bufferSize <= 0) return;
    const juce::ScopedLock sl(bufferCacheLock);
    bufferCache[deviceName] = bufferSize;
}

std::unique_ptr<juce::AudioIODeviceType> createTypeInstance(const juce::String& typeName) {
    if (typeName == kTypeLowLatency)
        return std::unique_ptr<juce::AudioIODeviceType>(
            juce::AudioIODeviceType::createAudioIODeviceType_WASAPI(juce::WASAPIDeviceMode::sharedLowLatency));
    if (typeName == kTypeExclusive)
        return std::unique_ptr<juce::AudioIODeviceType>(
            juce::AudioIODeviceType::createAudioIODeviceType_WASAPI(juce::WASAPIDeviceMode::exclusive));
    if (typeName == kTypeShared)
        return std::unique_ptr<juce::AudioIODeviceType>(
            juce::AudioIODeviceType::createAudioIODeviceType_WASAPI(juce::WASAPIDeviceMode::shared));
    if (typeName == kTypeAsio)
        return std::unique_ptr<juce::AudioIODeviceType>(
            juce::AudioIODeviceType::createAudioIODeviceType_ASIO()); // null unless JUCE_ASIO
    return {};
}

// Returns the type of this name registered with the manager, creating and
// adding it via AudioDeviceManager::addAudioDeviceType when it is missing.
// Null when the platform cannot provide it (e.g. WASAPI off Windows).
juce::AudioIODeviceType* ensureTypeRegistered(juce::AudioDeviceManager& manager,
                                              const juce::String& typeName) {
    for (auto* type : manager.getAvailableDeviceTypes())
        if (type->getTypeName() == typeName)
            return type;
    auto fresh = createTypeInstance(typeName);
    if (fresh == nullptr)
        return nullptr;
    auto* raw = fresh.get();
    manager.addAudioDeviceType(std::move(fresh));
    raw->scanForDevices(); // the manager only scanned the types it had before
    return raw;
}

// Opens preferredDeviceName (empty = system default) once, in the first device
// type that actually carries it, passing the full AudioDeviceSetup with the
// name explicit instead of relying on a wildcard match. The type is selected
// through AudioDeviceManager's setup XML so JUCE opens the requested endpoint
// directly; setCurrentAudioDeviceType would first open the system default,
// sleep 1.5 seconds, close it, and only then open the requested endpoint.
// There is deliberately no fallback chain and no retry: one attempt, then the
// caller reports the error and leaves the strip silent. On success returns the
// selected type name; on failure returns empty and sets errorOut.
juce::String openWithPreferredType(juce::AudioDeviceManager& manager,
                                   const juce::String& preferredDeviceName,
                                   bool wantInput,
                                   int numInputChannels,
                                   int numOutputChannels,
                                   double engineSampleRate,
                                   int requestedBufferSize,
                                   juce::String& errorOut,
                                   bool preferExclusive,
                                   pluginrack::StreamRole role,
                                   int duplexInputChannels) {
    if (duplexInputChannels > 0) {
        wantInput = true;
        numInputChannels = juce::jmax(1, duplexInputChannels);
    }
    // Only name lookups happen here (no driver probe). The first type that
    // carries the endpoint is the single attempt we make: every extra type is
    // another full WASAPI probe, and a startup that keeps failing for a minute
    // is worse than a strip that goes silent with a clear error.
    // The order itself comes from the shared policy so the clock can reach ASIO
    // first while captures and extra outputs stay on shareable types.
    juce::StringArray typeOrder;
    for (const auto name : pluginrack::deviceTypeOrder(role, preferExclusive, asioAvailable()))
        typeOrder.add(juce::String(name.data(), static_cast<int>(name.size())));

    juce::AudioIODeviceType* chosenType = nullptr;
    juce::String chosenTypeName;
    juce::StringArray chosenDeviceNames;
    for (const auto& typeName : typeOrder) {
        auto* type = ensureTypeRegistered(manager, typeName);
        if (type == nullptr)
            continue; // platform without this type (or ASIO built without JUCE_ASIO)
        auto deviceNames = type->getDeviceNames(wantInput);
        const bool hasDevice = preferredDeviceName.isEmpty()
                                   ? !deviceNames.isEmpty()
                                   : deviceNames.contains(preferredDeviceName);
        if (!hasDevice) continue;
        chosenType = type;
        chosenTypeName = typeName;
        chosenDeviceNames = std::move(deviceNames);
        break;
    }

    if (chosenType == nullptr) {
        errorOut = preferredDeviceName.isEmpty()
                       ? juce::String("No audio device is available.")
                       : "No such device: " + preferredDeviceName;
        return {};
    }

    // Resolve the default to its actual endpoint name before building the XML.
    // An empty name in AudioDeviceSetup means "let the manager pick a default",
    // but the XML path treats two empty names as "no device".
    juce::String selectedDeviceName = preferredDeviceName;
    if (selectedDeviceName.isEmpty()) {
        const int defaultIndex = chosenType->getDefaultDeviceIndex(wantInput);
        if (defaultIndex < 0 || defaultIndex >= chosenDeviceNames.size()) {
            errorOut = "No audio device is available.";
            return {};
        }
        selectedDeviceName = chosenDeviceNames[defaultIndex];
    }

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    if (wantInput)
        setup.inputDeviceName = selectedDeviceName;
    else
        setup.outputDeviceName = selectedDeviceName;
    if (requestedBufferSize >= 0) {
        setup.sampleRate = engineSampleRate; // 0 = driver default
        // Auto asks for the size this endpoint accepted last time; 0 on a first
        // run lets the driver open, and the block below settles on its minimum.
        setup.bufferSize = requestedBufferSize > 0 ? requestedBufferSize
                                                   : cachedBufferFor(selectedDeviceName);
    }
    juce::XmlElement typeSetup("DEVICESETUP");
    typeSetup.setAttribute("deviceType", chosenTypeName);
    if (wantInput)
        typeSetup.setAttribute("audioInputDeviceName", selectedDeviceName);
    else
        typeSetup.setAttribute("audioOutputDeviceName", selectedDeviceName);
    auto err = manager.initialise(numInputChannels, numOutputChannels, &typeSetup, false,
                                  selectedDeviceName, &setup);
    auto* device = err.isEmpty() ? manager.getCurrentAudioDevice() : nullptr;
    if (err.isEmpty() && device == nullptr)
        err = "Could not open " + selectedDeviceName + ".";

    // Auto means "the lowest this endpoint supports", not "whatever the driver
    // felt like today". Settle on it now and remember it for the next launch.
    if (err.isEmpty() && requestedBufferSize <= 0) {
        std::vector<int> available;
        for (const int size : device->getAvailableBufferSizes()) available.push_back(size);
        const int current = device->getCurrentBufferSizeSamples();
        const int wanted = pluginrack::smallestSupportedBufferSize(available, current, 0);
        if (wanted > 0 && wanted != current) {
            auto liveSetup = manager.getAudioDeviceSetup();
            liveSetup.bufferSize = wanted;
            // A device may advertise a size it then refuses; keeping the opened
            // stream is better than failing the whole strip over a few ms.
            if (manager.setAudioDeviceSetup(liveSetup, true).isEmpty()) {
                device = manager.getCurrentAudioDevice();
            }
        }
        if (device != nullptr) {
            rememberBufferFor(selectedDeviceName, device->getCurrentBufferSizeSamples());
        }
    }
    if (err.isEmpty() && requestedBufferSize > 0
        && device->getCurrentBufferSizeSamples() != requestedBufferSize) {
        err = "The device does not support a " + juce::String(requestedBufferSize)
            + "-sample buffer.";
    }
    if (err.isNotEmpty()) {
        manager.closeAudioDevice();
        if (chosenTypeName == kTypeAsio) {
            // ASIO drivers are single-client. Once the clock holds the
            // interface, any second PluginRack stream (or another application)
            // asking for it fails, and the raw driver error never says why.
            err += "\nASIO interfaces accept one client: close the other "
                   "stream or application using this interface, or give this "
                   "stream its Windows Audio device instead.";
        }
        errorOut = err;
        return {};
    }
    return chosenTypeName;
}
}

namespace DeviceEnumeration {
namespace {
struct DeviceCache {
    juce::StringArray names;
    juce::int64 timeMs = 0;
    bool valid = false;
};

DeviceCache& cacheFor(bool wantInputs) {
    static DeviceCache inCache;
    static DeviceCache outCache;
    return wantInputs ? inCache : outCache;
}

juce::CriticalSection cacheLock;

juce::StringArray enumerate(bool wantInputs) {
    const auto now = juce::Time::currentTimeMillis();
    {
        const juce::ScopedLock sl(cacheLock);
        auto& c = cacheFor(wantInputs);
        if (c.valid && (now - c.timeMs) < 5000) {
            return c.names;
        }
    }

    juce::AudioDeviceManager manager;
    juce::OwnedArray<juce::AudioIODeviceType> types;
    manager.createAudioDeviceTypes(types);

    juce::StringArray names;
    for (auto* type : types) {
        type->scanForDevices();
        for (const auto& name : type->getDeviceNames(wantInputs)) {
            if (!names.contains(name)) names.add(name);
        }
    }
    names.sort(true);

    {
        const juce::ScopedLock sl(cacheLock);
        auto& c = cacheFor(wantInputs);
        c.names = names;
        c.timeMs = now;
        c.valid = true;
    }
    return names;
}
}

juce::StringArray inputs() { return enumerate(true); }
juce::StringArray outputs() { return enumerate(false); }

juce::StringArray asioDevices() {
#if JUCE_ASIO
    // Scanning ASIO probes the driver, so cache it briefly: "Force ASIO" asks
    // on every UI timer tick.
    static juce::CriticalSection lock;
    static juce::StringArray cached;
    static juce::int64 cachedAt = 0;
    const auto now = juce::Time::currentTimeMillis();
    {
        const juce::ScopedLock sl(lock);
        if (cachedAt != 0 && (now - cachedAt) < 5000) return cached;
    }
    juce::StringArray names;
    if (auto type = std::unique_ptr<juce::AudioIODeviceType>(
            juce::AudioIODeviceType::createAudioIODeviceType_ASIO())) {
        type->scanForDevices();
        names = type->getDeviceNames(false);
        if (names.isEmpty()) names = type->getDeviceNames(true);
        names.sort(true);
    }
    const juce::ScopedLock sl(lock);
    cached = names;
    cachedAt = now;
    return names;
#else
    return {};
#endif
}
}

// ---------- InputCapture ----------

InputCapture::InputCapture() = default;

InputCapture::~InputCapture() {
    close();
}

juce::String InputCapture::open(const juce::String& preferredDeviceName,
                               double engineSampleRate,
                               int numChannelsNeeded,
                               juce::String& errorOut,
                               int requestedBufferSize,
                               int renderBlockFrames,
                               int readDelayBlocks,
                               bool preferExclusive,
                               int capacityReserveFrames) {
    close();
    error.clear();
    inputError.clear();

    const int numInputChannels = juce::jmax(2, numChannelsNeeded);
    // One attempt in the preferred device type (shared, or exclusive when it
    // was explicitly enabled). A refusal is reported, never retried.
    activeDeviceType_ = openWithPreferredType(manager, preferredDeviceName, /*wantInput*/ true,
                                              numInputChannels, 0,
                                              engineSampleRate, requestedBufferSize, errorOut,
                                              preferExclusive, pluginrack::StreamRole::Input,
                                              0);
    if (activeDeviceType_.isEmpty()) {
        error = errorOut;
        return errorOut;
    }

    auto* dev = manager.getCurrentAudioDevice();
    if (dev == nullptr) {
        const auto msg = juce::String("Could not open input device.");
        error = msg;
        errorOut = msg;
        close();
        return msg;
    }

    const double actual = dev->getCurrentSampleRate();
    if (engineSampleRate > 0.0 && std::abs(actual - engineSampleRate) > 0.5) {
        const auto msg = "Needs " + formatKhz(engineSampleRate) + " (device is " + formatKhz(actual) + ")";
        error = msg;
        errorOut = msg;
        close();
        return msg;
    }

    sampleRate = actual;
    deviceInputLatency = dev->getInputLatencyInSamples();
    deviceBufferSize = dev->getCurrentBufferSizeSamples();
    if (deviceBufferSize <= 0) deviceBufferSize = 256;

    // Keep every physical input the device actually provides. Requested
    // indexes it does not have are reported via getInputError() and stay
    // silent — never remapped onto input 0.
    const int availableInputs = (int) dev->getInputChannelNames().size();
    availableInputCount_.store(availableInputs < 1 ? 1 : availableInputs, std::memory_order_relaxed);
    inputChannelCount_ = (numInputChannels < availableInputs) ? numInputChannels : availableInputs;
    if (inputChannelCount_ < 1) inputChannelCount_ = 1;
    if (inputChannelCount_ < numChannelsNeeded) {
        juce::StringArray missing;
        for (int i = inputChannelCount_ + 1; i <= numChannelsNeeded; ++i) missing.add(juce::String(i));
        inputError = "Missing input" + (numChannelsNeeded - inputChannelCount_ > 1 ? juce::String("s ") : juce::String(" "))
                     + missing.joinIntoString(", ");
        inputUnavailable_.store(true, std::memory_order_relaxed);
    }

    // The render consumes this capture in output-clock blocks, which may be
    // larger than the input device's own buffer (independent input/output
    // buffer settings). Size the history for the larger of the two so a
    // 32-sample input feeding a 4096-sample render block still has a full
    // window to read.
    const int blockFrames = juce::jmax(
        deviceBufferSize, renderBlockFrames > 0 ? renderBlockFrames : deviceBufferSize);
    // The reserve only sizes the ring. Keeping it out of blockFrames matters:
    // the read delay below is derived from blockFrames, so a reserve sized for
    // a probe that has not finished yet would become ~85 ms of real latency.
    const int capacityUnit = juce::jmax(blockFrames, capacityReserveFrames);
    capacity = juce::jmax(capacityUnit * 4,
                          juce::jmin(capacityUnit * 8, 65536));
    storage.setSize(inputChannelCount_, capacity);
    storage.clear();
    writePos = 0;
    totalWritten.store(0, std::memory_order_relaxed);

    // Read delay absorbs callback phase jitter and gives the drift correction
    // room to work without an audible block repeat. Low-latency mode uses one
    // block instead of two.
    const int maxDelay = juce::jmax(0, capacity - blockFrames - 2);
    // The plan owns the arithmetic: it settles on one block plus a small
    // headroom (the minimum CaptureReadCursor can serve) and is re-settled when
    // the clock's real render block arrives.
    pluginrack::InputDelayPlan::Config planConfig;
    planConfig.readDelayBlocks = juce::jmax(1, readDelayBlocks);
    delayPlan_.open(deviceBufferSize, renderBlockFrames, planConfig);
    const int delay = juce::jlimit(0, maxDelay, delayPlan_.delay());
    readDelaySamples.store(delay, std::memory_order_relaxed);
    initialReadDelay_.store(delay, std::memory_order_relaxed);
    readCursor.prepare(capacity, delay, 0.005);
    pluginrack::DelayTargetController::Config targetConfig;
    targetConfig.blockFrames = juce::jmax(1, delayPlan_.blockFrames());
    targetConfig.minDelaySamples = delayPlan_.floor();
    // Hard ceiling: eight blocks is already a sick device.
    targetConfig.maxDelaySamples = juce::jmin(maxDelay, delayPlan_.blockFrames() * 8);
    delayTarget_.configure(targetConfig);
    delayTarget_.reset(delay);
    openedAtMs_.store(juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
    lastPlayed_[0] = 0.0f;
    lastPlayed_[1] = 0.0f;
    lastUnderrunEpoch_ = 0;
    fadeInEpoch_ = 0;
    latencySamples.store(deviceInputLatency + delay, std::memory_order_relaxed);
    inputOverruns.store(0, std::memory_order_relaxed);
    inputUnderruns.store(0, std::memory_order_relaxed);
    inputDroppedFrames.store(0, std::memory_order_relaxed);

    manager.addAudioCallback(this);
    return {};
}

void InputCapture::close() {
    manager.removeAudioCallback(this);
    manager.closeAudioDevice();
    storage.setSize(0, 0, false, true, false);
    sampleRate = 0.0;
    activeDeviceType_.clear();
    deviceInputLatency = 0;
    deviceBufferSize = 0;
    inputChannelCount_ = 0;
    availableInputCount_.store(0, std::memory_order_relaxed);
    inputUnavailable_.store(false, std::memory_order_relaxed);
    writePos = 0;
    totalWritten.store(0, std::memory_order_relaxed);
    capacity = 0;
    readCursor.reset();
    lastPlayed_[0] = 0.0f;
    lastPlayed_[1] = 0.0f;
    lastUnderrunEpoch_ = 0;
    fadeInEpoch_ = 0;
    readDelaySamples.store(0, std::memory_order_relaxed);
    initialReadDelay_.store(0, std::memory_order_relaxed);
    latencySamples.store(0, std::memory_order_relaxed);
}

void InputCapture::applyRenderBlockFrames(int blockFrames) {
    if (capacity <= 0 || blockFrames <= 0) return;
    // A starvation already told us this device needs more than the minimum.
    // Re-settling would give that safety back and glitch again.
    if (inputUnderruns.load(std::memory_order_relaxed) != 0) return;

    const int maxDelay = juce::jmax(0, capacity - blockFrames - 2);
    const int delay = juce::jlimit(0, maxDelay, delayPlan_.applyRenderBlock(blockFrames));
    if (delay == readDelaySamples.load(std::memory_order_relaxed)) return;

    readDelaySamples.store(delay, std::memory_order_relaxed);
    readCursor.setTargetDelay(delay);

    pluginrack::DelayTargetController::Config config = delayTarget_.config();
    config.blockFrames = juce::jmax(1, blockFrames);
    config.minDelaySamples = delayPlan_.floor();
    config.maxDelaySamples = juce::jmin(maxDelay, blockFrames * 8);
    delayTarget_.configure(config);
    delayTarget_.reset(delay);
}

bool InputCapture::pull(juce::AudioBuffer<float>& dest, int frames,
                        const std::vector<int>& physicalChannels,
                        std::uint64_t epoch) {
    if (frames <= 0) return true;

    // dest is sized by the caller: mono mappings need 1 channel, stereo 2.
    const int numDest = juce::jmin(2, (int)physicalChannels.size());
    if (numDest < 1 || dest.getNumChannels() < numDest || dest.getNumSamples() < frames) {
        return false;
    }
    dest.clear(0, frames);

    if (capacity <= 0) {
        latencySamples.store(0, std::memory_order_relaxed);
        return false;
    }

    // The writer never blocks: it fills the ring and then publishes the new
    // high-water mark with release semantics. The cursor reads from a fixed
    // delay behind that mark, so input-device callback jitter cannot repeat
    // or skip a whole block (the old "read the newest frames" behaviour).
    const auto written = totalWritten.load(std::memory_order_acquire);
    const auto window = readCursor.nextBlock(written, frames, epoch);
    // Startup and a stopped/restarting device legitimately have less than one
    // block of history; only a starved reader *after* the target delay was
    // reached counts as a real underrun.
    const auto targetDelay =
        static_cast<std::uint64_t>(readDelaySamples.load(std::memory_order_relaxed));
    // A one-block delay necessarily has less than a look-ahead sample on the
    // first pull. Do not report that expected startup condition as a glitch.
    const bool primedEnough = written >= targetDelay + static_cast<std::uint64_t>(frames);
    if (window.underrun && window.fresh && primedEnough) {
        inputUnderruns.fetch_add(1, std::memory_order_relaxed);
    }
    if (window.overrun && window.fresh) inputOverruns.fetch_add(1, std::memory_order_relaxed);

    if (window.fresh) {
        const int currentDelay = readDelaySamples.load(std::memory_order_relaxed);
        // The cursor clamps `available` to one block whenever it can serve a
        // full window, so it cannot report how much history is left: deriving a
        // fill error from it is always negative and would block every shrink.
        // Calm is therefore "no starvation and no overrun", which is exactly
        // what the controller needs; the cursor already steers clock drift
        // through its read ratio.
        const double blockMs = sampleRate > 0.0
                                   ? static_cast<double>(frames) / sampleRate * 1000.0
                                   : 0.0;
        // The first half second is the device priming: a short window there is
        // expected and says nothing about how much margin the stream needs.
        const auto openedAt = openedAtMs_.load(std::memory_order_relaxed);
        const bool settledIn = openedAt == 0
                               || juce::Time::getMillisecondCounter() - openedAt > 500;
        const bool starved = window.underrun && primedEnough && settledIn;
        const int nextDelay = delayTarget_.onBlock(currentDelay, starved, window.overrun,
                                                   0, blockMs);
        if (nextDelay != currentDelay) {
            readDelaySamples.store(nextDelay, std::memory_order_relaxed);
            readCursor.setTargetDelay(nextDelay);
        }
        if (starved) {
            // The cursor drops to the writer position on a starvation, so the
            // delay we just asked for does not exist yet. Re-prime it or every
            // following block is short as well and the delay walks upward.
            readCursor.requestReprime();
        }
    }

    // Declick bookkeeping: mark the epoch that ended in a fade-out, then
    // designate the first full epoch after it as the ramp-in epoch. Both the
    // fresh and replayed pulls of that same epoch see the same value.
    if (window.fresh) {
        if (window.underrun) {
            lastUnderrunEpoch_ = epoch;
        } else if (lastUnderrunEpoch_ != 0 && epoch > lastUnderrunEpoch_) {
            fadeInEpoch_ = epoch;
            lastUnderrunEpoch_ = 0;
        }
    }
    const bool applyFadeIn = !window.underrun && epoch == fadeInEpoch_;

    latencySamples.store(deviceInputLatency +
                             readDelaySamples.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);

    const int n = juce::jlimit(0, frames, juce::jmin(window.available, frames));
    const bool exact = window.ratio == 1.0 && window.fraction == 0.0;
    const int startIdx = capacity > 0 ? (int)(window.start % capacity) : 0;
    bool ok = true;
    bool missing = false;

    for (int i = 0; i < numDest; ++i) {
        const int src = physicalChannels[(size_t)i];
        if (src < 0 || src >= inputChannelCount_) {
            // Missing physical inputs leave their dest channel silent
            // (already cleared). Only flip the flag here - error strings are
            // built in open(), never on the audio thread.
            missing = true;
            ok = false;
            continue;
        }
        const float* ring = storage.getReadPointer(src);
        float* out = dest.getWritePointer(i);
        if (exact) {
            const int first = juce::jmin(n, capacity - startIdx);
            if (first > 0) {
                std::memcpy(out, ring + startIdx, sizeof(float) * (std::size_t)first);
            }
            if (n > first) {
                std::memcpy(out + first, ring, sizeof(float) * (std::size_t)(n - first));
            }
        } else {
            // Fractional read for clock drift: the cursor moves a fraction of
            // a sample per block instead of jumping a whole block.
            double pos = static_cast<double>(window.start) + window.fraction;
            for (int s = 0; s < n; ++s) {
                const auto base = (std::int64_t)std::floor(pos);
                const float frac = static_cast<float>(pos - (double)base);
                const int i0 = (int)(base % capacity);
                const int i1 = (i0 + 1 == capacity) ? 0 : i0 + 1;
                const float v0 = ring[i0];
                out[s] = v0 + (ring[i1] - v0) * frac;
                pos += window.ratio;
            }
        }

        // A capture underrun fades the last real sample to silence instead of
        // ending in a hard step (or repeating the previous block), and the
        // next complete block ramps back in.
        if (n < frames) {
            const float last = n > 0 ? out[n - 1] : lastPlayed_[i];
            const int gap = frames - n;
            for (int s = 0; s < gap; ++s) {
                const float t = 1.0f -
                                (float)(s + 1) / (float)(gap + 1);
                out[n + s] = last * t;
            }
        } else if (applyFadeIn) {
            const int fade = juce::jmin(64, frames);
            for (int s = 0; s < fade; ++s) {
                out[s] *= (float)(s + 1) / (float)(fade + 1);
            }
        }
        lastPlayed_[i] = out[frames - 1];

        // Mono (single physical channel): upmix to both L and R so that
        // panning works correctly (50/50 at center).
        if (numDest == 1 && dest.getNumChannels() >= 2) {
            float* outR = dest.getWritePointer(1);
            for (int s = 0; s < frames; ++s) outR[s] = out[s];
        }
    }
    if (missing)
        inputUnavailable_.store(true, std::memory_order_relaxed);
    if (n < frames && primedEnough && window.fresh) {
        inputDroppedFrames.fetch_add((std::uint64_t)(frames - n),
                                     std::memory_order_relaxed);
    }

    // Round-trip latency measurement: detect the injected impulse on input.
    if (latencyMeasureActive.load(std::memory_order_relaxed) &&
        !latencyImpulseDetected.load(std::memory_order_relaxed)) {
        for (int i = 0; i < numDest; ++i) {
            for (int s = 0; s < n; ++s) {
                if (std::abs(dest.getSample(i, s)) > 0.9f) {
                    latencyImpulseInputSample.store(
                        (std::uint64_t)(window.start + s),
                        std::memory_order_relaxed);
                    latencyImpulseDetected.store(true, std::memory_order_relaxed);
                    break;
                }
            }
            if (latencyImpulseDetected.load(std::memory_order_relaxed)) break;
        }
    }
    return ok;
}

void InputCapture::audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels, float* const*, int, int numSamples, const juce::AudioIODeviceCallbackContext&) {
    realtime::registerProAudioThread();
    const juce::ScopedNoDenormals noDenormals;
    if (capacity <= 0 || numSamples <= 0) return;
    if (numSamples > capacity) {
        inputDroppedFrames.fetch_add((std::uint64_t) numSamples,
                                     std::memory_order_relaxed);
        return;
    }

    const auto base = totalWritten.load(std::memory_order_relaxed);
    const int first = juce::jmin(numSamples, capacity - writePos);
    // Write to the ring (broadcast to all consumers) before publishing the
    // new total. No lock: the reader only ever consumes data older than the
    // target delay, so the producer is never blocked.
    for (int ch = 0; ch < inputChannelCount_; ++ch) {
        const bool have = numInputChannels > ch && inputChannelData != nullptr &&
                          inputChannelData[ch] != nullptr;
        if (!have)
            inputUnavailable_.store(true, std::memory_order_relaxed); // silence, never remap to input 0
        float* dst = storage.getWritePointer(ch);
        if (have) {
            const float* src = inputChannelData[ch];
            std::memcpy(dst + writePos, src,
                        sizeof(float) * (std::size_t)first);
            if (numSamples > first) {
                std::memcpy(dst, src + first,
                            sizeof(float) * (std::size_t)(numSamples - first));
            }
        } else {
            std::memset(dst + writePos, 0, sizeof(float) * (std::size_t)first);
            if (numSamples > first) {
                std::memset(dst, 0,
                            sizeof(float) * (std::size_t)(numSamples - first));
            }
        }
    }
    writePos = (writePos + numSamples) % capacity;
    totalWritten.store(base + (std::uint64_t) numSamples,
                       std::memory_order_release);
    // Channels beyond what the device provides are not stored (storage sized
    // to inputChannelCount_); they are reported via getInputError() and pull()
    // leaves their destination silent.
    // finishedWrite automatic
}

void InputCapture::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    if (device != nullptr) {
        sampleRate = device->getCurrentSampleRate();
        deviceInputLatency = device->getInputLatencyInSamples();
        deviceBufferSize = device->getCurrentBufferSizeSamples();
        if (deviceBufferSize <= 0) deviceBufferSize = 256;
        latencySamples.store(deviceInputLatency +
                                 readDelaySamples.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
    }
}

void InputCapture::audioDeviceStopped() {
    // Publishing a zero total makes the reader re-prime instead of replaying
    // its last window. The ring contents are stale either way, and the reader
    // owns the cursor, so no lock is needed here.
    writePos = 0;
    totalWritten.store(0, std::memory_order_relaxed);
}

// ---------- OutputPlayback ----------

OutputPlayback::OutputPlayback() = default;

OutputPlayback::~OutputPlayback() {
    close();
}

juce::String OutputPlayback::open(const juce::String& preferredDeviceName,
                                  double engineSampleRate,
                                  juce::String& errorOut,
                                  int requestedBufferSize,
                                  int targetFillBlocks,
                                  int primeBlocks,
                                  bool preferExclusive,
                                  int renderBlockFrames,
                                  pluginrack::StreamRole role,
                                  int duplexInputChannels) {
    close();
    closing_.store(false, std::memory_order_relaxed);
    error.clear();

    // One attempt in the preferred device type (shared, or exclusive when it
    // was explicitly enabled). A refusal is reported, never retried.
    activeDeviceType_ = openWithPreferredType(manager, preferredDeviceName, /*wantInput*/ false,
                                              0, 2,
                                              engineSampleRate, requestedBufferSize, errorOut,
                                              preferExclusive, role,
                                              duplexInputChannels);
    if (activeDeviceType_.isEmpty()) {
        error = errorOut;
        return errorOut;
    }
    auto* dev = manager.getCurrentAudioDevice();
    if (dev == nullptr) {
        const auto msg = juce::String("Could not open output device.");
        error = msg;
        errorOut = msg;
        close();
        return msg;
    }

    const double actual = dev->getCurrentSampleRate();
    if (engineSampleRate > 0.0 && std::abs(actual - engineSampleRate) > 0.5) {
        const auto msg = "Needs " + formatKhz(engineSampleRate) + " (device is " + formatKhz(actual) + ")";
        error = msg;
        errorOut = msg;
        close();
        return msg;
    }

    sampleRate = actual;
    duplex_.store(duplexInputChannels > 0, std::memory_order_relaxed);
    if (duplex_.load(std::memory_order_relaxed)) {
        duplexInputChannels_.store(static_cast<int>(dev->getInputChannelNames().size()),
                                   std::memory_order_relaxed);
        deviceInputLatency_.store(dev->getInputLatencyInSamples(), std::memory_order_relaxed);
        // The callback owns both directions and writes the render result
        // straight into the device buffer, so a FIFO target here would only add
        // latency: there is nothing to queue.
        targetFillBlocks = 0;
        primeBlocks = 0;
    }
    bufferSize = dev->getCurrentBufferSizeSamples();
    if (bufferSize <= 0) bufferSize = 256;
    renderBlockFrames_ = renderBlockFrames > 0 ? renderBlockFrames : bufferSize;
    deviceOutputLatency = dev->getOutputLatencyInSamples();
    // Target the requested number of device buffers of headroom with two
    // spare blocks for bursts. The old fixed 16384 cap could make the ring
    // smaller than the priming target at large device buffer sizes, so the
    // first push was always dropped.
    const int targetFrames = bufferSize * juce::jmax(0, targetFillBlocks);
    targetFillFrames.store(targetFrames, std::memory_order_relaxed);
    // The render block belongs to the clock output and can differ from this
    // device's callback size. Reserve enough room for a complete render block
    // as well as the normal device-buffer headroom; otherwise a small
    // non-clock buffer rejects every push from a larger clock block.
    const int renderHeadroom = juce::jmax(bufferSize, renderBlockFrames_ * 2);
    capacity = juce::jmax(targetFrames + renderHeadroom,
                          juce::jmin(juce::jmax(bufferSize * 8,
                                               renderBlockFrames_ * 2),
                                     65536));
    fifo = std::make_unique<juce::AbstractFifo>(capacity);
    storage.setSize(2, capacity);
    storage.clear();
    pushFadeInPending = false;
    deviceFadeInRemaining = 0;
    lowWatermarkActive = false;
    lastPushedSample[0] = 0.0f;
    lastPushedSample[1] = 0.0f;
    // Prime the FIFO so startup and a single delayed render callback cannot
    // expose an empty FIFO to the device. Leave room for one rendered block
    // so the first push is never dropped. primeBlocks = 0 is used by the
    // minimal-latency clock: the render/drain happen in the same callback, so
    // a primed block would only add latency there.
    const int prime = primeBlocks >= 0 ? primeBlocks : targetFillBlocks;
    const int primeFrames = juce::jlimit(0, juce::jmax(0, capacity - bufferSize),
                                         bufferSize * juce::jmax(0, prime));
    if (primeFrames > 0) {
        fifo->write(primeFrames);
    }
    fillFrames.store(primeFrames > 0 ? primeFrames : 0, std::memory_order_relaxed);
    latencySamples.store(deviceOutputLatency + bufferSize,
                         std::memory_order_relaxed);

    manager.addAudioCallback(this);
    return {};
}

void OutputPlayback::close() {
    closing_.store(true, std::memory_order_relaxed);
    manager.removeAudioCallback(this);
    manager.closeAudioDevice();
    duplex_.store(false, std::memory_order_relaxed);
    duplexInputChannels_.store(0, std::memory_order_relaxed);
    deviceInputLatency_.store(0, std::memory_order_relaxed);
    duplexInputHook = nullptr;
    {
        // The clock device's render thread may still be inside push() from an
        // in-flight render. Keep close() from pulling the FIFO out from under
        // it; push() only tries this lock and drops its block when contended.
        const juce::SpinLock::ScopedLockType lock(fifoLock);
        fifo.reset();
        storage.setSize(0, 0, false, true, false);
    }
    sampleRate = 0.0;
    activeDeviceType_.clear();
    deviceOutputLatency = 0;
    renderBlockFrames_ = 0;
    latencySamples.store(0, std::memory_order_relaxed);
    fillFrames.store(0, std::memory_order_relaxed);
    deviceFadeInRemaining = 0;
    pushFadeInPending = false;
    lowWatermarkActive = false;
}

void OutputPlayback::setTargetFillBlocks(int blocks) {
    if (bufferSize <= 0) return;
    const int wanted = bufferSize * juce::jmax(0, blocks);
    if (wanted == targetFillFrames.load(std::memory_order_relaxed)) return;
    targetFillFrames.store(wanted, std::memory_order_relaxed);
    trimToTarget.store(true, std::memory_order_release);
}

void OutputPlayback::push(const juce::AudioBuffer<float>& src, int frames) {
    if (frames <= 0) return;

    // Duplex: the render result goes straight into the device buffer that this
    // same callback is about to hand to the driver. No queue, no latency, and
    // no consumer to race with.
    if (duplex_.load(std::memory_order_relaxed)) {
        if (currentOutputChannels == nullptr) {
            outputDroppedFrames.fetch_add((std::uint64_t) frames,
                                          std::memory_order_relaxed);
            return;
        }
        const int srcChans = juce::jmax(1, src.getNumChannels());
        const int chans = juce::jmin(2, currentNumOutputChannels);
        for (int ch = 0; ch < chans; ++ch) {
            float* dst = currentOutputChannels[ch];
            if (dst == nullptr) continue;
            const int srcCh = juce::jmin(ch, srcChans - 1);
            std::memcpy(dst, src.getReadPointer(srcCh),
                        sizeof(float) * (std::size_t) frames);
        }
        fillFrames.store(0, std::memory_order_relaxed);
        return;
    }

    const juce::SpinLock::ScopedTryLockType lock(fifoLock);
    if (!lock.isLocked() || fifo == nullptr) {
        outputDroppedFrames.fetch_add((std::uint64_t) frames,
                                      std::memory_order_relaxed);
        return;
    }
    if (frames > capacity || fifo->getFreeSpace() < frames) {
        outputDroppedFrames.fetch_add((std::uint64_t) frames,
                                      std::memory_order_relaxed);
        return;
    }
    auto writeHandle = fifo->write(frames);
    const int b1 = writeHandle.blockSize1;
    const int b2 = writeHandle.blockSize2;
    const int total = b1 + b2;
    if (total != frames) return;
    const int srcChans = juce::jmax(1, src.getNumChannels());
    const int fade = pushFadeInPending ? juce::jmin(64, frames) : 0;

    for (int ch = 0; ch < 2; ++ch) {
        const int srcCh = juce::jmin(ch, srcChans - 1);
        float* dst = storage.getWritePointer(ch);
        if (b1 > 0) {
            std::memcpy(dst + writeHandle.startIndex1, src.getReadPointer(srcCh),
                        sizeof(float) * (std::size_t)b1);
        }
        if (b2 > 0) {
            std::memcpy(dst + writeHandle.startIndex2,
                        src.getReadPointer(srcCh) + b1,
                        sizeof(float) * (std::size_t)b2);
        }
        if (fade > 0) {
            // Resume after a silent fallback with a short ramp so the first
            // real block does not start with a step.
            int seen = 0;
            for (int i = 0; i < b1; ++i, ++seen) {
                if (seen < fade) {
                    dst[writeHandle.startIndex1 + i] *=
                        (float)(seen + 1) / (float)(fade + 1);
                }
            }
            for (int i = 0; i < b2; ++i, ++seen) {
                if (seen < fade) {
                    dst[writeHandle.startIndex2 + i] *=
                        (float)(seen + 1) / (float)(fade + 1);
                }
            }
        }
        const int lastIndex = b2 > 0 ? writeHandle.startIndex2 + b2 - 1
                                     : writeHandle.startIndex1 + b1 - 1;
        lastPushedSample[ch] = dst[lastIndex];
    }
    pushFadeInPending = false;
    fillFrames.store(fifo->getNumReady(), std::memory_order_relaxed);
    // Structural delay only (see InputCapture::pull): device + one buffer.
    // The FIFO fill itself is reported separately by getFillFrames().
    latencySamples.store(deviceOutputLatency + bufferSize,
                         std::memory_order_relaxed);
}

void OutputPlayback::pushSilence(int frames) {
    if (frames <= 0) return;

    if (duplex_.load(std::memory_order_relaxed)) {
        // Nothing is queued in duplex mode: silence the device buffer directly.
        if (currentOutputChannels != nullptr) {
            for (int ch = 0; ch < juce::jmin(2, currentNumOutputChannels); ++ch) {
                if (currentOutputChannels[ch] != nullptr) {
                    juce::FloatVectorOperations::clear(currentOutputChannels[ch], frames);
                }
            }
        }
        return;
    }

    const juce::SpinLock::ScopedTryLockType lock(fifoLock);
    if (!lock.isLocked() || fifo == nullptr || capacity <= 0) return;
    // Only top the FIFO back up to its target. A single missed render is
    // already covered by the buffered audio; inserting silence on top of a
    // full FIFO would both delay the dropout and spam the dropped-frame
    // counter for the whole stall.
    if (targetFillFrames.load(std::memory_order_relaxed) > 0
        && fifo->getNumReady() >= targetFillFrames.load(std::memory_order_relaxed)) {
        return;
    }
    if (frames > capacity || fifo->getFreeSpace() < frames) {
        outputDroppedFrames.fetch_add((std::uint64_t) frames,
                                      std::memory_order_relaxed);
        return;
    }
    auto writeHandle = fifo->write(frames);
    const int b1 = writeHandle.blockSize1;
    const int b2 = writeHandle.blockSize2;
    if (b1 + b2 != frames) return;
    const int fade = juce::jmin(64, frames);
    for (int ch = 0; ch < 2; ++ch) {
        float* dst = storage.getWritePointer(ch);
        const float start = lastPushedSample[ch];
        int seen = 0;
        for (int i = 0; i < b1; ++i, ++seen) {
            const float gain = seen < fade
                ? 1.0f - (float)(seen + 1) / (float)(fade + 1)
                : 0.0f;
            dst[writeHandle.startIndex1 + i] = start * gain;
        }
        for (int i = 0; i < b2; ++i, ++seen) {
            const float gain = seen < fade
                ? 1.0f - (float)(seen + 1) / (float)(fade + 1)
                : 0.0f;
            dst[writeHandle.startIndex2 + i] = start * gain;
        }
        lastPushedSample[ch] = 0.0f;
    }
    pushFadeInPending = true;
    fillFrames.store(fifo->getNumReady(), std::memory_order_relaxed);
}

void OutputPlayback::audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels, float* const* outputChannelData, int numOutputChannels, int numSamples, const juce::AudioIODeviceCallbackContext&) {
    realtime::registerProAudioThread();
    const juce::ScopedNoDenormals noDenormals;
    if (closing_.load(std::memory_order_relaxed)) {
        for (int ch = 0; ch < numOutputChannels; ++ch) {
            if (outputChannelData[ch] != nullptr) {
                juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
            }
        }
        return;
    }

    currentOutputChannels = outputChannelData;
    currentNumOutputChannels = numOutputChannels;

    // Duplex: this callback owns both directions, so hand the capture to the
    // engine before it renders. No ring, no second device client.
    const bool duplex = duplex_.load(std::memory_order_relaxed);
    if (duplex && duplexInputHook) {
        duplexInputHook(inputChannelData, numInputChannels);
    }

    // A stream opened before its feed was running is primed deep enough to
    // survive the hand-off. Once it is fed every callback that prime is pure
    // latency, so drop the stale backlog here, on the only thread that reads
    // this FIFO, before the new block is pushed.
    if (trimToTarget.exchange(false, std::memory_order_acquire)) {
        const int ready = fifo->getNumReady();
        const int target = juce::jmax(0, targetFillFrames.load(std::memory_order_relaxed));
        if (ready > target) {
            auto stale = fifo->read(ready - target);
            juce::ignoreUnused(stale);
            deviceFadeInRemaining = 64;
            lowWatermarkActive = false;
        }
    }

    bool rendered = false;
    if (renderActive.load() && renderHook) {
        renderHook(numSamples);
        rendered = true;
    }

    if (duplex) {
        // push() wrote the rendered block straight into these buffers, so a
        // clear here would erase it. Only a callback that did not render needs
        // one, and there is no FIFO to drain either way.
        if (!rendered) {
            for (int ch = 0; ch < numOutputChannels; ++ch) {
                if (outputChannelData[ch] != nullptr) {
                    juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
                }
            }
        }
        currentOutputChannels = nullptr;
        fillFrames.store(0, std::memory_order_relaxed);
        return;
    }

    // clear device
    for (int ch = 0; ch < numOutputChannels; ++ch) {
        if (outputChannelData[ch] != nullptr) {
            juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
        }
    }

    if (fifo == nullptr) {
        currentOutputChannels = nullptr;
        return;
    }

    // Independent output devices do not share a sample clock with the clock
    // output. Nudge the read rate by at most 0.5% according to FIFO fill so a
    // two-buffer low-latency target does not slowly drain or grow over time.
    // When the fill is on target this remains the old memcpy fast path.
    const int readyBefore = fifo->getNumReady();
    constexpr double kMaxDriftCorrection = 0.005;
    const int targetFill = targetFillFrames.load(std::memory_order_relaxed);
    const int fillScale = juce::jmax(1, juce::jmax(targetFill, bufferSize));
    const double fillCorrection = targetFill > 0
        ? std::clamp((double)(readyBefore - targetFill)
                         / (double)fillScale * kMaxDriftCorrection,
                     -kMaxDriftCorrection, kMaxDriftCorrection)
        : 0.0;
    const double readRatio = 1.0 + fillCorrection;
    const int requestedSourceFrames = static_cast<int>(std::ceil(
        (double)juce::jmax(0, numSamples - 1) * readRatio)) + 1;
    const int sourceFrames = juce::jlimit(1, juce::jmax(1, capacity),
                                          requestedSourceFrames);
    auto readHandle = fifo->read(sourceFrames);
    const int b1 = readHandle.blockSize1;
    const int b2 = readHandle.blockSize2;
    const int total = b1 + b2;
    const bool shortRead = total < sourceFrames;
    const int produced = total > 0
        ? juce::jmin(numSamples, static_cast<int>(std::ceil((double)total / readRatio)))
        : 0;
    if (shortRead) {
        outputUnderruns.fetch_add(1, std::memory_order_relaxed);
        outputDroppedFrames.fetch_add((std::uint64_t)(numSamples - produced),
                                      std::memory_order_relaxed);
        // Fade the short tail to silence; the next complete block fades back
        // in instead of starting at an arbitrary amplitude.
        deviceFadeInRemaining = 64;
        // A zero target assumes the render hook always pushes exactly one
        // device block before this read. If that assumption ever fails, buy
        // one block of margin back rather than glitching on every callback.
        if (targetFillFrames.load(std::memory_order_relaxed) == 0
            && !trimToTarget.load(std::memory_order_relaxed)) {
            targetFillFrames.store(bufferSize, std::memory_order_relaxed);
        }
    }

    const int chans = juce::jmin(2, numOutputChannels);
    const bool exactRead = !shortRead && std::abs(readRatio - 1.0) < 1.0e-12
                        && sourceFrames == numSamples;
    const int fadeIn = shortRead ? 0 : juce::jmin(deviceFadeInRemaining, numSamples);
    const int fadeDone = 64 - deviceFadeInRemaining;
    const auto sampleAt = [this, &readHandle, b1](int channel, int index) noexcept {
        return index < b1
            ? this->storage.getReadPointer(channel)[readHandle.startIndex1 + index]
            : this->storage.getReadPointer(channel)[readHandle.startIndex2 + index - b1];
    };
    for (int ch = 0; ch < chans; ++ch) {
        float* out = outputChannelData[ch];
        if (out == nullptr) continue;
        if (exactRead) {
            const float* src = storage.getReadPointer(ch);
            if (b1 > 0) {
                std::memcpy(out, src + readHandle.startIndex1,
                            sizeof(float) * (std::size_t)b1);
            }
            if (b2 > 0) {
                std::memcpy(out + b1, src + readHandle.startIndex2,
                            sizeof(float) * (std::size_t)b2);
            }
        } else {
            for (int i = 0; i < produced; ++i) {
                const double position = (double)i * readRatio;
                const int base = static_cast<int>(std::floor(position));
                const float fraction = static_cast<float>(position - (double)base);
                const float first = sampleAt(ch, base);
                out[i] = base + 1 < total
                    ? first + (sampleAt(ch, base + 1) - first) * fraction
                    : first;
            }
        }
        if (produced < numSamples) {
            const float last = produced > 0 ? out[produced - 1] : 0.0f;
            const int gap = numSamples - produced;
            for (int i = 0; i < gap; ++i) {
                out[produced + i] =
                    last * (1.0f - (float)(i + 1) / (float)(gap + 1));
            }
        } else if (fadeIn > 0) {
            // Continue the same 64-sample ramp across small device blocks so
            // the gain does not restart at every block boundary.
            constexpr int kFadeLength = 64;
            for (int i = 0; i < fadeIn; ++i) {
                out[i] *= (float)(fadeDone + i + 1) / (float)(kFadeLength + 1);
            }
        }
    }

    if (!shortRead && deviceFadeInRemaining > 0)
        deviceFadeInRemaining -= fadeIn;

    const int ready = fifo->getNumReady();
    fillFrames.store(ready, std::memory_order_relaxed);
    // Count low-watermark events, not every callback spent below the mark: a
    // persistent starvation would otherwise inflate the counter forever. A
    // zero target (the clock's running depth) is not a watermark at all.
    const bool low = targetFill > 0 && ready < bufferSize;
    if (low) {
        if (!lowWatermarkActive) {
            outputLowWatermarks.fetch_add(1, std::memory_order_relaxed);
            lowWatermarkActive = true;
        }
    } else if (ready >= bufferSize * 2) {
        lowWatermarkActive = false;
    }
    currentOutputChannels = nullptr;
}

void OutputPlayback::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    if (device != nullptr) {
        sampleRate = device->getCurrentSampleRate();
        bufferSize = device->getCurrentBufferSizeSamples();
        if (bufferSize <= 0) bufferSize = 256;
        deviceOutputLatency = device->getOutputLatencyInSamples();
        latencySamples.store(deviceOutputLatency + bufferSize,
                             std::memory_order_relaxed);
    }
}

void OutputPlayback::audioDeviceStopped() {}
