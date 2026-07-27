#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "AudioCapture.hpp"
#include "Asr.hpp"
#include "Clock.hpp"
#include "Config.hpp"
#include "Pipeline.hpp"
#include "PreRoll.hpp"
#include "Vad.hpp"
#include "Wav.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

// ---------------------------------------------------------------------------
// Deterministic live-capture integration test (the lasting value from bug #1).
//
// It exercises the EXACT live path the daemon uses — a real PipeWire capture stream
// (format negotiation, RT callback, timestamps) into the adaptive VAD and whisper —
// with NO microphone, by playing a fixed WAV through a self-owned null sink and
// capturing its monitor. It plays JFK MIXED OVER ~0.09 RMS noise, i.e. the user's
// observed hot-source condition, and asserts the utterance is captured, endpointed,
// and transcribed with per-word timestamps.
//
// SAFETY: it creates and destroys ONLY its own null sink (tracked module id, unloaded
// even on failure), never touches the default sink / routing / wivrn.* nodes, and
// self-skips when PipeWire tooling is unavailable. The ctest wrapper additionally only
// RUNS it when HYPXRVOICE_LOOPBACK=1, so a normal `ctest` never perturbs live audio.
// ---------------------------------------------------------------------------

namespace {
    std::string runCapture(const std::string& cmd, int& rc) {
        std::string full = cmd + " 2>/dev/null";
        FILE*       p    = popen(full.c_str(), "r");
        if (!p) { rc = -1; return ""; }
        std::string out;
        char        buf[512];
        size_t      n;
        while ((n = fread(buf, 1, sizeof(buf), p)) > 0)
            out.append(buf, n);
        rc = pclose(p);
        return out;
    }

    bool haveCmd(const char* c) {
        int rc;
        runCapture(std::string("command -v ") + c, rc);
        return rc == 0;
    }

    bool pipewireUp() {
        int rc;
        runCapture("pactl info", rc);
        return rc == 0;
    }

    std::string lower(std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // Create a private null sink and return its module id ("" on failure). The caller
    // MUST unload it (see SSinkGuard). Never touches default routing or wivrn.* nodes.
    std::string loadNullSink(const std::string& sinkName) {
        int         rc    = 0;
        std::string idOut = runCapture(
            "pactl load-module module-null-sink media.class=Audio/Sink sink_name=" + sinkName +
                " channel_map=mono object.linger=false",
            rc);
        std::string moduleId;
        for (char c : idOut) {
            if (std::isdigit(static_cast<unsigned char>(c)))
                moduleId += c;
            else if (!moduleId.empty())
                break;
        }
        return rc == 0 ? moduleId : "";
    }

    struct SSinkGuard {
        std::string id;
        ~SSinkGuard() {
            if (id.empty())
                return;
            int rc;
            runCapture("pactl unload-module " + id, rc);
        }
    };

    std::string findModel() {
        if (const char* e = std::getenv("HYPXRVOICE_TEST_MODEL"); e && *e && std::filesystem::exists(e))
            return e;
        std::string def = std::string(HYPXRVOICE_SOURCE_DIR) + "/models/ggml-base.en.bin";
        return std::filesystem::exists(def) ? def : "";
    }

    // Minimal S16LE mono WAV writer (16 kHz), for the noisy fixture we play.
    bool writeWavS16(const std::string& path, const std::vector<float>& mono, int sr) {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        const uint32_t dataBytes = static_cast<uint32_t>(mono.size() * 2);
        std::fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); std::fwrite("WAVE", 1, 4, f);
        std::fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(1);
        u32(static_cast<uint32_t>(sr)); u32(static_cast<uint32_t>(sr * 2)); u16(2); u16(16);
        std::fwrite("data", 1, 4, f); u32(dataBytes);
        for (float x : mono) {
            float c = x < -1.f ? -1.f : (x > 1.f ? 1.f : x);
            int16_t s = static_cast<int16_t>(c * 32767.f);
            std::fwrite(&s, 2, 1, f);
        }
        std::fclose(f);
        return true;
    }
}

