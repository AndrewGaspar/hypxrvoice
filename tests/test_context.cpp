#include "doctest.h"

#include "DesktopContext.hpp"

namespace {
    // A fixture desktop: a laptop panel + two XR monitors (a code editor, a browser
    // playing YouTube). Shapes trimmed to the fields the snapshot reads.
    const char* kMonitors = R"json([
        {"id":0,"name":"eDP-1","focused":true,"activeWorkspace":{"name":"1"}},
        {"id":3,"name":"XR-code","focused":false,"activeWorkspace":{"name":"code"}},
        {"id":4,"name":"XR-web","focused":false,"activeWorkspace":{"name":"web"}}
    ])json";
    const char* kClients = R"json([
        {"class":"nvim","title":"main.cpp - NVIM","monitor":3,"mapped":true},
        {"class":"firefox","title":"YouTube - Mozilla Firefox","monitor":4,"mapped":true},
        {"class":"foot","title":"~","monitor":0,"mapped":true}
    ])json";
    const char* kOpenxr = R"json({
        "state":"focused",
        "monitors":[
            {"name":"XR-code","id":3,"hovered":false,"grabbed":false,"anchor":{"mode":"local"}},
            {"name":"XR-web","id":4,"hovered":true,"grabbed":false,"anchor":{"mode":"body"}}
        ]
    })json";

    SDesktopContext fixture() {
        return SDesktopContext::parse(kMonitors, kClients, kOpenxr);
    }
}

TEST_CASE("context: parses monitors, apps, and XR annotations") {
    SDesktopContext c = fixture();
    REQUIRE(c.monitors.size() == 3);
    CHECK(c.xrAvailable);
    CHECK(c.xrState == "focused");

    const SMonitorInfo* code = c.find("XR-code");
    REQUIRE(code);
    CHECK(code->xr);
    CHECK(code->anchorMode == "local");
    CHECK(code->appClasses.size() == 1);
    CHECK(code->appClasses[0] == "nvim");

    const SMonitorInfo* web = c.find("XR-web");
    REQUIRE(web);
    CHECK(web->hovered);
    CHECK(web->anchorMode == "body");
}

TEST_CASE("context: hovered monitor resolves for cheap deixis fallback") {
    SDesktopContext c = fixture();
    const SMonitorInfo* h = c.hoveredMonitor();
    REQUIRE(h);
    CHECK(h->name == "XR-web");
}

TEST_CASE("context: semantic resolution by name stem, app class, and title") {
    SDesktopContext c = fixture();

    // "coding" matches the XR-code name stem ("code" ~ "coding").
    SMonitorMatch m1 = c.resolveMonitor("the coding monitor");
    CHECK(m1.matched);
    CHECK(m1.name == "XR-code");
    CHECK(m1.confidence > 0.6);

    // "youtube" matches only via the browser window title.
    SMonitorMatch m2 = c.resolveMonitor("youtube");
    CHECK(m2.matched);
    CHECK(m2.name == "XR-web");

    // "firefox" matches the window class.
    SMonitorMatch m3 = c.resolveMonitor("firefox");
    CHECK(m3.matched);
    CHECK(m3.name == "XR-web");

    // A word matching nothing -> no match.
    SMonitorMatch m4 = c.resolveMonitor("spreadsheet");
    CHECK_FALSE(m4.matched);
}

TEST_CASE("context: two monitors matching the same app are ambiguous, low confidence") {
    const char* mons = R"json([
        {"id":3,"name":"XR-a","focused":false},
        {"id":4,"name":"XR-b","focused":false}
    ])json";
    const char* cls = R"json([
        {"class":"firefox","title":"Docs","monitor":3,"mapped":true},
        {"class":"firefox","title":"Mail","monitor":4,"mapped":true}
    ])json";
    SDesktopContext c = SDesktopContext::parse(mons, cls, "");
    SMonitorMatch m = c.resolveMonitor("firefox");
    CHECK(m.matched);
    CHECK(m.candidates.size() == 2);
    CHECK(m.confidence < 0.5);
}

