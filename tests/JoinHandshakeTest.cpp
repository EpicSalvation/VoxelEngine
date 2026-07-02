// M11 world-join handshake: a joining client receives the world seed and the
// serialized LayerConfig in JoinResponse, then every dirty chunk held by the
// server's WorldSave — and nothing else (clean chunks are re-derived locally
// from the shared seed, never sent). Headless (no window, no renderer).

#include <gtest/gtest.h>

#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "io/ChunkPersistence.h"
#include "net/ENetTransport.h"
#include "net/NetJoinHandshake.h"
#include "net/NetworkManager.h"
#include "world/World.h"
#include "world/Voxel.h"

#include "world/ChunkCoordMath.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <limits>
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

TEST_F(JoinHandshakeTest, ResyncRequestsAreRateLimitedPerPeer) {
    LayerConfig config = LayerConfig::loadFromString(kLayerYaml);

    World         serverWorld(config);
    PluginManager serverPm;
    serverWorld.loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    serverWorld.setVoxel(WorldCoord(1.5, 1.5, 1.5), mat(2, 1500.0f));

    persistence::WorldSave save(dir.string(), persistence::WorldIdentity{1.0, 8});
    ASSERT_EQ(save.saveDirtyChunks(serverWorld), 1);

    net::NetworkManager serverNm;
    serverNm.init(serverWorld, serverPm);
    serverNm.setWorldSave(&save);
    serverNm.setWorldSeed(kSeed);
    serverNm.setLayerConfig(&config);
    ASSERT_TRUE(serverNm.startServer(28032, 8));

    // Raw transport peer: lets the test send crafted ResyncRequest packets and
    // count the JoinComplete markers that end each dirty-chunk stream.
    net::ENetTransport raw;
    ASSERT_TRUE(raw.connect("127.0.0.1", 28032));

    net::PeerId serverPeer   = net::kInvalidPeer;
    int         joinCompletes = 0;
    auto pump = [&](const std::function<bool()>& pred, int timeout_ms = 5000) {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            serverNm.update(0.016);
            net::InboundPacket pkt;
            while (raw.poll(&pkt)) {
                if (pkt.type == net::PacketType::PeerConnected) {
                    serverPeer = pkt.peer_id;
                } else if (pkt.type == net::PacketType::Data && !pkt.data.empty() &&
                           pkt.data[0] ==
                               static_cast<uint8_t>(net::NetPacketKind::JoinComplete)) {
                    ++joinCompletes;
                }
            }
            raw.flush();
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };

    // Initial join handshake ends with the first JoinComplete.
    ASSERT_TRUE(pump([&] { return joinCompletes == 1; }));
    ASSERT_NE(serverPeer, net::kInvalidPeer);

    // Two back-to-back resync requests: the first is served (a second
    // JoinComplete arrives), the second lands inside the cooldown and is
    // dropped, not queued.
    std::vector<uint8_t> req;
    net::write_u8(req, static_cast<uint8_t>(net::NetPacketKind::ResyncRequest));
    ASSERT_TRUE(raw.send(serverPeer, 0, req.data(), req.size(),
                         net::Reliability::Reliable));
    ASSERT_TRUE(raw.send(serverPeer, 0, req.data(), req.size(),
                         net::Reliability::Reliable));
    raw.flush();

    ASSERT_TRUE(pump([&] {
        return joinCompletes >= 2 && serverNm.suppressedResyncCount() >= 1;
    }));
    EXPECT_EQ(joinCompletes, 2);
    EXPECT_EQ(serverNm.suppressedResyncCount(), 1u);
}

// ── Authority hardening (2026-07 security review) ─────────────────────────────
// Client-only handshake packets and malformed edits arriving AT the authority
// must be ignored: accepting them would let any connected peer overwrite the
// server's world seed, inject chunks past the authority policies, flip its join
// state, or feed NaN coordinates into the chunk-coord math.

