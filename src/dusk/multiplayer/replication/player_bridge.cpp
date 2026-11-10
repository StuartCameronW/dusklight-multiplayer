#include "player_bridge.hpp"

#include "world_clock.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <unordered_map>

#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "SSystem/SComponent/c_counter.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_arrow.h"
#include "d/actor/d_a_remote_player.h"
#include "d/d_com_inf_game.h"
#include "dusk/logging.h"
#include "dusk/player_fx.hpp"
#include "f_op/f_op_actor_mng.h"
#include "m_Do/m_Do_mtx.h"

/**
 * The one translation unit allowed to include engine headers (see player_bridge.hpp).
 *
 * Everything here has to tolerate being called when there is no scene, no Link and no puppet: the
 * tick hooks fire during loads, and are skipped entirely during DVD-error and shutdown states, so
 * this code sees discontinuities rather than a tidy lifecycle.
 */

namespace dusk::mp {
namespace {

constexpr borealis::Log Log{"dusk::mp"};

/*
 * ── Puppet spawn failure policy ──────────────────────────────────────────────────────────────
 *
 * Everything below is counted in SIM TICKS. ensure_puppet() is called exactly once per sim tick per
 * player from ReplicationManager::drive_puppets(), and the sim runs at 30 Hz (network_manager.cpp
 * measures and reports this), so 30 ticks is one second.
 *
 * The failure this exists for: create() returning cPhs_ERROR_e used to be indistinguishable from
 * "a room unload took the puppet", so every permanent failure produced a full-speed respawn loop —
 * hundreds of "Puppet heap/model setup failed" / "vanished; respawning" pairs, one every couple of
 * ticks. The log flood was the visible part; the real cost is that each attempt runs create(),
 * which starts a private dRes_info_c archive mount before it ever reaches the heap that fails.
 */

/// First retry delay after a creation failure; doubles with each consecutive failure. One second is
/// chosen so that a permanent failure costs about one archive mount per second instead of one per
/// tick, while a genuinely transient one (a room that happened to be at peak heap use as we asked)
/// is picked up almost immediately.
constexpr int kRetryBaseTicks = 30;

/// Ceiling on the doubling, 10 s. With the give-up threshold below, only the last delay is actually
/// clamped by it (480 -> 300); it exists so that raising kMaxCreateFailures cannot silently turn
/// into a multi-minute wait.
constexpr int kRetryMaxTicks = 300;

/// Consecutive creation failures tolerated before the puppet is disabled for this location. The
/// delays are then 30 + 60 + 120 + 240 + 300 = 750 ticks, so a failing puppet is retried over ~25 s
/// and costs six archive mounts before we stop. Six rather than two or three because the most
/// likely cause — the 0x20000 solid heap not fitting in what the room has left — genuinely does
/// come and go as other actors load and unload, and giving up on the first miss would make the
/// puppet vanish for the rest of the room over a momentary squeeze.
constexpr int kMaxCreateFailures = 6;

/// How long a puppet may sit in the actor framework's create phase before that is reported, 10 s.
/// ★ This is NOT a failure test and NOT a respawn trigger. create() legitimately returns
/// cPhs_LOADING_e for as many frames as the private archive mount needs, and the framework tells us
/// the moment it gives up (see look_up_puppet). The old code could not tell those apart and used a
/// 4 s timer as a stand-in for both, which is precisely why a permanent failure looked like a
/// vanished puppet. This constant now only buys one log line for a mount that never finishes.
constexpr int kCreateStuckTicks = 300;

/// Floor on how often an outfit change may respawn one player's puppet; ~1 s at 30 Hz.
///
/// A real clothes change happens in the pause menu and is rare, so a second of latency on it costs
/// nothing. What this buys is a bound on the pathological case: poses arrive unreliably and
/// unauthenticated, so a single flipping byte — or a peer genuinely rummaging through the menu —
/// must not be able to destroy and rebuild an actor (archive mount included) every tick.
constexpr int kOutfitRespawnCooldownTicks = 30;

/// v21: the create parameter's top byte for a WOLF puppet (puppet-wolf.md §6), in the slot the
/// outfit uses for a human. Pre-assigned so piece B's isWolf() reads the same value; any byte
/// >= l_outfitNum already resolves to the default clothes on an older build
/// (daRemotePlayer_outfitFromWire), so it cannot be mistaken for an outfit.
constexpr std::uint8_t kPuppetParamWolf = 0xFE;

/*
 * ★ Do NOT add a "wait N settled ticks before creating the puppet" gate here.
 *
 * That was tried on 2026-08-12 as a mitigation for a hang, on the theory that creating a puppet on
 * the first playable tick after an event ends was too early. It is a reasonable-sounding theory and
 * it is wrong: with a 15-tick delay the joining instance hung on the puppet's first calc() in two
 * runs out of two, and with the delay removed it completed 1,076 in-world ticks. Delaying creation
 * did not avoid a bad moment, it moved creation INTO one.
 *
 * The useful part of that result is the clue: puppet creation is sensitive to where in the room's
 * lifecycle it lands, which is worth knowing when the real hang is finally chased. Whatever the
 * eventual fix is, it will not be a timer.
 */

/**
 * Where the local player is, as the puppet-spawn policy cares about it.
 *
 * A creation failure is usually local to one room: the puppet's 0x20000 solid heap has to fit in
 * what is left of the actor heap, and what is left depends entirely on what else that room loaded.
 * So leaving the room, or the stage, is the signal that a failure is worth re-trying.
 */
struct Location {
    /// dStage_startStage_c::mName is a char[8] which need not be NUL-terminated; the ninth byte
    /// keeps it printable and comparable as a plain C string.
    char stage[9] = {};
    int roomNo = -1;
};

bool same_location(const Location& a, const Location& b) {
    return a.roomNo == b.roomNo && std::strcmp(a.stage, b.stage) == 0;
}

/// False while there is no scene at all (a load, the title screen), where the answer would be
/// meaningless rather than merely unknown.
bool current_location(Location& out) {
    fopAc_ac_c* link = dComIfGp_getPlayer(0);
    if (link == nullptr) {
        return false;
    }
    // getStartStageName() is the stage we are IN, not one we are heading to: dComIfGp_setStartStage
    // is what d_s_play.cpp:1157 does as the new stage begins, and the game's own "am I in stage X"
    // tests read it (d_a_alink.cpp:2050). It always points at dStage_startStage_c::mName, a char[8]
    // member of the game-info block, so copying eight bytes out of it is always in bounds.
    std::memcpy(out.stage, dComIfGp_getStartStageName(), 8);
    out.roomNo = fopAcM_GetRoomNo(link);
    return true;
}

struct PuppetRef {
    /// The live (or in-flight) puppet's process id, or fpcM_ERROR_PROCESS_ID_e when there is none —
    /// nothing requested yet, or the last request failed and we are backing off.
    fpc_ProcID id = fpcM_ERROR_PROCESS_ID_e;
    /// True once the actor has resolved at least once, i.e. creation demonstrably works here. This
    /// is what separates "vanished" from "never made it": see ensure_puppet().
    bool everResolved = false;
    int ticksSinceRequest = 0;
    /// Latches the stuck-in-create report, so it stays one line rather than one per tick.
    bool loggedStuck = false;

    /* --- Failure policy. Reset by a location change, by the puppet coming up, and by the player
     * leaving (destroy_puppet erases the whole entry). --- */
    int consecutiveFailures = 0;
    /// Ticks left before the next attempt. Counted down in ensure_puppet(), which returns early
    /// while it is non-zero — the early return is the thing that stops the per-tick archive mount.
    int retryCooldown = 0;
    /// Ticks elapsed since the first failure of the current run, purely so the give-up line can say
    /// how long this went on for.
    int failureWindowTicks = 0;
    /// True once we have stopped retrying. Cleared only by the re-arm conditions above.
    bool disabled = false;
    /// The actor's own account of the last failure. A string literal or null; never owned.
    const char* lastReason = nullptr;

