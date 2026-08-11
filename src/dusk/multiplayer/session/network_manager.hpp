#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../net/transport.hpp"
#include "dusk/multiplayer.hpp"

namespace dusk::mp {

enum class Role {
    Inactive,
    Host,
    Client,
};

/// Per-peer session state. Grows into the full peer record (puppet ProcID, ...) at M1/M2.
struct PeerSession {
    PeerId id = kInvalidPeer;
    /// World identity, assigned by the host from its own counter. Deliberately NOT the transport's
    /// PeerId: ENet's id space is per-host and starts at 0, which is reserved for the host itself,
    /// so reusing it would let a client collide with kHostPlayerId.
    std::uint32_t playerId = 0;
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
    /// Copy in what the command line asked for. Must happen before the first tick.
    void set_startup_options(const StartupOptions& options);

    /// Opens a session if the command line or the DUSK_MP_* environment variables ask for one,
    /// with the command line winning. Idempotent; called lazily from the first tick.
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
    std::uint32_t local_player_id() const { return mLocalPlayerId; }

private:
    void handle_event(const TransportEvent& event);
    void handle_packet(PeerId peer, const std::uint8_t* data, std::size_t size);
    void send_hello(PeerId peer);
    void send_heartbeats();
    void report_tick_rate();

    /// Host: tell a freshly-admitted peer about everyone already in the session, and tell everyone
    /// else about it. Reliable, because a missed roster update leaves a permanently invisible peer.
    void send_peer_list(PeerId peer);
    void broadcast_peer_joined(const PeerSession& joined);
    void broadcast_peer_left(std::uint32_t playerId);

    /// Client: our own pose, up to the host. Host: everyone's pose, down to every client.
    void send_local_state();
    void broadcast_snapshot();

    void report_interpolation();

    /// What --mp-* asked for, owned as strings so the caller's argv can go away.
    struct StartupRequest {
        bool host = false;
        std::string connect;
        std::uint16_t port = 0;
        std::string nickname;
        std::string color;
        std::string trace;
    };

    std::unique_ptr<ITransport> mTransport;
    Role mRole = Role::Inactive;
    bool mInitialized = false;
    StartupRequest mStartup;

    std::string mLocalNickname;
    std::uint32_t mLocalColor = 0xFFFFFF;
    /// True if the player chose a colour explicitly, so the host must not reassign it.
    bool mColorExplicit = false;
    /// 0 while hosting (the host reserves kHostPlayerId); set from HelloAck when joining.
    std::uint32_t mLocalPlayerId = 0;
    /// Host-side allocator for world identities. Starts at 1 so it can never hand out the host's.
    std::uint32_t mNextPlayerId = 1;

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
