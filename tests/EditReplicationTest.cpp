// M11 edit replication through the single applyEdit choke point: a client edit
// reaches the server over loopback, passes on_edit_received, is committed, and
// arrives back at the client with the correct source on on_voxel_modified.
// Also: last-write-wins in arrival order, and a Discard handler suppressing the
// edit on both nodes. Headless (no window, no renderer).

#include <gtest/gtest.h>

#include "core/PluginManager.h"
#include "net/NetworkManager.h"
#include "world/World.h"
#include "world/Voxel.h"

#include <chrono>
#include <functional>
#include <thread>
#include <vector>

namespace {

LayerDef terrainLayer() {
    LayerDef d;
    d.name              = "terrain";
    d.voxel_size_m      = 1.0;
    d.mode              = VoxelMode::terminal;
    d.chunk_size_voxels = 8;
    return d;
}

Voxel mat(uint8_t palette, float density) {
    Voxel v;
    v.material.density       = density;
    v.material.palette_index = palette;
    return v;
}

// One in-process network node: world + plugin registry + manager.
struct Node {
    World                world;
    PluginManager        pm;
    net::NetworkManager  nm;

    Node() : world(terrainLayer()) {
        world.loadChunk(ChunkCoord{0, 0, 0}, nullptr);
        nm.init(world, pm);
    }
};

// Pump every node's update loop until pred holds or the timeout expires.
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

// on_voxel_modified records, one vector per node, written by the hook below.
struct EditEvent {
    WorldCoord pos;
    Voxel      voxel;
    PlayerId   source;
};
std::vector<EditEvent> g_serverEvents;
std::vector<EditEvent> g_clientEvents;

void recordEdit(WorldCoord pos, const Voxel* /*old_v*/, const Voxel* new_v,
                PlayerId source, void* user_data) {
    static_cast<std::vector<EditEvent>*>(user_data)
        ->push_back({pos, *new_v, source});
}

int initServerHooks(PluginContext* ctx) {
    ctx->register_on_voxel_modified(ctx, recordEdit, &g_serverEvents);
    return 0;
}
int initClientHooks(PluginContext* ctx) {
    ctx->register_on_voxel_modified(ctx, recordEdit, &g_clientEvents);
    return 0;
}

EditResolution discardAll(PlayerId, WorldCoord, const Voxel*, Voxel*, void*) {
    return EditResolution::Discard;
}
int initDiscardPolicy(PluginContext* ctx) {
    ctx->register_on_edit_received(ctx, discardAll, nullptr);
    return 0;
}

class EditReplicationTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_serverEvents.clear();
        g_clientEvents.clear();
    }

    // Bring up a connected server + client pair on the given port.
    bool connect(Node& server, Node& client, uint16_t port) {
        if (!server.nm.startServer(port, 8)) return false;
        if (!client.nm.startClient("127.0.0.1", port)) return false;
        return pumpUntil({&server.nm, &client.nm}, [&] {
            return server.nm.connectedPeerCount() == 1 &&
                   client.nm.connectedPeerCount() == 1;
        });
    }
};

}  // namespace

TEST_F(EditReplicationTest, ClientEditRoundTripsThroughAuthority) {
    Node server, client;
    server.pm.wireInPlugin(initServerHooks);
    client.pm.wireInPlugin(initClientHooks);
    ASSERT_TRUE(connect(server, client, 28021));

    const WorldCoord pos(1.5, 2.5, 3.5);
    const Voxel      stone = mat(2, 1500.0f);
    client.nm.applyEdit(net::kLocalPlayer, pos, stone);

    // The edit must travel client → server (EditIntent), commit, and return
    // (CommittedEdit) before the client's world reflects it.
    ASSERT_TRUE(pumpUntil({&server.nm, &client.nm}, [&] {
        return client.world.getVoxel(pos).material.palette_index == 2;
    }));

    EXPECT_EQ(server.world.getVoxel(pos).material.palette_index, 2);
    EXPECT_EQ(server.world.getVoxel(pos).material.density, 1500.0f);

    // The server committed with the player id it assigned to the client's peer
    // (1 — the first connection), and the client's replicated on_voxel_modified
    // carries that same id, not kLocalPlayer.
    ASSERT_EQ(g_serverEvents.size(), 1u);
    EXPECT_EQ(g_serverEvents[0].source, 1u);
    ASSERT_EQ(g_clientEvents.size(), 1u);
    EXPECT_EQ(g_clientEvents[0].source, 1u);
    EXPECT_EQ(g_clientEvents[0].voxel.material.palette_index, 2);
}

TEST_F(EditReplicationTest, ConcurrentEditsResolveLastWriteWinsInArrivalOrder) {
    Node server, client;
    ASSERT_TRUE(connect(server, client, 28022));

    const WorldCoord pos(4.5, 4.5, 4.5);

    // The server's local edit arrives first (it commits in-process)...
    server.nm.applyEdit(net::kLocalPlayer, pos, mat(3, 600.0f));
    ASSERT_TRUE(pumpUntil({&server.nm, &client.nm}, [&] {
        return client.world.getVoxel(pos).material.palette_index == 3;
    }));

    // ...then the client's competing write to the same voxel arrives and wins.
    client.nm.applyEdit(net::kLocalPlayer, pos, mat(5, 999.0f));
    ASSERT_TRUE(pumpUntil({&server.nm, &client.nm}, [&] {
        return client.world.getVoxel(pos).material.palette_index == 5;
    }));
    EXPECT_EQ(server.world.getVoxel(pos).material.palette_index, 5);
}

TEST_F(EditReplicationTest, DiscardResolutionSuppressesEditOnBothNodes) {
    Node server, client;
    server.pm.wireInPlugin(initServerHooks);
    server.pm.wireInPlugin(initDiscardPolicy);
    ASSERT_TRUE(connect(server, client, 28023));

    const WorldCoord discarded(2.5, 2.5, 2.5);
    const uint64_t   before = server.nm.packetsReceived();
    client.nm.applyEdit(net::kLocalPlayer, discarded, mat(2, 1500.0f));

    // Sentinel: a later edit on the same reliable, ordered channel. Its arrival
    // proves the discarded edit would have arrived too, had it been sent.
    // (Discard rejects everything at this server, so the sentinel is also never
    // committed — its round trip alone is the synchronisation point.)
    const WorldCoord sentinel(6.5, 6.5, 6.5);
    client.nm.applyEdit(net::kLocalPlayer, sentinel, mat(4, 100.0f));
    ASSERT_TRUE(pumpUntil({&server.nm, &client.nm}, [&] {
        return server.nm.packetsReceived() >= before + 2;
    }));

    EXPECT_TRUE(server.world.getVoxel(discarded).isEmpty());
    EXPECT_TRUE(client.world.getVoxel(discarded).isEmpty());
    EXPECT_TRUE(g_serverEvents.empty());  // never committed → never modified
    EXPECT_TRUE(g_clientEvents.empty());
}
