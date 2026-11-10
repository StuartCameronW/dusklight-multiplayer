/* Gale Boomerang (ObjKind::Boomerang 0x07): the ghost side. Included at the end of
 * d_a_mp_ghost.cpp. Spec: .claude/future-plans/mp-object-channel.md §2.3; the blob layout is in
 * src/dusk/multiplayer/replication/object_blob_boom.hpp, the owner side in object_capture_boom.cpp.
 *
 * Draws what a thrown daBoomerang_c draws (d_a_boomerang.cpp:487-541, the param != 0 branch): the
 * spinning blade (Alink 0x1F), the tornado (0x34 with bck 0x13 and btk 0x48), the two trace
 * particles that ride the blade and the wind particle (setEffect, :833-836), the tornado's level
 * sound and the blade's whoosh. Not drawn: the lock-on reticles (the thrower's own UI, spec §2.3)
 * and the ground/water dust (m_papo, 0x256/0x257), which needs the boomerang's own BG checks.
 *
 * ★ Never a daBoomerang_c: its procMove calls the LOCAL player's returnBoomerang(), which changes
 * his equipment (:1113), and enemies look the boomerang up BY NAME (fpcNm_BOOMERANG_e). This ghost
 * touches no player and has no collider.
 *
 * ★ Shared model data. Model 0x34's J3DModelData is the resident archive's, the same one the local
 * player's own boomerang uses, and that boomerang writes three things INTO it: the root joint's
 * mtxCalc (its bck), joint 4's callback (daBoomeang_windModelCallBack, which casts the model's
 * user area to daBoomerang_c and would read garbage off this ghost), and the btk's frame. So this
 * ghost installs its own three only around its own calc / draw and puts the previous ones back
 * (calcBoomShippu, drawBoom). Single-threaded, so nothing else can calc in between. */

#include "JSystem/J3DGraphBase/J3DSys.h"
#include "JSystem/JParticle/JPAEmitter.h"
#include "Z2AudioLib/Z2SeMgr.h"
#include "dusk/multiplayer/replication/object_blob_boom.hpp"

namespace {

/// "Alink" resource indices, as daBoomerang_c::createHeap / create load them
/// (d_a_boomerang.cpp:1406-1513).
constexpr int l_boomBladeBmd = 0x1F;
constexpr int l_boomShippuBmd = 0x34;
constexpr int l_boomShippuBck = 0x13;
constexpr int l_boomWindBtk = 0x48;
/// The tornado joint that is held upright (daBoomerang_c::create, :1510).
constexpr u16 l_boomWindJoint = 4;
/// setRotAngle (:728).
constexpr s16 l_boomSpinStep = 0x1F00;
/// setEffect's particles (:834-836).
constexpr u16 l_boomFxName[3] = {0x1FE, 0x1FF, 0x24C};

/// daBoomeang_windModelCallBack + daBoomerang_c::windModelCallBack (:463-485), for a ghost:
/// undo the pose's rotation on joint 4 down to its yaw, so the funnel stands upright while the
/// blade banks. Installed on the shared joint only for the duration of this ghost's own calc, so
/// the model's user area is always a daMpGhost_c.
int l_boomWindCallBack(J3DJoint* i_joint, int i_param) {
    (void)i_joint;
    J3DModel* model = j3dSys.getModel();
    fopAc_ac_c* actor = model != NULL ? reinterpret_cast<fopAc_ac_c*>(model->getUserArea()) : NULL;
    if (i_param != 0 || actor == NULL) {
        return 1;
    }
    const csXyz& angle = actor->shape_angle;
    mDoMtx_stack_c::YrotS(angle.y);
    mDoMtx_stack_c::ZrotM(-angle.z);
    mDoMtx_stack_c::XrotM(-angle.x);
    mDoMtx_stack_c::YrotM(-angle.y);
    mDoMtx_stack_c::concat(J3DSys::mCurrentMtx);

    mDoMtx_stack_c::get()[0][3] = J3DSys::mCurrentMtx[0][3];
    mDoMtx_stack_c::get()[1][3] = J3DSys::mCurrentMtx[1][3];
    mDoMtx_stack_c::get()[2][3] = J3DSys::mCurrentMtx[2][3];

    cMtx_copy(mDoMtx_stack_c::get(), J3DSys::mCurrentMtx);
    model->setAnmMtx(l_boomWindJoint, mDoMtx_stack_c::get());
    return 1;
}

/// Advance a looping frame by one and wrap it, as daAlink_c::simpleAnmPlay does for a loop.
f32 l_boomStepFrame(f32 i_frame, s16 i_frameMax) {
    f32 frame = i_frame + 1.0f;
    if (i_frameMax > 0 && frame >= static_cast<f32>(i_frameMax)) {
        frame -= static_cast<f32>(i_frameMax);
    }
    return frame;
}

}  // namespace

