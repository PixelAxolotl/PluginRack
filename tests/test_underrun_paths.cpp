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
    CHECK(contains(body, "targetFillFrames = bufferSize * juce::jmax(0, targetFillBlocks)"));
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
    // Read delay is configurable (1 or 2 blocks); the ring must still clamp.
    CHECK(contains(body, "blockFrames * juce::jmax(1, readDelayBlocks)"));
    CHECK(contains(body, "readCursor.prepare"));
}

TEST_CASE("input read delay ignores the probe capacity reserve", "[underrun][latency]") {
    const auto source = readSource("app/src/DeviceStream.cpp");
    const auto body = functionBody(source, "juce::String InputCapture::open");
    // The reserve for an unfinished clock probe may only grow the ring. It must
    // not reach the delay unit, or it is added to every input as real latency.
    CHECK(contains(body, "capacityUnit = juce::jmax(blockFrames, capacityReserveFrames)"));
    CHECK(contains(body,
                   "const int delay = juce::jlimit(0, maxDelay, "
                   "blockFrames * juce::jmax(1, readDelayBlocks))"));

    const auto engine = readSource("app/src/AudioEngine.cpp");
    CHECK(contains(engine, "renderBlockFrames = currentBufferSize"));
    CHECK(contains(engine,
                   "capacityReserveFrames = juce::jmax(currentBufferSize, "
                   "kProbeRenderBlockFrames)"));
}

TEST_CASE("low-latency input backs off only after a real starvation", "[underrun][latency]") {
    const auto source = readSource("app/src/DeviceStream.cpp");
    const auto body = functionBody(source, "bool InputCapture::pull");
    // The first one-block pull is allowed to prime without becoming a false
    // glitch; later starvation adds one block and keeps the cursor continuous.
    CHECK(contains(body, "written >= targetDelay + static_cast<std::uint64_t>(frames)"));
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
    CHECK(contains(device, "blockFrames * juce::jmax(1, readDelayBlocks)"));
    CHECK(contains(device, "preferExclusive"));
    CHECK(contains(device, "kTypeShared, kTypeLowLatency, kTypeExclusive"));
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
    // Every endpoint starts at once, so the progress text lists the whole
    // in-flight set instead of the last worker to start.
    CHECK(countOccurrences(pass, "beginDeviceOpen(status)") == 2);
    CHECK(countOccurrences(pass, "endDeviceOpen(status)") == 2);
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
    CHECK(contains(preferred, "getCurrentBufferSizeSamples() != requestedBufferSize"));
    CHECK(contains(preferred, "manager.closeAudioDevice()"));
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
    const auto header = readSource("app/src/PluginHost.h");
    const auto body = functionBody(readSource("app/src/PluginHost.cpp"),
                                   "void PluginHost::prepareToPlay");
    CHECK(contains(header, "bool prepared = false"));
    CHECK(contains(body, "const bool unchanged"));
    CHECK(contains(body, "if (plugin != nullptr && !unchanged)"));
    CHECK(contains(body, "plugin->releaseResources()"));
    CHECK(contains(body, "prepared = true"));
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

TEST_CASE("adding a VST never instantiates on the message thread", "[plugin][ui]") {
    const auto engine = readSource("app/src/AudioEngine.cpp");
    const auto add = functionBody(engine, "void AudioEngine::addPluginAsync");
    CHECK(contains(add, "PluginHost::loadFromFile"));
    CHECK(contains(add, "juce::Thread::launch"));
    CHECK(contains(add, "MessageManager::callAsync"));
    // Install happens back on the message thread, after the instance exists.
    const auto adopt = add.find("insert->pluginHost.adopt(loaded)");
    const auto post = add.find("MessageManager::callAsync");
    REQUIRE(adopt != std::string::npos);
    REQUIRE(post != std::string::npos);
    CHECK(post < adopt);

    // One format manager, serialised: the instantiation worker and the scene
    // loader must not use it concurrently.
    const auto host = readSource("app/src/PluginHost.cpp");
    CHECK(contains(host, "sharedFormatLock()"));
    CHECK(contains(host, "PluginHost::loadFromFile"));
    CHECK(contains(host, "plugin->prepareToPlay(currentSampleRate, currentBufferSize)"));

    const auto rack = readSource("app/src/RackView.cpp");
    const auto insert = functionBody(rack, "juce::String RackView::insertPlugin");
    CHECK(contains(insert, "addPluginAsync"));
    CHECK(contains(insert, "addOutputPluginAsync"));
    CHECK_FALSE(contains(insert, "audioEngine.addPlugin("));
}

TEST_CASE("crashes are logged with a backtrace", "[crash]") {
    const auto main = readSource("app/src/Main.cpp");
    CHECK(contains(main, "setApplicationCrashHandler"));
    CHECK(contains(main, "getStackBacktrace"));
    CHECK(contains(main, "crash-"));
    CHECK(contains(main, "setCurrentLogger"));
    const auto cmake = readSource("app/CMakeLists.txt");
    CHECK(contains(cmake, "/Zi"));
    CHECK(contains(cmake, "target_link_options(pluginrack_app PRIVATE /DEBUG)"));
}
