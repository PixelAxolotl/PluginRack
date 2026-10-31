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
    const auto body = functionBody(readSource("app/src/DeviceStream.cpp"),
                                   "OutputPlayback::open");
    // Depth: 8x the device buffer, clamped so a large buffer cannot balloon.
    CHECK(contains(body, "bufferSize * 8"));
    CHECK(contains(body, "16384"));
    // Prime with one silent block before the device callback can read, so a
    // steady-state consumer never finds an empty FIFO first.
    const auto prime = body.find("fifo->write(bufferSize");
    const auto callback = body.find("addAudioCallback");
    CHECK(prime != std::string::npos);
    CHECK(callback != std::string::npos);
    CHECK(prime < callback);
}

TEST_CASE("InputCapture ring buffer gets the same depth clamp", "[underrun]") {
    const auto body = functionBody(readSource("app/src/DeviceStream.cpp"),
                                   "InputCapture::open");
    CHECK(contains(body, "deviceBufferSize * 8"));
    CHECK(contains(body, "16384"));
}

TEST_CASE("InputCapture counts callback drops instead of swallowing them",
          "[underrun][telemetry]") {
    const auto body = functionBody(
        readSource("app/src/DeviceStream.cpp"),
        "InputCapture::audioDeviceIOCallbackWithContext");
    CHECK(contains(body, "inputDroppedFrames.fetch_add"));
    // The drop path must still be the early return: counted, then dropped.
    REQUIRE(body.find("tryLock.isLocked()") != std::string::npos);
    CHECK(body.find("inputDroppedFrames.fetch_add") > body.find("tryLock.isLocked()"));
    CHECK(body.find("inputDroppedFrames.fetch_add") < body.find("if (capacity <= 0)"));
}

TEST_CASE("AudioEngine::render never allocates on the audio thread", "[underrun][rt]") {
    const auto body = functionBody(readSource("app/src/AudioEngine.cpp"),
                                   "void AudioEngine::render(int frames)");
    // The heap-allocating silence buffers are gone; the remaining
    // AudioBuffer<float> uses are non-owning views over preallocated data.
    CHECK_FALSE(contains(body, "AudioBuffer<float> silence"));
    CHECK_FALSE(contains(body, "(2, frames)"));
    // Both silent-output paths still exist and route through the prepared
    // scratch buffer instead of allocating one.
    CHECK(countOccurrences(body, "pushSilenceToOutputs(frames)") >= 2);
    // Preserved semantics: oversized blocks are counted, lock loss is not.
    CHECK(contains(body, "oversizedBlockCount_.fetch_add"));
    CHECK(contains(body, "lock.isLocked()"));
}

TEST_CASE("silence scratch is fixed at construction and never resized",
          "[underrun][rt]") {
    // render()'s lock-loss path reads the scratch WITHOUT stateLock, so any
    // resize from the UI thread would be a data race. The buffer must be
    // allocated once, be big enough for every block a callback can deliver,
    // and only ever be read after construction.
    const auto header = readSource("app/src/AudioEngine.h");
    CHECK(contains(header, "silenceBuffer_{ 2, 16384 }"));
    const auto cpp = readSource("app/src/AudioEngine.cpp");
    const auto prepare = functionBody(cpp, "void AudioEngine::prepareToPlay(int maxBlockFrames)");
    CHECK_FALSE(contains(prepare, "silenceBuffer_"));
    const auto helper = functionBody(cpp, "void AudioEngine::pushSilenceToOutputs(int frames)");
    CHECK_FALSE(contains(helper, "setSize"));
    CHECK_FALSE(contains(helper, "clear"));
}

TEST_CASE("Clock reopen keeps feeding other outputs until the new clock runs",
          "[underrun]") {
    const auto body = functionBody(readSource("app/src/AudioEngine.cpp"),
                                   "juce::String AudioEngine::setOutputDevice");
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
