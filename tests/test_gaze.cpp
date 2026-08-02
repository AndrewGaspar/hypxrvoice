#include "doctest.h"

#include "DesktopContext.hpp"
#include "GazeResolver.hpp"

#include <string>

namespace {
    // Build a `gaze at` reply for a monitor pick (or a passthrough miss when id<0).
    std::string gazeJson(int id, const char* name, double dwell, int64_t matched) {
        std::string sel = id >= 0 ? "true" : "false";
        std::string nm  = id >= 0 ? name : "";
        char buf[512];
        std::snprintf(buf, sizeof(buf), R"json({
            "ok":true,"viewValid":true,"timestampMs":%lld,
            "head":{"pos":[0.1,1.4,-0.2],"quat":[0,0,0,1],"forward":[0,0,-1]},
            "gaze":{"monitorId":%d,"name":"%s","selected":%s,"dwellSec":%.3f},
            "query":{"requestedTimestampMs":%lld,"matchedTimestampMs":%lld,"ageMs":0}
        })json",
                      (long long)matched, id, nm.c_str(), sel.c_str(), dwell,
                      (long long)matched, (long long)matched);
        return buf;
    }

    SDesktopContext ctxWith(const char* name) {
        std::string mons = std::string("[{\"id\":3,\"name\":\"") + name + "\"}]";
        std::string xr   = std::string("{\"state\":\"focused\",\"monitors\":[{\"name\":\"") +
                         name + "\",\"id\":3,\"hovered\":true,\"anchor\":{\"mode\":\"local\"}}]}";
        return SDesktopContext::parse(mons, "", xr);
    }
}

TEST_CASE("gaze: parses a full reply") {
    SGazeSample s = SGazeSample::parse(gazeJson(3, "XR-1", 0.25, 84213765));
    CHECK(s.ok);
    CHECK(s.viewValid);
    CHECK(s.monitorId == 3);
    CHECK(s.name == "XR-1");
    CHECK(s.selected);
    CHECK(s.dwellSec == doctest::Approx(0.25));
    CHECK(s.matchedMs == 84213765);
    CHECK(s.pos[1] == doctest::Approx(1.4));
}

TEST_CASE("gaze: ok:false and malformed replies are rejected") {
    CHECK_FALSE(SGazeSample::parse(R"json({"ok":false})json").ok);
    CHECK_FALSE(SGazeSample::parse("garbage").ok);
    CHECK_FALSE(SGazeSample::parse("").ok);
}

TEST_CASE("gaze: stable window agrees on one monitor") {
    SDesktopContext ctx = ctxWith("XR-1");
    SGazeConfig cfg; cfg.samples = 5; cfg.windowMs = 300; cfg.leadMs = 200;

    // Every ring sample says XR-1 with a healthy dwell.
    GazeQueryFn q = [](int64_t at) { return gazeJson(3, "XR-1", 0.30, at); };
    SGazeResolution r = resolveDeixis(100000, cfg, q, &ctx);
    CHECK(r.valid);
    CHECK(r.monitorId == 3);
    CHECK(r.name == "XR-1");
    CHECK(r.stable);
    CHECK(r.agreeCount == 5);
    CHECK(r.sampleCount == 5);
    // The requested time is lead-shifted; matched reflects the sample used.
    CHECK(r.requestedMs == 100000);
}

TEST_CASE("gaze: one saccade cannot hijack the modal pick") {
    SDesktopContext ctx = ctxWith("XR-1");
    SGazeConfig cfg; cfg.samples = 5; cfg.windowMs = 400; cfg.leadMs = 0;

    // 4 samples on XR-1 (id 3), one stray sample on a different monitor id 9.
    GazeQueryFn q = [](int64_t at) {
        // The stray is at the earliest sample (at == anchor-window == 100000-400).
        if (at <= 99600) return gazeJson(9, "XR-9", 0.05, at);
        return gazeJson(3, "XR-1", 0.30, at);
    };
    SGazeResolution r = resolveDeixis(100000, cfg, q, &ctx);
    CHECK(r.monitorId == 3); // majority wins
    CHECK(r.stable);
    CHECK(r.agreeCount == 4);
}

