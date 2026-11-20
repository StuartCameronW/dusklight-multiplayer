#include "enet_transport.hpp"

#include <enet/enet.h>

#include <chrono>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "dusk/logging.h"
#include "packet.hpp"

namespace dusk::mp {
namespace {

constexpr borealis::Log Log{"dusk::mp::enet"};

/// STUN retransmit interval. Well under RFC 5389's 500 ms RTO because the whole attempt is short
/// and a lost request should not eat a quarter of it.
constexpr std::chrono::milliseconds kStunRetryInterval{250};
/// Used when the caller's server fails to resolve, or has not answered by half the timeout.
constexpr const char* kStunFallbackServer = "stun.l.google.com";
constexpr std::uint16_t kStunFallbackPort = 19302;

constexpr std::chrono::milliseconds kPunchInterval{100};
/// "DUSKPUNCH" then a version byte. Never parsed by ENet: the intercept drops it on arrival.
constexpr std::uint8_t kPunchMagic[] = {'D', 'U', 'S', 'K', 'P', 'U', 'N', 'C', 'H'};
constexpr std::uint8_t kPunchVersion = 1;

/// ENetHost has no user-data field, so the intercept callback finds its transport here. Touched
/// only from the game thread, like everything else in EnetTransport.
std::unordered_map<ENetHost*, EnetTransport*> sInterceptOwners;

/// Test runs set DUSK_MP_BIND_LOOPBACK=1: a loopback-only socket never triggers the Windows
/// firewall prompt, which otherwise appears for every fresh build directory's exe.
bool bind_loopback_only() {
    const char* loopbackOnly = std::getenv("DUSK_MP_BIND_LOOPBACK");
    return loopbackOnly != nullptr && loopbackOnly[0] == '1';
}

/// Dotted-quad text for an IPv4 address in HOST byte order.
std::string ipv4_to_string(std::uint32_t hostOrder) {
    return std::to_string((hostOrder >> 24) & 0xFF) + '.' +
           std::to_string((hostOrder >> 16) & 0xFF) + '.' +
           std::to_string((hostOrder >> 8) & 0xFF) + '.' + std::to_string(hostOrder & 0xFF);
}

/// Addresses that can never be reached from another machine: unset, loopback, link-local.
bool is_unroutable_local(std::uint32_t hostOrder) {
    return hostOrder == 0 || (hostOrder >> 24) == 127 || (hostOrder >> 16) == 0xA9FE;
}

/// IPv4 addresses (host byte order) of this machine's interfaces that are up and not loopback.
void collect_interface_ipv4(std::vector<std::uint32_t>& out) {
#if defined(_WIN32)
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    // The table can grow between the size query and the fetch, hence the retry.
    for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        rc = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER |
                GAA_FLAG_SKIP_FRIENDLY_NAME,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc != NO_ERROR) {
        Log.warn("GetAdaptersAddresses failed ({})", static_cast<unsigned long>(rc));
        return;
    }
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr;
        adapter = adapter->Next)
    {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
            unicast = unicast->Next)
        {
            const sockaddr* sa = unicast->Address.lpSockaddr;
            if (sa != nullptr && sa->sa_family == AF_INET) {
                out.push_back(ntohl(reinterpret_cast<const sockaddr_in*>(sa)->sin_addr.s_addr));
            }
        }
    }
