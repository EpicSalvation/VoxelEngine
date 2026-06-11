#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "plugin_api.h"  // WorldCoord, Voxel forward, PlayerId

// Forward declarations — no World/PluginManager headers pulled into this header
// to keep the net/ tier's include surface narrow (ARCHITECTURE §15).
// Note: Voxel and WorldCoord are already declared via plugin_api.h above.
class World;
class PluginManager;
class LayerConfig;
class LODManager;

namespace persistence { class WorldSave; }

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

    // Start as a dedicated server: bind to port and accept up to max_peers connections.
    bool startServer(uint16_t port, int max_peers = 32);

    // Start as a host-peer (authority + local client in one process).
    // The authority logic runs in-process; no separate server binary is needed.
    // Role becomes HostPeer; the authority policy plugin path is identical to Server.
    bool startHostPeer(uint16_t port, int max_peers = 32);

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

    // Single edit-application choke point (ARCHITECTURE §15).
    // Every voxel write — local player actions and remote incoming edits alike —
    // must pass through this function. It:
    //   1. Calls registered authority policies (returning false → reject).
    //   2. Calls on_edit_received at the authority node.
    //   3. Commits the result via World::setVoxel.
    //   4. Fires on_voxel_modified with the originating source PlayerId.
    //   5. Broadcasts the committed edit to all connected peers (Server/HostPeer).
    // When role == Client the edit intent is forwarded to the server instead of
    // being applied locally; the committed broadcast will apply it back.
    // When role == Offline the edit is applied directly (single-player path).
    void applyEdit(PlayerId source, WorldCoord pos, const Voxel& voxel);

    // Send a MessageEnvelope to the appropriate peer(s). Called by the
    // send_network_message plugin-context function.
    void sendNetworkMessage(const MessageEnvelope& env);

    // World state accessors for the join handshake.
    void setWorldSave(persistence::WorldSave* ws);
    void setWorldSeed(uint64_t seed);
    void setLayerConfig(const LayerConfig* config);

    // Streaming-radius interest management.
    enum class InterestMode { BroadcastAll, StreamingRadius };
    void setInterestMode(InterestMode mode);
    void updatePeerPosition(PlayerId player_id, WorldCoord pos);

    // Player position replication.
    const std::unordered_map<PlayerId, WorldCoord>& playerPositions() const;
    void broadcastLocalPosition(WorldCoord pos);

private:
    void handlePacket(InboundPacket& pkt);
    void onPeerConnected(PeerId peer_id);
    void onPeerDisconnected(PeerId peer_id);
    void handleEditIntent(PeerId sender_peer, const InboundPacket& pkt);
    void handleCommittedEdit(const InboundPacket& pkt);
    void handleNetMessage(const InboundPacket& pkt);

    // Commit an edit on the authority (Server/HostPeer) and broadcast it.
    // Called from applyEdit when running as authority.
    void commitAndBroadcast(PlayerId source, WorldCoord pos, const Voxel& voxel);

    // Send a committed-edit packet to all connected peers except the originator
    // (the originator will apply through the choke point on their end).
    void broadcastCommittedEdit(uint32_t seq, PlayerId source,
                                WorldCoord pos, const Voxel& voxel,
                                PeerId exclude_peer = 0);

    // Send join-handshake packets to a newly connected peer (Server/HostPeer only).
    void sendHandshakeToPeer(PeerId peer_id);
    // Re-send dirty chunks to a peer (server resync response).
    void sendDirtyChunksToPeer(PeerId peer_id);
    void handleJoinResponse(const InboundPacket& pkt);
    void handleDirtyChunkData(const InboundPacket& pkt);
    void handleJoinComplete(const InboundPacket& pkt);
    void handleResyncRequest(PeerId sender_peer, const InboundPacket& pkt);

    std::unique_ptr<ITransport> transport_;
    World*                      world_         = nullptr;
    PluginManager*              pm_            = nullptr;
    persistence::WorldSave*     worldSave_     = nullptr;
    const LayerConfig*          layerConfig_   = nullptr;
    uint64_t                    worldSeed_     = 0;
    SessionRole                 role_          = SessionRole::Offline;
    PlayerId                    localPlayerId_ = kLocalPlayer;
    PlayerId                    nextPlayerId_  = 1;
    uint32_t                    nextSeqNo_     = 1;
    uint32_t                    lastAppliedSeq_ = 0;

    InterestMode interestMode_ = InterestMode::BroadcastAll;

    // peer_id (transport-level) → player_id (game-level)
    std::unordered_map<PeerId, PlayerId> peerToPlayer_;
    // player_id (game-level) → peer_id (transport-level)
    std::unordered_map<PlayerId, PeerId> playerToPeer_;
    // last known WorldCoord per player (updated by player_position messages)
    std::unordered_map<PlayerId, WorldCoord> peerPositions_;

    // Received LayerConfig bytes during join handshake (client-side).
    std::vector<uint8_t> receivedConfigBytes_;

    // LODManager for streaming-radius interest filtering (lazily created).
    std::unique_ptr<LODManager> lodManager_;
};

} // namespace net
