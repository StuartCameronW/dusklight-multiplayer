#pragma once

/**
 * Dusklight multiplayer, M6 enemy sync: the only surface decomp code touches.
 *
 * Callers: f_op_actor.cpp (fopAc_Create / fopAc_Execute / fopAc_Delete), d_stage.cpp (the actor
 * iterators, for the placement key) and the per-enemy adapters (src/d/actor/d_a_e_<x>_mp.inc),
 * all inside #if TARGET_PC. Implemented in src/dusk/multiplayer/replication/enemy_*.cpp.
 *
 * Dependency-free on purpose, like dusk/multiplayer.hpp: forward declarations only, no STL, no
 * engine headers. Design: .claude/future-plans/mp-m6-enemies.md (§2.2 ghost mode, §3 identity,
 * §5.6 nearest-player aggro).
 *
 * Outside a live session, or with the cvar multiplayer.enemySync off (the default until M6
 * phase 3), every function here is a no-op that answers "vanilla".
 */

class fopAc_ac_c;
class mDoExt_McaMorfSO;
class cCcD_Obj;

namespace dusk::mp {

/// Adapter-defined state bytes an authority captures and a ghost applies (action/mode, sub-state
/// bytes, collider set bits, effect bits). The layout belongs to the adapter.
struct EnemyExtra {
    unsigned char len = 0;
    unsigned char data[24] = {};
};

/// Why an enemy left (phase 3: death, drops, kill flags). Reserved now so the adapter table does not
/// change shape later.
struct EnemyDespawn {
    unsigned char reason = 0;
    unsigned char killerPeer = 0xFF;
    /* --- batch 10B: death and drops --- the authority's fopAcM_createDisappear args (valid when
     * haveDisappear). The ghost replays them with enemy id 0xFF: the smoke, and no drop roll. */
    bool haveDisappear = false;
    bool rollstab = false;
    unsigned char disappearSize = 0;
    unsigned char disappearType = 0;
    unsigned char disappearEnemyId = 0xFF;
    float pos[3] = {0.0f, 0.0f, 0.0f};
};

/**
 * One table entry per adapted enemy profile. Plain function pointers taking fopAc_ac_c*, so no
 * Dusk type leaks into decomp code. A null pointer means "not supported yet"; phase 1 needs arc,
 * morf, captureExtra, applyExtra, present and engaged.
 */
struct EnemyNetAdapter {
    /// The enemy's archive name (e.g. "E_OC"), for the anm pointer -> resource index lookup.
    const char* arc;
    /// The enemy's main skeletal morf; the ghost path drives its animation through it.
    mDoExt_McaMorfSO* (*morf)(fopAc_ac_c* actor);
    void (*captureExtra)(fopAc_ac_c* actor, EnemyExtra& out);
    void (*applyExtra)(fopAc_ac_c* actor, const EnemyExtra& in);
    /// The enemy's OWN matrix + collider set (mtx_set + cc_set, weapon models): no AI, no
    /// damage_check. Makes lock-on, push, being hit and hitting work on a ghost.
    void (*present)(fopAc_ac_c* actor);
    /// Mid-swing / mid-grab: the target must not switch now (§5.6.4).
    bool (*engaged)(fopAc_ac_c* actor);
    /// True: this enemy may target a remote player through EnemyTargetScope (§5.6.6).
    bool remoteTargetOk;
    /* Phase 2+ (null until then). */
    int (*tgIndexOf)(fopAc_ac_c* actor, cCcD_Obj* tg);
    cCcD_Obj* (*tgByIndex)(fopAc_ac_c* actor, int index);
    void (*onBecomeAuthority)(fopAc_ac_c* actor);
    void (*onGhostDeath)(fopAc_ac_c* actor, const EnemyDespawn& why);
    /* --- batch 10A: ghost AT --- */
    /// Ghost only, read before applyExtra: how many of the ghost's AT colliders hit THIS machine's
    /// Link in the last collision pass, and (out) how many of those he blocked with his shield.
    /// Counting only; the damage itself is Link's own TG path (spec 5.3).
    int (*ghostAtHitsLocal)(fopAc_ac_c* actor, int* shielded);
    /* --- end batch 10A --- */
};

/// Register an adapter for an fpcNm_* profile. Call from the adapter .inc (a file-scope static
/// initializer is fine: the table is plain storage). A second call for a profile replaces the first.
void enemy_register_adapter(short profName, const EnemyNetAdapter* adapter);

/// Placement key for an actor about to be created from stage data: a hash of the current stage,
/// layer, roomNo, chunkTag (e.g. 'ACTR', 'TRES', 'SCOB') and the entry's index in its chunk.
/// Called from the d_stage.cpp iterators; never 0 (0 means "not from stage data").
unsigned int enemy_placement_key(int roomNo, unsigned int chunkTag, int entryIndex);

/// fopAc_Create, after the append has been copied: remember the actor's placement key (0 = none).
void enemy_note_created(fopAc_ac_c* actor, unsigned int placementKey);

/// fopAc_Delete (or the actor's final teardown): forget it.
void enemy_note_deleted(fopAc_ac_c* actor);

/// fopAc_Execute, inside the original execute gate, immediately before the subclass execute.
/// True: the actor is a synced enemy that is a ghost here and was driven from the network; the
/// caller must skip its execute. False for everything else (the original path runs).
bool enemy_ghost_execute(fopAc_ac_c* actor);

/**
 * fopAc_Execute, on the line before enemy_ghost_execute: on the authority, for a synced enemy whose
 * chosen target is a remote player, points player slot 0 at that player's stand-in for the
 * duration of the subclass execute and puts Link back on destruction. A no-op otherwise.
 */
class EnemyTargetScope {
public:
    explicit EnemyTargetScope(fopAc_ac_c* actor);
    ~EnemyTargetScope();

private:
    EnemyTargetScope(const EnemyTargetScope&);
    EnemyTargetScope& operator=(const EnemyTargetScope&);
    void* mSavedPlayer;
    bool mActive;
    /* --- batch 10B: death and drops --- the executing authority enemy (enemy_drop_shared). */
    unsigned int mPrevExecProc;
    bool mExecSet;
};

/* --- batch 9: enemy hits (M6 phase 2, mp-m6-enemies.md §5.1, §5.6.5 items 3 and 7) --- */

/// True while enemy sync is live (session live and multiplayer.enemySync on). The d_cc_uty.cpp
/// hooks then read the attacker from the LinkPlayer slot instead of player slot 0, which may hold
/// a target stand-in inside a swapped enemy execute (§5.6.5 item 3). False: vanilla.
bool enemy_sync_live();

/// What an injected remote hit carries in place of the local player's state. Filled on the
/// authority from an EnemyHit event; the proxy AT collider it belongs to lives until the next tick.
struct EnemyHitOverride {
    /// at_power_check's results as the ATTACKER's machine computed them (dCcU_AtInfo fields).
    unsigned char hitType;
    unsigned int hitBit;
    signed char hitStatus;
    /// The attacker's own Link at the time of the hit (cc_at_check's sword modifiers etc.).
    bool masterSword;
    bool woodSword;
    bool wolf;
    bool swordUp;
    bool headJump;
    bool cutJumpCancelTurn;
    bool horseRide;
    short shapeAngleY;
    /* --- batch 10B: death and drops --- who hit, and his hearts then (the kill drop's roll). */
    unsigned int attacker;
    short life;
    short maxLife;
    bool rollstab;
};

/// Non-null when `atObj` is the proxy AT collider of an injected remote hit: at_power_check and
/// cc_at_check take the hit type, bits and sword modifiers from it, and cc_at_check does not start
/// the global hit-stop (the remote attacker's machine runs its own). Null for every real collider.
const EnemyHitOverride* enemy_hit_override(const cCcD_Obj* atObj);

/* --- batch 10B: death and drops (M6 phase 3, mp-m6-enemies.md §5.4) --- */

/// fopAcM_createDisappear: `actor` leaves through the disappear smoke with these args. Kept, on the
/// authority of a synced enemy, for the EnemyDespawn its deletion sends. A no-op otherwise.
void enemy_note_disappear(const fopAc_ac_c* actor, float x, float y, float z, unsigned char size,
    unsigned char type, unsigned char enemyId);

/// cc_at_check, on the hit that takes an enemy's HP to 0: who killed it (`mpHit` non-null: that
/// injected hit's remote attacker; null: this end's own Link). A no-op for an unsynced enemy.
void enemy_note_killing_blow(const fopAc_ac_c* enemy, const EnemyHitOverride* mpHit);

/// fopAcM_createItemFromEnemyID: true while a synced enemy this end is the authority for executes.
/// Its kill drop is then shared (batch 7's ObjKind::Drop, one drop, first come) instead of staying
/// this player's own. False otherwise (vanilla / batch 7 rule).
bool enemy_drop_shared();

/// The drop rolls that read the local hearts (fopAcM_getItemNoFromTableNo; tableNo -1 for a direct
/// drop): inside a synced authority enemy's execute whose killer is a remote player, replaces
/// `life` and `maxLife` with the killer's (§5.4 item 4) and logs the roll. Untouched otherwise.
void enemy_roll_life(int tableNo, int& life, int& maxLife);

/// fopAcM_getItemNoFromTableNo, after the hearts picked the table variant: logs it on the
/// authority of the executing synced enemy (Test 3's low-life check). A no-op otherwise.
void enemy_note_roll_table(int tableIn, int tableOut);

}  // namespace dusk::mp