TEST_CASE("gaze: passthrough miss is a valid world-point deixis (no monitor)") {
    SDesktopContext ctx = ctxWith("XR-1");
    SGazeConfig cfg; cfg.samples = 3; cfg.windowMs = 200; cfg.leadMs = 100;
    GazeQueryFn q = [](int64_t at) { return gazeJson(-1, "", 0.0, at); };
    SGazeResolution r = resolveDeixis(50000, cfg, q, &ctx);
    CHECK(r.valid);
    CHECK(r.monitorId == -1);
    CHECK(r.name.empty());
}

TEST_CASE("gaze: a pick naming a dead monitor is dropped") {
    // Ring says XR-ghost, but that monitor is not in the live context.
    SDesktopContext ctx = ctxWith("XR-1");
    SGazeConfig cfg; cfg.samples = 3; cfg.windowMs = 200; cfg.leadMs = 0;
    GazeQueryFn q = [](int64_t at) { return gazeJson(7, "XR-ghost", 0.5, at); };
    SGazeResolution r = resolveDeixis(50000, cfg, q, &ctx);
    CHECK(r.valid);
    CHECK(r.name.empty());     // ghost dropped
    CHECK(r.monitorId == -1);
}

// ---------------------------------------------------------------------------
// Round 5: "here" is a point in FRONT of the user, never the user's own head.
// The first non-dry-run live round executed `place active at 0.789,1.188,0.638`
// and the monitor landed AT the HMD origin — the daemon had been passing the head
// position verbatim. These pin the projection down.
// ---------------------------------------------------------------------------

TEST_CASE("gaze: place point is projected along forward, not the head origin") {
    double pos[3] = {0.5, 1.4, -1.2};
    double fwd[3] = {0, 0, -1};
    double out[3] = {0, 0, 0};
    double dist    = 0;
    projectPlacePoint(pos, fwd, false, nullptr, 1.3, 0.5, out, &dist);
    CHECK(out[0] == doctest::Approx(0.5));
    CHECK(out[1] == doctest::Approx(1.4));   // eye height is preserved…
    CHECK(out[2] == doctest::Approx(-2.5));  // …and the point is 1.3 m ahead
    CHECK(dist == doctest::Approx(1.3));
}

TEST_CASE("gaze: a non-unit forward is normalized before projection") {
    double pos[3] = {0, 1.2, 0};
    double fwd[3] = {0, 0, -4};   // length 4, not 1
    double out[3], dist;
    projectPlacePoint(pos, fwd, false, nullptr, 1.3, 0.5, out, &dist);
    CHECK(out[2] == doctest::Approx(-1.3));
    CHECK(dist == doctest::Approx(1.3));
}

TEST_CASE("gaze: the minimum-distance clamp makes an in-head placement impossible") {
    double pos[3] = {0.1, 1.2, -0.3};
    double fwd[3] = {0, 0, -1};
    double out[3], dist;

    // A configured distance INSIDE the head sphere is pushed back out.
    projectPlacePoint(pos, fwd, false, nullptr, 0.05, 0.5, out, &dist);
    CHECK(dist == doctest::Approx(0.5));
    CHECK(out[2] == doctest::Approx(-0.8));

    // So is a compositor-reported hit point that lands on the wearer.
    double hit[3] = {0.1, 1.2, -0.3}; // exactly the head
    projectPlacePoint(pos, fwd, true, hit, 1.3, 0.5, out, &dist);
    CHECK(dist == doctest::Approx(0.5));
    CHECK(out[2] == doctest::Approx(-0.8));
}

