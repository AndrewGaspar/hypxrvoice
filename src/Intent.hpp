#pragma once

#include "Command.hpp"
#include "DesktopContext.hpp"
#include "GazeResolver.hpp"
#include "Transcript.hpp"

#include <string>

// WP-V4 intent tier. Turns a word-timestamped transcript + a desktop snapshot into a
// typed SAction. Two backends produce the same SAction schema:
//   * CRuleIntent  — a deterministic keyword grammar. Fast, offline, no model. It is
//                    the default and the fixture-test path.
//   * CLlamaIntent — a GBNF-grammar-constrained local LLM (subprojects/llama.cpp),
//                    for looser phrasings. See LlamaIntent.hpp (HAVE_LLAMA).
//
// Both emit a backend-agnostic RAW intent (verb + a target hint + a deixis flag),
// then the SHARED finalizeAction() performs the safety-critical resolution — deixis
// via the gaze ring, semantic monitor resolution against ENUMERATED live names — so
// no backend can ever target an invented monitor.

struct SIntentConfig {
    SGazeConfig gaze;              // deixis stability window / lead.
    double      distanceStep = 0.25; // "closer"/"further" step (m).
};

// The backend-neutral, pre-resolution intent. `targetPhrase` is the spoken words that
// name the target ("the coding monitor"); it is resolved semantically in finalize.
struct SRawIntent {
    EVerb       verb = EVerb::None;
    std::string targetPhrase;          // words that may name a monitor/app target.
    bool        deictic        = false; // a "this"/"here"/"that" governs targeting.
    bool        deicticIsPlace = false; // "here"/"there" -> resolve a POSE, not a monitor.
    int64_t     deicticWordMs  = 0;     // timestamp of the deictic word.
    EAnchorMode anchor = EAnchorMode::Unset;
    std::string sub;                    // on|off|toggle|auto|here|head|body.
    double      deltaM = 0.0;
    std::string appPhrase;              // LaunchApp: spoken app words.
    double      confidence = 1.0;
    std::string note;
};

// SHARED resolution: deixis (via `gazeQuery`) + semantic target (via `ctx`) + target
// validation. `gazeQuery` may be a no-op (returns "") when no compositor is present;
// deixis then degrades to the hovered-monitor hint or `active`.
SAction finalizeAction(const SRawIntent& raw, const STranscript& t,
                       const SDesktopContext& ctx, const GazeQueryFn& gazeQuery,
                       const SIntentConfig& cfg);

// The deterministic rule-based backend.
class CRuleIntent {
  public:
    explicit CRuleIntent(const SIntentConfig& cfg) : m_cfg(cfg) {}

    // Full path: raw detection -> shared finalize.
    SAction resolve(const STranscript& t, const SDesktopContext& ctx,
                    const GazeQueryFn& gazeQuery) const;

    // Exposed for tests: keyword grammar only, no resolution.
    SRawIntent detect(const STranscript& t) const;

  private:
    SIntentConfig m_cfg;
};
