#pragma once

/**
 * The one question every save path must ask multiplayer before it writes bytes to a memory card.
 *
 * ★ THE OWNERSHIP RULE (Stuart's call, 2026-08-13). **The host owns the save.** A guest plays in
 * the host's world — v1 policy is that rupees, inventory, equipment, ammo, magic and quest flags
 * are all Shared with the host authoritative (.claude/plan/04-architecture.md, "Player-state / save
 * split"; health is the one per-player exception) — and a guest therefore **never writes its own
 * memory card for the rest of the process's life**. Nothing is transferred, copied or pushed
 * between machines: "the guest takes the host's save" means the guest plays the host's world in
 * memory and its own .gci is simply left alone. The host's save stays an ordinary single-player
 * save; hosting changes nothing about what the host writes.
 *
 * ★ WHY THE FLAG IS STICKY, AND WHY IT IS NOT CLEARED ON DISCONNECT. This is the whole point of
 * the design. Once host-authored state has been applied into this process's g_dComIfG_gameInfo,
 * that object is the host's world wearing the client's filename, and it stays that way after the
 * socket closes. A predicate that merely asked "am I in a session right now" would return false
 * the moment the guest quit to the file screen — and then autosave, which fires on every stage
 * entry (d_s_play.cpp:750) and every shutter door (d_a_door_shutter.cpp:575), would serialise
 * someone else's game over their single-player progress. That is exactly the failure the Dolphin
 * mod is infamous for (.claude/plan/08-tpmp-reference.md), reached by a different route. So the
 * flag is set when this process joins a session and is cleared only by a fresh load from card,
 * which in practice today means: not until the program is restarted.
 *
 * The cost of being conservative is that a player who joins a friend, leaves, and keeps playing
 * solo cannot save until they restart — and they are told so by a toast rather than finding out
 * later. The cost of being permissive is a destroyed save file. This is not a close call.
 *
 * ★ WHERE THE GUARD LIVES, AND WHERE IT MUST NOT. The funnel to disk is
 * mDoMemCd_Ctrl_c::save() (src/m_Do/m_Do_MemCard.cpp), the single entry to COMM_STORE_e, with
 * exactly two callers program-wide: src/dusk/autosave.cpp and d_menu_save.cpp:2888 (the in-game
 * save prompt). Guarding there covers both. canAutoSave() also asks, purely so autosave skips the
 * pointless work silently — an autosave firing on every door must not raise a toast.
 *
 * ★ BUT THE CARD THREAD IS NOT THE ONLY WAY TO DISK ANY MORE (2026-11, after the upstream merge).
 * Four other routes write save files, and each is guarded separately: the format request
 * (mDoMemCd_Ctrl_c::command_format, refuse_card_format), upstream's Save Manager
 * (save_manager.cpp ensure_write_allowed and delete_backup, refuse_save_file_write), mod save
 * sidecars (mods/svc/save.cpp write_mod_sidecar, and the legacy migration is deferred), and the mod
 * file API when a location resolves into save storage (mods/svc/file.cpp,
 * refuse_save_location_write). A new path that writes under a card folder must join this list.
 *
 * Do NOT put a guard on dComIfGs_putSave. An earlier draft of this comment said to, and it was
 * wrong in a way that would have been expensive: dComIfGs_putSave (include/d/d_com_inf_game.h:2367)
 * resolves to dSv_info_c::putSave (d_save.cpp:1546), which is pure in-memory bookkeeping — it folds
 * the current stage's temp switches and chests into the persistent table — and d_stage.cpp:2781
 * calls it on ordinary stage teardown with nothing written to a card. Guarding it would break
 * normal play and would not stop a single byte reaching disk.
 *
 * (src/dusk/imgui/ImGuiSaveEditor.cpp writes game state too, but it is a developer tool the player
 * drives deliberately, not a path a session can trigger.)
 *
 * See .claude/plan/13-save-safety.md for the full audit this implements one item of.
 */

#include <filesystem>
#include <string_view>

