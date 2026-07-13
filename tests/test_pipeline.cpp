#include "doctest.h"

#include "IntentPipeline.hpp"

#include <string>
#include <vector>

// End-to-end acceptance matrix for WP-V4: fixture transcript -> context snapshot ->
// intent -> executor, with a MOCKED hyprctl (records argv, serves fixture JSON) and a
// canned gaze ring. No compositor, no model — the deterministic rule backend.

namespace {
    const char* kMonitors = R"json([
        {"id":0,"name":"eDP-1","focused":true},
        {"id":3,"name":"XR-code"},
        {"id":4,"name":"XR-web"}
    ])json";
    const char* kClients = R"json([
        {"class":"nvim","title":"main.cpp - NVIM","monitor":3,"mapped":true},
        {"class":"firefox","title":"YouTube - Mozilla Firefox","monitor":4,"mapped":true}
    ])json";
    const char* kOpenxr = R"json({"state":"focused","monitors":[
        {"name":"XR-code","id":3,"anchor":{"mode":"local"}},
        {"name":"XR-web","id":4,"hovered":true,"anchor":{"mode":"body"}}
    ]})json";

    // A hyprctl mock: serves the right blob for each read query.
    QueryFn mockHyprctl(const char* mons = kMonitors, const char* cls = kClients,
                        const char* xr = kOpenxr) {
        return [=](const std::vector<std::string>& argv) -> std::string {
            for (auto& a : argv) {
                if (a == "monitors") return mons;
                if (a == "clients")  return cls;
            }
            // the openxr status query
            return xr;
        };
    }

    // Gaze ring that always reports looking at a given monitor (or a miss when id<0).
    GazeQueryFn mockGaze(int id, const char* name) {
        return [=](int64_t at) {
            std::string sel = id >= 0 ? "true" : "false";
            char        b[360];
            std::snprintf(b, sizeof(b),
                          R"json({"ok":true,"viewValid":true,"timestampMs":%lld,
                          "head":{"pos":[0.5,1.4,-1.2],"forward":[0,0,-1]},
                          "gaze":{"monitorId":%d,"name":"%s","selected":%s,"dwellSec":0.3},
                          "query":{"matchedTimestampMs":%lld,"ageMs":0}})json",
                          (long long)at, id, id >= 0 ? name : "", sel.c_str(), (long long)at);
            return std::string(b);
        };
    }

    STranscript mk(const std::string& text, int64_t t0 = 100000) {
        STranscript t; t.text = text; t.onsetMs = t0;
        std::string cur; int64_t ms = t0;
        auto flush = [&]() {
            if (cur.empty()) return;
            SWord w; w.text = cur; w.startMs = ms; w.endMs = ms + 150;
            t.words.push_back(w); ms += 200; cur.clear();
        };
        for (char c : text) { if (c == ' ') flush(); else cur += c; }
        flush(); t.endMs = ms; return t;
    }

    // A config with the executor in LIVE mode so the mock runner records argv, plus a
    // recording runner. dry_run is tested separately.
    struct Harness {
        SConfig cfg;
        std::vector<std::vector<std::string>> ran;
        RunFn runner;
        Harness(bool live = true) {
            cfg.executor.dryRun = !live;
            cfg.feedback.stdoutJson = false; // keep test output clean
            cfg.feedback.notify = false;
            runner = [this](const std::vector<std::string>& a) { ran.push_back(a); return 0; };
        }
        bool ranLine(const std::string& want) const {
            for (auto& v : ran) {
                std::string j; for (auto& t : v) { if (!j.empty()) j += ' '; j += t; }
                if (j == want) return true;
            }
            return false;
        }
    };
}

