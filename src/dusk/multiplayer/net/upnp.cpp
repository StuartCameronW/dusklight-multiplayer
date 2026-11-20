#include "upnp.hpp"

#include "dusk/logging.h"

#ifdef DUSK_MP_HAS_UPNP
#include <cstdio>
#include <cstring>
#include <mutex>

#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>
#endif

namespace dusk::mp::upnp {
namespace {

constexpr borealis::Log Log{"dusk::mp::upnp"};

}  // namespace

#ifdef DUSK_MP_HAS_UPNP

namespace {

/// Shows up in the router's port-forwarding table, so the user can tell what opened the port.
constexpr const char* kDescription = "Dusklight co-op";
/// Two hours. A crashed host's mapping then expires on its own instead of lingering forever.
constexpr const char* kLeaseSeconds = "7200";
/// UPnP error 725, OnlyPermanentLeasesSupported: some routers refuse any finite lease.
constexpr int kErrOnlyPermanentLeases = 725;
/// SSDP discovery wait. The whole map_udp() call stays around 3 s with the router round trips.
constexpr int kDiscoverDelayMs = 2000;

std::mutex sMutex;

/// The router found by the last successful discovery, kept so unmap_udp() need not search again.
struct Igd {
    bool valid = false;
    UPNPUrls urls{};
    IGDdatas data{};
    char lanAddr[64] = {};
};
Igd sIgd;

void forget_igd() {
    if (sIgd.valid) {
        FreeUPNPUrls(&sIgd.urls);
    }
    sIgd = Igd{};
}

/// Find the router. Caller holds sMutex.
bool discover_igd() {
    forget_igd();

    int error = 0;
    UPNPDev* devices =
        upnpDiscover(kDiscoverDelayMs, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, 0, 2, &error);
    if (devices == nullptr) {
        Log.info("[mp-upnp] no UPnP devices answered discovery (error {})", error);
        return false;
    }

    char wanAddr[64] = {};
    // miniupnpc 2.2.8 (API 18) return codes: 1 connected IGD, 2 connected but its WAN address is
    // reserved/private (carrier-grade NAT: a mapping there is useless), 3 IGD reports itself not
    // connected, 4 a UPnP device that is not an IGD, 0 nothing.
    const int result = UPNP_GetValidIGD(devices, &sIgd.urls, &sIgd.data, sIgd.lanAddr,
        static_cast<int>(sizeof(sIgd.lanAddr)), wanAddr, static_cast<int>(sizeof(wanAddr)));
    freeUPNPDevlist(devices);

    switch (result) {
    case 1:
        break;
    case 3:
        // Some routers misreport their link state; trying costs nothing.
        Log.warn("[mp-upnp] router says it is not connected; trying anyway");
        break;
    case 2:
        Log.warn("[mp-upnp] router's WAN address {} is private (carrier-grade NAT?); a port "
                 "mapping there would not reach the internet",
            wanAddr);
        FreeUPNPUrls(&sIgd.urls);
        sIgd = Igd{};
        return false;
    default:
        Log.info("[mp-upnp] no usable Internet Gateway Device (result {})", result);
        if (result != 0) {
            FreeUPNPUrls(&sIgd.urls);
        }
        sIgd = Igd{};
        return false;
    }

    sIgd.valid = true;
    Log.info(
        "[mp-upnp] router found at {}, our LAN address {}", sIgd.urls.controlURL, sIgd.lanAddr);
    return true;
}

int add_mapping(const char* portStr, const char* lease) {
    return UPNP_AddPortMapping(sIgd.urls.controlURL, sIgd.data.first.servicetype, portStr, portStr,
        sIgd.lanAddr, kDescription, "UDP", nullptr, lease);
}

}  // namespace

bool map_udp(std::uint16_t port, std::string* externalIp) {
    std::lock_guard<std::mutex> lock(sMutex);
    if (externalIp != nullptr) {
        externalIp->clear();
    }

    if (!discover_igd()) {
        return false;
    }

    char portStr[8];
    std::snprintf(portStr, sizeof(portStr), "%u", static_cast<unsigned>(port));

    int r = add_mapping(portStr, kLeaseSeconds);
    if (r == kErrOnlyPermanentLeases) {
        // A permanent lease outlives a crash, so unmap_udp() on teardown matters even more here.
        Log.info("[mp-upnp] router only accepts permanent leases; retrying without one");
        r = add_mapping(portStr, "0");
    }
    if (r != UPNPCOMMAND_SUCCESS) {
        // Most often a leftover mapping of ours from a crashed session that the router will not
        // overwrite. Clear it and try once more.
        UPNP_DeletePortMapping(
            sIgd.urls.controlURL, sIgd.data.first.servicetype, portStr, "UDP", nullptr);
        r = add_mapping(portStr, kLeaseSeconds);
        if (r == kErrOnlyPermanentLeases) {
            r = add_mapping(portStr, "0");
        }
    }
    if (r != UPNPCOMMAND_SUCCESS) {
        const char* why = strupnperror(r);
        Log.warn("[mp-upnp] AddPortMapping UDP {} failed: {} ({})", port, r,
            why != nullptr ? why : "unknown");
        return false;
    }

    char extIp[40] = {};
    const int ipResult =
        UPNP_GetExternalIPAddress(sIgd.urls.controlURL, sIgd.data.first.servicetype, extIp);
    if (ipResult == UPNPCOMMAND_SUCCESS && extIp[0] != '\0') {
        if (externalIp != nullptr) {
            *externalIp = extIp;
        }
        Log.info("[mp-upnp] mapped UDP {} -> {}:{} (public {})", port, sIgd.lanAddr, port, extIp);
    } else {
        Log.info("[mp-upnp] mapped UDP {} -> {}:{} (router did not give its public address)", port,
            sIgd.lanAddr, port);
    }
    return true;
}

void unmap_udp(std::uint16_t port) {
    std::lock_guard<std::mutex> lock(sMutex);
    if (!sIgd.valid && !discover_igd()) {
        return;
    }

    char portStr[8];
    std::snprintf(portStr, sizeof(portStr), "%u", static_cast<unsigned>(port));
    const int r = UPNP_DeletePortMapping(
        sIgd.urls.controlURL, sIgd.data.first.servicetype, portStr, "UDP", nullptr);
    if (r == UPNPCOMMAND_SUCCESS) {
        Log.info("[mp-upnp] removed mapping for UDP {}", port);
    } else {
        const char* why = strupnperror(r);
        Log.info("[mp-upnp] DeletePortMapping UDP {}: {} ({})", port, r,
            why != nullptr ? why : "unknown");
    }
}

#else  // !DUSK_MP_HAS_UPNP

bool map_udp(std::uint16_t port, std::string* externalIp) {
    if (externalIp != nullptr) {
        externalIp->clear();
    }
    Log.info("[mp-upnp] not built on this platform; UDP {} not mapped", port);
    return false;
}

void unmap_udp(std::uint16_t /*port*/) {}

#endif  // DUSK_MP_HAS_UPNP

}  // namespace dusk::mp::upnp
