# PluginRack Core Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the platform-neutral PluginRack core library — scene format, validation, routing math, insert chains, metering, and an offline render CLI — so the mixer rules are proven headless before any audio device or UI code exists.

**Architecture:** A JUCE-free C++20 static library (`pluginrack_core`) owns all mixer semantics. Sources, plugins, and devices enter through narrow interfaces so tests supply doubles and the Windows host supplies real implementations later. Rendering is a preallocated, allocation-free pass over planar float32 blocks.

**Tech Stack:** C++20, CMake ≥ 3.28, Ninja, MSVC 19.3x, Catch2 v3 (FetchContent), nlohmann/json (FetchContent).

**Spec:** `docs/superpowers/specs/2026-10-28-pluginrack-design.md`

## Global Constraints

- Platform: Windows 10/11 x64. No JUCE, no OS APIs, and no third-party audio code in `core/`.
- Language: C++20. Public headers use `#pragma once` and namespace `pluginrack`.
- Sample rates: 44100, 48000, 88200, 96000. Default 48000. Default buffer 256 frames.
- Gain and send gain: dB, clamped to `[-60, +12]`, default `0`. `-60` is exact silence.
- Pan: constant power, `theta = (pan + 1) * π / 4`, `leftGain = cos(theta)`, `rightGain = sin(theta)`. Center = `0.70710678` per side.
- Solo: any soloed channel silences all non-soloed channels. Mute beats solo.
- `MixerEngine::render` must be `noexcept`, must not allocate, lock, log, or throw.
- Scene JSON `schemaVersion` is `1`. Unsupported versions are rejected. Unknown fields are ignored on load and dropped on save.
- No `Co-authored-by` or AI attribution in commits.
- Every new `tests/*.cpp` file must be added to the `pluginrack_tests` source list in `tests/CMakeLists.txt`; every new `core/src/*.cpp` file to `pluginrack_core` in `core/CMakeLists.txt`.

## Review Focus

1. Scene JSON claiming `schemaVersion: 2` — must be rejected with an error, never reinterpreted as v1.
2. Route referencing a missing output id — must be a validation error, and the rest of the scene must still render.
3. Channel gain exactly `-60 dB` — must produce exact `0.0f`, not a denormal tail.
4. Duplicate `(channelId, outputId)` route pair — must be rejected, not silently summed twice.
5. Scene with zero outputs — `MixerEngine` construction must fail with a message, not crash or render into null buffers.

---

### Task 1: Repository scaffold, toolchain, and first test

**Files:**
- Create: `.gitignore`, `README.md`, `CMakeLists.txt`, `CMakePresets.json`, `core/CMakeLists.txt`, `tests/CMakeLists.txt`
- Create: `core/include/pluginrack/Version.h`, `core/src/Version.cpp`
- Test: `tests/test_version.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `const char* pluginrack::versionString() noexcept` returning `"0.1.0"`; CMake targets `pluginrack_core` (static lib, alias `pluginrack::core`) and `pluginrack_tests` (Catch2 executable registered with CTest).

- [ ] **Step 1: Install the toolchain and verify it**

Install Visual Studio Build Tools with the "Desktop development with C++" workload, then CMake and Ninja. Open a new shell and run:

```
cmake --version
ninja --version
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationVersion
```

Expected: CMake ≥ 3.28, Ninja ≥ 1.11, MSVC ≥ 19.3.

- [ ] **Step 2: Write the failing test**

`tests/test_version.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "pluginrack/Version.h"

TEST_CASE("version string reports the library version") {
    REQUIRE(std::string(pluginrack::versionString()) == "0.1.0");
}
```

- [ ] **Step 3: Write the build files**

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.28)
project(PluginRack VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_subdirectory(core)
add_subdirectory(tests)
```

`core/CMakeLists.txt`:

```cmake
add_library(pluginrack_core STATIC src/Version.cpp)
add_library(pluginrack::core ALIAS pluginrack_core)
target_include_directories(pluginrack_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(pluginrack_core PRIVATE /W4 /WX)
```

