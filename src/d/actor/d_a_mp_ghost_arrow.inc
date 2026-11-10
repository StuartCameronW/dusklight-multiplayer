/* Arrow + pellet (ObjKind Arrow 0x01, Pellet 0x02): the ghost side. Included at the end of
 * d_a_mp_ghost.cpp. Spec: .claude/future-plans/mp-object-channel.md §2.1 and §3. Wire blobs:
 * src/dusk/multiplayer/replication/object_kind_arrow.hpp. Owner side:
 * src/dusk/multiplayer/replication/object_capture_arrow.cpp.
 *
 * ★ Never a daArrow_c. A real arrow on this end would re-aim from OUR camera, add OUR horse's
 * speed, refill OUR quiver when walked over (checkReget, d_a_arrow.cpp:465-477) and take a slot in
 * OUR 6-arrow ring, evicting this player's own stuck arrows (m_count, :327-331, :1029-1030). This
 * ghost touches none of that: it draws the game's own model at the pose the owner drew it at, with
 * daArrow_c::draw's own lighting, and plays the arrow's own sounds and particles. */

#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/JParticle/JPAEmitter.h"
#include "Z2AudioLib/Z2SeMgr.h"
#include "d/d_cc_d.h"
#include "dusk/multiplayer/replication/object_kind_arrow.hpp"
#include "dusk/settings.h"

/// P1b's remote explosion, defined in d_a_mp_ghost_bomb.inc (included after this file): the game's
/// own explosion-only bomb with a Dusk collision mask (spec §2.2).
void daMpGhost_spawnRemoteExplosion(const cXyz& i_pos, bool i_water, u8 i_owner);

namespace {

/// "Alink" resource indices, exactly as daArrow_c::createHeap picks them (d_a_arrow.cpp:27-42).
constexpr int l_arrowResPlain = 0x1c;
constexpr int l_arrowResBomb = 0x1d;
constexpr int l_arrowResSling = 0x20;

/// The arrow model's tip in its own space: getArrowOutLength() (d_a_arrow.h), where the game's own
/// hit capsule ends (setArrowAt, d_a_arrow.cpp:238 with mOutLengthRate = 95 / speed).
constexpr f32 l_arrowTipZ = 95.0f;
/// setArrowAt's radius before the charge growth: 5 for an arrow, 8 for a pellet (:217-222).
constexpr f32 l_arrowFfRadius = 5.0f;
constexpr f32 l_arrowFfRadiusSling = 8.0f;

/// The bomb arrow's fuse smoke and sparks, and their spot on the model (setSmokePos /
/// setBombMoveEffect, d_a_arrow.cpp:393-400, 446-463).
constexpr u16 l_arrowSmokeTrail = 0x1e0;
constexpr u16 l_arrowSmokeSpark = 0x1de;

/// Friendly fire (spec §3), the same recipe as the puppet's sword (d_a_remote_player.cpp:8108):
///  - AT_TYPE_CSTATUE_SWING, which is what the game's own enemy arrows hurt Link with
///    (d_a_e_arrow.cpp:711), rather than AT_TYPE_ARROW, whose path from a non-enemy into Link's
///    damage code nobody has read.
///  - SPrm 0x5: hit group 0x4 ONLY, Link's hurtbox (d_a_alink.cpp:1489). Enemies and objects are
///    never in 0x4, so this cannot hurt anything but the local player.
///  - Power 1, the enemy arrow's. A pellet gets power 0 and the slingshot's own SE, set below.
///  - GObjAt copied from the enemy arrow's sphere (d_a_e_arrow.cpp:712).
dCcD_SrcCps l_arrowFfCpsSrc = {
    {
        {0, {{AT_TYPE_CSTATUE_SWING, 1, 0x5}, {0, 0}, 0}},
        {dCcD_SE_ARROW_STICK, 0, 8, 0, {0}},
        {dCcD_SE_NONE, 0, 0, 0, {0}},
        {0},
    },
    {{
        {0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f},
        5.0f,
    }},
};

}  // namespace

struct daMpGhost_c::ArrowFf {
    dCcD_Stts stts;
    dCcD_Cps cps;
};

/* ---- create ---------------------------------------------------------------------------------- */

