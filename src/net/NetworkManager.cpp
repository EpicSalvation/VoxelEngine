#include "net/NetworkManager.h"
#include "net/ITransport.h"
#include "net/ENetTransport.h"
#include "net/NetPackets.h"
#include "net/NetJoinHandshake.h"
#include "core/Logger.h"
#include "core/PluginManager.h"
#include "core/LayerConfig.h"
#include "world/World.h"
#include "world/Voxel.h"
#include "world/LODManager.h"
#include "io/ChunkPersistence.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace net {

NetworkManager::NetworkManager() = default;
NetworkManager::~NetworkManager() { stop(); }

void NetworkManager::init(World& world, PluginManager& pm)
{
    world_ = &world;
    pm_    = &pm;

    // Wire plugin sends (ctx->send_network_message) through to this manager so
    // a plugin-built MessageEnvelope reaches the transport (ARCHITECTURE §15).
    pm.setNetworkSendHandler(
        [](const MessageEnvelope* env, void* user) {
            if (env) static_cast<NetworkManager*>(user)->sendNetworkMessage(*env);
        },
        this);

    // Install the default ENet transport if no custom one was set.
    if (!transport_) {
        transport_ = std::make_unique<ENetTransport>();
    }
}

bool NetworkManager::startServer(uint16_t port, int max_peers)
{
    if (!transport_) {
        Log::warn("[NetworkManager] startServer called before init()");
        return false;
    }
    if (!transport_->listen(port, max_peers)) {
        Log::warn("[NetworkManager] transport listen() failed");
        return false;
    }
    role_          = SessionRole::Server;
    localPlayerId_ = kLocalPlayer;
    joinComplete_  = false;
    std::cout << "[NetworkManager] server listening on port " << port << '\n';
    return true;
}

bool NetworkManager::startHostPeer(uint16_t port, int max_peers)
{
    if (!transport_) {
        Log::warn("[NetworkManager] startHostPeer called before init()");
        return false;
    }
    if (!transport_->listen(port, max_peers)) {
        Log::warn("[NetworkManager] transport listen() failed");
        return false;
    }
    // HostPeer runs the authority in-process and acts as its own local client.
    // The authority-plugin code path is identical to Server; the only difference
    // is that SessionRole::HostPeer signals to demos that no external connect is
    // required for the local player.
    role_          = SessionRole::HostPeer;
    localPlayerId_ = kLocalPlayer;
    joinComplete_  = false;
    std::cout << "[NetworkManager] host-peer listening on port " << port << '\n';
    return true;
}

bool NetworkManager::startClient(const std::string& host, uint16_t port)
{
    if (!transport_) {
        Log::warn("[NetworkManager] startClient called before init()");
        return false;
    }
    if (!transport_->connect(host, port)) {
        Log::warn(("[NetworkManager] transport connect() to " + host + ":" +
                   std::to_string(port) + " failed").c_str());
        return false;
    }
    role_          = SessionRole::Client;
    localPlayerId_ = kLocalPlayer;
    joinComplete_  = false;
    std::cout << "[NetworkManager] client connecting to " << host << ':' << port << '\n';
    return true;
}

