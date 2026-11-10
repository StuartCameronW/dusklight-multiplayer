#include "object_capture.hpp"

#include <cstdint>

#include "../session/network_manager.hpp"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_crod.h"
#include "d/actor/d_a_cstatue.h"
#include "d/actor/d_a_mp_ghost.h"
#include "d/d_com_inf_game.h"
#include "dusk/logging.h"
#include "f_op/f_op_actor_mng.h"
#include "object_channel.hpp"
#include "player_bridge.hpp"

/**
 * Owner-side capture for the Dominion Rod ball (ObjKind::RodBall, spec
 * .claude/future-plans/mp-object-channel.md §2.5, phase 1 row P1d).
 *
 * The ball (daCrod_c, fpcNm_CROD_e) is ONE actor per equip, not one per throw: it sits on the rod
 * (param 0 idle / 1 aim), is thrown (2, then 3 fly the same tick it executes), may possess a
 * statue (4), and comes back (5) until Link catches it (d_a_crod.cpp:187-356). While it is away
 * from the rod Link holds it in mCopyRodAcKeep (throwCopyRod / returnCopyRod,
 * d_a_alink_copyrod.inc:149-195), and only a powered rod is thrown at all (checkCopyRodTopUse).
 *
 * So one replicated transient is one FLIGHT: spawned when the ball is seen flying (3) or
 * returning (5) with no live flight, streamed while it flies, and ended when it is caught,
 * possesses a statue, or vanishes. Possession ends the flight in phase 1 (the receiver's statue is
 * not driven until phase 2b); the return flight after the statue lets go is a new transient.
 *
 * A flight that ends sends its event at once, with the pose hidden, and its despawn
 * kCrodDespawnDelay ticks later: the receiver applies a despawn BEFORE the events of the same
 * played tick (object_channel.cpp receiver_tick), so an event sent on the despawn tick would never
 * reach the ghost.
 *
 * The blob layouts are daMpGhost_c's CROD_* constants (include/d/actor/d_a_mp_ghost.h), the one
 * definition both ends read.
 */

namespace dusk::mp::objects::capture {
namespace {

constexpr borealis::Log Log{"dusk::mp"};

/// daCrod_c params (d_a_crod.cpp:187-356). 6 is the iron ball's dummy, never a rod ball.
constexpr u32 crod_kParamIdle = 0;
constexpr u32 crod_kParamAim = 1;
constexpr u32 crod_kParamThrow = 2;
constexpr u32 crod_kParamFly = daMpGhost_c::CROD_PHASE_FLY;
constexpr u32 crod_kParamControl = 4;
constexpr u32 crod_kParamReturn = daMpGhost_c::CROD_PHASE_RETURN;
constexpr u32 crod_kParamIronBall = 6;

/// Ticks between a flight's ending event and its despawn. More than one, because the receiver's
/// playback clock can step two ticks at once while it catches up.
constexpr std::uint64_t kCrodDespawnDelay = 3;

struct CrodFlight {
    bool live = false;
    ObjRef ref;
    fpc_ProcID proc = fpcM_ERROR_PROCESS_ID_e;
    u32 lastParam = 0;
    ObjPose lastPose;
    KindBlob lastBlob;
    std::uint64_t spawnTick = 0;
};

struct CrodPending {
    bool live = false;
    ObjRef ref;
    ObjDespawnReason reason = ObjDespawnReason::Gone;
    std::uint64_t despawnAt = 0;
};

struct CrodState {
    CrodFlight flight;
    CrodPending pending;
    /// The ball proc whose spawn was last refused, so a refusal is logged once, not every tick.
    fpc_ProcID refusedProc = fpcM_ERROR_PROCESS_ID_e;
};

CrodState& crod_state() {
    static CrodState s;
    return s;
}

std::uint64_t crod_now() {
    return network_manager().sim_tick();
}

/// The local Link's ball while it is away from the rod, or NULL. By id rather than through the
/// keep's cached pointer, which is only refreshed when Link executes.
daCrod_c* crod_away_ball() {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == NULL) {
        return NULL;
    }
    const fpc_ProcID id = link->mCopyRodAcKeep.getID();
    if (id == fpcM_ERROR_PROCESS_ID_e) {
        return NULL;
    }
    fopAc_ac_c* actor = fopAcM_SearchByID(id);
    if (actor == NULL || fopAcM_GetName(actor) != fpcNm_CROD_e ||
        fopAcM_GetParam(actor) == crod_kParamIronBall)
    {
        return NULL;
    }
    return static_cast<daCrod_c*>(actor);
}

/// "Alink" resource index of the BCK the ball is playing, or 0 when it is none of the six.
u8 crod_bck_id(daCrod_c* ball) {
    J3DAnmTransform* playing = ball->mBck.getBckAnm();
    if (playing == NULL) {
        return 0;
    }
    const char* arc = daAlink_c::getAlinkArcName();
    for (int id = daMpGhost_c::CROD_BCK_FIRST; id <= daMpGhost_c::CROD_BCK_LAST; id++) {
        if (dComIfG_getObjectRes(arc, id) == playing) {
            return static_cast<u8>(id);
        }
    }
    return 0;
}

/// This tick's pose and state blob. Hidden exactly when daCrod_c::draw would not draw it
/// (d_a_crod.cpp:341-345), plus whenever the caller says so.
void crod_capture(daCrod_c* ball, u32 param, bool hide, ObjPose& pose, KindBlob& blob) {
    pose.pos[0] = ball->current.pos.x;
    pose.pos[1] = ball->current.pos.y;
    pose.pos[2] = ball->current.pos.z;
    // daCrod_c::setMatrix draws with shape_angle (d_a_crod.cpp:131-135).
    pose.angle[0] = ball->shape_angle.x;
    pose.angle[1] = ball->shape_angle.y;
    pose.angle[2] = ball->shape_angle.z;
    pose.flags = 0;
    daAlink_c* link = daAlink_getAlinkActorClass();
    const bool drawn = link != NULL && (link->checkCopyRodTopUse() || ball->field_0x732 != 0);
    if (hide || !drawn) {
        pose.flags |= kObjPoseHidden;
    }

    Writer w;
    w.write_u8(static_cast<std::uint8_t>(param));
    w.write_u8(crod_bck_id(ball));
    blob.assign(w);
}

KindBlob crod_proc_blob(fopAc_ac_c* target, bool withBoss) {
    const u16 proc = target != NULL ? static_cast<u16>(fopAcM_GetName(target)) : 0;
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(proc >> 8));
    w.write_u8(static_cast<std::uint8_t>(proc & 0xFF));
    if (withBoss) {
        const bool boss = target != NULL && fopAcM_GetName(target) == fpcNm_CSTATUE_e &&
                          static_cast<daCstatue_c*>(target)->checkBossType();
        w.write_u8(boss ? 1 : 0);
    }
    KindBlob blob;
    blob.assign(w);
    return blob;
}

