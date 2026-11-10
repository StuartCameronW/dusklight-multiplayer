#include "object_capture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "../session/network_manager.hpp"
#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_arrow.h"
#include "d/d_bomb.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "dusk/logging.h"
#include "f_op/f_op_actor_mng.h"
#include "m_Do/m_Do_mtx.h"
#include "object_channel.hpp"
#include "object_kind_arrow.hpp"
#include "player_bridge.hpp"

/**
 * Owner-side capture for arrows and slingshot pellets (ObjKind::Arrow, ObjKind::Pellet).
 * Spec: .claude/future-plans/mp-object-channel.md §2.1. Wire blobs: object_kind_arrow.hpp.
 * The receiver's half is src/d/actor/d_a_mp_ghost_arrow.inc.
 *
 * daArrow_c has no process parent (f_op_actor_mng.cpp:282-283), so this end finds its own arrows
 * by scanning for fpcNm_ARROW_e each tick. Every arrow of type 0 (normal), 1 (bomb) or 4 (pellet)
 * on this machine is the local Link's: nothing else fires those types (Horseback Zelda fires
 * type 2, which is skipped). A partner's arrows are never real actors here, only ghosts, so they
 * can never be picked up by this scan and echoed back.
 *
 * Lifecycle of one arrow, keyed by its process id:
 *  - nocked (state 0): not an object. The partner's puppet draws the nocked arrow itself
 *    (player_bridge.cpp v19 block A, kPlayerHeldArrowNocked), so the handoff is the tick the
 *    owner's param leaves 0, which is the tick the puppet stops drawing it.
 *  - shot/charged: spawn_transient, then update_owned every tick with the model's pose.
 *  - stuck in a wall or an actor, or a pellet hit: one reliable Stuck event with the final pose.
 *    The stream goes on (an arrow in an actor follows it; one in a moving platform moves), and
 *    falls silent by itself once the pose stops changing.
 *  - bomb arrow goes off: one reliable Explode event with the position of the owner's own
 *    explosion-only bomb (found, not guessed: see find_explosion).
 *  - the actor is gone (deleted, picked back up, evicted from the 6-arrow ring): despawn_owned.
 */

