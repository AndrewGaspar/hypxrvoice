#include "GazeResolver.hpp"

#include <jansson.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>

namespace {
    void readVec(json_t* obj, const char* key, double* out, int n) {
        json_t* a = json_object_get(obj, key);
        if (!a || !json_is_array(a))
            return;
        for (int i = 0; i < n && i < static_cast<int>(json_array_size(a)); i++) {
            json_t* e = json_array_get(a, i);
            if (json_is_number(e))
                out[i] = json_number_value(e);
        }
    }
}

SGazeSample SGazeSample::parse(const std::string& json) {
    SGazeSample s;
    if (json.empty())
        return s;
    json_error_t jerr{};
    json_t*      root = json_loads(json.c_str(), 0, &jerr);
    if (!root)
        return s;
    if (!json_is_object(root)) {
        json_decref(root);
        return s;
    }

    json_t* ok = json_object_get(root, "ok");
    s.ok = ok ? json_is_true(ok) : true; // tolerate replies that omit ok
    if (!s.ok) {
        json_decref(root);
        return s;
    }
    if (json_t* v = json_object_get(root, "viewValid"))
        s.viewValid = json_is_true(v);
    if (json_t* t = json_object_get(root, "timestampMs"); t && json_is_integer(t))
        s.timestampMs = json_integer_value(t);

    if (json_t* h = json_object_get(root, "head"); h && json_is_object(h)) {
        readVec(h, "pos", s.pos, 3);
        readVec(h, "quat", s.quat, 4);
        readVec(h, "forward", s.forward, 3);
    }
    if (json_t* g = json_object_get(root, "gaze"); g && json_is_object(g)) {
        // Forward-compat: use a real ray/quad intersection if the compositor ever grows
        // one. Today's reply has no such field, so this stays false.
        for (const char* k : {"hitPoint", "point"}) {
            json_t* hp = json_object_get(g, k);
            if (hp && json_is_array(hp) && json_array_size(hp) >= 3) {
                readVec(g, k, s.hit, 3);
                s.hasHit = true;
                break;
            }
        }
        if (json_t* m = json_object_get(g, "monitorId"); m && json_is_integer(m))
            s.monitorId = static_cast<int>(json_integer_value(m));
        if (json_t* n = json_object_get(g, "name"); n && json_is_string(n))
            s.name = json_string_value(n);
        if (json_t* sel = json_object_get(g, "selected"))
            s.selected = json_is_true(sel);
        if (json_t* d = json_object_get(g, "dwellSec"); d && json_is_number(d))
            s.dwellSec = json_number_value(d);
    }
    if (json_t* q = json_object_get(root, "query"); q && json_is_object(q)) {
        if (json_t* r = json_object_get(q, "requestedTimestampMs"); r && json_is_integer(r))
            s.requestedMs = json_integer_value(r);
        if (json_t* m = json_object_get(q, "matchedTimestampMs"); m && json_is_integer(m))
            s.matchedMs = json_integer_value(m);
        if (json_t* a = json_object_get(q, "ageMs"); a && json_is_integer(a))
            s.ageMs = json_integer_value(a);
    }
    if (s.matchedMs == 0)
        s.matchedMs = s.timestampMs;

    json_decref(root);
    return s;
}

std::string defaultGazeQuery(int64_t atMs) {
    std::string cmd = "hyprctl -j openxr gaze at " + std::to_string(atMs) + " 2>/dev/null";
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
    std::string out;
    if (pipe) {
        std::array<char, 4096> buf{};
        size_t                 n;
        while ((n = fread(buf.data(), 1, buf.size(), pipe.get())) > 0)
            out.append(buf.data(), n);
    }
    return out;
}

