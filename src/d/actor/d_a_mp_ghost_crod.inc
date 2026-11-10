/* Dominion Rod ball (0x08): the ghost side. Included at the end of d_a_mp_ghost.cpp.
 * Spec: .claude/future-plans/mp-object-channel.md §2.5 (phase 1 is the ball only; statues are
 * phase 2). The owner side is src/dusk/multiplayer/replication/object_capture_crod.cpp.
 *
 * Draws the game's own ball the way daCrod_c::draw does (d_a_crod.cpp:340-352): model 0x22 out of
 * the resident "Alink" archive, its BCK, BRK and BTK, and the same MAJI lighting. It never creates
 * fpcNm_CROD_e and never writes to the local Link: a real daCrod_c reads the local rod matrix and
 * calls returnCopyRod on the local Link (d_a_crod.cpp:104-106, 281-286).
 *
 * Not drawn: the ball's point light (daCrod_c::mLight, dKy_plight_set). The ghost would have to
 * cut it in its destructor, which is not this file's; a light left registered after the ghost is
 * freed would be a dangling pointer in the environment's light table. */

#include "SSystem/SComponent/c_counter.h"

namespace {

/// g_Counter.mCounter0 on the pass that last advanced the shared BRK / BTK frame.
u32 l_crodSharedAnmPass = 0xFFFFFFFF;

/**
 * True while the local Link has a daCrod_c of his own, which already advances the shared BRK / BTK
 * every tick (d_a_crod.cpp:332-333). A second frame controller on the same J3DAnm would run it at
 * double speed. Read-only: nothing on the local Link is changed.
 *
 * The ball exists while the rod is the equipped item (setItemActor, d_a_alink.cpp:14229-14239),
 * and while thrown it stays alive in mCopyRodAcKeep even after an unequip (d_a_crod.cpp:281-288).
 */
bool crod_local_ball_exists() {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == NULL) {
        return false;
    }
    return link->checkCopyRodEquip() || link->mCopyRodAcKeep.getID() != fpcM_ERROR_PROCESS_ID_e;
}

}  // namespace

int daMpGhost_c::createHeapCrod() {
    // fopAcM_entrySolidHeap may call this more than once; start clean every time.
    mpModel = NULL;
    mpCrodBrk = NULL;
    mpCrodBtk = NULL;
    mCrodBckId = 0;
    mCrodAnmEntered = false;
    mCrodDrawnLogged = false;

    /* daCrod_c::createHeap loads all of this from daAlink_c::getAlinkArcName() with no resLoad of
     * its own (d_a_crod.cpp:32-49), so the ball lives in the same resident archive as the arrow.
     * Still checked rather than assumed, as the debug kind does: a missing archive fails this
     * create cleanly (and the channel retries, then gives up) instead of dereferencing NULL. */
    const char* arc = daAlink_c::getAlinkArcName();
    if (dComIfG_getObjectResInfo(arc) == NULL) {
        Log.warn("[mp-obj] ghost rodball: archive '{}' is not resident; no ghost", arc);
        return FALSE;
    }
    J3DModelData* modelData = static_cast<J3DModelData*>(dComIfG_getObjectRes(arc, CROD_RES_BMD));
    if (modelData == NULL) {
        Log.warn("[mp-obj] ghost rodball: '{}' has no ball model 0x{:X}", arc, static_cast<int>(CROD_RES_BMD));
        return FALSE;
    }
    // daCrod_c::createHeap's own flags (d_a_crod.cpp:35).
    mpModel = mDoExt_J3DModel__create(modelData, 0x80000, 0x11000284);
    if (mpModel == NULL) {
        return FALSE;
    }

    J3DAnmTransform* bck =
        static_cast<J3DAnmTransform*>(dComIfG_getObjectRes(arc, CROD_BCK_WAIT_A_T));
    if (bck == NULL) {
        Log.warn("[mp-obj] ghost rodball: '{}' has no ball BCK {}", arc, static_cast<int>(CROD_BCK_WAIT_A_T));
        mpModel = NULL;
        return FALSE;
    }
    // The J3DMtxCalc this allocates must be in this solid heap: no later init may allocate.
    if (!mCrodBck.init(bck, TRUE, -1, 1.0f, 0, -1, false)) {
        mpModel = NULL;
        return FALSE;
    }
    mCrodBckId = CROD_BCK_WAIT_A_T;

    // The ball is ~20 units across (its AT capsule radius, d_a_crod.cpp:61-72).
    fopAcM_SetMin(this, -40.0f, -40.0f, -40.0f);
    fopAcM_SetMax(this, 40.0f, 40.0f, 40.0f);
    return TRUE;
}

