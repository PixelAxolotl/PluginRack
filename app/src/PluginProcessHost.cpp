#include "PluginProcessHost.h"

#include <windows.h>
#include <appmodel.h>

#include <UserEnv.h>

namespace {

// Best effort: create the AppContainer profile once. "Already exists" is a
// success: the profile is a per-user identity, not a per-run resource. The
// child still runs with the user token (see the fault containment spec §6), so
// this is profile preparation only, not a sandbox.
bool ensureAppContainer() {
    const HRESULT hr = CreateAppContainerProfile(
        L"PixelAxolotl.PluginRack.PluginHost", nullptr, nullptr, nullptr, 0, nullptr);
    return SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
}

juce::String exitCodeText(int code) {
    return "0x" + juce::String::toHexString(code).paddedLeft('0', 8).toUpperCase();
}

// Pulls one value out of the child's one-line JSON reply.
juce::var jsonValue(const juce::String& json, const juce::Identifier& key) {
    const auto parsed = juce::JSON::parse(json);
    if (auto* object = parsed.getDynamicObject()) return object->getProperty(key);
    return {};
}

} // namespace

PluginProcessHost::PluginProcessHost() = default;

PluginProcessHost::~PluginProcessHost() {
    killAndReap();
}

bool PluginProcessHost::start(const juce::File& file, int pluginUniqueId, double sampleRate,
                              int blockSize, juce::String& error) {
    (void) ensureAppContainer();
    static std::atomic<bool> tokenNoticeLogged{false};
    if (!tokenNoticeLogged.exchange(true, std::memory_order_relaxed)) {
        juce::Logger::writeToLog("plugin: child hosts run with the normal user token "
                                 "(process isolation only; AppContainer launch is not wired up)");
    }

    const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    if (!executable.existsAsFile()) {
        error = "The plugin host process is missing. No plugin was loaded.";
        return false;
    }

    pluginPath = file.getFullPathName();
    if (!openChannel(file)) {
        error = "Could not create the plugin host channels. No plugin was loaded.";
        return false;
    }

    // The mapping name goes before the state so the empty-state placeholder
    // keeps every later argument in a fixed position (an empty argument would
    // be dropped by the command-line split and shift them by one).
    //
    // The saved state is NOT passed here: base64 state can reach 1 MiB and
    // Windows caps a command line at 32767 characters. It travels as the first
    // setState control request once the child reports HOSTREADY.
    const juce::StringArray arguments {
        executable.getFullPathName(), "--plugin-host", file.getFullPathName(),
        juce::String(pluginUniqueId), sharedName, "-", juce::String(sampleRate, 3),
        juce::String(blockSize)};
    child = std::make_unique<juce::ChildProcess>();
    if (!child->start(arguments, juce::ChildProcess::wantStdOut)) {
        error = "The plugin host process could not start. No plugin was loaded.";
        child.reset();
        killAndReap();
        return false;
    }

    launched = true;
    hostDead.store(false, std::memory_order_relaxed);
    stopWatcher.store(false, std::memory_order_relaxed);
    startWatcher();
    juce::Logger::writeToLog("plugin: child host started for " + file.getFileName());
    return true;
}

bool PluginProcessHost::openChannel(const juce::File& file) {
    // Per hosted instance, not per plugin file: a scene can use the same VST3
    // in several slots, and two children must never share one block.
    static std::atomic<unsigned> channelCounter{0};
    sharedName = pluginHostIpc::sharedNameFor(file.getFullPathName()) + "-"
                 + juce::String(static_cast<int>(++channelCounter));

    if (!audioSection.create(sharedName.toWideCharPointer(),
                             sizeof(pluginHostIpc::SharedAudioBlock))) {
        juce::Logger::writeToLog("plugin: audio channel '" + sharedName + "' unavailable (err "
                                 + juce::String(static_cast<int>(audioSection.error())) + ")");
        return false;
    }
    if (!paramSection.create((sharedName + "-param").toWideCharPointer(),
                             sizeof(pluginHostIpc::ParamSection))) {
        juce::Logger::writeToLog("plugin: param channel '" + sharedName + "' unavailable (err "
                                 + juce::String(static_cast<int>(paramSection.error())) + ")");
        audioSection.close();
        return false;
    }
    if (!openControlChannel()) {
        audioSection.close();
        paramSection.close();
        return false;
    }

    auto* block = static_cast<pluginHostIpc::SharedAudioBlock*>(audioSection.data());
    block->sequence = 0;
    block->responseSequence = 0;
    block->writeSequence = 0;
    block->readSequence = 0;
    block->ackSequence = 0;
    block->blockSequence = 0;
    block->lastSequence = 0;
    block->numFrames = 0;
    block->bufferSize = 0;
    block->sampleRate = 0.0;
    block->latencySamples = 0;

    const auto requestName = pluginHostIpc::requestEventNameFor(sharedName);
    const auto responseName = pluginHostIpc::responseEventNameFor(sharedName);
    if (!requestEvent.create(requestName.toWideCharPointer())
        || !responseEvent.create(responseName.toWideCharPointer())) {
        juce::Logger::writeToLog("plugin: audio events '" + requestName + "', '" + responseName
                                 + "' unavailable (err "
                                 + juce::String(static_cast<int>(requestEvent.error())) + "/"
                                 + juce::String(static_cast<int>(responseEvent.error())) + ")");
        return false;
    }

    juce::Logger::writeToLog("plugin: audio channel '" + sharedName + "' ready ("
                             + juce::String(static_cast<int>(sizeof(
                                   pluginHostIpc::SharedAudioBlock)))
                             + " bytes)");
    return true;
}

