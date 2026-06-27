// M17 profiler facility tests (include/core/Profiler.h).
//
// The profiler underpins the performance-pass benchmark. These tests pin its two
// load-bearing properties: it accumulates per-zone call counts / elapsed time
// while enabled, and a ProfileScope is INERT when the profiler is disabled — the
// default state, which must leave instrumented code byte-identical in behavior
// (no zone recorded, registry untouched).

#include "core/Profiler.h"

#include <gtest/gtest.h>

#include <thread>

namespace {

// Find a zone by name in a snapshot, or nullptr.
const ProfileZoneStat* find(const std::vector<ProfileZoneStat>& v,
                            const std::string& name) {
    for (const auto& z : v)
        if (z.name == name) return &z;
    return nullptr;
}

}  // namespace

TEST(ProfilerTest, DisabledScopeRecordsNothing) {
    profiler().reset();
    profiler().setEnabled(false);
    ASSERT_FALSE(profiler().enabled());

    for (int i = 0; i < 100; ++i) {
        VOXEL_PROFILE_SCOPE("disabled.zone");
    }

    // No zone exists: a disabled ProfileScope must not touch the registry.
    EXPECT_TRUE(profiler().snapshot().empty());
    EXPECT_EQ(find(profiler().snapshot(), "disabled.zone"), nullptr);
}

TEST(ProfilerTest, EnabledScopeAccumulatesCalls) {
    profiler().reset();
    profiler().setEnabled(true);

    const int kCalls = 50;
    for (int i = 0; i < kCalls; ++i) {
        VOXEL_PROFILE_SCOPE("counted.zone");
        // A little work so the recorded duration is non-degenerate.
        volatile int sink = 0;
        for (int j = 0; j < 1000; ++j) sink += j;
        (void)sink;
    }
    profiler().setEnabled(false);

    const auto snap = profiler().snapshot();
    const ProfileZoneStat* z = find(snap, "counted.zone");
    ASSERT_NE(z, nullptr);
    EXPECT_EQ(z->calls, static_cast<uint64_t>(kCalls));
    EXPECT_GE(z->maxNs, 0u);
    EXPECT_GE(z->totalNs, z->maxNs);  // total is the sum of all samples
}

TEST(ProfilerTest, ResetClearsZones) {
    profiler().reset();
    profiler().setEnabled(true);
    { VOXEL_PROFILE_SCOPE("ephemeral.zone"); }
    profiler().setEnabled(false);
    ASSERT_FALSE(profiler().snapshot().empty());

    profiler().reset();
    EXPECT_TRUE(profiler().snapshot().empty());
}

TEST(ProfilerTest, RecordIsThreadSafe) {
    // The decomposition worker threads may record concurrently; recording from
    // several threads must not corrupt the counters (run under TSan/ASan in CI).
    profiler().reset();
    profiler().setEnabled(true);

    const int kThreads = 4;
    const int kPer     = 1000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < kPer; ++i) profiler().record("mt.zone", 1);
        });
    for (auto& th : threads) th.join();
    profiler().setEnabled(false);

    const ProfileZoneStat* z = find(profiler().snapshot(), "mt.zone");
    ASSERT_NE(z, nullptr);
    EXPECT_EQ(z->calls, static_cast<uint64_t>(kThreads) * kPer);
    EXPECT_EQ(z->totalNs, static_cast<uint64_t>(kThreads) * kPer);  // 1 ns each

    profiler().reset();  // leave global state clean for other suites
}
