#include "doctest.h"

#include "Config.hpp"

TEST_CASE("config: parses sections and typed values") {
    const char* doc = R"(
# hypxrvoice config
[activation]
mode = "wake"
fallback = "gate"
keyboard_idle_ms = 2500

[audio]
source = "wivrn.source"   # trailing comment ignored
sample_rate = 16000

[vad]
energy_threshold = 0.02
start_ms = 120

[wake]
enabled = false
phrase = "hey hypr"
fuzz = 3

[asr]
model = "/models/ggml-base.en.bin"
threads = 6
translate = true
)";
    SConfig                  c;
    std::vector<std::string> errs, warns;
    REQUIRE(parseConfig(doc, c, errs, warns));
    CHECK(errs.empty());
    CHECK(c.activation.mode == EActivationMode::Wake);
    CHECK(c.activation.fallback == EFallback::Gate);
    CHECK(c.activation.keyboardIdleMs == 2500);
    CHECK(c.audio.source == "wivrn.source");
    CHECK(c.audio.sampleRate == 16000);
    CHECK(c.vad.energyThreshold == doctest::Approx(0.02));
    CHECK(c.vad.startMs == 120);
    CHECK(c.wake.enabled == false);
    CHECK(c.wake.phrase == "hey hypr");
    CHECK(c.wake.fuzz == 3);
    CHECK(c.asr.model == "/models/ggml-base.en.bin");
    CHECK(c.asr.threads == 6);
    CHECK(c.asr.translate == true);
}

TEST_CASE("config: unknown keys warn but do not fail") {
    const char* doc = "[activation]\nmode = \"auto\"\nnonsense = 3\n";
    SConfig                  c;
    std::vector<std::string> errs, warns;
    CHECK(parseConfig(doc, c, errs, warns));
    CHECK(errs.empty());
    CHECK(warns.size() == 1);
}

TEST_CASE("config: type errors fail the parse") {
    const char* doc = "[audio]\nsample_rate = \"lots\"\n";
    SConfig                  c;
    std::vector<std::string> errs, warns;
    CHECK_FALSE(parseConfig(doc, c, errs, warns));
    CHECK_FALSE(errs.empty());
}

TEST_CASE("config: bad enum value is an error") {
    const char* doc = "[activation]\nmode = \"telepathy\"\n";
    SConfig                  c;
    std::vector<std::string> errs, warns;
    CHECK_FALSE(parseConfig(doc, c, errs, warns));
}

TEST_CASE("config: empty document yields defaults") {
    SConfig                  c;
    std::vector<std::string> errs, warns;
    CHECK(parseConfig("", c, errs, warns));
    CHECK(c.activation.mode == EActivationMode::Auto);
    CHECK(c.wake.phrase == "hey hypr");
    CHECK(c.audio.sampleRate == 16000);
    // WP-V6: the persistent capture stream + pre-roll splice are ON by default — the
    // per-window stream teardown cost the user the first word of every utterance.
    CHECK(c.capture.hold);
    CHECK(c.capture.preRollMs == 1000);
}

TEST_CASE("config: the capture knobs parse") {
    const char* doc = "[capture]\nhold = false\npreroll_ms = 450\n";
    SConfig                  c;
    std::vector<std::string> errs, warns;
    REQUIRE(parseConfig(doc, c, errs, warns));
    CHECK(errs.empty());
    CHECK(warns.empty());
    CHECK_FALSE(c.capture.hold);
    CHECK(c.capture.preRollMs == 450);
}

TEST_CASE("config: the onset back-pad and the debug dump knobs parse") {
    const char* doc = "[vad]\nonset_backpad_ms = 420\n"
                      "[executor]\nallow_window = false\n"
                      "[debug]\ndump_audio_dir = \"/tmp/hypxrvoice-dumps\"\ndump_audio_keep = 12\n";
    SConfig                  c;
    std::vector<std::string> errs, warns;
    REQUIRE(parseConfig(doc, c, errs, warns));
    CHECK(errs.empty());
    CHECK(warns.empty());
    CHECK(c.vad.onsetBackpadMs == 420);
    CHECK_FALSE(c.executor.allowWindow);
    CHECK(c.debug.dumpAudioDir == "/tmp/hypxrvoice-dumps");
    CHECK(c.debug.dumpAudioKeep == 12);
}

TEST_CASE("config: audio dumping is OFF and window control is ON by default") {
    SConfig                  c;
    std::vector<std::string> errs, warns;
    REQUIRE(parseConfig("", c, errs, warns));
    CHECK(c.debug.dumpAudioDir.empty()); // never writes mic audio unless asked
    CHECK(c.vad.onsetBackpadMs == 300);
    CHECK(c.executor.allowWindow);
}

TEST_CASE("config: the round-5 placement + create knobs parse, with safe defaults") {
    SConfig                  d;
    std::vector<std::string> e0, w0;
    REQUIRE(parseConfig("", d, e0, w0));
    CHECK(d.intent.placeDistanceM == doctest::Approx(1.3));
    CHECK(d.intent.placeMinDistanceM == doctest::Approx(0.5));
    CHECK(d.executor.allowCreateMonitor);

    const char* doc = "[intent]\nplace_distance_m = 2.0\nplace_min_distance_m = 0.75\n"
                      "[executor]\nallow_create_monitor = false\n";
    SConfig                  c;
    std::vector<std::string> errs, warns;
    REQUIRE(parseConfig(doc, c, errs, warns));
    CHECK(errs.empty());
    CHECK(warns.empty());
    CHECK(c.intent.placeDistanceM == doctest::Approx(2.0));
    CHECK(c.intent.placeMinDistanceM == doctest::Approx(0.75));
    CHECK_FALSE(c.executor.allowCreateMonitor);
}
