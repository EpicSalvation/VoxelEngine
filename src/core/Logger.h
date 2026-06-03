#pragma once

#include <functional>

// Minimal engine logger — currently only the warn level is used.
// Tests can redirect output by calling Log::setWarnHandler before the code
// under test runs; the default handler writes to stderr.
namespace Log {

void warn(const char* msg);

// Replace the warn handler. Pass nullptr to restore the default (stderr).
void setWarnHandler(std::function<void(const char*)> fn);

}  // namespace Log
