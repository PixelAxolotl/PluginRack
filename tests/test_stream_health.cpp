#include <catch2/catch_test_macros.hpp>

#include <pluginrack/AudioTelemetry.h>

namespace {

using pluginrack::StreamHealth;
using pluginrack::StreamHealthTracker;
using pluginrack::StreamWindow;

} // namespace

TEST_CASE("health thresholds are exact at their boundaries", "[latency][health]") {
    StreamWindow window;
    CHECK(pluginrack::classifyHealth(window) == StreamHealth::Green);

    window.underrunsPerMinute = 0.99;
    CHECK(pluginrack::classifyHealth(window) == StreamHealth::Green);

    window.underrunsPerMinute = 1.0;
    CHECK(pluginrack::classifyHealth(window) == StreamHealth::Yellow);

    window.underrunsPerMinute = 9.99;
    CHECK(pluginrack::classifyHealth(window) == StreamHealth::Yellow);

    window.underrunsPerMinute = 10.0;
    CHECK(pluginrack::classifyHealth(window) == StreamHealth::Red);

    // The fill edge is a strict "greater than" test.
    StreamWindow fill;
    fill.fillPinnedToEdgeMs = 5000.0;
    CHECK(pluginrack::classifyHealth(fill) == StreamHealth::Green);
    fill.fillPinnedToEdgeMs = 5000.1;
    CHECK(pluginrack::classifyHealth(fill) == StreamHealth::Yellow);

    StreamWindow hardError;
    hardError.error = true;
    CHECK(pluginrack::classifyHealth(hardError) == StreamHealth::Red);
}

TEST_CASE("the rolling window prunes old events and stays bounded",
          "[latency][health]") {
    StreamHealthTracker tracker;
    tracker.configure(10000.0);

    // More underruns than the tracker can hold: the cap keeps memory fixed.
    for (int i = 0; i < 200; ++i) tracker.onBlock(static_cast<double>(i), true, false);
    CHECK(tracker.underrunsInWindow() == StreamHealthTracker::kMaxUnderrunEvents);
    CHECK(tracker.underrunsInWindow() <= StreamHealthTracker::kMaxUnderrunEvents);

    // Far beyond the window every event has aged out.
    CHECK(tracker.window(20000.0).underrunsPerMinute == 0.0);
    CHECK(tracker.underrunsInWindow() == 0);
}

TEST_CASE("a starved stream climbs from green to yellow to red",
          "[latency][health]") {
    StreamHealthTracker tracker;
    tracker.configure(60000.0);

    tracker.onBlock(0.0, false, false);
    CHECK(tracker.health(0.0) == StreamHealth::Green);

    tracker.onBlock(1000.0, true, false);
    CHECK(tracker.health(1000.0) == StreamHealth::Yellow);

    for (int i = 0; i < 9; ++i) tracker.onBlock(2000.0 + i, true, false);
    CHECK(tracker.health(3000.0) == StreamHealth::Red);

    // A hard error is red regardless of the counters.
    StreamHealthTracker errored;
    errored.configure(60000.0);
    errored.onBlock(0.0, false, false);
    errored.noteError();
    CHECK(errored.health(0.0) == StreamHealth::Red);
}