    /// Where we were when this was last looked at. Any change re-arms the policy.
    Location location;
    bool locationValid = false;
};

/// playerId -> the puppet's process id. Process ids are re-resolved to pointers every tick via
/// fopAcM_SearchByID and never cached: the actor can be destroyed by a room unload at any time,
/// and a stale fopAc_ac_c* would be a use-after-free.
std::unordered_map<std::uint32_t, PuppetRef> s_puppets;

/// What the actor framework currently thinks of a requested puppet.
enum class PuppetLookup {
    Alive,     ///< The actor exists and is in the actor pass.
    Creating,  ///< The framework still holds a create request for it: create() is returning
               ///< cPhs_LOADING_e, which it does legitimately for as long as the mount takes.
    Gone,      ///< Neither a request nor an actor. Either creation ended in cPhs_ERROR_e (the
               ///< framework cancels the request and destroys the process) or something tore down
               ///< an actor that had been alive.
};

/**
 * ★ The loading-vs-gone distinction, and the reason this policy is not just a timer.
 *
 * fopAcM_SearchByID's RETURN VALUE carries information the out-pointer does not
 * (f_op_actor_mng.cpp:152-163): it returns 1 with a NULL actor while the framework still holds a
 * create request for that id, and 0 only when there is neither a request nor a process in the actor
 * layer. A process joins that layer only on cPhs_COMPLEATE_e (fpcCtRq_Do -> fpcEx_ToExecuteQ,
 * f_pc_create_req.cpp:104-109), and cPhs_ERROR_e takes the opposite branch — fpcCtRq_Cancel, which
 * deletes the request there and then (f_pc_create_req.cpp:110-117). So "1 with NULL" means still
 * loading, and "0" means the create request is finished with, one way or the other, on the very
 * next tick after it happens. No grace period is needed to tell them apart, and none is used.
 *
 * The old code discarded that return value, so it had only "the pointer is NULL" to work with and
 * had to guess with a 4 s timer — which is what made a permanent failure look like a puppet that
 * had merely wandered off.
 */
PuppetLookup look_up_puppet(std::uint32_t playerId, daRemotePlayer_c** o_puppet) {
    *o_puppet = nullptr;

    const auto it = s_puppets.find(playerId);
    if (it == s_puppets.end() || it->second.id == fpcM_ERROR_PROCESS_ID_e) {
        return PuppetLookup::Gone;
    }

    fopAc_ac_c* actor = nullptr;
    // The two-argument form reports NULL while the actor is still mid-create, where the inline
    // one-argument overload would hand back a half-constructed process.
    const bool known = fopAcM_SearchByID(it->second.id, &actor) != 0;
    if (actor == nullptr) {
        return known ? PuppetLookup::Creating : PuppetLookup::Gone;
    }

    daRemotePlayer_c* const puppet = static_cast<daRemotePlayer_c*>(actor);

    /* ★ Findable is NOT the same as created, and this cost a test run to learn. create() can log
     * its failure, return cPhs_ERROR_e, and STILL be handed back by fopAcM_SearchByID on the next
     * tick — measured, with createHeap forced to return 0. Treating that as "alive" set
     * everResolved on a puppet that never existed, so the failure below was classified as a vanish
     * and respawned immediately, forever: 234 failures and 233 respawns in one 200 s run, with the
     * backoff never once engaging. So take the actor's own explicit end-of-create() flag instead of
     * inferring success from the framework's bookkeeping.
     *
     * Reporting Creating rather than Gone here is correct for both cases it covers: a puppet that
     * is genuinely still mounting, and a failed one in the tick before the framework drops it. The
     * failed one becomes Gone on the next tick with everResolved still false, which is exactly the
     * path that counts it as a creation failure. */
    if (!puppet->createComplete()) {
        return PuppetLookup::Creating;
    }

    if (!it->second.everResolved) {
        it->second.everResolved = true;
        // One line, once, on the tick the actor first becomes resolvable. The hang on 2026-08-11
        // left a log ending at "using body archive" with the actor's own first-calc dump absent,
        // which narrowed it to "after create() returned, before the first execute() did any work"
        // and no further — because nothing was logged in between. This is that missing line.
        Log.debug("Puppet for player {} resolved after {} tick(s); entering the actor pass",
            playerId, it->second.ticksSinceRequest);
    }

    *o_puppet = puppet;
    return PuppetLookup::Alive;
}

daRemotePlayer_c* resolve_puppet(std::uint32_t playerId) {
    daRemotePlayer_c* puppet = nullptr;
    look_up_puppet(playerId, &puppet);
    return puppet;
}

/// Take the actor's account of why creation failed, if it is about this player. Cleared as it is
/// read so that a later, unrelated failure can never inherit a stale reason; null when the failure
/// came from somewhere that never reached create() at all.
const char* take_failure_reason(std::uint32_t playerId) {
    if (g_daRemotePlayer_lastCreateFail.mReason == nullptr ||
        g_daRemotePlayer_lastCreateFail.mPlayerId != playerId)
    {
        return nullptr;
    }
    const char* reason = g_daRemotePlayer_lastCreateFail.mReason;
    g_daRemotePlayer_lastCreateFail.mReason = nullptr;
    return reason;
}

const char* reason_text(const PuppetRef& ref) {
    return ref.lastReason != nullptr ? ref.lastReason : "not reported by the actor";
}

/**
 * One failed creation: count it, schedule the next attempt, or stop.
 *
 * The give-up line is deliberately fat. A backoff that hides a real bug behind a tidy "gave up" is
 * worse than the log flood it replaces, so the line has to carry everything needed to work out the
 * cause without the reader having to correlate timestamps: how many attempts, over how long, the
 * actor's own reason, where it happened, and what would make us try again.
 */
void note_create_failure(std::uint32_t playerId, PuppetRef& ref, const char* reason) {
    ref.id = fpcM_ERROR_PROCESS_ID_e;
    ref.everResolved = false;
    ref.ticksSinceRequest = 0;
    ref.loggedStuck = false;
    if (reason != nullptr) {
        ref.lastReason = reason;
    }
    ref.consecutiveFailures++;

    if (ref.consecutiveFailures >= kMaxCreateFailures) {
        ref.disabled = true;
        Log.error(
            "Puppet for player {} DISABLED after {} consecutive creation failures over {} tick(s). "
            "Cause: {}. Last attempt in stage '{}' room {}. Nothing will retry it until that "
            "player's puppet is asked for in a different room or stage, or the peer leaves and "
            "rejoins. If a player is simply invisible from here on, this line is why.",
            playerId, ref.consecutiveFailures, ref.failureWindowTicks, reason_text(ref),
            static_cast<const char*>(ref.location.stage), ref.location.roomNo);
        return;
    }

    // 30, 60, 120, 240, then clamped: doubled once per failure already recorded. Written as a loop
    // that stops at the ceiling rather than a shift, so raising kMaxCreateFailures can never turn
    // this into a shift wider than an int.
    int delay = kRetryBaseTicks;
    for (int doubled = 1; doubled < ref.consecutiveFailures && delay < kRetryMaxTicks; doubled++) {
        delay *= 2;
    }
    if (delay > kRetryMaxTicks) {
        delay = kRetryMaxTicks;
    }
    ref.retryCooldown = delay;
    Log.warn("Puppet for player {} failed to spawn ({}); attempt {} of {}, next try in {} tick(s)",
        playerId, reason_text(ref), ref.consecutiveFailures, kMaxCreateFailures, delay);
}

/// The enumerator each PlayerIdleKind stands for, for the log line below. Indexed by the wire
/// value, so a value this table does not cover is reported as such rather than reading off the end.
const char* idle_kind_name(std::uint8_t kind) {
    switch (kind) {
    case kPlayerIdleWait:
        return "ANM_WAIT";
    case kPlayerIdleWaitB:
        return "ANM_WAIT_B";
    case kPlayerIdleTired:
        return "ANM_WAIT_TIRED";
    case kPlayerIdleService:
        return "ANM_SERVICE_WAIT";
    case kPlayerIdleWind:
        return "ANM_WAIT_WIND";
    case kPlayerIdleInsect:
        return "ANM_WAIT_INSECT";
    case kPlayerIdleAtnLeft:
        return "ANM_ATN_WAIT_LEFT";
    case kPlayerIdleAtnRight:
        return "ANM_ATN_WAIT_RIGHT";
    default:
        return "unnamed";
    }
}

/**
 * Fill the idle byte with the animation daAlink_c is genuinely playing.
 *
 * ★ This reads the sender's ANSWER, not his inputs. checkUnderMove0BckNoArc (d_a_alink.cpp:6951)
 * compares mUnderAnmHeap[0].getIdx() against getMainBckData(id)->m_underID — i.e. the resource the
 * lower body is loaded with RIGHT NOW, with every substitution the state machine applied already
 * baked in. Re-deriving an animation choice from Link's flags has been wrong three times on this
 * project (the gait speed twice, the standing predicate once); asking what he is playing cannot be
 * wrong in that way, because it is not a prediction.
 *
 * The ORDER is load-bearing and is not a priority ranking. During a morf mUnderAnmHeap[0] holds
 * exactly one animation, so at most one of these tests can pass and the first match IS the answer;
 * the chain is written as else-if only to stop asking once it has been answered.
 *
 * Slot 1 (checkUnderMove1BckNoArc) is deliberately not consulted: that is the OUTGOING animation
 * being blended away, so reading it would report the idle he is LEAVING for the length of every
 * transition.
 *
 * ANM_WAIT itself is the default rather than a case, because it is what "none of the above" means:
 * it is the idle daAlink_c falls back to, and it is what a null alink has to report.
 */
void fill_idle_kind(daAlink_c* alink, PlayerState& out) {
    out.idleKind = kPlayerIdleWait;
    if (alink != nullptr) {
        if (alink->checkUnderMove0BckNoArc(daAlink_c::ANM_ATN_WAIT_LEFT)) {
            out.idleKind = kPlayerIdleAtnLeft;
        } else if (alink->checkUnderMove0BckNoArc(daAlink_c::ANM_ATN_WAIT_RIGHT)) {
            out.idleKind = kPlayerIdleAtnRight;
        } else if (alink->checkUnderMove0BckNoArc(daAlink_c::ANM_SERVICE_WAIT)) {
            out.idleKind = kPlayerIdleService;
        } else if (alink->checkUnderMove0BckNoArc(daAlink_c::ANM_WAIT_TIRED)) {
            out.idleKind = kPlayerIdleTired;
        } else if (alink->checkUnderMove0BckNoArc(daAlink_c::ANM_WAIT_B)) {
            out.idleKind = kPlayerIdleWaitB;
        } else if (alink->checkUnderMove0BckNoArc(daAlink_c::ANM_WAIT_WIND)) {
            out.idleKind = kPlayerIdleWind;
        } else if (alink->checkUnderMove0BckNoArc(daAlink_c::ANM_WAIT_INSECT)) {
            out.idleKind = kPlayerIdleInsect;
        }
    }

    /* One line, once, the first time this instance reports anything other than the plain idle.
     *
     * It exists because the alternative is an unfalsifiable claim. Every idle here plays only in a
     * narrow situation — the alert idles need a lock-on, the tired one needs low hearts — so "the
     * alert idle works" is otherwise established by one person watching another person's screen and
     * saying it looked right. This line makes it a fact with a timestamp: if it never appears, the
     * sender never sampled one, and no amount of receiver work was ever going to show one. */
    static bool s_loggedIdleKind = false;
    if (!s_loggedIdleKind && out.idleKind != kPlayerIdleWait) {
        s_loggedIdleKind = true;
        Log.debug("Local idle kind {} ({}) sampled off the sender's animation heap and sent; see "
                  "fill_idle_kind(). This is the only line — later changes are not reported.",
            static_cast<unsigned>(out.idleKind), idle_kind_name(out.idleKind));
    }
}

/**
 * Fill the equipment byte from the sender's own answers.
 *
 * Nothing here is a fact about the world that the receiver could look up; every one is a decision
 * daAlink_c made this tick. See PlayerEquipFlags for why each is a wire bit rather than a
 * derivation — the short version is that `checkSwordDraw()` folds in a change-over timer and two
 * no-draw flags, and the shield's in-hand test is a seven-term disjunction over guard and demo
 * state.
 *
 * ★ The KIND fields are reported even when the matching draw bit is clear, and that is not sloppy:
 * daAlink_c does the same, because it must have a model and an archive selected before it can
 * decide whether to draw them. A receiver that only mounted a shield archive when the shield was
 * currently visible would have nothing loaded at the moment it became visible.
 *
 * The whole byte is filled from the first protocol version that carries it, including the shield
 * half the puppet does not draw yet, so that finishing the shield costs no second bump.
 */
void fill_equip(daAlink_c* alink, PlayerState& out) {
    out.equip = 0;
    if (alink == nullptr) {
        return;
    }

    /* Order copied from setSelectEquipItem (d_a_alink.cpp:4354-4366), wood first, because the tests
     * are not mutually exclusive on their own — the wooden sword also answers "not the master
     * sword", so testing for the master sword first would still be right but testing for the ordon
     * sword first would claim every sword. */
    std::uint8_t swordKind;
    if (daPy_py_c::checkWoodSwordEquip()) {
        swordKind = kPlayerEquipSwordWood;
    } else if (daPy_py_c::checkMasterSwordEquip()) {
        swordKind = kPlayerEquipSwordMaster;
    } else {
        swordKind = kPlayerEquipSwordOrdon;
    }
    out.equip |= static_cast<std::uint8_t>(swordKind << kPlayerEquipSwordKindShift);

    if (alink->checkSwordDraw()) {
        out.equip |= kPlayerEquipSwordDraw;
    }
    /* 0x103 is daAlink_c's "sword in hand" pseudo item — NOT dItemNo_SWORD_e, which is 0x28 and is
     * mapped onto 0x103 at create (d_a_alink.cpp:4716-4717). Written as the literal setItemMatrix
     * places the blade by (:5907) so the two read identically.
     *
     * ★ Deliberately independent of the draw bit above, as it is in daAlink_c: in hand while not
     * drawn is a real state (see kPlayerEquipSwordInHand). Checked against a trace before keeping
     * this line unchanged — the "in hand but undrawn" ticks were a stage arrival, not a B tap. */
    if (alink->mEquipItem == 0x103) {
        out.equip |= kPlayerEquipSwordInHand;
    }

    // setShieldArcName's order, and its first branch really does fold "no shield at all" in with
    // the carved wooden one (d_a_alink_swindow.inc:29-30).
    std::uint8_t shieldKind;
    if (daPy_py_c::checkCarvingWoodShieldEquip() || !daPy_py_c::checkShieldGet()) {
        shieldKind = kPlayerEquipShieldCarvingWood;
    } else if (daPy_py_c::checkShopWoodShieldEquip()) {
        shieldKind = kPlayerEquipShieldShopWood;
    } else {
        shieldKind = kPlayerEquipShieldHylian;
    }
    out.equip |= static_cast<std::uint8_t>(shieldKind << kPlayerEquipShieldKindShift);

    if (alink->checkShieldDraw()) {
        out.equip |= kPlayerEquipShieldDraw;
    }

    /* The seven-term test from setItemMatrix (d_a_alink.cpp:5921-5930), transcribed rather than
     * summarised. mShieldChangeWaitTimer != 0 is the outer gate there and means "leave the shield
     * exactly where it was"; there is no such memory here, so it is folded in as "on the back",
     * which is where the shield sits for all but the guarding frames anyway.
     *
     * ★ checkShieldGet() is ANDed in, and leaving it out was a real bug (2026-08-13). It is not
     * part of the disjunction — it is the test INSIDE the branch that decides which way
     * field_0x2e44's 0xF pass flag goes (:5931-5935), and that flag is what makes setDrawHand show
     * hand pose 6. With no shield owned, onPassNum(0xF) fires and his right hand falls through to
     * the ANIMATION's own pose; running around shieldless is an ordinary thing to do in this game
     * and he does not grip then. Without this term a shieldless puppet gripped an invisible shield.
     *
     * Folding it in here rather than testing it on the receiver keeps the bit meaning one thing.
     * Nothing is lost for D1b either: he does not draw a shield he does not own, so "positioned at
     * the hand but not owned" is not a state anything needs to render. */
    if (alink->mShieldChangeWaitTimer == 0 && daPy_py_c::checkShieldGet() &&
        ((alink->checkPlayerGuardAndAttack() && alink->mEquipItem != dItemNo_IRONBALL_e &&
             !alink->checkModeFlg(0x400)) ||
            alink->checkNoResetFlg0(daPy_py_c::FLG0_UNK_2) ||
            (alink->mProcID == daAlink_c::PROC_TOOL_DEMO && alink->mProcVar4.field_0x3010 != 0) ||
            (alink->mProcID == daAlink_c::PROC_CUT_REVERSE && alink->mProcVar2.field_0x300c != 0) ||
            alink->mProcID == daAlink_c::PROC_GUARD_BREAK ||
            (alink->mEquipItem == 0x103 &&
                !alink->checkEndResetFlg1(daPy_py_c::ERFLG1_SHIELD_BACKBONE) &&
                !alink->checkModeFlg(0x400))))
    {
        out.equip |= kPlayerEquipShieldInHand;
    }
}

/**
 * Fill the gear byte. Bit 0 is the iron boots, as daAlink_c's own checkEquipHeavyBoots() — the
 * flag setHeavyBoots raises together with Z2CreatureLink's boots type, so it is exactly what
 * decides his heavy footstep. See kPlayerGearHeavyBoots for why it is not checkBootsOrArmorHeavy().
 *
 * Read off the same member that sits beside checkMagneBootsOn(), which the foot-IK gate above
 * already calls on this pointer. A wolf never has the flag set, so he sends it clear.
 *
 * Bit 1 is the heavy GAIT, read off his animation heap rather than re-derived from the boots.
 * Slot 1 of setBlendMoveAnime's blend holds ANM_WALK_HEAVY in every band of the booted branch,
 * standing included (d_a_alink.cpp:7626-7627), and a one-shot resets slot 1 (:7164), so this
 * matches only while that branch is what his legs are playing. See kPlayerGearHeavyGait.
 */
void fill_gear(daAlink_c* alink, PlayerState& out) {
    out.gear = 0;
    if (alink != nullptr && alink->checkEquipHeavyBoots()) {
        out.gear |= kPlayerGearHeavyBoots;
    }
    if (alink != nullptr && !alink->checkWolf() &&
        alink->checkUnderMove1BckNoArc(daAlink_c::ANM_WALK_HEAVY))
    {
        out.gear |= kPlayerGearHeavyGait;
    }
    // Bit 2, the iron-ball carry gait: the same read on the same slot. See kPlayerGearIronBallGait.
    if (alink != nullptr && !alink->checkWolf() &&
        alink->checkUnderMove1BckNoArc(daAlink_c::ANM_IRONBALL_WALK))
    {
        out.gear |= kPlayerGearIronBallGait;
    }
    // Bits 3-6 (agent G): the outfit's material state and the shield burn, each the value his own
    // draw code reads. See PlayerGearFlags.
    if (alink != nullptr) {
        out.gear |= kPlayerGearFxReported;
        if (alink->checkMagicArmorWearAbility() && alink->field_0x2fd7 != 0) {
            out.gear |= kPlayerGearMagicArmorPowered;
        }
        if (alink->checkZoraWearMaskDraw()) {
            out.gear |= kPlayerGearZoraMask;
        }
        if (alink->field_0x2fcb != 0) {
            out.gear |= kPlayerGearShieldBurning;
        }
    }
    // The sender's half of the both-ends check: one capped line per change of bits 3-6. The
    // puppet logs the same changes as it draws them ("Puppet N magic armour BRK", "Zora mask",
    // "shield burn").
    static std::uint8_t sLoggedGearFx = 0;
    static int sGearFxLogCount = 0;
    const std::uint8_t gearFx =
        static_cast<std::uint8_t>(out.gear & (kPlayerGearFxReported | kPlayerGearMagicArmorPowered |
                                                 kPlayerGearZoraMask | kPlayerGearShieldBurning));
    if (gearFx != sLoggedGearFx && sGearFxLogCount < 48) {
        sGearFxLogCount++;
        Log.info(
            "Local gear fx: reported {} | armour powered {} | Zora mask {} | shield burning {} "
            "(timer {})",
            (gearFx & kPlayerGearFxReported) != 0, (gearFx & kPlayerGearMagicArmorPowered) != 0,
            (gearFx & kPlayerGearZoraMask) != 0, (gearFx & kPlayerGearShieldBurning) != 0,
            alink != nullptr ? alink->field_0x2fcb : 0);
    }
    sLoggedGearFx = gearFx;
}

/* --- D3b: the attention (Z-target / lock-on) move set. ---------------------------------------- */

/// The animation that sits in the sender's under slot 1 for each attention kind, in every band.
/// See fill_atn_kind() for why slot 1 is the one to read.
struct AtnSlotAnm {
    daAlink_c::daAlink_ANM anm;
    std::uint8_t kind;
};

const AtnSlotAnm l_atnSlot1Anm[] = {
    {daAlink_c::ANM_ATN_RETURN_FROM_WALK, kPlayerAtnForward},
    {daAlink_c::ANM_ATN_RETURN_FROM_RUN, kPlayerAtnForward},
    {daAlink_c::ANM_ATN_BACK_WALK, kPlayerAtnBack},
    {daAlink_c::ANM_ATN_BACK_RUN, kPlayerAtnBack},
    {daAlink_c::ANM_ATN_LOCK_BACK_WALK, kPlayerAtnBackLock},
    {daAlink_c::ANM_ATN_LOCK_BACK_RUN, kPlayerAtnBackLock},
    {daAlink_c::ANM_ATN_WALK_LEFT, kPlayerAtnLeft},
    {daAlink_c::ANM_ATN_RUN_LEFT, kPlayerAtnLeft},
    {daAlink_c::ANM_ATN_LOCK_WALK_LEFT, kPlayerAtnLeftLock},
    {daAlink_c::ANM_ATN_LOCK_RUN_LEFT, kPlayerAtnLeftLock},
    {daAlink_c::ANM_ATN_WALK_RIGHT, kPlayerAtnRight},
    {daAlink_c::ANM_ATN_RUN_RIGHT, kPlayerAtnRight},
    {daAlink_c::ANM_ATN_LOCK_WALK_RIGHT, kPlayerAtnRightLock},
    {daAlink_c::ANM_ATN_LOCK_RUN_RIGHT, kPlayerAtnRightLock},
    {daAlink_c::ANM_ATN_BACK_WALK_HEAVY, kPlayerAtnBackHeavy},
};

/// What may legitimately sit in slot 0 next to one of the above. Checked first, so that a slot-1
/// index left behind by an earlier blend can't be read as a current one.
const daAlink_c::daAlink_ANM l_atnSlot0Anm[] = {
    daAlink_c::ANM_ATN_WAIT_LEFT,
    daAlink_c::ANM_ATN_WAIT_RIGHT,
    daAlink_c::ANM_WAIT,  // the stance band of the no-target back-off (setBlendAtnBackMoveAnime)
    daAlink_c::ANM_ATN_RETURN_FROM_WALK,
    daAlink_c::ANM_ATN_RETURN_FROM_RUN,
    daAlink_c::ANM_ATN_BACK_WALK,
    daAlink_c::ANM_ATN_BACK_RUN,
    daAlink_c::ANM_ATN_LOCK_BACK_WALK,
    daAlink_c::ANM_ATN_LOCK_BACK_RUN,
    daAlink_c::ANM_ATN_WALK_LEFT,
    daAlink_c::ANM_ATN_LOCK_WALK_LEFT,
    daAlink_c::ANM_ATN_WALK_RIGHT,
    daAlink_c::ANM_ATN_LOCK_WALK_RIGHT,
    daAlink_c::ANM_ATN_RUN_LEFT,
    daAlink_c::ANM_ATN_LOCK_RUN_LEFT,
    daAlink_c::ANM_ATN_RUN_RIGHT,
    daAlink_c::ANM_ATN_LOCK_RUN_RIGHT,
    daAlink_c::ANM_ATN_BACK_WALK_HEAVY,  // bands 2 and 3 of the booted back-off (:8136-8150)
};

const char* atn_kind_name(std::uint8_t kind) {
    switch (kind) {
    case kPlayerAtnNone:
        return "none";
    case kPlayerAtnForward:
        return "forward";
    case kPlayerAtnForwardLock:
        return "forward (lock)";
    case kPlayerAtnBack:
        return "back";
    case kPlayerAtnBackLock:
        return "back (lock)";
    case kPlayerAtnLeft:
        return "left";
    case kPlayerAtnLeftLock:
        return "left (lock)";
    case kPlayerAtnRight:
        return "right";
    case kPlayerAtnRightLock:
        return "right (lock)";
    case kPlayerAtnBackHeavy:
        return "back (iron boots)";
    default:
        return "unnamed";
    }
}

/**
 * Fill the attention byte with the attention move set daAlink_c is actually playing.
 *
 * Same rule as fill_idle_kind(): read the answer off his animation heap, not his inputs.
 * `field_0x2f98` (the direction) and `mTargetedActor` (the lock) are the inputs to the choice, and
 * both are also written by procs that never play this set (jumps, bottles, side rolls). The heap
 * holds the choice itself.
 *
 * ★ The decision comes from slot 1, the B side of the double animation, on purpose. In every band
 * of setBlendAtnMoveAnime (d_a_alink.cpp:7965-8031) and setBlendAtnBackMoveAnime (:8124-8150),
 * slot 1 holds an animation that names the direction and the lock by itself. Slot 0 can't: in the
 * stance band it is ATN_WAIT_RIGHT for forward, for right AND for locked back-off. Slot 1 here is
 * NOT "the outgoing animation". The cross-fade lives in the old-frame morf, and commonSingleAnime
 * resets slot 1 (:7164), so a single animation never matches.
 *
 * Each of these animations' UNDER resources is unique within the set. getMainBckData's shield and
 * fishing-rod tables leave the under half of rows 0x00-0x0D alone (:233-254, :264-291), so the
 * test still holds while he is guarding or fishing.
 *
 * One override comes from slot 0. With no target, setBlendMoveAnime's guard branch (:7685) pairs
 * ATN_WAIT_RIGHT with ATN_WALK_LEFT, a typo in the original kept as it is. It plays at a weight of
 * exactly 0, so slot 0 is all that is drawn. When slot 0 is a side stance that disagrees with slot
 * 1's side, slot 0 wins.
 *
 * Forward is the one kind whose animations don't name the lock. ATN_RETURN_FROM_WALK/RUN play
 * with or without a target, and only the band table differs (:7945-7961). So the lock for forward
 * is read from mTargetedActor, the exact test setBlendAtnMoveAnime makes on that same tick.
 *
 * ATN_BACK_WALK_HEAVY (backing off in iron boots) reports kPlayerAtnBackHeavy. Its stance band
 * pairs it with ATN_WAIT_RIGHT, which is already in the slot-0 list.
 */
void fill_atn_kind(daAlink_c* alink, PlayerState& out) {
    out.atnKind = kPlayerAtnNone;
    if (alink != nullptr && !alink->checkWolf()) {
        bool slot0InSet = false;
        for (const daAlink_c::daAlink_ANM anm : l_atnSlot0Anm) {
            if (alink->checkUnderMove0BckNoArc(anm)) {
                slot0InSet = true;
                break;
            }
        }

        if (slot0InSet) {
            for (const AtnSlotAnm& entry : l_atnSlot1Anm) {
                if (alink->checkUnderMove1BckNoArc(entry.anm)) {
                    out.atnKind = entry.kind;
                    break;
                }
            }
        }

        if (out.atnKind == kPlayerAtnLeft || out.atnKind == kPlayerAtnRight) {
            if (alink->checkUnderMove0BckNoArc(daAlink_c::ANM_ATN_WAIT_RIGHT)) {
                out.atnKind = kPlayerAtnRight;
            } else if (alink->checkUnderMove0BckNoArc(daAlink_c::ANM_ATN_WAIT_LEFT)) {
                out.atnKind = kPlayerAtnLeft;
            }
        }

        if (out.atnKind == kPlayerAtnForward && alink->mTargetedActor != NULL) {
            out.atnKind = kPlayerAtnForwardLock;
        }
    }

    /* Logged on every change, not latched. A strafe changes direction a few times a second at most,
     * so this can't flood, and the both-ends check needs the timestamped sequence of changes. */
    static std::uint8_t s_lastAtnKind = kPlayerAtnNone;
    if (out.atnKind != s_lastAtnKind) {
        Log.debug("Local attention set {} -> {} ({}); rate {:.3f}, target {}, dir field {}",
            static_cast<unsigned>(s_lastAtnKind), static_cast<unsigned>(out.atnKind),
            atn_kind_name(out.atnKind),
            alink != nullptr ? alink->getMoveGroundAngleSpeedRate() : 0.0f,
            alink != nullptr && alink->mTargetedActor != NULL ? "yes" : "no",
            alink != nullptr ? static_cast<unsigned>(alink->field_0x2f98) : 0u);
        s_lastAtnKind = out.atnKind;
    }
}

/**
 * The shield-guard bit: daAlink_c's own checkUpperGuardAnime(), the exact predicate getMainBckData
 * consults before choosing m_mainBckShield (d_a_alink.cpp:6947). Wolf Link never takes that path
 * (his animations come from the wolf tables), so a stale flag on a wolf is not sent.
 *
 * TEST ONLY: DUSK_MP_TEST_FORCE_UPPER_GUARD=1 also sets it whenever the attention set is playing,
 * so the receiver's guard path can be driven end to end without a shielded enemy to lock onto.
 * The sender's own animation does NOT change under the knob, so a sender/receiver BCK comparison
 * disagrees on the stance rows by design while it is set.
 */
void fill_upper_guard(daAlink_c* alink, PlayerState& out) {
    static const bool s_forceGuard = [] {
        const char* value = std::getenv("DUSK_MP_TEST_FORCE_UPPER_GUARD");
        const bool on = value != nullptr && value[0] != '\0' && value[0] != '0';
        if (on) {
            Log.warn("TEST KNOB DUSK_MP_TEST_FORCE_UPPER_GUARD is set: the guard bit is forced "
                     "on whenever the attention set plays");
        }
        return on;
    }();

    if (alink != nullptr && !alink->checkWolf() &&
        (alink->checkUpperGuardAnime() || (s_forceGuard && out.atnKind != kPlayerAtnNone)))
    {
        out.flags |= kPlayerStateUpperGuard;
    }

    static bool s_lastGuard = false;
    const bool guard = out.upper_guard();
    if (guard != s_lastGuard) {
        Log.debug("Local upper guard {} -> {}; atn kind {}, target {}", s_lastGuard ? 1 : 0,
            guard ? 1 : 0, static_cast<unsigned>(out.atnKind),
            alink != nullptr && alink->mTargetedActor != NULL ? "yes" : "no");
        s_lastGuard = guard;
    }
}

/// Running count of joint-aim values the sender had to clamp to the wire range. Reported on every
/// `local` trace row, so a range mistake is a non-zero number rather than a silent wrap.
std::uint32_t s_jointAimClamps = 0;

std::int16_t clamp_joint_aim(int v, const char* name) {
    if (v > kJointAimWireMax || v < -kJointAimWireMax) {
        s_jointAimClamps++;
        /* Logged on the first few only: a clamp is a range assumption being wrong, and the first
         * one names which field and in which state, which is all the diagnosis needs. */
        if (s_jointAimClamps <= 8) {
            Log.warn("Joint aim {} = {} is outside the wire range +-{}; clamped (#{})", name, v,
                kJointAimWireMax, s_jointAimClamps);
        }
        return static_cast<std::int16_t>(v > 0 ? kJointAimWireMax : -kJointAimWireMax);
    }
    return static_cast<std::int16_t>(v);
}

/**
 * Fill the six head-turn / torso-lean angles with the answers daAlink_c::jointControll consumes
 * this tick (d_a_alink.cpp:2192-2212), plus shape_angle.z.
 *
 * Fresh at capture: setNeckAngle runs inside Link's execute after setMatrix, and field_0x30c8 is
 * written from mBodyAngle.y at the end of it (:18563), so these are the values his next calc — the
 * one being drawn — reads.
 *
 * ★ bodyX is jointControll's `sp8`, evaluated HERE with the same predicate (:2199-2204), not
 * mBodyAngle.x. When rein-riding the joint takes the difference with shape_angle.x; the receiver
 * has no idea he is riding, and must not be asked to find out (reuse-the-games-own-values).
 *
 * Joint 16 (field_0x3088/308a) is skipped: every writer of it is a horse, boar, glide or demo
 * state, none of which the puppet can show yet.
 *
 * Wolf: the same six fields carry his jointControll arms (:2150-2191) on the 40-joint skeleton,
 * each the value that arm consumes (puppet-wolf.md §2 table). neckX/neckY are joint 4, as for the
 * human. bodyX is joint 3's X, `field_0x30d6 + field_0x30b2` (the standing slope pitch,
 * wolf.inc:2201, plus the howl demo's lean, :4259), summed as jointControll sums them into an s16.
 * bodyTwist is the turn bend `field_0x2fec` that joints 2 and 26 apply NEGATED (:2179-2187); the
 * sign travels as he stores it and the receiver negates. bodyZ is joint 3's Z, `field_0x30b0`.
 * bankZ is shape_angle.z, as for the human. The tail (37-39) is not sent: the receiver runs the
 * spring itself.
 */
void fill_joint_aim(daAlink_c* alink, PlayerState& out) {
    out.neckX = 0;
    out.neckY = 0;
    out.bodyX = 0;
    out.bodyTwist = 0;
    out.bodyZ = 0;
    out.bankZ = 0;
    if (alink == nullptr) {
        return;
    }
    if (alink->checkWolf()) {
        out.neckX = clamp_joint_aim(alink->field_0x3124.x, "wolf neckX");
        out.neckY = clamp_joint_aim(alink->field_0x3124.y, "wolf neckY");
        out.bodyX = clamp_joint_aim(
            static_cast<s16>(alink->field_0x30d6 + alink->field_0x30b2), "wolf bodyX");
        out.bodyTwist = clamp_joint_aim(alink->field_0x2fec, "wolf bodyTwist");
        out.bodyZ = clamp_joint_aim(alink->field_0x30b0, "wolf bodyZ");
        out.bankZ = clamp_joint_aim(alink->shape_angle.z, "wolf bankZ");
        return;
    }

    s16 bodyX;
    if (alink->checkReinRide() && !alink->checkHorseLieAnime() &&
        alink->mProcID != daAlink_c::PROC_HORSE_RUN && alink->mProcID != daAlink_c::PROC_BOAR_RUN)
    {
        bodyX = (s16)(alink->mBodyAngle.x - alink->shape_angle.x);
    } else {
        bodyX = alink->mBodyAngle.x;
    }

    out.neckX = clamp_joint_aim(alink->field_0x3124.x, "neckX");
    out.neckY = clamp_joint_aim(alink->field_0x3124.y, "neckY");
    out.bodyX = clamp_joint_aim(bodyX, "bodyX");
    out.bodyTwist = clamp_joint_aim(alink->field_0x30c8, "bodyTwist");
    out.bodyZ = clamp_joint_aim(alink->mBodyAngle.z, "bodyZ");
    out.bankZ = clamp_joint_aim(alink->shape_angle.z, "bankZ");

    /* field_0x3124.z is never written anywhere in daAlink_c (only read), so it is not on the wire.
     * If that ever stops being true this line says so instead of the head silently missing a roll.
     */
    static bool s_loggedNeckZ = false;
    if (!s_loggedNeckZ && alink->field_0x3124.z != 0) {
        s_loggedNeckZ = true;
        Log.warn("field_0x3124.z is {} — the neck roll is NOT replicated; see fill_joint_aim()",
            alink->field_0x3124.z);
    }
}

/// s16 binary angle to degrees, for the trace.
float angle_to_degrees(s16 v) {
    return static_cast<float>(v) * (360.0f / 65536.0f);
}

/**
 * The drawn head and torso angles off a body model, relative to a facing. Shared by the local and
 * puppet readers so the two sides of the comparison are the same arithmetic on the same joints.
 */
void measure_joint_aim(J3DModel* bodyModel, s16 facingY, DrawnJointAim& out) {
    if (bodyModel == NULL || bodyModel->getModelData() == NULL ||
        bodyModel->getModelData()->getJointNum() <= 4)
    {
        return;
    }

    cXyz headFwd;
    mDoMtx_multVecSR(bodyModel->getAnmMtx(4), &cXyz::BaseX, &headFwd);
    out.headYawRel = angle_to_degrees((s16)(headFwd.atan2sX_Z() - facingY));
    out.headPitch = angle_to_degrees(headFwd.atan2sY_XZ());

    cXyz torsoSide;
    mDoMtx_multVecSR(bodyModel->getAnmMtx(2), &cXyz::BaseY, &torsoSide);
    out.torsoYawRel = angle_to_degrees((s16)(torsoSide.atan2sX_Z() - facingY));
}

/* ── Action animation override (wire block 2) ───────────────────────────────────────────────
 * .claude/future-plans/puppet-action-states.md §2.1. */

/**
 * The procs whose animation the puppet already REPRODUCES from the rest of the wire: the idles,
 * the gait, the attention set, the moving turn and the skid. Whitelist-the-known,
 * override-the-rest: a proc nobody has thought about gets the sender's real BCK rather than a
 * glide, which is the right direction for an omission to fail in.
 */
bool puppet_reproduces_proc(std::uint16_t proc) {
    switch (proc) {
    case daAlink_c::PROC_SERVICE_WAIT:
    case daAlink_c::PROC_TIRED_WAIT:
    case daAlink_c::PROC_WAIT:
    case daAlink_c::PROC_MOVE:
    case daAlink_c::PROC_ATN_MOVE:
    case daAlink_c::PROC_ATN_ACTOR_WAIT:
    case daAlink_c::PROC_ATN_ACTOR_MOVE:
    /* NOT PROC_WAIT_TURN: procWaitTurnInit plays BCK STEPL (ANM_STEP_TURN, d_a_alink.cpp:15775)
     * as a single anime when he turns >~169 deg from standing, and the puppet has no STEPL path,
     * so it showed WAITS through the step-turn. Overriding carries STEPL, and the override exit
     * restarts the puppet's run at phase 0 like the sender's. */
    case daAlink_c::PROC_MOVE_TURN:
    case daAlink_c::PROC_SLIP:
        return true;
    default:
        return false;
    }
}

/**
 * The hand-model material whose shape the local player is SHOWING, or kPlayerActBodyHand for the
 * body's own hand. The output of daAlink_c::setDrawHand (d_a_alink.cpp:18944-19045), not its
 * input: mLeftHandIndex/mRightHandIndex are rewritten by that chain (the sword grip, the shield
 * grip, 0x64 -> 2...), and comparing inputs is how the grip bug was once misdiagnosed (memory:
 * measure the output, not the input). Same resolution d_a_remote_player.cpp's grip log uses.
 */
std::uint8_t shown_hand_index(J3DModel* handModel, J3DShape* shown) {
    if (handModel == nullptr || shown == nullptr) {
        return kPlayerActBodyHand;
    }
    J3DModelData* handData = handModel->getModelData();
    if (handData == nullptr) {
        return kPlayerActBodyHand;
    }
    for (u16 i = 0; i < handData->getMaterialNum(); i++) {
        if (handData->getMaterialNodePointer(i)->getShape() == shown) {
            return static_cast<std::uint8_t>(i);
        }
    }
    // field_0x06d8/06dc — the body model's plain hands, his own answer for 0xFE.
    return kPlayerActBodyHand;
}

void clear_action_anim(PlayerState& out) {
    out.actUnder = kPlayerActNone;
    out.actUpper = kPlayerActNone;
    out.actFrame = 0;
    out.actUpperFrame = 0;
    out.actRate = 0;
    out.actMorf = 0;
    out.actHandL = kPlayerActBodyHand;
    out.actHandR = kPlayerActBodyHand;
    out.actRootX = 0;
    out.actRootY = 0;
    out.actRootZ = 0;
    // block (4): action slot 1 / swim — only ever sent alongside block 2.
    out.act1Under = kPlayerActNone;
    out.act1Upper = kPlayerActNone;
    out.act1Phase = 0;
    out.act1Ratio = 0;
    out.actRootRotX = 0;
    out.actRootRotZ = 0;
    out.actYawOff = 0;
    out.actPitch = 0;
}

/**
 * Fill block (4): the second animation of a double anime, the joint-0 rotation jointControll adds
 * and setMatrix's yaw offset. Only called while block 2 is being sent for an ALANM slot 0.
 *
 * Slot 1 is read off the pack AND the heap, both of which commonSingleAnime clears (:7163-7169),
 * so a single anime reads as "no slot 1" even if the heap index lingered. A demo-archive slot 1 is
 * dropped for the same reason block 2 drops a demo slot 0.
 */
void fill_action_slot1(daAlink_c* alink, PlayerState& out) {
    const daPy_anmHeap_c& under1 = alink->mUnderAnmHeap[1];
    const daPy_anmHeap_c& upper1 = alink->mUpperAnmHeap[1];
    if (alink->mNowAnmPackUnder[1].getAnmTransform() != nullptr && !under1.checkNoSetIdx() &&
        under1.checkNoSetArcNo() && (upper1.checkNoSetIdx() || upper1.checkNoSetArcNo()))
    {
        out.act1Under = under1.getIdx();
        out.act1Upper = upper1.getIdx();  // 0xFFFF: follows under1, as for slot 0
        const f32 end1 = alink->mUnderFrameCtrl[1].getEnd();
        out.act1Phase =
            act_encode_unit(end1 > 0.0f ? alink->mUnderFrameCtrl[1].getFrame() / end1 : 0.0f);
        out.act1Ratio = act_encode_unit(alink->mNowAnmPackUnder[1].getRatio());
    }

    // jointControll's joint-0 arm, `sp18.set(field_0x3080, 0, field_0x3082)` (d_a_alink.cpp:2231).
    // The values his calc this tick consumed: nothing writes them between his calc and here.
    out.actRootRotX = alink->field_0x3080;
    out.actRootRotZ = alink->field_0x3082;
    // setMatrix's yaw offset (:5776). Clamped and counted like the joint aim it is quantised with.
    out.actYawOff = clamp_joint_aim(alink->field_0x308c, "yawOff");
    // setMatrix's whole-model pitch (:5776). Full circle on the wire, so no clamp.
    out.actPitch = alink->shape_angle.x;
}

/**
 * Fill block (2): what Link is DRAWING when he is in a proc the puppet does not reconstruct.
 *
 * ★ capture_local_player() runs twice per tick (once to send, once for --mp-trace), so every piece
 * of state here must be idempotent within a tick. The morf latch is keyed on the idx CHANGING,
 * which the second call does not see, so it hands back the value the first call latched.
 */
void fill_action_anim(daAlink_c* alink, PlayerState& out) {
    clear_action_anim(out);

    static std::uint16_t s_latchedUnder = kPlayerActNone;
    static std::uint8_t s_latchedMorf = 0;
    static std::uint16_t s_loggedDemoProc = 0xFFFF;
    static std::uint16_t s_demoLogCount = 0;
    static std::uint16_t s_changeLogCount = 0;
    static std::uint16_t s_lastSentUnder = kPlayerActNone;
    static std::uint16_t s_latchedUnder1 = kPlayerActNone;

    /* ★ A wolf sender keeps the override ON in every proc, the gait and the idles included
     * (puppet-wolf.md §2): the puppet has no wolf locomotion of its own, and rebuilding
     * setBlendWolfMoveAnime's bands on the receiver is the re-derivation the rules forbid. Every
     * wolf row is a dRes_ID_ALANM_BCK_WL_* in the same archive, so actUnder needs no new id space.
     */
    const bool wolf = alink != nullptr && alink->checkWolf();
    if (alink == nullptr || (!wolf && puppet_reproduces_proc(alink->mProcID))) {
        s_latchedUnder = kPlayerActNone;
        s_latchedUnder1 = kPlayerActNone;
    } else {
        const daPy_anmHeap_c& under = alink->mUnderAnmHeap[0];
        const daPy_anmHeap_c& upper = alink->mUpperAnmHeap[0];

        if (!under.checkNoSetArcNo() || (!upper.checkNoSetIdx() && !upper.checkNoSetArcNo())) {
            /* A demo-archive BCK (loadDataDemoRID, d_a_player.cpp:356) — doors, sumo, boss grabs.
             * The puppet has none of those archives, so "no override" (the glide) is sent rather
             * than an id it would resolve against the wrong archive. Logged once per proc so the
             * gap is visible instead of being mistaken for "works". */
            if (alink->mProcID != s_loggedDemoProc && s_demoLogCount < 32) {
                s_loggedDemoProc = alink->mProcID;
                s_demoLogCount++;
                Log.debug(
                    "Local action override skipped: proc 0x{:X} plays bck {} from demo arc {} "
                    "(upper {} arc {}); the puppet glides for this one",
                    alink->mProcID, under.getIdx(), under.getArcNo(), upper.getIdx(),
                    upper.getArcNo());
            }
            s_latchedUnder = kPlayerActNone;
            s_latchedUnder1 = kPlayerActNone;
        } else if (!under.checkNoSetIdx()) {
            out.actUnder = under.getIdx();
            out.actUpper = upper.getIdx();  // 0xFFFF already means "follows under"

            out.actFrame =
                act_encode_u16(alink->mUnderFrameCtrl[0].getFrame(), kPlayerActFrameScale);
            if (out.actUpper != kPlayerActNone) {
                out.actUpperFrame =
                    act_encode_u16(alink->mUpperFrameCtrl[0].getFrame(), kPlayerActFrameScale);
            }
            out.actRate = act_encode_s8(alink->mUnderFrameCtrl[0].getRate(), kPlayerActRateScale);

            /* The morf Link started, latched on the tick the idx changed. setSingleAnime calls
             * initOldFrameMorf(m) (d_a_alink.cpp:7239), which decrements once (counter m-1), and
             * the body's calc later in the same execute decrements again (m_Do_ext.cpp:1233), so a
             * capture after the actor pass reads m-2. The receiver's setAnm + calc repeat exactly
             * that sequence, which is why m — not the counter — is what travels. REASONED, not
             * measured: a proc that sets its animation after modelCalc would read m-1 and send
             * one frame long.
             *
             * Slot 1 is filled FIRST so a wolf can key the latch on it too: setDoubleAnimeWolf
             * re-morfs when EITHER slot loads a new BCK (wolf.inc:1271-1294, var_r30 |= 1 on the
             * slot-1 load), so a gait band change that only swaps slot 1 (WALK_A -> WALK_B over
             * an unchanged slot 0) starts a morf the slot-0 key would never see. The human key is
             * left as it was: his gait never reaches this block (puppet_reproduces_proc). */
            fill_action_slot1(alink, out);
            const std::uint16_t key1 = wolf ? out.act1Under : kPlayerActNone;
            if (out.actUnder != s_latchedUnder || key1 != s_latchedUnder1) {
                s_latchedUnder = out.actUnder;
                s_latchedUnder1 = key1;
                const f32 counter = alink->field_0x2060 != nullptr ?
                                        alink->field_0x2060->getOldFrameMorfCounter() :
                                        0.0f;
                const f32 morf = counter > 0.0f ? counter + 2.0f : 0.0f;
                s_latchedMorf = static_cast<std::uint8_t>(morf > 255.0f ? 255.0f : morf + 0.5f);
            }
            out.actMorf = s_latchedMorf;

            /* Joint 0's FINAL translation, after jointControll has applied whatever root clear
             * this proc asked for (d_a_alink.cpp:2213-2235, including the 0x60 case). Read off the
             * matrix his calc produced — this runs after the actor pass, and his modelCalc is
             * inside execute. Reading it, not writing it: writes after calc are invisible
             * (memory).
             *
             * ★ Taken back to model space through the base the RECEIVER builds from the wire —
             * the sent position, then ZXYrot(actPitch, angleY + actYawOff, bankZ), each as
             * quantised as the puppet will see it — and NOT through his mInvMtx. setMatrix's base
             * also carries Y offsets that are not on the wire (the swim bob field_0x2b98,
             * field_0x2b9c, the sink field_0x2ba0, :5774); measured this way they land in the root
             * translation, and the puppet's joint 0 ends up where his is in WORLD space. The
             * rotations match his base to the wire's quantisation. Block (4) is filled above, so
             * its values are already in `out`. */
            if (alink->mpLinkModel != nullptr) {
                MtxP root = alink->mpLinkModel->getAnmMtx(0);
                const cXyz world(root[0][3], root[1][3], root[2][3]);
                const s16 wirePitch = act_decode_turn(act_encode_turn(out.actPitch));
                const s16 wireYaw = static_cast<s16>(
                    out.angleY + joint_aim_from_wire(joint_aim_to_wire(out.actYawOff)));
                const s16 wireBank = joint_aim_from_wire(joint_aim_to_wire(out.bankZ));
                mDoMtx_stack_c::transS(out.posX, out.posY, out.posZ);
                mDoMtx_stack_c::ZXYrotM(wirePitch, wireYaw, wireBank);
                Mtx wireInv;
                mDoMtx_inverse(mDoMtx_stack_c::get(), wireInv);
                cXyz local;
                mDoMtx_multVec(wireInv, &world, &local);
                out.actRootX = act_encode_s16(local.x, kPlayerActRootScale);
                out.actRootY = act_encode_s16(local.y, kPlayerActRootScale);
                out.actRootZ = act_encode_s16(local.z, kPlayerActRootScale);
            }
        } else {
            s_latchedUnder = kPlayerActNone;
            s_latchedUnder1 = kPlayerActNone;
        }
    }

    /* The hands he is SHOWING, on every tick and in every proc (protocol 17), not only during an
     * override. setDrawHand's chain (d_a_alink.cpp:18944-19016) resolves the held-item overrides
     * field_0x2f94-0x2f97, the bow's left hand, the canoe paddle, the cut-type right hand and the
     * shield pass flag. Sending the result means the puppet never has to rebuild that chain from
     * the held item, and it cannot miss a branch.
     *
     * setDrawHand runs in his draw, so this is the pair from the previous frame. The puppet is one
     * frame late on a hand change, and that is the whole cost.
     *
     * While the status window is open, setDrawHand takes the pause-menu branch (:18950-18961): 0 if
     * he owns a sword, 6 if he owns a shield. That describes the collection-screen model, not his
     * hands in the world, so "unknown" is sent and the puppet uses its own derivation. The
     * held-item block makes the same test (fill_held_item). A wolf has no hands model, so he keeps
     * clear_action_anim's kPlayerActBodyHand. */
    if (alink != nullptr && !wolf) {
        if (alink->checkNoResetFlg2(daPy_py_c::FLG2_STATUS_WINDOW_DRAW)) {
            out.actHandL = kPlayerHandUnknown;
            out.actHandR = kPlayerHandUnknown;
        } else {
            out.actHandL = shown_hand_index(alink->mpLinkHandModel, alink->field_0x06d0);
            out.actHandR = shown_hand_index(alink->mpLinkHandModel, alink->field_0x06d4);
        }
    }

    /* On every change of what is sent, capped: combat changes BCK several times a second, and the
     * trace carries the full series anyway. This is for a human reading the log. */
    if (out.actUnder != s_lastSentUnder) {
        if (s_changeLogCount < 200) {
            s_changeLogCount++;
            Log.debug("Local action override {} -> {} (upper {}), proc 0x{:X}, frame {:.1f}, rate "
                      "{:.2f}, morf {}, hands {}/{}",
                s_lastSentUnder, out.actUnder, out.actUpper,
                alink != nullptr ? static_cast<unsigned>(alink->mProcID) : 0xFFFFu, out.act_frame(),
                out.act_rate(), static_cast<unsigned>(out.actMorf),
                static_cast<unsigned>(out.actHandL), static_cast<unsigned>(out.actHandR));
        }
        s_lastSentUnder = out.actUnder;
    }
}

/**
 * Where Link is in his blended stride, 0..1, or -1 when he is not playing a blended pair.
 *
 * His own phase, read where he reads it: commonDoubleAnime takes `mUnderFrameCtrl[0].getFrame() /
 * getEnd()` as the phase to carry, and only while field_0x2f8c says the last thing set was a
 * blended pair (d_a_alink.cpp:7029-7033). Every slot of the pair is wound to that same phase and
 * stepped at a rate scaled to keep it there, so slot 0 speaks for the whole stride. That includes
 * the run band, where slot 1 carries all the weight.
 *
 * Read after his execute, so it is the frame his model is drawn with this tick: allAnimePlay has
 * already stepped the controller and pushed the frame into the animation.
 */
float local_loc_phase(daAlink_c* alink) {
    if (alink == nullptr || alink->checkWolf() || alink->field_0x2f8c == 0) {
        return -1.0f;
    }
    const daPy_frameCtrl_c& ctrl = alink->mUnderFrameCtrl[0];
    if (ctrl.getEnd() <= 0) {
        return -1.0f;
    }
    float phase = ctrl.getFrame() / static_cast<float>(ctrl.getEnd());
    phase -= std::floor(phase);
    return phase >= 1.0f ? 0.0f : phase;
}

/// Fill v15 block (1). Runs after fill_action_anim: the phase is sent only while the override is
/// off. See PlayerState::locPhase for why 0 is the "nothing" value.
void fill_loc_phase(daAlink_c* alink, PlayerState& out) {
    out.locPhase = 0;
    if (out.act_active()) {
        return;
    }
    const float phase = local_loc_phase(alink);
    if (phase >= 0.0f) {
        // Rounded, and 256 (a phase of 0.998+) wraps to 0, which is the same point in the cycle.
        out.locPhase = static_cast<std::uint8_t>(static_cast<int>(phase * 256.0f + 0.5f) & 0xFF);
    }
}

// v19 block B: iron ball
/**
 * Fill the iron-ball block: where his ball is, read off the members setIronBallPos has just written
 * this tick (d_a_alink_ironball.inc:465-810), never re-simulated. Gated on heldItem, i.e. on the
 * very checkItemDraw() that decides whether his ball and chain are drawn at all.
 *
 *   - the position is mIronBallChainPos[0], the ball's chain end and its model's origin
 *     (setBaseTRMtx at :768-770), relative to the position this same sample carries;
 *   - the rotation is mIronBallChainAngle[0], the ZXY rotation that matrix is built from;
 *   - the link count is mItemMode, and the state mItemVar0.field_0x3018, whose values
 *     setIronBallPos and setIronBallChainPos branch on.
 *
 * The chain links themselves are not sent: the receiver runs setIronBallChainPos's own relaxation
 * between its hand and this ball, as he does between his. In state 0 (carried) the receiver also
 * places the ball off its own left item joint, as he does, and the position is only a fallback.
 *
 * Idempotent within a tick (capture runs twice per tick, see fill_action_anim): the statics only
 * gate logging.
 */
void fill_iron_ball(daAlink_c* alink, PlayerState& out) {
    out.ironBallX = 0;
    out.ironBallY = 0;
    out.ironBallZ = 0;
    out.ironBallAngX = 0;
    out.ironBallAngY = 0;
    out.ironBallLinks = 0;
    out.ironBallState = 0;

    static std::uint8_t s_lastState = 0xFF;
    static std::uint16_t s_stateLogCount = 0;
    static bool s_loggedClamp = false;

    if (alink == nullptr || out.heldItem != kPlayerHeldIronBall ||
        alink->mIronBallChainPos == nullptr || alink->mIronBallChainAngle == nullptr)
    {
        s_lastState = 0xFF;
        return;
    }

    const cXyz& ball = alink->mIronBallChainPos[0];
    const float relX = ball.x - out.posX;
    const float relY = ball.y - out.posY;
    const float relZ = ball.z - out.posZ;
    out.ironBallX = act_encode_s16(relX, kPlayerIronBallPosScale);
    out.ironBallY = act_encode_s16(relY, kPlayerIronBallPosScale);
    out.ironBallZ = act_encode_s16(relZ, kPlayerIronBallPosScale);
    const float reach = 32767.0f / kPlayerIronBallPosScale;
    if (!s_loggedClamp &&
        (std::fabs(relX) > reach || std::fabs(relY) > reach || std::fabs(relZ) > reach))
    {
        s_loggedClamp = true;
        Log.warn("Local iron ball is ({:.0f}, {:.0f}, {:.0f}) from Link, past the wire's +-{:.0f}; "
                 "sent clamped (logged once)",
            relX, relY, relZ, reach);
    }
    out.ironBallAngX = alink->mIronBallChainAngle[0].x;
    out.ironBallAngY = alink->mIronBallChainAngle[0].y;

    const s16 links = alink->mItemMode;
    out.ironBallLinks = static_cast<std::uint8_t>(links < 0 ? 0 : (links > 255 ? 255 : links));
    out.ironBallState =
        static_cast<std::uint8_t>(alink->mItemVar0.field_0x3018 & kPlayerIronBallStateMask);
    if (alink->mProcID == daAlink_c::PROC_IRON_BALL_SUBJECT ||
        alink->mProcID == daAlink_c::PROC_IRON_BALL_MOVE)
    {
        out.ironBallState |= kPlayerIronBallJointRot;
    }

    if (out.ironBallState != s_lastState) {
        if (s_stateLogCount < 100) {
            s_stateLogCount++;
            Log.debug("Local iron ball state 0x{:02x} -> 0x{:02x}: {} links, ball ({:.1f}, {:.1f}, "
                      "{:.1f}) from Link, rot ({}, {}), proc 0x{:X}",
                static_cast<unsigned>(s_lastState), static_cast<unsigned>(out.ironBallState),
                static_cast<unsigned>(out.ironBallLinks), relX, relY, relZ,
                static_cast<int>(out.ironBallAngX), static_cast<int>(out.ironBallAngY),
                static_cast<unsigned>(alink->mProcID));
        }
        s_lastState = out.ironBallState;
    }
}

// v20 clawshot block
/**
 * Fill the v20 clawshot block: where his two clawshot sets' tips are and what they show, read
 * after setHookshotPos placed them (setItemMatrix, d_a_alink.cpp:6067-6068, inside his execute).
 *
 * Read, not re-derived (memory: measure the output, not the input):
 *   - "out" is his draw's own chain test, `root.abs(top) > 1` per set (d_a_alink.cpp:19793-19796),
 *     on the very anchors hsChainShape_c::draw reads (getHsChainTopPos etc.,
 *     d_a_alink.h:3828-3830);
 *   - an out tip's rotation is read back off the tip MODEL's base matrix, whichever of his three
 *     placements (fired, hanging, flying back; d_a_alink_hook.inc:1195-1229) produced it;
 *   - slot 1's tip-open frame is mHookTipBck's own frame, which setHookshotPos entered last
 *     (:1240); slot 0's is field_0x33e0, which it entered just before (:1208).
 *
 * Gated on the held block's kind, filled first: a clawshot his draw is not drawing is not sent.
 * Idempotent within a tick (capture runs twice per tick): the statics only gate logging.
 */
void fill_clawshot(daAlink_c* alink, PlayerState& out) {
    out.clawFlags = 0;
    out.clawTipFrame[0] = 0;
    out.clawTipFrame[1] = 0;
    std::fill(std::begin(out.clawTipPos), std::end(out.clawTipPos), std::int16_t{0});
    std::fill(std::begin(out.clawTipAng), std::end(out.clawTipAng), std::int16_t{0});

    static std::uint8_t s_lastFlags = 0xFF;
    static std::uint16_t s_logCount = 0;
    static bool s_loggedClamp = false;

    if (alink == nullptr || !out.clawshot_out() || alink->mpHookTipModel == nullptr ||
        alink->field_0x0714 == nullptr || alink->mHookTipBck.getBckAnm() == nullptr)
    {
        s_lastFlags = 0xFF;
        return;
    }

    const cXyz* const root[2] = {&alink->mHeldItemRootPos, &alink->field_0x3810};
    const cXyz* const top[2] = {&alink->mHookshotTopPos, &alink->mIronBallBgChkPos};
    J3DModel* const tip[2] = {alink->mpHookTipModel, alink->field_0x0714};
    const float reach = 32767.0f / kPlayerClawPosScale;
    float rel[6] = {};

    std::uint8_t flags = 0;
    for (int s = 0; s < 2; s++) {
        if (!(root[s]->abs(*top[s]) > 1.0f)) {
            continue;
        }
        flags |= static_cast<std::uint8_t>(kPlayerClawTipOut0 << s);
        rel[s * 3 + 0] = top[s]->x - out.posX;
        rel[s * 3 + 1] = top[s]->y - out.posY;
        rel[s * 3 + 2] = top[s]->z - out.posZ;
        for (int i = s * 3; i < s * 3 + 3; i++) {
            out.clawTipPos[i] = act_encode_s16(rel[i], kPlayerClawPosScale);
            if (!s_loggedClamp && std::fabs(rel[i]) > reach) {
                s_loggedClamp = true;
                Log.warn("Local clawshot tip {} is ({:.0f}, {:.0f}, {:.0f}) from Link, past the "
                         "wire's +-{:.0f}; sent clamped (logged once)",
                    s, rel[s * 3 + 0], rel[s * 3 + 1], rel[s * 3 + 2], reach);
            }
        }
        csXyz rot;
        mDoMtx_MtxToRot(tip[s]->getBaseTRMtx(), &rot);
        out.clawTipAng[s * 2 + 0] = rot.x;
        out.clawTipAng[s * 2 + 1] = rot.y;
    }
    if (alink->checkHookshotWait()) {
        flags |= kPlayerClawWait;
    }
    if (dComIfGp_checkPlayerStatus1(0, 0x10000)) {
        flags |= kPlayerClawRoofHang;
    }
    const s16 stop = alink->field_0x3026;
    flags |= static_cast<std::uint8_t>(
        ((stop < 0 ? 0 : (stop > 15 ? 15 : stop)) << kPlayerClawStopShift) & kPlayerClawStopMask);
    out.clawFlags = flags;

    const f32 frames[2] = {alink->field_0x33e0, alink->mHookTipBck.getBckAnm()->getFrame()};
    for (int s = 0; s < 2; s++) {
        out.clawTipFrame[s] = static_cast<std::uint8_t>(
            frames[s] <= 0.0f ? 0.0f : (frames[s] >= 255.0f ? 255.0f : frames[s] + 0.5f));
    }

    // Logged on a change of which tips are out, wait or hang - not of the stop time, which counts.
    const std::uint8_t logKey = static_cast<std::uint8_t>(flags & ~kPlayerClawStopMask);
    if (logKey != s_lastFlags) {
        if (s_logCount < 100) {
            s_logCount++;
            Log.debug("Local clawshot 0x{:02x} -> 0x{:02x}: kind {}, mode {}, right {}, tip0 "
                      "({:.1f}, {:.1f}, {:.1f}) rot ({}, {}) frame {}, tip1 ({:.1f}, {:.1f}, "
                      "{:.1f}) frame {}, stop {}",
                static_cast<unsigned>(s_lastFlags), static_cast<unsigned>(logKey),
                static_cast<unsigned>(out.heldItem), static_cast<int>(alink->mItemMode),
                static_cast<int>(alink->field_0x3020), rel[0], rel[1], rel[2],
                static_cast<int>(out.clawTipAng[0]), static_cast<int>(out.clawTipAng[1]),
                static_cast<unsigned>(out.clawTipFrame[0]), rel[3], rel[4], rel[5],
                static_cast<unsigned>(out.clawTipFrame[1]), static_cast<int>(stop));
        }
        s_lastFlags = logKey;
    }
}

/// The v20 clawshot block, decoded from the wire's fixed point here so the actor never sees the
/// wire layout. A tip on its gun decodes to a zero offset, which the puppet never reads.
void apply_clawshot(daRemotePlayer_c* puppet, const PlayerState& state) {
    cXyz rel[2];
    for (int s = 0; s < 2; s++) {
        rel[s].set(state.claw_tip_pos(s * 3 + 0), state.claw_tip_pos(s * 3 + 1),
            state.claw_tip_pos(s * 3 + 2));
    }
    puppet->setNetworkClawshot(state.clawFlags, state.clawTipFrame, rel, state.clawTipAng);
}

// block (3): held item
/**
 * Fill block (3): the item model Link is DRAWING in his hands, and the UPPER_2 overlay posing his
 * arms around it.
 *
 * Every field is read AFTER his own logic decided it, never re-derived (memory: measure the output,
 * not the input):
 *   - the kind is mHeldItemModel non-NULL, setItemMatrix's own status-window gate
 *     (d_a_alink.cpp:6026), and checkItemDraw() (:19086-19096) — the exact predicate his draw uses
 *     (:19771) — then mEquipItem only to NAME which model it is;
 *   - the boomerang is daBoomerang_c's own "in hand" state, fopAcM_GetParam == 0 (procWait,
 *     d_a_boomerang.cpp:932-935), off the actor he keeps in mItemAcKeep (setItemActor, :14216);
 *   - the bottle and rod material frames are read off HIS anm objects (field_0x0724 / 072c), which
 *     is what his model is showing;
 *   - the overlay is mUpperAnmHeap[UPPER_2] / mUpperFrameCtrl[2] / mNowAnmPackUpper[2].
 *
 * Idempotent within a tick (capture runs twice per tick, see fill_action_anim): the only state is
 * the morf latch, keyed on the overlay idx CHANGING.
 */
void fill_held_item(daAlink_c* alink, PlayerState& out) {
    out.heldItem = kPlayerHeldNone;
    out.heldFlags = 0;
    out.heldMat = 0;
    out.upper2Idx = kPlayerActNone;
    out.upper2Frame = 0;
    out.upper2Ratio = 0;
    out.upper2Morf = 0;
    out.heldBck = 0;
    out.heldBckFrame = 0;

    static std::uint16_t s_latchedUpper2 = kPlayerActNone;
    static std::uint8_t s_latchedUpper2Morf = 0;
    static std::uint8_t s_lastSentKind = kPlayerHeldNone;
    static std::uint16_t s_kindLogCount = 0;
    static std::uint16_t s_lastSentUpper2 = kPlayerActNone;
    static std::uint16_t s_upper2LogCount = 0;
    static bool s_loggedDemoUpper2 = false;

    if (alink == nullptr) {
        s_latchedUpper2 = kPlayerActNone;
        return;
    }

    /* A wolf sends the UPPER_2 overlay only (puppet-wolf.md §2): WL_DASHWIND, the WL_PICKUP carry.
     * His held models (0x109 Midna's lock-on dome, 0x10A the poe soul) are deferred, and the arrow,
     * boomerang and lantern are human-only, so every held-item test below is gated off for him and
     * the kind stays none. */
    const bool wolf = alink->checkWolf();
    const u16 item = alink->mEquipItem;
    J3DModel* held = alink->mHeldItemModel;

    if (!wolf && held != nullptr && !alink->checkNoResetFlg2(daPy_py_c::FLG2_STATUS_WINDOW_DRAW) &&
        alink->checkItemDraw())
    {
        if (daPy_py_c::checkBottleItem(item)) {
            out.heldItem = kPlayerHeldBottle;
            if (alink->checkOilBottleItemNotGet(item)) {
                out.heldFlags |= kPlayerHeldBottlePour;
            } else if (alink->mItemMode != 0) {
                // bottleModelCallBack only runs when the callback is installed, i.e. not pouring
                // (setBottleModel, d_a_alink_bottle.inc:91-98).
                out.heldFlags |= kPlayerHeldBottleCork;
            }
            std::uint8_t mat = 0;
            if (alink->field_0x0724 != nullptr) {
                const f32 brk = alink->field_0x0724->getFrame();
                const int frame = brk > 0.0f ? static_cast<int>(brk + 0.5f) : 0;
                mat |= static_cast<std::uint8_t>(
                    (frame > 15 ? 15 : frame) & kPlayerHeldMatBottleBrkMask);
            }
            if (alink->field_0x072c != nullptr) {
                const f32 btp = alink->field_0x072c->getFrame();
                const int frame = btp > 0.0f ? static_cast<int>(btp + 0.5f) : 0;
                mat |= static_cast<std::uint8_t>(
                    ((frame > 3 ? 3 : frame) << kPlayerHeldMatBottleBtpShift) &
                    kPlayerHeldMatBottleBtpMask);
            }
            J3DModelData* data = held->getModelData();
            if (data != nullptr && data->getMaterialNum() > 0 &&
                data->getMaterialNodePointer(0)->getShape()->checkFlag(J3DShpFlag_Visible))
            {
                // J3DShpFlag_Visible SET means hidden (J3DShape.h:173-174).
                mat |= kPlayerHeldMatBottleMat0Hidden;
            }
            out.heldMat = mat;

            // --- bottle contents (agent H) ---
            /* heldBck for a bottle: the BTK slot ENTERED on his bottle's materials, read off the
             * material animators themselves (not inferred from the proc), with that animation
             * object's own frame; which set setBottleModel loaded into slots 1/2, by its own test
             * (d_a_alink_bottle.inc:56); and the contents model he draws in it, mpHookTipModel
             * (setItemMatrix poses it, d_a_alink.cpp:6053-6057; drawn under the checkItemDraw()
             * above, :19775-19777), named by the item setBottleModel built it for (:66-82). */
            {
                J3DAnmTextureSRTKey* const slots[3] = {
                    alink->field_0x0718, alink->field_0x071c, alink->field_0x0720};
                J3DAnmTextureSRTKey* btk = nullptr;
                const std::uint8_t btkId = daRemotePlayer_c::bottleBtkEntered(held, slots, &btk);
                std::uint8_t contents = kPlayerBottleContentsNone;
                if (alink->mpHookTipModel != nullptr) {
                    if (item == dItemNo_FAIRY_e) {
                        contents = kPlayerBottleContentsFairy;
                    } else if (item == dItemNo_WORM_e) {
                        contents = kPlayerBottleContentsWorm;
                    } else if (item == dItemNo_BEE_CHILD_e) {
                        contents = kPlayerBottleContentsBee;
                    }
                }
                const bool drinkSet =
                    daPy_py_c::checkDrinkBottleItem(item) || alink->checkOilBottleItemNotGet(item);
                out.heldBck = static_cast<std::uint8_t>(
                    btkId | (contents << kPlayerHeldBckBottleContentsShift) |
                    (drinkSet ? kPlayerHeldBckBottleDrinkSet : 0));
                if (btk != nullptr && btk->getFrameMax() > 0) {
                    const f32 f = btk->getFrame() / btk->getFrameMax();
                    out.heldBckFrame = static_cast<std::uint8_t>(
                        f <= 0.0f ? 0.0f : (f >= 1.0f ? 255.0f : f * 255.0f + 0.5f));
                }

                static std::uint8_t s_lastBottleBck = 0xFF;
                static std::uint16_t s_bottleLogCount = 0;
                const std::uint8_t bottleKey =
                    static_cast<std::uint8_t>(out.heldBck & ~kPlayerHeldBckBombArrow);
                if (bottleKey != s_lastBottleBck && s_bottleLogCount < 64) {
                    s_bottleLogCount++;
                    Log.info("Local bottle BTK slot {} ({} set), bottle contents {} (mEquipItem "
                             "0x{:X}, tip model {}, frame {}/255)",
                        static_cast<unsigned>(btkId), drinkSet ? "drink" : "open",
                        static_cast<unsigned>(contents), static_cast<unsigned>(item),
                        alink->mpHookTipModel != nullptr ? "set" : "none",
                        static_cast<unsigned>(out.heldBckFrame));
                }
                s_lastBottleBck = bottleKey;
            }
        } else if (daPy_py_c::checkBowItem(item) || item == dItemNo_PACHINKO_e) {
            out.heldItem = item == dItemNo_PACHINKO_e ? kPlayerHeldSlingshot : kPlayerHeldBow;
            if (alink->checkBowGrabLeftHand()) {
                out.heldFlags |= kPlayerHeldBowLeftHand;
            }
            /* v19 block A: the model's own BCK. mItemBck holds what changeItemBck() last loaded
             * into mAnmHeap9 (d_a_alink.cpp:14386-14397); its frame is the one setItemMatrix
             * ENTERED this tick, after his clamp below frameMax (:6083-6089) — read off the
             * animation object, not re-derived from field_0x33dc's many writers. */
            J3DAnmTransform* itemBck = alink->mItemBck.getBckAnm();
            if (itemBck != nullptr && !alink->mAnmHeap9.checkNoSetIdx() &&
                alink->mAnmHeap9.checkNoSetArcNo())
            {
                const std::uint8_t id =
                    daRemotePlayer_heldItemBckToWire(alink->mAnmHeap9.getIdx(), out.heldItem);
                const f32 frameMax = itemBck->getFrameMax();
                if (id != kPlayerHeldBckNone && frameMax > 0.0f) {
                    const f32 f = itemBck->getFrame() / frameMax;
                    out.heldBck = id;
                    out.heldBckFrame = static_cast<std::uint8_t>(
                        f <= 0.0f ? 0.0f : (f >= 1.0f ? 255.0f : f * 255.0f + 0.5f));
                }
            }
        } else if (daPy_py_c::checkHookshotItem(item)) {
            if (item == dItemNo_W_HOOKSHOT_e) {
                out.heldItem = kPlayerHeldDoubleClawshot;
                // The draw's own test for the second pair (d_a_alink.cpp:19779-19780).
                if (!alink->checkPlayerGuardAndAttack() &&
                    !alink->checkNoResetFlg0(daPy_py_c::FLG0_UNK_2))
                {
                    out.heldFlags |= kPlayerHeldClawBoth;
                }
            } else {
                out.heldItem = kPlayerHeldClawshot;
            }
            /* For the single clawshot too since the v20 clawshot block: a hang flips field_0x3020
             * and changeHookshotDrawModel then draws his second pair
             * (d_a_alink_hook.inc:346-356). */
            if (alink->field_0x3020 != 0) {
                out.heldFlags |= kPlayerHeldClawRight;
            }
            /* The gun's own BCK, HS_SPIN: setHookshotModel's mAnmHeap9 load into mItemBck
             * (d_a_alink_hook.inc:428-433), entered on mHeldItemModel with field_0x33dc by
             * setItemMatrix (d_a_alink.cpp:6083-6089) - read off the animation object, as the bow
             * case above reads his. The table row's kind is kPlayerHeldClawshot for both kinds:
             * they share one model. A single clawshot drawn swapped shows field_0x0710 instead,
             * which setHookshotPos enters at frame 0 (d_a_alink_hook.inc:911), so 0 is sent. */
            J3DAnmTransform* itemBck = alink->mItemBck.getBckAnm();
            if (itemBck != nullptr && !alink->mAnmHeap9.checkNoSetIdx() &&
                alink->mAnmHeap9.checkNoSetArcNo())
            {
                const std::uint8_t id = daRemotePlayer_heldItemBckToWire(
                    alink->mAnmHeap9.getIdx(), kPlayerHeldClawshot);
                const f32 frameMax = itemBck->getFrameMax();
                if (id != kPlayerHeldBckNone && frameMax > 0.0f) {
                    out.heldBck = id;
                    if (out.heldItem == kPlayerHeldDoubleClawshot || alink->field_0x3020 == 0) {
                        const f32 f = itemBck->getFrame() / frameMax;
                        out.heldBckFrame = static_cast<std::uint8_t>(
                            f <= 0.0f ? 0.0f : (f >= 1.0f ? 255.0f : f * 255.0f + 0.5f));
                    }
                }
            }
        } else if (item == dItemNo_COPY_ROD_e) {
            out.heldItem = kPlayerHeldCopyRod;
            if (alink->field_0x0724 != nullptr && alink->field_0x0724->getFrameMax() > 0) {
                const f32 f = alink->field_0x0724->getFrame() / alink->field_0x0724->getFrameMax();
                out.heldMat = static_cast<std::uint8_t>(
                    f <= 0.0f ? 0.0f : (f >= 1.0f ? 255.0f : f * 255.0f + 0.5f));
            }
        } else if (item == dItemNo_HORSE_FLUTE_e) {
            out.heldItem = kPlayerHeldHorseCall;
        } else if (item == dItemNo_IRONBALL_e) {
            // The ball AND its chain are drawn under this same checkItemDraw() (d_a_alink.cpp:
            // 19771, :19793-19798). Where it is: fill_iron_ball().
            out.heldItem = kPlayerHeldIronBall;
        }
        // Anything else (grass whistle, wolf change, fishing) is not reproduced.
    }

    if (!wolf && item == dItemNo_BOOMERANG_e) {
        fopAc_ac_c* boom = alink->mItemAcKeep.getActor();
        if (boom != nullptr && fopAcM_GetName(boom) == fpcNm_BOOMERANG_e &&
            fopAcM_GetParam(boom) == 0)
        {
            out.heldItem = kPlayerHeldBoomerang;
        }
    }

    /* v19 block A: the nocked arrow. A daArrow_c of its own, kept in mItemAcKeep by makeArrow
     * (d_a_alink_bow.inc:110-126), that draws itself on his left item joint while it waits
     * (setKeepMatrix, d_a_arrow.cpp:402-423) — so it is read off that actor, independent of the
     * bow model above. Its draw gates are checkWait() (procWait; 1/2 = shot ordered) and
     * deleteArrow()'s field_0x93f. NOT field_0x942: that is "OUR camera is in first person"
     * (procWait, d_a_arrow.cpp:538-542), a property of the viewer, and the partner looking at the
     * puppet is never inside this Link's head. Types 0/1 only — 4 is the slingshot pellet, which
     * flies the tick it is made, and 2 is Zelda's (d_a_hozelda.cpp). */
    if (!wolf) {
        fopAc_ac_c* keep = alink->mItemAcKeep.getActor();
        if (keep != nullptr && fopAcM_GetName(keep) == fpcNm_ARROW_e) {
            daArrow_c* arrow = static_cast<daArrow_c*>(keep);
            if (arrow->checkWait() && arrow->field_0x93f == 0 &&
                (arrow->mArrowType == 0 || arrow->mArrowType == 1))
            {
                out.heldFlags |= kPlayerHeldArrowNocked;
                if (arrow->checkBombArrow()) {
                    out.heldBck |= kPlayerHeldBckBombArrow;
                }
            }
        }
    }

    // The lantern: his draw gate (d_a_alink.cpp:19822) and his placement test (:6098).
    if (!wolf && alink->checkNoResetFlg2(daPy_py_c::FLG2_UNK_1)) {
        out.heldFlags |= kPlayerHeldLanternShown;
        if (item == dItemNo_KANTERA_e || alink->checkOilBottleItemNotGet(item)) {
            out.heldFlags |= kPlayerHeldLanternInHand;
        }
    }

    // The UPPER_2 overlay. ALANM only: a demo-archive overlay cannot be resolved by the puppet.
    const daPy_anmHeap_c& upper2 = alink->mUpperAnmHeap[2];
    if (!upper2.checkNoSetIdx() && upper2.checkNoSetArcNo() &&
        alink->mNowAnmPackUpper[2].getAnmTransform() != nullptr)
    {
        out.upper2Idx = upper2.getIdx();
        out.upper2Frame =
            act_encode_u16(alink->mUpperFrameCtrl[2].getFrame(), kPlayerActFrameScale);
        const f32 ratio = alink->mNowAnmPackUpper[2].getRatio();
        out.upper2Ratio = static_cast<std::uint8_t>(
            ratio <= 0.0f ? 0.0f : (ratio >= 1.0f ? 255.0f : ratio * 255.0f + 0.5f));
    } else if (!upper2.checkNoSetIdx() && !upper2.checkNoSetArcNo() && !s_loggedDemoUpper2) {
        s_loggedDemoUpper2 = true;
        Log.debug("Local UPPER_2 overlay {} is from demo arc {}; not replicated (logged once)",
            upper2.getIdx(), upper2.getArcNo());
    }

    /* The morf, latched on the tick the overlay idx changed (to another BCK OR to none: his
     * resetUpperAnime morfs too, d_a_alink.cpp:7440). Same counter+2 reasoning as block 2's actMorf
     * (fill_action_anim) — REASONED, not measured, and it shares field_0x2060 with the under morf,
     * so a tick that changes both reads whichever he set last, exactly as his own blend does. */
    if (out.upper2Idx != s_latchedUpper2) {
        s_latchedUpper2 = out.upper2Idx;
        const f32 counter =
            alink->field_0x2060 != nullptr ? alink->field_0x2060->getOldFrameMorfCounter() : 0.0f;
        const f32 morf = counter > 0.0f ? counter + 2.0f : 0.0f;
        s_latchedUpper2Morf = static_cast<std::uint8_t>(morf > 255.0f ? 255.0f : morf + 0.5f);
    }
    out.upper2Morf = s_latchedUpper2Morf;

    {
        static std::uint8_t s_lastSentBck = 0;
        static bool s_lastSentArrow = false;
        static std::uint16_t s_bckLogCount = 0;
        const bool arrow = (out.heldFlags & kPlayerHeldArrowNocked) != 0;
        if ((out.held_bck_id() != (s_lastSentBck & kPlayerHeldBckIdMask) ||
                arrow != s_lastSentArrow) &&
            s_bckLogCount < 200)
        {
            s_bckLogCount++;
            Log.debug("Local held item BCK {} -> {} (res {}, frame {}/255), arrow {} -> {}{}",
                static_cast<unsigned>(s_lastSentBck & kPlayerHeldBckIdMask),
                static_cast<unsigned>(out.held_bck_id()),
                alink->mAnmHeap9.checkNoSetIdx() ? -1 : static_cast<int>(alink->mAnmHeap9.getIdx()),
                static_cast<unsigned>(out.heldBckFrame), s_lastSentArrow ? "nocked" : "none",
                arrow ? "nocked" : "none",
                (out.heldBck & kPlayerHeldBckBombArrow) != 0 ? " (bomb)" : "");
        }
        s_lastSentBck = out.heldBck;
        s_lastSentArrow = arrow;
    }
    if (out.heldItem != s_lastSentKind) {
        if (s_kindLogCount < 100) {
            s_kindLogCount++;
            Log.debug("Local held item {} -> {} (mEquipItem 0x{:X}, flags 0x{:02x}, mat 0x{:02x})",
                static_cast<unsigned>(s_lastSentKind), static_cast<unsigned>(out.heldItem),
                static_cast<unsigned>(item), static_cast<unsigned>(out.heldFlags),
                static_cast<unsigned>(out.heldMat));
        }
        s_lastSentKind = out.heldItem;
    }
    if (out.upper2Idx != s_lastSentUpper2) {
        if (s_upper2LogCount < 200) {
            s_upper2LogCount++;
            Log.debug(
                "Local UPPER_2 overlay {} -> {} (frame {:.1f}, ratio {}, morf {}), proc 0x{:X}",
                s_lastSentUpper2, out.upper2Idx, out.upper2_frame(),
                static_cast<unsigned>(out.upper2Ratio), static_cast<unsigned>(out.upper2Morf),
                static_cast<unsigned>(alink->mProcID));
        }
        s_lastSentUpper2 = out.upper2Idx;
    }
}

/* ── v21 form block (wolf piece A) ─────────────────────────────────────────────────────────────
 * .claude/future-plans/puppet-wolf.md §3 and §6. */

/// AL_WF, the fur overlay setMetamorphoseModel makes his held model (d_a_alink.cpp:14145). There is
/// no named item number for it.
constexpr u16 kMetamorphoseFurItem = 0x106;

/**
 * The form byte and the tint, from SCALARS on daAlink_c only: mProcID, mClothesChangeWaitTimer,
 * mProcVar1/3/5 (field_0x300a / 300e / 3012), mEquipItem, the mHeldItemModel POINTER (compared,
 * never dereferenced) and one save flag. No model, no animation object.
 *
 * ★ That restriction is the point. During the model swap (mClothesChangeWaitTimer != 0)
 * loadModelDVD frees mpArcHeap at timer 2 (d_a_alink_swindow.inc:67-115), so capture does not run
 * (local_pose_is_live) and the far end gets the HELD pose. Those are exactly the ticks
 * kPlayerFormChanging exists for, so refresh_held_form() runs this on the held copy then, and it
 * must be safe while his models are gone. AL_WF itself lives in the item heap (setItemHeap,
 * :14136), not mpArcHeap, but nothing here relies on that.
 *
 * The proc vars are a union shared by every proc, so they are read only while mProcID is
 * PROC_METAMORPHOSE. Their meaning there, checked against the writers (procCoMetamorphoseInit /
 * procCoMetamorphose, d_a_alink.cpp:17421-17696):
 *   - field_0x300a != 0: the no-effect demo branch (init :17447) or the second half has ended and
 *     he waits for the event to finish (:17572, :17622) — metaPhase 0, no tint, per the spec.
 *     procCoMetamorphose returns at once then (:17545), so field_0x300e keeps its last value and
 *     his draw keeps using it (:19531-19536). That residue is ~0 when the anim ran out, but NOT
 *     when the player cancelled the second half early (:17603); the puppet then drops a faint
 *     tint a few frames before he does. Accepted, not measured.
 *   - field_0x3012 == 0: before the swap (zeroed :17531), != 0 after it (set :17560);
 *   - field_0x300e: the TevColor he draws with, written every tick (:17643-17666), range -64..64.
 *
 * PROC_METAMORPHOSE_ONLY shares loadModelDVD but draws nothing during the swap (the blob is
 * PROC_METAMORPHOSE only, :19505-19522), so it sends metaPhase 0 and no changing bit; the far end
 * holds his last pose for those few ticks, as for an outfit change.
 */
std::uint8_t local_form_scalars(daAlink_c* alink, std::int8_t& o_tint) {
    o_tint = 0;
    std::uint8_t form = 0;
    if (alink == nullptr) {
        return form;
    }
    // His own draw gate for the shackle chain (d_a_alink.cpp:19616); the receiver applies it to a
    // wolf only, as his draw does.
    if (dComIfGs_isEventBit(dSv_event_flag_c::M_011)) {
        form |= kPlayerFormChain;
    }
    if (alink->mProcID == daAlink_c::PROC_METAMORPHOSE) {
        if (alink->mClothesChangeWaitTimer != 0) {
            form |= kPlayerFormChanging;
        }
        if (alink->mProcVar1.field_0x300a == 0) {
            form |= static_cast<std::uint8_t>(alink->mProcVar5.field_0x3012 == 0 ? 1 : 2);
            const int tint = alink->mProcVar3.field_0x300e;
            o_tint = static_cast<std::int8_t>(tint > 127 ? 127 : (tint < -128 ? -128 : tint));
        }
    }
    if (alink->mEquipItem == kMetamorphoseFurItem && alink->mHeldItemModel != nullptr) {
        form |= kPlayerFormFur;
    }
    return form;
}

/// Latched edge logs for the form, shared by the live and held paths. Keyed on CHANGE, so the
/// second capture of a tick (--mp-trace) and the held refresh both see "no change" and stay quiet.
void note_local_form(daAlink_c* alink, std::uint8_t form, std::int8_t tint, const char* path) {
    static int s_lastWolf = -1;
    static int s_lastForm = -1;
    static std::uint16_t s_logCount = 0;
    if (alink == nullptr) {
        return;
    }
    const int wolf = alink->checkWolf() ? 1 : 0;
    if (wolf != s_lastWolf) {
        if (s_logCount < 200) {
            s_logCount++;
            if (s_lastWolf < 0) {
                Log.info("Local form is {} at first capture (proc 0x{:X})", wolf ? "wolf" : "human",
                    static_cast<unsigned>(alink->mProcID));
            } else {
                Log.info("Local form {} (proc 0x{:X}, {} path)",
                    wolf ? "human->wolf" : "wolf->human", static_cast<unsigned>(alink->mProcID),
                    path);
            }
        }
        s_lastWolf = wolf;
    }
    if (static_cast<int>(form) != s_lastForm) {
        if (s_logCount < 200) {
            s_logCount++;
            const unsigned from = s_lastForm < 0 ? 0u : static_cast<unsigned>(s_lastForm);
            Log.info("Local form metaPhase {}->{} (form 0x{:02x}->0x{:02x}: changing {}, fur {}, "
                     "chain {}; tint {}, clothes timer {}, proc 0x{:X}, {} path)",
                from & kPlayerFormMetaPhaseMask, form & kPlayerFormMetaPhaseMask, from,
                static_cast<unsigned>(form), (form & kPlayerFormChanging) != 0 ? 1 : 0,
                (form & kPlayerFormFur) != 0 ? 1 : 0, (form & kPlayerFormChain) != 0 ? 1 : 0,
                static_cast<int>(tint), static_cast<unsigned>(alink->mClothesChangeWaitTimer),
                static_cast<unsigned>(alink->mProcID), path);
        }
        s_lastForm = form;
    }
}

/**
 * Fill the v21 form block on the LIVE path. The scalars above, plus the fur overlay's frame off
 * the animation object his setItemMatrix entered it into (mItemBck, init'd by setMetamorphoseModel,
 * :14140), as frame / frameMax x255 — the same encoding as heldBckFrame. Only while metaPhase != 0,
 * which is when the wire carries it.
 *
 * Never runs while mClothesChangeWaitTimer != 0: capture_local_player is gated off then
 * (local_pose_is_live), so reading mItemBck here is safe. refresh_held_form covers those ticks
 * without it.
 */
void fill_form(daAlink_c* alink, PlayerState& out) {
    std::int8_t tint = 0;
    out.form = local_form_scalars(alink, tint);
    out.metaTint = 0;
    out.furFrame = 0;
    if (alink == nullptr || out.meta_phase() == 0) {
        note_local_form(alink, out.form, 0, "live");
        return;
    }
    out.metaTint = tint;
    if ((out.form & kPlayerFormFur) != 0) {
        J3DAnmTransform* furBck = alink->mItemBck.getBckAnm();
        if (furBck != nullptr && furBck->getFrameMax() > 0) {
            const f32 f = furBck->getFrame() / furBck->getFrameMax();
            out.furFrame = static_cast<std::uint8_t>(
                f <= 0.0f ? 0.0f : (f >= 1.0f ? 255.0f : f * 255.0f + 0.5f));
        }
    }
    note_local_form(alink, out.form, tint, "live");
}

}  // namespace

/**
 * "There is a real, playable scene" — the gate for both sending our pose and spawning puppets.
 *
 * getPlayer(0) alone is NOT enough, and this took three measured attempts to get right.
 *
 * The title screen runs an attract demo in which Link is a live actor walking real stages, so the
 * null check passes several seconds before a save is even loaded, and passes again whenever the
 * game idles back to the title. Two bugs came out of trusting it: a puppet created into the demo
 * scene (which is about to be torn down) hung the game outright, and demo poses went out on the
 * wire as gameplay, so every remote puppet re-enacted the attract sequence and then teleported
 * ~44,000 units when the real save finally loaded.
 *
 * Adding !event_runCheck() was still not enough — the demo has stretches with no event running,
 * which a trace caught as a stray in-world sample at the demo's start pose. The decisive test is
 * whether the title actor exists at all: it owns the attract sequence, so while it is alive
 * nothing on screen is gameplay no matter what the player actor says.
 *
 * The event check stays because it additionally keeps puppets out of cutscenes and warp
 * transitions, which is wanted in its own right.
 */
bool world_is_playable() {
    return dComIfGp_getPlayer(0) != nullptr && !dComIfGp_event_runCheck() &&
           fopAcM_SearchByName(fpcNm_TITLE_e) == NULL;
}

/**
 * Which event the local Link is in. Spec: .claude/future-plans/puppet-events.md §1 and §6.
 *
 * Read from what he is doing, not from inputs. setDemoData forces the TOOL type whenever a
 * dDemo_actor_c drives him, which is an STB cutscene (d_a_alink_demo.inc:636-656). Otherwise it
 * sets SYSTEM, and checkDemoAction picks the proc. The procs below are the frequent ones the spec
 * lists.
 *
 * SPECIAL is tested first so the in-place events keep exactly the classification
 * link_acting_in_place gave them before. A demo-archive BCK in slot 0 (mArcNo != 0xFFFF,
 * daPy_anmHeap_c::loadData, d_a_player.cpp:300-331) counts as a cutscene even under SYSTEM. The
 * receiver has none of those archives, so live capture would only send "no override" for them
 * (fill_action_anim) and the puppet would glide.
 */
LocalEventKind local_event_kind() {
    fopAc_ac_c* link = dComIfGp_getPlayer(0);
    if (link == nullptr || !dComIfGp_event_runCheck() || fopAcM_SearchByName(fpcNm_TITLE_e) != NULL)
    {
        return LocalEventKind::None;
    }
    daAlink_c* alink = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    /* dComIfGp_setPlayer runs at the very top of daAlink_c::create (d_a_alink.cpp:4937), long
     * before his heaps and model exist, so a Link that is still being created has nothing to
     * classify. fpcM_IsCreating is the game's own test (d_a_alink_demo.inc:2541). */
    if (alink == nullptr || fpcM_IsCreating(fopAcM_GetID(alink))) {
        return LocalEventKind::None;
    }

    const int demoType = alink->mDemo.getDemoType();
    if (demoType == daPy_demo_c::DEMO_TYPE_SPECIAL_e) {
        return LocalEventKind::InPlace;
    }
    const daPy_anmHeap_c& under = alink->mUnderAnmHeap[0];
    const daPy_anmHeap_c& upper = alink->mUpperAnmHeap[0];
    if (demoType == daPy_demo_c::DEMO_TYPE_TOOL_e || alink->mProcID == daAlink_c::PROC_TOOL_DEMO ||
        !under.checkNoSetArcNo() || (!upper.checkNoSetIdx() && !upper.checkNoSetArcNo()))
    {
        return LocalEventKind::Cutscene;
    }
    switch (alink->mProcID) {
    case daAlink_c::PROC_TALK:
        return LocalEventKind::Talk;
    case daAlink_c::PROC_OPEN_TREASURE:
        return LocalEventKind::Chest;
    case daAlink_c::PROC_GET_ITEM:
        return LocalEventKind::GetItem;
    case daAlink_c::PROC_DOOR_OPEN:
        return LocalEventKind::Door;
    default:
        return LocalEventKind::Scripted;
    }
}

const char* local_event_kind_name(LocalEventKind kind) {
    switch (kind) {
    case LocalEventKind::None:
        return "none";
    case LocalEventKind::Talk:
        return "talk";
    case LocalEventKind::Chest:
        return "chest";
    case LocalEventKind::GetItem:
        return "get-item";
    case LocalEventKind::Door:
        return "door";
    case LocalEventKind::InPlace:
        return "in-place";
    case LocalEventKind::Scripted:
        return "scripted";
    case LocalEventKind::Cutscene:
        return "cutscene";
    }
    return "?";
}

/**
 * Link is in an event, but what he draws is still his own ALANM animation, so his real pose is
 * worth sending. Block 2 carries it as it is: ANM_TALK, TRES_OPEN_*, GET_A, DOOR_OPEN_* are all
 * ALANM indices and none of those procs is in puppet_reproduces_proc (puppet-events.md §2).
 *
 * This widens the old link_acting_in_place carve-out, which covered DEMO_TYPE_SPECIAL_e only:
 * drinking from a bottle, pouring oil, the whistle, the boomerang catch (~23 procs). Without that
 * one the far end froze on the first frame of the drink for ~3.6 s. Talk, chest, item get, door and
 * the stage-entry walk-in froze the same way, mid-stride, until now.
 *
 * The scene-change and title tests come first, as before. A queued scene change (death, lava
 * return, warp, a door to another stage) cuts the event off before the load starts. STB cutscenes
 * stay out: they teleport him onto the cast's marks and play demo-archive BCKs that the receiver
 * cannot resolve. wire_local_state sends a standing idle for those instead (P2).
 *
 * Hidden Link (FLG0_PLAYER_NO_DRAW alone, set by the warp, the cannon, boss doors, Henna's boat)
 * stays out too, so the far end keeps the last visible pose rather than a puppet standing inside a
 * cannon. checkPlayerNoDraw() is NOT that test: it is also true in his own first-person view
 * (d_a_alink_link.inc:436-439). In-place events skip the hidden test, which keeps them exactly as
 * they were.
 *
 * Capture only: puppet spawning, name tags and the receive side still use world_is_playable().
 */
static bool link_in_live_event(LocalEventKind kind) {
    // local_event_kind() has already answered None for no Link and for the title's attract demo.
    if (dComIfGp_isEnableNextStage()) {
        return false;
    }
    if (kind == LocalEventKind::None || kind == LocalEventKind::Cutscene) {
        return false;
    }
    daAlink_c* alink = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    // local_event_kind already refused a Link that is still being created; the model check is
    // what the readers below assume.
    if (alink == nullptr || alink->mpLinkModel == nullptr) {
        return false;
    }
    if (kind == LocalEventKind::InPlace) {
        return true;
    }
    return alink->checkNoResetFlg0(daPy_py_c::FLG0_PLAYER_NO_DRAW) == 0;
}

/**
 * One line per change of event kind and of how the pose goes out, so the two ends' logs can be
 * lined up against "Puppet N action override" on the receiver. Edge-keyed, so the several calls
 * per tick (capture twice, the trace readers) log it once. Capped.
 */
static void log_local_event_edge(LocalEventKind kind, bool live) {
    static LocalEventKind s_lastKind = LocalEventKind::None;
    static bool s_lastLive = true;
    static int s_logCount = 0;
    if (kind == s_lastKind && live == s_lastLive) {
        return;
    }
    const LocalEventKind was = s_lastKind;
    s_lastKind = kind;
    s_lastLive = live;
    if (s_logCount >= 200) {
        return;
    }
    s_logCount++;

    const char* how =
        live ? "live pose" : (kind == LocalEventKind::Cutscene ? "stand-in idle" : "held pose");
    const daAlink_c* alink = static_cast<const daAlink_c*>(dComIfGp_getLinkPlayer());
    if (alink == nullptr || kind == LocalEventKind::None) {
        Log.info("Local event {} -> {} ({})", local_event_kind_name(was),
            local_event_kind_name(kind), how);
        return;
    }
    Log.info("Local event {} -> {} ({}): proc 0x{:X}, demo type {}, mode {}, actUnder {} arc {}, "
             "hidden {}",
        local_event_kind_name(was), local_event_kind_name(kind), how,
        static_cast<unsigned>(alink->mProcID), alink->mDemo.getDemoType(),
        static_cast<unsigned>(alink->mDemo.getDemoMode()), alink->mUnderAnmHeap[0].getIdx(),
        alink->mUnderAnmHeap[0].getArcNo(),
        alink->checkNoResetFlg0(daPy_py_c::FLG0_PLAYER_NO_DRAW) != 0 ? 1 : 0);
}

/// The process id of the Link the last successful capture read. See local_player_is_last_captured.
static fpc_ProcID s_lastCapturedLink = fpcM_ERROR_PROCESS_ID_e;

bool local_player_is_last_captured() {
    fopAc_ac_c* link = dComIfGp_getPlayer(0);
    return link != nullptr && fopAcM_GetID(link) == s_lastCapturedLink;
}

bool local_scene_change_queued() {
    /* Latched: the queued flag clears once the scene request is issued, partway through the load,
     * and the held pose then went out "in world" again: the far end respawned the puppet frozen at
     * the exit for ~1.4 s (measured, F_SP103). So "leaving" lasts until a different Link exists,
     * i.e. the new scene has its player. Capped so a cancelled change can't hide us forever. */
    static bool s_leaving = false;
    static fpc_ProcID s_leavingLink = fpcM_ERROR_PROCESS_ID_e;
    static std::chrono::steady_clock::time_point s_since;
    constexpr auto kMaxLeaving = std::chrono::seconds(10);

    fopAc_ac_c* link = dComIfGp_getPlayer(0);
    const auto now = std::chrono::steady_clock::now();
    if (dComIfGp_isEnableNextStage()) {
        if (!s_leaving) {
            s_leaving = true;
            s_since = now;
        }
        s_leavingLink = link != nullptr ? fopAcM_GetID(link) : fpcM_ERROR_PROCESS_ID_e;
        return true;
    }
    if (s_leaving) {
        const bool newLink = link != nullptr && fopAcM_GetID(link) != s_leavingLink;
        if (newLink || now - s_since > kMaxLeaving) {
            s_leaving = false;
        }
    }
    return s_leaving;
}

/// The gate for reading Link's pose: in play, or in one of his own events that still draws his
/// own animation (link_in_live_event).
bool local_pose_is_live() {
    /* A queued scene change counts as gone. The load that follows can stall this process for
     * seconds without a tick, so whatever we sent last is what the far end holds: an "in world"
     * pose left the puppet frozen mid-roll at the exit for ~2 s (measured, F_SP103). Saying "not
     * in world" here makes that last packet hide it instead. */
    if (local_scene_change_queued()) {
        return false;
    }
    /* His model archive is being reloaded: an outfit change (the collection menu, the save editor,
     * the autopilot) or the human/wolf change, which share loadModelDVD. It frees mpArcHeap at
     * timer 2 and changeLink rebuilds the models at timer 1 (d_a_alink_swindow.inc:67-115), and his
     * own draw skips those ticks. Every fill_* that reads his models would read freed memory then
     * (measured: an access violation in shown_hand_index on `clothes 0x30`), so the far end holds
     * the last pose for those few ticks instead. */
    if (fopAc_ac_c* link = dComIfGp_getPlayer(0); link != nullptr &&
        static_cast<daPy_py_c*>(link)->getClothesChangeWaitTimer() != 0)
    {
        return false;
    }
    if (world_is_playable()) {
        log_local_event_edge(LocalEventKind::None, true);
        return true;
    }
    const LocalEventKind kind = local_event_kind();
    const bool live = link_in_live_event(kind);
    log_local_event_edge(kind, live);
    return live;
}

/**
 * v16: where the local player is, for the far end's hide and snap decisions.
 *
 * The epoch counts re-creations of Link's actor. Every scene load rebuilds him (a stage change, a
 * same-stage warp, a void-out respawn), and those are exactly the discontinuities a receiver must
 * not interpolate across; walking into the next room is continuous and does not rebuild him. The
 * stage hash is checked as well, so a process id that happened to repeat could not hide a stage
 * change. Static state, so the trace's second call per tick sees the same answer and logs nothing.
 */
static void fill_location(fopAc_ac_c* link, PlayerState& out) {
    static fpc_ProcID s_lastLinkId = fpcM_ERROR_PROCESS_ID_e;
    static std::uint32_t s_lastStageHash = 0;
    static std::uint8_t s_epoch = 0;

    const char* stage = dComIfGp_getStartStageName();
    const std::uint32_t hash = location_stage_hash(stage);
    const fpc_ProcID id = fopAcM_GetID(link);
    if (id != s_lastLinkId || hash != s_lastStageHash) {
        s_lastLinkId = id;
        s_lastStageHash = hash;
        s_epoch++;
        char name[9] = {};
        std::memcpy(name, stage, 8);
        Log.info("Local location epoch {}: stage '{}' (hash {:08X}) room {}, Link process {}",
            s_epoch, static_cast<const char*>(name), hash, static_cast<int>(fopAcM_GetRoomNo(link)),
            id);
    }
    out.stageHash = hash;
    out.roomNo = static_cast<std::int8_t>(fopAcM_GetRoomNo(link));
    out.locEpoch = s_epoch;
}

/* --- v19: the live sword hitbox, for friendly fire. ------------------------------------------ */
/**
 * Live exactly while daAlink_c's own sword hitbox is: setSwordAtCollision raises FLG0_CUT_AT_FLG
 * and each capsule's set bit for the frames the blade can hit, and resetAtCollision drops both
 * (d_a_alink.cpp:6190-6247, :6348-6367). Sword attacks only: capsule 0 also carries the hookshot
 * and the lantern swing, which friendly fire leaves out.
 */
void fill_attack(daAlink_c* alink, PlayerState& out) {
    out.atMask = 0;
    out.atRadius = 0;
    out.atSpl = 0;
    std::fill(std::begin(out.atPt), std::end(out.atPt), std::int16_t{0});
    if (alink == nullptr || alink->checkWolf() || !alink->getCutAtFlg()) {
        return;
    }
    dCcD_Cps* cps = alink->mAtCps;
    if ((cps[0].GetAtType() & (AT_TYPE_NORMAL_SWORD | AT_TYPE_MASTER_SWORD)) == 0) {
        return;
    }
    std::uint8_t mask = 0;
    for (int i = 0; i < 3; i++) {
        if (cps[i].ChkAtSet()) {
            mask |= static_cast<std::uint8_t>(1 << i);
        }
    }
    if (mask == 0) {
        return;
    }
    const cXyz& origin = alink->current.pos;
    const cXyz* points[3] = {cps[0].GetStartP(), cps[0].GetEndP(), cps[1].GetStartP()};
    for (int p = 0; p < 3; p++) {
        const cXyz rel = *points[p] - origin;
        const float axes[3] = {rel.x, rel.y, rel.z};
        for (int k = 0; k < 3; k++) {
            out.atPt[p * 3 + k] =
                static_cast<std::int16_t>(std::lround(std::clamp(axes[k], -32767.0f, 32767.0f)));
        }
    }
    out.atMask = mask;
    out.atRadius = static_cast<std::uint8_t>(std::clamp(cps[0].GetR(), 1.0f, 255.0f));
    out.atSpl = static_cast<std::uint8_t>(cps[0].GetAtSpl());
}

// --- damage flash + lantern fx (agent D) ---
namespace {

const char* dmg_tint_name(std::uint8_t i_kind) {
    switch (i_kind) {
    case kPlayerDmgTintDamage:
        return "damage";
    case kPlayerDmgTintFreeze:
        return "freeze";
    case kPlayerDmgTintIce:
        return "ice";
    default:
        return "none";
    }
}

/**
 * The damage tint: WHICH branch of daAlink_c::draw's tint ladder he took this tick
 * (d_a_alink.cpp:19530-19583) and the timer that branch reads. The ladder is an else-if chain, so
 * it is walked in his order and the first branch that holds wins, including the three above the
 * damage tints that this feature does not reproduce: while one of those owns his colour, None is
 * sent rather than a tint he is not showing.
 *
 * The damage branch is entered on mDamageTimer alone; with the Magic Armor absorbing the hit
 * (checkMagicArmorNoDamage) it then tints nothing AND skips the ice test below it, so that case
 * sends None, as the chain does.
 *
 * Idempotent within a tick (capture runs twice, see fill_action_anim): the statics only gate logs.
 */
void fill_damage_flash(daAlink_c* alink, PlayerState& out) {
    static std::uint8_t s_lastKind = 0;
    static std::uint16_t s_logCount = 0;

    if (alink == nullptr) {
        s_lastKind = kPlayerDmgTintNone;
        return;
    }

    std::uint8_t kind = kPlayerDmgTintNone;
    int timer = 0;
    if (alink->field_0x06f4 != nullptr || alink->mProcID == daAlink_c::PROC_METAMORPHOSE ||
        alink->mProcID == daAlink_c::PROC_DUNGEON_WARP ||
        alink->mProcID == daAlink_c::PROC_DUNGEON_WARP_SCN_START || alink->field_0x346c != 0.0f)
    {
        // A higher branch of his ladder owns the colour; none of them is a damage tint.
    } else if (alink->checkFreezeDamage()) {
        kind = kPlayerDmgTintFreeze;
    } else if (alink->mDamageTimer != 0) {
        if (!alink->checkMagicArmorNoDamage()) {
            kind = kPlayerDmgTintDamage;
            timer = alink->mDamageTimer + alink->mDamageColorTime;
        }
    } else if (alink->mIceDamageWaitTimer != 0) {
        kind = kPlayerDmgTintIce;
        timer = alink->mIceDamageWaitTimer;
    }

    out.fxFlags |=
        static_cast<std::uint8_t>((kind << kPlayerFxDmgTintShift) & kPlayerFxDmgTintMask);
    out.dmgFlash = static_cast<std::uint8_t>(std::clamp(timer, 0, 255));

    if (kind != s_lastKind) {
        if (s_logCount < 64) {
            s_logCount++;
            Log.debug("Local damage tint {} -> {}: dmgFlash {} (mDamageTimer {}, colorTime {}, "
                      "iceTimer {}, proc 0x{:X})",
                dmg_tint_name(s_lastKind), dmg_tint_name(kind), static_cast<unsigned>(out.dmgFlash),
                static_cast<int>(alink->mDamageTimer),
                static_cast<unsigned>(alink->mDamageColorTime),
                static_cast<int>(alink->mIceDamageWaitTimer),
                static_cast<unsigned>(alink->mProcID));
        }
        s_lastKind = kind;
    }
}

/**
 * kPlayerFxLanternLit: his flame is burning. This is the condition daAlink_c::setLight refreshes
 * the ZI_J_KANTERA_FIRE emitter and the glow under (d_a_alink.cpp:14862-14888), transcribed for the
 * case the wire can show at all — the lantern out (FLG2_UNK_1, which is kPlayerHeldLanternShown):
 * oil left, not snuffed (FLG2_KANDELAAR_LIGHT_OFF: set while pouring or scooping oil into it,
 * d_a_alink_kandelaar.inc:276, d_a_alink_bottle.inc:578, and by d_a_npc_bans.cpp:1342), and not
 * frozen. The ERFLG1_UNK_4 / _10 half is setKandelaarMtx's cutscene lantern, which clears
 * FLG2_UNK_1 as it takes over (d_a_alink_kandelaar.inc:37-52), so it never reaches here.
 *
 * Not his FLG1_UNK_80, though that is the flag his setLight sets for the same thing: the Light
 * Sword sets it too (:14911-14914), so it would light a cold lantern on the hip of a player holding
 * the Light Sword. It is compared, and a disagreement in the other direction (predicate lit, flag
 * off) is logged once, because that one would mean this transcription has missed a branch.
 */
void fill_lantern_fx(daAlink_c* alink, PlayerState& out) {
    static bool s_lastLit = false;
    static std::uint16_t s_logCount = 0;
    static bool s_loggedDisagree = false;

    const bool lit = alink != nullptr && !alink->checkWolf() &&
                     (out.heldFlags & kPlayerHeldLanternShown) != 0 &&
                     alink->checkNoResetFlg2(daPy_py_c::FLG2_UNK_1) && dComIfGs_getOil() != 0 &&
                     !alink->checkNoResetFlg2(daPy_py_c::FLG2_KANDELAAR_LIGHT_OFF) &&
                     !alink->checkFreezeDamage();
    if (lit) {
        out.fxFlags |= kPlayerFxLanternLit;
        if (!s_loggedDisagree && !alink->checkNoResetFlg1(daPy_py_c::FLG1_UNK_80)) {
            s_loggedDisagree = true;
            Log.warn("Local lantern reads lit but his FLG1_UNK_80 is off (proc 0x{:X}, oil {}); "
                     "fill_lantern_fx's predicate may be missing a branch of setLight (logged "
                     "once)",
                static_cast<unsigned>(alink->mProcID), static_cast<unsigned>(dComIfGs_getOil()));
        }
    }

    if (lit != s_lastLit) {
        if (s_logCount < 64) {
            s_logCount++;
            Log.debug("Local lantern {} (oil {}, held 0x{:02x})", lit ? "LIT" : "out",
                alink != nullptr ? static_cast<unsigned>(dComIfGs_getOil()) : 0u,
                static_cast<unsigned>(out.heldFlags));
        }
        s_lastLit = lit;
    }
}

}  // namespace

/**
 * v20 fxFlags bits 1-3 and 6-7 (agent F): the sword trail, the spin attack's effect and the sword
 * charge. What each bit means, and the daAlink_c line it comes from, is in the block under
 * kPlayerFxKnownMask (player_state.hpp); this is the reading.
 *
 * ★ The trail is read off its OUTCOME, m_swordBlur's point count, rather than by re-deriving the
 * branch in setAtCollision that decides whether to extend it (d_a_alink.cpp:6560-6636). That
 * decision is a reset flag, four procs, a large-jump sub-state, a no-blur cut-type list and a
 * collision path that is not reached every tick; each re-derivation of a daAlink_c decision so far
 * has missed a branch. The count only RISES in copyBlur, so "it rose since the previous tick" is
 * the answer itself — with one subtlety: at its cap a copy leaves it at 58 while a decay takes it
 * to 48, so 58 -> 58 is a copy too.
 *
 * "The previous tick" is keyed on g_Counter.mCounter0 (bumped once per fapGm_Execute). The session
 * trace calls capture_local_player a second time in the same tick, and without the key that call
 * would see "no change" and send the trail as off. A skipped capture (a load, a cutscene) just
 * leaves the history a tick or more stale, which can cost one trail tick and nothing else.
 */
static void fill_sword_fx(daAlink_c* alink, PlayerState& out) {
    static bool s_haveTick = false;
    static u32 s_tick = 0;
    static int s_prevCount = 0;
    static std::uint8_t s_bits = 0;
    static std::uint8_t s_loggedBits = 0;
    static u32 s_onSince[3] = {0, 0, 0};
    static u16 s_logCount = 0;
    const u16 logMax = 64;

    if (s_haveTick && g_Counter.mCounter0 == s_tick) {
        out.fxFlags |= s_bits;
        return;
    }

    std::uint8_t bits = 0;
    int count = 0;
    if (alink != nullptr && !alink->checkWolf()) {
        count = alink->m_swordBlur.field_0x14;
        const bool spinProc = alink->mProcID == daAlink_c::PROC_CUT_TURN ||
                              alink->mProcID == daAlink_c::PROC_BOARD_CUT_TURN;

        if (spinProc && alink->field_0x3204[0] != 0) {
            /* setCutTurnEffect's own list choice, in its order (d_a_alink_effect.inc:1034-1061):
             * underwater first (only reachable here with the Zora armour, or the id would be 0),
             * then the large spin, then the light sword, then the ordinary one. */
            std::uint8_t kind = kPlayerFxSpinNormal;
            if (alink->checkNoResetFlg0(daPy_py_c::FLG0_WATER_IN_MOVE)) {
                kind = kPlayerFxSpinWater;
            } else if (alink->mCutType == daPy_py_c::CUT_TYPE_LARGE_TURN_RIGHT ||
                       alink->mCutType == daPy_py_c::CUT_TYPE_LARGE_TURN_LEFT)
            {
                kind = kPlayerFxSpinLarge;
            } else if (alink->checkNoResetFlg3(daPy_py_c::FLG3_UNK_100000)) {
                kind = kPlayerFxSpinLight;
            }
            bits |= kPlayerFxSpin;
            bits |= static_cast<std::uint8_t>(kind << kPlayerFxVariantShift);
        } else if (s_haveTick && (count > s_prevCount || (count == 58 && s_prevCount == 58))) {
            bits |= kPlayerFxSwordTrail;
            // The alpha the blur was given on this attack tick (d_a_alink.cpp:6567-6572).
            if (alink->m_swordBlur.field_0x20 == daAlinkHIO_cut_c0::m.mDashBlurAlpha) {
                bits |= kPlayerFxTrailDash;
            }
            /* The direction copyBlur stored (field_0x2c = its third argument, field_0x34a4)
             * against the blade's local +Z: setSwordPos makes it +Z for the twirl and the right
             * finisher and -Z for everything else (d_a_alink.cpp:5875-5879). Read off what the
             * blur used rather than off mCutType, which the next execute may already have
             * cleared. */
            if (alink->mSwordModel != nullptr) {
                cXyz localZ;
                mDoMtx_multVecSR(alink->mSwordModel->getBaseTRMtx(), &cXyz::BaseZ, &localZ);
                if (localZ.inprod(alink->m_swordBlur.field_0x2c) > 0.0f) {
                    bits |= kPlayerFxTrailReversed;
                }
            }
        }

        if (alink->m_nSwordBtk != nullptr && alink->m_nSwordBtk->getFrame() > 0.0f) {
            bits |= kPlayerFxSwordCharge;
        }
    }

    /* One line per edge, capped. Each names the tick so the receiver's matching line can be lined
     * up against it; a sender line with no receiver line is the failure this exists to show. */
    const u32 tick = g_Counter.mCounter0;
    const std::uint8_t edgeBits[3] = {kPlayerFxSwordTrail, kPlayerFxSpin, kPlayerFxSwordCharge};
    const char* edgeNames[3] = {"trail", "spin", "charge"};
    for (int i = 0; i < 3; i++) {
        const bool on = (bits & edgeBits[i]) != 0;
        const bool was = (s_loggedBits & edgeBits[i]) != 0;
        if (on == was) {
            continue;
        }
        if (on) {
            s_onSince[i] = tick;
        }
        if (s_logCount < logMax) {
            s_logCount++;
            if (on) {
                // variant = the spin kind with the spin bit, dash | reversed << 1 with the trail.
                Log.debug("Local sword fx: {} ON at tick {} (fx 0x{:02X}, variant {}; blur count "
                          "{}, charge frame {:.0f})",
                    edgeNames[i], tick, static_cast<unsigned>(bits),
                    (bits & kPlayerFxVariantMask) >> kPlayerFxVariantShift, count,
                    alink != nullptr && alink->m_nSwordBtk != nullptr ?
                        alink->m_nSwordBtk->getFrame() :
                        0.0f);
            } else {
                Log.debug("Local sword fx: {} OFF at tick {} after {} tick(s)", edgeNames[i], tick,
                    tick - s_onSince[i]);
            }
        }
    }

    s_loggedBits = bits;
    s_bits = bits;
    s_prevCount = count;
    s_tick = tick;
    s_haveTick = true;
    out.fxFlags |= bits;
}

/**
 * fill_sword_fx's bits, decoded for the puppet, which does not see the wire layout. The two-bit
 * variant field is read as the spin kind only with the spin bit and as the trail's two flags only
 * with the trail bit; a (malformed) state with both set is read as a spin, as the sender sends it.
 */
static void apply_sword_fx(daRemotePlayer_c* puppet, const PlayerState& state) {
    const std::uint8_t fx = state.fxFlags;
    const bool spin = (fx & kPlayerFxSpin) != 0;
    const bool trail = !spin && (fx & kPlayerFxSwordTrail) != 0;

    u8 spinKind = daRemotePlayer_c::SWORD_SPIN_NORMAL_e;
    switch ((fx & kPlayerFxVariantMask) >> kPlayerFxVariantShift) {
    case kPlayerFxSpinLight:
        spinKind = daRemotePlayer_c::SWORD_SPIN_LIGHT_e;
        break;
    case kPlayerFxSpinLarge:
        spinKind = daRemotePlayer_c::SWORD_SPIN_LARGE_e;
        break;
    case kPlayerFxSpinWater:
        spinKind = daRemotePlayer_c::SWORD_SPIN_WATER_e;
        break;
    default:
        break;
    }

    puppet->setNetworkSwordFx(trail, trail && (fx & kPlayerFxTrailDash) != 0,
        trail && (fx & kPlayerFxTrailReversed) != 0, spin, spin ? spinKind : 0,
        (fx & kPlayerFxSwordCharge) != 0);
}

bool capture_local_player(PlayerState& out) {
    if (!local_pose_is_live()) {
        // Nothing worth sending: no scene, or the pose belongs to the attract demo or a cutscene
        // rather than to the player. See world_is_playable().
        return false;
    }

    fopAc_ac_c* link = dComIfGp_getPlayer(0);
    s_lastCapturedLink = fopAcM_GetID(link);
    /* The same actor through the accessor that is documented to hold a daAlink_c — the one the rest
     * of the puppet code reads Link from (d_a_remote_player.cpp:414). Null-checked at every use
     * below; nothing here may assume a scene. */
    daAlink_c* alink = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());

