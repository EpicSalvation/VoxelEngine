#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// Lightweight, always-compiled CPU zone profiler (M17 performance pass).
//
// Design goal: instrument the engine's hot paths (decomposition, chunk
// management, meshing) without affecting normal builds. The profiler is
// DISABLED by default; a ProfileScope reads enablement once at construction and,
// when off, never touches the clock or the registry — so the only cost in an
// un-profiled run is a single relaxed atomic load plus a predictable branch, and
// the observable behavior of instrumented code is byte-for-byte unchanged.
//
// A benchmark or developer harness flips it on explicitly:
//     profiler().reset();
//     profiler().setEnabled(true);
//     ... run workload ...
//     profiler().setEnabled(false);
//     std::puts(profiler().report().c_str());
//
// It is a process-global singleton (like engineConfig()): zones accumulate
// across whatever code runs while enabled. Thread-safe — the worker threads that
// run decomposition jobs may record concurrently — but the lock is taken only
// while enabled, so the default path never contends.

struct ProfileZoneStat {
    std::string name;
    uint64_t    calls   = 0;
    uint64_t    totalNs = 0;
    uint64_t    maxNs   = 0;
};

class Profiler {
public:
    static Profiler& instance();

    void setEnabled(bool on) { enabled_.store(on, std::memory_order_relaxed); }
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

    // Clear all accumulated zone statistics.
    void reset();

    // Record one timed sample for `name`. Called by ProfileScope's destructor;
    // safe to call directly for manual timing.
    void record(const std::string& name, uint64_t ns);

    // Snapshot of every zone, sorted by total elapsed time descending.
    std::vector<ProfileZoneStat> snapshot() const;

    // Human-readable table (zone | calls | total ms | avg us | max us), sorted by
    // total time descending. Empty body when nothing was recorded.
    std::string report() const;

private:
    Profiler() = default;
    std::atomic<bool>                      enabled_{false};
    mutable std::mutex                     mutex_;
    std::map<std::string, ProfileZoneStat> zones_;
};

// Process-global accessor, mirroring engineConfig().
inline Profiler& profiler() { return Profiler::instance(); }

// RAII scope timer. Construction snapshots enablement: when disabled the object
// is inert (no clock read, no registry access), so instrumented code pays only a
// branch on the common (un-profiled) path.
class ProfileScope {
public:
    explicit ProfileScope(const char* name)
        : name_(name), active_(Profiler::instance().enabled()) {
        if (active_) start_ = std::chrono::steady_clock::now();
    }
    ~ProfileScope() {
        if (!active_) return;
        const auto end = std::chrono::steady_clock::now();
        const auto ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             end - start_).count();
        Profiler::instance().record(name_, static_cast<uint64_t>(ns));
    }
    ProfileScope(const ProfileScope&)            = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
    const char*                           name_;
    bool                                  active_;
    std::chrono::steady_clock::time_point start_;
};

#define VOXEL_PROFILE_CONCAT_(a, b) a##b
#define VOXEL_PROFILE_CONCAT(a, b)  VOXEL_PROFILE_CONCAT_(a, b)

// Time the enclosing scope under `name` (a string literal). No-op cost when the
// profiler is disabled (the default).
#define VOXEL_PROFILE_SCOPE(name) \
    ProfileScope VOXEL_PROFILE_CONCAT(voxel_prof_scope_, __LINE__)(name)
