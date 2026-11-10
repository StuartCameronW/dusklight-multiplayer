#include "item_claims.hpp"

#include <string>
#include <unordered_map>

#include "../policy/mp_policy.hpp"
#include "../session/network_manager.hpp"
#include "d/d_com_inf_game.h"
#include "d/d_save.h"
#include "dusk/logging.h"
#include "dusk/multiplayer.hpp"
#include "fmt/format.h"
#include "world_bridge.hpp"

/**
 * Chest / floor-item claims. Design and wire format: item_claims.hpp. Spec:
 * .claude/future-plans/mp-m4-m5-world.md II.6 (H3-H5).
 */

namespace dusk::mp {
namespace item_claims {
namespace {

constexpr borealis::Log Log{"dusk::mp"};

/// Guest: entries nobody has asked about for this long are dropped (memory hygiene only).
constexpr std::uint64_t kForgetTicks = 900;

std::uint32_t key_of(const ClaimTarget& t) {
    return (static_cast<std::uint32_t>(t.kind) << 24) | (static_cast<std::uint32_t>(t.tbl) << 16) |
           t.no;
}

ClaimTarget target_of(std::uint32_t key) {
    ClaimTarget t;
    t.kind = static_cast<ClaimKind>((key >> 24) & 0xFF);
    t.tbl = static_cast<std::uint8_t>((key >> 16) & 0xFF);
    t.no = static_cast<std::uint16_t>(key & 0xFFFF);
    return t;
}

/// "tbox 0/60" or "item 0/0x9E" (the item as the save bit scripts and onItem use).
std::string describe(const ClaimTarget& t) {
    if (t.kind == ClaimKind::Item) {
        return fmt::format("item {}/0x{:02X}", static_cast<int>(t.tbl), kFirstSavedItemBit + t.no);
    }
    return fmt::format("tbox {}/{}", static_cast<int>(t.tbl), static_cast<int>(t.no));
}

std::uint64_t now_tick() {
    return network_manager().sim_tick();
}

bool is_host() {
    return network_manager().role() == Role::Host;
}

/// The bit as THIS machine's save has it: mMemory when the table is the live one (where the game
/// writes it), mSave[tbl] otherwise (world sync writes both; see world_bridge apply_to_engine).
bool bit_set_here(const ClaimTarget& t) {
    if (dComIfGs_getSaveData() == nullptr) {
        return false;
    }
    dSv_info_c& in = g_dComIfG_gameInfo.info;
    dSv_memBit_c& bit = static_cast<int>(t.tbl) == world_bridge::live_tbl() ?
                            in.getMemory().getBit() :
                            in.getSavedata().getSave(t.tbl).getBit();
    return t.kind == ClaimKind::Tbox ? bit.isTbox(t.no) != FALSE : bit.isItem(t.no) != FALSE;
}

// --- host ----------------------------------------------------------------------------------------

struct Lease {
    std::uint32_t holder = 0;
    std::uint64_t expiry = 0;
};

/// What was last logged for a target judged on the host, so a claim made every tick logs once per
/// change instead of 30 times a second.
struct HostLogged {
    ClaimState state = ClaimState::Pending;
    DenyReason reason = DenyReason::None;
    std::uint32_t player = 0;
};

// --- guest ---------------------------------------------------------------------------------------

struct GuestEntry {
    /// Pending: a request is out and nothing is known. Granted / Denied: the last verdict.
    ClaimState state = ClaimState::Pending;
    DenyReason reason = DenyReason::None;
    /// Serial of the newest request sent for this target; replies to older ones are ignored.
    std::uint32_t serial = 0;
    std::uint64_t sentTick = 0;
    std::uint64_t resolvedTick = 0;
    std::uint64_t lastAskTick = 0;
    /// A renewal of a held grant is out (state stays Granted meanwhile).
    bool renewing = false;
    /// The first request after a fresh start went out at this tick (for "after N ticks" logs).
    std::uint64_t firstAskTick = 0;
    /// The latest verdict was "host not ready": keep returning Pending until the retry.
    bool retryNotReady = false;
};

struct State {
    std::unordered_map<std::uint32_t, Lease> leases;
    std::unordered_map<std::uint32_t, HostLogged> hostLogged;
    std::unordered_map<std::uint32_t, GuestEntry> guest;
    /// Last reason a Denied was returned for, per target (both roles), for last_reason().
    std::unordered_map<std::uint32_t, DenyReason> lastDenied;
    std::uint32_t nextSerial = 1;
    std::uint64_t lastPruneTick = 0;
};

State& st() {
    static State s;
    return s;
}

/// Host: the verdict for @p player on @p t, recording the lease when granted.
ClaimState host_judge(const ClaimTarget& t, std::uint32_t player, DenyReason& reason,
    std::uint32_t& holder) {
    State& s = st();
    const std::uint32_t key = key_of(t);
    const std::uint64_t now = now_tick();
    reason = DenyReason::None;
    holder = player;

    if (!active()) {
        reason = DenyReason::Invalid;
        return ClaimState::Denied;
    }
    if (!world_bridge::world_ready()) {
        // Title, file select or mid stage change: nothing reliable to judge against.
        reason = DenyReason::NotReady;
        return ClaimState::Denied;
    }
    if (bit_set_here(t)) {
        s.leases.erase(key);
        reason = DenyReason::AlreadySet;
        return ClaimState::Denied;
    }
    auto it = s.leases.find(key);
    if (it != s.leases.end() && it->second.holder != player && now < it->second.expiry) {
        reason = DenyReason::HeldByOther;
        holder = it->second.holder;
        return ClaimState::Denied;
    }
    s.leases[key] = Lease{player, now + kLeaseTicks};
    return ClaimState::Granted;
}

/// Log a host-side verdict once per change of (state, reason, player) for that target.
void host_log(const ClaimTarget& t, std::uint32_t player, ClaimState state, DenyReason reason,
    std::uint32_t holder, std::uint32_t serial) {
    State& s = st();
    const std::uint32_t key = key_of(t);
    auto it = s.hostLogged.find(key);
    if (it != s.hostLogged.end() && it->second.state == state && it->second.reason == reason &&
        it->second.player == player)
    {
        return;
    }
    s.hostLogged[key] = HostLogged{state, reason, player};
    const std::string who =
        player == kHostPlayerId ? std::string("host") : fmt::format("player {}", player);
    const std::string via = serial != 0 ? fmt::format(" serial={}", serial) : std::string(" local");
    if (state == ClaimState::Granted) {
        Log.info("[mp-claim] granted {} to {}{} (lease {} ticks) tick={}", describe(t), who, via,
            kLeaseTicks, now_tick());
    } else if (reason == DenyReason::HeldByOther) {
        Log.info("[mp-claim] denied {} to {}{} (held by player {}) tick={}", describe(t), who, via,
            holder, now_tick());
    } else {
        Log.info("[mp-claim] denied {} to {}{} ({}) tick={}", describe(t), who, via,
            reason_name(reason), now_tick());
    }
}

void guest_send_request(const ClaimTarget& t, GuestEntry& e) {
    State& s = st();
    e.serial = s.nextSerial++;
    e.sentTick = now_tick();
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::ClaimRequest));
    w.write_u8(static_cast<std::uint8_t>(t.kind));
    w.write_u8(t.tbl);
    w.write_u16(t.no);
    w.write_u32(e.serial);
    network_manager().send_to_player(kHostPlayerId, w, kChannelControl, true);
    Log.info("[mp-claim] request {} serial={}{} tick={}", describe(t), e.serial,
        e.renewing ? " (renewing a held grant)" : "", e.sentTick);
}

/// Guest: start a fresh request for @p t (no verdict held).
ClaimState guest_start(const ClaimTarget& t, GuestEntry& e) {
    e.state = ClaimState::Pending;
    e.reason = DenyReason::None;
    e.renewing = false;
    e.retryNotReady = false;
    e.firstAskTick = now_tick();
    guest_send_request(t, e);
    return ClaimState::Pending;
}

ClaimState guest_claim(const ClaimTarget& t) {
    State& s = st();
    const std::uint32_t key = key_of(t);
    const std::uint64_t now = now_tick();

    // Our own copy of the host's world already has it: no need to ask anyone.
    if (bit_set_here(t)) {
        s.guest.erase(key);
        s.lastDenied[key] = DenyReason::AlreadySet;
        return ClaimState::Denied;
    }

    auto found = s.guest.find(key);
    if (found == s.guest.end()) {
        GuestEntry& e = s.guest[key];
        e.lastAskTick = now;
        return guest_start(t, e);
    }
    GuestEntry& e = found->second;
    e.lastAskTick = now;

    switch (e.state) {
    case ClaimState::Pending:
        if (e.retryNotReady) {
            if (now - e.resolvedTick >= kDeniedRetryTicks) {
                return guest_start(t, e);
            }
            return ClaimState::Pending;
        }
        if (now - e.sentTick > kTimeoutTicks) {
            // tick() normally gets here first; this is the same transition for a caller that is
            // faster than it.
            e.state = ClaimState::Denied;
            e.reason = DenyReason::Timeout;
            e.resolvedTick = now;
            Log.warn("[mp-claim] timeout {} serial={}: no reply in {} ticks; denied (never "
                     "granted on a timeout) tick={}",
                describe(t), e.serial, now - e.sentTick, now);
            s.lastDenied[key] = DenyReason::Timeout;
            return ClaimState::Denied;
        }
        return ClaimState::Pending;

    case ClaimState::Granted: {
        const std::uint64_t age = now - e.resolvedTick;
        if (age >= kGrantTrustTicks) {
            // The renewal never came back in time. The host's lease may be about to lapse, so
            // stop trusting the grant and ask from scratch.
            Log.info("[mp-claim] grant on {} expired after {} ticks without a renewal; asking "
                     "again tick={}",
                describe(t), age, now);
            return guest_start(t, e);
        }
        if (age >= kGrantRenewTicks && !e.renewing) {
            e.renewing = true;
            guest_send_request(t, e);
        }
        return ClaimState::Granted;
    }

    case ClaimState::Denied:
        if (now - e.resolvedTick >= kDeniedRetryTicks) {
            return guest_start(t, e);
        }
        s.lastDenied[key] = e.reason;
        return ClaimState::Denied;
    }
    return ClaimState::Pending;
}

void host_on_request(Reader& r, std::uint32_t fromPlayerId) {
    std::uint8_t kind = 0;
    std::uint8_t tbl = 0;
    std::uint16_t no = 0;
    std::uint32_t serial = 0;
    if (!r.read_u8(kind) || !r.read_u8(tbl) || !r.read_u16(no) || !r.read_u32(serial)) {
        Log.warn("[mp-claim] host: truncated ClaimRequest from player {}; dropped", fromPlayerId);
        return;
    }

    ClaimTarget t;
    t.kind = static_cast<ClaimKind>(kind);
    t.tbl = tbl;
    t.no = no;

    ClaimState verdict = ClaimState::Denied;
    DenyReason reason = DenyReason::Invalid;
    std::uint32_t holder = fromPlayerId;
    if (kind > static_cast<std::uint8_t>(ClaimKind::Item) || !target_valid(t)) {
        Log.warn("[mp-claim] host: player {} asked for an invalid target kind={} tbl={} no={}; "
                 "denied",
            fromPlayerId, static_cast<int>(kind), static_cast<int>(tbl), static_cast<int>(no));
    } else {
        verdict = host_judge(t, fromPlayerId, reason, holder);
        host_log(t, fromPlayerId, verdict, reason, holder, serial);
    }

    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::ClaimReply));
    w.write_u8(kind);
    w.write_u8(tbl);
    w.write_u16(no);
    w.write_u32(serial);
    w.write_u8(verdict == ClaimState::Granted ? 1 : 0);
    w.write_u8(static_cast<std::uint8_t>(reason));
    network_manager().send_to_player(fromPlayerId, w, kChannelControl, true);
}