namespace dusk::mp {

/**
 * True when this machine's in-memory game state has been contaminated by another player's
 * authority, and therefore must NOT be serialised into this machine's own save file.
 *
 * Pure query, no side effects — safe to call from a predicate like canAutoSave().
 */
bool save_would_be_contaminated();

/**
 * Latch the contamination flag. Called when this process joins a session as a guest, and the place
 * for the first flag-replication apply path to call when it lands. Idempotent; only the first call
 * logs. @p i_reason is recorded in the log so the cause is visible after the fact.
 */
void mark_save_contaminated(const char* i_reason);

/**
 * The refuse-a-card-write decision, for the one choke point that reaches disk. Returns true when
 * the caller must drop the write on the floor, having already told the player why (a toast, rate
 * limited so a stuck save loop cannot spam it) and logged it.
 *
 * Separate from save_would_be_contaminated() because it is NOT a pure query, and because it keeps
 * the toast and the logging on the Dusk side of the fence — the decomp call site stays a two-line
 * TARGET_PC bracket with no UI knowledge in it.
 */
bool refuse_card_write();

/**
 * ★ Consumed by mDoMemCd_Ctrl_c::SaveSync so a refused write COMPLETES instead of hanging.
 *
 * This is not optional politeness — without it the in-game save prompt softlocks. dMenu_save_c sits
 * in PROC_MEMCARD_DATA_SAVE_WAIT calling SaveSync() every frame until it returns non-zero
 * (d_menu_save.cpp:1375), and SaveSync only returns non-zero once the card thread has finished the
 * command it was handed. Refuse the write by simply returning and that command never exists, so the
 * menu waits forever. Worse than the bug it was meant to prevent.
 *
 * So the refusal is modelled as what it actually is from the game's point of view — a card write
 * that did not happen — and reported through the path the game already has for that: SaveSync
 * returns 2, dMenu_save_c::memCardDataSaveWait2 (:1408) plays the error sound and shows message
 * 0x3CD, *"An error might have occurred when saving."* The player gets the game's own honest
 * failure plus our toast saying why, rather than a cheerful "Saved." over a write we dropped.
 *
 * Deliberately a Dusk-side latch rather than a new member on mDoMemCd_Ctrl_c: it keeps the decomp
 * class layout untouched and leaves mCardState alone, so nothing about loading is disturbed.
 *
 * Returns true exactly once per refused write, and clears itself.
 */
bool consume_refused_card_write();

/**
 * The same refusal as refuse_card_write(), for CARDFormat: mDoMemCd_Ctrl_c::command_format() is the
 * one place a format is queued (the in-game save prompt and the file select both offer it when a
 * card reads as damaged). A format destroys every save on the card, so it is the most destructive
 * write there is. Sets its own completion latch, consumed by FormatSync, for exactly the reason
 * consume_refused_card_write() exists: both callers poll FormatSync() every frame until it is
 * non-zero, and a format that was never queued would otherwise leave them waiting forever.
 */
bool refuse_card_format();

/// Consumed by mDoMemCd_Ctrl_c::FormatSync. True exactly once per refused format.
bool consume_refused_card_format();

/**
 * ★ The guard for every save-file write that does NOT go through the card thread — upstream's
 * Save Manager (import, restore, delete, backups; src/dusk/save_manager.cpp), mod save sidecars
 * (src/dusk/mods/svc/save.cpp), and the mod file API when it is pointed at save storage
 * (src/dusk/mods/svc/file.cpp). Those write the .gci, the raw card image or the .mods sidecars
 * directly with std::filesystem, so the choke point in mDoMemCd_Ctrl_c::save never sees them.
 *
 * Same sticky rule as the card: once this process has joined as a guest, none of them may write or
 * delete save files for the rest of the process's life. Returns true when the caller must refuse,
 * having toasted (rate limited, shared with the card toast) and logged @p i_what. The caller must
 * still finish whatever the player or a mod is waiting on — every call site returns a failure
 * result rather than skipping a completion.
 */
bool refuse_save_file_write(const char* i_what);

/**
 * True when @p i_path is save storage under the data directory @p i_dataRoot: a memory card folder
 * (`<root>/<region>/Card *`, which also holds the GCI-mode .mods sidecars and the Save Manager's
 * backups), a raw card image or its sidecars (`<root>/MemoryCard*`), the raw-mode Save Manager
 * backups (`<root>/backups`), the startup backups (`<root>/save-backups`) or the legacy
 * `mod_saves.json`. Everything else under the root — mods' own folders, config, logs, caches —
 * is not save storage, which is what keeps mods' own writes working while the guard is engaged.
 *
 * Pure. Compares lexically after making both paths absolute and normal, case-insensitively on
 * Windows; a `..` cannot walk out of the check.
 */
bool is_save_storage_path(
    const std::filesystem::path& i_path, const std::filesystem::path& i_dataRoot);

/**
 * refuse_save_file_write() for the mod file API, whose locations are opaque strings: refuses only
 * when the guard is engaged AND @p i_location resolves to save storage under dusk::ConfigPath, so a
 * mod writing its own files keeps working. A location that does not resolve to a filesystem path
 * (an Android content:// URI) cannot be the app's own card folder and is let through.
 */
bool refuse_save_location_write(std::string_view i_location, const char* i_what);

/// How a guest's session came to an end, so the "saving stays disabled" notice says the right
/// thing.
enum class SessionEnd {
    /// The guest chose to leave (the overlay's Leave, or the test knob).
    LeftByChoice,
    /// The host went away, refused us, or the connection dropped.
    HostGone,
};

/**
 * Tell the player, once, that the session ending did not give them their save back.
 *
 * Stuart, 2026-08-13: *"if the host is gone, fire the notif that saving wont work."* This is the
 * moment the sticky flag stops being self-explanatory — while the session is up, "I can't save"
 * reads as part of playing in someone else's game; the instant it ends, the natural assumption is
 * that your own game resumed. It did not: `g_dComIfG_gameInfo` still holds the host's world.
 *
 * No-op unless the flag is actually set, and fires at most once, so a flaky connection cannot turn
 * it into spam. @p i_how only changes the wording: an earlier version said "Host left" to a guest
 * who had left by choice.
 */
void notify_save_disabled_after_session_end(SessionEnd i_how);

/**
 * ★ Test knob DUSK_MP_TEST_SAVE_GUARD_PROBE=<n>: on the n-th call, drive every guarded path once
 * and log what each one returned, so a scripted two-instance run can prove the refusals — and that
 * they do not hang — from the logs. Called every frame from NetworkManager::pre_actor_tick, before
 * the no-transport early-out, so it also runs after the guest has left (the sticky case).
 *
 * Destructive on purpose where the guard is engaged: it really asks the Save Manager to delete the
 * current save and really queues a card format, because a probe that only asked the predicate would
 * prove nothing about the plumbing. Where the guard is NOT engaged (host, single player) it only
 * runs the non-destructive Save Manager backup, to prove the guard does not over-block. Never set
 * in normal play.
 */
void run_save_guard_probe_if_requested();

}  // namespace dusk::mp
