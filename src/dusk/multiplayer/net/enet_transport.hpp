#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "stun.hpp"
#include "transport.hpp"

struct _ENetHost;
struct _ENetPeer;
struct _ENetEvent;

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
    void disconnect(PeerId peer, std::uint32_t reason, bool afterQueued) override;
    void drop(PeerId peer, std::uint32_t reason) override;
    void disconnect_all(std::uint32_t reason) override;

    bool open_client() override;
    void begin_stun(const std::string& server, std::uint16_t port, int timeoutMs) override;
    StunState stun_state(Endpoint* outPublic) const override;
    void punch(const Endpoint& to, int durationMs) override;
    std::vector<Endpoint> lan_endpoints() const override;

private:
    using Clock = std::chrono::steady_clock;

    /// IPv4 in network byte order + port in host byte order, i.e. ENet's ENetAddress, kept here
    /// without the ENet header.
    struct RawAddress {
        std::uint32_t host = 0;
        std::uint16_t port = 0;
    };

    struct StunJob {
        StunState state = StunState::Idle;
        stun::TransactionId tid{};
        RawAddress primary;
        bool primaryResolved = false;
        RawAddress fallback;
        bool fallbackResolved = false;
        Clock::time_point start;
        Clock::time_point fallbackAt;
        Clock::time_point deadline;
        Clock::time_point nextSend;
        int sent = 0;
        Endpoint result;
    };

    struct PunchTarget {
        RawAddress address;
        Endpoint endpoint;
        Clock::time_point until;
        Clock::time_point nextSend;
        int sent = 0;
    };

    /// Hook this transport's host into ENet's raw-datagram intercept. Called whenever mHost is set.
    void install_intercept();
    /// ENet's intercept callback. Consumes STUN replies and DUSKPUNCH datagrams (returns 1);
    /// everything else returns 0 so ENet parses it as usual.
    static int intercept_raw(_ENetHost* host, _ENetEvent* event);
    bool on_raw_datagram(const std::uint8_t* data, std::size_t size);
    /// Fire a datagram from ENet's socket, bypassing ENet's protocol. False if the send failed.
    bool send_raw(const RawAddress& to, const std::uint8_t* data, std::size_t size) const;
    void tick_stun(Clock::time_point now);
    void tick_punch(Clock::time_point now);

    /// Stable ids handed out to callers, so nothing above this layer ever holds an ENetPeer*.
    PeerId acquire_id(_ENetPeer* peer);
    _ENetPeer* lookup(PeerId id) const;
    /// Apply the session's timeout policy. See kPeerTimeout* in enet_transport.cpp.
    static void apply_timeouts(_ENetPeer* peer);

    _ENetHost* mHost = nullptr;
    std::unordered_map<PeerId, _ENetPeer*> mPeers;
    PeerId mNextPeerId = 1;

    /// open_client() created mHost and connect() has not used it yet.
    bool mClientOpen = false;
    bool mConnectIssued = false;
    /// The outgoing connection made from an open_client() socket. Any other incoming CONNECT on
    /// that socket is a stranger who found the port (it is public once STUN has run) and is reset.
    _ENetPeer* mClientPeer = nullptr;
    bool mPunchReceivedLogged = false;
    StunJob mStun;
    std::vector<PunchTarget> mPunches;
};

}  // namespace dusk::mp
