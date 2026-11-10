#include "object_capture.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "../session/network_manager.hpp"
#include "d/actor/d_a_nbomb.h"
#include "dusk/logging.h"
#include "f_op/f_op_actor_mng.h"
#include "m_Do/m_Do_mtx.h"
#include "object_channel.hpp"
#include "object_kind_bomb.hpp"
#include "player_bridge.hpp"

/**
 * Owner-side capture for bombs, water bombs and bomblings (spec .claude/future-plans/
 * mp-object-channel.md §2.2). Reads daNbomb_c directly, as the phase-1 capture files are meant to
 * (object_capture.hpp); everything it sends is engine-free (object_kind_bomb.hpp).
 *
 * WHICH bombs are this player's: FLG0_PLAYER_MAKE. daNbomb_c::create sets it for exactly the
 * three bombs Link makes from his bag (params 8 normal, 9 water, 10 bombling,
 * d_a_nbomb.cpp:268-305), and nothing ever clears it. Not this player's, and never captured:
 *  - enemy bombs (Bulblin / Bokoblin bomb-throwers, the Ooccoo... params 11-16), even once Link
 *    has picked one up (setEnemyBomb): no PLAYER_MAKE. M6 owns enemy objects.
 *  - bomb flowers (param 4) and cargo bombs (17): world objects; carrying is phase 2.
 *  - explosion-only bombs (params 0 / 18): a bomb arrow's blast (the arrow kind sends its own
 *    explode event) and every REMOTE explosion this end replays (daMpGhost_spawnRemoteExplosion),
 *    so a replayed explosion can never echo back to its owner.
 * On this end every PLAYER_MAKE bomb is the local Link's: the destructor's decrementBombCnt is
 * keyed off the same flag (d_a_nbomb.cpp:397-403), which is the game saying so.
 *
 * Lifetime of one tracked bomb:
 *   seen (held, thrown, placed, rolling)  -> ObjSpawn ("[mp-obj] bomb spawn")
 *   every tick                            -> update_owned: model pose + BombBlob
 *   procExplodeInit ran (param 0)         -> ObjEvent Explode ("[mp-obj] bomb explode"), then
 *                                            hidden; the actor lives on ~15 ticks for its blast
 *   actor gone, >= kDespawnAfterExplode   -> ObjDespawn(Exploded)   ("[mp-obj] bomb despawn")
 *   actor gone without exploding          -> ObjDespawn(Gone): drowned, frozen and thawed, deleted
 *
 * The despawn is held back after the explode event on purpose: the receiver checks a pending
 * despawn BEFORE it delivers events (object_channel.cpp receiver_tick), so a despawn on the same
 * tick as the explode would remove the ghost and silently drop the explosion with it.
 */

