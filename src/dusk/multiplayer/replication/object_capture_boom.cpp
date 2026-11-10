#include "object_capture.hpp"

#include "../session/network_manager.hpp"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_boomerang.h"
#include "d/d_com_inf_game.h"
#include "dusk/logging.h"
#include "f_op/f_op_actor_mng.h"
#include "object_blob_boom.hpp"
#include "object_channel.hpp"
#include "player_bridge.hpp"
#include "player_state.hpp"

/**
 * Owner-side capture for the Gale Boomerang (ObjKind::Boomerang). Spec:
 * .claude/future-plans/mp-object-channel.md §2.3; blob layout in object_blob_boom.hpp.
 *
 * daBoomerang_c lives for as long as it is equipped. Thrown, it is param 1 and sits in the LOCAL
 * Link's mThrowBoomerangAcKeep (daAlink_c::throwBoomerang, d_a_alink_boom.inc:171-192). It then
 * ends one of two ways:
 *  - caught: returnBoomerang succeeds, the keep is cleared and the SAME actor goes back to param 0
 *    in his hand (d_a_boomerang.cpp:1113-1120). Reported as an ObjEvent Caught and a despawn
 *    with reason Caught.
 *  - gone: returnBoomerang refused (he is busy), it sank, or it was force-deleted
 *    (:1122, :1295, :1301). Reported as a despawn with reason Gone.
 * So one actor is thrown many times, and every throw is its own transient.
 *
 * What is captured is what the game DRAWS: the blade model's base matrix translation (set in
 * setMoveMatrix during procMove), not current.pos, which execute() moves again afterwards while
 * the boomerang chases a lock target (field_0x957, :1370-1373).
 */

