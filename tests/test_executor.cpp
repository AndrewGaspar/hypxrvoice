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
    // dispatch that isn't `exec --`.
    CHECK_FALSE(validateStep({{"hyprctl", "dispatch", "workspace", "5"}, ""}, err));
    // Control characters cannot ride an argv token.
    CHECK_FALSE(validateStep({{"hyprctl", "openxr", "select", "XR\ncode"}, ""}, err));
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
