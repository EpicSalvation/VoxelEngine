#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

// Forward declarations — no World/PluginManager headers pulled into this header
// to keep the net/ tier's include surface narrow (ARCHITECTURE §15).
class World;
class PluginManager;

namespace net {

class ITransport;
struct InboundPacket;
using PeerId = uint32_t;

// Stable identifier for a player / remote peer. 0 == kLocalPlayer.
using PlayerId = uint32_t;
constexpr PlayerId kLocalPlayer   = 0;
constexpr PlayerId kInvalidPlayer = ~PlayerId{0};

enum class SessionRole {
    Offline,    // networking disabled; all hooks are no-ops
    Server,     // dedicated authority node (listens)
    Client,     // remote participant (connects)
    HostPeer,   // host-as-authority: authority + client logic in one process
};

// NetworkManager owns the transport, the peer→player id table, and drives the
// per-tick poll/dispatch/flush loop. It must not depend on Renderer,
// PhysicsSystem, or DecompositionWorker (ARCHITECTURE §15).
class NetworkManager {
public:
    NetworkManager();
    ~NetworkManager();

    // Non-copyable (owns unique transport resource).
    NetworkManager(const NetworkManager&)            = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;

    // Bind engine systems. Must call before startServer/startClient.
    void init(World& world, PluginManager& pm);

    // Start as a server: bind to port and accept up to max_peers connections.
    bool startServer(uint16_t port, int max_peers = 32);

    // Start as a client: connect to a remote server.
    bool startClient(const std::string& host, uint16_t port);

    // Stop all networking and release the transport.
    void stop();

    // Per-tick update — called from Engine::tick() after world update, before
    // render. Polls the transport, dispatches packets, flushes outbound data.
    // Safe to call when role == Offline (becomes a no-op).
    void update(double dt);

    SessionRole role()          const { return role_; }
    bool        isActive()      const { return role_ != SessionRole::Offline; }
    PlayerId    localPlayerId() const { return localPlayerId_; }

    // Replace the default ENetTransport with a custom backend. Must be called
    // before startServer / startClient; the replaced transport is destroyed.
    void setTransport(std::unique_ptr<ITransport> transport);

private:
    void handlePacket(InboundPacket& pkt);
    void onPeerConnected(PeerId peer_id);
    void onPeerDisconnected(PeerId peer_id);

    std::unique_ptr<ITransport> transport_;
    World*         world_         = nullptr;
    PluginManager* pm_            = nullptr;
    SessionRole    role_          = SessionRole::Offline;
    PlayerId       localPlayerId_ = kLocalPlayer;
    PlayerId       nextPlayerId_  = 1;

    // peer_id (transport-level) → player_id (game-level)
    std::unordered_map<PeerId, PlayerId> peerToPlayer_;
};

} // namespace net
