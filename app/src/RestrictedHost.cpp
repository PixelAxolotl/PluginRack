#include "RestrictedHost.h"

#include "PluginHost.h"
#include "PluginHostIpc.h"
#include "PluginPrepare.h"

#include <atomic>
#include <thread>

#include <tlhelp32.h>
#include <windows.h>

// The child process: the only place in the fault-containment design where
// plugin code runs with permission to die. It opens no devices (ASIO is single
// client on the reference rig), renders the blocks the app hands it, and
// answers control requests. See PluginProcessHost for the app side.

namespace {

DWORD parentProcessId() {
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = ::GetCurrentProcessId();
    DWORD parent = 0;
    if (::Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == self) {
                parent = entry.th32ParentProcessID;
                break;
            }
        } while (::Process32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return parent;
}

// The app exits with TerminateProcess (plugin teardown can hang forever), so it
// cannot send a quit message. Watch its process handle instead and end this
// process when it goes away; otherwise orphaned hosts keep the exe locked.
void terminateWhenParentExits() {
    const DWORD parentPid = parentProcessId();
    if (parentPid == 0) return;

    const HANDLE parent = ::OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (parent == nullptr) return;

    std::thread([parent] {
        ::WaitForSingleObject(parent, INFINITE);
        ::TerminateProcess(::GetCurrentProcess(), 0);
    }).detach();
}

juce::String errorReply(const juce::String& message) {
    auto* object = new juce::DynamicObject();
    object->setProperty("ok", false);
    object->setProperty("error", message);
    return juce::JSON::toString(juce::var(object), true);
}

juce::String okReply() {
    auto* object = new juce::DynamicObject();
    object->setProperty("ok", true);
    return juce::JSON::toString(juce::var(object), true);
}

juce::var parseObject(const juce::String& json) {
    const auto parsed = juce::JSON::parse(json);
    return parsed;
}

} // namespace

