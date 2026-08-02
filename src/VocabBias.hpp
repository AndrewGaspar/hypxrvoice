#pragma once

#include "DesktopContext.hpp"

#include <string>
#include <vector>

// WP-V7: bias the ASR itself toward the proper nouns that are ACTUALLY ON SCREEN.
//
// THE PROBLEM. whisper base.en has no idea what a "Plex" is. Live, it produced
// "clicks" / "plaques" for Plex, "what's up" for WhatsApp, "B-top" for btop,
// "ghost it" for ghostty, "for"/"forward" for the workspace number four. Every one
// of those is a perfectly reasonable English guess — the model is not broken, it is
// simply decoding without the one piece of context that disambiguates the utterance:
// the window list the speaker is looking at. Homophone tables are the wrong fix
// (unbounded, hand-maintained, and stale the moment an app is opened); the right fix
// is to hand the decoder the live vocabulary.
//
// THE MECHANISM. whisper_full_params::initial_prompt / prompt_tokens seed the text
// decoder's context with tokens that are then far cheaper for it to emit. The vendored
// whisper.cpp (080bbbe, v1.9.1) supports both, plus carry_initial_prompt and a GBNF
// grammar hook. We use prompt_tokens (tokenized once per window, so the budget is
// EXACT rather than a chars-per-token guess) — see CAsr::setVocabBias.
//
// THE FORMAT, AND WHY. Measured on a 12-phrase corpus (espeak-ng speech degraded with
// a real Quest/WiVRn ambient bed) against ggml-base.en, exact-match scoring:
//
//     no prompt ................................. 3/12
//     bare comma list ("Plex, WhatsApp, btop.") .. 4/12
//     "Voice commands for windows: <list>. ..." .. 5/12
//     "Open windows: <list>. Monitors: <list>." .. 5/12
//     prose ("The windows on screen are ...") .... 9/12
//     EXEMPLAR COMMANDS (what this builder emits) 11/12
//
// A list of nouns barely helps; the same nouns placed inside the *sentence shapes the
// user actually speaks* help enormously, because the bias then lands on the whole
// (verb, noun, preposition) trajectory rather than on an isolated token. So the prompt
// this builder emits is a short script of plausible commands over the live vocabulary,
// not a glossary.
//
// The one remaining miss is a clip whose synthesized "Focus Plex" is unintelligible to
// begin with (unprompted it decodes as "Focus the eggs"). Against the same eight REAL
// recordings of the user, the bias changed no transcript for the worse.
//
// BOUNDED BY CONSTRUCTION. Terms are capped (maxTerms) and taken in focus-recency order,
// so the cap drops the windows you are least likely to be talking about. The caller fits
// the token budget by calling build() again with fewer terms (see
// CDaemon::refreshVocabBias) rather than truncating the assembled prompt — the app NAMES
// are the point, and a string truncation would keep the generic verb tail and throw them
// away. CAsr::setVocabBias truncates only as a backstop.
struct SVocabBiasConfig {
    bool enabled     = true;
    int  maxTerms    = 12;  // vocabulary terms woven into the exemplar script
    int  maxMonitors = 4;   // monitor names referenced by the script
    int  maxTokens   = 96;  // hard whisper-token ceiling for the assembled prompt
    // Hallucination guard (see Pipeline::processSegment). A buffer with less than this
    // much voiced audio is transcribed WITHOUT the bias.
    int  minVoicedMs = 200;
    // How long a built prompt may be reused before the desktop is re-snapshotted.
    int  refreshMs   = 5000;
};

namespace VocabBias {
    // Reduce a window class to the name a person would SAY for it. Purely structural —
    // no per-app table, nothing to maintain:
    //   com.mitchellh.ghostty            -> ghostty   (reverse-DNS: take the last part)
    //   chrome-web.whatsapp.com__-Default-> whatsapp  (strip the chrome wrapper + the
    //                                                  profile suffix, then take the
    //                                                  domain's second-level label)
    //   google-chrome                    -> chrome    (hyphenated: take the last part)
    //   Plex                             -> Plex      (already a name; casing kept)
    // Returns "" when nothing usable survives (too short, all digits, a bare TLD).
    std::string spokenClassName(const std::string& cls);

    // Distinctive tokens from a window title — this is where "btop", "nvtop", "nvim"
    // and project names live, none of which appear in any class. Leading tokens only
    // (the salient name comes first in a title), stop-words and the window's own class
    // term dropped, at most `max` per title.
    std::vector<std::string> titleTerms(const std::string& title, const std::string& classTerm, size_t max);

    // The ordered, deduplicated vocabulary the prompt will be built from: window terms
    // in FOCUS-RECENCY order (so the cap drops the least-likely referents), then the
    // monitor names. Deterministic — same context in, same vector out.
    std::vector<std::string> terms(const SDesktopContext& ctx, const SVocabBiasConfig& cfg);

    // Assemble the exemplar-command script. Always ends with a fixed tail covering the
    // command verbs and a spelled-out workspace number, so the bias is useful (and the
    // "four" -> "for"/"forward" failure is closed) even on an empty desktop.
    std::string build(const SDesktopContext& ctx, const SVocabBiasConfig& cfg);
}