int daMpGhost_c::createHeapBoom() {
    // createHeap can be retried with a bigger heap, so every field starts over here.
    mpBoomBlade = NULL;
    mpBoomWindBtk = NULL;
    mBoomShippuFrame = 0.0f;
    mBoomWindBtkFrame = 0.0f;
    mBoomShippuSize = 1.0f;
    mBoomSpin = 0;
    mBoomAge = 0;
    mBoomBits = 0;
    mBoomLocks = 0;
    mBoomCaught = false;
    mBoomLogged = false;
    mBoomFx[0] = mBoomFx[1] = mBoomFx[2] = 0;
    mBoomCalcTick = 0xFFFFFFFF;

    // Checked rather than assumed, as the debug kind does: [unverified] that "Alink" stays mounted
    // while the LOCAL player is a wolf.
    const char* arc = daAlink_c::getAlinkArcName();
    if (dComIfG_getObjectResInfo(arc) == NULL) {
        Log.warn("[mp-obj] ghost: archive '{}' is not resident; no boomerang ghost", arc);
        return FALSE;
    }

    J3DModelData* bladeData = static_cast<J3DModelData*>(dComIfG_getObjectRes(arc, l_boomBladeBmd));
    J3DModelData* shippuData =
        static_cast<J3DModelData*>(dComIfG_getObjectRes(arc, l_boomShippuBmd));
    J3DAnmTransform* shippuBck =
        static_cast<J3DAnmTransform*>(dComIfG_getObjectRes(arc, l_boomShippuBck));
    J3DAnmTextureSRTKey* windBtk =
        static_cast<J3DAnmTextureSRTKey*>(dComIfG_getObjectRes(arc, l_boomWindBtk));
    if (bladeData == NULL || shippuData == NULL || shippuBck == NULL || windBtk == NULL ||
        shippuData->getJointNum() <= l_boomWindJoint)
    {
        Log.warn("[mp-obj] ghost: boomerang resources missing from '{}'", arc);
        return FALSE;
    }

    // The flags daBoomerang_c::createHeap uses (:1408, :1415).
    mpBoomBlade = mDoExt_J3DModel__create(bladeData, 0x80000, 0x11000084);
    if (mpBoomBlade == NULL) {
        return FALSE;
    }
    mpModel = mDoExt_J3DModel__create(shippuData, 0, 0x11000284);
    if (mpModel == NULL) {
        return FALSE;
    }
    mpModel->setUserArea(reinterpret_cast<uintptr_t>(this));
    if (mBoomShippuBck.init(shippuBck, 0, 2, 1.0f, 0, -1, false) == 0) {
        return FALSE;
    }

    /* The btk onto the shared model data, exactly as daBoomerang_c::create does (:1508-1509).
     * Idempotent: the material animators already exist in the archive data and setTexMtxAnm
     * copies by value, so this allocates nothing in this (temporary) heap. A second boomerang, real
     * or ghost, installs the very same btk. */
    windBtk->searchUpdateMaterialID(shippuData);
    shippuData->entryTexMtxAnimator(windBtk);
    mpBoomWindBtk = windBtk;

    // The tornado reaches the ground below the blade (the wind cylinder spans down to it,
    // setEffect :911-919), and the box turns with the pose: generous on every axis.
    fopAcM_SetMin(this, -600.0f, -700.0f, -600.0f);
    fopAcM_SetMax(this, 600.0f, 400.0f, 600.0f);

    mBoomSound.init(&current.pos, 2);
    return TRUE;
}