namespace dusk::mp::objects::capture {

namespace {

constexpr borealis::Log Log{"dusk::mp"};

/// Ticks between the explode event and the despawn, at the least (see the file comment).
constexpr int kBombDespawnAfterExplode = 10;
/// A bomb younger than this is not sent its explode event yet: the receiver creates the ghost
/// when playback reaches the spawn and it comes alive a tick or two later, and an event that
/// arrives while the ghost is still being created is dropped (receiver_tick). Only matters for a
/// bombling that has nowhere to walk and blows up on the tick it is made (d_a_nbomb.cpp:336).
constexpr int kBombMinAgeForEvent = 8;
/// Spawn attempts logged per bomb when the channel refuses (no local location yet, mid-load).
constexpr int kBombSpawnFailLogs = 2;

struct TrackedBomb {
    ObjRef ref;
    ObjKind kind = ObjKind::Bomb;
    bool spawned = false;
    int spawnFailures = 0;
    /// Ticks since first seen.
    int age = 0;
    /// Fuse sync state (object_kind_bomb.hpp).
    std::int16_t syncFuse = 0;
    int syncAge = 0;
    std::uint8_t fuseSeq = 0;
    std::uint8_t bounceSeq = 0;
    std::uint8_t bouncePower = 0;
    /// The last pose sent: what a hidden (exploded) or vanished bomb keeps reporting.
    ObjPose pose;
    BombBlob blob;
    /// procExplodeInit has run on it.
    bool exploded = false;
    bool explodeSent = false;
    BombExplodeBlob explodeBlob;
    int explodeSentAge = 0;
    /// The actor no longer exists (only kept for the delayed despawn).
    bool gone = false;
};

std::map<fpc_ProcID, TrackedBomb>& bombs() {
    static std::map<fpc_ProcID, TrackedBomb> s;
    return s;
}

void* bomb_collect(void* i_actor, void* i_data) {
    fopAc_ac_c* actor = static_cast<fopAc_ac_c*>(i_actor);
    if (fopAcM_GetName(actor) != fpcNm_NBOMB_e) {
        return NULL;
    }
    daNbomb_c* bomb = static_cast<daNbomb_c*>(actor);
    if (!bomb->checkPlayerMake() || bomb->mpModel == NULL) {
        return NULL;
    }
    static_cast<std::vector<daNbomb_c*>*>(i_data)->push_back(bomb);
    return NULL;  // keep iterating
}

ObjKind bomb_kind(const daNbomb_c* bomb) {
    switch (bomb->mType) {
    case daNbomb_c::TYPE_WATER_PLAYER:
        return ObjKind::WaterBomb;
    case daNbomb_c::TYPE_INSECT_PLAYER:
        return ObjKind::Bombling;
    default:
        return ObjKind::Bomb;
    }
}

/// The pose the bomb is DRAWN with: its model's base matrix, which carries the roll
/// (field_0xa40), the slope lift and the bombling's wall/ceiling orientation (d_a_nbomb.cpp:
/// 1516-1523, setHitPolygon). The ghost rebuilds it as transS(pos) * ZXYrotM(angle);
/// mDoMtx_MtxToRot is ZXYrotM's inverse (setHitPolygon round-trips through it).
ObjPose bomb_pose(daNbomb_c* bomb) {
    MtxP m = bomb->mpModel->getBaseTRMtx();
    ObjPose pose;
    pose.pos[0] = m[0][3];
    pose.pos[1] = m[1][3];
    pose.pos[2] = m[2][3];
    csXyz rot;
    mDoMtx_MtxToRot(m, &rot);
    pose.angle[0] = rot.x;
    pose.angle[1] = rot.y;
    pose.angle[2] = rot.z;
    pose.flags = 0;
    return pose;
}

void bomb_fill_blob(daNbomb_c* bomb, TrackedBomb& t) {
    BombBlob& b = t.blob;
    b.flags = 0;
    if (fopAcM_checkCarryNow(bomb) || fopAcM_checkHookCarryNow(bomb)) {
        b.flags |= kBombHeld;
    }
    if (bomb->checkStateFlg0(daNbomb_c::FLG0_FROZEN)) {
        b.flags |= kBombFrozen;
    }
    if (bomb->checkStateFlg0(daNbomb_c::FLG0_UNK_800)) {
        b.flags |= kBombSinking;
    }
    if (bomb->checkTimerStop()) {
        b.flags |= kBombTimerStopped;
    }
    if (fopAcM_GetParam(bomb) == dBomb_c::PRM_INSECT_BOMB_MOVE) {
        b.flags |= kBombWalking;
    }
    if (bomb->checkStateFlg0(daNbomb_c::FLG0_UNDERWATER)) {
        b.flags |= kBombUnderwater;
    }

    // Fuse sync: resend only when the fuse stopped burning one per tick.
    const std::int16_t exTime = bomb->getExTime();
    const int expected = static_cast<int>(t.syncFuse) - (t.age - t.syncAge);
    if (!t.spawned || exTime != expected) {
        if (t.spawned) {
            t.fuseSeq++;
        }
        t.syncFuse = exTime;
        t.syncAge = t.age;
    }
    b.fuse = t.syncFuse;
    b.fuseSeq = t.fuseSeq;

    const float scale = std::clamp(bomb->scale.x * 100.0f + 0.5f, 0.0f, 255.0f);
    b.scale100 = static_cast<std::uint8_t>(scale);

    if (bomb->mAcch.ChkGroundLanding()) {
        // procWait's own volume: |the fall speed before landing| * 2, capped at 100. old.pos is
        // this tick's start, so old.y - current.y is that fall.
        const float fall = std::fabs(bomb->old.pos.y - bomb->current.pos.y) * 2.0f;
        t.bounceSeq++;
        t.bouncePower = static_cast<std::uint8_t>(std::clamp(fall, 0.0f, 100.0f));
    }
    b.bounceSeq = t.bounceSeq;
    b.bouncePower = t.bouncePower;
}

KindBlob bomb_kind_blob(const BombBlob& b) {
    Writer w;
    write_bomb_blob(w, b);
    KindBlob out;
    out.assign(w);
    return out;
}

void bomb_send_explode(TrackedBomb& t) {
    Writer w;
    write_bomb_explode_blob(w, t.explodeBlob);
    KindBlob blob;
    blob.assign(w);
    send_event(t.ref, ObjEventId::Explode, blob);
    t.explodeSent = true;
    t.explodeSentAge = t.age;
    Log.info("[mp-obj] bomb explode {} kind={} tick={} pos=({:.1f}, {:.1f}, {:.1f}) water={} "
             "age={}",
        t.ref.str(), kind_name(static_cast<std::uint8_t>(t.kind)), network_manager().sim_tick(),
        t.explodeBlob.pos[0], t.explodeBlob.pos[1], t.explodeBlob.pos[2],
        static_cast<unsigned>(t.explodeBlob.water), t.age);
}

void bomb_despawn(TrackedBomb& t, ObjDespawnReason reason) {
    if (t.spawned) {
        Log.info("[mp-obj] bomb despawn {} kind={} tick={} reason={} age={}", t.ref.str(),
            kind_name(static_cast<std::uint8_t>(t.kind)), network_manager().sim_tick(),
            reason == ObjDespawnReason::Exploded ? "exploded" : "gone", t.age);
        // Unknown to the channel (a scene change already dropped it silently): a no-op.
        despawn_owned(t.ref, reason);
    }
}

}  // namespace

void tick_bomb() {
    std::map<fpc_ProcID, TrackedBomb>& tracked = bombs();

    std::vector<daNbomb_c*> live;
    fopAcM_Search(bomb_collect, &live);

    /* A queued scene change destroys every bomb, as vanilla does. Nothing new is spawned into the
     * dying scene; what is already out is dropped by the channel itself (owner_tick), and the
     * actors' disappearance next tick then despawns into a ref the channel no longer knows. */
    const bool sceneChange = local_scene_change_queued();

    for (std::size_t i = 0; i < live.size(); i++) {
        daNbomb_c* bomb = live[i];
        const fpc_ProcID id = fopAcM_GetID(bomb);
        auto it = tracked.find(id);
        if (it == tracked.end()) {
            if (sceneChange) {
                continue;
            }
            TrackedBomb t;
            t.kind = bomb_kind(bomb);
            it = tracked.emplace(id, t).first;
        }
        TrackedBomb& t = it->second;
        t.age++;

        if (!t.exploded && bomb->checkStateExplode()) {
            // procExplodeInit ran this tick: the param is rewritten to 0 there (d_a_nbomb.cpp:681).
            t.exploded = true;
            t.explodeBlob.pos[0] = bomb->current.pos.x;
            t.explodeBlob.pos[1] = bomb->current.pos.y;
            t.explodeBlob.pos[2] = bomb->current.pos.z;
            t.explodeBlob.water = bomb->checkStateFlg0(daNbomb_c::FLG0_UNDERWATER) ? 1 : 0;
        }

        if (!t.exploded) {
            t.pose = bomb_pose(bomb);
            bomb_fill_blob(bomb, t);
        }

        if (!t.spawned) {
            if (sceneChange) {
                continue;
            }
            if (t.exploded) {
                // Never seen alive (blew up on the tick it was made): show it at the blast.
                t.pose.pos[0] = t.explodeBlob.pos[0];
                t.pose.pos[1] = t.explodeBlob.pos[1];
                t.pose.pos[2] = t.explodeBlob.pos[2];
            }
            ObjRef ref;
            if (!spawn_transient(t.kind, t.pose, bomb_kind_blob(t.blob), ref)) {
                if (t.spawnFailures++ < kBombSpawnFailLogs) {
                    Log.info("[mp-obj] bomb spawn refused proc={} kind={} (no local location "
                             "yet?); retrying",
                        id, kind_name(static_cast<std::uint8_t>(t.kind)));
                }
                continue;
            }
            t.ref = ref;
            t.spawned = true;
            Log.info("[mp-obj] bomb spawn {} kind={} proc={} tick={} pos=({:.1f}, {:.1f}, {:.1f}) "
                     "fuse={} held={}",
                t.ref.str(), kind_name(static_cast<std::uint8_t>(t.kind)), id,
                network_manager().sim_tick(), t.pose.pos[0], t.pose.pos[1], t.pose.pos[2],
                t.blob.fuse, (t.blob.flags & kBombHeld) != 0 ? 1 : 0);
        }

        if (t.exploded && !t.explodeSent && t.age >= kBombMinAgeForEvent) {
            bomb_send_explode(t);
        }

        ObjPose pose = t.pose;
        // Hidden from the tick AFTER the explode event on: on the event's own tick the ghost must
        // still be visible, because it decides from that whether its room is shown.
        if (t.explodeSent && t.age > t.explodeSentAge) {
            pose.flags |= kObjPoseHidden;
        }
        update_owned(t.ref, pose, bomb_kind_blob(t.blob));
    }

    // Bombs that no longer exist.
    for (auto it = tracked.begin(); it != tracked.end();) {
        TrackedBomb& t = it->second;
        const bool alive =
            std::any_of(live.begin(), live.end(),
                [&](daNbomb_c* bomb) { return fopAcM_GetID(bomb) == it->first; });
        if (alive) {
            ++it;
            continue;
        }
        if (!t.spawned) {
            it = tracked.erase(it);
            continue;
        }
        if (!t.gone) {
            t.gone = true;
        }
        if (t.exploded) {
            if (!t.explodeSent) {
                bomb_send_explode(t);
            }
            // Hold the despawn back until the explode event has had its ticks (file comment).
            t.age++;
            if (t.age - t.explodeSentAge < kBombDespawnAfterExplode) {
                ObjPose pose = t.pose;
                pose.flags |= kObjPoseHidden;
                update_owned(t.ref, pose, bomb_kind_blob(t.blob));
                ++it;
                continue;
            }
            bomb_despawn(t, ObjDespawnReason::Exploded);
        } else {
            bomb_despawn(t, ObjDespawnReason::Gone);
        }
        it = tracked.erase(it);
    }
}

void reset_bomb() {
    bombs().clear();
}

}  // namespace dusk::mp::objects::capture
