# Opt-in WASAPI Channel Probe Cap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an off-by-default Settings toggle plus a 2–64 channel numeric field that caps JUCE's WASAPI `queryMaxNumChannels` probe, cutting slow-endpoint startup from ~12–13 s to ~1–1.5 s at cap 8.

**Architecture:** A small env-gated patch in the external JUCE checkout reads `PLUGINRACK_WASAPI_MAX_PROBE_CHANNELS` and starts the channel loop at `min(64, cap)`. The app writes that variable from `AudioEngine::applySettingsValues`, which is driven by a scene-persisted setting. The patch is shipped in-repo and applied idempotently at CMake configure time and from the build script.

**Tech Stack:** C++20, MSVC `/W4 /WX`, CMake ≥ 3.28 + Ninja, JUCE 9.0.4 (`454fe891`), Catch2 3.7.1.

**Spec:** `docs/superpowers/specs/2026-11-13-wasapi-probe-cap-design.md`

## Global Constraints

- Toggle off by default; off means `PLUGINRACK_WASAPI_MAX_PROBE_CHANNELS=0`, stock JUCE behavior.
- Cap range is exactly `2..64`; default value when the toggle turns on is `8`.
- The environment variable name is exactly `PLUGINRACK_WASAPI_MAX_PROBE_CHANNELS`.
- Scene schema becomes `7`; pre-v7 scenes force the toggle off and the value to 8.
- The JUCE patch must remain inert unless the env var is set; do not change JUCE defaults.
- The app is Windows x64; `_putenv_s` calls must be inside `#if JUCE_WINDOWS`.
- Every task ends with `ctest --test-dir build/dev --output-on-failure` green and, when it changes app/core sources, `powershell -ExecutionPolicy Bypass -File .\tools\build-and-copy.ps1` run and copied.
- Commit messages are Conventional Commits with no AI-attribution trailers.

## Review Focus

1. **Toggle turned off after being on.** The next Apply must write `0` to the env var; a write only on enable would leave the cap active until restart. Pinned by Task 1 (`wasapiProbeCapValue(false, …) == 0`) and Task 3 (the env write is unconditional).
2. **Cap below the device's real channel count.** The device must report/offer at most `cap` channels, and a saved mapping above the cap must degrade to a silent strip, never crash. Covered by the tooltip wording (Task 4) and the manual acceptance in Task 6; no hardware-free test exists.
3. **Pre-v7 scene with hand-edited cap keys.** Loading must force the toggle off regardless of the keys. Pinned in Task 2.
4. **Patch missing or stale while the UI says on.** No speedup is acceptable only if the log reports the requested cap and CMake warned. Pinned by the CMake warning (Task 5) and the `device: wasapi probe cap` log line (Task 3).
5. **Applying while devices are opening.** The Apply button is disabled while `isOpeningDevices()`; the new controls must obey the same busy rule. Pinned in Task 4.

---

### Task 1: Core cap rule

**Files:**
- Create: `core/include/pluginrack/AudioDeviceProbe.h`
- Create: `core/src/AudioDeviceProbe.cpp`
- Modify: `core/CMakeLists.txt`
- Create: `tests/test_audio_device_probe.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `pluginrack::kWasapiProbeCapMinChannels` (2), `pluginrack::kWasapiProbeCapMaxChannels` (64), `int pluginrack::wasapiProbeCapValue(bool enabled, int channels)` returning 0 when disabled and a clamped 2..64 when enabled.

- [ ] **Step 1: Write the failing test**

Create `tests/test_audio_device_probe.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <pluginrack/AudioDeviceProbe.h>

TEST_CASE("a disabled probe cap is zero", "[device][probe]") {
    CHECK(pluginrack::wasapiProbeCapValue(false, 8) == 0);
    CHECK(pluginrack::wasapiProbeCapValue(false, 64) == 0);
}

