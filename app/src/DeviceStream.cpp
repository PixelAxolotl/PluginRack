#include "DeviceStream.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
juce::String formatKhz(double rate) {
    double k = rate / 1000.0;
    if (std::abs(k - std::round(k)) < 0.001) {
        return juce::String((int)std::round(k)) + " kHz";
    }
    return juce::String(k, 1) + " kHz";
}

// Device type names registered by JUCE for the WASAPI modes, in the order we
// prefer them: event-driven shared first, then exclusive, then plain shared.
// ASIO (only when this build has JUCE_ASIO) is tried last, after the WASAPI
// modes. "Windows Audio (Exclusive Mode)" is JUCE's exact registered name.
const juce::String kTypeLowLatency = "Windows Audio (Low Latency Mode)";
const juce::String kTypeExclusive = "Windows Audio (Exclusive Mode)";
const juce::String kTypeShared = "Windows Audio";
const juce::String kTypeAsio = "ASIO";

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

// Legacy open path: wildcard device-name search over all registered types,
// then rate/buffer applied through setAudioDeviceSetup (rolled back on error).
// Returns the active type name, or empty with errorOut set on failure.
juce::String openWithWildcardSearch(juce::AudioDeviceManager& manager,
                                    const juce::String& preferredDeviceName,
                                    int numInputChannels,
                                    int numOutputChannels,
                                    double engineSampleRate,
                                    int requestedBufferSize,
                                    juce::String& errorOut) {
    auto err = manager.initialise(numInputChannels, numOutputChannels, nullptr, true,
                                  preferredDeviceName, nullptr);
    if (err.isEmpty() && requestedBufferSize >= 0) {
        const juce::AudioDeviceManager::AudioDeviceSetup prev = manager.getAudioDeviceSetup();
        juce::AudioDeviceManager::AudioDeviceSetup setup = prev;
        if (requestedBufferSize > 0) setup.bufferSize = requestedBufferSize;
        if (engineSampleRate > 0.0) setup.sampleRate = engineSampleRate;
        err = manager.setAudioDeviceSetup(setup, true);
        if (err.isEmpty() && requestedBufferSize > 0) {
            auto* device = manager.getCurrentAudioDevice();
            if (device == nullptr || device->getCurrentBufferSizeSamples() != requestedBufferSize) {
                err = "The device does not support a " + juce::String(requestedBufferSize)
                    + "-sample buffer.";
            }
        }
        if (err.isNotEmpty())
            manager.setAudioDeviceSetup(prev, true);
    }
    // initialise() silently falls back to the default device when the wildcard
    // search finds no match, so an unplugged device would otherwise report
    // success and leave the strip's status dot green. Treat that as the
    // missing-device error it is.
    if (err.isEmpty() && preferredDeviceName.isNotEmpty()) {
        const auto setup = manager.getAudioDeviceSetup();
        const bool requested = setup.outputDeviceName.equalsIgnoreCase(preferredDeviceName)
                            || setup.inputDeviceName.equalsIgnoreCase(preferredDeviceName);
        if (!requested) {
            err = "No such device: " + preferredDeviceName;
            manager.closeAudioDevice();
        }
    }
    if (err.isNotEmpty()) {
        errorOut = err;
        return {};
    }
    return manager.getCurrentAudioDeviceType();
}

