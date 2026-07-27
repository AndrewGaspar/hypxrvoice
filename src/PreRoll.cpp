#include "PreRoll.hpp"

#include <algorithm>

namespace {
    // How far the reported capture clock may drift from the ring's own sample count
    // before the retained audio is treated as non-contiguous. Generous enough to absorb
    // ordinary PipeWire buffer jitter, tight enough to catch a real stream restart.
    constexpr int64_t kGapToleranceMs = 120;
}

void CPreRollRing::configure(int sampleRate, int ms) {
    m_sampleRate = sampleRate > 0 ? sampleRate : 16000;
    m_capacity   = ms > 0 ? static_cast<size_t>(m_sampleRate) * static_cast<size_t>(ms) / 1000 : 0;
    clear();
}

void CPreRollRing::clear() {
    m_buf.clear();
    m_oldestIdx = m_nextIdx;
    m_anchorIdx = m_nextIdx;
    m_anchorMs  = 0;
}

int64_t CPreRollRing::startMonoMs() const {
    if (m_buf.empty())
        return 0;
    return m_anchorMs + ((m_oldestIdx - m_anchorIdx) * 1000) / m_sampleRate;
}

int CPreRollRing::bufferedMs() const {
    return static_cast<int>((m_buf.size() * 1000) / static_cast<size_t>(m_sampleRate));
}

void CPreRollRing::push(const float* frames, size_t n, int64_t monoMs) {
    if (m_capacity == 0 || n == 0 || !frames)
        return;

    if (!m_buf.empty()) {
        // Where the capture clock SHOULD be if the retained audio is contiguous.
        const int64_t expected = m_anchorMs + ((m_nextIdx - m_anchorIdx) * 1000) / m_sampleRate;
        if (monoMs < expected - kGapToleranceMs || monoMs > expected + kGapToleranceMs)
            clear(); // stream restart / xrun: drop the stale audio, re-anchor below.
    }

    if (m_buf.empty()) {
        m_anchorMs  = monoMs;
        m_anchorIdx = m_nextIdx;
        m_oldestIdx = m_nextIdx;
    }

    m_buf.insert(m_buf.end(), frames, frames + n);
    m_nextIdx += static_cast<int64_t>(n);

    if (m_buf.size() > m_capacity) {
        const size_t drop = m_buf.size() - m_capacity;
        m_buf.erase(m_buf.begin(), m_buf.begin() + static_cast<long>(drop));
        m_oldestIdx += static_cast<int64_t>(drop);
    }
}

size_t CPreRollRing::drain(std::vector<float>& out, int64_t& startMonoMs) {
    if (m_buf.empty())
        return 0;
    const size_t n = m_buf.size();
    startMonoMs    = this->startMonoMs();
    out.insert(out.end(), m_buf.begin(), m_buf.end());
    clear();
    return n;
}
