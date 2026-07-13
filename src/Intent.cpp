#include "Intent.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>

namespace {
    std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    std::vector<std::string> words(const std::string& s) {
        std::vector<std::string> out;
        std::string              cur;
        for (char c : s) {
            if (std::isalnum(static_cast<unsigned char>(c))) {
                cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            } else if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        }
        if (!cur.empty())
            out.push_back(cur);
        return out;
    }

    bool contains(const std::string& hay, const std::string& needle) {
        return hay.find(needle) != std::string::npos;
    }

    // Command keywords that must not leak into a semantic target phrase.
    bool isCommandWord(const std::string& t) {
        static const char* kw[] = {
            "pick", "grab", "lift", "carry", "place", "drop", "put", "leave", "set",
            "move", "bring", "push", "pull", "closer", "nearer", "further", "farther",
            "away", "back", "center", "centre", "dock", "undock", "follow", "anchor",
            "lock", "world", "hands", "hand", "open", "launch", "start", "up", "down",
            "make", "have", "let", "it", "me", "my", "the", "this", "that", "here",
            "there", "closer", "please", "to", "in", "front", "of", "on", "off",
            "attach", "monitor", "screen", "window", "display"};
        for (auto* k : kw)
            if (t == k)
                return true;
        return false;
    }
}

double sanitizeDeltaM(double modelDelta, const std::string& utterance, double step) {
    std::string low;
    low.reserve(utterance.size());
    for (char c : utterance)
        low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Magnitude: model value if sane, else the configured step.
    double mag = std::abs(modelDelta);
    if (mag < 0.05 || mag > 1.0)
        mag = std::abs(step) > 0.0 ? std::abs(step) : 0.25;

    // Direction: the utterance is authoritative when it contains a direction word.
    auto has = [&](const char* w) { return low.find(w) != std::string::npos; };
    if (has("closer") || has("nearer") || has("bring") || has("come here"))
        return -mag;
    if (has("further") || has("farther") || has("away") || has("back") || has("push"))
        return +mag;
    // No lexical direction: trust the model's sign (negative default if zero).
    return modelDelta < 0 ? -mag : (modelDelta > 0 ? +mag : -mag);
}

SDeicticHit findDeictic(const STranscript& t) {
    SDeicticHit hit;
    int  bestIdx   = -1;
    bool bestPlace = false;
    for (size_t i = 0; i < t.words.size(); i++) {
        std::string ww;
        for (char c : t.words[i].text)
            if (std::isalnum(static_cast<unsigned char>(c)))
                ww += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ww == "this" || ww == "that") { bestIdx = static_cast<int>(i); bestPlace = false; }
        else if (ww == "here" || ww == "there") { bestIdx = static_cast<int>(i); bestPlace = true; }
    }
    if (bestIdx >= 0) {
        hit.found   = true;
        hit.isPlace = bestPlace;
        hit.ms      = t.words[bestIdx].startMs;
        return hit;
    }
    // No per-word timestamps (heuristic ASR): fall back to the whole-utterance bounds.
    const std::string text = lower(t.text);
    if (contains(text, "here") || contains(text, "there")) {
        hit.found = true; hit.isPlace = true; hit.ms = t.endMs;
    } else if (contains(text, "this") || contains(text, "that")) {
        hit.found = true; hit.isPlace = false; hit.ms = t.onsetMs;
    }
    return hit;
}