#else
    // Linux, macOS, iOS, and Android (bionic has getifaddrs from API 24; we target 28).
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) {
        Log.warn("getifaddrs failed");
        return;
    }
    for (ifaddrs* it = list; it != nullptr; it = it->ifa_next) {
        if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((it->ifa_flags & IFF_UP) == 0 || (it->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        out.push_back(ntohl(reinterpret_cast<const sockaddr_in*>(it->ifa_addr)->sin_addr.s_addr));
    }
    freeifaddrs(list);
#endif
}

/**
 * ★ How long a silent peer is given before ENet declares it gone. Set explicitly rather than left
 * at ENet's defaults (limit 32, minimum 5000 ms, MAXIMUM 30000 ms), because the default maximum is
 * the one that bites: over a real internet link the retransmit backoff never reaches its limit
 * inside the minimum, so the maximum governs, and a player whose game crashed would stand frozen in
 * everyone else's world for up to half a minute.
 *
 * Detection = "reliable data has gone unacknowledged for this long". Our own heartbeat is reliable
 * and ENet pings every 500 ms on top, so there is always something in flight to go unanswered.
 *
 *   - MINIMUM 8 s, not ENet's 5 s. The game is single-threaded and the network is pumped from the
 *     sim tick, so ANY long frame on the far end looks exactly like a dead peer: a first-run shader
 *     compile at a stage load, or Windows' modal loop while the window is dragged. Dropping a
 *     player out of the session over a hitch is far worse than a puppet that freezes a few seconds
 *     longer before vanishing.
 *   - MAXIMUM 12 s, not ENet's 30 s. This is the bound that matters to the person left behind.
 *   - LIMIT 32 (ENet's). Six doublings of the retransmit timeout before the minimum applies.
 *
 * ★ The maximum is NOT a bound in practice. ENet tests it only when some reliable command's
 * retransmit timer expires (protocol.c, enet_protocol_check_timeouts), and that timer doubles on
 * every retry, so the check that finally sees "over 12 s" can land anywhere up to about twice
 * that. Measured: 8.8 s and 12.9 s on loopback (2026-11-04), then 23.8 s for a force-killed guest
 * in a three-peer run (2026-11-05). NetworkManager's silence watchdog is what actually enforces
 * kPeerSilenceTimeoutMs; this policy is the backstop under it. A clean quit is seen in ~0.3 s,
 * because the destructor hangs up instead of leaving it to either.
 */
constexpr enet_uint32 kPeerTimeoutLimit = 32;
constexpr enet_uint32 kPeerTimeoutMinimumMs = 8000;
constexpr enet_uint32 kPeerTimeoutMaximumMs = kPeerSilenceTimeoutMs;

/// How long disconnect_all() blocks waiting for the far ends to acknowledge. The disconnect is
/// sent on the first service call, so a peer on a healthy link hears it almost at once; the wait
/// only buys a retransmit or two when the goodbye itself is lost. Bounded because it runs on the
/// game thread, including at process exit.
constexpr std::chrono::milliseconds kDisconnectLinger{250};

/// ENet requires exactly one global init per process. Ref-counting instead of a plain bool so a
/// future dedicated-server build can create and destroy transports freely without tearing down
/// the library out from under another one.
int sInitRefCount = 0;

bool ensure_enet_initialized() {
    if (sInitRefCount == 0) {
        if (enet_initialize() != 0) {
            Log.error("enet_initialize() failed");
            return false;
        }
    }
    ++sInitRefCount;
    return true;
}

void release_enet() {
    if (sInitRefCount > 0 && --sInitRefCount == 0) {
        enet_deinitialize();
    }
}

}  // namespace

EnetTransport::EnetTransport() {
    ensure_enet_initialized();
}

EnetTransport::~EnetTransport() {
    if (mHost != nullptr) {
        // Reached at process exit through the session's destructor, which is what makes quitting
        // the game a clean hang-up for everyone else rather than a 12-second timeout.
        disconnect_all(static_cast<std::uint32_t>(DisconnectReason::Left));
        sInterceptOwners.erase(reinterpret_cast<ENetHost*>(mHost));
        enet_host_destroy(reinterpret_cast<ENetHost*>(mHost));
        mHost = nullptr;
    }
    release_enet();
}

bool EnetTransport::listen(std::uint16_t port, std::size_t maxPeers) {
    if (mHost != nullptr) {
        Log.warn("listen() called on a transport that is already open");
        return false;
    }

    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;

    if (bind_loopback_only()) {
        enet_address_set_host_ip(&address, "127.0.0.1");
    }

    ENetHost* host = enet_host_create(&address, maxPeers, kChannelCount, 0, 0);
    if (host == nullptr) {
        Log.error("Failed to bind UDP port {} — is it already in use?", port);
        return false;
    }

    mHost = reinterpret_cast<_ENetHost*>(host);
    install_intercept();
    Log.info("Hosting on UDP port {} (max {} peers)", port, maxPeers);
    return true;
}

