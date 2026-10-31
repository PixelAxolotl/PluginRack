#include "DeviceStream.h"

#include <cmath>

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
        if (err.isNotEmpty())
            manager.setAudioDeviceSetup(prev, true);
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
                                   juce::String& errorOut) {
    static const juce::StringArray typeOrder { kTypeLowLatency, kTypeExclusive,
                                               kTypeShared, kTypeAsio };

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

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        if (wantInput)
            setup.inputDeviceName = preferredDeviceName;
        else
            setup.outputDeviceName = preferredDeviceName;
        if (requestedBufferSize >= 0) {
            setup.sampleRate = engineSampleRate; // 0 = driver default
            setup.bufferSize = requestedBufferSize > 0 ? requestedBufferSize : 0;
        }
        auto err = manager.initialise(numInputChannels, numOutputChannels, nullptr, true,
                                      preferredDeviceName, &setup);
        if (err.isEmpty())
            return typeName;
        lastError = err;
        manager.closeAudioDevice();
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
                               int requestedBufferSize) {
    close();
    error.clear();
    inputError.clear();

    const int numInputChannels = juce::jmax(2, numChannelsNeeded);
    // Pick the lowest-latency device type that carries this device before
    // initialise (sharedLowLatency -> exclusive -> shared -> ASIO).
    activeDeviceType_ = openWithPreferredType(manager, preferredDeviceName, /*wantInput*/ true,
                                              numInputChannels, 0,
                                              engineSampleRate, requestedBufferSize, errorOut);
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
    if (requestedBufferSize > 0 && deviceBufferSize != requestedBufferSize) {
        // setAudioDeviceSetup() clamps to the nearest size from
        // getAvailableBufferSizes(); accept the rounded value and report it
        // informationally instead of failing the open.
        error = "info: device buffer is " + juce::String(deviceBufferSize)
                + " samples (requested " + juce::String(requestedBufferSize) + ")";
    }

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

    // 8 device buffers (capped) so transient render pauses cannot drain the
    // ring between pull() calls. Never smaller than one device block.
    capacity = juce::jmax(deviceBufferSize, juce::jmin(deviceBufferSize * 8, 16384));
    storage.setSize(inputChannelCount_, capacity);
    storage.clear();
    writePos = 0;
    totalWritten = 0;

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
    totalWritten = 0;
    latencySamples.store(0, std::memory_order_relaxed);
}

