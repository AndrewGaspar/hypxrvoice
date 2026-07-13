#include "HudOverlay.hpp"
#include "HudMessage.hpp"
#include "Log.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace fs = std::filesystem;

// Exit code the subprocess uses to report "no usable XR runtime / init failed" so the
// daemon can degrade quietly rather than treat it as a crash. Kept in sync with
// hud/hud_main.cpp.
static constexpr int kHudExitNoRuntime = 3;

namespace {
    fs::path exeDir() {
        char    buf[4096];
        ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0)
            return fs::current_path();
        buf[n] = '\0';
        return fs::path(buf).parent_path();
    }

    // Find the hypxrvoice-hud binary: next to us first (build tree / install), then PATH.
    std::string locateHud() {
        std::error_code ec;
        fs::path        sib = exeDir() / "hypxrvoice-hud";
        if (fs::exists(sib, ec) && ::access(sib.c_str(), X_OK) == 0)
            return sib.string();
        if (const char* p = std::getenv("PATH")) {
            std::string path = p;
            size_t      start = 0;
            while (start <= path.size()) {
                size_t      end = path.find(':', start);
                std::string dir = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (!dir.empty()) {
                    fs::path cand = fs::path(dir) / "hypxrvoice-hud";
                    if (fs::exists(cand, ec) && ::access(cand.c_str(), X_OK) == 0)
                        return cand.string();
                }
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
        }
        return "";
    }
}

CHudOverlay::~CHudOverlay() {
    stop();
}

void CHudOverlay::configure(const SConfig& cfg) {
    m_cfg = cfg;
}

void CHudOverlay::degrade(const std::string& why) {
    m_degraded = true;
    if (m_writeFd >= 0) {
        close(m_writeFd);
        m_writeFd = -1;
    }
    if (!m_notedDegrade) {
        Log::log(Log::WARN, "HUD overlay unavailable ({}); using notifications only", why);
        m_notedDegrade = true;
    }
}

bool CHudOverlay::ensureSpawned() {
    if (m_spawned)
        return m_writeFd >= 0;
    m_spawned = true;

    std::string exe = locateHud();
    if (exe.empty()) {
        degrade("hypxrvoice-hud binary not found");
        return false;
    }

    int fds[2];
    if (pipe(fds) != 0) {
        degrade("pipe() failed");
        return false;
    }

    // Build argv from the feedback config; content arrives on stdin.
    std::string sizeS = std::to_string(m_cfg.feedback.hudSize);
    std::string opac  = std::to_string(m_cfg.feedback.hudOpacity);
    std::string zS    = std::to_string(m_cfg.feedback.hudZ);
    std::vector<const char*> argv = {exe.c_str(),
                                     "--pose", m_cfg.feedback.hudPose.c_str(),
                                     "--size", sizeS.c_str(),
                                     "--opacity", opac.c_str(),
                                     "--z", zS.c_str()};
    if (!m_cfg.feedback.hudGpu.empty()) {
        argv.push_back("--gpu");
        argv.push_back(m_cfg.feedback.hudGpu.c_str());
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[0], STDIN_FILENO); // child stdin = pipe read
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);

    pid_t pid = -1;
    int   rc  = posix_spawn(&pid, exe.c_str(), &fa, nullptr,
                            const_cast<char* const*>(argv.data()), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[0]); // parent doesn't read.

    if (rc != 0) {
        close(fds[1]);
        degrade("spawn failed");
        return false;
    }
    m_childPid = pid;
    m_writeFd  = fds[1];
    Log::log(Log::INFO, "HUD overlay subprocess started (pid {}, {})", pid, exe);
    return true;
}

bool CHudOverlay::send(const SHudView& v) {
    if (!m_cfg.feedback.hud || m_degraded)
        return false;
    if (!ensureSpawned())
        return false;

    std::string line = HudMsg::serialize(v);
    size_t      off  = 0;
    while (off < line.size()) {
        ssize_t w = write(m_writeFd, line.data() + off, line.size() - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            degrade("subprocess pipe closed"); // EPIPE etc.
            return false;
        }
        off += static_cast<size_t>(w);
    }
    return true;
}

void CHudOverlay::poll() {
    if (m_childPid < 0)
        return;
    int   status = 0;
    pid_t r      = waitpid(static_cast<pid_t>(m_childPid), &status, WNOHANG);
    if (r == static_cast<pid_t>(m_childPid)) {
        m_childPid = -1;
        if (WIFEXITED(status) && WEXITSTATUS(status) == kHudExitNoRuntime)
            degrade("no XR runtime");
        else if (WIFEXITED(status))
            degrade("subprocess exited (code " + std::to_string(WEXITSTATUS(status)) + ")");
        else
            degrade("subprocess terminated");
    }
}

void CHudOverlay::stop() {
    if (m_writeFd >= 0) {
        close(m_writeFd); // EOF -> subprocess exits cleanly.
        m_writeFd = -1;
    }
    if (m_childPid >= 0) {
        int status = 0;
        // Give it a brief chance; then it will be reaped on next daemon cycle anyway.
        waitpid(static_cast<pid_t>(m_childPid), &status, WNOHANG);
        m_childPid = -1;
    }
}
