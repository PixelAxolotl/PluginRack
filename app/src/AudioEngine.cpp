#include "AudioEngine.h"

#include <algorithm>
#include <cmath>
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
// A detected impulse further away than this cannot be the loopback.
constexpr double kLatencyMeasureMaxSeconds = 5.0;
// Device probes can finish in either order. Reserve enough FIFO/ring history
// for a larger negotiated clock block so outputs and inputs can be opened in
// parallel without making startup wait for a second, serialized probe.
constexpr int kProbeRenderBlockFrames = 4096;
// Prefix of the error shown for an assigned device that is not enumerated.
const juce::String kNoSuchDevice = "No such device: ";

std::vector<pluginrack::ChainItem> defaultChain() {
    return {{pluginrack::ChainItemType::Eq, 0},
            {pluginrack::ChainItemType::Dynamics, 1},
            {pluginrack::ChainItemType::Dynamics, 2}};
}

// Compact label for the clock description: the registered device type names
// ("Windows Audio (Low Latency Mode)") are too long for the toolbar.
juce::String shortDeviceTypeTag(const juce::String& type) {
    if (type == "Windows Audio (Low Latency Mode)") return "LowLatency";
    if (type == "Windows Audio (Exclusive Mode)") return "Exclusive";
    if (type == "Windows Audio") return "Shared";
    return type;
}

// Scenes saved before the built-in dynamics rows existed can carry effects
// with no matching chain entry. Run those too, otherwise the panel and graph
// work while the audio passes through untouched.
void processEffectsMissingFromChain(
    const std::vector<pluginrack::ChainItem>& chain,
    const std::vector<pluginrack::DynamicsEffect>& effects,
    std::vector<pluginrack::DynamicsProcessor>& processors, float* left, float* right,
    int frames) noexcept {
    const std::size_t count = std::min(effects.size(), processors.size());
    for (std::size_t i = 0; i < count; ++i) {
        const pluginrack::ChainItem item{pluginrack::ChainItemType::Dynamics, effects[i].id};
        if (std::find(chain.begin(), chain.end(), item) == chain.end())
            processors[i].process(left, right, frames);
    }
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
}

AudioEngine::AudioEngine() {
    for (auto& slot : renderOutputs_) {
        slot.store(nullptr, std::memory_order_relaxed);
    }
    for (int i = 0; i < 3; ++i) addOutput();
    addChannel();
}

AudioEngine::~AudioEngine() {
    prepareAliveToken_->store(false, std::memory_order_release);
    stopDeviceOpenThread();
}

void AudioEngine::openDefaultDevices() {
    if (!outputs.empty()) {
        setOutputDevice(outputs.front()->id, "System Default");
    }
    if (!channels.empty()) {
        setChannelInputDevice(channels.front()->id, "System Default");
    }
}