`tests/CMakeLists.txt`:

```cmake
include(FetchContent)
FetchContent_Declare(Catch2 GIT_REPOSITORY https://github.com/catchorg/Catch2.git GIT_TAG v3.7.1)
FetchContent_MakeAvailable(Catch2)
add_executable(pluginrack_tests test_version.cpp)
target_link_libraries(pluginrack_tests PRIVATE pluginrack::core Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(pluginrack_tests)
```

`CMakePresets.json`:

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "dev",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/dev",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" }
    }
  ],
  "buildPresets": [{ "name": "dev", "configurePreset": "dev" }],
  "testPresets": [
    { "name": "dev", "configurePreset": "dev", "output": { "outputOnFailure": true } }
  ]
}
```

`.gitignore`: `build/`, `out/`, `.vs/`, `CMakeUserPresets.json`.

- [ ] **Step 4: Run the test to verify it fails**

```
cmake --preset dev
cmake --build --preset dev
```

Expected: compile error — `pluginrack/Version.h` does not exist.

- [ ] **Step 5: Implement `versionString`**

`core/include/pluginrack/Version.h` declares `const char* versionString() noexcept;`. `core/src/Version.cpp` returns `"0.1.0"`.

- [ ] **Step 6: Run the test to verify it passes**

```
cmake --build --preset dev
ctest --preset dev
```

Expected: `100% tests passed, 0 tests failed out of 1`.

- [ ] **Step 7: Commit**

```
git init
git add .gitignore README.md CMakeLists.txt CMakePresets.json core tests
git commit -m "chore: scaffold core library and test harness"
```

---

### Task 2: Scene model and JSON codec

**Files:**
- Create: `core/include/pluginrack/Types.h`, `core/include/pluginrack/Scene.h`, `core/include/pluginrack/SceneCodec.h`, `core/src/SceneCodec.cpp`
- Modify: `core/CMakeLists.txt`
- Test: `tests/test_scene_codec.cpp`

**Interfaces:**
- Consumes: `pluginrack::versionString()` (Task 1).
- Produces: the `Scene` aggregate from spec §4 with defaulted `operator==` on every struct; `std::string pluginrack::toJson(const Scene&)`; `SceneLoadResult pluginrack::fromJson(std::string_view)` where

```cpp
struct SceneLoadResult {
    std::optional<Scene> scene;      // engaged only when errors is empty
    std::vector<std::string> errors;
};
```

- [ ] **Step 1: Write the failing tests**

`tests/test_scene_codec.cpp` covers (include `<catch2/matchers/catch_matchers_string.hpp>` for `ContainsSubstring`):

```cpp
TEST_CASE("scene survives a JSON round trip") {
    // 2 channels (one Mono hardware, one Stereo SystemLoopback with one bypassed insert),
    // 2 outputs (Physical, VirtualOutput), 3 routes, sampleRate 48000, bufferFrames 256.
    // REQUIRE(loaded.errors.empty()); REQUIRE(*loaded.scene == original);
}

TEST_CASE("unsupported schema version is rejected") {
    // JSON with "schemaVersion": 2 and nothing else.
    // REQUIRE(!loaded.scene.has_value());
    // REQUIRE(loaded.errors.size() == 1);
    // REQUIRE_THAT(loaded.errors[0], Catch::Matchers::ContainsSubstring("schemaVersion"));
}