TEST_CASE("an enabled probe cap is clamped to two through sixty-four", "[device][probe]") {
    CHECK(pluginrack::wasapiProbeCapValue(true, 2) == 2);
    CHECK(pluginrack::wasapiProbeCapValue(true, 8) == 8);
    CHECK(pluginrack::wasapiProbeCapValue(true, 64) == 64);
    CHECK(pluginrack::wasapiProbeCapValue(true, 0) == 2);
    CHECK(pluginrack::wasapiProbeCapValue(true, 1) == 2);
    CHECK(pluginrack::wasapiProbeCapValue(true, -4) == 2);
    CHECK(pluginrack::wasapiProbeCapValue(true, 65) == 64);
}
```

Add `test_audio_device_probe.cpp` to the `add_executable(pluginrack_tests …)` list in `tests/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/dev --target pluginrack_tests` from a shell with the MSVC environment, then `build/dev/tests/pluginrack_tests.exe "*probe cap*"`.
Expected: FAIL to compile with "Cannot open include file: 'pluginrack/AudioDeviceProbe.h'".

- [ ] **Step 3: Implement the header and source**

`core/include/pluginrack/AudioDeviceProbe.h`:

```cpp
#pragma once

namespace pluginrack {

// The opt-in cap the app applies to JUCE's WASAPI channel-count probe.
// Zero means "no cap": the backend keeps its stock 64..default loop.
inline constexpr int kWasapiProbeCapMinChannels = 2;
inline constexpr int kWasapiProbeCapMaxChannels = 64;

int wasapiProbeCapValue(bool enabled, int channels);

} // namespace pluginrack
```

`core/src/AudioDeviceProbe.cpp`:

```cpp
#include <pluginrack/AudioDeviceProbe.h>

#include <algorithm>

namespace pluginrack {

int wasapiProbeCapValue(bool enabled, int channels) {
    if (!enabled) return 0;
    return std::clamp(channels, kWasapiProbeCapMinChannels,
                      kWasapiProbeCapMaxChannels);
}

} // namespace pluginrack
```

Add `src/AudioDeviceProbe.cpp` to the `pluginrack_core` source list in `core/CMakeLists.txt`.

- [ ] **Step 4: Run test to verify it passes**

Run: `build/dev/tests/pluginrack_tests.exe "*probe cap*"`
Expected: All tests passed (9 assertions in 2 test cases).

- [ ] **Step 5: Run the full suite, then commit**

```bash
ctest --test-dir build/dev --output-on-failure
git add core/include/pluginrack/AudioDeviceProbe.h core/src/AudioDeviceProbe.cpp \
        core/CMakeLists.txt tests/test_audio_device_probe.cpp tests/CMakeLists.txt
git commit -m "feat(core): add the wasapi probe cap rule"
```

---

### Task 2: Scene schema v7 for the cap

**Files:**
- Modify: `core/include/pluginrack/Scene.h`
- Modify: `core/src/SceneCodec.cpp`
- Modify: `core/src/SceneValidator.cpp`
- Modify: `tests/test_scene_codec.cpp`
- Modify: `tests/test_scene_validator.cpp`

**Interfaces:**
- Produces: `pluginrack::AudioSettings::wasapiProbeCapEnabled` (bool, default false) and `wasapiProbeCapChannels` (int, default 8); schema version 7.

- [ ] **Step 1: Write the failing tests and update the schema expectations**

In `tests/test_scene_codec.cpp`:

1. Add before the end of the file:

```cpp
TEST_CASE("scene v7 audio probe cap round-trips") {
    pluginrack::Scene original;
    original.schemaVersion = 7;
    pluginrack::AudioSettings audio;
    audio.wasapiProbeCapEnabled = true;
    audio.wasapiProbeCapChannels = 16;
    original.audio = audio;

    const std::string json = pluginrack::toJson(original);
    REQUIRE(json.find("\"schemaVersion\":7") != std::string::npos);
    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(loaded.scene->audio.has_value());
    REQUIRE(*loaded.scene->audio == audio);
}

