#include "ControlSocket.hpp"

#include <cstdio>
#include <string>

static void usage(const char* a0) {
    std::fprintf(stderr,
                 "hypxrvoicectl — control the hypxrvoiced daemon\n"
                 "\n"
                 "Usage: %s <command>\n"
                 "\n"
                 "  ptt start        Open the mic (push-to-talk press).\n"
                 "  ptt stop         Close the mic and transcribe (push-to-talk release).\n"
                 "  ptt toggle       Toggle the push-to-talk window.\n"
                 "  status           Print daemon status as JSON.\n"
                 "  reload           Re-read the config file.\n"
                 "\n"
                 "Bind these to Hyprland keys, e.g.:\n"
                 "  bind = SUPER, V, exec, hypxrvoicectl ptt toggle\n",
                 a0);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    std::string cmd;
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "-h" || std::string(argv[i]) == "--help") {
            usage(argv[0]);
            return 0;
        }
        if (!cmd.empty())
            cmd += ' ';
        cmd += argv[i];
    }

    std::string response, err;
    if (!Control::sendCommand(cmd, response, err)) {
        std::fprintf(stderr, "hypxrvoicectl: %s\n", err.c_str());
        return 1;
    }
    std::printf("%s\n", response.c_str());
    // Non-zero exit if the daemon reported an error.
    return response.rfind("error", 0) == 0 ? 1 : 0;
}