bool EnetTransport::open_client() {
    if (mHost != nullptr) {
        if (mClientOpen) {
            return true;
        }
        Log.warn("open_client() called on a transport that is already open");
        return false;
    }

    // Bound explicitly (to port 0, so the OS picks one) rather than left unbound as connect()
    // does, so the port is known now: STUN learns the public side of THIS mapping, and
    // lan_endpoints() can report it before any connection exists.
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = 0;
    if (bind_loopback_only()) {
        enet_address_set_host_ip(&address, "127.0.0.1");
    }

    ENetHost* host = enet_host_create(&address, 1, kChannelCount, 0, 0);
    if (host == nullptr) {
        Log.error("Failed to create client socket");
        return false;
    }

    mHost = reinterpret_cast<_ENetHost*>(host);
    mClientOpen = true;
    install_intercept();
    Log.info("Client socket open on UDP port {}", host->address.port);
    return true;
}

bool EnetTransport::connect(const std::string& address, std::uint16_t port) {
    // open_client() made the socket already: connect from it, so the NAT mapping STUN saw is the
    // one the connection uses.
    const bool reuse = mHost != nullptr && mClientOpen && !mConnectIssued;
    if (mHost != nullptr && !reuse) {
        Log.warn("connect() called on a transport that is already open");
        return false;
    }

    ENetHost* host = reinterpret_cast<ENetHost*>(mHost);
    if (!reuse) {
        // Client side: no bound address, one outgoing connection.
        host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
        if (host == nullptr) {
            Log.error("Failed to create client socket");
            return false;
        }
    }

    ENetAddress target{};
    target.port = port;
    if (enet_address_set_host(&target, address.c_str()) != 0) {
        Log.error("Could not resolve host '{}'", address);
        if (!reuse) {
            enet_host_destroy(host);
        }
        return false;
    }

    ENetPeer* peer = enet_host_connect(host, &target, kChannelCount, 0);
    if (peer == nullptr) {
        Log.error("No available peer slot to connect to {}:{}", address, port);
        if (!reuse) {
            enet_host_destroy(host);
        }
        return false;
    }

    // The connect attempt itself is a peer too, and is subject to the same timeout policy.
    apply_timeouts(reinterpret_cast<_ENetPeer*>(peer));

    if (reuse) {
        mClientPeer = reinterpret_cast<_ENetPeer*>(peer);
    } else {
        mHost = reinterpret_cast<_ENetHost*>(host);
        install_intercept();
    }
    mConnectIssued = true;
    Log.info("Connecting to {}:{}", address, port);
    return true;
}

PeerId EnetTransport::acquire_id(_ENetPeer* peer) {
    auto* raw = reinterpret_cast<ENetPeer*>(peer);
    // ENet's peer->data is free for application use; we stash the id there so an incoming event
    // resolves to our id without a linear scan.
    if (raw->data != nullptr) {
        return static_cast<PeerId>(reinterpret_cast<std::uintptr_t>(raw->data));
    }

    const PeerId id = mNextPeerId++;
    raw->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
    mPeers[id] = peer;
    return id;
}

void EnetTransport::apply_timeouts(_ENetPeer* peer) {
    // Test knob: DUSK_MP_TEST_ENET_TIMEOUT_SCALE=<n> stretches ENet's own timeouts n-fold so only
    // NetworkManager's silence watchdog can drop a dead peer at kPeerSilenceTimeoutMs.
    static const enet_uint32 scale = [] {
        const char* v = std::getenv("DUSK_MP_TEST_ENET_TIMEOUT_SCALE");
        const long n = v != nullptr ? std::strtol(v, nullptr, 10) : 1;
        return static_cast<enet_uint32>(n > 1 && n <= 100 ? n : 1);
    }();
    enet_peer_timeout(reinterpret_cast<ENetPeer*>(peer), kPeerTimeoutLimit * scale,
        kPeerTimeoutMinimumMs * scale, kPeerTimeoutMaximumMs * scale);
}

_ENetPeer* EnetTransport::lookup(PeerId id) const {
    const auto it = mPeers.find(id);
    return it == mPeers.end() ? nullptr : it->second;
}

