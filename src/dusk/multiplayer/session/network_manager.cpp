#include "network_manager.hpp"

#include <cstdlib>
#include <iterator>
#include <string>

#include "../net/enet_transport.hpp"
#include "../net/packet.hpp"
#include "../net/serializer.hpp"
#include "../replication/player_state.hpp"
#include "../replication/replication_manager.hpp"
#include "dusk/logging.h"
#include "trace.hpp"

namespace dusk::mp {
namespace {

aurora::Module Log{"dusk::mp"};

/// Heartbeat cadence, in sim ticks. The sim runs at 30 Hz, so this is roughly once a second.
constexpr std::uint64_t kHeartbeatIntervalTicks = 30;

/// How often to log the measured tick rate while a session is live.
constexpr std::uint64_t kRateReportIntervalTicks = 150;

/// How often to log interpolation health (buffer depth, starvations). ~10 s at 30 Hz.
constexpr std::uint64_t kInterpReportIntervalTicks = 300;

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
    // packet is sized so that all of them fit in one unfragmented datagram.
    if (!transport->listen(port, kMaxPlayers - 1)) {
        return false;
    }

    mTransport = std::move(transport);
    mRole = Role::Host;
    mRateWindowStart = std::chrono::steady_clock::now();
    mRateWindowStartTick = mSimTick;
    Log.info("Session started as HOST (protocol v{})", kProtocolVersion);
    return true;
}

bool NetworkManager::join(const std::string& address, std::uint16_t port) {
    if (mRole != Role::Inactive) {
        Log.warn("Already in a session");
        return false;
    }

    auto transport = std::make_unique<EnetTransport>();
    if (!transport->connect(address, port)) {
        return false;
    }

    mTransport = std::move(transport);
    mRole = Role::Client;
    mRateWindowStart = std::chrono::steady_clock::now();
    mRateWindowStartTick = mSimTick;
    Log.info("Session started as CLIENT (protocol v{})", kProtocolVersion);
    return true;
}

void NetworkManager::shutdown() {
    if (mTransport) {
        mTransport->disconnect_all();
        mTransport.reset();
    }
    replication_manager().clear();
    mPeers.clear();
    mPendingHeartbeats.clear();
    mRole = Role::Inactive;
    trace::close();
    Log.info("Session ended");
}

void NetworkManager::pre_actor_tick() {
    ensure_initialized();
    if (!mTransport) {
        return;
    }

    ++mSimTick;

    mEventScratch.clear();
    mTransport->poll(mEventScratch);
    for (const TransportEvent& event : mEventScratch) {
        handle_event(event);
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
        mPeers[event.peer] = session;
        Log.info("Peer {} connected ({} total)", event.peer, mPeers.size());
        // The client speaks first so the host can vet its protocol version.
        if (mRole == Role::Client) {
            send_hello(event.peer);
        }
        break;
    }
    case TransportEventType::Disconnected: {
        const auto it = mPeers.find(event.peer);
        if (it != mPeers.end()) {
            const std::uint32_t playerId = it->second.playerId;
            mPeers.erase(it);
            if (mRole == Role::Host && playerId != 0) {
                // Despawn locally and tell the remaining clients, otherwise a puppet stands frozen
                // in the world forever — one of the ways tpmp sessions visibly rot.
                replication_manager().remove_player(playerId);
                broadcast_peer_left(playerId);
            } else if (mRole == Role::Client) {
                // Lost the host: every puppet's authority is gone, so clear the whole world view.
                replication_manager().clear();
            }
        }
        Log.info("Peer {} disconnected", event.peer);
        break;
    }
    case TransportEventType::Data:
        handle_packet(event.peer, event.data.data(), event.data.size());
        break;
    }
}

void NetworkManager::send_hello(PeerId peer) {
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::Hello));
    w.write_u32(kProtocolVersion);
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

    switch (static_cast<PacketId>(rawId)) {
    case PacketId::Hello: {
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

        const bool accepted = version == kProtocolVersion;
        if (!accepted) {
            Log.warn("Rejecting peer {}: protocol v{} != our v{}", peer, version, kProtocolVersion);
        }

        const auto peerIt = mPeers.find(peer);
        if (peerIt == mPeers.end()) {
            return;
        }

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

        if (accepted) {
            peerIt->second.playerId = playerId;
            peerIt->second.nickname = nickname;
            peerIt->second.color = assignedColor;
            peerIt->second.handshakeComplete = true;

            // Order matters: the newcomer must learn the existing roster before anyone is told
            // about the newcomer, so no client can receive a PeerJoined for a session it has an
            // incomplete view of.
            send_peer_list(peer);
            broadcast_peer_joined(peerIt->second);

            replication_manager().add_player(playerId, nickname, assignedColor);
            Log.info("Peer {} admitted as player {} ('{}', #{:06X})", peer, playerId, nickname,
                assignedColor);
        }
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
            Log.error("Host rejected us (host protocol v{}, ours v{})", version, kProtocolVersion);
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

        Log.info("Handshake complete — we are player {} ('{}', #{:06X}), host is '{}' (#{:06X})",
            assignedId, mLocalNickname, mLocalColor, hostNickname, hostColor);
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

        const auto elapsed = std::chrono::steady_clock::now() - it->second;
        const auto rttMs =
            std::chrono::duration_cast<std::chrono::duration<double, std::milli> >(elapsed).count();
        mPendingHeartbeats.erase(it);

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
        break;
    }
    case PacketId::PeerLeft: {
        std::uint32_t playerId = 0;
        if (!r.read_u32(playerId) || !r.ok()) {
            return;
        }
        replication_manager().remove_player(playerId);
        Log.info("Player {} left", playerId);
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

    mTransport->broadcast(w.data().data(), w.size(), kChannelState, false);
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
             "pose ({:.0f}, {:.0f}, {:.0f}) angleY {} speed {:.2f}",
        replication.players().size(), diag.worstDelayTicks, diag.totalStarvations, diag.totalSnaps,
        diag.samplePose.posX, diag.samplePose.posY, diag.samplePose.posZ, diag.samplePose.angleY,
        diag.samplePose.speed);
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
    if (mPendingHeartbeats.size() > 64) {
        mPendingHeartbeats.clear();
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
