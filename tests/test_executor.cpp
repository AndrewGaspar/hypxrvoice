#include "doctest.h"

#include "Executor.hpp"

#include <string>
#include <vector>

namespace {
    SDesktopContext fixtureCtx() {
        const char* mons = R"json([
            {"id":3,"name":"XR-code"},{"id":4,"name":"XR-web"}
        ])json";
        const char* xr = R"json({"state":"focused","monitors":[
            {"name":"XR-code","id":3,"anchor":{"mode":"local"}},
            {"name":"XR-web","id":4,"hovered":true,"anchor":{"mode":"body"}}
        ]})json";
        return SDesktopContext::parse(mons, "", xr);
    }

    // Flatten a plan's steps into space-joined argv strings for easy assertions.
    std::vector<std::string> lines(const SExecPlan& p) {
        std::vector<std::string> out;
        for (auto& s : p.steps) {
            std::string j;
            for (auto& t : s.argv) { if (!j.empty()) j += ' '; j += t; }
            out.push_back(j);
        }
        return out;
    }
    bool hasLine(const SExecPlan& p, const std::string& want) {
        for (auto& l : lines(p)) if (l == want) return true;
        return false;
    }
}

TEST_CASE("executor: anchor a named monitor maps to a single exact argv") {
    SAction a; a.verb = EVerb::Anchor; a.target = "XR-code"; a.anchor = EAnchorMode::Local;
    SExecConfig cfg; cfg.dryRun = true;
    SExecPlan p = planFor(a, fixtureCtx(), cfg);
    REQUIRE(p.ok);
    REQUIRE(p.steps.size() == 1);
    CHECK(hasLine(p, "hyprctl openxr anchor XR-code local"));
}

TEST_CASE("executor: move closer selects then pushes a WE-formatted signed distance") {
    SAction a; a.verb = EVerb::MoveDist; a.target = "XR-web"; a.deltaM = -0.25;
    SExecConfig cfg;
    SExecPlan p = planFor(a, fixtureCtx(), cfg);
    REQUIRE(p.ok);
    CHECK(hasLine(p, "hyprctl openxr select XR-web"));
    CHECK(hasLine(p, "hyprctl openxr distance -0.25"));
}

TEST_CASE("executor: pick uses select+anchor-head approximation by default") {
    SAction a; a.verb = EVerb::Pick; a.target = "XR-code";
    SExecConfig cfg;
    SExecPlan p = planFor(a, fixtureCtx(), cfg);
    REQUIRE(p.ok);
    CHECK(p.approximated);
    CHECK(hasLine(p, "hyprctl openxr select XR-code"));
    CHECK(hasLine(p, "hyprctl openxr anchor XR-code head"));
}

TEST_CASE("executor: pick uses the targeted grab when the capability is present") {
    SAction a; a.verb = EVerb::Pick; a.target = "XR-code";
    SExecConfig cfg; cfg.caps.targetedGrab = true;
    SExecPlan p = planFor(a, fixtureCtx(), cfg);
    REQUIRE(p.ok);
    CHECK_FALSE(p.approximated);
    CHECK(hasLine(p, "hyprctl openxr gazegrab XR-code"));
}

TEST_CASE("executor: place freezes in place by default (anchor local)") {
    SAction a; a.verb = EVerb::Place; a.target = "active"; a.targetSource = ETargetSource::Active;
    SExecConfig cfg;
    SExecPlan p = planFor(a, fixtureCtx(), cfg);
    REQUIRE(p.ok);
    CHECK(p.approximated);
    CHECK(hasLine(p, "hyprctl openxr anchor active local"));
}

TEST_CASE("executor: place-at-pose uses the resolved gaze point when advertised") {
    SAction a; a.verb = EVerb::Place; a.target = "active";
    a.gaze.valid = true; a.gaze.pos[0] = 0.5; a.gaze.pos[1] = 1.4; a.gaze.pos[2] = -1.2;
    SExecConfig cfg; cfg.caps.placeAtPose = true;
    SExecPlan p = planFor(a, fixtureCtx(), cfg);
    REQUIRE(p.ok);
    CHECK(hasLine(p, "hyprctl openxr place active at 0.500,1.400,-1.200"));
}

