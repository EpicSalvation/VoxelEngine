// Decomposition determinism (M6).
//
// The DecompositionWorker generates a composite macro voxel's child grid on a
// thread pool. Decomposition must be deterministic (ARCHITECTURE §4): the same
// (generator, coords) must yield a byte-for-byte identical child grid every run,
// regardless of which worker thread produced it or how many ran concurrently.
// These tests pin that property down.

#include "world/DecompositionWorker.h"
#include "world/MacroVoxel.h"
#include "world/Chunk.h"
#include "world/Voxel.h"
#include "world/ChunkCoordMath.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

namespace {

// A deterministic generator: a pure function of world position only. No rand, no
// time, no shared state — exactly the contract a decomposition generator must
// honour. Fills a sparse, position-dependent pattern so distinct chunks differ.
void deterministicGen(WorldCoord origin, int n, Voxel* out, void*) {
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const int64_t wx = static_cast<int64_t>(origin.value.x) + x;
                const int64_t wy = static_cast<int64_t>(origin.value.y) + y;
                const int64_t wz = static_cast<int64_t>(origin.value.z) + z;
                uint64_t h = static_cast<uint64_t>(wx) * 0x9E3779B97F4A7C15ull;
                h = (h + static_cast<uint64_t>(wy)) * 0x9E3779B97F4A7C15ull;
                h = (h + static_cast<uint64_t>(wz)) * 0x9E3779B97F4A7C15ull;
                h ^= h >> 29;
                Voxel v = Voxel::empty();
                if (h & 1ull) {
                    v.material.palette_index = static_cast<uint8_t>((h >> 1) % 15 + 1);
                    v.material.density       = static_cast<float>((h >> 8) % 1000);
                    v.material.hardness      = static_cast<float>((h >> 16) % 50);
                }
                out[x + n * (y + n * z)] = v;
            }
}

bool sameVoxel(const Voxel& a, const Voxel& b) {
    const MaterialProperties& x = a.material;
    const MaterialProperties& y = b.material;
    return x.density == y.density &&
           x.structural_strength == y.structural_strength &&
           x.thermal_conductivity == y.thermal_conductivity &&
           x.porosity == y.porosity &&
           x.hardness == y.hardness &&
           x.palette_index == y.palette_index;
}

bool sameGrid(const Chunk& a, const Chunk& b) {
    if (a.size() != b.size()) return false;
    const int n = a.size();
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                if (!sameVoxel(a.at(x, y, z), b.at(x, y, z))) return false;
    return true;
}

// Pull n results off the worker, yielding until they arrive (with a safety
// deadline so a bug fails the test instead of hanging it).
std::vector<DecompositionResult> collect(DecompositionWorker& w, size_t n) {
    std::vector<DecompositionResult> all;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (all.size() < n) {
        for (auto& r : w.drain()) all.push_back(std::move(r));
        if (all.size() < n) {
            std::this_thread::yield();
            if (std::chrono::steady_clock::now() > deadline) break;
        }
    }
    return all;
}

constexpr int kChunkSize = 8;
constexpr double kVoxelSize = 1.0;

}  // namespace

TEST(Decomposition, GenerateChunkIsDeterministicAcrossCalls) {
    for (ChunkCoord c : {ChunkCoord{0, 0, 0}, ChunkCoord{3, -2, 7}, ChunkCoord{-5, 1, -9}}) {
        auto a = DecompositionWorker::generateChunk(c, kChunkSize, kVoxelSize,
                                                    deterministicGen, nullptr);
        auto b = DecompositionWorker::generateChunk(c, kChunkSize, kVoxelSize,
                                                    deterministicGen, nullptr);
        ASSERT_TRUE(a && b);
        EXPECT_TRUE(sameGrid(*a, *b));
    }
}

