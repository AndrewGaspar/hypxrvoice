#include "VocabBias.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>

namespace {
    std::string lower(const std::string& s) {
        std::string o = s;
        std::transform(o.begin(), o.end(), o.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return o;
    }

    // Top-level domains. Their job here is to tell a HOSTNAME from a reverse-DNS app id
    // by looking at the LAST label only: "web.whatsapp.com" ends in a TLD and is a host
    // (the name is the second-level label), "com.mitchellh.ghostty" does not and is an
    // app id (the name is the last label). Looking at the FIRST label instead is what
    // made "web.whatsapp.com" resolve to nothing at all — "web" is not a TLD, but it is
    // not a name either, and the two questions are different.
    bool isTld(const std::string& t) {
        static const std::set<std::string> kSet = {
            "com", "org", "net", "io", "dev", "app", "co", "uk", "de", "fr", "eu",
            "tv", "me", "info", "xyz", "gg", "ai", "cloud", "site", "local", "localhost"};
        return kSet.count(t) > 0;
    }

    // Labels that are pure addressing or packaging, never something a person SAYS.
    // A superset of the TLDs.
    bool isAddressLabel(const std::string& t) {
        static const std::set<std::string> kSet = {
            "www", "web", "gnome", "kde", "freedesktop", "github", "gitlab", "desktop"};
        return isTld(t) || kSet.count(t) > 0;
    }

    // Browser "web app" wrappers put the real identity after a fixed prefix.
    const std::array<const char*, 5> kWrapperPrefixes = {"chrome-", "chromium-", "firefox-",
                                                         "brave-", "epiphany-"};

    bool allDigits(const std::string& s) {
        return !s.empty() && std::all_of(s.begin(), s.end(),
                                         [](unsigned char c) { return std::isdigit(c) != 0; });
    }

    std::vector<std::string> split(const std::string& s, char sep) {
        std::vector<std::string> out;
        std::string              cur;
        for (char c : s) {
            if (c == sep) {
                out.push_back(cur);
                cur.clear();
            } else
                cur += c;
        }
        out.push_back(cur);
        return out;
    }

    // Title stop-words: shell/editor chrome and generic desktop nouns that would burn a
    // vocabulary slot without ever being spoken as a target. Address labels ("web",
    // "com", "www") are stopped here too — "web.whatsapp.com" must contribute
    // "whatsapp", never "web".
    bool isTitleStop(const std::string& t) {
        static const std::set<std::string> kStop = {
            "the", "and", "for", "with", "new", "tab", "window", "monitor", "screen",
            "home", "main", "default", "untitled", "file", "edit", "view", "help",
            "http", "https", "html", "index", "page", "http2"};
        const std::string lt = lower(t);
        return kStop.count(lt) > 0 || isAddressLabel(lt);
    }

    // Case-insensitive presence test over the term list built so far.
    bool has(const std::vector<std::string>& v, const std::string& t) {
        const std::string lt = lower(t);
        return std::any_of(v.begin(), v.end(), [&](const std::string& x) { return lower(x) == lt; });
    }
}

std::string VocabBias::spokenClassName(const std::string& cls) {
    if (cls.empty())
        return "";
    std::string s = cls;

    // Chrome/Chromium append a profile suffix: "...__-Default", "...__Profile 1".
    const size_t prof = s.find("__");
    if (prof != std::string::npos)
        s = s.substr(0, prof);

    // Strip a browser-app wrapper prefix ("chrome-web.whatsapp.com" -> "web.whatsapp.com").
    for (const char* p : kWrapperPrefixes) {
        const std::string pfx = p;
        if (s.size() > pfx.size() && lower(s).compare(0, pfx.size(), pfx) == 0) {
            s = s.substr(pfx.size());
            break;
        }
    }

    // Dotted identifiers are either a hostname (web.whatsapp.com, app.slack.com) or a
    // reverse-DNS app id (com.mitchellh.ghostty, org.gnome.Nautilus). The LAST label
    // settles it: a TLD there means a host, and the name is the second-level label;
    // anything else means an app id, and the name is the last label.
    std::vector<std::string> parts = split(s, '.');
    if (parts.size() >= 2)
        s = isTld(lower(parts.back())) ? parts[parts.size() - 2] : parts.back();

    // Hyphen/underscore compounds name the vendor first and the app last
    // ("google-chrome" -> chrome, "org_kde_konsole" -> konsole).
    for (char sep : {'-', '_'}) {
        std::vector<std::string> hp = split(s, sep);
        if (hp.size() >= 2 && !hp.back().empty())
            s = hp.back();
    }

    // Trim anything non-alphanumeric off the ends (a trailing "-Default" leaves debris).
    size_t a = 0, b = s.size();
    while (a < b && !std::isalnum(static_cast<unsigned char>(s[a])))
        a++;
    while (b > a && !std::isalnum(static_cast<unsigned char>(s[b - 1])))
        b--;
    s = s.substr(a, b - a);

    if (s.size() < 3 || allDigits(s) || isAddressLabel(lower(s)))
        return "";
    return s;
}

std::vector<std::string> VocabBias::titleTerms(const std::string& title, const std::string& classTerm,
                                               size_t max) {
    std::vector<std::string> out;
    if (max == 0)
        return out;
    const std::string ct = lower(classTerm);

    // Tokenize on non-alphanumerics but KEEP the source casing — "Claude" must not come
    // back as "claude" when that is how it is written on screen.
    std::string cur;
    auto        flush = [&]() {
        if (cur.empty())
            return;
        std::string t = cur;
        cur.clear();
        // Letters only. A title token mixing letters and digits is an identifier, not a
        // word — a session hash, a build number, a video id. The live desktop offered
        // "RaldS7" as a vocabulary term before this line existed; nobody is going to say
        // that out loud, and every such token evicts one that might have been spoken.
        if (t.size() < 3 || !std::all_of(t.begin(), t.end(),
                                         [](unsigned char c) { return std::isalpha(c) != 0; }))
            return;
        if (isTitleStop(t) || lower(t) == ct)
            return;
        if (has(out, t))
            return;
        if (out.size() < max)
            out.push_back(t);
    };
    // Only the LEADING part of a title is scanned: the salient name comes first
    // ("btop", "nvim ~/.config/...", "Plex - ArchWiki - Google Chrome"), and the tail is
    // browser/app boilerplate that would crowd out real vocabulary.
    //
    // Scanning also STOPS at the first path character. An editor title is "<app>
    // <path>", and a path is pure noise as spoken vocabulary — it is what filled the
    // prompt with "config", "toml" and "src" while btop and nvtop, the words the user
    // actually says, were pushed out by the cap.
    size_t scanned = 0;
    for (char c : title) {
        if (out.size() >= max)
            break;
        if (++scanned > 48)
            break;
        if (c == '/' || c == '~' || c == '\\')
            break;
        if (std::isalnum(static_cast<unsigned char>(c)))
            cur += c;
        else
            flush();
    }
    if (out.size() < max)
        flush();
    return out;
}

std::vector<std::string> VocabBias::terms(const SDesktopContext& ctx, const SVocabBiasConfig& cfg) {
    std::vector<std::string> out;
    if (cfg.maxTerms <= 0)
        return out;

    // Focus-recency order: focusHistoryId 0 is the focused window. When the cap bites it
    // must drop the windows the speaker is LEAST likely to be naming, not an arbitrary
    // suffix of the compositor's list.
    std::vector<const SWindowInfo*> wins;
    wins.reserve(ctx.windows.size());
    for (auto& w : ctx.windows)
        wins.push_back(&w);
    std::stable_sort(wins.begin(), wins.end(), [](const SWindowInfo* a, const SWindowInfo* b) {
        return a->focusHistoryId < b->focusHistoryId;
    });

    // Pass 1: one term per window (its class name, else its best title token), so every
    // visible app is represented before any single app gets a second slot.
    std::vector<std::string> classOf(wins.size());
    for (size_t i = 0; i < wins.size() && static_cast<int>(out.size()) < cfg.maxTerms; i++) {
        classOf[i] = spokenClassName(wins[i]->cls);
        std::string term = classOf[i];
        if (term.empty()) {
            std::vector<std::string> tt = titleTerms(wins[i]->title, "", 1);
            if (!tt.empty())
                term = tt.front();
        }
        if (!term.empty() && !has(out, term))
            out.push_back(term);
    }

    // Pass 2: title keywords, ROUND-ROBIN — one per window per round. "btop", "nvtop"
    // and "nvim" are all ghostty windows: the class says nothing about them and they are
    // exactly what the user says out loud. Taking two tokens from the first window
    // before looking at the second is how the first terminal's title spent the whole
    // budget and btop never made it into the prompt at all.
    for (size_t round = 0; round < 2 && static_cast<int>(out.size()) < cfg.maxTerms; round++) {
        for (size_t i = 0; i < wins.size() && static_cast<int>(out.size()) < cfg.maxTerms; i++) {
            std::vector<std::string> tt = titleTerms(wins[i]->title, classOf[i], round + 1);
            if (tt.size() <= round)
                continue;
            if (!has(out, tt[round]))
                out.push_back(tt[round]);
        }
    }

    // Monitor names last: they are OUR names ("XR-main", "XR-2"), short, and already
    // near-unambiguous — they are worth biasing but not worth evicting an app for.
    int mons = 0;
    for (auto& m : ctx.monitors) {
        if (mons >= cfg.maxMonitors || static_cast<int>(out.size()) >= cfg.maxTerms + cfg.maxMonitors)
            break;
        if (m.name.empty() || has(out, m.name))
            continue;
        out.push_back(m.name);
        mons++;
    }
    return out;
}

std::string VocabBias::build(const SDesktopContext& ctx, const SVocabBiasConfig& cfg) {
    if (!cfg.enabled)
        return "";

    const std::vector<std::string> v = terms(ctx, cfg);

    // Monitor names, for the "move X to <monitor>" exemplar shape.
    std::vector<std::string> mons;
    for (auto& m : ctx.monitors) {
        if (static_cast<int>(mons.size()) >= cfg.maxMonitors)
            break;
        if (!m.name.empty())
            mons.push_back(m.name);
    }

    // Each term goes into a DIFFERENT command shape. Rotating the verb/preposition is
    // what makes the bias generalize: the measured win came from the nouns sitting in
    // real command trajectories, and varying the trajectory keeps the prompt from
    // teaching the decoder one sentence by rote.
    std::string o;
    size_t      mi = 0;
    for (size_t i = 0; i < v.size(); i++) {
        switch (i % 4) {
            case 0: o += "Move " + v[i] + " here. "; break;
            case 1: o += "Focus " + v[i] + ". "; break;
            case 2: o += "Move " + v[i] + " to this monitor. "; break;
            default: {
                // Never emit "Move XR-3 to XR-3." — the monitor names are themselves in
                // the term list, so the rotation can otherwise pair one with itself.
                std::string dest;
                for (size_t k = 0; k < mons.size(); k++) {
                    const std::string& cand = mons[(mi + k) % mons.size()];
                    if (lower(cand) != lower(v[i])) {
                        dest = cand;
                        mi += k + 1;
                        break;
                    }
                }
                if (!dest.empty())
                    o += "Move " + v[i] + " to " + dest + ". ";
                else
                    o += "Put " + v[i] + " here. ";
                break;
            }
        }
    }

    // Fixed tail: the command verbs and a SPELLED-OUT workspace number. Present even on
    // an empty desktop, because the verb vocabulary is what turns "four" into a
    // workspace index instead of "for"/"forward" (a live round-6 failure), "full screen"
    // into "fullscreen", and rescues "closer".
    //
    // It used to end "... Focus the browser." That sentence was REMOVED after it was
    // measured driving whisper into a repetition loop — decoding a real recording of the
    // user saying exactly "Focus the browser." produced "Focus the browser." fifty-five
    // times over, which whisper's temperature-fallback ladder then spent FIVE extra
    // decode passes repairing: 1.2 s became 5.8-7.4 s, reproducibly, on that one
    // utterance. Deleting the sentence from the prompt returned it to 1.6 s with the same
    // (correct) text. Note the fallback is doing its job and must stay on —
    // temperature_inc = 0 was tried and simply let the fifty-five repetitions through.
    //
    // It is the only tail sentence with no unique job: "focus" is already in the rotating
    // templates above, and "the browser" is a generic noun the intent tier resolves from
    // its own closed table, not something the ASR needs vocabulary for. The lesson
    // generalizes — a bias prompt earns its keep with NAMES the decoder has never seen,
    // not with whole sentences the user is about to say verbatim.
    o += "Move workspace four to this monitor. Make this window fullscreen. "
         "Move this monitor closer.";
    return o;
}
