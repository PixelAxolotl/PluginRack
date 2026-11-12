#pragma once

#include <JuceHeader.h>

#include "NamedEvent.h"
#include "NamedSection.h"
#include "PluginHostIpc.h"

#include <atomic>
#include <cstdint>
#include <thread>

// App-side owner of one child-process plugin host (fault containment, phase 1).
//
// The child process runs the IAudioProcessor. This class moves audio, parameter
// edits, control requests and state across the shared channels in
// PluginHostIpc.h. It never renders anything itself, and it never opens an
// audio device: the child renders the buffers it is handed (ASIO is single
// client on the reference rig, so a second device client would break it).
//
// Threading contract:
//   start/waitReady/setState/getState/prepare/requestLatency/killAndReap
//       -> any non-audio thread. Bounded waits, never used from processBlock.
//   submitBlock/fetchBlock
//       -> the audio thread only. No allocation, no locks, bounded waits.
//   setParameter
//       -> one producer thread at a time (the message thread today).
//
// The failure rule lives in PluginHost, not here: on a miss the caller leaves
// the block dry. There is deliberately no "fall back to the in-process
// instance" path in this class, and callers must not add one (see
// docs/superpowers/specs/2026-11-13-fault-containment-ipc.md §3.2).

class PluginProcessHost {
public:
    struct Peaks {
        float inL = 0.0f;
        float inR = 0.0f;
        float outL = 0.0f;
        float outR = 0.0f;
    };

    PluginProcessHost();
    ~PluginProcessHost();

    // Spawns the child for `file`. Does not wait for the plugin to load: call
    // waitReady() from the same worker next, so the message thread never
    // blocks for the seconds a large VST3 takes to initialise.
    bool start(const juce::File& file, int pluginUniqueId, double sampleRate, int blockSize,
               juce::String& error);

    // Waits for HOSTREADY / HOSTERROR / timeout. Safe on a worker thread.
    bool waitReady(int timeoutMs, juce::String& error);

    // Pushes the saved state into the child. Must be called after waitReady and
    // before the first block, so the plugin never renders with defaults when a
    // scene had settings. Over the base64 cap the child refuses and `error`
    // says so; the caller keeps the in-process path or reports the failure.
    bool setState(const juce::String& stateBase64, int timeoutMs, juce::String& error);
    juce::String getState(int timeoutMs, juce::String& error);

    // Re-prepares the child after a sample-rate / buffer-size change.
    bool prepare(double sampleRate, int blockSize, int timeoutMs, juce::String& error);

    // Fast path for the latency report; the child also refreshes the audio
    // header after every prepare.
    int latencySamples(int timeoutMs);

    // Audio thread. submitBlock copies into the shared block and wakes the
    // child; fetchBlock waits at most `deadlineMs` for that block's answer.
    // Neither mutates the caller's buffer on failure.
    bool submitBlock(const juce::AudioBuffer<float>& input, double sampleRate, int blockSize);
    bool fetchBlock(juce::AudioBuffer<float>& output, int deadlineMs);

    // Message thread. Pushes one parameter edit at the child; dropped messages
    // leave a detectable sequence gap rather than silently reordering.
    bool setParameter(std::uint32_t parameterIndex, float value, std::uint32_t flags = 0);

    bool isAlive() const;
    bool isDead() const;
    bool isReady() const { return ready; }
    int exitCode() const;
    int childProcessId() const;
    // Last state the child reported, used when a live fetch times out.
    const juce::String& lastKnownState() const { return lastKnownState_; }
    int missedBlocks() const { return missedBlocks_.load(std::memory_order_relaxed); }
    void noteMissedBlock() { missedBlocks_.fetch_add(1, std::memory_order_relaxed); }

    // Peaks the child wrote for the last block it acked.
    Peaks peaks() const;

    // Whatever the child wrote to stdout, for the failure path. Only call this
    // after the child has exited: the output pipe blocks while it is open and
    // empty (see ControlSection::hostReady).
    juce::String outputAfterExit();

    // Kills and reaps the child, closes every channel. Idempotent.
    void killAndReap();

private:
    bool openChannel(const juce::File& file);
    bool openControlChannel();
    void startWatcher();
    void stopWatcherThread();

    // One request/response round trip on the control channel.
    bool controlCall(pluginHostIpc::ControlOp op, const juce::String& requestJson, int timeoutMs,
                     juce::String& responseJson, juce::String& error);

    std::unique_ptr<juce::ChildProcess> child;
    juce::String sharedName;
    juce::String pluginPath;

    pluginHostIpc::NamedSection audioSection;
    pluginHostIpc::NamedSection paramSection;
    pluginHostIpc::NamedSection controlSection;

    pluginHostIpc::NamedEvent requestEvent;
    pluginHostIpc::NamedEvent responseEvent;
    pluginHostIpc::NamedEvent controlRequestEvent;
    pluginHostIpc::NamedEvent controlResponseEvent;

    std::uint64_t lastSubmitted = 0;
    bool launched = false;
    bool ready = false;
    juce::String lastKnownState_;

    std::atomic<int> missedBlocks_{0};
    std::atomic<bool> hostDead{false};
    std::atomic<bool> stopWatcher{false};
    std::thread watcher;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginProcessHost)
};