TEST_CASE("pre-v7 scenes never enable the wasapi probe cap") {
    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(R"({
        "schemaVersion": 6,
        "audio": {"wasapiProbeCapEnabled": true, "wasapiProbeCapChannels": 16}
    })");
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(loaded.scene->audio.has_value());
    CHECK_FALSE(loaded.scene->audio->wasapiProbeCapEnabled);
    CHECK(loaded.scene->audio->wasapiProbeCapChannels == 8);
}
```

2. Change every `original.schemaVersion = 6;` / `expected.schemaVersion = 6;` / `fresh.schemaVersion = 6;` that constructs a *current* scene for a round-trip to `7`, and every `"schemaVersion":6` string assertion to `"schemaVersion":7`. The affected lines are 8, 144, 183, 229, 328, 417, 412, 477, 455, 506, 582, 612, 629, 661. Do **not** change the raw-JSON tests that deliberately load an older scene (lines 527, 546, 569, 599, 620).
3. Change `unsupported schema version is rejected` to load `{"schemaVersion": 8}`.

In `tests/test_scene_validator.cpp`:

1. Add a case:

```cpp
TEST_CASE("the wasapi probe cap is bounded", "[scene][device]") {
    auto scene = makeValidScene();
    scene.audio = pluginrack::AudioSettings{};
    scene.audio->wasapiProbeCapEnabled = true;
    scene.audio->wasapiProbeCapChannels = 1;
    auto errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);

    scene.audio->wasapiProbeCapChannels = 65;
    errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);

    scene.audio->wasapiProbeCapChannels = 2;
    CHECK(pluginrack::validate(scene).empty());
    scene.audio->wasapiProbeCapChannels = 64;
    CHECK(pluginrack::validate(scene).empty());
}
```

Use the existing valid-scene helper in that file (the same one the watched-device cases use; adjust its name if it differs).
2. Change `unsupported schema version is rejected by validation` to set `scene.schemaVersion = 8`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build/dev --target pluginrack_tests` then `build/dev/tests/pluginrack_tests.exe "*probe cap*,*schema version*"`.
Expected: compile errors for the missing fields, or assertion failures on `"schemaVersion":7`.

- [ ] **Step 3: Implement the schema change**

`core/include/pluginrack/Scene.h`:
- `struct Scene { int schemaVersion = 7; ... }`
- In `AudioSettings`, after `ladderRecovery`:

```cpp
    // Scene v7: opt-in cap for JUCE's WASAPI channel-count probe. Off by
    // default; pre-v7 saves never turn it on.
    bool wasapiProbeCapEnabled = false;
    int wasapiProbeCapChannels = 8;
```

`core/src/SceneCodec.cpp`:
- `constexpr int kSchemaVersion = 7;`
- `to_json(AudioSettings)`: add `{"wasapiProbeCapEnabled", settings.wasapiProbeCapEnabled},` and `{"wasapiProbeCapChannels", settings.wasapiProbeCapChannels},`.
- `from_json(AudioSettings)`: read both with defaults `false` / `8`, clamping the channel value:

```cpp
    settings.wasapiProbeCapEnabled = j.value("wasapiProbeCapEnabled", false);
    settings.wasapiProbeCapChannels =
        std::clamp(j.value("wasapiProbeCapChannels", 8), 2, 64);
```

Add `#include <algorithm>` to `core/src/SceneCodec.cpp` if it is not already present.
- In `fromJson(Scene)`: accept `1..7`; add, next to the v6 gate:

```cpp
            // v7 fields are opt-in: a pre-v7 save never turns the cap on.
            if (schemaVersion < 7) {
                settings.wasapiProbeCapEnabled = false;
                settings.wasapiProbeCapChannels = 8;
            }
```

`core/src/SceneValidator.cpp`:
- Change the schema range to `1..7` and the message to "expected 1 to 7".
- Inside `if (scene.audio.has_value())`, next to the watched-device check:

```cpp
        if (scene.audio->wasapiProbeCapEnabled
            && (scene.audio->wasapiProbeCapChannels < 2
                || scene.audio->wasapiProbeCapChannels > 64)) {
            errors.push_back({SceneErrorCode::ValueOutOfRange, 0,
                              "wasapiProbeCapChannels out of range [2, 64]"});
        }
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `build/dev/tests/pluginrack_tests.exe "*probe cap*,*schema version*"`
Expected: all pass.

- [ ] **Step 5: Run the full suite, then commit**

```bash
ctest --test-dir build/dev --output-on-failure
git add core/include/pluginrack/Scene.h core/src/SceneCodec.cpp core/src/SceneValidator.cpp \
        tests/test_scene_codec.cpp tests/test_scene_validator.cpp
