#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>

// Minimal stderr logger, shared with the hypxrpaper sibling project. hypxrvoice is
// a standalone daemon, so we log straight to stderr (line-flushed) rather than
// through any compositor logger. A systemd user unit captures this into the journal.
namespace Log {
    enum eLevel {
        TRACE,
        DEBUG,
        INFO,
        WARN,
        ERR,
    };

    // Messages below this level are dropped. Set from config / --verbose.
    void setLevel(eLevel level);
    bool enabled(eLevel level);

    void logString(eLevel level, const std::string& msg);

    template <typename... Args>
    void log(eLevel level, std::format_string<Args...> fmt, Args&&... args) {
        if (!enabled(level))
            return;
        logString(level, std::format(fmt, std::forward<Args>(args)...));
    }
}
