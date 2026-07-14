#include "HudClient.hpp"
#include "Log.hpp"

#include <cstring>
#include <systemd/sd-bus.h>

// The shared hypxrhud daemon's well-known name / object / interface (WP-H3). Kept in
// sync with hypxrhud/src/Dbus.hpp (kBusName/kObjPath/kIface).
namespace {
    constexpr const char* kBusName = "io.github.andrewgaspar.hypxrhud";
    constexpr const char* kObjPath = "/io/github/andrewgaspar/hypxrhud";
    constexpr const char* kIface   = "io.github.andrewgaspar.hypxrhud1";
}

// ---- pure mapping -------------------------------------------------------------------

uint32_t hudColorRole(EHudColor c) {
    switch (c) {
        case EHudColor::Normal: return 0;
        case EHudColor::Dim:    return 1;
        case EHudColor::Accent: return 2;
        case EHudColor::Good:   return 3;
        case EHudColor::Warn:   return 4;
        case EHudColor::Bad:    return 5;
    }
    return 0;
}

uint32_t hudUrgencyForState(EHudState s) {
    switch (s) {
        case EHudState::Hidden:    return 0;
        case EHudState::Listening: return 1;
        case EHudState::Action:    return 2;
        case EHudState::Clarify:   return 3; // a question the user must answer wins the slot.
        case EHudState::Error:     return 3; // a refusal likewise outranks routine panels.
    }
    return 1;
}

SHudProps hudPropsFromView(const SHudView& v, const std::string& slot) {
    SHudProps p;
    p.slot       = slot.empty() ? "voice" : slot;
    p.urgency    = hudUrgencyForState(v.state);
    p.kind       = "text";
    p.lines      = v.lines;
    p.confidence = v.confidence;
    p.riseMs     = v.riseMs;
    p.holdMs     = v.holdMs;
    p.fadeMs     = v.fadeMs;
    return p;
}

// ---- sd-bus serialisation of the props ----------------------------------------------

namespace {
    void appendStr(sd_bus_message* m, const char* key, const char* val) {
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", key);
        sd_bus_message_append(m, "v", "s", val);
        sd_bus_message_close_container(m);
    }
    void appendU(sd_bus_message* m, const char* key, uint32_t val) {
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", key);
        sd_bus_message_append(m, "v", "u", val);
        sd_bus_message_close_container(m);
    }
    void appendI(sd_bus_message* m, const char* key, int32_t val) {
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", key);
        sd_bus_message_append(m, "v", "i", val);
        sd_bus_message_close_container(m);
    }
    void appendD(sd_bus_message* m, const char* key, double val) {
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", key);
        sd_bus_message_append(m, "v", "d", val);
        sd_bus_message_close_container(m);
    }

    // Serialise SHudProps onto an already-created method-call message as one `a{sv}`.
    void appendProps(sd_bus_message* m, const SHudProps& p) {
        sd_bus_message_open_container(m, 'a', "{sv}");

        appendStr(m, "slot", p.slot.c_str());
        appendU(m, "urgency", p.urgency);
        appendStr(m, "kind", p.kind.c_str());

        // lines: a(sub) = [(text, colorRole, big)]
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", "lines");
        sd_bus_message_open_container(m, 'v', "a(sub)");
        sd_bus_message_open_container(m, 'a', "(sub)");
        for (const auto& ln : p.lines)
            sd_bus_message_append(m, "(sub)", ln.text.c_str(), hudColorRole(ln.color),
                                  ln.big ? 1 : 0);
        sd_bus_message_close_container(m);
        sd_bus_message_close_container(m);
        sd_bus_message_close_container(m);

        if (p.confidence >= 0.f)
            appendD(m, "confidence", p.confidence);

        // Fade envelope — forwarded so the daemon reproduces the same rise/hold/fade
        // (hold_ms<0 keeps the listening panel up). Geometry/opacity are hypxrhud config.
        appendI(m, "rise_ms", p.riseMs);
        appendI(m, "hold_ms", p.holdMs);
        appendI(m, "fade_ms", p.fadeMs);

        sd_bus_message_close_container(m); // a{sv}
    }
}