void guest_on_reply(Reader& r) {
    std::uint8_t kind = 0;
    std::uint8_t tbl = 0;
    std::uint16_t no = 0;
    std::uint32_t serial = 0;
    std::uint8_t granted = 0;
    std::uint8_t reasonByte = 0;
    if (!r.read_u8(kind) || !r.read_u8(tbl) || !r.read_u16(no) || !r.read_u32(serial) ||
        !r.read_u8(granted) || !r.read_u8(reasonByte))
    {
        Log.warn("[mp-claim] truncated ClaimReply; dropped");
        return;
    }
    ClaimTarget t;
    t.kind = static_cast<ClaimKind>(kind);
    t.tbl = tbl;
    t.no = no;
    if (kind > static_cast<std::uint8_t>(ClaimKind::Item) || !target_valid(t) || granted > 1 ||
        reasonByte > static_cast<std::uint8_t>(DenyReason::Timeout))
    {
        Log.warn("[mp-claim] ClaimReply out of range kind={} tbl={} no={} granted={} reason={}; "
                 "dropped",
            static_cast<int>(kind), static_cast<int>(tbl), static_cast<int>(no),
            static_cast<int>(granted), static_cast<int>(reasonByte));
        return;
    }

    State& s = st();
    const std::uint32_t key = key_of(t);
    auto it = s.guest.find(key);
    if (it == s.guest.end() || it->second.serial != serial) {
        // Superseded by a newer request, or forgotten (session reset). The newer reply decides.
        Log.info("[mp-claim] stale reply for {} serial={} ignored", describe(t), serial);
        return;
    }
    GuestEntry& e = it->second;
    const std::uint64_t now = now_tick();
    const DenyReason reason = static_cast<DenyReason>(reasonByte);
    const bool wasRenewal = e.renewing;
    e.renewing = false;

    if (granted != 0) {
        e.state = ClaimState::Granted;
        e.reason = DenyReason::None;
        e.resolvedTick = now;
        e.retryNotReady = false;
        if (!wasRenewal) {
            Log.info("[mp-claim] granted {} serial={} after {} ticks tick={}", describe(t), serial,
                now - e.firstAskTick, now);
        }
        return;
    }

    if (reason == DenyReason::NotReady) {
        // Not a verdict: the host had no world to judge with. Stay Pending and ask again shortly.
        e.state = ClaimState::Pending;
        e.retryNotReady = true;
        e.resolvedTick = now;
        Log.info("[mp-claim] denied {} serial={} (host not ready; asking again in {} ticks) "
                 "tick={}",
            describe(t), serial, kDeniedRetryTicks, now);
        return;
    }

    e.state = ClaimState::Denied;
    e.reason = reason;
    e.resolvedTick = now;
    e.retryNotReady = false;
    s.lastDenied[key] = reason;
    Log.info("[mp-claim] denied {} serial={} ({}){} after {} ticks tick={}", describe(t), serial,
        reason_name(reason), wasRenewal ? " on renewal" : "", now - e.firstAskTick, now);
}

}  // namespace