TEST_CASE("executor: follow issues adaptive on (and roam when a mode is given)") {
    SAction a; a.verb = EVerb::Follow; a.target = "XR-web"; a.anchor = EAnchorMode::Body;
    SExecConfig cfg;
    SExecPlan p = planFor(a, fixtureCtx(), cfg);
    REQUIRE(p.ok);
    CHECK(hasLine(p, "hyprctl openxr select XR-web"));
    CHECK(hasLine(p, "hyprctl openxr adaptive on"));
    CHECK(hasLine(p, "hyprctl openxr roam body"));
}

TEST_CASE("executor: hand input toggle needs no monitor target") {
    SAction a; a.verb = EVerb::HandInput; a.sub = "off";
    SExecConfig cfg;
    SExecPlan p = planFor(a, fixtureCtx(), cfg);
    REQUIRE(p.ok);
    CHECK(hasLine(p, "hyprctl openxr handinput off"));
}

TEST_CASE("executor: launch is refused unless enabled AND allowlisted") {
    SAction a; a.verb = EVerb::LaunchApp; a.app = "browser";
    SExecConfig off; // allowLaunch=false
    CHECK_FALSE(planFor(a, fixtureCtx(), off).ok);

    SExecConfig on; on.allowLaunch = true;
    CHECK_FALSE(planFor(a, fixtureCtx(), on).ok); // not in allowlist

    on.appAllowlist["browser"] = "uwsm app -- firefox.desktop";
    SExecPlan p = planFor(a, fixtureCtx(), on);
    REQUIRE(p.ok);
    CHECK(hasLine(p, "hyprctl dispatch exec -- uwsm app -- firefox.desktop"));
}

TEST_CASE("executor: a named-but-dead target is refused, never actuated") {
    SAction a; a.verb = EVerb::Anchor; a.target = "XR-ghost"; a.anchor = EAnchorMode::Local;
    SExecConfig cfg;
    SExecPlan p = planFor(a, fixtureCtx(), cfg);
    CHECK_FALSE(p.ok);
    CHECK(p.steps.empty());
}

TEST_CASE("executor: None and Clarify produce no steps") {
    SExecConfig cfg;
    SAction none; none.verb = EVerb::None;
    CHECK_FALSE(planFor(none, fixtureCtx(), cfg).ok);
    SAction cl; cl.verb = EVerb::Clarify; cl.clarifyQuestion = "which one?";
    SExecPlan p = planFor(cl, fixtureCtx(), cfg);
    CHECK_FALSE(p.ok);
    CHECK(p.steps.empty());
}

TEST_CASE("executor: validateStep rejects anything outside the allowlist") {
    std::string err;
    CHECK(validateStep({{"hyprctl", "openxr", "anchor", "XR-code", "local"}, ""}, err));
    CHECK(validateStep({{"hyprctl", "dispatch", "exec", "--", "firefox"}, ""}, err));

    // Not hyprctl.
    CHECK_FALSE(validateStep({{"rm", "-rf", "/"}, ""}, err));
    // An openxr verb outside the closed set (e.g. destroy is deliberately absent).
    CHECK_FALSE(validateStep({{"hyprctl", "openxr", "destroy", "XR-code"}, ""}, err));
    // A dispatcher outside the closed set (killactive is deliberately absent).
    CHECK_FALSE(validateStep({{"hyprctl", "dispatch", "killactive"}, ""}, err));
    // `movetoworkspace` JOINED the allowlist in round 3 (it is how a window move reaches a
    // workspace) — but only in its checked shape; `movetoworkspacesilent` did not.
    CHECK(validateStep({{"hyprctl", "dispatch", "movetoworkspace", "5"}, ""}, err));
    CHECK_FALSE(validateStep({{"hyprctl", "dispatch", "movetoworkspacesilent", "5"}, ""}, err));
    // Control characters cannot ride an argv token.
    CHECK_FALSE(validateStep({{"hyprctl", "openxr", "select", "XR\ncode"}, ""}, err));
}

