#include "net/NetworkManager.h"
#include "net/ITransport.h"
#include "net/ENetTransport.h"
#include "net/NetPackets.h"
#include "core/Logger.h"
#include "core/PluginManager.h"
#include "world/World.h"
#include "world/Voxel.h"

#include <iostream>
#include <string>

namespace net {

NetworkManager::NetworkManager() = default;
NetworkManager::~NetworkManager() { stop(); }

void NetworkManager::init(World& world, PluginManager& pm)
{
    world_ = &world;
    pm_    = &pm;

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
    std::cout << "[NetworkManager] client connecting to " << host << ':' << port << '\n';
    return true;
}

void NetworkManager::stop()
{
    if (role_ == SessionRole::Offline) return;
    // Drain and discard any queued packets, then shut down the transport.
    if (transport_) {
        InboundPacket pkt;
        while (transport_->poll(&pkt)) {}
        transport_->flush();
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

    // Interest filter: if registered, ask the plugin whether to send to each peer.
    const auto& filters = pm_->interestFilters();

    for (auto& [peer_id, player_id] : peerToPlayer_) {
        if (peer_id == exclude_peer) continue;

        // Apply the interest filter when registered.
        bool send = true;
        if (!filters.empty()) {
            send = filters[0].fn(player_id, pos, filters[0].user_data);
        }
        if (send) {
            transport_->send(peer_id, 0, buf.data(), buf.size(), Reliability::Reliable);
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

    // Build a transient MessageEnvelope backed by the decoded strings.
    MessageEnvelope env;
    env.channel_id    = p.channel_id.c_str();
    env.sender_id     = p.sender_id;
    env.target_player = p.target_player;
    env.target        = static_cast<MessageTarget>(p.target);
    env.reliability   = static_cast<MessageReliability>(p.reliability);
    env.payload       = p.payload.empty() ? nullptr : p.payload.data();
    env.payload_size  = p.payload.size();

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

} // namespace net
