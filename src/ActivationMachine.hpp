#pragma once

#include "Config.hpp"
#include "Transcript.hpp"

// Pure activation state machine (WP-V2 core). No audio, no sockets, no I/O — it is
// driven entirely by events and an environment signal, so it is fully unit-testable.
// The daemon feeds it PTT commands, wake-word hits, VAD speech-end, ASR completion,
// and ~1 Hz compositor status; it answers with the current state and, crucially,
// whether the microphone PCM stream should be open (the process-level privacy
// invariant from research VOICE-CONTROL.md §6).
enum class EState {
    GatedKeyboard,  // at the keyboard: PTT-only, mic closed until PTT
    ArmedWakeword,  // away / headset donned: wake word listening, mic open
    Listening,      // actively capturing an utterance (PTT held or wake fired)
    Transcribing,   // running ASR on the captured segment
};

inline const char* stateName(EState s) {
    switch (s) {
        case EState::GatedKeyboard: return "GATED_KEYBOARD";
        case EState::ArmedWakeword: return "ARMED_WAKEWORD";
        case EState::Listening:     return "LISTENING";
        case EState::Transcribing:  return "TRANSCRIBING";
    }
    return "?";
}

// The environment signal, normally sourced from `hyprctl openxr status -j` at ~1 Hz.
struct SEnvSignal {
    bool compositorAvailable = false; // could we read openxr status at all?
    bool headsetPresent      = false; // userPresence / session visible
    bool atKeyboard          = true;  // recent keyboard activity / hand_input=at-keyboard
};

// The base gate (Gated vs Armed) implied by mode + fallback + environment. Exposed
// as a free function because it is the single most important policy decision and is
// heavily unit-tested on its own.
EState baseGate(EActivationMode mode, EFallback fallback, const SEnvSignal& env);

class CActivationMachine {
  public:
    CActivationMachine() = default;
    void configure(EActivationMode mode, EFallback fallback);

    // Event inputs. Each returns the (possibly unchanged) resulting state.
    EState onEnv(const SEnvSignal& env);   // ~1 Hz compositor poll
    EState onPttStart();                   // control socket `ptt start`
    EState onPttStop();                    // control socket `ptt stop`
    EState onWakeDetected();               // wake-word tier fired
    EState onSpeechEnd();                  // VAD detected end-of-utterance
    EState onListenTimeout();              // Listening expired with no speech
    EState onTranscribeDone();             // ASR finished

    EState      state() const { return m_state; }
    EActivation currentActivation() const { return m_activation; }
    bool        pttHeld() const { return m_pttHeld; }

    // The privacy invariant: is the PCM stream permitted to be open right now?
    bool micShouldBeOpen() const { return m_state == EState::ArmedWakeword || m_state == EState::Listening; }

  private:
    EState recomputeBase();

    EActivationMode m_mode     = EActivationMode::Auto;
    EFallback       m_fallback = EFallback::Wake;
    SEnvSignal      m_env{};
    EState          m_state      = EState::GatedKeyboard;
    EActivation     m_activation = EActivation::PushToTalk;
    bool            m_pttHeld    = false;
};
