#include "AudioDump.hpp"
#include "Log.hpp"
#include "Wav.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace {
    // Hard memory bound on one window's buffered audio. The PTT safety deadline caps a
    // window near max_utterance_ms + 5 s, so 60 s is far past any legitimate window and
    // exists only so a stuck gate cannot grow the buffer without limit.
    constexpr int kMaxWindowSec = 60;

    // Same tolerance the pre-roll ring uses for "is this audio still contiguous?".
    constexpr int64_t kGapToleranceMs = 120;

    // "20260726-143052.417" — sorts chronologically as a string, which is what the
    // retention prune relies on.
    std::string wallStamp() {
        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        std::tm    tm{};
        std::time_t secs = ts.tv_sec;
        localtime_r(&secs, &tm);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d.%03d", tm.tm_year + 1900,
                      tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                      static_cast<int>(ts.tv_nsec / 1'000'000));
        return buf;
    }

    // Recover the window stem from one of our filenames, or "" if it is not ours.
    std::string stemOf(const std::string& name) {
        static const char* kSuffix[] = {"-window.wav", ".txt"};
        for (auto* s : kSuffix) {
            const size_t n = std::string(s).size();
            if (name.size() > n && name.compare(name.size() - n, n, s) == 0)
                return name.substr(0, name.size() - n);
        }
        if (const size_t p = name.rfind("-seg"); p != std::string::npos && name.size() > p + 4)
            return name.substr(0, p);
        return "";
    }

    int64_t samplesToMs(size_t n, int sampleRate) {
        if (sampleRate <= 0)
            return 0;
        return static_cast<int64_t>(n) * 1000 / sampleRate;
    }
}

bool CAudioDump::configure(const std::string& dir, int keepWindows, int sampleRate, std::string& err) {
    m_dir.clear();
    m_open = false;
    m_window.clear();
    m_window.shrink_to_fit();
    m_notes.clear();
    m_sampleRate = sampleRate > 0 ? sampleRate : 16000;
    m_keep       = keepWindows > 0 ? keepWindows : 1;

    if (dir.empty())
        return true; // disabled — nothing created, nothing written.

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec && !fs::is_directory(dir)) {
        err = "debug.dump_audio_dir '" + dir + "' is not usable: " + ec.message();
        return false;
    }
    m_dir = dir;
    return true;
}

std::string CAudioDump::stemPath(const std::string& suffix) const {
    return (fs::path(m_dir) / (m_stem + suffix)).string();
}

// Keep the newest m_keep windows. Stems carry a leading wall-clock stamp, so
// lexicographic order IS chronological order.
void CAudioDump::pruneOldWindows() {
    std::error_code ec;
    std::set<std::string> stems;
    for (const auto& e : fs::directory_iterator(m_dir, ec)) {
        if (ec)
            return;
        std::string stem = stemOf(e.path().filename().string());
        if (!stem.empty())
            stems.insert(stem);
    }
    if (static_cast<int>(stems.size()) <= m_keep)
        return;
    // std::set is sorted ascending; everything before the last m_keep is expendable.
    size_t drop = stems.size() - static_cast<size_t>(m_keep);
    std::set<std::string> doomed(stems.begin(), std::next(stems.begin(), static_cast<long>(drop)));
    for (const auto& e : fs::directory_iterator(m_dir, ec)) {
        if (ec)
            return;
        std::string stem = stemOf(e.path().filename().string());
        if (!stem.empty() && doomed.count(stem))
            fs::remove(e.path(), ec);
    }
}

void CAudioDump::beginWindow(int64_t openedMonoMs) {
    if (!enabled())
        return;
    // A window left open (no gate-close seen) still gets flushed rather than lost.
    if (m_open)
        endWindow("superseded", "");

    m_stem = wallStamp() + "-w" + std::to_string(openedMonoMs);
    pruneOldWindows();

    m_open           = true;
    m_openedMonoMs   = openedMonoMs;
    m_startMonoMs    = 0;
    m_haveStart      = false;
    m_nextMonoMs     = 0;
    m_preRollSamples = 0;
    m_preRollStartMs = 0;
    m_truncated      = false;
    m_segments       = 0;
    m_window.clear();
    m_notes.clear();
}

void CAudioDump::notePreRoll(const float* samples, size_t n, int64_t startMonoMs) {
    if (!enabled() || !m_open || !samples || n == 0)
        return;
    m_preRollSamples = n;
    m_preRollStartMs = startMonoMs;
    appendWindow(samples, n, startMonoMs);
}