    out.posX = link->current.pos.x;
    out.posY = link->current.pos.y;
    out.posZ = link->current.pos.z;
    // shape_angle is the visual facing that gets baked into the model matrix; current.angle is the
    // logical one and the two diverge while turning. The puppet is a visual, so mirror the visual.
    out.angleY = link->shape_angle.y;
    /* ★ The gait rate daAlink_c computes for HIMSELF, sent whole rather than as any of the inputs
     * to it. getMoveGroundAngleSpeedRate() (d_a_alink.cpp:7546-7556) is the first thing
     * setBlendMoveAnime does (:7561), and everything the receiver needs to choose a gait follows
     * from it.
     *
     * This field has now been wrong twice, both times because the receiver was re-deriving what
     * this line could simply have carried:
     *
     *   - It was speedF, Link's TRANSLATION speed, which is root-motion blended —
     *     speedF = mNormalSpeed * (1 - mSpeedModifier) + footSpeed * mSpeedModifier
     *     (:13028-13034), and mSpeedModifier is mFootPositionRatio, 0.99, at a walk (:7760). So
     *     while walking essentially ALL of Link's movement comes from the walk cycle's own foot
     *     motion — which is why his feet never slide — and speedF sits pinned near that cycle's
     *     natural speed however hard the stick is pushed. Stuart, 2026-08-13: "when MC walks,
     *     puppet walks very slowly", correct at a run. mNormalSpeed, the INTENT, fixed that.
     *   - Sending mNormalSpeed still left the receiver dividing by the HIO constant 23.0 and
     *     ignoring the ground angle. mMaxSpeed is a daAlink_c MEMBER and drops to the lock-on
     *     maximum, or a flat 13.0, while targeting (:7867-7871, :12344-12353); and the cosine of
     *     the ground angle shifts a climbing Link toward the slower gait at the same speed.
     *
     * Both retired here for no extra bytes. Nothing on the receiver wants a speed in engine units:
     * position is replicated outright, so this value only picks the gait and feeds the trace.
     * Checked before changing it. */
    out.moveRate = alink != nullptr ? alink->getMoveGroundAngleSpeedRate() : 0.0f;
    // The game's own function divides by mMaxSpeed; the log shows it returning NaN on the tick an
    // attention set ends. Link survives that internally; a peer receiving it would not.
    if (!std::isfinite(out.moveRate)) {
        out.moveRate = 0.0f;
    }

