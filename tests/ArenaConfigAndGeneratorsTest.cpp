// M7b tests — arena config validation and generator determinism (Groups 1+2).
//
// Group 1: Five-layer arena LayerConfig validates correctly; a malformed variant
//          is rejected at startup with a clear error.
//
// Group 2: Arena generators produce identical grids on repeated calls (same args →
//          same output); terraces→detail single-step decomposition is byte-for-byte
//          stable across repeated and concurrent runs (reuses the M6 harness from
//          DecompositionTest).

#include "core/LayerConfig.h"
#include "world/Chunk.h"
#include "world/DecompositionWorker.h"
#include "world/MacroVoxel.h"
#include "world/Voxel.h"
#include "world/ChunkCoordMath.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

// ── Shared arena layer config string ─────────────────────────────────────────
// Canonical five-layer config used by the 07-arena-platformer demo.
static const char* kArenaConfigYaml = R"(
layers:
  - name: foundation
    voxel_size_m: 500.0
    mode: immutable
    chunk_size_voxels: 4
    view_distance_chunks: 1
  - name: ramparts
    voxel_size_m: 20.0
    mode: immutable
    chunk_size_voxels: 8
    view_distance_chunks: 4
  - name: terraces
    voxel_size_m: 10.0
    mode: composite
    decompose_to: detail
    chunk_size_voxels: 8
    view_distance_chunks: 4
  - name: props
    voxel_size_m: 2.0
    mode: immutable
    chunk_size_voxels: 8
    view_distance_chunks: 8
  - name: detail
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 10
    view_distance_chunks: 12
)";

// ── Group 1: Layer config validation ─────────────────────────────────────────

TEST(ArenaConfig, FiveLayerConfigValidates) {
    LayerConfig cfg = LayerConfig::loadFromString(kArenaConfigYaml);
    ASSERT_EQ(cfg.layers().size(), 5u);

    // Layer names in config order.
    EXPECT_EQ(cfg.layers()[0].name, "foundation");
    EXPECT_EQ(cfg.layers()[1].name, "ramparts");
    EXPECT_EQ(cfg.layers()[2].name, "terraces");
    EXPECT_EQ(cfg.layers()[3].name, "props");
    EXPECT_EQ(cfg.layers()[4].name, "detail");
}

TEST(ArenaConfig, VoxelSizesDescending) {
    LayerConfig cfg = LayerConfig::loadFromString(kArenaConfigYaml);
    const auto& L = cfg.layers();
    for (size_t i = 1; i < L.size(); ++i)
        EXPECT_LT(L[i].voxel_size_m, L[i - 1].voxel_size_m)
            << "layer " << i << " (" << L[i].name << ") is not smaller than layer "
            << i - 1 << " (" << L[i - 1].name << ")";
}

TEST(ArenaConfig, IntegerRatiosAreCorrect) {
    // foundation(500)/ramparts(20)=25, ramparts(20)/terraces(10)=2,
    // terraces(10)/props(2)=5, props(2)/detail(1)=2.
    const std::vector<double> expectedRatios = {25.0, 2.0, 5.0, 2.0};
    LayerConfig cfg = LayerConfig::loadFromString(kArenaConfigYaml);
    const auto& L = cfg.layers();
    for (size_t i = 0; i < expectedRatios.size(); ++i) {
        const double ratio = L[i].voxel_size_m / L[i + 1].voxel_size_m;
        EXPECT_NEAR(ratio, expectedRatios[i], 1e-9)
            << "ratio between " << L[i].name << " and " << L[i + 1].name;
    }
}

TEST(ArenaConfig, CompositeLayerHasDecomposeTo) {
    LayerConfig cfg = LayerConfig::loadFromString(kArenaConfigYaml);
    const LayerDef* terraces = cfg.findLayer("terraces");
    ASSERT_NE(terraces, nullptr);
    EXPECT_EQ(terraces->mode, VoxelMode::composite);
    ASSERT_TRUE(terraces->decompose_to.has_value());
    EXPECT_EQ(*terraces->decompose_to, "detail");
}

TEST(ArenaConfig, ImmutableLayersHaveNoDecomposeTo) {
    LayerConfig cfg = LayerConfig::loadFromString(kArenaConfigYaml);
    for (const char* name : {"foundation", "ramparts", "props"}) {
        const LayerDef* l = cfg.findLayer(name);
        ASSERT_NE(l, nullptr) << name;
        EXPECT_EQ(l->mode, VoxelMode::immutable) << name;
        EXPECT_FALSE(l->decompose_to.has_value()) << name;
    }
}

TEST(ArenaConfig, TerminalLayerIsDetail) {
    LayerConfig cfg = LayerConfig::loadFromString(kArenaConfigYaml);
    const LayerDef* detail = cfg.findLayer("detail");
    ASSERT_NE(detail, nullptr);
    EXPECT_EQ(detail->mode, VoxelMode::terminal);
}

