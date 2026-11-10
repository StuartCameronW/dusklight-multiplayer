#pragma once

#include <cstdint>

#include "../net/packet.hpp"
#include "../net/serializer.hpp"

/**
 * Chest and floor-item claims (Batch W-B, protocol 23): the duplicate-item trap of
 * .claude/future-plans/mp-m4-m5-world.md II.6, hooks H4 and H5.
 *
 * World sync keeps the FLAGS consistent, but an item can still be taken twice if two players reach
 * it before either one's flag has crossed the wire. A claim closes that window: before a chest is
 * offered ("Open" prompt) or a bit-tracked floor item is picked up, the player asks the host for
 * it, and only one player at a time can hold it.
 *
 * ★ A GRANT IS A LEASE, NOT A FOREVER. The host remembers who holds each target until
 * kLeaseTicks after that holder last asked, or until the target's save bit is set in the host's
 * save, whichever comes first. A chest is claimed when its prompt would be offered (Link right in
 * front of it, d_a_tbox.cpp boxCheck), so a permanent grant would let a player who merely stood at
 * a chest and walked away lock it for the whole session. The lease is long enough to cover the
 * holder's open event up to the moment OpenInit_com sets the bit, and that bit then reaches the
 * host by the ordinary world proposal, after which the claim is moot ("already set").
 *
 * Host: claim() is judged on the spot. Guest: the first call sends ClaimRequest (17) and returns
 * Pending; ClaimReply (18) records the verdict. A guest re-asks in the background while it keeps
 * claiming (so a held grant never lapses under a player who is still standing there), and trusts a
 * grant for kGrantTrustTicks, which is shorter than the host's lease by more than any sane RTT.
 * No reply within kTimeoutTicks is a DENIAL, never a grant: no duplicate is better than a
 * duplicate.
 *
 * Wire layouts (reliable, kChannelControl):
 *   ClaimRequest  guest -> host  [u8 kind][u8 tbl][u16 no][u32 serial]
 *   ClaimReply    host -> guest  [u8 kind][u8 tbl][u16 no][u32 serial][u8 granted][u8 reason]
 * `no` is the tbox number (0-63) for a chest and the dSv_memBit_c item index (0-31, the save bit
 * dSv_info_c::onItem numbers 0x80 + no) for a floor item. `reason` is a DenyReason.
 *
 * Where the hooks are (all TARGET_PC):
 *   - chests, daTbox_c (d_a_tbox.cpp): H3 re-check in actionOpenWait, H4 claim before the chest is
 *     offered (boxCheck branch).
 *   - floor items with a saved item bit, daItem_c (d_a_obj_item.cpp): H5 re-check in execute,
 *     claim in itemGetNextExecute.
 *   - heart pieces, daObjLife_c (d_a_obj_life_container.cpp): H5 re-check in execute, claim in
 *     initActionOrderGetDemo, renewed through the get demo (its bit is only set when the demo
 *     ENDS, so the lease must outlive a slow read of the text box).
 *   - floor small keys, daKey_c (d_a_obj_smallkey.cpp): H5 re-check only (their "taken" state is
 *     a TBOX bit). OPEN: no claim at the grab yet. The pickup starts inside Link's grab
 *     (fopAcM_cancelCarryNow + getGrabUpStart), and holding it for a round trip there needs a
 *     runtime test of what Link's grab proc does with a cancelled carry. Proposed: claim (kind
 *     Tbox) while Link is within reach and only raise fopAc_AttnFlag_CARRY_e once granted, the way
 *     the chest gates its prompt.
 *
 * Outside a live session with shared quest flags, nothing here runs and the hooks change nothing.
 * Game thread only.
 */

namespace dusk::mp::item_claims {

enum class ClaimKind : std::uint8_t {
    Tbox = 0,
    Item = 1,
};

enum class ClaimState : std::uint8_t {
    Granted,
    Denied,
    Pending,
};

/// Why a claim was denied. On the wire (ClaimReply's last byte); values are append-only.
enum class DenyReason : std::uint8_t {
    None = 0,         ///< granted
    AlreadySet = 1,   ///< the save bit is already set: someone has it
    HeldByOther = 2,  ///< another player holds the lease right now
    Invalid = 3,      ///< out of range, or claims are not in force on the host
    NotReady = 4,     ///< the host has no world loaded to judge against; asked again shortly
    Timeout = 5,      ///< local only: the host never answered
};

struct ClaimTarget {
    ClaimKind kind = ClaimKind::Tbox;
    /// Stage table, 0..31 (dSv_save_c::STAGE_MAX).
    std::uint8_t tbl = 0;
    /// Tbox number 0..63, or memBit item index 0..31 (see the file comment).
    std::uint16_t no = 0;
};

/// Number of stage tables, chests per table and saved item bits per table (dSv_memBit_c).
inline constexpr int kStageTables = 32;
inline constexpr int kTboxBits = 64;
inline constexpr int kItemBits = 32;
/// dSv_info_c::MEMORY_ITEM: floor-item save bits below this are dungeon temps (mDan), above the
/// saved range zone temps. Only the saved range (0x80-0x9F) is claimed.
inline constexpr int kFirstSavedItemBit = 0x80;

/// Host: how long a grant stays held after its holder last asked (10 s at 30 Hz).
inline constexpr std::uint64_t kLeaseTicks = 300;
/// Guest: how long a grant is trusted after the reply that gave it, and when to renew it.
inline constexpr std::uint64_t kGrantTrustTicks = 120;
inline constexpr std::uint64_t kGrantRenewTicks = 45;
/// Guest: no reply after this long (3 s) is a denial.
inline constexpr std::uint64_t kTimeoutTicks = 90;
/// Guest: a denial (or a "host not ready") is remembered this long before asking again.
inline constexpr std::uint64_t kDeniedRetryTicks = 30;

/// True for a target whose fields are in range.
bool target_valid(const ClaimTarget& t);

/// Claims are in force: a live session (host, or a guest the host has accepted) whose quest flags
/// (chests, item bits) are Shared. False means "behave exactly like vanilla".
bool active();

/// Ask for @p t. Cheap; meant to be called every tick while the player is at the chest or touching
/// the item. Outside active() it returns Granted (vanilla behaviour), and Denied for an invalid
/// target.
ClaimState claim(const ClaimTarget& t);

/// ClaimRequest on the host, ClaimReply on a guest. Validates ranges; a malformed packet is dropped.
void on_packet(PacketId id, Reader& r, std::uint32_t fromPlayerId);

/// Once per sim tick while the session is live: guest timeouts (logged even if nobody asks again),
/// host lease expiry.
void tick();

/// Host: a guest left; its leases go with it.
void on_peer_left(std::uint32_t playerId);

/// Session over: forget every lease and every pending claim.
void reset();

const char* state_name(ClaimState s);
const char* reason_name(DenyReason r);
const char* kind_name(ClaimKind k);

/// The reason behind the most recent Denied that claim() returned for @p t on this end, or None.
DenyReason last_reason(const ClaimTarget& t);

}  // namespace dusk::mp::item_claims
