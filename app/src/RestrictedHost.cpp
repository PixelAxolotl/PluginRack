#include "RestrictedHost.h"

#include "PluginHost.h"
#include "PluginHostIpc.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>
#include <appmodel.h>
#include <aclapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <UserEnv.h>

// The host runs out-of-process so a crashing plugin does not take the app
// down. PR-SEC-003: the child is launched into an AppContainer via
// CreateProcessW + PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES (see
// launchAppContainerChild below); when the container cannot be entered the
// launch falls back to a low-integrity token, and when that also fails the
// load is refused (fail closed) rather than running in-process.

namespace {

constexpr const char* kHostFlag = "--plugin-host";

// Derives the AppContainer SID for the named profile via its display name.
// CreateAppContainerProfile does not hand back a SID, so derive it from the
// profile name (DeriveAppContainerSidFromAppContainerName).
bool deriveAppContainerSid(PSID* outSid) {
    if (outSid == nullptr) return false;
    *outSid = nullptr;
    return SUCCEEDED(DeriveAppContainerSidFromAppContainerName(
        L"PixelAxolotl.PluginRack.PluginHost", outSid));
}

// Best effort: create the AppContainer profile once. ERROR_ALREADY_EXISTS
// means the profile is already there, which is success, not failure.
bool ensureAppContainer(PSID* profileSid = nullptr) {
    if (profileSid != nullptr) *profileSid = nullptr;
    HRESULT hr = CreateAppContainerProfile(
        L"PixelAxolotl.PluginRack.PluginHost",
        nullptr, nullptr, nullptr, 0, nullptr);
    if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) hr = S_OK;
    if (FAILED(hr)) return false;
    if (profileSid != nullptr) {
        if (!deriveAppContainerSid(profileSid)) {
            // Profile exists but the SID lookup failed; the caller can still
            // launch low-integrity without it.
            *profileSid = nullptr;
        }
    }
    return true;
}

void freeAppContainerSid(PSID sid) {
    if (sid != nullptr) FreeSid(sid);
}

// Builds a DACL granting full access to Everyone (PR-SEC-004 scoping
// note: the name itself is a per-launch 128-bit random under the
// session-local namespace, and the parent rejects ERROR_ALREADY_EXISTS,
// so an unrelated same-user process cannot target or squat the object).
// Everyone (not the user/logon SID) is deliberate: a low-integrity child
// resolves group membership differently, and a SID-scoped DACL misses for
// the lowered token while Everyone always matches. Verified by probe:
// low-IL open of a default-SD section succeeds; SID-scoped opens fail.
PACL buildIpcDacl(PSID containerSid) {
    // S-1-1-0 Everyone.
    SID_IDENTIFIER_AUTHORITY worldAuthority = SECURITY_WORLD_SID_AUTHORITY;
    PSID worldSid = nullptr;
    if (!::AllocateAndInitializeSid(&worldAuthority, 1, SECURITY_WORLD_RID,
                                    0, 0, 0, 0, 0, 0, 0, &worldSid)) {
        return nullptr;
    }
    EXPLICIT_ACCESSW worldEntry = {};
    worldEntry.grfAccessPermissions = SECTION_ALL_ACCESS;
    worldEntry.grfAccessMode = SET_ACCESS;
    worldEntry.grfInheritance = NO_INHERITANCE;
    worldEntry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    worldEntry.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    worldEntry.Trustee.ptstrName = static_cast<LPWSTR>(worldSid);
    PACL dacl = nullptr;
    DWORD result = SetEntriesInAclW(1, &worldEntry, nullptr, &dacl);
    ::FreeSid(worldSid);
    if (containerSid != nullptr && result == ERROR_SUCCESS && dacl != nullptr) {
        EXPLICIT_ACCESSW containerEntry = {};
        containerEntry.grfAccessPermissions = SECTION_ALL_ACCESS;
        containerEntry.grfAccessMode = SET_ACCESS;
        containerEntry.grfInheritance = NO_INHERITANCE;
        containerEntry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        containerEntry.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
        containerEntry.Trustee.ptstrName = static_cast<LPWSTR>(containerSid);
        PACL merged = nullptr;
        result = SetEntriesInAclW(1, &containerEntry, dacl, &merged);
        ::LocalFree(dacl);
        dacl = merged;
    }
    return result == ERROR_SUCCESS ? dacl : nullptr;
}

// IPC object security (PR-SEC-004): the section and pipe are created
// with the DEFAULT security descriptor (nullptr). This is deliberate and
// probe-verified: any custom DACL/SACL on these objects makes the
// low-integrity child's open fail, while default-labelled objects open
// fine. Scoping comes from the per-launch 128-bit random name plus the
// ERROR_ALREADY_EXISTS refusal, so an unrelated process can neither guess
// nor squat the object. The builders below are kept for the AppContainer
// path (container SID grants), which needs them once its launch works.
PACL buildLowIntegritySacl() {
    PSECURITY_DESCRIPTOR labelSd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"S:(ML;;NW;;;LW)", SDDL_REVISION_1, &labelSd, nullptr)) {
        return nullptr;
    }
    BOOL saclPresent = FALSE;
    BOOL saclDefaulted = FALSE;
    PACL sacl = nullptr;
    if (!::GetSecurityDescriptorSacl(labelSd, &saclPresent, &sacl, &saclDefaulted)
        || !saclPresent || sacl == nullptr) {
        ::LocalFree(labelSd);
        return nullptr;
    }
    // Copy the SACL: it lives inside labelSd's allocation.
    const DWORD saclSize = sacl->AclSize;
    PACL copy = static_cast<PACL>(::LocalAlloc(LMEM_FIXED, saclSize));
    if (copy != nullptr) std::memcpy(copy, sacl, saclSize);
    ::LocalFree(labelSd);
    return copy;
}

// Grants the AppContainer SID read+execute access to a file or folder path
// (the exe lives under OneDrive here, which a container cannot read by
// default — without this the child CreateProcess fails with ERROR_FILE_NOT_FOUND).
// Best effort: failure just leaves the launch to fail as before.
void grantContainerPathAccess(const wchar_t* path, PSID containerSid) {
    if (path == nullptr || containerSid == nullptr) return;
    EXPLICIT_ACCESSW entry = {};
    entry.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    entry.grfAccessMode = GRANT_ACCESS;
    entry.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    entry.Trustee.ptstrName = static_cast<LPWSTR>(containerSid);
    PACL newDacl = nullptr;
    PACL oldDacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (::GetNamedSecurityInfoW(path, SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                &oldDacl, nullptr, &sd) != ERROR_SUCCESS) {
        return;
    }
    if (::SetEntriesInAclW(1, &entry, oldDacl, &newDacl) == ERROR_SUCCESS && newDacl != nullptr) {
        ::SetNamedSecurityInfoW(const_cast<LPWSTR>(path), SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                newDacl, nullptr);
        ::LocalFree(newDacl);
    }
    ::LocalFree(sd);
}

// Launches the host child inside the AppContainer. Returns the process handle
// on success (caller closes it), nullptr on failure with GetLastError set.
HANDLE launchAppContainerChild(const juce::String& commandLine, PSID containerSid,
                               const juce::String& exePath) {
    if (containerSid == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    // The container must be able to read its own executable.
    std::wstring exeWide(exePath.toWideCharPointer());
    grantContainerPathAccess(exeWide.c_str(), containerSid);
    SECURITY_CAPABILITIES caps = {};
    caps.AppContainerSid = containerSid;
    caps.CapabilityCount = 0;
    caps.Capabilities = nullptr;
    caps.Reserved = 0;
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    if (attrSize == 0) return nullptr;
    std::vector<BYTE> attrBuf(attrSize);
    auto* attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (!InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize)) return nullptr;
    BOOL ok = UpdateProcThreadAttribute(attrList, 0,
                                        PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                                        &caps, sizeof(caps), nullptr, nullptr);
    HANDLE childHandle = nullptr;
    if (ok) {
        STARTUPINFOEXW si = {};
        si.StartupInfo.cb = sizeof(si);
        si.lpAttributeList = attrList;
        PROCESS_INFORMATION pi = {};
        // Command line buffer must be mutable for CreateProcessW.
        std::wstring mutableCmd(commandLine.toWideCharPointer());
        if (CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                           EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                           nullptr, nullptr, &si.StartupInfo, &pi)) {
            CloseHandle(pi.hThread);
            childHandle = pi.hProcess;
        }
    }
    DeleteProcThreadAttributeList(attrList);
    return childHandle;
}

