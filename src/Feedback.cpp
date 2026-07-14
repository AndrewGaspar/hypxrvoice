#include "Feedback.hpp"
#include "HudClient.hpp"
#include "HudModel.hpp"
#include "Log.hpp"
#include "Tts.hpp"

#include <cstdio>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {
    // Fire a notify-send toast without a shell (no injection from transcript text).
    void notifySend(const std::string& summary, const std::string& body) {
        const char* argv[] = {"notify-send", "-a", "hypxrvoice", "-t", "3000",
                              summary.c_str(), body.c_str(), nullptr};
        pid_t       pid    = -1;
        int         rc     = posix_spawnp(&pid, "notify-send", nullptr, nullptr,
                                          const_cast<char* const*>(argv), environ);
        if (rc != 0)
            return; // notify-send absent — silently skip
        int status = 0;
        waitpid(pid, &status, 0);
    }

    // The notify-send sink, redirectable for tests (see _setNotifySinkForTest).
    std::function<void(const std::string&, const std::string&)> g_notifySink = notifySend;
    void notify(const std::string& summary, const std::string& body) {
        if (g_notifySink)
            g_notifySink(summary, body);
    }

    // Daemon-only feedback runtime. Activated by startRuntime(); the emit functions
    // consult g_rt.active so oneshot/tests (which never start it) keep the pure
    // stdout/log/notify behaviour and never touch the HUD daemon / D-Bus.
    struct SRuntime {
        bool       active = false;
        CHudClient hud;
    };
    SRuntime g_rt;
}

namespace Feedback {
    void _setNotifySinkForTest(std::function<void(const std::string&, const std::string&)> sink) {
        g_notifySink = sink ? std::move(sink) : notifySend;
    }

    void startRuntime(const SConfig& cfg) {
        g_rt.active = true;
        g_rt.hud.configure(cfg);
        if (cfg.feedback.ttsMode != "off")
            Log::log(Log::INFO, "TTS mode '{}' ({})", cfg.feedback.ttsMode,
                     Tts::available() ? "espeak-ng found" : "espeak-ng NOT on PATH — TTS disabled");
        if (cfg.feedback.hud)
            Log::log(Log::INFO, "HUD enabled — pushing panels to hypxrhud (slot '{}')", cfg.feedback.hudSlot);
    }

    void stopRuntime() {
        g_rt.hud.stop();
        g_rt.active = false;
    }

    void pollRuntime() {
        if (g_rt.active)
            g_rt.hud.poll();
    }

    void onListeningStart(const SConfig& cfg) {
        if (g_rt.active && cfg.feedback.hud)
            g_rt.hud.show(hudForListening("", cfg));
    }

    void onListeningStop(const SConfig& cfg) {
        if (g_rt.active && cfg.feedback.hud)
            g_rt.hud.hide();
    }

    void emitTranscript(const STranscript& t, const SConfig& cfg) {
        if (cfg.feedback.stdoutJson) {
            std::fprintf(stdout, "%s\n", t.toJson().c_str());
            std::fflush(stdout);
        }
        Log::log(Log::INFO, "transcript [{}] onset={}ms words={}: \"{}\"",
                 activationName(t.activation), t.onsetMs, t.words.size(), t.text);

        // While listening, echo the partial transcript on the HUD (the veto preview).
        // show() returns true only when it reached a LIVE hypxrhud (daemon up + runtime
        // "live"), so the notify-send fallback below stays quiet in that case.
        bool hudCarrying = false;
        if (g_rt.active && cfg.feedback.hud && !t.text.empty())
            hudCarrying = g_rt.hud.show(hudForListening(t.text, cfg));

        // notify-send is the HUD-less fallback: only toast the transcript when the HUD
        // is not carrying it (keeps a headset user's desktop quiet).
        if (cfg.feedback.notify && !hudCarrying && !t.text.empty())
            notify("hypxrvoice", t.text);
    }

    void emitAction(const SAction& a, const SExecPlan& plan, const SConfig& cfg) {
        // Single machine-readable line: {"kind":"action","action":{…},"plan":{…}}.
        if (cfg.feedback.stdoutJson) {
            std::fprintf(stdout, "{\"kind\":\"action\",\"action\":%s,\"plan\":%s}\n",
                         a.toJson().c_str(), plan.toJson().c_str());
            std::fflush(stdout);
        }
        Log::log(Log::INFO, "intent [{}] target={} src={} conf={:.2f} steps={} {}",
                 verbName(a.verb), a.target.empty() ? "-" : a.target,
                 targetSourceName(a.targetSource), a.confidence, plan.steps.size(),
                 plan.approximated ? "(approx)" : "");

        // Nothing recognised as a command: keep quiet (and clear a listening HUD).
        if (a.verb == EVerb::None) {
            if (g_rt.active && cfg.feedback.hud)
                onListeningStop(cfg);
            return;
        }

        // In-headset HUD: the primary channel. Build the view and push it to hypxrhud.
        // show() returns true only when it reached a live daemon (else notify-send below).
        bool hudShown = false;
        if (g_rt.active && cfg.feedback.hud) {
            SHudView v = hudForAction(a, plan, cfg);
            hudShown   = g_rt.hud.show(v);
        }

        // Terse TTS: confirms what the HUD can't (errors, clarify; success only in
        // "all" mode). Independent of the HUD channel.
        if (g_rt.active) {
            std::string phrase = Tts::phraseFor(a, plan, cfg);
            if (!phrase.empty())
                Tts::speak(phrase, cfg);
        }

        // notify-send fallback: only when the HUD didn't carry it (out of headset, no
        // XR runtime, or HUD disabled) — preserves the V4 toast behaviour there.
        if (cfg.feedback.notify && !hudShown) {
            std::string body;
            if (a.verb == EVerb::Clarify)
                body = a.clarifyQuestion.empty() ? "please clarify" : a.clarifyQuestion;
            else {
                body = std::string(verbName(a.verb));
                if (!a.target.empty() && a.target != "active")
                    body += " " + a.target;
                if (a.confidence < 0.6)
                    body += " (unsure)";
                if (!plan.ok && !plan.reason.empty())
                    body += " — " + plan.reason;
            }
            if (!body.empty())
                notify("hypxrvoice", body);
        }
    }
}