int daMpGhost_c::createHeapArrow() {
    /* Everything this kind owns is (re)set here: the shared constructor does not know these
     * members, and fopAcM_entrySolidHeap may run this twice (estimated heap, then the largest). */
    mpModel = NULL;
    mpArrowFf = NULL;
    mpArrowPlainModel = NULL;
    mpArrowBombModel = NULL;
    mArrowPrevPos = current.pos;
    mArrowSmokePos = current.pos;
    mArrowSmokeAngle = csXyz(0, 0, 0);
    mArrowBlurId = 0;
    mArrowSmokeId[0] = 0;
    mArrowSmokeId[1] = 0;
    mArrowType = mKind == l_kindPellet ? objects::kArrowTypeSling : objects::kArrowTypeNormal;
    mArrowState = objects::kArrowStateShot;
    mArrowBits = 0;
    mArrowScale = 255;
    mArrowFlash = 0;
    mArrowPrevState = objects::kArrowStateShot;
    mArrowSoundReady = false;
    mArrowHavePrev = false;
    mArrowStuck = false;
    mArrowGone = false;
    mArrowFfHit = false;
    mArrowLoggedUp = false;

    // The resident "Alink" archive, as the game's own arrow loads it. Checked, not assumed: see
    // createHeapDebug ([unverified] that it stays mounted while the LOCAL player is a wolf).
    const char* arc = daAlink_c::getAlinkArcName();
    if (dComIfG_getObjectResInfo(arc) == NULL) {
        Log.warn("[mp-obj] ghost: archive '{}' is not resident; no arrow ghost", arc);
        return FALSE;
    }
    const int plainRes = mKind == l_kindPellet ? l_arrowResSling : l_arrowResPlain;
    J3DModelData* plainData = static_cast<J3DModelData*>(dComIfG_getObjectRes(arc, plainRes));
    if (plainData == NULL) {
        return FALSE;
    }
    mpArrowPlainModel = mDoExt_J3DModel__create(plainData, 0x80000, 0x11000084);
    if (mpArrowPlainModel == NULL) {
        return FALSE;
    }
    if (mKind == l_kindArrow) {
        J3DModelData* bombData =
            static_cast<J3DModelData*>(dComIfG_getObjectRes(arc, l_arrowResBomb));
        if (bombData == NULL) {
            return FALSE;
        }
        mpArrowBombModel = mDoExt_J3DModel__create(bombData, 0x80000, 0x11000084);
        if (mpArrowBombModel == NULL) {
            return FALSE;
        }
    }
    mpModel = mpArrowPlainModel;

    // In this solid heap: freed with it, so the shared destructor needs no arrow code.
    mpArrowFf = JKR_NEW ArrowFf();
    if (mpArrowFf == NULL) {
        return FALSE;
    }
    mpArrowFf->stts.Init(0xFF, 0xFF, this);
    mpArrowFf->cps.Set(l_arrowFfCpsSrc);
    mpArrowFf->cps.SetStts(&mpArrowFf->stts);
    if (mKind == l_kindPellet) {
        // What arrowShooting does to a pellet's capsule (d_a_arrow.cpp:320-325): no damage.
        mpArrowFf->cps.SetAtAtp(0);
        mpArrowFf->cps.SetAtSe(dCcD_SE_PACHINKO);
    }
    return TRUE;
}

/* ---- helpers --------------------------------------------------------------------------------- */

/// Decode the latest blob into the mArrow* fields. A short or missing blob leaves them as they
/// were.
void daMpGhost_c::arrowReadBlob() {
    if (mBlobLen == 0) {
        return;
    }
    dusk::mp::Reader r(mBlob, mBlobLen);
    objects::ArrowBlob b;
    if (!objects::read_arrow_blob(r, b)) {
        return;
    }
    mArrowType = b.type;
    mArrowState = b.state;
    mArrowBits = b.bits;
    mArrowScale = b.scale;
    mArrowFlash = b.flash;
}

/// Point mpModel at the model the type calls for, and pose it. Draw calls it too (i_drawOnly), for
/// the one frame where the channel pushed the spawn's blob after this tick's execute: then it only
/// acts on a model swap, so it never rewrites a matrix the frame interpolator already recorded.
void daMpGhost_c::arrowSyncModel(bool i_drawOnly) {
    J3DModel* want = mArrowType == objects::kArrowTypeBomb && mpArrowBombModel != NULL ?
                         mpArrowBombModel :
                         mpArrowPlainModel;
    if (want == NULL) {
        return;
    }
    if (mpModel != want) {
        mpModel = want;
        fopAcM_SetMtx(this, mpModel->getBaseTRMtx());
    } else if (i_drawOnly) {
        return;
    }
    const f32 s = mArrowScale / 255.0f;
    scale.set(s, s, s);
    mpModel->setBaseScale(scale);
    updateMatrix();
}

