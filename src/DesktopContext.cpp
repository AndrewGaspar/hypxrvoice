#include "DesktopContext.hpp"

#include <jansson.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <memory>
#include <set>
#include <unordered_map>

namespace {
    // Tokenize on non-alphanumeric boundaries, lowercased. "XR-code" -> {xr, code};
    // "YouTube — Mozilla Firefox" -> {youtube, mozilla, firefox}.
    std::vector<std::string> tokenize(const std::string& s) {
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

    // Stop-words that carry no target identity in a spoken reference.
    bool isStop(const std::string& t) {
        static const std::set<std::string> kStop = {
            "the", "a", "an", "my", "this", "that", "these", "those", "it", "one",
            "monitor", "screen", "display", "window", "panel", "over", "there", "here",
            "please", "to", "of", "on", "app", "application", "show", "showing", "with"};
        return kStop.count(t) > 0;
    }

    // Fuzzy token match on a shared stem — cheap, deterministic, no external stemmer.
    // Two tokens match when they share a long-enough common prefix: exact wins; else
    // a common prefix of >=4, or >=3 that is within one char of the shorter token's
    // length. This catches inflections like "code"~"coding" (share "cod") and
    // "web"~"website" without letting "code"~"coffee" (share only "co") through.
    // Tokens shorter than 3 chars must match exactly.
    bool tokMatch(const std::string& a, const std::string& b) {
        if (a == b)
            return true;
        if (a.size() < 3 || b.size() < 3)
            return false;
        size_t L = 0;
        while (L < a.size() && L < b.size() && a[L] == b[L])
            L++;
        const size_t mn = std::min(a.size(), b.size());
        if (L >= 4)
            return true;
        return L >= 3 && L + 1 >= mn;
    }

    std::string jstr(json_t* o, const char* k) {
        json_t* v = json_object_get(o, k);
        return (v && json_is_string(v)) ? json_string_value(v) : "";
    }
    int jint(json_t* o, const char* k, int def) {
        json_t* v = json_object_get(o, k);
        return (v && json_is_integer(v)) ? static_cast<int>(json_integer_value(v)) : def;
    }
    bool jbool(json_t* o, const char* k) {
        json_t* v = json_object_get(o, k);
        return v && json_is_true(v);
    }
}

SDesktopContext SDesktopContext::parse(const std::string& monitorsJson,
                                       const std::string& clientsJson,
                                       const std::string& openxrJson) {
    SDesktopContext ctx;
    std::unordered_map<int, size_t> byId; // monitor id -> index in ctx.monitors

    // ---- monitors -j : the authoritative output list ----
    if (!monitorsJson.empty()) {
        json_error_t jerr{};
        if (json_t* root = json_loads(monitorsJson.c_str(), 0, &jerr)) {
            if (json_is_array(root)) {
                size_t  i;
                json_t* m;
                json_array_foreach(root, i, m) {
                    if (!json_is_object(m))
                        continue;
                    SMonitorInfo info;
                    info.name    = jstr(m, "name");
                    info.id      = jint(m, "id", -1);
                    info.focused = jbool(m, "focused");
                    info.xr      = info.name.rfind("XR-", 0) == 0;
                    if (json_t* aw = json_object_get(m, "activeWorkspace"); aw && json_is_object(aw))
                        info.workspace = jstr(aw, "name");
                    byId[info.id] = ctx.monitors.size();
                    ctx.monitors.push_back(std::move(info));
                }
            }
            json_decref(root);
        }
    }

    // ---- clients -j : attach apps to their monitor ----
    if (!clientsJson.empty()) {
        json_error_t jerr{};
        if (json_t* root = json_loads(clientsJson.c_str(), 0, &jerr)) {
            if (json_is_array(root)) {
                size_t  i;
                json_t* c;
                json_array_foreach(root, i, c) {
                    if (!json_is_object(c))
                        continue;
                    if (json_object_get(c, "mapped") && !jbool(c, "mapped"))
                        continue; // skip unmapped/hidden surfaces
                    int monId = jint(c, "monitor", -1);
                    auto it   = byId.find(monId);
                    if (it == byId.end())
                        continue;
                    SMonitorInfo& mon = ctx.monitors[it->second];
                    std::string   cls = jstr(c, "class");
                    std::string   ttl = jstr(c, "title");
                    if (!cls.empty() && std::find(mon.appClasses.begin(), mon.appClasses.end(), cls) == mon.appClasses.end())
                        mon.appClasses.push_back(cls);
                    if (!ttl.empty())
                        mon.appTitles.push_back(ttl);
                }
            }
            json_decref(root);
        }
    }

    // ---- openxr status : XR anchor/hover/grab annotations ----
    if (!openxrJson.empty()) {
        json_error_t jerr{};
        if (json_t* root = json_loads(openxrJson.c_str(), 0, &jerr)) {
            if (json_is_object(root)) {
                ctx.xrAvailable = true;
                ctx.xrState     = jstr(root, "state");
                if (json_t* mons = json_object_get(root, "monitors"); mons && json_is_array(mons)) {
                    size_t  i;
                    json_t* m;
                    json_array_foreach(mons, i, m) {
                        if (!json_is_object(m))
                            continue;
                        std::string name = jstr(m, "name");
                        SMonitorInfo* mon = nullptr;
                        for (auto& e : ctx.monitors)
                            if (e.name == name) { mon = &e; break; }
                        if (!mon) {
                            // XR monitor not in `monitors -j` (rare) — add it.
                            ctx.monitors.push_back(SMonitorInfo{});
                            mon       = &ctx.monitors.back();
                            mon->name = name;
                            mon->id   = jint(m, "id", -1);
                        }
                        mon->xr      = true;
                        mon->hovered = jbool(m, "hovered");
                        mon->grabbed = jbool(m, "grabbed");
                        if (json_t* a = json_object_get(m, "anchor"); a && json_is_object(a))
                            mon->anchorMode = jstr(a, "mode");
                    }
                }
            }
            json_decref(root);
        }
    }

    return ctx;
}

std::vector<std::string> SDesktopContext::monitorNames() const {
    std::vector<std::string> v;
    for (auto& m : monitors)
        v.push_back(m.name);
    return v;
}

bool SDesktopContext::hasMonitor(const std::string& name) const {
    return find(name) != nullptr;
}

const SMonitorInfo* SDesktopContext::find(const std::string& name) const {
    for (auto& m : monitors)
        if (m.name == name)
            return &m;
    return nullptr;
}

const SMonitorInfo* SDesktopContext::hoveredMonitor() const {
    const SMonitorInfo* hit = nullptr;
    for (auto& m : monitors) {
        if (m.hovered) {
            if (hit)
                return nullptr; // more than one — ambiguous
            hit = &m;
        }
    }
    return hit;
}

SMonitorMatch SDesktopContext::resolveMonitor(const std::string& phrase) const {
    SMonitorMatch res;
    std::vector<std::string> ptoks;
    for (auto& t : tokenize(phrase))
        if (!isStop(t))
            ptoks.push_back(t);
    if (ptoks.empty() || monitors.empty())
        return res;

    // Score each monitor: a phrase token scores if it matches any name/class/title
    // token. Name matches weigh more than app matches so "XR-code" beats an app that
    // merely mentions "code". Distinct phrase tokens matched drives the score, so a
    // 2-word reference that hits two facets ranks above a 1-word coincidence.
    struct Scored { const SMonitorInfo* mon; double score; int distinctHits; };
    std::vector<Scored> scored;
    for (auto& m : monitors) {
        std::vector<std::string> nameToks = tokenize(m.name);
        std::vector<std::string> appToks;
        for (auto& c : m.appClasses)
            for (auto& t : tokenize(c))
                appToks.push_back(t);
        for (auto& t : m.appTitles)
            for (auto& tk : tokenize(t))
                appToks.push_back(tk);
        if (!m.workspace.empty())
            for (auto& t : tokenize(m.workspace))
                appToks.push_back(t);

        double score        = 0.0;
        int    distinctHits = 0;
        for (auto& pt : ptoks) {
            bool nameHit = false, appHit = false;
            for (auto& nt : nameToks)
                if (tokMatch(pt, nt)) { nameHit = true; break; }
            if (!nameHit)
                for (auto& at : appToks)
                    if (tokMatch(pt, at)) { appHit = true; break; }
            if (nameHit) { score += 2.0; distinctHits++; }
            else if (appHit) { score += 1.0; distinctHits++; }
        }
        if (score > 0.0)
            scored.push_back({&m, score, distinctHits});
    }
    if (scored.empty())
        return res;

    std::stable_sort(scored.begin(), scored.end(),
                     [](const Scored& a, const Scored& b) { return a.score > b.score; });

    const double best = scored.front().score;
    // Collect everything within a whisker of the best as candidates.
    std::vector<const SMonitorInfo*> top;
    for (auto& s : scored)
        if (s.score >= best - 0.001)
            top.push_back(s.mon);

    res.matched = true;
    res.name    = scored.front().mon->name;
    if (top.size() > 1) {
        // Ambiguous: two-plus monitors matched equally well.
        res.confidence = 0.4;
        for (auto* m : top)
            res.candidates.push_back(m->name);
    } else {
        // Unique winner. Confidence scales with how much of the phrase it explained
        // and how far ahead of the runner-up it is.
        double margin = scored.size() > 1 ? (best - scored[1].score) : best;
        double cover  = std::min(1.0, best / (2.0 * static_cast<double>(ptoks.size())));
        res.confidence = std::min(1.0, 0.6 + 0.2 * cover + 0.2 * std::min(1.0, margin / 2.0));
    }
    return res;
}

std::string SDesktopContext::digest(int maxMonitors, int maxAppsPerMon) const {
    // Deterministic, compact, human+model readable. One line per monitor.
    std::string o = "MONITORS (" + std::to_string(monitors.size()) + "):\n";
    int shown = 0;
    for (auto& m : monitors) {
        if (shown++ >= maxMonitors) {
            o += "  ... (" + std::to_string(monitors.size() - maxMonitors) + " more)\n";
            break;
        }
        o += "  " + m.name;
        if (m.xr) {
            o += " [xr";
            if (!m.anchorMode.empty())
                o += " anchor=" + m.anchorMode;
            if (m.hovered)
                o += " hovered";
            if (m.grabbed)
                o += " grabbed";
            o += "]";
        }
        if (m.focused)
            o += " [focused]";
        if (!m.workspace.empty())
            o += " ws=" + m.workspace;
        // apps: prefer class names (stable), fall back to titles.
        std::vector<std::string> apps = m.appClasses;
        if (apps.empty())
            apps = m.appTitles;
        if (!apps.empty()) {
            o += " apps=";
            for (int i = 0; i < static_cast<int>(apps.size()) && i < maxAppsPerMon; i++) {
                if (i) o += ",";
                o += apps[i];
            }
        }
        o += "\n";
    }
    if (xrAvailable)
        o += "XR session: " + xrState + "\n";
    else
        o += "XR session: (unavailable)\n";
    return o;
}

std::string defaultHyprctlQuery(const std::vector<std::string>& argv) {
    // Build a shell-free-ish command string. argv here is always our own constant
    // strings (never transcript text), so quoting is trivial and safe.
    std::string cmd;
    for (auto& a : argv) {
        if (!cmd.empty())
            cmd += ' ';
        cmd += a;
    }
    cmd += " 2>/dev/null";
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
    std::string out;
    if (pipe) {
        std::array<char, 8192> buf{};
        size_t                 n;
        while ((n = fread(buf.data(), 1, buf.size(), pipe.get())) > 0)
            out.append(buf.data(), n);
    }
    return out;
}

SDesktopContext snapshotDesktop(const QueryFn& runQuery) {
    std::string mons = runQuery({"hyprctl", "monitors", "-j"});
    std::string cls  = runQuery({"hyprctl", "clients", "-j"});
    std::string xr   = runQuery({"hyprctl", "-j", "openxr", "status"});
    return SDesktopContext::parse(mons, cls, xr);
}
