#include "policy_sync.hpp"

#include <array>
#include <iterator>

#include "../net/packet.hpp"
#include "dusk/logging.h"
#include "dusk/settings.h"
#include "fmt/format.h"
#include "network_manager.hpp"
#include "world_sync.hpp"

namespace dusk::mp::policy_sync {
namespace {

constexpr borealis::Log Log{"dusk::mp"};

/// Guest: this machine's own policy from before the host's replaced it.
MultiplayerPolicy s_localPolicy{};
bool s_haveLocal = false;

const char* advance_name(TimeAdvanceRule r) {
    switch (r) {
    case TimeAdvanceRule::AllPlayers:
        return "AllPlayers";
    case TimeAdvanceRule::AnyPlayer:
        return "AnyPlayer";
    case TimeAdvanceRule::HostRoom:
        return "HostRoom";
    }
    return "?";
}

const char* death_name(DeathRule r) {
    switch (r) {
    case DeathRule::ReloadRoom:
        return "ReloadRoom";
    case DeathRule::RespawnAtCheckpoint:
        return "RespawnAtCheckpoint";
    case DeathRule::DownedUntilRevived:
        return "DownedUntilRevived";
    case DeathRule::RespawnSelf:
        return "RespawnSelf";
    }
    return "?";
}

/// The field's value in @p p, as it travels.
std::uint8_t encode(const MultiplayerPolicy& p, Field f) {
    switch (f) {
    case Field::Rupees:
        return static_cast<std::uint8_t>(p.rupees);
    case Field::Inventory:
        return static_cast<std::uint8_t>(p.inventory);
    case Field::Equipment:
        return static_cast<std::uint8_t>(p.equipment);
    case Field::Ammo:
        return static_cast<std::uint8_t>(p.ammo);
    case Field::Bottles:
        return static_cast<std::uint8_t>(p.bottles);
    case Field::Oil:
        return static_cast<std::uint8_t>(p.oil);
    case Field::Magic:
        return static_cast<std::uint8_t>(p.magic);
    case Field::QuestFlags:
        return static_cast<std::uint8_t>(p.questFlags);
    case Field::SceneRoom:
        return static_cast<std::uint8_t>(p.sceneRoom);
    case Field::TimeOfDay:
        return static_cast<std::uint8_t>(p.timeOfDay);
    case Field::TimeAdvance:
        return static_cast<std::uint8_t>(p.timeAdvance);
    case Field::AllowJoinPartner:
        return p.allowJoinPartner ? 1 : 0;
    case Field::Health:
        return static_cast<std::uint8_t>(p.health);
    case Field::Transform:
        return static_cast<std::uint8_t>(p.transform);
    case Field::OnDeath:
        return static_cast<std::uint8_t>(p.onDeath);
    }
    return 0;
}

bool decode_ownership(std::uint8_t v, Ownership& out) {
    if (v > static_cast<std::uint8_t>(Ownership::Individual)) {
        return false;
    }
    out = static_cast<Ownership>(v);
    return true;
}

/// Writes one received field into @p p. False (and @p p untouched) for a value out of range.
bool decode(MultiplayerPolicy& p, Field f, std::uint8_t v) {
    switch (f) {
    case Field::Rupees:
        return decode_ownership(v, p.rupees);
    case Field::Inventory:
        return decode_ownership(v, p.inventory);
    case Field::Equipment:
        return decode_ownership(v, p.equipment);
    case Field::Ammo:
        return decode_ownership(v, p.ammo);
    case Field::Bottles:
        return decode_ownership(v, p.bottles);
    case Field::Oil:
        return decode_ownership(v, p.oil);
    case Field::Magic:
        return decode_ownership(v, p.magic);
    case Field::QuestFlags:
        return decode_ownership(v, p.questFlags);
    case Field::SceneRoom:
        return decode_ownership(v, p.sceneRoom);
    case Field::TimeOfDay:
        return decode_ownership(v, p.timeOfDay);
    case Field::TimeAdvance:
        if (v > static_cast<std::uint8_t>(TimeAdvanceRule::HostRoom)) {
            return false;
        }
        p.timeAdvance = static_cast<TimeAdvanceRule>(v);
        return true;
    case Field::AllowJoinPartner:
        if (v > 1) {
            return false;
        }
        p.allowJoinPartner = v != 0;
        return true;
    case Field::Health:
        return decode_ownership(v, p.health);
    case Field::Transform:
        return decode_ownership(v, p.transform);
    case Field::OnDeath:
        if (v > static_cast<std::uint8_t>(DeathRule::RespawnSelf)) {
            return false;
        }
        p.onDeath = static_cast<DeathRule>(v);
        return true;
    }
    return false;
}

Writer build_packet() {
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::PolicySync));
    w.write_u8(kLayout);
    w.write_u8(kFieldCount);
    for (std::uint8_t i = 0; i < kFieldCount; ++i) {
        w.write_u8(encode(g_mpPolicy, static_cast<Field>(i)));
    }
    w.write_s32(world_sync::host_wallet());
    return w;
}

