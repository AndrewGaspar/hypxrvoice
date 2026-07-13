#include "Pipeline.hpp"
#include "WakeWord.hpp"

#include <algorithm>

namespace Pipeline {
    bool processSegment(CAsr& asr, const SConfig& cfg, const SSpeechSegment& seg,
                        EActivation activation, bool requireWake, STranscript& out) {
        out = asr.transcribe(seg.samples, seg.bufferStartMs, seg.onsetMs, seg.endMs, activation);
        if (out.text.empty())
            return false;

        if (requireWake && cfg.wake.enabled) {
            std::string stripped;
            if (!WakeWord::matchPrefix(out.text, cfg.wake.phrase, cfg.wake.fuzz, stripped))
                return false; // not addressed to us
            out.wakePhrase = cfg.wake.phrase;

            // Re-scope the transcript to the post-wake command: drop leading words
            // that belong to the wake phrase so word timestamps stay aligned to the
            // command text (which the downstream gaze/intent tier consumes).
            std::vector<std::string> phraseToks = WakeWord::normalize(cfg.wake.phrase);
            size_t                   drop        = std::min(phraseToks.size(), out.words.size());
            if (drop > 0 && drop <= out.words.size())
                out.words.erase(out.words.begin(), out.words.begin() + drop);
            out.text = stripped;
            // onsetMs stays at the true speech onset (the wake word itself) — it is
            // the anchoring instant; the first *command* word carries its own ts.
        }
        return true;
    }
}
