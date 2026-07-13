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
}