// Opens preferredDeviceName (empty = system default) in the first device type
// from the low-latency chain that actually carries it, passing the full
// AudioDeviceSetup with the name explicit instead of relying on a wildcard
// match. A type switch that opens another device is closed again before the
// chosen setup is applied. On success returns the selected type name; on
// failure returns empty and sets errorOut.
juce::String openWithPreferredType(juce::AudioDeviceManager& manager,
                                   const juce::String& preferredDeviceName,
                                   bool wantInput,
                                   int numInputChannels,
                                   int numOutputChannels,
                                   double engineSampleRate,
                                   int requestedBufferSize,
                                   juce::String& errorOut,
                                   bool preferExclusive = false) {
    juce::StringArray typeOrder { kTypeLowLatency, kTypeExclusive, kTypeShared, kTypeAsio };
    if (preferExclusive) {
        typeOrder.removeString(kTypeExclusive);
        typeOrder.insert(0, kTypeExclusive);
    }

    juce::String lastError;
    for (const auto& typeName : typeOrder) {
        auto* type = ensureTypeRegistered(manager, typeName);
        if (type == nullptr)
            continue; // platform without this type (or ASIO built without JUCE_ASIO)
        const auto deviceNames = type->getDeviceNames(wantInput);
        const bool hasDevice = preferredDeviceName.isEmpty()
                                   ? !deviceNames.isEmpty()
                                   : deviceNames.contains(preferredDeviceName);
        if (!hasDevice)
            continue; // no matching device name in this type: fall through

        manager.setCurrentAudioDeviceType(typeName, false);
        if (manager.getCurrentAudioDeviceType() != typeName)
            continue;
        // The type switch probes the type's default device; release it so the
        // selected endpoint below is the only one that stays open.
        manager.closeAudioDevice();

        const std::vector<int> bufferCandidates{requestedBufferSize};
        for (const int candidate : bufferCandidates) {
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            if (wantInput)
                setup.inputDeviceName = preferredDeviceName;
            else
                setup.outputDeviceName = preferredDeviceName;
            if (requestedBufferSize >= 0) {
                setup.sampleRate = engineSampleRate; // 0 = driver default
                setup.bufferSize = candidate;
            }
            auto err = manager.initialise(numInputChannels, numOutputChannels, nullptr, true,
                                          preferredDeviceName, &setup);
            if (err.isEmpty()) {
                auto* device = manager.getCurrentAudioDevice();
                if (requestedBufferSize > 0
                    && (device == nullptr
                        || device->getCurrentBufferSizeSamples() != requestedBufferSize)) {
                    err = "The device does not support a " + juce::String(requestedBufferSize)
                        + "-sample buffer.";
                } else {
                    return typeName;
                }
            }
            lastError = err;
            manager.closeAudioDevice();
        }
    }

    // Nothing in the chain opened this device (non-WASAPI backends, DirectSound
    // only names, or every mode refused): keep the legacy behaviour.
    auto fallback = openWithWildcardSearch(manager, preferredDeviceName, numInputChannels,
                                           numOutputChannels, engineSampleRate,
                                           requestedBufferSize, errorOut);
    if (fallback.isEmpty() && lastError.isNotEmpty())
        errorOut = lastError;
    return fallback;
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
                               bool preferExclusive) {
    close();
    error.clear();
    inputError.clear();

    const int numInputChannels = juce::jmax(2, numChannelsNeeded);
    // Pick the lowest-latency device type that carries this device before
    // initialise (sharedLowLatency -> exclusive -> shared -> ASIO).
    activeDeviceType_ = openWithPreferredType(manager, preferredDeviceName, /*wantInput*/ true,
                                              numInputChannels, 0,
                                              engineSampleRate, requestedBufferSize, errorOut,
                                              preferExclusive);
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
    availableInputCount_ = availableInputs < 1 ? 1 : availableInputs;
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
    capacity = juce::jmax(blockFrames * 4,
                          juce::jmin(blockFrames * 8, 65536));
    storage.setSize(inputChannelCount_, capacity);
    storage.clear();
    writePos = 0;
    totalWritten.store(0, std::memory_order_relaxed);

    // Read delay absorbs callback phase jitter and gives the drift correction
    // room to work without an audible block repeat. Low-latency mode uses one
    // block instead of two.
    const int maxDelay = juce::jmax(0, capacity - blockFrames - 2);
    const int delay = juce::jlimit(0, maxDelay, blockFrames * juce::jmax(1, readDelayBlocks));
    readDelaySamples.store(delay, std::memory_order_relaxed);
    readCursor.prepare(capacity, delay, 0.005);
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
    availableInputCount_ = 0;
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
    latencySamples.store(0, std::memory_order_relaxed);
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
    const bool primedEnough = written >= targetDelay;
    if (window.underrun && window.fresh && primedEnough)
        inputUnderruns.fetch_add(1, std::memory_order_relaxed);
    if (window.overrun && window.fresh) inputOverruns.fetch_add(1, std::memory_order_relaxed);

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
                                 bool preferExclusive) {
    close();
    closing_.store(false, std::memory_order_relaxed);
    error.clear();

    // Pick the lowest-latency device type that carries this device before
    // initialise (sharedLowLatency -> exclusive -> shared -> ASIO).
    activeDeviceType_ = openWithPreferredType(manager, preferredDeviceName, /*wantInput*/ false,
                                              0, 2,
                                              engineSampleRate, requestedBufferSize, errorOut,
                                              preferExclusive);
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
    bufferSize = dev->getCurrentBufferSizeSamples();
    if (bufferSize <= 0) bufferSize = 256;
    deviceOutputLatency = dev->getOutputLatencyInSamples();
    // Target the requested number of device buffers of headroom with two
    // spare blocks for bursts. The old fixed 16384 cap could make the ring
    // smaller than the priming target at large device buffer sizes, so the
    // first push was always dropped.
    targetFillFrames = bufferSize * juce::jmax(0, targetFillBlocks);
    capacity = juce::jmax(targetFillFrames + bufferSize * 2,
                          juce::jmin(bufferSize * 8, 65536));
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
    latencySamples.store(0, std::memory_order_relaxed);
    fillFrames.store(0, std::memory_order_relaxed);
    deviceFadeInRemaining = 0;
    pushFadeInPending = false;
    lowWatermarkActive = false;
}