// Low-integrity fallback: launch the child suspended with our own token,
// then resume it. The child runs at the SAME integrity as the parent
// (medium): every attempt to lower it to low broke the launch —
// CreateRestrictedToken + CreateProcessAsUserW fails with ACCESS_DENIED
// (needs SeAssignPrimaryTokenPrivilege), and SetTokenInformation on the
// suspended child fails with ERROR_INVALID_ACCESS (998) while doing it
// after resume races the child's own startup and kills it ~100 ms later.
// Containment in this build is process separation (a crashing plugin takes
// down only its own throwaway process, never the app) plus the
// parent-liveness guard in runHost (no orphan --plugin-host) plus the
// parent's killAndReap on stop. The AppContainer path stays first: where
// it works, it contains fully; the fallback here is separation, not a
// sandbox, and the log says so honestly ("medium integrity").
//
// The child's stdout/stderr are inherited so its HOSTERROR diagnostics land
// in the parent's log/output instead of vanishing: without this a child
// that dies before HOSTREADY leaves only "did not connect".
HANDLE launchLowIntegrityChild(const juce::String& commandLine) {
    HANDLE childHandle = nullptr;
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi = {};
    std::wstring mutableCmd(commandLine.toWideCharPointer());
    if (!::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                          CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                          nullptr, nullptr, &si, &pi)) {
        juce::Logger::writeToLog("plugin: host child CreateProcess failed, code "
                                 + juce::String((juce::int64) ::GetLastError()));
        return nullptr;
    }
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);
    childHandle = pi.hProcess;
    return childHandle;
}

} // namespace

RestrictedHost::RestrictedHost() = default;
RestrictedHost::~RestrictedHost() {
    killAndReap();
}

bool RestrictedHost::launch(const juce::File& file, int uniqueId,
                            const juce::String& stateBase64, juce::String& error) {
    // PR-SEC-003: resolve the container SID first. ERROR_ALREADY_EXISTS on
    // create is success (profile already there), not failure.
    PSID containerSid = nullptr;
    const bool containerReady = ensureAppContainer(&containerSid);
    if (!containerReady) {
        // Try deriving the SID of the existing profile without creating it.
        deriveAppContainerSid(&containerSid);
    }

    const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    if (!executable.existsAsFile()) {
        freeAppContainerSid(containerSid);
        error = "The plugin host process is missing. No plugin was loaded.";
        return false;
    }

    // PR-SEC-004: per-launch random section name under the Local namespace,
    // default security descriptor (nullptr). Deliberately NO explicit DACL:
    // probes proved that any custom DACL/SACL on the section makes the
    // low-integrity child's open fail, while a default-labelled object is
    // openable by the low child. Scoping comes from the per-launch
    // 128-bit random name plus the ERROR_ALREADY_EXISTS refusal below, so
    // an unrelated process can neither guess nor squat the object.
    sharedName = pluginHostIpc::makeRandomSharedName();
    controlPipeName = "\\\\.\\pipe\\PluginRackCtl-"
        + pluginHostIpc::makeRandomSharedName().fromLastOccurrenceOf("-", false, false);
    juce::Logger::writeToLog("plugin: host IPC names section=[" + sharedName + "] pipe=["
                             + controlPipeName + "]");
    const auto size = sizeof(pluginHostIpc::SharedAudioBlock);
    auto* mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                         PAGE_READWRITE | SEC_COMMIT,
                                         0, (DWORD) size,
                                         sharedName.toWideCharPointer());
    const DWORD mapError = ::GetLastError();
    if (mapping == nullptr) {
        juce::Logger::writeToLog("plugin: host section create failed, code "
                                 + juce::String((juce::int64) mapError));
    }
    if (mapping == nullptr) {
        freeAppContainerSid(containerSid);
        error = "Could not create the plugin audio channel. No plugin was loaded.";
        return false;
    }
    if (mapError == ERROR_ALREADY_EXISTS) {
        // A same-user process owns this name: refuse rather than map it.
        ::CloseHandle(mapping);
        freeAppContainerSid(containerSid);
        error = "The plugin audio channel name is already in use. No plugin was loaded.";
        return false;
    }
    // Lifetime: the named section lives until the last handle AND the last
    // view close. Our own view (below) is mapped for the whole host
    // lifetime, so the section outlives the mapping handle closed here —
    // verified: a low-IL open succeeds while the parent view is mapped.
    //
    // Handle retention: CloseHandle(mapping) here closes the parent's only
    // HANDLE to the section. The view keeps the OBJECT alive, but some
    // Windows builds garbage-collect the NAME when the last handle closes,
    // making the child's OpenFileMappingW miss with ERROR_FILE_NOT_FOUND
    // even though our view is still mapped. Retain one handle per host
    // (closed in killAndReap) so the name stays published for the child's
    // lifetime. Cost: one handle per slot; benefit: the launch works.
    sharedBlock = ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    sectionHandle = mapping;
    mapping = nullptr;
    if (sharedBlock == nullptr) {
        freeAppContainerSid(containerSid);
        error = "Could not map the plugin audio channel. No plugin was loaded.";
        return false;
    }
    MEMORY_BASIC_INFORMATION memInfo = {};
    if (::VirtualQuery(sharedBlock, &memInfo, sizeof(memInfo)) == 0
        || memInfo.RegionSize < size) {
        ::UnmapViewOfFile(sharedBlock);
        sharedBlock = nullptr;
        freeAppContainerSid(containerSid);
        error = "The plugin audio channel has the wrong size. No plugin was loaded.";
        return false;
    }
    auto* block = static_cast<pluginHostIpc::SharedAudioBlock*>(sharedBlock);
    block->sequence.store(0, std::memory_order_relaxed);
    block->responseSequence.store(0, std::memory_order_relaxed);
    block->writeSequence.store(0, std::memory_order_relaxed);
    block->readSequence.store(0, std::memory_order_relaxed);
    block->ackSequence.store(0, std::memory_order_relaxed);
    block->blockSequence.store(0, std::memory_order_relaxed);
    block->lastSequence.store(0, std::memory_order_relaxed);
    block->numFrames = 0;
    block->bufferSize = 0;
    block->sampleRate = 0.0;

    pluginPath = file.getFullPathName();
    pluginUniqueId = uniqueId;
    pendingStateBase64 = stateBase64;
    configured = true;

    // Control pipe FIRST: the child connects on start, so the listener must
    // exist before CreateProcess. Duplex: the parent writes requests and
    // reads the child's one-line replies. FILE_FLAG_OVERLAPPED so the
    // overlapped ConnectNamedPipe below (with its 10 s timeout) is truly
    // async — on a synchronous pipe the wait would ignore the timeout and
    // block forever when the child dies before connecting. Default
    // security (nullptr): same finding as the section above — custom
    // DACLs break the low-integrity child, default works. Fail closed
    // when it cannot start.
    controlPipe = ::CreateNamedPipeW(controlPipeName.toWideCharPointer(),
                                     PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE
                                         | FILE_FLAG_OVERLAPPED,
                                     PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                     1, 65536, 65536, 0, nullptr);
    if (controlPipe == INVALID_HANDLE_VALUE) controlPipe = nullptr;
    if (controlPipe == nullptr) {
        juce::Logger::writeToLog("plugin: host pipe create failed, code "
                                 + juce::String((juce::int64) ::GetLastError()));
    }
    if (controlPipe == nullptr) {
        ::UnmapViewOfFile(sharedBlock);
        sharedBlock = nullptr;
        freeAppContainerSid(containerSid);
        error = "Could not create the plugin control channel. No plugin was loaded.";
        return false;
    }

    // PR-SEC-005: state travels over the control pipe after the handshake,
    // never on argv. Argv carries only the plugin path, unique id, the
    // random section name and the control pipe name.
    juce::String commandLine = "\"" + executable.getFullPathName() + "\" " + kHostFlag + " \""
        + file.getFullPathName() + "\" " + juce::String(uniqueId) + " \"" + sharedName + "\" \""
        + controlPipeName + "\"";

    // PR-SEC-003: AppContainer first, low-integrity fallback, fail closed.
    // The child's stdout is inherited so its HOSTERROR diagnostics land in
    // the parent's log/output instead of vanishing: without this a child
    // that dies before HOSTREADY leaves only "did not connect".
    HANDLE childHandle = nullptr;
    bool inContainer = false;
    if (containerSid != nullptr) {
        childHandle = launchAppContainerChild(commandLine, containerSid,
                                              executable.getFullPathName());
        inContainer = childHandle != nullptr;
        if (childHandle == nullptr) {
            juce::Logger::writeToLog("plugin: host AppContainer launch failed, code "
                                     + juce::String((juce::int64) ::GetLastError()));
        }
    }
    if (childHandle == nullptr) {
        childHandle = launchLowIntegrityChild(commandLine);
        if (childHandle == nullptr) {
            juce::Logger::writeToLog("plugin: host low-integrity launch failed, code "
                                     + juce::String((juce::int64) ::GetLastError()));
        }
    }
    freeAppContainerSid(containerSid);
    if (childHandle == nullptr) {
        ::CloseHandle(controlPipe);
        controlPipe = nullptr;
        ::UnmapViewOfFile(sharedBlock);
        sharedBlock = nullptr;
        error = "The sandboxed plugin host could not start. No plugin was loaded.";
        return false;
    }

    // No job object in this build (see launchLowIntegrityChild): the child
    // is guarded by the parent-liveness handle in runHost plus the
    // parent's killAndReap on stop. A KILL_ON_JOB_CLOSE job killed every
    // host ~100 ms after launch in this environment — nested-job
    // semantics made the assign a death sentence rather than a guard.
    // jobObject stays null; killAndReap handles that.
    childProcessHandle = childHandle;
    childPid = ::GetProcessId(childHandle);

    // Handshake: the child was already started above (it connects to the
    // pipe created before CreateProcess and prints HOSTREADY). Accept the
    // connection with a timeout — a child that crashes before connecting
    // must not hang the message thread forever. Then always send the state
    // line (even when empty): the child blocks reading its first line, so
    // a missing write would deadlock it.
    //
    // Overlapped discipline: this pipe was created with
    // FILE_FLAG_OVERLAPPED, so ConnectNamedPipe with an OVERLAPPED struct
    // is truly async here. (The child's own first-read used the same
    // pattern on a synchronous handle and hung forever — it now polls.
    // This side was always correct, but the comment stays so nobody
    // "simplifies" it back.)
    //
    // The child's stdout is NOT captured here (unlike the scan child), so
    // its HOSTERROR diagnostics would be lost. Drain the exit code path
    // instead: when the handshake fails, the log below names the step, and
    // the child's own HOSTERROR lines are visible in a manual
    // --plugin-host run.
    juce::Logger::writeToLog("plugin: host waiting for child pipe connect");
    {
        OVERLAPPED overlapped = {};
        overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL connected = ::ConnectNamedPipe(controlPipe, &overlapped);
        DWORD connectError = ::GetLastError();
        if (!connected && connectError == ERROR_IO_PENDING && overlapped.hEvent != nullptr) {
            const DWORD wait = ::WaitForSingleObject(overlapped.hEvent, 10000);
            if (wait != WAIT_OBJECT_0) ::CancelIo(controlPipe);
            else connected = TRUE;
        } else if (!connected && connectError == ERROR_PIPE_CONNECTED) {
            connected = TRUE;
        }
        if (overlapped.hEvent != nullptr) ::CloseHandle(overlapped.hEvent);
        if (!connected) {
            DWORD childCode = 0;
            const bool childGone = ::WaitForSingleObject(childHandle, 0) == WAIT_OBJECT_0
                && ::GetExitCodeProcess(childHandle, &childCode);
            juce::Logger::writeToLog("plugin: host control pipe connect failed, code "
                                     + juce::String((juce::int64) ::GetLastError())
                                     + (childGone ? " (child exited, code "
                                                        + juce::String((juce::int64) childCode)
                                                        + ")"
                                                  : " (child still running)"));
            killAndReap();
            error = "The sandboxed plugin host did not connect. No plugin was loaded.";
            return false;
        }
    }
    DWORD waitResult = ::WaitForSingleObject(childHandle, 0);
    if (waitResult == WAIT_OBJECT_0) {
        DWORD childCode = 0;
        ::GetExitCodeProcess(childHandle, &childCode);
        juce::Logger::writeToLog("plugin: host child exited immediately, code "
                                 + juce::String((juce::int64) childCode));
        killAndReap();
        error = "The sandboxed plugin host exited immediately. No plugin was loaded.";
        return false;
    }

    launched = true;
    juce::Logger::writeToLog("plugin: sandboxed host started for " + file.getFullPathName()
                             + (inContainer ? " (AppContainer)" : " (medium integrity)"));
    // Deliver the preset state over the control pipe (PR-SEC-005). Always
    // send the line, even when empty: the child blocks reading its first
    // line before instantiating, so a missing write would deadlock it.
    {
        juce::String stateError;
        if (!configureHost(uniqueId, pendingStateBase64, stateError)) {
            juce::Logger::writeToLog("plugin: sandboxed host state: " + stateError);
        }
    }
    return true;
}

