#pragma once

#include "Config.hpp"
#include "HudModel.hpp"

#include <cstdint>
#include <string>
#include <vector>

// WP-H8 feedback tier — hypxrvoice as a pure D-Bus CLIENT of the shared `hypxrhud`
// daemon (io.github.andrewgaspar.hypxrhud1). The in-headset HUD render core, EGL/GBM
// session, and fade compositing used to live here (WP-V5, an OpenXR subprocess); they
// now live once in hypxrhud and every XR utility pushes panels to it. hypxrvoice links
// NO OpenXR/EGL/GBM/stb — only sd-bus.
//
// Call mapping (design memo §WP-H8):
//   onListeningStart      -> CreatePanel({slot:"voice", ...})   (keep the returned id)
//   per-word transcript   -> UpdatePanel(id, {lines, confidence})  NO_REPLY_EXPECTED
//   action / clarify       -> UpdatePanel(id, ...) (or CreatePanel if none live)
//   onListeningStop / None -> DismissPanel(id)
//   daemon absent OR RuntimeState != "live" -> notify-send fallback (Feedback.cpp)
//
// Availability is tracked by watching signals, not polling: RuntimeStateChanged keeps
// the runtime state current and NameOwnerChanged tells us when the daemon appears/drops.
// Bus activation means the first CreatePanel starts the daemon if its service file is
// installed; an activation failure is treated exactly like "daemon absent".

// ---- pure view -> panel-props mapping (no sd-bus; unit-tested with no daemon) --------

// hypxrhud's `a{sv}` panel props subset hypxrvoice populates. Mirrors the daemon's
// props schema 1:1 (slot/urgency/kind/lines a(sub)/confidence + the rise/hold/fade
// envelope). `hudPropsFromView` turns an SHudView into this; the sd-bus code in
// HudClient.cpp then serialises it onto the wire.
struct SHudProps {
    std::string           slot;             // target slot (default "voice", config-overridable).
    uint32_t              urgency = 1;       // singleton-slot arbitration weight.
    std::string           kind    = "text";  // text panel (vs the daemon's gauges panels).
    std::vector<SHudLine> lines;             // (text, colorRole, big) — same fields as a(sub).
    float                 confidence = -1.f; // [0,1] draws a bar; <0 omits the confidence prop.
    int                   riseMs  = 110;
    int                   holdMs  = 2600;    // <0 => persistent (listening); daemon honours it.
    int                   fadeMs  = 450;
};

// EHudColor -> hypxrhud colorRole uint (0 Normal, 1 Dim, 2 Accent, 3 Good, 4 Warn,
// 5 Bad). The order is locked to match the daemon's EColor so a small int rides the wire.
uint32_t hudColorRole(EHudColor c);

// HUD state -> singleton-slot urgency. A clarify/error veto outweighs a routine
// listening/action panel so it wins the slot against a lower-priority producer.
uint32_t hudUrgencyForState(EHudState s);

// Map a fully-resolved view onto the daemon props. Pure. `slot` is the configured
// target slot (feedback.hud_slot, default "voice").
SHudProps hudPropsFromView(const SHudView& v, const std::string& slot);

// ---- the sd-bus client (daemon-side; never constructed by oneshot/tests) ------------

struct sd_bus;
struct sd_bus_slot;
struct sd_bus_message;
struct sd_bus_error;

class CHudClient {
  public:
    CHudClient() = default;
    ~CHudClient();

    // Configure from feedback config. When the HUD is enabled this opens the session bus
    // and installs the signal matches so we start watching runtime/ownership immediately
    // (daemon-only path — oneshot/tests never call Feedback::startRuntime). Re-callable on
    // reload; a hud=false config tears the bus down.
    void configure(const SConfig& cfg);

    // Will a push actually reach a LIVE HUD right now? True iff the bus is up, the daemon
    // owns its name, and RuntimeState == "live". The notify-send fallback fires when this
    // is false (daemon absent, no headset/runtime, or hud disabled).
    bool available() const;

    // Create-or-update the single voice panel from a view. Returns available() so the
    // caller keeps the notify-send fallback whenever the message did not reach a live HUD.
    // A create round-trips (to learn the panel id + activate the daemon); updates are
    // fire-and-forget (NO_REPLY_EXPECTED).
    bool show(const SHudView& v);

    // Dismiss the voice panel (mic closed without a command).
    void hide();

    // Drain the bus fd: process RuntimeStateChanged / NameOwnerChanged / PanelDismissed.
    // Called from the daemon tick. Cheap no-op when the bus is closed.
    void poll();

    // Dismiss + release matches + close the bus. Idempotent.
    void stop();

  private:
    bool ensureBus();
    void refreshOwnerAndState(); // GetNameOwner + read RuntimeState (non-activating).
    void degradeNote(const std::string& why);

    // signal trampolines (userdata = this).
    static int onRuntimeStateChanged(sd_bus_message*, void*, sd_bus_error*);
    static int onPanelDismissed(sd_bus_message*, void*, sd_bus_error*);
    static int onNameOwnerChanged(sd_bus_message*, void*, sd_bus_error*);

    SConfig      m_cfg;
    std::string  m_slot        = "voice";
    sd_bus*      m_bus         = nullptr;
    sd_bus_slot* m_matchState  = nullptr;
    sd_bus_slot* m_matchDismiss = nullptr;
    sd_bus_slot* m_matchNoc    = nullptr;

    uint32_t    m_panelId      = 0;         // 0 = no live voice panel.
    bool        m_ownerPresent = false;     // the daemon currently owns the well-known name.
    std::string m_runtimeState = "absent";  // last seen RuntimeState.
    bool        m_notedDegrade = false;     // logged the "using notifications only" note once.
};
