#include "doctest.h"

#include "ActivationMachine.hpp"

static SEnvSignal env(bool avail, bool present, bool atKbd) {
    SEnvSignal e;
    e.compositorAvailable = avail;
    e.headsetPresent      = present;
    e.atKeyboard          = atKbd;
    return e;
}

TEST_CASE("baseGate: auto mode follows presence + at-keyboard") {
    using M = EActivationMode;
    // Headset donned AND away from keyboard => armed.
    CHECK(baseGate(M::Auto, EFallback::Wake, env(true, true, false)) == EState::ArmedWakeword);
    // Donned but typing => gated (PTT-only).
    CHECK(baseGate(M::Auto, EFallback::Wake, env(true, true, true)) == EState::GatedKeyboard);
    // Not donned => gated regardless of keyboard.
    CHECK(baseGate(M::Auto, EFallback::Wake, env(true, false, false)) == EState::GatedKeyboard);
}

TEST_CASE("baseGate: compositor-absent fallback") {
    using M = EActivationMode;
    // Task directive: degrade to wake-armed when the compositor/openxr is absent.
    CHECK(baseGate(M::Auto, EFallback::Wake, env(false, false, true)) == EState::ArmedWakeword);
    // ...unless the user configured a gate-fallback.
    CHECK(baseGate(M::Auto, EFallback::Gate, env(false, false, true)) == EState::GatedKeyboard);
}

TEST_CASE("baseGate: forced modes ignore the environment") {
    using M = EActivationMode;
    CHECK(baseGate(M::Ptt, EFallback::Wake, env(true, true, false)) == EState::GatedKeyboard);
    CHECK(baseGate(M::Wake, EFallback::Gate, env(true, true, true)) == EState::ArmedWakeword);
}

TEST_CASE("machine: PTT overrides the gate and drives the mic") {
    CActivationMachine m;
    m.configure(EActivationMode::Auto, EFallback::Wake);
    m.onEnv(env(true, true, true)); // at keyboard => gated
    CHECK(m.state() == EState::GatedKeyboard);
    CHECK_FALSE(m.micShouldBeOpen()); // mic closed while gated

    m.onPttStart();
    CHECK(m.state() == EState::Listening);
    CHECK(m.currentActivation() == EActivation::PushToTalk);
    CHECK(m.micShouldBeOpen()); // PTT opens the mic even at the keyboard

    m.onSpeechEnd();
    CHECK(m.state() == EState::Transcribing);
    CHECK_FALSE(m.micShouldBeOpen()); // decoding: mic closed

    m.onTranscribeDone();
    CHECK(m.state() == EState::GatedKeyboard); // back to the gate
}

TEST_CASE("machine: wake trigger only fires when armed") {
    CActivationMachine m;
    m.configure(EActivationMode::Auto, EFallback::Wake);

    m.onEnv(env(true, true, true)); // gated
    CHECK(m.onWakeDetected() == EState::GatedKeyboard); // ignored while gated

    m.onEnv(env(true, true, false)); // armed
    CHECK(m.state() == EState::ArmedWakeword);
    CHECK(m.micShouldBeOpen());
    CHECK(m.onWakeDetected() == EState::Listening);
    CHECK(m.currentActivation() == EActivation::WakeWord);
}

TEST_CASE("machine: env change never interrupts an in-flight utterance") {
    CActivationMachine m;
    m.configure(EActivationMode::Auto, EFallback::Wake);
    m.onEnv(env(true, true, false)); // armed
    m.onWakeDetected();              // Listening
    CHECK(m.state() == EState::Listening);
    // User sits back down at the keyboard mid-utterance.
    m.onEnv(env(true, true, true));
    CHECK(m.state() == EState::Listening); // not yanked away
    m.onSpeechEnd();
    m.onTranscribeDone();
    CHECK(m.state() == EState::GatedKeyboard); // new gate applied on settle
}

TEST_CASE("machine: listen timeout returns to the base gate") {
    CActivationMachine m;
    m.configure(EActivationMode::Auto, EFallback::Wake);
    m.onEnv(env(true, true, false));
    m.onWakeDetected();
    CHECK(m.state() == EState::Listening);
    m.onListenTimeout();
    CHECK(m.state() == EState::ArmedWakeword);
}