TEST_CASE("context: digest is compact and enumerates names") {
    SDesktopContext c = fixture();
    std::string d = c.digest();
    CHECK(d.find("XR-code") != std::string::npos);
    CHECK(d.find("XR-web") != std::string::npos);
    CHECK(d.find("nvim") != std::string::npos);
    // The digest names the XR session state.
    CHECK(d.find("focused") != std::string::npos);

    auto names = c.monitorNames();
    CHECK(names.size() == 3);
    CHECK(c.hasMonitor("XR-web"));
    CHECK_FALSE(c.hasMonitor("XR-nope"));
}

// ---- Generic-name fixtures: names carry NO content signal; only the client
// context (classes, window titles, workspaces) can disambiguate. This encodes the
// user directive: "it should also look at the context of which clients are on
// which monitor", with auto-assigned names like XR-1/XR-2/XR-3.
namespace {
    const char* kGenericMons = R"json([
        {"id":3,"name":"XR-1","focused":false},
        {"id":4,"name":"XR-2","focused":false},
        {"id":5,"name":"XR-3","focused":false}
    ])json";
    const char* kGenericClients = R"json([
        {"class":"mpv","title":"family-video.mp4 - mpv","monitor":3,"mapped":true},
        {"class":"nvim","title":"main.cpp - ~/code/hypxrland - NVIM","monitor":4,"mapped":true},
        {"class":"ghostty","title":"~/code/hypxrland","monitor":4,"mapped":true},
        {"class":"chromium","title":"YouTube - Big Buck Bunny - Chromium","monitor":5,"mapped":true}
    ])json";

    SDesktopContext genericFixture() {
        return SDesktopContext::parse(kGenericMons, kGenericClients, "");
    }
}

TEST_CASE("context: generic names — client context alone disambiguates") {
    SDesktopContext c = genericFixture();

    // "the coding monitor": no name matches anything; the nvim/ghostty monitor's
    // titles carry the project dir (~/code/…), which is the only lexical signal.
    SMonitorMatch coding = c.resolveMonitor("the coding monitor");
    CHECK(coding.matched);
    CHECK(coding.name == "XR-2");
    CHECK(coding.candidates.empty());

    // "youtube": lives only in the chromium window TITLE (class carries nothing).
    SMonitorMatch yt = c.resolveMonitor("youtube");
    CHECK(yt.matched);
    CHECK(yt.name == "XR-3");

    // "the video monitor": the mpv title token "video".
    SMonitorMatch vid = c.resolveMonitor("the video monitor");
    CHECK(vid.matched);
    CHECK(vid.name == "XR-1");
}

TEST_CASE("context: name-coincidence trap — content beats a partial name hit") {
    // Auto-named XR-code is actually PLAYING A VIDEO; the real editor (nvim/ghostty,
    // project dir in titles) lives on generic XR-2. "the coding monitor" must go to
    // the editor: a fuzzy name resemblance (coding~code, 0.5) may not dominate a
    // genuine content match (1.0).
    const char* mons = R"json([
        {"id":3,"name":"XR-code","focused":false},
        {"id":4,"name":"XR-2","focused":false}
    ])json";
    const char* cls = R"json([
        {"class":"mpv","title":"family-video.mp4 - mpv","monitor":3,"mapped":true},
        {"class":"nvim","title":"main.cpp - ~/code/hypxrland - NVIM","monitor":4,"mapped":true},
        {"class":"ghostty","title":"~/code/hypxrland","monitor":4,"mapped":true}
    ])json";
    SDesktopContext c = SDesktopContext::parse(mons, cls, "");

    SMonitorMatch coding = c.resolveMonitor("the coding monitor");
    CHECK(coding.matched);
    CHECK(coding.name == "XR-2"); // content wins over the name coincidence

    // But the LITERAL name stays decisive: "code" is an exact, distinctive name
    // token of XR-code — the user said the monitor's name, so the name wins even
    // though something else is playing on it.
    SMonitorMatch literal = c.resolveMonitor("the code monitor");
    CHECK(literal.matched);
    CHECK(literal.name == "XR-code");

    // Full-name reference on a generic name also stays decisive.
    SMonitorMatch full = c.resolveMonitor("XR-2");
    CHECK(full.matched);
    CHECK(full.name == "XR-2");
}