TEST_F(JoinHandshakeTest, AuthorityIgnoresClientOnlyAndMalformedPackets) {
    LayerConfig config = LayerConfig::loadFromString(kLayerYaml);

    World         serverWorld(config);
    PluginManager serverPm;

    persistence::WorldSave save(dir.string(), persistence::WorldIdentity{1.0, 8});

    net::NetworkManager serverNm;
    serverNm.init(serverWorld, serverPm);
    serverNm.setWorldSave(&save);
    serverNm.setWorldSeed(kSeed);
    serverNm.setLayerConfig(&config);
    ASSERT_TRUE(serverNm.startServer(28033, 8));

    net::ENetTransport raw;
    ASSERT_TRUE(raw.connect("127.0.0.1", 28033));

    net::PeerId serverPeer = net::kInvalidPeer;
    auto pump = [&](const std::function<bool()>& pred, int timeout_ms = 5000) {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            serverNm.update(0.016);
            net::InboundPacket pkt;
            while (raw.poll(&pkt)) {
                if (pkt.type == net::PacketType::PeerConnected)
                    serverPeer = pkt.peer_id;
            }
            raw.flush();
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };
    ASSERT_TRUE(pump([&] { return serverPeer != net::kInvalidPeer &&
                                  serverNm.connectedPeerCount() == 1; }));

    const uint64_t packetsBefore = serverNm.packetsReceived();

    // 1) JoinResponse claiming a different seed.
    net::JoinResponsePayload jr;
    jr.world_seed = 0xDEADBEEFULL;
    auto jrBuf = net::encode_join_response(jr);
    ASSERT_TRUE(raw.send(serverPeer, 0, jrBuf.data(), jrBuf.size(),
                         net::Reliability::Reliable));

    // 2) DirtyChunkData carrying a validly encoded chunk the server never made.
    Chunk evil(ChunkCoord{7, 0, 0}, 8, chunkmath::chunkOrigin({7, 0, 0}, 1.0, 8));
    net::DirtyChunkDataPayload dcd;
    dcd.chunk_bytes = persistence::encodeChunkFile(
        evil, persistence::WorldIdentity{1.0, 8});
    auto dcdBuf = net::encode_dirty_chunk_data(dcd);
    ASSERT_TRUE(raw.send(serverPeer, 0, dcdBuf.data(), dcdBuf.size(),
                         net::Reliability::Reliable));

    // 3) JoinComplete aimed at the authority.
    std::vector<uint8_t> jc;
    net::write_u8(jc, static_cast<uint8_t>(net::NetPacketKind::JoinComplete));
    ASSERT_TRUE(raw.send(serverPeer, 0, jc.data(), jc.size(),
                         net::Reliability::Reliable));

    // 4) EditIntent with non-finite coordinates.
    net::EditIntentPayload ei{};
    ei.x = std::numeric_limits<double>::quiet_NaN();
    ei.y = std::numeric_limits<double>::infinity();
    ei.z = 0.0;
    ei.palette_index = 3;
    auto eiBuf = net::encode_edit_intent(ei);
    ASSERT_TRUE(raw.send(serverPeer, 0, eiBuf.data(), eiBuf.size(),
                         net::Reliability::Reliable));
    raw.flush();

    // All four packets dispatched (reliable channel, so none can be lost).
    ASSERT_TRUE(pump([&] {
        return serverNm.packetsReceived() >= packetsBefore + 4;
    }));

    EXPECT_EQ(serverNm.worldSeed(), kSeed);                      // (1) ignored
    EXPECT_EQ(serverWorld.getChunk(ChunkCoord{7, 0, 0}), nullptr);  // (2) ignored
    EXPECT_FALSE(serverNm.joinComplete());                       // (3) ignored
    // (4): reaching here without UB/crash and with no chunk created is the
    // observable contract; the edit was dropped before the coord math ran.
    EXPECT_EQ(serverWorld.getChunk(ChunkCoord{7, 0, 0}), nullptr);
}

// A layer name carrying YAML metacharacters must round-trip through the binary
// handshake as a literal string — not break out of the quoted scalar and inject
// config keys (pre-escaping, the newline+quote below rewrote chunk_size_voxels).
TEST(NetJoinHandshakeCodec, LayerNameWithYamlMetacharactersRoundTrips) {
    const std::string evil =
        "x\"\n    chunk_size_voxels: 999\n    name: \"y";

    std::vector<uint8_t> buf;
    net::write_u32(buf, 1);  // num_layers
    net::write_u32(buf, static_cast<uint32_t>(evil.size()));
    for (char c : evil) buf.push_back(static_cast<uint8_t>(c));
    net::write_f64(buf, 1.0);   // voxel_size_m
    net::write_u8(buf, 2);      // mode = terminal
    net::write_u8(buf, 0);      // no decompose_to
    net::write_u32(buf, 8);     // chunk_size_voxels
    net::write_u32(buf, 4);     // view_distance_chunks
    net::write_u32(buf, 0);     // resident_chunk_budget

    LayerConfig config = net::deserializeLayerConfig(buf);
    ASSERT_EQ(config.layers().size(), 1u);
    EXPECT_EQ(config.layers()[0].name, evil);          // literal round-trip
    EXPECT_EQ(config.layers()[0].chunk_size_voxels, 8);  // injection had no effect
}