    /* Latched, permanent, and it exists to stop this being re-diagnosed a third time. The rate is
     * derived from the intent, so the gap it records is the one that was invisible: the two speeds
     * AGREE at a run and diverge most at a walk, and a glance at the local player's numbers proves
     * nothing unless it is taken while walking — exactly when nobody thinks to look. If anyone
     * later "simplifies" this back to a speed, this line is the argument. */
    static bool s_loggedSpeedGap = false;
    if (!s_loggedSpeedGap && alink != nullptr && std::fabs(alink->mNormalSpeed) > 1.0f &&
        std::fabs(link->speedF) < 0.8f * std::fabs(alink->mNormalSpeed))
    {
        s_loggedSpeedGap = true;
        Log.debug("Local gait rate {:.3f} on the wire; intent (mNormalSpeed) {:.2f} vs translation "
                  "(speedF) {:.2f} — {:.0f}% of intent. The wire carries the rate daAlink_c blends "
                  "from; see capture_local_player().",
            out.moveRate, alink->mNormalSpeed, link->speedF,
            100.0f * link->speedF / alink->mNormalSpeed);
    }
    // Sampled every tick rather than on a change event, because there is no change event to hook:
    // daAlink_c::setArcName just overwrites mArcName during the pause menu's model rebuild. Reading
    // it is a pointer compare against four names, so the cost of doing it per tick is nil and the
    // alternative — caching it and missing an unhooked path that changes clothes — is not worth it.
    out.outfit = daRemotePlayer_localOutfitToWire();
    out.flags = kPlayerStateInWorld;