SRawIntent CRuleIntent::detect(const STranscript& t) const {
    SRawIntent r;
    const std::string text = lower(t.text);
    if (text.empty())
        return r; // None

    // ---- deixis: the (trailing) deictic word governs targeting/placement ----
    if (SDeicticHit d = findDeictic(t); d.found) {
        r.deictic        = true;
        r.deicticIsPlace = d.isPlace;
        r.deicticWordMs  = d.ms;
    }

    // ---- verb detection (order matters: specific before generic) ----
    auto setVerb = [&](EVerb v, const char* n) { r.verb = v; r.note = n; };

    if (contains(text, "hand")) {
        setVerb(EVerb::HandInput, "hand-input keyword");
        if (contains(text, "off") || contains(text, "disable") || contains(text, "stop"))
            r.sub = "off";
        else if (contains(text, "on") || contains(text, "enable"))
            r.sub = "on";
        else if (contains(text, "auto"))
            r.sub = "auto";
        else
            r.sub = "toggle";
    } else if (contains(text, "open") || contains(text, "launch") ||
               (contains(text, "start") && !contains(text, "follow"))) {
        setVerb(EVerb::LaunchApp, "launch keyword");
    } else if (contains(text, "pick") && (contains(text, " up") || contains(text, "up ")) &&
               contains(text, "follow")) {
        setVerb(EVerb::Undock, "pick-up-and-follow");
    } else if ((contains(text, "pick") && contains(text, "up")) || contains(text, "grab") ||
               contains(text, "lift") || (contains(text, "carry") && !contains(text, "with me"))) {
        setVerb(EVerb::Pick, "pick-up keyword");
    } else if (contains(text, "place") || contains(text, "drop") ||
               (contains(text, "put") && r.deictic) || contains(text, "leave it") ||
               contains(text, "set it down")) {
        setVerb(EVerb::Place, "place/drop keyword");
    } else if (contains(text, "undock")) {
        setVerb(EVerb::Undock, "undock keyword");
    } else if (contains(text, "follow") || contains(text, "come with me")) {
        setVerb(EVerb::Follow, "follow keyword");
        if (contains(text, "head")) r.anchor = EAnchorMode::Head;
        else if (contains(text, "body")) r.anchor = EAnchorMode::Body;
    } else if (contains(text, "dock")) {
        setVerb(EVerb::Dock, "dock keyword");
        if (contains(text, "here")) r.sub = "here";
    } else if (contains(text, "center") || contains(text, "centre") ||
               contains(text, "in front of me")) {
        setVerb(EVerb::Center, "center keyword");
    } else if (contains(text, "closer") || contains(text, "nearer") ||
               contains(text, "bring it") || contains(text, "come here")) {
        setVerb(EVerb::MoveDist, "closer keyword");
        r.deltaM = -m_cfg.distanceStep;
        r.deictic = false; // "come here"/"closer" is motion, not a place-deixis
    } else if (contains(text, "further") || contains(text, "farther") ||
               contains(text, "push it") || contains(text, "away") || contains(text, "back")) {
        setVerb(EVerb::MoveDist, "further keyword");
        r.deltaM = +m_cfg.distanceStep;
        r.deictic = false;
    } else if (contains(text, "anchor") || contains(text, "world lock") ||
               contains(text, "world-lock") || contains(text, "lock it")) {
        setVerb(EVerb::Anchor, "anchor keyword");
        if (contains(text, "head")) r.anchor = EAnchorMode::Head;
        else if (contains(text, "body")) r.anchor = EAnchorMode::Body;
        else r.anchor = EAnchorMode::Local; // "lock it" -> world-lock
    } else {
        return r; // None — no command keyword matched
    }

    // ---- build the semantic target phrase / app phrase from content words ----
    std::string phrase;
    for (auto& w : words(t.text)) {
        if (isCommandWord(w))
            continue;
        if (!phrase.empty()) phrase += ' ';
        phrase += w;
    }
    if (r.verb == EVerb::LaunchApp)
        r.appPhrase = phrase;
    else
        r.targetPhrase = phrase;

    return r;
}

