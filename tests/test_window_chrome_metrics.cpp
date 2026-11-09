#include <catch2/catch_test_macros.hpp>

#include <pluginrack/WindowChromeMetrics.h>

TEST_CASE("window chrome metrics share one layout", "[window-chrome]") {
    using namespace pluginrack::windowChrome;
    CHECK(kHeaderHeight == 36);
    CHECK(kAnalyzerHeight == 28);
    CHECK(kKnobRowHeight == 132);
    CHECK(kHeaderSidePad == 12);
    CHECK(kWindowPad == 8);
    CHECK(kTitleFontPt == 15.0f);
    CHECK(kEnableButtonW == 72);
    CHECK(kPresetComboW == 120);
    CHECK(kAnalyzerGap == 8);
    CHECK(kHeaderGap == 6);
}

TEST_CASE("every built-in window uses one-word uppercase titles", "[window-chrome]") {
    using namespace pluginrack::windowChrome;
    CHECK(kEqTitle == "EQ");
    CHECK(kDynamicsTitle == "DYNAMICS");
    CHECK(kMultibandTitle == "MULTIBAND");
    CHECK(kDeEsserTitle == "DE-ESSER");
    CHECK(kLimiterTitle == "LIMITER");
    for (const std::string_view title : { kEqTitle, kDynamicsTitle, kMultibandTitle, kDeEsserTitle, kLimiterTitle }) {
        for (const char c : title) {
            const bool isUpper = (c >= 'A' && c <= 'Z');
            CHECK((isUpper || c == '-'));
        }
        CHECK(title.find_first_of(" /") == std::string_view::npos);
    }
}