TEST_CASE("context: digest includes salient title keywords alongside classes") {
    SDesktopContext c = genericFixture();
    std::string d = c.digest();
    // Classes still listed…
    CHECK(d.find("chromium") != std::string::npos);
    CHECK(d.find("nvim") != std::string::npos);
    // …and title keywords now too (this is where "YouTube"/project dirs live).
    CHECK(d.find("youtube") != std::string::npos);
    CHECK(d.find("titles=") != std::string::npos);
    // Class-duplicate tokens are not repeated in titles= (mpv is a class; its title
    // token "mpv" must not re-appear as a keyword).
    size_t line = d.find("XR-1");
    REQUIRE(line != std::string::npos);
    size_t nl = d.find('\n', line);
    std::string xr1 = d.substr(line, nl - line);
    CHECK(xr1.find("titles=") != std::string::npos);
    CHECK(xr1.find("video") != std::string::npos);
    CHECK(xr1.find("mpv") == xr1.rfind("mpv")); // "mpv" appears once (the class)
}

TEST_CASE("context: degrades gracefully with empty/invalid blobs") {
    SDesktopContext c = SDesktopContext::parse("", "", "");
    CHECK(c.monitors.empty());
    CHECK_FALSE(c.xrAvailable);
    CHECK_FALSE(c.resolveMonitor("anything").matched);

    // Garbage does not crash or fabricate monitors.
    SDesktopContext g = SDesktopContext::parse("not json", "{}", "[]");
    CHECK(g.monitors.empty());
}

// ---- live window list + spoken window references (round 2) --------------------------

namespace {
    SDesktopContext windowSnapshot() {
        const char* mons = R"json([{"id":0,"name":"eDP-1","focused":true},{"id":4,"name":"XR-web"}])json";
        const char* cls  = R"json([
            {"address":"0xaaa","class":"firefox","title":"YouTube - Mozilla Firefox",
             "monitor":4,"mapped":true,"focusHistoryID":2},
            {"address":"0xbbb","class":"neovim","title":"Vad.cpp - NVIM",
             "monitor":0,"mapped":true,"focusHistoryID":0},
            {"address":"0xccc","class":"kitty","title":"~/code/hypxrvoice",
             "monitor":0,"mapped":true,"focusHistoryID":1},
            {"address":"0xddd","class":"firefox","title":"Hyprland docs - Mozilla Firefox",
             "monitor":0,"mapped":true,"focusHistoryID":3}
        ])json";
        return SDesktopContext::parse(mons, cls, "");
    }
}

TEST_CASE("context: parses live windows with their addresses and focus recency") {
    SDesktopContext ctx = windowSnapshot();
    REQUIRE(ctx.windows.size() == 4);
    CHECK(ctx.hasWindow("0xaaa"));
    CHECK_FALSE(ctx.hasWindow("0xdeadbeef"));
    REQUIRE(ctx.focusedWindow() != nullptr);
    CHECK(ctx.focusedWindow()->cls == "neovim");
}

TEST_CASE("context: a generic spoken noun resolves to a live app of that kind") {
    SDesktopContext ctx = windowSnapshot();

    // "browser" is not any window's class — it resolves through the closed generic-noun
    // table, then intersects with what is actually running.
    SWindowMatch b = ctx.resolveWindow("browser");
    REQUIRE(b.matched);
    CHECK(b.label == "firefox");
    // Two firefox windows tie; recency picks the one you were last in, and the tie is
    // NOT reported as ambiguous because both are the same app.
    CHECK(b.address == "0xaaa");
    CHECK(b.candidates.empty());

    // "editor" reaches neovim through the same table (stem "nvim" ~ "neovim").
    CHECK(ctx.resolveWindow("editor").label == "neovim");
    CHECK(ctx.resolveWindow("terminal").label == "kitty");
}

TEST_CASE("context: naming the class outright beats a title coincidence") {
    SDesktopContext ctx = windowSnapshot();
    SWindowMatch k = ctx.resolveWindow("kitty");
    REQUIRE(k.matched);
    CHECK(k.address == "0xccc");

    // A word that only appears in a TITLE still resolves — content-first — but scores
    // below an outright class hit.
    SWindowMatch y = ctx.resolveWindow("youtube");
    REQUIRE(y.matched);
    CHECK(y.address == "0xaaa");
}

