#include "network_manager.hpp"

#include <cstdlib>
#include <iterator>
#include <string>

#include "../net/enet_transport.hpp"
#include "../net/packet.hpp"
#include "../net/serializer.hpp"
#include "../replication/player_state.hpp"
#include "../replication/replication_manager.hpp"
#include "../replication/world_clock.hpp"
#include "dusk/logging.h"
#include "dusk/ui/ui.hpp"
#include "fmt/format.h"
#include "save_guard.hpp"
#include "trace.hpp"

namespace dusk::mp {
namespace {

constexpr borealis::Log Log{"dusk::mp"};

/// Heartbeat cadence, in sim ticks. The sim runs at 30 Hz, so this is roughly once a second.
constexpr std::uint64_t kHeartbeatIntervalTicks = 30;

/// How often to log the measured tick rate while a session is live.
constexpr std::uint64_t kRateReportIntervalTicks = 150;

/// How often to log interpolation health (buffer depth, starvations). ~10 s at 30 Hz.
constexpr std::uint64_t kInterpReportIntervalTicks = 300;

/**
 * Client: how long to wait for the host's HelloAck before giving up on the join.
 *
 * Our own deadline rather than ENet's, because ENet's connect timeout falls out of its retransmit
 * backoff from a 500 ms initial guess at the RTT and lands somewhere near 15 s — long enough that a
 * player who typed the wrong address assumes the game froze. And it only covers "nobody answered":
 * something that accepts the connection and never replies would leave us waiting forever.
 */
constexpr std::chrono::seconds kJoinTimeout{10};

/// Host: how long a connected peer gets to send its Hello. A real client sends it the instant it
/// connects, on the reliable channel, so this only ever fires for something that is not one of us
/// and would otherwise sit in a player slot indefinitely.
constexpr std::chrono::seconds kHelloTimeout{5};

/// Extra ENet peer slots beyond the player cap. Without a spare, ENet ignores the connect of the
/// player who makes it one too many, and they wait out a timeout and are told "unreachable". With
/// one, we can accept the connection just long enough to tell them the session is full.
constexpr std::size_t kSpareConnectionSlots = 2;

/// Heartbeats older than this are forgotten unanswered. They are broadcast, so one sequence number
/// is answered once per peer; keeping it around until it ages out is what lets every peer's reply
/// produce an RTT, not just the first one.
constexpr std::chrono::seconds kHeartbeatExpiry{10};

/**
 * ★ Test knobs for the two refusals, which a scripted run otherwise cannot reach: "full" needs
 * eight running copies, and "wrong version" needs two different builds. Same trade as
 * DUSK_MP_TEST_SAVE_REFUSAL — without them these paths could only be reasoned about. Read once;
 * never set in normal play.
 *
 * DUSK_MP_TEST_MAX_PLAYERS (host): lower the player cap, host included. Clamped to [2,
 * kMaxPlayers]. DUSK_MP_TEST_PROTOCOL_VERSION (client): the version this client CLAIMS in its
 * Hello.
 */
std::size_t max_players() {
    static const std::size_t value = [] {
        const char* env = std::getenv("DUSK_MP_TEST_MAX_PLAYERS");
        const long parsed = env != nullptr ? std::strtol(env, nullptr, 10) : 0;
        return parsed >= 2 && parsed <= static_cast<long>(kMaxPlayers) ?
                   static_cast<std::size_t>(parsed) :
                   kMaxPlayers;
    }();
    return value;
}

std::uint32_t advertised_protocol_version() {
    static const std::uint32_t value = [] {
        const char* env = std::getenv("DUSK_MP_TEST_PROTOCOL_VERSION");
        return env != nullptr && env[0] != '\0' ?
                   static_cast<std::uint32_t>(std::strtoul(env, nullptr, 10)) :
                   kProtocolVersion;
    }();
    return value;
}

/// DUSK_MP_TEST_LEAVE_AT_TICK: call leave_session() on this session tick, exactly as the overlay's
/// Leave button does. The button is the only other way in, and a scripted run cannot press it.
std::uint64_t test_leave_tick() {
    static const std::uint64_t value = [] {
        const char* env = std::getenv("DUSK_MP_TEST_LEAVE_AT_TICK");
        return env != nullptr ? std::strtoull(env, nullptr, 10) : 0ull;
    }();
    return value;
}

/// Both the toast and the log, always together, so a run's log shows exactly what the player saw.
void notify(const std::string& title, const std::string& content, int seconds = 5) {
    Log.info("Notify: {} — {}", title, content);
    dusk::ui::push_toast({
        .title = title,
        .content = content,
        .duration = std::chrono::seconds(seconds),
    });
}

const char* reason_text(std::uint32_t reason) {
    switch (static_cast<DisconnectReason>(reason)) {
    case DisconnectReason::ConnectionLost:
        return "connection lost";
    case DisconnectReason::Left:
        return "left";
    case DisconnectReason::VersionMismatch:
        return "protocol version mismatch";
    case DisconnectReason::SessionFull:
        return "session full";
    case DisconnectReason::HandshakeTimeout:
        return "handshake timed out";
    }
    return "unknown reason";
}

std::uint32_t ms_since(std::chrono::steady_clock::time_point then) {
    if (then == std::chrono::steady_clock::time_point{}) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - then)
            .count());
}

const char* env_or_null(const char* name) {
    const char* value = std::getenv(name);
    return (value != nullptr && value[0] != '\0') ? value : nullptr;
}

std::uint16_t env_port(const char* name, std::uint16_t fallback) {
    const char* value = env_or_null(name);
    if (value == nullptr) {
        return fallback;
    }
    const long parsed = std::strtol(value, nullptr, 10);
    if (parsed <= 0 || parsed > 0xFFFF) {
        Log.warn("{}='{}' is not a valid port; using {}", name, value, fallback);
        return fallback;
    }
    return static_cast<std::uint16_t>(parsed);
}