TEST_CASE("unknown fields are ignored on load and dropped on save") {
    // Load a v1 scene containing "futureThing": {"a": 1}.
    // REQUIRE(loaded.errors.empty());
    // REQUIRE_FALSE(toJson(*loaded.scene).contains("futureThing"));
}
```

- [ ] **Step 2: Run to verify failure**

`cmake --build --preset dev` — expected: `SceneCodec.h` not found.

- [ ] **Step 3: Implement the model and codec**

`Types.h` holds `SourceKind`, `OutputKind`, `ChannelLayout`. `Scene.h` holds `InsertSlot`, `Channel`, `Output`, `Route`, `Scene` with in-class defaults matching Global Constraints and `bool operator==(const X&) const = default;` on each. `SceneCodec.cpp` uses `nlohmann::json` with explicit `to_json`/`from_json` free functions; add `FetchContent` for nlohmann/json v3.11.3 and link `nlohmann_json::nlohmann_json` PUBLIC to `pluginrack_core`. `fromJson` parses inside a try/catch and converts every parse failure into an `errors` entry. Check `schemaVersion` before reading any other field.

- [ ] **Step 4: Run to verify pass**

`cmake --build --preset dev && ctest --preset dev` — expected: all tests pass.

- [ ] **Step 5: Commit**

```
git add core tests
git commit -m "feat: add scene model and JSON codec"
```

---

### Task 3: Scene validation

**Files:**
- Create: `core/include/pluginrack/SceneValidator.h`, `core/src/SceneValidator.cpp`
- Modify: `core/CMakeLists.txt`
- Test: `tests/test_scene_validator.cpp`

**Interfaces:**
- Consumes: `Scene` (Task 2).
- Produces:

```cpp
enum class SceneErrorCode {
    DuplicateChannelId, DuplicateOutputId, DuplicateRoute,
    UnknownChannelInRoute, UnknownOutputInRoute,
    ValueOutOfRange, NoChannels, NoOutputs
};
struct SceneError { SceneErrorCode code; std::uint32_t entityId; std::string message; };
std::vector<SceneError> pluginrack::validate(const Scene& scene);
```

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("valid scene produces no errors") { /* 1 channel, 1 output, 1 route */ }

TEST_CASE("duplicate route pair is rejected") {
    // two routes with the same channelId and outputId
    // REQUIRE(errors.size() == 1);
    // REQUIRE(errors[0].code == SceneErrorCode::DuplicateRoute);
}

TEST_CASE("route to missing output is rejected") {
    // route.outputId = 99, outputs contains only id 1
    // REQUIRE(errors.size() == 1);
    // REQUIRE(errors[0].code == SceneErrorCode::UnknownOutputInRoute);
    // REQUIRE(errors[0].entityId == 99);
}

TEST_CASE("out of range values are reported") {
    // gainDb 20.0, pan -2.0, sendGainDb -100.0, sampleRate 44101
    // REQUIRE(errors.size() == 4); all code == ValueOutOfRange
}

TEST_CASE("scene with no outputs is rejected") {
    // REQUIRE(errors.size() == 1); REQUIRE(errors[0].code == SceneErrorCode::NoOutputs);
}
```

- [ ] **Step 2: Run to verify failure** — `validate` is not declared.

- [ ] **Step 3: Implement `validate`**

Single pass per collection. Duplicate detection uses `std::unordered_set`. Range checks use the Global Constraints values; `sampleRate` must be one of the four listed rates. Error order must be deterministic: collection order, then field order `gainDb`, `pan`, `sendGainDb`, `sampleRate`.

- [ ] **Step 4: Run to verify pass** — `ctest --preset dev`.

- [ ] **Step 5: Commit**

```
git add core tests
git commit -m "feat: validate scene structure and ranges"
```

---

### Task 4: Mixer engine — fader, pan, solo, mute, routing matrix

**Files:**
- Create: `core/include/pluginrack/AudioBlock.h`, `core/include/pluginrack/MixerEngine.h`, `core/src/MixerEngine.cpp`
- Create: `tests/support/AllocCounter.h`
- Modify: `core/CMakeLists.txt`
- Test: `tests/test_mixer_engine.cpp`

**Interfaces:**
- Consumes: `Scene`, `validate` (Tasks 2–3).
- Produces:

```cpp
struct AudioBlock { float* const* channels; int numChannels; int numFrames; };

class MixerEngine {
public:
    // Returns false and fills *error when the scene is invalid or has no outputs.
    bool prepare(const Scene& scene, int maxBlockFrames, std::string* error);
    void render(std::span<const AudioBlock> channelInputs,
                std::span<AudioBlock> outputBlocks) noexcept;
    int channelCount() const noexcept;
    int outputCount() const noexcept;
};
```

`channelInputs` is indexed in `scene.channels` order and must contain stereo blocks (`numChannels == 2`); `outputBlocks` is indexed in `scene.outputs` order, also stereo. Mismatched sizes cause the extra blocks to be left untouched.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("unity route passes signal at center pan") {
    // 1 mono channel, 1 output, route sendGainDb 0, gainDb 0, pan 0
    // input both sides 0.5f for 128 frames
    // REQUIRE(out[0][i] == Approx(0.35355339f)); REQUIRE(out[1][i] == Approx(0.35355339f));
}

TEST_CASE("send gain of -6 dB scales the route") {
    // same scene, sendGainDb -6
    // REQUIRE(out[0][i] == Approx(0.5f * 0.70710678f * 0.5011872f));
}

TEST_CASE("one channel reaches two outputs independently") {
    // routes: (1,1) at 0 dB, (1,2) at -12 dB
    // output 1 peak 0.35355339f, output 2 peak 0.0888f (0.35355339f * 0.2511886f)
}

TEST_CASE("muted channel is silent") { /* REQUIRE all output samples == 0.0f exactly */ }

TEST_CASE("solo isolates the soloed channel") {
    // channels 1 and 2 both routed to output 1; channel 2 soloed
    // REQUIRE output carries channel 2's signal and channel 1's contribution is absent
}

TEST_CASE("gain of -60 dB is exact silence") {
    // gainDb -60, input 1.0f
    // REQUIRE(out[0][i] == 0.0f); REQUIRE(std::signbit(out[0][i]) == false);
}

TEST_CASE("prepare rejects a scene with no outputs") {
    // REQUIRE(engine.prepare(scene, 256, &error) == false);
    // REQUIRE_FALSE(error.empty());
}

TEST_CASE("render allocates nothing") {
    // AllocCounter::reset(); engine.render(...); REQUIRE(AllocCounter::count() == 0);
}
```

- [ ] **Step 2: Run to verify failure** — `MixerEngine.h` not found.

- [ ] **Step 3: Implement the engine**

`prepare` validates, allocates per-channel stereo scratch buffers, per-output stereo accumulators, and a `std::vector<float>` of cached linear gains (`gain`, `panL`, `panR`, `sendGain` per route) so `render` only multiplies. `render` for each channel: copy input into scratch, apply cached gain and pan, zero it if muted or (anySolo && !soloed), then for each route add `scratch * sendGain` into the target output accumulator. Finally copy accumulators into `outputBlocks`. `AllocCounter.h` overrides global `operator new`/`delete` with a thread-local counter; tests include it directly.

- [ ] **Step 4: Run to verify pass** — `ctest --preset dev`.

- [ ] **Step 5: Commit**

```
git add core tests
git commit -m "feat: add mixer engine with matrix routing"
```

---

### Task 5: Insert chain and plugin host seam

**Files:**
- Create: `core/include/pluginrack/PluginHost.h`, `core/include/pluginrack/InsertChain.h`, `core/src/InsertChain.cpp`
- Create: `tests/support/TestPlugins.h`
- Modify: `core/CMakeLists.txt`
- Test: `tests/test_insert_chain.cpp`

**Interfaces:**
- Consumes: `AudioBlock` (Task 4), `InsertSlot` (Task 2).
- Produces:

```cpp
class IPluginInstance {
public:
    virtual ~IPluginInstance() = default;
    virtual void prepare(double sampleRate, int maxBlockFrames) = 0;
    virtual void process(AudioBlock& block) noexcept = 0;
    virtual void reset() noexcept = 0;
};