void EnetTransport::poll(std::vector<TransportEvent>& out) {
    if (mHost == nullptr) {
        return;
    }

    auto* host = reinterpret_cast<ENetHost*>(mHost);
    ENetEvent event{};
    // Timeout 0 — never block the game thread.
    while (enet_host_service(host, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            if (mClientOpen && reinterpret_cast<_ENetPeer*>(event.peer) != mClientPeer) {
                // A client socket made by open_client() is bound and, after STUN, publicly known,
                // so ENet would accept a stranger's connect on it. Only our own outgoing one
                // counts.
                Log.warn("Refusing an unexpected incoming connection on the client socket");
                enet_peer_reset(event.peer);
                break;
            }
            // Incoming peers on the host start with ENet's defaults; bring them in line.
            apply_timeouts(reinterpret_cast<_ENetPeer*>(event.peer));
            const PeerId id = acquire_id(reinterpret_cast<_ENetPeer*>(event.peer));
            TransportEvent ev;
            ev.type = TransportEventType::Connected;
            ev.peer = id;
            out.push_back(std::move(ev));
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE: {
            const PeerId id = acquire_id(reinterpret_cast<_ENetPeer*>(event.peer));
            TransportEvent ev;
            ev.type = TransportEventType::Data;
            ev.peer = id;
            ev.data.assign(event.packet->data, event.packet->data + event.packet->dataLength);
            out.push_back(std::move(ev));
            enet_packet_destroy(event.packet);
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
            const PeerId id = acquire_id(reinterpret_cast<_ENetPeer*>(event.peer));
            TransportEvent ev;
            ev.type = TransportEventType::Disconnected;
            ev.peer = id;
            // ENet delivers the far end's disconnect data here, and 0 on a timeout.
            ev.reason = event.data;
            out.push_back(std::move(ev));
            event.peer->data = nullptr;
            mPeers.erase(id);
            break;
        }
        default:
            break;
        }
    }

    // After servicing, so a STUN reply that arrived this call stops the retransmit below.
    const auto now = Clock::now();
    tick_stun(now);
    tick_punch(now);
}

void EnetTransport::send(
    PeerId peer, const std::uint8_t* data, std::size_t size, std::uint8_t channel, bool reliable) {
    _ENetPeer* target = lookup(peer);
    if (target == nullptr || size == 0) {
        return;
    }

    const enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    ENetPacket* packet = enet_packet_create(data, size, flags);
    if (packet == nullptr) {
        return;
    }
    if (enet_peer_send(reinterpret_cast<ENetPeer*>(target), channel, packet) != 0) {
        // enet_peer_send only takes ownership on success.
        enet_packet_destroy(packet);
    }
}

void EnetTransport::broadcast(
    const std::uint8_t* data, std::size_t size, std::uint8_t channel, bool reliable) {
    if (mHost == nullptr || size == 0) {
        return;
    }

    const enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    ENetPacket* packet = enet_packet_create(data, size, flags);
    if (packet == nullptr) {
        return;
    }
    // enet_host_broadcast takes ownership, and frees the packet itself if there are no peers.
    enet_host_broadcast(reinterpret_cast<ENetHost*>(mHost), channel, packet);
}

void EnetTransport::flush() {
    if (mHost != nullptr) {
        enet_host_flush(reinterpret_cast<ENetHost*>(mHost));
    }
}

std::uint32_t EnetTransport::rtt_ms(PeerId peer) const {
    _ENetPeer* target = lookup(peer);
    if (target == nullptr) {
        return 0;
    }
    return static_cast<std::uint32_t>(reinterpret_cast<ENetPeer*>(target)->roundTripTime);
}

std::size_t EnetTransport::peer_count() const {
    return mPeers.size();
}

void EnetTransport::disconnect(PeerId peer, std::uint32_t reason, bool afterQueued) {
    _ENetPeer* target = lookup(peer);
    if (target == nullptr) {
        return;
    }
    // The peer stays in mPeers until its DISCONNECT event arrives, so that event still resolves to
    // the id the caller knows it by.
    if (afterQueued) {
        enet_peer_disconnect_later(reinterpret_cast<ENetPeer*>(target), reason);
    } else {
        enet_peer_disconnect(reinterpret_cast<ENetPeer*>(target), reason);
    }
}

