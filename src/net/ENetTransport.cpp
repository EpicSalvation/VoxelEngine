#include "net/ENetTransport.h"

// ENet headers are confined to this single translation unit.
// Nothing else in the engine may include <enet/enet.h> (ARCHITECTURE §15).
#include <enet/enet.h>

#include <cstring>
#include <queue>
#include <unordered_map>
#include <iostream>

namespace net {

// ── PIMPL body ────────────────────────────────────────────────────────────────

struct ENetTransport::Impl {
    ENetHost* host = nullptr;

    // Bidirectional mapping: our stable PeerId ↔ ENetPeer*.
    // PeerId 0 == kInvalidPeer is reserved.
    std::unordered_map<ENetPeer*, PeerId> peerToId;
    std::unordered_map<PeerId, ENetPeer*> idToPeer;
    PeerId nextPeerId = 1;

    std::queue<InboundPacket> incoming;

    bool listening  = false;
    bool hasClient  = false; // true once a client-mode connection attempt is made

    PeerId assignPeer(ENetPeer* peer) {
        PeerId id = nextPeerId++;
        peerToId[peer]  = id;
        idToPeer[id]    = peer;
        return id;
    }

    void removePeer(ENetPeer* peer) {
        auto it = peerToId.find(peer);
        if (it == peerToId.end()) return;
        PeerId id = it->second;
        idToPeer.erase(id);
        peerToId.erase(it);
    }

    // Service the ENet host (up to max_events per call) and queue results.
    void service(int max_events = 64, int timeout_ms = 0) {
        if (!host) return;
        ENetEvent ev;
        for (int i = 0; i < max_events; ++i) {
            int result = enet_host_service(host, &ev, timeout_ms);
            if (result <= 0) break;  // 0 = nothing, < 0 = error

            switch (ev.type) {
                case ENET_EVENT_TYPE_CONNECT: {
                    PeerId id = assignPeer(ev.peer);
                    ev.peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
                    InboundPacket pkt;
                    pkt.peer_id = id;
                    pkt.type    = PacketType::PeerConnected;
                    incoming.push(std::move(pkt));
                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT: {
                    auto it = peerToId.find(ev.peer);
                    if (it != peerToId.end()) {
                        InboundPacket pkt;
                        pkt.peer_id = it->second;
                        pkt.type    = PacketType::PeerDisconnected;
                        incoming.push(std::move(pkt));
                        removePeer(ev.peer);
                    }
                    break;
                }
                case ENET_EVENT_TYPE_RECEIVE: {
                    auto it = peerToId.find(ev.peer);
                    if (it != peerToId.end() && ev.packet) {
                        InboundPacket pkt;
                        pkt.peer_id = it->second;
                        pkt.type    = PacketType::Data;
                        pkt.channel = static_cast<int>(ev.channelID);
                        pkt.data.assign(ev.packet->data,
                                        ev.packet->data + ev.packet->dataLength);
                        incoming.push(std::move(pkt));
                    }
                    if (ev.packet) enet_packet_destroy(ev.packet);
                    break;
                }
                default:
                    break;
            }
            timeout_ms = 0;  // only block on the first event; drain the rest
        }
    }
};

// ── Lifecycle ──────────────────────────────────────────────────────────────────

ENetTransport::ENetTransport()
    : impl_(std::make_unique<Impl>())
{
    if (enet_initialize() != 0) {
        std::cerr << "[ENetTransport] enet_initialize() failed\n";
    }
}

ENetTransport::~ENetTransport()
{
    if (impl_->host) {
        enet_host_destroy(impl_->host);
        impl_->host = nullptr;
    }
    enet_deinitialize();
}

// ── ITransport implementation ──────────────────────────────────────────────────

bool ENetTransport::listen(uint16_t port, int max_peers)
{
    if (impl_->host) {
        std::cerr << "[ENetTransport] already initialised\n";
        return false;
    }
    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = port;
    impl_->host = enet_host_create(&addr, static_cast<size_t>(max_peers),
                                   2 /*channels*/, 0, 0);
    if (!impl_->host) {
        std::cerr << "[ENetTransport] enet_host_create (server) failed\n";
        return false;
    }
    impl_->listening = true;
    return true;
}

bool ENetTransport::connect(const std::string& host, uint16_t port)
{
    if (impl_->host) {
        std::cerr << "[ENetTransport] already initialised\n";
        return false;
    }
    // Client host: no bound address, 1 outbound peer, 2 channels.
    impl_->host = enet_host_create(nullptr, 1, 2, 0, 0);
    if (!impl_->host) {
        std::cerr << "[ENetTransport] enet_host_create (client) failed\n";
        return false;
    }

    ENetAddress addr;
    enet_address_set_host(&addr, host.c_str());
    addr.port = port;

    ENetPeer* peer = enet_host_connect(impl_->host, &addr, 2, 0);
    if (!peer) {
        std::cerr << "[ENetTransport] enet_host_connect failed\n";
        enet_host_destroy(impl_->host);
        impl_->host = nullptr;
        return false;
    }
    impl_->hasClient = true;
    return true;
}

void ENetTransport::disconnect(PeerId peer_id)
{
    auto it = impl_->idToPeer.find(peer_id);
    if (it == impl_->idToPeer.end()) return;
    enet_peer_disconnect(it->second, 0);
}

bool ENetTransport::send(PeerId peer_id, int channel,
                         const void* data, size_t size,
                         Reliability reliability)
{
    auto it = impl_->idToPeer.find(peer_id);
    if (it == impl_->idToPeer.end()) return false;

    uint32_t flags = (reliability == Reliability::Reliable)
                     ? ENET_PACKET_FLAG_RELIABLE
                     : 0;
    ENetPacket* pkt = enet_packet_create(data, size, flags);
    if (!pkt) return false;

    // ENet channels: 0 = reliable, 1 = unreliable.
    enet_uint8 ch = static_cast<enet_uint8>(channel & 0xFF);
    return enet_peer_send(it->second, ch, pkt) == 0;
}

bool ENetTransport::poll(InboundPacket* out)
{
    if (!impl_->host) return false;
    // Service ENet (non-blocking) to refresh the queue, then pop one item.
    impl_->service(64, 0);
    if (impl_->incoming.empty()) return false;
    *out = std::move(impl_->incoming.front());
    impl_->incoming.pop();
    return true;
}

void ENetTransport::flush()
{
    if (impl_->host) enet_host_flush(impl_->host);
}

bool ENetTransport::isListening() const { return impl_->listening; }
bool ENetTransport::isConnected()  const { return !impl_->idToPeer.empty(); }

uint32_t ENetTransport::roundTripTimeMs(PeerId peer_id) const
{
    auto it = impl_->idToPeer.find(peer_id);
    if (it == impl_->idToPeer.end()) return 0;
    return static_cast<uint32_t>(it->second->roundTripTime);
}

} // namespace net