    /* v21: the wolf bit is checkWolf(), i.e. FLG1_IS_WOLF. Checked against its writers rather than
     * its name: set by changeWolf (d_a_alink_wolf.inc:231) and at create for a wolf start
     * (d_a_alink.cpp:4969), cleared by changeLink (wolf.inc:521). Both run from loadModelDVD at the
     * end of the swap, so the bit flips on the first LIVE tick after the blob, with the new body
     * already built. `outfit` stays his human clothes throughout: it is what he turns back into. */
    if (alink != nullptr && alink->checkWolf()) {
        out.flags |= kPlayerStateWolf;
    }

    /* The sharp turn. Read straight off the proc the state machine is IN rather than inferred from
     * anything: commonProcInit writes mProcID = the proc it is entering (d_a_alink.cpp:15144) and
     * procSlipInit enters PROC_SLIP and plays ANM_SLIP in the same two lines
     * (d_a_alink.cpp:16673-16675), so this bit is true for exactly the ticks Link is skidding.
     *
     * Read off `alink` — the LINK_PTR slot fetched at the top — rather than `link`, because that is
     * the slot the rest of the puppet code reads Link from (d_a_remote_player.cpp:414) and the one
     * documented to hold a daAlink_c. Wolf Link never reaches PROC_SLIP (he has his own
     * PROC_WOLF_SLIP_TURN), so a wolf sender simply sends the bit clear, which is the right answer
     * while the puppet has no wolf model to play it on.
     */
    if (alink != nullptr && alink->mProcID == daAlink_c::PROC_SLIP) {
        out.flags |= kPlayerStateSharpTurn;
    }