bool PluginProcessHost::openControlChannel() {
    if (!controlSection.create((sharedName + "-ctl").toWideCharPointer(),
                               sizeof(pluginHostIpc::ControlSection))) {
        juce::Logger::writeToLog("plugin: control channel '" + sharedName + "' unavailable (err "
                                 + juce::String(static_cast<int>(controlSection.error())) + ")");
        return false;
    }
    const auto requestName = pluginHostIpc::controlRequestEventNameFor(sharedName);
    const auto responseName = pluginHostIpc::controlResponseEventNameFor(sharedName);
    if (!controlRequestEvent.create(requestName.toWideCharPointer())
        || !controlResponseEvent.create(responseName.toWideCharPointer())) {
        juce::Logger::writeToLog("plugin: control events '" + requestName + "', '" + responseName
                                 + "' unavailable (err "
                                 + juce::String(static_cast<int>(controlRequestEvent.error())) + "/"
                                 + juce::String(static_cast<int>(controlResponseEvent.error()))
                                 + ")");
        return false;
    }
    return true;
}

bool PluginProcessHost::waitReady(int timeoutMs, juce::String& error) {
    if (!launched || child == nullptr) {
        error = "The plugin host process is not running.";
        return false;
    }
    if (controlSection.data() == nullptr) {
        error = "The plugin host control channel is not open.";
        return false;
    }

    // Poll the shared ready flag, never stdout: reading the child's output pipe
    // blocks when there is nothing buffered, which would freeze the caller (see
    // ControlSection::hostReady).
    const auto* control = static_cast<const pluginHostIpc::ControlSection*>(controlSection.data());
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    for (;;) {
        const auto state = control->hostReady.load(std::memory_order_acquire);
        if (state == pluginHostIpc::kHostReadyLoaded) {
            ready = true;
            return true;
        }
        if (state == pluginHostIpc::kHostReadyFailed) {
            error = "The plugin host could not load '" + juce::File(pluginPath).getFileName()
                    + "'. " + outputAfterExit();
            return false;
        }
        if (isDead()) {
            error = "The plugin host exited before it was ready (exit " + exitCodeText(exitCode())
                    + "). " + outputAfterExit();
            return false;
        }
        if (juce::Time::getMillisecondCounterHiRes() >= deadline) {
            error = "The plugin host did not load '" + juce::File(pluginPath).getFileName()
                    + "' within " + juce::String(timeoutMs / 1000) + " s.";
            return false;
        }
        juce::Thread::sleep(20);
    }
}