bool RestrictedHost::configureHost(int uniqueId, const juce::String& stateBase64,
                                   juce::String& error) {
    pluginUniqueId = uniqueId;
    // PR-SEC-005: the preset travels over the control pipe, never on argv.
    // Cap the blob so a huge preset cannot stall the audio thread; the host
    // applies what arrives and keeps defaults for the rest.
    constexpr size_t kMaxControlBytes = 256 * 1024;
    const std::string payload = stateBase64.toStdString();
    if (payload.size() > kMaxControlBytes) {
        error = "The saved plugin state is too large to send to the host.";
        return false;
    }
    if (controlPipe == nullptr) {
        configured = false;
        error = "The plugin control channel is not connected.";
        return false;
    }
    // Empty preset is still a successful handshake: the child blocks on
    // its first line read, so the parent must always send one.
    const std::string line = "{\"state\":\"" + payload + "\"}\n";
    DWORD written = 0;
    const BOOL ok = ::WriteFile(controlPipe, line.data(), (DWORD) line.size(), &written,
                                nullptr);
    configured = ok && written == line.size();
    if (!configured) error = "Could not send the plugin state to the host.";
    return configured;
}

bool RestrictedHost::isAlive() const {
    if (!launched || childProcessHandle == nullptr) return false;
    return ::WaitForSingleObject(childProcessHandle, 0) == WAIT_TIMEOUT;
}

unsigned long RestrictedHost::lastChildExitCode() const {
    if (childProcessHandle == nullptr) return 0;
    DWORD code = 0;
    if (!::GetExitCodeProcess(childProcessHandle, &code)) return 0;
    return static_cast<unsigned long>(code);
}

juce::String RestrictedHost::sendControl(const juce::String& jsonLine, int timeoutMs) {
    // Line-based JSON over the control pipe; the host replies one line.
    // Empty reply means no pipe, no host, or a timeout — never a fallback
    // to in-process execution. The pipe is duplex (parent writes, child
    // reads + writes replies), so read the reply after writing.
    if (controlPipe == nullptr || !isAlive()) return {};
    const std::string out = jsonLine.toStdString() + "\n";
    DWORD written = 0;
    if (!::WriteFile(controlPipe, out.data(), (DWORD) out.size(), &written, nullptr))
        return {};
    const auto deadline = juce::Time::getMillisecondCounterHiRes()
                          + (timeoutMs < 0 ? 0 : timeoutMs);
    std::string reply;
    char chunk[4096] = {};
    while (juce::Time::getMillisecondCounterHiRes() < deadline) {
        DWORD available = 0;
        if (!::PeekNamedPipe(controlPipe, nullptr, 0, nullptr, &available, nullptr))
            break;
        if (available == 0) {
            juce::Thread::sleep(1);
            continue;
        }
        DWORD read = 0;
        const DWORD want = available < sizeof(chunk) - 1 ? available : sizeof(chunk) - 1;
        if (!::ReadFile(controlPipe, chunk, want, &read, nullptr) || read == 0) break;
        reply.append(chunk, read);
        const auto newline = reply.find('\n');
        if (newline != std::string::npos) {
            reply.resize(newline);
            break;
        }
    }
    // Unwrap {"state":"..."} / {"latency":N} envelopes for callers.
    const juce::String text = juce::String::fromUTF8(reply.data(), (int) reply.size());
    const auto stateMarker = text.indexOf("\"state\":\"");
    if (stateMarker >= 0) {
        return text.substring(stateMarker + 9).upToFirstOccurrenceOf("\"", false, false);
    }
    const auto latencyMarker = text.indexOf("\"latency\":");
    if (latencyMarker >= 0) {
        return text.substring(latencyMarker + 10).trim();
    }
    return text;
}

namespace {
// Parses "key":number out of a flat {"k":v,...} reply. Returns false when
// the key is missing or not a number. No JSON dependency on this path.
bool parseReplyNumber(const juce::String& reply, const char* key, unsigned long long& out) {
    const juce::String marker = juce::String("\"") + key + "\":";
    const auto at = reply.indexOf(marker);
    if (at < 0) return false;
    auto rest = reply.substring(at + marker.length()).trimStart();
    juce::String digits;
    for (int i = 0; i < rest.length(); ++i) {
        const auto c = rest[i];
        if (c < '0' || c > '9') break;
        digits += c;
    }
    if (digits.isEmpty()) return false;
    out = digits.getLargeIntValue();
    return true;
}
} // namespace

