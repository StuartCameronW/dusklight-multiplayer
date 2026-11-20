#pragma once

#include <cstdint>

#include "../replication/player_state.hpp"

/**
 * Batch 8: the Dying Light pause model (.claude/future-plans/mp-pause.md, decisions in
 * .claude/plan/17-decisions-2026-11-20.md).
 *
 * The game pauses only when EVERY player is in a menu. One player alone in a menu keeps his world
 * running behind it: his Link can be hit, the clock runs, puppets keep moving, timers keep
 * counting. The decomp-facing half of the API (world_frozen(), the input scope, the force-close,
 * the Collect-doll and warp deferrals, the hearts flash) is in dusk/multiplayer.hpp; this header is
 * the Dusk-side half, for the network manager and the autopilot.
 *
 * Outside a live session nothing here changes anything: world_frozen() is the vanilla menu flag.
 */

namespace dusk::mp::pause {

/// Once per sim tick, from pre_actor_tick, after the network drain (so every partner's bit is the
/// newest one received). Edges, logs, the hearts-flash timer, the deferral latches.
void pre_actor_tick();

/// Set (or clear) PlayerState::gear's kPlayerGearInMenu from the local player as he is NOW. Both of
/// wire_local_state's paths call it, so a held pose still carries a current bit.
void fill_wire_bit(PlayerState& state);

/// The local player has a menu open: dMeter2Info_getWindowStatus() != 0, or the menu pause flag.
bool local_in_menu();

/// For the autopilot's PASS/FAIL lines and the logs.
struct Stats {
    bool sessionLive = false;
    bool frozen = false;
    bool localInMenu = false;
    int windowStatus = 0;
    /// In-world partners, and how many of them report being in a menu.
    int countedPeers = 0;
    int pausedPeers = 0;
    /// How many hearts flashes have started since the counter was last reset.
    int heartsFlashes = 0;
    bool heartsFlashing = false;
    /// Force-closes performed since the counter was last reset, and the last reason.
    int forceCloses = 0;
    int lastForceCloseReason = 0;
    /// Actor/camera executes that ran with the pad zeroed on the last tick.
    int isolatedExecsLastTick = 0;
    bool collectDollHidden = false;
};
Stats stats();

/// Reset the counters stats() reports (heartsFlashes, forceCloses), for a script's "since" checks.
void reset_counters();

const char* force_close_reason_name(int reason);

}  // namespace dusk::mp::pause
