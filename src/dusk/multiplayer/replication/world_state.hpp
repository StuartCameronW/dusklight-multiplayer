#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "dusk/multiplayer/net/serializer.hpp"

/**
 * The shared world (M5): a canonical, engine-free copy of the save flags both players share, and
 * the ops that move changes between machines. Spec: .claude/future-plans/mp-m4-m5-world.md Part II.
 *
 * ★ DETECTION IS BY DIFF, NOT BY HOOKING SETTERS. About 60 writers touch these flags, plus raw
 * struct copies (getSave/putSave, card_to_memory), and there is no single choke point. So each tick
 * the bridge (world_bridge.cpp) copies the engine's flags into a WorldView, diffs it against the
 * last view (the "shadow"), and turns every changed byte into an op. The world clock proved the
 * same trick (15-world-clock.md section 3).
 *
 * ★ APPLYING A REMOTE OP MUST ALSO UPDATE THE SHADOW. Otherwise the next diff sees the remote change
 * as a local one and sends it back, forever. apply_op() therefore takes the view to write AND the
 * shadow, and writes both.
 *
 * ★ COUNTERS ARE NOT BITS. Rupees, small keys, max life and poe souls travel as deltas plus the
 * resulting absolute value. Sending absolutes loses one of two keys picked up within one round trip
 * (a dungeon softlock); sending deltas double-counts duplicates, which are stopped at the chest or
 * item instead (Batch W-B), never by de-duplicating counters afterwards.
 *
 * Everything here is engine-free: raw byte arrays whose sizes the bridge static_asserts against the
 * engine's own structs. Bytes are stored exactly as the engine holds them (some engine fields are
 * BE-wrapped); bit diffs work on raw bytes, so byte order doesn't matter for them. Counter values in
 * this header are host-native integers; the bridge converts.
 */

namespace dusk::mp::world {

// --- sizes (the bridge static_asserts each against the engine type) -------------------------------

/// dSv_event_c::mEvent (d_save.h:695+).
constexpr std::size_t kEventBytes = 256;
/// dSv_memory_c::mBit, one dSv_memBit_c per stage table (STAGE_MAX = 32, d_save.h:619+).
constexpr std::size_t kMemTbls = 32;
constexpr std::size_t kMemBitBytes = 0x20;
/// Inside dSv_memBit_c: mKeyNum is a counter, not bits. It is masked out of the bit diff and
/// diffed as the KeyNum counter instead.
constexpr std::size_t kMemKeyNumOffset = 0x1C;
/// Stage table 6's key count is forced to 0 or 2 on EVERY setNextStage (d_com_inf_game.cpp:1453-1459)
/// and in card_to_memory (d_save.cpp:1893), on both machines. Never replicate it, or every warp
/// would send the partner a key-count correction.
constexpr std::uint8_t kLocalOnlyKeyTbl = 6;
/// dSv_memory2_c (visited rooms), STAGE2_MAX = 64.
constexpr std::size_t kMem2Tbls = 64;
constexpr std::size_t kMem2Bytes = 8;
/// dSv_player_get_item_c::mItemFlags (first-get bits, 256 bits).
constexpr std::size_t kFirstGetBytes = 32;
/// dSv_player_item_c::mItems (which item is in each of the 24 slots).
constexpr std::size_t kItemSlots = 24;
/// dSv_player_item_max_c::mItemMax (quiver / bomb bag capacities).
constexpr std::size_t kItemMaxBytes = 8;
/// dSv_player_collect_c::mItem (collection bits).
constexpr std::size_t kCollectBytes = 8;

// --- domains (wire values; never renumber) -------------------------------------------------------

enum class Domain : std::uint8_t {
    // bit domains: op = set/clear masks on one byte
    Event = 0,     ///< view.event; tbl = 0xFF
    MemBit = 1,    ///< view.mem[tbl], keyNum byte excluded
    Mem2 = 2,      ///< view.mem2[tbl]
    FirstGet = 3,  ///< view.firstGet; tbl = 0xFF
    Items = 4,     ///< view.items, view.itemMax, view.collect, crystal, mirror, walletSize (see
                   ///< kPlayerBytes for the byte map); tbl = 0xFF
    Dan = 5,       ///< dungeon-temporary bits (mDan); tbl = dan stage table; index = byte, see
                   ///< "temporary domains" below. NOT saved world state.
    Zone = 6,      ///< room-temporary bits (mZone); tbl = stage table; index = room << 8 | byte.
                   ///< NOT saved world state.
    // counter domains: op = delta + resulting absolute
    Rupee = 16,    ///< tbl = 0xFF
    KeyNum = 17,   ///< tbl = stage table (never kLocalOnlyKeyTbl)
    MaxLife = 18,  ///< tbl = 0xFF
    Poh = 19,      ///< tbl = 0xFF
};

constexpr std::uint8_t kNoTbl = 0xFF;

inline bool is_counter(Domain d) { return static_cast<std::uint8_t>(d) >= 16; }

// --- the canonical view --------------------------------------------------------------------------

/// The player-subset bytes in the Items domain, in this order:
///   [0, 24) items, [24, 32) itemMax, [32, 40) collect, 40 crystal, 41 mirror, 42 walletSize.
/// Which of these are shared depends on MultiplayerPolicy (inventory); the bridge leaves fields
/// that are Individual at zero on BOTH sides so they never diff.
constexpr std::size_t kPlayerBytes = kItemSlots + kItemMaxBytes + kCollectBytes + 3;

struct WorldView {
    std::array<std::uint8_t, kEventBytes> event{};
    std::array<std::array<std::uint8_t, kMemBitBytes>, kMemTbls> mem{};
    std::array<std::array<std::uint8_t, kMem2Bytes>, kMem2Tbls> mem2{};
    std::array<std::uint8_t, kFirstGetBytes> firstGet{};
    std::array<std::uint8_t, kPlayerBytes> player{};

