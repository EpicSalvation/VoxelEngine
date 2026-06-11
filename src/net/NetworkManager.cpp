#include "net/NetworkManager.h"
#include "net/ITransport.h"
#include "net/ENetTransport.h"
#include "core/Logger.h"

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
            // Application-level dispatch will be wired in the Plugin API surface
            // and handshake tasks (M11 §"plugin_api.h additions" / §"handshake").
            break;
    }
}

void NetworkManager::onPeerConnected(PeerId peer_id)
{
    PlayerId player_id = nextPlayerId_++;
    peerToPlayer_[peer_id] = player_id;
    std::cout << "[NetworkManager] peer " << peer_id
              << " connected → player " << player_id << '\n';
    // on_player_joined hook fires here once the Plugin API surface task lands.
}

void NetworkManager::onPeerDisconnected(PeerId peer_id)
{
    auto it = peerToPlayer_.find(peer_id);
    PlayerId player_id = (it != peerToPlayer_.end()) ? it->second : kInvalidPlayer;
    std::cout << "[NetworkManager] peer " << peer_id
              << " disconnected (player " << player_id << ")\n";
    // on_player_left hook fires here once the Plugin API surface task lands.
    if (it != peerToPlayer_.end()) peerToPlayer_.erase(it);
}

} // namespace net
