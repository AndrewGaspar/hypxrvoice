#include "PttWindow.hpp"
#include "Log.hpp"

#include <algorithm>
#include <cmath>

namespace {
    // A capture-clock deviation larger than this means the stream was not contiguous.
    // We still concatenate (the words matter more than the drift) but we say so.
    constexpr int64_t kGapToleranceMs = 60;

    // How much silence to KEEP after the last energetic frame. Whisper is happier with a
    // little run-out than with audio that stops mid-breath, and the point of the trim is
    // only to drop the dead seconds a forgotten PTT toggle leaves behind.
    constexpr int64_t kTailKeepMs = 400;

    int64_t samplesToMs(size_t n, int sr) {
        return sr > 0 ? static_cast<int64_t>(n) * 1000 / sr : 0;
    }
}

void CPttWindow::configure(const SVadConfig& vad) {
    m_cfg = vad;
    if (m_cfg.sampleRate <= 0)
        m_cfg.sampleRate = 16000;
    if (m_cfg.frameMs <= 0)
        m_cfg.frameMs = 20;
}

void CPttWindow::begin() {
    m_open      = true;
    m_haveStart = false;
    m_startMs   = 0;
    m_nextMs    = 0;
    m_gapSeen   = false;
    m_buf.clear();
}

void CPttWindow::push(const float* frames, size_t n, int64_t monoMs) {
    if (!m_open || !frames || n == 0)
        return;

    if (!m_haveStart) {
        m_startMs   = monoMs;
        m_haveStart = true;
    } else if (monoMs < m_nextMs - kGapToleranceMs || monoMs > m_nextMs + kGapToleranceMs) {
        m_gapSeen = true;
    }
    m_nextMs = monoMs + samplesToMs(n, m_cfg.sampleRate);

    // Hard ceiling on retention: the PTT deadline already bounds a window, but a stuck
    // client must not be able to grow this without limit. Twice the cap is plenty of
    // headroom for the trim/cap logic below to still have something sensible to work on.
    const size_t retain = static_cast<size_t>(m_cfg.sampleRate) *
                          static_cast<size_t>(std::max(1000, m_cfg.maxUtteranceMs) * 2) / 1000;
    if (m_buf.size() >= retain)
        return;
    m_buf.insert(m_buf.end(), frames, frames + std::min(n, retain - m_buf.size()));
}

int64_t CPttWindow::durationMs() const {
    return samplesToMs(m_buf.size(), m_cfg.sampleRate);
}

SPttUtterance CPttWindow::finish() {
    SPttUtterance u;
    u.samples = std::move(m_buf);
    u.startMs = m_startMs;
    m_buf.clear();
    m_open      = false;
    m_haveStart = false;

    const int sr = m_cfg.sampleRate;
    u.endMs      = u.startMs + samplesToMs(u.samples.size(), sr);
    if (u.samples.empty())
        return u;

    // 1. The verdict, over EVERYTHING we captured (before any trimming can hide the one
    //    quiet burst that proves the user spoke).
    u.presence = detectSpeechPresence(u.samples.data(), u.samples.size(), m_cfg);
    u.speech   = u.presence.found;

    // 2. Trim the obviously-empty tail: everything after the last energetic frame, less
    //    kTailKeepMs of run-out. Only meaningful when something was energetic at all.
    if (u.speech) {
        const size_t frameN = std::max<size_t>(1, static_cast<size_t>(sr) * m_cfg.frameMs / 1000);
        size_t       lastEnd = 0;
        for (size_t off = 0; off + frameN <= u.samples.size(); off += frameN) {
            double acc = 0;
            for (size_t i = 0; i < frameN; i++)
                acc += static_cast<double>(u.samples[off + i]) * u.samples[off + i];
            if (std::sqrt(acc / frameN) >= u.presence.threshold)
                lastEnd = off + frameN;
        }
        const size_t keep = std::min(u.samples.size(),
                                     lastEnd + static_cast<size_t>(sr) * static_cast<size_t>(kTailKeepMs) / 1000);
        if (keep > 0 && keep < u.samples.size()) {
            u.trimmedTailMs = samplesToMs(u.samples.size() - keep, sr);
            u.samples.resize(keep);
            u.endMs = u.startMs + samplesToMs(u.samples.size(), sr);
        }
    }

    // 3. Cap what whisper is asked to chew on. The HEAD is kept: a command's verb is at
    //    the start of the utterance, and a window this long means the user forgot to
    //    close it, not that they spoke for twelve seconds.
    const size_t cap = static_cast<size_t>(sr) * static_cast<size_t>(std::max(1000, m_cfg.maxUtteranceMs)) / 1000;
    if (u.samples.size() > cap) {
        u.samples.resize(cap);
        u.truncated = true;
        u.endMs     = u.startMs + samplesToMs(u.samples.size(), sr);
    }

    if (m_gapSeen)
        Log::log(Log::DEBUG, "ptt window: capture clock was not contiguous — audio concatenated anyway");
    return u;
}
