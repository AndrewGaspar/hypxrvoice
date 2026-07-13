#include "Log.hpp"

#include <atomic>
#include <cstdio>
#include <ctime>

namespace Log {
    static std::atomic<eLevel> g_min{INFO};

    void setLevel(eLevel level) {
        g_min.store(level);
    }

    bool enabled(eLevel level) {
        return level >= g_min.load();
    }

    static const char* levelName(eLevel l) {
        switch (l) {
            case TRACE: return "TRACE";
            case DEBUG: return "DEBUG";
            case INFO: return "INFO";
            case WARN: return "WARN";
            case ERR: return "ERROR";
        }
        return "?";
    }

    void logString(eLevel level, const std::string& msg) {
        char        tbuf[16] = {};
        std::time_t t        = std::time(nullptr);
        std::tm     tm{};
        localtime_r(&t, &tm);
        std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm);
        std::fprintf(stderr, "[%s] [%s] %s\n", tbuf, levelName(level), msg.c_str());
        std::fflush(stderr);
    }
}