    // counters, host-native
    std::int32_t rupee = 0;
    std::int32_t maxLife = 0;
    std::int32_t poh = 0;
    // keyNum lives in mem[tbl][kMemKeyNumOffset]; read it with key_num(), write with set_key_num().

    std::int32_t key_num(std::uint8_t tbl) const { return mem[tbl][kMemKeyNumOffset]; }
    void set_key_num(std::uint8_t tbl, std::int32_t v) {
        mem[tbl][kMemKeyNumOffset] = static_cast<std::uint8_t>(v < 0 ? 0 : (v > 99 ? 99 : v));
    }
};

// --- ops -----------------------------------------------------------------------------------------

/**
 * One change. Wire encoding (write_op / read_op):
 *   bit domains:     [u8 domain][u8 tbl][u16 index][u8 set][u8 clr]
 *   counter domains: [u8 domain][u8 tbl][s16 delta][s32 resultAbs]
 * Applying a bit op: b = (b | set) & ~clr. That commutes for disjoint bits, so two players flipping
 * different switches in one byte merge correctly.
 */
struct WorldOp {
    Domain domain = Domain::Event;
    std::uint8_t tbl = kNoTbl;
    std::uint16_t index = 0;  ///< byte index within the domain's array (bit domains)
    std::uint8_t set = 0;
    std::uint8_t clr = 0;
    std::int16_t delta = 0;   ///< counter domains
    std::int32_t resultAbs = 0;
};

void write_op(Writer& w, const WorldOp& op);
bool read_op(Reader& r, WorldOp& out);
void write_ops(Writer& w, const std::vector<WorldOp>& ops);  ///< [u16 n][op]*n
bool read_ops(Reader& r, std::vector<WorldOp>& out);         ///< rejects n > kMaxOpsPerMessage
constexpr std::size_t kMaxOpsPerMessage = 1024;

// --- diff / apply --------------------------------------------------------------------------------

/// Appends one op per changed byte (bit domains) and one per changed counter, cur vs shadow.
/// Never emits the keyNum byte as a bit op, and never emits KeyNum for kLocalOnlyKeyTbl.
void diff(const WorldView& shadow, const WorldView& cur, std::vector<WorldOp>& out);

/// Applies one op to `view`, and the same op to `shadow` (see the header comment). For a counter,
/// writes resultAbs when `useAbsolute`, else adds delta (clamped: rupee 0..9999, maxLife 0..400,
/// poh 0..255, keyNum 0..99). Returns false for an op that is malformed (bad domain, tbl or index
/// out of range); nothing is written then.
bool apply_op(WorldView& view, WorldView& shadow, const WorldOp& op, bool useAbsolute);

/// Validity check without applying. Same rules as apply_op's return value for the saved domains;
/// for Dan/Zone it is the temp layout below (apply_op itself refuses Dan/Zone: they are not in the
/// WorldView).
bool op_in_range(const WorldOp& op);

// --- temporary domains: Dan and Zone (spec II.7, Batch W-B) --------------------------------------
//
// ★ NOT SAVED WORLD STATE. dSv_info_c::mDan and mZone live outside mSavedata, so they never reach a
// save file on any machine (memory_to_card writes mSavedata only). They are not in the WorldView,
// not in the digest, and not in write_view; WorldSync carries them as a separate trailer
// (write_temp_mirrors). A digest therefore never resyncs over them while players are in different
// stages.
//
// ★ SHARED ONLY BETWEEN CO-LOCATED PLAYERS. Each machine diffs only the table it is standing in
// (TempView). Every machine also keeps a TempMirror per stage table that someone occupies, fed by
// every Dan/Zone op; a player entering that table is seeded from it (hooks H6/H7). Who occupies
// what, and when a mirror is dropped, is the session layer's business (world_sync.cpp).
//
// ★ MIRROR-ONLY OPS. A machine that enters a table (or creates a room's zone) WITHOUT seeding from
// a mirror has fresh, vanilla-reset bits, and every other machine's mirror for it may be stale.
// It announces its whole state as ops with kTempMirrorOnly set in the index: receivers overwrite
// their MIRROR with it and never touch their engine. That is what makes "the bits reset only when
// everyone has left" hold on every machine, without one player's lifecycle ever clearing bits a
// partner is still standing on.

/// dSv_danBit_c::mSwitch[2] + mItem[4] (offsets 0x04..0x1C), raw engine bytes (host-native u32s,
/// so byte * 8 + bit is the switch / item number: 0-63 switches, 64-191 item bits 0-127).
/// mStageNo, unk1 and unk28[] are not replicated.
constexpr std::size_t kDanBytes = 24;
constexpr std::size_t kDanSwitchBytes = 8;
/// dSv_zoneBit_c up to (not including) unk12, raw engine bytes: six BE u16s, in order mSwitch[0],
/// mSwitch[1], mRoomSwitch (one-zone), mItem[0], mItem[1], mRoomItem (one-zone). dSv_zoneActor_c
/// (enemies that stay dead in a zone) is M6 and is deliberately NOT here.
constexpr std::size_t kZoneBytes = 12;
/// dStage_roomControl_c::mStatus[0x40]: room numbers 0..63.
constexpr std::size_t kZoneRooms = 64;
/// In a temp op's index (low byte): write receivers' mirrors only, never their engine.
constexpr std::uint16_t kTempMirrorOnly = 0x80;

inline bool is_temp(Domain d) { return d == Domain::Dan || d == Domain::Zone; }
/// The temp byte a Dan/Zone op addresses (the mirror-only flag stripped).
inline std::uint8_t temp_byte(std::uint16_t index) {
    return static_cast<std::uint8_t>(index & 0x7F);
}
inline bool temp_mirror_only(std::uint16_t index) { return (index & kTempMirrorOnly) != 0; }
inline std::uint8_t zone_room(std::uint16_t index) { return static_cast<std::uint8_t>(index >> 8); }
inline std::uint16_t zone_index(std::size_t room, std::size_t byte, bool mirrorOnly = false) {
    return static_cast<std::uint16_t>(room << 8 | byte | (mirrorOnly ? kTempMirrorOnly : 0));
}
/// Zone bytes that live as long as the zone does (mSwitch, mItem). The others (mRoomSwitch,
/// mRoomItem) are one-zone bits: vanilla clears them on every room change, so a player's visit
/// always starts with them clear, and H7 seeds only the persistent bytes.
inline bool zone_byte_persistent(std::size_t byte) {
    return byte < 4 || (byte >= 6 && byte < 10);
}

/// One machine's temp bits for the stage table it stands in (what the bridge captures each tick).
struct TempView {
    /// The dan stage table these bits belong to; -1 = none (diff_temp then emits nothing).
    std::int8_t tbl = -1;
    std::array<std::uint8_t, kDanBytes> dan{};
    /// Bit r set: this machine has a zone for room r, and zone[r] holds its bytes.
    std::uint64_t rooms = 0;
    std::array<std::array<std::uint8_t, kZoneBytes>, kZoneRooms> zone{};
};

/// What a machine knows about one occupied stage table's temp bits (keyed by table elsewhere).
struct TempMirror {
    std::array<std::uint8_t, kDanBytes> dan{};
    /// Bit r set: zone[r] has been written at least once (by an op or an announcement).
    std::uint64_t rooms = 0;
    std::array<std::array<std::uint8_t, kZoneBytes>, kZoneRooms> zone{};
};
using TempMirrors = std::map<std::uint8_t, TempMirror>;

/// Gameplay changes only: Dan ops for every changed dan byte, Zone ops for every changed byte of a
/// room present in BOTH views. Nothing when the tables differ or either is -1 (a stage change is
/// lifecycle), and nothing for a room that appeared or disappeared (zone create/remove is too).
void diff_temp(const TempView& shadow, const TempView& cur, std::vector<WorldOp>& out);

/// Applies a (validated) temp op to a mirror; mirror-only ops too. False for a non-temp or
/// malformed op.
bool apply_temp_op(TempMirror& m, const WorldOp& op);

/// Applies a (validated) temp op to a view when it addresses that view's table (and, for Zone, a
/// room the view has). Ignores the mirror-only flag: callers skip those for anything but mirrors.
/// Returns true when it wrote.
bool apply_temp_op(TempView& v, const WorldOp& op);

/// Mirror-only ops that overwrite a receiver's mirror of `tbl` with `dan` (set = byte,
/// clr = ~byte, every byte).
void announce_dan(std::uint8_t tbl, const std::array<std::uint8_t, kDanBytes>& dan,
    std::vector<WorldOp>& out);
/// The same for one room's zone bytes.
void announce_zone(std::uint8_t tbl, std::uint8_t room,
    const std::array<std::uint8_t, kZoneBytes>& zone, std::vector<WorldOp>& out);

/// The WorldSync trailer: [u8 format][u8 n] then per mirror [u8 tbl][kDanBytes][u64 rooms]
/// [kZoneBytes per set room bit, ascending].
void write_temp_mirrors(Writer& w, const TempMirrors& m);
bool read_temp_mirrors(Reader& r, TempMirrors& out);

// --- host-side proposal limits (spec II.8 b) -----------------------------------------------------

struct ProposalVerdict {
    bool accept = true;
    std::uint32_t bitsTouched = 0;     ///< popcount(set | clr) over all SAVED bit ops
    std::uint32_t eventClears = 0;     ///< popcount(clr) over Event ops
    std::uint32_t badOps = 0;          ///< ops failing op_in_range
    std::uint32_t tempBitsTouched = 0; ///< popcount(set | clr) over Dan/Zone ops
};
constexpr std::uint32_t kMaxProposalBits = 64;
constexpr std::uint32_t kMaxProposalEventClears = 8;
/// Dan/Zone ops cannot reach anyone's save (see "temporary domains"), so the save-protection limit
/// does not apply to them; this one only bounds garbage. A stage entry legitimately announces the
/// whole dan block plus every loaded room's zone in one tick, so it allows one full table
/// overwrite plus a tick of gameplay.
constexpr std::uint32_t kMaxProposalTempBits =
    static_cast<std::uint32_t>((kDanBytes + kZoneRooms * kZoneBytes) * 8) + kMaxProposalBits;

/// The host refuses a guest proposal touching more than kMaxProposalBits saved bits, carrying more
/// than kMaxProposalEventClears saved-event clears, more than kMaxProposalTempBits temp bits, or
/// any malformed op. Real gameplay changes a handful of bits per tick; a refusal means a guest bug
/// (e.g. dSv_info_c::init while synced), and the host answers it with a fresh WorldSync instead of
/// saving it.
ProposalVerdict judge_proposal(const std::vector<WorldOp>& ops);

// --- whole-view transfer + digest ----------------------------------------------------------------

void write_view(Writer& w, const WorldView& v);
bool read_view(Reader& r, WorldView& out);

/// FNV-1a 32 over every byte of the view, counters included (as little-endian s32).
std::uint32_t digest(const WorldView& v);

// --- self-test -----------------------------------------------------------------------------------

/// Engine-free self-test of everything above (diff/apply round-trip and the shadow update, mask
/// merge, counter paths and clamps, view transfer + digest, proposal limits, truncated input, and
/// the Dan/Zone temp domains: co-location diff, mirrors, mirror-only announcements, the WorldSync
/// trailer).
/// Returns false with a message in `failure` on the first failed check.
bool selftest(std::string& failure);

} // namespace dusk::mp::world
