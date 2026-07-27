#include "Vad.hpp"

#include <algorithm>
#include <cmath>

CVad::CVad(const SVadConfig& cfg) : m_cfg(cfg) {
    if (m_cfg.frameMs <= 0)
        m_cfg.frameMs = 20;
    if (m_cfg.sampleRate <= 0)
        m_cfg.sampleRate = 16000;
    if (m_cfg.noiseFloorFactor < 1.f)
        m_cfg.noiseFloorFactor = 1.f;
    m_frameSamples  = static_cast<size_t>(m_cfg.sampleRate) * m_cfg.frameMs / 1000;
    if (m_frameSamples == 0)
        m_frameSamples = 1;

    // Ring depth: deep enough that onsetBackpadMs of audio still precedes the ONSET
    // instant once the startMs voiced run that declares it has been consumed (see
    // SVadConfig::onsetBackpadMs). Rounded UP so the guarantee is met rather than missed
    // by a frame. Always at least one frame — a zero-depth ring would drop the very
    // frame that triggered onset.
    const int backpadMs = std::max(0, m_cfg.onsetBackpadMs);
    const int depthMs   = std::max(std::max(0, m_cfg.preRollMs), backpadMs + std::max(0, m_cfg.startMs));
    m_preRollFrames     = static_cast<size_t>((depthMs + m_cfg.frameMs - 1) / m_cfg.frameMs);
    if (m_preRollFrames == 0)
        m_preRollFrames = 1;

    m_windowFrames  = std::max(1, m_cfg.noiseWindowMs) / std::max(1, m_cfg.frameMs);
    if (m_windowFrames == 0)
        m_windowFrames = 1;
    // Seed the floor at the absolute-minimum threshold; it converges toward the real
    // ambient as the rolling window fills.
    m_noiseFloor = m_cfg.energyThreshold;
}

float CVad::currentThreshold() const {
    if (!m_cfg.adaptive)
        return m_cfg.energyThreshold;
    return std::max(m_cfg.energyThreshold, m_noiseFloor * m_cfg.noiseFloorFactor);
}

static float rms(const float* x, size_t n) {
    if (n == 0)
        return 0.f;
    double acc = 0;
    for (size_t i = 0; i < n; i++)
        acc += static_cast<double>(x[i]) * x[i];
    return static_cast<float>(std::sqrt(acc / n));
}

int CVad::push(const float* samples, size_t n, int64_t frameStartMonoMs, std::vector<SSpeechSegment>& out) {
    if (!m_haveBase) {
        // Anchor the sample->time mapping to the first sample we ever see.
        m_baseMonoMs = frameStartMonoMs;
        m_haveBase   = true;
    }
    const size_t before = out.size();

    // Accumulate into full frames, carrying any remainder across calls.
    m_pending.insert(m_pending.end(), samples, samples + n);
    size_t off = 0;
    while (m_pending.size() - off >= m_frameSamples) {
        int64_t frameStartMs = m_baseMonoMs + (m_sampleIdx * 1000) / m_cfg.sampleRate;
        processFrame(m_pending.data() + off, frameStartMs, out);
        off += m_frameSamples;
        m_sampleIdx += static_cast<int64_t>(m_frameSamples);
    }
    if (off > 0)
        m_pending.erase(m_pending.begin(), m_pending.begin() + off);

    return static_cast<int>(out.size() - before);
}