    /* Standing, as setBlendMoveAnime itself asks the question (d_a_alink.cpp:7656) — the same two
     * tests in the same order, not an approximation of them. It selects a branch with a STEP in it:
     * on the moving side Link's walk weight starts at mMinWalkRate, 0.7, so this bit is the
     * difference between a puppet standing still and a puppet at seven tenths of a walk.
     *
     * A null alink sends it SET rather than clear. It means "no daAlink_c to ask", so the rate
     * above went out as 0.0, and 0.0 on the moving side of that step is the one combination that
     * cannot occur naturally — it would put a motionless puppet into a walk. The receiver has its
     * own guard for the same reason; see selectAnimation(). */
    if (alink == nullptr || alink->checkModeFlg(daAlink_c::MODE_IDLE) || alink->checkZeroSpeedF()) {
        out.flags |= kPlayerStateZeroSpeed;
    }

    /* MODE_IDLE on its own, for the foot IK — see kPlayerStateModeIdle for why it is not the OR
     * above. A null alink sends it clear: with no daAlink_c there is nothing standing. */
    if (alink != nullptr && alink->checkModeFlg(daAlink_c::MODE_IDLE)) {
        out.flags |= kPlayerStateModeIdle;
    }

    /* daAlink_c's own foot-IK gate, transcribed term for term from footBgCheck
     * (d_a_alink.cpp:3872) rather than summarised, so a reader can diff the two lines:
     *
     *   !ChkGroundHit() || magne boots || (ChkGroundHit() && sinking) || any of 0x78C52
     *
     * The middle term is redundant with the first (`!G || (G && S)` is `!G || S`) and is kept
     * anyway — the point of this line is that it matches the original, not that it is minimal.
     *
     * 0x78C52 is MODE_JUMP | MODE_CLIMB | MODE_NO_COLLISION | MODE_RIDING | MODE_UNK_800 |
     * MODE_UNK_8000 | MODE_VINE_CLIMB | MODE_ROPE_WALK | MODE_SWIMMING. Written as the literal the
     * game uses because two of those flags have no confirmed name, and inventing names for them in
     * a comparison that has to stay identical to the original is how the two drift apart. */
    if (alink == nullptr || !alink->mLinkAcch.ChkGroundHit() || alink->checkMagneBootsOn() ||
        (alink->mLinkAcch.ChkGroundHit() && alink->mSinkShapeOffset < 0.0f) ||
        alink->checkModeFlg(0x78C52))
    {
        out.flags |= kPlayerStateNoFootIk;
    }
    /* A wolf's foot IK is the four-legged wolfFootBgCheck (d_a_alink_wolf.inc:1968), which the
     * puppet does not port (puppet-wolf.md §7 Q3, deferred), so a wolf always says "no IK": his
     * feet follow the BCK and the slope pitch arrives via bodyX. Kept apart from the footBgCheck
     * transcription above so that line still diffs clean against the original. */
    if (alink != nullptr && alink->checkWolf()) {
        out.flags |= kPlayerStateNoFootIk;
    }