bool PluginProcessHost::controlCall(pluginHostIpc::ControlOp op, const juce::String& requestJson,
                                    int timeoutMs, juce::String& responseJson,
                                    juce::String& error) {
    const juce::ScopedLock lock(controlLock_);
    if (!controlSection.isOpen() || !controlRequestEvent.isOpen()
        || !controlResponseEvent.isOpen()) {
        error = "The plugin host control channel is not open.";
        return false;
    }
    if (!isAlive()) {
        error = "The plugin host is not running.";
        return false;
    }

    auto* control = static_cast<pluginHostIpc::ControlSection*>(controlSection.data());
    auto request = control->requestSeq.load(std::memory_order_acquire);
    const auto response = control->responseSeq.load(std::memory_order_acquire);
    if (response != request) {
        // Recover (fix 4): a previous timeout left the sequences out of step.
        // Abandon the stale request and continue rather than poisoning forever.
        request = response;
        control->requestSeq.store(response, std::memory_order_release);
    }

    pluginHostIpc::controlBeginWrite(*control);
    const auto bytes = requestJson.getNumBytesAsUTF8();
    if (!pluginHostIpc::controlSetPayload(*control, requestJson.toRawUTF8(), bytes)) {
        pluginHostIpc::controlEndWrite(*control);
        error = "The control payload is larger than the channel.";
        return false;
    }
    control->op.store(static_cast<std::uint32_t>(op), std::memory_order_release);
    pluginHostIpc::controlEndWrite(*control);

    const auto next = request + 1;
    control->requestSeq.store(next, std::memory_order_release);
    controlRequestEvent.signal();

    const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    for (;;) {
        if (control->responseSeq.load(std::memory_order_acquire) == next) break;
        if (isDead()) {
            error = "The plugin host exited during a control request (exit "
                    + exitCodeText(exitCode()) + ")";
            return false;
        }
        const double remaining = deadline - juce::Time::getMillisecondCounterHiRes();
        if (remaining <= 0.0) {
            // Recover the sequence (fix 4): abandon the in-flight request so the
            // next control call is not poisoned with "still answering previous".
            control->requestSeq.store(control->responseSeq.load(std::memory_order_acquire),
                                     std::memory_order_release);
            error = juce::String("The plugin host did not answer '")
                    + pluginHostIpc::controlOpName(op) + "' within " + juce::String(timeoutMs)
                    + " ms.";
            return false;
        }
        controlResponseEvent.wait(static_cast<unsigned long>(juce::jmax(1.0, remaining)));
    }

    if (!pluginHostIpc::controlPayloadTerminated(*control)) {
        error = "The plugin host sent a malformed control reply.";
        return false;
    }
    responseJson = juce::String::fromUTF8(control->payload);
    if (control->status.load(std::memory_order_acquire)
        != static_cast<std::uint32_t>(pluginHostIpc::ControlStatus::ok)) {
        error = jsonValue(responseJson, "error").toString();
        if (error.isEmpty()) error = "The plugin host refused the request.";
        return false;
    }
    return true;
}

bool PluginProcessHost::setState(const juce::String& stateBase64, int timeoutMs,
                                 juce::String& error) {
    if (stateBase64.isEmpty()) return true;
    auto* object = new juce::DynamicObject();
    object->setProperty("state", stateBase64);
    juce::String reply;
    const bool ok =
        controlCall(pluginHostIpc::ControlOp::setState, juce::JSON::toString(juce::var(object), true),
                    timeoutMs, reply, error);
    if (ok) lastKnownState_ = stateBase64;
    return ok;
}

juce::String PluginProcessHost::getState(int timeoutMs, juce::String& error) {
    juce::String reply;
    if (!controlCall(pluginHostIpc::ControlOp::getState, "{}", timeoutMs, reply, error)) {
        return {};
    }
    const auto state = jsonValue(reply, "state").toString();
    if (state.isNotEmpty()) lastKnownState_ = state;
    return state;
}

bool PluginProcessHost::prepare(double sampleRate, int blockSize, int timeoutMs,
                                juce::String& error) {
    auto* object = new juce::DynamicObject();
    object->setProperty("sampleRate", sampleRate);
    object->setProperty("blockSize", blockSize);
    juce::String reply;
    return controlCall(pluginHostIpc::ControlOp::prepare,
                       juce::JSON::toString(juce::var(object), true), timeoutMs, reply, error);
}

int PluginProcessHost::latencySamples(int timeoutMs) {
    juce::String reply;
    juce::String error;
    if (!controlCall(pluginHostIpc::ControlOp::getLatency, "{}", timeoutMs, reply, error)) {
        return -1;
    }
    return static_cast<int>(jsonValue(reply, "latencySamples").toString().getIntValue());
}

bool PluginProcessHost::submitBlock(const juce::AudioBuffer<float>& input, int numSamples,
                                    double sampleRate, int blockSize) {
    if (audioSection.data() == nullptr || !isAlive()) return false;

    auto* block = static_cast<pluginHostIpc::SharedAudioBlock*>(audioSection.data());
    const auto previous = block->sequence;
    if (block->responseSequence != previous) return false; // host still lagging

    const int frames = juce::jmin(numSamples, input.getNumSamples(), pluginHostIpc::kMaxBlockFrames);
    if (frames <= 0) return false;
    block->blockSequence = block->blockSequence + 1; // odd: writing
    block->numFrames = frames;
    block->bufferSize = blockSize;
    block->sampleRate = sampleRate;
    const auto* left = input.getReadPointer(0);
    const auto* right = input.getNumChannels() > 1 ? input.getReadPointer(1) : left;
    for (int i = 0; i < frames; ++i) {
        block->interleave[i * 2] = left[i];
        block->interleave[i * 2 + 1] = right[i];
    }
    block->blockSequence = block->blockSequence + 1; // even: committed
    block->writeSequence = block->writeSequence + 1;
    const auto next = previous + 1;
    block->sequence = next;
    lastSubmitted = next;
    requestEvent.signal();
    return true;
}