/// Every field that differs between @p a and @p b, as " name a->b".
std::string describe_changes(const MultiplayerPolicy& a, const MultiplayerPolicy& b) {
    const std::string before = describe(a);
    const std::string after = describe(b);
    // Both strings list the same keys in the same order; compare them token by token.
    std::string out;
    std::size_t pa = 0;
    std::size_t pb = 0;
    while (pa < before.size() && pb < after.size()) {
        const std::size_t ea = before.find(' ', pa);
        const std::size_t eb = after.find(' ', pb);
        const std::string ta = before.substr(pa, ea == std::string::npos ? ea : ea - pa);
        const std::string tb = after.substr(pb, eb == std::string::npos ? eb : eb - pb);
        if (ta != tb) {
            const std::size_t eq = tb.find('=');
            fmt::format_to(std::back_inserter(out), " {}:{}->{}", tb.substr(0, eq),
                ta.substr(ta.find('=') + 1), tb.substr(eq + 1));
        }
        if (ea == std::string::npos || eb == std::string::npos) {
            break;
        }
        pa = ea + 1;
        pb = eb + 1;
    }
    return out.empty() ? std::string(" (no change)") : out;
}

}  // namespace

const char* ownership_name(Ownership o) {
    return o == Ownership::Shared ? "Shared" : "Individual";
}

std::string describe(const MultiplayerPolicy& p) {
    return fmt::format(
        "rupees={} inventory={} equipment={} ammo={} bottles={} oil={} magic={} questFlags={} "
        "sceneRoom={} timeOfDay={} timeAdvance={} allowJoinPartner={} health={} transform={} "
        "onDeath={}",
        ownership_name(p.rupees), ownership_name(p.inventory), ownership_name(p.equipment),
        ownership_name(p.ammo), ownership_name(p.bottles), ownership_name(p.oil),
        ownership_name(p.magic), ownership_name(p.questFlags), ownership_name(p.sceneRoom),
        ownership_name(p.timeOfDay), advance_name(p.timeAdvance), p.allowJoinPartner ? 1 : 0,
        ownership_name(p.health), ownership_name(p.transform), death_name(p.onDeath));
}

void on_host_started() {
    g_mpPolicy.rupees = getSettings().multiplayer.sharedWallet.getValue() ? Ownership::Shared :
                                                                            Ownership::Individual;
    Log.info("[mp-policy] hosting with: {}", describe(g_mpPolicy));
}

void send_to(std::uint32_t playerId) {
    NetworkManager& net = network_manager();
    if (net.role() != Role::Host) {
        return;
    }
    const Writer w = build_packet();
    net.send_to_player(playerId, w, kChannelControl, true);
    Log.info("[mp-policy] host: PolicySync -> player {} ({} bytes): {}", playerId, w.size(),
        describe(g_mpPolicy));
}