// ---- CHudClient ---------------------------------------------------------------------

CHudClient::~CHudClient() {
    stop();
}

void CHudClient::configure(const SConfig& cfg) {
    m_cfg  = cfg;
    m_slot = cfg.feedback.hudSlot.empty() ? "voice" : cfg.feedback.hudSlot;

    if (!cfg.feedback.hud) {
        // HUD turned off (e.g. via reload): drop any panel + the bus watch.
        stop();
        return;
    }
    m_notedDegrade = false; // a reload re-arms the one-time degrade note.
    // Daemon-only path: open the bus now so we watch runtime/ownership signals live.
    ensureBus();
}

bool CHudClient::available() const {
    return m_bus != nullptr && m_ownerPresent && m_runtimeState == "live";
}

bool CHudClient::ensureBus() {
    if (m_bus)
        return true;

    sd_bus* bus = nullptr;
    int     r   = sd_bus_open_user(&bus);
    if (r < 0 || !bus) {
        degradeNote("session bus unavailable");
        if (bus)
            sd_bus_unref(bus);
        return false;
    }
    m_bus = bus;

    // Watch the runtime state + our-panel dismissals + the daemon's name ownership. All
    // best-effort: a failed match just means we fall back to per-call round-trips.
    sd_bus_match_signal(m_bus, &m_matchState, kBusName, kObjPath, kIface,
                        "RuntimeStateChanged", &CHudClient::onRuntimeStateChanged, this);
    sd_bus_match_signal(m_bus, &m_matchDismiss, kBusName, kObjPath, kIface,
                        "PanelDismissed", &CHudClient::onPanelDismissed, this);
    sd_bus_match_signal(m_bus, &m_matchNoc, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                        "org.freedesktop.DBus", "NameOwnerChanged",
                        &CHudClient::onNameOwnerChanged, this);

    refreshOwnerAndState();
    return true;
}

// Non-activating: ask the bus daemon who owns the name (if anyone), and if present read
// RuntimeState. Never triggers activation — that only happens on a real method call.
void CHudClient::refreshOwnerAndState() {
    if (!m_bus)
        return;
    // GetNameOwner errors with NameHasNoOwner when the daemon isn't up (never activates).
    sd_bus_error    err   = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_call_method(m_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                               "org.freedesktop.DBus", "GetNameOwner", &err, &reply, "s", kBusName);
    if (reply)
        sd_bus_message_unref(reply);
    if (r < 0) {
        m_ownerPresent = false;
        m_runtimeState = "absent";
        sd_bus_error_free(&err);
        return;
    }
    m_ownerPresent = true;
    sd_bus_error_free(&err);

    sd_bus_error serr = SD_BUS_ERROR_NULL;
    char*        st   = nullptr;
    if (sd_bus_get_property_string(m_bus, kBusName, kObjPath, kIface, "RuntimeState", &serr, &st) >= 0 && st)
        m_runtimeState = st;
    free(st);
    sd_bus_error_free(&serr);
}

void CHudClient::degradeNote(const std::string& why) {
    if (!m_notedDegrade) {
        Log::log(Log::WARN, "HUD daemon unavailable ({}); using notifications only", why);
        m_notedDegrade = true;
    }
}

