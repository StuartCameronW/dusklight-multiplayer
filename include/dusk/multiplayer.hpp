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

}  // namespace dusk::mp