/* The BRK / BTK go onto the SHARED model data, exactly as daCrod_c::create does
 * (d_a_crod.cpp:89-97): idempotent when the local Link's own ball already did it (the same
 * J3DAnm pointers). Done from execute, never from createHeap: entryTexMtxAnimator allocates a
 * J3DTexMtx for any material that lacks one (J3DMaterialAttach.cpp:156-159), and one allocated
 * in this ghost's solid heap would be left dangling in the shared model data when the ghost dies.
 */
void daMpGhost_c::crodEnterMaterialAnm() {
    mCrodAnmEntered = true;
    const char* arc = daAlink_c::getAlinkArcName();
    if (mpModel == NULL || dComIfG_getObjectResInfo(arc) == NULL) {
        return;
    }
    J3DModelData* modelData = mpModel->getModelData();
    mpCrodBrk = static_cast<J3DAnmTevRegKey*>(dComIfG_getObjectRes(arc, CROD_RES_BRK));
    if (mpCrodBrk != NULL) {
        mpCrodBrk->searchUpdateMaterialID(modelData);
        modelData->entryTevRegAnimator(mpCrodBrk);
    }
    mpCrodBtk = static_cast<J3DAnmTextureSRTKey*>(dComIfG_getObjectRes(arc, CROD_RES_BTK));
    if (mpCrodBtk != NULL) {
        mpCrodBtk->searchUpdateMaterialID(modelData);
        modelData->entryTexMtxAnimator(mpCrodBtk);
    }
    Log.info("[mp-obj] ghost proc {} (rodball) authority={} material anims brk={} btk={}",
        fopAcM_GetID(this), mAuthority, mpCrodBrk != NULL ? "ok" : "MISSING",
        mpCrodBtk != NULL ? "ok" : "MISSING");
}

void daMpGhost_c::executeCrod() {
    if (mpModel == NULL) {
        return;
    }
    if (!mCrodAnmEntered) {
        crodEnterMaterialAnm();
    }

    // The owner's BCK, as daCrod_c::setBckAnm switches it (d_a_crod.cpp:150-155).
    if (mBlobLen >= CROD_STATE_LEN) {
        const u8 want = mBlob[CROD_STATE_BCK];
        if (want != mCrodBckId && want >= CROD_BCK_FIRST && want <= CROD_BCK_LAST) {
            J3DAnmTransform* bck = static_cast<J3DAnmTransform*>(
                dComIfG_getObjectRes(daAlink_c::getAlinkArcName(), want));
            if (bck != NULL && mCrodBck.init(bck, TRUE, -1, 1.0f, 0, -1, true)) {
                Log.info("[mp-obj] ghost proc {} (rodball) bck {} -> {} phase={}",
                    fopAcM_GetID(this), mCrodBckId, want, mBlob[CROD_STATE_PHASE]);
                mCrodBckId = want;
            }
        }
    }
    mCrodBck.play();

    /* The BRK / BTK frames live on the shared J3DAnm objects. Advance them at most once per pass
     * across every rod-ball ghost, and not at all while the local Link's own ball does it. */
    if (!crod_local_ball_exists() && l_crodSharedAnmPass != g_Counter.mCounter0) {
        l_crodSharedAnmPass = g_Counter.mCounter0;
        daAlink_c::simpleAnmPlay(mpCrodBrk);
        daAlink_c::simpleAnmPlay(mpCrodBtk);
    }
}