void EnetTransport::drop(PeerId peer, std::uint32_t reason) {
    _ENetPeer* target = lookup(peer);
    if (target == nullptr) {
        return;
    }
    auto* raw = reinterpret_cast<ENetPeer*>(target);
    // disconnect_now sends one unsequenced goodbye and resets the peer on the spot, and a reset
    // peer never produces an ENet DISCONNECT event; ENet discards whatever still arrives for it.
    // Clearing data matters for the slot's next occupant, which must get a fresh id, not this one.
    raw->data = nullptr;
    mPeers.erase(peer);
    enet_peer_disconnect_now(raw, reason);
}

void EnetTransport::disconnect_all(std::uint32_t reason) {
    if (mHost == nullptr) {
        return;
    }

    for (auto& [id, peer] : mPeers) {
        enet_peer_disconnect(reinterpret_cast<ENetPeer*>(peer), reason);
    }

    // Wait for the acknowledgements, so a lost goodbye gets retransmitted instead of leaving the
    // far end to time out — but never longer than the linger, because this blocks the game thread.
    auto* host = reinterpret_cast<ENetHost*>(mHost);
    const auto deadline = std::chrono::steady_clock::now() + kDisconnectLinger;
    std::size_t remaining = mPeers.size();
    ENetEvent event{};
    while (remaining > 0 && std::chrono::steady_clock::now() < deadline) {
        if (enet_host_service(host, &event, 10) <= 0) {
            continue;
        }
        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            enet_packet_destroy(event.packet);
        } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            const auto it = mPeers.find(
                static_cast<PeerId>(reinterpret_cast<std::uintptr_t>(event.peer->data)));
            if (it != mPeers.end()) {
                event.peer->data = nullptr;
                mPeers.erase(it);
                --remaining;
            }
        }
    }

    // Anyone who did not answer in time is dropped; they will time out on their own side.
    for (auto& [id, peer] : mPeers) {
        enet_peer_reset(reinterpret_cast<ENetPeer*>(peer));
    }
    mPeers.clear();
}

// --- NAT traversal -------------------------------------------------------------------------------

void EnetTransport::install_intercept() {
    auto* host = reinterpret_cast<ENetHost*>(mHost);
    sInterceptOwners[host] = this;
    host->intercept = &EnetTransport::intercept_raw;
}

int EnetTransport::intercept_raw(_ENetHost* host, _ENetEvent* /*event*/) {
    // Called by ENet for every datagram it reads, before it parses one. Returning 1 with the event
    // left at NONE makes ENet drop the datagram and read the next.
    auto* raw = reinterpret_cast<ENetHost*>(host);
    const auto it = sInterceptOwners.find(raw);
    if (it == sInterceptOwners.end()) {
        return 0;
    }
    return it->second->on_raw_datagram(raw->receivedData, raw->receivedDataLength) ? 1 : 0;
}

bool EnetTransport::on_raw_datagram(const std::uint8_t* data, std::size_t size) {
    if (size >= sizeof(kPunchMagic) && std::memcmp(data, kPunchMagic, sizeof(kPunchMagic)) == 0) {
        if (!mPunchReceivedLogged) {
            const ENetAddress& from = reinterpret_cast<ENetHost*>(mHost)->receivedAddress;
            Log.info("[mp-punch] first punch received from {}:{}",
                ipv4_to_string(ENET_NET_TO_HOST_32(from.host)), from.port);
            mPunchReceivedLogged = true;
        }
        return true;
    }

    if (!stun::looks_like_stun(data, size)) {
        return false;
    }
    // Consumed whether or not it is the reply we are waiting for: a late duplicate of an answered
    // request is still STUN, and ENet would only discard it as garbage.
    if (mStun.state == StunState::Pending) {
        std::uint32_t ip = 0;
        std::uint16_t port = 0;
        if (stun::parse_binding_response(data, size, mStun.tid, &ip, &port)) {
            const ENetAddress& from = reinterpret_cast<ENetHost*>(mHost)->receivedAddress;
            mStun.result.ip = ipv4_to_string(ip);
            mStun.result.port = port;
            mStun.state = StunState::Done;
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - mStun.start);
            Log.info("[mp-stun] public endpoint {}:{} (from {}:{}, {} ms, {} request(s))",
                mStun.result.ip, mStun.result.port, ipv4_to_string(ENET_NET_TO_HOST_32(from.host)),
                from.port, ms.count(), mStun.sent);
        }
    }
    return true;
}