/// End the live flight: its last pose goes out hidden this tick, the despawn kCrodDespawnDelay
/// ticks later. Any earlier pending despawn is flushed first (one pending slot).
void crod_end_flight(CrodState& s, ObjDespawnReason reason, const char* why) {
    CrodFlight& f = s.flight;
    if (s.pending.live) {
        despawn_owned(s.pending.ref, s.pending.reason);
        s.pending.live = false;
    }
    ObjPose hidden = f.lastPose;
    hidden.flags |= kObjPoseHidden;
    update_owned(f.ref, hidden, f.lastBlob);
    Log.info("[mp-obj] rodball end {} why={} lastParam={} tick={} flewTicks={}", f.ref.str(), why,
        f.lastParam, crod_now(), crod_now() - f.spawnTick);
    s.pending.live = true;
    s.pending.ref = f.ref;
    s.pending.reason = reason;
    s.pending.despawnAt = crod_now() + kCrodDespawnDelay;
    f = CrodFlight();
}

}  // namespace

void tick_rodball() {
    CrodState& s = crod_state();
    const std::uint64_t now = crod_now();

    /* A queued scene change destroys the ball with every other actor. Forgotten silently: the
     * channel drops its owned objects the same tick (object_channel.cpp owner_tick) and the far end
     * despawns them on our epoch advancing. Nothing new may spawn until the new scene's Link. */
    if (local_scene_change_queued()) {
        if (s.flight.live || s.pending.live) {
            Log.info("[mp-obj] rodball forget {} (scene change queued) tick={}",
                s.flight.live ? s.flight.ref.str() : s.pending.ref.str(), now);
        }
        s = CrodState();
        return;
    }

    if (s.pending.live && now >= s.pending.despawnAt) {
        despawn_owned(s.pending.ref, s.pending.reason);
        s.pending.live = false;
    }

    daCrod_c* ball = crod_away_ball();
    const u32 param = ball != NULL ? fopAcM_GetParam(ball) : crod_kParamIdle;
    CrodFlight& f = s.flight;

    if (f.live) {
        const bool same = ball != NULL && fopAcM_GetID(ball) == f.proc;
        if (!same || param == crod_kParamIdle || param == crod_kParamAim ||
            param == crod_kParamThrow ||
            (param == crod_kParamFly && f.lastParam == crod_kParamReturn))
        {
            /* No longer away from the rod. From a return flight that is the catch: returnCopyRod
             * clears mCopyRodAcKeep (d_a_alink_copyrod.inc:177-195), or the ball deletes itself
             * when the rod is no longer equipped (d_a_crod.cpp:281-289). Anything else (an
             * unequip mid-flight, Link's actor going away) is just gone. */
            if (f.lastParam == crod_kParamReturn) {
                send_event(f.ref, ObjEventId::Caught, KindBlob{});
                crod_end_flight(s, ObjDespawnReason::Caught, "caught");
            } else {
                crod_end_flight(s, ObjDespawnReason::Gone, "gone");
            }
        } else if (param == crod_kParamControl) {
            /* Possessed a statue (d_a_crod.cpp:220-239). Phase 1: the event, then the flight ends;
             * the ball now sits at the statue's ball position, which on the receiver is a statue
             * that is not moving. Its pose this tick is that position, so it goes out hidden. */
            fopAc_ac_c* statue = ball->getControllActor();
            const KindBlob ev = crod_proc_blob(statue, true);
            crod_capture(ball, param, true, f.lastPose, f.lastBlob);
            Log.info("[mp-obj] rodball possess {} statue=0x{:03X} boss={} tick={} "
                     "pos=({:.1f}, {:.1f}, {:.1f})",
                f.ref.str(),
                statue != NULL ? static_cast<unsigned>(static_cast<u16>(fopAcM_GetName(statue))) :
                                 0u,
                ev.len >= daMpGhost_c::CROD_EVENT_POSSESS_LEN ?
                    ev.bytes[daMpGhost_c::CROD_EVENT_BOSS] :
                    0,
                now, f.lastPose.pos[0], f.lastPose.pos[1], f.lastPose.pos[2]);
            send_event(f.ref, ObjEventId::Stuck, ev);
            crod_end_flight(s, ObjDespawnReason::Gone, "possess");
        } else {
            // Flying (3) or returning (5).
            if (f.lastParam == crod_kParamFly && param == crod_kParamReturn) {
                /* Turned back. An AT hit is still flagged here: the ball stops Set()ting its AT
                 * once it returns, so nothing clears the hit until it is back on the rod
                 * (d_a_crod.cpp:210-243, 295-312). No hit means range, an unequip, or 0x731. */
                if (ball->mAtCps.ChkAtHit()) {
                    fopAc_ac_c* hit = ball->mAtCps.GetAtHitAc();
                    const KindBlob ev = crod_proc_blob(hit, false);
                    Log.info("[mp-obj] rodball turn {} cause=hit target=0x{:03X} tick={}",
                        f.ref.str(),
                        hit != NULL ? static_cast<unsigned>(static_cast<u16>(fopAcM_GetName(hit))) :
                                      0u,
                        now);
                    send_event(f.ref, ObjEventId::Hit, ev);
                } else {
                    Log.info("[mp-obj] rodball turn {} cause=range tick={}", f.ref.str(), now);
                }
            }
            crod_capture(ball, param, false, f.lastPose, f.lastBlob);
            f.lastParam = param;
            update_owned(f.ref, f.lastPose, f.lastBlob);
        }
    }

    if (f.live || ball == NULL || (param != crod_kParamFly && param != crod_kParamReturn)) {
        return;
    }

    // A new flight: thrown, or let go by a statue.
    ObjPose pose;
    KindBlob blob;
    crod_capture(ball, param, false, pose, blob);
    ObjRef ref;
    if (!spawn_transient(ObjKind::RodBall, pose, blob, ref)) {
        if (s.refusedProc != fopAcM_GetID(ball)) {
            s.refusedProc = fopAcM_GetID(ball);
            Log.warn("[mp-obj] rodball spawn refused (no session location yet?) proc={} param={} "
                     "tick={}",
                fopAcM_GetID(ball), param, now);
        }
        return;
    }
    s.refusedProc = fpcM_ERROR_PROCESS_ID_e;
    f.live = true;
    f.ref = ref;
    f.proc = fopAcM_GetID(ball);
    f.lastParam = param;
    f.lastPose = pose;
    f.lastBlob = blob;
    f.spawnTick = now;
    Log.info("[mp-obj] rodball spawn {} proc={} param={} bck={} hidden={} tick={} "
             "pos=({:.1f}, {:.1f}, {:.1f}) angle=({}, {}, {})",
        ref.str(), f.proc, param,
        blob.len >= daMpGhost_c::CROD_STATE_LEN ? blob.bytes[daMpGhost_c::CROD_STATE_BCK] : 0,
        (pose.flags & kObjPoseHidden) != 0 ? 1 : 0, now, pose.pos[0], pose.pos[1], pose.pos[2],
        pose.angle[0], pose.angle[1], pose.angle[2]);
}

void reset_rodball() {
    // Session over: forget everything, send nothing (the channel has already dropped it all).
    crod_state() = CrodState();
}

}  // namespace dusk::mp::objects::capture