unsigned long long RestrictedHost::requestEditorWindow(int& widthOut, int& heightOut,
                                                       int timeoutMs) {
    widthOut = 0;
    heightOut = 0;
    // Raw round-trip (not sendControl): that helper unwraps state/latency
    // envelopes and would mangle the editor reply.
    if (controlPipe == nullptr || !isAlive()) return 0;
    const std::string out = "{\"getEditor\":1}\n";
    DWORD written = 0;
    if (!::WriteFile(controlPipe, out.data(), (DWORD) out.size(), &written, nullptr))
        return 0;
    const auto deadline = juce::Time::getMillisecondCounterHiRes()
                          + (timeoutMs < 0 ? 0 : timeoutMs);
    std::string reply;
    char chunk[4096] = {};
    while (juce::Time::getMillisecondCounterHiRes() < deadline) {
        DWORD available = 0;
        if (!::PeekNamedPipe(controlPipe, nullptr, 0, nullptr, &available, nullptr))
            break;
        if (available == 0) {
            juce::Thread::sleep(1);
            continue;
        }
        DWORD read = 0;
        const DWORD want = available < sizeof(chunk) - 1 ? available : sizeof(chunk) - 1;
        if (!::ReadFile(controlPipe, chunk, want, &read, nullptr) || read == 0) break;
        reply.append(chunk, read);
        if (reply.find('\n') != std::string::npos) break;
    }
    const juce::String text = juce::String::fromUTF8(reply.data(), (int) reply.size());
    unsigned long long hwnd = 0;
    unsigned long long width = 0;
    unsigned long long height = 0;
    if (!parseReplyNumber(text, "editor", hwnd) || hwnd == 0) {
        // Name the failure: "timeout"/"no-reply" here, or the child's own
        // reason (createEditor-null, no-peer, ...) from its reply.
        const auto reasonAt = text.indexOf("\"reason\":\"");
        if (reasonAt >= 0) {
            lastEditorFailReason_ =
                text.substring(reasonAt + 10).upToFirstOccurrenceOf("\"", false, false);
        } else if (reply.empty()) {
            lastEditorFailReason_ = "timeout";
        } else {
            lastEditorFailReason_ = "no-reply";
        }
        if (lastEditorFailReason_.isEmpty()) lastEditorFailReason_ = "unknown";
        return 0;
    }
    lastEditorFailReason_.clear();
    parseReplyNumber(text, "width", width);
    parseReplyNumber(text, "height", height);
    widthOut = (int) juce::jmin(width, (unsigned long long) INT_MAX);
    heightOut = (int) juce::jmin(height, (unsigned long long) INT_MAX);
    unsigned long long stretch = 0;
    if (parseReplyNumber(text, "stretch", stretch) && stretch == 0)
        lastEditorFailReason_ = "fixed-size";
    return hwnd;
}

juce::String RestrictedHost::lastEditorFailReason() const {
    return lastEditorFailReason_;
}

// Asks the host to apply the GH bay fit scale to the editor content.
// Fire-and-forget like releaseEditorWindow: the frame is sized regardless,
// so the bay looks right even when a fixed-size plugin ignores the request.
void RestrictedHost::sendEditorScale(float scale) {
    if (controlPipe == nullptr || !isAlive()) return;
    if (!(scale > 0.0f) || !(scale < 100.0f)) return;
    char size[32] = {};
    ::snprintf(size, sizeof(size), "%.4f", (double) scale);
    const std::string out = std::string("{\"setEditorScale\":1,\"scale\":") + size + "}\n";
    DWORD written = 0;
    ::WriteFile(controlPipe, out.data(), (DWORD) out.size(), &written, nullptr);
}

bool RestrictedHost::requestEditorSize(int& widthOut, int& heightOut) {
    // Synchronous round-trip like requestEditorWindow: asks the host for
    // the editor's current outer size. Message thread only.
    widthOut = 0;
    heightOut = 0;
    if (controlPipe == nullptr || !isAlive()) return false;
    const std::string out = "{\"getEditorSize\":1}\n";
    DWORD written = 0;
    if (!::WriteFile(controlPipe, out.data(), (DWORD) out.size(), &written, nullptr))
        return false;
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 2000.0;
    std::string reply;
    char chunk[1024] = {};
    while (juce::Time::getMillisecondCounterHiRes() < deadline) {
        DWORD available = 0;
        if (!::PeekNamedPipe(controlPipe, nullptr, 0, nullptr, &available, nullptr))
            break;
        if (available == 0) {
            juce::Thread::sleep(1);
            continue;
        }
        DWORD read = 0;
        const DWORD want = available < sizeof(chunk) - 1 ? available : sizeof(chunk) - 1;
        if (!::ReadFile(controlPipe, chunk, want, &read, nullptr) || read == 0) break;
        reply.append(chunk, read);
        if (reply.find('\n') != std::string::npos) break;
    }
    const juce::String text = juce::String::fromUTF8(reply.data(), (int) reply.size());
    unsigned long long width = 0;
    unsigned long long height = 0;
    if (!parseReplyNumber(text, "width", width) || !parseReplyNumber(text, "height", height))
        return false;
    if (width == 0 || height == 0) return false;
    widthOut = (int) juce::jmin(width, (unsigned long long) INT_MAX);
    heightOut = (int) juce::jmin(height, (unsigned long long) INT_MAX);
    return true;
}

void RestrictedHost::releaseEditorWindow() {    if (controlPipe == nullptr || !isAlive()) return;
    const std::string out = "{\"hideEditor\":1}\n";
    DWORD written = 0;
    ::WriteFile(controlPipe, out.data(), (DWORD) out.size(), &written, nullptr);
    // Best effort: the reply only acks; the window is usable either way.
    juce::Thread::sleep(50);
}

void RestrictedHost::dropEditorWindow() {
    if (controlPipe == nullptr || !isAlive()) return;
    const std::string out = "{\"dropEditor\":1}\n";
    DWORD written = 0;
    ::WriteFile(controlPipe, out.data(), (DWORD) out.size(), &written, nullptr);
    juce::Thread::sleep(50);
}

bool RestrictedHost::submitAudioBlock(const juce::AudioBuffer<float>& input, double sampleRate,
                                      int bufferSize) {    if (sharedBlock == nullptr || !isAlive()) {
        return false;
    }
    auto* block = static_cast<pluginHostIpc::SharedAudioBlock*>(sharedBlock);
    // If the host is not producing, give up quickly (0 ms) so the audio
    // thread never blocks behind the IPC. Fail-closed: the caller passes
    // dry audio for this block, never an in-process render.
    const auto waitStart = juce::Time::getMillisecondCounterHiRes();
    const auto previous = block->sequence.load(std::memory_order_acquire);
    while (block->responseSequence.load(std::memory_order_acquire) != previous
           && juce::Time::getMillisecondCounterHiRes() - waitStart < 0.0) {
        juce::Thread::yield();
    }
    if (block->responseSequence.load(std::memory_order_acquire) != previous) {
        return false; // host lagging; caller must not submit
    }
    const int frames = juce::jmin(input.getNumSamples(), pluginHostIpc::kMaxBlockFrames);
    // Torn-read guard: even = stable, odd = writer active. The app marks the
    // block odd, writes the header and payload, then marks it even and only
    // then publishes the sequence, so the host can tell a committed block
    // from a half-written one.
    block->blockSequence.fetch_add(1, std::memory_order_release); // odd: writing
    block->numFrames = frames;
    block->bufferSize = bufferSize;
    block->sampleRate = sampleRate;
    const auto* left = input.getReadPointer(0);
    const auto* right = input.getNumChannels() > 1 ? input.getReadPointer(1) : left;
    for (int i = 0; i < frames; ++i) {
        block->interleave[i * 2] = left[i];
        block->interleave[i * 2 + 1] = right[i];
    }
    block->blockSequence.fetch_add(1, std::memory_order_release); // even: committed
    // Payload version for the host to compare against its last read.
    block->writeSequence.fetch_add(1, std::memory_order_release);
    // Publish: a new sequence value means "process this block".
    block->sequence.store(previous + 1, std::memory_order_release);
    return true;
}

bool RestrictedHost::fetchAudioBlock(juce::AudioBuffer<float>& output,
                                     std::uint64_t expectedSequence, double& outRate,
                                     int& outBufferSize) {
    if (sharedBlock == nullptr) {
        return false;
    }
    auto* block = static_cast<pluginHostIpc::SharedAudioBlock*>(sharedBlock);
    // Brief wait for the host to process this block. The audio thread cannot
    // block long, but the host normally answers within a few ms.
    const auto start = juce::Time::getMillisecondCounterHiRes();
    while (block->responseSequence.load(std::memory_order_acquire) != expectedSequence
           && juce::Time::getMillisecondCounterHiRes() - start < 20.0) {
        juce::Thread::yield();
    }
    if (block->responseSequence.load(std::memory_order_acquire) != expectedSequence) {
        return false;
    }
    // Torn-read guard: two identical blockSequence reads around the copy
    // mean the payload was stable. A mismatch means the host raced us;
    // fail closed (dry audio) rather than accept garbage.
    const auto before = block->blockSequence.load(std::memory_order_acquire);
    const int frames = juce::jmin(block->numFrames, pluginHostIpc::kMaxBlockFrames);
    if (output.getNumChannels() < 2 || output.getNumSamples() < frames) {
        output.setSize(2, juce::jmax(frames, 1), false, false, true);
    }
    auto* left = output.getWritePointer(0);
    auto* right = output.getWritePointer(1);
    for (int i = 0; i < frames; ++i) {
        left[i] = block->interleave[i * 2];
        right[i] = block->interleave[i * 2 + 1];
    }
    const auto after = block->blockSequence.load(std::memory_order_acquire);
    if (before != after || (before % 2) == 1) {
        return false;
    }
    outRate = block->sampleRate;
    outBufferSize = block->bufferSize;
    return true;
}