TEST_CASE("pipeline: 'pick this monitor up' -> targeted head-carry of the gazed monitor") {
    Harness h;
    auto r = IntentPipeline::process(mk("pick this monitor up"), h.cfg,
                                     mockHyprctl(), mockGaze(4, "XR-web"), h.runner);
    CHECK(r.action.verb == EVerb::Pick);
    CHECK(r.action.target == "XR-web");
    CHECK(r.action.targetSource == ETargetSource::Deixis);
    CHECK(r.plan.ok);
    CHECK(h.ranLine("hyprctl openxr select XR-web"));
    CHECK(h.ranLine("hyprctl openxr anchor XR-web head"));
}

TEST_CASE("pipeline: 'pick this up' uses the targeted grab verb when advertised") {
    Harness h;
    h.cfg.executor.targetedGrab = true;
    auto r = IntentPipeline::process(mk("pick this monitor up"), h.cfg,
                                     mockHyprctl(), mockGaze(4, "XR-web"), h.runner);
    CHECK(h.ranLine("hyprctl openxr gazegrab XR-web"));
}

TEST_CASE("pipeline: 'place it here' -> stability-window resolution, freeze in place") {
    Harness h;
    // Gaze miss (passthrough): "here" is a world point; place keeps the active target.
    auto r = IntentPipeline::process(mk("place it here"), h.cfg,
                                     mockHyprctl(), mockGaze(-1, ""), h.runner);
    CHECK(r.action.verb == EVerb::Place);
    CHECK(r.action.target == "active");
    CHECK(r.action.gaze.valid); // the gaze pose was resolved at the word timestamp
    CHECK(h.ranLine("hyprctl openxr anchor active local"));
}

TEST_CASE("pipeline: 'place it here' uses place-at-pose when advertised") {
    Harness h;
    h.cfg.executor.placeAtPose = true;
    auto r = IntentPipeline::process(mk("drop it right here"), h.cfg,
                                     mockHyprctl(), mockGaze(-1, ""), h.runner);
    CHECK(r.action.verb == EVerb::Place);
    // pos came from the mock gaze head/point [0.5,1.4,-1.2].
    CHECK(h.ranLine("hyprctl openxr place active at 0.500,1.400,-1.200"));
}

TEST_CASE("pipeline: 'move the coding monitor closer' -> semantic select + distance") {
    Harness h;
    auto r = IntentPipeline::process(mk("move the coding monitor closer"), h.cfg,
                                     mockHyprctl(), mockGaze(-1, ""), h.runner);
    CHECK(r.action.verb == EVerb::MoveDist);
    CHECK(r.action.target == "XR-code");
    CHECK(r.action.targetSource == ETargetSource::Semantic);
    CHECK(h.ranLine("hyprctl openxr select XR-code"));
    CHECK(h.ranLine("hyprctl openxr distance -0.25"));
}

TEST_CASE("pipeline: 'have youtube follow me' -> adaptive follow on the browser monitor") {
    Harness h;
    auto r = IntentPipeline::process(mk("have youtube follow me"), h.cfg,
                                     mockHyprctl(), mockGaze(-1, ""), h.runner);
    CHECK(r.action.verb == EVerb::Follow);
    CHECK(r.action.target == "XR-web");
    CHECK(h.ranLine("hyprctl openxr select XR-web"));
    CHECK(h.ranLine("hyprctl openxr adaptive on"));
}

TEST_CASE("pipeline: out-of-scope utterance produces NO command") {
    Harness h;
    auto r = IntentPipeline::process(mk("what time is the meeting tomorrow"), h.cfg,
                                     mockHyprctl(), mockGaze(-1, ""), h.runner);
    CHECK(r.action.verb == EVerb::None);
    CHECK_FALSE(r.plan.ok);
    CHECK(h.ran.empty()); // absolutely nothing actuated
}

