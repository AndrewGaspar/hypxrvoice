#pragma once

#include <cstdint>
#include <functional>
#include <string>

// PipeWire microphone capture, delivering 16 kHz mono float frames.
//
// PRIVACY (revised in WP-V6): the stream is created only while the daemon wants capture
// at all, and stop() genuinely tears it down (observable in pw-top / wpctl). What the
// daemon changed is WHEN it wants capture: a headset session now HOLDS the stream across
// windows, because re-opening it cost ~0.5–1 s of the user's first words (see PreRoll.hpp).
// The gate that decides whether PCM reaches the VAD/ASR/wake tiers moved up into the
// daemon (CDaemon::captureIsActive) — while it is closed the frames go nowhere but a
// sub-second pre-roll ring.
//
// SEAM (WP-V2 follow-up): the research doc calls for a pipewire-module-echo-cancel
// (WebRTC AEC) node between the raw source and this capture, so wake/ASR don't
// trigger on played-back desktop/TTS audio. That is a graph-wiring concern (load the
// module, target its virtual source); this class captures whatever source it is
// pointed at, so pointing `audio.source` at the echo-cancel source is all that is
// needed. AEC wiring itself is left to WP-V5 (TTS) when self-hearing first matters.
class CAudioCapture {
  public:
    // frames are mono float [-1,1]; monoMs is the CLOCK_MONOTONIC time of frame[0].
    using Callback = std::function<void(const float* frames, size_t n, int64_t monoMs)>;

    CAudioCapture() = default;
    ~CAudioCapture();

    // `source` is a PipeWire node name/target (empty => default source). Returns
    // false if the stream could not be created. Idempotent if already running with
    // the same source.
    bool start(const std::string& source, int sampleRate, Callback cb);
    void stop();
    bool running() const { return m_running; }
    const std::string& source() const { return m_source; }

    // The live PipeWire stream state, as a short name ("streaming", "error", …).
    // "closed" when no stream exists. Surfaced in `hypxrvoicectl status`.
    const char* stateName() const;

    // True once a started stream can no longer deliver PCM: PipeWire reported an error,
    // or the stream fell back to UNCONNECTED after having been connected (what a
    // disappearing `wivrn.source` looks like when the headset drops). The daemon
    // reconnects with backoff rather than spinning on it.
    bool failed() const;

    // Defined in the .cpp; public so the C-style PipeWire callbacks (free functions)
    // can name it.
    struct Impl;

  private:
    Impl*       m_impl = nullptr;
    bool        m_running = false;
    std::string m_source;
};