    /* Whether the world's clock is allowed to run where this player is standing — the vote that
     * decides the shared time of day under TimeAdvanceRule::AllPlayers (world_clock.hpp).
     *
     * The room's flag, from stage data (`d_stage.cpp:1511`), plus the two conditions that mean the
     * local environment is deliberately holding time still: a time-control tag region
     * (`d_a_kytag11.cpp:58`) and the twilight, which zeroes daytime outright
     * (`d_kankyo.cpp:1592-1599`).
     *
     * ★ Deliberately NOT the whole of setDaytime's condition. That also folds in "an event is
     * running" and "a message box is open" (`d_kankyo.cpp:1538-1545`) — transient, local, and
     * nobody else's business. Sending those would mean one player reading a signpost stops the sun
     * for everyone, which is a bug that would be very hard to recognise from the outside. */
    if (dComIfGp_roomControl_getTimePass() && g_env_light.using_time_control_tag == 0 &&
        !dKy_darkworld_check() && !test_time_blocked())
    {
        out.flags |= kPlayerStateTimeCanPass;
    }

    fill_idle_kind(alink, out);
    fill_equip(alink, out);
    fill_gear(alink, out);
    fill_atn_kind(alink, out);
    fill_upper_guard(alink, out);
    fill_joint_aim(alink, out);
    fill_action_anim(alink, out);
    fill_loc_phase(alink, out);
    // block (3): held item
    fill_held_item(alink, out);
    // v19 block B: iron ball. After the held item, whose kind it is gated on.
    fill_iron_ball(alink, out);
    // v16: location
    fill_location(link, out);
    // v19: attack
    fill_attack(alink, out);
    // v20: effects. Each feature ORs in only its own PlayerFx bits.
    out.fxFlags = 0;
    out.dmgFlash = 0;
    fill_damage_flash(alink, out);  // fxFlags bits 4-5 + dmgFlash (agent D)
    fill_lantern_fx(alink, out);    // fxFlags bit 0, after fill_held_item (agent D)
    fill_sword_fx(alink, out); // sword trail + spin/charge fx (agent F): bits 1-3, 6-7
    fill_clawshot(alink, out); // v20 clawshot block, gated on the held item's kind
    fill_form(alink, out);     // v21 form block (wolf piece A)

    return true;
}

void refresh_held_form(PlayerState& held) {
    /* Only for the Link the held pose came from: a new scene's Link (or none) is not his, and
     * wire_local_state sends that pose as not in the world anyway. */
    if (!local_player_is_last_captured()) {
        return;
    }
    daAlink_c* alink = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (alink == nullptr) {
        return;
    }
    std::int8_t tint = 0;
    const std::uint8_t form = local_form_scalars(alink, tint);
    const bool phase = (form & kPlayerFormMetaPhaseMask) != 0;
    /* The fur frame needs mItemBck, which is off limits here. While the fur is still his held model
     * the last live frame is carried; it is near the end of the first half's BCK, and during the
     * swap itself he draws only the blob, so nothing shows it. */
    const bool keepFur = phase && (form & kPlayerFormFur) != 0 && (held.form & kPlayerFormFur) != 0;
    held.furFrame = keepFur ? held.furFrame : 0;
    held.form = form;
    held.metaTint = phase ? tint : 0;
    note_local_form(alink, form, held.metaTint, "held");
}

RemoteLocation remote_location(const PlayerState& pose) {
    if (pose.stageHash == 0 || dComIfGp_getPlayer(0) == nullptr) {
        // Nothing reported, or no scene here to compare against. The in-world flag and
        // world_is_playable() already decide those cases; answering anything else would add a
        // second, disagreeing gate.
        return RemoteLocation::Here;
    }
    if (pose.stageHash != location_stage_hash(dComIfGp_getStartStageName())) {
        return RemoteLocation::OtherStage;
    }
    const int room = pose.roomNo;
    /* The game's own "is this room drawn" test, the one the doors use for their own rooms
     * (d_a_door_shutter.cpp:1439). Flag 8 hides a loaded room, flag 16 marks it displayed
     * (d_stage.cpp:295-305). Our own room always counts, and an out-of-range number is "don't
     * know": mStatus has 64 entries and checkStatusFlag only asserts the bound. */
    if (room < 0 || room >= 64 || room == dComIfGp_roomControl_getStayNo()) {
        return RemoteLocation::Here;
    }
    return dComIfGp_roomControl_checkRoomDisp(room) ? RemoteLocation::Here :
                                                      RemoteLocation::RoomNotShown;
}

bool ensure_puppet(std::uint32_t playerId, std::uint32_t colorRgb, std::uint8_t outfit, bool wolf) {
    // Inserted on first sight rather than on first successful spawn: the failure history has to
    // outlive the process id it belongs to, or backing off would be impossible.
    PuppetRef& ref = s_puppets[playerId];

    /* Re-arm on any change of scene. Sampled every tick, so a change is seen on the tick it
     * happens and the record never goes stale.
     *
     * This is the only automatic recovery, and deliberately so. The realistic transient cause is
     * the room being at peak heap use when we asked, and that is fixed by being somewhere else; a
     * plain timer-based re-arm would fix nothing and would simply turn the flood into a slower
     * flood. The other two recoveries are events rather than conditions: a peer that leaves and
     * rejoins is a new player id and therefore a new entry, and the render toggle being cycled
     * erases the entry through destroy_puppet().
     *
     * ★ Known, accepted imprecision. A room unload that cancels a create request mid-flight looks
     * exactly like a real failure — the framework cancels the request either way — and because the
     * location is compared BEFORE that is noticed, it costs one strike and one kRetryBaseTicks
     * wait rather than being forgiven. That is deliberate: it is a narrow window (only while the
     * archive is mounting), the strike is wiped the moment the puppet does come up, and the old
     * code was worse in the same case — it sat out the full 120-tick grace before retrying. Do not
     * "fix" it by re-ordering these blocks without checking what that does to a stale location on
     * the ticks that return early below. */
    Location here;
    if (current_location(here)) {
        if (ref.locationValid && !same_location(here, ref.location) &&
            (ref.disabled || ref.consecutiveFailures > 0))
        {
            Log.info("Player {}'s puppet: now in stage '{}' room {}, re-arming after {} creation "
                     "failure(s){}",
                playerId, static_cast<const char*>(here.stage), here.roomNo,
                ref.consecutiveFailures, ref.disabled ? " and a give-up" : "");
            ref.consecutiveFailures = 0;
            ref.retryCooldown = 0;
            ref.failureWindowTicks = 0;
            ref.disabled = false;
            ref.lastReason = nullptr;
        }
        ref.location = here;
        ref.locationValid = true;
    }

    if (ref.consecutiveFailures > 0) {
        ref.failureWindowTicks++;
    }

    if (ref.id != fpcM_ERROR_PROCESS_ID_e) {
        ref.ticksSinceRequest++;

        daRemotePlayer_c* puppet = nullptr;
        switch (look_up_puppet(playerId, &puppet)) {
        case PuppetLookup::Alive:
            // It came up, so creation demonstrably works where we are standing. Forget the history
            // rather than carrying it into the next room.
            ref.consecutiveFailures = 0;
            ref.failureWindowTicks = 0;
            ref.lastReason = nullptr;
            return true;

        case PuppetLookup::Creating:
            // Expected, and for as many ticks as the private archive mount needs. Say so once if it
            // starts to look like never, but do NOT respawn: the old request is still live and
            // cannot be cancelled through fopAcM_delete (which resolves the id to an actor, and a
            // still-creating process is not in the actor layer yet), so a second create here would
            // give this player two puppets rather than one.
            if (ref.ticksSinceRequest >= kCreateStuckTicks && !ref.loggedStuck) {
                ref.loggedStuck = true;
                Log.warn("Puppet for player {} has been in the actor framework's create phase for "
                         "{} tick(s) — its archive mount is not completing. Still waiting; nothing "
                         "here will retry it.",
                    playerId, ref.ticksSinceRequest);
            }
            return true;

        case PuppetLookup::Gone:
            break;
        }

        if (ref.everResolved) {
            // It was alive and is not any more: a room unload or a stage change took it. Creation
            // is known to work here, so this respawns at once, exactly as it always did.
            //
            // ★ This is NOT a creation failure and must never be counted as one. Conflating the
            // two is the entire bug: it made a permanent failure retry at full speed forever.
            Log.info("Puppet for player {} vanished; respawning", playerId);
            ref.id = fpcM_ERROR_PROCESS_ID_e;
            ref.everResolved = false;
            ref.ticksSinceRequest = 0;
            ref.loggedStuck = false;
        } else {
            // It never resolved, so the framework cancelled the create request. It does that for
            // cPhs_ERROR_e (f_pc_create_req.cpp:110-117) and when the layer the request was queued
            // in is torn down; the second is re-armed above the moment the room number changes.
            note_create_failure(playerId, ref, take_failure_reason(playerId));
        }
    }

    if (ref.disabled) {
        return false;
    }
    if (ref.retryCooldown > 0) {
        /* ★ The whole point of the exercise is this early return, not the quieter log. Everything
         * expensive about a failed spawn is inside create(): mountOwnArchive() starts a private
         * dRes_info_c mount of the outfit archive and only then does entrySolidHeap() get a chance
         * to fail. Returning here means fopAcM_create() is never called, so none of that runs. */
        ref.retryCooldown--;
        return false;
    }

    if (!world_is_playable()) {
        // No playable scene. Spawning now would put the puppet in the title demo or in a room
        // that is about to be unloaded.
        return false;
    }

    fopAc_ac_c* link = dComIfGp_getPlayer(0);

    // Spawn on top of the local player: the first network pose arrives within a tick or two and
    // moves it, and starting at the origin would show a puppet sliding in from the map corner.
    const cXyz pos = link->current.pos;
    const csXyz angle(0, link->shape_angle.y, 0);

    // The outfit rides in the top byte of the create parameter. The actor needs it at its first
    // create() entry, before the multi-phase archive mount starts, and this is the only channel
    // that exists that early — see the packing note in d_a_remote_player.h.
    //
    // v21: a wolf sender gets kPuppetParamWolf (0xFE) in that byte instead, which mounts Wmdl in
    // place of the outfit archive (puppet-wolf.md §6). His `outfit` is not lost: the next pose
    // still carries it, and the rebuild when he turns back reads it from there.
    const std::uint8_t topByte = wolf ? kPuppetParamWolf : outfit;
    const std::uint32_t param =
        (static_cast<std::uint32_t>(topByte) << daRemotePlayer_outfitParamShift) |
        (playerId & daRemotePlayer_playerIdMask);

    const fpc_ProcID id = fopAcM_create(
        fpcNm_REMOTE_PLAYER_e, param, &pos, fopAcM_GetRoomNo(link), &angle, nullptr, 0xFF);

    if (id == fpcM_ERROR_PROCESS_ID_e) {
        // The actor manager refused outright — no process, so create() never runs and there is no
        // reason to collect from it. Counted as a creation failure like any other: this path used
        // to warn once per tick with no backoff of its own either.
        note_create_failure(
            playerId, ref, "the actor manager refused to create the process at all");
        return false;
    }

    ref.id = id;
    ref.everResolved = false;
    ref.ticksSinceRequest = 0;
    ref.loggedStuck = false;
    Log.info("Spawned puppet for player {} (#{:06X}) as process {}{}", playerId, colorRgb, id,
        wolf ? " (wolf, param top byte 0xFE)" : "");
    return true;
}