// --- public --------------------------------------------------------------------------------------

bool target_valid(const ClaimTarget& t) {
    if (t.tbl >= kStageTables) {
        return false;
    }
    switch (t.kind) {
    case ClaimKind::Tbox:
        return t.no < kTboxBits;
    case ClaimKind::Item:
        return t.no < kItemBits;
    }
    return false;
}

bool active() {
    const NetworkManager& net = network_manager();
    return net.is_active() && net.session_live() && g_mpPolicy.questFlags == Ownership::Shared;
}

ClaimState claim(const ClaimTarget& t) {
    if (!target_valid(t)) {
        return ClaimState::Denied;
    }
    if (!active()) {
        return ClaimState::Granted;
    }
    if (is_host()) {
        DenyReason reason = DenyReason::None;
        std::uint32_t holder = kHostPlayerId;
        ClaimState verdict = host_judge(t, kHostPlayerId, reason, holder);
        if (verdict == ClaimState::Denied && reason == DenyReason::NotReady) {
            // The host's own actors only run with a world, so this is a transient; don't deny.
            verdict = ClaimState::Pending;
        }
        host_log(t, kHostPlayerId, verdict, reason, holder, 0);
        if (verdict == ClaimState::Denied) {
            st().lastDenied[key_of(t)] = reason;
        }
        return verdict;
    }
    return guest_claim(t);
}