// The window-management dispatchers added in round 2 are permitted, but only in the
// exact argument SHAPES the planner emits — this is the last line of defence, so it is
// asserted independently of how the planner happens to build them today.
TEST_CASE("executor: validateStep pins the shape of every window dispatcher") {
    std::string err;

    CHECK(validateStep({{"hyprctl", "dispatch", "workspace", "3"}, ""}, err));
    CHECK(validateStep({{"hyprctl", "dispatch", "workspace", "42"}, ""}, err));
    CHECK_FALSE(validateStep({{"hyprctl", "dispatch", "workspace", "0"}, ""}, err));
    CHECK_FALSE(validateStep({{"hyprctl", "dispatch", "workspace", "100"}, ""}, err));
    CHECK_FALSE(validateStep({{"hyprctl", "dispatch", "workspace", "3a"}, ""}, err));
    CHECK_FALSE(validateStep({{"hyprctl", "dispatch", "workspace"}, ""}, err));

    CHECK(validateStep({{"hyprctl", "dispatch", "fullscreen", "0"}, ""}, err));
    CHECK(validateStep({{"hyprctl", "dispatch", "fullscreen", "1"}, ""}, err));
    CHECK_FALSE(validateStep({{"hyprctl", "dispatch", "fullscreen", "2"}, ""}, err));
    CHECK_FALSE(validateStep({{"hyprctl", "dispatch", "fullscreen"}, ""}, err));

    CHECK(validateStep({{"hyprctl", "dispatch", "focuswindow", "address:0x55f0abcd"}, ""}, err));
    // `class:` is a REGEX in Hyprland — never permitted, whatever it contains.
    CHECK_FALSE(validateStep({{"hyprctl", "dispatch", "focuswindow", "class:firefox"}, ""}, err));
    CHECK_FALSE(validateStep({{"hyprctl", "dispatch", "focuswindow", "address:0xzz"}, ""}, err));
    CHECK_FALSE(validateStep({{"hyprctl", "dispatch", "focuswindow", "address:0x"}, ""}, err));

    CHECK(validateStep({{"hyprctl", "dispatch", "focusmonitor", "XR-code"}, ""}, err));
    CHECK_FALSE(validateStep({{"hyprctl", "dispatch", "focusmonitor", "XR code; rm -rf /"}, ""}, err));
}

TEST_CASE("executor: dry-run runs nothing; live run records exact argv") {
    SAction a; a.verb = EVerb::Anchor; a.target = "XR-code"; a.anchor = EAnchorMode::Head;
    SDesktopContext ctx = fixtureCtx();

    std::vector<std::vector<std::string>> recorded;
    RunFn rec = [&](const std::vector<std::string>& argv) { recorded.push_back(argv); return 0; };

    SExecConfig dry; dry.dryRun = true;
    SExecPlan   pd = planFor(a, ctx, dry);
    int         nd = runPlan(pd, dry, rec);
    CHECK(nd == 0);
    CHECK(recorded.empty()); // dry-run actuates nothing

    SExecConfig live; live.dryRun = false;
    SExecPlan   pl = planFor(a, ctx, live);
    int         nl = runPlan(pl, live, rec);
    CHECK(nl == 1);
    REQUIRE(recorded.size() == 1);
    CHECK(recorded[0] == std::vector<std::string>{"hyprctl", "openxr", "anchor", "XR-code", "head"});
}

// ---- window management (round 2) --------------------------------------------------

namespace {
    // A snapshot with real clients, so the window verbs have live addresses to resolve.
    SDesktopContext windowCtx() {
        const char* mons = R"json([
            {"id":3,"name":"XR-code"},{"id":4,"name":"XR-web"}
        ])json";
        const char* cls = R"json([
            {"address":"0x55f0aaaa","class":"firefox","title":"YouTube - Mozilla Firefox",
             "monitor":4,"mapped":true,"focusHistoryID":1},
            {"address":"0x55f0bbbb","class":"nvim","title":"main.cpp - NVIM",
             "monitor":3,"mapped":true,"focusHistoryID":0}
        ])json";
        return SDesktopContext::parse(mons, cls, "");
    }
}

TEST_CASE("executor: workspace switches with a number WE formatted") {
    SAction a; a.verb = EVerb::Workspace; a.workspace = 3;
    SExecConfig cfg;
    SExecPlan p = planFor(a, windowCtx(), cfg);
    REQUIRE(p.ok);
    REQUIRE(p.steps.size() == 1);
    CHECK(hasLine(p, "hyprctl dispatch workspace 3"));
}

TEST_CASE("executor: an out-of-range workspace is refused, never clamped") {
    SAction a; a.verb = EVerb::Workspace; a.workspace = 0;
    SExecConfig cfg;
    CHECK_FALSE(planFor(a, windowCtx(), cfg).ok);
    a.workspace = 250;
    CHECK_FALSE(planFor(a, windowCtx(), cfg).ok);
}

