#pragma once

#include "Vad.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Opt-in capture forensics: write, for every capture window, exactly what the daemon
// heard and exactly what it handed to whisper.
//
// WHY (round-2 live finding): short utterances kept arriving with their LEADING word
// missing ("browser." for "focus the browser", "3." for "workspace three") even after
// the capture stream was held open and a 1 s pre-roll ring was spliced in front of every
// PTT window — i.e. even though, daemon-side, the audio should provably all be there.
// Two very different causes produce that same transcript:
//
//   (a) SOURCE-SIDE. The Quest mic path ramps/gates at utterance onset (AGC settling
//       after `wivrn.source` starts flowing), so the first word was never in the PCM we
//       received. Nothing downstream can recover it; the fix is padding/compensation.
//   (b) OURS. The word IS in the window audio, and the VAD/segmenter cut it off — the
//       segment handed to whisper simply starts too late.
//
// Guessing between those two burned a live round. This writes both artefacts so the
// answer is a waveform, not an argument: if the leading word is inaudible in the
// FULL-WINDOW wav it is (a); if it is audible there but absent from the SEGMENT wav it
// is (b). The sidecar records where the pre-roll splice sits and where each VAD segment
// starts inside the window, so the two line up sample-for-sample.
//
// DEFAULT OFF. With `debug.dump_audio_dir` unset nothing here opens, creates, or writes
// anything. When it is set, microphone audio lands on disk — that is the point, and it
// is why the daemon logs a standing WARN for as long as it is enabled. Disk use is
// bounded by pruning to the most recent `keep` windows.
class CAudioDump {
  public:
    // Enable dumping into `dir` (created if needed), retaining `keepWindows` windows.
    // An empty `dir` disables and is never an error. Returns false with `err` when an
    // enabled directory could not be created.
    bool configure(const std::string& dir, int keepWindows, int sampleRate, std::string& err);

    bool               enabled() const { return !m_dir.empty(); }
    const std::string& dir() const { return m_dir; }

    // ---- one capture window ----------------------------------------------------

    // A window just opened at `openedMonoMs`. Prunes older windows off disk.
    void beginWindow(int64_t openedMonoMs);

    // The pre-roll ring spliced onto the front of this window (call before any live
    // frames; a no-op when the ring was empty).
    void notePreRoll(const float* samples, size_t n, int64_t startMonoMs);

    // Live capture frames, in arrival order, while the gate is open.
    void appendWindow(const float* samples, size_t n, int64_t monoMs);

    // A segment the VAD emitted inside this window: the EXACT buffer handed to ASR.
    // Written immediately (a later crash still leaves the evidence on disk).
    void noteSegment(const SSpeechSegment& seg);

    // Window closed. Writes the full-window wav + the sidecar. Safe to call with no
    // window open, and safe to call twice.
    void endWindow(const std::string& outcome, const std::string& transcript);

    // Carried into the sidecar so a dump is self-describing about the settings that
    // produced it (the numbers that decide whether a quiet onset could survive).
    void noteVadConfig(const SVadConfig& vc) { m_vad = vc; }

  private:
    void        pruneOldWindows();
    std::string stemPath(const std::string& suffix) const;

    std::string m_dir;
    int         m_keep       = 50;
    int         m_sampleRate = 16000;
    SVadConfig  m_vad{};

    // ---- open-window state -----------------------------------------------------
    bool               m_open = false;
    std::string        m_stem;             // basename prefix shared by this window's files
    int64_t            m_openedMonoMs = 0;
    int64_t            m_startMonoMs  = 0; // capture time of m_window[0]
    bool               m_haveStart    = false;
    int64_t            m_nextMonoMs   = 0; // where the capture clock is expected next
    size_t             m_preRollSamples = 0;
    int64_t            m_preRollStartMs = 0;
    bool               m_truncated = false;
    int                m_segments  = 0;
    std::vector<float> m_window;
    std::vector<std::string> m_notes;      // sidecar lines accumulated during the window
};
