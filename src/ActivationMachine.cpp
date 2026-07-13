#include "ActivationMachine.hpp"

EState baseGate(EActivationMode mode, EFallback fallback, const SEnvSignal& env) {
    switch (mode) {
        case EActivationMode::Ptt:
            return EState::GatedKeyboard;
        case EActivationMode::Wake:
            return EState::ArmedWakeword;
        case EActivationMode::Auto:
            if (!env.compositorAvailable)
                return fallback == EFallback::Wake ? EState::ArmedWakeword : EState::GatedKeyboard;
            // Armed only when the headset is on the face AND the user is not at the
            // keyboard — mirrors the compositor's hand_input=auto policy.
            if (env.headsetPresent && !env.atKeyboard)
                return EState::ArmedWakeword;
            return EState::GatedKeyboard;
    }
    return EState::GatedKeyboard;
}

void CActivationMachine::configure(EActivationMode mode, EFallback fallback) {
    m_mode     = mode;
    m_fallback = fallback;
    // Re-settle the base gate if we're currently sitting in one.
    if (m_state == EState::GatedKeyboard || m_state == EState::ArmedWakeword)
        recomputeBase();
}

EState CActivationMachine::recomputeBase() {
    m_state = baseGate(m_mode, m_fallback, m_env);
    return m_state;
}

EState CActivationMachine::onEnv(const SEnvSignal& env) {
    m_env = env;
    // Never yank the mic out from under an in-flight utterance; the new gate is
    // applied when we return to a base state (onTranscribeDone / onListenTimeout).
    if (m_state == EState::GatedKeyboard || m_state == EState::ArmedWakeword)
        return recomputeBase();
    return m_state;
}

EState CActivationMachine::onPttStart() {
    m_pttHeld = true;
    // PTT overrides the gate from any base state: it opens the mic regardless of
    // at-keyboard / presence, and bypasses the wake word (LISTENING directly).
    if (m_state == EState::GatedKeyboard || m_state == EState::ArmedWakeword) {
        m_activation = EActivation::PushToTalk;
        m_state      = EState::Listening;
    }
    return m_state;
}

EState CActivationMachine::onPttStop() {
    m_pttHeld = false;
    // A PTT release finalizes only a PTT-initiated capture. A wake-word capture in
    // progress is ended by VAD, not by the (unrelated) PTT button.
    if (m_state == EState::Listening && m_activation == EActivation::PushToTalk)
        m_state = EState::Transcribing;
    return m_state;
}

EState CActivationMachine::onWakeDetected() {
    if (m_state == EState::ArmedWakeword) {
        m_activation = EActivation::WakeWord;
        m_state      = EState::Listening;
    }
    return m_state;
}

EState CActivationMachine::onSpeechEnd() {
    if (m_state == EState::Listening)
        m_state = EState::Transcribing;
    return m_state;
}

EState CActivationMachine::onListenTimeout() {
    // Listening expired without capturing usable speech: drop back to the base gate.
    if (m_state == EState::Listening)
        return recomputeBase();
    return m_state;
}

EState CActivationMachine::onTranscribeDone() {
    if (m_state == EState::Transcribing)
        return recomputeBase();
    return m_state;
}
