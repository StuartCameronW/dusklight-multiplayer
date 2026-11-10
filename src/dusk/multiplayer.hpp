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

}  // namespace dusk::mp
