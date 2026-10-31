#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "pluginrack/EqEngine.h"

using namespace pluginrack;

namespace {

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

// Deterministic non-trivial signal (impulse plus a slow oscillation).
void fillSignal(float* L, float* R, int n) {
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i);
        L[i] = (i == 0) ? 1.0f : 0.25f * std::sin(0.1f * t);
        R[i] = (i == 1) ? -1.0f : 0.25f * std::cos(0.1f * t);
    }
}

} // namespace

TEST_CASE("eq flat passes audio") {
    EqEngine e;
    e.prepare(44100, 512);
    e.setBands(defaultSixBands());
    float L[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    float R[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    e.process(L, R, 8);
    // 0 dB peaks and shelves are exact identities at A == 1; the 80 Hz high-pass
    // passes the impulse's first tap at (1+cos w0)/2 / (1+alpha) = 0.99197.
    // Brief's epsilon(0.001) is unsatisfiable with RBJ math; 0.01 preserves the near-flat intent.
    REQUIRE(L[0] == Catch::Approx(1.0f).epsilon(0.01));
    REQUIRE(R[0] == Catch::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("eq bypassed band is bit-identical") {
    auto bands = defaultSixBands();
    for (auto& band : bands) {
        band.enabled = false;
    }
    EqEngine e;
    e.prepare(44100, 512);
    e.setBands(bands);

    constexpr int kFrames = 64;
    float L[kFrames], R[kFrames], origL[kFrames], origR[kFrames];
    fillSignal(L, R, kFrames);
    for (int i = 0; i < kFrames; ++i) {
        origL[i] = L[i];
        origR[i] = R[i];
    }
    e.process(L, R, kFrames);
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(L[i] == origL[i]);
        REQUIRE(R[i] == origR[i]);
    }
}

TEST_CASE("eq HP removes DC") {
    auto bands = defaultSixBands();
    bands[0].solo = true; // solo rule: only the high-pass runs
    EqEngine e;
    e.prepare(44100, 512);
    e.setBands(bands);

    constexpr int kFrames = 4096;
    std::vector<float> L(static_cast<std::size_t>(kFrames), 0.5f);
    std::vector<float> R(static_cast<std::size_t>(kFrames), 0.5f);
    for (int off = 0; off < kFrames; off += 512) {
        e.process(L.data() + off, R.data() + off, 512);
    }
    REQUIRE(L[0] == Catch::Approx(0.5f).epsilon(0.01)); // DC passes at first
    REQUIRE(std::fabs(L[static_cast<std::size_t>(kFrames) - 1]) < 1e-3f);
    REQUIRE(std::fabs(R[static_cast<std::size_t>(kFrames) - 1]) < 1e-3f);
}

TEST_CASE("eq NaN params never NaN out") {
    auto bands = defaultSixBands();
    bands[2].freqHz = kNan;
    bands[3].gainDb = kNan;
    bands[4].q = kNan;
    auto ref = defaultSixBands();
    ref[2].enabled = false; // same effect as the NaN guard: band skipped
    ref[3].enabled = false;
    ref[4].enabled = false;

    EqEngine e;
    EqEngine reference;
    e.prepare(44100, 512);
    reference.prepare(44100, 512);
    e.setBands(bands);
    reference.setBands(ref);

    constexpr int kFrames = 64;
    std::vector<float> L(static_cast<std::size_t>(kFrames));
    std::vector<float> R(static_cast<std::size_t>(kFrames));
    fillSignal(L.data(), R.data(), kFrames);
    std::vector<float> refL = L;
    std::vector<float> refR = R;
    e.process(L.data(), R.data(), kFrames);
    reference.process(refL.data(), refR.data(), kFrames);

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(std::isfinite(L[static_cast<std::size_t>(i)]));
        REQUIRE(std::isfinite(R[static_cast<std::size_t>(i)]));
        REQUIRE(L[static_cast<std::size_t>(i)] == refL[static_cast<std::size_t>(i)]);
        REQUIRE(R[static_cast<std::size_t>(i)] == refR[static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("eq solo isolates band") {
    auto bands = defaultSixBands();
    bands[1].type = EqType::LowShelf;
    bands[1].gainDb = 6.0f;
    bands[1].solo = true;
    bands[2].gainDb = -4.0f; // must NOT run while another band is soloed

    auto only = defaultSixBands();
    for (int i = 0; i < 6; ++i) {
        only[static_cast<std::size_t>(i)].enabled = (i == 1); // no solo flags
    }
    only[1].gainDb = 6.0f;

    auto bypassed = defaultSixBands();
    for (auto& band : bypassed) {
        band.enabled = false;
    }

    EqEngine soloed;
    EqEngine explicitBand;
    EqEngine allOff;
    soloed.prepare(44100, 512);
    explicitBand.prepare(44100, 512);
    allOff.prepare(44100, 512);
    soloed.setBands(bands);
    explicitBand.setBands(only);
    allOff.setBands(bypassed);

    constexpr int kFrames = 64;
    std::vector<float> L(static_cast<std::size_t>(kFrames));
    std::vector<float> R(static_cast<std::size_t>(kFrames));
    fillSignal(L.data(), R.data(), kFrames);
    std::vector<float> refL = L;
    std::vector<float> refR = R;
    std::vector<float> offL = L;
    std::vector<float> offR = R;
    soloed.process(L.data(), R.data(), kFrames);
    explicitBand.process(refL.data(), refR.data(), kFrames);
    allOff.process(offL.data(), offR.data(), kFrames);

    for (int i = 0; i < kFrames; ++i) {
        // Soloed run equals the run with just that band enabled, bit for bit.
        REQUIRE(L[static_cast<std::size_t>(i)] == refL[static_cast<std::size_t>(i)]);
        REQUIRE(R[static_cast<std::size_t>(i)] == refR[static_cast<std::size_t>(i)]);
    }
    // And the soloed band actually changed the signal.
    REQUIRE(L[0] != offL[0]);
    REQUIRE(R[0] != offR[0]);
}

TEST_CASE("eq delay shifts the signal by the requested samples") {
    EqEngine e;
    e.prepare(48000.0, 512);
    auto bands = defaultSixBands();
    for (auto& band : bands) {
        band.enabled = false; // delay only: no band may touch the block
    }
    e.setBands(bands);
    e.setDelayMs(1.0f); // 48 samples at 48 kHz

    constexpr int kFrames = 256;
    std::vector<float> L(static_cast<std::size_t>(kFrames), 0.0f);
    std::vector<float> R(static_cast<std::size_t>(kFrames), 0.0f);
    L[0] = 1.0f;
    R[0] = -0.5f;
    e.process(L.data(), R.data(), kFrames);

    for (int i = 0; i < 48; ++i) {
        REQUIRE(L[static_cast<std::size_t>(i)] == 0.0f);
        REQUIRE(R[static_cast<std::size_t>(i)] == 0.0f);
    }
    REQUIRE(L[48] == Catch::Approx(1.0f));
    REQUIRE(R[48] == Catch::Approx(-0.5f));
}

TEST_CASE("eq delay holds across render blocks") {
    EqEngine e;
    e.prepare(48000.0, 512);
    auto bands = defaultSixBands();
    for (auto& band : bands) {
        band.enabled = false;
    }
    e.setBands(bands);
    e.setDelayMs(250.0f); // 12000 samples at 48 kHz

    constexpr int kBlock = 512;
    constexpr int kTotal = 13000;
    std::vector<float> L(static_cast<std::size_t>(kTotal), 0.0f);
    std::vector<float> R(static_cast<std::size_t>(kTotal), 0.0f);
    L[0] = 1.0f;
    R[0] = 1.0f;
    for (int offset = 0; offset < kTotal; offset += kBlock) {
        const int frames = std::min(kBlock, kTotal - offset);
        e.process(L.data() + offset, R.data() + offset, frames);
    }

    for (int i = 0; i < 12000; ++i) {
        REQUIRE(L[static_cast<std::size_t>(i)] == 0.0f);
    }
    REQUIRE(L[12000] == Catch::Approx(1.0f));
    REQUIRE(R[12000] == Catch::Approx(1.0f));
}

TEST_CASE("eq zero delay is bit-identical") {
    EqEngine e;
    e.prepare(48000.0, 512);
    e.setDelayMs(0.0f);
    auto bands = defaultSixBands();
    for (auto& band : bands) {
        band.enabled = false;
    }
    e.setBands(bands);

    constexpr int kFrames = 64;
    std::vector<float> L(static_cast<std::size_t>(kFrames));
    std::vector<float> R(static_cast<std::size_t>(kFrames));
    fillSignal(L.data(), R.data(), kFrames);
    const std::vector<float> origL = L;
    const std::vector<float> origR = R;
    e.process(L.data(), R.data(), kFrames);

    REQUIRE(L == origL);
    REQUIRE(R == origR);
}

TEST_CASE("eq delay clamps to the 0-1 s range") {
    EqEngine e;
    e.prepare(48000.0, 256);

    e.setDelayMs(5000.0f);
    REQUIRE(e.delayMs() == Catch::Approx(EqEngine::kMaxDelayMs));
    e.setDelayMs(-5.0f);
    REQUIRE(e.delayMs() == 0.0f);
    e.setDelayMs(kNan);
    REQUIRE(e.delayMs() == 0.0f);
    e.setDelayMs(250.0f);
    REQUIRE(e.delayMs() == Catch::Approx(250.0f));
}

TEST_CASE("eq delay change mid-stream glides without blowing up") {
    EqEngine e;
    e.prepare(48000.0, 512);
    auto bands = defaultSixBands();
    for (auto& band : bands) {
        band.enabled = false;
    }
    e.setBands(bands);
    e.setDelayMs(10.0f);

    constexpr int kFrames = 8192;
    std::vector<float> L(static_cast<std::size_t>(kFrames));
    std::vector<float> R(static_cast<std::size_t>(kFrames));
    for (int i = 0; i < kFrames; ++i) {
        const float v = 0.5f * std::sin(0.05f * static_cast<float>(i));
        L[static_cast<std::size_t>(i)] = v;
        R[static_cast<std::size_t>(i)] = v;
    }

    e.process(L.data(), R.data(), 512);
    e.setDelayMs(500.0f); // the glide must stay bounded across the change
    e.process(L.data() + 512, R.data() + 512, kFrames - 512);

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(std::isfinite(L[static_cast<std::size_t>(i)]));
        REQUIRE(std::isfinite(R[static_cast<std::size_t>(i)]));
        REQUIRE(std::fabs(L[static_cast<std::size_t>(i)]) < 1.0f);
    }
    REQUIRE(e.delayMs() == Catch::Approx(500.0f));
}
