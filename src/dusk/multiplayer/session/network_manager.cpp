#include "network_manager.hpp"

#include <cstdlib>
#include <iterator>
#include <string>

#include "../net/enet_transport.hpp"
#include "../net/packet.hpp"
#include "../net/serializer.hpp"
#include "dusk/logging.h"

namespace dusk::mp {
namespace {

aurora::Module Log{"dusk::mp"};

/// Heartbeat cadence, in sim ticks. The sim runs at 30 Hz, so this is roughly once a second.
constexpr std::uint64_t kHeartbeatIntervalTicks = 30;

/// How often to log the measured tick rate while a session is live.
constexpr std::uint64_t kRateReportIntervalTicks = 150;

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

void NetworkManager::ensure_initialized() {
    if (mInitialized) {
        return;
    }
    mInitialized = true;

    // Milestone 0 activation is environment-driven so that no UI or CLI plumbing is needed yet.
    // The real Host/Join lobby replaces this at M2 (see 05-ui-integration.md).
    const std::uint16_t port = env_port("DUSK_MP_PORT", kDefaultPort);

    if (const char* name = env_or_null("DUSK_MP_NAME")) {
        mLocalNickname = name;
    }
    if (mLocalNickname.empty()) {
        mLocalNickname = "Player";
    }
    if (const char* color = env_or_null("DUSK_MP_COLOR")) {
        mLocalColor = static_cast<std::uint32_t>(std::strtoul(color, nullptr, 16)) & 0xFFFFFFu;
        mColorExplicit = true;
    } else {
        // The host takes the first palette slot; a client adopts the slot matching the player id
        // the host assigns it, so two players never end up the same colour by accident.
        mLocalColor = kPlayerColors[0];
    }

    if (const char* target = env_or_null("DUSK_MP_CONNECT")) {
        std::string address;
        std::uint16_t targetPort = port;
        split_address(target, address, targetPort);
        join(address, targetPort);
        return;
    }

    if (env_or_null("DUSK_MP_HOST") != nullptr) {
        host(port);
    }
}

bool NetworkManager::host(std::uint16_t port) {
    if (mRole != Role::Inactive) {
        Log.warn("Already in a session");
        return false;
    }

    auto transport = std::make_unique<EnetTransport>();
    if (!transport->listen(port, 8)) {
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
    mPeers.clear();
    mPendingHeartbeats.clear();
    mRole = Role::Inactive;
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

    // M1 applies inbound player state to the puppet actors here, so that actors execute this
    // tick against fresh remote positions.
}

void NetworkManager::post_actor_tick() {
    if (!mTransport) {
        return;
    }

    // M1 captures the local Link's pos/rot/anim here and sends it.
    send_heartbeats();
    report_tick_rate();
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
    case TransportEventType::Disconnected:
        Log.info("Peer {} disconnected", event.peer);
        mPeers.erase(event.peer);
        break;
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

        // The host owns colour assignment — otherwise its record of a peer goes stale the moment
        // the peer picks a different one, and the puppet would be tinted wrong at M1.
        const std::uint32_t assignedColor =
            colorExplicit != 0 ? preferredColor : kPlayerColors[peer % std::size(kPlayerColors)];

        Writer w;
        w.write_u8(static_cast<std::uint8_t>(PacketId::HelloAck));
        w.write_u8(accepted ? 1 : 0);
        w.write_u32(kProtocolVersion);
        w.write_u32(peer);
        w.write_u32(assignedColor);
        w.write_string(mLocalNickname);
        w.write_u32(mLocalColor);
        mTransport->send(peer, w.data().data(), w.size(), kChannelControl, true);

        if (accepted) {
            if (auto it = mPeers.find(peer); it != mPeers.end()) {
                it->second.nickname = nickname;
                it->second.color = assignedColor;
                it->second.handshakeComplete = true;
            }
            Log.info("Peer {} ('{}', #{:06X}) completed handshake", peer, nickname, assignedColor);
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

        // The host is authoritative on colour, so take what it assigns rather than deriving it
        // locally — that keeps both ends agreeing on what this player looks like.
        mLocalColor = assignedColor;

        if (auto it = mPeers.find(peer); it != mPeers.end()) {
            it->second.nickname = hostNickname;
            it->second.color = hostColor;
            it->second.handshakeComplete = true;
        }
        Log.info("Handshake complete — we are player {} ('{}', #{:06X}), host is '{}'", assignedId,
            mLocalNickname, mLocalColor, hostNickname);
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
    default:
        Log.warn("Unknown packet id {} from peer {}", rawId, peer);
        break;
    }
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
