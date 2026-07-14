#include "Asr.hpp"
#include "Clock.hpp"
#include "Config.hpp"
#include "Daemon.hpp"
#include "DesktopContext.hpp"
#include "Executor.hpp"
#include "Feedback.hpp"
#include "GazeResolver.hpp"
#include "IntentPipeline.hpp"
#include "Log.hpp"
#include "Pipeline.hpp"
#include "Vad.hpp"
#include "Wav.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

static std::atomic<bool> g_stop{false};
static void              onSignal(int) { g_stop.store(true); }

static void printUsage(const char* argv0) {
    std::fprintf(stderr,
                 "hypxrvoiced — local-first voice-control daemon for HypXRland\n"
                 "\n"
                 "Usage: %s [options]\n"
                 "\n"
                 "  --config <path>     Config file (default: $XDG_CONFIG_HOME/hypxrvoice/config.toml).\n"
                 "  --oneshot <file>    Debug/test: transcribe an audio file through the full\n"
                 "                      VAD->ASR->transcript pipeline (no mic) and print each\n"
                 "                      transcript as a JSON line, then exit.\n"
                 "  --intent            With --oneshot: also run each transcript through the\n"
                 "                      intent tier + executor (honours executor.dry_run).\n"
                 "  --intent-text <s>   Debug: run the intent tier + executor on the given\n"
                 "                      utterance text (no audio) and exit.\n"
                 "  --model <path>      Override asr.model (whisper ggml model path).\n"
                 "  --verbose           Enable DEBUG logging.\n"
                 "  -h, --help          Show this help.\n"
                 "\n"
                 "Control the running daemon with `hypxrvoicectl` (ptt start|stop|toggle,\n"
                 "status, reload).\n",
                 argv0);
}

// Offline pipeline: file -> VAD segments -> ASR -> transcripts. Shares the exact
// segment path with the live daemon (Pipeline::processSegment).
static int runOneshot(const std::string& file, const SConfig& cfg, bool runIntent) {
    CAsr          asr;
    std::string   err;
    CAsr::SParams ap{cfg.asr.model, cfg.asr.language, cfg.asr.threads, cfg.asr.translate};
    if (!asr.load(ap, err)) {
        Log::log(Log::ERR, "{}", err);
        return 1;
    }

    // Optional local-LLM intent backend (falls back to the rule backend if it fails).
    std::unique_ptr<CLlamaIntent> llama;
    IntentFn                      backend;
    if (runIntent && cfg.intent.enabled && cfg.intent.backend == "llama" && !cfg.intent.model.empty()) {
        llama = std::make_unique<CLlamaIntent>();
        SLlamaParams lp{cfg.intent.model, cfg.intent.temperature, cfg.intent.nThreads, 4096, 256};
        std::string  lerr;
        if (llama->load(lp, lerr)) {
            SIntentConfig ic = IntentPipeline::intentConfig(cfg);
            backend = [&llama, ic](const STranscript& t, const SDesktopContext& c,
                                   const GazeQueryFn& g) { return llama->resolve(t, c, g, ic); };
        } else {
            Log::log(Log::WARN, "llama backend unavailable ({}); using rule backend", lerr);
        }
    }

    std::vector<float> audio;
    if (!loadAudioMono16k(file, audio, err)) {
        Log::log(Log::ERR, "{}", err);
        return 1;
    }
    Log::log(Log::INFO, "loaded {} ({} samples @16kHz, {:.2f}s)", file, audio.size(), audio.size() / 16000.0);

    SVadConfig vc;
    vc.sampleRate      = 16000;
    vc.energyThreshold = cfg.vad.energyThreshold;
    vc.startMs         = cfg.vad.startMs;
    vc.endMs           = cfg.vad.endMs;
    vc.maxUtteranceMs  = cfg.vad.maxUtteranceMs;
    vc.preRollMs       = cfg.vad.preRollMs;
    CVad vad(vc);

    // Anchor the file's virtual capture start to "now" so onset/word timestamps are
    // real CLOCK_MONOTONIC values, exactly like a live capture.
    const int64_t             base = Clock::monotonicMs();
    std::vector<SSpeechSegment> segs;
    vad.push(audio.data(), audio.size(), base, segs);
    vad.flush(segs);

    if (segs.empty()) {
        Log::log(Log::WARN, "VAD found no speech in {} (try lowering vad.energy_threshold)", file);
        return 2;
    }

    int emitted = 0;
    for (auto& s : segs) {
        STranscript t;
        if (Pipeline::processSegment(asr, cfg, s, EActivation::Oneshot, /*requireWake=*/false, t)) {
            Feedback::emitTranscript(t, cfg);
            emitted++;
            // --intent: run the transcript through the full V4 chain against the live
            // compositor (read-only snapshot + gaze ring; executor honours dry_run).
            if (runIntent) {
                const int64_t t0 = Clock::monotonicMs();
                auto r = IntentPipeline::process(t, cfg, defaultHyprctlQuery, defaultGazeQuery,
                                                 defaultRunner, backend);
                Log::log(Log::INFO, "intent resolved in {}ms ({}): {} target={}",
                         Clock::monotonicMs() - t0, backend ? "llama" : "rule",
                         verbName(r.action.verb), r.action.target.empty() ? "-" : r.action.target);
            }
        }
    }
    return emitted > 0 ? 0 : 2;
}

