#include "HudModel.hpp"

#include <algorithm>

const char* hudStateName(EHudState s) {
    switch (s) {
        case EHudState::Hidden:    return "hidden";
        case EHudState::Listening: return "listening";
        case EHudState::Action:    return "action";
        case EHudState::Clarify:   return "clarify";
        case EHudState::Error:     return "error";
    }
    return "?";
}

// Present-participle phrase for the acted verb, e.g. "moving", "docking".
static const char* verbGerund(EVerb v) {
    switch (v) {
        case EVerb::Pick:      return "picking up";
        case EVerb::Place:     return "placing";
        case EVerb::MoveDist:  return "moving";
        case EVerb::Center:    return "centering";
        case EVerb::Dock:      return "docking";
        case EVerb::Undock:    return "undocking";
        case EVerb::Follow:    return "following";
        case EVerb::Anchor:    return "anchoring";
        case EVerb::HandInput: return "hands";
        case EVerb::LaunchApp: return "opening";
        default:               return "";
    }
}

std::string hudActionPhrase(const SAction& a) {
    if (a.verb == EVerb::Clarify) {
        if (!a.clarifyQuestion.empty())
            return a.clarifyQuestion;
        return "which one?";
    }
    if (a.verb == EVerb::None)
        return "";

    std::string phrase = verbGerund(a.verb);

    if (a.verb == EVerb::LaunchApp) {
        if (!a.app.empty())
            phrase += " " + a.app;
        return phrase;
    }
    if (a.verb == EVerb::HandInput) {
        // sub = on|off|toggle
        if (!a.sub.empty())
            phrase += " " + a.sub;
        return phrase;
    }

    // Monitor-targeted verbs: append the target unless it is the implicit "active".
    if (!a.target.empty() && a.target != "active")
        phrase += " " + a.target;

    // MoveDist reads better with a direction word.
    if (a.verb == EVerb::MoveDist)
        phrase = std::string(a.deltaM < 0 ? "closer" : "further") +
                 (a.target.empty() || a.target == "active" ? "" : " — " + a.target);

    return phrase;
}

// A short "how we resolved the target" hint, or "" when not worth showing.
static std::string sourceHint(const SAction& a) {
    switch (a.targetSource) {
        case ETargetSource::Deixis:   return a.gaze.stable ? "you looked at it" : "best-guess gaze";
        case ETargetSource::Semantic: return "matched";
        case ETargetSource::Named:    return "";       // verbatim name; no hint needed.
        case ETargetSource::Active:   return "active";
        case ETargetSource::None:     return "";
    }
    return "";
}

SHudView hudForListening(const std::string& partial, const SConfig& cfg) {
    (void)cfg;
    SHudView v;
    v.state  = EHudState::Listening;
    v.holdMs = -1; // persist until replaced / hidden (daemon treats hold<0 as persistent).
    v.lines.push_back({partial.empty() ? "listening…" : "listening", EHudColor::Accent, true});
    if (!partial.empty())
        v.lines.push_back({partial, EHudColor::Normal, false});
    return v;
}

SHudView hudForAction(const SAction& a, const SExecPlan& plan, const SConfig& cfg) {
    // Envelope defaults come from SHudView (rise 110 / hold 2600 / fade 450) and are
    // forwarded to hypxrhud as panel props; geometry + opacity are the daemon's config.
    SHudView v;
    v.dryRun       = cfg.executor.dryRun;
    v.approximated = plan.approximated;

    if (a.verb == EVerb::None) {
        v.state = EHudState::Hidden;
        return v;
    }

    if (a.verb == EVerb::Clarify) {
        v.state = EHudState::Clarify;
        v.holdMs = std::max(v.holdMs, 3500); // give a question longer dwell.
        v.lines.push_back({a.clarifyQuestion.empty() ? "which one?" : a.clarifyQuestion,
                           EHudColor::Accent, true});
        for (const auto& c : a.clarifyCandidates)
            v.lines.push_back({"• " + c, EHudColor::Normal, false});
        if (a.clarifyCandidates.empty())
            v.lines.push_back({"say it again", EHudColor::Dim, false});
        return v;
    }

    const bool refused = !plan.ok;
    v.state = refused ? EHudState::Error : EHudState::Action;

    // Title = the action phrase, coloured by outcome.
    v.lines.push_back({hudActionPhrase(a), refused ? EHudColor::Bad : EHudColor::Accent, true});

    // Target provenance hint (skip for verbs without a monitor target).
    if (a.targetSource != ETargetSource::None && a.targetSource != ETargetSource::Named) {
        std::string hint = sourceHint(a);
        if (!hint.empty())
            v.lines.push_back({hint, a.targetSource == ETargetSource::Deixis && !a.gaze.stable
                                          ? EHudColor::Warn : EHudColor::Dim, false});
    }

    if (refused) {
        v.lines.push_back({plan.reason.empty() ? "can't do that" : plan.reason, EHudColor::Bad, false});
    } else {
        if (plan.approximated)
            v.lines.push_back({"approx", EHudColor::Warn, false});
        if (v.dryRun)
            v.lines.push_back({"dry-run", EHudColor::Dim, false});
    }

    // Confidence bar (only meaningful for actionable verbs).
    v.confidence = std::clamp(static_cast<float>(a.confidence), 0.f, 1.f);
    return v;
}
