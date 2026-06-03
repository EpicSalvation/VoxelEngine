#include "Logger.h"

#include <cstdio>
#include <functional>

namespace Log {

namespace {
std::function<void(const char*)> g_warnHandler;
}

void warn(const char* msg) {
    if (g_warnHandler) {
        g_warnHandler(msg);
    } else {
        std::fprintf(stderr, "[WARN] %s\n", msg);
    }
}

void setWarnHandler(std::function<void(const char*)> fn) {
    g_warnHandler = std::move(fn);
}

}  // namespace Log
