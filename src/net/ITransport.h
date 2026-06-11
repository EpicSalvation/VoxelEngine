#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Abstract transport interface — the seam a plugin can replace to swap
// ENet for GameNetworkingSockets, yojimbo, or any other backend.
// No ENet types appear anywhere in this header (ARCHITECTURE §15).

namespace net {

using PeerId = uint32_t;
constexpr PeerId kInvalidPeer = 0;

enum class Reliability { Reliable, Unreliable };

// PacketType distinguishes data payloads from peer lifecycle events so
// NetworkManager can route both through a single poll() loop.
enum class PacketType { Data, PeerConnected, PeerDisconnected };

struct InboundPacket {
    PeerId               peer_id = kInvalidPeer;
    PacketType           type    = PacketType::Data;
    int                  channel = 0;  // meaningful only when type == Data
    std::vector<uint8_t> data;         // copied out of the transport's buffer
};

class ITransport {
public:
    virtual ~ITransport() = default;

    // Server mode: bind and start accepting connections.
    virtual bool listen(uint16_t port, int max_peers) = 0;

    // Client mode: initiate connection to a remote host.
    virtual bool connect(const std::string& host, uint16_t port) = 0;

    // Gracefully disconnect a peer. Safe to call with kInvalidPeer (no-op).
    virtual void disconnect(PeerId peer_id) = 0;

    // Send data to a peer on the given channel.
    // channel 0 → Reliable, channel 1 → Unreliable (ENet convention).
    virtual bool send(PeerId peer_id, int channel,
                      const void* data, size_t size,
                      Reliability reliability) = 0;

    // Service the transport and return one pending packet or event.
    // Returns true and fills *out when data or a peer event is available.
    // Returns false when the queue is empty.
    virtual bool poll(InboundPacket* out) = 0;

    // Flush any queued outbound packets to the wire.
    virtual void flush() = 0;

    virtual bool isListening() const = 0;
    virtual bool isConnected()  const = 0;
};

} // namespace net