TEST_CASE("loopback: live PipeWire capture -> adaptive VAD -> ASR over a hot noise floor") {
    if (!haveCmd("pactl") || !haveCmd("pw-play") || !pipewireUp()) {
        MESSAGE("PipeWire tooling/server unavailable — skipping live loopback test");
        return;
    }

    // Build the noisy fixture: JFK + ~0.09 RMS gaussian noise (the user's condition).
    std::vector<float> jfk;
    std::string        werr;
    std::string        src = std::string(HYPXRVOICE_TEST_ASSETS) + "/jfk.wav";
    REQUIRE_MESSAGE(loadAudioMono16k(src, jfk, werr), werr);
    {
        std::mt19937                    rng(20260714);
        std::normal_distribution<float> g(0.f, 0.09f);
        for (auto& x : jfk) x += g(rng);
    }
    std::string noisyPath = std::filesystem::temp_directory_path() /
                            ("hypxrvoice_loopback_" + std::to_string(getpid()) + ".wav");
    REQUIRE(writeWavS16(noisyPath, jfk, 16000));

    // Create OUR null sink (tracked id; RAII unload). Never touches default routing.
    const std::string sinkName = "hypxrvoice_test_" + std::to_string(getpid());
    int               rc       = 0;
    std::string       idOut    = runCapture(
        "pactl load-module module-null-sink media.class=Audio/Sink sink_name=" + sinkName +
            " channel_map=mono object.linger=false",
        rc);
    // Trim to the integer module id.
    std::string moduleId;
    for (char c : idOut) { if (std::isdigit(static_cast<unsigned char>(c))) moduleId += c; else if (!moduleId.empty()) break; }
    if (rc != 0 || moduleId.empty()) {
        std::filesystem::remove(noisyPath);
        MESSAGE("could not create null sink (pactl load-module failed) — skipping");
        return;
    }
    struct SGuard {
        std::string id, wav;
        ~SGuard() {
            int rc;
            runCapture("pactl unload-module " + id, rc);
            std::error_code ec; std::filesystem::remove(wav, ec);
        }
    } guard{moduleId, noisyPath};

    // Live capture of the sink's monitor into the adaptive VAD (the daemon's path).
    SConfig    cfg; // defaults: adaptive VAD on, factor 1.6, window 1500ms
    SVadConfig vc;
    vc.sampleRate       = 16000;
    vc.energyThreshold  = cfg.vad.energyThreshold;
    vc.startMs          = cfg.vad.startMs;
    vc.endMs            = cfg.vad.endMs;
    vc.maxUtteranceMs   = cfg.vad.maxUtteranceMs;
    vc.preRollMs        = cfg.vad.preRollMs;
    vc.adaptive         = cfg.vad.adaptive;
    vc.noiseFloorFactor = cfg.vad.noiseFloorFactor;
    vc.noiseWindowMs    = cfg.vad.noiseWindowMs;
    CVad vad(vc);

    std::mutex                  mu;
    std::vector<SSpeechSegment> segs;
    std::atomic<uint64_t>       framesReceived{0};
    float                       maxFloor = 0.f, maxThr = 0.f; // peak seen during capture

    CAudioCapture cap;
    bool started = cap.start(sinkName + ".monitor", 16000,
                             [&](const float* f, size_t n, int64_t ms) {
                                 framesReceived.fetch_add(n, std::memory_order_relaxed);
                                 std::lock_guard<std::mutex> lk(mu);
                                 vad.push(f, n, ms, segs);
                                 // The floor is a point-in-time estimate; capture its
                                 // peak while the noisy speech is flowing (it falls back
                                 // to ~0 once the fixture ends and the window refills
                                 // with silence).
                                 maxFloor = std::max(maxFloor, vad.noiseFloor());
                                 maxThr   = std::max(maxThr, vad.threshold());
                             });
    if (!started) {
        MESSAGE("CAudioCapture.start() failed on the null-sink monitor — skipping");
        return;
    }

    // Let the stream connect, then play the noisy fixture INTO our sink (blocking).
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    runCapture("pw-play --target=" + sinkName + " " + noisyPath, rc);
    // Drain trailing buffers, then close the utterance.
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    cap.stop();
    {
        std::lock_guard<std::mutex> lk(mu);
        vad.flush(segs);
    }

    // The live capture path delivered real PCM...
    CHECK(framesReceived.load() > 16000); // > ~1 s of frames
    // ...the floor tracked the hot ambient (well above the fixed 0.012) during speech...
    CAPTURE(maxFloor);
    CAPTURE(maxThr);
    CHECK(maxFloor > 0.03f);
    CHECK(maxThr > 0.05f);
    // ...and the utterance was captured and ENDPOINTED (not one 12 s noise blob).
    REQUIRE_MESSAGE(!segs.empty(), "adaptive VAD produced no speech segment from the live path");
    for (const auto& s : segs)
        CHECK((s.endMs - s.onsetMs) < vc.maxUtteranceMs);

    // If a model is present, prove a real transcript with per-word timestamps emerges
    // from the live-captured audio (the end-to-end acceptance for bug #1).
    std::string model = findModel();
    if (model.empty()) {
        MESSAGE("no whisper model — asserted VAD onset only; skipping ASR assertions");
        return;
    }
    CAsr          asr;
    std::string   aerr;
    CAsr::SParams ap{model, "en", 4, false};
    REQUIRE_MESSAGE(asr.load(ap, aerr), aerr);
    CHECK(asr.dtwActive()); // DTW word timestamps must be live (bug #3)

    std::string joined;
    bool        anyWords    = false;
    bool        wordsHaveTs = false;
    for (const auto& s : segs) {
        STranscript t;
        if (!Pipeline::processSegment(asr, cfg, s, EActivation::Oneshot, /*requireWake=*/false, t))
            continue;
        joined += " " + lower(t.text);
        for (const auto& w : t.words) {
            anyWords = true;
            // Word timestamps are absolute monotonic ms anchored in the segment.
            if (w.startMs >= s.bufferStartMs && w.endMs >= w.startMs)
                wordsHaveTs = true;
        }
    }
    std::fprintf(stderr, "[loopback] live-path transcript (noisy JFK):%s\n", joined.c_str());
    CAPTURE(joined);
    CHECK(anyWords);
    CHECK(wordsHaveTs);
    // JFK content should survive the noise (loose substring — ASR/noise tolerant).
    CHECK((joined.find("country") != std::string::npos || joined.find("ask") != std::string::npos ||
           joined.find("americ") != std::string::npos));
}