/// Distinct, readable tints so players can tell each other apart at a glance. tpmp made every
/// player pick a name and a colour and it is genuinely necessary once more than two people play.
constexpr std::uint32_t kPlayerColors[] = {
    0x4FC3F7,  // blue
    0xEF5350,  // red
    0x66BB6A,  // green
    0xFFCA28,  // amber
    0xAB47BC,  // purple
    0xFF7043,  // orange
    0x26A69A,  // teal
    0xEC407A,  // pink
};

/// Splits "host" or "host:port". IPv6 literals are not handled yet — ENet 1.3 is IPv4 only.
void split_address(const std::string& input, std::string& host, std::uint16_t& port) {
    const std::size_t colon = input.rfind(':');
    if (colon == std::string::npos) {
        host = input;
        return;
    }
    host = input.substr(0, colon);
    const long parsed = std::strtol(input.c_str() + colon + 1, nullptr, 10);
    if (parsed > 0 && parsed <= 0xFFFF) {
        port = static_cast<std::uint16_t>(parsed);
    }
}

}  // namespace

NetworkManager& network_manager() {
    static NetworkManager sInstance;
    return sInstance;
}

void NetworkManager::set_startup_options(const StartupOptions& options) {
    if (mInitialized) {
        // The session is opened on the first tick, so anything arriving later is a wiring mistake
        // rather than a user error. Say so instead of silently doing nothing.
        Log.warn("Startup options arrived after the session was already initialized; ignoring");
        return;
    }
    mStartup.host = options.host;
    mStartup.connect = options.connect != nullptr ? options.connect : "";
    mStartup.port = options.port;
    mStartup.nickname = options.nickname != nullptr ? options.nickname : "";
    mStartup.color = options.color != nullptr ? options.color : "";
    mStartup.trace = options.trace != nullptr ? options.trace : "";
}

void NetworkManager::ensure_initialized() {
    if (mInitialized) {
        return;
    }
    mInitialized = true;

    // Command line first, environment second. The real Host/Join lobby replaces both at M2
    // (see 05-ui-integration.md); until then these are how a session gets opened.
    const std::uint16_t port =
        mStartup.port != 0 ? mStartup.port : env_port("DUSK_MP_PORT", kDefaultPort);

    if (!mStartup.nickname.empty()) {
        mLocalNickname = mStartup.nickname;
    } else if (const char* name = env_or_null("DUSK_MP_NAME")) {
        mLocalNickname = name;
    }
    if (mLocalNickname.empty()) {
        mLocalNickname = "Player";
    }

    const char* envColor = env_or_null("DUSK_MP_COLOR");
    const char* color = !mStartup.color.empty() ? mStartup.color.c_str() : envColor;
    if (color != nullptr) {
        mLocalColor = static_cast<std::uint32_t>(std::strtoul(color, nullptr, 16)) & 0xFFFFFFu;
        mColorExplicit = true;
    } else {
        // The host takes the first palette slot; a client adopts the slot matching the player id
        // the host assigns it, so two players never end up the same colour by accident.
        mLocalColor = kPlayerColors[0];
    }

    const char* envTarget = env_or_null("DUSK_MP_CONNECT");
    const char* target = !mStartup.connect.empty() ? mStartup.connect.c_str() : envTarget;
    const bool wantsHost = mStartup.host || env_or_null("DUSK_MP_HOST") != nullptr;

    if (target != nullptr && wantsHost) {
        // Both were asked for. Hosting and joining are mutually exclusive, and guessing which one
        // was meant is worse than refusing: pick neither and say why.
        Log.warn("Both a host and a join were requested ('{}'); not starting a session", target);
        return;
    }

    // Opened before the session so the trace covers the handshake as well as the play.
    if (!mStartup.trace.empty()) {
        trace::open(mStartup.trace, wantsHost ? "host" : "client");
    }

    if (target != nullptr) {
        std::string address;
        std::uint16_t targetPort = port;
        split_address(target, address, targetPort);
        join(address, targetPort);
        return;
    }

    if (wantsHost) {
        host(port);
    }
}

bool NetworkManager::host(std::uint16_t port) {
    if (mRole != Role::Inactive) {
        Log.warn("Already in a session");
        return false;
    }

    auto transport = std::make_unique<EnetTransport>();
    // Peers, not players: the host occupies one of the kMaxPlayers slots itself, and the snapshot
    // packet is sized so that all of them fit in one unfragmented datagram. The cap itself is
    // enforced at Hello; the spare connections exist only so a refusal can be delivered.
    if (!transport->listen(port, kMaxPlayers - 1 + kSpareConnectionSlots)) {
        notify("Couldn't host a session",
            fmt::format(
                "UDP port {} is unavailable. Is another copy of Dusklight already hosting?", port),
            8);
        return false;
    }

    mTransport = std::move(transport);
    mRole = Role::Host;
    mEndpoint = fmt::format("port {}", port);
    mLastEndMessage.clear();
    mRateWindowStart = std::chrono::steady_clock::now();
    mRateWindowStartTick = mSimTick;
    Log.info("Session started as HOST (protocol v{})", kProtocolVersion);
    notify("Hosting a co-op session", fmt::format("Waiting for players on UDP port {}.", port));
    return true;
}

bool NetworkManager::join(const std::string& address, std::uint16_t port) {
    if (mRole != Role::Inactive) {
        Log.warn("Already in a session");
        return false;
    }

    auto transport = std::make_unique<EnetTransport>();
    if (!transport->connect(address, port)) {
        notify("Couldn't join", fmt::format("'{}' is not an address Dusklight can reach.", address),
            8);
        return false;
    }

    mTransport = std::move(transport);
    mRole = Role::Client;
    mJoined = false;
    mJoinStarted = std::chrono::steady_clock::now();
    mEndpoint = fmt::format("{}:{}", address, port);
    mHostNickname.clear();
    mLastEndMessage.clear();
    mRateWindowStart = std::chrono::steady_clock::now();
    mRateWindowStartTick = mSimTick;
    // The host owns the save. Latch it here, at the moment we become a guest, rather than at the
    // first applied packet: v1 policy makes the host authoritative over inventory, rupees and quest
    // flags, so by the time any of that has arrived it is already too late to decide. It becomes
    // sticky for the life of the process when the host accepts us (HelloAck); a join that never
    // gets that far gives saving back in shutdown() — see save_guard.hpp.
    begin_guest_join();
    Log.info("Session started as CLIENT (protocol v{})", kProtocolVersion);
    return true;
}

