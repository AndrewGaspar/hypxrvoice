#include "Feedback.hpp"
#include "Log.hpp"

#include <cstdio>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {
    // Fire a notify-send toast without a shell (no injection from transcript text).
    void notify(const std::string& summary, const std::string& body) {
        const char* argv[] = {"notify-send", "-a", "hypxrvoice", "-t", "3000",
                              summary.c_str(), body.c_str(), nullptr};
        pid_t       pid    = -1;
        int         rc     = posix_spawnp(&pid, "notify-send", nullptr, nullptr,
                                          const_cast<char* const*>(argv), environ);
        if (rc != 0)
            return; // notify-send absent — silently skip
        // Reap without blocking the loop meaningfully.
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

namespace Feedback {
    void emitTranscript(const STranscript& t, const SConfig& cfg) {
        if (cfg.feedback.stdoutJson) {
            std::fprintf(stdout, "%s\n", t.toJson().c_str());
            std::fflush(stdout);
        }
        Log::log(Log::INFO, "transcript [{}] onset={}ms words={}: \"{}\"",
                 activationName(t.activation), t.onsetMs, t.words.size(), t.text);
        if (cfg.feedback.notify && !t.text.empty())
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

        // A human-facing toast: the phrasing + a low-confidence / clarify hint.
        if (cfg.feedback.notify) {
            std::string body;
            if (a.verb == EVerb::Clarify)
                body = a.clarifyQuestion.empty() ? "please clarify" : a.clarifyQuestion;
            else if (a.verb == EVerb::None)
                body = ""; // nothing spoken that was a command — stay silent
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