void daMpGhost_c::arrowInitSound() {
    if (!mArrowSoundReady) {
        // As daArrow_c::createHeap does (d_a_arrow.cpp:48), deferred to first use so a heap
        // callback that runs twice cannot init it twice.
        mArrowSound.init(&current.pos, 1);
        mArrowSoundReady = true;
    }
}

/**
 * Friendly fire, victim-judged (spec §3): this end decides whether ITS player was hit, by the
 * arrow it is drawing. Armed only when THIS end's multiplayer.friendlyFire is on. One hit per
 * projectile; after it, the projectile is spent on this screen and stops drawing.
 *
 * The capsule runs from last execute's model origin (the tail) to this one's tip, so a fast arrow
 * cannot step over the player between two ticks — the same sweep setArrowAt makes ahead of the
 * real arrow (d_a_arrow.cpp:238-263), one tick behind it.
 */
void daMpGhost_c::arrowFriendlyFire(bool i_armed) {
    ArrowFf* ff = mpArrowFf;
    if (ff == NULL) {
        return;
    }
    // A hit is reported on the tick after the collision pass that found it.
    if (ff->cps.ChkAtHit()) {
        if (!mArrowFfHit) {
            fopAc_ac_c* hitAc = ff->cps.GetAtHitAc();
            const bool link = hitAc != NULL && hitAc == dComIfGp_getPlayer(0);
            Log.info("Friendly fire: player {}'s {} hit the local player{} (ghost proc {}, local "
                     "life now {})",
                mAuthority, mKind == l_kindPellet ? "pellet" : "arrow",
                link ? "" : " [hit actor is not the local Link?]", fopAcM_GetID(this),
                static_cast<unsigned>(dComIfGs_getLife()));
        }
        mArrowFfHit = true;
        mArrowGone = true;
    }
    ff->cps.ResetAtHit();

    if (!i_armed || mArrowFfHit || !dusk::getSettings().multiplayer.friendlyFire.getValue()) {
        ff->cps.OffAtSetBit();
        return;
    }
    const bool sling = mKind == l_kindPellet;
    cXyz tip;
    const cXyz tipLocal(0.0f, 0.0f, sling ? 0.0f : l_arrowTipZ);
    mDoMtx_multVec(mpModel->getBaseTRMtx(), &tipLocal, &tip);
    const cXyz tail = mArrowHavePrev ? mArrowPrevPos : current.pos;
    static_cast<cM3dGCps*>(&ff->cps)->Set(
        tail, tip, sling ? l_arrowFfRadiusSling : l_arrowFfRadius);
    ff->cps.CalcAtVec();
    ff->cps.OnAtSetBit();
    dComIfG_Ccsp()->Set(&ff->cps);
}

/* ---- execute / draw -------------------------------------------------------------------------- */

