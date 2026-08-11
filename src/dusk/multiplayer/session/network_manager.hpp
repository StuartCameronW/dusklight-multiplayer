#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../net/transport.hpp"

namespace dusk::mp {

enum class Role {
    Inactive,
    Host,
    Client,
};

/// Per-peer session state. Grows into the full peer record (puppet ProcID, ...) at M1/M2.
struct PeerSession {
    PeerId id = kInvalidPeer;
    std::string nickname;
    /// 0xRRGGBB tint used to tell players apart, borrowed from tpmp's name+colour idea (08).
    std::uint32_t color = 0xFFFFFF;
    bool handshakeComplete = false;
    std::uint32_t lastRttMs = 0;
};

/**
 * Top-level session object: owns the transport and is pumped explicitly from the game loop.
 *
 * Deliberately NOT an fpc actor/process — it has to run BOTH before and after the actor pass,
 * which the process framework can't express (see "Why not alternatives" in 04-architecture.md).
 *
 * Single-threaded by design: ENet is poll-based, so everything runs on the game thread inside
 * the two tick hooks. No locking, and no risk of touching game state off-thread.
 */
class NetworkManager {
public:
    /// Reads DUSK_MP_* environment variables and opens a session if they ask for one.
    /// Idempotent; called lazily from the first tick.
    void ensure_initialized();

    void shutdown();

    bool host(std::uint16_t port);
    bool join(const std::string& address, std::uint16_t port);

    /// Drain the transport and apply inbound state. Called before the actor pass.
    void pre_actor_tick();

    /// Capture and send local state, then flush. Called after the actor pass.
    void post_actor_tick();

    bool is_active() const { return mRole != Role::Inactive; }
    Role role() const { return mRole; }
    std::uint64_t sim_tick() const { return mSimTick; }
    const std::unordered_map<PeerId, PeerSession>& peers() const { return mPeers; }
    const std::string& local_nickname() const { return mLocalNickname; }
    std::uint32_t local_color() const { return mLocalColor; }

private:
    void handle_event(const TransportEvent& event);
    void handle_packet(PeerId peer, const std::uint8_t* data, std::size_t size);
    void send_hello(PeerId peer);
    void send_heartbeats();
    void report_tick_rate();

    std::unique_ptr<ITransport> mTransport;
    Role mRole = Role::Inactive;
    bool mInitialized = false;

    std::string mLocalNickname;
    std::uint32_t mLocalColor = 0xFFFFFF;
    /// True if the player chose a colour explicitly, so the host must not reassign it.
    bool mColorExplicit = false;

    std::unordered_map<PeerId, PeerSession> mPeers;
    std::vector<TransportEvent> mEventScratch;

    std::uint64_t mSimTick = 0;
    std::uint32_t mHeartbeatSeq = 0;
    std::unordered_map<std::uint32_t, std::chrono::steady_clock::time_point> mPendingHeartbeats;

    /// For proving the pump really runs at the 30 Hz sim rate rather than the render rate.
    std::chrono::steady_clock::time_point mRateWindowStart;
    std::uint64_t mRateWindowStartTick = 0;
};

/// The process-wide session.
NetworkManager& network_manager();

}  // namespace dusk::mp
