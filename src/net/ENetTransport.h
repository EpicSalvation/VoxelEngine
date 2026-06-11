#pragma once

#include "net/ITransport.h"
#include <memory>
#include <unordered_map>
#include <queue>

// ENet implementation of ITransport.
// ENet headers are included only in ENetTransport.cpp; no ENet types appear here.

namespace net {

class ENetTransport : public ITransport {
public:
    ENetTransport();
    ~ENetTransport() override;

    // Non-copyable; unique resource (ENetHost).
    ENetTransport(const ENetTransport&)            = delete;
    ENetTransport& operator=(const ENetTransport&) = delete;

    bool listen(uint16_t port, int max_peers) override;
    bool connect(const std::string& host, uint16_t port) override;
    void disconnect(PeerId peer_id) override;
    bool send(PeerId peer_id, int channel,
              const void* data, size_t size,
              Reliability reliability) override;
    bool poll(InboundPacket* out) override;
    void flush() override;
    bool isListening() const override;
    bool isConnected()  const override;
    uint32_t roundTripTimeMs(PeerId peer_id) const override;

private:
    // PIMPL: ENet types are confined to the .cpp.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace net