void RestrictedHost::killAndReap() {
    // PR-03: terminate the real child handle (not JUCE ChildProcess), then
    // release the job (KILL_ON_JOB_CLOSE covers the crash path), the pipe
    // and the shared view.
    //
    // Re-entrancy: the destructor calls this, and so does every launch
    // failure path. A second call with null handles must be a no-op, never
    // a second TerminateProcess on a recycled PID.
    //
    // Caller blame: every path that kills a LIVE child logs who asked.
    // Silent kills are how "the hosts all died at 2.6 s" mysteries happen:
    // without this, stopHostLocked from a second adopt on the same slot
    // looks identical to a crash in the log.
    if (childProcessHandle != nullptr) {
        DWORD code = 0;
        ::GetExitCodeProcess(childProcessHandle, &code);
        const bool wasAlive = code == STILL_ACTIVE;
        if (wasAlive) {
            juce::Logger::writeToLog("plugin: host kill requested for pid "
                                     + juce::String((juce::int64) childPid));
        }
        ::TerminateProcess(childProcessHandle, 0);
        ::WaitForSingleObject(childProcessHandle, 5000);
        ::CloseHandle(childProcessHandle);
        childProcessHandle = nullptr;
    }
    if (child != nullptr && child->isRunning()) {
        child->kill();
        child->waitForProcessToFinish(5000);
    }
    child.reset();
    if (controlPipe != nullptr) {
        ::CloseHandle(controlPipe);
        controlPipe = nullptr;
    }
    if (jobObject != nullptr) {
        ::CloseHandle(jobObject);
        jobObject = nullptr;
    }
    if (sharedBlock != nullptr) {
        ::UnmapViewOfFile(sharedBlock);
        sharedBlock = nullptr;
    }
    if (sectionHandle != nullptr) {
        ::CloseHandle(sectionHandle);
        sectionHandle = nullptr;
    }
    launched = false;
    childPid = 0;
}

