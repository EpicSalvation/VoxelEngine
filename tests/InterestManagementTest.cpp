// M11 interest management at the authority: broadcast-all delivers every
// committed edit regardless of distance; mirrored-streaming-radius suppresses
// edits outside a peer's view distance; a registered InterestFilterFn overrides
// both built-in modes; and loading/unloading the filter plugin mid-session
// takes effect on the next edit. Headless (no window, no renderer).

#include <gtest/gtest.h>

#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "net/NetworkManager.h"
#include "world/World.h"
#include "world/Voxel.h"

#include <chrono>
#include <functional>
#include <thread>
#include <vector>

namespace {

// view_distance_chunks 2 × 8-voxel chunks at 1 m → a 16 m streaming radius.
const char* kLayerYaml = R"(
layers:
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 8
    view_distance_chunks: 2
)";

Voxel mat(uint8_t palette) {
    Voxel v;
    v.material.density       = 1000.0f;
    v.material.palette_index = palette;
    return v;
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

// Per-client counters of replicated on_voxel_modified events.
size_t g_editsAtA = 0;
size_t g_editsAtB = 0;

void countEdit(WorldCoord, const Voxel*, const Voxel*, PlayerId, void* user_data) {
    ++*static_cast<size_t*>(user_data);
}
int initCountA(PluginContext* ctx) {
    ctx->register_on_voxel_modified(ctx, countEdit, &g_editsAtA);
    return 0;
}
int initCountB(PluginContext* ctx) {
    ctx->register_on_voxel_modified(ctx, countEdit, &g_editsAtB);
    return 0;
}

bool suppressAll(PlayerId, WorldCoord, void*) { return false; }
int initSuppressFilter(PluginContext* ctx) {
    ctx->register_interest_filter(ctx, suppressAll, nullptr);
    return 0;
}

}  // namespace

TEST(InterestManagement, ModesFilterAndPluginOverride) {
    g_editsAtA = 0;
    g_editsAtB = 0;

    LayerConfig config = LayerConfig::loadFromString(kLayerYaml);

    World               serverWorld(config);
    PluginManager       serverPm;
    net::NetworkManager serverNm;
    serverNm.init(serverWorld, serverPm);
    serverNm.setLayerConfig(&config);  // enables the streaming-radius check
    ASSERT_TRUE(serverNm.startServer(28041, 8));

    World               worldA(config), worldB(config);
    PluginManager       pmA, pmB;
    net::NetworkManager nmA, nmB;
    pmA.wireInPlugin(initCountA);
    pmB.wireInPlugin(initCountB);
    nmA.init(worldA, pmA);
    nmB.init(worldB, pmB);

    // Connect A then B so the server assigns player 1 to A and player 2 to B.
    ASSERT_TRUE(nmA.startClient("127.0.0.1", 28041));
    ASSERT_TRUE(pumpUntil({&serverNm, &nmA},
                          [&] { return serverNm.connectedPeerCount() == 1; }));
    ASSERT_TRUE(nmB.startClient("127.0.0.1", 28041));
    ASSERT_TRUE(pumpUntil({&serverNm, &nmA, &nmB},
                          [&] { return serverNm.connectedPeerCount() == 2; }));

    auto pumpAll = [&](const std::function<bool()>& pred) {
        return pumpUntil({&serverNm, &nmA, &nmB}, pred);
    };

    // Peer A stands at the origin; peer B is far away.
    serverNm.updatePeerPosition(1, WorldCoord(0.0, 0.0, 0.0));
    serverNm.updatePeerPosition(2, WorldCoord(1000.0, 0.0, 1000.0));

    // ── Broadcast-all (default): distance is irrelevant ──────────────────────
    // An edit nowhere near either peer is still delivered to both.
    serverNm.applyEdit(net::kLocalPlayer, WorldCoord(500.5, 1.5, 500.5), mat(2));
    ASSERT_TRUE(pumpAll([&] { return g_editsAtA == 1 && g_editsAtB == 1; }));
    EXPECT_EQ(serverNm.suppressedEditCount(), 0u);

    // ── Mirrored streaming radius: only peers whose radius covers the edit ───
    serverNm.setInterestMode(net::NetworkManager::InterestMode::StreamingRadius);
    serverNm.applyEdit(net::kLocalPlayer, WorldCoord(1.5, 1.5, 1.5), mat(3));
    ASSERT_TRUE(pumpAll([&] { return g_editsAtA == 2; }));
    EXPECT_EQ(serverNm.suppressedEditCount(), 1u);  // suppressed for B

    // A broadcast-all sentinel on the same ordered channel: when it reaches B,
    // any earlier packet addressed to B would have arrived too — proving the
    // radius-filtered edit was suppressed, not merely late.
    serverNm.setInterestMode(net::NetworkManager::InterestMode::BroadcastAll);
    serverNm.applyEdit(net::kLocalPlayer, WorldCoord(900.5, 1.5, 900.5), mat(4));
    ASSERT_TRUE(pumpAll([&] { return g_editsAtA == 3 && g_editsAtB == 2; }));

    // ── Plugin filter overrides the built-in mode entirely ───────────────────
    // suppressAll returns false for every (peer, edit) pair, so nothing is
    // delivered even though the mode is broadcast-all and, after switching, even
    // for an edit inside A's streaming radius.
    PluginId filterId = serverPm.wireInPlugin(initSuppressFilter);
    ASSERT_NE(filterId, kInvalidPluginId);
    serverNm.applyEdit(net::kLocalPlayer, WorldCoord(2.5, 1.5, 2.5), mat(5));
    serverNm.setInterestMode(net::NetworkManager::InterestMode::StreamingRadius);
    serverNm.applyEdit(net::kLocalPlayer, WorldCoord(3.5, 1.5, 3.5), mat(6));
    EXPECT_EQ(serverNm.suppressedEditCount(), 5u);  // 1 + (2 peers × 2 edits)

    // ── Unloading the filter plugin takes effect on the next edit ────────────
    serverNm.setInterestMode(net::NetworkManager::InterestMode::BroadcastAll);
    ASSERT_TRUE(serverPm.unloadPlugin(filterId));
    serverNm.applyEdit(net::kLocalPlayer, WorldCoord(4.5, 1.5, 4.5), mat(7));
    ASSERT_TRUE(pumpAll([&] { return g_editsAtA == 4 && g_editsAtB == 3; }));
    // The filtered edits never arrive: the sentinel above proves suppression.
    EXPECT_EQ(g_editsAtA, 4u);
    EXPECT_EQ(g_editsAtB, 3u);
}