NetworkManager::~NetworkManager() {
    // Process exit. Hang up so everyone else sees "left" at once instead of a frozen puppet until
    // the timeout; nothing else, because the game this would otherwise tidy up is already gone.
    if (mTransport) {
        mTransport->disconnect_all(static_cast<std::uint32_t>(DisconnectReason::Left));
        mTransport.reset();
    }
}

void NetworkManager::shutdown() {
    if (mRole == Role::Client && !mJoined) {
        // Never accepted, so nothing of the host's was applied (every host packet waits for the
        // handshake). Saving comes back; a session that did get going stays latched.
        abandon_guest_join();
    }
    if (mTransport) {
        mTransport->disconnect_all(static_cast<std::uint32_t>(DisconnectReason::Left));
        mTransport.reset();
    }
    // Every puppet goes, and the clock stops being anyone else's: with the role back to Inactive,
    // WorldClock::should_advance and reconcile hand control straight back to vanilla. The save
    // flag is deliberately NOT touched — see save_guard.hpp for why leaving does not undo joining.
    replication_manager().clear();
    world_clock().clear();
    mPeers.clear();
    mPendingHeartbeats.clear();
    mRole = Role::Inactive;
    mJoined = false;
    mHostNickname.clear();
    mLocalPlayerId = 0;
    mNextPlayerId = 1;
    mLeaveRequested = false;
    trace::close();
    Log.info("Session ended");
}

void NetworkManager::pre_actor_tick() {
    ensure_initialized();
    // Test-only knob. Before the early-out, so it also runs after the guest has left.
    run_save_guard_probe_if_requested();
    if (!mTransport) {
        return;
    }

    if (test_leave_tick() != 0 && mSimTick == test_leave_tick()) {
        Log.info("DUSK_MP_TEST_LEAVE_AT_TICK: leaving at tick {}", mSimTick);
        leave_session();
    }

    if (mLeaveRequested) {
        const bool wasClient = mRole == Role::Client;
        const bool wasJoined = mJoined;
        shutdown();
        if (wasClient) {
            mLastEndMessage = "You left the session.";
            notify("Left the co-op session", "You are playing on your own now.");
            if (wasJoined) {
                notify_save_disabled_after_session_end(SessionEnd::LeftByChoice);
            }
        } else {
            mLastEndMessage = "You ended the session.";
            notify("Co-op session ended", "Everyone who was connected has been told.");
        }
        return;
    }

    ++mSimTick;

    mEventScratch.clear();
    mTransport->poll(mEventScratch);
    for (const TransportEvent& event : mEventScratch) {
        handle_event(event);
        if (!mTransport) {
            // That event ended the session (the host went away). The rest of the batch belongs to
            // a connection that no longer exists.
            return;
        }
    }

    check_handshake_deadlines();
    if (!mTransport) {
        return;
    }

    // Apply inbound state BEFORE actors run, so this tick's actors see fresh remote positions.
    // Advancing the interpolation cursor here (once per sim tick, never per rendered frame) is
    // what keeps playback locked to the same clock the poses were captured on.
    replication_manager().drive_puppets();
}

void NetworkManager::post_actor_tick() {
    if (!mTransport) {
        return;
    }

    // Captured after the actor pass, so the pose sent is the one this tick actually ended at.
    if (mRole == Role::Client) {
        send_local_state();
        send_time_override();
    } else if (mRole == Role::Host) {
        broadcast_snapshot();
    }

    send_heartbeats();
    report_tick_rate();
    report_interpolation();
    // Sampled here rather than in pre_actor_tick: the puppets have executed by now, so what gets
    // recorded is where they ended the tick, not where they were asked to go.
    trace::write_tick(mSimTick, mLocalPlayerId);
    mTransport->flush();
}

void NetworkManager::handle_event(const TransportEvent& event) {
    switch (event.type) {
    case TransportEventType::Connected: {
        PeerSession session;
        session.id = event.peer;
        session.connectedAt = std::chrono::steady_clock::now();
        session.lastHeard = session.connectedAt;
        mPeers[event.peer] = session;
        Log.info("Peer {} connected ({} total)", event.peer, mPeers.size());
        // The client speaks first so the host can vet its protocol version.
        if (mRole == Role::Client) {
            send_hello(event.peer);
        }
        break;
    }
    case TransportEventType::Disconnected:
        handle_disconnect(event.peer, event.reason);
        break;
    case TransportEventType::Data:
        if (auto it = mPeers.find(event.peer); it != mPeers.end()) {
            it->second.lastHeard = std::chrono::steady_clock::now();
        }
        handle_packet(event.peer, event.data.data(), event.data.size());
        break;
    }
}