// ---------------------------------------------------------------------------
// WP-V6 bug #1 — persistent capture + pre-roll splice, through the REAL PipeWire path.
//
// The daemon used to open the stream at the PTT press; `wivrn.source` needed ~0.5–1 s to
// resume (the WiVRn server has to ask the headset client to start its mic over the
// network) and the user's first words fell into that hole. The daemon now HOLDS the
// stream and, while the gate is shut, routes frames nowhere but a rolling pre-roll ring
// which is spliced onto the front of the next window.
//
// This exercises that exact arrangement against a live PipeWire stream: capture is up and
// GATED while speech is already playing, then the gate opens mid-sentence. The assertion
// is that the captured utterance PREDATES the gate — i.e. the words spoken before the
// "press" were not lost. Same safety envelope as the test above (own null sink only).
// ---------------------------------------------------------------------------

TEST_CASE("loopback: a held stream's pre-roll ring recovers speech spoken before the gate opens") {
    if (!haveCmd("pactl") || !haveCmd("pw-play") || !pipewireUp()) {
        MESSAGE("PipeWire tooling/server unavailable — skipping live pre-roll test");
        return;
    }

    const std::string sinkName = "hypxrvoice_preroll_" + std::to_string(getpid());
    SSinkGuard        guard{loadNullSink(sinkName)};
    if (guard.id.empty()) {
        MESSAGE("could not create null sink (pactl load-module failed) — skipping");
        return;
    }

    SConfig cfg;
    REQUIRE(cfg.capture.hold);          // the shipped default
    REQUIRE(cfg.capture.preRollMs > 0);

    SVadConfig vc;
    vc.sampleRate       = 16000;
    vc.energyThreshold  = cfg.vad.energyThreshold;
    vc.startMs          = cfg.vad.startMs;
    vc.endMs            = cfg.vad.endMs;
    vc.maxUtteranceMs   = cfg.vad.maxUtteranceMs;
    vc.preRollMs        = cfg.vad.preRollMs;
    vc.adaptive         = cfg.vad.adaptive;
    vc.noiseFloorFactor = cfg.vad.noiseFloorFactor;
    vc.noiseWindowMs    = cfg.vad.noiseWindowMs;

    std::mutex                  mu;
    CPreRollRing                ring;
    ring.configure(16000, 1500); // a touch longer than the default, to span the whole gate
    std::unique_ptr<CVad>       vad;
    std::vector<SSpeechSegment> segs;
    bool                        gateOpen      = false;
    int64_t                     firstSpeechMs = 0; // capture time of the first loud frame
    std::atomic<uint64_t>       framesReceived{0};
    size_t                      maxRingSamples = 0;
    size_t                      gatedFramesToVad = 0; // MUST stay 0: the privacy invariant

    CAudioCapture cap;
    // Exactly CDaemon::drainAudio's gate: shut => the ring and nothing else.
    bool started = cap.start(sinkName + ".monitor", 16000, [&](const float* f, size_t n, int64_t ms) {
        framesReceived.fetch_add(n, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(mu);
        if (!gateOpen) {
            double acc = 0;
            for (size_t i = 0; i < n; i++)
                acc += static_cast<double>(f[i]) * f[i];
            if (firstSpeechMs == 0 && n > 0 && std::sqrt(acc / n) > 0.05)
                firstSpeechMs = ms;
            ring.push(f, n, ms);
            maxRingSamples = std::max(maxRingSamples, ring.size());
            return;
        }
        if (!vad)
            gatedFramesToVad += n; // would mean the gate leaked (never happens)
        else
            vad->push(f, n, ms, segs);
    });
    if (!started) {
        MESSAGE("CAudioCapture.start() failed on the null-sink monitor — skipping");
        return;
    }

    // The stream is HELD open before anything is played — the whole point of the fix.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK_FALSE(cap.failed());
    const std::string heldState = cap.stateName();
    CAPTURE(heldState);

    // Play the fixture while the gate is still SHUT: the speaker starts before the press.
    std::string wav = std::string(HYPXRVOICE_TEST_ASSETS) + "/jfk.wav";
    std::thread player([&] {
        int rc;
        runCapture("pw-play --target=" + sinkName + " " + wav, rc);
    });

    // Wait for speech to actually be flowing, then let ~900 ms of it accumulate in the
    // ring before the "PTT press" — the hole the user was falling into.
    int64_t onset = 0;
    for (int i = 0; i < 100 && onset == 0; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::lock_guard<std::mutex> lk(mu);
        onset = firstSpeechMs;
    }
    REQUIRE_MESSAGE(onset != 0, "no audible frames arrived from the null-sink monitor");
    std::this_thread::sleep_for(std::chrono::milliseconds(900));

    // ---- the press: open the gate, splicing the ring in front of a fresh VAD ----
    int64_t gateOpenMs = 0, spliceStartMs = 0;
    size_t  spliced = 0;
    {
        std::lock_guard<std::mutex> lk(mu);
        gateOpenMs = Clock::monotonicMs();
        vad        = std::make_unique<CVad>(vc);
        std::vector<float> pre;
        spliced = ring.drain(pre, spliceStartMs);
        if (spliced > 0)
            vad->push(pre.data(), pre.size(), spliceStartMs, segs);
        gateOpen = true;
    }

    player.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    cap.stop();
    {
        std::lock_guard<std::mutex> lk(mu);
        vad->flush(segs);
    }

    CHECK(framesReceived.load() > 16000);
    CHECK(gatedFramesToVad == 0);               // gated audio never reached the VAD
    CHECK(maxRingSamples <= ring.capacity());   // ...and the ring stayed bounded
    CAPTURE(spliced);
    REQUIRE_MESSAGE(spliced > 0, "the held stream buffered no pre-roll while gated");
    // The splice carried real, pre-press audio: at least ~700 ms of it, starting before
    // the gate opened.
    CHECK(spliced >= static_cast<size_t>(16000 * 0.7));
    CHECK(spliceStartMs < gateOpenMs);

    REQUIRE_MESSAGE(!segs.empty(), "no speech segment came out of the spliced window");
    // THE ASSERTION: the utterance we captured began BEFORE the press. Without the ring
    // this is impossible — the audio simply did not exist yet.
    CHECK(segs.front().bufferStartMs < gateOpenMs);
    CHECK(segs.front().onsetMs < gateOpenMs);

    std::string model = findModel();
    if (model.empty()) {
        MESSAGE("no whisper model — asserted the splice timing only; skipping ASR assertions");
        return;
    }
    CAsr          asr;
    std::string   aerr;
    CAsr::SParams ap{model, "en", 4, false};
    REQUIRE_MESSAGE(asr.load(ap, aerr), aerr);

    STranscript first;
    REQUIRE(Pipeline::processSegment(asr, cfg, segs.front(), EActivation::Oneshot,
                                     /*requireWake=*/false, first));
    std::fprintf(stderr, "[loopback] pre-roll spliced first segment: \"%s\"\n", first.text.c_str());
    CAPTURE(first.text);
    CHECK(!first.text.empty());
    // The fixture opens with "And so my fellow Americans…", which is spoken entirely in
    // the GATED region: those words can only be transcribed if the splice worked. Before
    // the fix this segment could not exist at all — the audio was never captured.
    const std::string lowered = lower(first.text);
    CHECK((lowered.find("fellow") != std::string::npos ||
           lowered.find("americ") != std::string::npos ||
           lowered.find("and so") != std::string::npos));
}
