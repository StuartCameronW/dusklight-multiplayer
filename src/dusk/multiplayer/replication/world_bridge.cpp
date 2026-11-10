#include "world_bridge.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "../policy/mp_policy.hpp"
#include "../session/save_guard.hpp"
#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_save.h"
#include "dusk/logging.h"
#include "dusk/multiplayer.hpp"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"

/**
 * The one translation unit that moves world-sync state in and out of g_dComIfG_gameInfo. See
 * world_bridge.hpp for the design; the engine-free half is world_state.cpp.
 */

namespace dusk::mp {
namespace world_bridge {
namespace {

constexpr borealis::Log Log{"dusk::mp"};

// --- the view's sizes must be the engine's sizes -------------------------------------------------

static_assert(sizeof(dSv_event_c) == world::kEventBytes, "WorldView::event vs dSv_event_c");
static_assert(dSv_save_c::STAGE_MAX == world::kMemTbls, "WorldView::mem vs STAGE_MAX");
static_assert(sizeof(dSv_memBit_c) == world::kMemBitBytes, "WorldView::mem vs dSv_memBit_c");
static_assert(offsetof(dSv_memBit_c, mKeyNum) == world::kMemKeyNumOffset,
    "kMemKeyNumOffset vs dSv_memBit_c::mKeyNum");
static_assert(dSv_save_c::STAGE2_MAX == world::kMem2Tbls, "WorldView::mem2 vs STAGE2_MAX");
static_assert(sizeof(dSv_memory2_c) == world::kMem2Bytes, "WorldView::mem2 vs dSv_memory2_c");
static_assert(sizeof(dSv_player_get_item_c) == world::kFirstGetBytes,
    "WorldView::firstGet vs dSv_player_get_item_c");
static_assert(sizeof(dSv_player_item_c::mItems) == world::kItemSlots,
    "kItemSlots vs dSv_player_item_c::mItems");
static_assert(sizeof(dSv_player_item_max_c::mItemMax) == world::kItemMaxBytes,
    "kItemMaxBytes vs dSv_player_item_max_c::mItemMax");
static_assert(sizeof(dSv_player_collect_c::mItem) == world::kCollectBytes,
    "kCollectBytes vs dSv_player_collect_c::mItem");

/// dSv_memBit_c ends at mDungeonItem (0x1D); 0x1E-0x1F are padding. The compiler's copy assignment
/// (getSave/putSave are `mMemory = mSave[i]` and back) is not obliged to carry padding, so those
/// two bytes could differ between the two copies and fake a diff on every stage change. capture()
/// zeroes them and nothing writes them.
constexpr std::size_t kMemPadStart = 0x1E;
static_assert(offsetof(dSv_memBit_c, mDungeonItem) + 1 == kMemPadStart, "dSv_memBit_c padding");

// --- the temp domains' byte layouts (world_state.hpp, "temporary domains") ----------------------

static_assert(offsetof(dSv_danBit_c, mItem) ==
                  offsetof(dSv_danBit_c, mSwitch) + world::kDanSwitchBytes,
    "kDanSwitchBytes vs dSv_danBit_c::mSwitch");
static_assert(offsetof(dSv_danBit_c, unk28) == offsetof(dSv_danBit_c, mSwitch) + world::kDanBytes,
    "kDanBytes vs dSv_danBit_c::mSwitch + mItem");
static_assert(offsetof(dSv_zoneBit_c, unk12) == world::kZoneBytes,
    "kZoneBytes vs dSv_zoneBit_c (everything before unk12)");
static_assert(dSv_info_c::ZONE_MAX == 32, "zone ids");

// --- the Items-domain byte map (world_state.hpp, kPlayerBytes) ---------------------------------

constexpr std::size_t kPbItems = 0;
constexpr std::size_t kPbItemMax = kPbItems + world::kItemSlots;
constexpr std::size_t kPbCollect = kPbItemMax + world::kItemMaxBytes;
constexpr std::size_t kPbCrystal = kPbCollect + world::kCollectBytes;
constexpr std::size_t kPbMirror = kPbCrystal + 1;
constexpr std::size_t kPbWallet = kPbMirror + 1;
static_assert(kPbWallet + 1 == world::kPlayerBytes, "Items byte map vs kPlayerBytes");

// --- state ---------------------------------------------------------------------------------------

/// Which mSave table mMemory currently holds; -1 between putSave and the next getSave.
int s_liveTbl = -1;
std::uint32_t s_resetEpoch = 0;
bool s_reloadPending = false;
/// See temp_epoch(): bumped by H1 and by a save reset.
std::uint32_t s_tempEpoch = 0;
/// A stage has loaded its table since the last bulk reset/load (a game is actually in progress).
bool s_loadedSinceReset = false;

// Log-once latches for ops that are dropped on purpose; one line each is enough to diagnose.
bool s_loggedIndividualOp = false;
bool s_loggedReservedDomain = false;
bool s_loggedLocalKeyOp = false;
bool s_loggedMalformedOp = false;

// --- policy --------------------------------------------------------------------------------------

/*
 * Which policy field owns which domain:
 *   questFlags: event bits/regs, stage-table bits (chests, switches, items, dungeon items), the
 *               small-key counters inside them, and the visited-room map (mem2).
 *   inventory:  first-get bits, item slots, item capacities, collection (crystals, mirror shards),
 *               wallet size, heart containers (maxLife) and poe souls (both items you collect).
 *   rupees:     the rupee counter.
 * Current hearts (mLife) are never in the view: they are per player (policy `health`). maxLife is
 * deliberately NOT governed by `health` — spec Part IV Q3 defaults to shared heart containers, and
 * the save format has no per-player heart-piece bits to support anything else.
 */
bool quest_shared() {
    return g_mpPolicy.questFlags == Ownership::Shared;
}
bool inventory_shared() {
    return g_mpPolicy.inventory == Ownership::Shared;
}
bool rupees_shared() {
    return g_mpPolicy.rupees == Ownership::Shared;
}

}  // namespace

bool domain_shared(world::Domain d) {
    switch (d) {
    case world::Domain::Event:
    case world::Domain::MemBit:
    case world::Domain::Mem2:
    case world::Domain::KeyNum:
        return quest_shared();
    case world::Domain::FirstGet:
    case world::Domain::Items:
    case world::Domain::MaxLife:
    case world::Domain::Poh:
        return inventory_shared();
    case world::Domain::Rupee:
        return rupees_shared();
    default:
        return false;
    }
}

namespace {

// --- raw access ----------------------------------------------------------------------------------

dSv_info_c& info() {
    return g_dComIfG_gameInfo.info;
}

/// The engine's bytes, exactly as stored (some fields are BE-wrapped; bit diffs don't care).
template <typename T>
std::uint8_t* bytes_of(T& t) {
    return reinterpret_cast<std::uint8_t*>(&t);
}

std::uint8_t mask_apply(std::uint8_t b, std::uint8_t set, std::uint8_t clr) {
    return static_cast<std::uint8_t>((b | set) & ~clr);
}

std::int32_t clamp_i32(std::int32_t v, std::int32_t lo, std::int32_t hi) {
    return std::min(std::max(v, lo), hi);
}

/**
 * Item slots whose byte mixes shared ownership with per-player contents (see world_bridge.hpp).
 * Bottle contents and the bomb type in a bag are Individual (spec Part IV Q2; MultiplayerPolicy
 * `bottles` / `ammo`). This is unconditional today rather than read from those policy fields: ammo
 * counts and bottle contents are never in the view at all, so sharing the bag's bomb type or the
 * bottle's contents without them would be incoherent.
 */
std::uint8_t canonical_slot(int slot, std::uint8_t item) {
    if (item == dItemNo_NONE_e) {
        return item;
    }
    if (slot >= SLOT_11 && slot < SLOT_11 + dSv_player_item_c::BOTTLE_MAX) {
        return dItemNo_EMPTY_BOTTLE_e;
    }
    if (slot >= SLOT_15 && slot < SLOT_15 + dSv_player_item_c::BOMB_BAG_MAX) {
        return dItemNo_BOMB_BAG_LV1_e;
    }
    return item;
}

/// One Items-domain byte, as the view holds it.
std::uint8_t get_player_byte(std::size_t i) {
    dSv_player_c& pl = info().getPlayer();
    if (i < kPbItemMax) {
        const int slot = static_cast<int>(i - kPbItems);
        return canonical_slot(slot, pl.getItem().mItems[slot]);
    }
    if (i < kPbCollect) {
        return pl.getItemMax().mItemMax[i - kPbItemMax];
    }
    if (i < kPbCrystal) {
        return pl.getCollect().mItem[i - kPbCollect];
    }
    if (i == kPbCrystal) {
        return pl.getCollect().mCrystal;
    }
    if (i == kPbMirror) {
        return pl.getCollect().mMirror;
    }
    return pl.getPlayerStatusA().mWalletSize;
}

/// Writes one Items-domain byte. Callers only call this when the canonical value changes, so a
/// bottle the player already holds keeps its contents.
void put_player_byte(std::size_t i, std::uint8_t v) {
    dSv_player_c& pl = info().getPlayer();
    if (i < kPbItemMax) {
        // setItem, not a raw store: it rebuilds the lined-up slot list (mItemSlots) and refreshes
        // any select button showing this slot, exactly as an item get does.
        pl.getItem().setItem(static_cast<int>(i - kPbItems), v);
    } else if (i < kPbCollect) {
        pl.getItemMax().mItemMax[i - kPbItemMax] = v;
    } else if (i < kPbCrystal) {
        pl.getCollect().mItem[i - kPbCollect] = v;
    } else if (i == kPbCrystal) {
        pl.getCollect().mCrystal = v;
    } else if (i == kPbMirror) {
        pl.getCollect().mMirror = v;
    } else {
        pl.getPlayerStatusA().mWalletSize = v;
    }
}

/// Max life is shared; current life is the player's own. Keep the player's life valid if the
/// shared maximum ever comes in below it (a resync, or a guest installing an earlier world).
void set_max_life(std::int32_t v) {
    dSv_player_status_a_c& a = info().getPlayer().getPlayerStatusA();
    const u16 maxLife = static_cast<u16>(clamp_i32(v, 0, 400));
    a.setMaxLife(maxLife);
    if (a.getLife() > maxLife) {
        a.setLife(maxLife);
    }
}

void set_key_num(int tbl, std::int32_t v) {
    const u8 keys = static_cast<u8>(clamp_i32(v, 0, 99));
    info().getSavedata().getSave(tbl).getBit().setKeyNum(keys);
    if (tbl == s_liveTbl) {
        info().getMemory().getBit().setKeyNum(keys);
    }
}

/// Applies set/clr masks to one byte of a stage table: mSave[tbl] always, mMemory too when it holds
/// that table. Never the keyNum byte or the padding.
void mem_apply(int tbl, std::size_t index, std::uint8_t set, std::uint8_t clr) {
    if (index == world::kMemKeyNumOffset || index >= kMemPadStart) {
        return;
    }
    std::uint8_t* save = bytes_of(info().getSavedata().getSave(tbl).getBit());
    save[index] = mask_apply(save[index], set, clr);
    if (tbl == s_liveTbl) {
        std::uint8_t* mem = bytes_of(info().getMemory().getBit());
        mem[index] = mask_apply(mem[index], set, clr);
    }
}

/// Copies a whole stage table from the view, keeping the local bytes the view must not move:
/// padding, and stage table 6's key count (kLocalOnlyKeyTbl).
void mem_overwrite(
    std::uint8_t* dst, const std::array<std::uint8_t, world::kMemBitBytes>& src, int tbl) {
    for (std::size_t i = 0; i < kMemPadStart; i++) {
        if (i == world::kMemKeyNumOffset && tbl == world::kLocalOnlyKeyTbl) {
            continue;
        }
        dst[i] = src[i];
    }
}

void log_dropped(bool& latch, const world::WorldOp& op, const char* why) {
    if (latch) {
        return;
    }
    latch = true;
    Log.warn(
        "World op dropped ({}): domain {} tbl {} index {} — further drops of this kind are not "
        "logged",
        why, static_cast<int>(op.domain), static_cast<int>(op.tbl), op.index);
}

/**
 * Reload the current stage around Link so actors that read their flags only at create (chests,
 * floor items, doors, NPCs) re-read the world just installed. The join-partner mechanism aimed at
 * yourself (spec I.4): restart room at Link's pose + point -1, which is what dStage_restartRoom
 * (d_stage.cpp:3070) does for a void-out. Layer -1 lets the game pick the layer from the
 * (now shared) flags.
 */
void reload_in_place() {
    if (dComIfGp_isEnableNextStage()) {
        Log.info(
            "World install: a stage change is already pending; it recreates every actor, so no "
            "extra reload is queued");
        return;
    }
    fopAc_ac_c* link = dComIfGp_getPlayer(0);
    if (link == nullptr) {
        Log.warn(
            "World install: no player actor, so no reload in place. Actors already created keep "
            "the flags they read before the install until the next stage change");
        return;
    }

    // A private copy: setNextStage writes the next-stage buffer, and the name must not alias it.
    char stage[sizeof(dStage_startStage_c::mName) + 1] = {};
    const char* curStage = dComIfGp_getStartStageName();
    for (std::size_t n = 0; n + 1 < sizeof(stage) && curStage[n] != '\0'; n++) {
        stage[n] = curStage[n];
    }
    s8 room = fopAcM_GetRoomNo(link);
    if (room < 0) {
        room = static_cast<s8>(dComIfGp_roomControl_getStayNo());
    }
    const cXyz pos = link->current.pos;
    const s16 angle = link->shape_angle.y;

    dComIfGs_setRestartRoom(pos, angle, room);
    dComIfGp_setNextStage(stage, -1, room, -1);
    s_reloadPending = true;
    // Start mode 0 = arrive standing. Without this the param left by the last void-out would be
    // reused (d_stage.cpp:1714 copies it into Link's parameters for point -1).
    dComIfGs_setRestartRoomParam(daPy_py_c::setParamData(room, 0, 0, 0));
    Log.info("World install: reloading {} room {} in place at ({:.0f}, {:.0f}, {:.0f}) angle {} so "
             "existing actors re-read the host's flags",
        stage, static_cast<int>(room), pos.x, pos.y, pos.z, static_cast<int>(angle));
}

}  // namespace

// --- public API ----------------------------------------------------------------------------------

bool world_ready() {
    if (!s_loadedSinceReset || dComIfGs_getSaveData() == nullptr) {
        return false;
    }
    // The title screen is an OPENING_SCENE (d_s_play.cpp:1507) and file select a NAME_SCENE, so
    // "a PLAY_SCENE exists" rules both out. The title actor check is belt and braces, matching
    // world_is_playable().
    return fpcM_SearchByName(fpcNm_PLAY_SCENE_e) != nullptr &&
           fopAcM_SearchByName(fpcNm_TITLE_e) == NULL;
}

void capture(world::WorldView& out) {
    out = world::WorldView{};
    dSv_info_c& in = info();
    dSv_save_c& sv = in.getSavedata();

    if (quest_shared()) {
        std::memcpy(out.event.data(), bytes_of(sv.getEvent()), world::kEventBytes);
        for (int tbl = 0; tbl < static_cast<int>(world::kMemTbls); tbl++) {
            // The live table comes from mMemory, which is where the game writes it; mSave[tbl] is
            // a stale copy until the next putSave.
            dSv_memBit_c& bit =
                tbl == s_liveTbl ? in.getMemory().getBit() : sv.getSave(tbl).getBit();
            std::memcpy(out.mem[tbl].data(), bytes_of(bit), kMemPadStart);
            // keyNum (0x1C) is copied like any other byte, slot 6 included: diff() masks it.
        }
        for (int tbl = 0; tbl < static_cast<int>(world::kMem2Tbls); tbl++) {
            std::memcpy(out.mem2[tbl].data(), bytes_of(*sv.getSave2(tbl)), world::kMem2Bytes);
        }
    }

    if (inventory_shared()) {
        dSv_player_c& pl = sv.getPlayer();
        std::memcpy(out.firstGet.data(), bytes_of(pl.getGetItem()), world::kFirstGetBytes);
        for (std::size_t i = 0; i < world::kPlayerBytes; i++) {
            out.player[i] = get_player_byte(i);
        }
        out.maxLife = pl.getPlayerStatusA().getMaxLife();
        out.poh = pl.getCollect().getPohNum();
    }

    if (rupees_shared()) {
        out.rupee = sv.getPlayer().getPlayerStatusA().getRupee();
    }
}

void apply_to_engine(const world::WorldOp& op, std::int32_t value) {
    if (world::is_temp(op.domain)) {
        log_dropped(s_loggedReservedDomain, op, "Dan/Zone go through apply_temp_to_engine");
        return;
    }
    if (!domain_shared(op.domain)) {
        log_dropped(s_loggedIndividualOp, op, "the policy makes this domain Individual");
        return;
    }

    dSv_info_c& in = info();
    dSv_save_c& sv = in.getSavedata();

    switch (op.domain) {
    case world::Domain::Event: {
        if (op.index >= world::kEventBytes) {
            break;
        }
        std::uint8_t* b = bytes_of(sv.getEvent()) + op.index;
        *b = mask_apply(*b, op.set, op.clr);
        return;
    }
    case world::Domain::MemBit:
        if (op.tbl >= world::kMemTbls || op.index >= world::kMemBitBytes) {
            break;
        }
        if (op.index == world::kMemKeyNumOffset) {
            log_dropped(s_loggedLocalKeyOp, op, "keyNum travels as the KeyNum counter, not bits");
            return;
        }
        mem_apply(op.tbl, op.index, op.set, op.clr);
        return;
    case world::Domain::Mem2: {
        if (op.tbl >= world::kMem2Tbls || op.index >= world::kMem2Bytes) {
            break;
        }
        std::uint8_t* b = bytes_of(*sv.getSave2(op.tbl)) + op.index;
        *b = mask_apply(*b, op.set, op.clr);
        return;
    }
    case world::Domain::FirstGet: {
        if (op.index >= world::kFirstGetBytes) {
            break;
        }
        std::uint8_t* b = bytes_of(sv.getPlayer().getGetItem()) + op.index;
        *b = mask_apply(*b, op.set, op.clr);
        return;
    }
    case world::Domain::Items: {
        if (op.index >= world::kPlayerBytes) {
            break;
        }
        const std::uint8_t cur = get_player_byte(op.index);
        const std::uint8_t next = mask_apply(cur, op.set, op.clr);
        if (next != cur) {
            put_player_byte(op.index, next);
        }
        return;
    }
    case world::Domain::Rupee:
        sv.getPlayer().getPlayerStatusA().setRupee(static_cast<u16>(clamp_i32(value, 0, 9999)));
        return;
    case world::Domain::KeyNum:
        if (op.tbl >= world::kMemTbls) {
            break;
        }
        if (op.tbl == world::kLocalOnlyKeyTbl) {
            log_dropped(s_loggedLocalKeyOp, op, "stage table 6's key count is local");
            return;
        }
        set_key_num(op.tbl, value);
        return;
    case world::Domain::MaxLife:
        set_max_life(value);
        return;
    case world::Domain::Poh:
        sv.getPlayer().getCollect().setPohNum(static_cast<u8>(clamp_i32(value, 0, 255)));
        return;
    default:
        break;
    }
    log_dropped(s_loggedMalformedOp, op, "out of range or unknown domain");
}

void overwrite_world(const world::WorldView& v) {
    dSv_info_c& in = info();
    dSv_save_c& sv = in.getSavedata();

    if (quest_shared()) {
        std::memcpy(bytes_of(sv.getEvent()), v.event.data(), world::kEventBytes);
        for (int tbl = 0; tbl < static_cast<int>(world::kMemTbls); tbl++) {
            mem_overwrite(bytes_of(sv.getSave(tbl).getBit()), v.mem[tbl], tbl);
            if (tbl == s_liveTbl) {
                mem_overwrite(bytes_of(in.getMemory().getBit()), v.mem[tbl], tbl);
            }
        }
        for (int tbl = 0; tbl < static_cast<int>(world::kMem2Tbls); tbl++) {
            std::memcpy(bytes_of(*sv.getSave2(tbl)), v.mem2[tbl].data(), world::kMem2Bytes);
        }
    }

    if (inventory_shared()) {
        dSv_player_c& pl = sv.getPlayer();
        std::memcpy(bytes_of(pl.getGetItem()), v.firstGet.data(), world::kFirstGetBytes);
        for (std::size_t i = 0; i < world::kPlayerBytes; i++) {
            if (get_player_byte(i) != v.player[i]) {
                put_player_byte(i, v.player[i]);
            }
        }
        set_max_life(v.maxLife);
        pl.getCollect().setPohNum(static_cast<u8>(clamp_i32(v.poh, 0, 255)));
    }

    if (rupees_shared()) {
        sv.getPlayer().getPlayerStatusA().setRupee(static_cast<u16>(clamp_i32(v.rupee, 0, 9999)));
    }
}

bool install_host_world(const world::WorldView& v) {
    // ★ The one rule that must never slip (13-save-safety.md): the host's world is about to be
    // written into this process's save struct, so this process must already be barred from ever
    // writing its own card. The latch is set at join() and made sticky by HelloAck; this is the
    // first code that DEPENDS on it rather than being covered by it, so it checks instead of
    // trusting the call order.
    if (!save_would_be_contaminated()) {
        Log.error(
            "World install REFUSED: the save latch is not engaged, so writing the host's world "
            "here could reach this machine's own save file. This is a bug in the join path.");
        return false;
    }
    if (!world_ready()) {
        Log.warn("World install deferred: no game is loaded yet (title, file select or a reset); "
                 "nothing written");
        return false;
    }

    overwrite_world(v);
    Log.info("World install: host world written (digest 0x{:08X}, live table {}, reset epoch {})",
        world::digest(v), s_liveTbl, s_resetEpoch);
    reload_in_place();
    return true;
}

int live_tbl() {
    return s_liveTbl;
}

std::uint32_t reset_epoch() {
    return s_resetEpoch;
}

bool reload_pending() {
    return s_reloadPending;
}

// --- temporary bits (Dan, Zone) ------------------------------------------------------------------

namespace {

/// mDan.mSwitch[0] .. mItem[3], as raw bytes (world::kDanBytes of them).
std::uint8_t* dan_bytes() {
    return reinterpret_cast<std::uint8_t*>(&info().getDan().mSwitch[0]);
}

std::uint8_t* zone_bytes(int zoneNo) {
    return bytes_of(info().getZone(zoneNo).getBit());
}

/// The temp bits of `tbl` are this machine's live ones: a world, mMemory owned by `tbl` (so the
/// stage really is that table, not mid-change) and mDan initialised for it. Between dStage_Delete
/// (H2) and the next stage's getSave (H1) this is false for every table, which is what keeps a
/// stage change from ever being diffed or written as temp ops.
bool temp_table_live(int tbl) {
    return tbl >= 0 && tbl == s_liveTbl && info().getDan().mStageNo == tbl && world_ready();
}

}  // namespace

int zone_no(int room) {
    if (room < 0 || room >= static_cast<int>(world::kZoneRooms)) {
        return -1;
    }
    const int zoneNo = dComIfGp_roomControl_getZoneNo(room);
    if (zoneNo < 0 || zoneNo >= dSv_info_c::ZONE_MAX) {
        return -1;
    }
    // Both directions must agree: dStage_roomStatus_c::mZoneNo says which slot, and the slot's own
    // room number says it was not freed (removeZone) and handed to another room since.
    if (info().getZone(zoneNo).getRoomNo() != room) {
        return -1;
    }
    return zoneNo;
}

int stay_room() {
    return dComIfGp_roomControl_getStayNo();
}

int dan_stage_no() {
    return info().getDan().mStageNo;
}

std::uint32_t temp_epoch() {
    return s_tempEpoch;
}

bool capture_temp(world::TempView& out) {
    out = world::TempView{};
    if (!temp_table_live(s_liveTbl)) {
        return false;
    }
    out.tbl = static_cast<std::int8_t>(s_liveTbl);
    std::memcpy(out.dan.data(), dan_bytes(), world::kDanBytes);
    for (int room = 0; room < static_cast<int>(world::kZoneRooms); room++) {
        const int zoneNo = zone_no(room);
        if (zoneNo < 0) {
            continue;
        }
        out.rooms |= std::uint64_t{1} << room;
        std::memcpy(out.zone[room].data(), zone_bytes(zoneNo), world::kZoneBytes);
    }
    return true;
}

bool apply_temp_to_engine(const world::WorldOp& op) {
    if (!world::is_temp(op.domain) || !world::op_in_range(op) ||
        world::temp_mirror_only(op.index) || !temp_table_live(op.tbl))
    {
        return false;
    }
    std::uint8_t* b = nullptr;
    if (op.domain == world::Domain::Dan) {
        b = dan_bytes() + world::temp_byte(op.index);
    } else {
        const int zoneNo = zone_no(world::zone_room(op.index));
        if (zoneNo < 0) {
            // Zone ids are per machine: no zone here for that room means the room is not loaded
            // here, and room-temporary state only matters where the room is loaded.
            return false;
        }
        b = zone_bytes(zoneNo) + world::temp_byte(op.index);
    }
    *b = mask_apply(*b, op.set, op.clr);
    return true;
}

void seed_dan(const std::array<std::uint8_t, world::kDanBytes>& dan) {
    std::memcpy(dan_bytes(), dan.data(), world::kDanBytes);
}

bool seed_zone(
    int room, const std::array<std::uint8_t, world::kZoneBytes>& bytes, bool persistentOnly) {
    const int zoneNo = zone_no(room);
    if (zoneNo < 0) {
        return false;
    }
    std::uint8_t* dst = zone_bytes(zoneNo);
    for (std::size_t i = 0; i < world::kZoneBytes; i++) {
        if (!persistentOnly || world::zone_byte_persistent(i)) {
            dst[i] = bytes[i];
        }
    }
    return true;
}

bool read_zone(int room, std::array<std::uint8_t, world::kZoneBytes>& out) {
    const int zoneNo = zone_no(room);
    if (zoneNo < 0) {
        return false;
    }
    std::memcpy(out.data(), zone_bytes(zoneNo), world::kZoneBytes);
    return true;
}

}  // namespace world_bridge

// --- decomp hooks (declared in dusk/multiplayer.hpp) ---------------------------------------------

void world_on_memory_loaded(int stageNo) {
    if (stageNo < 0 || stageNo >= static_cast<int>(world::kMemTbls)) {
        world_bridge::Log.error(
            "getSave reported stage table {}, outside 0..{}; live table unknown", stageNo,
            world::kMemTbls - 1);
        world_bridge::s_liveTbl = -1;
        return;
    }
    world_bridge::s_liveTbl = stageNo;
    world_bridge::s_reloadPending = false;
    world_bridge::s_loadedSinceReset = true;
    // A new stage's tables: whatever temp shadow world sync holds is from before the change.
    world_bridge::s_tempEpoch++;
}

void world_on_memory_stored(int stageNo) {
    if (stageNo != world_bridge::s_liveTbl && world_bridge::s_liveTbl != -1) {
        // Not fatal (the table is released either way), but it means mMemory was tracked against
        // the wrong table for a while, which is exactly the corruption the hooks exist to prevent.
        world_bridge::Log.warn("putSave stored table {} but mMemory was tracked as table {}",
            stageNo, world_bridge::s_liveTbl);
    }
    world_bridge::s_liveTbl = -1;
}

void world_on_save_reset() {
    world_bridge::s_resetEpoch++;
    world_bridge::s_tempEpoch++;
    // dSv_info_c::init re-inits mMemory and card_to_memory replaces mSave under it, so mMemory
    // belongs to no table of the save now in memory until the next getSave.
    world_bridge::s_liveTbl = -1;
    world_bridge::s_loadedSinceReset = false;
}

}  // namespace dusk::mp