void NetworkManager::handle_disconnect(PeerId peer, std::uint32_t reason) {
    // "Silent for" is the time-to-detection: for a clean leave it is about one tick, for a peer
    // that vanished it is how long the transport took to give up on it.
    const auto known = mPeers.find(peer);
    Log.info("Peer {} disconnected ({}), silent for {} ms", peer, reason_text(reason),
        known != mPeers.end() ? ms_since(known->second.lastHeard) : 0);

    if (mRole == Role::Client) {
        // A client has exactly one connection, so whatever this was, the session is over. Say why
        // in terms of what the player was doing: a join that never landed reads differently from
        // a game that was going and stopped.
        const std::string host = mHostNickname.empty() ? mEndpoint : mHostNickname;
        switch (static_cast<DisconnectReason>(reason)) {
        case DisconnectReason::Left:
            end_client_session("Co-op session ended", fmt::format("{} ended the session.", host));
            break;
        case DisconnectReason::VersionMismatch:
            end_client_session("Couldn't join",
                "The host is running a different version of Dusklight. Both players need the "
                "same build.");
            break;
        case DisconnectReason::SessionFull:
            end_client_session("Couldn't join", "The session is full.");
            break;
        case DisconnectReason::HandshakeTimeout:
            end_client_session("Couldn't join", "The host stopped waiting for us to introduce "
                                                "ourselves. Try joining again.");
            break;
        case DisconnectReason::ConnectionLost:
        default:
            if (mJoined) {
                end_client_session(
                    "Lost connection to the host", fmt::format("{} stopped responding.", host));
            } else {
                end_client_session("Couldn't join",
                    fmt::format("No answer from {}. Check the address and that the host is "
                                "running, and that UDP is not blocked by a firewall.",
                        mEndpoint));
            }
            break;
        }
        return;
    }

    const auto it = mPeers.find(peer);
    if (it == mPeers.end()) {
        return;
    }
    const PeerSession session = it->second;
    mPeers.erase(it);

    if (mRole != Role::Host || !session.handshakeComplete) {
        // Never admitted, so no puppet, no roster entry and nothing anyone else was told about.
        return;
    }

    // Despawn locally and tell the remaining clients, otherwise a puppet stands frozen in the world
    // forever — one of the ways tpmp sessions visibly rot.
    replication_manager().remove_player(session.playerId);
    // ★ And drop their vote on the world clock. Under the AllPlayers rule, someone who quits while
    // standing in a dungeon would otherwise stop the sun for everyone left, permanently, with no
    // way to see why.
    world_clock().forget_peer(session.playerId);
    broadcast_peer_left(session.playerId);

    if (reason == static_cast<std::uint32_t>(DisconnectReason::Left)) {
        notify(fmt::format("{} left", session.nickname), "They left the session.", 4);
    } else {
        notify(fmt::format("{} lost connection", session.nickname),
            "They stopped responding and were removed from the session.", 5);
    }
}

void NetworkManager::end_client_session(const std::string& title, const std::string& message) {
    const bool wasJoined = mJoined;
    // Everything a host drove goes with it — puppets, and the world clock, which reverts to this
    // machine's own the moment the role is Inactive. See shutdown().
    shutdown();
    mLastEndMessage = message;
    notify(title, message, 8);
    // ★ And say out loud that saving is still off. This is the one moment the rule becomes
    // surprising: up to here "I can't save" reads as part of being in someone else's game, and the
    // instant that game ends a player reasonably assumes their own came back. It did not — the
    // world in memory is still the host's (save_guard.hpp), and the flag is sticky for exactly that
    // reason. Only after a session that actually got going: a join that never connected has nothing
    // of the host's in memory to warn about, though the flag stays latched regardless.
    if (wasJoined) {
        notify_save_disabled_after_session_end(SessionEnd::HostGone);
    }
}

void NetworkManager::refuse_peer(PeerSession& session, DisconnectReason reason) {
    session.refused = true;
    Log.warn("Refusing peer {}: {}", session.id, reason_text(static_cast<std::uint32_t>(reason)));
    // After queued packets, so the HelloAck that says no is delivered before the line drops. The
    // reason travels in the hang-up itself, which even a client whose protocol we cannot parse can
    // read.
    mTransport->disconnect(session.id, static_cast<std::uint32_t>(reason), true);
}

void NetworkManager::check_handshake_deadlines() {
    const auto now = std::chrono::steady_clock::now();

    if (mRole == Role::Client) {
        if (!mJoined && now - mJoinStarted > kJoinTimeout) {
            Log.warn("No HelloAck from {} after {} s; giving up", mEndpoint, kJoinTimeout.count());
            end_client_session("Couldn't join",
                fmt::format("No answer from {}. Check the address and that the host is running, "
                            "and that UDP is not blocked by a firewall.",
                    mEndpoint));
        }
        return;
    }

    for (auto& [id, session] : mPeers) {
        if (!session.handshakeComplete && !session.refused &&
            now - session.connectedAt > kHelloTimeout)
        {
            refuse_peer(session, DisconnectReason::HandshakeTimeout);
        }
    }
}

std::string NetworkManager::player_name(std::uint32_t playerId) const {
    const auto& players = replication_manager().players();
    if (const auto it = players.find(playerId); it != players.end() && !it->second.nickname.empty())
    {
        return it->second.nickname;
    }
    return fmt::format("Player {}", playerId);
}

SessionStatus NetworkManager::status() const {
    SessionStatus out;
    out.role = mRole;
    out.connected = mRole == Role::Host || (mRole == Role::Client && mJoined);
    out.endpoint = mEndpoint;
    out.localNickname = mLocalNickname;
    out.localColor = mLocalColor;
    out.localPlayerId = mLocalPlayerId;
    out.lastEndMessage = mLastEndMessage;

    // Direct connections first: on a client that is just the host.
    for (const auto& [id, session] : mPeers) {
        PlayerStatus p;
        p.playerId = session.playerId;
        p.nickname = session.nickname.empty() ? "(connecting)" : session.nickname;
        p.color = session.color;
        p.direct = true;
        p.handshakeComplete = session.handshakeComplete;
        p.rttMs = mTransport ? mTransport->rtt_ms(id) : 0;
        p.silentMs = ms_since(session.lastHeard);
        out.players.push_back(std::move(p));
    }

    // A client reaches everyone else through the host. The roster it was given is the replication
    // view, so read it from there rather than keeping a second copy that could disagree.
    if (mRole == Role::Client) {
        for (const auto& [playerId, player] : replication_manager().players()) {
            if (playerId == kHostPlayerId || playerId == mLocalPlayerId) {
                continue;
            }
            PlayerStatus p;
            p.playerId = playerId;
            p.nickname = player.nickname;
            p.color = player.color;
            p.handshakeComplete = true;
            out.players.push_back(std::move(p));
        }
    }
    return out;
}

