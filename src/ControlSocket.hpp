#pragma once

#include <functional>
#include <string>

// AF_UNIX control socket shared by the daemon (server) and hypxrvoicectl (client).
// Line protocol: the client writes one command line and reads a single response
// line, then the connection closes. Commands: `ptt start|stop|toggle`, `status`,
// `reload`. Path: $XDG_RUNTIME_DIR/hypxrvoice/control.sock.
namespace Control {
    std::string socketPath();

    // Client: connect, send `cmd`, read the response into `response`. Returns false
    // (with `err` set) if the daemon is not reachable.
    bool sendCommand(const std::string& cmd, std::string& response, std::string& err);
}

// Server side, owned by the daemon. Non-blocking listen fd integrated into the
// daemon's epoll loop; each accepted connection is serviced synchronously (tiny
// messages) via the handler.
class CControlServer {
  public:
    CControlServer() = default;
    ~CControlServer();

    // Handler maps a command line to a response line.
    using Handler = std::function<std::string(const std::string& cmd)>;

    bool start(Handler handler, std::string& err);
    void stop();
    int  fd() const { return m_listenFd; }

    // Call when epoll reports the listen fd readable: accept + service one client.
    void serviceOnce();

  private:
    int         m_listenFd = -1;
    std::string m_path;
    Handler     m_handler;
};