void daMpGhost_c::setBoomBladeMatrix() {
    // setMoveMatrix's second half (d_a_boomerang.cpp:717-722).
    mDoMtx_stack_c::transS(current.pos.x, current.pos.y, current.pos.z);
    mDoMtx_stack_c::ZXYrotM(shape_angle.x, shape_angle.y, shape_angle.z);
    mDoMtx_stack_c::YrotM(mBoomSpin);
    mDoMtx_stack_c::XrotM(0x7FFF);
    mpBoomBlade->setBaseTRMtx(mDoMtx_stack_c::get());
}

void daMpGhost_c::calcBoomShippu() {
    J3DModelData* data = mpModel->getModelData();
    J3DJoint* root = data->getJointNodePointer(0);
    J3DJoint* windJoint = data->getJointNodePointer(l_boomWindJoint);
    J3DAnmTransform* bck = mBoomShippuBck.getBckAnm();

    // What the local boomerang (or nobody) left in the shared data.
    J3DMtxCalc* savedCalc = root->getMtxCalc();
    J3DJointCallBack savedCallBack = windJoint->getCallBack();
    const f32 savedFrame = bck->getFrame();

    // procMove (:1102-1105) and setMoveMatrix (:714-715).
    Vec scale = {mBoomShippuSize, 1.0f, mBoomShippuSize};
    mpModel->setBaseScale(scale);
    mBoomShippuBck.entry(data, mBoomShippuFrame);
    windJoint->setCallBack(l_boomWindCallBack);
    mpModel->calc();

    windJoint->setCallBack(savedCallBack);
    root->setMtxCalc(savedCalc);
    bck->setFrame(savedFrame);
}

void daMpGhost_c::setBoomEffect() {
    // setEffect's thrown branch (:833-836): two traces that ride the blade, one at the centre.
    for (int i = 0; i < 3; i++) {
        mBoomFx[i] = dComIfGp_particle_set(mBoomFx[i], l_boomFxName[i], &current.pos, &tevStr);
        if (i < 2) {
            JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(mBoomFx[i]);
            if (emitter != NULL) {
                emitter->setGlobalRTMatrix(mpBoomBlade->getBaseTRMtx());
            }
        }
    }
}

void daMpGhost_c::executeBoom() {
    if (mpBoomBlade == NULL || mpModel == NULL) {
        return;
    }

    // The pushed blob: [u16 age][u8 size x64][u8 bits] (+ the lock list on the spawn sample).
    dusk::mp::objects::BoomBlob blob;
    dusk::mp::Reader reader(mBlob, mBlobLen);
    const bool haveBlob = dusk::mp::objects::read_boom_blob(reader, blob);
    if (haveBlob) {
        u8 locks = 0;
        if (!mBoomLogged && reader.read_u8(locks)) {
            mBoomLocks = locks;
        }
        mBoomShippuSize = blob.shippuSize;
        mBoomAge = blob.age;
    }

    if (!mBoomLogged) {
        mBoomLogged = true;
        mBoomBits = haveBlob ? blob.bits : 0;
        Log.info("[mp-obj] ghost boomerang proc={} authority={} appear age={} returning={} "
                 "locks={} pos=({:.1f}, {:.1f}, {:.1f})",
            fopAcM_GetID(this), mAuthority, mBoomAge,
            (mBoomBits & dusk::mp::objects::kBoomReturning) != 0 ? 1 : 0, mBoomLocks, current.pos.x,
            current.pos.y, current.pos.z);
    } else if (haveBlob) {
        if ((blob.bits & dusk::mp::objects::kBoomReturning) != 0 &&
            (mBoomBits & dusk::mp::objects::kBoomReturning) == 0)
        {
            Log.info("[mp-obj] ghost boomerang proc={} authority={} return age={} "
                     "pos=({:.1f}, {:.1f}, {:.1f})",
                fopAcM_GetID(this), mAuthority, mBoomAge, current.pos.x, current.pos.y,
                current.pos.z);
        }
        mBoomBits = blob.bits;
    }

    // The game's own per-tick animation, run here: setRotAngle (:725-732), the tornado's bck
    // (:708-714) and btk (simpleAnmPlay, :706).
    const s16 prevSpin = mBoomSpin;
    mBoomSpin -= l_boomSpinStep;
    mBoomShippuFrame = l_boomStepFrame(mBoomShippuFrame, mBoomShippuBck.getBckAnm()->getFrameMax());
    mBoomWindBtkFrame = l_boomStepFrame(mBoomWindBtkFrame, mpBoomWindBtk->getFrameMax());

    const bool shown = mVisible && !mBoomCaught;
    if (shown) {
        setBoomBladeMatrix();
        setBoomEffect();
        if (prevSpin >= 0 && mBoomSpin < 0) {
            fopAcM_seStart(this, JA_SE_LK_BOOM_FLY, 0);
        }
        mBoomSound.startLevelSound(Z2SE_BOOM_TORNADO, 0, -1);
    } else {
        /* Not refreshed, so dPa_control_c::level_c::execute cuts them next frame (the way the
         * real boomerang's end when it is caught); forget the ids so a later show starts fresh. */
        mBoomFx[0] = mBoomFx[1] = mBoomFx[2] = 0;
    }
    mBoomSound.framework(0, dComIfGp_getReverb(fopAcM_GetRoomNo(this)));
}