class IPluginFactory {
public:
    virtual ~IPluginFactory() = default;
    // Returns nullptr and fills *error on failure. Must not throw.
    virtual std::unique_ptr<IPluginInstance> create(const std::string& pluginPath, std::string* error) = 0;
};

class InsertChain {
public:
    InsertChain(const std::vector<InsertSlot>& slots, IPluginFactory& factory,
                double sampleRate, int maxBlockFrames);
    void process(AudioBlock& block) noexcept;          // allocation-free
    std::vector<std::uint32_t> failedSlots() const;     // slot ids that failed to load
};
```

`InsertChain` keeps `IPluginFactory&` only during construction; after that it owns instances.

`MixerEngine` gains `void setPluginFactory(IPluginFactory* factory) noexcept;`. It must be called before `prepare`; a null factory (the default) means channels render with no inserts. When a factory is set, `prepare` builds one `InsertChain` per channel from that channel's `inserts`.

- [ ] **Step 1: Write the failing tests**

`tests/support/TestPlugins.h` provides `HalvePlugin` (multiplies by `0.5f`), `AddOnePlugin` (adds `0.25f`), and `FailingFactory` (always returns nullptr with `*error = "boom"`).

```cpp
TEST_CASE("inserts run in order") {
    // slots: AddOne then Halve; input 0.0f -> (0.0 + 0.25) * 0.5 = 0.125f
}

TEST_CASE("bypassed insert is skipped") {
    // Halve with bypassed = true; input 1.0f stays 1.0f
}

TEST_CASE("failed plugin load is reported and skipped") {
    // 3 slots, factory fails only for path "/bad"
    // REQUIRE(chain.failedSlots() == std::vector<std::uint32_t>{2});
    // REQUIRE signal passes through the remaining two inserts
}

TEST_CASE("chain processing allocates nothing") { /* AllocCounter around process() */ }

TEST_CASE("engine applies the insert chain before the fader") {
    // 1 mono channel with one Halve insert, unity route, gainDb 0, pan 0, input 1.0f
    // engine.setPluginFactory(&factory) before prepare
    // REQUIRE(out[0][i] == Approx(0.5f * 0.70710678f));
}
```

- [ ] **Step 2: Run to verify failure** — `InsertChain.h` not found.

- [ ] **Step 3: Implement the chain**

Construction creates an instance per non-bypassed slot and calls `prepare`. A null instance marks the slot failed and it is skipped in `process`. `process` iterates slots in order, skipping failed or bypassed ones, and wraps the caller's `AudioBlock` in place (no copy).

`MixerEngine::render` calls each channel's `InsertChain::process` on the channel scratch block after the input copy and before the cached gain and pan.

- [ ] **Step 4: Run to verify pass** — `ctest --preset dev`.

- [ ] **Step 5: Commit**

```
git add core tests
git commit -m "feat: add insert chain with failure isolation"
```

---

### Task 6: Device availability resolution

**Files:**
- Create: `core/include/pluginrack/DeviceRegistry.h`, `core/include/pluginrack/SceneAvailability.h`, `core/src/SceneAvailability.cpp`
- Modify: `core/CMakeLists.txt`, `core/include/pluginrack/MixerEngine.h`, `core/src/MixerEngine.cpp`
- Test: `tests/test_scene_availability.cpp`

**Interfaces:**
- Consumes: `Scene` (Task 2), `MixerEngine` (Task 4).
- Produces:

```cpp
class DeviceRegistry {
public:
    virtual ~DeviceRegistry() = default;
    virtual bool hasInput(const std::string& deviceId) const = 0;
    virtual bool hasOutput(const std::string& deviceId) const = 0;
};

