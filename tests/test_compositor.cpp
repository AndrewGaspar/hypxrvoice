#include "doctest.h"

#include "Compositor.hpp"

TEST_CASE("compositor: parses presence + at-keyboard from status JSON") {
    // Trimmed shape of `hyprctl openxr status -j`.
    const char* json = R"json({
        "state": "focused",
        "userPresence": "yes",
        "visible": "yes",
        "handInput": { "mode": "auto", "state": "active" }
    })json";
    SEnvSignal e = CCompositor::parseStatus(json);
    CHECK(e.compositorAvailable);
    CHECK(e.headsetPresent);
    CHECK_FALSE(e.atKeyboard); // "active" hands => away from keyboard
}

TEST_CASE("compositor: typing state maps to at-keyboard") {
    const char* json = R"json({
        "userPresence": "yes", "visible": "yes",
        "handInput": { "mode": "auto", "state": "gated (keyboard)" }
    })json";
    SEnvSignal e = CCompositor::parseStatus(json);
    CHECK(e.compositorAvailable);
    CHECK(e.headsetPresent);
    CHECK(e.atKeyboard);
}

TEST_CASE("compositor: doffed headset is not present") {
    const char* json = R"json({
        "userPresence": "no", "visible": "no",
        "handInput": { "mode": "auto", "state": "off" }
    })json";
    SEnvSignal e = CCompositor::parseStatus(json);
    CHECK(e.compositorAvailable);
    CHECK_FALSE(e.headsetPresent);
    CHECK(e.atKeyboard); // uninformative hand state => conservative
}

TEST_CASE("compositor: presence falls back to visibility when unknown") {
    const char* json = R"json({
        "userPresence": "unsupported", "visible": "yes",
        "handInput": { "mode": "auto", "state": "active" }
    })json";
    SEnvSignal e = CCompositor::parseStatus(json);
    CHECK(e.headsetPresent); // visible=yes stands in for presence
}

TEST_CASE("compositor: absent openxr section => unavailable") {
    // Valid JSON but no XR fields (e.g. a non-XR hyprctl reply or an error blob).
    CHECK_FALSE(CCompositor::parseStatus(R"json({"someOtherField": 1})json").compositorAvailable);
    CHECK_FALSE(CCompositor::parseStatus("not json at all").compositorAvailable);
    CHECK_FALSE(CCompositor::parseStatus("").compositorAvailable);
    // Unavailable defaults must be safe: at-keyboard true (stay PTT-gated).
    CHECK(CCompositor::parseStatus("").atKeyboard);
}
