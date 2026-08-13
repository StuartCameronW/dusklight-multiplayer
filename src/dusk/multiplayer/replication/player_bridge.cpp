#include "player_bridge.hpp"

#include "world_clock.hpp"

#include <cmath>
#include <cstring>
#include <unordered_map>

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_remote_player.h"
#include "d/d_com_inf_game.h"
#include "dusk/logging.h"
#include "f_op/f_op_actor_mng.h"

/**
 * The one translation unit allowed to include engine headers (see player_bridge.hpp).
 *
 * Everything here has to tolerate being called when there is no scene, no Link and no puppet: the
 * tick hooks fire during loads, and are skipped entirely during DVD-error and shutdown states, so
 * this code sees discontinuities rather than a tidy lifecycle.
 */

namespace dusk::mp {
namespace {

aurora::Module Log{"dusk::mp"};

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
    // 0x103 is dItemNo_SWORD_e; written as the literal daAlink_c compares against at :5894 and
    // :4386 so the two read identically.
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

bool capture_local_player(PlayerState& out) {
    if (!world_is_playable()) {
        // Nothing worth sending: no scene, or the pose belongs to the attract demo or a cutscene
        // rather than to the player. See world_is_playable().
        return false;
    }

    fopAc_ac_c* link = dComIfGp_getPlayer(0);
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

    return true;
}

bool ensure_puppet(std::uint32_t playerId, std::uint32_t colorRgb, std::uint8_t outfit) {
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
    const std::uint32_t param =
        (static_cast<std::uint32_t>(outfit) << daRemotePlayer_outfitParamShift) |
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
    Log.info("Spawned puppet for player {} (#{:06X}) as process {}", playerId, colorRgb, id);
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
bool reconcile_puppet_outfit(std::uint32_t playerId, std::uint8_t outfit) {
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

    const int wanted = daRemotePlayer_outfitFromWire(outfit);
    if (puppet->getOutfit() == wanted) {
        return false;
    }
    if (it->second.ticksSinceRequest < kOutfitRespawnCooldownTicks) {
        return false;
    }

    Log.info("Player {} changed outfit ({} -> {}); rebuilding their puppet", playerId,
        puppet->getOutfit(), wanted);
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
