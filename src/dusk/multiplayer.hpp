#pragma once

/**
 * Dusklight multiplayer — the only surface the game loop touches.
 *
 * Everything else lives under src/dusk/multiplayer/. These two functions are called from
 * f_ap_game.cpp around the actor pass; see .claude/plan/04-architecture.md for the design and
 * .claude/plan/07-milestones.md for the build order.
 *
 * Deliberately dependency-free: this header is included by decomp code, so it must not drag in
 * ENet, the STL, or any engine type.
 */

/* --- batch 7: drops --- (a declaration only: no engine header comes with it) */
class daItem_c;

namespace dusk::mp {

/**
 * A session asked for on the command line.
 *
 * Plain C types on purpose: this header is included by decomp translation units, so it must not
 * pull in <string>. The strings are copied when the options are applied, so the caller keeps
 * ownership of them.
 */
struct StartupOptions {
    /// --mp-host: open a session and wait for players.
    bool host = false;
    /// --mp-connect: "address" or "address:port". Null or empty means no join was requested.
    const char* connect = nullptr;
    /// --mp-port: 0 leaves the port at its default.
    unsigned short port = 0;
    /// --mp-name: nickname shown to the other players.
    const char* nickname = nullptr;
    /// --mp-color: "RRGGBB" hex. Null or empty leaves the colour to the automatic palette.
    const char* color = nullptr;
    /// --mp-trace: development pose trace, written per sim tick. See multiplayer/session/trace.hpp.
    const char* trace = nullptr;
};

/**
 * Record what the command line asked for. Must be called before the first tick; the session is
 * actually opened lazily on that tick, once the game's own subsystems are up.
 *
 * These win over the DUSK_MP_* environment variables, which stay supported because they survive
 * a process restart and because the two-instance test script uses them.
 */
void apply_startup_options(const StartupOptions& options);

/**
 * Hold the session back until the pre-game menu has been dismissed.
 *
 * With a disc already configured the game boots straight away and runs its title scene behind the
 * prelaunch menu, so the "first tick" that opens the session arrives long before the player has
 * pressed Host or Join. While held, the tick hooks do nothing at all, so no session is opened and
 * the startup options can still be changed. Must be called before the first tick.
 */
void defer_session_start();

/**
 * Release the hold placed by defer_session_start(); the session then opens on the next tick.
 *
 * `lobby` is what the in-game Multiplayer menu asked for, or null when the player just pressed
 * Play. It is merged UNDER the command line: any --mp-* flag that was given keeps its value, and
 * if the command line asked to host or join at all, the menu's choice of host or join is ignored.
 * Strings are copied, so the caller keeps ownership of them. Safe to call when nothing is held.
 */
void begin_deferred_session(const StartupOptions* lobby);

/// Called from fapGm_Before, immediately BEFORE the actor pass runs.
/// Drains the transport and applies inbound state so actors execute against fresh data.
/// Cheap and safe to call when multiplayer is inactive.
void pre_actor_tick();

/// Called from fapGm_AfterRecord, immediately AFTER the actor pass runs.
/// Captures local state, sends it, and flushes the transport.
/// Cheap and safe to call when multiplayer is inactive.
void post_actor_tick();

/// True once a session is live (hosting, or connected to a host).
bool is_active();

/*
 * Shared-world (M5) hooks, called from decomp code inside #if TARGET_PC. They only record which
 * stage table dSv_info_c::mMemory holds and when the save info was bulk-reset, so they are cheap and
 * safe to call with multiplayer inactive. Implemented in multiplayer/replication/world_bridge.cpp.
 */

/// d_stage.cpp, right after dComIfGs_getSave(stageNo): mMemory now holds stage table `stageNo`.
void world_on_memory_loaded(int stageNo);

/// d_stage.cpp dStage_Delete, right after dComIfGs_putSave(...): mMemory was folded back into
/// stage table `stageNo` and belongs to no table until the next world_on_memory_loaded.
void world_on_memory_stored(int stageNo);

/// d_save.cpp dSv_info_c::init and card_to_memory: the whole save info was reset or replaced, so
/// the world sync layer must stop diffing until a game is loaded and resynced.
void world_on_save_reset();

/*
 * Temporary bits (Batch W-B, spec II.7): dungeon bits (mDan) and room bits (mZone) shared between
 * players in the same stage table / room. Never saved. Implemented in
 * multiplayer/session/world_sync.cpp; no-ops outside a synced session.
 */

/// H6, d_stage.cpp dStage_stagInfoInit, right AFTER dComIfGs_initDan(stageNo): if a partner is in
/// that stage table, overwrite mDan's bits with the shared copy (vanilla's reset/keep decision and
/// its carry-object save flag are left as they are).
void world_on_dan_init(int stageNo);

/// H7, d_s_room.cpp room phase_2, right after a zone was created for `roomNo` and before the
/// room's actors exist: seed it from the shared copy if a partner stands in that room.
void world_on_zone_created(int roomNo);

/// H8, around the room-lifecycle clears of zone bits (dStage_roomControl_c::zoneCountCheck, and
/// the room-kill clear in d_s_room.cpp): `begin` sends any gameplay change made before the clear,
/// `end` adopts the clear without sending it. One player's room change is his own lifecycle and
/// must never clear a puzzle a partner is standing in.
void world_on_zone_lifecycle_begin();
void world_on_zone_lifecycle_end();

/**
 * Batch 6 (W-B): chest and floor-item claims, hooks H3-H5 (mp-m4-m5-world.md II.6), called from
 * d_a_tbox.cpp and d_a_obj_item.cpp inside #if TARGET_PC. Implemented in
 * multiplayer/replication/item_claims.cpp. Outside a live session with shared quest flags,
 * claims_active() is false and every claim is granted at once: vanilla behaviour.
 */

/// What claim_tbox / claim_item return.
enum ClaimResult {
    kClaimGranted = 0,  ///< take it
    kClaimDenied = 1,   ///< someone else has it (or the host never answered): don't
    kClaimPending = 2,  ///< asked the host; not this tick
};

/// True in a live session whose quest flags (chests, item bits) are shared.
bool claims_active();

/// Chest `tboxNo` (0-63) of the stage table mMemory holds. A ClaimResult.
int claim_tbox(int tboxNo);

/// Floor item with save bit `itemBitNo`, numbered as dSv_info_c::onItem numbers it. Only the saved
/// range (0x80-0x9F) is claimed; any other bit is granted at once. A ClaimResult.
int claim_item(int itemBitNo);

/// Logging for the hooks: a chest whose bit the partner set (H3), and whether my own open event
/// had already started when it was noticed.
void claims_note_tbox_opened(int tboxNo, bool eventAlreadyStarted);

/// Logging for the hooks: a floor item removed because the partner took it (H5 re-check) or
/// because its claim was denied.
void claims_note_item_removed(int itemBitNo, bool takenByPartner);

/// Logging for the hooks: a floor small key (its "taken" state is tbox bit `tboxNo`) removed
/// because the partner picked it up.
void claims_note_key_taken(int tboxNo);

/*
 * Batch 7: death (.claude/future-plans/mp-death-rule.md, hook H1), called from d_menu_save.cpp
 * dMenu_save_c::retryQuestion1 inside #if TARGET_PC. Implemented in
 * multiplayer/session/network_manager.cpp.
 */

/// True in a live session whose MultiplayerPolicy::onDeath is RespawnSelf: the death menu's
/// "Retry?" must not lead on to "Return to the title screen?". False outside a session (vanilla).
bool death_menu_hides_title();

/// The player answered No to "Retry?" while death_menu_hides_title(): toast (and log) where
/// leaving the session lives.
void death_menu_title_refused();

/* --- batch 7: drops ---
 * Shared drops (multiplayer/replication/object_drops.hpp), called from f_op_actor_mng.cpp and
 * d_a_obj_item.cpp inside #if TARGET_PC. Implemented in object_capture_drop.cpp. Outside a live
 * session with claims in force every one of them is a no-op that answers "vanilla". */

/// An item helper (fopAcM_createItem / fopAcM_fastCreateItem / fopAcM_fastCreateItem2) just
/// asked for a daItem_c with this process id: it is a run-time drop, not a stage-placed item, so
/// it may be shared. Any id is accepted (0xFFFFFFFF and non-items are ignored later).
void drops_note_created(unsigned int procId);

/// Around fopAcM_createItemFromEnemyID: items created inside stay local (enemy kill drops are
/// shared from M6, once the enemies are). Nests.
void drops_enemy_scope(bool enter);

/// Top of daItem_c::_daItem_execute. Switches a partner's copy to follow the network, and settles
/// a pickup claim that is still out. True: the item removed itself this tick; return at once.
bool drops_execute(daItem_c* item);

/// Top of daItem_c::itemGetNextExecute, before anything is granted. A ClaimResult: Granted for an
/// item that is not a shared drop.
int drops_claim_pickup(daItem_c* item);

/* --- batch 8: pause ---
 * The Dying Light pause model (.claude/future-plans/mp-pause.md; Stuart's calls in
 * .claude/plan/17-decisions-2026-11-20.md): the world pauses only when EVERY player is in a menu.
 * One player alone in a menu keeps his world running behind it, and he can be hit there.
 * Called from decomp code inside #if TARGET_PC; implemented in multiplayer/session/pause.cpp.
 * Outside a live session every one of them answers "vanilla": world_frozen() is the menu pause
 * flag, nothing is isolated, deferred, hidden or force-closed. */

/// Replaces dComIfGp_isPauseFlag() at the world's execute gates (actors, camera, kankyo, the two
/// d_s_play blocks) and the timers. No live session: exactly the flag. Live: the flag AND every
/// in-world partner reports being in a menu. The flag itself is never touched: the menus' own
/// drawing needs it.
bool world_frozen();

/// The local player has a menu open, a session is live and the world runs behind the menu: the
/// menu owns the pad, and the world must see none of it.
bool pause_isolates_world();

/// True inside a PauseWorldExecScope that is isolating: Dusk's own input feeds (action binds,
/// mouse and gyro aim) must answer "nothing" to the world while the menu has the pad.
bool pause_world_input_blocked();

/**
 * Around one actor or camera execute (f_op_actor.cpp / f_op_camera.cpp). While
 * pause_isolates_world(), the outermost scope zeroes mDoCPd_c::m_cpadInfo[PAD_1] and restores it
 * after, and shows the world the X/Y assignment it had when the menu opened (the menu's changes
 * apply on close). Otherwise it does nothing at all.
 */
class PauseWorldExecScope {
public:
    PauseWorldExecScope();
    ~PauseWorldExecScope();

private:
    bool mActive;
    PauseWorldExecScope(const PauseWorldExecScope&);
    PauseWorldExecScope& operator=(const PauseWorldExecScope&);
};

/// Why an open menu must close now (pause_menu_force_close_reason). Something that started behind
/// the menu while the world ran: it cannot wait for the player to close it.
enum PauseForceClose {
    kPauseForceCloseNone = 0,
    kPauseForceCloseDeath = 1,  ///< life 0: the gameover waits for the menu (d_gameover.cpp:133)
    kPauseForceCloseEvent = 2,  ///< an event started behind the menu
    kPauseForceCloseScene = 3,  ///< a scene change (void-out, exit, warp) was queued
    kPauseForceCloseDemo = 4,   ///< Link went into a demo (an enemy grab) behind the menu
};

/// dMw_c::_execute: a PauseForceClose, kPauseForceCloseNone unless a live session's world runs
/// behind the open menu and one of the reasons started after it opened.
int pause_menu_force_close_reason();

/// Logging for the force-close: `waitingFor` non-null while it is held back (a save in progress,
/// a fade landing, a sub-menu still loading), null once the menu is closed.
void pause_note_force_close(int reason, int menuProc, const char* waitingFor);

/// dMenu_Collect3D_c::_create: true if this Collect screen runs without its 3-D Link doll (any
/// live session, latched for the screen's life). The doll drives Link's real model, and Link's own
/// execute keeps running behind the menu in co-op.
bool pause_collect_doll_begin();
/// The latched answer, for the Collect screen's _move / draw / _delete.
bool pause_collect_doll_hidden();
/// dMenu_Collect3D_c::_move with the doll hidden: the doll's sword-wait countdown alone, so the
/// screen can still close (dMw_isPush_S_Button waits for it).
void pause_collect_doll_execute();
/// dMenu_Collect3D_c::_delete.
void pause_collect_doll_end();
/// The Collect screen's outfit / shield change with the doll hidden: applied when the menu closes.
void pause_defer_clothes_change();
void pause_defer_shield_change();

/// daAlink_c::execute: a deferred shield reload is landing in the world. His execute must not run
/// while mShieldModel is null (setCollision dereferences it), so it holds until the reload is done.
bool pause_equip_hold();

/// daAlink_c::execute: hold checkWarpStart while a menu is open in a live session, so a warp chosen
/// on the field map starts when the map has closed, as it does when the map freezes him.
bool pause_defer_warp_start();

/// dMeter2_c::alphaAnimeLife: true on the "shown" half of the hearts flash, which runs for a while
/// after the player was hurt behind a menu.
bool pause_hearts_flash_on();

}  // namespace dusk::mp
