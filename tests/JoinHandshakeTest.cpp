// M11 world-join handshake: a joining client receives the world seed and the
// serialized LayerConfig in JoinResponse, then every dirty chunk held by the
// server's WorldSave — and nothing else (clean chunks are re-derived locally
// from the shared seed, never sent). Headless (no window, no renderer).

#include <gtest/gtest.h>

#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "io/ChunkPersistence.h"
#include "net/NetJoinHandshake.h"
#include "net/NetworkManager.h"
#include "world/World.h"
#include "world/Voxel.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t kSeed = 0xC0FFEE1234ULL;

const char* kLayerYaml = R"(
layers:
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 8
    view_distance_chunks: 4
)";

Voxel mat(uint8_t palette, float density) {
    Voxel v;
    v.material.density       = density;
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

class JoinHandshakeTest : public ::testing::Test {
protected:
    std::filesystem::path dir;

    void SetUp() override {
        dir = std::filesystem::temp_directory_path() / "voxel_join_handshake_test";
        std::filesystem::remove_all(dir);
    }
    void TearDown() override { std::filesystem::remove_all(dir); }
};

}  // namespace

TEST_F(JoinHandshakeTest, ClientReceivesSeedConfigAndOnlyDirtyChunks) {
    LayerConfig config = LayerConfig::loadFromString(kLayerYaml);

    // Server world: two chunks made dirty by player edits and saved, plus one
    // resident chunk that was never edited (stays clean, never saved).
    World         serverWorld(config);
    PluginManager serverPm;
    serverWorld.loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    serverWorld.loadChunk(ChunkCoord{1, 0, 0}, nullptr);
    serverWorld.loadChunk(ChunkCoord{2, 0, 0}, nullptr);  // clean
    serverWorld.setVoxel(WorldCoord(1.5, 1.5, 1.5),  mat(2, 1500.0f));
    serverWorld.setVoxel(WorldCoord(9.5, 2.5, 3.5),  mat(5, 999.0f));

    persistence::WorldSave save(dir.string(), persistence::WorldIdentity{1.0, 8});
    ASSERT_EQ(save.saveDirtyChunks(serverWorld), 2);

    net::NetworkManager serverNm;
    serverNm.init(serverWorld, serverPm);
    serverNm.setWorldSave(&save);
    serverNm.setWorldSeed(kSeed);
    serverNm.setLayerConfig(&config);
    ASSERT_TRUE(serverNm.startServer(28031, 8));

    auto joinClient = [&](World& clientWorld, PluginManager& clientPm,
                          net::NetworkManager& clientNm) {
        clientNm.init(clientWorld, clientPm);
        ASSERT_TRUE(clientNm.startClient("127.0.0.1", 28031));
        ASSERT_TRUE(pumpUntil({&serverNm, &clientNm},
                              [&] { return clientNm.joinComplete(); }));

        // JoinResponse carried the seed and a LayerConfig that round-trips to
        // the server's stack — the client can initialise its World the same way
        // single-player startup does.
        EXPECT_EQ(clientNm.worldSeed(), kSeed);
        LayerConfig received =
            net::deserializeLayerConfig(clientNm.receivedConfigBytes());
        ASSERT_EQ(received.layers().size(), 1u);
        EXPECT_EQ(received.layers()[0].name, "terrain");
        EXPECT_EQ(received.layers()[0].mode, VoxelMode::terminal);
        EXPECT_EQ(received.layers()[0].chunk_size_voxels, 8);
        EXPECT_EQ(received.layers()[0].voxel_size_m, 1.0);

        // Both dirty chunks arrived and were inserted with the edits intact.
        ASSERT_NE(clientWorld.getChunk(ChunkCoord{0, 0, 0}), nullptr);
        ASSERT_NE(clientWorld.getChunk(ChunkCoord{1, 0, 0}), nullptr);
        EXPECT_EQ(clientWorld.getVoxel(WorldCoord(1.5, 1.5, 1.5)).material.palette_index, 2);
        EXPECT_EQ(clientWorld.getVoxel(WorldCoord(9.5, 2.5, 3.5)).material.palette_index, 5);

        // The clean chunk was not sent — it is re-derived locally.
        EXPECT_EQ(clientWorld.getChunk(ChunkCoord{2, 0, 0}), nullptr);
    };

    World               clientWorldA(config);
    PluginManager       clientPmA;
    net::NetworkManager clientNmA;
    joinClient(clientWorldA, clientPmA, clientNmA);

    // A second client joining the live session gets the same dirty-chunk
    // stream, independently of the first.
    World               clientWorldB(config);
    PluginManager       clientPmB;
    net::NetworkManager clientNmB;
    joinClient(clientWorldB, clientPmB, clientNmB);
}