bool InputCapture::pull(juce::AudioBuffer<float>& dest, int frames,
                        const std::vector<int>& physicalChannels) {
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

    // The writer (audioDeviceIOCallback) holds this spinlock while mutating
    // storage/writePos/totalWritten. Reading them unlocked races and produces
    // torn samples (crackling). Take the lock; bail to silence if contended
    // rather than blocking the audio thread.
    juce::SpinLock::ScopedTryLockType tryLock(lock);
    if (!tryLock.isLocked()) {
        return false;
    }

    // Structural delay only: device latency + one device buffer.
    latencySamples.store(deviceInputLatency + deviceBufferSize,
                        std::memory_order_relaxed);

    bool ok = true;
    // Read the last `frames` samples from the ring buffer (broadcast, no
    // consumption). Multiple channels share the same capture.
    const int available = (totalWritten < frames) ? totalWritten : frames;
    const int startIdx = (writePos - available + capacity * 4) % capacity;
    const int offset = frames - available; // leading silence if not enough data
    // Missing physical inputs leave their dest channel silent (already
    // cleared). Only flip the flag here - error strings are built in open(),
    // never on the audio thread.
    for (int i = 0; i < numDest; ++i) {
        const int src = physicalChannels[(size_t)i];
        if (src < 0 || src >= inputChannelCount_) {
            ok = false;
            continue;
        }
        for (int s = 0; s < available; ++s) {
            const int idx = (startIdx + s) % capacity;
            const float v = storage.getSample(src, idx);
            dest.setSample(i, offset + s, v);
            // Mono (single physical channel): upmix to both L and R so that
            // panning works correctly (50/50 at center).
            if (numDest == 1) {
                dest.setSample(1, offset + s, v);
            }
        }
    }
    if (!ok)
        inputUnavailable_.store(true, std::memory_order_relaxed);

    // Round-trip latency measurement: detect the injected impulse on input.
    if (latencyMeasureActive.load(std::memory_order_relaxed) &&
        !latencyImpulseDetected.load(std::memory_order_relaxed)) {
        const int base = (totalWritten >= frames) ? (totalWritten - frames) : 0;
        for (int i = 0; i < numDest; ++i) {
            for (int s = 0; s < available; ++s) {
                if (std::abs(dest.getSample(i, offset + s)) > 0.9f) {
                    latencyImpulseInputSample.store((std::uint64_t)(base + s),
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
    juce::SpinLock::ScopedTryLockType tryLock(lock);
    if (!tryLock.isLocked()) {
        // Counted rather than silent: a held lock here means dropped input.
        inputDroppedFrames.fetch_add((std::uint64_t) numSamples, std::memory_order_relaxed);
        return;
    }
    if (capacity <= 0) return;

    // Write to ring buffer (broadcast to all consumers - no FIFO consumption).
    for (int ch = 0; ch < inputChannelCount_; ++ch) {
        int srcCh = (numInputChannels > ch && inputChannelData != nullptr && inputChannelData[ch] != nullptr) ? ch : -1;
        if (srcCh < 0)
            inputUnavailable_.store(true, std::memory_order_relaxed); // silence, never remap to input 0
        for (int i = 0; i < numSamples; ++i) {
            const float v = (srcCh >= 0) ? inputChannelData[srcCh][i] : 0.0f;
            storage.setSample(ch, (writePos + i) % capacity, v);
        }
    }
    writePos = (writePos + numSamples) % capacity;
    totalWritten += numSamples;
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
    }
}

void InputCapture::audioDeviceStopped() {
    // Without this the ring keeps its last window, replayable via pull()
    // forever; drop it under the writer lock so the next pull() is silence.
    const juce::SpinLock::ScopedLockType sl(lock);
    writePos = 0;
    totalWritten = 0;
}

// ---------- OutputPlayback ----------

OutputPlayback::OutputPlayback() = default;

OutputPlayback::~OutputPlayback() {
    close();
}

juce::String OutputPlayback::open(const juce::String& preferredDeviceName,
                                 double engineSampleRate,
                                 juce::String& errorOut,
                                 int requestedBufferSize) {
    close();
    error.clear();

    // Pick the lowest-latency device type that carries this device before
    // initialise (sharedLowLatency -> exclusive -> shared -> ASIO).
    activeDeviceType_ = openWithPreferredType(manager, preferredDeviceName, /*wantInput*/ false,
                                              0, 2,
                                              engineSampleRate, requestedBufferSize, errorOut);
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
    if (requestedBufferSize > 0 && bufferSize != requestedBufferSize) {
        // setAudioDeviceSetup() clamps to the nearest size from
        // getAvailableBufferSizes(); accept the rounded value and report it
        // informationally instead of failing the open.
        error = "info: device buffer is " + juce::String(bufferSize)
                + " samples (requested " + juce::String(requestedBufferSize) + ")";
    }
    deviceOutputLatency = dev->getOutputLatencyInSamples();
    // 8 device buffers (capped) so transient render stalls cannot starve a
    // device callback; never smaller than one device block.
    capacity = juce::jmax(bufferSize, juce::jmin(bufferSize * 8, 16384));
    fifo = std::make_unique<juce::AbstractFifo>(capacity);
    storage.setSize(2, capacity);
    storage.clear();
    // Prime with one silent block (storage is cleared above) so steady-state
    // occupancy starts above zero and a consumer that runs before the first
    // render does not count an underrun. Skipped when the cap left no more
    // than one block of room: a full FIFO would drop the first real push.
    if (capacity > bufferSize) {
        fifo->write(bufferSize);
    }

    manager.addAudioCallback(this);
    return {};
}

void OutputPlayback::close() {
    manager.removeAudioCallback(this);
    manager.closeAudioDevice();
    fifo.reset();
    storage.setSize(0, 0, false, true, false);
    sampleRate = 0.0;
    activeDeviceType_.clear();
    deviceOutputLatency = 0;
    latencySamples.store(0, std::memory_order_relaxed);
}

void OutputPlayback::push(const juce::AudioBuffer<float>& src, int frames) {
    if (frames <= 0 || fifo == nullptr) return;
    if (frames > capacity) frames = capacity;
    auto writeHandle = fifo->write(frames);
    const int b1 = writeHandle.blockSize1;
    const int b2 = writeHandle.blockSize2;
    const int total = b1 + b2;
    if (total < frames) {
        outputDroppedFrames.fetch_add((std::uint64_t)(frames - total),
                                      std::memory_order_relaxed);
    }
    const int srcChans = juce::jmax(1, src.getNumChannels());

    for (int ch = 0; ch < 2; ++ch) {
        const int srcCh = juce::jmin(ch, srcChans - 1);
        for (int i = 0; i < b1; ++i) {
            storage.setSample(ch, writeHandle.startIndex1 + i, src.getSample(srcCh, i));
        }
        for (int i = 0; i < b2; ++i) {
            storage.setSample(ch, writeHandle.startIndex2 + i, src.getSample(srcCh, b1 + i));
        }
    }
    // finishedWrite automatic
    // Structural delay only (see InputCapture::pull): device + one buffer.
    if (fifo != nullptr) {
        latencySamples.store(deviceOutputLatency + bufferSize,
                            std::memory_order_relaxed);
    }
}

void OutputPlayback::audioDeviceIOCallbackWithContext (const float* const*, int, float* const* outputChannelData, int numOutputChannels, int numSamples, const juce::AudioIODeviceCallbackContext&) {
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
    const int b1 = readHandle.blockSize1;
    const int b2 = readHandle.blockSize2;
    const int total = b1 + b2;
    if (total < numSamples) {
        outputUnderruns.fetch_add(1, std::memory_order_relaxed);
        outputDroppedFrames.fetch_add((std::uint64_t)(numSamples - total),
                                      std::memory_order_relaxed);
    }

    const int chans = juce::jmin(2, numOutputChannels);
    for (int ch = 0; ch < chans; ++ch) {
        if (outputChannelData[ch] == nullptr) continue;
        for (int i = 0; i < b1; ++i) {
            outputChannelData[ch][i] = storage.getSample(ch, readHandle.startIndex1 + i);
        }
        for (int i = 0; i < b2; ++i) {
            outputChannelData[ch][b1 + i] = storage.getSample(ch, readHandle.startIndex2 + i);
        }
    }
    currentOutputChannels = nullptr;
}

void OutputPlayback::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    if (device != nullptr) {
        sampleRate = device->getCurrentSampleRate();
        bufferSize = device->getCurrentBufferSizeSamples();
        if (bufferSize <= 0) bufferSize = 256;
        deviceOutputLatency = device->getOutputLatencyInSamples();
    }
}

void OutputPlayback::audioDeviceStopped() {}