bool host_apply(const MultiplayerPolicy& next, const char* why) {
    NetworkManager& net = network_manager();
    const char* reason = why != nullptr ? why : "";
    if (net.role() == Role::Client) {
        Log.warn("[mp-policy] only the host can change the session's policy ({}); ignored", reason);
        return false;
    }
    const MultiplayerPolicy before = g_mpPolicy;
    g_mpPolicy = next;
    Log.info("[mp-policy] {}: policy changed ({}):{}",
        net.role() == Role::Host ? "host" : "offline", reason, describe_changes(before, next));
    if (net.role() != Role::Host) {
        return true;
    }
    // Rebase first, so the wallet the guests are told to install is the rebased one, and so the
    // next diff (this tick's post_actor_tick) sees no change caused by the switch itself.
    world_sync::host_on_policy_changed(before);
    const Writer w = build_packet();
    net.send_to_session(w, kChannelControl, true);
    Log.info("[mp-policy] host: PolicySync -> everyone ({} bytes, wallet {}): {}", w.size(),
        world_sync::host_wallet(), describe(g_mpPolicy));
    return true;
}

bool host_set_rupees(Ownership rupees, const char* why) {
    MultiplayerPolicy next = g_mpPolicy;
    next.rupees = rupees;
    return host_apply(next, why);
}

void on_packet(Reader& r) {
    NetworkManager& net = network_manager();
    if (net.role() != Role::Client) {
        Log.error("[mp-policy] PolicySync arrived at a {}; ignored",
            net.role() == Role::Host ? "host" : "machine not in a session");
        return;
    }
    std::uint8_t layout = 0;
    std::uint8_t n = 0;
    if (!r.read_u8(layout) || !r.read_u8(n)) {
        Log.error("[mp-policy] malformed PolicySync; keeping the current policy");
        return;
    }
    std::array<std::uint8_t, 256> raw{};
    for (std::uint16_t i = 0; i < n; ++i) {
        if (!r.read_u8(raw[i])) {
            Log.error("[mp-policy] truncated PolicySync ({} of {} fields); keeping the current "
                      "policy",
                i, n);
            return;
        }
    }
    std::int32_t hostWallet = -1;
    if (!r.read_s32(hostWallet) || !r.ok()) {
        Log.error("[mp-policy] PolicySync without its wallet; keeping the current policy");
        return;
    }
    if (layout != kLayout) {
        // Same protocol version, so the fields still mean what they say; a later layout only
        // appends after the wallet.
        Log.warn("[mp-policy] PolicySync layout {} (ours {}); reading the fields we know", layout,
            kLayout);
    }

    if (!s_haveLocal) {
        s_localPolicy = g_mpPolicy;
        s_haveLocal = true;
    }
    MultiplayerPolicy next = g_mpPolicy;
    const std::uint8_t known = n < kFieldCount ? n : kFieldCount;
    for (std::uint8_t i = 0; i < known; ++i) {
        if (!decode(next, static_cast<Field>(i), raw[i])) {
            Log.warn("[mp-policy] PolicySync field {} has value {}, out of range; keeping ours", i,
                raw[i]);
        }
    }
    if (n < kFieldCount) {
        Log.warn("[mp-policy] host sent {} of our {} policy fields; the rest keep this machine's "
                 "values",
            n, kFieldCount);
    }

    const MultiplayerPolicy before = g_mpPolicy;
    g_mpPolicy = next;
    Log.info("[mp-policy] from host: {} (wallet {}); changes:{}", describe(g_mpPolicy), hostWallet,
        describe_changes(before, next));
    world_sync::guest_on_policy_changed(before, hostWallet);
}

void reset() {
    if (!s_haveLocal) {
        return;
    }
    g_mpPolicy = s_localPolicy;
    s_haveLocal = false;
    Log.info(
        "[mp-policy] session over: this machine's own policy is back: {}", describe(g_mpPolicy));
}

bool from_host() {
    return s_haveLocal;
}

}  // namespace dusk::mp::policy_sync