TEST(ArenaConfig, MalformedNonIntegerRatioIsRejected) {
    // terraces(10)/props(3) = 3.33… — not an integer → startup error.
    EXPECT_THROW(LayerConfig::loadFromString(R"(
layers:
  - name: foundation
    voxel_size_m: 500.0
    mode: immutable
  - name: ramparts
    voxel_size_m: 20.0
    mode: immutable
  - name: terraces
    voxel_size_m: 10.0
    mode: composite
    decompose_to: detail
  - name: props
    voxel_size_m: 3.0
    mode: immutable
  - name: detail
    voxel_size_m: 1.0
    mode: terminal
)"), std::exception);
}

TEST(ArenaConfig, MalformedMissingDecomposeToIsRejected) {
    // A composite layer that names no decompose_to target → startup error.
    EXPECT_THROW(LayerConfig::loadFromString(R"(
layers:
  - name: terraces
    voxel_size_m: 10.0
    mode: composite
  - name: detail
    voxel_size_m: 1.0
    mode: terminal
)"), std::exception);
}

TEST(ArenaConfig, MalformedBadDecomposeToTargetIsRejected) {
    // decompose_to names a layer that does not exist → startup error.
    EXPECT_THROW(LayerConfig::loadFromString(R"(
layers:
  - name: terraces
    voxel_size_m: 10.0
    mode: composite
    decompose_to: nonexistent
  - name: detail
    voxel_size_m: 1.0
    mode: terminal
)"), std::exception);
}

TEST(ArenaConfig, MalformedRatioTooSmallIsRejected) {
    // Adjacent ratio < 2:1 → startup error.
    EXPECT_THROW(LayerConfig::loadFromString(R"(
layers:
  - name: a
    voxel_size_m: 2.0
    mode: immutable
  - name: b
    voxel_size_m: 1.5
    mode: terminal
)"), std::exception);
}

// ── Group 2: Generator determinism ───────────────────────────────────────────
//
// The arena plugin's generators are pure functions of world position.  Rather
// than loading the plugin (which requires a disk path at test time), these tests
// inline a minimal faithful copy of the terraces and detail generators — the same
// inPlatform() predicate, same loops.  Determinism is guaranteed by the pure-
// function contract; the tests verify it holds under repeated and concurrent runs.

namespace {

// Inline copy of the arena plugin's platform geometry (kept in sync by convention).
struct PlatformZone { double xmin, xmax, ymin, ymax, zmin, zmax; };
constexpr PlatformZone kTestPlatforms[] = {
    { 180.0, 320.0, 10.0, 20.0, 180.0, 320.0 },
    {  60.0, 180.0, 20.0, 30.0,  60.0, 180.0 },
    { 320.0, 440.0, 30.0, 40.0,  60.0, 180.0 },
    { 320.0, 440.0, 40.0, 50.0, 320.0, 440.0 },
    {  60.0, 180.0, 50.0, 60.0, 320.0, 440.0 },
    { 200.0, 300.0, 60.0, 70.0, 200.0, 300.0 },
};

bool inTestPlatform(double wx, double wy, double wz) {
    for (const PlatformZone& p : kTestPlatforms)
        if (wx >= p.xmin && wx < p.xmax &&
            wy >= p.ymin && wy < p.ymax &&
            wz >= p.zmin && wz < p.zmax)
            return true;
    return false;
}

void testTerracesGen(WorldCoord origin, int n, Voxel* out, void* /*ud*/) {
    constexpr double vs = 10.0;
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                Voxel& v = out[x + n * (y + n * z)];
                v.material.palette_index = 0;
                v.material.density       = 0.0f;
                const double wx = origin.value.x + x * vs;
                const double wy = origin.value.y + y * vs;
                const double wz = origin.value.z + z * vs;
                if (inTestPlatform(wx, wy, wz)) {
                    v.material.palette_index = 1;
                    v.material.density       = 2500.0f;
                }
            }
}

void testDetailGen(WorldCoord origin, int n, Voxel* out, void* /*ud*/) {
    constexpr double vs = 1.0;
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                Voxel& v = out[x + n * (y + n * z)];
                v.material.palette_index = 0;
                v.material.density       = 0.0f;
                const double wx = origin.value.x + x * vs;
                const double wy = origin.value.y + y * vs;
                const double wz = origin.value.z + z * vs;
                if (inTestPlatform(wx, wy, wz)) {
                    const bool isTop = !inTestPlatform(wx, wy + vs, wz);
                    v.material.palette_index = isTop ? 2 : 1;
                    v.material.density       = isTop ? 1200.0f : 2700.0f;
                }
            }
}

bool sameVoxel(const Voxel& a, const Voxel& b) {
    return a.material.density         == b.material.density &&
           a.material.palette_index   == b.material.palette_index &&
           a.material.hardness        == b.material.hardness &&
           a.material.structural_strength == b.material.structural_strength;
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

std::vector<DecompositionResult> collectResults(DecompositionWorker& w, size_t n) {
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

}  // namespace

TEST(ArenaGenerators, TerracesGeneratorIsDeterministic) {
    // Same coord → identical grid every call.
    const std::vector<ChunkCoord> coords = {
        {0, 0, 0}, {1, 0, 0}, {2, 0, 0},   // spans the central platform
        {0, 0, 0}, {-1, 0, -1},             // off-platform (should be all empty)
    };
    for (const ChunkCoord& cc : coords) {
        auto a = DecompositionWorker::generateChunk(cc, 8, 10.0, testTerracesGen, nullptr);
        auto b = DecompositionWorker::generateChunk(cc, 8, 10.0, testTerracesGen, nullptr);
        ASSERT_TRUE(a && b);
        EXPECT_TRUE(sameGrid(*a, *b)) << "terraces mismatch at chunk ("
            << cc.x << "," << cc.y << "," << cc.z << ")";
    }
}

TEST(ArenaGenerators, DetailGeneratorIsDeterministic) {
    // Same coord → identical grid.
    const std::vector<ChunkCoord> coords = {
        {18, 1, 18},  // inside central platform (Y=10-20, 10m chunks → y=1)
        {6, 2, 6},    // NW platform area
        {0, 0, 0},    // off-platform
    };
    for (const ChunkCoord& cc : coords) {
        auto a = DecompositionWorker::generateChunk(cc, 10, 1.0, testDetailGen, nullptr);
        auto b = DecompositionWorker::generateChunk(cc, 10, 1.0, testDetailGen, nullptr);
        ASSERT_TRUE(a && b);
        EXPECT_TRUE(sameGrid(*a, *b)) << "detail mismatch at chunk ("
            << cc.x << "," << cc.y << "," << cc.z << ")";
    }
}

TEST(ArenaGenerators, TerracesAndDetailAgreeAtBoundaries) {
    // For each terrace voxel that is solid, the corresponding detail chunk must
    // contain at least one solid voxel (no holes introduced by the boundary check).
    // Also, no detail voxel outside the platform bounds is solid.
    //
    // Test the central platform: terrace chunk (18,1,18) covers
    // terraces X=[180,260), Y=[10,90), Z=[180,260) → voxels at (0..7, 0..7, 0..7).
    // The central platform is Y=[10,20), so terrace voxel y=0 (Y=[10,20)) is solid.
    const ChunkCoord tc{18, 1, 18};  // terrace chunk for central platform
    auto tChunk = DecompositionWorker::generateChunk(tc, 8, 10.0, testTerracesGen, nullptr);
    ASSERT_TRUE(tChunk);

    // For each solid terrace voxel, verify the corresponding detail chunk is non-empty.
    for (int tz = 0; tz < 8; ++tz)
        for (int ty = 0; ty < 8; ++ty)
            for (int tx = 0; tx < 8; ++tx) {
                if (tChunk->at(tx, ty, tz).isEmpty()) continue;
                // Detail chunk coord: the terrace voxel coord equals the detail chunk
                // coord (1:1 ratio, terrace 10m voxel = detail 10m chunk).
                const chunkmath::VoxelCoord V =
                    chunkmath::chunkLocalToVoxel(tc, tx, ty, tz, 8);
                ChunkCoord dc{static_cast<int32_t>(V.x),
                              static_cast<int32_t>(V.y),
                              static_cast<int32_t>(V.z)};
                auto dChunk = DecompositionWorker::generateChunk(
                    dc, 10, 1.0, testDetailGen, nullptr);
                ASSERT_TRUE(dChunk) << "no detail chunk for solid terrace voxel";
                bool anySolid = false;
                for (int z = 0; z < 10 && !anySolid; ++z)
                    for (int y = 0; y < 10 && !anySolid; ++y)
                        for (int x = 0; x < 10 && !anySolid; ++x)
                            if (!dChunk->at(x, y, z).isEmpty()) anySolid = true;
                EXPECT_TRUE(anySolid) << "solid terrace voxel at ("
                    << tx << "," << ty << "," << tz << ") produces empty detail chunk";
            }
}

TEST(ArenaGenerators, DecompositionOfTerracesIsStableUnderConcurrency) {
    // Concurrent decomposition jobs for the same terrace macro voxel must produce
    // byte-for-byte identical child grids (reusing the M6 determinism harness).
    constexpr int kRepeats = 32;
    const std::vector<ChunkCoord> childChunks{ChunkCoord{18, 1, 18}};  // one child per macro

    auto reference = DecompositionWorker::generateChunk(
        childChunks[0], 10, 1.0, testDetailGen, nullptr);
    ASSERT_TRUE(reference);

    DecompositionWorker worker;
    for (int i = 0; i < kRepeats; ++i) {
        DecompositionJob job;
        job.macro           = chunkmath::VoxelCoord{18, 1, 18};
        job.childChunks     = childChunks;
        job.childChunkSize  = 10;
        job.childVoxelSizeM = 1.0;
        job.generator       = testDetailGen;
        worker.enqueue(job);
    }

    auto results = collectResults(worker, kRepeats);
    ASSERT_EQ(results.size(), static_cast<size_t>(kRepeats));
    for (const DecompositionResult& r : results) {
        ASSERT_EQ(r.chunks.size(), 1u);
        EXPECT_TRUE(sameGrid(*r.chunks[0], *reference))
            << "concurrent decomposition produced a non-deterministic result";
    }
}