TEST_CASE("executor: focus dispatches at the live window ADDRESS, not a class regex") {
    SAction a; a.verb = EVerb::Focus; a.windowAddress = "0x55f0aaaa"; a.windowLabel = "firefox";
    SExecConfig cfg;
    SExecPlan p = planFor(a, windowCtx(), cfg);
    REQUIRE(p.ok);
    REQUIRE(p.steps.size() == 1);
    CHECK(hasLine(p, "hyprctl dispatch focuswindow address:0x55f0aaaa"));
}

TEST_CASE("executor: a window address that is not live is refused, never actuated") {
    SAction a; a.verb = EVerb::Focus; a.windowAddress = "0xdeadbeef"; a.windowLabel = "ghost";
    SExecConfig cfg;
    SExecPlan p = planFor(a, windowCtx(), cfg);
    CHECK_FALSE(p.ok);
    CHECK(p.steps.empty());
}

TEST_CASE("executor: fullscreen on a named window focuses it first, then toggles") {
    SAction a; a.verb = EVerb::Fullscreen; a.windowAddress = "0x55f0aaaa"; a.windowLabel = "firefox";
    SExecConfig cfg;
    SExecPlan p = planFor(a, windowCtx(), cfg);
    REQUIRE(p.ok);
    REQUIRE(p.steps.size() == 2);
    CHECK(lines(p)[0] == "hyprctl dispatch focuswindow address:0x55f0aaaa");
    CHECK(lines(p)[1] == "hyprctl dispatch fullscreen 0");
}

TEST_CASE("executor: fullscreen with no named window acts on the focused one") {
    SAction a; a.verb = EVerb::Fullscreen; a.target = "active"; a.targetSource = ETargetSource::Active;
    SExecConfig cfg;
    SExecPlan p = planFor(a, windowCtx(), cfg);
    REQUIRE(p.ok);
    REQUIRE(p.steps.size() == 1);
    CHECK(hasLine(p, "hyprctl dispatch fullscreen 0"));
}

TEST_CASE("executor: a deictic fullscreen goes to the gazed monitor first") {
    SAction a; a.verb = EVerb::Fullscreen; a.target = "XR-web"; a.targetSource = ETargetSource::Deixis;
    SExecConfig cfg;
    SExecPlan p = planFor(a, windowCtx(), cfg);
    REQUIRE(p.ok);
    REQUIRE(p.steps.size() == 2);
    CHECK(lines(p)[0] == "hyprctl dispatch focusmonitor XR-web");
    CHECK(lines(p)[1] == "hyprctl dispatch fullscreen 0");
}

TEST_CASE("executor: 'maximize' picks Hyprland's other fullscreen mode") {
    SAction a; a.verb = EVerb::Fullscreen; a.sub = "maximize"; a.target = "active";
    SExecConfig cfg;
    SExecPlan p = planFor(a, windowCtx(), cfg);
    REQUIRE(p.ok);
    CHECK(hasLine(p, "hyprctl dispatch fullscreen 1"));
}

TEST_CASE("executor: allow_window=false refuses every window verb") {
    SExecConfig off; off.allowWindow = false;
    SAction ws; ws.verb = EVerb::Workspace; ws.workspace = 2;
    SAction fo; fo.verb = EVerb::Focus; fo.windowAddress = "0x55f0aaaa";
    SAction fs; fs.verb = EVerb::Fullscreen; fs.target = "active";
    CHECK_FALSE(planFor(ws, windowCtx(), off).ok);
    CHECK_FALSE(planFor(fo, windowCtx(), off).ok);
    CHECK_FALSE(planFor(fs, windowCtx(), off).ok);
}

// ---- window -> monitor / workspace moves (round 3) ---------------------------------

namespace {
    // Live windows + a real layout, so both halves of a move can be validated.
    SDesktopContext moveCtx() {
        const char* mons = R"json([
            {"id":0,"name":"eDP-1","focused":true,"x":0,"y":0},
            {"id":3,"name":"XR-code","x":1920,"y":0}
        ])json";
        const char* cls = R"json([
            {"address":"0x2222","class":"kitty","title":"~","monitor":0,"mapped":true,"focusHistoryID":0}
        ])json";
        return SDesktopContext::parse(mons, cls, "");
    }
}

