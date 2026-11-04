#include "enet_transport.hpp"

#include <enet/enet.h>

#include <chrono>
#include <cstdlib>

#include "dusk/logging.h"
#include "packet.hpp"

namespace dusk::mp {
namespace {

aurora::Module Log{"dusk::mp::enet"};

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
 * Measured on loopback (2026-11-04), hard-killing one end with Stop-Process: the host noticed a
 * dead client after 8.8 s of silence and a client noticed a dead host after 12.9 s. A clean quit
 * is seen in ~0.3 s, because the destructor hangs up instead of leaving it to this.
 */
constexpr enet_uint32 kPeerTimeoutLimit = 32;
constexpr enet_uint32 kPeerTimeoutMinimumMs = 8000;
constexpr enet_uint32 kPeerTimeoutMaximumMs = 12000;

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

    ENetHost* host = enet_host_create(&address, maxPeers, kChannelCount, 0, 0);
    if (host == nullptr) {
        Log.error("Failed to bind UDP port {} — is it already in use?", port);
        return false;
    }

    mHost = reinterpret_cast<_ENetHost*>(host);
    Log.info("Hosting on UDP port {} (max {} peers)", port, maxPeers);
    return true;
}

bool EnetTransport::connect(const std::string& address, std::uint16_t port) {
    if (mHost != nullptr) {
        Log.warn("connect() called on a transport that is already open");
        return false;
    }

    // Client side: no bound address, one outgoing connection.
    ENetHost* host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
    if (host == nullptr) {
        Log.error("Failed to create client socket");
        return false;
    }

    ENetAddress target{};
    target.port = port;
    if (enet_address_set_host(&target, address.c_str()) != 0) {
        Log.error("Could not resolve host '{}'", address);
        enet_host_destroy(host);
        return false;
    }

    ENetPeer* peer = enet_host_connect(host, &target, kChannelCount, 0);
    if (peer == nullptr) {
        Log.error("No available peer slot to connect to {}:{}", address, port);
        enet_host_destroy(host);
        return false;
    }

    // The connect attempt itself is a peer too, and is subject to the same timeout policy.
    apply_timeouts(reinterpret_cast<_ENetPeer*>(peer));

    mHost = reinterpret_cast<_ENetHost*>(host);
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
    enet_peer_timeout(reinterpret_cast<ENetPeer*>(peer), kPeerTimeoutLimit, kPeerTimeoutMinimumMs,
        kPeerTimeoutMaximumMs);
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

}  // namespace dusk::mp