namespace dusk::mp::objects::capture {
namespace {

constexpr borealis::Log Log{"dusk::mp"};

/// How far from the arrow its own explosion-only bomb may be. The bomb is created at the hit
/// point (d_a_arrow.cpp:637), at the line-cross point (:727), or 95 units ahead of the arrow's
/// origin when the fuse runs out (:1043-1049); 400 covers all three with margin.
constexpr float kExplodeSearchRadius = 400.0f;

struct Tracked {
    fpc_ProcID id = fpcM_ERROR_PROCESS_ID_e;
    ObjRef ref;
    std::uint8_t type = kArrowTypeNormal;
    std::uint8_t lastState = kArrowStateNocked;
    /// Existed (already flying or stuck) before this session's capture began: never replicated.
    bool ignored = false;
    bool spawned = false;
    bool spawnFailLogged = false;
    bool stuckSent = false;
    bool explodeSent = false;
    /// field_0x93f (the arrow's "delete me") has been seen set.
    bool dying = false;
    bool seen = false;
    /// Bomb arrows: the explosion-only bombs that existed on the last tick before it started
    /// dying, so its own explosion is the one bomb NOT in this list.
    std::vector<fpc_ProcID> bombsBefore;
};

struct ExplodingBomb {
    fpc_ProcID id = fpcM_ERROR_PROCESS_ID_e;
    cXyz pos;
    bool water = false;
    bool claimed = false;
};

struct Scan {
    std::vector<daArrow_c*> arrows;
    /// Every daNbomb_c whose param says "exploding" (dBomb_c::checkStateExplode): the
    /// explosion-only bombs a bomb arrow creates start life in that state.
    std::vector<ExplodingBomb> bombs;
};

std::vector<Tracked> s_tracked;
/// False until the first scan of this session: arrows already out on that scan are left alone.
bool s_primed = false;

void* scan_actor(void* i_actor, void* i_data) {
    fopAc_ac_c* actor = static_cast<fopAc_ac_c*>(i_actor);
    Scan* scan = static_cast<Scan*>(i_data);
    const s16 name = fopAcM_GetName(actor);
    if (name == fpcNm_ARROW_e) {
        daArrow_c* arrow = static_cast<daArrow_c*>(actor);
        if (arrow->mpModel != NULL &&
            (arrow->mArrowType == kArrowTypeNormal || arrow->mArrowType == kArrowTypeBomb ||
                arrow->mArrowType == kArrowTypeSling))
        {
            scan->arrows.push_back(arrow);
        }
    } else if (name == fpcNm_NBOMB_e) {
        const u32 prm = fopAcM_GetParam(actor);
        if (prm == dBomb_c::PRM_NORMAL_BOMB_EXPLODE || prm == dBomb_c::PRM_WATER_BOMB_EXPLODE) {
            ExplodingBomb bomb;
            bomb.id = fopAcM_GetID(actor);
            bomb.pos = actor->current.pos;
            bomb.water = prm == dBomb_c::PRM_WATER_BOMB_EXPLODE;
            scan->bombs.push_back(bomb);
        }
    }
    return NULL;  // keep iterating
}

std::uint64_t now_tick() {
    return network_manager().sim_tick();
}

const char* type_name(std::uint8_t type) {
    switch (type) {
    case kArrowTypeNormal:
        return "normal";
    case kArrowTypeBomb:
        return "bomb";
    case kArrowTypeSling:
        return "pellet";
    default:
        return "?";
    }
}

/// The red TEV value daArrow_c::draw gives a bomb arrow's fuse (d_a_arrow.cpp:1095-1106), so the
/// ghost flashes in step without knowing the fuse or the HIO.
std::uint8_t fuse_flash(const daArrow_c* arrow) {
    const daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == NULL) {
        return 0;
    }
    const s16 explodeTime = link->getBombExplodeTime();
    const int half = explodeTime >> 1;
    const int quarter = explodeTime >> 2;
    const int eighth = explodeTime >> 3;
    const int sixteenth = explodeTime >> 4;
    const int fuse = arrow->field_0x950;
    constexpr float kPi = 3.14159265358979f;
    float r = 0.0f;
    if (fuse > half) {
        if (quarter != 0) {
            r = std::fabs(cM_fsin((fuse - half) * kPi / quarter));
        }
    } else if (fuse > quarter) {
        if (eighth != 0) {
            r = std::fabs(cM_fsin((fuse - quarter) * kPi / eighth));
        }
    } else if (sixteenth != 0) {
        r = std::fabs(cM_fsin((fuse - eighth) * kPi / sixteenth));
    }
    return static_cast<std::uint8_t>(std::clamp(r * 50.0f, 0.0f, 50.0f));
}

/// The arrow as the owner DRAWS it: its model's base matrix, plus the blob.
void capture_pose(daArrow_c* arrow, ObjPose& pose, ArrowBlob& blob) {
    MtxP m = arrow->mpModel->getBaseTRMtx();
    pose.pos[0] = m[0][3];
    pose.pos[1] = m[1][3];
    pose.pos[2] = m[2][3];
    csXyz rot;
    mDoMtx_MtxToRot(m, &rot);
    pose.angle[0] = rot.x;
    pose.angle[1] = rot.y;
    pose.angle[2] = rot.z;

    const std::uint8_t state = static_cast<std::uint8_t>(fopAcM_GetParam(arrow));
    pose.flags = 0;
    /* What daArrow_c::draw skips (d_a_arrow.cpp:1076), minus field_0x942: that one is "OUR camera
     * is in first person" (procWait :541-545), a property of this viewer, not of the arrow.
     * Enemy-held (6) is hidden too: the enemy holding it exists only on this machine. */
    if (arrow->field_0x93f != 0 || state == kArrowStateSlingHit || state == kArrowStateEnemyHeld) {
        pose.flags |= kObjPoseHidden;
    }

    blob.type = arrow->mArrowType;
    blob.state = state;
    blob.bits = 0;
    if (arrow->field_0x943 != 0) {
        blob.bits |= kArrowBitFrozen;
    }
    if (arrow->field_0x945 != 0) {
        blob.bits |= kArrowBitWater;
    }
    blob.scale = static_cast<std::uint8_t>(std::clamp(arrow->scale.x, 0.0f, 1.0f) * 255.0f + 0.5f);
    blob.flash =
        arrow->mArrowType == kArrowTypeBomb && arrow->field_0x943 == 0 ? fuse_flash(arrow) : 0;
}

KindBlob to_kind_blob(const ArrowBlob& b) {
    Writer w;
    write_arrow_blob(w, b);
    KindBlob out;
    out.assign(w);
    return out;
}

/// The explosion-only bomb this bomb arrow just created, or NULL. A bomb arrow's explosion is
/// dBomb_c::createNormalBombExplode (d_a_arrow.cpp:339-342), a fastCreate, so the bomb is already
/// in the actor list on the tick the arrow sets field_0x93f. "Its own" = exploding, absent before
/// the arrow started dying, nearest to the arrow, and not already claimed by another arrow.
ExplodingBomb* find_explosion(Scan& scan, const Tracked& t, const daArrow_c* arrow) {
    ExplodingBomb* best = NULL;
    float bestDist2 = kExplodeSearchRadius * kExplodeSearchRadius;
    for (ExplodingBomb& bomb : scan.bombs) {
        if (bomb.claimed ||
            std::find(t.bombsBefore.begin(), t.bombsBefore.end(), bomb.id) != t.bombsBefore.end())
        {
            continue;
        }
        const float d2 = (bomb.pos - arrow->current.pos).abs2();
        if (d2 < bestDist2) {
            bestDist2 = d2;
            best = &bomb;
        }
    }
    return best;
}

void step(Tracked& t, daArrow_c* arrow, Scan& scan) {
    const std::uint8_t state = static_cast<std::uint8_t>(fopAcM_GetParam(arrow));
    const bool dyingNow = arrow->field_0x93f != 0;

    if (!t.spawned) {
        /* Not an object while nocked: the puppet draws that, and a nocked arrow that dies was put
         * away (deleteArrow). The one exception: a nocked bomb arrow whose fuse runs out explodes
         * in the hand (d_a_arrow.cpp:1039-1051 runs in procWait too). That one is spawned, hidden,
         * only so its Explode event has an object to ride on. */
        const bool nockedExplosion = state == kArrowStateNocked && dyingNow &&
                                     t.type == kArrowTypeBomb &&
                                     find_explosion(scan, t, arrow) != NULL;
        if ((state == kArrowStateNocked && !nockedExplosion) || state == kArrowStateEnemyHeld) {
            t.lastState = state;
            return;
        }
        ObjPose pose;
        ArrowBlob blob;
        capture_pose(arrow, pose, blob);
        if (nockedExplosion) {
            pose.flags |= kObjPoseHidden;
        }
        const ObjKind kind = t.type == kArrowTypeSling ? ObjKind::Pellet : ObjKind::Arrow;
        if (!spawn_transient(kind, pose, to_kind_blob(blob), t.ref)) {
            if (!t.spawnFailLogged) {
                t.spawnFailLogged = true;
                Log.warn("[mp-obj] arrow spawn refused proc={} type={} state={} tick={} (no local "
                         "location yet?); retrying each tick",
                    t.id, type_name(t.type), state, now_tick());
            }
            return;
        }
        t.spawned = true;
        Log.info("[mp-obj] arrow spawn {} type={} state={} proc={} tick={} pos=({:.1f}, {:.1f}, "
                 "{:.1f}) angle=({}, {}, {})",
            t.ref.str(), type_name(t.type), state, t.id, now_tick(), pose.pos[0], pose.pos[1],
            pose.pos[2], pose.angle[0], pose.angle[1], pose.angle[2]);
    }

    ObjPose pose;
    ArrowBlob blob;
    capture_pose(arrow, pose, blob);
    update_owned(t.ref, pose, to_kind_blob(blob));

    if (state != t.lastState) {
        Log.info("[mp-obj] arrow state {} {} -> {} tick={} pos=({:.1f}, {:.1f}, {:.1f})",
            t.ref.str(), t.lastState, state, now_tick(), pose.pos[0], pose.pos[1], pose.pos[2]);
    }

    if (!t.stuckSent && (state == kArrowStateBgStop || state == kArrowStateActorStop ||
                            state == kArrowStateSlingHit))
    {
        t.stuckSent = true;
        ArrowStuckEvent ev;
        ev.state = state;
        ev.pose = pose;
        ev.pose.flags = 0;
        // The map info the game's own stick SE used (d_a_arrow.cpp:769). field_0x56c still holds
        // the polygon procMove hit; procBGStop itself checks it the same way (:925).
        if (state == kArrowStateBgStop && dComIfG_Bgsp().ChkPolySafe(arrow->field_0x56c)) {
            ev.polySound = static_cast<std::uint8_t>(dKy_pol_sound_get(&arrow->field_0x56c) & 0xFF);
        }
        Writer w;
        write_arrow_stuck(w, ev);
        KindBlob kb;
        kb.assign(w);
        send_event(t.ref, ObjEventId::Stuck, kb);
        Log.info("[mp-obj] arrow stuck {} state={} tick={} pos=({:.1f}, {:.1f}, {:.1f}) angle=({}, "
                 "{}, {}) polySound={}",
            t.ref.str(), state, now_tick(), pose.pos[0], pose.pos[1], pose.pos[2], pose.angle[0],
            pose.angle[1], pose.angle[2], ev.polySound);
    }

    if (dyingNow && t.type == kArrowTypeBomb && !t.explodeSent) {
        /* Not every dying bomb arrow explodes: in water, or after falling 5000 below its start, it
         * just vanishes (d_a_arrow.cpp:631-633, 719-721, 913-916). Only a real explosion-only bomb
         * near it counts. Searched again on later ticks while the arrow lingers. */
        ExplodingBomb* bomb = find_explosion(scan, t, arrow);
        if (bomb != NULL) {
            bomb->claimed = true;
            t.explodeSent = true;
            ArrowExplodeEvent ev;
            ev.pos[0] = bomb->pos.x;
            ev.pos[1] = bomb->pos.y;
            ev.pos[2] = bomb->pos.z;
            ev.water = bomb->water ? 1 : 0;
            Writer w;
            write_arrow_explode(w, ev);
            KindBlob kb;
            kb.assign(w);
            send_event(t.ref, ObjEventId::Explode, kb);
            Log.info("[mp-obj] arrow explode {} tick={} pos=({:.1f}, {:.1f}, {:.1f}) water={} "
                     "bombProc={} state={}",
                t.ref.str(), now_tick(), ev.pos[0], ev.pos[1], ev.pos[2], ev.water, bomb->id,
                state);
        } else if (!t.dying) {
            Log.info("[mp-obj] arrow dying without an explosion (yet) {} tick={} state={} bits={}",
                t.ref.str(), now_tick(), state, blob.bits);
        }
    }

    if (dyingNow) {
        t.dying = true;
    }
    t.lastState = state;
}

void forget_all(const char* i_why) {
    if (!s_tracked.empty()) {
        Log.info(
            "[mp-obj] arrow capture: forgetting {} tracked arrow(s): {}", s_tracked.size(), i_why);
    }
    s_tracked.clear();
    s_primed = false;
}

}  // namespace

