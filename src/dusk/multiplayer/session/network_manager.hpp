#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../net/packet.hpp"
#include "../net/serializer.hpp"
#include "../net/transport.hpp"
#include "../rendezvous/rendezvous_client.hpp"
#include "../replication/player_state.hpp"
#include "dusk/multiplayer.hpp"
#include "mod_check.hpp"

namespace dusk::mp {

/// "No player": the default for send_to_session's exception.
inline constexpr std::uint32_t kNoPlayerId = 0xFFFFFFFFu;

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
    /// Host: the admitted peer's latest mod fingerprint (Hello, then each ModsUpdate), so a change
    /// on EITHER end can be re-checked without asking again.
    ModFingerprint mods;
    std::uint32_t lastRttMs = 0;
    /// When the transport connected, so a peer that never says Hello can be timed out.
    std::chrono::steady_clock::time_point connectedAt{};
    /// Last time ANY packet arrived from this peer. Surfaced in the status so a stalling player
    /// reads as "not responding" seconds before the transport gives up on them.
    std::chrono::steady_clock::time_point lastHeard{};
    /// NetworkManager::mAttentiveTime when lastHeard was last set: the silence watchdog's clock,
    /// which does not run while this process itself is stalled. See check_peer_silence().
    std::chrono::steady_clock::duration heardAtAttentive{};

    /// v28, host: a join-code guest whose Hello checked out (version, mods, room, token), held
    /// before its HelloAck until the host presses Allow or Deny. Nothing of the world is sent to
    /// it meanwhile. nickname and mods above already hold what its Hello said.
    bool awaitingApproval = false;
    std::chrono::steady_clock::time_point approvalAskedAt{};
    /// Set from the UI (answer_join_request), acted on in the next pre_actor_tick: 0 undecided,
    /// 1 allow, 2 deny.
    std::uint8_t approvalDecision = 0;
    /// The rendezvous joinId behind this guest's token, so a Deny can give the room slot back.
    std::string rdvJoinId;
    std::uint8_t helloColorExplicit = 0;
    std::uint32_t helloPreferredColor = 0;
};

/// Host: one join-code guest waiting for Allow / Deny.
struct JoinApproval {
    PeerId peer = kInvalidPeer;
    std::string nickname;
    /// Until the request is declined on its own.
    std::uint32_t secondsLeft = 0;
};