void NetworkManager::send_hello(PeerId peer) {
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::Hello));
    w.write_u32(advertised_protocol_version());
    w.write_string(mLocalNickname);
    w.write_u8(mColorExplicit ? 1 : 0);
    w.write_u32(mLocalColor);
    mTransport->send(peer, w.data().data(), w.size(), kChannelControl, true);
}

void NetworkManager::handle_packet(PeerId peer, const std::uint8_t* data, std::size_t size) {
    Reader r(data, size);
    std::uint8_t rawId = 0;
    if (!r.read_u8(rawId)) {
        return;
    }

    const PacketId id = static_cast<PacketId>(rawId);
    const auto sender = mPeers.find(peer);
    if (sender == mPeers.end()) {
        return;
    }

    // ★ Who may say what. Every packet below changes someone's world, so it has to come from the
    // side that is allowed to change it: roster and snapshot updates only ever flow down from the
    // host, poses and Hellos only ever up to it. Without this a client could send the host a
    // PeerLeft and make it despawn a third player. And a host acts on nothing from a peer it has
    // not admitted (or has refused) beyond the Hello itself — a heartbeat is harmless either way.
    const bool hostBound =
        id == PacketId::Hello || id == PacketId::PlayerStateUpdate || id == PacketId::TimeOverride;
    const bool clientBound = id == PacketId::HelloAck || id == PacketId::WorldSnapshot ||
                             id == PacketId::PeerList || id == PacketId::PeerJoined ||
                             id == PacketId::PeerLeft;
    if ((hostBound && mRole != Role::Host) || (clientBound && mRole != Role::Client)) {
        Log.warn("Ignoring packet {} from peer {}: wrong direction for this role", rawId, peer);
        return;
    }
    if (mRole == Role::Host && id != PacketId::Hello && id != PacketId::Heartbeat &&
        id != PacketId::HeartbeatAck &&
        (!sender->second.handshakeComplete || sender->second.refused))
    {
        return;
    }

    switch (id) {
    case PacketId::Hello: {
        if (sender->second.handshakeComplete || sender->second.refused) {
            // Already decided. A second Hello must not mint a second player id and puppet.
            Log.warn("Ignoring repeated Hello from peer {}", peer);
            return;
        }
        std::uint32_t version = 0;
        std::string nickname;
        std::uint8_t colorExplicit = 0;
        std::uint32_t preferredColor = 0;
        if (!r.read_u32(version) || !r.read_string(nickname) || !r.read_u8(colorExplicit) ||
            !r.read_u32(preferredColor) || !r.ok())
        {
            Log.warn("Malformed Hello from peer {}", peer);
            return;
        }

        std::size_t admitted = 0;
        for (const auto& entry : mPeers) {
            if (entry.second.handshakeComplete) {
                ++admitted;
            }
        }
        const bool versionOk = version == kProtocolVersion;
        // The host is one of the kMaxPlayers.
        const bool roomOk = admitted + 1 < max_players();
        const bool accepted = versionOk && roomOk;
        if (!versionOk) {
            Log.warn("Rejecting peer {}: protocol v{} != our v{}", peer, version, kProtocolVersion);
        } else if (!roomOk) {
            Log.warn("Rejecting peer {}: session full ({} players)", peer, max_players());
        }

        const auto peerIt = sender;

        // World identity comes from our own counter, never from the transport's peer id — see
        // PeerSession::playerId for why reusing ENet's id space would collide with the host.
        const std::uint32_t playerId = accepted ? mNextPlayerId++ : 0;

        // The host owns colour assignment — otherwise its record of a peer goes stale the moment
        // the peer picks a different one, and the puppet would be tinted wrong at M1.
        const std::uint32_t assignedColor = colorExplicit != 0 ?
                                                preferredColor :
                                                kPlayerColors[playerId % std::size(kPlayerColors)];

        Writer w;
        w.write_u8(static_cast<std::uint8_t>(PacketId::HelloAck));
        w.write_u8(accepted ? 1 : 0);
        w.write_u32(kProtocolVersion);
        w.write_u32(playerId);
        w.write_u32(assignedColor);
        w.write_string(mLocalNickname);
        w.write_u32(mLocalColor);
        mTransport->send(peer, w.data().data(), w.size(), kChannelControl, true);

        if (!accepted) {
            // Hang up, not just say no. A refused peer left connected holds a player slot and keeps
            // a puppet-less ghost in the transport until it gives up on its own.
            refuse_peer(peerIt->second,
                versionOk ? DisconnectReason::SessionFull : DisconnectReason::VersionMismatch);
            break;
        }

        peerIt->second.playerId = playerId;
        peerIt->second.nickname = nickname;
        peerIt->second.color = assignedColor;
        peerIt->second.handshakeComplete = true;

        // Order matters: the newcomer must learn the existing roster before anyone is told about
        // the newcomer, so no client can receive a PeerJoined for a session it has an incomplete
        // view of.
        send_peer_list(peer);
        broadcast_peer_joined(peerIt->second);

        replication_manager().add_player(playerId, nickname, assignedColor);
        Log.info("Peer {} admitted as player {} ('{}', #{:06X})", peer, playerId, nickname,
            assignedColor);
        notify(fmt::format("{} joined", nickname), "They are now in your world.", 4);
        break;
    }
    case PacketId::HelloAck: {
        std::uint8_t accepted = 0;
        std::uint32_t version = 0;
        std::uint32_t assignedId = 0;
        std::uint32_t assignedColor = 0;
        std::string hostNickname;
        std::uint32_t hostColor = 0;
        if (!r.read_u8(accepted) || !r.read_u32(version) || !r.read_u32(assignedId) ||
            !r.read_u32(assignedColor) || !r.read_string(hostNickname) || !r.read_u32(hostColor) ||
            !r.ok())
        {
            Log.warn("Malformed HelloAck from peer {}", peer);
            return;
        }

        if (accepted == 0) {
            Log.error("Host rejected us (host protocol v{}, ours v{})", version,
                advertised_protocol_version());
            // Leave now rather than wait for the host's hang-up: a pre-v11 host never sends one,
            // and would leave us connected to nothing until the transport gave up. The host
            // states its own version, so a matching one means the refusal was for room.
            if (version != advertised_protocol_version()) {
                end_client_session("Couldn't join",
                    fmt::format("The host is running a different version of Dusklight (protocol "
                                "v{}, you have v{}). Both players need the same build.",
                        version, advertised_protocol_version()));
            } else {
                end_client_session("Couldn't join", "The session is full.");
            }
            return;
        }
        if (mJoined) {
            Log.warn("Ignoring repeated HelloAck from peer {}", peer);
            return;
        }

        // The host is authoritative on colour and identity, so take what it assigns rather than
        // deriving either locally — that keeps both ends agreeing on what this player is.
        mLocalColor = assignedColor;
        mLocalPlayerId = assignedId;

        if (auto it = mPeers.find(peer); it != mPeers.end()) {
            it->second.playerId = kHostPlayerId;
            it->second.nickname = hostNickname;
            it->second.color = hostColor;
            it->second.handshakeComplete = true;
        }

        // The host is a player too, and it is the one player never announced by PeerJoined.
        replication_manager().add_player(kHostPlayerId, hostNickname, hostColor);

        confirm_guest_join();
        mJoined = true;
        mHostNickname = hostNickname;
        Log.info("Handshake complete — we are player {} ('{}', #{:06X}), host is '{}' (#{:06X})",
            assignedId, mLocalNickname, mLocalColor, hostNickname, hostColor);
        notify(fmt::format("Joined {}'s session", hostNickname),
            "You are playing in their world. Their save is the one that counts.", 6);
        break;
    }
    case PacketId::Heartbeat: {
        std::uint32_t seq = 0;
        std::uint64_t senderTick = 0;
        if (!r.read_u32(seq) || !r.read_u64(senderTick) || !r.ok()) {
            return;
        }

        Writer w;
        w.write_u8(static_cast<std::uint8_t>(PacketId::HeartbeatAck));
        w.write_u32(seq);
        w.write_u64(senderTick);
        mTransport->send(peer, w.data().data(), w.size(), kChannelControl, true);
        break;
    }
    case PacketId::HeartbeatAck: {
        std::uint32_t seq = 0;
        std::uint64_t echoedTick = 0;
        if (!r.read_u32(seq) || !r.read_u64(echoedTick) || !r.ok()) {
            return;
        }

        const auto it = mPendingHeartbeats.find(seq);
        if (it == mPendingHeartbeats.end()) {
            return;
        }

        // Left in the map on purpose: the heartbeat was broadcast, so every peer answers this same
        // sequence, and erasing on the first reply silently denied everyone else an RTT. Expired
        // in send_heartbeats() instead.
        const auto elapsed = std::chrono::steady_clock::now() - it->second;
        const auto rttMs =
            std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(elapsed).count();

        if (auto peerIt = mPeers.find(peer); peerIt != mPeers.end()) {
            peerIt->second.lastRttMs = static_cast<std::uint32_t>(rttMs);
        }
        Log.info("Heartbeat #{} from peer {} — RTT {:.1f} ms (enet estimate {} ms)", seq, peer,
            rttMs, mTransport->rtt_ms(peer));
        break;
    }
    case PacketId::PlayerStateUpdate: {
        std::uint64_t senderTick = 0;
        PlayerState state;
        if (!r.read_u64(senderTick) || !state.read(r) || !r.ok()) {
            return;
        }
        const auto it = mPeers.find(peer);
        if (it == mPeers.end() || !it->second.handshakeComplete) {
            // Unauthenticated pose. Dropping it is the whole point of gating on the handshake.
            return;
        }
        // Stamped with the SENDER's tick, which is the only clock that describes when the pose was
        // actually true. Stamping with our own arrival tick instead (as this did until measured)
        // bakes network jitter into the spatial timeline: two poses one tick of motion apart that
        // arrive two ticks apart get stretched into a half-speed glide, and the next on-time one
        // lurches to catch up. Measured at 34.6% mean tick-to-tick change in the puppet's step
        // against the sender's own 1.3%.
        //
        // Mixing tick origins is not a risk here even though the two ends share no epoch: buffers
        // are per-player, so one only ever sees one sender's ticks, and playback is seeded relative
        // to the newest sample rather than to any absolute time.
        replication_manager().record_remote(it->second.playerId, senderTick, state);
        // Under the AllPlayers rule this is the vote that decides whether the sun moves. Taken from
        // the pose packet rather than asked for separately: it is one bit that was already free in
        // the flags byte, and it arrives at the sim rate for the same cost as nothing.
        world_clock().set_peer_time_can_pass(
            it->second.playerId, state.time_can_pass(), state.in_world());
        break;
    }
    case PacketId::TimeOverride: {
        float daytime = 0.0f;
        std::uint16_t date = 0;
        if (!r.read_f32(daytime) || !r.read_u16(date) || !r.ok()) {
            return;
        }
        const auto it = mPeers.find(peer);
        if (it == mPeers.end() || !it->second.handshakeComplete) {
            return;
        }
        if (mRole != Role::Host) {
            // Only the host arbitrates. A client receiving one of these means someone is confused
            // about who is authoritative, and adopting it would make the confusion contagious.
            Log.warn("Ignoring a TimeOverride from peer {} — this process is not the host", peer);
            return;
        }
        world_clock().receive_override(it->second.playerId, daytime, date);
        break;
    }
    case PacketId::WorldSnapshot: {
        std::uint64_t hostTick = 0;
        std::uint8_t count = 0;
        if (!r.read_u64(hostTick) || !r.read_u8(count)) {
            return;
        }
        if (count > kMaxPlayers) {
            Log.warn("Snapshot claims {} players (max {}) — dropping", count, kMaxPlayers);
            return;
        }
        if (const auto host = mPeers.find(peer);
            host == mPeers.end() || !host->second.handshakeComplete)
        {
            // Before HelloAck: drop it like every other unauthenticated packet. It carries the
            // host's clock, and a failed join may only give saving back if nothing of the host's
            // was applied (save_guard.hpp, abandon_guest_join).
            return;
        }
        for (std::uint8_t i = 0; i < count; ++i) {
            std::uint32_t playerId = 0;
            std::uint64_t originTick = 0;
            PlayerState state;
            if (!r.read_u32(playerId) || !r.read_u64(originTick) || !state.read(r)) {
                return;
            }
            if (playerId == mLocalPlayerId) {
                // Our own pose echoed back. We are authoritative over ourselves at M1, so
                // applying it would fight local input.
                continue;
            }
            // Each entry carries the clock of whoever produced it, so every player is interpolated
            // on the timeline they actually moved on. hostTick is kept only as the snapshot's own
            // send time; using it for the poses inside would re-quantise them onto our arrival
            // pattern and distort their speed.
            replication_manager().record_remote(playerId, originTick, state);
        }
        {
            // Read the clock BEFORE the ok() check so a truncated packet cannot half-apply it.
            float daytime = 0.0f;
            std::uint16_t date = 0;
            if (r.read_f32(daytime) && r.read_u16(date) && r.ok()) {
                world_clock().receive(daytime, date);
            }
        }
        if (!r.ok()) {
            Log.warn("Truncated snapshot from peer {}", peer);
        }
        break;
    }
    case PacketId::PeerList: {
        std::uint8_t count = 0;
        if (!r.read_u8(count) || count > kMaxPlayers) {
            return;
        }
        for (std::uint8_t i = 0; i < count; ++i) {
            std::uint32_t playerId = 0;
            std::string nickname;
            std::uint32_t color = 0;
            if (!r.read_u32(playerId) || !r.read_string(nickname) || !r.read_u32(color)) {
                return;
            }
            if (playerId == mLocalPlayerId) {
                continue;
            }
            replication_manager().add_player(playerId, nickname, color);
            Log.info("Roster: player {} ('{}', #{:06X})", playerId, nickname, color);
        }
        break;
    }
    case PacketId::PeerJoined: {
        std::uint32_t playerId = 0;
        std::string nickname;
        std::uint32_t color = 0;
        if (!r.read_u32(playerId) || !r.read_string(nickname) || !r.read_u32(color) || !r.ok()) {
            return;
        }
        if (playerId == mLocalPlayerId) {
            return;
        }
        replication_manager().add_player(playerId, nickname, color);
        Log.info("Player {} ('{}', #{:06X}) joined", playerId, nickname, color);
        notify(fmt::format("{} joined", nickname), "Another player is in the session.", 4);
        break;
    }
    case PacketId::PeerLeft: {
        std::uint32_t playerId = 0;
        if (!r.read_u32(playerId) || !r.ok()) {
            return;
        }
        if (playerId == mLocalPlayerId || playerId == kHostPlayerId) {
            // Neither is a third player. Losing the host is a transport event, not a roster one.
            return;
        }
        // Named before removal; the roster is the only place the name lives on a client.
        const std::string name = player_name(playerId);
        const bool known = replication_manager().players().count(playerId) != 0;
        replication_manager().remove_player(playerId);
        Log.info("Player {} left", playerId);
        if (known) {
            notify(fmt::format("{} left", name), "They are no longer in the session.", 4);
        }
        break;
    }
    default:
        Log.warn("Unknown packet id {} from peer {}", rawId, peer);
        break;
    }
}