int daMpGhost_c::drawCrod() {
    if (mpModel == NULL) {
        return 1;
    }
    if (!mCrodDrawnLogged) {
        mCrodDrawnLogged = true;
        Log.info("[mp-obj] ghost proc {} (rodball) authority={} first drawn pos=({:.1f}, {:.1f}, "
                 "{:.1f}) phase={} bck={}",
            fopAcM_GetID(this), mAuthority, current.pos.x, current.pos.y, current.pos.z,
            mBlobLen >= CROD_STATE_LEN ? mBlob[CROD_STATE_PHASE] : 0, mCrodBckId);
    }

    // daCrod_c::draw's own lighting (d_a_crod.cpp:348-350).
    g_env_light.settingTevStruct(0, &current.pos, &tevStr);
    g_env_light.setLightTevColorType_MAJI(mpModel, &tevStr);

    /* The BCK is entered onto the SHARED model data's root joint (mDoExt_bckAnm::entry), the same
     * joint the local Link's own ball enters its BCK on. So enter ours immediately before this
     * model's calc and put back whatever was there afterwards: the local ball, whichever side of
     * this draw it is drawn on, then sees its own animation and frame, never ours. */
    J3DModelData* modelData = mpModel->getModelData();
    J3DJoint* root = modelData->getJointNodePointer(0);
    J3DMtxCalc* savedCalc = root->getMtxCalc();
    J3DAnmTransform* anm = mCrodBck.getBckAnm();
    const f32 savedFrame = anm != NULL ? anm->getFrame() : 0.0f;
    if (anm != NULL) {
        mCrodBck.entry(modelData);
    }
    mDoExt_modelUpdateDL(mpModel);
    root->setMtxCalc(savedCalc);
    if (anm != NULL) {
        anm->setFrame(savedFrame);
    }
    return 1;
}

void daMpGhost_c::eventCrod(u8 i_event, const u8* i_blob, u8 i_blobLen) {
    const objects::ObjEventId ev = static_cast<objects::ObjEventId>(i_event);
    u16 proc = 0;
    if (i_blob != NULL && i_blobLen >= CROD_EVENT_HIT_LEN) {
        proc = static_cast<u16>((i_blob[CROD_EVENT_PROC_HI] << 8) | i_blob[CROD_EVENT_PROC_LO]);
    }
    switch (ev) {
    case objects::ObjEventId::Stuck: {
        /* Phase 1: the ball went into a statue. The owner hides it this tick and despawns it a few
         * ticks later; the statue itself is not driven here (phase 2b, spec §2.5). */
        const int boss =
            i_blob != NULL && i_blobLen >= CROD_EVENT_POSSESS_LEN ? i_blob[CROD_EVENT_BOSS] : -1;
        Log.info("[mp-obj] ghost proc {} (rodball) event possess statue=0x{:03X} boss={} "
                 "pos=({:.1f}, {:.1f}, {:.1f}) (#{})",
            fopAcM_GetID(this), proc, boss, current.pos.x, current.pos.y, current.pos.z,
            mEventCount);
        break;
    }
    case objects::ObjEventId::Hit:
        Log.info("[mp-obj] ghost proc {} (rodball) event hit target=0x{:03X} "
                 "pos=({:.1f}, {:.1f}, {:.1f}) (#{})",
            fopAcM_GetID(this), proc, current.pos.x, current.pos.y, current.pos.z, mEventCount);
        break;
    case objects::ObjEventId::Caught:
        Log.info("[mp-obj] ghost proc {} (rodball) event caught pos=({:.1f}, {:.1f}, {:.1f}) (#{})",
            fopAcM_GetID(this), current.pos.x, current.pos.y, current.pos.z, mEventCount);
        break;
    default:
        Log.info("[mp-obj] ghost proc {} (rodball) event {} (unknown to this kind) (#{})",
            fopAcM_GetID(this), i_event, mEventCount);
        break;
    }
}