bool PluginProcessHost::fetchBlock(juce::AudioBuffer<float>& output, int deadlineMs) {
    if (audioSection.data() == nullptr) return false;

    auto* block = static_cast<pluginHostIpc::SharedAudioBlock*>(audioSection.data());
    if (!responseEvent.isOpen()) return false;

    const auto expected = lastSubmitted;
    const auto start = juce::Time::getMillisecondCounterHiRes();
    while (block->responseSequence != expected) {
        const double remaining = deadlineMs - (juce::Time::getMillisecondCounterHiRes() - start);
        if (remaining <= 0.0) return false;
        if (!responseEvent.wait(static_cast<unsigned long>(juce::jmax(1.0, remaining)))) {
            if (block->responseSequence == expected) break;
            return false;
        }
    }

    const auto before = block->blockSequence;
    const int frames = juce::jmin(block->numFrames, pluginHostIpc::kMaxBlockFrames);
    if (output.getNumChannels() < 2 || output.getNumSamples() < frames) {
        return false; // the caller must pre-size this; never reallocate here
    }
    auto* left = output.getWritePointer(0);
    auto* right = output.getWritePointer(1);
    for (int i = 0; i < frames; ++i) {
        left[i] = block->interleave[i * 2];
        right[i] = block->interleave[i * 2 + 1];
    }
    const auto after = block->blockSequence;
    if (before != after || (before % 2) == 1) return false; // torn read
    return true;
}

bool PluginProcessHost::setParameter(std::uint32_t parameterIndex, float value,
                                     std::uint32_t flags) {
    if (paramSection.data() == nullptr) return false;
    auto* params = static_cast<pluginHostIpc::ParamSection*>(paramSection.data());
    pluginHostIpc::ParamMsg msg;
    msg.id = parameterIndex;
    msg.value = value;
    msg.flags = flags;
    return pluginHostIpc::paramRingPush(params->toHost, msg);
}

PluginProcessHost::Peaks PluginProcessHost::peaks() const {
    Peaks result;
    if (audioSection.data() == nullptr) return result;
    const auto* block = static_cast<const pluginHostIpc::SharedAudioBlock*>(audioSection.data());
    result.inL = block->peakInL;
    result.inR = block->peakInR;
    result.outL = block->peakOutL;
    result.outR = block->peakOutR;
    return result;
}

bool PluginProcessHost::isAlive() const {
    return launched && child != nullptr && !hostDead.load(std::memory_order_acquire);
}

bool PluginProcessHost::isDead() const {
    return hostDead.load(std::memory_order_acquire);
}

int PluginProcessHost::exitCode() const {
    return child != nullptr ? static_cast<int>(child->getExitCode()) : -1;
}

int PluginProcessHost::childProcessId() const {
    if (controlSection.data() == nullptr) return 0;
    const auto* control = static_cast<const pluginHostIpc::ControlSection*>(controlSection.data());
    return static_cast<int>(control->hostPid.load(std::memory_order_acquire));
}

juce::String PluginProcessHost::outputAfterExit() {
    // Only safe once the child is gone: readProcessOutput blocks while the pipe
    // is still open and empty, so a live child would hang the caller.
    if (child == nullptr) return {};
    if (child->isRunning()) {
        child->kill();
        child->waitForProcessToFinish(1000);
    }
    return child->readAllProcessOutput().trim();
}

void PluginProcessHost::startWatcher() {
    watcher = std::thread([this] {
        while (!stopWatcher.load(std::memory_order_relaxed)) {
            if (child == nullptr || !child->isRunning()) {
                hostDead.store(true, std::memory_order_release);
                return;
            }
            juce::Thread::sleep(100);
        }
    });
}

void PluginProcessHost::stopWatcherThread() {
    stopWatcher.store(true, std::memory_order_relaxed);
    if (watcher.joinable()) watcher.join();
}

void PluginProcessHost::killAndReap() {
    stopWatcherThread();
    if (child != nullptr && child->isRunning()) {
        child->kill();
        child->waitForProcessToFinish(5000);
    }
    child.reset();
    audioSection.close();
    paramSection.close();
    controlSection.close();
    requestEvent.close();
    responseEvent.close();
    controlRequestEvent.close();
    controlResponseEvent.close();
    launched = false;
    ready = false;
}