git commit -m "feat(scene): persist the opt-in wasapi probe cap in schema v7"
```

---

### Task 3: Engine setting, environment write and scene mapping

**Files:**
- Modify: `app/src/AudioEngine.h`
- Modify: `app/src/AudioEngine.cpp`
- Modify: `tests/test_underrun_paths.cpp`

**Interfaces:**
- Consumes: `pluginrack::wasapiProbeCapValue(bool, int)` from Task 1.
- Produces: `DeviceSettings::wasapiProbeCapEnabled` / `wasapiProbeCapChannels`; a file-local `setWasapiProbeCapEnvironment(bool, int)`.

- [ ] **Step 1: Write the failing source scans**

Append to `tests/test_underrun_paths.cpp`:

```cpp
TEST_CASE("the wasapi probe cap reaches the juce backend", "[device][probe]") {
    const auto engine = readSource("app/src/AudioEngine.cpp");
    const auto header = readSource("app/src/AudioEngine.h");
    CHECK(contains(header, "bool wasapiProbeCapEnabled = false"));
    CHECK(contains(header, "int wasapiProbeCapChannels = 8"));
    CHECK(contains(header, "wasapiProbeCapEnabled_"));
    CHECK(contains(header, "wasapiProbeCapChannels_"));

    const auto env = functionBody(engine, "void setWasapiProbeCapEnvironment");
    CHECK(contains(env, "PLUGINRACK_WASAPI_MAX_PROBE_CHANNELS"));
    CHECK(contains(env, "pluginrack::wasapiProbeCapValue("));
    CHECK(contains(env, "_putenv_s("));

    const auto constructor = functionBody(engine, "AudioEngine::AudioEngine");
    CHECK(contains(constructor, "setWasapiProbeCapEnvironment("));

    const auto apply = functionBody(engine, "void AudioEngine::applySettingsValues");
    CHECK(contains(apply, "wasapiProbeCapEnabled_.store("));
    CHECK(contains(apply, "wasapiProbeCapChannels_.store("));
    CHECK(contains(apply, "setWasapiProbeCapEnvironment("));

    const auto get = functionBody(engine, "AudioEngine::DeviceSettings AudioEngine::getDeviceSettings");
    CHECK(contains(get, "wasapiProbeCapEnabled_.load()"));
    CHECK(contains(get, "wasapiProbeCapChannels_.load()"));

    const auto save = functionBody(engine, "pluginrack::Scene AudioEngine::makeScene");
    CHECK(contains(save, "audio.wasapiProbeCapEnabled = settings.wasapiProbeCapEnabled"));
    CHECK(contains(save, "audio.wasapiProbeCapChannels = settings.wasapiProbeCapChannels"));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `build/dev/tests/pluginrack_tests.exe "*wasapi probe cap reaches*"`
Expected: FAIL on the first missing symbol.

- [ ] **Step 3: Implement the engine side**

`app/src/AudioEngine.h`:
- Add the two `DeviceSettings` fields next to the other audio tuning fields.
- Add members next to `preferExclusive_`:

```cpp
    std::atomic<bool> wasapiProbeCapEnabled_{false};
    std::atomic<int> wasapiProbeCapChannels_{8};
```

`app/src/AudioEngine.cpp`:
- Include `<pluginrack/AudioDeviceProbe.h>` and `<cstdlib>` (`_putenv_s`).
- Add next to `deviceModeName` in the anonymous namespace:

```cpp
void setWasapiProbeCapEnvironment(bool enabled, int channels) {
   #if JUCE_WINDOWS
    const auto value = pluginrack::wasapiProbeCapValue(enabled, channels);
    const auto name = juce::String("PLUGINRACK_WASAPI_MAX_PROBE_CHANNELS");
    _putenv_s(name.toRawUTF8(), juce::String(value).toRawUTF8());
    juce::Logger::writeToLog("device: wasapi probe cap " + juce::String(value)
                             + (value == 0 ? " (stock)" : " channels"));
   #else
    juce::ignoreUnused(enabled, channels);
   #endif
}
```

- In the `AudioEngine` constructor, before any stream is created, call `setWasapiProbeCapEnvironment(false, 8);`.
- In `getDeviceSettings`, set both fields from the atomics.
- In `applySettingsValues`, add:

```cpp
    wasapiProbeCapEnabled_.store(settings.wasapiProbeCapEnabled);
    wasapiProbeCapChannels_.store(settings.wasapiProbeCapChannels);
    setWasapiProbeCapEnvironment(settings.wasapiProbeCapEnabled,
                                 settings.wasapiProbeCapChannels);
```

The write is unconditional so turning the toggle off writes `0`.
- In the scene save block (around `audio.ladderRecovery = settings.ladderRecovery;`), add the two field copies.
- In the scene restore block (around `settings.ladderRecovery = scene.audio->ladderRecovery;`), add the two field copies.

- [ ] **Step 4: Build and run the test**

Run: `powershell -ExecutionPolicy Bypass -File .\tools\build-and-copy.ps1`, then `build/dev/tests/pluginrack_tests.exe "*wasapi probe cap reaches*"`.
Expected: PASS and a fresh `PluginRack.exe`.

- [ ] **Step 5: Run the full suite, then commit**

```bash
ctest --test-dir build/dev --output-on-failure
git add app/src/AudioEngine.h app/src/AudioEngine.cpp tests/test_underrun_paths.cpp
git commit -m "feat(audio): apply the wasapi probe cap from settings"
```

---

### Task 4: Settings UI

**Files:**
- Modify: `app/src/SettingsView.h`
- Modify: `app/src/SettingsView.cpp`
- Modify: `tests/test_underrun_paths.cpp`

**Interfaces:**
- Consumes: `DeviceSettings::wasapiProbeCapEnabled` / `wasapiProbeCapChannels` from Task 3.

- [ ] **Step 1: Write the failing source scans**

Append to `tests/test_underrun_paths.cpp`:

```cpp
TEST_CASE("settings exposes the wasapi probe cap", "[device][probe][ui]") {
    const auto header = readSource("app/src/SettingsView.h");
    CHECK(contains(header, "probeCapToggle"));
    CHECK(contains(header, "probeCapChannels"));

    const auto settings = readSource("app/src/SettingsView.cpp");
    CHECK(contains(settings, "Cap WASAPI channel probe"));
    CHECK(contains(settings, "more I/O than the cap"));
    CHECK(contains(settings, "probeCapChannels.setRange(2.0, 64.0, 1.0)"));
    CHECK(contains(settings, "staged_.wasapiProbeCapEnabled"));
    CHECK(contains(settings, "staged_.wasapiProbeCapChannels"));
    CHECK(contains(settings, "probeCapToggle.setEnabled(!busy)"));
    CHECK(contains(settings, "probeCapChannels.setEnabled(!busy"));
    CHECK(contains(settings, "wasapiProbeCapEnabled != applied.wasapiProbeCapEnabled"));
    CHECK(contains(settings, "wasapiProbeCapChannels != applied.wasapiProbeCapChannels"));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `build/dev/tests/pluginrack_tests.exe "*settings exposes*"`
Expected: FAIL on the missing header member.

- [ ] **Step 3: Implement the controls**

`app/src/SettingsView.h`, after `fullDuplexToggle`:

```cpp
    juce::ToggleButton probeCapToggle { "Cap WASAPI channel probe" };
    juce::Label probeCapLabel { "", "Max channels" };
    juce::Slider probeCapChannels;
```

`app/src/SettingsView.cpp`:
- Construct next to the hardening toggles. Style the toggle with `theme::ui::styleToggle`, add it, and set the tooltip:

```cpp
    probeCapToggle.setTooltip(
        "Caps JUCE's WASAPI channel-count probe at the value below. Cuts a "
        "slow endpoint's startup probe from seconds to under one; a device "
        "with more I/O than the cap reports only the cap while enabled.");
    probeCapToggle.onClick = [this] {
        staged_.wasapiProbeCapEnabled = probeCapToggle.getToggleState();
        probeCapChannels.setEnabled(staged_.wasapiProbeCapEnabled);
        updatePendingState();
    };
```

- Configure and add the numeric field:

```cpp
    theme::ui::styleCaption(probeCapLabel);
    add(probeCapLabel);
    probeCapChannels.setSliderStyle(juce::Slider::IncDecButtons);
    probeCapChannels.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 64,
                                     theme::ui::kControlH);
    probeCapChannels.setRange(2.0, 64.0, 1.0);
    probeCapChannels.setValue(8.0, juce::dontSendNotification);
    probeCapChannels.setTextValueSuffix(" ch");
    probeCapChannels.setColour(juce::Slider::textBoxTextColourId, theme::text());
    probeCapChannels.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
    probeCapChannels.setColour(juce::Slider::textBoxOutlineColourId, theme::line());
    probeCapChannels.onValueChange = [this] {
        staged_.wasapiProbeCapChannels = (int) probeCapChannels.getValue();
        updatePendingState();
    };
    add(probeCapChannels);
