#include "ControlSocket.hpp"
#include "Log.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
    bool setSunPath(sockaddr_un& addr, const std::string& path, std::string& err) {
        if (path.size() + 1 > sizeof(addr.sun_path)) {
            err = "control socket path too long: " + path;
            return false;
        }
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        return true;
    }
}

std::string Control::socketPath() {
    // Full-path override wins (both daemon + ctl honour it), so a test instance can
    // avoid colliding with a live daemon's socket. Then a runtime-dir override, then
    // the standard XDG_RUNTIME_DIR, then /tmp.
    if (const char* sock = std::getenv("HYPXRVOICE_CONTROL_SOCK"); sock && *sock)
        return sock;
    std::string dir;
    if (const char* rt = std::getenv("HYPXRVOICE_RUNTIME_DIR"); rt && *rt)
        dir = rt;
    else if (const char* xdg = std::getenv("XDG_RUNTIME_DIR"); xdg && *xdg)
        dir = xdg;
    else
        dir = "/tmp";
    return dir + "/hypxrvoice/control.sock";
}

bool Control::sendCommand(const std::string& cmd, std::string& response, std::string& err) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        err = std::string("socket(): ") + std::strerror(errno);
        return false;
    }
    sockaddr_un addr{};
    std::string path = socketPath();
    if (!setSunPath(addr, path, err)) {
        close(fd);
        return false;
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        err = "cannot reach hypxrvoiced (is it running?): " + std::string(std::strerror(errno));
        close(fd);
        return false;
    }
    std::string line = cmd;
    line += '\n';
    if (write(fd, line.data(), line.size()) < 0) {
        err = std::string("write(): ") + std::strerror(errno);
        close(fd);
        return false;
    }
    response.clear();
    char    buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        response.append(buf, n);
    close(fd);
    while (!response.empty() && (response.back() == '\n' || response.back() == '\r'))
        response.pop_back();
    return true;
}

CControlServer::~CControlServer() {
    stop();
}

bool CControlServer::start(Handler handler, std::string& err) {
    m_handler = std::move(handler);
    m_path    = Control::socketPath();

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(m_path).parent_path(), ec);

    // Remove a stale socket file from a previous run.
    ::unlink(m_path.c_str());

    m_listenFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_listenFd < 0) {
        err = std::string("socket(): ") + std::strerror(errno);
        return false;
    }
    sockaddr_un addr{};
    if (!setSunPath(addr, m_path, err)) {
        stop();
        return false;
    }
    if (bind(m_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        err = "bind('" + m_path + "'): " + std::strerror(errno);
        stop();
        return false;
    }
    if (listen(m_listenFd, 8) < 0) {
        err = std::string("listen(): ") + std::strerror(errno);
        stop();
        return false;
    }
    Log::log(Log::INFO, "control socket listening at {}", m_path);
    return true;
}

void CControlServer::serviceOnce() {
    int cfd = accept(m_listenFd, nullptr, nullptr);
    if (cfd < 0)
        return;
    std::string in;
    char        buf[1024];
    ssize_t     n;
    // Read until newline or EOF.
    while ((n = read(cfd, buf, sizeof(buf))) > 0) {
        in.append(buf, n);
        if (in.find('\n') != std::string::npos)
            break;
    }
    // Trim to first line.
    size_t nl = in.find('\n');
    if (nl != std::string::npos)
        in.resize(nl);
    while (!in.empty() && (in.back() == '\r' || in.back() == ' '))
        in.pop_back();

    std::string resp = m_handler ? m_handler(in) : "error: no handler";
    resp += '\n';
    (void)!write(cfd, resp.data(), resp.size());
    close(cfd);
}

void CControlServer::stop() {
    if (m_listenFd >= 0) {
        close(m_listenFd);
        m_listenFd = -1;
    }
    if (!m_path.empty()) {
        ::unlink(m_path.c_str());
        m_path.clear();
    }
}
