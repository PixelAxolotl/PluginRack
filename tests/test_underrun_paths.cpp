// Static regression proof for the audio-callback underrun fixes. The code
// under test (DeviceStream.cpp, AudioEngine.cpp) needs JUCE device objects
// that cannot be instantiated in the core test binary, so these assertions
// pin the invariants that keep the audio threads starvation-free and
// allocation-free: deep-enough FIFOs, silence priming, no heap work in
// render(), and a clock reopen that never stops feeding the other outputs.

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string readSource(const char* relativePath) {
    const std::string path = std::string(PLUGINRACK_SOURCE_DIR) + "/" + relativePath;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        FAIL("cannot open " << path);
    }
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

// Extract the body of a function definition: the text between the first '{'
// after `signature` and its matching '}'. The targeted sources keep braces
// out of string literals and comments, so plain depth counting is enough.
std::string functionBody(const std::string& source, const std::string& signature) {
    const auto start = source.find(signature);
    if (start == std::string::npos) {
        FAIL("missing definition: " << signature);
    }
    const auto brace = source.find('{', start);
    if (brace == std::string::npos) {
        FAIL("missing body for: " << signature);
    }
    int depth = 0;
    for (auto i = brace; i < source.size(); ++i) {
        if (source[i] == '{') ++depth;
        if (source[i] == '}') {
            --depth;
            if (depth == 0) {
                return source.substr(brace, i - brace + 1);
            }
        }
    }
    FAIL("unbalanced braces in: " << signature);
    return {};
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

int countOccurrences(const std::string& haystack, const std::string& needle) {
    int count = 0;
    for (auto pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

} // namespace

TEST_CASE("OutputPlayback FIFO holds eight device buffers and starts primed",
          "[underrun]") {
    const auto source = readSource("app/src/DeviceStream.cpp");
    const auto body = functionBody(source, "OutputPlayback::open");
    const auto header = readSource("app/src/DeviceStream.h");
    // Depth: the requested target plus two spare blocks, capped so a large
    // buffer cannot balloon, and never smaller than the target. Four blocks
    // is the default; the startup clock asks for one because it renders and
    // drains in the same callback.
    CHECK(contains(header, "int targetFillBlocks = 4"));
    CHECK(contains(header, "int primeBlocks = -1"));
    CHECK(contains(body, "const int targetFrames = bufferSize * juce::jmax(0, targetFillBlocks)"));
    CHECK(contains(body, "bufferSize * 8"));
    CHECK(contains(body, "65536"));
    const auto engine = readSource("app/src/AudioEngine.cpp");
    CHECK(contains(engine, "(isClock && isOpeningDevices())"));
    // Prime before the device callback can read, so a delayed first render
    // cannot expose an empty FIFO.
    const auto prime = body.find("capacity - bufferSize");
    const auto callback = body.find("addAudioCallback");
    CHECK(prime != std::string::npos);
    CHECK(callback != std::string::npos);
    CHECK(prime < callback);
}

TEST_CASE("OutputPlayback never publishes a partial FIFO block", "[underrun]") {
    const auto body = functionBody(readSource("app/src/DeviceStream.cpp"),
                                   "void OutputPlayback::push");
    CHECK(contains(body, "fifo->getFreeSpace() < frames"));
    CHECK(contains(body, "return"));
}

TEST_CASE("OutputPlayback fades short reads instead of repeating a block",
          "[underrun]") {
    const auto body = functionBody(
        readSource("app/src/DeviceStream.cpp"),
        "void OutputPlayback::audioDeviceIOCallbackWithContext");
    // The shortfall fades to silence and the next block fades back in; a
    // repeated block would step at both seams.
    CHECK(contains(body, "deviceFadeInRemaining = 64"));
    CHECK(contains(body, "last * (1.0f -"));
    CHECK(contains(body, "out[i] *= "));
    CHECK_FALSE(contains(body, "lastOutputBlock"));
    // Low watermarks count one event per starvation episode, not per block.
    CHECK(contains(body, "if (!lowWatermarkActive)"));
    CHECK(contains(body, "outputLowWatermarks.fetch_add"));
}

TEST_CASE("InputCapture ring buffer gets the same depth clamp", "[underrun]") {
    const auto body = functionBody(readSource("app/src/DeviceStream.cpp"),
                                   "InputCapture::open");
    // The ring is sized from the larger of the input device buffer and the
    // output render block, so mismatched buffer settings cannot starve it.
    CHECK(contains(body, "capacityUnit * 4"));
    CHECK(contains(body, "65536"));
    // Read delay is configurable (1 or 2 blocks); the plan owns that
    // arithmetic and the ring must still clamp.
    CHECK(contains(body, "delayPlan_.open(deviceBufferSize, renderBlockFrames, planConfig)"));
    CHECK(contains(body, "planConfig.readDelayBlocks = juce::jmax(1, readDelayBlocks)"));
    CHECK(contains(body, "readCursor.prepare"));
}

TEST_CASE("input read delay ignores the probe capacity reserve", "[underrun][latency]") {
    const auto source = readSource("app/src/DeviceStream.cpp");
    const auto body = functionBody(source, "juce::String InputCapture::open");
    // The reserve for an unfinished clock probe may only grow the ring. It must
    // not reach the delay unit, or it is added to every input as real latency.
    CHECK(contains(body, "capacityUnit = juce::jmax(blockFrames, capacityReserveFrames)"));
    // The delay comes from the plan, which is fed the device buffer and the
    // render block only — the reserve never reaches it.
    CHECK(contains(body, "delayPlan_.open(deviceBufferSize, renderBlockFrames, planConfig)"));
    CHECK_FALSE(contains(body, "delayPlan_.open(capacityUnit"));

    const auto engine = readSource("app/src/AudioEngine.cpp");
    CHECK(contains(engine, "renderBlockFrames = currentBufferSize"));
    CHECK(contains(engine,
                   "capacityReserveFrames = juce::jmax(currentBufferSize, "
                   "kProbeRenderBlockFrames)"));
}

TEST_CASE("input read delay is owned by one bounded controller", "[underrun][latency]") {
    const auto source = readSource("app/src/DeviceStream.cpp");
    const auto body = functionBody(source, "bool InputCapture::pull");
    // The delay decision lives in DelayTargetController: it grows on a real
    // starvation and gives the margin back slowly, clamped to the ring. The
    // pull path must not grow the delay itself, or repeated underruns during a
    // device reload could accumulate without bound.
    CHECK(contains(body, "delayTarget_.onBlock("));
    CHECK(contains(body, "readCursor.setTargetDelay(nextDelay)"));
    CHECK_FALSE(contains(body, "currentDelay <= baseDelay"));
    const auto open = functionBody(source, "juce::String InputCapture::open");
    CHECK(contains(open, "delayPlan_.open(deviceBufferSize, renderBlockFrames, planConfig)"));
    CHECK(contains(open, "readCursor.prepare(capacity, delay, 0.005)"));
    // The delay is settled on the final render block, not on whatever the open
    // happened to see.
    CHECK(contains(body, "delayTarget_.onBlock(currentDelay, starved, window.overrun,"));
    CHECK(contains(source, "void InputCapture::applyRenderBlockFrames"));
    CHECK(contains(body, "const bool starved = window.underrun && primedEnough && settledIn"));
    CHECK_FALSE(contains(body, "window.available) - currentDelay"));

    const auto engine = readSource("app/src/AudioEngine.cpp");
    CHECK(contains(functionBody(engine,
                                "void AudioEngine::prepareToPlayImpl(int maxBlockFrames, bool openInputs)"),
                   "applyRenderBlockFrames(maxBlockFrames_)"));
}

TEST_CASE("low-latency input backs off only after a real starvation", "[underrun][latency]") {
    const auto source = readSource("app/src/DeviceStream.cpp");
    const auto body = functionBody(source, "bool InputCapture::pull");
    // The first one-block pull is allowed to prime without becoming a false
    // glitch; later starvation adds margin and keeps the cursor continuous.
    CHECK(contains(body, "written >= targetDelay + static_cast<std::uint64_t>(frames)"));
    CHECK(contains(body, "const bool starved = window.underrun && primedEnough"));
    CHECK(contains(body, "const int nextDelay"));
    CHECK(contains(body, "readCursor.setTargetDelay(nextDelay)"));
}

TEST_CASE("output FIFO follows the clock block and corrects independent clocks", "[underrun][latency]") {
    const auto header = readSource("app/src/DeviceStream.h");
    const auto device = readSource("app/src/DeviceStream.cpp");
    const auto open = functionBody(device, "OutputPlayback::open");
    const auto callback = functionBody(
        device, "void OutputPlayback::audioDeviceIOCallbackWithContext");
    CHECK(contains(header, "int renderBlockFrames = 0"));
    CHECK(contains(open, "renderBlockFrames_ * 2"));
    CHECK(contains(callback, "kMaxDriftCorrection"));
    CHECK(contains(callback, "fifo->read(sourceFrames)"));
    CHECK(contains(callback, "exactRead"));
}

TEST_CASE("InputCapture publishes lock-free and counts real underruns",
          "[underrun][telemetry]") {
    const auto cpp = readSource("app/src/DeviceStream.cpp");
    const auto writer = functionBody(
        cpp, "void InputCapture::audioDeviceIOCallbackWithContext");
    CHECK(contains(writer, "totalWritten.store"));
    CHECK(contains(writer, "std::memory_order_release"));
    // No spinlock on the capture path: a busy UI must never make the device
    // callback drop input.
    CHECK_FALSE(contains(writer, "SpinLock"));

    const auto reader = functionBody(cpp, "bool InputCapture::pull");
    CHECK(contains(reader, "totalWritten.load"));
    CHECK(contains(reader, "std::memory_order_acquire"));
    CHECK(contains(reader, "readCursor.nextBlock"));
    CHECK(contains(reader, "inputUnderruns.fetch_add"));
    CHECK(contains(reader, "inputDroppedFrames.fetch_add"));
    CHECK(contains(reader, "fadeInEpoch_"));
}

TEST_CASE("AudioEngine::render never allocates on the audio thread", "[underrun][rt]") {
    const auto body = functionBody(readSource("app/src/AudioEngine.cpp"),
                                   "void AudioEngine::render(int frames)");
    // The heap-allocating silence buffers are gone; the remaining
    // AudioBuffer<float> uses are non-owning views over preallocated data.
    CHECK_FALSE(contains(body, "AudioBuffer<float> silence"));
    CHECK_FALSE(contains(body, "(2, frames)"));
    // Both silent-output paths still exist and route through the
    // published playback snapshot instead of allocating a buffer.
    CHECK(countOccurrences(body, "pushSilenceToOutputs(frames)") >= 1);
    CHECK(contains(body, "pushSilenceToOutputsLocked(frames)"));
    // Preserved semantics: oversized blocks are counted, lock loss is not.
    CHECK(contains(body, "oversizedBlockCount_.fetch_add"));
    CHECK(contains(body, "lock.isLocked()"));
    CHECK(contains(body, "renderMisses_.fetch_add"));
}

TEST_CASE("render-miss silence path is lock-free on published outputs",
          "[underrun][rt]") {
    // This path runs exactly when the message thread holds stateLock, so it
    // must not take it again or it would silently feed nothing and let the
    // output FIFO starve.
    const auto cpp = readSource("app/src/AudioEngine.cpp");
    const auto helper = functionBody(
        cpp, "void AudioEngine::pushSilenceToOutputs(int frames)");
    CHECK_FALSE(contains(helper, "stateLock"));
    CHECK_FALSE(contains(helper, "ScopedTryLock"));
    CHECK_FALSE(contains(helper, "ScopedLock"));
    CHECK(contains(helper, "renderOutputs_"));
    CHECK(contains(helper, "pushSilence"));

    const auto publish = functionBody(
        cpp, "void AudioEngine::publishRenderOutputsLocked()");
    CHECK(contains(publish, "memory_order_release"));
    CHECK(contains(publish, "output->output.get()"));
}

TEST_CASE("render waits briefly for the state lock before dropping a block",
          "[underrun][rt]") {
    const auto cpp = readSource("app/src/AudioEngine.cpp");
    const auto guard = functionBody(cpp, "struct BriefTryLock");
    CHECK(contains(guard, "tryEnter"));
    CHECK(contains(guard, "juce::Thread::yield"));
    const auto render = functionBody(cpp, "void AudioEngine::render(int frames)");
    CHECK(contains(render, "BriefTryLock lock(stateLock, "));
    CHECK(contains(render, "renderMisses_.fetch_add"));
}

TEST_CASE("plugin prepare and release run outside the state lock",
          "[underrun][rt]") {
    // Holding stateLock across a VST3 prepareToPlay is the longest known
    // render-miss source (device reopen / scene restore).
    const auto cpp = readSource("app/src/AudioEngine.cpp");
    const auto prepare = functionBody(
        cpp, "void AudioEngine::prepareToPlayImpl(int maxBlockFrames, bool openInputs)");
    CHECK_FALSE(contains(prepare, "insert->pluginHost.prepareToPlay"));
    CHECK(contains(prepare, "std::vector<PluginHost*> hosts"));
    CHECK(contains(prepare, "host->prepareToPlay"));
    // A render-block change must also resize the capture rings.
    CHECK(contains(prepare, "if (openInputs) prepareInputs()"));

    const auto release = functionBody(cpp, "void AudioEngine::releaseResources()");
    CHECK_FALSE(contains(release, "insert->pluginHost.releaseResources"));
    CHECK(contains(release, "host->releaseResources"));
}

TEST_CASE("latency tuning options reach the device opens",
          "[underrun][latency]") {
    const auto engine = readSource("app/src/AudioEngine.cpp");
    CHECK(contains(engine, "AudioEngine::setLatencyOption"));
    CHECK(contains(engine, "useLowLatencyInput ? 1 : 2"));
    CHECK(contains(engine, "minimalClockFifo_.load"));
    CHECK(contains(engine, "lowLatencyOutputs_.load"));
    CHECK(contains(engine, "preferExclusive_.load"));
    CHECK(contains(engine, "fullDuplexInput_.load"));
    const auto device = readSource("app/src/DeviceStream.cpp");
    CHECK(contains(device, "delayPlan_.open(deviceBufferSize, renderBlockFrames, planConfig)"));
    CHECK(contains(device, "preferExclusive"));
    // The probe order is policy-driven so the clock can reach ASIO first while
    // captures and extra outputs stay shareable.
    CHECK(contains(device, "pluginrack::deviceTypeOrder(role, preferExclusive, asioAvailable())"));
    CHECK(contains(device, "pluginrack::StreamRole::Input"));
    CHECK_FALSE(contains(device, "juce::StringArray typeOrder { kTypeShared"));
    CHECK(contains(device, "typeSetup.setAttribute(\"deviceType\", chosenTypeName)"));
    CHECK_FALSE(contains(device, "manager.setCurrentAudioDeviceType(typeName, false)"));
    const auto settings = readSource("app/src/SettingsView.cpp");
    CHECK(contains(settings, "LowLatencyInput"));
    CHECK(contains(settings, "MinimalClockFifo"));
    CHECK(contains(settings, "showDeviceBuffer"));
}

TEST_CASE("device startup overlaps probes and opens every stream once", "[underrun][latency]") {
    const auto engine = readSource("app/src/AudioEngine.cpp");
    const auto openPass = functionBody(
        engine, "juce::String AudioEngine::openAllDevicesPass");
    CHECK(contains(openPass, "launchOutputs(openRate, outputResults, outputHelpers)"));
    CHECK(contains(openPass, "launchInputs(openRate, forceInputs, inputResults, inputHelpers)"));
    CHECK(contains(openPass, "setChannelInputDeviceImpl(request.id, request.name, force, rate)"));
    // No rollback wave, no second rate pass: exactly one attempt per stream.
    CHECK_FALSE(contains(openPass, "retryInputs"));
    CHECK_FALSE(contains(openPass, "clockOpened"));
    const auto strips = functionBody(engine, "void AudioEngine::openStripsDevicesNow");
    CHECK(countOccurrences(strips, "openAllDevicesPass(") == 1);
    CHECK(contains(engine, "currentBufferSize = newBuf"));
    CHECK(contains(engine, "kProbeRenderBlockFrames"));
}

TEST_CASE("failing devices are silenced and named in the progress text",
          "[underrun][latency]") {
    const auto engine = readSource("app/src/AudioEngine.cpp");
    const auto pass = functionBody(engine, "juce::String AudioEngine::openAllDevicesPass");
    // Each endpoint reports the endpoint it is opening: the output launcher,
    // the input launcher, and the one clock retry for a single-client driver.
    CHECK(countOccurrences(pass, "beginDeviceOpen(status)") == 3);
    CHECK(countOccurrences(pass, "endDeviceOpen(status)") == 3);
    const auto engine2 = readSource("app/src/AudioEngine.cpp");
    const auto status = functionBody(engine2, "juce::String AudioEngine::getDeviceOpenStatus");
    CHECK(contains(status, "endpoints in parallel"));
    CHECK(contains(status, " more)"));
    // A refusal is reported against the strip and endpoint it belongs to, and
    // a refused clock explains why nothing plays.
    CHECK(contains(pass, "outputsWanted[i].label"));
    CHECK(contains(pass, "inputsWanted[i].label"));
    CHECK(contains(pass, "could not be opened as the clock"));
}

TEST_CASE("each device is opened with one device type and no retry",
          "[underrun][latency]") {
    const auto source = readSource("app/src/DeviceStream.cpp");
    const auto preferred = functionBody(source, "juce::String openWithPreferredType");
    // One initialise() call: picking the type is a name lookup, never a probe.
    CHECK(countOccurrences(preferred, "manager.initialise(") == 1);
    CHECK_FALSE(contains(source, "openWithWildcardSearch"));
    // A missing endpoint fails immediately instead of falling back to the
    // system default and reporting success.
    CHECK(contains(preferred, "No such device: "));
    CHECK(contains(preferred, "manager.closeAudioDevice()"));
}

TEST_CASE("settings apply fails safe and reloads single devices",
          "[underrun][latency]") {
    const auto engine = readSource("app/src/AudioEngine.cpp");
    CHECK(contains(engine, "AudioEngine::applyDeviceSettings"));
    // No "restore the previous settings and try again" second pass.
    CHECK_FALSE(contains(engine, "restored the previous ones"));
    CHECK(contains(engine, "AudioEngine::reloadAllDevicesAsync"));
    CHECK(contains(engine, "AudioEngine::reloadDeviceAsync"));
    CHECK(contains(engine, "forceReopen"));
    const auto settings = readSource("app/src/SettingsView.cpp");
    CHECK(contains(settings, "Pending changes - press Apply"));
    CHECK(contains(settings, "applyDeviceSettings"));
    const auto header = readSource("app/src/SettingsView.h");
    CHECK(contains(header, "Reload All Audio Devices"));
    CHECK(contains(header, "Auto-reload checked devices on error"));
    CHECK(contains(header, "reloadDeviceToggles"));
    CHECK(contains(header, "autoReloadToggle"));
    CHECK(contains(engine, "AudioEngine::reloadDevicesAsync"));
    // One attempt per device: a busy or failing endpoint is reported and left
    // silent instead of being retried after a pause.
    const auto reload = functionBody(engine, "void AudioEngine::reloadDevicesAsync");
    CHECK_FALSE(contains(reload, "attempt <"));
    CHECK_FALSE(contains(reload, "juce::Thread::sleep(400)"));
    CHECK(contains(reload, "beginDeviceOpen(status)"));
}

TEST_CASE("clock reopen keeps feeding other outputs until the new clock runs",
          "[underrun]") {
    const auto body = functionBody(readSource("app/src/AudioEngine.cpp"),
                                   "juce::String AudioEngine::setOutputDeviceImpl");
    // The old clock must not be cut off before the new device is open: the
    // prevOutput hand-off stop happens after newPlayback->open succeeds.
    const auto open = body.find("newPlayback->open(");
    REQUIRE(open != std::string::npos);
    const auto stopOld = body.find("prevOutput->setRenderActive(false)");
    REQUIRE(stopOld != std::string::npos);
    CHECK(stopOld > open);
    // The incoming clock starts inactive and is activated with the hand-off,
    // so no second render hook runs while the old clock still feeds.
    CHECK(contains(body, "newPlayback->setRenderActive(false)"));
    CHECK(contains(body, "setRenderActive(true)"));
}

TEST_CASE("buffer-size requests must match the device buffer",
          "[underrun][buffer]") {
    const auto source = readSource("app/src/DeviceStream.cpp");
    const auto preferred = functionBody(source, "juce::String openWithPreferredType");
    CHECK(contains(preferred, "getAvailableBufferSizes()"));
    CHECK(contains(preferred, "The device does not support"));
    CHECK(contains(preferred, "manager.closeAudioDevice()"));
    // The old exact-match requirement is gone for drivers that negotiate a
    // supported size different from the ask (M-Audio Duo 480 vs 32).
    CHECK_FALSE(contains(preferred,
                         "device->getCurrentBufferSizeSamples() != requestedBufferSize) {"));
}

TEST_CASE("applying devices releases a reused ASIO client before reopening", "[latency][duplex]") {
    const auto engine = readSource("app/src/AudioEngine.cpp");
    const auto pass = functionBody(engine, "juce::String AudioEngine::openAllDevicesPass");
    // The old behavior: outputs and inputs raced for a single-client ASIO
    // driver in parallel, so Apply could hit "No such device" while Force
    // ASIO (which opens the clock synchronously first) worked.
    // The fix: clock (outputs) open before inputs, and prior playback is
    // closed before the new open.
    const auto out = pass.find("launchOutputs(");
    const auto in = pass.find("launchInputs(");
    REQUIRE(out != std::string::npos);
    REQUIRE(in != std::string::npos);
    CHECK(out < in);
    // Not just launched in order: the clock's callback must be live before any
    // input opens, or the input thread can still win the single-client race.
    const auto clockJoin = pass.find("for (auto& helper : outputHelpers) helper.join()");
    REQUIRE(clockJoin != std::string::npos);
    CHECK(clockJoin < in);
    // Inputs on the clock device must be released too, not left holding the
    // endpoint while the clock fronts the join.
    CHECK(contains(pass, "releaseDeviceInputsForClock"));
    // The prior playback must be closed before the new open or the driver
    // refuses the second client and Apply reports "No such device".
    const auto open = functionBody(engine, "juce::String AudioEngine::setOutputDeviceImpl");
    const auto closeIdx = open.find("prevOutput->close()");
    const auto openIdx = open.find("newPlayback->open(");
    REQUIRE(closeIdx != std::string::npos);
    REQUIRE(openIdx != std::string::npos);
    CHECK(closeIdx < openIdx);
    // ASIO's single-client driver keeps refusing CoCreateInstance for a while
    // after the previous stream closes (JUCE only releases it on destruction).
    // The clock is therefore retried, bounded, before any input launches — the
    // recovery Force ASIO performs by hand.
    const auto retryIdx = pass.find("retryClockOpen");
    const auto inputIdx = pass.find("launchInputs(");
    REQUIRE(retryIdx != std::string::npos);
    REQUIRE(inputIdx != std::string::npos);
    CHECK(retryIdx < inputIdx);
    // More than one extra attempt: 150 ms was not enough for the M-Audio Duo.
    CHECK(contains(pass, "kClockOpenAttempts"));
    CHECK(contains(pass, "\"No such device\""));
    CHECK(contains(engine, "constexpr int kClockOpenAttempts = 4"));
    // A single-client ASIO driver also refuses forever while the open runs on a
    // throwaway worker apartment. Force ASIO works because it opens on the
    // message thread, so a still-refused clock is reopened there.
    CHECK(contains(pass, "existsAndIsCurrentThread"));
    CHECK(contains(pass, "MessageManager::callSync"));
}

TEST_CASE("input and output buffer sizes are independently configurable",
          "[underrun][buffer]") {
    const auto header = readSource("app/src/AudioEngine.h");
    const auto cpp = readSource("app/src/AudioEngine.cpp");
    CHECK(contains(header, "setInputBufferSize"));
    CHECK(contains(header, "setOutputBufferSize"));
    CHECK(contains(cpp, "requestedInputBufferSize_"));
    CHECK(contains(cpp, "requestedOutputBufferSize_"));
}

TEST_CASE("restored plugins do not reprepare for an unchanged device format",
          "[plugin][restore]") {
    // Out-of-process-only: prepareToPlay records the format; the sandboxed
    // host prepares its own instance on first block. No in-process instance
    // exists, so there is nothing to reprepare here.
    const auto header = readSource("app/src/PluginHost.h");
    const auto body = functionBody(readSource("app/src/PluginHost.cpp"),
                                   "void PluginHost::prepareToPlay");
    CHECK(contains(header, "bool prepared = false"));
    CHECK(contains(body, "currentSampleRate = sampleRate"));
    CHECK(contains(body, "currentBufferSize = samplesPerBlock"));
    CHECK(contains(body, "prepared = host != nullptr && hostReady"));
    CHECK_FALSE(contains(body, "plugin->prepareToPlay"));
}

TEST_CASE("scene restore loads plugins before the device handoff",
          "[plugin][restore]") {
    const auto body = functionBody(readSource("app/src/AudioEngine.cpp"),
                                   "juce::String AudioEngine::restoreScene");
    const auto load = body.find("loadPlugin(juce::File(insert->pluginPath)");
    const auto device = body.find("openStripsDevicesAsync()");
    REQUIRE(load != std::string::npos);
    REQUIRE(device != std::string::npos);
    CHECK(load < device);
    // Restored slots hand their saved state back to the plugin instance.
    CHECK(contains(body, "savedInsert.pluginState"));
}

TEST_CASE("scene save and restore carry every mix level", "[plugin][restore]") {
    const auto cpp = readSource("app/src/AudioEngine.cpp");
    const auto save = functionBody(cpp, "pluginrack::Scene AudioEngine::makeScene");
    const auto restore = functionBody(cpp, "juce::String AudioEngine::restoreScene");

    CHECK(contains(save, "savedChannel.gainDb"));
    CHECK(contains(save, "savedChannel.trimDb"));
    CHECK(contains(save, "savedChannel.pan"));
    CHECK(contains(save, "savedOutput.gainDb"));
    CHECK(contains(save, "savedOutput.muted"));

    CHECK(contains(restore, "channel->gainDb = savedChannel.gainDb"));
    CHECK(contains(restore, "channel->trimDb = savedChannel.trimDb"));
    CHECK(contains(restore, "output->gainDb = savedOutput.gainDb"));
    CHECK(contains(restore, "output->muted = savedOutput.muted"));
}

TEST_CASE("scene save captures the live plugin state", "[plugin][restore]") {
    const auto body = functionBody(readSource("app/src/AudioEngine.cpp"),
                                   "pluginrack::Scene AudioEngine::makeScene");
    CHECK(contains(body, "getStateAsBase64"));
    CHECK(contains(body, "pluginState"));
}

TEST_CASE("scene save and restore carry the audio tuning", "[plugin][restore]") {
    const auto cpp = readSource("app/src/AudioEngine.cpp");
    const auto save = functionBody(cpp, "pluginrack::Scene AudioEngine::makeScene");
    const auto restore = functionBody(cpp, "juce::String AudioEngine::restoreScene");

    CHECK(contains(save, "pluginrack::AudioSettings audio"));
    CHECK(contains(save, "audio.latencyTuningOptIn"));
    CHECK(contains(save, "audio.autoReloadDevices"));
    CHECK(contains(save, "getWatchedDevices"));
    CHECK(contains(restore, "scene.audio.has_value()"));
    CHECK(contains(restore, "settings.latencyTuningOptIn"));
    CHECK(contains(restore, "applySettingsValues(settings)"));
    CHECK(contains(restore, "setWatchedDevices(scene.audio->watchedDevices)"));
}

TEST_CASE("views read device strings through the locked snapshot", "[underrun][thread]") {
    // The device opener assigns the strip device name/error Strings on its
    // worker thread. Copying them from the message thread without stateLock
    // reads a torn StringHolder pointer and faults inside String::operator=.
    const auto channelStrip = readSource("app/src/ChannelStrip.cpp");
    const auto refresh = functionBody(channelStrip, "void ChannelStrip::refreshInput");
    CHECK(contains(refresh, "audioEngine.getChannelDeviceState(channel.id)"));
    CHECK_FALSE(contains(refresh, "setDeviceName(channel.inputDeviceName)"));
    CHECK_FALSE(contains(refresh, "setError(channel.inputError)"));

    const auto mixer = readSource("app/src/MixerComponent.cpp");
    const auto outputRefresh =
        functionBody(mixer, "void MixerComponent::OutputStrip::refresh()");
    CHECK(contains(outputRefresh, "audioEngine.getOutputDeviceState(output.id)"));
    CHECK_FALSE(contains(outputRefresh, "setDeviceName(output.outputDeviceName)"));

    const auto engine = readSource("app/src/AudioEngine.cpp");
    const auto clock = functionBody(engine, "juce::String AudioEngine::getClockDescription");
    CHECK(contains(clock, "const juce::ScopedLock lock(stateLock)"));
    CHECK_FALSE(contains(clock, "getActiveDeviceType"));

    const auto main = readSource("app/src/MainComponent.cpp");
    CHECK(contains(main, "audioEngine.getDeviceErrors()"));
    CHECK_FALSE(contains(main, "c->inputError"));
}

TEST_CASE("an already-open input is reused or released before reopening",
          "[underrun][thread]") {
    const auto engine = readSource("app/src/AudioEngine.cpp");
    const auto body =
        functionBody(engine, "juce::String AudioEngine::setChannelInputDeviceImpl");
    // Reuse compares the ring the capture was sized for, not the clock block
    // that was still unknown when it opened: an exact block match reopens a
    // device this process already holds, the driver refuses the second client,
    // and the user sees an error next to working audio.
    CHECK(contains(body, "it->second.renderBlockFrames >= capacityReserveFrames"));
    CHECK_FALSE(contains(body, "it->second.renderBlockFrames >= currentBufferSize"));
    // Any real reopen releases the endpoint first.
    const auto close = body.find("previous->close()");
    const auto open = body.find("capture->open(");
    REQUIRE(close != std::string::npos);
    REQUIRE(open != std::string::npos);
    CHECK(close < open);
}

TEST_CASE("adding a VST scans in a child process and hosts out-of-process",
          "[plugin][ui]") {
    const auto engine = readSource("app/src/AudioEngine.cpp");
    const auto add = functionBody(engine, "void AudioEngine::addPluginAsync");
    // The scan runs in a child process (seconds of enumeration, and a plugin
    // that faults there must not take the app down), so it is safe on a worker.
    CHECK(contains(add, "juce::Thread::launch"));
    CHECK(contains(add, "PluginHost::scanFile"));
    CHECK(contains(add, "MessageManager::callAsync"));
    // Out-of-process-only: the message thread resolves metadata and starts
    // the sandboxed host. No instance is ever created in the main app.
    CHECK(contains(add, "chooseDescription"));
    CHECK(contains(add, "loaded.uniqueId = chosen.uniqueId"));
    CHECK_FALSE(contains(add, "PluginHost::instantiate("));
    CHECK_FALSE(contains(add, "PluginHost::instantiateHostInstance"));
    const auto post = add.find("juce::MessageManager::callAsync");
    const auto adopt = add.find("insert->pluginHost.adopt(loaded)");
    REQUIRE(post != std::string::npos);
    REQUIRE(adopt != std::string::npos);
    CHECK(post < adopt);

    const auto addOutput = functionBody(engine, "void AudioEngine::addOutputPluginAsync");
    CHECK(contains(addOutput, "juce::Thread::launch"));
    CHECK(contains(addOutput, "PluginHost::scanFile"));
    CHECK(contains(addOutput, "chooseDescription"));
    CHECK_FALSE(contains(addOutput, "PluginHost::instantiate("));

    // The child process is this executable with --scan-plugin, and the scan
    // itself only runs there. Instances are created only in --plugin-host.
    const auto host = readSource("app/src/PluginHost.cpp");
    CHECK(contains(host, "juce::ChildProcess"));
    CHECK(contains(host, "--scan-plugin"));
    CHECK(contains(host, "PluginHost::scanFileInProcess"));
    CHECK(contains(host, "sharedFormatLock()"));
    CHECK(contains(host, "instantiateHostInstance"));
    CHECK_FALSE(contains(host, "plugin->processBlock(buffer, midiBuffer)"));

    const auto rack = readSource("app/src/RackView.cpp");
    const auto insert = functionBody(rack, "juce::String RackView::insertPlugin");
    CHECK(contains(insert, "addPluginAsync"));
    CHECK(contains(insert, "addOutputPluginAsync"));
    CHECK_FALSE(contains(insert, "audioEngine.addPlugin("));
}

TEST_CASE("crashes are logged with a backtrace", "[crash]") {
    const auto main = readSource("app/src/Main.cpp");
    CHECK(contains(main, "setApplicationCrashHandler"));
    // PR-09: the handler writes only a pending record (allocation-free);
    // startup drains it into the blocklist and writes the full report.
    CHECK(contains(main, "currentLoadingPluginBytes"));
    CHECK(contains(main, "pending-crash-plugin"));
    CHECK(contains(main, "PluginLoadGuard::block"));
    CHECK(contains(main, "getStackBacktrace"));
    CHECK(contains(main, "crash-"));
    CHECK(contains(main, "setCurrentLogger"));
    const auto cmake = readSource("app/CMakeLists.txt");
    CHECK(contains(cmake, "/Zi"));
    // PR-SEC-015: PDBs ship via the symbol server (build/symbols), not beside
    // the exe; the link still emits debug info for the report offsets.
    CHECK(contains(cmake, "target_link_options(pluginrack_app PRIVATE /DEBUG"));
}

TEST_CASE("scene save captures plugin state outside the audio lock", "[underrun][rt]") {
    // render() only yields a few times for stateLock before it pushes silence,
    // so makeScene must not run plugin code while holding it.
    const auto cpp = readSource("app/src/AudioEngine.cpp");
    const auto save = functionBody(cpp, "pluginrack::Scene AudioEngine::makeScene");
    const auto lockStart = save.find("const juce::ScopedLock lock(stateLock)");
    REQUIRE(lockStart != std::string::npos);
    // The settings block is the last thing copied under the lock; everything
    // after it (the plugin state capture) runs unlocked.
    const auto lockEnd = save.find("scene.audio = std::move(audio);");
    REQUIRE(lockEnd != std::string::npos);
    const auto capture = save.find("getStateAsBase64");
    REQUIRE(capture != std::string::npos);
    CHECK(capture > lockEnd);
}

TEST_CASE("removing the last channel is refused instead of losing the session",
          "[app][session]") {
    const auto cpp = readSource("app/src/AudioEngine.cpp");
    // A refusal needs a message, so the signature returns one like removeOutput.
    CHECK(contains(cpp, "juce::String AudioEngine::removeChannel"));
    const auto remove = functionBody(cpp, "juce::String AudioEngine::removeChannel");
    CHECK(contains(remove, "isOpeningDevices()"));
    CHECK(contains(remove, "Audio devices are still starting up"));
    CHECK(contains(remove, "channels.size() <= 1"));
    CHECK(contains(remove, "At least one channel is required"));

    // ...and the strip has to show it, like the output strip does.
    const auto mixer = readSource("app/src/MixerComponent.cpp");
    const auto strips = functionBody(mixer, "void MixerComponent::rebuildChannelStrips");
    CHECK(contains(strips, "removeChannel(id)"));
    CHECK(contains(strips, "showMessageBoxAsync"));
}

TEST_CASE("exit autosave never swallows a failed save", "[app][session]") {
    // shutdown() terminates the process right after this call, so the error is
    // logged and left for the next launch instead of being dropped.
    const auto mainComponent = readSource("app/src/MainComponent.cpp");
    const auto exitSave = functionBody(mainComponent, "void MainComponent::saveSceneOnExit");
    CHECK(contains(exitSave, "SceneBridge::saveLast"));
    CHECK(contains(exitSave, "isNotEmpty()"));
    CHECK(contains(exitSave, "writeSaveFailureNote"));

    const auto bridge = readSource("app/src/SceneBridge.cpp");
    CHECK(contains(bridge, "SceneBridge::writeSaveFailureNote"));
    CHECK(contains(bridge, "SceneBridge::takeSaveFailureNote"));
    const auto startup = functionBody(mainComponent, "void MainComponent::initialiseAudio");
    CHECK(contains(startup, "takeSaveFailureNote"));
}

TEST_CASE("scenes are validated after loading, not only before saving", "[app][scene]") {
    const auto bridge = readSource("app/src/SceneBridge.cpp");
    const auto helper = functionBody(bridge, "std::optional<pluginrack::Scene> loadValidatedScene");
    CHECK(contains(helper, "pluginrack::validate"));
    const auto load = functionBody(
        bridge, "std::optional<pluginrack::Scene> SceneBridge::load(const juce::File& file");
    CHECK(contains(load, "loadValidatedScene"));
    const auto last = functionBody(bridge, "std::optional<pluginrack::Scene> SceneBridge::loadLast");
    CHECK(contains(last, "loadValidatedScene"));
}

TEST_CASE("scene and plugin inputs are bounded before native code runs", "[app][security]") {
    const auto bridge = readSource("app/src/SceneBridge.cpp");
    const auto helper = functionBody(bridge, "std::optional<pluginrack::Scene> loadValidatedScene");
    CHECK(contains(bridge, "fileSize > kMaxSceneFileBytes"));
    CHECK(contains(bridge, "kMaxSceneFileBytes"));
    CHECK(contains(helper, "loadFileAsString"));
    // PR-SEC-009: the size/depth/key guards live in core so every consumer
    // (SceneBridge, render_cli) gets them.
    const auto codec = readSource("core/src/SceneCodec.cpp");
    CHECK(contains(codec, "kMaxSceneJsonBytes"));
    CHECK(contains(codec, "kMaxSceneJsonDepth"));
    CHECK(contains(codec, "kMaxSceneJsonKeys"));
    CHECK(contains(codec, "sax_parse"));

    const auto host = readSource("app/src/PluginHost.cpp");
    CHECK(contains(host, "kMaxPluginStateBase64Chars"));
    CHECK(contains(host, "INT_MAX"));
}

TEST_CASE("save names reject Windows device names and excessive lengths", "[app][security]") {
    const auto bridge = readSource("app/src/SceneBridge.cpp");
    CHECK(contains(bridge, "kMaxSaveNameLength"));
    CHECK(contains(bridge, "startsWith(\"com\")"));
    CHECK(contains(bridge, "startsWith(\"lpt\")"));
}

TEST_CASE("fetched dependencies are pinned to commit hashes", "[app][security]") {
    // PR-04: a moving Git tag can silently change the build. Fetched deps pin
    // to an exact 40-char SHA instead.
    for (const char* path : {"core/CMakeLists.txt", "tests/CMakeLists.txt"}) {
        const auto text = readSource(path);
        CHECK_FALSE(contains(text, "GIT_TAG v"));
        bool hasSha = false;
        for (auto pos = text.find("GIT_TAG "); pos != std::string::npos;
             pos = text.find("GIT_TAG ", pos + 1)) {
            const auto shaStart = pos + 8;
            if (shaStart + 40 <= text.size()) {
                const auto sha = text.substr(shaStart, 40);
                bool hex = true;
                for (const char c : sha) {
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                        hex = false;
                    }
                }
                if (hex) {
                    hasSha = true;
                }
            }
        }
        CHECK(hasSha);
    }
}

TEST_CASE("plugin discovery and child scanning are bounded", "[plugin][security]") {
    const auto library = readSource("app/src/PluginLibrary.cpp");
    CHECK(contains(library, "kMaxScanBundles"));
    CHECK(contains(library, "kMaxScanDepth"));
    CHECK(contains(library, "isSymbolicLink"));

    const auto host = readSource("app/src/PluginHost.cpp");
    const auto scan = functionBody(host, "juce::Array<juce::PluginDescription> PluginHost::scanFile");
    CHECK(contains(scan, "StringArray"));
    CHECK_FALSE(contains(scan, "getFullPathName().quoted()"));
    CHECK(contains(scan, "Network plugin paths are not supported"));
    // PR-02: the child scanner must be bounded in time and output, and must
    // fail closed (never fall back to scanning in the main app).
    CHECK(contains(scan, "kScanTimeoutMs"));
    CHECK(contains(scan, "kMaxScanOutputBytes"));
    CHECK(contains(scan, "child.kill()"));
    CHECK(contains(scan, "waitForProcessToFinish"));
    CHECK_FALSE(contains(scan, "scanFileInProcess(file, error)"));
}

TEST_CASE("the picked class inside a bundle reaches the loader", "[plugin][ui]") {
    const auto cpp = readSource("app/src/AudioEngine.cpp");
    const auto choose = functionBody(cpp, "juce::PluginDescription chooseDescription");
    CHECK(contains(choose, "description.uniqueId == pluginUniqueId"));

    for (const char* signature : {"void AudioEngine::addPluginAsync",
                                  "void AudioEngine::addOutputPluginAsync"}) {
        const auto add = functionBody(cpp, signature);
        CHECK(contains(add, "pluginUniqueId"));
        CHECK(contains(add, "chooseDescription"));
        CHECK_FALSE(contains(add, "descriptions.getReference(0)"));
    }

    // Scene restore reopens the same class the slot was created from.
    // Metadata-only: the instance is created inside --plugin-host.
    const auto host = readSource("app/src/PluginHost.cpp");
    const auto load = functionBody(host, "PluginHost::loadFromFile");
    CHECK(contains(load, "pluginUniqueId != 0 && description.uniqueId == pluginUniqueId"));
    CHECK(contains(load, "chosen->uniqueId"));
    CHECK_FALSE(contains(load, "instantiateHostInstance"));
}

TEST_CASE("the plugin menu refreshes when the scan finishes", "[plugin][ui]") {
    const auto menu = readSource("app/src/PluginMenu.cpp");
    const auto refresh = functionBody(menu, "class ScanRefresh final : public juce::ChangeListener");
    CHECK(contains(refresh, "addChangeListener"));
    CHECK(contains(refresh, "isScanning()"));
    CHECK(contains(refresh, "showAddPluginMenu"));
    // The menu row must carry the class, not just the bundle path.
    CHECK(contains(menu, "description.uniqueId"));
}

TEST_CASE("reset to defaults restores the dynamics rows and the chain", "[app][ui]") {
    const auto mainComponent = readSource("app/src/MainComponent.cpp");
    CHECK(contains(mainComponent, "setChannelEffects(c->id, pluginrack::defaultDynamicsEffects())"));
    CHECK(contains(mainComponent, "setChannelChain(c->id, AudioEngine::defaultChain())"));
    CHECK(contains(mainComponent, "setOutputEffects(outputId, pluginrack::defaultDynamicsEffects())"));
    CHECK(contains(mainComponent, "setOutputChain(outputId, AudioEngine::defaultChain())"));
    CHECK(contains(mainComponent, "Strip names, device assignments and watched streams are kept"));
}

TEST_CASE("the clock callback records render telemetry without locking",
          "[latency][telemetry]") {
    const auto cpp = readSource("app/src/AudioEngine.cpp");
    const auto render = functionBody(cpp, "void AudioEngine::render(int frames)");
    CHECK(contains(render, "std::chrono::steady_clock::now()"));
    // Every exit path that the user can hear must leave a record, or the p99
    // would describe a filtered subset of callbacks.
    CHECK(contains(render, "pluginrack::AudioTelemetry::flagRenderMiss"));
    CHECK(contains(render, "pluginrack::AudioTelemetry::flagOversized"));
    CHECK(contains(render, "recordRenderTelemetry(frames, elapsedNanos(), 0, clockFillFrames)"));

    const auto record = functionBody(cpp, "void AudioEngine::recordRenderTelemetry");
    CHECK(contains(record, "renderTelemetry_.push(sample)"));
    CHECK_FALSE(contains(record, "ScopedLock"));
    CHECK_FALSE(contains(record, "new "));

    const auto settings = readSource("app/src/SettingsView.cpp");
    CHECK(contains(settings, "audioEngine.getRenderTelemetry()"));
    CHECK(contains(settings, "render.renderWorstOverBlockPercent"));
}

TEST_CASE("the guided low-latency search only moves through the settings path",
          "[latency][profile]") {
    const auto cpp = readSource("app/src/AudioEngine.cpp");
    const auto tick = functionBody(cpp, "AudioEngine::LatencyProfileState AudioEngine::tickLatencyProfile");
    // Every step is an ordinary Apply, so a refused buffer still restores the
    // previous one through the existing safety net.
    CHECK(contains(tick, "applyDeviceSettings(settings)"));
    CHECK(contains(tick, "latencyProfile_.onApplied("));
    CHECK(contains(tick, "takeDeviceOpenErrors()"));
    CHECK(contains(tick, "pluginrack::LatencyProfile::Action::Revert"));

    const auto start = functionBody(cpp, "void AudioEngine::startLatencyProfile");
    CHECK(contains(start, "latencyProfile_.start("));

    const auto settings = readSource("app/src/SettingsView.cpp");
    CHECK(contains(settings, "audioEngine.isLatencyProfileActive()"));
    CHECK(contains(settings, "audioEngine.startLatencyProfile()"));
    CHECK(contains(settings, "audioEngine.cancelLatencyProfile()"));
    CHECK(contains(settings, "audioEngine.tickLatencyProfile()"));
}

TEST_CASE("a non-clock output keeps the monitor FIFO depth", "[latency][outputs]") {
    const auto cpp = readSource("app/src/AudioEngine.cpp");
    const auto open = functionBody(cpp, "juce::String AudioEngine::setOutputDeviceImpl");
    CHECK(contains(open, "lowLatencyOutputs_.load(std::memory_order_relaxed) ? 2 : 4"));
    // The retired stream-output class no longer deepens any output.
    CHECK_FALSE(contains(open, "streamOutput"));
    const auto scene = readSource("core/include/pluginrack/Scene.h");
    CHECK_FALSE(contains(scene, "bool streamOutput"));
    const auto codec = readSource("core/src/SceneCodec.cpp");
    CHECK_FALSE(contains(codec, "j.value(\"streamOutput\""));
}

TEST_CASE("audio callbacks register with MMCSS and disable denormals",
          "[latency][rt]") {
    const auto device = readSource("app/src/DeviceStream.cpp");
    const auto input = functionBody(device, "void InputCapture::audioDeviceIOCallbackWithContext");
    const auto output = functionBody(device, "void OutputPlayback::audioDeviceIOCallbackWithContext");
    for (const auto& body : {input, output}) {
        CHECK(contains(body, "realtime::registerProAudioThread()"));
        CHECK(contains(body, "juce::ScopedNoDenormals"));
    }
    const auto helper = readSource("app/src/RealtimeThread.h");
    CHECK(contains(helper, "AvSetMmThreadCharacteristicsW"));
    CHECK(contains(helper, "thread_local Registration"));
}

TEST_CASE("the clock FIFO is trimmed to its running depth after the hand-off",
          "[underrun][latency]") {
    const auto device = readSource("app/src/DeviceStream.cpp");
    const auto header = readSource("app/src/DeviceStream.h");
    CHECK(contains(header, "void setTargetFillBlocks(int blocks)"));
    CHECK(contains(header, "std::atomic<bool> trimToTarget"));
    const auto setter = functionBody(device, "void OutputPlayback::setTargetFillBlocks");
    CHECK(contains(setter, "targetFillFrames.store(wanted"));
    CHECK(contains(setter, "trimToTarget.store(true"));

    // The trim must happen on the device callback (the only consumer) before
    // the new block is pushed, so no second thread ever touches the FIFO.
    const auto callback =
        functionBody(device, "void OutputPlayback::audioDeviceIOCallbackWithContext");
    CHECK(contains(callback, "trimToTarget.exchange(false"));
    const auto trim = callback.find("trimToTarget.exchange");
    const auto push = callback.find("renderHook(numSamples)");
    CHECK(trim != std::string::npos);
    CHECK(push != std::string::npos);
    CHECK(trim < push);

    // A synchronous open primes four blocks; the hand-off is where that
    // latency is given back — to zero, because the block pushed in a callback
    // is the block that same callback plays.
    const auto engine = readSource("app/src/AudioEngine.cpp");
    const auto open = functionBody(engine, "juce::String AudioEngine::setOutputDeviceImpl");
    const auto handoff = open.find("out->output = std::move(newPlayback);");
    REQUIRE(handoff != std::string::npos);
    const auto settle = open.find("setTargetFillBlocks(0)", handoff);
    const auto activate = open.find("setRenderActive(true)", handoff);
    REQUIRE(settle != std::string::npos);
    REQUIRE(activate != std::string::npos);
    CHECK(settle < activate);
}

TEST_CASE("an ASIO open failure explains the single-client rule", "[latency][devices]") {
    const auto device = readSource("app/src/DeviceStream.cpp");
    const auto open = functionBody(device, "juce::String openWithPreferredType");
    // The most common ASIO failure is a second client on a driver that only
    // accepts one; the raw driver error does not say that.
    CHECK(contains(open, "if (chosenTypeName == kTypeAsio)"));
    CHECK(contains(open, "ASIO interfaces accept one client"));
    // ASIO must still be reachable end to end.
    CHECK(contains(device, "createAudioIODeviceType_ASIO"));
    CHECK(contains(device, "pluginrack::deviceTypeOrder(role, preferExclusive, asioAvailable())"));
}

TEST_CASE("the duplex clock carries its captures in one callback", "[latency][duplex]") {
    const auto device = readSource("app/src/DeviceStream.cpp");
    const auto header = readSource("app/src/DeviceStream.h");

    // One driver client, both directions.
    CHECK(contains(header, "int duplexInputChannels = 0"));
    const auto openWithType = functionBody(device, "juce::String openWithPreferredType");
    CHECK(contains(openWithType, "if (duplexInputChannels > 0)"));
    CHECK(contains(openWithType, "wantInput = true"));

    // Capture is handed to the engine before it renders, and the callback
    // returns without touching the FIFO.
    const auto callback =
        functionBody(device, "void OutputPlayback::audioDeviceIOCallbackWithContext");
    const auto feed = callback.find("duplexInputHook(inputChannelData, numInputChannels)");
    const auto render = callback.find("renderHook(numSamples)");
    const auto earlyReturn = callback.find("fillFrames.store(0, std::memory_order_relaxed)");
    REQUIRE(feed != std::string::npos);
    REQUIRE(render != std::string::npos);
    REQUIRE(earlyReturn != std::string::npos);
    CHECK(feed < render);
    CHECK(render < earlyReturn);
    // The render result is already in the device buffer: clearing it after the
    // hook would erase the audio.
    CHECK(contains(callback, "if (!rendered) {"));

    // push() writes straight through instead of queueing.
    const auto push = functionBody(device, "void OutputPlayback::push");
    CHECK(contains(push, "if (duplex_.load(std::memory_order_relaxed))"));
    CHECK(contains(push, "currentOutputChannels[ch]"));
    CHECK(contains(push, "std::memcpy(dst, src.getReadPointer(srcCh)"));

    // Engine: no second client, and the channel reads the callback's buffers.
    const auto engine = readSource("app/src/AudioEngine.cpp");
    const auto inputOpen = functionBody(engine, "juce::String AudioEngine::setChannelInputDeviceImpl");
    CHECK(contains(inputOpen, "duplexActive_.load(std::memory_order_acquire)"));
    CHECK(contains(inputOpen, "deviceName == duplexDeviceName_"));
    CHECK(contains(inputOpen, "c->duplexInput = true"));
    // A capture already holding the endpoint must be released before the
    // duplex open, or a single-client driver refuses it.
    const auto clockOpen = functionBody(engine, "juce::String AudioEngine::setOutputDeviceImpl");
    CHECK(contains(clockOpen, "if (c->inputDeviceName == deviceName) c->input.reset();"));
    CHECK(contains(clockOpen, "inputCaptures_.erase(it)"));
    const auto renderBody = functionBody(engine, "void AudioEngine::render(int frames)");
    CHECK(contains(renderBody, "if (channel->duplexInput.get())"));
    CHECK(contains(renderBody, "pullDuplexInput(*channel, frames)"));
    const auto pull = functionBody(engine, "void AudioEngine::pullDuplexInput");
    CHECK(contains(pull, "duplexInputBuffers_[source]"));
    CHECK(contains(pull, "channel.inputChannels"));
}

TEST_CASE("a refused duplex open falls back to the split path", "[latency][duplex]") {
    const auto engine = readSource("app/src/AudioEngine.cpp");
    const auto open = functionBody(engine, "juce::String AudioEngine::setOutputDeviceImpl");
    const auto fallback = open.find("if (openErr.isNotEmpty() && duplexInputs > 0)");
    REQUIRE(fallback != std::string::npos);
    // Exactly one retry, and it is the output-only variant.
    const auto retry = open.find("0);", fallback);
    CHECK(retry != std::string::npos);
    CHECK(contains(open, "Falling back keeps the clock alive"));
}

TEST_CASE("the audio backend is visible on the strips and in the budget",
          "[latency][ui]") {
    const auto helper = readSource("app/src/DeviceBackend.h");
    CHECK(contains(helper, "\"ASIO\""));
    CHECK(contains(helper, "\"WASAPI\""));
    CHECK(contains(helper, "deviceBackendIsAsio"));

    // Per-strip tag: the device line must name the backend, not only the device.
    const auto control = readSource("app/src/DeviceControl.cpp");
    CHECK(contains(control, "deviceBackendTag(backendType)"));
    CHECK(contains(control, "deviceBackendIsAsio(backendType)"));
    const auto controlHeader = readSource("app/src/DeviceControl.h");
    CHECK(contains(controlHeader, "void setBackendType("));

    // Every strip that shows a device must also set its backend.
    for (const char* path : {"app/src/ChannelStrip.cpp", "app/src/MixerComponent.cpp"}) {
        const auto cpp = readSource(path);
        CHECK(contains(cpp, "setBackendType(deviceState.deviceType)"));
        CHECK(contains(cpp, "setDeviceName(deviceState.deviceName)"));
    }

    // The engine exposes it and the Settings clock line prints it.
    const auto engine = readSource("app/src/AudioEngine.cpp");
    CHECK(contains(engine, "getActiveDeviceType()"));
    CHECK(contains(engine, "shortDeviceTypeTag(o->outputDeviceType)"));
    const auto budget = readSource("app/src/LatencyBudgetView.h");
    CHECK(contains(budget, "deviceBackendTag(report_.clockDeviceType)"));
}

TEST_CASE("Force ASIO picks the interface and reports what it did", "[latency][asio]") {
    const auto engine = readSource("app/src/AudioEngine.cpp");
    const auto force = functionBody(engine, "juce::String AudioEngine::forceAsio");
    // It must never guess: no ASIO driver, no clock, or no convincing name all
    // end in an explanation instead of a random device switch.
    CHECK(contains(force, "if (asio.isEmpty())"));
    CHECK(contains(force, "if (clockId == 0)"));
    CHECK(contains(force, "if (match.empty())"));
    CHECK(contains(force, "bestAsioDeviceMatch"));
    // Arming duplex must not start an async device job: setOutputDevice refuses
    // to run while one is in flight, which left the clock on WASAPI.
    CHECK(contains(force, "applySettingsValues(settings)"));
    CHECK_FALSE(contains(force, "applyDeviceSettings(settings)"));
    // The capture strips follow the clock, or duplex never engages.
    CHECK(contains(force, "retargetInputsToClockDevice(target)"));
    const auto retarget = force.find("retargetInputsToClockDevice(target)");
    const auto moveClock = force.find("setOutputDevice(clockId, target)");
    REQUIRE(retarget != std::string::npos);
    REQUIRE(moveClock != std::string::npos);
    CHECK(retarget < moveClock);
    CHECK(contains(force, "ASIO refused: "));

    // The same adoption must happen when the clock is picked in the device
    // menu, not only from the button.
    const auto setOutput = functionBody(engine, "juce::String AudioEngine::setOutputDevice(");
    CHECK(contains(setOutput, "if (isClock && fullDuplexInput_.load(std::memory_order_relaxed))"));
    CHECK(contains(setOutput, "retargetInputsToClockDevice(deviceName, &previous)"));
    // A refused clock must put the captures back, or those strips stay flagged
    // duplex with nothing feeding them.
    CHECK(contains(setOutput, "if (!error.isEmpty() && retargeted > 0)"));
    CHECK(contains(setOutput, "c->duplexInput = false"));
    CHECK(contains(setOutput, "prepareInputs()"));
    const auto adopt = functionBody(engine, "int AudioEngine::retargetInputsToClockDevice");
    CHECK(contains(adopt, "inputsToRetargetForClock("));
    CHECK(contains(adopt, "c->duplexInput = true"));
    CHECK(contains(adopt, "c->input.reset()"));

    const auto enumeration = readSource("app/src/DeviceStream.cpp");
    CHECK(contains(enumeration, "juce::StringArray asioDevices()"));
    CHECK(contains(enumeration, "createAudioIODeviceType_ASIO"));

    const auto settings = readSource("app/src/SettingsView.cpp");
    CHECK(contains(settings, "audioEngine.forceAsio()"));
    CHECK(contains(settings, "forceAsioButton.setEnabled"));
    CHECK(contains(settings, "No ASIO driver installed"));
}

TEST_CASE("only one GUI session runs, but CLI children are never blocked",
          "[app][instance]") {
    const auto main = readSource("app/src/Main.cpp");
    const auto app = functionBody(main, "class PluginRackApplication final : public juce::JUCEApplication");
    // The GUI takes the single-instance lock.
    CHECK(contains(app, "bool moreThanOneInstanceAllowed() override"));
    CHECK(contains(app, "return false;"));
    // ...but the scanner and the CLI switches must still start alongside it,
    // otherwise plugin scanning breaks while the app is open.
    CHECK(contains(app, "getCommandLineParameterArray()"));
    CHECK(contains(app, "arg == \"--scan-plugin\""));
    CHECK(contains(app, "return true;"));
    // A second GUI launch focuses the running window (possibly from the tray).
    CHECK(contains(app, "void anotherInstanceStarted(const juce::String& commandLine) override"));
    CHECK(contains(app, "mainWindow->setVisible(true)"));
    CHECK(contains(app, "mainWindow->toFront(true)"));

    // Every headless mode the child processes rely on is still handled here.
    for (const char* flag : {"--scan-plugin", "--list-plugins", "--list-devices", "--screenshot"}) {
        CHECK(contains(main, flag));
    }
}

TEST_CASE("an Auto input buffer follows the clock instead of the driver's mood",
          "[latency][input]") {
    const auto engine = readSource("app/src/AudioEngine.cpp");
    const auto open = functionBody(engine, "juce::String AudioEngine::setChannelInputDeviceImpl");
    // Auto must resolve to the render block, and a device that refuses that
    // size must fall back to its own default rather than leaving the strip
    // silent.
    CHECK(contains(open, "pluginrack::inputBufferRequest(requestedInputBufferSize_, renderBlockFrames)"));
    CHECK(contains(open, "err, bufferRequest,"));
    CHECK(contains(open, "requestedInputBufferSize_ == 0 && bufferRequest > 0"));
    CHECK(contains(open, "err, 0,"));

    const auto policy = readSource("core/include/pluginrack/InputBufferPolicy.h");
    CHECK(contains(policy, "requestedBufferSize > 0 ? requestedBufferSize"));
}

TEST_CASE("an Auto output buffer settles on the device minimum and keeps it",
          "[latency][output]") {
    const auto device = readSource("app/src/DeviceStream.cpp");
    const auto open = functionBody(device, "juce::String openWithPreferredType");
    // Auto must not leave the driver free to change the period between launches.
    CHECK(contains(open, "cachedBufferFor(selectedDeviceName)"));
    CHECK(contains(open, "pluginrack::smallestSupportedBufferSize(available, current, 0)"));
    CHECK(contains(open, "manager.setAudioDeviceSetup(liveSetup, true)"));
    CHECK(contains(open, "rememberBufferFor(selectedDeviceName"));
    // A refused size keeps the opened stream rather than failing the strip.
    CHECK(contains(open, "if (manager.setAudioDeviceSetup(liveSetup, true).isEmpty())"));

    const auto policy = readSource("core/include/pluginrack/OutputBufferPolicy.h");
    CHECK(contains(policy, "if (requestedBufferSize > 0) return requestedBufferSize"));
    CHECK(contains(policy, "smallest == 0 || size < smallest"));
}

TEST_CASE("the budget says when capture and playback are different devices",
          "[latency][duplex]") {
    const auto engine = readSource("app/src/AudioEngine.cpp");
    const auto report = functionBody(engine, "AudioEngine::LatencyReport AudioEngine::getLatencyReport");
    CHECK(contains(report, "report.separateClocks"));
    CHECK(contains(report, "!inputChannel->duplexInput.get()"));
    const auto header = readSource("app/src/AudioEngine.h");
    CHECK(contains(header, "bool separateClocks"));

    // ...and the panel explains what the read delay is for.
    const auto view = readSource("app/src/LatencyBudgetView.h");
    CHECK(contains(view, "report_.separateClocks"));
    CHECK(contains(view, "bridges two clocks"));

    // The button can move the clock onto the input's driver, which is the only
    // way to drop that delay.
    const auto force = functionBody(engine, "juce::String AudioEngine::forceAsio");
    CHECK(contains(force, "asioClockFromInputs(inputNames, names)"));
    CHECK(contains(force, "Clock moved onto "));
    CHECK(contains(force, "different devices"));
}

TEST_CASE("input menus are sized by the device, not a fixed 16", "[app][ui]") {
    for (const char* path : {"app/src/RackView.cpp", "app/src/ChannelStrip.cpp"}) {
        const auto cpp = readSource(path);
        CHECK_FALSE(contains(cpp, "juce::jlimit(1, 16"));
        CHECK_FALSE(contains(cpp, "for (int i = 1; i <= 16; ++i)"));
        CHECK(contains(cpp, "rebuildInputCombos"));
        CHECK(contains(cpp, "deviceState.availableInputs"));
    }
}

TEST_CASE("latency report splits every stage and reports the unexplained part",
          "[latency][ui]") {
    const auto cpp = readSource("app/src/AudioEngine.cpp");
    const auto report = functionBody(cpp, "AudioEngine::LatencyReport AudioEngine::getLatencyReport");
    // The capture and playback totals each fold a queue into the driver figure;
    // the budget has to show them apart or the user cannot tell which stage to
    // shrink.
    CHECK(contains(report, "inputReadDelaySamples = inputChannel->input->getReadDelaySamples()"));
    CHECK(contains(report, "report.inputFifoDelayMs ="));
    CHECK(contains(report, "report.inputDeviceLatencyMs ="));
    CHECK(contains(report, "report.outputBufferMs ="));
    CHECK(contains(report, "report.outputDeviceLatencyMs ="));
    CHECK(contains(report, "clockTargetFillFrames = output->output->getTargetFillFrames()"));
    CHECK(contains(report, "report.unexpectedMs = report.measuredValid"));

    const auto header = readSource("app/src/AudioEngine.h");
    for (const char* field : {"inputDeviceLatencyMs", "inputFifoDelayMs", "outputDeviceLatencyMs",
                              "outputBufferMs", "unexpectedMs", "inputReadDelayFrames",
                              "clockFillFrames", "clockTargetFillFrames"}) {
        CHECK(contains(header, field));
    }

    // Settings must render the per-stage view, not only the single estimate.
    const auto settings = readSource("app/src/SettingsView.cpp");
    CHECK(contains(settings, "budgetView.setReport(report)"));
    CHECK(contains(settings, "LatencyBudgetView::preferredHeight()"));
    const auto view = readSource("app/src/LatencyBudgetView.h");
    CHECK(contains(view, "Predicted"));
    CHECK(contains(view, "Unexplained"));
    CHECK(contains(view, "report_.unexpectedMs"));
}

TEST_CASE("a duplex open names the endpoint for output as well as input", "[latency][duplex]") {
    const auto source = readSource("app/src/DeviceStream.cpp");
    const auto open = functionBody(source, "juce::String openWithPreferredType");
    // The duplex branch flips wantInput to true, so an open that names only the
    // input endpoint silently leaves the ASIO device without output channels
    // (M-Audio Duo: clock streams but nothing is audible).
    CHECK(contains(open, "setup.outputDeviceName = selectedDeviceName"));
    CHECK(contains(open, "typeSetup.setAttribute(\"audioOutputDeviceName\", selectedDeviceName)"));
}

TEST_CASE("a driver negotiated buffer is accepted when it is supported", "[latency][duplex]") {
    const auto source = readSource("app/src/DeviceStream.cpp");
    const auto open = functionBody(source, "juce::String openWithPreferredType");
    // The M-Audio Duo reports its preferred block (480) even when asked for 32.
    // Requiring an exact match errored Apply; Force ASIO sidestepped it. Keep
    // the device when the driver negotiated a supported size instead.
    CHECK(contains(open, "getAvailableBufferSizes()"));
    CHECK(contains(open, "The device does not support"));
    // The old strict check `device->getCurrentBufferSizeSamples() != requestedBufferSize`
    // must be gone for duplex; the new check supports negotiated sizes.
    CHECK_FALSE(contains(open,
                         "device->getCurrentBufferSizeSamples() != requestedBufferSize) {"));
}
