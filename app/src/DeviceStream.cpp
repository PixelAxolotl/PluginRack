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
}

namespace DeviceEnumeration {
namespace {
juce::StringArray enumerate(bool wantInputs) {
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
                               juce::String& errorOut) {
    close();
    error.clear();
    inputError.clear();

    const int numInputChannels = juce::jmax(2, numChannelsNeeded);
    auto err = manager.initialise(numInputChannels, 0, nullptr, true, preferredDeviceName, nullptr);
    if (err.isNotEmpty()) {
        error = err;
        errorOut = err;
        return err;
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

    capacity = juce::jmax(1, (int)std::round(sampleRate * 2.0));
    fifo = std::make_unique<juce::AbstractFifo>(capacity);
    storage.setSize(inputChannelCount_, capacity);
    storage.clear();

    manager.addAudioCallback(this);
    return {};
}

void InputCapture::close() {
    manager.removeAudioCallback(this);
    manager.closeAudioDevice();
    fifo.reset();
    storage.setSize(0, 0, false, true, false);
    sampleRate = 0.0;
    deviceInputLatency = 0;
    deviceBufferSize = 0;
    inputChannelCount_ = 0;
    availableInputCount_ = 0;
    inputUnavailable_.store(false, std::memory_order_relaxed);
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

    if (fifo == nullptr) {
        latencySamples.store(0, std::memory_order_relaxed);
        return false;
    }

    // Structural delay only: device latency + one device buffer.
    latencySamples.store(deviceInputLatency + deviceBufferSize,
                        std::memory_order_relaxed);

    bool ok = true;
    auto readHandle = fifo->read(frames);
    const int b1 = readHandle.blockSize1;
    const int b2 = readHandle.blockSize2;
    // Missing physical inputs leave their dest channel silent (already
    // cleared). Only flip the flag here — error strings are built in open(),
    // never on the audio thread.
    for (int i = 0; i < numDest; ++i) {
        const int src = physicalChannels[(size_t)i];
        if (src < 0 || src >= inputChannelCount_) {
            ok = false;
            continue;
        }
        for (int s = 0; s < b1; ++s) {
            dest.setSample(i, s, storage.getSample(src, readHandle.startIndex1 + s));
        }
        for (int s = 0; s < b2; ++s) {
            dest.setSample(i, b1 + s, storage.getSample(src, readHandle.startIndex2 + s));
        }
    }
    if (!ok)
        inputUnavailable_.store(true, std::memory_order_relaxed);
    // finishedRead automatic
    return ok;
}

void InputCapture::audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels, float* const*, int, int numSamples, const juce::AudioIODeviceCallbackContext&) {
    juce::SpinLock::ScopedTryLockType tryLock(lock);
    if (!tryLock.isLocked()) return; // drop
    if (fifo == nullptr) return;

    auto writeHandle = fifo->write(numSamples);
    const int b1 = writeHandle.blockSize1;
    const int b2 = writeHandle.blockSize2;
    if (b1 + b2 <= 0) return;

    for (int ch = 0; ch < inputChannelCount_; ++ch) {
        int srcCh = (numInputChannels > ch && inputChannelData != nullptr && inputChannelData[ch] != nullptr) ? ch : -1;
        if (srcCh < 0)
            inputUnavailable_.store(true, std::memory_order_relaxed); // silence, never remap to input 0
        for (int i = 0; i < b1; ++i) {
            const float v = (srcCh >= 0) ? inputChannelData[srcCh][i] : 0.0f;
            storage.setSample(ch, writeHandle.startIndex1 + i, v);
        }
        for (int i = 0; i < b2; ++i) {
            const float v = (srcCh >= 0) ? inputChannelData[srcCh][b1 + i] : 0.0f;
            storage.setSample(ch, writeHandle.startIndex2 + i, v);
        }
    }
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

void InputCapture::audioDeviceStopped() {}

// ---------- OutputPlayback ----------

OutputPlayback::OutputPlayback() = default;

OutputPlayback::~OutputPlayback() {
    close();
}

juce::String OutputPlayback::open(const juce::String& preferredDeviceName,
                                 double engineSampleRate,
                                 juce::String& errorOut) {
    close();
    error.clear();

    auto err = manager.initialise(0, 2, nullptr, true, preferredDeviceName, nullptr);
    if (err.isNotEmpty()) {
        error = err;
        errorOut = err;
        return err;
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
    capacity = juce::jmax(1, (int)std::round(sampleRate * 2.0));
    fifo = std::make_unique<juce::AbstractFifo>(capacity);
    storage.setSize(2, capacity);
    storage.clear();

    manager.addAudioCallback(this);
    return {};
}

void OutputPlayback::close() {
    manager.removeAudioCallback(this);
    manager.closeAudioDevice();
    fifo.reset();
    storage.setSize(0, 0, false, true, false);
    sampleRate = 0.0;
    deviceOutputLatency = 0;
    latencySamples.store(0, std::memory_order_relaxed);
}

void OutputPlayback::push(const juce::AudioBuffer<float>& src, int frames) {
    if (frames <= 0 || fifo == nullptr) return;
    if (frames > capacity) frames = capacity;
    auto writeHandle = fifo->write(frames);
    const int b1 = writeHandle.blockSize1;
    const int b2 = writeHandle.blockSize2;
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