void CVad::processFrame(const float* frame, int64_t frameStartMs, std::vector<SSpeechSegment>& out) {
    const float r = rms(frame, m_frameSamples);
    m_lastRms     = r;

    // Update the rolling noise-floor estimate: the low percentile of the RMS over the
    // recent window. A low percentile follows the ambient/pauses (which are persistent)
    // and ignores transient speech bursts, so the gate rises to meet a hot source but
    // never gates out real speech.
    if (m_cfg.adaptive) {
        m_rmsWindow.push_back(r);
        while (m_rmsWindow.size() > m_windowFrames)
            m_rmsWindow.pop_front();
        std::vector<float> tmp(m_rmsWindow.begin(), m_rmsWindow.end());
        size_t             idx = static_cast<size_t>(tmp.size() * 0.2f); // 20th percentile
        if (idx >= tmp.size())
            idx = tmp.size() - 1;
        std::nth_element(tmp.begin(), tmp.begin() + idx, tmp.end());
        m_noiseFloor = tmp[idx];
    }

    const bool voiced = r >= currentThreshold();

    // The retention ring runs CONTINUOUSLY — in speech as well as out of it. It used to
    // be fed only while idle, so it was emptied into the utterance at onset and stayed
    // empty until that utterance endpointed: a user who spoke again shortly after the
    // hangover got a near-zero back-pad and lost their first syllable to exactly the gap
    // this back-pad exists to close. Keeping it fed costs one bounded deque and means the
    // ring always holds the tail of whatever just happened.
    m_preRoll.emplace_back(frame, frame + m_frameSamples);
    while (m_preRoll.size() > m_preRollFrames)
        m_preRoll.pop_front();

    if (!m_inSpeech) {
        if (voiced) {
            if (m_voicedRunMs == 0)
                m_runStartMs = frameStartMs; // first voiced frame of this run
            m_voicedRunMs += m_cfg.frameMs;
            if (m_voicedRunMs >= m_cfg.startMs) {
                // Onset. The utterance IS the ring: the current frame was pushed into it
                // at the top of this call, so appending `frame` again (as this used to)
                // duplicated 20 ms of audio at the splice and shifted every ASR word
                // timestamp after it by one frame.
                m_inSpeech     = true;
                m_onsetMs      = m_runStartMs;
                m_silenceRunMs = 0;
                // samples[0] is the oldest ring frame; the newest ring frame is the
                // current one, hence size()-1 frames of lead-in ahead of frameStartMs.
                m_bufferStartMs = frameStartMs - static_cast<int64_t>(m_preRoll.size() - 1) * m_cfg.frameMs;
                m_utterance.clear();
                for (auto& pf : m_preRoll)
                    m_utterance.insert(m_utterance.end(), pf.begin(), pf.end());
                m_preRoll.clear();
            }
        } else {
            m_voicedRunMs = 0;
        }
        return;
    }

    // In speech: keep appending; track trailing silence for endpointing.
    m_utterance.insert(m_utterance.end(), frame, frame + m_frameSamples);
    if (voiced) {
        m_silenceRunMs = 0;
    } else {
        m_silenceRunMs += m_cfg.frameMs;
    }

    const int64_t frameEndMs      = frameStartMs + m_cfg.frameMs;
    const int64_t utteranceLenMs  = frameEndMs - m_onsetMs;
    if (m_silenceRunMs >= m_cfg.endMs || utteranceLenMs >= m_cfg.maxUtteranceMs)
        emit(frameEndMs, out);
}

void CVad::emit(int64_t endMs, std::vector<SSpeechSegment>& out) {
    SSpeechSegment seg;
    seg.onsetMs       = m_onsetMs;
    seg.endMs         = endMs;
    seg.bufferStartMs = m_bufferStartMs;
    seg.samples       = std::move(m_utterance);
    out.push_back(std::move(seg));

    m_inSpeech     = false;
    m_voicedRunMs  = 0;
    m_silenceRunMs = 0;
    m_utterance.clear();
    // NOTE: the ring is deliberately NOT cleared here — what it holds is the trailing
    // hangover silence of the utterance just emitted, which is precisely the lead-in the
    // next utterance needs.
}

bool CVad::flush(std::vector<SSpeechSegment>& out) {
    if (!m_inSpeech)
        return false;
    int64_t endMs = m_baseMonoMs + (m_sampleIdx * 1000) / m_cfg.sampleRate;
    emit(endMs, out);
    return true;
}