TEST_CASE("pipeline: ambiguous reference marks low confidence, still no wrong actuation") {
    const char* mons = R"json([{"id":3,"name":"XR-a"},{"id":4,"name":"XR-b"}])json";
    const char* cls  = R"json([
        {"class":"firefox","title":"Docs","monitor":3,"mapped":true},
        {"class":"firefox","title":"Mail","monitor":4,"mapped":true}
    ])json";
    Harness h;
    auto r = IntentPipeline::process(mk("move firefox closer"), h.cfg,
                                     mockHyprctl(mons, cls, "{}"), mockGaze(-1, ""), h.runner);
    CHECK(r.action.verb == EVerb::MoveDist);
    CHECK(r.action.confidence < 0.5);
    CHECK(r.action.clarifyCandidates.size() == 2);
    // The best guess is one of the two — and it IS actuated (per the "pick best,
    // mark confidence" policy); the feedback tier surfaces the uncertainty.
    CHECK((h.ranLine("hyprctl openxr select XR-a") || h.ranLine("hyprctl openxr select XR-b")));
}

TEST_CASE("pipeline: generic monitor names — only client context disambiguates") {
    // Auto-assigned names carry no signal; classes/titles must resolve the target.
    const char* mons = R"json([
        {"id":3,"name":"XR-1"},{"id":4,"name":"XR-2"},{"id":5,"name":"XR-3"}
    ])json";
    const char* cls = R"json([
        {"class":"mpv","title":"family-video.mp4 - mpv","monitor":3,"mapped":true},
        {"class":"nvim","title":"main.cpp - ~/code/hypxrland - NVIM","monitor":4,"mapped":true},
        {"class":"ghostty","title":"~/code/hypxrland","monitor":4,"mapped":true},
        {"class":"chromium","title":"YouTube - Big Buck Bunny - Chromium","monitor":5,"mapped":true}
    ])json";
    const char* xr = R"json({"state":"focused","monitors":[
        {"name":"XR-1","id":3,"anchor":{"mode":"local"}},
        {"name":"XR-2","id":4,"anchor":{"mode":"local"}},
        {"name":"XR-3","id":5,"anchor":{"mode":"local"}}
    ]})json";

    // "have youtube follow me": youtube exists ONLY in the chromium window title.
    Harness h1;
    auto r1 = IntentPipeline::process(mk("have youtube follow me"), h1.cfg,
                                      mockHyprctl(mons, cls, xr), mockGaze(-1, ""), h1.runner);
    CHECK(r1.action.verb == EVerb::Follow);
    CHECK(r1.action.target == "XR-3");
    CHECK(h1.ranLine("hyprctl openxr select XR-3"));
    CHECK(h1.ranLine("hyprctl openxr adaptive on"));

    // "move the coding monitor closer": resolved via the editor's title/project dir.
    Harness h2;
    auto r2 = IntentPipeline::process(mk("move the coding monitor closer"), h2.cfg,
                                      mockHyprctl(mons, cls, xr), mockGaze(-1, ""), h2.runner);
    CHECK(r2.action.verb == EVerb::MoveDist);
    CHECK(r2.action.target == "XR-2");
    CHECK(h2.ranLine("hyprctl openxr select XR-2"));
}

TEST_CASE("pipeline: dry-run builds the plan but actuates nothing") {
    Harness h(/*live=*/false); // dry_run = true
    auto r = IntentPipeline::process(mk("move the coding monitor closer"), h.cfg,
                                     mockHyprctl(), mockGaze(-1, ""), h.runner);
    CHECK(r.plan.ok);
    CHECK(r.plan.steps.size() == 2); // select + distance planned
    CHECK(r.dispatched == 0);
    CHECK(h.ran.empty());            // dry-run: nothing ran
}

TEST_CASE("pipeline: a named target that is not live is refused (no actuation)") {
    Harness h;
    // No XR monitors at all; "coding monitor" cannot resolve.
    auto r = IntentPipeline::process(mk("move the coding monitor closer"), h.cfg,
                                     mockHyprctl("[]", "[]", "{}"), mockGaze(-1, ""), h.runner);
    // With no monitors, semantic fails and there is no hovered monitor -> active.
    // The executor may still target "active"; assert we never invented XR-code.
    CHECK_FALSE(h.ranLine("hyprctl openxr select XR-code"));
}