void apply_puppet_state(std::uint32_t playerId, const PlayerState& state) {
    daRemotePlayer_c* puppet = resolve_puppet(playerId);
    if (puppet == nullptr) {
        return;
    }

    cXyz pos(state.posX, state.posY, state.posZ);
    /* The equipment byte goes over WHOLE rather than being decoded into six more arguments here.
     * Its layout lives in dusk/player_equip.hpp precisely so both ends can read it without this
     * function becoming the place that knows which bit pattern means the master sword. */
    puppet->setNetworkPose(pos, state.angleY, state.moveRate, state.sharp_turn(),
        state.zero_speed(), state.mode_idle(), state.no_foot_ik(), state.equip, state.idleKind);
    puppet->setNetworkAtn(state.atnKind);
    puppet->setNetworkUpperGuard(state.upper_guard());
    puppet->setNetworkHeavyBoots(state.heavy_boots());
    puppet->setNetworkHeavyGait(state.heavy_gait());
    puppet->setNetworkGearFx((state.gear & kPlayerGearFxReported) != 0,
        (state.gear & kPlayerGearMagicArmorPowered) != 0, (state.gear & kPlayerGearZoraMask) != 0,
        (state.gear & kPlayerGearShieldBurning) != 0);  // gear bits 3-6 (agent G)
    // v19 block B: iron ball. The carry gait and where the ball is, decoded here.
    puppet->setNetworkIronBall(state.iron_ball_gait(),
        cXyz(state.iron_ball_x(), state.iron_ball_y(), state.iron_ball_z()), state.ironBallAngX,
        state.ironBallAngY, state.ironBallLinks, state.ironBallState);
    puppet->setNetworkDamageLantern((state.fxFlags & kPlayerFxDmgTintMask) >> kPlayerFxDmgTintShift,
        state.dmgFlash, (state.fxFlags & kPlayerFxLanternLit) != 0);  // v20 (agent D)
    apply_sword_fx(puppet, state); // v20 fxFlags bits 1-3, 6-7: sword trail + spin/charge (agent F)
    apply_clawshot(puppet, state); // v20 clawshot block
    puppet->setNetworkJointAim(
        state.neckX, state.neckY, state.bodyX, state.bodyTwist, state.bodyZ, state.bankZ);
    // Block (2), decoded from the wire's fixed point here so the actor never sees the wire layout.
    const cXyz rootTrans(state.act_root_x(), state.act_root_y(), state.act_root_z());
    puppet->setNetworkAction(state.actUnder, state.actUpper, state.act_frame(),
        state.act_upper_frame(), state.act_rate(), static_cast<f32>(state.actMorf), state.actHandL,
        state.actHandR, rootTrans);
    // v15 block (1). Meaningless while the override is on, and the puppet is told so.
    puppet->setNetworkLocPhase(state.locPhase, !state.act_active());
    // Block (4): slot 1, the joint-0 rotation and the yaw offset. Same decoding rule.
    puppet->setNetworkActionSlot1(state.act1Under, state.act1Upper, state.act1_phase(),
        state.act1_ratio(), state.actRootRotX, state.actRootRotZ, state.actYawOff, state.actPitch);
    // block (3): held item
    puppet->setNetworkHeldItem(state.heldItem, state.heldFlags, state.heldMat, state.upper2Idx,
        state.upper2_frame(), state.upper2_ratio(), static_cast<f32>(state.upper2Morf));
    // v19: the sword hitbox, back in world space around the position the puppet is drawn at.
    cXyz atPoints[3];
    for (int p = 0; p < 3; p++) {
        atPoints[p].set(pos.x + static_cast<f32>(state.atPt[p * 3 + 0]),
            pos.y + static_cast<f32>(state.atPt[p * 3 + 1]),
            pos.z + static_cast<f32>(state.atPt[p * 3 + 2]));
    }
    puppet->setNetworkAttack(state.atMask, static_cast<f32>(state.atRadius), state.atSpl, atPoints);
    // v19 block A: bow
    puppet->setNetworkHeldItemBck(state.heldBck, state.heldBckFrame);
    // v21 form block, whole: the wolf body (piece B) and the transform visuals (piece C) decode it.
    // Whether the puppet IS a wolf is not pushed here: that is the build, see
    // reconcile_puppet_outfit.
    puppet->setNetworkForm(state.form, state.metaTint, state.furFrame);
}

bool read_puppet_pose(std::uint32_t playerId, PlayerState& out) {
    daRemotePlayer_c* puppet = resolve_puppet(playerId);
    if (puppet == nullptr) {
        return false;
    }

    out.posX = puppet->current.pos.x;
    out.posY = puppet->current.pos.y;
    out.posZ = puppet->current.pos.z;
    out.angleY = puppet->shape_angle.y;
    out.moveRate = puppet->getNetMoveRate();
    // The outfit the puppet actually mounted, not the byte we last received, for the same reason
    // the rest of this function reads the actor rather than the network: this is the "did the
    // instruction land?" side of the comparison.
    out.outfit = static_cast<std::uint8_t>(puppet->getOutfit());
    out.flags = puppet->hasPose() ? kPlayerStateInWorld : 0;
    // Read back off the puppet rather than remembered here, for the same reason as everything else
    // in this function: this is the "did the instruction land?" side of the comparison.
    if (puppet->getNetZeroSpeed()) {
        out.flags |= kPlayerStateZeroSpeed;
    }
    // Read back off the puppet for the same reason, which makes it the both-ends check a wire
    // change is required to pass: the trace can compare the byte the sender packed against the byte
    // the receiver is actually drawing from.
    out.equip = puppet->getNetEquip();
    // Same again, and this one is the reason the trace has an idleKind column at all: "the sender
    // chose the fidget idle" and "the puppet is playing the fidget idle" are two different claims,
    // and only the second one is about the feature working.
    out.idleKind = puppet->getNetIdleKind();
    out.atnKind = puppet->getNetAtnKind();
    if (puppet->getNetUpperGuard()) {
        out.flags |= kPlayerStateUpperGuard;
    }
    // Read back off the puppet, so the trace's `gear` column is the both-ends check for the bit.
    out.gear = (puppet->getNetHeavyBoots() ? kPlayerGearHeavyBoots : 0) |
               (puppet->getNetHeavyGait() ? kPlayerGearHeavyGait : 0);
    if (puppet->getNetIronBallGait()) {
        out.gear |= kPlayerGearIronBallGait;
    }
    // Bits 3-6 as the puppet is DRAWING them, not as received (agent G).
    out.gear |= (puppet->getGearFxReported() ? kPlayerGearFxReported : 0) |
                (puppet->getMagicArmorPoweredDrawn() ? kPlayerGearMagicArmorPowered : 0) |
                (puppet->getZoraMaskDrawn() ? kPlayerGearZoraMask : 0) |
                (puppet->getShieldBurnDrawn() ? kPlayerGearShieldBurning : 0);
    // The joint aim values the puppet's callbacks are drawing from this tick, read back off it.
    out.neckX = puppet->getNetNeckX();
    out.neckY = puppet->getNetNeckY();
    out.bodyX = puppet->getNetBodyX();
    out.bodyTwist = puppet->getNetBodyTwist();
    out.bodyZ = puppet->getNetBodyZ();
    out.bankZ = puppet->shape_angle.z;
    /* Block (2): what the puppet DRAWS, not what it received — the heap idx installed in its packs,
     * the frame on the animation object itself, the hand shapes it is showing and joint 0 as its
     * calc left it. The local row carries the same columns for the sender, so the two compare. */
    out.actUnder = puppet->getActDrawnUnder();
    out.actUpper = puppet->getActDrawnUpper();
    out.actFrame = act_encode_u16(puppet->getActDrawnFrame(), kPlayerActFrameScale);
    out.actUpperFrame = act_encode_u16(puppet->getActDrawnUpperFrame(), kPlayerActFrameScale);
    out.actRate = act_encode_s8(puppet->getActDrawnRate(), kPlayerActRateScale);
    const f32 morf = puppet->getActDrawnMorf();
    out.actMorf =
        static_cast<std::uint8_t>(morf > 0.0f ? (morf > 255.0f ? 255.0f : morf + 0.5f) : 0.0f);
    out.actHandL = puppet->getDrawnHand(0);
    out.actHandR = puppet->getDrawnHand(1);
    cXyz rootLocal;
    cXyz rootWorld;
    if (puppet->getDrawnRoot(&rootLocal, &rootWorld)) {
        out.actRootX = act_encode_s16(rootLocal.x, kPlayerActRootScale);
        out.actRootY = act_encode_s16(rootLocal.y, kPlayerActRootScale);
        out.actRootZ = act_encode_s16(rootLocal.z, kPlayerActRootScale);
    }
    // The stride phase the puppet DRAWS, on the wire's scale. The trace's locPhase column reads the
    // unquantised value through read_puppet_loc_phase instead.
    const f32 drawnPhase = puppet->getDrawnLocPhase();
    out.locPhase = drawnPhase >= 0.0f ? static_cast<std::uint8_t>(
                                            static_cast<int>(drawnPhase * 256.0f + 0.5f) & 0xFF) :
                                        0;
    /* Block (4), drawn: the slot-1 animation installed in the packs, its object's own frame, the
     * ratio on the pack, and the rotation / yaw the callback and setMatrix actually applied. */
    out.act1Under = puppet->getActDrawnUnder1();
    out.act1Upper = puppet->getActDrawnUpper1();
    out.act1Phase = act_encode_unit(puppet->getActDrawnPhase1());
    out.act1Ratio = act_encode_unit(puppet->getActDrawnRatio1());
    out.actRootRotX = puppet->getActDrawnRootRotX();
    out.actRootRotZ = puppet->getActDrawnRootRotZ();
    out.actYawOff = puppet->getActDrawnYawOff();
    out.actPitch = puppet->getActDrawnPitch();
    /* block (3): held item — what the puppet DREW last pass (its own models, which may be absent
     * if one failed to load), and the overlay installed in its upper slot 2 with that animation
     * object's own frame and the pack's ratio. Not the wire values. */
    out.heldItem = puppet->getHeldDrawnKind();
    out.heldFlags = puppet->getHeldDrawnFlags();
    out.heldMat = puppet->getHeldDrawnMat();
    out.upper2Idx = puppet->getUpper2DrawnIdx();
    out.upper2Frame = act_encode_u16(puppet->getUpper2DrawnFrame(), kPlayerActFrameScale);
    {
        const f32 ratio = puppet->getUpper2DrawnRatio();
        out.upper2Ratio = static_cast<std::uint8_t>(
            ratio <= 0.0f ? 0.0f : (ratio >= 1.0f ? 255.0f : ratio * 255.0f + 0.5f));
    }
    out.upper2Morf = puppet->getUpper2DrawnMorf();
    // v19 block A: bow — the item BCK the puppet installed and its frame, and the arrow it drew.
    out.heldBck =
        static_cast<std::uint8_t>((puppet->getHeldItemBckShown() & kPlayerHeldBckIdMask) |
                                  (puppet->getHeldArrowDrawn() == 1 ? kPlayerHeldBckBombArrow : 0));
    out.heldBckFrame = puppet->getHeldItemBckDrawnFrame();
    puppet->getBottleDrawnWire(out.heldBck, out.heldBckFrame); // bottle contents (agent H)
    return true;
}

bool read_local_loc_phase(float& out) {
    out = -1.0f;
    daAlink_c* alink = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (alink == nullptr || !local_pose_is_live()) {
        return false;
    }
    out = local_loc_phase(alink);
    return out >= 0.0f;
}

bool read_puppet_loc_phase(std::uint32_t playerId, float& out) {
    out = -1.0f;
    daRemotePlayer_c* puppet = resolve_puppet(playerId);
    if (puppet == nullptr) {
        return false;
    }
    out = puppet->getDrawnLocPhase();
    return out >= 0.0f;
}

bool read_local_joint_aim(DrawnJointAim& out) {
    out = DrawnJointAim{};
    out.clamps = s_jointAimClamps;
    daAlink_c* alink = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (alink == nullptr || !local_pose_is_live()) {
        return false;
    }
    measure_joint_aim(alink->mpLinkModel, alink->shape_angle.y, out);
    return true;
}

bool read_puppet_joint_aim(std::uint32_t playerId, DrawnJointAim& out) {
    out = DrawnJointAim{};
    daRemotePlayer_c* puppet = resolve_puppet(playerId);
    if (puppet == nullptr) {
        return false;
    }
    measure_joint_aim(puppet->model, puppet->shape_angle.y, out);
    for (int i = 0; i < 3; i++) {
        out.armFires[i] = puppet->getJointAimFires(i);
    }
    return true;
}

bool read_local_anim(std::uint16_t& out) {
    out = 0;
    const daAlink_c* alink = static_cast<const daAlink_c*>(dComIfGp_getLinkPlayer());
    if (alink == nullptr || !local_pose_is_live() || !alink->mUnderAnmHeap[0].checkNoSetArcNo()) {
        return false;
    }
    out = alink->mUnderAnmHeap[0].getIdx();
    return true;
}

bool read_local_root(float& x, float& y, float& z) {
    x = y = z = 0.0f;
    const daAlink_c* alink = static_cast<const daAlink_c*>(dComIfGp_getLinkPlayer());
    if (alink == nullptr || !local_pose_is_live() || alink->mpLinkModel == nullptr) {
        return false;
    }
    MtxP root = alink->mpLinkModel->getAnmMtx(0);
    x = root[0][3];
    y = root[1][3];
    z = root[2][3];
    return true;
}

bool read_puppet_root(std::uint32_t playerId, float& x, float& y, float& z) {
    x = y = z = 0.0f;
    daRemotePlayer_c* puppet = resolve_puppet(playerId);
    if (puppet == nullptr) {
        return false;
    }
    cXyz local;
    cXyz world;
    if (!puppet->getDrawnRoot(&local, &world)) {
        return false;
    }
    x = world.x;
    y = world.y;
    z = world.z;
    return true;
}

bool read_puppet_anim(std::uint32_t playerId, std::uint16_t& out) {
    daRemotePlayer_c* puppet = resolve_puppet(playerId);
    if (puppet == nullptr) {
        return false;
    }
    out = puppet->getCurrentAnm();
    return true;
}

void destroy_puppet(std::uint32_t playerId) {
    const auto it = s_puppets.find(playerId);
    if (it == s_puppets.end()) {
        return;
    }
    // The fpc_ProcID overload copes with the actor already being gone, which it will be whenever a
    // room unload beat us to it. The id check is for the other case an entry can now be in: known
    // player, no puppet, because we are backing off a failed creation.
    if (it->second.id != fpcM_ERROR_PROCESS_ID_e) {
        fopAcM_delete(it->second.id);
    }
    // Erasing the whole entry drops the failure history with it, which is the intended re-arm for a
    // peer that disconnects and rejoins: a fresh connection deserves a fresh set of attempts.
    s_puppets.erase(it);
}

/**
 * Make a mid-session clothes change reach the puppet — by respawning it.
 *
 * ★ This is the deliberate trade-off, and it is not merely the easy option: an in-place re-mount is
 * not available. Everything the puppet is made of — the four J3DModels, the McaMorfSO, the material
 * anms — is allocated inside the actor's own JKRSolidHeap (fopAcM_entrySolidHeap, 0x20000). A solid
 * heap cannot free an individual allocation; the only way to reclaim it is to destroy the heap, and
 * destroying an actor's heap is exactly what fopAcM_delete does. So "swap the archive under a live
 * actor" would mean re-running the whole create phase in place, on machinery designed to run once,
 * for no gain over letting the actor die and be rebuilt by the path that already handles a room
 * unload taking a puppet away.
 *
 * What it costs is honest to state: the puppet is ABSENT for the tick it takes to notice plus the
 * frames the DVD mount of the new archive takes, and it loses its accumulated local presentation
 * state (cap and hair sway, blink phase, eye smoothing). None of that is replicated, all of it
 * re-converges within a second, and the wearer is standing in a pause menu while it happens.
 *
 * Two rules keep it from turning into a respawn storm:
 *  - The comparison is between RESOLVED indices, not raw bytes. If the actor clamped a byte it did
 *    not recognise to the hero's clothes and we compared against the raw byte, every single tick
 *    would read as "still wearing the wrong thing" and rebuild the actor forever.
 *  - A cooldown, measured on the puppet's own age (ticksSinceRequest is zeroed at create and
 *    stepped once per tick), so even a genuinely flapping outfit costs one respawn per second at
 *    worst. It defers a change, never drops one: the sender keeps reporting the new outfit, so the
 *    mismatch is still there when the cooldown expires.
 */
bool reconcile_puppet_outfit(std::uint32_t playerId, std::uint8_t outfit, bool wolf) {
    const auto it = s_puppets.find(playerId);
    if (it == s_puppets.end()) {
        return false;
    }

    daRemotePlayer_c* puppet = resolve_puppet(playerId);
    if (puppet == nullptr) {
        // Still mounting its archive, or already destroyed by a room unload. In the first case the
        // outfit it will wear is already latched and tearing down a half-created actor is the one
        // thing worth not doing; in the second, ensure_puppet() is about to rebuild it anyway.
        return false;
    }

    /* v21: the pair (form, outfit). A wolf puppet's outfit is whatever 0xFE resolved to and means
     * nothing, so only the form is compared for a wolf; a human compares the clothes as before.
     * The same cooldown applies: it is shorter than a transformation (puppet-wolf.md §3), so it
     * never delays one, and it still bounds a flapping bit to one rebuild per second. */
    const bool builtWolf = puppet->isWolf();
    const int wanted = daRemotePlayer_outfitFromWire(outfit);
    if (builtWolf == wolf && (wolf || puppet->getOutfit() == wanted)) {
        return false;
    }
    if (it->second.ticksSinceRequest < kOutfitRespawnCooldownTicks) {
        return false;
    }

    if (builtWolf != wolf) {
        Log.info("Player {} changed form ({} -> {}); rebuilding their puppet", playerId,
            builtWolf ? "wolf" : "human", wolf ? "wolf" : "human");
    } else {
        Log.info("Player {} changed outfit ({} -> {}); rebuilding their puppet", playerId,
            puppet->getOutfit(), wanted);
    }
    destroy_puppet(playerId);
    return true;
}

void destroy_all_puppets() {
    for (const auto& entry : s_puppets) {
        if (entry.second.id != fpcM_ERROR_PROCESS_ID_e) {
            fopAcM_delete(entry.second.id);
        }
    }
    s_puppets.clear();
}

}  // namespace dusk::mp
