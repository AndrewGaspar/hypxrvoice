#pragma once

#include <map>
#include <string>
#include <vector>

// Activation policy. Mirrors the compositor's hand_input=auto idea (locked
// decision #2): at the keyboard -> push-to-talk only; away / headset donned ->
// wake word armed.
enum class EActivationMode {
    Auto, // follow the compositor presence + at-keyboard signal
    Ptt,  // push-to-talk only, never auto-arm the wake word (maximally private)
    Wake, // always wake-word armed, ignore the compositor signal
};

// When the compositor / openxr status is unavailable, which base gate to fall
// back to. Task directive: degrade gracefully to wake-word-armed.
enum class EFallback {
    Wake,
    Gate,
};

struct SConfig {
    struct {
        EActivationMode mode           = EActivationMode::Auto;
        EFallback       fallback       = EFallback::Wake;
        int             keyboardIdleMs = 3000; // local at-keyboard heuristic window
    } activation;

    struct {
        std::string source;       // empty => PipeWire default source
        std::string headsetSource = "wivrn.source"; // preferred when headset present
        int         sampleRate    = 16000;          // whisper wants 16 kHz mono
    } audio;

    struct {
        float energyThreshold = 0.012f; // RMS above this = voiced
        int   startMs         = 150;    // sustained voiced to declare onset
        int   endMs           = 600;    // sustained silence (hangover) to end
        int   maxUtteranceMs  = 12000;
        int   preRollMs       = 300;    // audio retained before the onset point
    } vad;

    struct {
        bool        enabled = true;
        std::string backend = "vad-transcribe"; // or "openwakeword" (seam, WP-V7)
        std::string phrase  = "hey hypr";
        int         fuzz    = 2; // max Levenshtein distance for the phrase match
    } wake;

    struct {
        std::string model;          // whisper ggml model path — REQUIRED to run
        std::string language = "en";
        int         threads  = 4;
        bool        translate = false;
    } asr;

    struct {
        bool stdoutJson = true;
        bool notify     = true; // notify-send toasts
    } feedback;

    struct {
        bool enabled = true;
        int  pollMs  = 1000;
    } compositor;

    // WP-V4 intent tier: transcript -> typed command.
    struct {
        bool        enabled = true;
        std::string backend = "rule";  // "rule" (deterministic) | "llama" (local GGUF).
        std::string model;             // GGUF path for the llama backend (required for it).
        double      temperature = 0.0; // llama sampling temperature (0 = greedy).
        int         nThreads    = 4;   // llama decode threads.
        int         contextMaxMonitors = 16; // digest caps (prompt-size bound).
        int         contextMaxApps     = 6;
        int         deixisWindowMs = 300; // gaze stability window.
        int         deixisLeadMs   = 200; // gaze-leads-speech shift.
        int         deixisSamples  = 5;
        double      distanceStep   = 0.25; // "closer"/"further" step (m).
    } intent;

    // WP-V4 executor: typed command -> allowlisted hyprctl argv.
    struct {
        bool dryRun         = true;  // DEFAULT: log argv, actuate nothing.
        bool allowXrmonitor = true;  // permit openxr/xrmonitor actuation.
        bool allowLaunch    = false; // permit app launch (allowlisted only).
        bool targetedGrab   = false; // compositor advertises `gazegrab <name>` (GAP).
        bool placeAtPose    = false; // compositor advertises `place <name> at …` (GAP).
    } executor;

    // [apps] allowlist: spoken app key -> a TRUSTED launch command (uwsm/systemd-run).
    // Never derived from transcript text; consumed only by launch_app.
    std::map<std::string, std::string> apps;
};

// Parse a TOML-subset config document. Returns true on success. Unknown keys are
// reported as warnings in `warnings` but do not fail the parse (forward-compat).
// Hard type errors are reported in `errors` and fail the parse. This is a pure
// function over a string so it is directly unit-testable.
bool parseConfig(const std::string& text, SConfig& out, std::vector<std::string>& errors, std::vector<std::string>& warnings);

// Load config from a path. Missing file is NOT an error — defaults are returned
// with a note in `warnings`. Returns false only on a parse error.
bool loadConfigFile(const std::string& path, SConfig& out, std::vector<std::string>& errors, std::vector<std::string>& warnings);

// Default config path: $XDG_CONFIG_HOME/hypxrvoice/config.toml (or ~/.config/...).
std::string defaultConfigPath();