int RestrictedHost::runHost(const juce::File& file, int pluginUniqueId,
                            const juce::String& stateBase64, const juce::String& sharedMappingName,
                            double sampleRate, int blockSize) {
    terminateWhenParentExits();

    // The app hands over the mapping name; only fall back to recomputing it
    // when an older caller did not pass one.
    const auto mappingName = sharedMappingName.isNotEmpty()
                                 ? sharedMappingName
                                 : pluginHostIpc::sharedNameFor(file.getFullPathName());

    // Open every channel before loading the plugin: a host that cannot talk to
    // the app must say so instead of spending seconds loading a plugin whose
    // blocks, parameters and state it could never deliver.
    pluginHostIpc::NamedSection audioSection;
    if (!audioSection.open(mappingName.toWideCharPointer(),
                           sizeof(pluginHostIpc::SharedAudioBlock))) {
        std::cout << "HOSTERROR audio channel '" << mappingName.toStdString()
                  << "' unavailable (err " << audioSection.error() << ")\n";
        std::cout.flush();
        return 1;
    }
    auto* block = static_cast<pluginHostIpc::SharedAudioBlock*>(audioSection.data());

    pluginHostIpc::NamedSection paramSection;
    if (!paramSection.open(pluginHostIpc::paramSectionNameFor(mappingName).toWideCharPointer(),
                           sizeof(pluginHostIpc::ParamSection))) {
        std::cout << "HOSTERROR param channel '" << mappingName.toStdString()
                  << "' unavailable (err " << paramSection.error() << ")\n";
        std::cout.flush();
        return 1;
    }
    auto* params = static_cast<pluginHostIpc::ParamSection*>(paramSection.data());

    pluginHostIpc::NamedSection controlSection;
    if (!controlSection.open(pluginHostIpc::controlSectionNameFor(mappingName).toWideCharPointer(),
                             sizeof(pluginHostIpc::ControlSection))) {
        std::cout << "HOSTERROR control channel '" << mappingName.toStdString()
                  << "' unavailable (err " << controlSection.error() << ")\n";
        std::cout.flush();
        return 1;
    }
    auto* control = static_cast<pluginHostIpc::ControlSection*>(controlSection.data());
    // Record the pid before anything can go wrong, so a fault record always
    // names the process that died.
    control->hostPid.store(static_cast<std::uint32_t>(::GetCurrentProcessId()),
                           std::memory_order_release);

    // Failures after this point also publish the ready flag: the app polls the
    // flag instead of the stdout pipe, which blocks while it has no data.
    const auto hostFail = [&](const juce::String& reason) {
        control->hostReady.store(pluginHostIpc::kHostReadyFailed, std::memory_order_release);
        std::cout << "HOSTERROR " << reason.toStdString() << "\n";
        std::cout.flush();
    };

    pluginHostIpc::NamedEvent requestEvent;
    pluginHostIpc::NamedEvent responseEvent;
    pluginHostIpc::NamedEvent controlRequestEvent;
    pluginHostIpc::NamedEvent controlResponseEvent;
    if (!requestEvent.open(pluginHostIpc::requestEventNameFor(mappingName).toWideCharPointer())
        || !responseEvent.open(
            pluginHostIpc::responseEventNameFor(mappingName).toWideCharPointer())
        || !controlRequestEvent.open(
            pluginHostIpc::controlRequestEventNameFor(mappingName).toWideCharPointer())
        || !controlResponseEvent.open(
            pluginHostIpc::controlResponseEventNameFor(mappingName).toWideCharPointer())) {
        std::cout << "HOSTERROR audio/control events unavailable (err " << requestEvent.error()
                  << "/" << responseEvent.error() << "/" << controlRequestEvent.error() << "/"
                  << controlResponseEvent.error() << ")\n";
        std::cout.flush();
        return 1;
    }

    // The plugin is loaded only in this throwaway process.
    std::cout << "HOSTSTEP opening plugin\n";
    std::cout.flush();
    juce::String scanError;
    const auto descriptions = PluginHost::scanFileInProcess(file, scanError);
    if (descriptions.isEmpty()) {
        hostFail(scanError.isNotEmpty() ? scanError : juce::String("no plugin classes found"));
        return 1;
    }
    std::cout << "HOSTSTEP scanned " << descriptions.size() << "\n";
    std::cout.flush();

    // Pick the class the app asked for. A saved id that matches no class must
    // fail: applying the saved state to the first class would load a
    // different plugin with foreign parameters.
    juce::PluginDescription chosen = descriptions.getReference(0);
    bool matched = pluginUniqueId == 0 && descriptions.size() == 1;
    if (pluginUniqueId != 0) {
        for (const auto& description : descriptions) {
            if (description.uniqueId == pluginUniqueId) {
                chosen = description;
                matched = true;
                break;
            }
        }
    }
    if (!matched && (pluginUniqueId != 0
                     || (descriptions.size() > 1 && stateBase64.isNotEmpty()))) {
        hostFail("The saved plugin class is no longer installed in "
                 + file.getFileName()
                 + ". The insert is left empty instead of loading a different plugin.");
        return 1;
    }

    juce::String instError;
    auto loaded = PluginHost::instantiate(chosen, file, sampleRate, blockSize, stateBase64);
    if (loaded.error.isNotEmpty() || loaded.instance == nullptr) {
        hostFail(loaded.error.isNotEmpty() ? loaded.error : juce::String("instantiate failed"));
        return 1;
    }

    control->hostReady.store(pluginHostIpc::kHostReadyLoaded, std::memory_order_release);
    std::cout << "HOSTREADY " << file.getFullPathName().toStdString() << "\n";
    std::cout.flush();

    auto* plugin = loaded.instance.get();
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> scratch(2, pluginHostIpc::kMaxBlockFrames);
    std::uint64_t lastSequence = 0;
    // Highest parameter sequence number this host has applied. A delivered
    // message skipping a number means the parent dropped an edit (ring full),
    // which only a full state push can repair.
    std::uint64_t lastAppliedParamSeq = 0;
    double currentRate = sampleRate;
    int currentBufferSize = blockSize;

    const auto applyParams = [&] {
        pluginHostIpc::ParamMsg msg;
        while (pluginHostIpc::paramRingPop(params->toHost, msg)) {
            if (pluginHostIpc::paramSeqGapped(lastAppliedParamSeq, msg.seq)) {
                // Counted where the loss is seen: the parent watches this
                // counter and resyncs state; the gap itself is not replayable.
                params->toHost.paramGapCount.fetch_add(1, std::memory_order_acq_rel);
            }
            lastAppliedParamSeq = msg.seq;
            const auto index = static_cast<int>(msg.id);
            auto& parameters = plugin->getParameters();
            if (index >= 0 && index < parameters.size()) {
                if (auto* parameter = parameters[index]) {
                    // msg.value is normalised (0..1), the domain the plugin
                    // reports and the editor instance holds, so no per-plugin
                    // value conversion has to cross the process boundary.
                    parameter->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, msg.value));
                }
            }
        }
    };

    const auto serviceControl = [&] {
        const auto request = control->requestSeq.load(std::memory_order_acquire);
        if (request == control->responseSeq.load(std::memory_order_acquire)) return;

        const auto op = static_cast<pluginHostIpc::ControlOp>(
            control->op.load(std::memory_order_acquire));
        auto status = pluginHostIpc::ControlStatus::ok;
        juce::String reply;

        if (!pluginHostIpc::controlPayloadTerminated(*control)) {
            status = pluginHostIpc::ControlStatus::failed;
            reply = errorReply("malformed control request");
        } else {
            const auto payload = juce::String::fromUTF8(control->payload);
            const auto body = parseObject(payload);
            const auto getDouble = [&body](const juce::Identifier& key, double fallback) {
                const auto value = body.getProperty(key, juce::var());
                return value.isVoid() ? fallback : static_cast<double>(value);
            };
            const auto getInt = [&body](const juce::Identifier& key, int fallback) {
                const auto value = body.getProperty(key, juce::var());
                return value.isVoid() ? fallback : static_cast<int>(value);
            };

            switch (op) {
                case pluginHostIpc::ControlOp::prepare: {
                    const auto rate = getDouble("sampleRate", currentRate);
                    const auto frames = getInt("blockSize", currentBufferSize);
                    if (rate <= 0.0 || frames <= 0) {
                        status = pluginHostIpc::ControlStatus::refused;
                        reply = errorReply("invalid sample rate or block size");
                        break;
                    }
                    // Always re-prepare, even when the format matches what the
                    // plugin was instantiated with. The app's own load path
                    // prepares twice (instantiate, then adopt), and plugins that
                    // reseed internal state in prepareToPlay must be given the
                    // same lifecycle here or they render differently - the null
                    // test caught exactly that with a compressor.
                    pluginprepare::prepareInstance(*plugin, rate, frames);
                    currentRate = rate;
                    currentBufferSize = frames;
                    block->latencySamples = plugin->getLatencySamples();
                    reply = okReply();
                    break;
                }
                case pluginHostIpc::ControlOp::setState: {
                    const auto encoded = body.getProperty("state", juce::var()).toString();
                    if (encoded.isEmpty()) {
                        status = pluginHostIpc::ControlStatus::refused;
                        reply = errorReply("empty state");
                        break;
                    }
                    auto decoded = juce::MemoryBlock();
                    if (!decoded.fromBase64Encoding(encoded) || decoded.getSize() == 0) {
                        status = pluginHostIpc::ControlStatus::failed;
                        reply = errorReply("state could not be decoded");
                        break;
                    }
                    // Snapshot the cutoff before the restore: the sender took
                    // its state after producing this sequence, so queued edits
                    // up to it are stale relative to the blob.
                    const auto snapshotSeq = static_cast<std::uint64_t>(
                        static_cast<juce::int64>(body.getProperty("paramSeq", 0)));
                    plugin->setStateInformation(decoded.getData(),
                                                static_cast<int>(decoded.getSize()));
                    block->latencySamples = plugin->getLatencySamples();
                    // A full state supersedes every parameter edit the sender
                    // had already made when it took the snapshot: drop exactly
                    // those entries and take the snapshot's sequence as
                    // applied, so the next delivered message is not read as a
                    // new gap - while an edit published after the snapshot
                    // stays queued and is applied on top.
                    if (snapshotSeq > 0) {
                        pluginHostIpc::paramRingDrainUpTo(params->toHost, snapshotSeq);
                        lastAppliedParamSeq = snapshotSeq;
                    } else {
                        // No snapshot information (scene load, adoption): the
                        // state is the whole truth, so everything queued so far
                        // is stale.
                        pluginHostIpc::ParamMsg stale;
                        while (pluginHostIpc::paramRingPop(params->toHost, stale)) {}
                        lastAppliedParamSeq =
                            params->toHost.producerSeq.load(std::memory_order_acquire);
                    }
                    reply = okReply();
                    break;
                }
                case pluginHostIpc::ControlOp::getState: {
                    juce::MemoryBlock state;
                    plugin->getStateInformation(state);
                    const auto encoded = state.toBase64Encoding();
                    if (encoded.isEmpty()) {
                        status = pluginHostIpc::ControlStatus::refused;
                        reply = errorReply("the plugin exposes no state");
                        break;
                    }
                    auto* object = new juce::DynamicObject();
                    object->setProperty("ok", true);
                    object->setProperty("state", encoded);
                    reply = juce::JSON::toString(juce::var(object), true);
                    break;
                }
                case pluginHostIpc::ControlOp::getLatency: {
                    auto* object = new juce::DynamicObject();
                    object->setProperty("ok", true);
                    object->setProperty("latencySamples", plugin->getLatencySamples());
                    reply = juce::JSON::toString(juce::var(object), true);
                    break;
                }
                case pluginHostIpc::ControlOp::ping:
                case pluginHostIpc::ControlOp::none:
                    reply = okReply();
                    break;
            }
        }

        // Reply in place, then publish the sequence: the app only reads the
        // payload after it sees responseSeq match its request.
        pluginHostIpc::controlBeginWrite(*control);
        if (!pluginHostIpc::controlSetPayload(*control, reply.toRawUTF8(),
                                             reply.getNumBytesAsUTF8())) {
            const auto tooLarge = errorReply("the reply is larger than the control channel");
            pluginHostIpc::controlSetPayload(*control, tooLarge.toRawUTF8(),
                                             tooLarge.getNumBytesAsUTF8());
            status = pluginHostIpc::ControlStatus::refused;
        }
        control->status.store(static_cast<std::uint32_t>(status), std::memory_order_release);
        pluginHostIpc::controlEndWrite(*control);
        control->responseSeq.store(request, std::memory_order_release);
        controlResponseEvent.signal();
    };

    const auto processOneBlock = [&] {
        const auto seq = block->sequence;
        if (seq == 0 || seq == lastSequence) return;

        const int frames = juce::jmin(block->numFrames, pluginHostIpc::kMaxBlockFrames);
        const double rate = block->sampleRate > 0.0 ? block->sampleRate : currentRate;
        const int framesPerBlock = block->bufferSize > 0 ? block->bufferSize : currentBufferSize;
        if (rate != currentRate || framesPerBlock != currentBufferSize) {
            pluginprepare::prepareInstance(*plugin, rate, framesPerBlock);
            currentRate = rate;
            currentBufferSize = framesPerBlock;
            block->latencySamples = plugin->getLatencySamples();
        }

        if (frames > 0 && frames <= pluginHostIpc::kMaxBlockFrames) {
            // Torn-read guard: stable even sequence on both sides.
            const auto before = block->blockSequence;
            for (int i = 0; i < frames; ++i) {
                scratch.getWritePointer(0)[i] = block->interleave[i * 2];
                scratch.getWritePointer(1)[i] = block->interleave[i * 2 + 1];
            }
            const auto after = block->blockSequence;
            if (before != after || (before % 2) == 1) {
                // Torn: ack anyway (below), otherwise the app, which only
                // submits after an ack, would stop feeding this host forever.
                // The app's own torn guard rejects the payload and plays dry.
            } else {
                juce::AudioBuffer<float> view(scratch.getArrayOfWritePointers(), 2, 0, frames);
                applyParams();
                midi.clear();
                plugin->processBlock(view, midi);
                // Write-back with the same guard: mark odd, copy, mark even.
                block->blockSequence = block->blockSequence + 1;
                for (int i = 0; i < frames; ++i) {
                    block->interleave[i * 2] = scratch.getReadPointer(0)[i];
                    block->interleave[i * 2 + 1] = scratch.getReadPointer(1)[i];
                }
                block->blockSequence = block->blockSequence + 1;
            }
        }

        block->responseSequence = seq;
        lastSequence = seq;
        // Wake the app's audio thread; it waits on this event instead of
        // spinning, so the response is not bound to the system timer tick.
        responseEvent.signal();
    };

    // One loop, two wake-ups: audio blocks and control requests. Waiting on
    // both handles keeps control off the system timer tick too; the 1 s timeout
    // is only an idle re-check (e.g. after the app stopped feeding).
    HANDLE wakeups[2] = {requestEvent.nativeHandle(), controlRequestEvent.nativeHandle()};
    for (;;) {
        const DWORD signalled = ::WaitForMultipleObjects(2, wakeups, FALSE, 1000);
        if (signalled == WAIT_OBJECT_0 + 1) {
            serviceControl();
        } else if (signalled == WAIT_OBJECT_0) {
            processOneBlock();
        }
        // Both set in the same wake-up: the second event stays signalled and
        // the next iteration services it, so nothing is missed.
    }
}