/// Host side of hosting with a join code. Shared with the thread doing the UPnP request.
struct UpnpJob;

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
    /// Internet play, host: the join code as "ABC-123" once the rendezvous issued one, else empty.
    std::string roomCode;
    /// Internet play: where getting (host) or using (guest) a join code has got to, in words for
    /// the player. Empty when no join code is involved.
    std::string internetStatus;
    /// Host: join-code guests waiting for Allow / Deny.
    std::vector<JoinApproval> approvals;
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
    /// @p reason is what everyone else is told in the hang-up.
    void shutdown(DisconnectReason reason = DisconnectReason::Left);

    /// Leave (client) or end (host) the session, from UI. Deferred to the next pre_actor_tick, so
    /// a button press can never destroy puppet actors in the middle of a frame.
    void leave_session() { mLeaveRequested = is_active(); }

    /// Batch 7 (death rule H2, mp-death-rule.md): this machine's save memory was reset or replaced
    /// after the session had a world (back to the title, a soft reset, a save reloaded). Host:
    /// end the session for everyone (DisconnectReason::SaveReset) rather than send them a
    /// different world. Guest: leave. Deferred like leave_session(), so it never runs inside the
    /// game's own reset code.
    void leave_session_for_save_reset() {
        if (is_active()) {
            mLeaveRequested = true;
            mLeaveForSaveReset = true;
        }
    }

    SessionStatus status() const;

    bool host(std::uint16_t port);
    bool join(const std::string& address, std::uint16_t port);

    /// Internet play (mp-internet-play.md §3): host() and then, in the background, learn this
    /// machine's addresses (LAN, STUN, UPnP) and register them with the rendezvous for a join
    /// code. Progress and the code are in status().
    bool host_with_code(std::uint16_t port);
    /// Internet play: look @p code up at the rendezvous and connect to the host it names, LAN
    /// addresses first. Every step after the first is asynchronous; failures end the session with
    /// a message, like a direct join.
    bool join_by_code(const std::string& code);
    /// Host, from UI: Allow (true) or Deny a join-code guest from status().approvals. Acted on in
    /// the next pre_actor_tick, so the button press never spawns a puppet mid-frame.
    void answer_join_request(PeerId peer, bool allow);

    /// Drain the transport and apply inbound state. Called before the actor pass.
    void pre_actor_tick();

    /// Capture and send local state, then flush. Called after the actor pass.
    void post_actor_tick();

    bool is_active() const { return mRole != Role::Inactive; }
    /// Host: always while hosting. Client: only once the host has accepted us (HelloAck). Nothing
    /// of the host's world may be applied, or anything of ours sent for it, before that.
    bool session_live() const { return mRole == Role::Host || (mRole == Role::Client && mJoined); }
    Role role() const { return mRole; }

    /**
     * Send a ready-made packet (its first byte is the PacketId) to one player by world identity.
     * Only admitted, unrefused peers are reachable; a client can reach only the host
     * (kHostPlayerId). Does nothing if that player is not connected. For the session modules
     * (world_sync, presence, object_channel), which own their payloads but not the transport.
     */
    void send_to_player(
        std::uint32_t playerId, const Writer& w, std::uint8_t channel, bool reliable);

    /// Send a ready-made packet to every admitted peer except @p exceptPlayerId. On a client that
    /// is just the host; on the host, every guest.
    void send_to_session(const Writer& w, std::uint8_t channel, bool reliable,
        std::uint32_t exceptPlayerId = kNoPlayerId);
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

    /// Drop any connected peer that has sent nothing for kPeerSilenceTimeoutMs, through the same
    /// path as a transport timeout. ENet's own timeout overshoots (enet_transport.cpp); this does
    /// not.
    void check_peer_silence();
    std::string player_name(std::uint32_t playerId) const;
    void handle_packet(PeerId peer, const std::uint8_t* data, std::size_t size);
    void send_hello(PeerId peer);
    void send_hello_ack(PeerId peer, bool accepted, std::uint32_t playerId,
        std::uint32_t assignedColor, const ModFingerprint& ourMods);
    /// Host: let in a peer whose Hello checked out. Its nickname, colour request and mods are
    /// already on the session.
    void admit_peer(PeerId peer, const ModFingerprint& ourMods);

    /// Client: what join() and join_by_code() both do once they have a transport.
    void begin_client_session(std::unique_ptr<ITransport> transport, std::string endpoint);

    // --- Internet play (mp-internet-play.md §3-§6) ---
    /// Host: gather candidates, get the code, take join requests and punch toward them.
    void tick_host_code();
    /// Host: act on Allow / Deny, and decline requests nobody answered.
    void tick_join_approvals();
    /// Client: STUN, then the rendezvous, then the host's candidates one by one.
    void tick_code_join();
    void start_code_join_attempt();
    void fail_code_join(const std::string& message);
    /// Both: stop talking to the rendezvous and release the UPnP mapping, off the game thread.
    void teardown_internet();
    /// Rebuild this end's mod fingerprint every kModRecheckTicks and, if it changed since the
    /// handshake (a save loaded and so a randomizer seed became known, or a mod was toggled), tell
    /// the other end. The host also re-checks every admitted guest against its new list.
    void check_mod_changes();
    void send_mods_update(PeerId peer, const ModFingerprint& fp);
    /// Host: compare an admitted guest's list with ours and remove it on a gameplay difference.
    /// Returns false if the guest was removed.
    bool recheck_peer_mods(PeerSession& session, const ModFingerprint& ours);
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

    /// v22: this end's Presence, when it changed (session/presence.hpp). Client: up to the host.
    /// Host: down to every guest. A guest's presence reaches the others by relay (handle_packet).
    void send_presence();

    /// Host: everyone's last Presence (the newcomer's own excepted) to a newly admitted peer, so it
    /// knows where everyone is before they next move.
    void send_cached_presence(PeerId peer, std::uint32_t newcomer);

    /// Host: forward a packet exactly as it arrived to every admitted peer but @p from.
    void relay_to_others(PeerId from, const std::uint8_t* data, std::size_t size,
        std::uint8_t channel, bool reliable);

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
        bool hostWithCode = false;
        std::string joinCode;
    };

    /// Host side of a join code. Inactive for a plain host().
    struct HostCodeState {
        enum class Phase { Off, Gathering, Creating, Ready, Failed };
        Phase phase = Phase::Off;
        std::chrono::steady_clock::time_point startedAt{};
        std::uint16_t port = 0;
        /// Null when UPnP is off or not built for this platform.
        std::shared_ptr<UpnpJob> upnp;
        /// The router forwards our port, so a guest reaches us without a hole punch.
        bool upnpMapped = false;
        std::unique_ptr<rdv::Client> rdv;
        /// The six characters, no dash.
        std::string code;
        std::string status;
        struct PendingJoin {
            std::string name;
            std::string joinId;
            std::chrono::steady_clock::time_point expires{};
        };
        /// By session token: guests the rendezvous announced whose Hello has not arrived yet.
        std::unordered_map<std::string, PendingJoin> pendingJoins;
    };

    /// Client side of a join by code. Phase None for a plain join().
    struct CodeJoinState {
        enum class Phase { None, Stun, Rendezvous, Connecting, Connected };
        Phase phase = Phase::None;
        std::string code;
        /// From the rendezvous; goes in our Hello. Never logged beyond 4 characters.
        std::string token;
        std::unique_ptr<rdv::Client> rdv;
        /// The open_client() socket STUN measured, parked while a LAN attempt uses its own, so the
        /// first public attempt can still go out through the NAT mapping the host punches toward.
        std::unique_ptr<ITransport> stunTransport;
        bool stunSocketUsed = false;
        /// The host's candidates, LAN first.
        std::vector<rdv::Candidate> hostCandidates;
        std::size_t attempt = 0;
        bool attemptFailed = false;
        std::chrono::steady_clock::time_point startedAt{};
        std::chrono::steady_clock::time_point phaseStartedAt{};
        std::string status;
    };

    std::unique_ptr<ITransport> mTransport;
    HostCodeState mHostCode;
    CodeJoinState mCodeJoin;
    Role mRole = Role::Inactive;
    bool mInitialized = false;
    StartupRequest mStartup;
    bool mLeaveRequested = false;
    /// The pending leave is leave_session_for_save_reset()'s, not the player's own choice.
    bool mLeaveForSaveReset = false;

    /// Client: when join() started, and whether the host has accepted us yet.
    std::chrono::steady_clock::time_point mJoinStarted{};
    bool mJoined = false;
    std::string mEndpoint;
    std::string mHostNickname;
    std::string mLastEndMessage;
    /// The mod list we last told the other end(s): the Hello (client) or the start of hosting
    /// (host), then each ModsUpdate. A refusal is explained against exactly what the other end saw.
    ModFingerprint mSentMods;
    /// Client: the host's list, from HelloAck and each ModsUpdate, so being removed mid-session
    /// can name what differs.
    ModFingerprint mHostMods;
    bool mHaveHostMods = false;

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

    /// Host: the last Presence packet (PacketId byte included) from each player, its own under
    /// kHostPlayerId. Replayed to each newcomer; see send_cached_presence().
    std::unordered_map<std::uint32_t, std::vector<std::uint8_t>> mLastPresence;

    /// Last pose we could actually read off Link, resent while there is none. See
    /// wire_local_state().
    PlayerState mLastLocalState;
    bool mHaveLastLocal = false;

    std::uint64_t mSimTick = 0;
    std::uint32_t mHeartbeatSeq = 0;
    std::unordered_map<std::uint32_t, std::chrono::steady_clock::time_point> mPendingHeartbeats;

    /// The silence watchdog's clock: wall time this process spent listening, advanced once per
    /// poll by the gap since the previous one, but by no more than a capped amount. A long frame
    /// of OUR own (shader compile, window drag) is time nobody could have been heard in, so it
    /// must not count against every peer at once. Zero-based per session.
    std::chrono::steady_clock::duration mAttentiveTime{};
    std::chrono::steady_clock::time_point mLastPollAt{};

    /// For proving the pump really runs at the 30 Hz sim rate rather than the render rate.
    std::chrono::steady_clock::time_point mRateWindowStart;
    std::uint64_t mRateWindowStartTick = 0;
};

/// The process-wide session.
NetworkManager& network_manager();

}  // namespace dusk::mp
