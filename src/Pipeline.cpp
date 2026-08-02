#include "Pipeline.hpp"
#include "Log.hpp"
#include "WakeWord.hpp"

#include <algorithm>
#include <cctype>

namespace Pipeline {
    SVadConfig vadConfig(const SConfig& cfg) {
        SVadConfig vc;
        vc.sampleRate       = cfg.audio.sampleRate;
        vc.energyThreshold  = cfg.vad.energyThreshold;
        vc.startMs          = cfg.vad.startMs;
        vc.endMs            = cfg.vad.endMs;
        vc.maxUtteranceMs   = cfg.vad.maxUtteranceMs;
        vc.preRollMs        = cfg.vad.preRollMs;
        vc.onsetBackpadMs   = cfg.vad.onsetBackpadMs;
        vc.gapToleranceMs   = cfg.vad.gapToleranceMs;
        vc.presenceMs       = cfg.vad.presenceMs;
        vc.adaptive         = cfg.vad.adaptive;
        vc.noiseFloorFactor = cfg.vad.noiseFloorFactor;
        vc.noiseWindowMs    = cfg.vad.noiseWindowMs;
        return vc;
    }

    bool vocabBiasAllowed(const SConfig& cfg, const SSpeechSegment& seg) {
        if (!cfg.asr.vocabBias)
            return false;
        if (cfg.asr.vocabBiasMinVoicedMs <= 0)
            return true; // guard explicitly disabled
        if (seg.samples.empty())
            return false;
        const SSpeechPresence p = detectSpeechPresence(seg.samples.data(), seg.samples.size(), vadConfig(cfg));
        return p.voicedMs >= cfg.asr.vocabBiasMinVoicedMs;
    }

    bool isPromptEcho(const std::string& text, const std::string& prompt) {
        if (prompt.empty())
            return false;
        // Compare on letters+digits only, lowercased: whisper re-punctuates and re-cases
        // freely, so a raw substring test would miss the very case we care about
        // ("Move Plex here" vs "move plex here.").
        auto squash = [](const std::string& s) {
            std::string o;
            for (char c : s)
                if (std::isalnum(static_cast<unsigned char>(c)))
                    o += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return o;
        };
        const std::string t = squash(text);
        // A short utterance is LEGITIMATELY a substring of the prompt — that is the whole
        // point, "Plex" and "Move Plex here" are both in there on purpose. Only a run long
        // enough to be a replayed stretch of the script counts as an echo.
        if (t.size() < 40)
            return false;
        return squash(prompt).find(t) != std::string::npos;
    }

    bool processSegment(CAsr& asr, const SConfig& cfg, const SSpeechSegment& seg,
                        EActivation activation, bool requireWake, STranscript& out) {
        const bool bias = vocabBiasAllowed(cfg, seg);
        if (cfg.asr.vocabBias && !bias && !asr.vocabBias().empty())
            Log::log(Log::DEBUG, "vocab bias suppressed for this segment (under {}ms voiced)",
                     cfg.asr.vocabBiasMinVoicedMs);
        out = asr.transcribe(seg.samples, seg.bufferStartMs, seg.onsetMs, seg.endMs, activation, bias);
        if (out.text.empty())
            return false;
        if (bias && isPromptEcho(out.text, asr.vocabBias())) {
            // The decoder replayed our own prompt instead of the audio. Nothing the user
            // said is in there, so treat it as no speech rather than actuating on it.
            out.text.clear();
            out.words.clear();
            return false;
        }

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