void AudioEngine::openDefaultDevicesAsync() {
    {
        const juce::ScopedLock lock(stateLock);
        if (!outputs.empty()) outputs.front()->outputDeviceName = "System Default";
        if (!channels.empty()) channels.front()->inputDeviceName = "System Default";
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
    {
        const juce::ScopedLock lock(stateLock);
        maxBlockFrames_ = juce::jmax(1, maxBlockFrames);
        currentBufferSize = maxBlockFrames_;
        for (auto& channel : channels) {
            channel->buffer.setSize(2, maxBlockFrames_, false, true, true);
            channel->buffer.clear();
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
                hosts.push_back(&insert->pluginHost);
            }
            meters.observeChannel(channel->id, 0.0f);
        }
        for (auto& output : outputs) {
            output->buffer.setSize(2, maxBlockFrames_, false, true, true);
            output->buffer.clear();
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
                hosts.push_back(&insert->pluginHost);
            }
            meters.observeOutput(output->id, 0.0f);
        }
    }
    for (auto* host : hosts) {
        host->prepareToPlay(currentSampleRate, currentBufferSize);
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
        outputSampleCounter_ += (std::uint64_t)frames;
        return;
    }

    // Oversized block: silence it, count once, skip processing.
    if (maxBlockFrames_ > 0 && frames > maxBlockFrames_) {
        oversizedBlockCount_.fetch_add(1, std::memory_order_relaxed);
        pushSilenceToOutputsLocked(frames);
        outputSampleCounter_ += (std::uint64_t)frames;
        return;
    }

    const std::uint64_t epoch = ++renderEpoch_;

    // buffers are preallocated in prepareToPlay; clear used range only.
    for (auto& channel : channels) {
        channel->buffer.clear(0, frames);
        if (channel->input) {
            channel->input->pull(channel->buffer, frames, channel->inputChannels, epoch);
        }
    }
    for (auto& output : outputs) {
        output->buffer.clear(0, frames);
    }

    meters.advance(frames, currentSampleRate);
    bool anySolo = false;
    for (const auto& channel : channels) {
        anySolo = anySolo || channel->soloed.get();
    }

    for (auto& channel : channels) {
        juce::AudioBuffer<float> block(channel->buffer.getArrayOfWritePointers(), 2, 0, frames);
        // EQ-before-inserts (pre-fader), matching core's channel tap order.
        // stateLock (held since render entry) serializes this against the EQ
        // setters, mirroring core's EqState mutex; bypassed strips skip
        // process() entirely so output stays bit-identical to the pre-EQ
        // engine. No allocation: the EQ runs through the block view.
        // Analyzer tap wraps the call so Pre and Post exist even when the
        // EQ is bypassed (both rings then carry the identical input block).
        const float trim = decibelsToGain(channel->trimDb.get());
        channel->buffer.applyGain(0, 0, frames, trim);
        channel->buffer.applyGain(1, 0, frames, trim);
        channel->analyzerTap.writeTap(false, block.getReadPointer(0), block.getReadPointer(1), frames);
        for (const auto& item : channel->chain) {
            if (item.type == pluginrack::ChainItemType::Eq) {
                if (!channel->eqBypassed.get())
                    channel->eq.process(block.getWritePointer(0), block.getWritePointer(1), frames);
            } else if (item.type == pluginrack::ChainItemType::Dynamics) {
                auto effect = std::find_if(channel->effects.begin(), channel->effects.end(),
                                           [id = item.id](const auto& value) { return value.id == id; });
                if (effect != channel->effects.end()) {
                    const auto index = static_cast<std::size_t>(effect - channel->effects.begin());
                    if (index < channel->effectProcessors.size())
                        channel->effectProcessors[index].process(
                            block.getWritePointer(0), block.getWritePointer(1), frames);
                }
            } else {
                auto insert = std::find_if(channel->inserts.begin(), channel->inserts.end(),
                                           [id = item.id](const auto& value) { return value->id == id; });
                if (insert != channel->inserts.end())
                    (*insert)->pluginHost.processBlock(block, (*insert)->bypassed.get());
            }
        }
        // Older scenes can have effects without their dynamics chain entries.
        // Keep those effects live instead of showing a working but inaudible UI.
        processEffectsMissingFromChain(channel->chain, channel->effects,
                                       channel->effectProcessors,
                                       block.getWritePointer(0), block.getWritePointer(1), frames);
        channel->analyzerTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), frames);

        const bool silent = channel->muted.get() || (anySolo && !channel->soloed.get());
        const float gain = silent ? 0.0f : decibelsToGain(channel->gainDb.get());
        const float theta = (channel->pan.get() + 1.0f) * juce::MathConstants<float>::pi / 4.0f;
        channel->buffer.applyGain(0, 0, frames, gain * std::cos(theta));
        channel->buffer.applyGain(1, 0, frames, gain * std::sin(theta));

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
        juce::AudioBuffer<float> block(output->buffer.getArrayOfWritePointers(), 2, 0, frames);
        // Output EQ before the output inserts (pre-master-write), mirroring
        // core's output tap and the channel tap above. Same lock rules; the
        // analyzer tap wraps the call exactly like the channel loop does.
        output->analyzerTap.writeTap(false, block.getReadPointer(0), block.getReadPointer(1), frames);
        for (const auto& item : output->chain) {
            if (item.type == pluginrack::ChainItemType::Eq) {
                if (!output->eqBypassed.get())
                    output->eq.process(block.getWritePointer(0), block.getWritePointer(1), frames);
            } else if (item.type == pluginrack::ChainItemType::Dynamics) {
                auto effect = std::find_if(output->effects.begin(), output->effects.end(),
                                           [id = item.id](const auto& value) { return value.id == id; });
                if (effect != output->effects.end()) {
                    const auto index = static_cast<std::size_t>(effect - output->effects.begin());
                    if (index < output->effectProcessors.size())
                        output->effectProcessors[index].process(
                            block.getWritePointer(0), block.getWritePointer(1), frames);
                }
            } else {
                auto insert = std::find_if(output->inserts.begin(), output->inserts.end(),
                                           [id = item.id](const auto& value) { return value->id == id; });
                if (insert != output->inserts.end())
                    (*insert)->pluginHost.processBlock(block, (*insert)->bypassed.get());
            }
        }
        // Older scenes can have effects without their dynamics chain entries.
        processEffectsMissingFromChain(output->chain, output->effects,
                                       output->effectProcessors,
                                       block.getWritePointer(0), block.getWritePointer(1), frames);
        output->analyzerTap.writeTap(true, block.getReadPointer(0), block.getReadPointer(1), frames);
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

    outputSampleCounter_ += (std::uint64_t)frames;
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

