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
    // The gap tolerance lets the onset run span a dip, so the run can be up to
    // gapToleranceMs longer in WALL time than startMs; add it so the back-pad guarantee
    // survives a choppy first word too.
    const int backpadMs = std::max(0, m_cfg.onsetBackpadMs);
    const int runMs     = std::max(0, m_cfg.startMs) + std::max(0, m_cfg.gapToleranceMs);
    const int depthMs   = std::max(std::max(0, m_cfg.preRollMs), backpadMs + runMs);
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
            m_runGapMs = 0;
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
        } else if (m_voicedRunMs > 0) {
            // A DIP inside the run, not (yet) the end of it. Real words are full of these
            // — the stop before "focus"'s /k/, the one before "workspace"'s /sp/ — and
            // resetting on the first of them is what made a short, choppy word structurally
            // undetectable (see SVadConfig::gapToleranceMs). The run is PAUSED: the gap
            // does not count toward startMs, so noise gains nothing, and it is discarded
            // only once the gap outlasts the tolerance.
            m_runGapMs += m_cfg.frameMs;
            if (m_runGapMs > m_cfg.gapToleranceMs) {
                m_voicedRunMs = 0;
                m_runGapMs    = 0;
            }
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
    m_runGapMs     = 0;
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

// ---------------------------------------------------------------------------
// Whole-buffer speech presence (the PTT path's no-speech verdict).
// ---------------------------------------------------------------------------

SSpeechPresence detectSpeechPresence(const float* samples, size_t n, const SVadConfig& cfg) {
    SSpeechPresence out;
    const int    frameMs = cfg.frameMs > 0 ? cfg.frameMs : 20;
    const int    sr      = cfg.sampleRate > 0 ? cfg.sampleRate : 16000;
    const size_t frameN  = std::max<size_t>(1, static_cast<size_t>(sr) * frameMs / 1000);
    if (!samples || n < frameN)
        return out;

    std::vector<float> rmsPerFrame;
    rmsPerFrame.reserve(n / frameN);
    for (size_t off = 0; off + frameN <= n; off += frameN) {
        const float r = rms(samples + off, frameN);
        rmsPerFrame.push_back(r);
        out.peakRms = std::max(out.peakRms, r);
    }
    if (rmsPerFrame.empty())
        return out;

    // The buffer's own ambient: the same 20th percentile the live detector uses, so the
    // two agree about what "silence" means on this source.
    std::vector<float> sorted(rmsPerFrame);
    size_t             idx = static_cast<size_t>(sorted.size() * 0.2f);
    if (idx >= sorted.size())
        idx = sorted.size() - 1;
    std::nth_element(sorted.begin(), sorted.begin() + idx, sorted.end());
    out.floorRms = sorted[idx];

    // DELIBERATELY below the live gate: half the fixed floor, or 1.5x the measured
    // ambient. Both terms sit under the VAD's own max(energyThreshold, floor*1.6), so a
    // buffer this rejects could never have produced a segment either — the verdict can
    // only ever transcribe MORE than the old path, never less.
    //
    // The ambient term is additionally capped at half the PEAK. A buffer that is speech
    // end to end (the user talked through the whole window) has no quiet frames for the
    // percentile to find, so its "floor" IS the speech — and an uncapped 1.5x of that
    // would declare the loudest utterance in the round to be silence. Capping cannot
    // break the invariant above: min(floor*1.5, peak*0.5) <= floor*1.6 either way.
    static constexpr float kFixedFraction = 0.5f;
    static constexpr float kNoiseFactor   = 1.5f;
    static constexpr float kPeakFraction  = 0.5f;
    out.threshold = std::max(cfg.energyThreshold * kFixedFraction,
                             std::min(out.floorRms * kNoiseFactor, out.peakRms * kPeakFraction));

    for (float r : rmsPerFrame)
        if (r >= out.threshold)
            out.voicedMs += frameMs;
    out.found = out.voicedMs >= std::max(0, cfg.presenceMs);
    return out;
}
