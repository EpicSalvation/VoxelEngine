// M16 (L1) streaming-volume tests — the axis-agnostic residency predicate that
// replaces LODManager's old XZ-Chebyshev-disc × absolute-Y-band footprint.
//
// Covers: each shape is a correct camera-relative predicate (box isotropic with
// no Y privilege, sphere excludes box corners, shell admits only its band); the
// desired set tracks the camera through a deep descent where the old absolute
// band would have emptied; eviction hysteresis still prevents thrash; and the
// per-layer shape/radii are read from LayerDef through LODManager.

#include "core/LayerConfig.h"
#include "world/LODManager.h"
#include "world/StreamingVolume.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace {

bool contains(const std::vector<ChunkCoord>& v, ChunkCoord c) {
    return std::find(v.begin(), v.end(), c) != v.end();
}

StreamingVolume box(int r) {
    StreamingVolume v;
    v.shape = StreamingShape::box;
    v.radiusChunks = r;
    return v;
}

StreamingVolume sphere(int r) {
    StreamingVolume v;
    v.shape = StreamingShape::sphere;
    v.radiusChunks = r;
    return v;
}

StreamingVolume shell(int r, int thickness) {
    StreamingVolume v;
    v.shape = StreamingShape::shell;
    v.radiusChunks = r;
    v.shellThicknessChunks = thickness;
    return v;
}

}  // namespace

// ── Box: isotropic, no privileged axis ─────────────────────────────────────────

TEST(StreamingVolume, BoxIsIsotropicNoYPrivilege) {
    const StreamingVolume v = box(3);
    const ChunkCoord c{0, 0, 0};

    // A chunk at +r along ANY axis is treated identically — no Y bias.
    EXPECT_TRUE(v.contains(c, ChunkCoord{3, 0, 0}));
    EXPECT_TRUE(v.contains(c, ChunkCoord{0, 3, 0}));
    EXPECT_TRUE(v.contains(c, ChunkCoord{0, 0, 3}));
    EXPECT_TRUE(v.contains(c, ChunkCoord{-3, 0, 0}));
    EXPECT_TRUE(v.contains(c, ChunkCoord{0, -3, 0}));
    EXPECT_TRUE(v.contains(c, ChunkCoord{0, 0, -3}));

    // One past the radius along any axis is excluded, again identically.
    EXPECT_FALSE(v.contains(c, ChunkCoord{4, 0, 0}));
    EXPECT_FALSE(v.contains(c, ChunkCoord{0, 4, 0}));
    EXPECT_FALSE(v.contains(c, ChunkCoord{0, 0, 4}));

    // The corner is admitted (Chebyshev cube), and the desired set is the full cube.
    EXPECT_TRUE(v.contains(c, ChunkCoord{3, 3, 3}));
    const int side = 2 * 3 + 1;
    EXPECT_EQ(v.desired(c).size(), static_cast<size_t>(side * side * side));
}