void AudioEngine::removeChannel(std::uint32_t channelId) {
    // The startup opener holds Channel* across a device probe; refuse the
    // structural change until it is done instead of invalidating it.
    if (isOpeningDevices()) return;
    const juce::ScopedLock lock(stateLock);
    for (auto it = channels.begin(); it != channels.end();) {
        if ((*it)->id == channelId) {
            (*it)->input.reset();
            it = channels.erase(it);
        } else {
            ++it;
        }
    }
    routes.erase(std::remove_if(routes.begin(), routes.end(),
                               [channelId](const auto& route) { return route->channelId == channelId; }),
                  routes.end());
    pruneUnusedCaptures();
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
    for (const auto& output : outputs) {
        auto route = std::make_unique<Route>();
        route->channelId = channel->id;
        route->outputId = output->id;
        const float g = output == outputs.front() ? 0.0f : -60.0f;
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
    for (const auto& channel : channels) {
        auto route = std::make_unique<Route>();
        route->channelId = channel->id;
        route->outputId = output->id;
        route->sendGainDbL = -60.0f;
        route->sendGainDbR = -60.0f;
        routes.push_back(std::move(route));
    }
    outputs.push_back(std::move(output));
}

juce::String AudioEngine::removeOutput(std::uint32_t outputId) {
    if (isOpeningDevices()) {
        return "Audio devices are still starting up. Try again in a moment.";
    }
    // Find the output.
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
                                 std::function<void(juce::String)> done) {
    {
        const juce::ScopedLock lock(stateLock);
        auto* channel = findChannel(channelId);
        if (channel == nullptr) {
            done("The channel no longer exists.");
            return;
        }
        if (channel->inserts.size() >= 5) {
            done("Each strip holds 3 built-in effects and at most 5 VST inserts.");
            return;
        }
    }

    // Instantiation is a blocking driver call that can take seconds: build the
    // instance on a worker and only touch the strip back here on the message
    // thread. The window stays responsive meanwhile.
    const double rate = currentSampleRate;
    const int block = currentBufferSize;
    auto alive = prepareAliveToken_;
    juce::Logger::writeToLog("plugin: loading " + file.getFileName());
    juce::Thread::launch([this, alive, channelId, file, rate, block,
                          done = std::move(done)]() mutable {
        const auto loaded = PluginHost::loadFromFile(file, rate, block);
        juce::MessageManager::callAsync([this, alive, channelId, file, loaded,
                                         done = std::move(done)]() mutable {
            if (!alive->load(std::memory_order_acquire)) return;
            if (loaded.error.isNotEmpty()) {
                juce::Logger::writeToLog("plugin: failed - " + loaded.error);
                done(loaded.error);
                return;
            }

            auto insert = std::make_unique<InsertSlot>();
            insert->pluginHost.prepareToPlay(currentSampleRate, currentBufferSize);
            insert->pluginHost.adopt(loaded);
            insert->pluginPath = loaded.path;
            insert->pluginName = loaded.name;
            insert->latencySamples = insert->pluginHost.getLatencySamples();

            juce::String error;
            {
                const juce::ScopedLock lock(stateLock);
                auto* channel = findChannel(channelId);
                if (channel == nullptr) {
                    error = "The channel no longer exists.";
                } else if (channel->inserts.size() >= 5) {
                    error = "Each strip holds 3 built-in effects and at most 5 VST inserts.";
                } else {
                    insert->id = nextInsertId++;
                    const auto insertId = insert->id;
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

void AudioEngine::moveChannelChainItem(std::uint32_t channelId, pluginrack::ChainItem item,
                                       int targetIndex) {
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) return;
    auto& chain = channel->chain;
    const auto it = std::find(chain.begin(), chain.end(), item);
    if (it == chain.end()) return;
    const auto value = *it;
    chain.erase(it);
    const auto index = juce::jlimit(0, static_cast<int>(chain.size()), targetIndex);
    chain.insert(chain.begin() + index, value);
}

void AudioEngine::setPluginBypassed(std::uint32_t channelId, std::uint32_t insertId, bool bypassed) {
    if (auto* insert = findInsert(channelId, insertId)) {
        insert->bypassed = bypassed;
        insert->pluginHost.refreshLatencyCache();
        insert->latencySamples = insert->pluginHost.getLatencySamples();
    }
}

void AudioEngine::addOutputPluginAsync(std::uint32_t outputId, const juce::File& file,
                                       std::function<void(juce::String)> done) {
    {
        const juce::ScopedLock lock(stateLock);
        auto* output = findOutput(outputId);
        if (output == nullptr) {
            done("The output no longer exists.");
            return;
        }
        if (output->inserts.size() >= 5) {
            done("Each strip holds 3 built-in effects and at most 5 VST inserts.");
            return;
        }
    }

    const double rate = currentSampleRate;
    const int block = currentBufferSize;
    auto alive = prepareAliveToken_;
    juce::Logger::writeToLog("plugin: loading " + file.getFileName());
    juce::Thread::launch([this, alive, outputId, file, rate, block,
                          done = std::move(done)]() mutable {
        const auto loaded = PluginHost::loadFromFile(file, rate, block);
        juce::MessageManager::callAsync([this, alive, outputId, file, loaded,
                                         done = std::move(done)]() mutable {
            if (!alive->load(std::memory_order_acquire)) return;
            if (loaded.error.isNotEmpty()) {
                juce::Logger::writeToLog("plugin: failed - " + loaded.error);
                done(loaded.error);
                return;
            }

            auto insert = std::make_unique<InsertSlot>();
            insert->pluginHost.prepareToPlay(currentSampleRate, currentBufferSize);
            insert->pluginHost.adopt(loaded);
            insert->pluginPath = loaded.path;
            insert->pluginName = loaded.name;
            insert->latencySamples = insert->pluginHost.getLatencySamples();

            juce::String error;
            {
                const juce::ScopedLock lock(stateLock);
                auto* output = findOutput(outputId);
                if (output == nullptr) {
                    error = "The output no longer exists.";
                } else if (output->inserts.size() >= 5) {
                    error = "Each strip holds 3 built-in effects and at most 5 VST inserts.";
                } else {
                    insert->id = nextInsertId++;
                    const auto insertId = insert->id;
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
    const auto it = std::find(chain.begin(), chain.end(), item);
    if (it == chain.end()) return;
    const auto value = *it;
    chain.erase(it);
    const auto index = juce::jlimit(0, static_cast<int>(chain.size()), targetIndex);
    chain.insert(chain.begin() + index, value);
}

void AudioEngine::setOutputPluginBypassed(std::uint32_t outputId, std::uint32_t insertId, bool bypassed) {
    if (auto* insert = findOutputInsert(outputId, insertId)) {
        insert->bypassed = bypassed;
        insert->pluginHost.refreshLatencyCache();
        insert->latencySamples = insert->pluginHost.getLatencySamples();
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
    for (auto& channel : channels) {
        if (channel->id == channelId) { channel->name = name; return; }
    }
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
    if (deviceName.isNotEmpty() && deviceName != "None - silent") {
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
    if (deviceName.isNotEmpty() && deviceName != "None - silent") {
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
    if (deviceName.isNotEmpty() && deviceName != "None - silent") {
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
    for (auto& output : outputs) {
        if (output->id == outputId) { output->gainDb = gainDb; return; }
    }
}

void AudioEngine::setOutputMuted(std::uint32_t outputId, bool muted) {
    for (auto& output : outputs) {
        if (output->id == outputId) { output->muted = muted; return; }
    }
}

void AudioEngine::setOutputName(std::uint32_t outputId, const juce::String& name) {
    const juce::ScopedLock lock(stateLock);
    for (auto& output : outputs) {
        if (output->id == outputId) { output->name = name; return; }
    }
}

void AudioEngine::setOutputLayout(std::uint32_t outputId, pluginrack::ChannelLayout layout) {
    const juce::ScopedLock lock(stateLock);
    for (auto& output : outputs) {
        if (output->id == outputId) { output->layout = layout; return; }
    }
}

void AudioEngine::setOutputFlipped(std::uint32_t outputId, bool flipped) {
    for (auto& output : outputs) {
        if (output->id == outputId) { output->flipped = flipped; return; }
    }
}

void AudioEngine::setChannelEq(std::uint32_t channelId, const std::vector<pluginrack::EqBand>& bands) {
    // Message thread: stateLock serializes the band copy and the coefficient
    // rebuild against render()'s taps (mirrors core's EqState mutex). Bypass
    // state is kept, like core's setChannelEq.
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) {
        channel->eqBands = bands;
        channel->eqDirty = true;
        channel->eq.setBands(channel->eqBands);
        channel->eqDirty = false;
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

void AudioEngine::setOutputEq(int outIdx, const std::vector<pluginrack::EqBand>& bands) {
    const juce::ScopedLock lock(stateLock);
    // Out-of-range indexes are ignored, matching core's setOutputEq.
    if (outIdx < 0 || static_cast<std::size_t>(outIdx) >= outputs.size()) {
        return;
    }
    auto* output = outputs[static_cast<std::size_t>(outIdx)].get();
    output->eqBands = bands;
    output->eqDirty = true;
    output->eq.setBands(output->eqBands);
    output->eqDirty = false;
}

void AudioEngine::setOutputEqBypassed(int outIdx, bool bypassed) {
    const juce::ScopedLock lock(stateLock);
    if (outIdx < 0 || static_cast<std::size_t>(outIdx) >= outputs.size()) {
        return;
    }

    auto* output = outputs[static_cast<std::size_t>(outIdx)].get();
    output->eqBypassed = bypassed;
    if (bypassed) {
        output->eq.reset();
    }
}

void AudioEngine::setOutputEqDelay(int outIdx, float delayMs) {
    const juce::ScopedLock lock(stateLock);
    if (outIdx < 0 || static_cast<std::size_t>(outIdx) >= outputs.size()) {
        return;
    }
    auto* output = outputs[static_cast<std::size_t>(outIdx)].get();
    output->eqDelayMs = delayMs;
    output->eq.setDelayMs(delayMs);
}

void AudioEngine::setOutputEqRtaOffset(int outIdx, float offsetDb) {
    const juce::ScopedLock lock(stateLock);
    if (outIdx < 0 || static_cast<std::size_t>(outIdx) >= outputs.size()) {
        return;
    }
    outputs[static_cast<std::size_t>(outIdx)]->eqRtaOffsetDb = offsetDb;
}

void AudioEngine::setOutputEqRtaWeighting(int outIdx, int weighting) {
    const juce::ScopedLock lock(stateLock);
    if (outIdx < 0 || static_cast<std::size_t>(outIdx) >= outputs.size()) {
        return;
    }
    outputs[static_cast<std::size_t>(outIdx)]->eqRtaWeighting = juce::jlimit(0, 2, weighting);
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

float AudioEngine::getSendGain(std::uint32_t channelId, std::uint32_t outputId) const {
    for (const auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            return route->sendGainDbL.get();
        }
    }
    return -60.0f;
}

float AudioEngine::getSendGainL(std::uint32_t channelId, std::uint32_t outputId) const {
    for (const auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            return route->sendGainDbL.get();
        }
    }
    return -60.0f;
}

float AudioEngine::getSendGainR(std::uint32_t channelId, std::uint32_t outputId) const {
    for (const auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            return route->sendGainDbR.get();
        }
    }
    return -60.0f;
}

const std::vector<std::unique_ptr<AudioEngine::Channel>>& AudioEngine::getChannels() const {
    return channels;
}

const std::vector<std::unique_ptr<AudioEngine::Output>>& AudioEngine::getOutputs() const {
    return outputs;
}

const std::vector<std::unique_ptr<AudioEngine::Route>>& AudioEngine::getRoutes() const {
    return routes;
}

pluginrack::Scene AudioEngine::makeScene() const {
    const juce::ScopedLock lock(stateLock);
    pluginrack::Scene scene;
    scene.name = "Default";
    scene.sampleRate = currentSampleRate;
    scene.bufferFrames = currentBufferSize;
    for (const auto& channel : channels) {
        pluginrack::Channel savedChannel;
        savedChannel.id = channel->id;
        savedChannel.name = channel->name.toStdString();
        savedChannel.sourceKind = channel->sourceKind;
        savedChannel.sourceDeviceId = channel->sourceDeviceId;
        savedChannel.layout = channel->layout;
        savedChannel.inputChannels = channel->inputChannels;
        savedChannel.gainDb = channel->gainDb.get();
        savedChannel.trimDb = channel->trimDb.get();
        savedChannel.pan = channel->pan.get();
        savedChannel.muted = channel->muted.get();
        savedChannel.soloed = channel->soloed.get();
        savedChannel.eqEnabled = !channel->eqBypassed.get();
        savedChannel.eqBands = channel->eqBands;
        savedChannel.eqDelayMs = channel->eqDelayMs;
        savedChannel.eqRtaOffsetDb = channel->eqRtaOffsetDb;
        savedChannel.eqRtaWeighting = channel->eqRtaWeighting;
        savedChannel.effects = channel->effects;
        savedChannel.chain = channel->chain;
        for (const auto& insert : channel->inserts) {
            pluginrack::InsertSlot savedInsert;
            savedInsert.id = insert->id;
            savedInsert.pluginPath = insert->pluginPath.toStdString();
            savedInsert.pluginName = insert->pluginName.toStdString();
            savedInsert.bypassed = insert->bypassed.get();
            // Capture the live parameter/program state so reopening the scene
            // does not reset every insert to its defaults.
            savedInsert.pluginState = insert->pluginHost.getStateAsBase64().toStdString();
            savedChannel.inserts.push_back(std::move(savedInsert));
        }
        scene.channels.push_back(std::move(savedChannel));
    }
    for (const auto& output : outputs) {
        pluginrack::Output savedOutput;
        savedOutput.id = output->id;
        savedOutput.name = output->name.toStdString();
        savedOutput.layout = output->layout;
        savedOutput.flipped = output->flipped.get();
        savedOutput.gainDb = output->gainDb.get();
        savedOutput.muted = output->muted.get();
        savedOutput.deviceId = output->outputDeviceName.toStdString();
        savedOutput.eqEnabled = !output->eqBypassed.get();
        savedOutput.eqBands = output->eqBands;
        savedOutput.eqDelayMs = output->eqDelayMs;
        savedOutput.eqRtaOffsetDb = output->eqRtaOffsetDb;
        savedOutput.eqRtaWeighting = output->eqRtaWeighting;
        savedOutput.effects = output->effects;
        savedOutput.chain = output->chain;
        for (const auto& insert : output->inserts) {
            pluginrack::InsertSlot savedInsert;
            savedInsert.id = insert->id;
            savedInsert.pluginPath = insert->pluginPath.toStdString();
            savedInsert.pluginName = insert->pluginName.toStdString();
            savedInsert.bypassed = insert->bypassed.get();
            savedInsert.pluginState = insert->pluginHost.getStateAsBase64().toStdString();
            savedOutput.inserts.push_back(std::move(savedInsert));
        }
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

    // The tuning the session runs with travels with the save, so loading it
    // restores the same buffers, stream modes and device watchdog.
    const auto settings = getDeviceSettings();
    pluginrack::AudioSettings audio;
    audio.inputBuffer = settings.inputBuffer;
    audio.outputBuffer = settings.outputBuffer;
    audio.latencyTuningOptIn = settings.latencyTuningOptIn;
    audio.lowLatencyInput = settings.lowLatencyInput;
    audio.preferExclusive = settings.preferExclusive;
    audio.lowLatencyOutputs = settings.lowLatencyOutputs;
    audio.minimalClockFifo = settings.minimalClockFifo;
    audio.fullDuplexInput = settings.fullDuplexInput;
    audio.autoReloadDevices = settings.autoReloadDevices;
    audio.rackThumbnailFps = settings.rackThumbnailFps;
    audio.watchedDevices = getWatchedDevices();
    scene.audio = std::move(audio);
    return scene;
}

juce::String AudioEngine::restoreScene(const pluginrack::Scene& scene, bool openDevicesNow) {
    if (isOpeningDevices()) {
        return "Audio devices are still starting up. Please try again in a moment.";
    }
    // Tuning first: the stream open below must already use the saved buffer
    // sizes and modes. Scenes written before schema 4 carry no tuning block,
    // and loading one leaves the live settings alone.
    if (scene.audio.has_value()) {
        DeviceSettings settings = getDeviceSettings();
        settings.inputBuffer = scene.audio->inputBuffer;
        settings.outputBuffer = scene.audio->outputBuffer;
        settings.latencyTuningOptIn = scene.audio->latencyTuningOptIn;
        settings.lowLatencyInput = scene.audio->lowLatencyInput;
        settings.preferExclusive = scene.audio->preferExclusive;
        settings.lowLatencyOutputs = scene.audio->lowLatencyOutputs;
        settings.minimalClockFifo = scene.audio->minimalClockFifo;
        settings.fullDuplexInput = scene.audio->fullDuplexInput;
        settings.autoReloadDevices = scene.audio->autoReloadDevices;
        settings.rackThumbnailFps = scene.audio->rackThumbnailFps;
        applySettingsValues(settings);
        setWatchedDevices(scene.audio->watchedDevices);
    }
    double sampleRate;
    int bufferSize;
    std::map<std::uint32_t, juce::String> savedInputDevices;
    std::map<std::uint32_t, juce::String> savedOutputDevices;
    {
        const juce::ScopedLock lock(stateLock);
        sampleRate = currentSampleRate;
        bufferSize = currentBufferSize;
        for (const auto& c : channels) savedInputDevices[c->id] = c->inputDeviceName;
        for (const auto& o : outputs) savedOutputDevices[o->id] = o->outputDeviceName;
    }

    std::vector<std::unique_ptr<Channel>> restoredChannels;
    std::vector<std::unique_ptr<Output>> restoredOutputs;
    std::vector<std::unique_ptr<Route>> restoredRoutes;
    pluginrack::MeterBank restoredMeters;
    juce::String loadErrors;
    // Highest ids seen while building: applied to the next-id counters under
    // stateLock together with the vector swap below, so the counters and
    // the strips they number advance as one committed step.
    std::uint32_t maxInsertId = 0;
    std::uint32_t maxChannelId = 0;
    std::uint32_t maxOutputId = 0;

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
        // EQ bands are scene state; rebuild coefficients here on the message
        // thread before the strip goes live (never in render). The delay line
        // is sized here for the same reason.
        channel->eqBands = savedChannel.eqBands;
        channel->eqDelayMs = savedChannel.eqDelayMs;
        channel->eqRtaOffsetDb = savedChannel.eqRtaOffsetDb;
        channel->eqRtaWeighting = savedChannel.eqRtaWeighting;
        channel->eqBypassed = !savedChannel.eqEnabled;
        channel->eq.prepare(sampleRate, bufferSize);
        channel->eqDirty = true;
        channel->eq.setBands(channel->eqBands);
        channel->eqDirty = false;
        channel->eq.setDelayMs(channel->eqDelayMs);
        channel->effects = savedChannel.effects;
        channel->chain = savedChannel.chain.empty() ? defaultChain() : savedChannel.chain;
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
            insert->bypassed = savedInsert.bypassed;
            insert->pluginHost.prepareToPlay(sampleRate, bufferSize);
            // Scene restore still instantiates here, on the message thread:
            // log it so a slow plugin is visible in the run log.
            juce::Logger::writeToLog("scene: instantiating "
                                     + juce::File(insert->pluginPath).getFileName());
            const auto error = insert->pluginHost.loadPlugin(juce::File(insert->pluginPath),
                                                             juce::String(savedInsert.pluginState));
            if (error.isNotEmpty()) {
                loadErrors += channel->name + ": " + error + "\n";
            }
            insert->latencySamples = insert->pluginHost.getLatencySamples();
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
        restoredMeters.observeChannel(channel->id, 0.0f);
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
        output->eqBands = savedOutput.eqBands;
        output->eqDelayMs = savedOutput.eqDelayMs;
        output->eqRtaOffsetDb = savedOutput.eqRtaOffsetDb;
        output->eqRtaWeighting = savedOutput.eqRtaWeighting;
        output->eqBypassed = !savedOutput.eqEnabled;
        output->eq.prepare(sampleRate, bufferSize);
        output->eqDirty = true;
        output->eq.setBands(output->eqBands);
        output->eqDirty = false;
        output->eq.setDelayMs(output->eqDelayMs);
        output->effects = savedOutput.effects;
        output->chain = savedOutput.chain.empty() ? defaultChain() : savedOutput.chain;
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
            insert->bypassed = savedInsert.bypassed;
            insert->pluginHost.prepareToPlay(sampleRate, bufferSize);
            juce::Logger::writeToLog("scene: instantiating "
                                     + juce::File(insert->pluginPath).getFileName());
            const auto error = insert->pluginHost.loadPlugin(juce::File(insert->pluginPath),
                                                             juce::String(savedInsert.pluginState));
            if (error.isNotEmpty()) {
                loadErrors += output->name + ": " + error + "\n";
            }
            insert->latencySamples = insert->pluginHost.getLatencySamples();
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
        restoredMeters.observeOutput(output->id, 0.0f);
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
    std::vector<std::unique_ptr<Route>> oldRoutes;
    std::map<juce::String, SharedInputCapture> retiredCaptures;
    std::vector<OutputPlayback*> oldPlaybacks;
    {
        const juce::ScopedLock lock(stateLock);
        oldChannels = std::move(channels);
        oldOutputs = std::move(outputs);
        oldRoutes = std::move(routes);
        channels = std::move(restoredChannels);
        outputs = std::move(restoredOutputs);
        routes = std::move(restoredRoutes);
        meters = std::move(restoredMeters);
        // Keep the old playbacks alive for the lock-free silence snapshot,
        // but close their devices once the lock is released.
        for (auto& output : oldOutputs) {
            if (output->output != nullptr) {
                oldPlaybacks.push_back(output->output.get());
                retiredOutputs_.push_back(std::move(output->output));
            }
        }
        publishRenderOutputsLocked();
        nextInsertId = juce::jmax(nextInsertId, maxInsertId);
        nextChannelId = juce::jmax(nextChannelId, maxChannelId);
        nextOutputId = juce::jmax(nextOutputId, maxOutputId);
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
    oldOutputs.clear();
    oldChannels.clear();
    retiredCaptures.clear();
    for (auto* playback : oldPlaybacks) {
        playback->close();
    }

    // Re-apply device assignments where possible: the device stored in the
    // scene wins; channels from scenes saved before sourceDeviceId was
    // persisted keep whatever device was live for the same id.
    std::map<std::uint32_t, juce::String> desiredInputDevices;
    for (const auto& savedChannel : scene.channels) {
        const juce::String sceneDevice(savedChannel.sourceDeviceId);
        if (sceneDevice.isNotEmpty()) {
            desiredInputDevices[savedChannel.id] = sceneDevice;
            continue;
        }
        const auto live = savedInputDevices.find(savedChannel.id);
        if (live != savedInputDevices.end() && live->second.isNotEmpty()) {
            desiredInputDevices[savedChannel.id] = live->second;
        }
    }
    std::map<std::uint32_t, juce::String> desiredOutputDevices;
    for (const auto& savedOutput : scene.outputs) {
        const juce::String sceneDevice(savedOutput.deviceId);
        if (sceneDevice.isNotEmpty()) {
            desiredOutputDevices[savedOutput.id] = sceneDevice;
            continue;
        }
        const auto live = savedOutputDevices.find(savedOutput.id);
        if (live != savedOutputDevices.end() && live->second.isNotEmpty()) {
            desiredOutputDevices[savedOutput.id] = live->second;
        }
    }
    // Record the assignments only. Opening them inline would run one blocking
    // driver probe per endpoint on the message thread, which is what made
    // loading a scene look like a freeze; the background opener below (or the
    // caller at startup) opens them in parallel and reports any refusal.
    {
        const juce::ScopedLock lock(stateLock);
        clockOutputId = 0;
        for (const auto& [id, name] : desiredInputDevices) {
            if (auto* channel = findChannel(id)) {
                channel->inputDeviceName = name;
                channel->sourceDeviceId = name == "None - silent" ? std::string{}
                                                                 : name.toStdString();
                channel->inputError.clear();
            }
        }
        for (const auto& [id, name] : desiredOutputDevices) {
            if (auto* output = findOutput(id)) {
                // setOutputDevice treats the silent choice as "no device".
                output->outputDeviceName = name == "None - silent" ? juce::String{} : name;
                output->outputError.clear();
            }
        }
    }

    if (openDevicesNow)
        openStripsDevicesAsync();

    return loadErrors;
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

    if (deviceName == "None - silent" || deviceName.isEmpty()) {
        const juce::ScopedLock sl(stateLock);
        channel->input.reset();
        channel->inputDeviceName = "None - silent";
        channel->sourceDeviceId.clear();
        channel->inputError.clear();
        pruneUnusedCaptures();
        return {};
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
    const auto openErr = capture->open(deviceName == "System Default" ? juce::String() : deviceName,
                                       requestedRate, needed, err, requestedInputBufferSize_,
                                       renderBlockFrames,
                                       useLowLatencyInput ? 1 : 2,
                                       inputPreferExclusive,
                                       capacityReserveFrames);
    if (openErr.isNotEmpty()) {
        if (!forceReopen) {
            const juce::ScopedLock sl(stateLock);
            channel->inputError = openErr;
        }
        return openErr;
    }

    std::shared_ptr<InputCapture> retired;
    {
        const juce::ScopedLock sl(stateLock);
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
    }
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
                && channel->inputDeviceName != "None - silent") {
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
    const auto error = setOutputDeviceImpl(outputId, deviceName, currentSampleRate);
    // The previous figure described the previous output chain.
    if (error.isEmpty()) resetLatencyMeasurement();
    return error;
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

    if (deviceName == "None - silent" || deviceName.isEmpty()) {
        bool wasClock = false;
        OutputPlayback* retiredPlayback = nullptr;
        {
            const juce::ScopedLock sl(stateLock);
            wasClock = (clockOutputId == outputId);
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
    const auto openName = deviceName == "System Default" ? juce::String() : deviceName;
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
    const bool preferExclusive = preferExclusive_.load(std::memory_order_relaxed)
                              && isClock && !sharesClockInput;
    auto openErr = newPlayback->open(openName, engineRate, err, requestedOutputBufferSize_,
                                     targetBlocks, primeBlocks, preferExclusive,
                                     renderBlockFrames);
    if (openErr.isNotEmpty()) {
        const juce::ScopedLock sl(stateLock);
        out->output = std::move(prevOutput);
        if (out->output != nullptr) out->output->setRenderActive(true);
        // Nothing is playing: keep the name the user assigned so the strip and
        // the saved scene still say which device is missing, instead of
        // dropping back to "Choose device" with only an error line.
        if (out->output == nullptr) out->outputDeviceName = deviceName;
        // Some drivers refuse a second client for an endpoint, so reopening an
        // already-open output fails while the old stream keeps playing. Say
        // that instead of reporting the strip as dead.
        auto message = openErr;
        if (out->output != nullptr)
            message += " (the previous stream is still running)";
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
        }
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
    } else {
        // ensure clock id remains; if no clock, maybe set? handled by isClock true when none
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
    if (inputError.isEmpty() && outputError.isEmpty()) requestedBufferSize_ = size;
    return inputError.isNotEmpty() ? inputError : outputError;
}

juce::String AudioEngine::setInputBufferSize(int size) {
    if (isOpeningDevices()) return "Audio devices are still starting up. Try again in a moment.";
    const int previous = requestedInputBufferSize_;
    requestedInputBufferSize_ = size;
    juce::String firstError;
    for (auto& channel : channels) {
        if (channel->inputDeviceName.isNotEmpty() &&
            channel->inputDeviceName != "None - silent") {
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
                channel->inputDeviceName != "None - silent") {
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
            output->outputDeviceName != "None - silent") {
            const auto e = setOutputDevice(output->id, output->outputDeviceName);
            if (firstError.isEmpty() && e.isNotEmpty()) firstError = e;
        }
    }
    if (firstError.isNotEmpty() && previous != size) {
        requestedOutputBufferSize_ = previous;
        for (auto& output : outputs) {
            if (output->outputDeviceName.isNotEmpty() &&
                output->outputDeviceName != "None - silent") {
                setOutputDevice(output->id, output->outputDeviceName);
            }
        }
        return "The output device refused the new buffer size; kept the previous one.\n"
             + firstError;
    }
    return firstError;
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
            const auto device = o->outputDeviceName.isEmpty() ? juce::String("System Default")
                                                              : o->outputDeviceName;
            const auto khzValue = currentSampleRate / 1000.0;
            const auto khz = std::abs(khzValue - std::round(khzValue)) < 0.001
                                 ? juce::String((int)std::round(khzValue))
                                 : juce::String(khzValue, 1);
            auto desc = o->name + "  -  " + device + "  -  " + khz + " kHz  -  "
                      + juce::String(currentBufferSize) + " smp";
            if (o->outputDeviceType.isNotEmpty())
                desc += " [" + shortDeviceTypeTag(o->outputDeviceType) + "]";
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

bool AudioEngine::refreshDeviceAvailability() {
    // Enumerate before taking stateLock: the scan creates a device manager and
    // must never run with the audio lock held.
    const auto outputsPresent = getAvailableOutputDevices();
    const auto inputsPresent = getAvailableInputDevices();
    // An empty list means the enumeration itself failed; do not report every
    // strip as missing in that case.
    const auto markMissing = [](juce::String& error, const juce::String& name,
                                const juce::StringArray& present, bool listUsable) {
        if (!listUsable || name.isEmpty() || name == "None - silent" || name == "System Default")
            return false;
        if (error.isNotEmpty()) return false; // a real open error already explains it
        if (present.indexOf(name, true) >= 0) return false;
        error = kNoSuchDevice + name;
        return true;
    };

    bool changed = false;
    const juce::ScopedLock lock(stateLock);
    for (auto& output : outputs) {
        changed |= markMissing(output->outputError, output->outputDeviceName,
                               outputsPresent, !outputsPresent.isEmpty());
    }
    for (auto& channel : channels) {
        changed |= markMissing(channel->inputError, channel->inputDeviceName,
                               inputsPresent, !inputsPresent.isEmpty());
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
    if (inputChannel != nullptr && inputChannel->input != nullptr) {
        inputDeviceSamples = inputChannel->input->getLatencySamples();
        inputBufferSize = inputChannel->input->getDeviceBufferSize();
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
    bool valid = false;
    if (clockOutputId != 0) {
        for (const auto& output : outputs) {
            if (output->id == clockOutputId && output->output != nullptr) {
                outputDeviceSamples = output->output->getLatencySamples();
                outputBufferSize = output->output->getBufferSize();
                if (outputBufferSize <= 0) outputBufferSize = currentBufferSize;
                outputFillFrames = output->output->getFillFrames();
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

bool AudioEngine::openFullDuplexIfPossible(const juce::String& inputDevice, const juce::String& outputDevice) {
    fullDuplex_ = false;
    if (inputDevice.isEmpty() || outputDevice.isEmpty()) return false;
    if (inputDevice != outputDevice) return false;
    // The current stream model still owns input and output callbacks
    // separately. Treating that as a shared full-duplex device was misleading:
    // it made the beta option promise one hardware clock without actually
    // sharing an AudioDeviceManager. Same-device input gets the safer
    // one-block starting delay through FullDuplexInput instead.
    return false;
}

float AudioEngine::decibelsToGain(float gainDb) {
    return gainDb <= -60.0f ? 0.0f : std::pow(10.0f, gainDb / 20.0f);
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
    settings.autoReloadDevices = autoReloadDevices_.load();
    settings.rackThumbnailFps = rackThumbnailFps_.load();
    return settings;
}

void AudioEngine::setAutoReloadDevices(bool enabled) {
    autoReloadDevices_.store(enabled);
}

void AudioEngine::setRackThumbnailFps(int fps) {
    rackThumbnailFps_.store(juce::jlimit(1, 60, fps));
}

void AudioEngine::setWatchedDevices(const std::vector<pluginrack::WatchedDevice>& devices) {
    const juce::ScopedLock lock(watchedDevicesLock_);
    watchedDevices_ = devices;
}

std::vector<pluginrack::WatchedDevice> AudioEngine::getWatchedDevices() const {
    const juce::ScopedLock lock(watchedDevicesLock_);
    return watchedDevices_;
}

AudioEngine::DeviceState AudioEngine::getChannelDeviceState(std::uint32_t channelId) const {
    const juce::ScopedLock lock(stateLock);
    for (const auto& channel : channels) {
        if (channel->id == channelId)
            return {channel->inputDeviceName, channel->inputError};
    }
    return {};
}

AudioEngine::DeviceState AudioEngine::getOutputDeviceState(std::uint32_t outputId) const {
    const juce::ScopedLock lock(stateLock);
    for (const auto& output : outputs) {
        if (output->id == outputId)
            return {output->outputDeviceName, output->outputError};
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
    autoReloadDevices_.store(settings.autoReloadDevices);
    if (settings.rackThumbnailFps > 0) rackThumbnailFps_.store(settings.rackThumbnailFps);
    // Buffer and stream-mode changes replace every device, so a measurement
    // taken against the old configuration is no longer true.
    resetLatencyMeasurement();
}

void AudioEngine::applyDeviceSettings(const DeviceSettings& settings) {
    if (isOpeningDevices()) return;
    applySettingsValues(settings);
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
            && output->outputDeviceName != "None - silent") {
            devices.push_back({"Output  " + output->name + "  -  " + output->outputDeviceName,
                               output->outputDeviceName, output->id, false});
        }
    }
    std::set<juce::String> seenInputs;
    for (const auto& channel : channels) {
        if (channel->inputDeviceName.isNotEmpty()
            && channel->inputDeviceName != "None - silent"
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
                && output->outputDeviceName != "None - silent") {
                outputsWanted.push_back({output->id, output->outputDeviceName, output->name});
            }
        }
        std::sort(outputsWanted.begin(), outputsWanted.end(),
                  [](const Request& a, const Request& b) { return a.id < b.id; });
        std::set<juce::String> seenInputs;
        for (const auto& channel : channels) {
            if (channel->inputDeviceName.isEmpty()
                || channel->inputDeviceName == "None - silent") {
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

    if (!shouldExit()) {
        launchOutputs(openRate, outputResults, outputHelpers);
        launchInputs(openRate, forceInputs, inputResults, inputHelpers);
    }
    for (auto& helper : outputHelpers) helper.join();
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