TEST(Decomposition, ConcurrentJobsMatchSingleThreadedReference) {
    constexpr int kJobs = 48;

    // Single-threaded reference: for macro i, three child chunks.
    auto childChunksFor = [](int i) {
        return std::vector<ChunkCoord>{
            ChunkCoord{i, 0, 0}, ChunkCoord{i, 1, 0}, ChunkCoord{i, 0, 1}};
    };

    std::vector<std::vector<std::unique_ptr<Chunk>>> reference(kJobs);
    for (int i = 0; i < kJobs; ++i)
        for (ChunkCoord c : childChunksFor(i))
            reference[i].push_back(DecompositionWorker::generateChunk(
                c, kChunkSize, kVoxelSize, deterministicGen, nullptr));

    DecompositionWorker worker;  // hardware-sized pool
    EXPECT_GE(worker.threadCount(), 1u);

    for (int i = 0; i < kJobs; ++i) {
        DecompositionJob job;
        job.macro           = chunkmath::VoxelCoord{i, 0, 0};
        job.childChunks     = childChunksFor(i);
        job.childChunkSize  = kChunkSize;
        job.childVoxelSizeM = kVoxelSize;
        job.generator       = deterministicGen;
        worker.enqueue(job);
    }

    std::vector<DecompositionResult> results = collect(worker, kJobs);
    ASSERT_EQ(results.size(), static_cast<size_t>(kJobs));

    for (const DecompositionResult& r : results) {
        const int i = static_cast<int>(r.macro.x);
        ASSERT_GE(i, 0);
        ASSERT_LT(i, kJobs);
        ASSERT_EQ(r.chunks.size(), reference[i].size());
        for (size_t k = 0; k < r.chunks.size(); ++k)
            EXPECT_TRUE(sameGrid(*r.chunks[k], *reference[i][k]))
                << "macro " << i << " child chunk " << k;
    }
}

TEST(Decomposition, RepeatedIdenticalJobIsByteIdentical) {
    const std::vector<ChunkCoord> childChunks{ChunkCoord{2, 0, -1}, ChunkCoord{2, 1, -1}};
    std::vector<std::unique_ptr<Chunk>> reference;
    for (ChunkCoord c : childChunks)
        reference.push_back(DecompositionWorker::generateChunk(
            c, kChunkSize, kVoxelSize, deterministicGen, nullptr));

    constexpr int kRepeats = 64;
    DecompositionWorker worker;
    for (int i = 0; i < kRepeats; ++i) {
        DecompositionJob job;
        job.macro           = chunkmath::VoxelCoord{2, 0, -1};
        job.childChunks     = childChunks;
        job.childChunkSize  = kChunkSize;
        job.childVoxelSizeM = kVoxelSize;
        job.generator       = deterministicGen;
        worker.enqueue(job);
    }

    std::vector<DecompositionResult> results = collect(worker, kRepeats);
    ASSERT_EQ(results.size(), static_cast<size_t>(kRepeats));
    for (const DecompositionResult& r : results) {
        ASSERT_EQ(r.chunks.size(), reference.size());
        for (size_t k = 0; k < r.chunks.size(); ++k)
            EXPECT_TRUE(sameGrid(*r.chunks[k], *reference[k]));
    }
}

TEST(Decomposition, StateTracksPendingAndDecomposed) {
    DecompositionState state;
    const chunkmath::VoxelCoord m{4, 0, -2};

    EXPECT_TRUE(state.needsDecompose(m));
    EXPECT_TRUE(state.markPending(m));    // first claim succeeds
    EXPECT_FALSE(state.markPending(m));   // already pending — no double-enqueue
    EXPECT_FALSE(state.needsDecompose(m));
    EXPECT_TRUE(state.isPending(m));

    state.markDecomposed(m);
    EXPECT_FALSE(state.isPending(m));
    EXPECT_TRUE(state.isDecomposed(m));
    EXPECT_FALSE(state.needsDecompose(m));
    EXPECT_FALSE(state.markPending(m));   // decomposed — still no re-enqueue

    state.clear(m);
    EXPECT_TRUE(state.needsDecompose(m)); // back to atomic
}
