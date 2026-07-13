#include "Tts.hpp"
#include "HudModel.hpp"
#include "Log.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace fs = std::filesystem;

namespace {
    // Condense a refusal reason to a short spoken clause. The executor reasons are
    // already short; we just drop a trailing period and keep it lowercase-ish.
    std::string terseReason(const std::string& reason) {
        std::string r = reason;
        while (!r.empty() && (r.back() == '.' || r.back() == ' '))
            r.pop_back();
        return r;
    }

    // Resolve `espeak-ng` on PATH once. Returns "" if not found.
    const std::string& espeakPath() {
        static const std::string cached = [] {
            const char* pathEnv = std::getenv("PATH");
            if (!pathEnv)
                return std::string();
            std::string        path = pathEnv;
            size_t             start = 0;
            std::error_code    ec;
            while (start <= path.size()) {
                size_t      end = path.find(':', start);
                std::string dir = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (!dir.empty()) {
                    fs::path cand = fs::path(dir) / "espeak-ng";
                    if (fs::exists(cand, ec) && ::access(cand.c_str(), X_OK) == 0)
                        return cand.string();
                }
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
            return std::string();
        }();
        return cached;
    }
}

namespace Tts {
    std::string phraseFor(const SAction& a, const SExecPlan& plan, const SConfig& cfg) {
        const std::string& mode = cfg.feedback.ttsMode;
        if (mode == "off")
            return "";
        if (a.verb == EVerb::None)
            return "";

        // Clarify: always spoken (both errors + all) — it demands a response.
        if (a.verb == EVerb::Clarify)
            return a.clarifyQuestion.empty() ? "which one?" : a.clarifyQuestion;

        // Refusal: spoken in errors + all.
        if (!plan.ok)
            return "can't, " + (plan.reason.empty() ? std::string("not possible") : terseReason(plan.reason));

        // Success confirmation: only in "all".
        if (mode == "all")
            return hudActionPhrase(a);
        return "";
    }

    bool available() {
        return !espeakPath().empty();
    }

    void speak(const std::string& text, const SConfig& cfg) {
        if (text.empty() || cfg.feedback.ttsMode == "off")
            return;
        const std::string& exe = espeakPath();
        if (exe.empty())
            return; // cleanly disabled — no espeak-ng.

        const std::string rate = std::to_string(cfg.feedback.ttsRate);
        std::vector<const char*> argv;
        argv.push_back("espeak-ng");
        argv.push_back("-s");
        argv.push_back(rate.c_str());
        if (!cfg.feedback.ttsVoice.empty()) {
            argv.push_back("-v");
            argv.push_back(cfg.feedback.ttsVoice.c_str());
        }
        argv.push_back("--"); // end of options; text is a positional arg (no shell).
        argv.push_back(text.c_str());
        argv.push_back(nullptr);

        // espeak-ng blocks for the duration of playback (seconds). Double-fork so the
        // speaking process reparents to init and is auto-reaped — the daemon loop is
        // never blocked and no zombie accumulates. The short-lived middle child is
        // reaped here immediately.
        pid_t mid = fork();
        if (mid < 0) {
            Log::log(Log::DEBUG, "tts: fork failed");
            return;
        }
        if (mid == 0) {
            setsid();
            pid_t gc = fork();
            if (gc == 0) {
                execv(exe.c_str(), const_cast<char* const*>(argv.data()));
                _exit(127); // exec failed
            }
            _exit(0); // middle child exits; grandchild reparents to init.
        }
        int status = 0;
        waitpid(mid, &status, 0); // fast: middle child exits immediately.
    }
}
