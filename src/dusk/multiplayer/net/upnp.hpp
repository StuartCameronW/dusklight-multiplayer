#pragma once

#include <cstdint>
#include <string>

/**
 * UPnP-IGD automatic port forwarding for the host (miniupnpc). Option (F) in
 * .claude/future-plans/mp-internet-play.md: asks the router to forward a UDP port to this machine.
 * Only helps where the router has UPnP enabled, and is useless behind CGNAT, so it complements
 * STUN + hole punching rather than replacing it.
 *
 * ★ Both calls BLOCK (SSDP discovery alone waits up to 2 s, then several HTTP round trips to the
 * router), so callers must run them off the game thread. They are serialised by an internal mutex
 * and safe to call from any thread.
 *
 * Built without miniupnpc (DUSK_MP_HAS_UPNP undefined: Android, iOS, tvOS, where devices only
 * join), map_udp() returns false and unmap_udp() does nothing.
 */

namespace dusk::mp::upnp {

/// Forward external UDP @p port to the same port on this machine, leased for two hours. On
/// success, fills @p externalIp (if non-null) with the router's public IPv4 address, which may be
/// empty if the router would not say.
bool map_udp(std::uint16_t port, std::string* externalIp);

/// Remove a mapping made by map_udp(). Harmless if there is none.
void unmap_udp(std::uint16_t port);

}  // namespace dusk::mp::upnp