void projectPlacePoint(const double pos[3], const double forward[3], bool hasHit,
                       const double hit[3], double distanceM, double minDistanceM,
                       double out[3], double* outDistM) {
    // Ray direction: the head forward, normalized. A zero/NaN forward (no view pose in
    // the sample) degrades to LOCAL_FLOOR forward rather than to "no direction" — the
    // one outcome we refuse is a point at the origin of the ray.
    double dir[3] = {forward[0], forward[1], forward[2]};
    double len    = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!(len > 1e-6) || !std::isfinite(len)) {
        dir[0] = 0.0; dir[1] = 0.0; dir[2] = -1.0;
        len    = 1.0;
    }
    for (double& d : dir)
        d /= len;

    const double minD = std::max(0.0, minDistanceM);
    // Projection distance: a non-positive or absurd config value falls back to the
    // floor, never to zero (zero is exactly the bug this function exists to prevent).
    double projD = distanceM;
    if (!std::isfinite(projD) || projD <= 0.0)
        projD = std::max(minD, 1.3);

    double p[3];
    if (hasHit) {
        p[0] = hit[0]; p[1] = hit[1]; p[2] = hit[2];
    } else {
        for (int i = 0; i < 3; i++)
            p[i] = pos[i] + dir[i] * projD;
    }

    // Hard floor: no candidate point — projected OR compositor-reported — may sit inside
    // the sphere of radius minD around the head. Anything that does is pushed back out
    // along the ray.
    double d[3] = {p[0] - pos[0], p[1] - pos[1], p[2] - pos[2]};
    double dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!std::isfinite(dist) || dist < minD) {
        for (int i = 0; i < 3; i++)
            p[i] = pos[i] + dir[i] * minD;
        dist = minD;
    }

    for (int i = 0; i < 3; i++)
        out[i] = p[i];
    if (outDistM)
        *outDistM = dist;
}

SGazeResolution resolveDeixis(int64_t wordMs, const SGazeConfig& cfg,
                              const GazeQueryFn& query, const SDesktopContext* ctx) {
    SGazeResolution r;
    r.requestedMs = wordMs;

    const int64_t anchor = wordMs - cfg.leadMs;      // lead-shifted target.
    const int     n      = std::max(1, cfg.samples);
    const int64_t span   = std::max(0, cfg.windowMs);

    // Sample evenly across [anchor-span, anchor]. Newest (anchor) first isn't required;
    // we tally the modal candidate.
    std::vector<SGazeSample> samples;
    for (int i = 0; i < n; i++) {
        int64_t at = anchor;
        if (n > 1)
            at = anchor - span + (span * i) / (n - 1);
        SGazeSample s = SGazeSample::parse(query(at));
        if (s.ok)
            samples.push_back(s);
    }
    r.sampleCount = static_cast<int>(samples.size());
    if (samples.empty())
        return r;

    // Modal gaze candidate across the window. A miss (monitorId==-1) is a real vote —
    // "here" while looking at passthrough is a legitimate world-point deixis.
    std::map<int, int>    votes;    // monitorId -> count
    std::map<int, double> bestDwell;// monitorId -> max dwell seen
    for (auto& s : samples) {
        int id = s.selected ? s.monitorId : -1;
        votes[id]++;
        bestDwell[id] = std::max(bestDwell[id], s.dwellSec);
    }
    // Winner = most votes, tie broken by higher dwell.
    int    winner    = -1;
    int    winVotes  = -1;
    double winDwell  = -1.0;
    for (auto& [id, v] : votes) {
        if (v > winVotes || (v == winVotes && bestDwell[id] > winDwell)) {
            winner   = id;
            winVotes = v;
            winDwell = bestDwell[id];
        }
    }

    // Representative sample: the agreeing sample with the highest dwell (most settled).
    const SGazeSample* rep = nullptr;
    for (auto& s : samples) {
        int id = s.selected ? s.monitorId : -1;
        if (id != winner)
            continue;
        if (!rep || s.dwellSec > rep->dwellSec)
            rep = &s;
    }
    if (!rep)
        rep = &samples.back();

    r.valid       = true;
    r.monitorId   = winner;
    r.name        = rep->name;
    r.dwellSec    = rep->dwellSec;
    r.matchedMs   = rep->matchedMs ? rep->matchedMs : rep->timestampMs;
    r.ageMs       = r.requestedMs - r.matchedMs;
    r.agreeCount  = winVotes;
    r.stable      = winVotes * 2 > r.sampleCount; // strict majority agreed
    for (int i = 0; i < 3; i++) {
        r.pos[i]     = rep->pos[i];
        r.forward[i] = rep->forward[i];
    }
    for (int i = 0; i < 4; i++)
        r.quat[i] = rep->quat[i];

    // The point "here" designates. Computed HERE, once, so every consumer reads the same
    // projected point and none of them can accidentally place at the head origin.
    r.placeFromHit = rep->hasHit;
    projectPlacePoint(r.pos, r.forward, rep->hasHit, rep->hit, cfg.placeDistanceM,
                      cfg.placeMinDistanceM, r.place, &r.placeDistM);

    // If the picked candidate names a monitor that is no longer live, drop the name
    // (still a valid world-point deixis, just not a monitor pick).
    if (ctx && winner >= 0 && !r.name.empty() && !ctx->hasMonitor(r.name)) {
        r.name      = "";
        r.monitorId = -1;
    }
    return r;
}