```

- `refresh()`: when `!dirty_`, set the toggle state and slider value from `staged_`; always call `probeCapToggle.setEnabled(!busy);` and `probeCapChannels.setEnabled(!busy && staged_.wasapiProbeCapEnabled);`.
- `updatePendingState()`: add the two comparisons shown in the test.
- `resized()` latency section: make it ten toggles (`constexpr int kToggleCount = 10;`), add `&probeCapToggle` to the `toggles` array, add one row of `theme::ui::kControlH` height after `inner.removeFromTop(togglesH);` for `probeCapLabel` (160 wide) and `probeCapChannels` (200 wide), and add `6 + theme::ui::kControlH` to `panelH`.

- [ ] **Step 4: Build and run the test**

Run: `powershell -ExecutionPolicy Bypass -File .\tools\build-and-copy.ps1`, then `build/dev/tests/pluginrack_tests.exe "*settings exposes*"`.
Expected: PASS.

- [ ] **Step 5: Run the full suite, then commit**

```bash
ctest --test-dir build/dev --output-on-failure
git add app/src/SettingsView.h app/src/SettingsView.cpp tests/test_underrun_paths.cpp
git commit -m "feat(settings): add the wasapi probe cap controls"
```

---

### Task 5: Ship and apply the JUCE patch

**Files:**
- Create: `tools/juce-patches/0001-wasapi-probe-cap.patch`
- Create: `tools/juce-patches/apply-wasapi-probe-cap.cmake`
- Modify: `CMakeLists.txt`
- Modify: `tools/build-and-copy.ps1`
- Modify: `tests/test_underrun_paths.cpp`

**Interfaces:**
- Produces: an idempotent `cmake -DJUCE_DIR=… -DPATCH_FILE=… -P apply-wasapi-probe-cap.cmake`.

- [ ] **Step 1: Write the failing source scans**

Append to `tests/test_underrun_paths.cpp`:

```cpp
TEST_CASE("the juce probe cap patch is shipped and applied", "[device][probe][build]") {
    const auto patch = readSource("tools/juce-patches/0001-wasapi-probe-cap.patch");
    CHECK(contains(patch, "PLUGINRACK_WASAPI_MAX_PROBE_CHANNELS"));
    CHECK(contains(patch, "highestChannelToQuery"));
    CHECK(contains(patch, "queryMaxNumChannels"));

    const auto cmake = readSource("CMakeLists.txt");
    CHECK(contains(cmake, "apply-wasapi-probe-cap.cmake"));
    CHECK(contains(cmake, "PLUGINRACK_PATCH_JUCE_WASAPI_PROBE"));

    const auto script = readSource("tools/juce-patches/apply-wasapi-probe-cap.cmake");
    CHECK(contains(script, "apply --check --reverse"));
    CHECK(contains(script, "does not apply"));

    const auto build = readSource("tools/build-and-copy.ps1");
    CHECK(contains(build, "apply-wasapi-probe-cap.cmake"));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `build/dev/tests/pluginrack_tests.exe "*juce probe cap patch*"`
Expected: FAIL with "cannot open … 0001-wasapi-probe-cap.patch".

- [ ] **Step 3: Create the patch**

Do **not** modify the JUCE checkout in this step (it already carries an
unrelated whitespace edit, and editing it would capture that edit in the
diff). Write the patch file directly with the single hunk below.

`$JUCE_DIR/modules/juce_audio_devices/native/juce_WASAPI_windows.cpp`
`queryMaxNumChannels` currently reads:

```cpp
        static constexpr auto maxNumChannelsToQuery = static_cast<int> (AudioChannelSet::maxChannelsOfNamedLayout);
        const auto fallbackNumChannels = defaultNumChannels;

        if (fallbackNumChannels >= maxNumChannelsToQuery)
            return fallbackNumChannels;

        auto result = fallbackNumChannels;

        for (auto ch = maxNumChannelsToQuery; ch > result; --ch)
```

and the patch turns it into:

```cpp
        static constexpr auto maxNumChannelsToQuery = static_cast<int> (AudioChannelSet::maxChannelsOfNamedLayout);
        const auto fallbackNumChannels = defaultNumChannels;

        auto highestChannelToQuery = maxNumChannelsToQuery;

        // PluginRack: opt-in cap (absent/0/invalid = stock behaviour). The app
        // sets this from Settings; it skips the 64..2 channel probe on drivers
        // that answer each format check slowly.
        if (const auto cap = SystemStats::getEnvironmentVariable (
                "PLUGINRACK_WASAPI_MAX_PROBE_CHANNELS", {}).getIntValue();
            cap >= 2)
            highestChannelToQuery = jmin (highestChannelToQuery, cap);

        if (fallbackNumChannels >= highestChannelToQuery)
            return fallbackNumChannels;

        auto result = fallbackNumChannels;

        for (auto ch = highestChannelToQuery; ch > result; --ch)
```

Write the patch as a single unified hunk, using exactly this content (`git
apply` tolerates a small line-number offset, but the context and counts must
match):

```diff
diff --git a/modules/juce_audio_devices/native/juce_WASAPI_windows.cpp b/modules/juce_audio_devices/native/juce_WASAPI_windows.cpp
--- a/modules/juce_audio_devices/native/juce_WASAPI_windows.cpp
+++ b/modules/juce_audio_devices/native/juce_WASAPI_windows.cpp
@@ -831,12 +831,22 @@
     int queryMaxNumChannels (IAudioClient* clientToUse) const
     {
         static constexpr auto maxNumChannelsToQuery = static_cast<int> (AudioChannelSet::maxChannelsOfNamedLayout);
         const auto fallbackNumChannels = defaultNumChannels;
 
+        auto highestChannelToQuery = maxNumChannelsToQuery;
+
+        // PluginRack: opt-in cap (absent/0/invalid = stock behaviour). The app
+        // sets this from Settings; it skips the 64..2 channel probe on drivers
+        // that answer each format check slowly.
+        if (const auto cap = SystemStats::getEnvironmentVariable (
+                "PLUGINRACK_WASAPI_MAX_PROBE_CHANNELS", {}).getIntValue();
+            cap >= 2)
+            highestChannelToQuery = jmin (highestChannelToQuery, cap);
+
-        if (fallbackNumChannels >= maxNumChannelsToQuery)
+        if (fallbackNumChannels >= highestChannelToQuery)
             return fallbackNumChannels;
 
         auto result = fallbackNumChannels;
 
-        for (auto ch = maxNumChannelsToQuery; ch > result; --ch)
+        for (auto ch = highestChannelToQuery; ch > result; --ch)
         {
```

- [ ] **Step 4: Create the apply script and wire it in**

`tools/juce-patches/apply-wasapi-probe-cap.cmake`:

```cmake
# Idempotently apply PluginRack's opt-in WASAPI probe-cap patch.
# Usage: cmake -DJUCE_DIR=<path> -DPATCH_FILE=<patch> -P apply-wasapi-probe-cap.cmake
if(NOT DEFINED JUCE_DIR OR NOT DEFINED PATCH_FILE)
    message(WARNING "JUCE probe-cap patch: missing JUCE_DIR or PATCH_FILE")
    return()
endif()
if(NOT EXISTS "${JUCE_DIR}/.git" OR NOT EXISTS "${PATCH_FILE}")
    message(STATUS "JUCE probe-cap patch: no git checkout at ${JUCE_DIR}; skipped")
    return()
endif()
find_program(GIT_EXECUTABLE git)
if(NOT GIT_EXECUTABLE)
    message(WARNING "JUCE probe-cap patch: git not found; the cap toggle will have no effect")
    return()
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${JUCE_DIR}" apply --check --reverse "${PATCH_FILE}"
                RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
if(already EQUAL 0)
    message(STATUS "JUCE probe-cap patch: already applied")
    return()
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${JUCE_DIR}" apply --check "${PATCH_FILE}"
                RESULT_VARIABLE can_apply OUTPUT_QUIET ERROR_QUIET)
if(NOT can_apply EQUAL 0)
    message(WARNING "JUCE probe-cap patch does not apply to ${JUCE_DIR}; the cap toggle will have no effect until the patch is updated")
    return()
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${JUCE_DIR}" apply "${PATCH_FILE}"
                RESULT_VARIABLE applied)
if(applied EQUAL 0)
    message(STATUS "JUCE probe-cap patch: applied to ${JUCE_DIR}")
else()
    message(WARNING "JUCE probe-cap patch: git apply failed")
endif()
```

Top-level `CMakeLists.txt`, after the VST3 version check and before `if(PLUGINRACK_BUILD_APP)`:

```cmake
option(PLUGINRACK_PATCH_JUCE_WASAPI_PROBE
       "Apply PluginRack's opt-in WASAPI probe cap to JUCE_DIR at configure time" ON)
if(PLUGINRACK_BUILD_APP AND PLUGINRACK_PATCH_JUCE_WASAPI_PROBE)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
                "-DJUCE_DIR=${JUCE_DIR}"
                "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tools/juce-patches/0001-wasapi-probe-cap.patch"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/juce-patches/apply-wasapi-probe-cap.cmake")
endif()
```

`tools/build-and-copy.ps1`, before the build command:

```powershell
$cacheFile = Join-Path $repoRoot "build\dev\CMakeCache.txt"
if (Test-Path $cacheFile) {
    $juceLine = Select-String -Path $cacheFile -Pattern '^JUCE_DIR:PATH=(.+)$'
    if ($juceLine) {
        $juceDir = $juceLine.Matches[0].Groups[1].Value
        & $cmake "-DJUCE_DIR=$juceDir" `
                 "-DPATCH_FILE=$repoRoot\tools\juce-patches\0001-wasapi-probe-cap.patch" `
                 -P "$repoRoot\tools\juce-patches\apply-wasapi-probe-cap.cmake"
    }
}
```

- [ ] **Step 5: Verify the patch applies, is idempotent, and builds**

Run:

```powershell
cmake "-DJUCE_DIR=C:/Users/thorz/JUCE" "-DPATCH_FILE=$PWD/tools/juce-patches/0001-wasapi-probe-cap.patch" -P tools/juce-patches/apply-wasapi-probe-cap.cmake
```

Expected: `JUCE probe-cap patch: applied to C:/Users/thorz/JUCE`. Run it again; expected: `already applied`. Then `git -C C:/Users/thorz/JUCE diff --stat` shows only `modules/juce_audio_devices/native/juce_WASAPI_windows.cpp`.

Run `powershell -ExecutionPolicy Bypass -File .\tools\build-and-copy.ps1`.
Expected: `juce_audio_devices` recompiles, build succeeds, fresh `PluginRack.exe`.

If `git apply --check` rejects the hunk, correct the `@@` line numbers/context against the real file until it applies, then re-run the check.

- [ ] **Step 6: Run the full suite, then commit**

```bash
ctest --test-dir build/dev --output-on-failure
git add tools/juce-patches CMakeLists.txt tools/build-and-copy.ps1 tests/test_underrun_paths.cpp
git commit -m "build(juce): ship the opt-in wasapi probe cap patch"
```

---

### Task 6: Docs and end-to-end acceptance

**Files:**
- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-11-13-wasapi-probe-cap-design.md` only if measured numbers change the spec's claims

- [ ] **Step 1: Update the README**

Replace the last paragraph of "Why the first device open can take a while" (the "This is a JUCE backend characteristic …" paragraph) with one that names the new setting:

```markdown
This is a JUCE backend characteristic, not a hang. Settings → Latency tuning
now has an opt-in **Cap WASAPI channel probe** toggle and a 2–64 channel value
(default 8). It caps the `queryMaxNumChannels` loop, so the two slow USB
endpoints here drop from ~12–13 s to roughly 1–1.5 s; value 2 is near-instant.
Devices with more I/O than the cap report the cap while it is enabled. The
toggle writes `PLUGINRACK_WASAPI_MAX_PROBE_CHANNELS`; with it off, JUCE runs
its stock probe. The patch lives in
`tools/juce-patches/0001-wasapi-probe-cap.patch` and is applied idempotently at
configure/build time; reverse it with
`git -C "$JUCE_DIR" apply -R tools/juce-patches/0001-wasapi-probe-cap.patch`.
```

- [ ] **Step 2: Measure cold and capped behaviour**

1. Launch with the toggle off, note `device: open … probe_ms=` for the two USB endpoints (~12–13 s expected).
2. Enable the toggle, set 8, press Apply; confirm the log line `device: wasapi probe cap 8 channels`.
3. Relaunch and note the same endpoints' `probe_ms` (~1–1.5 s expected).
4. Set 2, Apply, relaunch; confirm near-instant probes.
5. Turn the toggle off, Apply, relaunch; confirm ~12–13 s returns.
6. Leave the toggle off unless the user asked to keep it on.

- [ ] **Step 3: Run the gates**

```bash
powershell -ExecutionPolicy Bypass -File .\tools\build-and-copy.ps1
ctest --test-dir build/dev --output-on-failure
```

Expected: build succeeds with a fresh `PluginRack.exe`; all tests pass.

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "docs: document the wasapi probe cap setting"
```
