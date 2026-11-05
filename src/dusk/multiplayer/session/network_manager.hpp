#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../net/packet.hpp"
#include "../net/transport.hpp"
#include "../replication/player_state.hpp"
#include "dusk/multiplayer.hpp"
#include "mod_check.hpp"

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
    /// Host: we have refused this peer and are waiting for the hang-up to land. Nothing it sends
    /// is acted on from here, and it is never refused twice.
    bool refused = false;
    std::uint32_t lastRttMs = 0;
    /// When the transport connected, so a peer that never says Hello can be timed out.
    std::chrono::steady_clock::time_point connectedAt{};
    /// Last time ANY packet arrived from this peer. Surfaced in the status so a stalling player
    /// reads as "not responding" seconds before the transport gives up on them.
    std::chrono::steady_clock::time_point lastHeard{};
};

/// One player as the UI should show it. Built fresh by NetworkManager::status(); holds no state.
struct PlayerStatus {
    std::uint32_t playerId = 0;
    std::string nickname;
    std::uint32_t color = 0xFFFFFF;
    /// True for a peer this machine has its own connection to: every client, on the host; the host,
    /// on a client. Other clients are reached through the host, so a client has no RTT for them.
    bool direct = false;
    bool handshakeComplete = false;
    /// ENet's smoothed round-trip time. 0 when not direct or not yet measured.
    std::uint32_t rttMs = 0;
    /// Milliseconds since anything arrived from this peer. Only meaningful when direct.
    std::uint32_t silentMs = 0;
};

/// A read-only picture of the session for UI. Cheap to build; call it per frame if need be.
struct SessionStatus {
    Role role = Role::Inactive;
    /// Client: the handshake with the host completed. Host: always true while hosting.
    bool connected = false;
    /// Client: "address:port" we joined. Host: the UDP port we listen on, as text.
    std::string endpoint;
    std::string localNickname;
    std::uint32_t localColor = 0xFFFFFF;
    std::uint32_t localPlayerId = 0;
    /// Everyone else, host first on a client.
    std::vector<PlayerStatus> players;
    /// Why the last session ended, in words meant for the player. Empty if none has.
    std::string lastEndMessage;
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

    /// The session object outlives the game loop, and its destructor is what hangs up cleanly at
    /// process exit. It deliberately touches nothing but the transport: by then the actors, heaps
    /// and the other singletons may already be gone.
    ~NetworkManager();

    /// End the session immediately: hang up on everyone, despawn every puppet, release the world
    /// clock. Must run on the game thread outside the actor pass. Save stays disabled on a guest
    /// regardless (save_guard.hpp) — nothing here touches that flag.
    void shutdown();

    /// Leave (client) or end (host) the session, from UI. Deferred to the next pre_actor_tick, so
    /// a button press can never destroy puppet actors in the middle of a frame.
    void leave_session() { mLeaveRequested = is_active(); }

    SessionStatus status() const;

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
    void handle_disconnect(PeerId peer, std::uint32_t reason);

    /// Client: the session is over, whatever ended it. Tears down to a clean single-player state
    /// and tells the player @p title / @p message, which is also kept for the status page.
    void end_client_session(const std::string& title, const std::string& message);

    /// Host: refuse a peer. Explains with @p reason in the hang-up, after anything already queued
    /// to it (a HelloAck saying no) has gone out.
    void refuse_peer(PeerSession& session, DisconnectReason reason);

    /// Time out connections that are not getting anywhere: a client still waiting on its host, a
    /// host holding a peer that never introduced itself.
    void check_handshake_deadlines();
    std::string player_name(std::uint32_t playerId) const;
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

    /// Client: forward a locally-set world clock to the host, if there is one pending. See
    /// replication/world_clock.hpp for why a shared clock cannot be a one-way broadcast.
    void send_time_override();

    /// The pose to put on the wire this tick, which is a different question from "is there a Link
    /// to read". Never fails: see the definition for why silence is not an option.
    PlayerState wire_local_state();

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
    bool mLeaveRequested = false;

    /// Client: when join() started, and whether the host has accepted us yet.
    std::chrono::steady_clock::time_point mJoinStarted{};
    bool mJoined = false;
    std::string mEndpoint;
    std::string mHostNickname;
    std::string mLastEndMessage;
    /// Client: the mod list we sent in our Hello, kept so a refusal can be explained against
    /// exactly what the host saw rather than a list that changed in the meantime.
    ModFingerprint mSentMods;

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

    /// Last pose we could actually read off Link, resent while there is none. See
    /// wire_local_state().
    PlayerState mLastLocalState;
    bool mHaveLastLocal = false;

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