namespace dusk::mp::objects::capture {
namespace {

constexpr borealis::Log Log{"dusk::mp"};

struct BoomCapture {
    bool active = false;
    ObjRef ref;
    fpc_ProcID procId = fpcM_ERROR_PROCESS_ID_e;
    std::uint16_t age = 0;
    std::uint8_t lastBits = 0;
    ObjPose lastPose;
};

BoomCapture& boom_state() {
    static BoomCapture s;
    return s;
}

/* A caught throw's despawn goes out one tick AFTER its Caught event. The receiver applies a
 * despawn before the events of the same pass (object_channel.cpp receiver_tick), so an event and a
 * despawn stamped with the same origin tick would drop the event unseen. */
struct BoomPendingDespawn {
    bool pending = false;
    ObjRef ref;
};

BoomPendingDespawn& boom_pending() {
    static BoomPendingDespawn s;
    return s;
}

std::uint64_t boom_tick() {
    return network_manager().sim_tick();
}

/// The local Link, or NULL while there is none (loads, title screen, a Link being rebuilt).
daAlink_c* boom_local_link() {
    daPy_py_c* player = dComIfGp_getLinkPlayer();
    if (player == NULL || fopAcM_GetName(player) != fpcNm_ALINK_e) {
        return NULL;
    }
    return static_cast<daAlink_c*>(player);
}

/// The local boomerang that is in flight right now, or NULL. Name-checked, so a recycled process
/// id in the keep can never be cast.
daBoomerang_c* boom_find_thrown(daAlink_c* link) {
    if (link == NULL) {
        return NULL;
    }
    const fpc_ProcID id = link->getThrowBoomerangAcKeep()->getID();
    if (id == fpcM_ERROR_PROCESS_ID_e) {
        return NULL;
    }
    fopAc_ac_c* actor = fopAcM_SearchByID(id);
    if (actor == NULL || fopAcM_GetName(actor) != fpcNm_BOOMERANG_e || fopAcM_GetParam(actor) != 1)
    {
        return NULL;
    }
    return static_cast<daBoomerang_c*>(actor);
}

void boom_pose(daBoomerang_c* boom, ObjPose& out) {
    // The drawn position: the blade's base matrix (setMoveMatrix / setKeepMatrix). Falls back to
    // current.pos if the model is somehow missing.
    if (boom->mp_boomModel != NULL) {
        MtxP mtx = boom->mp_boomModel->getBaseTRMtx();
        out.pos[0] = mtx[0][3];
        out.pos[1] = mtx[1][3];
        out.pos[2] = mtx[2][3];
    } else {
        out.pos[0] = boom->current.pos.x;
        out.pos[1] = boom->current.pos.y;
        out.pos[2] = boom->current.pos.z;
    }
    out.angle[0] = boom->shape_angle.x;
    out.angle[1] = boom->shape_angle.y;
    out.angle[2] = boom->shape_angle.z;
    out.flags = 0;
}

BoomBlob boom_state_blob(daBoomerang_c* boom, std::uint16_t age) {
    BoomBlob b;
    b.age = age;
    b.shippuSize = boom->m_shippuSize;
    b.bits = 0;
    if (boom->getReturnFlg()) {
        b.bits |= kBoomReturning;
    }
    if (boom->field_0x957 != 0) {
        b.bits |= kBoomLockChase;
    }
    return b;
}

KindBlob boom_encode(const BoomBlob& b) {
    Writer w;
    write_boom_blob(w, b);
    KindBlob blob;
    blob.assign(w);
    return blob;
}

/// The init blob: the state blob plus the throw's lock targets as Placed refs.
KindBlob boom_init_blob(daBoomerang_c* boom, int& o_locks) {
    Writer w;
    write_boom_blob(w, boom_state_blob(boom, 0));

    const std::uint32_t stage = location_stage_hash(dComIfGp_getStartStageName());
    ObjRef refs[kBoomMaxLocks];
    std::uint8_t n = 0;
    const int count = boom->m_lockCnt < BOOMERANG_LOCK_MAX ? boom->m_lockCnt : BOOMERANG_LOCK_MAX;
    for (int i = 0; i < count && n < kBoomMaxLocks; i++) {
        if (boom->m_lockActorIDs[i] == fpcM_ERROR_PROCESS_ID_e) {
            continue;  // a ground-point lock (field_0x718), or one already passed
        }
        fopAc_ac_c* target = fopAcM_SearchByID(boom->m_lockActorIDs[i]);
        if (target == NULL) {
            continue;
        }
        const std::uint16_t setId = static_cast<std::uint16_t>(fopAcM_GetSetId(target));
        if (setId == 0xFFFF) {
            continue;
        }
        refs[n++] = ObjRef::placed(stage, fopAcM_GetHomeRoomNo(target), setId);
    }
    w.write_u8(n);
    for (std::uint8_t i = 0; i < n; i++) {
        refs[i].write(w);
    }
    o_locks = n;

    KindBlob blob;
    if (!blob.assign(w)) {
        // Cannot happen (4 + 1 + 5 x 8 = 45 <= 48); never send a truncated list.
        o_locks = 0;
        Writer bare;
        write_boom_blob(bare, boom_state_blob(boom, 0));
        bare.write_u8(0);
        blob.assign(bare);
    }
    return blob;
}

/// The tracked throw is over: say how, then forget it.
void boom_finish(BoomCapture& s, daAlink_c* link) {
    // Caught = the same actor is back in his hand (param 0). Anything else (sank, refused,
    // force-deleted, a scene change took it) is just gone.
    fopAc_ac_c* actor = fopAcM_SearchByID(s.procId);
    const bool caught = actor != NULL && fopAcM_GetName(actor) == fpcNm_BOOMERANG_e &&
                        fopAcM_GetParam(actor) == 0 && link != NULL &&
                        link->getThrowBoomerangAcKeep()->getID() == fpcM_ERROR_PROCESS_ID_e;
    if (caught) {
        Writer w;
        w.write_u16(s.age);
        KindBlob blob;
        blob.assign(w);
        Log.info("[mp-obj] boomerang caught {} age={} tick={} pos=({:.1f}, {:.1f}, {:.1f})",
            s.ref.str(), s.age, boom_tick(), s.lastPose.pos[0], s.lastPose.pos[1],
            s.lastPose.pos[2]);
        send_event(s.ref, ObjEventId::Caught, blob);
        const ObjRef ref = s.ref;
        s = BoomCapture();
        boom_pending().pending = true;
        boom_pending().ref = ref;
        return;
    } else {
        Log.info("[mp-obj] boomerang gone {} age={} tick={} actor={}", s.ref.str(), s.age,
            boom_tick(), actor != NULL ? "deleting" : "none");
        despawn_owned(s.ref, ObjDespawnReason::Gone);
    }
    s = BoomCapture();
}

}  // namespace

void tick_boomerang() {
    BoomPendingDespawn& pending = boom_pending();
    if (pending.pending) {
        despawn_owned(pending.ref, ObjDespawnReason::Caught);
        pending = BoomPendingDespawn();
    }

    BoomCapture& s = boom_state();
    daAlink_c* link = boom_local_link();
    daBoomerang_c* boom = boom_find_thrown(link);

    if (s.active) {
        if (boom == NULL || fopAcM_GetID(boom) != s.procId) {
            /* Ended. If the channel already dropped the ref (a queued scene change, an epoch
             * advance) the event and despawn below are ignored by it, which is right: the far
             * end despawns those on the epoch. */
            boom_finish(s, link);
        } else {
            if (s.age < 0xFFFF) {
                s.age++;
            }
            ObjPose pose;
            boom_pose(boom, pose);
            const BoomBlob b = boom_state_blob(boom, s.age);
            if ((b.bits & kBoomReturning) != 0 && (s.lastBits & kBoomReturning) == 0) {
                Log.info("[mp-obj] boomerang return {} age={} tick={} pos=({:.1f}, {:.1f}, {:.1f})",
                    s.ref.str(), s.age, boom_tick(), pose.pos[0], pose.pos[1], pose.pos[2]);
            }
            s.lastBits = b.bits;
            s.lastPose = pose;
            update_owned(s.ref, pose, boom_encode(b));
            return;
        }
    }

    if (boom == NULL) {
        return;
    }
    /* A new throw. Not while a scene change is queued (the actor is about to be destroyed and the
     * channel would drop it at once), and not outside playable gameplay. */
    if (local_scene_change_queued() || !world_is_playable()) {
        return;
    }
    ObjPose pose;
    boom_pose(boom, pose);
    int locks = 0;
    const KindBlob init = boom_init_blob(boom, locks);
    ObjRef ref;
    if (!spawn_transient(ObjKind::Boomerang, pose, init, ref)) {
        // No session or no location yet: retried next tick while it is still in flight.
        return;
    }
    s.active = true;
    s.ref = ref;
    s.procId = fopAcM_GetID(boom);
    s.age = 0;
    s.lastBits = boom_state_blob(boom, 0).bits;
    s.lastPose = pose;
    Log.info("[mp-obj] boomerang spawn {} proc={} tick={} pos=({:.1f}, {:.1f}, {:.1f}) "
             "angle=({}, {}, {}) locks={} returning={}",
        ref.str(), s.procId, boom_tick(), pose.pos[0], pose.pos[1], pose.pos[2], pose.angle[0],
        pose.angle[1], pose.angle[2], locks, (s.lastBits & kBoomReturning) != 0 ? 1 : 0);
}

void reset_boomerang() {
    // Session over: forget silently. The channel's own reset drops every owned object.
    boom_state() = BoomCapture();
    boom_pending() = BoomPendingDespawn();
}

}  // namespace dusk::mp::objects::capture
