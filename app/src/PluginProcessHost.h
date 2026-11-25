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
//          The control round trips are serialised by controlLock_, so the
//          message thread and the param-resync worker can both issue them: a
//          caller that finds the lock held waits for the other round trip to
//          finish before its own timeout starts.
//   submitBlock/fetchBlock
//       -> the audio thread only. No allocation, no locks, bounded waits.
//   setParameter
//       -> one producer thread at a time (the message thread today).
//   lastKnownState
//       -> any thread (read under controlLock_).
//   paramProducerSeq
//       -> the parameter producer thread (the message thread), like
//          setParameter: it reads the producer side of the param mapping, which
//          is only closed from that same thread.
//   watcher thread (private)
//       -> spawned by start(); every 100 ms it checks the child and marks the
//          host dead when it is gone. Stopped by stopWatcherThread(), which
//          sets stopWatcher and then signals stopWatcherEvent so the join()
//          returns immediately instead of waiting out the current tick.
//          killAndReap() (and therefore the destructor) goes through it, so a
//          host destroyed while the watcher runs never blocks for a tick.
//
// The failure rule lives in PluginHost, not here: on a miss the caller leaves
// the block dry. There is deliberately no "fall back to the in-process
// instance" path in this class, and callers must not add one (see
// docs/superpowers/specs/2026-11-13-fault-containment-ipc.md §3.2).

class PluginProcessHost {
public:
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
    //
    // paramSeq is the producer sequence the state was snapshotted at (0 when
    // the caller did not take a snapshot). With a sequence the child keeps any
    // parameter edit published after it queued and drops the older ones, which
    // it can only do when it knows which edit that state already contains: a
    // resync takes a sequence, a scene load or an adoption applies its state
    // before any edit is made and passes 0.
    bool setState(const juce::String& stateBase64, int timeoutMs, juce::String& error,
                  std::uint64_t paramSeq = 0);
    juce::String getState(int timeoutMs, juce::String& error);

    // Re-prepares the child after a sample-rate / buffer-size change.
    bool prepare(double sampleRate, int blockSize, int timeoutMs, juce::String& error);

    // Fast path for the latency report; the child also refreshes the audio
    // header after every prepare.
    int latencySamples(int timeoutMs);

    // Audio thread. submitBlock copies the block into the shared mapping and
    // wakes the child; fetchBlock waits at most `deadlineMs` for that block's
    // answer. numSamples is explicit because the scratch buffer is preallocated
    // to kMaxBlockFrames and is usually larger than the callback's block.
    // Neither mutates the caller's buffer on failure.
    bool submitBlock(const juce::AudioBuffer<float>& input, int numSamples, double sampleRate,
                     int blockSize);
    bool fetchBlock(juce::AudioBuffer<float>& output, int deadlineMs);

    // Message thread. Pushes one parameter edit at the child. The two facts
    // are reported separately: `pushed` says the edit entered the ring (so the
    // caller may count it as made), `resyncNeeded` says a parameter edit was
    // lost - this push overflowed the ring, or the child reported a new
    // sequence gap while draining - and the caller must hand the child a full
    // state instead. A successful push can still need a resync (the gap may
    // belong to an earlier dropped edit).
    struct ParameterPushResult {
        bool pushed = false;
        bool resyncNeeded = false;
    };
    ParameterPushResult setParameter(std::uint32_t parameterIndex, float value,
                                     std::uint32_t flags = 0);

    bool isAlive() const;
    bool isDead() const;
    int exitCode() const;
    int childProcessId() const;
    // Last state the child reported, used when a live fetch times out. Returns
    // a copy: the stored string is rewritten by setState/getState, which may
    // run on the resync worker while a caller on another thread reads it.
    juce::String lastKnownState() const;
    // Producer sequence the child's parameter ring has published. Read together
    // with the editor's state to build a resync snapshot: taken before the
    // state blob, it is the highest edit that blob is guaranteed to contain,
    // and everything above it stays queued at the child.
    std::uint64_t paramProducerSeq() const;

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
    std::uint64_t lastParamGapCount = 0; // setParameter baseline, message thread only
    // Serialises control round trips (request + wait + reply) and guards the
    // state/sequence reads that accompany them. Mutable so the const accessors
    // can take it too. Deliberately held for the whole round trip: the child
    // answers one request at a time, so overlapping calls would otherwise
    // interleave on the single request/response slot pair.
    mutable juce::CriticalSection controlLock_;
    bool launched = false;
    juce::String lastKnownState_; // guarded by controlLock_

    std::atomic<bool> hostDead{false};
    std::atomic<bool> stopWatcher{false};
    // Wakes the watcher out of its 100 ms wait (auto-reset: one signal releases
    // one wait; a signal that arrives first stays pending, so it cannot be
    // missed between the stopWatcher store and the wait). start() resets it
    // before spawning the next watcher, because a previous killAndReap() leaves
    // it signalled and the new loop would spin on immediate returns.
    juce::WaitableEvent stopWatcherEvent;
    std::thread watcher;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginProcessHost)
};