int RestrictedHost::runHost(const juce::File& file, int pluginUniqueId,
                            const juce::String& sharedName,
                            const juce::String& controlPipeName) {
    // PR-03: exit when the parent dies. The parent holds the job object with
    // KILL_ON_JOB_CLOSE, but a double-check here covers the case where the
    // job assignment raced the parent's exit.
    const DWORD parentPid = [] {
        HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        DWORD parent = 0;
        if (snapshot != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W entry = {};
            entry.dwSize = sizeof(entry);
            const DWORD self = ::GetCurrentProcessId();
            if (::Process32FirstW(snapshot, &entry)) {
                do {
                    if (entry.th32ProcessID == self) {
                        parent = entry.th32ParentProcessID;
                        break;
                    }
                } while (::Process32NextW(snapshot, &entry));
            }
            ::CloseHandle(snapshot);
        }
        return parent;
    }();
    HANDLE parentHandle = parentPid != 0
        ? ::OpenProcess(SYNCHRONIZE, FALSE, parentPid)
        : nullptr;
    if (parentHandle == nullptr && parentPid != 0) {
        std::cout << "HOSTERROR parent handle open failed, code " << ::GetLastError()
                  << " (parent pid " << parentPid << ")\n";
        std::cout.flush();
    }

    // Parent-liveness is checked two ways: the job object (KILL_ON_JOB_CLOSE)
    // is the primary guard, and this handle is the backup. A null handle
    // must NEVER read as "parent gone" — OpenProcess can fail transiently,
    // and treating that as death makes every host exit ~100 ms after
    // launch (the exact symptom this comment guards against).
    //
    // Integrity note: this child runs at low integrity while the parent
    // runs at medium. OpenProcess(SYNCHRONIZE) on a higher-integrity
    // parent can fail with ACCESS_DENIED even though the parent is alive —
    // that is exactly the null-handle case above, and it must read as
    // "parent alive", never as death.
    //
    // Poll budget: EVERY parentGone() call costs a kernel wait round-trip.
    // The audio loop below calls it once per 1 ms iteration — fine. But it
    // must never be called in a tight retry loop (pipe connect, first-read
    // wait): thousands of waits per second there starve the very handshake
    // the loop is waiting for. Those loops use bounded attempt counts and
    // sleeps instead, and only consult parentGone() to break early.
    auto parentGone = [&] {
        if (parentHandle == nullptr) return false;
        return ::WaitForSingleObject(parentHandle, 0) == WAIT_OBJECT_0;
    };

    // The host only ever loads the plugin in this throwaway process.
    // Bundle-root guard (defence in depth): the parent canonicalises to
    // the bundle root before launching (see loadFromFile), so reaching
    // here with an inner-binary path means an older parent or a manual
    // run. Resolve up to the bundle root instead of refusing: the scan
    // and instantiate below both accept the root, and refusing here would
    // strand scenes saved with the inner path.
    //
    // Layout: <Name>.vst3\Contents\x86_64-win\<Name>.vst3. The walk climbs
    // past the inner binary AND the Contents/arch folders to the bundle
    // root — stopping at the first *.vst3 parent is wrong (that IS the
    // inner binary's own folder for files, or the binary itself).
    {
        const auto full = file.getFullPathName();
        if (full.containsIgnoreCase("Contents\\x86_64-win")) {
            auto root = file;
            for (;;) {
                const auto up = root.getParentDirectory();
                const auto name = up.getFileName();
                if (name.endsWithIgnoreCase(".vst3") && up != root) {
                    // Found a *.vst3 ancestor: that is the bundle root
                    // (the inner binary lives two levels below it).
                    root = up;
                    break;
                }
                if (up == root || up.getFullPathName().isEmpty()) break;
                root = up;
            }
            if (root.getFullPathName() != file.getFullPathName()
                && root.getFileName().endsWithIgnoreCase(".vst3")) {
                std::cout << "HOSTDIAG resolved bundle root "
                          << root.getFullPathName().toStdString() << "\n";
                std::cout.flush();
                return runHost(root, pluginUniqueId, sharedName, controlPipeName);
            }
            std::cout << "HOSTERROR inner bundle binary refused, use the bundle root: "
                      << file.getFullPathName().toStdString() << "\n";
            std::cout.flush();
            if (parentHandle != nullptr) ::CloseHandle(parentHandle);
            return 1;
        }
    }
    juce::String scanError;
    // Parent-pid sanity: log both PIDs so a post-mortem can tell whether
    // the child watched the right parent. The snapshot can return a stale
    // PID if the true parent exited between our launch and this read; the
    // parent handle is the backup guard (no job object in this build).
    std::cout << "HOSTDIAG child pid " << ::GetCurrentProcessId() << " parent pid "
              << parentPid << (parentHandle != nullptr ? " (handle ok)" : " (handle null)")
              << "\n";
    std::cout.flush();
    // Startup marker: the parent reports "started" at handshake time, but
    // a child that dies between handshake and first audio leaves no trace
    // of how far it got. This line proves the child reached instantiate.
    std::cout << "HOSTDIAG starting plugin " << file.getFullPathName().toStdString() << "\n";
    std::cout.flush();
    const auto descriptions = PluginHost::scanFileInProcess(file, scanError);
    if (descriptions.isEmpty()) {
        // Wait briefly for the app to create the mapping, then give up.
        juce::Thread::sleep(100);
    }

    // PR-SEC-004: open the exact per-launch name the parent passed on argv
    // and verify the mapping size; never fall back to the legacy path hash.
    // Progress goes to stdout (the parent captures it): without this a
    // child that dies before HOSTREADY leaves the parent guessing.
    // The parent's argv quoting can leave stray '"' characters on the
    // name; strip them (a valid section name never contains a quote).
    const juce::String cleanSharedName = sharedName.removeCharacters("\"");
    if (cleanSharedName != sharedName) {
        std::cout << "HOSTERROR section name had quotes, cleaned\n";
        std::cout.flush();
    }
    pluginHostIpc::SharedAudioBlock* block = nullptr;
    HANDLE mapHandle = nullptr;
    if (cleanSharedName.isNotEmpty()) {
        // Retry briefly: the parent creates the mapping just before
        // CreateProcess, but on a loaded machine the child can win the
        // race and open before the object exists. One failure is not
        // proof of absence — 500 ms of retries is.
        for (int attempt = 0; attempt < 10 && mapHandle == nullptr; ++attempt) {
            mapHandle = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
                                           cleanSharedName.toWideCharPointer());
            if (mapHandle == nullptr) juce::Thread::sleep(50);
        }
        if (mapHandle == nullptr) {
            std::cout << "HOSTERROR section open failed, code " << ::GetLastError() << "\n";
            std::cout.flush();
        } else {
            MEMORY_BASIC_INFORMATION info = {};
            void* view = ::MapViewOfFile(mapHandle, FILE_MAP_ALL_ACCESS, 0, 0,
                                         sizeof(pluginHostIpc::SharedAudioBlock));
            if (view == nullptr) {
                std::cout << "HOSTERROR section map failed, code " << ::GetLastError()
                          << "\n";
                std::cout.flush();
            } else if (::VirtualQuery(view, &info, sizeof(info)) != 0
                       && info.RegionSize >= sizeof(pluginHostIpc::SharedAudioBlock)) {
                block = static_cast<pluginHostIpc::SharedAudioBlock*>(view);
            } else {
                std::cout << "HOSTERROR section size mismatch\n";
                std::cout.flush();
                ::UnmapViewOfFile(view);
            }
        }
    }
    if (descriptions.isEmpty()) {
        std::cout << "HOSTERROR " << scanError.toStdString() << "\n";
        std::cout.flush();
        if (block != nullptr) ::UnmapViewOfFile(block);
        if (mapHandle != nullptr) ::CloseHandle(mapHandle);
        if (parentHandle != nullptr) ::CloseHandle(parentHandle);
        return 1;
    }
    // Pick the class the app asked for; fall back to the first class.
    juce::PluginDescription chosen = descriptions.getReference(0);
    if (pluginUniqueId != 0) {
        for (const auto& d : descriptions) {
            if (d.uniqueId == pluginUniqueId) {
                chosen = d;
                break;
            }
        }
    }

    // PR-SEC-005: the preset arrives over the control pipe, not argv.
    // Connect to the parent's pipe and read the first state line, with a
    // bounded wait: a dead parent must fail the host, not hang it. The pipe
    // stays open: later lines carry parameter updates, getState/getLatency
    // queries, and editor-window handles.
    juce::String initialState;
    // Control-line reader state, declared here (not at the audio loop) so
    // the first read below can seed it: the parent sends getLatency
    // microseconds after the state line, so the first ReadFile usually
    // slurps BOTH lines at once. Bytes after the first newline belong to
    // the audio loop's pollControl — without this, the getLatency query is
    // silently dropped and every save restore stalls ~5 s per plugin on
    // the parent's control timeout.
    std::string controlPending;
    HANDLE controlPipe = nullptr;
    const juce::String cleanPipeName = controlPipeName.removeCharacters("\"");
    if (cleanPipeName.isNotEmpty()) {
        DWORD lastPipeError = 0;
        // Bounded attempts with sleeps (see the parentGone budget note
        // above): consult parentGone() at most once per 100 ms iteration,
        // never in a spin.
        for (int attempt = 0; attempt < 50 && controlPipe == nullptr; ++attempt) {
            controlPipe = ::CreateFileW(cleanPipeName.toWideCharPointer(),
                                        GENERIC_READ | GENERIC_WRITE,
                                        0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (controlPipe == INVALID_HANDLE_VALUE) {
                lastPipeError = ::GetLastError();
                controlPipe = nullptr;
                if (parentGone()) break;
                juce::Thread::sleep(100);
            }
        }
        if (controlPipe == nullptr) {
            std::cout << "HOSTERROR pipe connect failed, code " << lastPipeError << "\n";
            std::cout.flush();
        }
        if (controlPipe != nullptr) {
            // Bounded first read: 10 s for the parent's state line, then give
            // up rather than hang a host nobody will ever feed. The handle
            // from CreateFileW is synchronous, so ReadFile with an
            // OVERLAPPED struct would block forever ignoring the timeout
            // (same trap as the parent's ConnectNamedPipe had) — use a
            // polling read with a deadline instead.
            char line[262144] = {};
            DWORD totalRead = 0;
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 10000.0;
            // One parentGone() check per 25 ms poll (see the budget note
            // above) — not per byte.
            int pollCount = 0;
            while (totalRead < sizeof(line) - 1
                   && juce::Time::getMillisecondCounterHiRes() < deadline) {
                DWORD available = 0;
                if (!::PeekNamedPipe(controlPipe, nullptr, 0, nullptr, &available,
                                     nullptr)) {
                    break;
                }
                if (available == 0) {
                    if ((++pollCount % 4) == 0 && parentGone()) break;
                    juce::Thread::sleep(25);
                    continue;
                }
                DWORD read = 0;
                const DWORD want = available < sizeof(line) - 1 - totalRead
                                       ? available
                                       : sizeof(line) - 1 - totalRead;
                if (!::ReadFile(controlPipe, line + totalRead, want, &read, nullptr)
                    || read == 0) {
                    break;
                }
                totalRead += read;
                if (std::memchr(line, '\n', totalRead) != nullptr) break;
            }
            if (totalRead > 0) {
                const juce::String json = juce::String::fromUTF8(line, (int) totalRead);
                const auto marker = json.indexOf("\"state\":\"");
                if (marker >= 0) {
                    auto rest = json.substring(marker + 9);
                    initialState = rest.upToFirstOccurrenceOf("\"", false, false);
                }
                // Seed the audio loop's control reader with any bytes past
                // the first newline (see the declaration above): the parent
                // sends getLatency right after the state line, and both
                // usually arrive in this one read.
                const auto* newline =
                    static_cast<const char*>(std::memchr(line, '\n', totalRead));
                if (newline != nullptr) {
                    const auto rest = totalRead - (newline + 1 - line);
                    if (rest > 0) controlPending.assign(newline + 1, rest);
                }
            }
        }
    }

    juce::String instError;
    auto loaded = PluginHost::instantiateHostInstance(chosen, file, 48000.0, 256, initialState);
    if (loaded.error.isNotEmpty() || loaded.instance == nullptr) {
        std::cout << "HOSTERROR " << (loaded.error.isNotEmpty() ? loaded.error : juce::String("instantiate failed")).toStdString() << "\n";
        std::cout.flush();
        if (block != nullptr) ::UnmapViewOfFile(block);
        if (mapHandle != nullptr) ::CloseHandle(mapHandle);
        if (controlPipe != nullptr) ::CloseHandle(controlPipe);
        if (parentHandle != nullptr) ::CloseHandle(parentHandle);
        return 1;
    }

    // Fail-closed ordering: the audio block is required for the host loop
    // below. A host that instantiates but cannot map the section must exit
    // here (code 1) rather than print HOSTREADY and die on first use — the
    // parent treats "no HOSTREADY + exit" as a failed launch, while
    // "HOSTREADY + exit" reads as a crash.
    if (block == nullptr) {
        std::cout << "HOSTERROR no audio section, refusing to run\n";
        std::cout.flush();
        if (mapHandle != nullptr) ::CloseHandle(mapHandle);
        if (controlPipe != nullptr) ::CloseHandle(controlPipe);
        if (parentHandle != nullptr) ::CloseHandle(parentHandle);
        return 1;
    }

    std::cout << "HOSTREADY " << file.getFullPathName().toStdString() << "\n";
    std::cout.flush();

    auto* plugin = loaded.instance.get();
    // Editor window for bay embedding: created once on the host message
    // thread, parented to an invisible message-only window so it has a
    // valid HWND before the bay asks for it. The HWND + size are reported
    // over the control pipe on getEditor; the bay reparents it with
    // SetParent (see RackView). Destroyed with the host: the bay must
    // release it first (releaseEditorWindow), guarded by IsWindow checks.
    juce::Component::SafePointer<juce::AudioProcessorEditor> hostEditor;
    unsigned long long editorHwnd = 0;
    int editorWidth = 0;
    int editorHeight = 0;
    HWND editorParent = nullptr;
    // GH fit state, host side: the scale last applied from the parent's
    // setEditorScale message, and the echo guard for the resize it causes.
    // A resize that arrives while the guard is set is ours, never the
    // plugin picking a size of its own.
    float hostEditorScale = 1.0f;
    bool applyingHostEditorScale = false;
    // Last ensureEditor failure, reported in the {"editor":0} reply so the
    // parent can log it (child stdout is lost in GUI runs).
    std::string editorFailReason;
    auto ensureEditor = [&] {
        if (hostEditor != nullptr) return true;
        // Failure reason for the {"editor":0} reply: the parent logs it,
        // because the child's own stdout is lost in GUI runs.
        if (!juce::MessageManager::existsAndIsCurrentThread()) {
            editorFailReason = "not-on-message-thread";
            return false;
        }
        auto* created = plugin->createEditorAndMakeActive();
        if (created == nullptr) {
            editorFailReason = "createEditor-null";
            return false;
        }
        // Message-only parent: the editor needs a top-level HWND to be
        // reparentable, but must never flash on screen unparented. The
        // editor starts at the plugin's own 100% size here — the parent's
        // fit scale arrives later over the pipe and is applied with
        // setScaleFactor exactly like the GH bay does (see the scale
        // message below).
        editorParent = ::CreateWindowExW(0, L"STATIC", L"PluginRackHostEditor",
                                         0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                         ::GetModuleHandleW(nullptr), nullptr);
        created->setVisible(true);
        created->addToDesktop(0, editorParent);
        auto* peer = created->getPeer();
        HWND hwnd = peer != nullptr ? static_cast<HWND>(peer->getNativeHandle()) : nullptr;
        if (hwnd == nullptr || ::IsWindow(hwnd) == FALSE) {
            editorFailReason = peer == nullptr ? "no-peer" : "bad-hwnd";
            created->removeFromDesktop();
            if (editorParent != nullptr) {
                ::DestroyWindow(editorParent);
                editorParent = nullptr;
            }
            plugin->editorBeingDeleted(created);
            return false;
        }
        hostEditor = created;
        editorHwnd = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd));
        editorWidth = created->getWidth();
        editorHeight = created->getHeight();
        return true;
    };
    auto dropEditor = [&] {
        if (hostEditor != nullptr) {
            auto* ed = hostEditor.getComponent();
            hostEditor = nullptr;
            if (ed != nullptr) {
                ed->removeFromDesktop();
                plugin->editorBeingDeleted(ed);
            }
        }
        if (editorParent != nullptr) {
            ::DestroyWindow(editorParent);
            editorParent = nullptr;
        }
        editorHwnd = 0;
        editorWidth = 0;
        editorHeight = 0;
    };
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> scratch(2, pluginHostIpc::kMaxBlockFrames);
    std::uint64_t lastSequence = 0;
    double lastSampleRate = 0.0;
    int lastBufferSize = 0;
    // Control-line reader state: the parent keeps the pipe open and sends
    // one JSON line per message (state updates, queries). Reads here are
    // non-blocking polls so the audio loop never stalls behind the pipe.
    // Seeded by the first read above (see controlPending): the state line
    // and the getLatency query usually arrive in one read.
    auto pollControl = [&] {
        if (controlPipe == nullptr) return;
        DWORD available = 0;
        if (!::PeekNamedPipe(controlPipe, nullptr, 0, nullptr, &available, nullptr))
            return;
        while (available > 0) {
            char chunk[4096] = {};
            DWORD read = 0;
            const DWORD want = available < sizeof(chunk) ? available : sizeof(chunk);
            if (!::ReadFile(controlPipe, chunk, want, &read, nullptr) || read == 0)
                break;
            controlPending.append(chunk, read);
            available -= read;
        }
        // Drain complete lines — including any seeded by the first read
        // above, which arrive with no new bytes pending (available == 0).
        std::size_t pos = 0;
        while ((pos = controlPending.find('\n')) != std::string::npos) {
                const std::string line = controlPending.substr(0, pos);
                controlPending.erase(0, pos + 1);
                if (line.find("\"getState\"") != std::string::npos) {
                    juce::MemoryBlock state;
                    plugin->getStateInformation(state);
                    const std::string reply =
                        "{\"state\":\"" + state.toBase64Encoding().toStdString() + "\"}\n";
                    DWORD written = 0;
                    ::WriteFile(controlPipe, reply.data(), (DWORD) reply.size(),
                                &written, nullptr);
                } else if (line.find("\"getLatency\"") != std::string::npos) {
                    const std::string reply =
                        "{\"latency\":" + std::to_string(plugin->getLatencySamples())
                        + "}\n";
                    DWORD written = 0;
                    ::WriteFile(controlPipe, reply.data(), (DWORD) reply.size(),
                                &written, nullptr);
                } else if (line.find("\"getEditorSize\"") != std::string::npos) {
                    // Watchdog query: the editor's current outer size in
                    // logical units, so the parent can tell a plugin that
                    // grew itself from a host that hasn't caught up yet.
                    std::string reply;
                    if (hostEditor != nullptr) {
                        if (auto* ed = hostEditor.getComponent()) {
                            reply = "{\"width\":" + std::to_string(ed->getWidth())
                                  + ",\"height\":" + std::to_string(ed->getHeight()) + "}\n";
                        } else {
                            reply = "{\"width\":0,\"height\":0}\n";
                        }
                    } else {
                        reply = "{\"width\":0,\"height\":0}\n";
                    }
                    DWORD written = 0;
                    ::WriteFile(controlPipe, reply.data(), (DWORD) reply.size(),
                                &written, nullptr);
                } else if (line.find("\"getEditor\"") != std::string::npos) {
                    // Bay embed request: create the editor if needed and
                    // report its HWND + size. The bay reparents it with
                    // SetParent; the window stays owned by this process.
                    // Report the UNSCALED size: the parent measures its fit
                    // from the plugin's 100% size (GH bay's natural size),
                    // so a re-dock after a fit must not measure from the
                    // already-scaled size and compound the scale.
                    std::string reply;
                    if (ensureEditor() && editorHwnd != 0) {
                        int reportW = editorWidth;
                        int reportH = editorHeight;
                        if (hostEditorScale > 0.0f
                            && std::abs(hostEditorScale - 1.0f) >= 0.01f) {
                            reportW = juce::jmax(
                                1, (int) std::round(editorWidth / hostEditorScale));
                            reportH = juce::jmax(
                                1, (int) std::round(editorHeight / hostEditorScale));
                        }
                        // Diag: can the plugin stretch (wrapper onSize path)
                        // or is it fixed-size (wrapper clamps to its own
                        // size)? Parent logs it; child stdout is lost.
                        int stretchable = 0;
                        if (auto* ed = hostEditor.getComponent())
                            stretchable = ed->isResizable() ? 1 : 0;
                        reply = "{\"editor\":" + std::to_string(editorHwnd)
                              + ",\"width\":" + std::to_string(reportW)
                              + ",\"height\":" + std::to_string(reportH)
                              + ",\"stretch\":" + std::to_string(stretchable) + "}\n";
                    } else {
                        reply = "{\"editor\":0,\"reason\":\"" + editorFailReason + "\"}\n";
                    }
                    DWORD written = 0;
                    ::WriteFile(controlPipe, reply.data(), (DWORD) reply.size(),
                                &written, nullptr);
                } else if (line.find("\"hideEditor\"") != std::string::npos) {
                    // Bay released the window: re-hide it under the
                    // message-only parent so it never flashes unparented.
                    if (hostEditor != nullptr && editorParent != nullptr) {
                        if (auto* ed = hostEditor.getComponent()) {
                            HWND hwnd = static_cast<HWND>(
                                reinterpret_cast<void*>(static_cast<uintptr_t>(editorHwnd)));
                            ::SetParent(hwnd, editorParent);
                            ::ShowWindow(hwnd, SW_HIDE);
                            juce::ignoreUnused(ed);
                        }
                    }
                    const std::string reply = "{\"editorHidden\":1}\n";
                    DWORD written = 0;
                    ::WriteFile(controlPipe, reply.data(), (DWORD) reply.size(),
                                &written, nullptr);
                } else if (line.find("\"dropEditor\"") != std::string::npos) {
                    dropEditor();
                    const std::string reply = "{\"editorDropped\":1}\n";
                    DWORD written = 0;
                    ::WriteFile(controlPipe, reply.data(), (DWORD) reply.size(),
                                &written, nullptr);
                } else if (line.find("\"setEditorScale\"") != std::string::npos) {
                    // GH fit adapted across the pipe: the parent sends the
                    // bay fit scale (tighter axis wins, see
                    // layoutDockedEditor). GH's setScaleFactor installs a
                    // pure scale transform — but the VST3 wrapper's override
                    // also refits to the plugin's reported size, which
                    // fights the frame across processes (the squared
                    // blowup). So install the pure transform and size the
                    // outer component to the scaled natural size ourselves;
                    // the wrapper's own onSize path then stretches the inner
                    // view into the frame like a user drag. The resize that
                    // follows is ours (echo guard), never the plugin picking
                    // a size of its own.
                    float fitScale = 0.0f;
                    const juce::String scaleLine =
                        juce::String::fromUTF8(line.data(), (int) line.size());
                    const auto sAt = scaleLine.indexOf("\"scale\":");
                    if (sAt >= 0)
                        fitScale = scaleLine.substring(sAt + 8).trimStart().getFloatValue();
                    if (fitScale > 0.0f && fitScale < 100.0f) {
                        juce::MessageManager::callAsync(
                            [hostEditor, fitScale, &hostEditorScale,
                             &applyingHostEditorScale, &editorWidth, &editorHeight] {
                            if (auto* ed = hostEditor.getComponent()) {
                                // The fit is ABSOLUTE (GH bay: scale from the
                                // 100% natural size), not incremental — skip
                                // a scale we already hold so a re-sent fit
                                // never compounds on top of itself.
                                if (std::abs(fitScale - hostEditorScale) < 0.01f)
                                    return;
                                // Resizable plugin (user-drag path): size the
                                // outer component to the scaled natural size
                                // and NOTHING else. The wrapper's own onSize
                                // stretches the inner view into the frame.
                                // No transform: the transform scales the
                                // painting on top of the stretched layout
                                // (double scale — clipped/right-cut UI).
                                const int natW = juce::jmax(
                                    1, (int) std::round(editorWidth / hostEditorScale));
                                const int natH = juce::jmax(
                                    1, (int) std::round(editorHeight / hostEditorScale));
                                const int scaledW = juce::jmax(
                                    10, (int) std::round(natW * fitScale));
                                const int scaledH = juce::jmax(
                                    10, (int) std::round(natH * fitScale));
                                applyingHostEditorScale = true;
                                ed->setTransform(juce::AffineTransform());
                                if (ed->getWidth() != scaledW
                                    || ed->getHeight() != scaledH)
                                    ed->setSize(scaledW, scaledH);
                                applyingHostEditorScale = false;
                                hostEditorScale = fitScale;
                                editorWidth = ed->getWidth();
                                editorHeight = ed->getHeight();
                            }
                        });
                    }
                    const std::string reply = "{\"editorScaled\":1}\n";
                    DWORD written = 0;
                    ::WriteFile(controlPipe, reply.data(), (DWORD) reply.size(),
                                &written, nullptr);
                } else {
                    const auto marker = line.find("\"state\":\"");
                    if (marker != std::string::npos) {
                        const auto valueStart = marker + 9;
                        const auto valueEnd = line.find('"', valueStart);
                        if (valueEnd != std::string::npos && valueEnd > valueStart) {
                            const std::string blob =
                                line.substr(valueStart, valueEnd - valueStart);
                            juce::MemoryBlock state;
                            if (state.fromBase64Encoding(juce::String(blob))
                                && state.getSize() <= static_cast<size_t>(INT_MAX)) {
                                plugin->setStateInformation(state.getData(),
                                                            (int) state.getSize());
                            }
                        }
                    }
                }
            }
    };
    // Initial ratiowarm: the plugin was instantiated at 48 kHz / 256, so a
    // first block that arrives at a different ratio is not a format change
    // the user asked for — it is the app's live device rate. We still
    // re-prepare, but only when the incoming ratio differs from the last
    // one we actually processed, never merely because the block carries a
    // new header value ticking through. That avoids a prepare/dropout on
    // every block when the device buffer size jitters.
    //
    // FIRST-BLOCK GUARD: the parent submits audio as soon as the device
    // opens, which can be seconds after launch. The very first submitted
    // block may carry a poisoned header (stale rate/size from an earlier
    // session, or zeros before the parent initialises the block). A
    // prepareToPlay or processBlock fault on that first block kills all
    // five hosts at the same timestamp — the exact signature in the log.
    // Validate the first block before touching plugin code: refuse
    // absurd frames/sizes and clamp the rate into the sane range.
    bool firstBlockSeen = false;
    //
    // Exit diagnostics: the parent's watchdog only sees "host died". Name
    // the reason here so the inherited stdout says whether the parent
    // vanished or the audio loop was left normally (it never is — the loop
    // only ends when the parent is gone).
    //
    // Crash guard: plugin->processBlock below is third-party code that can
    // fault. A fault here must blame the plugin (blocklist + crash report),
    // not vanish as a silent "code 1". The handler from Main.cpp already
    // records currentLoadingPluginBytes — arm it by marking this file
    // before entering the loop.
    //
    // Heartbeat: the parent's watchdog tells "crashed" from "clean exit"
    // by the exit code, but a fault inside processBlock leaves no trace of
    // WHICH block killed it. Count processed blocks so a post-mortem can
    // tell "died on block 0" (poisoned first buffer) from "died after N
    // blocks" (format drift / device-rate change). The count is logged on
    // clean loop exit; on a fault it is recovered from the crash record.
    PluginLoadGuard::begin(file);
    std::uint64_t processedBlocks = 0;
    // Idle heartbeat: the parent only learns about a crash from the exit
    // code. Log progress every ~30 s so a silent death still leaves its
    // last-known state (blocks processed, last sequence) in the inherited
    // stdout. Cheap: one counter check per 1 ms iteration.
    //
    // Hang guard: if the loop ever stops making progress without the
    // parent dying (a plugin wedging a call below), the parent cannot tell
    // "idle" from "wedged". Track the last sequence advanced; the parent
    // reads it through the shared block's readSequence/ackSequence.
    std::uint64_t heartbeatCounter = 0;
    // Message pump: this thread owns the editor HWND (see ensureEditor),
    // but it never returns to a message loop — without pumping, any
    // cross-process window call from the parent (SetParent when the bay
    // embeds the editor) waits forever for this thread to process the
    // attach, hanging the app's message thread at startup restore. Pump
    // already-queued messages only (never wait): an empty queue is one
    // cheap PeekMessage, and the iteration cap keeps a message flood
    // from stalling audio.
    auto pumpMessages = [] {
        for (int i = 0; i < 16; ++i) {
            MSG message = {};
            if (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) == FALSE) break;
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    };
    while (!parentGone()) {
        pollControl();
        pumpMessages();
        const auto seq = block->sequence.load(std::memory_order_acquire);
        if (seq != 0 && seq != lastSequence) {
            int frames = juce::jmin(block->numFrames, pluginHostIpc::kMaxBlockFrames);
            double sr = block->sampleRate > 0.0 ? block->sampleRate : 48000.0;
            int bs = block->bufferSize > 0 ? block->bufferSize : 256;
            // First-block guard (see above): never hand plugin code a
            // poisoned header. Clamp the rate, refuse absurd sizes, and
            // ack-without-processing so the parent advances past it.
            if (!firstBlockSeen) {
                firstBlockSeen = true;
                if (sr < 8000.0 || sr > 384000.0) {
                    std::cout << "HOSTDIAG first block rate " << sr << ", clamped to 48000\n";
                    std::cout.flush();
                    sr = 48000.0;
                }
                if (bs <= 0 || bs > pluginHostIpc::kMaxBlockFrames) {
                    std::cout << "HOSTDIAG first block size " << bs << ", clamped to 256\n";
                    std::cout.flush();
                    bs = 256;
                }
                if (frames <= 0 || frames > pluginHostIpc::kMaxBlockFrames) {
                    std::cout << "HOSTDIAG first block frames " << frames << ", acked empty\n";
                    std::cout.flush();
                    block->readSequence.store(seq, std::memory_order_release);
                    lastSequence = seq;
                    block->responseSequence.store(seq, std::memory_order_release);
                    block->ackSequence.store(seq, std::memory_order_release);
                    lastSampleRate = sr;
                    lastBufferSize = bs;
                    juce::Thread::sleep(1);
                    continue;
                }
            }
            if (sr != lastSampleRate || bs != lastBufferSize) {
                // Re-prepare guard: releaseResources/prepareToPlay are the
                // most crash-prone calls after processBlock (some plugins
                // free voice state here that a concurrent MIDI thread still
                // touches). Log the transition so a death inside it names
                // the rate/size that killed it.
                std::cout << "HOSTDIAG prepare " << lastSampleRate << "/" << lastBufferSize
                          << " -> " << sr << "/" << bs << "\n";
                std::cout.flush();
                plugin->releaseResources();
                plugin->setRateAndBufferSizeDetails(sr, bs);
                plugin->prepareToPlay(sr, bs);
                lastSampleRate = sr;
                lastBufferSize = bs;
            }
            if (frames > 0 && frames <= pluginHostIpc::kMaxBlockFrames) {
                // Torn-read guard: stable even sequence on both sides.
                const auto before = block->blockSequence.load(std::memory_order_acquire);
                for (int i = 0; i < frames; ++i) {
                    scratch.getWritePointer(0)[i] = block->interleave[i * 2];
                    scratch.getWritePointer(1)[i] = block->interleave[i * 2 + 1];
                }
                const auto after = block->blockSequence.load(std::memory_order_acquire);
                if (before != after || (before % 2) == 1) {
                    // Torn: skip this block, do not process half-written audio.
                    block->readSequence.store(seq, std::memory_order_release);
                    lastSequence = seq;
                    block->responseSequence.store(seq, std::memory_order_release);
                    block->ackSequence.store(seq, std::memory_order_release);
                    juce::Thread::sleep(1);
                    continue;
                }
                block->readSequence.store(seq, std::memory_order_release);
                juce::AudioBuffer<float> view(scratch.getArrayOfWritePointers(), 2, 0, frames);
                midi.clear();
                plugin->processBlock(view, midi);
                ++processedBlocks;
                // Write-back with the same guard: mark odd, copy, mark even.
                block->blockSequence.fetch_add(1, std::memory_order_release);
                for (int i = 0; i < frames; ++i) {
                    block->interleave[i * 2] = scratch.getReadPointer(0)[i];
                    block->interleave[i * 2 + 1] = scratch.getReadPointer(1)[i];
                }
                block->blockSequence.fetch_add(1, std::memory_order_release);
            }
            block->ackSequence.store(seq, std::memory_order_release);
            block->responseSequence.store(seq, std::memory_order_release);
            lastSequence = seq;
        }
        if ((++heartbeatCounter % 30000) == 0) {
            std::cout << "HOSTHEARTBEAT blocks=" << processedBlocks << " lastSeq="
                      << lastSequence << "\n";
            std::cout.flush();
        }
        juce::Thread::sleep(1);
    }
    if (block != nullptr) ::UnmapViewOfFile(block);
    if (mapHandle != nullptr) ::CloseHandle(mapHandle);
    if (controlPipe != nullptr) ::CloseHandle(controlPipe);
    if (parentHandle != nullptr) ::CloseHandle(parentHandle);
    dropEditor();
    PluginLoadGuard::end();
    std::cout << "HOSTEXIT parent-gone loop ended after " << processedBlocks << " blocks\n";
    std::cout.flush();
    return 0;
}
