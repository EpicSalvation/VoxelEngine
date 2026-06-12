// M11 plugin messaging: a reliable Broadcast MessageEnvelope from one peer
// reaches every other peer (relayed through the authority) with the correct
// authority-stamped sender_id and the payload intact; channel_id prefix
// filtering keeps handlers on distinct channels isolated; and
// on_player_joined / on_player_left fire in connect/disconnect order.
// Headless (no window, no renderer).

#include <gtest/gtest.h>

#include "core/PluginManager.h"
#include "net/NetworkManager.h"
#include "world/World.h"
#include "world/Voxel.h"

#include <chrono>
#include <cstring>
#include <functional>
#include <string>
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

// A received message, deep-copied out of the transient envelope.
struct ReceivedMessage {
    std::string          channel;
    PlayerId             sender;
    std::vector<uint8_t> payload;
};

std::vector<ReceivedMessage> g_alphaAtServer;
std::vector<ReceivedMessage> g_alphaAtA;
std::vector<ReceivedMessage> g_alphaAtB;
std::vector<ReceivedMessage> g_betaAtA;

void recordMessage(const MessageEnvelope* env, void* user_data) {
    ReceivedMessage m;
    m.channel = env->channel_id ? env->channel_id : "";
    m.sender  = env->sender_id;
    if (env->payload && env->payload_size > 0) {
        const auto* bytes = static_cast<const uint8_t*>(env->payload);
        m.payload.assign(bytes, bytes + env->payload_size);
    }
    static_cast<std::vector<ReceivedMessage>*>(user_data)->push_back(std::move(m));
}

// Joined/left order observed at the server, encoded as +id / -id.
std::vector<int> g_serverLifecycle;

void onJoined(PlayerId id, WorldCoord, void*) {
    g_serverLifecycle.push_back(static_cast<int>(id));
}
void onLeft(PlayerId id, void*) {
    g_serverLifecycle.push_back(-static_cast<int>(id));
}

int initServerHooks(PluginContext* ctx) {
    ctx->register_on_network_message(ctx, "test.alpha", recordMessage, &g_alphaAtServer);
    ctx->register_on_player_joined(ctx, onJoined, nullptr);
    ctx->register_on_player_left(ctx, onLeft, nullptr);
    return 0;
}
int initClientAHooks(PluginContext* ctx) {
    ctx->register_on_network_message(ctx, "test.alpha", recordMessage, &g_alphaAtA);
    ctx->register_on_network_message(ctx, "test.beta",  recordMessage, &g_betaAtA);
    return 0;
}
int initClientBHooks(PluginContext* ctx) {
    ctx->register_on_network_message(ctx, "test.alpha", recordMessage, &g_alphaAtB);
    return 0;
}

}  // namespace

TEST(MessageEnvelope, BroadcastRelayChannelFilterAndLifecycleOrder) {
    g_alphaAtServer.clear();
    g_alphaAtA.clear();
    g_alphaAtB.clear();
    g_betaAtA.clear();
    g_serverLifecycle.clear();

    World               serverWorld(terrainLayer()), worldA(terrainLayer()),
                        worldB(terrainLayer());
    PluginManager       serverPm, pmA, pmB;
    net::NetworkManager serverNm, nmA, nmB;

    serverPm.wireInPlugin(initServerHooks);
    pmA.wireInPlugin(initClientAHooks);
    pmB.wireInPlugin(initClientBHooks);

    serverNm.init(serverWorld, serverPm);
    nmA.init(worldA, pmA);
    nmB.init(worldB, pmB);

    ASSERT_TRUE(serverNm.startServer(28051, 8));

    // Connect A then B so the server assigns player 1 to A and player 2 to B.
    ASSERT_TRUE(nmA.startClient("127.0.0.1", 28051));
    ASSERT_TRUE(pumpUntil({&serverNm, &nmA},
                          [&] { return serverNm.connectedPeerCount() == 1; }));
    ASSERT_TRUE(nmB.startClient("127.0.0.1", 28051));
    ASSERT_TRUE(pumpUntil({&serverNm, &nmA, &nmB},
                          [&] { return serverNm.connectedPeerCount() == 2; }));

    auto pumpAll = [&](const std::function<bool()>& pred) {
        return pumpUntil({&serverNm, &nmA, &nmB}, pred);
    };

    // ── Reliable Broadcast reaches every other peer with payload intact ──────
    // Binary payload with embedded zeros — must survive the round trip exactly.
    const uint8_t payload[] = {0x01, 0x00, 0xFF, 0x7E, 0x00, 0x42};
    MessageEnvelope env{};
    env.channel_id   = "test.alpha.chat";
    env.target       = MessageTarget::Broadcast;
    env.reliability  = MessageReliability::Reliable;
    env.payload      = payload;
    env.payload_size = sizeof(payload);
    nmA.sendNetworkMessage(env);

    ASSERT_TRUE(pumpAll([&] {
        return g_alphaAtServer.size() == 1 && g_alphaAtB.size() == 1;
    }));

    // The authority stamps the sender id from the connection (player 1 == A) and
    // relays to B; both deliveries carry it, with the full channel id and bytes.
    EXPECT_EQ(g_alphaAtServer[0].sender, 1u);
    EXPECT_EQ(g_alphaAtServer[0].channel, "test.alpha.chat");
    EXPECT_EQ(g_alphaAtServer[0].payload,
              std::vector<uint8_t>(payload, payload + sizeof(payload)));
    EXPECT_EQ(g_alphaAtB[0].sender, 1u);
    EXPECT_EQ(g_alphaAtB[0].payload,
              std::vector<uint8_t>(payload, payload + sizeof(payload)));
    EXPECT_TRUE(g_alphaAtA.empty());  // no self-delivery to the sender

    // ── Channel isolation: an unreliable message on a distinct channel ───────
    const char* text = "beta-only";
    MessageEnvelope beta{};
    beta.channel_id   = "test.beta.stream";
    beta.target       = MessageTarget::Broadcast;
    beta.reliability  = MessageReliability::Unreliable;
    beta.payload      = text;
    beta.payload_size = std::strlen(text);
    serverNm.sendNetworkMessage(beta);

    ASSERT_TRUE(pumpAll([&] { return g_betaAtA.size() == 1; }));
    EXPECT_EQ(g_betaAtA[0].channel, "test.beta.stream");
    // A's "test.alpha" handler (and B's, which has no beta handler) stay quiet.
    EXPECT_EQ(g_alphaAtA.size(), 0u);
    EXPECT_EQ(g_alphaAtB.size(), 1u);

    // ── Lifecycle order: joins in connect order, then A's departure ──────────
    nmA.stop();  // graceful disconnect — notifies the server
    ASSERT_TRUE(pumpUntil({&serverNm, &nmB},
                          [&] { return serverNm.connectedPeerCount() == 1; }));
    EXPECT_EQ(g_serverLifecycle, (std::vector<int>{1, 2, -1}));
}
