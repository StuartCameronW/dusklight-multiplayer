#pragma once

#include <cstdint>
#include <deque>

#include "dusk/mp_enemy.hpp"
#include "enemy_sync.hpp"

/**
 * Shared between enemy_sync.cpp (registry, authority, wire, ghosts, trace) and enemy_target.cpp
 * (the per-peer target stand-in, the target selector and EnemyTargetScope). Not for anyone else.
 */

class fopAc_ac_c;

namespace dusk::mp::enemies::detail {

/// One authority sample of one enemy, as decoded from EnemyUpdate.
struct Sample {
    std::uint64_t tick = 0;
    float pos[3] = {0.0f, 0.0f, 0.0f};
    std::int16_t shape[3] = {0, 0, 0};
    std::int16_t angleY = 0;
    bool anmValid = false;
    std::uint16_t anm = 0xFFFF;
    float frame = 0.0f;
    float rate = 1.0f;
    std::uint8_t attr = 0;
    std::uint8_t morph = 0;
    std::int16_t health = 0;
    std::uint16_t flags = 0;
    std::uint8_t targetPeer = 0xFF;
    bool engaged = false;
    EnemyExtra extra;
};

/// One adapted, placed enemy this machine has created. Keyed by its process id.
struct Rec {
    std::uint32_t proc = 0xFFFFFFFFu;
    short prof = 0;
    const EnemyNetAdapter* adapter = nullptr;
    std::uint32_t key = 0;
    std::uint32_t check = 0;
    std::uint32_t stageHash = 0;
    std::int8_t room = -1;
    std::int8_t layer = 0;

    Mode mode = Mode::Local;
    std::uint32_t authority = kNoPlayer;
    std::uint16_t epoch = 0;
    bool degraded = false;

    /* ---- ghost side ---- */
    std::deque<Sample> samples;
    /// The sample to show this tick, chosen in pre_tick.
    Sample played;
    bool havePlayed = false;
    bool everSampled = false;
    std::uint64_t ghostSince = 0;
    /// The anm index this ghost last set on its morf (0xFFFF = not set by us yet).
    std::uint16_t ghostAnm = 0xFFFF;
    /// Local tick the last sample flagged engaged arrived on (Rule 1's hold).
    std::uint64_t lastEngagedAt = 0;
    bool everEngaged = false;
    bool splitBrainLogged = false;

    /* ---- authority side ---- */
    /// The chosen target's player id (kNoPlayer = none yet).
    std::uint32_t target = kNoPlayer;
    std::uint32_t challenger = kNoPlayer;
    int challengerTicks = 0;
    std::uint32_t updatesSent = 0;
    float targetDist = -1.0f;

    /* ---- counters (trace, stats, autopilot) ---- */
    /// Ticks the ghost gate drove it. gateAsGhost - ghostTicks is real executes while a ghost.
    std::uint32_t ghostTicks = 0;
    std::uint32_t gateAsGhost = 0;
    std::uint32_t swapTicks = 0;
    std::uint32_t absorbedWrites = 0;
    std::uint32_t realLinkWrites = 0;
    std::uint32_t targetSwitches = 0;

    /* ---- batch 9: enemy hits (phase 2) ---- */
    /// Ghost: the mFlags value the ghost tick last wrote, to spot bits the local Link set since.
    std::uint16_t ghostFlagsWritten = 0;
    bool ghostFlagsValid = false;
    /// Authority: an injected hit this tick, and the HP before it (post_tick logs the result).
    bool injectedThisTick = false;
    std::int16_t hpBeforeInject = 0;
    /// Ghost: EnemyHit / EnemyFlagHit sent. Authority: received, injected, rejected (any reason).
    std::uint32_t hitsSent = 0;
    std::uint32_t hitsRecv = 0;
    std::uint32_t hitsInjected = 0;
    std::uint32_t hitsRejected = 0;
    std::uint32_t flagHitsSent = 0;
    std::uint32_t flagHitsApplied = 0;
    /* --- batch 10A: ghost AT --- Ghost: its AT colliders hit the local Link / of those, shield. */
    std::uint32_t atHitsLocal = 0;
    std::uint32_t atShieldedLocal = 0;
    /* --- end batch 10A --- */

    /* ---- batch 10B: death and drops (phase 3) ---- */
    /// Authority: the fopAcM_createDisappear args it left with (sent in its EnemyDespawn).
    bool haveDisappear = false;
    std::uint8_t disappearSize = 0;
    std::uint8_t disappearType = 0;
    std::uint8_t disappearEnemyId = 0xFF;
    float disappearPos[3] = {0.0f, 0.0f, 0.0f};
    /// Authority: HP was <= 0 (or the Dead flag set) at some post_tick. Kept here because the actor
    /// must not be read in fopAc_Delete (its subclass delete has already run).
    bool deadSeen = false;
    /// Authority: HP was above 0 at some post_tick (HP 0 is a death only after that).
    bool aliveSeen = false;
    /// Authority: who dealt the killing blow (kNoPlayer: not dead, or not through cc_at_check), and
    /// the killer's hearts then (remote killer only: from his EnemyHit).
    std::uint32_t killer = kNoPlayer;
    bool killerRollstab = false;
    std::int16_t killerLife = 0;
    std::int16_t killerMaxLife = 0;
    /// Authority: the last remote attacker whose hit was injected, and his hearts then (the killer
    /// when the death did not come through cc_at_check).
    std::uint32_t lastAttacker = kNoPlayer;
    std::int16_t lastAttackerLife = 0;
    std::int16_t lastAttackerMaxLife = 0;
    /// Ghost: an EnemyDespawn arrived; the next ghost tick plays the smoke and deletes it.
    bool despawnPending = false;
    bool despawnApplied = false;
    std::uint64_t despawnAt = 0;
    EnemyDespawn despawn;
};

/// Sync is live this tick: session live and multiplayer.enemySync on (latched by pre_tick, cleared
/// by reset). Cheap: the decomp seams call it for every actor.
bool enabled();
const EnemyNetAdapter* find_adapter(short prof);
Rec* find_rec(std::uint32_t procId);
/// pre_tick's own counter (local sim ticks since the process started syncing).
std::uint64_t local_tick();
/// Room `room` is loaded on this machine (-1: in a stage at all).
bool local_room_loaded(int room);

/* ---- batch 10B: death and drops ---- */

/// The proc of the synced enemy this end is the authority for whose execute is running now
/// (EnemyTargetScope sets and restores it), else kNoProc.
constexpr std::uint32_t kNoProc = 0xFFFFFFFFu;
std::uint32_t exec_proc();
void set_exec_proc(std::uint32_t proc);

/* ---- enemy_target.cpp ---- */

/// pre_tick: refill one stand-in per remote player from his puppet's pose and PlayerState.
void standins_refresh();
void standins_forget(std::uint32_t playerId);
void standins_reset();
bool is_standin(const fopAc_ac_c* actor);
/// Slot 0 must hold the real Link outside a swapped execute. Repairs it (and logs) if not.
void check_slot0(const char* where);
/// Where player `playerId` is (local Link or the remote's puppet pose). False when unknown.
bool player_position(std::uint32_t playerId, float out[3]);
/// A target change of `rec` (both selectors): logged, counted, challenger reset.
void set_target(Rec& rec, std::uint32_t target, const char* why, float dist, float oldDist);

}  // namespace dusk::mp::enemies::detail
