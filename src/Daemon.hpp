#pragma once

#include "ActivationMachine.hpp"
#include "Asr.hpp"
#include "AudioCapture.hpp"
#include "AudioDump.hpp"
#include "Compositor.hpp"
#include "Config.hpp"
#include "ControlSocket.hpp"
#include "LlamaIntent.hpp"
#include "PreRoll.hpp"
#include "PttWindow.hpp"
#include "Vad.hpp"
#include "VocabBias.hpp"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// The long-lived hypxrvoiced process. Single-threaded state model: everything
// (state machine, VAD, ASR, control handling, compositor poll) runs on the main
// epoll thread. Only PipeWire's RT callback runs elsewhere, and it merely copies
// samples into a queue and wakes the loop via an eventfd. This keeps the activation
// state authoritative in one place — no cross-thread state races.
class CDaemon {
  public:
    CDaemon() = default;
    ~CDaemon();

    bool init(const std::string& configPath, std::string& err);
    int  run(std::atomic<bool>& stop); // blocks until stop

  private:
    // ---- audio path ----
    struct SChunk {
        std::vector<float> samples;
        int64_t            monoMs = 0;
    };
    void onAudio(const float* frames, size_t n, int64_t monoMs); // PW thread
    void drainAudio();                                           // main thread
    void handleSegment(const SSpeechSegment& seg);

    // ---- mic gating ----
    //
    // Two independent decisions (see PreRoll.hpp for why):
    //   captureShouldBeHeld() — is the PipeWire STREAM connected? Held across windows in
    //                           a headset session, so a PTT press never pays the ~1 s
    //                           connect + `wivrn.source` resume that ate the first word.
    //   captureIsActive()     — do the frames reach the VAD/ASR/wake tiers? Only inside a
    //                           real window. While it is false the frames go nowhere but
    //                           the pre-roll ring.
    void applyMicPolicy();      // reconcile both against the state machine + environment
    bool captureShouldBeHeld() const;
    bool captureIsActive() const { return m_captureActive; }
    // Is a PUSH-TO-TALK window open right now? (The machine is in Listening and the
    // activation that opened it was a press, not the wake word.)
    bool pttListening() const;
    void startCapture(const std::string& source); // (re)connect, with failure backoff
    void openCaptureWindow();   // gate opens: fresh VAD seeded with the pre-roll splice
    std::string chooseSource() const;
    void updateListeningHud();  // show "listening…" only while actually capturing
    void noteWindowResult(const char* outcome); // this window has shown the user something

    void closePttWindow();      // flush + settle + disarm the PTT deadline (single path)

    // Hand the WHOLE open PTT window to the transcript tier (see PttWindow.hpp). Runs at
    // most once per window; a window with no speech in it is left for closePttWindow to
    // report as `no-speech`. `why` is logged so the journal says which trigger fired.
    void finalizePttUtterance(const char* why);

    // ---- periodic ----
    void tick(); // ~4 Hz: compositor poll cadence + listen timeout

    // ---- control ----
    std::string handleControl(const std::string& cmd);
    std::string statusJson() const;
    std::string doReload();

    void resetVad();
    // WP-V7: (re)build the ASR vocabulary bias from a LIVE desktop snapshot and install
    // it on the ASR. Called at capture-window open — before a word has been spoken, so
    // the three hyprctl reads are off the transcription critical path — and lazily from
    // handleSegment when the cached prompt has aged past asr.vocab_bias_refresh_ms (the
    // wake-word path has no window-open event to hang a rebuild on). `force` ignores the
    // TTL. Main thread only; read-only queries, no compositor mutation.
    void refreshVocabBias(bool force);
    void loadIntentBackend(); // (re)load the llama backend per config; rule fallback.
    void applyDumpConfig();   // (re)apply debug.dump_audio_dir; warns while it is on.

    SConfig            m_cfg;
    std::string        m_configPath;
    CActivationMachine m_machine;
    CAudioCapture      m_audio;
    CAsr               m_asr;
    CCompositor        m_compositor;
    CControlServer     m_control;

    std::unique_ptr<CVad>         m_vad;
    std::unique_ptr<CLlamaIntent> m_llama; // non-null only when the llama backend is loaded.

    std::mutex         m_qMu;
    std::deque<SChunk> m_queue;

    int m_eventFd = -1;
    int m_timerFd = -1;
    int m_epollFd = -1;

    // Frames arriving while the gate is shut land here and nowhere else; spliced onto
    // the front of the next window so speech that started before the PTT press survives.
    CPreRollRing m_preRoll;

    // Opt-in capture forensics (debug.dump_audio_dir). Disabled — and inert — unless the
    // operator sets a directory; see AudioDump.hpp for what it answers and why.
    CAudioDump m_dump;

    // The whole audio of the open PTT window. Live only while m_pttWholeWindow — the
    // wake-word path is unchanged and still segments on VAD onset.
    CPttWindow m_pttAudio;
    bool       m_pttWholeWindow = false; // this window is transcribed whole
    bool       m_pttSpent       = false; // ...and it already was (no second transcript)
    int64_t    m_pttVadOnsetMs  = 0;     // VAD's onset inside this window, 0 = never fired

    SEnvSignal m_env{};
    int64_t    m_lastPollMs   = 0;
    int64_t    m_pttDeadlineMs = 0; // absolute deadline for the OPEN PTT window (0 = none)
    EState     m_lastState    = EState::GatedKeyboard;
    bool       m_hudListening = false; // we currently own the HUD with a listening panel
    bool       m_hudResultShown = false; // a result panel replaced "listening…" this window
    bool       m_windowProduced = false; // the open PTT window told the user something
    int64_t    m_lastAudioLogMs = 0;   // throttle for the periodic capture-stats DEBUG log
    int64_t    m_vocabBiasBuiltMs = 0; // when the installed vocabulary bias was built (0 = never)

    // ---- persistent capture bookkeeping ----
    bool    m_captureActive     = false; // frames are reaching the VAD/ASR/wake tiers
    int64_t m_captureRetryAtMs  = 0;     // no (re)connect attempt before this instant
    int     m_captureBackoffMs  = 0;     // current reconnect backoff (doubles to a cap)
    uint64_t m_lastFrameCount   = 0;     // frames seen at m_lastFrameChangeMs
    int64_t m_lastFrameChangeMs = 0;     // last time the frame counter moved (stall watchdog)

    // ---- live audio observability (PW-thread writes, main-thread reads) ----
    std::atomic<uint64_t> m_framesReceived{0};   // total frames delivered by capture
    std::atomic<uint32_t> m_inRms1e4{0};         // rolling input RMS  * 1e4
    std::atomic<uint32_t> m_inPeak1e4{0};        // rolling input peak * 1e4

    // last transcript summary for `status`
    std::string m_lastText;
    // Outcome of the last capture window: "idle" | "ok" | "unparsed" | "no-speech" |
    // "asr-unavailable". Makes a rejected window visible to `hypxrvoicectl status`
    // instead of leaving a stale lastText behind.
    std::string m_lastOutcome = "idle";
    int64_t     m_lastOnsetMs = 0;
    std::string m_lastRawStatus;
    std::string m_lastAction; // last resolved command (verb + target), for `status`
};
