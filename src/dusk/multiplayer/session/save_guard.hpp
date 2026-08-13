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
 * Tell the player, once, that losing the host did not give them their save back.
 *
 * Stuart, 2026-08-13: *"if the host is gone, fire the notif that saving wont work."* This is the
 * moment the sticky flag stops being self-explanatory — while the session is up, "I can't save"
 * reads as part of playing in someone else's game; the instant it ends, the natural assumption is
 * that your own game resumed. It did not: `g_dComIfG_gameInfo` still holds the host's world.
 *
 * No-op unless the flag is actually set, and fires at most once, so a flaky connection cannot turn
 * it into spam.
 */
void notify_save_disabled_after_host_left();

}  // namespace dusk::mp
