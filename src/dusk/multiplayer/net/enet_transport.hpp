#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "transport.hpp"

struct _ENetHost;
struct _ENetPeer;

namespace dusk::mp {

/// ENet implementation of ITransport. See 06-networking.md for why ENet first.
class EnetTransport final : public ITransport {
public:
    EnetTransport();
    ~EnetTransport() override;

    EnetTransport(const EnetTransport&) = delete;
    EnetTransport& operator=(const EnetTransport&) = delete;

    bool listen(std::uint16_t port, std::size_t maxPeers) override;
    bool connect(const std::string& address, std::uint16_t port) override;
    void poll(std::vector<TransportEvent>& out) override;
    void send(PeerId peer, const std::uint8_t* data, std::size_t size, std::uint8_t channel,
        bool reliable) override;
    void broadcast(
        const std::uint8_t* data, std::size_t size, std::uint8_t channel, bool reliable) override;
    void flush() override;
    std::uint32_t rtt_ms(PeerId peer) const override;
    std::size_t peer_count() const override;
    void disconnect_all() override;

private:
    /// Stable ids handed out to callers, so nothing above this layer ever holds an ENetPeer*.
    PeerId acquire_id(_ENetPeer* peer);
    _ENetPeer* lookup(PeerId id) const;

    _ENetHost* mHost = nullptr;
    std::unordered_map<PeerId, _ENetPeer*> mPeers;
    PeerId mNextPeerId = 1;
};

}  // namespace dusk::mp