SAction finalizeAction(const SRawIntent& raw, const STranscript& t,
                       const SDesktopContext& ctx, const GazeQueryFn& gazeQuery,
                       const SIntentConfig& cfg) {
    SAction a;
    a.verb       = raw.verb;
    a.anchor     = raw.anchor;
    a.sub        = raw.sub;
    a.deltaM     = raw.deltaM;
    a.confidence = raw.confidence;
    a.note       = raw.note;
    a.utterance  = t.text;

    if (raw.verb == EVerb::None) {
        a.note = raw.note.empty() ? "no command recognized" : raw.note;
        return a;
    }

    // Verbs with no monitor target.
    if (raw.verb == EVerb::HandInput) {
        a.targetSource = ETargetSource::None;
        return a;
    }
    if (raw.verb == EVerb::LaunchApp) {
        a.targetSource = ETargetSource::None;
        a.app          = raw.appPhrase;
        if (a.app.empty()) {
            a.verb            = EVerb::Clarify;
            a.clarifyQuestion = "which app should I open?";
            a.confidence      = 0.3;
        }
        return a;
    }

    // ---- target resolution ----
    // 1. Semantic name/app match from the spoken phrase.
    SMonitorMatch sem;
    if (!raw.targetPhrase.empty())
        sem = ctx.resolveMonitor(raw.targetPhrase);

    // 2. Deixis via the gaze ring (only when no confident name was spoken).
    SGazeResolution gz;
    const bool wantDeixis = raw.deictic && (!sem.matched || sem.confidence < 0.6);
    if (wantDeixis && gazeQuery)
        gz = resolveDeixis(raw.deicticWordMs, cfg.gaze, gazeQuery, &ctx);

    // Place is special: "here" designates a POSE for the currently-carried monitor,
    // NOT a new monitor pick. Keep the target as `active`, attach the gaze pose.
    if (raw.verb == EVerb::Place) {
        a.target       = "active";
        a.targetSource = ETargetSource::Active;
        if (gz.valid) {
            a.gaze         = gz;
            a.targetSource = ETargetSource::Deixis;
            // Confidence reflects how settled the gaze pick was.
            a.confidence = gz.stable ? 0.9 : 0.6;
        }
        return a;
    }

    // For all other monitor verbs: prefer a live semantic match, else deixis, else
    // the hovered monitor, else `active`.
    if (sem.matched && sem.candidates.size() > 1) {
        // Ambiguous: pick the best but flag it low-confidence with the alternatives.
        a.target         = sem.name;
        a.targetSource   = ETargetSource::Semantic;
        a.confidence     = 0.4;
        a.clarifyCandidates = sem.candidates;
        a.note += " (ambiguous target)";
        return a;
    }
    if (sem.matched) {
        a.target       = sem.name;
        a.targetSource = ctx.find(sem.name) && ctx.find(sem.name)->name == raw.targetPhrase
                             ? ETargetSource::Named
                             : ETargetSource::Semantic;
        a.confidence   = std::min(a.confidence, sem.confidence);
        return a;
    }
    if (gz.valid && !gz.name.empty()) {
        a.target       = gz.name;
        a.targetSource = ETargetSource::Deixis;
        a.gaze         = gz;
        a.confidence   = gz.stable ? std::min(a.confidence, 0.9) : 0.6;
        return a;
    }
    if (raw.deictic && gz.valid) {
        // Deictic resolved but landed on passthrough (no monitor) — attach the pose
        // and fall back to the hovered/active target so the verb still has a subject.
        a.gaze = gz;
    }
    if (const SMonitorInfo* h = ctx.hoveredMonitor()) {
        a.target       = h->name;
        a.targetSource = ETargetSource::Deixis;
        a.confidence   = std::min(a.confidence, 0.7);
        a.note += " (hovered-monitor fallback)";
        return a;
    }
    // Last resort: defer to the compositor's selection order.
    a.target       = "active";
    a.targetSource = ETargetSource::Active;
    a.confidence   = std::min(a.confidence, 0.6);
    return a;
}

SAction CRuleIntent::resolve(const STranscript& t, const SDesktopContext& ctx,
                             const GazeQueryFn& gazeQuery) const {
    SRawIntent raw = detect(t);
    return finalizeAction(raw, t, ctx, gazeQuery, m_cfg);
}
