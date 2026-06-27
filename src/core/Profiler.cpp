#include "core/Profiler.h"

#include <algorithm>
#include <cstdio>

Profiler& Profiler::instance() {
    static Profiler p;
    return p;
}

void Profiler::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    zones_.clear();
}

void Profiler::record(const std::string& name, uint64_t ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    ProfileZoneStat& z = zones_[name];
    if (z.name.empty()) z.name = name;
    ++z.calls;
    z.totalNs += ns;
    if (ns > z.maxNs) z.maxNs = ns;
}

std::vector<ProfileZoneStat> Profiler::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ProfileZoneStat> out;
    out.reserve(zones_.size());
    for (const auto& kv : zones_) out.push_back(kv.second);
    std::sort(out.begin(), out.end(),
              [](const ProfileZoneStat& a, const ProfileZoneStat& b) {
                  return a.totalNs > b.totalNs;
              });
    return out;
}

std::string Profiler::report() const {
    const std::vector<ProfileZoneStat> zones = snapshot();
    std::string out;
    char line[256];
    std::snprintf(line, sizeof(line), "%-28s %10s %12s %12s %12s\n",
                  "zone", "calls", "total ms", "avg us", "max us");
    out += line;
    out += std::string(76, '-') + "\n";
    for (const ProfileZoneStat& z : zones) {
        const double totalMs = static_cast<double>(z.totalNs) / 1.0e6;
        const double avgUs    = z.calls
            ? static_cast<double>(z.totalNs) / static_cast<double>(z.calls) / 1.0e3
            : 0.0;
        const double maxUs    = static_cast<double>(z.maxNs) / 1.0e3;
        std::snprintf(line, sizeof(line), "%-28s %10llu %12.3f %12.3f %12.3f\n",
                      z.name.c_str(),
                      static_cast<unsigned long long>(z.calls),
                      totalMs, avgUs, maxUs);
        out += line;
    }
    return out;
}