struct SceneAvailability {
    std::vector<std::uint32_t> stoppedChannels;
    std::vector<std::uint32_t> disabledOutputs;
    std::vector<std::string> warnings;   // one per missing device, "<kind> '<name>' unavailable"
};
SceneAvailability pluginrack::resolveAvailability(const Scene& scene, const DeviceRegistry& registry);
```

`MixerEngine::prepare` gains an overload:
`bool prepare(const Scene& scene, const SceneAvailability& availability, int maxBlockFrames, std::string* error)`.
Stopped channels render silence; disabled outputs are written as silence.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("missing input stops only that channel") {
    // 2 channels; registry has input "mic-ok" but not "mic-gone"
    // REQUIRE(avail.stoppedChannels == {2});
    // REQUIRE(avail.warnings.size() == 1);
    // REQUIRE_THAT(avail.warnings[0], ContainsSubstring("mic-gone"));
}

TEST_CASE("missing output is disabled and rest of scene renders") {
    // 2 outputs; registry lacks output device "virt-1"
    // engine renders: output 1 has signal, output 2 is exact silence
}

TEST_CASE("all devices present yields no warnings") {
    // REQUIRE(avail.warnings.empty()); REQUIRE(avail.stoppedChannels.empty());
}
```

- [ ] **Step 2: Run to verify failure** — `resolveAvailability` not declared.

- [ ] **Step 3: Implement resolution and engine flags**

`resolveAvailability` walks channels then outputs in scene order, calling `hasInput(channel.sourceDeviceId)` and `hasOutput(output.deviceId)`. The engine stores two `std::vector<bool>` flags sized to channel/output count and consults them in `render`.

- [ ] **Step 4: Run to verify pass** — `ctest --preset dev`.

- [ ] **Step 5: Commit**

```
git add core tests
git commit -m "feat: resolve missing devices without dropping the scene"
```

---

### Task 7: Peak metering

**Files:**
- Create: `core/include/pluginrack/Meters.h`, `core/src/Meters.cpp`
- Modify: `core/CMakeLists.txt`, `core/include/pluginrack/MixerEngine.h`, `core/src/MixerEngine.cpp`
- Test: `tests/test_meters.cpp`

**Interfaces:**
- Consumes: `MixerEngine` (Tasks 4, 6).
- Produces:

```cpp
class MeterBank {
public:
    void observeChannel(std::uint32_t id, float peak) noexcept;
    void observeOutput(std::uint32_t id, float peak) noexcept;
    void advance(int numFrames, double sampleRate) noexcept;  // 20 dB/s release
    float channelPeak(std::uint32_t id) const noexcept;       // 0.0f when unknown
    float outputPeak(std::uint32_t id) const noexcept;
    void reset() noexcept;
};
const MeterBank& MixerEngine::meters() const noexcept;
```

Peaks are stored in `std::atomic<float>` with `memory_order_relaxed` and are linear amplitude, post-fader and post-send.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("peak holds through one block") {
    // observeChannel(1, 0.5f); REQUIRE(channelPeak(1) == 0.5f);
}

TEST_CASE("peak decays 20 dB per second") {
    // observeChannel(1, 1.0f); advance(48000, 48000.0);
    // REQUIRE(channelPeak(1) == Approx(0.1f).epsilon(0.001f));
}

TEST_CASE("engine reports post-pan channel peak") {
    // unity scene, 0.5f input at center pan -> channelPeak(1) == Approx(0.35355339f)
}

TEST_CASE("engine reports output peak after send gain") {
    // sendGainDb -6 -> outputPeak(1) == Approx(0.35355339f * 0.5011872f)
}