void daMpGhost_c::executeArrow() {
    arrowReadBlob();
    arrowSyncModel(false);
    arrowInitSound();
    if (!mArrowLoggedUp) {
        mArrowLoggedUp = true;
        Log.info("[mp-obj] arrow ghost up proc={} kind={} type={} state={} authority={} "
                 "pos=({:.1f}, {:.1f}, {:.1f})",
            fopAcM_GetID(this), mKind == l_kindPellet ? "pellet" : "arrow", mArrowType, mArrowState,
            mAuthority, current.pos.x, current.pos.y, current.pos.z);
    }

    const s8 reverb = dComIfGp_getReverb(fopAcM_GetRoomNo(this));
    // daArrow_c::setRoomInfo's lighting half (d_a_arrow.cpp:344-357). The actor's own room is left
    // alone: the channel created it in the local Link's room and owns its lifetime.
    if (fopAcM_gc_c::gndCheck(&current.pos)) {
        tevStr.room_no = fopAcM_gc_c::getRoomId();
        tevStr.YukaCol = fopAcM_gc_c::getPolyColor();
    } else {
        tevStr.room_no = dComIfGp_roomControl_getStayNo();
    }

    const bool shown = mVisible && !mArrowGone;
    const bool flying =
        mArrowState == objects::kArrowStateShot || mArrowState == objects::kArrowStateCharged;
    const bool water = (mArrowBits & objects::kArrowBitWater) != 0;
    const bool frozen = (mArrowBits & objects::kArrowBitFrozen) != 0;
    const bool bomb = mArrowType == objects::kArrowTypeBomb;
    const bool sling = mArrowType == objects::kArrowTypeSling;

    // Cues the stream carries (cosmetic: the ones that matter ride reliable events).
    if (mArrowState != mArrowPrevState) {
        if (mArrowState == objects::kArrowStateReturn && shown) {
            // procMove's rebound off a hard surface (d_a_arrow.cpp:755-757).
            mArrowSound.startSound(Z2SE_HIT_AL_ARROW_REBOUND, 0, reverb);
        }
        mArrowPrevState = mArrowState;
    }

    // Flight loop, procMove's choice of sound (d_a_arrow.cpp:822-841); not in water, not a pellet.
    if (shown && flying && !water && !sling) {
        u32 sound;
        if (mArrowState == objects::kArrowStateCharged) {
            sound = bomb ? Z2SE_OBJ_ARROWBOMB_FLYGAIN : Z2SE_OBJ_ARROW_FLY_GAIN;
        } else {
            sound = bomb ? Z2SE_OBJ_ARROWBOMB_FLY : Z2SE_OBJ_ARROW_FLY_NORMAL;
        }
        mArrowSound.startLevelSound(sound, 0, reverb);
    }

    /* The blur trail: one emitter for a normal arrow (0x3b8) or a pellet (0xa55), none for a bomb
     * arrow (daArrow_c::create, d_a_arrow.cpp:1153-1165). Kept alive while flying (setBlur), faded
     * out 51 alpha a tick once it stops (decAlphaBlur, :123-143). */
    const u16 blurName = sling ? 0xa55 : (bomb ? 0 : 0x3b8);
    if (blurName != 0) {
        if (shown && flying && !water) {
            mArrowBlurId = dComIfGp_particle_set(mArrowBlurId, blurName, &current.pos, &tevStr);
            dComIfGp_particle_levelEmitterOnEventMove(mArrowBlurId);
        } else {
            JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(mArrowBlurId);
            if (emitter != NULL) {
                const u8 alpha = emitter->getGlobalAlpha();
                if (alpha > 0x33) {
                    mArrowBlurId =
                        dComIfGp_particle_set(mArrowBlurId, blurName, &current.pos, &tevStr);
                    dComIfGp_particle_levelEmitterOnEventMove(mArrowBlurId);
                    emitter->setGlobalAlpha(alpha - 51);
                } else {
                    emitter->setGlobalAlpha(0);
                }
            }
        }
    }

    /* Bomb arrow fuse smoke and sparks while it flies or tumbles (setBombMoveEffect, :446-463).
     * [unverified look] the game threads the smoke through a hermite callback (field_0xa10) that
     * fills the gap between frames; the ghost emits it without one. */
    if (bomb && shown && !water && !frozen && (flying || mArrowState == objects::kArrowStateReturn))
    {
        static const cXyz smokeLocal(-3.3f, -3.0f, 94.0f);
        mDoMtx_multVec(mpModel->getBaseTRMtx(), &smokeLocal, &mArrowSmokePos);
        mArrowSmokeAngle.y = shape_angle.y;
        mArrowSmokeId[0] = dComIfGp_particle_set(mArrowSmokeId[0], l_arrowSmokeTrail,
            &mArrowSmokePos, &tevStr, &mArrowSmokeAngle, NULL, 0xff, NULL, -1, NULL, NULL, NULL);
        dComIfGp_particle_levelEmitterOnEventMove(mArrowSmokeId[0]);
        mArrowSmokeId[1] = dComIfGp_particle_set(mArrowSmokeId[1], l_arrowSmokeSpark,
            &mArrowSmokePos, &tevStr, &mArrowSmokeAngle, NULL, 0xff, NULL, -1, NULL, NULL, NULL);
        dComIfGp_particle_levelEmitterOnEventMove(mArrowSmokeId[1]);
        JPABaseEmitter* spark = dComIfGp_particle_getEmitter(mArrowSmokeId[1]);
        if (spark != NULL) {
            spark->playDrawParticle();
        }
    }

    // Only a flying normal arrow or pellet hurts: a bomb arrow's damage is its explosion (the
    // game sets its own capsule's power to 0, d_a_arrow.cpp:489-490).
    arrowFriendlyFire(shown && flying && !water && !bomb);

    mArrowPrevPos = current.pos;
    mArrowHavePrev = true;
    mArrowSound.framework(0, reverb);
}

