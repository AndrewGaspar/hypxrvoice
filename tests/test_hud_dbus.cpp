// WP-H8 — HUD D-Bus integration test. Drives the feedback tier against the REAL hypxrhud
// daemon on a PRIVATE session bus (dbus-run-session provides $DBUS_SESSION_BUS_ADDRESS),
// NEVER the user's real bus. The CMake wrapper launches this under dbus-run-session and
// sets HYPXRVOICE_HUD_DBUS_TEST_OK=1 + HYPXRHUD_BIN=<sibling hypxrhud binary>; both tests
// self-skip without those markers.
//
//   1. live round-trip: spawn `hypxrhud --no-xr`, then CHudClient listening -> update ->
//      dismiss, asserting the daemon's PanelCount (0 -> 1 -> 1 -> 0) + GetCapabilities.
//   2. daemon absent: no daemon on the bus -> Feedback::emitAction falls back to
//      notify-send, verified through the mocked notify sink.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Feedback.hpp"
#include "HudClient.hpp"
#include "HudModel.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <thread>
#include <unistd.h>

extern char** environ;

using namespace std::chrono_literals;

namespace {
    constexpr const char* kBusName = "io.github.andrewgaspar.hypxrhud";
    constexpr const char* kObjPath = "/io/github/andrewgaspar/hypxrhud";
    constexpr const char* kIface   = "io.github.andrewgaspar.hypxrhud1";

    int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    uint32_t panelCount(sd_bus* bus) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        uint32_t     n   = 0;
        sd_bus_get_property_trivial(bus, kBusName, kObjPath, kIface, "PanelCount", &err, 'u', &n);
        sd_bus_error_free(&err);
        return n;
    }

    bool waitForDaemon(sd_bus* bus, int timeoutMs) {
        const int64_t deadline = nowMs() + timeoutMs;
        while (nowMs() < deadline) {
            sd_bus_error    err   = SD_BUS_ERROR_NULL;
            sd_bus_message* reply = nullptr;
            int  r  = sd_bus_call_method(bus, kBusName, kObjPath, kIface, "GetCapabilities", &err, &reply, "");
            bool ok = r >= 0;
            sd_bus_error_free(&err);
            if (reply) sd_bus_message_unref(reply);
            if (ok) return true;
            std::this_thread::sleep_for(50ms);
        }
        return false;
    }

    bool waitPanelCount(sd_bus* bus, uint32_t want, int timeoutMs) {
        const int64_t deadline = nowMs() + timeoutMs;
        while (nowMs() < deadline) {
            if (panelCount(bus) == want) return true;
            std::this_thread::sleep_for(30ms);
        }
        return panelCount(bus) == want;
    }

    SConfig hudCfg() {
        SConfig c;
        c.feedback.hud        = true;
        c.feedback.hudSlot    = "voice";
        c.feedback.notify     = true;
        c.feedback.stdoutJson = false;   // keep the test output clean
        c.feedback.ttsMode    = "off";   // never shell espeak in a test
        return c;
    }
}

TEST_CASE("hud dbus: listening -> update -> dismiss round-trip against real hypxrhud") {
    const char* bin = std::getenv("HYPXRHUD_BIN");
    if (!std::getenv("HYPXRVOICE_HUD_DBUS_TEST_OK") || !bin) {
        MESSAGE("SKIP: not under the ctest dbus-run-session wrapper (markers unset)");
        return;
    }
    if (::access(bin, X_OK) != 0) {
        MESSAGE("SKIP: hypxrhud binary not built at HYPXRHUD_BIN — build the sibling repo first");
        return;
    }
    REQUIRE(std::getenv("DBUS_SESSION_BUS_ADDRESS") != nullptr); // private bus.

    // Spawn the daemon in runtime-absent mode on this private bus.
    pid_t       pid    = -1;
    const char* argv[] = {bin, "--no-xr", nullptr};
    REQUIRE(posix_spawn(&pid, bin, nullptr, nullptr, const_cast<char* const*>(argv), environ) == 0);
    REQUIRE(pid > 0);

    sd_bus* probe = nullptr;
    REQUIRE(sd_bus_open_user(&probe) >= 0);

    struct Guard {
        pid_t pid; sd_bus* bus;
        ~Guard() {
            if (bus) sd_bus_flush_close_unref(bus);
            if (pid > 0) { kill(pid, SIGTERM); int st = 0; waitpid(pid, &st, 0); }
        }
    } guard{pid, probe};

    REQUIRE(waitForDaemon(probe, 8000));
    CHECK(panelCount(probe) == 0);

    {
        CHudClient client;
        client.configure(hudCfg());

        // onListeningStart: create the voice panel (kept by id).
        bool live = client.show(hudForListening("", hudCfg()));
        CHECK_FALSE(live); // --no-xr => RuntimeState "absent" => not carried live => notify path
        CHECK(waitPanelCount(probe, 1, 2000));

        // per-word transcript: fire-and-forget UpdatePanel — count stays 1.
        client.show(hudForListening("open the browser", hudCfg()));
        client.poll();
        CHECK(waitPanelCount(probe, 1, 1000));

        // a recognised action replaces the listening content in-place (still one panel).
        SAction a; a.verb = EVerb::Anchor; a.target = "XR-code"; a.confidence = 0.8;
        a.targetSource = ETargetSource::Named;
        SExecPlan plan; plan.ok = true;
        client.show(hudForAction(a, plan, hudCfg()));
        client.poll();
        CHECK(waitPanelCount(probe, 1, 1000));

        // onListeningStop: dismiss.
        client.hide();
        CHECK(waitPanelCount(probe, 0, 2000));
    }
}

TEST_CASE("hud dbus: daemon absent falls back to notify-send") {
    if (!std::getenv("HYPXRVOICE_HUD_DBUS_TEST_OK")) {
        MESSAGE("SKIP: not under the ctest dbus-run-session wrapper (marker unset)");
        return;
    }
    REQUIRE(std::getenv("DBUS_SESSION_BUS_ADDRESS") != nullptr); // private bus, no daemon on it.

    // Capture the notify-send fallback instead of spawning it.
    std::vector<std::pair<std::string, std::string>> toasts;
    Feedback::_setNotifySinkForTest([&](const std::string& s, const std::string& b) {
        toasts.emplace_back(s, b);
    });

    SConfig cfg = hudCfg();
    Feedback::startRuntime(cfg); // opens the bus; GetNameOwner => no daemon => absent.

    // An actionable command with the HUD enabled but unreachable -> notify-send fires.
    SAction a; a.verb = EVerb::Anchor; a.target = "XR-code"; a.confidence = 0.9;
    a.targetSource = ETargetSource::Named;
    SExecPlan plan; plan.ok = true;
    Feedback::emitAction(a, plan, cfg);

    Feedback::stopRuntime();
    Feedback::_setNotifySinkForTest(nullptr); // restore real notify-send.

    REQUIRE(toasts.size() >= 1);
    CHECK(toasts.front().first == "hypxrvoice");
    CHECK(toasts.front().second.find("anchor") != std::string::npos);
    CHECK(toasts.front().second.find("XR-code") != std::string::npos);
}
