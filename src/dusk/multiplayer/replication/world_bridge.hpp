#pragma once

#include <array>
#include <cstdint>

#include "world_state.hpp"

/**
 * The shared world (M5), engine side: the only code that reads or writes g_dComIfG_gameInfo for
 * world sync. world_state.hpp is the engine-free half (diff, ops, digest); this file turns the
 * engine's save structs into a world::WorldView and writes ops and views back.
 * Spec: .claude/future-plans/mp-m4-m5-world.md Part II.
 *
 * ★ WHICH TABLE mMemory BELONGS TO IS TRACKED, NOT INFERRED. dSv_info_c::mMemory is the live copy
 * of ONE stage table's switches (dSv_save_c::mSave[tbl]). getSave copies the table in at stage init
 * (d_stage.cpp, dStage_stagInfoInit) and putSave copies it back in dStage_Delete. Both run inside
 * the scene pass, between our two ticks, so asking dComIfGp_getStageStagInfo() once per tick leaves
 * a window where a remote op lands in mMemory tagged for the wrong table and corrupts the new
 * stage's switches. So two decomp hooks report the hand-over exactly (world_on_memory_loaded /
 * world_on_memory_stored in dusk/multiplayer.hpp), and live_tbl() is -1 in between. With exact
 * ownership the view is invariant under getSave/putSave: they move bytes between two places that
 * map to the same canonical slot, so a stage change produces no diff at all.
 *
 * ★ PER-PLAYER BYTES INSIDE SHARED FIELDS. Bottle contents and the bomb type in a bomb bag live in
 * the same item-slot bytes as "do I own a bottle / bag" (dSv_player_item_c::mItems, slots
 * SLOT_11..14 and SLOT_15..17). Contents are Individual (spec Part IV Q2), ownership is Shared. So
 * the view stores those slots CANONICALISED (any bottle -> EMPTY_BOTTLE, any bag -> BOMB_BAG_LV1),
 * and the writer only changes a slot whose canonical value differs: a new bottle arrives empty, a
 * bottle you already hold keeps whatever is in it.
 *
 * ★ STAGE TABLE 6's KEY COUNT IS LOCAL. capture() copies mem[6]'s keyNum byte like any other
 * (diff() skips it), but nothing here ever writes it: setNextStage forces it to 0 or 2 on every
 * warp, on both machines (world_state.hpp, kLocalOnlyKeyTbl).
 *
 * All functions are game-thread only (the tick hooks and the decomp hooks both run there).
 */

namespace dusk::mp::world_bridge {

/// True when the play scene and save info are live and it's safe to capture/apply (not title,
/// not file select, not between dComIfGs_init and a loaded game).
///
/// False is often transient (a stage change briefly has no play scene); it does not by itself mean
/// the save was reset. reset_epoch() is the signal for that.
bool world_ready();

/// Engine -> view. Only call when world_ready(). Fields that the policy makes Individual stay 0.
void capture(world::WorldView& out);

/// Writes one already-validated op into the engine through the owner-aware writer: for MemBit,
/// mMemory if op.tbl == live_tbl() plus mSave[tbl] always (mirror dComIfGs_onStageSwitch,
/// d_com_inf_game.cpp:1518-1528); when live_tbl() == -1 only mSave[tbl]. `value` is the counter's
/// final value for counter domains (the caller decides absolute vs predicted).
///
/// Ops for a domain the policy makes Individual, the keyNum of kLocalOnlyKeyTbl, the keyNum byte
/// as a MemBit op, and Dan/Zone ops (those go through apply_temp_to_engine) are ignored (logged
/// once each).
void apply_to_engine(const world::WorldOp& op, std::int32_t value);

/// True when g_mpPolicy makes this saved domain Shared (so it is in the view at all). Always false
/// for Dan/Zone, which are not in the view. world_sync asks it so an op for a domain that turned
/// Individual mid-session (in flight across a policy switch) never reaches either shadow.
bool domain_shared(world::Domain d);

/// Guest join: asserts the save latch is engaged (refuse + log ERROR otherwise), writes the view
/// into the engine (per-player fields keep the guest's values), then reloads the current stage in
/// place (setRestartRoom at Link's pos/angle/room + setNextStage(cur stage, -1, cur room, -1)) so
/// already-created actors re-read their flags. Returns false if refused.
///
/// Also returns false (logged WARN, nothing written) when !world_ready(); the caller retries on a
/// later tick. If a stage change is already pending, the view is written and no extra reload is
/// queued: the pending change recreates every actor anyway.
bool install_host_world(const world::WorldView& v);

/// Writes the view into the engine WITHOUT a reload (digest resync).
void overwrite_world(const world::WorldView& v);

/// The stage table mMemory currently belongs to, or -1 (between dStage_Delete's putSave and the
/// next getSave).
int live_tbl();

/// Bumped every time the save info was bulk-reset or bulk-loaded (dSv_info_c::init, card_to_memory,
/// title/file select). The world_sync layer drops to Unsynced when it changes.
std::uint32_t reset_epoch();
/// True from install_host_world's reload in place until the reloaded stage has loaded its flags
/// (hook H1). A warp issued in that window is dropped by the scene change already under way.
bool reload_pending();

// --- temporary bits: Dan (mDan) and Zone (mZone), Batch W-B, spec II.7 ---------------------------
//
// Engine access only; who shares what with whom is world_sync.cpp's. None of this touches
// mSavedata, so nothing here can reach a save file.

/// Engine -> the temp bits of the stage table the local player is in. False (and `out.tbl = -1`)
/// whenever that table is not known for certain: no world, mMemory owned by no table (mid stage
/// change), or mDan not (yet) initialised for that table. Never send temp ops on false.
bool capture_temp(world::TempView& out);

/// Writes a (validated, NOT mirror-only) temp op into the engine when this machine holds it: Dan
/// when the op's table is the live one and mDan belongs to it; Zone additionally only when this
/// machine has a zone for the op's room (zone ids are per machine, so the room is mapped through
/// dStage_roomControl_c::getZoneNo here). Returns true when it wrote.
bool apply_temp_to_engine(const world::WorldOp& op);

/// H6 (and a resync): overwrite mDan's switch and item bits, never mStageNo / unk1 / unk28. Only
/// call while mDan belongs to the table the bits are for (right after dComIfGs_initDan(stageNo),
/// or when capture_temp() just reported that table).
void seed_dan(const std::array<std::uint8_t, world::kDanBytes>& dan);

/// H7: overwrite one room's zone bytes, only the persistent ones (world::zone_byte_persistent)
/// when `persistentOnly`. False when this machine has no zone for that room.
bool seed_zone(int room, const std::array<std::uint8_t, world::kZoneBytes>& bytes,
    bool persistentOnly);

/// One room's zone bytes as they are now; false when this machine has no zone for that room.
bool read_zone(int room, std::array<std::uint8_t, world::kZoneBytes>& out);

/// Bumped whenever the temp bits change owner wholesale: a stage's tables were loaded (H1) or the
/// save info was reset. A temp shadow from an older epoch must be rebased, never diffed.
std::uint32_t temp_epoch();

/// mDan.mStageNo (-1 = reset), for logs.
int dan_stage_no();
/// The zone id this machine uses for `room`, or -1 when it has none (or the table is not live).
int zone_no(int room);
/// The room Link stands in (dStage_roomControl_c stay number), for logs and autopilot.
int stay_room();

}  // namespace dusk::mp::world_bridge
