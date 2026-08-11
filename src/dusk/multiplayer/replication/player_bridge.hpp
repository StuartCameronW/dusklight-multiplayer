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

/// Create the puppet actor for `playerId` if it isn't alive yet. Returns false when creation isn't
/// possible right now (no scene loaded, allocation refused) — callers just retry next tick.
bool ensure_puppet(std::uint32_t playerId, std::uint32_t colorRgb);

/// Push an interpolated pose onto an existing puppet. No-op if the puppet isn't alive.
void apply_puppet_state(std::uint32_t playerId, const PlayerState& state);

/// Read back where the puppet actor actually is. False when it doesn't exist. Distinct from the
/// pose we last pushed: if the two disagree, the puppet is not honouring what it was given.
bool read_puppet_pose(std::uint32_t playerId, PlayerState& out);

/// Despawn one puppet (peer disconnected).
void destroy_puppet(std::uint32_t playerId);

/// Despawn every puppet (session ended, or the scene is being torn down).
void destroy_all_puppets();

}  // namespace dusk::mp