bool EnetTransport::send_raw(
    const RawAddress& to, const std::uint8_t* data, std::size_t size) const {
    if (mHost == nullptr) {
        return false;
    }
    ENetAddress address{};
    address.host = to.host;
    address.port = to.port;
    ENetBuffer buffer{};
    buffer.data = const_cast<std::uint8_t*>(data);
    buffer.dataLength = size;
    return enet_socket_send(reinterpret_cast<ENetHost*>(mHost)->socket, &address, &buffer, 1) > 0;
}

void EnetTransport::begin_stun(const std::string& server, std::uint16_t port, int timeoutMs) {
    StunJob job;
    if (mHost == nullptr) {
        Log.warn("[mp-stun] begin_stun() called before the socket is open");
        job.state = StunState::Failed;
        mStun = job;
        return;
    }
    if (mStun.state == StunState::Pending) {
        Log.info("[mp-stun] restarting a request that was still pending");
    }

    // ★ Blocking DNS, once per attempt, on the caller's thread. Usually a few ms from the OS
    // cache; a dead resolver can stall it for its own timeout. Accepted for now: begin_stun runs
    // once per session, at host/join time, never per frame.
    ENetAddress resolved{};
    if (!server.empty() && enet_address_set_host(&resolved, server.c_str()) == 0) {
        job.primary.host = resolved.host;
        job.primary.port = port;
        job.primaryResolved = true;
    } else {
        Log.warn("[mp-stun] could not resolve {}; using {}:{}", server, kStunFallbackServer,
            kStunFallbackPort);
    }
    if (server != kStunFallbackServer || port != kStunFallbackPort) {
        ENetAddress fallback{};
        if (enet_address_set_host(&fallback, kStunFallbackServer) == 0) {
            job.fallback.host = fallback.host;
            job.fallback.port = kStunFallbackPort;
            job.fallbackResolved = true;
        }
    }
    if (!job.primaryResolved && !job.fallbackResolved) {
        Log.warn("[mp-stun] no STUN server could be resolved (offline?)");
        job.state = StunState::Failed;
        mStun = job;
        return;
    }

    const int timeout = timeoutMs > 0 ? timeoutMs : 3000;
    const auto now = Clock::now();
    job.tid = stun::make_transaction_id();
    job.start = now;
    job.fallbackAt = now + std::chrono::milliseconds(timeout / 2);
    job.deadline = now + std::chrono::milliseconds(timeout);
    job.nextSend = now;
    job.state = StunState::Pending;
    mStun = job;

    Log.info("[mp-stun] asking {}:{} (fallback {}:{}, timeout {} ms)", server, port,
        kStunFallbackServer, kStunFallbackPort, timeout);
    tick_stun(now);
}

StunState EnetTransport::stun_state(Endpoint* outPublic) const {
    if (outPublic != nullptr && mStun.state == StunState::Done) {
        *outPublic = mStun.result;
    }
    return mStun.state;
}

void EnetTransport::tick_stun(Clock::time_point now) {
    if (mStun.state != StunState::Pending) {
        return;
    }
    if (now >= mStun.deadline) {
        mStun.state = StunState::Failed;
        Log.warn("[mp-stun] no answer after {} request(s); public endpoint unknown", mStun.sent);
        return;
    }
    if (now < mStun.nextSend) {
        return;
    }
    mStun.nextSend = now + kStunRetryInterval;

    // The same transaction id on every retransmit and to both servers (RFC 5389 §7.2.1), so a
    // reply to any of them completes the request.
    const auto request = stun::build_binding_request(mStun.tid);
    const bool useFallback =
        mStun.fallbackResolved && (!mStun.primaryResolved || now >= mStun.fallbackAt);
    bool anySent = false;
    if (mStun.primaryResolved) {
        anySent |= send_raw(mStun.primary, request.data(), request.size());
    }
    if (useFallback) {
        anySent |= send_raw(mStun.fallback, request.data(), request.size());
    }
    if (!anySent && mStun.sent == 0) {
        // Typically DUSK_MP_BIND_LOOPBACK=1: a loopback socket cannot reach the internet.
        Log.warn("[mp-stun] request could not be sent from this socket");
    }
    ++mStun.sent;
}

