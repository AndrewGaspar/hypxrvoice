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
