#include "Egl.hpp"
#include "HudSession.hpp"
#include "Log.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>

// The hypxrvoice HUD overlay subprocess. The daemon spawns it and streams SHudView
// JSON lines to its stdin (HudMsg wire format); this process owns the OpenXR overlay
// session and renders a head-locked quad. It exits kExitNoRuntime when it cannot bring
// up XR so the daemon degrades to notify-send. NOT meant to be run by hand normally;
// `--self-test` brings up the session with no daemon (Ctrl-C to quit) for live checks.
//
// SAFETY: only ONE XR runtime per box. This is launched by the daemon on demand; do
// not start it alongside another live session.

std::atomic<bool> g_stopRequested{false};
static void        onSignal(int) { g_stopRequested.store(true); }

static constexpr int kExitNoRuntime = 3; // keep in sync with HudOverlay.cpp.

static bool parseVec3(const char* s, float& x, float& y, float& z) {
    return s && std::sscanf(s, "%f,%f,%f", &x, &y, &z) == 3;
}

static void printUsage(const char* a0) {
    std::fprintf(stderr,
                 "hypxrvoice-hud — in-headset HUD overlay for hypxrvoice (spawned by the daemon)\n"
                 "\nUsage: %s [options]   (reads SHudView JSON lines on stdin)\n"
                 "  --pose x,y,z     VIEW-space centre, metres (default 0,-0.25,-1.0)\n"
                 "  --size <m>       Quad width in metres (default 0.42)\n"
                 "  --opacity <0..1> Peak layer opacity (default 0.92)\n"
                 "  --z <int>        Overlay sessionLayersPlacement (default 20)\n"
                 "  --gpu <path>     DRM render node (must match the runtime)\n"
                 "  --self-test      Stay up with no daemon (for a live check); Ctrl-C to quit\n"
                 "  -h, --help\n",
                 a0);
}

int main(int argc, char** argv) {
    CHudSession::SParams p;
    std::string          gpu;
    bool                 selfTest = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto        need = [&](const char* n) -> const char* {
            if (i + 1 >= argc) { Log::log(Log::ERR, "{} requires a value", n); std::exit(2); }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { printUsage(argv[0]); return 0; }
        else if (a == "--pose") { if (!parseVec3(need("--pose"), p.posX, p.posY, p.posZ)) { Log::log(Log::ERR, "--pose wants x,y,z"); return 2; } }
        else if (a == "--size") p.sizeW = std::strtof(need("--size"), nullptr);
        else if (a == "--opacity") p.opacity = std::strtof(need("--opacity"), nullptr);
        else if (a == "--z") p.overlayZ = std::atoi(need("--z"));
        else if (a == "--gpu") gpu = need("--gpu");
        else if (a == "--self-test") selfTest = true;
        else { Log::log(Log::ERR, "unknown option: {}", a); printUsage(argv[0]); return 2; }
    }
    (void)selfTest; // stdin EOF ends the loop regardless; --self-test just documents intent.

    // SA_RESTART: the handler only sets a flag; without it a signal EINTRs Monado's
    // blocking IPC inside xrEndFrame -> INSTANCE_LOST (hypxrpaper lesson).
    struct sigaction sa = {};
    sa.sa_handler       = onSignal;
    sa.sa_flags         = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    CEgl egl;
    if (!egl.init(gpu)) {
        Log::log(Log::ERR, "[hud] EGL init failed");
        return kExitNoRuntime;
    }

    int rc = 0;
    {
        CHudSession session;
        if (!session.init(egl, p)) {
            Log::log(Log::ERR, "[hud] XR session init failed — HUD unavailable");
            session.destroy();
            egl.destroy();
            return kExitNoRuntime;
        }
        session.run();
        session.destroy();
    }
    egl.destroy();
    Log::log(Log::INFO, "[hud] exited");
    return rc;
}