void on_packet(PacketId id, Reader& r, std::uint32_t fromPlayerId) {
    const Role role = network_manager().role();
    if (id == PacketId::ClaimRequest) {
        if (role != Role::Host) {
            Log.error("[mp-claim] ClaimRequest from player {} on a non-host; ignored", fromPlayerId);
            return;
        }
        host_on_request(r, fromPlayerId);
    } else if (id == PacketId::ClaimReply) {
        if (role != Role::Client) {
            Log.error("[mp-claim] ClaimReply from player {} on a non-guest; ignored", fromPlayerId);
            return;
        }
        guest_on_reply(r);
    }
}

void tick() {
    if (!active()) {
        return;
    }
    State& s = st();
    const std::uint64_t now = now_tick();

    if (is_host()) {
        if (now - s.lastPruneTick < 30) {
            return;
        }
        s.lastPruneTick = now;
        for (auto it = s.leases.begin(); it != s.leases.end();) {
            const ClaimTarget t = target_of(it->first);
            const bool expired = now >= it->second.expiry;
            const bool taken = world_bridge::world_ready() && bit_set_here(t);
            if (expired || taken) {
                Log.info("[mp-claim] lease on {} held by player {} ended ({}) tick={}", describe(t),
                    it->second.holder, taken ? "its bit is set" : "expired unused", now);
                s.hostLogged.erase(it->first);
                it = s.leases.erase(it);
            } else {
                ++it;
            }
        }
        return;
    }

    for (auto it = s.guest.begin(); it != s.guest.end();) {
        GuestEntry& e = it->second;
        const ClaimTarget t = target_of(it->first);
        if (e.state == ClaimState::Pending && !e.retryNotReady && now - e.sentTick > kTimeoutTicks)
        {
            e.state = ClaimState::Denied;
            e.reason = DenyReason::Timeout;
            e.resolvedTick = now;
            s.lastDenied[it->first] = DenyReason::Timeout;
            Log.warn("[mp-claim] timeout {} serial={}: no reply in {} ticks; denied (never "
                     "granted on a timeout) tick={}",
                describe(t), e.serial, now - e.sentTick, now);
        }
        if (now - e.lastAskTick > kForgetTicks) {
            it = s.guest.erase(it);
        } else {
            ++it;
        }
    }
}

