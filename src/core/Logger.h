#pragma once

#include <functional>
#include <string>

// Leveled, structured engine logger.
//
// Levels: Debug < Info < Warn < Error.
// Each message carries an optional category tag (e.g. "Net", "Physics").
// The minimum level defaults to Info; set it with Log::setMinLevel().
//
// Tests can redirect output by installing a custom handler via
// Log::setHandler(); the default writes to stderr.

namespace Log {

enum class Level { Debug, Info, Warn, Error };

struct Message {
    Level level;
    const char* category;  // nullable — empty string when untagged
    const char* text;
};

using Handler = std::function<void(const Message&)>;

void setMinLevel(Level lvl);
Level minLevel();

// Replace the output handler. Pass nullptr to restore the default (stderr).
void setHandler(Handler fn);

// --- Convenience free functions ---

void debug(const char* msg);
void info(const char* msg);
void warn(const char* msg);
void error(const char* msg);

void debug(const char* category, const char* msg);
void info(const char* category, const char* msg);
void warn(const char* category, const char* msg);
void error(const char* category, const char* msg);

// Legacy alias — redirects to setHandler wrapping the old signature.
void setWarnHandler(std::function<void(const char*)> fn);

}  // namespace Log
