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

    // Capture-stream lifetime (WP-V6). Opening the stream per PTT window cost ~0.5–1 s
    // (PipeWire connect + `wivrn.source` resume from SUSPENDED, which asks the headset
    // client to start its mic over the network) and swallowed the first word of every
    // utterance. In a headset session the stream is therefore HELD open, with gated
    // frames going nowhere but a rolling pre-roll ring that is spliced onto the front of
    // the next window. See PreRoll.hpp for the privacy semantics and README for the
    // battery trade-off (the headset mic keeps streaming while held).
    struct {
        bool hold      = true; // keep the stream connected across windows (headset only)
        int  preRollMs = 1000; // rolling pre-roll spliced in front of a new window; 0 = off
        // Transcribe the WHOLE push-to-talk window instead of the VAD's slice of it. A
        // PTT press is an explicit declaration that speech is coming, so nothing about
        // what reaches whisper is gated on onset detection; the VAD is demoted to early
        // endpointing plus a forgiving no-speech verdict. See PttWindow.hpp for the two
        // round-3 live failures this closes. Off restores the pre-round-3 behaviour.
        bool pttWholeWindow = true;
    } capture;

    struct {
        // RMS floor: a frame below this is always silence. Lowered from 0.012 in round 3
        // — the Quest/WiVRn mic is unprocessed, its measured ambient is ~0.0002 and its
        // speech peaks only ~0.03, so 0.012 gated out whole quiet words. See
        // SVadConfig::energyThreshold for the per-window numbers.
        float energyThreshold = 0.006f;
        int   startMs         = 150;    // sustained voiced to declare onset
        int   endMs           = 600;    // sustained silence (hangover) to end
        int   maxUtteranceMs  = 12000;
        int   preRollMs       = 300;    // raw depth of the VAD's idle retention ring
        // How long an unvoiced dip inside the onset run may last before the run resets.
        // Words have internal stops; demanding start_ms of CONSECUTIVE voiced frames made
        // short choppy words ("focus", "workspace") structurally undetectable.
        int   gapToleranceMs  = 100;
        // Energetic audio anywhere in a PTT window that counts as "somebody spoke".
        int   presenceMs      = 100;
        // Audio GUARANTEED to precede the declared onset instant. Not the same thing as
        // pre_roll_ms — see SVadConfig::onsetBackpadMs for why the old key delivered only
        // (pre_roll_ms - start_ms) and structurally lost quiet first syllables.
        int   onsetBackpadMs  = 300;
        // Noise-floor-adaptive gating (survives a hot AGC source like the WiVRn mic).
        bool  adaptive         = true;
        float noiseFloorFactor = 1.6f;  // voiced threshold = max(energy_threshold, floor*factor)
        int   noiseWindowMs    = 1500;  // rolling window for the noise-floor percentile
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

        // WP-H8 in-headset HUD. hypxrvoice is now a pure D-Bus CLIENT of the shared
        // `hypxrhud` daemon (io.github.andrewgaspar.hypxrhud1): it pushes a panel to a
        // named slot; hypxrhud owns the OpenXR overlay session, rendering, geometry, and
        // fades. Degrades to notify-send when the daemon is absent or its RuntimeState is
        // not "live". DEFAULT OFF so unit tests / offline tools never touch the bus.
        //
        // MIGRATED to hypxrhud config (was feedback.hud_* here): geometry (pose/size),
        // opacity, hold/fade envelope, overlay z, and the DRM gpu are hypxrhud's
        // [hud]/[slot.<name>] keys now — see $XDG_CONFIG_HOME/hypxrhud/hypxrhud.toml.
        bool        hud     = false;
        std::string hudSlot = "voice"; // hypxrhud slot the voice panel targets (config override).

        // WP-V5 terse TTS. Spoken confirmations for things you can't see on the HUD
        // (errors, clarify). "off" | "errors" (errors+clarify) | "all". espeak-ng
        // binary on PATH; cleanly disabled if absent.
        std::string ttsMode  = "errors";
        std::string ttsVoice;             // espeak-ng voice, empty = its default.
        int         ttsRate  = 175;       // words per minute.
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
        // Where "here" is. A deixis gives a head ORIGIN and a direction; the point the
        // word designates is projected this far along the gaze ray (unless the
        // compositor reports a real ray/monitor intersection). placeMinDistanceM is a
        // hard floor so a monitor can never land inside the wearer's head.
        double      placeDistanceM    = 1.3;
        double      placeMinDistanceM = 0.5;
    } intent;

    // WP-V4 executor: typed command -> allowlisted hyprctl argv.
    struct {
        bool dryRun         = true;  // DEFAULT: log argv, actuate nothing.
        bool allowXrmonitor = true;  // permit openxr/xrmonitor actuation.
        bool allowLaunch    = false; // permit app launch (allowlisted only).
        bool targetedGrab   = false; // compositor advertises `gazegrab <name>` (GAP).
        bool placeAtPose    = false; // compositor advertises `place <name> at …` (GAP).
        // Plain window management: focus a live window, toggle fullscreen, switch
        // workspace. Non-destructive and reversible (nothing here can close, kill, or
        // move a window), so it is on by default — executor.dry_run still gates it.
        bool allowWindow    = true;
        // "create a monitor here" -> `hyprctl openxr create XR-<n> <WxH@Hz>` (+ a place
        // step when the utterance carried a deixis). Additive and undoable.
        bool allowCreateMonitor = true;
    } executor;

    // Opt-in capture forensics. OFF unless dumpAudioDir is set; when it is, every
    // capture window writes the FULL window audio (pre-roll splice included) plus each
    // segment handed to whisper, with a sidecar recording the splice point and the VAD
    // boundaries. This is the discriminator between "the source never sent the first
    // word" and "our segmenter cut it" — see AudioDump.hpp.
    //
    // PRIVACY: setting this writes microphone audio to disk. It exists for a debugging
    // session and should be unset again afterwards; the daemon logs a WARN at startup
    // for as long as it is on.
    struct {
        std::string dumpAudioDir;      // empty (default) = no dumping, nothing touched
        int         dumpAudioKeep = 50; // windows retained on disk; older ones are pruned
    } debug;

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