void on_peer_left(std::uint32_t playerId) {
    State& s = st();
    for (auto it = s.leases.begin(); it != s.leases.end();) {
        if (it->second.holder == playerId) {
            Log.info("[mp-claim] lease on {} released: player {} left", describe(target_of(it->first)),
                playerId);
            s.hostLogged.erase(it->first);
            it = s.leases.erase(it);
        } else {
            ++it;
        }
    }
}

void reset() {
    State& s = st();
    s.leases.clear();
    s.hostLogged.clear();
    s.guest.clear();
    s.lastDenied.clear();
    s.lastPruneTick = 0;
    // nextSerial keeps counting: a reply to a request from the previous session can never match.
}

const char* state_name(ClaimState s) {
    switch (s) {
    case ClaimState::Granted:
        return "granted";
    case ClaimState::Denied:
        return "denied";
    case ClaimState::Pending:
        return "pending";
    }
    return "?";
}

const char* reason_name(DenyReason r) {
    switch (r) {
    case DenyReason::None:
        return "none";
    case DenyReason::AlreadySet:
        return "already set";
    case DenyReason::HeldByOther:
        return "held by another player";
    case DenyReason::Invalid:
        return "invalid";
    case DenyReason::NotReady:
        return "host not ready";
    case DenyReason::Timeout:
        return "timeout";
    }
    return "?";
}