void tick_arrow() {
    /* A queued scene change destroys every arrow, exactly as owner_tick drops every owned object
     * without a message (the receivers despawn on the owner's epoch). Forget them, and re-prime so
     * nothing still lingering at the end of the load is mistaken for a new shot. */
    if (local_scene_change_queued()) {
        forget_all("scene change queued");
        return;
    }

    Scan scan;
    fopAcM_Search(scan_actor, &scan);

    const bool priming = !s_primed;
    s_primed = true;

    for (Tracked& t : s_tracked) {
        t.seen = false;
    }

    for (daArrow_c* arrow : scan.arrows) {
        const fpc_ProcID id = fopAcM_GetID(arrow);
        auto it = std::find_if(
            s_tracked.begin(), s_tracked.end(), [id](const Tracked& t) { return t.id == id; });
        if (it == s_tracked.end()) {
            Tracked t;
            t.id = id;
            t.type = arrow->mArrowType;
            t.lastState = static_cast<std::uint8_t>(fopAcM_GetParam(arrow));
            /* On the session's first scan, an arrow already out (stuck in a wall, mid-flight) is
             * history: late arrivals never see old transients (spec §1.4, Q7). A nocked one has
             * not flown yet, so it is tracked like any other. */
            t.ignored = priming && t.lastState != kArrowStateNocked;
            s_tracked.push_back(t);
            it = s_tracked.end() - 1;
        }
        it->seen = true;
        if (!it->ignored) {
            step(*it, arrow, scan);
        }
        if (it->type == kArrowTypeBomb && arrow->field_0x93f == 0) {
            /* The explosions that already exist while this arrow is still whole. On the tick it
             * starts dying this list is NOT refreshed, so its own explosion is the new one. */
            it->bombsBefore.clear();
            for (const ExplodingBomb& bomb : scan.bombs) {
                it->bombsBefore.push_back(bomb.id);
            }
        }
    }

    for (auto it = s_tracked.begin(); it != s_tracked.end();) {
        if (it->seen) {
            ++it;
            continue;
        }
        if (it->spawned) {
            const ObjDespawnReason reason =
                it->explodeSent ? ObjDespawnReason::Exploded : ObjDespawnReason::Gone;
            Log.info("[mp-obj] arrow gone {} proc={} type={} lastState={} tick={} reason={}",
                it->ref.str(), it->id, type_name(it->type), it->lastState, now_tick(),
                it->explodeSent ? "exploded" : "gone");
            despawn_owned(it->ref, reason);
        }
        it = s_tracked.erase(it);
    }
}

void reset_arrow() {
    // Session over: the channel forgets every owned object itself. Nothing is sent.
    s_tracked.clear();
    s_primed = false;
}

}  // namespace dusk::mp::objects::capture