void NetworkManager::send_peer_list(PeerId peer) {
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::PeerList));

    // The host is always in the roster, and is always first.
    std::uint8_t count = 1;
    for (const auto& entry : mPeers) {
        if (entry.second.handshakeComplete && entry.first != peer) {
            ++count;
        }
    }
    w.write_u8(count);
    w.write_u32(kHostPlayerId);
    w.write_string(mLocalNickname);
    w.write_u32(mLocalColor);

    for (const auto& entry : mPeers) {
        if (!entry.second.handshakeComplete || entry.first == peer) {
            continue;
        }
        w.write_u32(entry.second.playerId);
        w.write_string(entry.second.nickname);
        w.write_u32(entry.second.color);
    }

    mTransport->send(peer, w.data().data(), w.size(), kChannelControl, true);
}

void NetworkManager::broadcast_peer_joined(const PeerSession& joined) {
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::PeerJoined));
    w.write_u32(joined.playerId);
    w.write_string(joined.nickname);
    w.write_u32(joined.color);

    for (const auto& entry : mPeers) {
        if (!entry.second.handshakeComplete || entry.first == joined.id) {
            continue;
        }
        mTransport->send(entry.first, w.data().data(), w.size(), kChannelControl, true);
    }
}

void NetworkManager::broadcast_peer_left(std::uint32_t playerId) {
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::PeerLeft));
    w.write_u32(playerId);
    mTransport->broadcast(w.data().data(), w.size(), kChannelControl, true);
}