int daMpGhost_c::drawArrow() {
    if (mArrowGone || mpArrowPlainModel == NULL) {
        return 1;
    }
    arrowReadBlob();
    arrowSyncModel(true);

    /* daArrow_c::draw (d_a_arrow.cpp:1073-1129), minus its reads of the arrow's own fuse: the
     * owner computed the flash value (ArrowBlob::flash). The freeze tint is HIO data, read off the
     * local Link exactly as the game reads it; with no Link it is simply skipped. */
    tevStr.TevColor.r = 0;  // procWait zeroes it when the arrow is shot (:484-486)
    tevStr.TevColor.g = 0;
    tevStr.TevColor.b = 0;
    g_env_light.settingTevStruct(0, &current.pos, &tevStr);

    static const GXColorS10 l_arrowTevZero = {0x00, 0x00, 0x00, 0x00};
    J3DGXColorS10 color = l_arrowTevZero;
    daAlink_c* link = daAlink_getAlinkActorClass();
    const bool frozen = (mArrowBits & objects::kArrowBitFrozen) != 0;
    const bool bombModel = mpModel == mpArrowBombModel && mpArrowBombModel != NULL;
    if (bombModel) {
        if (frozen && link != NULL) {
            color.r = link->getFreezeR();
            color.g = link->getFreezeG();
            color.b = link->getFreezeB();
        } else {
            color.r = mArrowFlash;
        }
        mpModel->getModelData()->getMaterialNodePointer(1)->setTevColor(1, &color);
    }
    if (frozen && link != NULL) {
        tevStr.TevColor.r = link->getFreezeR();
        tevStr.TevColor.g = link->getFreezeG();
        tevStr.TevColor.b = link->getFreezeB();
    }

    g_env_light.setLightTevColorType_MAJI(mpModel, &tevStr);
    mDoExt_modelUpdateDL(mpModel);

    if (bombModel) {
        // The model data is shared with every real bomb arrow: put it back (:1121-1126).
        color.r = 0;
        color.g = 0;
        color.b = 0;
        mpModel->getModelData()->getMaterialNodePointer(1)->setTevColor(1, &color);
    }
    return 1;
}

/* ---- events ---------------------------------------------------------------------------------- */

void daMpGhost_c::eventArrow(u8 i_event, const u8* i_blob, u8 i_blobLen) {
    dusk::mp::Reader r(i_blob, i_blobLen);
    arrowInitSound();
    const s8 reverb = dComIfGp_getReverb(fopAcM_GetRoomNo(this));

    switch (static_cast<objects::ObjEventId>(i_event)) {
    case objects::ObjEventId::Stuck: {
        objects::ArrowStuckEvent ev;
        if (i_blob == NULL || !objects::read_arrow_stuck(r, ev)) {
            Log.warn("[mp-obj] arrow ghost proc={}: malformed Stuck event ({} bytes)",
                fopAcM_GetID(this), i_blobLen);
            return;
        }
        // Snap to the final pose at once; the stream (and its forced samples) holds it after.
        current.pos.set(ev.pose.pos[0], ev.pose.pos[1], ev.pose.pos[2]);
        old.pos = current.pos;
        shape_angle.set(ev.pose.angle[0], ev.pose.angle[1], ev.pose.angle[2]);
        current.angle = shape_angle;
        mArrowState = ev.state;
        mArrowStuck = true;
        arrowSyncModel(false);
        if (ev.state == objects::kArrowStateSlingHit) {
            // procSlingHitInit: the hit mark, then the pellet is gone (draw skips param 8).
            dComIfGp_setHitMark(9, NULL, &current.pos, &shape_angle, NULL, 0);
            mArrowSound.startSound(Z2SE_HIT_PACHINKO, ev.polySound, reverb);
            mArrowGone = true;
        } else if (!mArrowGone && mVisible) {
            mArrowSound.startSound(Z2SE_HIT_AL_ARROW_STICK, ev.polySound, reverb);
        }
        Log.info("[mp-obj] arrow ghost stuck proc={} authority={} state={} pos=({:.1f}, {:.1f}, "
                 "{:.1f}) angle=({}, {}, {}) polySound={}",
            fopAcM_GetID(this), mAuthority, ev.state, current.pos.x, current.pos.y, current.pos.z,
            shape_angle.x, shape_angle.y, shape_angle.z, ev.polySound);
        break;
    }
    case objects::ObjEventId::Explode: {
        objects::ArrowExplodeEvent ev;
        if (i_blob == NULL || !objects::read_arrow_explode(r, ev)) {
            Log.warn("[mp-obj] arrow ghost proc={}: malformed Explode event ({} bytes)",
                fopAcM_GetID(this), i_blobLen);
            return;
        }
        const cXyz pos(ev.pos[0], ev.pos[1], ev.pos[2]);
        mArrowGone = true;
        Log.info("[mp-obj] arrow ghost explode proc={} authority={} pos=({:.1f}, {:.1f}, {:.1f}) "
                 "water={}",
            fopAcM_GetID(this), mAuthority, pos.x, pos.y, pos.z, ev.water);
        daMpGhost_spawnRemoteExplosion(pos, ev.water != 0, mAuthority);
        break;
    }
    default:
        break;
    }
}