TEST(StreamingVolume, BoxTracksCameraThroughDeepDescent) {
    // The whole point of L1: a box volume is camera-relative in Y, so a deep dig
    // streams downward with the player. The old absolute band would have emptied
    // the desired set the moment the camera left the configured Y range.
    const StreamingVolume v = box(2);
    for (int depth : {0, -50, -5000, -100'000}) {
        const ChunkCoord cam{0, depth, 0};
        const auto desired = v.desired(cam);
        const int side = 2 * 2 + 1;
        EXPECT_EQ(desired.size(), static_cast<size_t>(side * side * side)) << "depth " << depth;
        EXPECT_TRUE(contains(desired, cam)) << "depth " << depth;
        EXPECT_TRUE(contains(desired, ChunkCoord{0, depth - 2, 0})) << "depth " << depth;
        EXPECT_TRUE(v.contains(cam, ChunkCoord{0, depth - 2, 0})) << "depth " << depth;
    }
}

TEST(StreamingVolume, BoxVerticalBandClampsOnlyTheBox) {
    // The band is the box-volume convenience for heightmap worlds: it clamps the
    // otherwise isotropic cube in absolute chunk-Y.
    StreamingVolume v = box(3);
    v.yMin = 0;
    v.yMax = 1;
    const ChunkCoord cam{0, 0, 0};
    const auto desired = v.desired(cam);
    const int side = 2 * 3 + 1;
    EXPECT_EQ(desired.size(), static_cast<size_t>(side * side * 2));  // 2 band layers
    EXPECT_TRUE(contains(desired, ChunkCoord{3, 1, -3}));
    EXPECT_FALSE(contains(desired, ChunkCoord{0, 2, 0}));  // above band
    EXPECT_FALSE(v.contains(cam, ChunkCoord{0, -1, 0}));   // below band
}

// ── Sphere: isotropic Euclidean ball ───────────────────────────────────────────

TEST(StreamingVolume, SphereExcludesBoxCorners) {
    const StreamingVolume v = sphere(3);
    const ChunkCoord c{0, 0, 0};

    // On-axis at the radius: inside.
    EXPECT_TRUE(v.contains(c, ChunkCoord{3, 0, 0}));
    EXPECT_TRUE(v.contains(c, ChunkCoord{0, 3, 0}));
    EXPECT_TRUE(v.contains(c, ChunkCoord{0, 0, 3}));

    // The cube corner (|d| = 3√3 ≈ 5.2 > 3) is excluded — the difference from a box.
    EXPECT_FALSE(v.contains(c, ChunkCoord{3, 3, 3}));
    EXPECT_TRUE(box(3).contains(c, ChunkCoord{3, 3, 3}));

    // A point just inside the ball but off-axis is admitted (2² + 2² = 8 <= 9).
    EXPECT_TRUE(v.contains(c, ChunkCoord{2, 2, 0}));
    // And just outside (2² + 2² + 1² = 9 <= 9 is in; 2²+2²+2²=12 > 9 is out).
    EXPECT_TRUE(v.contains(c, ChunkCoord{2, 2, 1}));
    EXPECT_FALSE(v.contains(c, ChunkCoord{2, 2, 2}));

    // desired() never contains a chunk the predicate rejects, and is a strict
    // subset of the bounding box.
    const auto desired = v.desired(c);
    for (const ChunkCoord& cc : desired) EXPECT_TRUE(v.contains(c, cc));
    EXPECT_LT(desired.size(), box(3).desired(c).size());
    EXPECT_FALSE(contains(desired, ChunkCoord{3, 3, 3}));
}

// ── Shell: thin band for a backdrop ────────────────────────────────────────────

TEST(StreamingVolume, ShellAdmitsOnlyItsBand) {
    const StreamingVolume v = shell(/*outer=*/5, /*thickness=*/2);  // inner = 3
    const ChunkCoord c{0, 0, 0};

    // Inside the inner radius: excluded (a shell is hollow).
    EXPECT_FALSE(v.contains(c, ChunkCoord{0, 0, 0}));
    EXPECT_FALSE(v.contains(c, ChunkCoord{2, 0, 0}));  // |d|=2 < inner 3

    // Within the band [3, 5]: admitted, on any axis.
    EXPECT_TRUE(v.contains(c, ChunkCoord{3, 0, 0}));
    EXPECT_TRUE(v.contains(c, ChunkCoord{0, 4, 0}));
    EXPECT_TRUE(v.contains(c, ChunkCoord{0, 0, 5}));

    // Beyond the outer radius: excluded.
    EXPECT_FALSE(v.contains(c, ChunkCoord{6, 0, 0}));

    const auto desired = v.desired(c);
    for (const ChunkCoord& cc : desired) EXPECT_TRUE(v.contains(c, cc));
    EXPECT_FALSE(contains(desired, c));                  // hollow center
    EXPECT_TRUE(contains(desired, ChunkCoord{0, -4, 0}));
}

// ── Eviction hysteresis ────────────────────────────────────────────────────────

TEST(StreamingVolume, ExpandedByGrowsForHysteresis) {
    const StreamingVolume v = box(4);
    const StreamingVolume e = v.expandedBy(2);
    const ChunkCoord c{0, 0, 0};

    // Inside load radius: in both.
    EXPECT_TRUE(v.contains(c, ChunkCoord{4, 0, 0}));
    EXPECT_TRUE(e.contains(c, ChunkCoord{4, 0, 0}));
    // In the hysteresis margin: out of load, still inside the grown (evict) volume.
    EXPECT_FALSE(v.contains(c, ChunkCoord{5, 0, 0}));
    EXPECT_TRUE(e.contains(c, ChunkCoord{6, 0, 0}));
    // Beyond evict radius: out of both.
    EXPECT_FALSE(e.contains(c, ChunkCoord{7, 0, 0}));
}

TEST(StreamingVolume, ShellHysteresisWidensBothEdges) {
    const StreamingVolume v = shell(/*outer=*/6, /*thickness=*/2);  // band [4, 6]
    const StreamingVolume e = v.expandedBy(1);                      // band [3, 7]
    const ChunkCoord c{0, 0, 0};

    EXPECT_FALSE(v.contains(c, ChunkCoord{3, 0, 0}));  // below inner band
    EXPECT_TRUE(e.contains(c, ChunkCoord{3, 0, 0}));   // inner edge grew inward
    EXPECT_FALSE(v.contains(c, ChunkCoord{7, 0, 0}));  // above outer band
    EXPECT_TRUE(e.contains(c, ChunkCoord{7, 0, 0}));   // outer edge grew outward
}

// ── Per-layer shape read from LayerDef via LODManager ───────────────────────────

TEST(StreamingVolume, LODManagerReadsPerLayerShape) {
    LayerConfig cfg = LayerConfig::loadFromString(R"(
layers:
  - name: playspace
    voxel_size_m: 1.0
    mode: terminal
    view_distance_chunks: 4
  - name: backdrop
    voxel_size_m: 0.5
    mode: immutable
    view_distance_chunks: 10
    streaming_volume:
      shape: shell
      shell_thickness_chunks: 2
)");
    LODManager lod(cfg);

    const StreamingVolume play = lod.volumeFor("playspace");
    EXPECT_EQ(play.shape, StreamingShape::box);   // default
    EXPECT_EQ(play.radiusChunks, 4);

    const StreamingVolume back = lod.volumeFor("backdrop");
    EXPECT_EQ(back.shape, StreamingShape::shell);
    EXPECT_EQ(back.radiusChunks, 10);
    EXPECT_EQ(back.shellThicknessChunks, 2);

    // The shell layer's desired set is hollow; the box layer's is a full cube.
    const ChunkCoord cam{0, 0, 0};
    EXPECT_FALSE(contains(lod.desiredChunks(cam, "backdrop"), cam));
    EXPECT_TRUE(contains(lod.desiredChunks(cam, "playspace"), cam));

    // withinViewDistance / shouldEvict route through the same per-layer volume.
    EXPECT_TRUE(lod.withinViewDistance(cam, ChunkCoord{0, 10, 0}, "backdrop"));
    EXPECT_FALSE(lod.withinViewDistance(cam, cam, "backdrop"));  // hollow center
    EXPECT_TRUE(lod.shouldEvict(cam, ChunkCoord{0, 0, 0}, "backdrop"));
}
