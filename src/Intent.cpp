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

    // A cardinal in the WORKSPACE NUMBER SLOT, where Whisper's homophone errors are
    // both predictable and unambiguous. Round 5: "move workspace 4 to this monitor"
    // came back as "Move workspace forward to this monitor" — the number became a
    // direction word and the whole utterance turned into a window move.
    //
    // This is deliberately NOT general fuzzy matching, and it is NEVER applied outside
    // the slot immediately following a "workspace" token: "for"/"to"/"won" are ordinary
    // English everywhere else, and treating them as digits anywhere else would invent
    // commands. Inside the slot, no competing reading exists — "workspace to" is not a
    // sentence anyone says.
    int workspaceNumber(const std::string& tok) {
        if (int n = cardinal(tok); n >= 0)
            return n;
        struct { const char* w; int n; } kHomophones[] = {
            {"for", 4}, {"fore", 4}, {"forward", 4}, // "workspace 4"
            {"to", 2},  {"too", 2},                  // "workspace 2"
            {"ate", 8},                              // "workspace 8"
            {"won", 1},                              // "workspace 1"
            {"tree", 3}, {"free", 3},                // "workspace 3"
        };
        for (auto& h : kHomophones)
            if (tok == h.w)
                return h.n;
        return -1;
    }

    // The workspace index for a phrase whose "workspace" token ends at `from` (the index
    // findCompound returns). The token IMMEDIATELY after "workspace" is the number slot
    // and gets the homophone tolerance; any later token must be a real cardinal. Keeping
    // the tolerance to one token is what stops "…to the left" from ever reading as a
    // number. Returns -1 when the phrase names no index.
    int scanWorkspaceIndex(const std::vector<std::string>& toks, int from) {
        if (from < 0)
            return -1;
        size_t i = static_cast<size_t>(from);
        if (i < toks.size())
            if (int n = workspaceNumber(toks[i]); n >= 0)
                return n;
        for (; i < toks.size(); i++)
            if (int n = cardinal(toks[i]); n >= 0)
                return n;
        return -1;
    }

    // Determiners and fillers that carry no identity in a destination phrase.
    bool isDeterminer(const std::string& t) {
        return t == "the" || t == "a" || t == "an" || t == "my" || t == "our";
    }

    // Words that say "the thing I am naming is an output", not part of its name.
    bool isMonitorNoun(const std::string& t) {
        return t == "monitor" || t == "screen" || t == "display";
    }

    // Politeness/rhythm words that can sit after the last content word without changing
    // what was said. Stripped only from the TAIL, so they never eat a real reference.
    bool isTrailingFiller(const std::string& t) {
        return t == "please" || t == "now" || t == "ok" || t == "okay" || t == "thanks";
    }

    // "here"/"there" — a PLACE deixis. Used as a destination it names the monitor the
    // user was looking at when the word was spoken.
    bool isPlaceDeicticWord(const std::string& t) { return t == "here" || t == "there"; }

    // The prepositions that can introduce a move's destination. People do not consistently
    // say "to": the live round produced "move workspace forward IN this monitor", which the
    // to-only grammar dropped on the floor and the switch verb then swallowed. All of these
    // read identically before an output reference, and none of them can introduce one
    // without a destination phrase that still has to name an output (see parseMonitorDest),
    // so widening the set cannot invent a target.
    bool isDestPreposition(const std::string& t) {
        return t == "to" || t == "onto" || t == "on" || t == "over" || t == "in" || t == "at";
    }

    // The index of a TRAILING "here"/"there" (ignoring trailing filler), or -1. Trailing
    // is the whole point: "move Plex here" ends in a destination, whereas "put this here
    // on workspace 3" does not — there the deictic is not the last thing said, so it is
    // not a destination remnant.
    int trailingPlaceDeictic(const std::vector<std::string>& toks) {
        size_t i = toks.size();
        while (i > 0 && isTrailingFiller(toks[i - 1]))
            i--;
        if (i == 0)
            return -1;
        return isPlaceDeicticWord(toks[i - 1]) ? static_cast<int>(i) - 1 : -1;
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

    // Split "<verb> <subject> to <destination>" into its two halves. Returns false when
    // the utterance is not of that shape at all. `vi` receives the verb index, `subject`
    // the words between verb and preposition, `dest` the destination words (determiners
    // stripped). Shared by the window-move and workspace-move parsers, which differ only
    // in what the SUBJECT is allowed to be.
    //
    // Round 6: a bare TRAILING "here"/"there" is a destination in its own right, with no
    // preposition at all. "Move Plex here." and "Move workspace two here." were both
    // spoken on the live round and both failed to parse — the first fell out of the
    // grammar entirely, the second degraded to a workspace SWITCH — because the only
    // destination shape accepted was "to <monitor-ref>". `destDeictic` reports that case
    // so the caller resolves the destination through the gaze ring.
    bool splitMovePhrase(const std::vector<std::string>& toks, size_t& vi,
                         std::vector<std::string>& subject, std::vector<std::string>& dest,
                         bool& destDeictic) {
        static const char* kVerbs[] = {"move", "put", "send", "throw", "drag", "shift"};
        destDeictic = false;
        vi = toks.size();
        for (size_t i = 0; i < toks.size() && vi == toks.size(); i++)
            for (auto* v : kVerbs)
                if (toks[i] == v) { vi = i; break; }
        if (vi == toks.size())
            return false;

        // The preposition that introduces the destination.
        const int trailingDeictic = trailingPlaceDeictic(toks);
        size_t    pi              = toks.size();
        for (size_t i = vi + 1; i < toks.size(); i++)
            if (isDestPreposition(toks[i])) {
                // "move workspace TO here": the token sits in the workspace NUMBER SLOT
                // and the whole remaining destination is a bare deixis, so this is
                // Whisper's "two", not a preposition (the same reading workspaceNumber()
                // already gives it, for the same reason: "workspace to here" is not a
                // sentence anyone says). Any other shape — "move workspace to the left
                // monitor" — keeps the prepositional reading.
                if (i > 0 && toks[i - 1] == "workspace" && trailingDeictic == static_cast<int>(i) + 1)
                    continue;
                pi = i;
                break;
            }

        if (pi == toks.size()) {
            // No preposition. The utterance is only a move if it ENDS in a place-deixis;
            // anything else ("move it closer") belongs to another verb.
            const int ti = trailingDeictic;
            if (ti < 0 || static_cast<size_t>(ti) <= vi)
                return false;
            size_t end = static_cast<size_t>(ti);
            // "…right here", "…over here": a locative modifier of the deictic, never part
            // of the subject's name.
            while (end > vi + 1 &&
                   (toks[end - 1] == "right" || toks[end - 1] == "over" || toks[end - 1] == "in"))
                end--;
            subject.assign(toks.begin() + static_cast<long>(vi) + 1, toks.begin() + static_cast<long>(end));
            if (subject.empty())
                return false;
            dest.assign(1, toks[static_cast<size_t>(ti)]);
            destDeictic = true;
            return true;
        }

        size_t d = pi + 1;
        if (d < toks.size() && toks[d] == "to")
            d++; // "over to the left monitor"
        if (d >= toks.size())
            return false;

        subject.assign(toks.begin() + static_cast<long>(vi) + 1, toks.begin() + static_cast<long>(pi));
        if (subject.empty())
            return false;

        dest.assign(toks.begin() + static_cast<long>(d), toks.end());
        while (!dest.empty() && isDeterminer(dest.front()))
            dest.erase(dest.begin());
        while (!dest.empty() && isTrailingFiller(dest.back()))
            dest.pop_back();
        if (dest.empty())
            return false;
        // "…over here" / "…on there": the preposition led to a deixis, not to a name.
        destDeictic = dest.size() == 1 && isPlaceDeicticWord(dest.front());
        return true;
    }

    // A MONITOR destination: spatial ("…to the left"), a phrase that SAYS it is an output
    // ("…to the coding monitor", "…to this monitor"), or a bare place-deixis ("…here").
    // Fills r.spatial / r.monitorPhrase / r.destDeictic. False when the destination names
    // no output at all — without that requirement "move this closer to me" parses as a
    // move-to-"me", which is exactly the kind of confident nonsense the closed grammar
    // exists to prevent.
    bool parseMonitorDest(const std::vector<std::string>& dest, bool destDeictic, SRawIntent& r) {
        if (destDeictic) {
            // "…here" resolves against the gaze ring in finalize, exactly as "…to this
            // monitor" does: the MONITOR that was under gaze at word time. It is NOT the
            // projected place point — a window and a workspace land on an output.
            r.destDeictic = true;
            return true;
        }
        if (dest.front() == "left" || dest.front() == "leftmost")
            r.spatial = ESpatialRef::Left;
        else if (dest.front() == "right" || dest.front() == "rightmost")
            r.spatial = ESpatialRef::Right;
        if (r.spatial != ESpatialRef::None)
            return true;

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
        r.monitorPhrase = mon;
        return true;
    }

    // Does the tail from `from` carry a DESTINATION — a trailing "here"/"there", or a
    // preposition introducing something that names an output? Used by the workspace
    // SWITCH branch, which cannot honor a destination at all: if one is present, the
    // utterance is a move whose verb the ASR lost, not a switch. Fills `r` with whichever
    // destination it found, so finalize resolves it exactly like a parsed move.
    bool parseTrailingDestination(const std::vector<std::string>& toks, size_t from, SRawIntent& r) {
        if (const int ti = trailingPlaceDeictic(toks); ti >= static_cast<int>(from)) {
            r.destDeictic = true;
            return true;
        }
        for (size_t i = from; i < toks.size(); i++) {
            if (!isDestPreposition(toks[i]))
                continue;
            size_t d = i + 1;
            if (d < toks.size() && toks[d] == "to")
                d++;
            std::vector<std::string> dest(toks.begin() + static_cast<long>(d), toks.end());
            while (!dest.empty() && isDeterminer(dest.front()))
                dest.erase(dest.begin());
            while (!dest.empty() && isTrailingFiller(dest.back()))
                dest.pop_back();
            if (dest.empty())
                continue;
            if (parseMonitorDest(dest, false, r))
                return true;
        }
        return false;
    }

    // "move workspace <N> to <monitor>" — relocate a WHOLE workspace onto another output.
    // Checked BEFORE the window move: the subject here is a workspace index, and letting
    // it fall through to the window parser is what turned a misheard "move workspace 4"
    // into a movewindow on whatever happened to be focused (round 5).
    //
    // The workspace token must sit in the SUBJECT half — "move the terminal to workspace
    // three" keeps its existing reading, because there "workspace" is the destination.
    bool parseWorkspaceMove(const std::vector<std::string>& toks, SRawIntent& r) {
        size_t                   vi = 0;
        bool                     destDeictic = false;
        std::vector<std::string> subject, dest;
        if (!splitMovePhrase(toks, vi, subject, dest, destDeictic))
            return false;

        const int wsAt = findCompound(subject, "workspace", "work", "space");
        if (wsAt < 0)
            return false;
        // The destination must be a MONITOR. "move workspace 3 to workspace 4" is not a
        // thing; declining leaves it unparsed rather than guessing.
        if (findCompound(dest, "workspace", "work", "space") >= 0)
            return false;
        if (!parseMonitorDest(dest, destDeictic, r))
            return false;

        // The number slot (homophone-tolerant — see workspaceNumber). Absent is
        // underspecified, not out of scope: finalize turns 0 into a Clarify.
        r.workspace = std::max(0, scanWorkspaceIndex(subject, wsAt));
        return true;
    }

    // "create/add/make a [new] monitor [here]". The monitor noun must FOLLOW the creation
    // verb with nothing but determiners and new-ish adjectives between them. Merely
    // containing both words is not enough: "make this monitor follow me" and "make this
    // window fullscreen" both do, and neither creates anything. The caller also requires
    // that no workspace/fullscreen compound is present.
    bool parseCreateMonitor(const std::vector<std::string>& toks) {
        static const char* kVerbs[]  = {"create", "add", "spawn", "make", "open"};
        static const char* kFiller[] = {"new", "another", "second", "third", "extra",
                                        "more", "me", "us", "up"};
        for (size_t i = 0; i < toks.size(); i++) {
            bool isVerb = false;
            for (auto* v : kVerbs)
                if (toks[i] == v) { isVerb = true; break; }
            if (!isVerb)
                continue;
            for (size_t j = i + 1; j < toks.size(); j++) {
                if (isMonitorNoun(toks[j]))
                    return true;
                if (isDeterminer(toks[j]))
                    continue;
                bool filler = false;
                for (auto* f : kFiller)
                    if (toks[j] == f) { filler = true; break; }
                if (!filler)
                    break; // a content word intervened — not a creation phrase
            }
        }
        return false;
    }

    // Is this window phrase a bare DEIXIS ("this", "that", "it", "this window")? Those
    // legitimately mean "the window I am using", so they keep the focused-window reading.
    // Everything else — any phrase with content words in it — must MATCH a live window or
    // be refused; see the guard in finalizeAction.
    bool isDeicticWindowPhrase(const std::string& phrase) {
        static const char* kFiller[]  = {"the", "a", "an", "my", "our", "window", "windows",
                                         "one", "thing", "app", "please"};
        static const char* kDeictic[] = {"this", "that", "it", "current", "active", "focused",
                                         "here", "there"};
        bool sawDeictic = false;
        for (auto& t : words(phrase)) {
            bool ok = false;
            for (auto* d : kDeictic)
                if (t == d) { sawDeictic = true; ok = true; break; }
            if (ok)
                continue;
            for (auto* f : kFiller)
                if (t == f) { ok = true; break; }
            if (!ok)
                return false;
        }
        return sawDeictic;
    }

    // "move/put/send <window> to <destination>" — the one command that names a WINDOW and
    // a place to put it. Returns true only when BOTH halves parsed; the caller then treats
    // the utterance as a window move and skips the rest of the keyword chain.
    bool parseWindowMove(const std::vector<std::string>& toks, SRawIntent& r) {
        size_t                   vi = 0;
        bool                     destDeictic = false;
        std::vector<std::string> subject, dest;
        if (!splitMovePhrase(toks, vi, subject, dest, destDeictic))
            return false;

        // A WORKSPACE is never a window. "move workspace forward to this monitor" must
        // never reach the window resolver — the phrase either belongs to the workspace-move
        // verb (checked first) or is a mis-parse, and both are better than operating on
        // whatever window happens to be focused.
        if (findCompound(subject, "workspace", "work", "space") >= 0)
            return false;

        std::string win;
        for (auto& t : subject) {
            if (!win.empty()) win += ' ';
            win += t;
        }
        if (win.empty())
            return false;

        // Round 6. A bare-"here" destination makes this a WINDOW move only when the
        // subject actually names a window. Two subjects are declined outright, and both
        // fall through to the keyword chain that already reads them correctly:
        //
        //  * A BARE DEIXIS — "put it here", "place this here", "move this here". That is
        //    two deictics in one utterance, which the locked interaction model does not
        //    have (content-first semantic refs, ONE trailing deictic resolved at word
        //    time), and it is exactly how the XR place verb is spoken. Swallowing it here
        //    would steal "put it here" from Place — a verb the user issues daily — to
        //    guess at a window nobody named. "Move this to this monitor" remains the way
        //    to move the focused window by gaze: one deictic, at the destination.
        //  * A subject that SAYS it is an output — "put the coding monitor here". That is
        //    the XR place verb naming its monitor, not a window reference.
        if (destDeictic) {
            if (isDeicticWindowPhrase(win))
                return false;
            for (auto& t : subject)
                if (isMonitorNoun(t))
                    return false;
        }

        // A workspace destination: "…to workspace three". No number is underspecified,
        // not out of scope — finalize turns that into a Clarify.
        if (int wsAt = findCompound(dest, "workspace", "work", "space"); wsAt >= 0) {
            r.windowPhrase = win;
            r.sub          = "workspace";
            r.workspace    = std::max(0, scanWorkspaceIndex(dest, wsAt));
            return true;
        }

        if (!parseMonitorDest(dest, destDeictic, r))
            return false;
        r.windowPhrase = win;
        return true;
    }

}

