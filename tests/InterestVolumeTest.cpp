// M16 (L6) interest-volume test: in StreamingRadius interest mode the network
// filter admits/suppresses exactly the chunks the peer's StreamingVolume contains
// for a NON-box shape — proving the interest filter consumes L1's per-layer volume
// (NetworkManager → LODManager::withinViewDistance → StreamingVolume) rather than
// re-deriving a box, and that interest is not left Y-biased after the L1 fix.
//
// Setup mirrors InterestManagementTest's harness (real loopback transport, two
// clients). The interest layer is a SPHERE of radius 2 chunks: a box-corner edit
// (Chebyshev-near but Euclidean-far) is suppressed, while on-axis edits — in X
// AND straight up in Y — are delivered.

#include <gtest/gtest.h>

#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "net/NetworkManager.h"
#include "world/Voxel.h"
#include "world/World.h"

#include <chrono>
#include <functional>
#include <thread>
#include <vector>

namespace {

// Sphere volume, radius 2 chunks × 8-voxel chunks at 1 m → an 8 m chunk grid.
const char* kSphereLayerYaml = R"(
layers:
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 8
    view_distance_chunks: 2
    streaming_volume:
      shape: sphere
)";

Voxel mat(uint8_t palette) {
    Voxel v;
    v.material.density       = 1000.0f;
    v.material.palette_index = palette;
    return v;
}

size_t g_editsAtA = 0;
void countEdit(WorldCoord, const Voxel*, const Voxel*, PlayerId, void* user_data) {
    ++*static_cast<size_t*>(user_data);
}
int initCountA(PluginContext* ctx) {
    ctx->register_on_voxel_modified(ctx, countEdit, &g_editsAtA);
    return 0;
}

bool pumpUntil(std::vector<net::NetworkManager*> nodes,
               const std::function<bool()>& pred, int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto* n : nodes) n->update(0.016);
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

}  // namespace

TEST(InterestVolume, StreamingRadiusConsumesSphereShape) {
    g_editsAtA = 0;

    LayerConfig config = LayerConfig::loadFromString(kSphereLayerYaml);

    World               serverWorld(config);
    PluginManager       serverPm;
    net::NetworkManager serverNm;
    serverNm.init(serverWorld, serverPm);
    serverNm.setLayerConfig(&config);  // builds the LODManager over the sphere volume
    ASSERT_TRUE(serverNm.startServer(28061, 8));

    World               worldA(config);
    PluginManager       pmA;
    net::NetworkManager nmA;
    pmA.wireInPlugin(initCountA);
    nmA.init(worldA, pmA);

    ASSERT_TRUE(nmA.startClient("127.0.0.1", 28061));
    ASSERT_TRUE(pumpUntil({&serverNm, &nmA},
                          [&] { return serverNm.connectedPeerCount() == 1; }));

    auto pumpAll = [&](const std::function<bool()>& pred) {
        return pumpUntil({&serverNm, &nmA}, pred);
    };

    // Peer A stands at the origin (chunk {0,0,0}).
    serverNm.updatePeerPosition(1, WorldCoord(0.0, 0.0, 0.0));
    serverNm.setInterestMode(net::NetworkManager::InterestMode::StreamingRadius);

    // ── On-axis in X, at the radius (chunk {2,0,0}, dist²=4 ≤ 4): delivered ───
    serverNm.applyEdit(net::kLocalPlayer, WorldCoord(18.5, 4.5, 4.5), mat(2));
    ASSERT_TRUE(pumpAll([&] { return g_editsAtA == 1; }));
    EXPECT_EQ(serverNm.suppressedEditCount(), 0u);

    // ── Straight up in Y, at the radius (chunk {0,2,0}): delivered ───────────
    // A box footprint would also admit this, but the point is the sphere is NOT
    // Y-biased — a vertical edit at the radius is treated exactly like a lateral one.
    serverNm.applyEdit(net::kLocalPlayer, WorldCoord(4.5, 18.5, 4.5), mat(3));
    ASSERT_TRUE(pumpAll([&] { return g_editsAtA == 2; }));
    EXPECT_EQ(serverNm.suppressedEditCount(), 0u);

    // ── Box corner (chunk {2,2,0}, dist²=8 > 4): SUPPRESSED by the sphere ────
    // A box volume would admit this corner (Chebyshev max = 2 ≤ 2); the sphere
    // rejects it. This is the discriminating case: interest consumes the shape.
    serverNm.applyEdit(net::kLocalPlayer, WorldCoord(18.5, 18.5, 4.5), mat(4));

    // Sentinel: a broadcast-all edit on the same ordered channel. When it arrives,
    // the suppressed corner edit (sent earlier) would have arrived too if it had
    // been delivered — so a stable count of 2 proves suppression, not lateness.
    serverNm.setInterestMode(net::NetworkManager::InterestMode::BroadcastAll);
    serverNm.applyEdit(net::kLocalPlayer, WorldCoord(900.5, 1.5, 900.5), mat(5));
    ASSERT_TRUE(pumpAll([&] { return g_editsAtA == 3; }));

    EXPECT_EQ(g_editsAtA, 3u);                       // axis edits + sentinel, NOT the corner
    EXPECT_EQ(serverNm.suppressedEditCount(), 1u);   // exactly the corner
}
