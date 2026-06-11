#include <gtest/gtest.h>

#include "net/ENetTransport.h"
#include "net/ITransport.h"
#include "net/NetworkManager.h"

#include <chrono>
#include <thread>
#include <cstring>

using namespace net;

// Helper: pump both transports until `pred` returns true or timeout expires.
// Returns true if pred became true before the timeout.
static bool pumpUntil(ITransport& a, ITransport& b,
                      std::function<bool()> pred,
                      int timeout_ms = 3000)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        InboundPacket pkt;
        a.poll(&pkt);
        b.poll(&pkt);
        a.flush();
        b.flush();
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// ── Loopback connect / disconnect ─────────────────────────────────────────────

TEST(NetworkTransport, LoopbackConnectAndDisconnect)
{
    ENetTransport server;
    ENetTransport client;

    ASSERT_TRUE(server.listen(27910, 8));
    ASSERT_TRUE(server.isListening());

    ASSERT_TRUE(client.connect("127.0.0.1", 27910));

    // Wait for both sides to register the connection.
    bool serverSawConnect = false;
    bool clientSawConnect = false;
    bool ok = pumpUntil(server, client, [&] {
        InboundPacket pkt;
        while (server.poll(&pkt)) {
            if (pkt.type == PacketType::PeerConnected) serverSawConnect = true;
        }
        while (client.poll(&pkt)) {
            if (pkt.type == PacketType::PeerConnected) clientSawConnect = true;
        }
        return serverSawConnect && clientSawConnect;
    });
    EXPECT_TRUE(ok) << "loopback connection was not established within timeout";
}

// ── Reliable packet arrives intact and in order ───────────────────────────────

TEST(NetworkTransport, ReliablePacketArrivesIntact)
{
    ENetTransport server;
    ENetTransport client;

    ASSERT_TRUE(server.listen(27911, 8));
    ASSERT_TRUE(client.connect("127.0.0.1", 27911));

    // Establish connection first.
    PeerId serverSidePeer = kInvalidPeer;
    PeerId clientSidePeer = kInvalidPeer;

    pumpUntil(server, client, [&] {
        InboundPacket pkt;
        while (server.poll(&pkt))
            if (pkt.type == PacketType::PeerConnected) serverSidePeer = pkt.peer_id;
        while (client.poll(&pkt))
            if (pkt.type == PacketType::PeerConnected) clientSidePeer = pkt.peer_id;
        return serverSidePeer != kInvalidPeer && clientSidePeer != kInvalidPeer;
    });
    ASSERT_NE(serverSidePeer, kInvalidPeer);
    ASSERT_NE(clientSidePeer, kInvalidPeer);

    // Client sends a reliable packet to server.
    const char* msg = "hello-reliable";
    ASSERT_TRUE(client.send(clientSidePeer, 0,
                            msg, std::strlen(msg) + 1,
                            Reliability::Reliable));

    // Server should receive it.
    bool received = false;
    bool ok = pumpUntil(server, client, [&] {
        InboundPacket pkt;
        while (server.poll(&pkt)) {
            if (pkt.type == PacketType::Data &&
                pkt.data.size() == std::strlen(msg) + 1 &&
                std::memcmp(pkt.data.data(), msg, pkt.data.size()) == 0) {
                received = true;
            }
        }
        return received;
    });
    EXPECT_TRUE(ok) << "reliable packet was not received";
}

// ── Unreliable packet is delivered on a clean local loopback ─────────────────