void NetworkManager::stop()
{
    if (role_ == SessionRole::Offline) return;
    if (transport_) {
        // Politely disconnect every peer, then service the transport briefly so
        // the disconnect notifications actually reach the wire — otherwise the
        // remote side only finds out via its connection timeout.
        for (auto& [peer_id, player_id] : peerToPlayer_) {
            (void)player_id;
            transport_->disconnect(peer_id);
        }
        transport_->flush();
        InboundPacket pkt;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(100);
        while (std::chrono::steady_clock::now() < deadline) {
            while (transport_->poll(&pkt)) {}
            transport_->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    role_ = SessionRole::Offline;
    peerToPlayer_.clear();
    playerToPeer_.clear();
    std::cout << "[NetworkManager] stopped\n";
}

void NetworkManager::setTransport(std::unique_ptr<ITransport> transport)
{
    transport_ = std::move(transport);
}

// ── Per-tick update ────────────────────────────────────────────────────────────

void NetworkManager::update(double /*dt*/)
{
    if (role_ == SessionRole::Offline || !transport_) return;

    InboundPacket pkt;
    while (transport_->poll(&pkt)) {
        ++packetsReceived_;
        handlePacket(pkt);
    }
    transport_->flush();
}

// ── Edit application choke point ──────────────────────────────────────────────

void NetworkManager::applyEdit(PlayerId source, WorldCoord pos, const Voxel& voxel)
{
    if (!world_ || !pm_) return;

    const bool isAuthority = (role_ == SessionRole::Server ||
                               role_ == SessionRole::HostPeer ||
                               role_ == SessionRole::Offline);

    if (!isAuthority) {
        // Client: forward the edit intent to the server rather than applying locally.
        EditIntentPayload intent;
        intent.x                    = pos.value.x;
        intent.y                    = pos.value.y;
        intent.z                    = pos.value.z;
        intent.density              = voxel.material.density;
        intent.structural_strength  = voxel.material.structural_strength;
        intent.thermal_conductivity = voxel.material.thermal_conductivity;
        intent.porosity             = voxel.material.porosity;
        intent.hardness             = voxel.material.hardness;
        intent.palette_index        = voxel.material.palette_index;
        auto buf = encode_edit_intent(intent);
        // Send on channel 0 (Reliable) to the server (peer 1 in a simple two-node setup).
        // In a multi-peer topology, the "server peer" is always the first connected peer.
        if (transport_->isConnected()) {
            // Find any connected server peer — use peer id 1 as the convention for the
            // first connected peer (the server) in a client session.
            for (auto& [peer_id, player_id] : peerToPlayer_) {
                transport_->send(peer_id, 0, buf.data(), buf.size(), Reliability::Reliable);
                break;  // only one server
            }
        }
        return;
    }

    // Authority path: check registered authority policies first.
    for (const auto& policy : pm_->authorityPolicies()) {
        if (policy.fn && !policy.fn(source, pos, &voxel, policy.user_data)) {
            // Policy rejected the edit — silent discard.
            return;
        }
    }

    // Commit via on_edit_received (default: Apply / last-write-wins).
    Voxel committed = voxel;
    const auto& editHooks = pm_->editReceivedHooks();
    if (!editHooks.empty()) {
        Voxel transformed = voxel;
        EditResolution res = editHooks[0].fn(source, pos, &voxel, &transformed,
                                             editHooks[0].user_data);
        if (res == EditResolution::Discard) return;
        if (res == EditResolution::Transform) committed = transformed;
    }

    commitAndBroadcast(source, pos, committed);
}

void NetworkManager::commitAndBroadcast(PlayerId source, WorldCoord pos, const Voxel& voxel)
{
    if (!world_ || !pm_) return;

    // Commit the edit.
    const Voxel old_voxel = world_->getVoxel(pos);
    world_->setVoxel(pos, voxel);

    // Fire on_voxel_modified with the originating source.
    for (const auto& hook : pm_->voxelModifiedHooks()) {
        if (hook.fn) hook.fn(pos, &old_voxel, &voxel, source, hook.user_data);
    }

    // Broadcast the committed edit to all peers (Server/HostPeer only).
    if (role_ == SessionRole::Server || role_ == SessionRole::HostPeer) {
        uint32_t seq = nextSeqNo_++;
        broadcastCommittedEdit(seq, source, pos, voxel, /*exclude_peer=*/0);
    }
}

void NetworkManager::broadcastCommittedEdit(uint32_t seq, PlayerId source,
                                             WorldCoord pos, const Voxel& voxel,
                                             PeerId exclude_peer)
{
    if (!transport_) return;

    CommittedEditPayload p;
    p.seq                    = seq;
    p.source_player          = source;
    p.x                      = pos.value.x;
    p.y                      = pos.value.y;
    p.z                      = pos.value.z;
    p.density                = voxel.material.density;
    p.structural_strength    = voxel.material.structural_strength;
    p.thermal_conductivity   = voxel.material.thermal_conductivity;
    p.porosity               = voxel.material.porosity;
    p.hardness               = voxel.material.hardness;
    p.palette_index          = voxel.material.palette_index;

    auto buf = encode_committed_edit(p);

    // Interest filter: plugin escape hatch takes priority over built-in modes.
    const auto* filters_ptr = pm_ ? &pm_->interestFilters() : nullptr;

    for (auto& [peer_id, player_id] : peerToPlayer_) {
        if (peer_id == exclude_peer) continue;

        bool send = true;
        if (filters_ptr && !filters_ptr->empty()) {
            // Plugin filter overrides built-in mode entirely.
            send = (*filters_ptr)[0].fn(player_id, pos, (*filters_ptr)[0].user_data);
        } else if (interestMode_ == InterestMode::StreamingRadius && layerConfig_) {
            // Mirrored-streaming-radius: only send if the edit is within the
            // peer's streaming radius based on last known peer position.
            auto pit = peerPositions_.find(player_id);
            if (pit != peerPositions_.end()) {
                // Use the first terminal layer for radius check.
                const LayerDef* primary = nullptr;
                for (const auto& ld : layerConfig_->layers()) {
                    if (ld.mode == VoxelMode::terminal) { primary = &ld; break; }
                }
                if (!primary) primary = &layerConfig_->layers().front();
                const std::string& layerName = primary->name;
                double csz = static_cast<double>(primary->chunk_size_voxels) * primary->voxel_size_m;
                ChunkCoord edit_chunk{
                    static_cast<int>(std::floor(pos.value.x / csz)),
                    static_cast<int>(std::floor(pos.value.y / csz)),
                    static_cast<int>(std::floor(pos.value.z / csz))};
                const WorldCoord& peer_pos = pit->second;
                ChunkCoord peer_chunk{
                    static_cast<int>(std::floor(peer_pos.value.x / csz)),
                    static_cast<int>(std::floor(peer_pos.value.y / csz)),
                    static_cast<int>(std::floor(peer_pos.value.z / csz))};
                if (lodManager_)
                    send = lodManager_->withinViewDistance(peer_chunk, edit_chunk, layerName);
            }
        }
        // BroadcastAll: send == true (default, already set above).
        if (send) {
            transport_->send(peer_id, 0, buf.data(), buf.size(), Reliability::Reliable);
        } else {
            ++suppressedEdits_;
        }
    }
}

// ── send_network_message ───────────────────────────────────────────────────────

void NetworkManager::sendNetworkMessage(const MessageEnvelope& env)
{
    if (!transport_ || role_ == SessionRole::Offline) return;

    NetMessagePayload p;
    p.sender_id    = localPlayerId_;
    p.target       = static_cast<uint8_t>(env.target);
    p.target_player = env.target_player;
    p.reliability  = static_cast<uint8_t>(env.reliability);
    if (env.channel_id) p.channel_id = env.channel_id;
    if (env.payload && env.payload_size > 0) {
        const auto* bytes = static_cast<const uint8_t*>(env.payload);
        p.payload.assign(bytes, bytes + env.payload_size);
    }

    auto buf = encode_net_message(p);
    const int channel = (env.reliability == MessageReliability::Unreliable) ? 1 : 0;

    switch (env.target) {
        case MessageTarget::Broadcast:
            for (auto& [peer_id, player_id] : peerToPlayer_) {
                (void)player_id;
                transport_->send(peer_id, channel, buf.data(), buf.size(),
                                 (channel == 0) ? Reliability::Reliable : Reliability::Unreliable);
            }
            break;
        case MessageTarget::Server:
            // Send to the first connected peer (the server in a client session).
            for (auto& [peer_id, player_id] : peerToPlayer_) {
                transport_->send(peer_id, channel, buf.data(), buf.size(),
                                 (channel == 0) ? Reliability::Reliable : Reliability::Unreliable);
                break;
            }
            break;
        case MessageTarget::Player: {
            auto it = playerToPeer_.find(env.target_player);
            if (it != playerToPeer_.end()) {
                transport_->send(it->second, channel, buf.data(), buf.size(),
                                 (channel == 0) ? Reliability::Reliable : Reliability::Unreliable);
            }
            break;
        }
    }
}

// ── Internal dispatch ──────────────────────────────────────────────────────────

void NetworkManager::handlePacket(InboundPacket& pkt)
{
    switch (pkt.type) {
        case PacketType::PeerConnected:
            onPeerConnected(pkt.peer_id);
            break;
        case PacketType::PeerDisconnected:
            onPeerDisconnected(pkt.peer_id);
            break;
        case PacketType::Data:
            if (pkt.data.empty()) break;
            switch (static_cast<NetPacketKind>(pkt.data[0])) {
                case NetPacketKind::EditIntent:
                    handleEditIntent(pkt.peer_id, pkt);
                    break;
                case NetPacketKind::CommittedEdit:
                    handleCommittedEdit(pkt);
                    break;
                case NetPacketKind::NetMessage:
                    handleNetMessage(pkt);
                    break;
                case NetPacketKind::JoinResponse:
                    handleJoinResponse(pkt);
                    break;
                case NetPacketKind::DirtyChunkData:
                    handleDirtyChunkData(pkt);
                    break;
                case NetPacketKind::JoinComplete:
                    handleJoinComplete(pkt);
                    break;
                case NetPacketKind::ResyncRequest:
                    handleResyncRequest(pkt.peer_id, pkt);
                    break;
                default:
                    break;
            }
            break;
    }
}

void NetworkManager::handleEditIntent(PeerId sender_peer, const InboundPacket& pkt)
{
    // Only the authority node (Server/HostPeer) handles incoming edit intents.
    if (role_ != SessionRole::Server && role_ != SessionRole::HostPeer) return;

    EditIntentPayload intent;
    if (!decode_edit_intent(pkt.data, intent)) {
        Log::warn("[NetworkManager] malformed EditIntent packet");
        return;
    }

    auto it = peerToPlayer_.find(sender_peer);
    PlayerId source = (it != peerToPlayer_.end()) ? it->second : kInvalidPlayer;

    WorldCoord pos(intent.x, intent.y, intent.z);
    Voxel v;
    v.material.density              = intent.density;
    v.material.structural_strength  = intent.structural_strength;
    v.material.thermal_conductivity = intent.thermal_conductivity;
    v.material.porosity             = intent.porosity;
    v.material.hardness             = intent.hardness;
    v.material.palette_index        = intent.palette_index;

    // Route through the authority policies and on_edit_received, then commit.
    // Re-use applyEdit so the single choke-point invariant holds.
    applyEdit(source, pos, v);
}

void NetworkManager::handleCommittedEdit(const InboundPacket& pkt)
{
    // Only clients apply incoming committed edits directly (the authority already
    // committed its own copy via applyEdit).
    if (role_ != SessionRole::Client) return;
    if (!world_ || !pm_) return;

    CommittedEditPayload p;
    if (!decode_committed_edit(pkt.data, p)) {
        Log::warn("[NetworkManager] malformed CommittedEdit packet");
        return;
    }

    // Sequence-gap detection: if we miss more than kResyncGapThreshold edits,
    // request a full dirty-chunk resync from the server.
    static constexpr uint32_t kResyncGapThreshold = 32;
    if (lastAppliedSeq_ > 0 && p.seq > lastAppliedSeq_ + kResyncGapThreshold) {
        Log::warn("[NetworkManager] sequence gap detected — requesting resync");
        std::vector<uint8_t> resync_buf;
        write_u8(resync_buf, static_cast<uint8_t>(NetPacketKind::ResyncRequest));
        for (auto& [peer_id, player_id] : peerToPlayer_) {
            if (transport_) transport_->send(peer_id, 0, resync_buf.data(),
                                             resync_buf.size(), Reliability::Reliable);
            break;
        }
        lastAppliedSeq_ = p.seq;
    } else if (p.seq >= lastAppliedSeq_) {
        lastAppliedSeq_ = p.seq;
    }

    WorldCoord pos(p.x, p.y, p.z);
    Voxel v;
    v.material.density              = p.density;
    v.material.structural_strength  = p.structural_strength;
    v.material.thermal_conductivity = p.thermal_conductivity;
    v.material.porosity             = p.porosity;
    v.material.hardness             = p.hardness;
    v.material.palette_index        = p.palette_index;

    const Voxel old_voxel = world_->getVoxel(pos);
    world_->setVoxel(pos, v);

    PlayerId source = static_cast<PlayerId>(p.source_player);
    for (const auto& hook : pm_->voxelModifiedHooks()) {
        if (hook.fn) hook.fn(pos, &old_voxel, &v, source, hook.user_data);
    }
}

void NetworkManager::handleNetMessage(const InboundPacket& pkt)
{
    if (!pm_) return;

    NetMessagePayload p;
    if (!decode_net_message(pkt.data, p)) {
        Log::warn("[NetworkManager] malformed NetMessage packet");
        return;
    }

    if (role_ == SessionRole::Server || role_ == SessionRole::HostPeer) {
        // Authority: trust the connection, not the payload — stamp the sender id
        // from the peer the packet physically arrived on, so a peer cannot spoof
        // another player and every node downstream sees a consistent id.
        auto sender_it = peerToPlayer_.find(pkt.peer_id);
        if (sender_it != peerToPlayer_.end()) p.sender_id = sender_it->second;

        const auto rel = (static_cast<MessageReliability>(p.reliability) ==
                          MessageReliability::Unreliable)
                             ? Reliability::Unreliable : Reliability::Reliable;
        const int  channel = (rel == Reliability::Unreliable) ? 1 : 0;

        if (static_cast<MessageTarget>(p.target) == MessageTarget::Broadcast) {
            // Relay so a client broadcast reaches every other peer — clients are
            // only connected to the authority, never to each other.
            auto relay = encode_net_message(p);
            for (auto& [peer_id, player_id] : peerToPlayer_) {
                (void)player_id;
                if (peer_id == pkt.peer_id) continue;
                if (transport_)
                    transport_->send(peer_id, channel, relay.data(), relay.size(), rel);
            }
        } else if (static_cast<MessageTarget>(p.target) == MessageTarget::Player) {
            // Route a player-addressed message on to its destination peer; it is
            // not for the authority itself unless no such peer exists.
            auto dest = playerToPeer_.find(p.target_player);
            if (dest != playerToPeer_.end()) {
                auto fwd = encode_net_message(p);
                if (transport_)
                    transport_->send(dest->second, channel, fwd.data(), fwd.size(), rel);
                return;
            }
        }
    }

    // Build a transient MessageEnvelope backed by the decoded strings.
    MessageEnvelope env;
    env.channel_id    = p.channel_id.c_str();
    env.sender_id     = p.sender_id;
    env.target_player = p.target_player;
    env.target        = static_cast<MessageTarget>(p.target);
    env.reliability   = static_cast<MessageReliability>(p.reliability);
    env.payload       = p.payload.empty() ? nullptr : p.payload.data();
    env.payload_size  = p.payload.size();

    // Built-in: decode player position updates and store in peerPositions_.
    if (p.channel_id == "engine.player_position" && p.payload.size() >= 24) {
        size_t poff = 0;
        double px = read_f64(p.payload, poff);
        double py = read_f64(p.payload, poff);
        double pz = read_f64(p.payload, poff);
        peerPositions_[env.sender_id] = WorldCoord(px, py, pz);
    }

    for (const auto& hook : pm_->networkMessageHooks()) {
        if (hook.fn && !p.channel_id.empty() &&
            p.channel_id.substr(0, hook.channel_prefix.size()) == hook.channel_prefix)
        {
            hook.fn(&env, hook.user_data);
        }
    }
}

void NetworkManager::onPeerConnected(PeerId peer_id)
{
    PlayerId player_id = nextPlayerId_++;
    peerToPlayer_[peer_id] = player_id;
    playerToPeer_[player_id] = peer_id;
    std::cout << "[NetworkManager] peer " << peer_id
              << " connected → player " << player_id << '\n';

    if (role_ == SessionRole::Server || role_ == SessionRole::HostPeer) {
        sendHandshakeToPeer(peer_id);
        // on_player_joined fires after the client completes its side of the handshake;
        // on the server it fires immediately since the server is already live.
    }

    // Fire on_player_joined hooks.
    if (pm_) {
        WorldCoord initial_pos(0.0, 0.0, 0.0);
        for (const auto& hook : pm_->playerJoinedHooks()) {
            if (hook.fn) hook.fn(player_id, initial_pos, hook.user_data);
        }
    }
}

void NetworkManager::onPeerDisconnected(PeerId peer_id)
{
    auto it = peerToPlayer_.find(peer_id);
    PlayerId player_id = (it != peerToPlayer_.end()) ? it->second : kInvalidPlayer;
    std::cout << "[NetworkManager] peer " << peer_id
              << " disconnected (player " << player_id << ")\n";

    // Fire on_player_left hooks.
    if (pm_ && player_id != kInvalidPlayer) {
        for (const auto& hook : pm_->playerLeftHooks()) {
            if (hook.fn) hook.fn(player_id, hook.user_data);
        }
    }

    if (it != peerToPlayer_.end()) {
        playerToPeer_.erase(player_id);
        peerToPlayer_.erase(it);
    }
}

// ── World-state setters for handshake ─────────────────────────────────────────

void NetworkManager::setWorldSave(persistence::WorldSave* ws) { worldSave_ = ws; }
void NetworkManager::setWorldSeed(uint64_t seed)               { worldSeed_ = seed; }
void NetworkManager::setLayerConfig(const LayerConfig* config) {
    layerConfig_ = config;
    if (config) lodManager_ = std::make_unique<LODManager>(*config);
    else        lodManager_.reset();
}

// ── Streaming-radius interest management ──────────────────────────────────────

void NetworkManager::setInterestMode(InterestMode mode) { interestMode_ = mode; }

void NetworkManager::updatePeerPosition(PlayerId player_id, WorldCoord pos)
{
    peerPositions_[player_id] = pos;
}

// ── Player position replication ───────────────────────────────────────────────

const std::unordered_map<PlayerId, WorldCoord>& NetworkManager::playerPositions() const
{
    return peerPositions_;
}

uint32_t NetworkManager::rttMs(PlayerId player_id) const
{
    if (!transport_) return 0;
    auto it = playerToPeer_.find(player_id);
    if (it == playerToPeer_.end()) return 0;
    return transport_->roundTripTimeMs(it->second);
}

void NetworkManager::broadcastLocalPosition(WorldCoord pos)
{
    if (role_ == SessionRole::Offline || !transport_) return;

    // Encode as 24 bytes: x:f64 y:f64 z:f64
    std::vector<uint8_t> payload;
    payload.reserve(24);
    write_f64(payload, pos.value.x);
    write_f64(payload, pos.value.y);
    write_f64(payload, pos.value.z);

    MessageEnvelope env;
    env.channel_id    = "engine.player_position";
    env.sender_id     = localPlayerId_;
    env.target        = MessageTarget::Broadcast;
    env.target_player = 0;
    env.reliability   = MessageReliability::Unreliable;
    env.payload       = payload.data();
    env.payload_size  = payload.size();
    sendNetworkMessage(env);
}

// ── Join handshake — server side ──────────────────────────────────────────────

void NetworkManager::sendHandshakeToPeer(PeerId peer_id)
{
    if (!transport_) return;

    // 1. Send JoinResponse (world seed + LayerConfig).
    JoinResponsePayload jr;
    jr.world_seed = worldSeed_;
    if (layerConfig_) {
        jr.config_bytes = serializeLayerConfig(*layerConfig_);
    }
    auto jr_buf = encode_join_response(jr);
    transport_->send(peer_id, 0, jr_buf.data(), jr_buf.size(), Reliability::Reliable);

    // 2. Send dirty chunks if WorldSave is available.
    sendDirtyChunksToPeer(peer_id);

    // 3. Signal end of handshake with JoinComplete.
    std::vector<uint8_t> jc_buf;
    write_u8(jc_buf, static_cast<uint8_t>(NetPacketKind::JoinComplete));
    transport_->send(peer_id, 0, jc_buf.data(), jc_buf.size(), Reliability::Reliable);

    transport_->flush();
    std::cout << "[NetworkManager] handshake sent to peer " << peer_id << '\n';
}

void NetworkManager::sendDirtyChunksToPeer(PeerId peer_id)
{
    if (!worldSave_ || !transport_) return;

    // Read raw .vxc file bytes directly — avoids decode/re-encode and preserves
    // the original WorldIdentity so the client can validate on receive.
    const std::string& dir = worldSave_->directory();
    auto coords = worldSave_->listSavedChunks();
    for (const auto& coord : coords) {
        const std::string path = dir + "/c_" +
                                 std::to_string(coord.x) + "_" +
                                 std::to_string(coord.y) + "_" +
                                 std::to_string(coord.z) + ".vxc";
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) continue;
        auto sz = f.tellg();
        if (sz <= 0) continue;
        f.seekg(0);
        DirtyChunkDataPayload dcd;
        dcd.chunk_bytes.resize(static_cast<size_t>(sz));
        if (!f.read(reinterpret_cast<char*>(dcd.chunk_bytes.data()), sz)) continue;

        auto buf = encode_dirty_chunk_data(dcd);
        transport_->send(peer_id, 0, buf.data(), buf.size(), Reliability::Reliable);
    }
}

// ── Join handshake — client side ──────────────────────────────────────────────

void NetworkManager::handleJoinResponse(const InboundPacket& pkt)
{
    JoinResponsePayload p;
    if (!decode_join_response(pkt.data, p)) {
        Log::warn("[NetworkManager] malformed JoinResponse");
        return;
    }
    worldSeed_ = p.world_seed;
    std::cout << "[NetworkManager] join response received: seed=" << p.world_seed
              << " config_bytes=" << p.config_bytes.size() << '\n';
    // The demo is responsible for reinitialising its World from the received
    // config bytes (available via receivedConfigBytes_). For now we store them.
    receivedConfigBytes_ = std::move(p.config_bytes);
}

void NetworkManager::handleDirtyChunkData(const InboundPacket& pkt)
{
    DirtyChunkDataPayload p;
    if (!decode_dirty_chunk_data(pkt.data, p)) {
        Log::warn("[NetworkManager] malformed DirtyChunkData");
        return;
    }
    if (!world_ || p.chunk_bytes.empty()) return;

    // Use the permissive decode — the server sends raw .vxc bytes that already
    // have the correct WorldIdentity in their header; we accept any identity here
    // since the client may not yet have initialised its World to match.
    auto chunk = persistence::decodeChunkFilePermissive(
        p.chunk_bytes.data(), p.chunk_bytes.size());
    if (!chunk) {
        Log::warn("[NetworkManager] DirtyChunkData: could not decode chunk");
        return;
    }
    world_->insertChunk(std::move(chunk));
}

void NetworkManager::handleJoinComplete(const InboundPacket& /*pkt*/)
{
    joinComplete_ = true;
    std::cout << "[NetworkManager] join handshake complete\n";
    // Fire on_player_joined for the local player now that we have the world state.
    if (pm_) {
        WorldCoord initial_pos(0.0, 0.0, 0.0);
        for (const auto& hook : pm_->playerJoinedHooks()) {
            if (hook.fn) hook.fn(localPlayerId_, initial_pos, hook.user_data);
        }
    }
}

// ── Resync — client requests / server responds ────────────────────────────────

void NetworkManager::handleResyncRequest(PeerId sender_peer, const InboundPacket& /*pkt*/)
{
    if (role_ != SessionRole::Server && role_ != SessionRole::HostPeer) return;
    std::cout << "[NetworkManager] resync requested by peer " << sender_peer << '\n';
    sendDirtyChunksToPeer(sender_peer);
    // Re-send JoinComplete so the client knows the stream ended.
    std::vector<uint8_t> jc_buf;
    write_u8(jc_buf, static_cast<uint8_t>(NetPacketKind::JoinComplete));
    if (transport_)
        transport_->send(sender_peer, 0, jc_buf.data(), jc_buf.size(), Reliability::Reliable);
}

} // namespace net