void OutputPlayback::push(const juce::AudioBuffer<float>& src, int frames) {
    if (frames <= 0) return;
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
    const juce::SpinLock::ScopedTryLockType lock(fifoLock);
    if (!lock.isLocked() || fifo == nullptr) return;
    // Only top the FIFO back up to its target. A single missed render is
    // already covered by the buffered audio; inserting silence on top of a
    // full FIFO would both delay the dropout and spam the dropped-frame
    // counter for the whole stall.
    if (targetFillFrames > 0 && fifo->getNumReady() >= targetFillFrames) {
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

void OutputPlayback::audioDeviceIOCallbackWithContext (const float* const*, int, float* const* outputChannelData, int numOutputChannels, int numSamples, const juce::AudioIODeviceCallbackContext&) {
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

    if (renderActive.load() && renderHook) {
        renderHook(numSamples);
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
    auto readHandle = fifo->read(numSamples);
    fillFrames.store(fifo->getNumReady(), std::memory_order_relaxed);
    const int b1 = readHandle.blockSize1;
    const int b2 = readHandle.blockSize2;
    const int total = b1 + b2;
    const bool shortRead = total < numSamples;
    if (shortRead) {
        outputUnderruns.fetch_add(1, std::memory_order_relaxed);
        outputDroppedFrames.fetch_add((std::uint64_t)(numSamples - total),
                                      std::memory_order_relaxed);
        // Fade the short tail to silence; the next complete block fades back
        // in instead of starting at an arbitrary amplitude.
        deviceFadeInRemaining = 64;
    }

    const int chans = juce::jmin(2, numOutputChannels);
    for (int ch = 0; ch < chans; ++ch) {
        float* out = outputChannelData[ch];
        if (out == nullptr) continue;
        const float* src = storage.getReadPointer(ch);
        if (b1 > 0) {
            std::memcpy(out, src + readHandle.startIndex1,
                        sizeof(float) * (std::size_t)b1);
        }
        if (b2 > 0) {
            std::memcpy(out + b1, src + readHandle.startIndex2,
                        sizeof(float) * (std::size_t)b2);
        }
        if (shortRead) {
            const float last = total > 0 ? out[total - 1] : 0.0f;
            const int gap = numSamples - total;
            for (int i = 0; i < gap; ++i) {
                out[total + i] =
                    last * (1.0f - (float)(i + 1) / (float)(gap + 1));
            }
        } else if (deviceFadeInRemaining > 0) {
            // Continue the same 64-sample ramp across small device blocks so
            // the gain does not restart at every block boundary.
            constexpr int kFadeLength = 64;
            const int done = kFadeLength - deviceFadeInRemaining;
            const int fade = juce::jmin(deviceFadeInRemaining, numSamples);
            for (int i = 0; i < fade; ++i) {
                out[i] *= (float)(done + i + 1) / (float)(kFadeLength + 1);
            }
            deviceFadeInRemaining -= fade;
        }
    }

    const int ready = fifo->getNumReady();
    fillFrames.store(ready, std::memory_order_relaxed);
    // Count low-watermark events, not every callback spent below the mark: a
    // persistent starvation would otherwise inflate the counter forever.
    const bool low = ready < bufferSize;
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
