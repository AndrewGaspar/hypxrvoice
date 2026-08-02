#include "doctest.h"

#include "Executor.hpp"
#include "Intent.hpp"

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

TEST_CASE("executor: place-at-pose uses the PROJECTED gaze point, never the head origin") {
    SAction a; a.verb = EVerb::Place; a.target = "active";
    // Head at (0.5,1.4,-1.2) looking down -Z; the projected point is 1.3 m ahead of it.
    a.gaze.valid = true; a.gaze.pos[0] = 0.5; a.gaze.pos[1] = 1.4; a.gaze.pos[2] = -1.2;
    a.gaze.forward[2] = -1.0;
    a.gaze.place[0] = 0.5; a.gaze.place[1] = 1.4; a.gaze.place[2] = -2.5;
    a.gaze.placeDistM = 1.3;
    SExecConfig cfg; cfg.caps.placeAtPose = true;
    SExecPlan p = planFor(a, fixtureCtx(), cfg);
    REQUIRE(p.ok);
    CHECK(hasLine(p, "hyprctl openxr place active at 0.500,1.400,-2.500"));
    // The head origin must appear nowhere in the plan.
    CHECK_FALSE(hasLine(p, "hyprctl openxr place active at 0.500,1.400,-1.200"));
}

TEST_CASE("executor: an unprojected gaze (placeDistM==0) never becomes a place command") {
    // A resolution that never went through projectPlacePoint() carries place=[0,0,0].
    // Emitting it would drop the monitor at the LOCAL_FLOOR origin; the executor must
    // fall back to freezing in place instead.
    SAction a; a.verb = EVerb::Place; a.target = "active";
    a.gaze.valid = true; a.gaze.pos[0] = 0.5; a.gaze.pos[1] = 1.4; a.gaze.pos[2] = -1.2;
    SExecConfig cfg; cfg.caps.placeAtPose = true;
    SExecPlan p = planFor(a, fixtureCtx(), cfg);
    REQUIRE(p.ok);
    CHECK(p.approximated);
    CHECK(hasLine(p, "hyprctl openxr anchor active local"));
    for (auto& st : p.steps) {
        const bool isPlace = st.argv.size() >= 3 && st.argv[2] == "place";
        CHECK_FALSE(isPlace);
    }
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

// ---------------------------------------------------------------------------
// Round 5: the workspace-move verb and runtime monitor creation.
// ---------------------------------------------------------------------------

TEST_CASE("executor: move_workspace dispatches moveworkspacetomonitor") {
    SAction a; a.verb = EVerb::MoveWorkspace; a.workspace = 4; a.target = "XR-web";
    a.targetSource = ETargetSource::Deixis;
    SExecConfig cfg;
    SExecPlan p = planFor(a, fixtureCtx(), cfg);
    REQUIRE(p.ok);
    CHECK(hasLine(p, "hyprctl dispatch moveworkspacetomonitor 4 XR-web"));
    // A workspace move never touches a window.
    for (auto& st : p.steps) {
        const bool isMoveWindow = st.argv.size() >= 3 && st.argv[2] == "movewindow";
        CHECK_FALSE(isMoveWindow);
    }
    std::string err;
    for (auto& st : p.steps)
        CHECK(validateStep(st, err));
}

TEST_CASE("executor: move_workspace refuses a dead monitor or a bad index") {
    SExecConfig cfg;
    SAction a; a.verb = EVerb::MoveWorkspace; a.workspace = 4; a.target = "XR-ghost";
    CHECK_FALSE(planFor(a, fixtureCtx(), cfg).ok);
    SAction b; b.verb = EVerb::MoveWorkspace; b.workspace = 0; b.target = "XR-web";
    CHECK_FALSE(planFor(b, fixtureCtx(), cfg).ok);
    // "active" is not a destination: the workspace would land somewhere unasked.
    SAction c; c.verb = EVerb::MoveWorkspace; c.workspace = 4; c.target = "active";
    CHECK_FALSE(planFor(c, fixtureCtx(), cfg).ok);
}

TEST_CASE("executor: create_monitor creates then places at the projected point") {
    SAction a; a.verb = EVerb::CreateMonitor; a.target = "XR-2";
    a.gaze.valid = true;
    a.gaze.place[0] = 0.1; a.gaze.place[1] = 1.4; a.gaze.place[2] = -1.5;
    a.gaze.placeDistM = 1.3;
    SExecConfig cfg;
    SExecPlan p = planFor(a, fixtureCtx(), cfg);
    REQUIRE(p.ok);
    REQUIRE(p.steps.size() == 2);
    CHECK(p.steps[0].argv[2] == "create");
    CHECK(p.steps[0].argv[3] == "XR-2");
    CHECK(hasLine(p, "hyprctl openxr place XR-2 at 0.100,1.400,-1.500"));
    std::string err;
    for (auto& st : p.steps)
        CHECK(validateStep(st, err));
}

TEST_CASE("executor: create_monitor without a deixis just creates") {
    SAction a; a.verb = EVerb::CreateMonitor; a.target = "XR-2";
    SExecConfig cfg;
    SExecPlan p = planFor(a, fixtureCtx(), cfg);
    REQUIRE(p.ok);
    CHECK(p.steps.size() == 1);
    CHECK(p.steps[0].argv[2] == "create");
}

TEST_CASE("executor: create_monitor refuses an existing or malformed name") {
    SExecConfig cfg;
    SAction a; a.verb = EVerb::CreateMonitor; a.target = "XR-web"; // already live
    CHECK_FALSE(planFor(a, fixtureCtx(), cfg).ok);
    SAction b; b.verb = EVerb::CreateMonitor; b.target = "my monitor"; // never mintable
    CHECK_FALSE(planFor(b, fixtureCtx(), cfg).ok);
    SAction c; c.verb = EVerb::CreateMonitor; c.target = "";
    CHECK_FALSE(planFor(c, fixtureCtx(), cfg).ok);
    SExecConfig off; off.allowCreateMonitor = false;
    SAction d; d.verb = EVerb::CreateMonitor; d.target = "XR-2";
    CHECK_FALSE(planFor(d, fixtureCtx(), off).ok);
}

TEST_CASE("executor: validateStep pins the new argv shapes") {
    std::string err;
    CHECK(validateStep({{"hyprctl", "openxr", "create", "XR-2", "1920x1080@60"}, ""}, err));
    CHECK_FALSE(validateStep({{"hyprctl", "openxr", "create", "my monitor", "1920x1080@60"}, ""}, err));
    CHECK_FALSE(validateStep({{"hyprctl", "openxr", "create", "XR-2", "1920x1080"}, ""}, err));
    CHECK_FALSE(validateStep({{"hyprctl", "openxr", "create", "XR-2"}, ""}, err));

    CHECK(validateStep({{"hyprctl", "openxr", "place", "XR-2", "at", "0.100,1.400,-1.500"}, ""}, err));
    CHECK_FALSE(validateStep({{"hyprctl", "openxr", "place", "XR-2", "at", "0.1 1.4 -1.5"}, ""}, err));
    CHECK_FALSE(validateStep({{"hyprctl", "openxr", "place", "XR-2", "0.1,1.4,-1.5"}, ""}, err));

    CHECK(validateStep({{"hyprctl", "dispatch", "moveworkspacetomonitor", "4", "XR-web"}, ""}, err));
    CHECK_FALSE(validateStep({{"hyprctl", "dispatch", "moveworkspacetomonitor", "0", "XR-web"}, ""}, err));
    CHECK_FALSE(validateStep({{"hyprctl", "dispatch", "moveworkspacetomonitor", "4", "XR web"}, ""}, err));
    CHECK_FALSE(validateStep({{"hyprctl", "dispatch", "moveworkspacetomonitor", "4"}, ""}, err));
}

// ---------------------------------------------------------------------------
// Round 6, Task B: a step may WAIT for a monitor an earlier step created.
//
// Live fire, 21:51: `openxr create XR-2 2560x1440@60` succeeded and the `openxr place
// XR-2 at …` issued immediately after exited 1 ("step exited 1 — stopping plan"). The
// identical pair at 22:07 succeeded. `create` returns once the compositor has accepted
// the request, but the output is registered asynchronously — the place can beat it.
//
// The fix is a per-step PRECONDITION, not a create-specific sleep: any step may declare
// the monitor it depends on, and runPlan holds it until the compositor agrees the name
// exists (or refuses the step outright).
// ---------------------------------------------------------------------------

TEST_CASE("executor: the place that follows a create waits for the monitor") {
    SAction a; a.verb = EVerb::CreateMonitor; a.target = "XR-2";
    a.gaze.valid = true;
    a.gaze.place[0] = 0.1; a.gaze.place[1] = 1.4; a.gaze.place[2] = -1.5;
    a.gaze.placeDistM = 1.3;
    SExecPlan p = planFor(a, fixtureCtx(), SExecConfig{});
    REQUIRE(p.ok);
    REQUIRE(p.steps.size() == 2);
    CHECK(p.steps[0].waitForMonitor.empty());     // the create makes it; it cannot wait for it
    CHECK(p.steps[1].waitForMonitor == "XR-2");
    // The precondition is visible to the feedback tier / logs.
    CHECK(p.toJson().find("\"waitFor\":\"XR-2\"") != std::string::npos);
    // …and it changes no argv: the shapes are still exactly the allowlisted ones.
    std::string err;
    for (auto& st : p.steps)
        CHECK_MESSAGE(validateStep(st, err), err);
}

TEST_CASE("executor: a waiting step polls until the monitor is live, then dispatches") {
    SAction a; a.verb = EVerb::CreateMonitor; a.target = "XR-2";
    a.gaze.valid = true; a.gaze.place[2] = -1.5; a.gaze.placeDistM = 1.3;
    SExecConfig cfg; cfg.dryRun = false;
    SExecPlan   p = planFor(a, fixtureCtx(), cfg);
    REQUIRE(p.ok);

    std::vector<std::vector<std::string>> recorded;
    RunFn rec = [&](const std::vector<std::string>& argv) { recorded.push_back(argv); return 0; };

    int probes = 0, naps = 0, slept = 0;
    // The monitor shows up on the third look — the race, reproduced.
    MonitorProbeFn probe = [&](const std::string& n) { CHECK(n == "XR-2"); return ++probes >= 3; };
    SleepFn        nap   = [&](int ms) { naps++; slept += ms; };

    int n = runPlan(p, cfg, rec, probe, nap);
    CHECK(n == 2);
    REQUIRE(recorded.size() == 2);
    CHECK(recorded[0][2] == "create");
    CHECK(recorded[1][2] == "place");
    CHECK(probes == 3);
    CHECK(naps == 2);
    CHECK(slept == 2 * cfg.waitPollMs);
}

TEST_CASE("executor: a monitor that never appears stops the plan instead of dispatching") {
    SAction a; a.verb = EVerb::CreateMonitor; a.target = "XR-2";
    a.gaze.valid = true; a.gaze.place[2] = -1.5; a.gaze.placeDistM = 1.3;
    SExecConfig cfg; cfg.dryRun = false; cfg.waitMonitorMs = 250; cfg.waitPollMs = 100;
    SExecPlan   p = planFor(a, fixtureCtx(), cfg);

    std::vector<std::vector<std::string>> recorded;
    RunFn rec = [&](const std::vector<std::string>& argv) { recorded.push_back(argv); return 0; };
    int   probes = 0, slept = 0;
    MonitorProbeFn probe = [&](const std::string&) { probes++; return false; };
    SleepFn        nap   = [&](int ms) { slept += ms; };

    int n = runPlan(p, cfg, rec, probe, nap);
    CHECK(n == 1);                       // the create ran; the place never did
    REQUIRE(recorded.size() == 1);
    CHECK(recorded[0][2] == "create");
    CHECK(probes == 4);                  // one per poll, plus the final look after the budget
    CHECK(slept == 250);                 // and never longer than the budget
}

TEST_CASE("executor: wait_monitor_ms=0 looks exactly once, and dry-run never probes") {
    SAction a; a.verb = EVerb::CreateMonitor; a.target = "XR-2";
    a.gaze.valid = true; a.gaze.place[2] = -1.5; a.gaze.placeDistM = 1.3;

    SExecConfig off; off.dryRun = false; off.waitMonitorMs = 0;
    SExecPlan   p = planFor(a, fixtureCtx(), off);
    int         probes = 0, naps = 0;
    RunFn       rec  = [&](const std::vector<std::string>&) { return 0; };
    MonitorProbeFn pr = [&](const std::string&) { probes++; return true; };
    SleepFn        np = [&](int) { naps++; };
    CHECK(runPlan(p, off, rec, pr, np) == 2);
    CHECK(probes == 1);
    CHECK(naps == 0);

    // Dry-run creates nothing, so waiting for the thing it did not create would only ever
    // time out. It logs the precondition and moves on.
    SExecConfig dry; dry.dryRun = true;
    probes = 0; naps = 0;
    CHECK(runPlan(planFor(a, fixtureCtx(), dry), dry, rec, pr, np) == 0);
    CHECK(probes == 0);
    CHECK(naps == 0);
}

// Round 8, end to end. "Move this monitor closer." went out live as a bare
// `openxr distance -0.25` with no `select` — and `distance` takes no monitor argument at
// all (Hyprland OpenXRManager::cmdDistance), so it acted on whatever resolveSelected()
// pointed at: explicit `openxr select` > last pointer-ray hover > focused XR output.
// The user was looking somewhere else entirely. The plan must now name the monitor.
TEST_CASE("executor: a gaze-resolved 'move this monitor closer' selects before it pulls") {
    CRuleIntent eng(SIntentConfig{});
    STranscript t;
    t.text    = "move this monitor closer";
    t.onsetMs = 100000;
    int64_t ms = 100000;
    for (const char* w : {"move", "this", "monitor", "closer"}) {
        SWord sw; sw.text = w; sw.startMs = ms; sw.endMs = ms + 150;
        t.words.push_back(sw);
        ms += 200;
    }
    t.endMs = ms;

    GazeQueryFn q = [](int64_t at) {
        char b[360];
        std::snprintf(b, sizeof(b),
                      R"json({"ok":true,"viewValid":true,"timestampMs":%lld,
                      "head":{"pos":[0,1.4,0],"forward":[0,0,-1]},
                      "gaze":{"monitorId":3,"name":"XR-code","selected":true,"dwellSec":0.4},
                      "query":{"matchedTimestampMs":%lld,"ageMs":0}})json",
                      (long long)at, (long long)at);
        return std::string(b);
    };

    SDesktopContext ctx = fixtureCtx();
    SAction         a   = eng.resolve(t, ctx, q);
    REQUIRE(a.verb == EVerb::MoveDist);
    CHECK(a.target == "XR-code");
    CHECK(a.targetSource == ETargetSource::Deixis);

    SExecConfig cfg;
    SExecPlan   p = planFor(a, ctx, cfg);
    REQUIRE(p.ok);
    CHECK(hasLine(p, "hyprctl openxr select XR-code"));
    CHECK(hasLine(p, "hyprctl openxr distance -0.25"));
    // Order matters: `distance` reads the selection, so the select must come first.
    CHECK(lines(p).front() == "hyprctl openxr select XR-code");
}
