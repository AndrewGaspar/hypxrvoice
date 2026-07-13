#pragma once

#include <cstdint>
#include <functional>
#include <string>

// PipeWire microphone capture, delivering 16 kHz mono float frames. The stream is
// only created while running (start/stop) — this is the process-level privacy
// invariant: when the activation machine says the mic should be closed, we stop()
// the stream and no PCM is pulled at all (observable in pw-top / wpctl).
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

    // Defined in the .cpp; public so the C-style PipeWire callbacks (free functions)
    // can name it.
    struct Impl;

  private:
    Impl*       m_impl = nullptr;
    bool        m_running = false;
    std::string m_source;
};