void CAudioDump::appendWindow(const float* samples, size_t n, int64_t monoMs) {
    if (!enabled() || !m_open || !samples || n == 0)
        return;

    if (!m_haveStart) {
        m_startMonoMs = monoMs;
        m_haveStart   = true;
    } else if (monoMs < m_nextMonoMs - kGapToleranceMs || monoMs > m_nextMonoMs + kGapToleranceMs) {
        // Concatenate anyway — but say so, so nobody reads the wav as one clock.
        char buf[160];
        std::snprintf(buf, sizeof(buf), "gap at_sample=%zu expected_mono_ms=%lld got_mono_ms=%lld",
                      m_window.size(), static_cast<long long>(m_nextMonoMs),
                      static_cast<long long>(monoMs));
        m_notes.emplace_back(buf);
    }
    m_nextMonoMs = monoMs + samplesToMs(n, m_sampleRate);

    const size_t cap = static_cast<size_t>(kMaxWindowSec) * static_cast<size_t>(m_sampleRate);
    if (m_window.size() >= cap) {
        m_truncated = true;
        return;
    }
    m_window.insert(m_window.end(), samples, samples + std::min(n, cap - m_window.size()));
    if (m_window.size() >= cap)
        m_truncated = true;
}

void CAudioDump::noteSegment(const SSpeechSegment& seg) {
    if (!enabled() || !m_open)
        return;

    const int   idx  = m_segments++;
    std::string name = m_stem + "-seg" + std::to_string(idx) + ".wav";
    std::string err;
    if (!writeWavMono16(stemPath("-seg" + std::to_string(idx) + ".wav"), seg.samples, m_sampleRate, err))
        Log::log(Log::WARN, "audio dump: {}", err);

    // Where this segment starts inside the window wav, so the two can be aligned by eye.
    // -1 means the segment predates the window buffer (only possible across a gap).
    long long offSamples = -1;
    if (m_haveStart && seg.bufferStartMs >= m_startMonoMs)
        offSamples = static_cast<long long>((seg.bufferStartMs - m_startMonoMs) *
                                            static_cast<int64_t>(m_sampleRate) / 1000);

    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "segment %d file=%s onset_mono_ms=%lld end_mono_ms=%lld "
                  "buffer_start_mono_ms=%lld backpad_ms=%lld samples=%zu ms=%lld "
                  "window_offset_samples=%lld",
                  idx, name.c_str(), static_cast<long long>(seg.onsetMs),
                  static_cast<long long>(seg.endMs), static_cast<long long>(seg.bufferStartMs),
                  static_cast<long long>(seg.backpadMs()), seg.samples.size(),
                  static_cast<long long>(samplesToMs(seg.samples.size(), m_sampleRate)), offSamples);
    m_notes.emplace_back(buf);
}

void CAudioDump::endWindow(const std::string& outcome, const std::string& transcript) {
    if (!enabled() || !m_open)
        return;
    m_open = false;

    std::string err;
    if (!writeWavMono16(stemPath("-window.wav"), m_window, m_sampleRate, err))
        Log::log(Log::WARN, "audio dump: {}", err);

    std::ofstream f(stemPath(".txt"), std::ios::trunc);
    if (!f) {
        Log::log(Log::WARN, "audio dump: cannot write sidecar {}", stemPath(".txt"));
        m_window.clear();
        m_notes.clear();
        return;
    }
    f << "# hypxrvoice capture-window dump\n";
    f << "stem " << m_stem << "\n";
    f << "sample_rate " << m_sampleRate << "\n";
    f << "window_opened_mono_ms " << m_openedMonoMs << "\n";
    f << "window_start_mono_ms " << (m_haveStart ? m_startMonoMs : m_openedMonoMs) << "\n";
    f << "window_samples " << m_window.size() << "\n";
    f << "window_ms " << samplesToMs(m_window.size(), m_sampleRate) << "\n";
    f << "truncated " << (m_truncated ? 1 : 0) << "\n";
    // The splice point: everything before it came from the pre-roll ring (audio captured
    // BEFORE the window opened); everything at or after it arrived live.
    f << "preroll_samples " << m_preRollSamples << "\n";
    f << "preroll_ms " << samplesToMs(m_preRollSamples, m_sampleRate) << "\n";
    f << "preroll_start_mono_ms " << m_preRollStartMs << "\n";
    f << "splice_offset_samples " << m_preRollSamples << "\n";
    f << "splice_offset_ms " << samplesToMs(m_preRollSamples, m_sampleRate) << "\n";
    f << "vad_start_ms " << m_vad.startMs << "\n";
    f << "vad_end_ms " << m_vad.endMs << "\n";
    f << "vad_onset_backpad_ms " << m_vad.onsetBackpadMs << "\n";
    f << "vad_pre_roll_ms " << m_vad.preRollMs << "\n";
    f << "vad_adaptive " << (m_vad.adaptive ? 1 : 0) << "\n";
    f << "segments " << m_segments << "\n";
    for (const auto& n : m_notes)
        f << n << "\n";
    f << "outcome " << (outcome.empty() ? "unknown" : outcome) << "\n";
    f << "transcript " << transcript << "\n";
    f.close();

    Log::log(Log::DEBUG, "audio dump: wrote {} ({} segments, {}ms window)", m_stem, m_segments,
             samplesToMs(m_window.size(), m_sampleRate));
    m_window.clear();
    m_window.shrink_to_fit();
    m_notes.clear();
}
