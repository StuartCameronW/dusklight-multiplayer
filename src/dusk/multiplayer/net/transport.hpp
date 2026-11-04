#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * Transport abstraction — moves opaque byte buffers over reliable/unreliable channels.
 *
 * Knows nothing about the game. This is the ONLY layer that changes when we swap ENet for
 * GameNetworkingSockets to get NAT traversal and encryption (see 06-networking.md), so keep
 * every ENet type behind this interface.
 */

namespace dusk::mp {

using PeerId = std::uint32_t;

inline constexpr PeerId kInvalidPeer = 0xFFFFFFFFu;

enum class TransportEventType {
    Connected,
    Disconnected,
    Data,
};

struct TransportEvent {
    TransportEventType type = TransportEventType::Data;
    PeerId peer = kInvalidPeer;
    std::vector<std::uint8_t> data;
    /// Disconnected only: the code the far end gave when it hung up (see DisconnectReason in
    /// packet.hpp), or 0 when it gave none, which is what a timeout produces.
    std::uint32_t reason = 0;
};

class ITransport {
public:
    virtual ~ITransport() = default;

    /// Open a listening socket (host role). Returns false if the port is unavailable.
    virtual bool listen(std::uint16_t port, std::size_t maxPeers) = 0;

    /// Begin connecting to a host (client role). Completion arrives as a Connected event.
    virtual bool connect(const std::string& address, std::uint16_t port) = 0;

    /// Non-blocking. Appends everything that arrived since the last call.
    virtual void poll(std::vector<TransportEvent>& out) = 0;

    virtual void send(PeerId peer, const std::uint8_t* data, std::size_t size, std::uint8_t channel,
        bool reliable) = 0;

    virtual void broadcast(
        const std::uint8_t* data, std::size_t size, std::uint8_t channel, bool reliable) = 0;

    /// Push queued outgoing packets onto the wire now, rather than at the next poll().
    virtual void flush() = 0;

    /// Smoothed round-trip time in milliseconds, or 0 if not yet known.
    virtual std::uint32_t rtt_ms(PeerId peer) const = 0;

    virtual std::size_t peer_count() const = 0;

    /// Hang up on one peer, telling it why. With @p i_afterQueued the packets already queued to it
    /// go out first, so a refusal can be explained before the line drops. A Disconnected event for
    /// the peer follows once the far end acknowledges (or the attempt times out).
    virtual void disconnect(PeerId peer, std::uint32_t reason, bool afterQueued) = 0;

    /// Hang up on everyone and close, blocking briefly so the far ends hear it rather than having
    /// to time out. No Disconnected events are produced: the caller is the one leaving.
    virtual void disconnect_all(std::uint32_t reason) = 0;
};

}  // namespace dusk::mp