const char* kind_name(ClaimKind k) {
    return k == ClaimKind::Tbox ? "tbox" : "item";
}

DenyReason last_reason(const ClaimTarget& t) {
    const auto& m = st().lastDenied;
    auto it = m.find(key_of(t));
    return it != m.end() ? it->second : DenyReason::None;
}

}  // namespace item_claims

// --- decomp hooks (declared in dusk/multiplayer.hpp) ---------------------------------------------

namespace {

constexpr borealis::Log HookLog{"dusk::mp"};

int to_result(item_claims::ClaimState s) {
    switch (s) {
    case item_claims::ClaimState::Granted:
        return kClaimGranted;
    case item_claims::ClaimState::Denied:
        return kClaimDenied;
    case item_claims::ClaimState::Pending:
        return kClaimPending;
    }
    return kClaimPending;
}

}  // namespace

bool claims_active() {
    return item_claims::active();
}

int claim_tbox(int tboxNo) {
    if (!item_claims::active()) {
        return kClaimGranted;
    }
    const int tbl = world_bridge::live_tbl();
    if (tbl < 0 || tboxNo < 0 || tboxNo >= item_claims::kTboxBits) {
        // No stage table owns mMemory (mid stage change); actors don't run then, so this is only
        // reachable by a bug. Not offering the chest for a tick is the safe answer.
        return kClaimPending;
    }
    item_claims::ClaimTarget t;
    t.kind = item_claims::ClaimKind::Tbox;
    t.tbl = static_cast<std::uint8_t>(tbl);
    t.no = static_cast<std::uint16_t>(tboxNo);
    return to_result(item_claims::claim(t));
}

int claim_item(int itemBitNo) {
    if (!item_claims::active()) {
        return kClaimGranted;
    }
    // Only the saved item bits (0x80-0x9F) live in the host's save and can be judged there. Dungeon
    // (mDan) and zone temps are per-machine state the host may not even have loaded; those items
    // keep vanilla behaviour (H5's re-check in execute still removes them once the bit arrives).
    const int index = itemBitNo - item_claims::kFirstSavedItemBit;
    if (index < 0 || index >= item_claims::kItemBits) {
        return kClaimGranted;
    }
    const int tbl = world_bridge::live_tbl();
    if (tbl < 0) {
        return kClaimPending;
    }
    item_claims::ClaimTarget t;
    t.kind = item_claims::ClaimKind::Item;
    t.tbl = static_cast<std::uint8_t>(tbl);
    t.no = static_cast<std::uint16_t>(index);
    return to_result(item_claims::claim(t));
}

void claims_note_tbox_opened(int tboxNo, bool eventAlreadyStarted) {
    const int tbl = world_bridge::live_tbl();
    if (eventAlreadyStarted) {
        HookLog.warn("[mp-claim] tbox {}/{} opened by partner, but my own open event already started: "
                 "letting it run (a softlock is worse; this is a duplicate the claim should have "
                 "prevented) tick={}",
            tbl, tboxNo, network_manager().sim_tick());
        return;
    }
    HookLog.info("[mp-claim] tbox {}/{} opened by partner: showing it open tick={}", tbl, tboxNo,
        network_manager().sim_tick());
}

void claims_note_item_removed(int itemBitNo, bool takenByPartner) {
    const int tbl = world_bridge::live_tbl();
    if (takenByPartner) {
        HookLog.info("[mp-claim] item {}/0x{:02X} taken by partner: removing tick={}", tbl, itemBitNo,
            network_manager().sim_tick());
    } else {
        HookLog.info("[mp-claim] item {}/0x{:02X} claim denied: removing without granting tick={}",
            tbl, itemBitNo, network_manager().sim_tick());
    }
}

}  // namespace dusk::mp
