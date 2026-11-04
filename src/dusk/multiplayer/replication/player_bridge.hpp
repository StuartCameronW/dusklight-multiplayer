#pragma once

#include <cstdint>

#include "player_state.hpp"

/**
 * The ONLY seam between multiplayer code and the game engine.
 *
 * Everything else under src/dusk/multiplayer/ is engine-free: no fopAc_ac_c, no cXyz, no actor
 * headers. That is what keeps the protocol and the interpolation testable on their own and stops
 * engine types leaking into the network layer (layering rule, 04-architecture.md).
 *
 * Exactly one translation unit — player_bridge.cpp — includes decomp headers and implements these.
 * Every function must be safe to call when there is no Link, no scene, or no puppet: the tick
 * hooks keep firing through loads, and are skipped entirely during DVD-error and shutdown states.
 */

namespace dusk::mp {

/// True when the game is in real, playable gameplay — not a menu, a load, a cutscene, or the
/// title screen's attract demo. Exported rather than kept private because the autopilot needs the
/// identical test: when the two definitions drifted apart, scripts believed they were in the world
/// several hundred ticks before replication agreed, and the resulting traces were nonsense.
bool world_is_playable();

/// Read the local player's pose. False when Link doesn't currently exist (loading, title screen),
/// in which case nothing should be sent this tick.
bool capture_local_player(PlayerState& out);

/// Create the puppet actor for `playerId` if it isn't alive yet, wearing the outfit the sender
/// reported. Returns false when creation isn't possible right now (no scene loaded, allocation
/// refused) — callers just retry next tick.
///
/// Calling it every tick is correct and cheap: it owns the whole spawn policy, including backing
/// off after a failed creation and eventually disabling a puppet that will not come up. A false
/// return therefore does NOT mean "try harder"; it means there is no puppet this tick, for a reason
/// the log already carries. See the failure-policy block at the top of player_bridge.cpp.
bool ensure_puppet(std::uint32_t playerId, std::uint32_t colorRgb, std::uint8_t outfit);

/// Destroy `playerId`'s puppet if it is wearing an outfit the sender is no longer in, so the next
/// ensure_puppet() rebuilds it in the right clothes. Returns true when it did — the caller should
/// skip the rest of this tick for that player. No-op when the puppet is absent, still being
/// created, or already dressed correctly.
bool reconcile_puppet_outfit(std::uint32_t playerId, std::uint8_t outfit);

/// Push an interpolated pose onto an existing puppet. No-op if the puppet isn't alive.
void apply_puppet_state(std::uint32_t playerId, const PlayerState& state);

/// Read back where the puppet actor actually is. False when it doesn't exist. Distinct from the
/// pose we last pushed: if the two disagree, the puppet is not honouring what it was given.
bool read_puppet_pose(std::uint32_t playerId, PlayerState& out);

/// Read back which gait animation the puppet chose, as an AlAnm resource index. Kept out of
/// PlayerState, which is a wire type: this is local presentation, not replicated state.
bool read_puppet_anim(std::uint32_t playerId, std::uint16_t& out);

/// The BCK resource index in the LOCAL player's under slot 0, i.e. what he is drawing on his legs
/// right now, every substitution applied. For --mp-trace, so a puppet's choice can be compared
/// against what its sender actually played. 0 (false) when there is no playable Link.
bool read_local_anim(std::uint16_t& out);

/// World position of joint 0 (the root) as the LOCAL player's body was last calc'd, for
/// --mp-trace's root-motion check (puppet-action-states.md §4.5). False when there is no playable
/// Link.
bool read_local_root(float& x, float& y, float& z);

/// The same for a puppet: joint 0 as its own calc left it. False when it doesn't exist.
bool read_puppet_root(std::uint32_t playerId, float& x, float& y, float& z);
/// Despawn one puppet (peer disconnected).
void destroy_puppet(std::uint32_t playerId);

/// Despawn every puppet (session ended, or the scene is being torn down).
void destroy_all_puppets();

}  // namespace dusk::mp
