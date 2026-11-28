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

TEST_CASE("eq phase flip inverts the signal") {
    auto bands = defaultEqBands();
    for (auto& band : bands) {
        band.enabled = false; // flip only: no band may touch the block
    }
    EqEngine e;
    e.prepare(44100);
    e.setBands(bands);
    e.setPhaseFlip(true);
    REQUIRE(e.phaseFlip());

    constexpr int kFrames = 32;
    std::vector<float> L(static_cast<std::size_t>(kFrames));
    std::vector<float> R(static_cast<std::size_t>(kFrames));
    fillSignal(L.data(), R.data(), kFrames);
    std::vector<float> origL = L;
    std::vector<float> origR = R;
    e.process(L.data(), R.data(), kFrames);
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(L[static_cast<std::size_t>(i)] == -origL[static_cast<std::size_t>(i)]);
        REQUIRE(R[static_cast<std::size_t>(i)] == -origR[static_cast<std::size_t>(i)]);
    }
    e.setPhaseFlip(false);
    REQUIRE(!e.phaseFlip());
    e.process(L.data(), R.data(), kFrames);
    for (int i = 0; i < kFrames; ++i) {
        // Flip is off now: the buffer keeps its negated values untouched.
        REQUIRE(L[static_cast<std::size_t>(i)] == -origL[static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("eq flat passes audio") {
    EqEngine e;
    e.prepare(44100);
    e.setBands(defaultEqBands());
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
    auto bands = defaultEqBands();
    for (auto& band : bands) {
        band.enabled = false;
    }
    EqEngine e;
    e.prepare(44100);
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
    auto bands = defaultEqBands();
    bands[0].solo = true; // solo rule: only the high-pass runs
    EqEngine e;
    e.prepare(44100);
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

TEST_CASE("eq LP removes highs") {
    auto bands = defaultEqBands();
    bands[6].enabled = true;
    bands[6].solo = true; // solo rule: only the low-pass runs
    EqEngine e;
    e.prepare(44100);
    e.setBands(bands);

    // Alternating samples are the Nyquist tone (22.05 kHz here), far above the
    // 12 kHz corner, so the filter settles to near silence.
    constexpr int kFrames = 4096;
    std::vector<float> L(static_cast<std::size_t>(kFrames));
    std::vector<float> R(static_cast<std::size_t>(kFrames));
    for (int i = 0; i < kFrames; ++i) {
        const float value = (i % 2 == 0) ? 0.5f : -0.5f;
        L[static_cast<std::size_t>(i)] = value;
        R[static_cast<std::size_t>(i)] = value;
    }
    for (int off = 0; off < kFrames; off += 512) {
        e.process(L.data() + off, R.data() + off, 512);
    }
    REQUIRE(std::fabs(L[static_cast<std::size_t>(kFrames) - 1]) < 1e-3f);
    REQUIRE(std::fabs(R[static_cast<std::size_t>(kFrames) - 1]) < 1e-3f);
}

TEST_CASE("eq NaN params never NaN out") {
    auto bands = defaultEqBands();
    bands[2].freqHz = kNan;
    bands[3].gainDb = kNan;
    bands[4].q = kNan;
    auto ref = defaultEqBands();
    ref[2].enabled = false; // same effect as the NaN guard: band skipped
    ref[3].enabled = false;
    ref[4].enabled = false;

    EqEngine e;
    EqEngine reference;
    e.prepare(44100);
    reference.prepare(44100);
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
    auto bands = defaultEqBands();
    bands[1].type = EqType::LowShelf;
    bands[1].gainDb = 6.0f;
    bands[1].solo = true;
    bands[2].gainDb = -4.0f; // must NOT run while another band is soloed

    auto only = defaultEqBands();
    for (int i = 0; i < 6; ++i) {
        only[static_cast<std::size_t>(i)].enabled = (i == 1); // no solo flags
    }
    only[1].gainDb = 6.0f;

    auto bypassed = defaultEqBands();
    for (auto& band : bypassed) {
        band.enabled = false;
    }

    EqEngine soloed;
    EqEngine explicitBand;
    EqEngine allOff;
    soloed.prepare(44100);
    explicitBand.prepare(44100);
    allOff.prepare(44100);
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
    e.prepare(48000.0);
    auto bands = defaultEqBands();
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
    e.prepare(48000.0);
    auto bands = defaultEqBands();
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
    e.prepare(48000.0);
    e.setDelayMs(0.0f);
    auto bands = defaultEqBands();
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
    e.prepare(48000.0);

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
    e.prepare(48000.0);
    auto bands = defaultEqBands();
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

TEST_CASE("extreme shelf settings stay finite") {
    // LowShelf with Q 3 and +12 dB used to push the RBJ radicand negative and
    // emit NaN coefficients, which the whole strip then rendered forever.
    EqEngine e;
    e.prepare(48000.0);
    auto bands = defaultEqBands();
    for (auto& band : bands) {
        band.enabled = false;
        band.solo = false;
    }
    bands[1].enabled = true;
    bands[1].type = EqType::LowShelf;
    bands[1].gainDb = 12.0f;
    bands[1].q = 3.0f;
    bands[4].enabled = true;
    bands[4].type = EqType::HighShelf;
    bands[4].gainDb = 12.0f;
    bands[4].q = 3.0f;
    e.setBands(bands);

    constexpr int kFrames = 512;
    std::vector<float> L(static_cast<std::size_t>(kFrames), 0.5f);
    std::vector<float> R(static_cast<std::size_t>(kFrames), 0.5f);
    e.process(L.data(), R.data(), kFrames);

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(std::isfinite(L[static_cast<std::size_t>(i)]));
        REQUIRE(std::isfinite(R[static_cast<std::size_t>(i)]));
        REQUIRE(std::fabs(L[static_cast<std::size_t>(i)]) < 8.0f);
    }
}

TEST_CASE("eq shelf Q above one changes the shape and stays finite") {
    // The shelf clamped its RBJ slope at Q 1, so every Q above 1 drew and
    // sounded identical. The slope now folds past 1 as 1/Q: Q 3 and Q 20 must
    // both differ from Q 1 and stay finite at a normal +9 dB shelf.
    auto shelfBands = [](float q) {
        auto bands = defaultEqBands();
        for (auto& band : bands) {
            band.enabled = false;
            band.solo = false;
        }
        bands[1].enabled = true;
        bands[1].type = EqType::LowShelf;
        bands[1].freqHz = 200.0f;
        bands[1].gainDb = 9.0f;
        bands[1].q = q;
        return bands;
    };
    auto render = [&shelfBands](float q) {
        EqEngine engine;
        engine.prepare(48000.0);
        engine.setBands(shelfBands(q));
        constexpr int kFrames = 1024;
        std::vector<float> L(static_cast<std::size_t>(kFrames));
        std::vector<float> R(static_cast<std::size_t>(kFrames));
        fillSignal(L.data(), R.data(), kFrames);
        engine.process(L.data(), R.data(), kFrames);
        return L;
    };

    const std::vector<float> unitQ = render(1.0f);
    const std::vector<float> q3 = render(3.0f);
    const std::vector<float> q20 = render(20.0f);

    bool differs3 = false;
    bool differs20 = false;
    for (std::size_t i = 0; i < unitQ.size(); ++i) {
        REQUIRE(std::isfinite(q3[i]));
        REQUIRE(std::isfinite(q20[i]));
        if (std::fabs(q3[i] - unitQ[i]) > 1e-6f) differs3 = true;
        if (std::fabs(q20[i] - unitQ[i]) > 1e-6f) differs20 = true;
    }
    REQUIRE(differs3);
    REQUIRE(differs20);
}

TEST_CASE("eq 24 and 48 dB/oct slopes attenuate more than 12") {
    // One soloed filter point, a tone on the stop side, tail RMS only. Each
    // extra stage repeats the same biquad, so the stop band must keep dropping.
    const auto tailRms = [](EqType type, float cutoffHz, int slopeDbPerOct, double toneHz) {
        auto bands = defaultEqBands();
        for (auto& band : bands) {
            band.enabled = false;
            band.solo = false;
        }
        bands[0].enabled = true;
        bands[0].solo = true;
        bands[0].type = type;
        bands[0].freqHz = cutoffHz;
        bands[0].slopeDbPerOct = slopeDbPerOct;

        EqEngine engine;
        engine.prepare(48000.0);
        engine.setBands(bands);

        constexpr int kFrames = 8192;
        std::vector<float> L(static_cast<std::size_t>(kFrames));
        std::vector<float> R(static_cast<std::size_t>(kFrames));
        for (int i = 0; i < kFrames; ++i) {
            const double t = static_cast<double>(i) / 48000.0;
            const float v = static_cast<float>(
                0.5 * std::sin(2.0 * 3.14159265358979323846 * toneHz * t));
            L[static_cast<std::size_t>(i)] = v;
            R[static_cast<std::size_t>(i)] = v;
        }
        engine.process(L.data(), R.data(), kFrames);

        double sum = 0.0;
        for (int i = kFrames / 2; i < kFrames; ++i) {
            const double sample = L[static_cast<std::size_t>(i)];
            REQUIRE(std::isfinite(sample));
            sum += sample * sample;
        }
        return std::sqrt(sum / (kFrames / 2));
    };

    // High-pass at 1 kHz, 250 Hz tone: every extra stage cuts deeper.
    const double hp12 = tailRms(EqType::HighPass, 1000.0f, 12, 250.0);
    const double hp24 = tailRms(EqType::HighPass, 1000.0f, 24, 250.0);
    const double hp48 = tailRms(EqType::HighPass, 1000.0f, 48, 250.0);
    REQUIRE(hp24 < hp12);
    REQUIRE(hp48 < hp24);

    // Low-pass at 1 kHz, 4 kHz tone: same rule on the other side.
    const double lp12 = tailRms(EqType::LowPass, 1000.0f, 12, 4000.0);
    const double lp24 = tailRms(EqType::LowPass, 1000.0f, 24, 4000.0);
    const double lp48 = tailRms(EqType::LowPass, 1000.0f, 48, 4000.0);
    REQUIRE(lp24 < lp12);
    REQUIRE(lp48 < lp24);
}

TEST_CASE("eq 12 dB/oct is the single-stage filter") {
    // 12 dB/oct must stay bit-identical to one RBJ biquad, which is what every
    // scene before the slope field was written against.
    auto bands = defaultEqBands();
    for (auto& band : bands) {
        band.enabled = false;
    }
    bands[0].enabled = true;
    bands[0].solo = true;
    bands[0].slopeDbPerOct = 12;

    EqEngine engine;
    engine.prepare(48000.0);
    engine.setBands(bands);

    constexpr int kFrames = 512;
    std::vector<float> L(static_cast<std::size_t>(kFrames));
    std::vector<float> R(static_cast<std::size_t>(kFrames));
    fillSignal(L.data(), R.data(), kFrames);
    const std::vector<float> refL = L;
    const std::vector<float> refR = R;
    engine.process(L.data(), R.data(), kFrames);

    const dsp::Biquad c = dsp::highPass(bands[0].freqHz, 48000.0, dsp::kButterworthQ);
    std::vector<float> directL = refL;
    std::vector<float> directR = refR;
    float s1L = 0.0f, s2L = 0.0f, s1R = 0.0f, s2R = 0.0f;
    dsp::applyBiquad(directL.data(), kFrames, c, s1L, s2L);
    dsp::applyBiquad(directR.data(), kFrames, c, s1R, s2R);

    REQUIRE(L == directL);
    REQUIRE(R == directR);
}

TEST_CASE("eq prepare resets biquad states") {
    // A sample-rate change re-prepares every strip. Stale biquad state from
    // the old rate used to leak into the first block after prepare(); the
    // engine must start from zeroed filters, exactly like a fresh one.
    constexpr int kFrames = 4096;
    std::vector<float> signal(static_cast<std::size_t>(kFrames));
    for (int i = 0; i < kFrames; ++i) {
        const float t = static_cast<float>(i);
        signal[static_cast<std::size_t>(i)] =
            0.9f * std::sin(0.05f * t) + 0.3f * std::sin(0.9f * t) + 0.2f;
    }

    EqEngine warm;
    warm.prepare(48000.0);
    warm.setBands(defaultEqBands());
    for (int off = 0; off < kFrames; off += 256) {
        std::vector<float> L(signal.begin() + off, signal.begin() + off + 256);
        std::vector<float> R = L;
        warm.process(L.data(), R.data(), 256);
    }

    EqEngine fresh;
    fresh.prepare(48000.0);
    fresh.setBands(defaultEqBands());

    // Re-prepare the warm engine mid-life and replay the identical signal.
    warm.prepare(48000.0);
    for (int off = 0; off < kFrames; off += 256) {
        std::vector<float> La(signal.begin() + off, signal.begin() + off + 256);
        std::vector<float> Ra = La;
        std::vector<float> Lb(signal.begin() + off, signal.begin() + off + 256);
        std::vector<float> Rb = Lb;
        warm.process(La.data(), Ra.data(), 256);
        fresh.process(Lb.data(), Rb.data(), 256);
        for (int i = 0; i < 256; ++i) {
            REQUIRE(La[static_cast<std::size_t>(i)] == Lb[static_cast<std::size_t>(i)]);
            REQUIRE(Ra[static_cast<std::size_t>(i)] == Rb[static_cast<std::size_t>(i)]);
        }
    }
}

TEST_CASE("eq prepare at a new rate drops delay samples from the old one") {
    // A rate change re-prepares every strip. The delay line must be rebuilt for
    // the new rate, not kept: stale samples from the 48 kHz line used to leak
    // into the first 96 kHz block. The 1 ms request survives prepare() and is
    // now 96 samples, so an impulse must arrive there with silence before it.
    EqEngine e;
    e.prepare(48000.0);
    auto bands = defaultEqBands();
    for (auto& band : bands) {
        band.enabled = false; // delay only
    }
    e.setBands(bands);
    e.setDelayMs(1.0f); // 48 samples at 48 kHz

    // Fill the 48 kHz line with a constant burst: any retained history would
    // appear at the tap immediately at the new rate.
    std::vector<float> warmL(1024, 0.5f);
    std::vector<float> warmR(1024, 0.5f);
    for (int off = 0; off < 1024; off += 256) {
        e.process(warmL.data() + off, warmR.data() + off, 256);
    }

    e.prepare(96000.0);

    constexpr int kFrames = 512;
    std::vector<float> L(static_cast<std::size_t>(kFrames), 0.0f);
    std::vector<float> R(static_cast<std::size_t>(kFrames), 0.0f);
    L[0] = 1.0f;
    R[0] = -0.5f;
    e.process(L.data(), R.data(), kFrames);

    for (int i = 0; i < 96; ++i) {
        REQUIRE(L[static_cast<std::size_t>(i)] == 0.0f);
        REQUIRE(R[static_cast<std::size_t>(i)] == 0.0f);
    }
    REQUIRE(L[96] == Catch::Approx(1.0f));
    REQUIRE(R[96] == Catch::Approx(-0.5f));
}

TEST_CASE("eq latency helper reports the delay line") {
    // Freezing the compensation contract: only an enabled, un-bypassed delay
    // counts, and the report never exceeds the delay-line cap.
    REQUIRE(eqLatencySamples(false, false, 10.0f, 48000.0) == 0);
    REQUIRE(eqLatencySamples(true, true, 10.0f, 48000.0) == 0);
    REQUIRE(eqLatencySamples(true, false, 0.0f, 48000.0) == 0);
    REQUIRE(eqLatencySamples(true, false, -5.0f, 48000.0) == 0);
    REQUIRE(eqLatencySamples(true, false, kNan, 48000.0) == 0);
    REQUIRE(eqLatencySamples(true, false, 1.0f, 0.0) == 0);
    REQUIRE(eqLatencySamples(true, false, 1.0f, 48000.0) == 48);
    REQUIRE(eqLatencySamples(true, false, 1.5f, 48000.0) == 72);
    REQUIRE(eqLatencySamples(true, false, 20.0f, 96000.0) == 1920);
    // kMaxDelayMs caps the ring; an over-cap request reports the cap, not the
    // ask. That cap (48000 samples at 48 kHz) is the physical delay and sits
    // above kMaxTapDelaySamples (16384), which only bounds tap compensation.
    REQUIRE(eqLatencySamples(true, false, 5000.0f, 48000.0) == 48000);
}

