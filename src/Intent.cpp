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
            "attach", "monitor", "screen", "window", "display",
            // window-management vocabulary (round 2)
            "fullscreen", "full", "maximize", "maximise", "workspace", "work", "space",
            "focus", "switch", "go", "and",
            // spoken numbers — these name a workspace, never an app or a monitor
            "zero", "one", "two", "three", "four", "five", "six", "seven", "eight",
            "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen",
            "sixteen", "seventeen", "eighteen", "nineteen", "twenty"};
        for (auto* k : kw)
            if (t == k)
                return true;
        return false;
    }

    // A spoken or written cardinal, or -1. Whisper is inconsistent about which form it
    // emits for the same utterance — the round-2 live log has "workspace three" coming
    // back as "3." on one attempt and "three." on the next — so both must parse the same.
    int cardinal(const std::string& tok) {
        static const char* kWords[] = {"zero", "one", "two", "three", "four", "five", "six",
                                       "seven", "eight", "nine", "ten", "eleven", "twelve",
                                       "thirteen", "fourteen", "fifteen", "sixteen",
                                       "seventeen", "eighteen", "nineteen", "twenty"};
        for (int i = 0; i < 21; i++)
            if (tok == kWords[i])
                return i;
        if (tok.empty() || tok.size() > 2)
            return -1;
        for (char c : tok)
            if (!std::isdigit(static_cast<unsigned char>(c)))
                return -1;
        return std::stoi(tok);
    }

    // Determiners and fillers that carry no identity in a destination phrase.
    bool isDeterminer(const std::string& t) {
        return t == "the" || t == "a" || t == "an" || t == "my" || t == "our";
    }

    // Words that say "the thing I am naming is an output", not part of its name.
    bool isMonitorNoun(const std::string& t) {
        return t == "monitor" || t == "screen" || t == "display";
    }

    // Find `joined` in `toks`, tolerating the ASR splitting the compound ("work space",
    // "full screen" — both show up often enough that matching only the joined form loses
    // the command outright). Returns the index just PAST the match, or -1.
    int findCompound(const std::vector<std::string>& toks, const char* joined,
                     const char* a, const char* b) {
        for (size_t i = 0; i < toks.size(); i++) {
            if (toks[i] == joined)
                return static_cast<int>(i) + 1;
            if (toks[i] == a && i + 1 < toks.size() && toks[i + 1] == b)
                return static_cast<int>(i) + 2;
        }
        return -1;
    }

    // "move/put/send <window> to <destination>" — the one command that names a WINDOW and
    // a place to put it. Returns true only when BOTH halves parsed; the caller then treats
    // the utterance as a window move and skips the rest of the keyword chain.
    //
    // The destination must announce itself as an output or a workspace: a monitor noun
    // ("monitor"/"screen"/"display"), a spatial word ("left"/"right"), or "workspace".
    // Without that requirement "move this closer to me" parses as a move-to-"me", which
    // is exactly the kind of confident nonsense the closed grammar exists to prevent.
    bool parseWindowMove(const std::vector<std::string>& toks, SRawIntent& r) {
        static const char* kVerbs[] = {"move", "put", "send", "throw", "drag", "shift"};
        size_t vi = toks.size();
        for (size_t i = 0; i < toks.size() && vi == toks.size(); i++)
            for (auto* v : kVerbs)
                if (toks[i] == v) { vi = i; break; }
        if (vi == toks.size())
            return false;

        // The preposition that introduces the destination.
        size_t pi = toks.size();
        for (size_t i = vi + 1; i < toks.size(); i++)
            if (toks[i] == "to" || toks[i] == "onto" || toks[i] == "on" || toks[i] == "over") {
                pi = i;
                break;
            }
        if (pi == toks.size())
            return false;
        size_t d = pi + 1;
        if (d < toks.size() && toks[d] == "to")
            d++; // "over to the left monitor"
        if (d >= toks.size())
            return false;

        // The window half. REQUIRED: a bare "move to workspace 3" is a workspace SWITCH,
        // and guessing that it meant the focused window would silently relocate it.
        std::string win;
        for (size_t i = vi + 1; i < pi; i++) {
            if (!win.empty()) win += ' ';
            win += toks[i];
        }
        if (win.empty())
            return false;

        std::vector<std::string> dest(toks.begin() + static_cast<long>(d), toks.end());
        while (!dest.empty() && isDeterminer(dest.front()))
            dest.erase(dest.begin());
        if (dest.empty())
            return false;

        // A workspace destination: "…to workspace three". No number is underspecified,
        // not out of scope — finalize turns that into a Clarify.
        if (int wsAt = findCompound(dest, "workspace", "work", "space"); wsAt >= 0) {
            int idx = -1;
            for (size_t i = static_cast<size_t>(wsAt); i < dest.size(); i++)
                if (int n = cardinal(dest[i]); n >= 0) { idx = n; break; }
            r.windowPhrase = win;
            r.sub          = "workspace";
            r.workspace    = idx >= 0 ? idx : 0;
            return true;
        }

        // A spatial destination: "…to the left", "…to the right monitor".
        if (dest.front() == "left" || dest.front() == "leftmost")
            r.spatial = ESpatialRef::Left;
        else if (dest.front() == "right" || dest.front() == "rightmost")
            r.spatial = ESpatialRef::Right;
        if (r.spatial != ESpatialRef::None) {
            r.windowPhrase = win;
            return true;
        }

        // Anything else must still SAY it is an output; the phrase then resolves
        // semantically (a live name) or, for "…to this monitor", through the gaze ring.
        bool named = false;
        for (auto& t : dest)
            if (isMonitorNoun(t)) { named = true; break; }
        if (!named)
            return false;
        std::string mon;
        for (auto& t : dest) {
            if (!mon.empty()) mon += ' ';
            mon += t;
        }
        r.windowPhrase  = win;
        r.monitorPhrase = mon;
        return true;
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

    // Window management (round 2). These sit ahead of the XR verbs because their
    // triggers are multi-word and specific, and because a phrase like "focus the center
    // monitor" would otherwise be swallowed by the generic "center" keyword below.
    const std::vector<std::string> toks = words(t.text);
    const int wsAt = findCompound(toks, "workspace", "work", "space");
    const int fsAt = findCompound(toks, "fullscreen", "full", "screen");

    // Window MOVES are checked first: they are the only shape that names a window AND a
    // destination, and both "workspace" and the XR distance verbs would otherwise swallow
    // one ("move the terminal to workspace three" is not a workspace switch). The parse
    // is strict enough to decline anything that is not actually a move (see parseWindowMove),
    // so falling through costs nothing.
    if (parseWindowMove(toks, r)) {
        setVerb(EVerb::MoveWindow, "window-move keyword");
        return r; // the spans are already split; the generic phrase builder must not run
    }

    // "workspace three" / "go to workspace 3" / "switch to workspace three". The
    // "workspace" token is REQUIRED: a bare trailing "3." (which is exactly what the
    // live round produced when the leading word was lost) must stay unparsed rather
    // than have us guess at a workspace switch.
    int wsIndex = -1;
    if (wsAt >= 0)
        for (size_t i = static_cast<size_t>(wsAt); i < toks.size(); i++)
            if (int n = cardinal(toks[i]); n >= 0) { wsIndex = n; break; }

    if (wsAt >= 0) {
        // A "workspace" with no number is underspecified, not out of scope — finalize
        // turns workspace 0 into a Clarify rather than guessing at an index.
        setVerb(EVerb::Workspace, "workspace keyword");
        r.workspace = wsIndex >= 0 ? wsIndex : 0;
    } else if (fsAt >= 0 || contains(text, "maximize") || contains(text, "maximise")) {
        // "make this window fullscreen", "fullscreen this", "fullscreen the browser",
        // "make it fullscreen". Hyprland's dispatcher is a toggle, which is the right
        // reading of every one of those.
        setVerb(EVerb::Fullscreen, "fullscreen keyword");
        if (fsAt < 0)
            r.sub = "maximize"; // "maximize" is the other Hyprland fullscreen mode
    } else if (contains(text, "focus") || contains(text, "switch to")) {
        setVerb(EVerb::Focus, "focus keyword");
    } else if (contains(text, "hand")) {
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
    a.workspace  = raw.workspace;
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
    if (raw.verb == EVerb::Workspace) {
        a.targetSource = ETargetSource::None;
        if (a.workspace < 1 || a.workspace > 99) {
            // Heard "workspace" but no usable index. Ask; never pick one.
            a.verb            = EVerb::Clarify;
            a.clarifyQuestion = "which workspace?";
            a.confidence      = 0.3;
        }
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

    // MoveWindow names BOTH: a window to relocate and a place to put it. The two halves
    // resolve against different live lists, and either half being ambiguous is a Clarify —
    // moving the wrong window, or moving the right one somewhere unexpected, are both
    // annoying to undo in a headset.
    if (raw.verb == EVerb::MoveWindow) {
        a.targetSource = ETargetSource::None;

        // 1. WHICH window. Content-first, exactly like focus/fullscreen. Nothing named
        //    (or nothing matched) leaves the address empty, which the executor reads as
        //    "the focused window" — the right meaning of "move this to the left monitor".
        SWindowMatch win;
        if (!raw.windowPhrase.empty())
            win = ctx.resolveWindow(raw.windowPhrase);
        if (win.matched && win.candidates.size() > 1) {
            a.verb              = EVerb::Clarify;
            a.clarifyQuestion   = "which one?";
            a.clarifyCandidates = win.candidates;
            a.confidence        = 0.4;
            return a;
        }
        if (win.matched) {
            a.windowAddress = win.address;
            a.windowLabel   = win.label;
            a.confidence    = std::min(a.confidence, win.confidence);
        }

        // 2a. A WORKSPACE destination needs an index and nothing else.
        if (raw.sub == "workspace") {
            a.sub       = "";
            a.workspace = raw.workspace;
            if (a.workspace < 1 || a.workspace > 99) {
                a.verb            = EVerb::Clarify;
                a.clarifyQuestion = "which workspace?";
                a.confidence      = 0.3;
            }
            return a;
        }

        // 2b. A MONITOR destination: spatial (layout), then named/semantic, then deixis.
        SMonitorMatch mon;
        if (raw.spatial != ESpatialRef::None) {
            mon = ctx.resolveSpatialMonitor(raw.spatial);
            if (!mon.matched) {
                a.verb            = EVerb::Clarify;
                a.clarifyQuestion = "which monitor?";
                a.confidence      = 0.3;
                return a;
            }
        } else if (!raw.monitorPhrase.empty()) {
            mon = ctx.resolveMonitor(raw.monitorPhrase);
        }
        if (mon.matched && mon.candidates.size() > 1) {
            a.verb              = EVerb::Clarify;
            a.clarifyQuestion   = "which monitor?";
            a.clarifyCandidates = mon.candidates;
            a.confidence        = 0.4;
            return a;
        }
        if (mon.matched) {
            a.target       = mon.name;
            a.targetSource = raw.spatial != ESpatialRef::None ? ETargetSource::Semantic
                                                              : ETargetSource::Named;
            a.confidence   = std::min(a.confidence, mon.confidence);
            return a;
        }
        // "…to this monitor": the destination is wherever the user was looking.
        if (gz.valid && !gz.name.empty() && ctx.hasMonitor(gz.name)) {
            a.target       = gz.name;
            a.targetSource = ETargetSource::Deixis;
            a.gaze         = gz;
            a.confidence   = gz.stable ? std::min(a.confidence, 0.9) : 0.6;
            return a;
        }
        // Named a destination we cannot see. Ask; never fall back to "active" here — the
        // window would move somewhere the user did not ask for.
        a.verb            = EVerb::Clarify;
        a.clarifyQuestion = raw.monitorPhrase.empty() ? "move it where?"
                                                      : "I don't see " + raw.monitorPhrase;
        a.confidence      = 0.3;
        return a;
    }

    // Focus/Fullscreen name a WINDOW, not a monitor, so they resolve against the live
    // window list instead of the monitor list. Content-first, exactly as everywhere else:
    // "the browser" beats whatever the keyboard last touched, and only when nothing was
    // named does the deictic/active fallback take over — "make this window fullscreen"
    // means the one you are looking at, or failing that the focused one.
    if (raw.verb == EVerb::Focus || raw.verb == EVerb::Fullscreen) {
        a.targetSource = ETargetSource::None;
        SWindowMatch win;
        if (!raw.targetPhrase.empty())
            win = ctx.resolveWindow(raw.targetPhrase);

        if (win.matched && win.candidates.size() > 1) {
            a.verb              = EVerb::Clarify;
            a.clarifyQuestion   = "which one?";
            a.clarifyCandidates = win.candidates;
            a.confidence        = 0.4;
            return a;
        }
        if (win.matched) {
            a.windowAddress = win.address;
            a.windowLabel   = win.label;
            a.targetSource  = ETargetSource::Semantic;
            a.confidence    = std::min(a.confidence, win.confidence);
            return a;
        }
        // Nothing named. Fullscreen still has a subject (the focused window); a bare
        // "focus" does not, so it asks rather than re-focusing whatever is already up.
        if (raw.verb == EVerb::Focus && !raw.deictic && !(gz.valid && !gz.name.empty())) {
            a.verb            = EVerb::Clarify;
            a.clarifyQuestion = raw.targetPhrase.empty() ? "focus what?"
                                                         : "I don't see " + raw.targetPhrase;
            a.confidence      = 0.3;
            return a;
        }
        if (gz.valid && !gz.name.empty() && ctx.hasMonitor(gz.name)) {
            // A deictic that landed on a live monitor: act there rather than on whatever
            // last held keyboard focus.
            a.target       = gz.name;
            a.targetSource = ETargetSource::Deixis;
            a.gaze         = gz;
            a.confidence   = gz.stable ? std::min(a.confidence, 0.9) : 0.6;
            return a;
        }
        a.target       = "active";
        a.targetSource = ETargetSource::Active;
        a.confidence   = std::min(a.confidence, 0.7);
        return a;
    }

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
