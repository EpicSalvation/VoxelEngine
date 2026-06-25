#include "Logger.h"

#include <cstdio>

namespace Log {

namespace {

Level g_minLevel = Level::Info;
Handler g_handler;

const char* levelTag(Level lvl) {
    switch (lvl) {
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Error: return "ERROR";
    }
    return "???";
}

void emit(Level lvl, const char* category, const char* text) {
    if (lvl < g_minLevel) return;
    Message msg{lvl, category ? category : "", text};
    if (g_handler) {
        g_handler(msg);
    } else {
        if (category && category[0] != '\0') {
            std::fprintf(stderr, "[%s] [%s] %s\n", levelTag(lvl), category, text);
        } else {
            std::fprintf(stderr, "[%s] %s\n", levelTag(lvl), text);
        }
    }
}

}  // namespace

void setMinLevel(Level lvl) { g_minLevel = lvl; }
Level minLevel() { return g_minLevel; }

void setHandler(Handler fn) { g_handler = std::move(fn); }

void debug(const char* msg) { emit(Level::Debug, nullptr, msg); }
void info(const char* msg)  { emit(Level::Info,  nullptr, msg); }
void warn(const char* msg)  { emit(Level::Warn,  nullptr, msg); }
void error(const char* msg) { emit(Level::Error, nullptr, msg); }

void debug(const char* category, const char* msg) { emit(Level::Debug, category, msg); }
void info(const char* category, const char* msg)  { emit(Level::Info,  category, msg); }
void warn(const char* category, const char* msg)  { emit(Level::Warn,  category, msg); }
void error(const char* category, const char* msg) { emit(Level::Error, category, msg); }

void setWarnHandler(std::function<void(const char*)> fn) {
    if (fn) {
        setHandler([f = std::move(fn)](const Message& m) { f(m.text); });
    } else {
        setHandler(nullptr);
    }
}

}  // namespace Log