void EnetTransport::punch(const Endpoint& to, int durationMs) {
    if (mHost == nullptr) {
        Log.warn("[mp-punch] punch() called before the socket is open");
        return;
    }
    ENetAddress address{};
    if (to.port == 0 || enet_address_set_host_ip(&address, to.ip.c_str()) != 0) {
        Log.warn("[mp-punch] not a usable IPv4 endpoint: '{}':{}", to.ip, to.port);
        return;
    }

    const auto now = Clock::now();
    const auto until = now + std::chrono::milliseconds(durationMs > 0 ? durationMs : 0);
    for (auto& target : mPunches) {
        if (target.address.host == address.host && target.address.port == to.port) {
            if (until > target.until) {
                target.until = until;
            }
            Log.info("[mp-punch] extending punch to {}:{} by {} ms", to.ip, to.port, durationMs);
            return;
        }
    }

    PunchTarget target;
    target.address.host = address.host;
    target.address.port = to.port;
    target.endpoint = to;
    target.until = until;
    target.nextSend = now;
    mPunches.push_back(target);
    Log.info("[mp-punch] punching {}:{} for {} ms", to.ip, to.port, durationMs);
    tick_punch(now);
}

void EnetTransport::tick_punch(Clock::time_point now) {
    if (mPunches.empty()) {
        return;
    }
    std::uint8_t datagram[sizeof(kPunchMagic) + 1];
    std::memcpy(datagram, kPunchMagic, sizeof(kPunchMagic));
    datagram[sizeof(kPunchMagic)] = kPunchVersion;

    for (auto it = mPunches.begin(); it != mPunches.end();) {
        if (now >= it->until) {
            Log.info("[mp-punch] done punching {}:{} ({} sent)", it->endpoint.ip, it->endpoint.port,
                it->sent);
            it = mPunches.erase(it);
            continue;
        }
        if (now >= it->nextSend) {
            if (send_raw(it->address, datagram, sizeof(datagram))) {
                ++it->sent;
            }
            it->nextSend = now + kPunchInterval;
        }
        ++it;
    }
}

std::vector<Endpoint> EnetTransport::lan_endpoints() const {
    std::vector<Endpoint> out;
    if (mHost == nullptr) {
        return out;
    }
    // Asked of the socket rather than read from host->address, which ENet leaves unset for the
    // unbound socket connect() makes when open_client() was not called (port 0 until first send).
    ENetAddress bound{};
    if (enet_socket_get_address(reinterpret_cast<ENetHost*>(mHost)->socket, &bound) != 0 ||
        bound.port == 0)
    {
        return out;
    }

    const std::uint32_t boundHost = ENET_NET_TO_HOST_32(bound.host);
    if ((boundHost >> 24) == 127) {
        // DUSK_MP_BIND_LOOPBACK=1: the only address this socket answers on, which is exactly
        // what a two-instance test on one machine needs.
        out.push_back(Endpoint{ipv4_to_string(boundHost), bound.port});
        return out;
    }

    std::vector<std::uint32_t> addresses;
    collect_interface_ipv4(addresses);
    for (const std::uint32_t a : addresses) {
        if (is_unroutable_local(a)) {
            continue;
        }
        const std::string ip = ipv4_to_string(a);
        bool duplicate = false;
        for (const Endpoint& e : out) {
            duplicate = duplicate || e.ip == ip;
        }
        if (!duplicate) {
            out.push_back(Endpoint{ip, bound.port});
        }
    }
    return out;
}

}  // namespace dusk::mp
