#include "enet_transport.hpp"

#include <enet/enet.h>

#include <cstdlib>

#include "dusk/logging.h"
#include "packet.hpp"

namespace dusk::mp {
namespace {

aurora::Module Log{"dusk::mp::enet"};

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
        disconnect_all();
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

void EnetTransport::disconnect_all() {
    if (mHost == nullptr) {
        return;
    }

    for (auto& [id, peer] : mPeers) {
        enet_peer_disconnect(reinterpret_cast<ENetPeer*>(peer), 0);
    }

    // Give ENet a brief window to deliver the disconnects, then drop whatever is left.
    auto* host = reinterpret_cast<ENetHost*>(mHost);
    ENetEvent event{};
    while (enet_host_service(host, &event, 50) > 0) {
        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            enet_packet_destroy(event.packet);
        }
    }

    for (auto& [id, peer] : mPeers) {
        enet_peer_reset(reinterpret_cast<ENetPeer*>(peer));
    }
    mPeers.clear();
}

}  // namespace dusk::mp