int daMpGhost_c::drawBoom() {
    if (mpBoomBlade == NULL || mpModel == NULL || mBoomCaught) {
        return 1;
    }
    /* Calc once per sim tick (the first draw after it), with the pose the channel pushed AFTER this
     * tick's execute: calcing in executeBoom would put the tornado's joints a tick behind its base
     * matrix. Interpolated frames reuse that calc, as they do for the real boomerang. */
    const bool firstDrawThisTick = mBoomCalcTick != mExecuteCount;
    mBoomCalcTick = mExecuteCount;

    // daBoomerang_c::draw (:526-534), minus the mirror.
    g_env_light.settingTevStruct(0, &current.pos, &tevStr);
    setBoomBladeMatrix();
    g_env_light.setLightTevColorType_MAJI(mpBoomBlade, &tevStr);
    mDoExt_modelUpdateDL(mpBoomBlade);

    if (firstDrawThisTick) {
        calcBoomShippu();
    }
    g_env_light.setLightTevColorType_MAJI(mpModel, &tevStr);
    // The shared btk shows THIS ghost's frame while its materials are calculated.
    const f32 savedBtkFrame = mpBoomWindBtk->getFrame();
    mpBoomWindBtk->setFrame(mBoomWindBtkFrame);
    mDoExt_modelEntryDL(mpModel);
    mpBoomWindBtk->setFrame(savedBtkFrame);
    return 1;
}

void daMpGhost_c::eventBoom(u8 i_event, const u8* i_blob, u8 i_blobLen) {
    u16 age = 0;
    if (i_blob != NULL && i_blobLen >= 2) {
        dusk::mp::Reader reader(i_blob, i_blobLen);
        reader.read_u16(age);
    }
    if (i_event == static_cast<u8>(dusk::mp::objects::ObjEventId::Caught)) {
        // Back in the thrower's hand: the puppet draws it there from now on (held item), so the
        // flying one disappears at once rather than waiting for the despawn.
        mBoomCaught = true;
        mBoomFx[0] = mBoomFx[1] = mBoomFx[2] = 0;
        Log.info("[mp-obj] ghost boomerang proc={} authority={} caught age={} "
                 "pos=({:.1f}, {:.1f}, {:.1f})",
            fopAcM_GetID(this), mAuthority, age, current.pos.x, current.pos.y, current.pos.z);
    } else {
        Log.info("[mp-obj] ghost boomerang proc={} authority={} event {} age={}",
            fopAcM_GetID(this), mAuthority, i_event, age);
    }
}