TEST_CASE("executor: a window move focuses the window then moves it to the named monitor") {
    SAction a;
    a.verb          = EVerb::MoveWindow;
    a.windowAddress = "0x2222";
    a.windowLabel   = "kitty";
    a.target        = "XR-code";
    a.targetSource  = ETargetSource::Semantic;
    SExecConfig cfg;
    SExecPlan   p = planFor(a, moveCtx(), cfg);
    REQUIRE(p.ok);
    REQUIRE(p.steps.size() == 2);
    CHECK(hasLine(p, "hyprctl dispatch focuswindow address:0x2222"));
    CHECK(hasLine(p, "hyprctl dispatch movewindow mon:XR-code"));
    for (auto& s : p.steps) {
        std::string err;
        CHECK_MESSAGE(validateStep(s, err), err);
    }
}

TEST_CASE("executor: a move with no named window acts on the focused one") {
    SAction a;
    a.verb   = EVerb::MoveWindow;
    a.target = "XR-code";
    SExecPlan p = planFor(a, moveCtx(), SExecConfig{});
    REQUIRE(p.ok);
    REQUIRE(p.steps.size() == 1);
    CHECK(hasLine(p, "hyprctl dispatch movewindow mon:XR-code"));
}

TEST_CASE("executor: a workspace destination dispatches movetoworkspace") {
    SAction a;
    a.verb          = EVerb::MoveWindow;
    a.windowAddress = "0x2222";
    a.workspace     = 3;
    SExecPlan p = planFor(a, moveCtx(), SExecConfig{});
    REQUIRE(p.ok);
    CHECK(hasLine(p, "hyprctl dispatch focuswindow address:0x2222"));
    CHECK(hasLine(p, "hyprctl dispatch movetoworkspace 3"));
}

TEST_CASE("executor: a move refuses a dead window handle and a monitor that is not live") {
    SAction stale;
    stale.verb          = EVerb::MoveWindow;
    stale.windowAddress = "0xdead";
    stale.target        = "XR-code";
    CHECK_FALSE(planFor(stale, moveCtx(), SExecConfig{}).ok);

    SAction ghost;
    ghost.verb   = EVerb::MoveWindow;
    ghost.target = "XR-nowhere";
    CHECK_FALSE(planFor(ghost, moveCtx(), SExecConfig{}).ok);

    // "active" is not a destination: a move must know where it is going.
    SAction vague;
    vague.verb   = EVerb::MoveWindow;
    vague.target = "active";
    CHECK_FALSE(planFor(vague, moveCtx(), SExecConfig{}).ok);
}

TEST_CASE("executor: a move obeys allow_window like the rest of the window verbs") {
    SAction a;
    a.verb   = EVerb::MoveWindow;
    a.target = "XR-code";
    SExecConfig cfg;
    cfg.allowWindow = false;
    CHECK_FALSE(planFor(a, moveCtx(), cfg).ok);
}

TEST_CASE("executor: validateStep pins the shape of the two move dispatchers") {
    std::string err;
    CHECK(validateStep(SExecStep{{"hyprctl", "dispatch", "movewindow", "mon:XR-code"}, ""}, err));
    CHECK(validateStep(SExecStep{{"hyprctl", "dispatch", "movetoworkspace", "12"}, ""}, err));
    // No directional form, no bare name, no metacharacters, no out-of-range index.
    CHECK_FALSE(validateStep(SExecStep{{"hyprctl", "dispatch", "movewindow", "l"}, ""}, err));
    CHECK_FALSE(validateStep(SExecStep{{"hyprctl", "dispatch", "movewindow", "XR-code"}, ""}, err));
    CHECK_FALSE(validateStep(SExecStep{{"hyprctl", "dispatch", "movewindow", "mon:XR code; rm -rf"}, ""}, err));
    CHECK_FALSE(validateStep(SExecStep{{"hyprctl", "dispatch", "movewindow", "mon:"}, ""}, err));
    CHECK_FALSE(validateStep(SExecStep{{"hyprctl", "dispatch", "movetoworkspace", "0"}, ""}, err));
    CHECK_FALSE(validateStep(SExecStep{{"hyprctl", "dispatch", "movetoworkspace", "abc"}, ""}, err));
}