/**
 * ★ Always produce something to send. Going quiet is what used to cost the session its latency.
 *
 * capture_local() fails whenever world_is_playable() does: a load, the title screen, a cutscene, a
 * warp transition. That is a large fraction of a real session. The old code returned early and
 * sent nothing at all, which looks harmless — until you read it from the far end, where the
 * interpolation buffer has no way to tell "he has nothing to report" from "the link is dropping
 * packets". It assumes the latter, widens by up to five ticks per event, and then gives the
 * latency back at 0.5 ticks per five seconds. Measured on a staggered two-instance boot: the
 * buffer pinned at its 12-tick ceiling (400 ms) and was still at 5.5 ticks two minutes later.
 *
 * So we send every tick regardless. Holding the LAST GOOD pose, rather than sending a cleared
 * one, is deliberate: it reproduces exactly what the far end already displayed during these gaps
 * (its buffer held the last sample), so this change moves no puppet by a single unit. It only
 * stops the timeline stalling. Whether a puppet should instead HIDE during the other player's
 * cutscenes is a real question, but it is a visual-design decision and not this one's to make —
 * kPlayerStateInWorld is already allocated for it.
 *
 * Before the first readable pose there is genuinely nothing to show, so the flags stay clear and
 * drive_puppets() declines to spawn a puppet at the origin.
 */
