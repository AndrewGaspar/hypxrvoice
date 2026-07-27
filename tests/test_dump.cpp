#include "doctest.h"

#include "AudioDump.hpp"
#include "Wav.hpp"

#include <algorithm>
#include <filesystem>
#include <unistd.h>
#include <fstream>
#include <string>
#include <vector>

// The capture-forensics facility (debug.dump_audio_dir). These tests pin the three
// properties the next live round depends on: it writes NOTHING when disabled, the two
// artefacts that discriminate source-side loss from segmenter loss both land on disk with
// a sidecar tying them together, and disk use stays bounded.

namespace fs = std::filesystem;

namespace {
    constexpr int kSr = 16000;

    fs::path tempDir(const char* tag) {
        fs::path p = fs::temp_directory_path() /
                     ("hypxrvoice-dump-test-" + std::string(tag) + "-" + std::to_string(::getpid()));
        fs::remove_all(p);
        return p;
    }

    std::vector<float> ramp(size_t n, float scale = 0.1f) {
        std::vector<float> v(n);
        for (size_t i = 0; i < n; i++)
            v[i] = scale * static_cast<float>((i % 200) - 100) / 100.f;
        return v;
    }

    SSpeechSegment segment(int64_t onsetMs, int64_t bufferStartMs, size_t n) {
        SSpeechSegment s;
        s.onsetMs       = onsetMs;
        s.endMs         = onsetMs + 500;
        s.bufferStartMs = bufferStartMs;
        s.samples       = ramp(n, 0.2f);
        return s;
    }

    size_t countMatching(const fs::path& dir, const char* suffix) {
        size_t n = 0;
        for (const auto& e : fs::directory_iterator(dir)) {
            const std::string name = e.path().filename().string();
            const std::string suf(suffix);
            if (name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0)
                n++;
        }
        return n;
    }

    std::string slurp(const fs::path& p) {
        std::ifstream f(p);
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
}

TEST_CASE("dump: disabled by default — no directory is created and nothing is written") {
    const fs::path dir = tempDir("off");
    CAudioDump     d;
    std::string    err;
    REQUIRE(d.configure("", 10, kSr, err));
    CHECK_FALSE(d.enabled());

    // Every entry point must be inert while disabled.
    auto pre = ramp(kSr / 2);
    d.beginWindow(1000);
    d.notePreRoll(pre.data(), pre.size(), 500);
    d.appendWindow(pre.data(), pre.size(), 1000);
    d.noteSegment(segment(1200, 1000, kSr / 4));
    d.endWindow("ok", "hello");

    CHECK_FALSE(fs::exists(dir));
}

TEST_CASE("dump: a window writes the full audio, each segment, and a sidecar that ties them") {
    const fs::path dir = tempDir("window");
    CAudioDump     d;
    std::string    err;
    SVadConfig     vc;
    vc.onsetBackpadMs = 300;
    d.noteVadConfig(vc);
    REQUIRE(d.configure(dir.string(), 10, kSr, err));
    REQUIRE(d.enabled());

    // 1000 ms of spliced pre-roll, then 800 ms of live audio, then one segment that
    // starts 250 ms into the pre-roll.
    const int64_t preStart = 100'000;
    auto          pre      = ramp(static_cast<size_t>(kSr));       // 1000 ms
    auto          live     = ramp(static_cast<size_t>(kSr * 4 / 5)); // 800 ms

    d.beginWindow(preStart + 1000); // the gate opened at the END of the pre-roll
    d.notePreRoll(pre.data(), pre.size(), preStart);
    d.appendWindow(live.data(), live.size(), preStart + 1000);
    d.noteSegment(segment(/*onset*/ preStart + 550, /*bufferStart*/ preStart + 250, kSr / 2));
    d.endWindow("ok", "workspace three");

    CHECK(countMatching(dir, "-window.wav") == 1);
    CHECK(countMatching(dir, "-seg0.wav") == 1);
    CHECK(countMatching(dir, ".txt") == 1);

    // The window wav must hold the pre-roll AND the live audio, in that order.
    fs::path windowWav;
    fs::path sidecar;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().filename().string().find("-window.wav") != std::string::npos)
            windowWav = e.path();
        else if (e.path().extension() == ".txt")
            sidecar = e.path();
    }
    REQUIRE_FALSE(windowWav.empty());
    std::vector<float> back;
    std::string        loadErr;
    REQUIRE(loadAudioMono16k(windowWav.string(), back, loadErr));
    CHECK(back.size() == pre.size() + live.size());

    // The sidecar carries the splice point and the segment's offset INSIDE the window,
    // which is what makes "is the first word in the window but not in the segment?"
    // answerable by looking, not guessing.
    const std::string side = slurp(sidecar);
    CHECK(side.find("splice_offset_samples 16000") != std::string::npos);
    CHECK(side.find("splice_offset_ms 1000") != std::string::npos);
    CHECK(side.find("segments 1") != std::string::npos);
    CHECK(side.find("window_offset_samples=4000") != std::string::npos); // 250 ms in
    CHECK(side.find("backpad_ms 300") != std::string::npos);             // onset - bufferStart
    CHECK(side.find("vad_onset_backpad_ms 300") != std::string::npos);
    CHECK(side.find("outcome ok") != std::string::npos);
    CHECK(side.find("transcript workspace three") != std::string::npos);

    fs::remove_all(dir);
}

TEST_CASE("dump: retention prunes to the newest N windows") {
    const fs::path dir = tempDir("prune");
    CAudioDump     d;
    std::string    err;
    REQUIRE(d.configure(dir.string(), 3, kSr, err));

    auto audio = ramp(kSr / 10);
    for (int i = 0; i < 8; i++) {
        d.beginWindow(200'000 + i * 1000);
        d.appendWindow(audio.data(), audio.size(), 200'000 + i * 1000);
        d.noteSegment(segment(200'000 + i * 1000, 200'000 + i * 1000, kSr / 20));
        d.endWindow("ok", "utterance " + std::to_string(i));
    }

    // beginWindow prunes BEFORE writing, so the just-finished window is always kept:
    // at most keep+1 windows exist on disk, never the full eight.
    const size_t windows = countMatching(dir, "-window.wav");
    CHECK(windows >= 3);
    CHECK(windows <= 4);
    CHECK(countMatching(dir, ".txt") == windows);
    CHECK(countMatching(dir, "-seg0.wav") == windows);

    fs::remove_all(dir);
}

TEST_CASE("dump: a window left open is still flushed when the next one begins") {
    const fs::path dir = tempDir("orphan");
    CAudioDump     d;
    std::string    err;
    REQUIRE(d.configure(dir.string(), 10, kSr, err));

    auto audio = ramp(kSr / 10);
    d.beginWindow(1000);
    d.appendWindow(audio.data(), audio.size(), 1000);
    d.beginWindow(5000); // no endWindow for the first one
    d.appendWindow(audio.data(), audio.size(), 5000);
    d.endWindow("ok", "second");

    CHECK(countMatching(dir, "-window.wav") == 2);
    bool sawSuperseded = false;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().extension() == ".txt" && slurp(e.path()).find("outcome superseded") != std::string::npos)
            sawSuperseded = true;
    CHECK(sawSuperseded);

    fs::remove_all(dir);
}