TEST_CASE("gaze: a degenerate forward still lands the point outside the head") {
    double pos[3] = {0, 1.5, 0};
    double fwd[3] = {0, 0, 0}; // no view pose in the sample
    double out[3], dist;
    projectPlacePoint(pos, fwd, false, nullptr, 1.3, 0.5, out, &dist);
    CHECK(dist >= 0.5);
    CHECK(out[2] == doctest::Approx(-1.3));
    CHECK(out[1] == doctest::Approx(1.5)); // y stays at eye height, not 0
}

TEST_CASE("gaze: a nonsense place_distance_m falls back rather than collapsing to zero") {
    double pos[3] = {0, 1.4, 0};
    double fwd[3] = {0, 0, -1};
    double out[3], dist;
    projectPlacePoint(pos, fwd, false, nullptr, 0.0, 0.5, out, &dist);
    CHECK(dist >= 0.5);
    projectPlacePoint(pos, fwd, false, nullptr, -3.0, 0.5, out, &dist);
    CHECK(dist >= 0.5);
}

TEST_CASE("gaze: a resolved deixis carries a projected place point, and y stays sane") {
    SDesktopContext ctx = ctxWith("XR-1");
    SGazeConfig cfg; cfg.samples = 3; cfg.windowMs = 200; cfg.leadMs = 100;
    // gazeJson()'s head is pos [0.1,1.4,-0.2] forward [0,0,-1] — a seated head pose.
    GazeQueryFn q = [](int64_t at) { return gazeJson(-1, "", 0.0, at); };
    SGazeResolution r = resolveDeixis(50000, cfg, q, &ctx);
    REQUIRE(r.valid);
    CHECK_FALSE(r.placeFromHit);
    CHECK(r.placeDistM == doctest::Approx(cfg.placeDistanceM));
    CHECK(r.place[2] == doctest::Approx(-0.2 - cfg.placeDistanceM));
    // The head origin is preserved separately and is NOT the place point.
    CHECK(r.pos[2] == doctest::Approx(-0.2));
    CHECK(r.place[1] == doctest::Approx(1.4)); // eye height, not floor and not overhead
    CHECK(r.place[1] > 0.5);
    CHECK(r.place[1] < 2.5);
}

TEST_CASE("gaze: a compositor-reported hit point wins over the projection") {
    // Forward-compat: today's compositor sends no hitPoint, but if it ever does we must
    // prefer the real intersection to a fixed-distance guess.
    SDesktopContext ctx = ctxWith("XR-1");
    SGazeConfig cfg; cfg.samples = 1; cfg.windowMs = 0; cfg.leadMs = 0;
    GazeQueryFn q = [](int64_t at) {
        char b[420];
        std::snprintf(b, sizeof(b), R"json({
            "ok":true,"viewValid":true,"timestampMs":%lld,
            "head":{"pos":[0,1.4,0],"quat":[0,0,0,1],"forward":[0,0,-1]},
            "gaze":{"monitorId":3,"name":"XR-1","selected":true,"dwellSec":0.4,
                    "hitPoint":[0.2,1.3,-2.1]},
            "query":{"matchedTimestampMs":%lld,"ageMs":0}})json",
                      (long long)at, (long long)at);
        return std::string(b);
    };
    SGazeResolution r = resolveDeixis(50000, cfg, q, &ctx);
    REQUIRE(r.valid);
    CHECK(r.placeFromHit);
    CHECK(r.place[0] == doctest::Approx(0.2));
    CHECK(r.place[1] == doctest::Approx(1.3));
    CHECK(r.place[2] == doctest::Approx(-2.1));
}

// ---------------------------------------------------------------------------
// Round 8: the stability window decides WHICH candidate was meant, never WHEN.
//
// Every live "create a monitor here" logged ageMs ≈ 500 — exactly deixis_lead_ms(200) +
// deixis_window_ms(300), i.e. the OLDEST sample in the window. The representative sample
// used to be "highest dwellSec, first one wins a tie", and a deixis aimed at passthrough
// (which "here" usually is) reports dwellSec = 0.000 in EVERY sample, so the tie-break
// fell through to first-pushed — the oldest. The monitor then landed where the user had
// been looking half a second before they spoke.
// ---------------------------------------------------------------------------