namespace {
    // Did the user actually SPEAK a distance? A spoken amount always carries a UNIT.
    // Requiring the unit is what keeps an incidental number ("move monitor 2 closer")
    // from being read as a magnitude, and it is the only evidence that an amount was
    // asked for at all.
    bool utteranceNamesADistance(const std::string& utterance) {
        static const char* kUnits[] = {
            "meter", "meters", "metre", "metres", "cm", "cms",
            "centimeter", "centimeters", "centimetre", "centimetres",
            "inch", "inches", "foot", "feet"};
        for (auto& t : words(utterance))
            for (auto* u : kUnits)
                if (t == u)
                    return true;
        return false;
    }
}

double sanitizeDeltaM(double modelDelta, const std::string& utterance, double step) {
    std::string low;
    low.reserve(utterance.size());
    for (char c : utterance)
        low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    const double fallback = std::abs(step) > 0.0 ? std::abs(step) : 0.25;

    // Magnitude. A bare direction word ("move closer") names no amount, so it is ALWAYS
    // the configured step — whatever number the backend attached. Live: a 3B run answered
    // bare "move closer" with deltaM=-1.00, which passed the old [0.05, 1.0] clamp and put
    // the monitor on top of the user. The clamp only governs an amount the user actually
    // spoke ("move it half a meter closer"), where the model is reporting a quantity rather
    // than inventing one.
    double mag = fallback;
    if (utteranceNamesADistance(utterance)) {
        mag = std::abs(modelDelta);
        if (mag < 0.05 || mag > 1.0)
            mag = fallback;
    }

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
    // A WORKSPACE move is checked before the window move: its subject is an index, not a
    // window, and letting it fall through is what produced round 5's misfire.
    if (parseWorkspaceMove(toks, r)) {
        setVerb(EVerb::MoveWorkspace, "workspace-move keyword");
        return r;
    }
    if (parseWindowMove(toks, r)) {
        setVerb(EVerb::MoveWindow, "window-move keyword");
        return r; // the spans are already split; the generic phrase builder must not run
    }

    // "create a monitor here". Gated on there being no workspace/fullscreen compound so
    // "make this window fullscreen" keeps its reading.
    if (wsAt < 0 && fsAt < 0 && parseCreateMonitor(toks)) {
        setVerb(EVerb::CreateMonitor, "create-monitor keyword");
        return r; // the name is minted in finalize, from the live snapshot
    }

    // "workspace three" / "go to workspace 3" / "switch to workspace three". The
    // "workspace" token is REQUIRED: a bare trailing "3." (which is exactly what the
    // live round produced when the leading word was lost) must stay unparsed rather
    // than have us guess at a workspace switch.
    const int wsIndex = scanWorkspaceIndex(toks, wsAt);

    if (wsAt >= 0) {
        // Round 6. A DESTINATION after the workspace token — a trailing "here"/"there", or
        // any preposition leading to an output ("…in this monitor", "…on the left screen")
        // — is something the switch verb cannot honor: it would drop the destination and
        // switch anyway. The live round did exactly that, four times: "move workspace two
        // here" (x3) and "move workspace forward IN this monitor" all became switches. The
        // move parsers above have already had their chance at this utterance; reaching here
        // with a destination remnant means the move VERB was lost in transcription
        // ("*Re*move workspace 4 here"), not that a switch was meant — so read it as the
        // move it is, and let finalize ask when the destination cannot be resolved. An
        // EXPLICIT switch marker still wins: "go to workspace 3" names its verb outright,
        // and a stray remnant must not override a verb the user actually spoke.
        const bool switchMarker = contains(text, "go to") || contains(text, "switch to") ||
                                  contains(text, "take me to");
        SRawIntent dst;
        if (!switchMarker && parseTrailingDestination(toks, static_cast<size_t>(wsAt), dst)) {
            setVerb(EVerb::MoveWorkspace, "workspace-move (destination remnant, move verb lost)");
            r.workspace     = wsIndex >= 0 ? wsIndex : 0;
            r.spatial       = dst.spatial;
            r.monitorPhrase = dst.monitorPhrase;
            r.destDeictic   = dst.destDeictic;
            return r;
        }
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

    // The push/pull magnitude is settled HERE, for every backend, because a backend is
    // exactly what got it wrong: a bare "move closer" came back from the local LLM as
    // deltaM=-1.00 and landed the monitor on the wearer. sanitizeDeltaM is idempotent, so
    // running it again over a rule-backend action (which already carries ±distance_step_m)
    // changes nothing.
    if (raw.verb == EVerb::MoveDist)
        a.deltaM = sanitizeDeltaM(raw.deltaM, t.text, cfg.distanceStep);

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

    // The MONITOR half of a move: spatial (layout), then named/semantic, then deixis.
    // Shared by MoveWindow and MoveWorkspace. Sets a.target/a.targetSource on success;
    // on failure it turns `a` into a Clarify. Never falls back to "active" — the subject
    // would land somewhere the user did not ask for.
    auto resolveMoveDestination = [&](void) -> bool {
        SMonitorMatch mon;
        if (raw.spatial != ESpatialRef::None) {
            mon = ctx.resolveSpatialMonitor(raw.spatial);
            if (!mon.matched) {
                a.verb            = EVerb::Clarify;
                a.clarifyQuestion = "which monitor?";
                a.confidence      = 0.3;
                return false;
            }
        } else if (!raw.monitorPhrase.empty()) {
            mon = ctx.resolveMonitor(raw.monitorPhrase);
        }
        if (mon.matched && mon.candidates.size() > 1) {
            a.verb              = EVerb::Clarify;
            a.clarifyQuestion   = "which monitor?";
            a.clarifyCandidates = mon.candidates;
            a.confidence        = 0.4;
            return false;
        }
        if (mon.matched) {
            a.target       = mon.name;
            a.targetSource = raw.spatial != ESpatialRef::None ? ETargetSource::Semantic
                                                              : ETargetSource::Named;
            a.confidence   = std::min(a.confidence, mon.confidence);
            return true;
        }
        // "…to this monitor": the destination is wherever the user was looking.
        if (gz.valid && !gz.name.empty() && ctx.hasMonitor(gz.name)) {
            a.target       = gz.name;
            a.targetSource = ETargetSource::Deixis;
            a.gaze         = gz;
            a.confidence   = gz.stable ? std::min(a.confidence, 0.9) : 0.6;
            return true;
        }
        // "…here" with no monitor under gaze at word time. Say what would fix it, and
        // never guess: the active monitor is precisely where the user did NOT look.
        a.verb            = EVerb::Clarify;
        if (raw.destDeictic)
            a.clarifyQuestion = "look at the target monitor";
        else
            a.clarifyQuestion = raw.monitorPhrase.empty() ? "move it where?"
                                                          : "I don't see " + raw.monitorPhrase;
        a.confidence      = 0.3;
        return false;
    };

    // "move workspace 4 to this monitor". No window is involved at all: the subject is an
    // index we parsed and the destination a live output. Both halves must be concrete.
    if (raw.verb == EVerb::MoveWorkspace) {
        a.targetSource = ETargetSource::None;
        a.workspace    = raw.workspace;
        if (a.workspace < 1 || a.workspace > 99) {
            a.verb            = EVerb::Clarify;
            a.clarifyQuestion = "which workspace?";
            a.confidence      = 0.3;
            return a;
        }
        resolveMoveDestination();
        return a;
    }

    // "create a monitor here". The NAME is minted by us from the live snapshot — the
    // utterance never names one — and the deixis, if any, gives the place point.
    if (raw.verb == EVerb::CreateMonitor) {
        a.target = ctx.nextXrMonitorName();
        if (a.target.empty()) {
            a.verb            = EVerb::Clarify;
            a.clarifyQuestion = "no free XR monitor name";
            a.confidence      = 0.3;
            return a;
        }
        a.targetSource = ETargetSource::Named;
        if (gz.valid) {
            // "…here": the projected gaze point becomes the new monitor's placement.
            a.gaze       = gz;
            a.confidence = gz.stable ? std::min(a.confidence, 0.9) : 0.6;
        }
        return a;
    }

    // MoveWindow names BOTH: a window to relocate and a place to put it. The two halves
    // resolve against different live lists, and either half being ambiguous is a Clarify —
    // moving the wrong window, or moving the right one somewhere unexpected, are both
    // annoying to undo in a headset.
    if (raw.verb == EVerb::MoveWindow) {
        a.targetSource = ETargetSource::None;

        // 1. WHICH window. Content-first, exactly like focus/fullscreen.
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
        // A window phrase with CONTENT in it that matched NOTHING is a refusal, not a
        // fallback. It used to leave the address empty, which the executor reads as "the
        // focused window" — that is how a misheard "move workspace 4 …" dispatched a
        // movewindow on whatever was focused. Only a bare deixis ("this", "this window")
        // may still mean the focused one.
        if (!win.matched && !raw.windowPhrase.empty() && !isDeicticWindowPhrase(raw.windowPhrase)) {
            a.verb            = EVerb::Clarify;
            a.clarifyQuestion = "I don't see " + raw.windowPhrase;
            a.confidence      = 0.3;
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

        // 2b. A MONITOR destination.
        resolveMoveDestination();
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