TEST_CASE("context: an unknown window reference does not match anything") {
    SDesktopContext ctx = windowSnapshot();
    CHECK_FALSE(ctx.resolveWindow("spreadsheet").matched);
    CHECK_FALSE(ctx.resolveWindow("").matched);
    // Stop-words alone carry no identity.
    CHECK_FALSE(ctx.resolveWindow("the window").matched);
}

TEST_CASE("context: a genuinely ambiguous reference reports both apps") {
    // "docs" appears only in the TITLE of two DIFFERENT apps, so neither wins on
    // specificity — the caller is told rather than having one silently picked. (An
    // outright class hit would have settled it: exactness beats a title coincidence.)
    const char* mons = R"json([{"id":0,"name":"eDP-1"}])json";
    const char* cls  = R"json([
        {"address":"0x1","class":"firefox","title":"Hyprland docs","monitor":0,"mapped":true,"focusHistoryID":0},
        {"address":"0x2","class":"neovim","title":"docs.md - NVIM","monitor":0,"mapped":true,"focusHistoryID":1}
    ])json";
    SDesktopContext ctx = SDesktopContext::parse(mons, cls, "");
    SWindowMatch    m   = ctx.resolveWindow("docs");
    REQUIRE(m.matched);
    REQUIRE(m.candidates.size() == 2);
    CHECK(m.confidence < 0.5);

    // Two windows of the SAME app are not ambiguous: recency already answered it.
    SWindowMatch b = ctx.resolveWindow("browser");
    REQUIRE(b.matched);
    CHECK(b.candidates.empty());
}

// ---- spatial monitor references (round 3) ------------------------------------------

TEST_CASE("context: 'the left/right monitor' resolves against the layout, not the name") {
    // Deliberately named so that a name-based guess would get it BACKWARDS: the monitor
    // called "XR-left" sits on the right.
    SDesktopContext c = SDesktopContext::parse(
        R"json([
            {"id":0,"name":"XR-right","x":0,"y":0,"width":1920,"height":1080},
            {"id":1,"name":"XR-left","x":1920,"y":0,"width":1920,"height":1080}
        ])json",
        "", "");
    REQUIRE(c.monitors.size() == 2);
    CHECK(c.find("XR-right")->x == 0);

    SMonitorMatch l = c.resolveSpatialMonitor(ESpatialRef::Left);
    REQUIRE(l.matched);
    CHECK(l.name == "XR-right");
    CHECK(l.candidates.empty());
    CHECK(l.confidence > 0.9);

    SMonitorMatch r = c.resolveSpatialMonitor(ESpatialRef::Right);
    REQUIRE(r.matched);
    CHECK(r.name == "XR-left");
}

TEST_CASE("context: a spatial reference with nothing to compare against is unmatched") {
    SDesktopContext solo = SDesktopContext::parse(R"json([{"id":0,"name":"eDP-1","x":0}])json", "", "");
    CHECK_FALSE(solo.resolveSpatialMonitor(ESpatialRef::Left).matched);
    CHECK_FALSE(solo.resolveSpatialMonitor(ESpatialRef::None).matched);
}

TEST_CASE("context: monitors stacked at the same x make 'left' ambiguous, not arbitrary") {
    SDesktopContext c = SDesktopContext::parse(
        R"json([
            {"id":0,"name":"DP-1","x":0,"y":0},
            {"id":1,"name":"DP-2","x":0,"y":1080},
            {"id":2,"name":"DP-3","x":2560,"y":0}
        ])json",
        "", "");
    SMonitorMatch l = c.resolveSpatialMonitor(ESpatialRef::Left);
    REQUIRE(l.matched);
    CHECK(l.candidates.size() == 2);
    CHECK(l.confidence < 0.5);
    // The right edge is unique, so it still answers cleanly.
    SMonitorMatch r = c.resolveSpatialMonitor(ESpatialRef::Right);
    CHECK(r.name == "DP-3");
    CHECK(r.candidates.empty());
}