// Debug/test: run the intent tier + executor on a GIVEN utterance (no audio). Builds a
// synthetic transcript with evenly-spaced word timestamps ending "now", then runs the
// configured backend against the live compositor (read-only) + gaze ring. Honours
// executor.dry_run. Prints the resolved action + plan and the intent latency.
static int runIntentText(const std::string& text, const SConfig& cfg) {
    std::unique_ptr<CLlamaIntent> llama;
    IntentFn                      backend;
    if (cfg.intent.enabled && cfg.intent.backend == "llama" && !cfg.intent.model.empty()) {
        llama = std::make_unique<CLlamaIntent>();
        SLlamaParams lp{cfg.intent.model, cfg.intent.temperature, cfg.intent.nThreads, 4096, 256};
        std::string  lerr;
        if (llama->load(lp, lerr)) {
            SIntentConfig ic = IntentPipeline::intentConfig(cfg);
            backend = [&llama, ic](const STranscript& t, const SDesktopContext& c,
                                   const GazeQueryFn& g) { return llama->resolve(t, c, g, ic); };
        } else {
            Log::log(Log::WARN, "llama backend unavailable ({}); using rule backend", lerr);
        }
    }

    STranscript t;
    t.text          = text;
    const int64_t n = Clock::monotonicMs();
    // Split into words, spacing them 200 ms apart ending at "now".
    std::vector<std::string> ws;
    std::string              cur;
    for (char c : text) { if (c == ' ') { if (!cur.empty()) ws.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) ws.push_back(cur);
    int64_t ms = n - static_cast<int64_t>(ws.size()) * 200;
    t.onsetMs   = ms;
    for (auto& w : ws) { SWord sw; sw.text = w; sw.startMs = ms; sw.endMs = ms + 150; t.words.push_back(sw); ms += 200; }
    t.endMs = ms;

    const int64_t t0 = Clock::monotonicMs();
    auto r = IntentPipeline::process(t, cfg, defaultHyprctlQuery, defaultGazeQuery, defaultRunner, backend);
    Log::log(Log::INFO, "intent resolved in {}ms ({} backend)", Clock::monotonicMs() - t0,
             backend ? "llama" : "rule");
    return r.action.actionable() ? 0 : 3;
}

// The in-headset HUD is no longer rendered here (WP-H8): hypxrvoice pushes panels to the
// shared `hypxrhud` daemon, which owns the OpenXR overlay + rendering. For an offline
// review of the HUD's pixels use hypxrhud's `--preview` composite.

int main(int argc, char** argv) {
    std::string configPath = defaultConfigPath();
    std::string oneshot;
    std::string modelOverride;
    std::string intentText;
    bool        oneshotIntent = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto        needVal = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                Log::log(Log::ERR, "{} requires a value", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (a == "--config") {
            configPath = needVal("--config");
        } else if (a == "--oneshot") {
            oneshot = needVal("--oneshot");
        } else if (a == "--intent") {
            oneshotIntent = true;
        } else if (a == "--intent-text") {
            intentText = needVal("--intent-text");
        } else if (a == "--model") {
            modelOverride = needVal("--model");
        } else if (a == "--verbose") {
            Log::setLevel(Log::DEBUG);
        } else {
            Log::log(Log::ERR, "unknown option: {}", a);
            printUsage(argv[0]);
            return 2;
        }
    }

    // Load config early (needed by both paths). Missing file => defaults.
    SConfig                  cfg;
    std::vector<std::string> errors, warnings;
    if (!loadConfigFile(configPath, cfg, errors, warnings)) {
        for (auto& e : errors)
            Log::log(Log::ERR, "config: {}", e);
        return 1;
    }
    for (auto& w : warnings)
        Log::log(Log::DEBUG, "config: {}", w);
    if (!modelOverride.empty())
        cfg.asr.model = modelOverride;

    if (!intentText.empty())
        return runIntentText(intentText, cfg);

    if (!oneshot.empty())
        return runOneshot(oneshot, cfg, oneshotIntent);

    // Daemon mode.
    struct sigaction sa = {};
    sa.sa_handler       = onSignal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    CDaemon     daemon;
    std::string err;
    // Re-point the daemon at the same config (it re-reads and applies model override
    // via the file; --model override only affects oneshot + this process's cfg).
    if (!modelOverride.empty())
        Log::log(Log::WARN, "--model override applies to oneshot only; set asr.model in config for the daemon");
    if (!daemon.init(configPath, err)) {
        Log::log(Log::ERR, "daemon init failed: {}", err);
        return 1;
    }
    return daemon.run(g_stop);
}
