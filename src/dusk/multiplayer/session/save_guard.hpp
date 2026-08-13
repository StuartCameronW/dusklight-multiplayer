#pragma once

/**
 * The one question every save path must ask multiplayer before it writes bytes to a memory card.
 *
 * Lives next to the session state (network_manager.hpp) on purpose: when this grows a real body it
 * will read that state, and it must not have to move files to do so.
 *
 * Header-only for now because giving it a .cpp would mean editing files.cmake; the moment it needs
 * a real implementation it becomes a declaration here and a definition in a new
 * session/save_guard.cpp added to files.cmake alongside network_manager.cpp.
 */

namespace dusk::mp {

/**
 * True when the in-memory game state has been contaminated by another player's authority, and
 * therefore must NOT be serialised into this machine's own save file.
 *
 * WHAT IT WILL MEAN. The v1 replication design (.claude/plan/04-architecture.md, "Player-state /
 * save split") has the host broadcast flag flips, rupee and item changes, and clients apply them
 * through the same named setters — onEventBit / onSwitch / onTbox / dSv_player_c mutators — into
 * their OWN g_dComIfG_gameInfo. From that first applied packet onward, a client's
 * g_dComIfG_gameInfo is no longer a record of what that player did; it is the host's world wearing
 * the client's filename. Writing it to the client's card overwrites their single-player progress
 * with someone else's. That is precisely the failure the Dolphin-era mod is infamous for
 * (.claude/plan/ 08-tpmp-reference.md) reached by a different route, so the predicate has to
 * become: "has any host-authored state been applied into this process's g_dComIfG_gameInfo since it
 * was loaded from disk, and is that state not separated out into session storage?" A sticky
 * per-session flag set by the replication apply path, cleared only by a fresh load from card, is
 * the expected shape.
 *
 * IT IS A DELIBERATE NO-OP TODAY. It returns false unconditionally, and the compiler will fold the
 * call away entirely. That is correct rather than lazy: as of today the multiplayer subsystem
 * writes nothing at all into g_dComIfG_gameInfo (grep src/dusk/multiplayer for dSv_/gameInfo — no
 * hits), so a save taken during a session is exactly as valid as a single-player save, and
 * returning is_active() instead would block autosave in every session for no safety gained. This
 * must NOT be changed to is_active() as a "safe default" — it would be a behaviour regression that
 * also breaks the two-instance harness, and it would be the wrong question anyway: contamination is
 * about what was applied, not about whether a socket is open.
 *
 * WHY IT EXISTS AT ALL. It costs nothing and it protects nothing yet. Its entire value is that the
 * call site already exists, so the person who writes the first flag-replication apply path finds an
 * empty function with their name on it instead of having to remember, at that moment, that autosave
 * is running behind them. Do not cite this function as evidence that save safety is handled. It is
 * a placeholder with a docstring.
 *
 * KNOWN GAP. The call site this ships with is canAutoSave() (src/dusk/autosave.cpp) only, and
 * autosave is not the only save path.
 *
 * ★ Do NOT put the second guard on dComIfGs_putSave. An earlier draft of this comment said to, and
 * it was wrong in a way that would have been expensive: dComIfGs_putSave
 * (include/d/d_com_inf_game.h:2367) resolves to dSv_info_c::putSave (d_save.cpp:1546), which is
 * pure in-memory bookkeeping — it folds the current stage's temp switches and chests into the
 * persistent table — and d_stage.cpp:2781 calls it on ordinary stage teardown with nothing written
 * to a card. Guarding it would break normal play and would not stop a single byte reaching disk.
 *
 * The actual funnel to disk is mDoMemCd_Ctrl_c::save() (src/m_Do/m_Do_MemCard.cpp:258), the single
 * entry to COMM_STORE_e, with exactly two callers program-wide: src/dusk/autosave.cpp and
 * d_menu_save.cpp:2888 (the in-game save prompt). So the one path still uncovered is a contaminated
 * client walking up to a save point. (src/dusk/imgui/ImGuiSaveEditor.cpp writes game state too, but
 * it is a developer tool the player drives deliberately, not a path a session can trigger.)
 *
 * See .claude/plan/13-save-safety.md.
 */
inline bool save_would_be_contaminated() {
    return false;
}

}  // namespace dusk::mp