namespace {
    // A ring whose head pose encodes the instant it was asked for: x = at/1000. Lets a
    // test read the timestamp of the sample that actually became the pose.
    std::string gazeAtPose(int id, const char* name, double dwell, int64_t at) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), R"json({
            "ok":true,"viewValid":true,"timestampMs":%lld,
            "head":{"pos":[%.6f,1.4,0],"quat":[0,0,0,1],"forward":[0,0,-1]},
            "gaze":{"monitorId":%d,"name":"%s","selected":%s,"dwellSec":%.3f},
            "query":{"requestedTimestampMs":%lld,"matchedTimestampMs":%lld,"ageMs":0}
        })json",
                      (long long)at, static_cast<double>(at) / 1000.0, id,
                      id >= 0 ? name : "", id >= 0 ? "true" : "false", dwell,
                      (long long)at, (long long)at);
        return buf;
    }
}

TEST_CASE("gaze: the pose comes from the WORD, not from the oldest window sample") {
    SDesktopContext ctx = ctxWith("XR-1");
    SGazeConfig cfg; cfg.samples = 5; cfg.windowMs = 300; cfg.leadMs = 200;

    // The live shape: a passthrough deixis, so dwellSec ties at 0.000 everywhere and the
    // old "highest dwell" pick degenerated to first-pushed.
    GazeQueryFn q = [](int64_t at) { return gazeAtPose(-1, "", 0.0, at); };
    SGazeResolution r = resolveDeixis(100000, cfg, q, &ctx);
    REQUIRE(r.valid);
    CHECK(r.requestedMs == 100000);
    // The lead shift is deliberate and is the WHOLE offset: 200 ms, not 200 + 300.
    CHECK(r.matchedMs == 99800);
    CHECK(r.ageMs == 200);
    CHECK(r.pos[0] == doctest::Approx(99.8)); // the pose really is the anchor sample's
}

TEST_CASE("gaze: the modal vote still spans the whole window") {
    SDesktopContext ctx = ctxWith("XR-1");
    SGazeConfig cfg; cfg.samples = 5; cfg.windowMs = 400; cfg.leadMs = 0;

    // A saccade onto XR-9 across the two NEWEST samples. The pose must come from the word
    // instant, but the CANDIDATE is still the majority — the newest sample alone cannot
    // carry the pick.
    GazeQueryFn q = [](int64_t at) {
        if (at > 99800) return gazeAtPose(9, "XR-9", 0.05, at);
        return gazeAtPose(3, "XR-1", 0.30, at);
    };
    SGazeResolution r = resolveDeixis(100000, cfg, q, &ctx);
    CHECK(r.monitorId == 3);   // majority still wins the identity
    CHECK(r.agreeCount == 3);
    CHECK(r.stable);
    // …and the pose is the newest sample that AGREED with that majority, not the newest
    // sample overall (which was the saccade).
    CHECK(r.matchedMs == 99800);
    CHECK(r.pos[0] == doctest::Approx(99.8));
}

TEST_CASE("gaze: a settled dwell still breaks a tie between equidistant samples") {
    SDesktopContext ctx = ctxWith("XR-1");
    SGazeConfig cfg; cfg.samples = 1; cfg.windowMs = 0; cfg.leadMs = 150;
    GazeQueryFn q = [](int64_t at) { return gazeAtPose(3, "XR-1", 0.8, at); };
    SGazeResolution r = resolveDeixis(50000, cfg, q, &ctx);
    REQUIRE(r.valid);
    CHECK(r.matchedMs == 49850); // one sample, at the lead-shifted word instant
    CHECK(r.ageMs == 150);
    CHECK(r.dwellSec == doctest::Approx(0.8));
}
