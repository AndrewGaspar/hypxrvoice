#pragma once

#include "Config.hpp"
#include "HudModel.hpp"

#include <string>

// WP-V5 daemon-side HUD manager. Owns the `hypxrvoice-hud` overlay subprocess: spawns
// it lazily, streams SHudView updates to its stdin, and degrades cleanly (a single
// logged note, notify-send stays intact) when the binary is missing or the subprocess
// exits — e.g. no XR runtime, which the subprocess reports by exiting. The daemon
// never links OpenXR/EGL itself; all of that lives in the isolated subprocess (single
// -threaded, EGL-context-held — Monado's fence contract by construction). Only the
// daemon process ever constructs this, so unit tests / offline tools never spawn XR.
class CHudOverlay {
  public:
    CHudOverlay() = default;
    ~CHudOverlay();

    // Configure from the feedback config. Does not spawn yet (spawn is lazy on the
    // first send) so a daemon that never triggers feedback pays nothing.
    void configure(const SConfig& cfg);

    bool enabled() const { return m_cfg.feedback.hud && !m_degraded; }

    // Push a view to the overlay. Spawns the subprocess on first use. On any failure
    // (binary absent, pipe broken, subprocess gone) it degrades once and returns
    // false so the caller keeps the notify-send fallback.
    bool send(const SHudView& v);

    // Reap the subprocess if it exited; call from the daemon tick. Detects a runtime
    // -unavailable exit and degrades with a one-time note.
    void poll();

    // Close the pipe (subprocess sees EOF and exits) and reap.
    void stop();

  private:
    bool ensureSpawned();
    void degrade(const std::string& why);

    SConfig m_cfg;
    int     m_writeFd    = -1;
    long    m_childPid   = -1;
    bool    m_spawned    = false;
    bool    m_degraded   = false;
    bool    m_notedDegrade = false;
};