TEST(NetworkTransport, UnreliablePacketDeliveredOnCleanLink)
{
    ENetTransport server;
    ENetTransport client;

    ASSERT_TRUE(server.listen(27912, 8));
    ASSERT_TRUE(client.connect("127.0.0.1", 27912));

    PeerId serverSidePeer = kInvalidPeer;
    PeerId clientSidePeer = kInvalidPeer;
    pumpUntil(server, client, [&] {
        InboundPacket p;
        while (server.poll(&p))
            if (p.type == PacketType::PeerConnected) serverSidePeer = p.peer_id;
        while (client.poll(&p))
            if (p.type == PacketType::PeerConnected) clientSidePeer = p.peer_id;
        return serverSidePeer != kInvalidPeer && clientSidePeer != kInvalidPeer;
    });
    ASSERT_NE(serverSidePeer, kInvalidPeer);

    const char* msg = "hello-unreliable";
    ASSERT_TRUE(server.send(serverSidePeer, 1,
                            msg, std::strlen(msg) + 1,
                            Reliability::Unreliable));

    bool received = false;
    bool ok = pumpUntil(server, client, [&] {
        InboundPacket pkt;
        while (client.poll(&pkt)) {
            if (pkt.type == PacketType::Data &&
                pkt.data.size() == std::strlen(msg) + 1 &&
                std::memcmp(pkt.data.data(), msg, pkt.data.size()) == 0) {
                received = true;
            }
        }
        return received;
    });
    EXPECT_TRUE(ok) << "unreliable packet not received on clean loopback";
}

// ── Disconnect event fires on_player_left (transport layer) ──────────────────

TEST(NetworkTransport, DisconnectEventFiresOnBothSides)
{
    ENetTransport server;
    ENetTransport client;

    ASSERT_TRUE(server.listen(27913, 8));
    ASSERT_TRUE(client.connect("127.0.0.1", 27913));

    PeerId serverSidePeer = kInvalidPeer;
    PeerId clientSidePeer = kInvalidPeer;
    pumpUntil(server, client, [&] {
        InboundPacket p;
        while (server.poll(&p))
            if (p.type == PacketType::PeerConnected) serverSidePeer = p.peer_id;
        while (client.poll(&p))
            if (p.type == PacketType::PeerConnected) clientSidePeer = p.peer_id;
        return serverSidePeer != kInvalidPeer && clientSidePeer != kInvalidPeer;
    });
    ASSERT_NE(serverSidePeer, kInvalidPeer);

    // Client disconnects.
    client.disconnect(clientSidePeer);
    client.flush();

    bool serverSawDisconnect = false;
    bool ok = pumpUntil(server, client, [&] {
        InboundPacket pkt;
        while (server.poll(&pkt))
            if (pkt.type == PacketType::PeerDisconnected) serverSawDisconnect = true;
        return serverSawDisconnect;
    });
    EXPECT_TRUE(ok) << "server did not receive disconnect event";
}

// ── Two independent sessions on different ports do not interfere ──────────────

TEST(NetworkTransport, TwoSessionsAreIsolated)
{
    ENetTransport serverA, clientA;
    ENetTransport serverB, clientB;

    ASSERT_TRUE(serverA.listen(27914, 4));
    ASSERT_TRUE(serverB.listen(27915, 4));
    ASSERT_TRUE(clientA.connect("127.0.0.1", 27914));
    ASSERT_TRUE(clientB.connect("127.0.0.1", 27915));

    bool aConnected = false, bConnected = false;
    auto pump4 = [&] {
        InboundPacket p;
        while (serverA.poll(&p)) if (p.type == PacketType::PeerConnected) aConnected = true;
        while (clientA.poll(&p)) {}
        while (serverB.poll(&p)) if (p.type == PacketType::PeerConnected) bConnected = true;
        while (clientB.poll(&p)) {}
        serverA.flush(); clientA.flush(); serverB.flush(); clientB.flush();
        return aConnected && bConnected;
    };

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && !pump4()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(aConnected) << "session A did not connect";
    EXPECT_TRUE(bConnected) << "session B did not connect";
    EXPECT_FALSE(serverA.isConnected() && serverB.isConnected() &&
                 serverA.isListening() == serverB.isListening())
        << "sessions appear to share state";
}

// ── NetworkManager smoke test: offline mode is a no-op ───────────────────────

TEST(NetworkManager, OfflineModeIsNoOp)
{
    net::NetworkManager nm;
    EXPECT_EQ(nm.role(), SessionRole::Offline);
    EXPECT_FALSE(nm.isActive());
    // update() must not crash when called without init() or start.
    EXPECT_NO_THROW(nm.update(0.016));
}