TEST_CASE("unknown id reports zero") { /* REQUIRE(m.channelPeak(999) == 0.0f); */ }
```

- [ ] **Step 2: Run to verify failure** — `Meters.h` not found.

- [ ] **Step 3: Implement metering and wire it into `render`**

`advance` multiplies each stored peak by `pow(10.0, -20.0 * numFrames / sampleRate / 20.0)` and clamps to `[0, 8]` before storing the new observation (peaks above 1.0 are legitimate: the fader allows +12 dB). `render` computes a channel peak from the post-pan scratch buffer before routing, and an output peak from each accumulator after summation, then calls `advance(numFrames, sampleRate_)` once per render call.

- [ ] **Step 4: Run to verify pass** — `ctest --preset dev`.

- [ ] **Step 5: Commit**

```
git add core tests
git commit -m "feat: add peak metering"
```

---

### Task 8: WAV writer, offline renderer, and CLI

**Files:**
- Create: `core/include/pluginrack/WavWriter.h`, `core/src/WavWriter.cpp`, `core/include/pluginrack/OfflineRenderer.h`, `core/src/OfflineRenderer.cpp`
- Create: `tools/CMakeLists.txt`, `tools/render_cli/main.cpp`
- Create: `tests/fixtures/unity-440.json`
- Modify: `CMakeLists.txt`, `core/CMakeLists.txt`
- Test: `tests/test_offline_render.cpp`

**Interfaces:**
- Consumes: `MixerEngine` (Task 7), `SceneCodec` (Task 2), `resolveAvailability` (Task 6).
- Produces:

```cpp
bool pluginrack::writeWav(const std::string& path, std::span<const float> interleaved,
                          int numChannels, int sampleRate);   // 16-bit PCM
std::vector<std::vector<float>> pluginrack::renderSine(
    const Scene& scene, const SceneAvailability& availability,
    float toneHz, float amplitude, int numFrames);            // [outputIndex][interleaved stereo]
```

Executable `pluginrack-render`:

```
pluginrack-render <scene.json> <outDir> [--tone <hz>] [--seconds <n>]
```

Writes `<outDir>/<output-name>.wav` per output and prints `output "<name>": peak <value>` per line.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("sine render has the expected amplitude and frequency") {
    // 1 mono channel, 1 output, unity route, toneHz 440, amplitude 1.0f, 48000 frames
    // peak == Approx(0.70710678f).epsilon(0.01f)
    // count sign changes in the left channel; REQUIRE(count == 880 within +/- 2)
}

TEST_CASE("render returns one buffer per output") {
    // 2 outputs -> result.size() == 2; each size == numFrames * 2
}

TEST_CASE("wav header is 16-bit stereo at 48 kHz") {
    // write 100 frames, read the file back
    // REQUIRE first 4 bytes == "RIFF"; bytes 8..11 == "WAVE"; channels at offset 22 == 2;
    // sampleRate at offset 24 == 48000; bits at offset 34 == 16;
    // REQUIRE file size == 44 + 100 * 4
}
```

- [ ] **Step 2: Run to verify failure** — `WavWriter.h` not found.

- [ ] **Step 3: Implement writer, renderer, and CLI**

`writeWav` writes a canonical 44-byte header and clamps samples to `[-1, 1]` before converting with `lrintf(x * 32767.0f)`. `renderSine` builds a `Scene`-sized engine, generates one mono sine into both sides of the single channel block, renders block by block at 256 frames, and appends each output's interleaved result. The CLI parses arguments, loads the scene with `fromJson`, validates, resolves availability with a registry that reports every device present, then writes one WAV per output. Add `add_subdirectory(tools)` to the root `CMakeLists.txt`.

`tests/fixtures/unity-440.json` is a v1 scene with one Mono `Hardware` channel `id 1` (`gainDb 0`, `pan 0`), one `Physical` output `id 1`, one route `(1, 1, 0)`, `sampleRate 48000`, `bufferFrames 256`. The CLI step below reads it from the repository root.

- [ ] **Step 4: Run to verify pass** — `ctest --preset dev`.

- [ ] **Step 5: Run the CLI end to end**

```
cmake --build --preset dev
mkdir out
build/dev/tools/pluginrack-render tests/fixtures/unity-440.json out --tone 440 --seconds 1
```

Expected: one `output "..."` line with peak `0.7071`, and `out/*.wav` playable at 48 kHz stereo.

- [ ] **Step 6: Commit**

```
git add core tools tests CMakeLists.txt
git commit -m "feat: add offline render CLI and wav output"
```

---

## Verification

Before claiming the plan is complete:

```
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
```

Expected: every test passes, and `pluginrack-render` produces an audible 440 Hz tone.