bool CHudClient::show(const SHudView& v) {
    if (!m_cfg.feedback.hud)
        return false;
    if (!ensureBus())
        return false;

    SHudProps props = hudPropsFromView(v, m_slot);

    if (m_panelId == 0) {
        // CreatePanel — round-trips (learns the id) AND bus-activates the daemon.
        sd_bus_message* m = nullptr;
        if (sd_bus_message_new_method_call(m_bus, &m, kBusName, kObjPath, kIface, "CreatePanel") < 0 || !m)
            return false;
        appendProps(m, props);

        sd_bus_error    err   = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        int             r     = sd_bus_call(m_bus, m, 0, &err, &reply);
        if (r < 0) {
            degradeNote(err.name ? err.name : "CreatePanel failed");
            m_ownerPresent = false; // NameOwnerChanged will flip this back when it appears.
            sd_bus_error_free(&err);
            sd_bus_message_unref(m);
            return false;
        }
        sd_bus_message_read(reply, "u", &m_panelId);
        sd_bus_error_free(&err);
        sd_bus_message_unref(reply);
        sd_bus_message_unref(m);

        // The create just (re)activated the daemon; learn its runtime state now so the
        // notify-send decision below is accurate this first time.
        m_notedDegrade = false;
        refreshOwnerAndState();
    } else {
        // UpdatePanel — fire-and-forget (NO_REPLY_EXPECTED), no round-trip.
        sd_bus_message* m = nullptr;
        if (sd_bus_message_new_method_call(m_bus, &m, kBusName, kObjPath, kIface, "UpdatePanel") < 0 || !m)
            return false;
        sd_bus_message_append(m, "u", m_panelId);
        appendProps(m, props);
        sd_bus_message_set_expect_reply(m, 0);
        sd_bus_send(m_bus, m, nullptr);
        sd_bus_message_unref(m);
    }

    return available();
}

void CHudClient::hide() {
    if (!m_bus || m_panelId == 0)
        return;
    sd_bus_error    err = SD_BUS_ERROR_NULL;
    sd_bus_message* rep = nullptr;
    sd_bus_call_method(m_bus, kBusName, kObjPath, kIface, "DismissPanel", &err, &rep, "u", m_panelId);
    if (rep)
        sd_bus_message_unref(rep);
    sd_bus_error_free(&err);
    m_panelId = 0;
}

void CHudClient::poll() {
    if (!m_bus)
        return;
    while (sd_bus_process(m_bus, nullptr) > 0) { /* drain queued signals */ }
}

void CHudClient::stop() {
    if (m_bus && m_panelId != 0)
        hide();
    if (m_matchState)   { sd_bus_slot_unref(m_matchState);   m_matchState   = nullptr; }
    if (m_matchDismiss) { sd_bus_slot_unref(m_matchDismiss); m_matchDismiss = nullptr; }
    if (m_matchNoc)     { sd_bus_slot_unref(m_matchNoc);     m_matchNoc     = nullptr; }
    if (m_bus)          { sd_bus_flush_close_unref(m_bus);   m_bus          = nullptr; }
    m_panelId      = 0;
    m_ownerPresent = false;
    m_runtimeState = "absent";
}

// ---- signal handlers ----------------------------------------------------------------

int CHudClient::onRuntimeStateChanged(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto*       self  = static_cast<CHudClient*>(userdata);
    const char* state = nullptr;
    if (sd_bus_message_read(m, "s", &state) >= 0 && state) {
        self->m_runtimeState = state;
        self->m_ownerPresent = true; // a signal from it proves it owns the name.
    }
    return 0;
}

int CHudClient::onPanelDismissed(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto*       self   = static_cast<CHudClient*>(userdata);
    uint32_t    id     = 0;
    const char* reason = nullptr;
    if (sd_bus_message_read(m, "us", &id, &reason) >= 0 && id == self->m_panelId)
        self->m_panelId = 0; // the daemon expired/preempted our panel — forget the id.
    return 0;
}

int CHudClient::onNameOwnerChanged(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto*       self     = static_cast<CHudClient*>(userdata);
    const char* name     = nullptr;
    const char* oldOwner = nullptr;
    const char* newOwner = nullptr;
    if (sd_bus_message_read(m, "sss", &name, &oldOwner, &newOwner) < 0)
        return 0;
    if (!name || std::strcmp(name, kBusName) != 0)
        return 0;
    if (newOwner && newOwner[0] != '\0') {
        // Daemon appeared — read its runtime state and let the next show() create fresh.
        self->m_ownerPresent = true;
        self->m_notedDegrade = false;
        self->refreshOwnerAndState();
    } else {
        // Daemon gone — our panel(s) died with the connection; degrade to notify-send.
        self->m_ownerPresent = false;
        self->m_runtimeState = "absent";
        self->m_panelId      = 0;
    }
    return 0;
}