PlayerState NetworkManager::wire_local_state() {
    PlayerState state;
    if (replication_manager().capture_local(state)) {
        mLastLocalState = state;
        mHaveLastLocal = true;
        return state;
    }
    return mHaveLastLocal ? mLastLocalState : PlayerState{};
}

void NetworkManager::send_local_state() {
    const PlayerState state = wire_local_state();

    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::PlayerStateUpdate));
    w.write_u64(mSimTick);
    state.write(w);
    // Unreliable and unsequenced: a lost pose is always superseded by the next tick's, so
    // retransmitting it would only add latency to data that is already obsolete.
    mTransport->broadcast(w.data().data(), w.size(), kChannelState, false);
}

void NetworkManager::broadcast_snapshot() {
    if (mPeers.empty()) {
        return;
    }

    // Never conditional, and never skipped: see wire_local_state(). The host loading a room used
    // to drop itself out of its own snapshot, starving every client's buffer for the whole load.
    const PlayerState localState = wire_local_state();

    // Serialize once and send the same bytes to everyone — clients skip their own entry. Building
    // a per-recipient packet would cost N serializations for no benefit at this size.
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::WorldSnapshot));
    w.write_u64(mSimTick);

    std::uint8_t count = 1;
    for (const auto& entry : replication_manager().players()) {
        if (entry.second.hasLatest) {
            ++count;
        }
    }
    if (count > kMaxPlayers) {
        count = static_cast<std::uint8_t>(kMaxPlayers);
    }
    w.write_u8(count);

    w.write_u32(kHostPlayerId);
    // Our own pose was captured this tick, so our clock is its origin.
    w.write_u64(mSimTick);
    localState.write(w);
    std::uint8_t written = 1;
    for (const auto& entry : replication_manager().players()) {
        if (written >= count) {
            break;
        }
        if (!entry.second.hasLatest) {
            continue;
        }
        w.write_u32(entry.second.playerId);
        // Relayed verbatim on the ORIGINATING player's clock, not ours. Re-stamping here would
        // charge every third player the jitter of the first hop on top of their own, and would
        // turn a pose we simply haven't had an update for into a fresh sample claiming that player
        // stood still — which reads as a stutter rather than as the missing data it is.
        w.write_u64(entry.second.latestTick);
        entry.second.latest.write(w);
        ++written;
    }

    // The shared world clock rides along at the sim rate. Small, and a client that misses one is
    // corrected on the very next tick rather than drifting until some slower timer comes round.
    w.write_f32(world_clock().daytime());
    w.write_u16(world_clock().date());

    mTransport->broadcast(w.data().data(), w.size(), kChannelState, false);
}

void NetworkManager::send_time_override() {
    float daytime = 0.0f;
    std::uint16_t date = 0;
    if (!world_clock().consume_pending_override(daytime, date)) {
        return;
    }

    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::TimeOverride));
    w.write_f32(daytime);
    w.write_u16(date);
    // Reliable: these are rare, discrete, and a dropped one loses the whole point of the event that
    // produced it — the player would watch the sky snap back to whatever the host still thinks.
    mTransport->broadcast(w.data().data(), w.size(), kChannelControl, true);
}

void NetworkManager::report_interpolation() {
    if (mSimTick % kInterpReportIntervalTicks != 0) {
        return;
    }
    const ReplicationManager& replication = replication_manager();
    if (replication.empty()) {
        return;
    }
    const ReplicationManager::Diagnostics diag = replication.diagnostics();
    Log.info("Interp: {} remote player(s), buffer {:.1f} ticks, {} starvation(s), {} snap(s); "
             "pose ({:.0f}, {:.0f}, {:.0f}) angleY {} gait rate {:.3f}",
        replication.players().size(), diag.worstDelayTicks, diag.totalStarvations, diag.totalSnaps,
        diag.samplePose.posX, diag.samplePose.posY, diag.samplePose.posZ, diag.samplePose.angleY,
        diag.samplePose.moveRate);
}

void NetworkManager::send_heartbeats() {
    if (mPeers.empty() || mSimTick % kHeartbeatIntervalTicks != 0) {
        return;
    }

    const std::uint32_t seq = ++mHeartbeatSeq;
    mPendingHeartbeats[seq] = std::chrono::steady_clock::now();

    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::Heartbeat));
    w.write_u32(seq);
    w.write_u64(mSimTick);
    mTransport->broadcast(w.data().data(), w.size(), kChannelControl, true);

    // Don't let unanswered heartbeats accumulate if a peer goes quiet.
    const auto now = std::chrono::steady_clock::now();
    for (auto it = mPendingHeartbeats.begin(); it != mPendingHeartbeats.end();) {
        it = now - it->second > kHeartbeatExpiry ? mPendingHeartbeats.erase(it) : std::next(it);
    }
}

void NetworkManager::report_tick_rate() {
    if (mSimTick - mRateWindowStartTick < kRateReportIntervalTicks) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto seconds = std::chrono::duration<double>(now - mRateWindowStart).count();
    if (seconds > 0.0) {
        const double hz = static_cast<double>(mSimTick - mRateWindowStartTick) / seconds;
        Log.info("Sim tick rate: {:.1f} Hz over {} ticks ({} peers)", hz,
            mSimTick - mRateWindowStartTick, mPeers.size());
    }
    mRateWindowStart = now;
    mRateWindowStartTick = mSimTick;
}

}  // namespace dusk::mp
