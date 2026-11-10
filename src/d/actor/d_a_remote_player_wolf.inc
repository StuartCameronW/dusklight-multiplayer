/**
 * d_a_remote_player_wolf.inc
 * Remote player puppet: the WOLF body (wolf piece B, .claude/future-plans/puppet-wolf.md sections
 * 1, 2 and 6 B).
 *
 * A puppet is BUILT as a wolf when its create parameter's top byte is WOLF_OUTFIT_PARAM (0xFE).
 * The network layer re-creates the puppet when the sender's form flips, as it does for an outfit
 * change, so a puppet is one body for its whole life and there is no second model path to keep in
 * step with the first.
 *
 * What a wolf puppet is, against daAlink_c::changeWolf (d_a_alink_wolf.inc:147-310) and his draw
 * (d_a_alink.cpp:19481-19642):
 *   - "Wmdl" mounted PRIVATELY in place of the outfit archive (mountOwnArchive, via ownArcName()).
 *     Link installs daAlink_wolfModelCallBack on HIS copy's joint tree and frees his heap on every
 *     transform, so his copy is never read (memory: Link's model reload frees his models).
 *   - Body = res 14 with flags 0x20200, plus the PC wl_eyeball LOD fix; four shackle chains =
 *     res 15.
 *   - 40 joints. Under calc on 0 and 15, upper calc on 3 (changeModelDataDirectWolf,
 *     d_a_alink_swindow.inc:212-225).
 *   - Eyes: material anims on body materials 4/5, the WL_FMABA01 blink. No face, head or hands.
 *   - Locomotion is NOT reconstructed: a wolf sender keeps the action override on in every proc
 *     (piece A), and the body plays the received WL_* BCKs through selectActionAnimation(). When
 *     the override goes to none (a demo-archive BCK the puppet cannot load) the body HOLDS its last
 *     pose. It never falls back to the human gait.
 *   - Joint control: jointControll's wolf branch (d_a_alink.cpp:2150-2191) and changeWolfBlendRate
 *     (d_a_alink_wolf.inc:1853-1867), from a joint callback — never after calc (memory: post-calc
 *     joint writes are invisible).
 *   - The tail spring (setWolfTailAngle) and the chain spring (setWolfChainPos) run locally on the
 *     puppet's own yaw and joints, as the cap sway does. Neither is on the wire.
 *   - Draw: tev type 9 (behind draw()'s local-player early-out), the dark draw list, the chain
 *     while the sender's form byte says M_011, and the sword/sheath/shield on his back from the
 *     existing equip byte, placed by setWolfItemMatrix's offsets off joint 2.
 *
 * Skipped, because a wolf has none of it: hands, head, face, held items, bottle, attention set,
 * gait/idle BCKs, heavy boots, gear fx, clawshot, iron ball, sword fx, foot fx, foot IK (NoFootIk
 * is always set for a wolf), friendly fire (wolf hits are not sword capsules), the cap sway, the
 * eye aim. Skipped for now and named in the spec: Midna (piece D), 0x109 / 0x10A held models, the
 * light-drop tint, wolf foot IK, breath voices.
 */

#include "SSystem/SComponent/c_m3d_g_pla.h"

namespace {

/// l_wArcName (d_a_alink.cpp:90).
const char l_wolfArcName[] = "Wmdl";
/// dRes_INDEX_WMDL_BMD_WL_e / _WL_KUSARI_e, the literals changeWolf uses (wolf.inc:150, :169).
const s32 l_wolfBodyResIdx = 14;
const s32 l_wolfChainResIdx = 15;
const int l_wolfChainNum = 4;

/// changeModelDataDirectWolf's three calculator joints (d_a_alink_swindow.inc:219-221).
const u16 l_wolfUnderRootJointNo = 0;
const u16 l_wolfUpperRootJointNo = 3;
const u16 l_wolfUnderLegJointNo = 15;
/// The joints jointControll's wolf branch and wolfModelCallBack act on (d_a_alink.cpp:2150-2191,
/// :2530-2540). Joint 36 (setWolfFootMatrix) is the deferred wolf foot IK and is not hooked.
const u16 l_wolfBackJointNo = 2;   // WL_JNT_BACKBONE2: twist, and the equipment rides it
const u16 l_wolfNeckJointNo = 3;   // WL_JNT_NECK: slope pitch / howl lean, and the UPPER_2 ratio
const u16 l_wolfHeadJointNo = 4;   // WL_JNT_HEAD: the neck turn
const u16 l_wolfTongueJointNo = 7; // WL_JNT_TONGUE2: the UPPER_2 ratio back off
const u16 l_wolfFLegJointNo = 17;  // WL_JNT_FLEGL3: the chain hangs off it (setWolfChainPos)
const u16 l_wolfWaistJointNo = 26; // WL_JNT_WAIST: the twist, mirrored
const u16 l_wolfTailJointNo[3] = {37, 38, 39};
const u16 l_wolfJointNum = 40;

/// changeWolf's field_0x064C->getMaterialNodePointer(4/5)->setMaterialAnm (wolf.inc:226-227).
const u16 l_wolfEyeMaterialNo[2] = {4, 5};

/// setUpperAnimeMorf's wolf range: field_0x30a8 = 3, field_0x30aa = 15, field_0x30c6 = 40
/// (changeWolf, wolf.inc:262-264).
const u16 l_wolfUpper2MorfStartJoint = 3;
const u16 l_wolfUpper2MorfEndJoint = 15;

/// player_state.hpp's kPlayerFormChanging / kPlayerFormChain, mirrored because this actor does not
/// include the wire layout (the l_actNone arrangement). ★ If either is renumbered there, these must
/// move with it: the compiler cannot see the pair.
const u8 l_wolfFormChanging = 1 << 2;
const u8 l_wolfFormChain = 1 << 4;

const u16 l_wolfLogMax = 32;

/// setUpperAnimeMorf (d_a_alink.cpp:7326-7341) with the wolf's joint numbers.
void wolf_upper2_morf(mDoExt_MtxCalcOldFrame* i_oldFrame, f32 i_morf, u16 i_jointNum) {
    if (i_oldFrame == NULL || !(i_morf > 0.0f)) {
        return;
    }
    if (i_oldFrame->getOldFrameRate() > 0.1f && i_oldFrame->getOldFrameStartJoint() == 0) {
        i_oldFrame->initOldFrameMorf(i_morf, 0, i_jointNum);
    } else {
        i_oldFrame->initOldFrameMorf(i_morf, l_wolfUpper2MorfStartJoint, l_wolfUpper2MorfEndJoint);
    }
}

/// jointControll's three JMAEulerToQuat arms (d_a_alink.cpp:2263-2304): axis 0/1/2 = X/Y/Z.
void wolf_euler_quat(s16 i_axis, s16 i_angle, Quaternion* o_quat) {
    if (i_axis == 0) {
        JMAEulerToQuat(i_angle, 0, 0, o_quat);
    } else if (i_axis == 1) {
        JMAEulerToQuat(0, i_angle, 0, o_quat);
    } else {
        JMAEulerToQuat(0, 0, i_angle, o_quat);
    }
}

}  // namespace

bool daRemotePlayer_c::isWolf() const {
    return static_cast<u8>(fopAcM_GetParam(this) >> daRemotePlayer_outfitParamShift) ==
           WOLF_OUTFIT_PARAM;
}

const char* daRemotePlayer_c::ownArcName() const {
    return isWolf() ? l_wolfArcName : l_outfits[mOutfit].arcName;
}

/* --- build ------------------------------------------------------------------------------------ */

/**
 * createHeap() for a wolf: changeWolf's model half (d_a_alink_wolf.inc:147-310) out of this
 * puppet's own Wmdl mount, then the parts of the human createHeap a wolf shares (equipment, the
 * action override). Fatal only for what a wolf cannot draw without: the body, its calculators and
 * its first pose. Everything else degrades with a line saying what was lost.
 */
int daRemotePlayer_c::createHeapWolf() {
    JKRHeap* heap = JKRGetCurrentHeap();
    const u32 freeBefore = heap != NULL ? heap->getFreeSize() : 0;

    J3DModelData* modelData =
        static_cast<J3DModelData*>(own_archive_idx(mOwnRes, l_wolfBodyResIdx));
    if (modelData == NULL) {
        Log.warn("Puppet {} wolf: no body (res {}) in its private '{}' mount", mPlayerId,
            l_wolfBodyResIdx, l_wolfArcName);
        return 0;
    }

    /* The PC eye fix changeWolf applies to his copy (wolf.inc:152-167). Ours is a different copy,
     * so it is applied again, as the human face's fix is. */
    {
        J3DTexture* tex = modelData->getTexture();
        JUTNameTab* nameTab = modelData->getTextureName();
        if (tex != NULL && nameTab != NULL) {
            for (u16 i = 0; i < tex->getNum(); i++) {
                const char* texName = nameTab->getName(i);
                if (texName != NULL && std::strcmp(texName, "wl_eyeball") == 0) {
                    tex->getResTIMG(i)->maxLOD = 0;
                }
            }
        }
    }

    /* WANM_WAIT's BCK, resident for the puppet's life: the calculators dereference slot 0 without a
     * null check, so something has to be there before the first calc, and it is also what a failed
     * override load falls back to (selectWolfAnimation). It is the WOLF's wait, never a human
     * gait. */
    mWolfWaitAnm.mpUnder = static_cast<J3DAnmTransform*>(
        load_aram_anm(daAlink_c::m_wlAnmDataTable[daAlink_c::WANM_WAIT].m_bckID,
            ANM_FAMILY_TRANSFORM, &mWolfWaitAnm.mpUnderBas));
    mWolfWaitAnm.mpUpper = NULL;
    if (mWolfWaitAnm.mpUnder == NULL) {
        Log.warn("Puppet {} wolf: WANM_WAIT (bck {}) did not load", mPlayerId,
            daAlink_c::m_wlAnmDataTable[daAlink_c::WANM_WAIT].m_bckID);
        return 0;
    }

    model = init_model(modelData, 0x20200, "wolf body");
    if (model == NULL) {
        return 0;
    }

    if (!setupWolfAnimation(modelData)) {
        return 0;
    }

    // Cosmetic and non-fatal, as the human blink is.
    const bool eyes = setupWolfFace(modelData);

    setupWolfJointCallBacks(modelData);

    J3DModelData* chainData =
        static_cast<J3DModelData*>(own_archive_idx(mOwnRes, l_wolfChainResIdx));
    int chains = 0;
    for (int i = 0; i < l_wolfChainNum; i++) {
        mpWlChainModel[i] = init_model(chainData, 0, "wolf chain");
        if (mpWlChainModel[i] != NULL) {
            chains++;
        }
    }
    if (chains != l_wolfChainNum) {
        // All or nothing at draw time (drawWolf), so a partial set is dropped rather than drawn.
        for (int i = 0; i < l_wolfChainNum; i++) {
            mpWlChainModel[i] = NULL;
        }
    }

    /* The sword, sheaths and shields, built exactly as for the human: they come out of the private
     * Alink and shield mounts, which a wolf mounts too. The wooden sword is looked up in the
     * outfit archive, which is Wmdl here, so it is absent — as daAlink_c sets mWoodSwordModel to
     * NULL for a wolf (wolf.inc:259). The warning that names it is expected. */
    setupEquipModels();

    /* The UPPER_2 overlay's state. setupHeldItems() initialises it for the human, and a wolf skips
     * that function; zero from fopAcM_ct would read as BCK 0 rather than "none". */
    mNetUpper2Idx = l_actNone;
    mNetUpper2Frame = 0.0f;
    mNetUpper2Ratio = 0.0f;
    mNetUpper2Morf = 0.0f;
    mUpper2Shown = l_actNone;
    mpUpper2Bck = NULL;
    mUpper2Failed = l_actNone;
    mUpper2AppliedMorf = 0;
    mHeldLogCount = 0;

    for (int i = 0; i < 3; i++) {
        mWolfTailAngle[i] = 0;
        mWolfTailVel[i] = 0;
    }
    mWolfPrevYawValid = false;
    mWolfChainInit = false;
    mWolfRootValid = false;
    mWolfHolding = false;
    mWolfGrab = false;
    mWolfPitch = 0;
    mWolfYawOff = 0;
    mWolfRootRotX = 0;
    mWolfRootRotZ = 0;
    mLoggedWolfSplit = false;
    mLoggedWolfChain = false;
    mLoggedWolfGrab = false;
    mWolfLogCount = 0;
    /* The human "body split live" line reads joints 0/1/16, which are not this skeleton's calc
     * joints, and would print WRONG for a correct wolf. The wolf's own line is in
     * wolfSetMatrix(). */
    mLoggedSplitAnm = true;

    const u32 freeBeforeOverride = heap != NULL ? heap->getFreeSize() : 0;

    /* The override's buffer and six anm heaps, last as for the human. For a wolf this is the
     * whole of locomotion, so a short heap here leaves a puppet holding the wait pose. */
    setupActionAnimation();
    setupTransformFx(); // transform blob (wolf piece C); a wolf puppet loads no fur

    const u32 freeAfter = heap != NULL ? heap->getFreeSize() : 0;
    JKRSolidHeap* dataHeap = mOwnRes.getDataHeap();
    /* Latched by construction (once per build) and permanent. The numbers the spec could only
     * reason about: what a wolf puppet costs, and what the Wmdl mount weighs. */
    Log.info("Puppet {} built as WOLF: actor heap {} -> {} free ({} used; {} before the override "
             "buffer), largest block {} | Wmdl mount {} resources, data heap {} bytes | body {} "
             "joints, {} materials | chains {}/{} | eyes {} | override {}",
        mPlayerId, freeBefore, freeAfter, freeBefore - freeAfter, freeBefore - freeBeforeOverride,
        heap != NULL ? heap->getMaxAllocatableSize(0x20) : 0, mOwnRes.getResNum(),
        dataHeap != NULL ? dataHeap->getHeapSize() : 0, mBodyJointNum,
        modelData->getMaterialNum(), mpWlChainModel[0] != NULL ? l_wolfChainNum : 0,
        l_wolfChainNum, eyes ? "ok" : "MISSING", mActHeapReady ? "ready" : "OFF");
    return 1;
}

/**
 * The wolf's half of setupAnimation(): the same two-calculator rig over one shared old frame, on
 * the wolf's joints — under on 0 and 15, upper on 3 (changeModelDataDirectWolf,
 * d_a_alink_swindow.inc:219-221) — with the 40-entry arrays daAlink_c's morfs use
 * (initOldFrameMorf(.., 0, 40), wolf.inc:1292).
 */
bool daRemotePlayer_c::setupWolfAnimation(J3DModelData* i_modelData) {
    mBodyJointNum = i_modelData->getJointNum();
    if (mBodyJointNum < l_wolfJointNum) {
        Log.warn("Puppet {} wolf body has only {} joints; this is not the {}-joint skeleton the "
                 "joint numbers were read off",
            mPlayerId, mBodyJointNum, l_wolfJointNum);
        return false;
    }

    mpOldTransInfo = JKR_NEW_ARRAY(J3DTransformInfo, mBodyJointNum);
    mpOldQuat = JKR_NEW_ARRAY(Quaternion, mBodyJointNum);
    if (mpOldTransInfo == NULL || mpOldQuat == NULL) {
        return false;
    }
    std::memset(mpOldTransInfo, 0, sizeof(J3DTransformInfo) * mBodyJointNum);
    std::memset(mpOldQuat, 0, sizeof(Quaternion) * mBodyJointNum);

    mpOldFrame = JKR_NEW mDoExt_MtxCalcOldFrame(mpOldTransInfo, mpOldQuat);
    if (mpOldFrame == NULL) {
        return false;
    }

    mpUnderCalc =
        JKR_NEW mDoExt_MtxCalcAnmBlendTblOld(mpOldFrame, daRemotePlayer_anmSlotNum, mAnmPackUnder);
    mpUpperCalc =
        JKR_NEW mDoExt_MtxCalcAnmBlendTblOld(mpOldFrame, daRemotePlayer_anmSlotNum, mAnmPackUpper);
    if (mpUnderCalc == NULL || mpUpperCalc == NULL) {
        return false;
    }

    /* Slot 0 filled before anything calcs. The wolf's wait rate is mWlMove's mIdleAnmSpeed, the
     * one setBlendWolfMoveAnime plays WANM_WAIT at (wolf.inc:911-915). No morf: there is nothing
     * to fade from. */
    setAnm(mWolfWaitAnm, J3DFrameCtrl::EMode_LOOP, -1.0f, daAlinkHIO_wlMove_c0::m.mIdleAnmSpeed,
        0.0f, -1);

    i_modelData->getJointNodePointer(l_wolfUnderRootJointNo)->setMtxCalc(mpUnderCalc);
    i_modelData->getJointNodePointer(l_wolfUpperRootJointNo)->setMtxCalc(mpUpperCalc);
    i_modelData->getJointNodePointer(l_wolfUnderLegJointNo)->setMtxCalc(mpUnderCalc);
    return true;
}

/**
 * The wolf's blink. changeWolf hangs the material anims on body materials 4 and 5
 * (wolf.inc:226-227) and setFaceBasicTexture(FTANM_WL_MABA01) plays the WL_FMABA01 pair
 * (wolf.inc:1296, table d_a_alink.cpp:1002). Same order rule as setupFaceAnimation(): the material
 * anims must exist before the entries, or the entries attach nothing and say so only in their
 * return codes. daNpcF_MatAnm_c for the same reason as the human face (no singleton, no statics).
 *
 * playFaceTextureAnime() then drives mpBlinkBtp/Btk unchanged: his wolf blink chance is the same
 * 0.012 (setFaceBasicBtp, d_a_alink.cpp:8241-8246; 0.02 only in WAIT_WIND, which is not
 * reproduced).
 */
bool daRemotePlayer_c::setupWolfFace(J3DModelData* i_modelData) {
    mpBlinkBtp = NULL;
    mpBlinkBtk = NULL;
    mpEyeMatAnm[0] = NULL;
    mpEyeMatAnm[1] = NULL;

    const u16 materialNum = i_modelData->getMaterialNum();
    for (int i = 0; i < 2; i++) {
        if (l_wolfEyeMaterialNo[i] >= materialNum) {
            Log.warn("Puppet {} wolf body has {} materials; no eye material {}", mPlayerId,
                materialNum, l_wolfEyeMaterialNo[i]);
            return false;
        }
    }

    for (int i = 0; i < 2; i++) {
        mpEyeMatAnm[i] = JKR_NEW daNpcF_MatAnm_c();
        if (mpEyeMatAnm[i] == NULL) {
            Log.warn("Puppet {} wolf: out of heap for eye material anm {}", mPlayerId, i);
            return false;
        }
        i_modelData->getMaterialNodePointer(l_wolfEyeMaterialNo[i])->setMaterialAnm(mpEyeMatAnm[i]);
    }

    J3DAnmTexPattern* btp = static_cast<J3DAnmTexPattern*>(
        load_aram_anm(dRes_ID_ALANM_BTP_WL_FMABA01_e, ANM_FAMILY_TEX_PATTERN));
    J3DAnmTextureSRTKey* btk = static_cast<J3DAnmTextureSRTKey*>(
        load_aram_anm(dRes_ID_ALANM_BTK_WL_FMABA01_e, ANM_FAMILY_TEX_SRT));
    if (btp == NULL || btk == NULL) {
        return false;
    }

    J3DMaterialTable& materials = i_modelData->getMaterialTable();
    btp->searchUpdateMaterialID(i_modelData);
    const int btpResult = materials.entryTexNoAnimator(btp);
    btk->searchUpdateMaterialID(i_modelData);
    const int btkResult = materials.entryTexMtxAnimator(btk);
    btp->setFrame(0.0f);
    btk->setFrame(0.0f);
    mBlinkFrame = 0;

    if (btpResult != 0 || btkResult != 0) {
        Log.warn("Puppet {} wolf blink attached with warnings: BTP={} BTK={}", mPlayerId, btpResult,
            btkResult);
        return false;
    }

    mpBlinkBtp = btp;
    mpBlinkBtk = btk;
    Log.info("Puppet {} wolf blink attached to body materials {} and {}", mPlayerId,
        l_wolfEyeMaterialNo[0], l_wolfEyeMaterialNo[1]);
    return true;
}

/**
 * Hook the joints wolfModelCallBack acts on, through the same trampoline as the human's.
 * daAlink_c hangs it on all 40 (swindow.inc:223-225); the puppet hooks only the ones that do
 * something. setCallBack is on the model DATA, safe because the mount is private.
 */
void daRemotePlayer_c::setupWolfJointCallBacks(J3DModelData* i_modelData) {
    static const u16 hooked[] = {
        l_wolfUnderRootJointNo,
        l_wolfBackJointNo,
        l_wolfNeckJointNo,
        l_wolfHeadJointNo,
        l_wolfTongueJointNo,
        l_wolfWaistJointNo,
        l_wolfTailJointNo[0],
        l_wolfTailJointNo[1],
        l_wolfTailJointNo[2],
    };

    model->setUserArea((uintptr_t)this);
    for (u32 i = 0; i < sizeof(hooked) / sizeof(hooked[0]); i++) {
        i_modelData->getJointNodePointer(hooked[i])->setCallBack(daRemotePlayer_bodyModelCallBack);
    }
}

/* --- per tick --------------------------------------------------------------------------------- */

/**
 * execute() for a wolf, from after the mHasPose check. The human order with everything a wolf
 * does not have removed (see the file header). Each step keeps the reason it has in execute().
 */
void daRemotePlayer_c::executeWolf() {
    traceCalc("wolf: selectAnimation");
    selectWolfAnimation();
    // After the slot-0 install (setAnm clears slot 2) and before animePlay pushes its frame.
    wolfApplyUpper2();
    traceCalc("wolf: animePlay");
    animePlay();
    playFaceTextureAnime();
    traceCalc("wolf: groundCheck");
    groundCheck();
    setRoomInfo();
    soundFramework();
    traceCalc("wolf: setMatrix");
    wolfSetMatrix();
    // The transform visuals (piece C), after the body's calc as in execute().
    updateTransformFx();
    traceCalc("wolf: execute done");
    if (mCalcTraced < l_calcTraceNum) {
        mCalcTraced++;
    }
}

/**
 * The wolf's whole animation choice: the action override, or hold.
 *
 * ★ There is no locomotion branch on purpose (spec section 2): re-deriving setBlendWolfMoveAnime
 * means porting dash mode, slope, attention and heavy-state predicates, and the sender sends what
 * he draws instead. So when the override is not playing, the body HOLDS. Three cases:
 *   - It never played: the wolf's own WANM_WAIT loops (setupWolfAnimation()).
 *   - It went to none (a demo-archive BCK): the packs still name resident override BCKs, nothing
 *     reloads them, so the pose is frozen in place — rates to 0, the root kept.
 *   - A load FAILED: the attempt freeAll()ed the heap the packs pointed into
 *     (selectActionAnimation's failure path), so holding would read freed memory. The wolf wait is
 *     put back instead. Still the wolf's own animation; never the human gait.
 */
void daRemotePlayer_c::selectWolfAnimation() {
    mAnmChosen = true;

    if (selectActionAnimation()) {
        if (mWolfHolding && mWolfLogCount < l_wolfLogMax) {
            mWolfLogCount++;
            Log.debug("Puppet {} wolf hold released: bck {}", mPlayerId, mActShownUnder);
        }
        mWolfHolding = false;
        mWolfRoot = mNetActRoot;
        mWolfRootRotX = mNetActRootRotX;
        mWolfRootRotZ = mNetActRootRotZ;
        mWolfPitch = mNetActPitch;
        mWolfYawOff = mNetActYawOff;
        mWolfRootValid = true;
        return;
    }

    J3DAnmTransform* under0 = mAnmPackUnder[0].getAnmTransform();
    if (under0 == mWolfWaitAnm.mpUnder) {
        return;
    }

    if (under0 == NULL || under0 != mpActUnderBck) {
        setAnm(mWolfWaitAnm, J3DFrameCtrl::EMode_LOOP, daAlinkHIO_wlMove_c0::m.mNormalInterpolation,
            daAlinkHIO_wlMove_c0::m.mIdleAnmSpeed, 0.0f, -1);
        mWolfHolding = false;
        mWolfRootValid = false;
        mWolfPitch = 0;
        mWolfYawOff = 0;
        if (mWolfLogCount < l_wolfLogMax) {
            mWolfLogCount++;
            Log.debug("Puppet {} wolf back to WANM_WAIT: what it was drawing is no longer resident",
                mPlayerId);
        }
        return;
    }

    if (!mWolfHolding) {
        /* Slot 1 and the upper halves must name what is still resident too; anything else is
         * dropped rather than read. */
        if (mAnmPackUnder[1].getAnmTransform() != NULL &&
            mAnmPackUnder[1].getAnmTransform() != mpActUnder1Bck)
        {
            clearActionSlot1();
        }
        for (int i = 0; i < 2; i++) {
            J3DAnmTransform* upper = mAnmPackUpper[i].getAnmTransform();
            J3DAnmTransform* under = mAnmPackUnder[i].getAnmTransform();
            J3DAnmTransform* ownUpper = i == 0 ? mpActUpperBck : mpActUpper1Bck;
            if (upper != NULL && upper != under && upper != ownUpper) {
                mAnmPackUpper[i].setAnmTransform(under);
            }
            mUnderFrameCtrl[i].setRate(0.0f);
            mUpperFrameCtrl[i].setRate(0.0f);
        }
        mWolfHolding = true;
        if (mWolfLogCount < l_wolfLogMax) {
            mWolfLogCount++;
            Log.debug("Puppet {} wolf holds its last pose: bck {} frame {:.1f} (the sender is in "
                      "an animation the puppet cannot load)",
                mPlayerId, mActUnderHeap[0].getIdx(), under0->getFrame());
        }
    }
}

/**
 * applyUpper2Overlay() with the wolf's morf joints, and the grab flag for changeWolfBlendRate.
 * Same heap (mActUpperHeap[2]), same one-driver rule for the frame.
 */
void daRemotePlayer_c::wolfClearUpper2(f32 i_morf) {
    mAnmPackUpper[2].setAnmTransform(NULL);
    mAnmPackUpper[2].setRatio(0.0f);
    mUpperFrameCtrl[2].init(0);
    mUpper2Shown = l_actNone;
    wolf_upper2_morf(mpOldFrame, i_morf, mBodyJointNum);
    mUpper2AppliedMorf = static_cast<u8>(i_morf > 0.0f ? (i_morf > 255.0f ? 255 : i_morf) : 0);
}

void daRemotePlayer_c::wolfApplyUpper2() {
    const u16 want = mNetUpper2Idx;
    mWolfGrab = false;

    if (!mActHeapReady || want == l_actNone) {
        if (mUpper2Shown != l_actNone) {
            wolfClearUpper2(mNetUpper2Morf);
        }
        if (want == l_actNone) {
            mUpper2Failed = l_actNone;
        }
        return;
    }

    if (want != mUpper2Shown) {
        if (want == mUpper2Failed) {
            if (mUpper2Shown != l_actNone) {
                wolfClearUpper2(mNetUpper2Morf);
            }
            return;
        }

        bool reloaded = false;
        J3DAnmTransform* bck =
            loadActionBck(mActUpperHeap[2], want, l_upper2LoadSize, mpUpper2Bck, &reloaded);
        if (bck == NULL) {
            mUpper2Failed = want;
            mpUpper2Bck = NULL;
            if (mUpper2Shown != l_actNone) {
                wolfClearUpper2(mNetUpper2Morf);
            }
            if (mWolfLogCount < l_wolfLogMax) {
                mWolfLogCount++;
                Log.warn("Puppet {} wolf UPPER_2 overlay load failed idx={}", mPlayerId, want);
            }
            return;
        }

        if (mWolfLogCount < l_wolfLogMax) {
            mWolfLogCount++;
            Log.debug("Puppet {} wolf UPPER_2 overlay {} -> {} (morf {:.0f}, {})", mPlayerId,
                mUpper2Shown, want, mNetUpper2Morf, reloaded ? "loaded" : "resident");
        }
        mpUpper2Bck = bck;
        mUpper2Shown = want;
        mUpper2Failed = l_actNone;
        wolf_upper2_morf(mpOldFrame, mNetUpper2Morf, mBodyJointNum);
        mUpper2AppliedMorf = static_cast<u8>(
            mNetUpper2Morf > 0.0f ? (mNetUpper2Morf > 255.0f ? 255 : mNetUpper2Morf) : 0);
    }

    const f32 frameMax = mpUpper2Bck->getFrameMax();
    f32 frame = mNetUpper2Frame;
    if (frame > frameMax) {
        frame = frameMax;
    } else if (frame < 0.0f) {
        frame = 0.0f;
    }

    /* checkWolfGrabAnime() (d_a_alink.h:3506-3508), the game's own predicate on the overlay being
     * drawn. While it holds, changeWolfBlendRate sets the ratio to 1 at joint 3 and back to 0 at
     * joint 7 inside every calc, so the ratio a calc STARTS with is the 0 the previous calc left:
     * the mouth carries, the neck root and forelegs do not. The wire ratio is not used then. */
    mWolfGrab = want == dRes_ID_ALANM_BCK_WL_PICKUPA_e || want == dRes_ID_ALANM_BCK_WL_PICKUPB_e;
    if (mWolfGrab && !mLoggedWolfGrab) {
        mLoggedWolfGrab = true;
        Log.debug("Puppet {} wolf grab overlay live: bck {} on joints {}..{} only", mPlayerId, want,
            l_wolfNeckJointNo, l_wolfTongueJointNo);
    }

    mAnmPackUpper[2].setAnmTransform(mpUpper2Bck);
    mAnmPackUpper[2].setRatio(mWolfGrab ? 0.0f : mNetUpper2Ratio);
    mUpperFrameCtrl[2].setFrameCtrl(
        mpUpper2Bck->getAttribute(), 0, static_cast<s16>(frameMax), 0.0f, frame);
    mpUpper2Bck->setFrame(frame);
}

/**
 * The wolf's setMatrix: the base transform (daAlink_c's ZXY order, with the override's pitch and
 * yaw offset as setMatrix() applies them), the tail spring, the calc — inside which the joint
 * callbacks pose the root, neck, twist and tail — then the equipment and the chain off the
 * finished joints.
 */
void daRemotePlayer_c::wolfSetMatrix() {
    if (!mActActive && !mWolfHolding) {
        mWolfPitch = 0;
        mWolfYawOff = 0;
    }
    mActAppliedPitch = mActActive ? mWolfPitch : 0;
    mActAppliedYawOff = mActActive ? mWolfYawOff : 0;
    shape_angle.x = mWolfPitch;

    mDoMtx_stack_c::transS(current.pos);
    mDoMtx_stack_c::ZXYrotM(shape_angle.x, (s16)(shape_angle.y + mWolfYawOff), shape_angle.z);
    model->setBaseTRMtx(mDoMtx_stack_c::get());
    // getDrawnRoot() reads the world->model matrix of the base this calc uses.
    mDoMtx_inverse(mDoMtx_stack_c::get(), mInvMtx);

    // Before calc: the joint callback for 37-39 reads it during this calc.
    setWolfTailAngle();

    model->calc();

    setWolfItemMatrix();
    /* setWolfChainPos is gated on mClothesChangeWaitTimer == 0 (wolf.inc:2254); the sender's
     * kPlayerFormChanging is that timer being non-zero. */
    if ((mNetForm & l_wolfFormChanging) == 0) {
        setWolfChainPos();
    }

    /* ★ Latched and permanent: the calculators read back off the model. A wolf with the human's
     * joints (0/1/16) would draw with the legs on the head's animation and look merely odd. */
    if (!mLoggedWolfSplit) {
        mLoggedWolfSplit = true;
        J3DModelData* data = model->getModelData();
        const J3DMtxCalc* on0 = data->getJointNodePointer(l_wolfUnderRootJointNo)->getMtxCalc();
        const J3DMtxCalc* on3 = data->getJointNodePointer(l_wolfUpperRootJointNo)->getMtxCalc();
        const J3DMtxCalc* on15 = data->getJointNodePointer(l_wolfUnderLegJointNo)->getMtxCalc();
        Log.info("Puppet {} wolf body split live: joints {}/{}/{} -> {}/{}/{} | override {} (bck {}"
                 " upper {}), holding {}, {} joints",
            mPlayerId, l_wolfUnderRootJointNo, l_wolfUpperRootJointNo, l_wolfUnderLegJointNo,
            on0 == mpUnderCalc ? "under" : "WRONG", on3 == mpUpperCalc ? "UPPER" : "WRONG",
            on15 == mpUnderCalc ? "under" : "WRONG", mActActive ? "on" : "off", getActDrawnUnder(),
            getActDrawnUpper(), mWolfHolding ? "yes" : "no", mBodyJointNum);
    }
}

/// daAlink_c::setWolfTailAngle (d_a_alink_wolf.inc:645-666), on the puppet's replicated yaw.
/// ERFLG0_UNK_800000 (his teleport reset) has no puppet counterpart; the first tick stands in for
/// it, and the ±0x1000 clamp bounds what a snapped yaw can do to it.
void daRemotePlayer_c::setWolfTailAngle() {
    s16 step = mWolfPrevYawValid ? (s16)(shape_angle.y - mWolfPrevYaw) : 0;
    mWolfPrevYaw = shape_angle.y;
    mWolfPrevYawValid = true;

    for (int i = 0; i < 3; i++) {
        mWolfTailAngle[i] = (s16)(mWolfTailAngle[i] - step);
        const s16 before = mWolfTailAngle[i];
        cLib_addCalcAngleS(&mWolfTailAngle[i], 0, 5, 500, 50);
        mWolfTailAngle[i] = cLib_minMaxLimit<s16>(
            (s16)(mWolfTailAngle[i] + mWolfTailVel[i]), -0x1000, 0x1000);
        step = (s16)(mWolfTailAngle[i] - before);
        mWolfTailVel[i] = (s16)(step * 0.5f);
    }
}

/**
 * daAlink_c::setWolfItemMatrix (d_a_alink.cpp:6123-6143): sword, sheath and shield off joint 2 by
 * his fixed offsets. Only what currentSword()/currentShield() select is posed, on
 * setEquipMatrix()'s terms — a model that is never entered needs no matrices. The held model
 * (0x109 / 0x10A) that follows in his is deferred; his chain call is wolfSetMatrix()'s.
 */
void daRemotePlayer_c::setWolfItemMatrix() {
    MtxP backMtx = model->getAnmMtx(l_wolfBackJointNo);

    J3DModel* sheath = NULL;
    J3DModel* sword = currentSword(&sheath);
    if (sword != NULL) {
        mDoMtx_stack_c::copy(backMtx);
        mDoMtx_stack_c::transM(31.0f, -29.0f, 19.0f);
        mDoMtx_stack_c::XYZrotM(0, cM_deg2s(32.0f), cM_deg2s(157.0f));
        sword->setBaseTRMtx(mDoMtx_stack_c::get());
        sword->calc();
    }
    if (sheath != NULL) {
        mDoMtx_stack_c::copy(backMtx);
        mDoMtx_stack_c::transM(13.0f, -21.0f, 7.0f);
        mDoMtx_stack_c::XYZrotM(0, cM_deg2s(-0.8f), cM_deg2s(157.0f));
        sheath->setBaseTRMtx(mDoMtx_stack_c::get());
        sheath->calc();
    }

    J3DModel* shield = currentShield();
    if (shield != NULL) {
        mDoMtx_stack_c::copy(backMtx);
        mDoMtx_stack_c::transM(11.0f, -18.0f, -13.0f);
        mDoMtx_stack_c::XYZrotM(cM_deg2s(90.0f), cM_deg2s(58.0f), cM_deg2s(-24.0f));
        shield->setBaseTRMtx(mDoMtx_stack_c::get());
        shield->calc();
    }
}

/**
 * daAlink_c::setWolfChainPos (d_a_alink_wolf.inc:2245-2383): four links hung off joint 17 with
 * their own spring and a floor probe each. Posed every tick whether or not M_011 is set, as his is;
 * drawn only when it is (drawWolf).
 *
 * ★ One read of his is out of bounds and is answered here: the floor branch compares against
 * `var_r30[-1].z`, which for link 0 is field_0x3136[1].z, the member before the array — a human
 * grab-arm angle that is zero for a wolf. Link 0 compares against 0.
 */
void daRemotePlayer_c::setWolfChainPos() {
    static const Vec wolfChainBaseOffset = {22.0f, 0.0f, -12.0f};
    static const Vec wolfChainVec = {0.0f, 0.0f, -9.0f};

    if (mpWlChainModel[0] == NULL || mBodyJointNum <= l_wolfFLegJointNo) {
        return;
    }

    MtxP legMtx = model->getAnmMtx(l_wolfFLegJointNo);
    cXyz base;
    mDoMtx_multVec(legMtx, &wolfChainBaseOffset, &base);

    cXyz axis;
    mDoMtx_multVecSR(legMtx, &cXyz::BaseZ, &axis);
    const s16 baseYaw = axis.atan2sX_Z();
    const s16 basePitch = axis.atan2sY_XZ();
    mDoMtx_multVecSR(legMtx, &cXyz::BaseY, &axis);
    s16 roll = (s16)(cM_atan2s(axis.y, axis.absXZ()) + 0x4000);

    if (!mWolfChainInit) {
        mWolfChainInit = true;

        mDoMtx_stack_c::transS(base);
        mDoMtx_stack_c::ZXYrotM(-0x2000, baseYaw, 0);
        mDoMtx_stack_c::multVec(&wolfChainVec, &mWolfChainPos[0]);
        mWolfChainAngle[0].set(-0x2000, baseYaw, roll);
        mWolfChainVel[0] = cXyz::Zero;

        for (int i = 1; i < l_wolfChainNum; i++) {
            mWolfChainPos[i] = mWolfChainPos[i - 1];
            mWolfChainPos[i].y -= 9.0f;
            mWolfChainAngle[i] = mWolfChainAngle[i - 1];
            mWolfChainAngle[i].z = (s16)(mWolfChainAngle[i].z + 0x4000);
            mWolfChainVel[i] = cXyz::Zero;
        }
    } else {
        for (int i = 0; i < l_wolfChainNum; i++) {
            const cXyz prev = mWolfChainPos[i];
            cXyz dir = (mWolfChainPos[i] - base) + mWolfChainVel[i];
            dir.y += -1.5f;

            const f32 distXZ = dir.absXZ();
            s16 pitch = cM_atan2s(dir.y, distXZ);
            s16 yaw = distXZ < 1.0f ? mWolfChainAngle[i].y : cM_atan2s(-dir.x, -dir.z);

            if (i == 0) {
                s16 delta = (s16)(yaw - baseYaw);
                if (delta > 0x2000) {
                    yaw = (s16)(baseYaw + 0x2000);
                } else if (delta < -0x2000) {
                    yaw = (s16)(baseYaw - 0x2000);
                }

                delta = (s16)(pitch - basePitch);
                if (delta > 0x2000) {
                    pitch = (s16)(basePitch + 0x2000);
                } else if (delta < -0x2000) {
                    pitch = (s16)(basePitch - 0x2000);
                }
            }

            csXyz& angle = mWolfChainAngle[i];
            angle.set(pitch, yaw, roll);
            mDoMtx_stack_c::transS(base);
            mDoMtx_stack_c::ZXYrotM(angle);
            mDoMtx_stack_c::multVec(&wolfChainVec, &mWolfChainPos[i]);

            cXyz probe(mWolfChainPos[i].x, 10.0f + mWolfChainPos[i].y, mWolfChainPos[i].z);
            mWolfChainGndChk.SetPos(&probe);
            const f32 floorY = 3.0f + dComIfG_Bgsp().GroundCross(&mWolfChainGndChk);
            if (floorY > mWolfChainPos[i].y) {
                const f32 lift = floorY - mWolfChainPos[i].y;
                angle.x = cM_atan2s(dir.y + lift, distXZ + lift);

                cM3dGPla plane;
                dComIfG_Bgsp().GetTriPla(mWolfChainGndChk, &plane);
                angle.z = fopAcM_getPolygonAngle(&plane, (s16)(angle.y + 0x4000));

                const s16 prevRoll = i == 0 ? 0 : mWolfChainAngle[i - 1].z;
                const s16 delta = (s16)(prevRoll - angle.z);
                if ((delta >= 0 && delta <= 0x4000) || delta < -0x4000) {
                    angle.z = (s16)(angle.z - 0x2000);
                } else {
                    angle.z = (s16)(angle.z + 0x2000);
                }

                mDoMtx_stack_c::transS(base);
                mDoMtx_stack_c::ZXYrotM(angle);
                mDoMtx_stack_c::multVec(&wolfChainVec, &mWolfChainPos[i]);
            }

            mWolfChainVel[i] = (mWolfChainPos[i] - prev) * 0.8f;
            base = mWolfChainPos[i];
            roll = (s16)(angle.z + 0x4000);
        }
    }

    for (int i = 0; i < l_wolfChainNum; i++) {
        mDoMtx_stack_c::transS(mWolfChainPos[i]);
        mDoMtx_stack_c::ZXYrotM(mWolfChainAngle[i]);
        mpWlChainModel[i]->setBaseTRMtx(mDoMtx_stack_c::get());
        mpWlChainModel[i]->calc();
    }
}

/* --- joints ----------------------------------------------------------------------------------- */

/// daAlink_c::wolfModelCallBack (d_a_alink.cpp:2530-2540), minus setWolfFootMatrix (joint 36, the
/// deferred wolf foot IK). changeWolfBlendRate's bark arm (joint 2) is dead: checkBarkAnime() is
/// constant FALSE (d_a_alink.h:3505).
int daRemotePlayer_c::wolfBodyModelCallBack(int i_jointNo) {
    wolfJointControl(i_jointNo);

    if (mWolfGrab) {
        if (i_jointNo == l_wolfTongueJointNo) {
            mAnmPackUpper[2].setRatio(0.0f);
        } else if (i_jointNo == l_wolfNeckJointNo) {
            mAnmPackUpper[2].setRatio(1.0f);
        }
    }
    return 1;
}

/**
 * jointControll's wolf branch (d_a_alink.cpp:2150-2191) and its shared quaternion tail
 * (:2260-2336), against this actor's own old frame, from the sender's answers:
 *   joint 0:      root translation = the override's (the sender's cleared root), rotation
 *                 (actRootRotX, 0, actRootRotZ) in the WOLF axis order (2,1,0) — the human uses
 *                 (2,0,1). Always rebuilt (his var_r27 |= 2).
 *   joint 3:      (bodyX, 0, bodyZ), order (2,0,1) — bodyX is field_0x30d6 + field_0x30b2 for a
 *                 wolf.
 *   joints 2, 26: (0, -bodyTwist, 0), order (2,1,0) — bodyTwist is field_0x2fec.
 *   joints 37-39: (0, tail[i], 0), order (2,1,0) — the local spring.
 *   joint 4:      (neckX, -neckY, 0), order (2,1,0) — field_0x3124.z is never written.
 */
void daRemotePlayer_c::wolfJointControl(int i_jointNo) {
    if (mpOldFrame == NULL || i_jointNo >= mBodyJointNum) {
        return;
    }

    csXyz sp18(0, 0, 0);
    csXyz sp10(0, 1, 2);
    int mode = 0;

    // Copied at entry, BEFORE joint 0's root is overwritten: the parent is recovered from these.
    const J3DTransformInfo rootTrans = *mpOldFrame->getOldFrameTransInfo(i_jointNo);
    Quaternion sp20 = *mpOldFrame->getOldFrameQuaternion(i_jointNo);
    J3DTransformInfo jointTrans;

    if (i_jointNo == l_wolfUnderRootJointNo) {
        /* The applyActionRoot() idea for the wolf: daAlink_c clears the root by writing the old
         * frame's trans info; the puppet writes the sender's final answer there. While holding,
         * the held animation's own root is overwritten with the last one applied, so the frozen
         * pose stays where it was. */
        const bool useRoot = mActActive || (mWolfHolding && mWolfRootValid);
        if (useRoot) {
            const cXyz& root = mActActive ? mNetActRoot : mWolfRoot;
            J3DTransformInfo* oldTrans = mpOldFrame->getOldFrameTransInfo(l_wolfUnderRootJointNo);
            oldTrans->mTranslate.x = root.x;
            oldTrans->mTranslate.y = root.y;
            oldTrans->mTranslate.z = root.z;
        }
        const s16 rotX = mActActive ? mNetActRootRotX : (mWolfHolding ? mWolfRootRotX : 0);
        const s16 rotZ = mActActive ? mNetActRootRotZ : (mWolfHolding ? mWolfRootRotZ : 0);
        mActAppliedRootRotX = mActActive ? rotX : 0;
        mActAppliedRootRotZ = mActActive ? rotZ : 0;
        sp18.set(rotX, 0, rotZ);
        sp10.set(2, 1, 0);
        jointTrans = *mpOldFrame->getOldFrameTransInfo(l_wolfUnderRootJointNo);
        mode |= 2;

        if (useRoot && !mLoggedActRoot) {
            mLoggedActRoot = true;
            Log.debug("Puppet {} wolf root overwrite live: joint 0 local ({:.1f}, {:.1f}, {:.1f}) "
                      "rot {} / {} in order (2,1,0)",
                mPlayerId, jointTrans.mTranslate.x, jointTrans.mTranslate.y,
                jointTrans.mTranslate.z, rotX, rotZ);
        }
    } else if (i_jointNo == l_wolfNeckJointNo) {
        sp18.set(mNetBodyX, 0, mNetBodyZ);
        sp10.set(2, 0, 1);
    } else if (i_jointNo == l_wolfWaistJointNo || i_jointNo == l_wolfBackJointNo) {
        sp18.set(0, (s16)-mNetBodyTwist, 0);
        sp10.set(2, 1, 0);
    } else if (i_jointNo >= l_wolfTailJointNo[0] && i_jointNo <= l_wolfTailJointNo[2]) {
        sp18.set(0, mWolfTailAngle[i_jointNo - l_wolfTailJointNo[0]], 0);
        sp10.set(2, 1, 0);
    } else if (i_jointNo == l_wolfHeadJointNo) {
        sp18.set(mNetNeckX, (s16)-mNetNeckY, 0);
        sp10.set(2, 1, 0);
    } else {
        // Joint 7 is hooked for the UPPER_2 ratio only.
        return;
    }

    Quaternion sp50;
    Quaternion sp40;
    Quaternion sp30 = sp20;
    if (sp18.x != 0 || sp18.y != 0 || sp18.z != 0) {
        mode |= 1;
        if (sp18.y != 0) {
            sp50 = sp20;
            wolf_euler_quat(sp10.y, sp18.y, &sp40);
            mDoMtx_QuatConcat(&sp50, &sp40, &sp30);
        }
        if (sp18.x != 0) {
            sp50 = sp30;
            wolf_euler_quat(sp10.x, sp18.x, &sp40);
            mDoMtx_QuatConcat(&sp50, &sp40, &sp30);
        }
        if (sp18.z != 0) {
            sp50 = sp30;
            wolf_euler_quat(sp10.z, sp18.z, &sp40);
            mDoMtx_QuatConcat(&sp50, &sp40, &sp30);
        }
    }

    if (mode == 0) {
        return;
    }

    MtxP jointMtx = model->getAnmMtx(i_jointNo);
    const J3DTransformInfo* trans =
        (mode & 2) != 0 ? &jointTrans : mpOldFrame->getOldFrameTransInfo(i_jointNo);
    Quaternion* quat = (mode & 1) != 0 ? &sp30 : mpOldFrame->getOldFrameQuaternion(i_jointNo);

    mDoMtx_stack_c::transS(rootTrans.mTranslate.x, rootTrans.mTranslate.y, rootTrans.mTranslate.z);
    mDoMtx_stack_c::quatM(&sp20);
    mDoMtx_stack_c::inverse();
    cMtx_concat(jointMtx, mDoMtx_stack_c::get(), J3DSys::mCurrentMtx);

    MTXQuat(jointMtx, quat);
    jointMtx[0][3] = trans->mTranslate.x;
    jointMtx[1][3] = trans->mTranslate.y;
    jointMtx[2][3] = trans->mTranslate.z;

    cMtx_concat(J3DSys::mCurrentMtx, jointMtx, J3DSys::mCurrentMtx);
    cMtx_copy(J3DSys::mCurrentMtx, jointMtx);
}

/* --- draw ------------------------------------------------------------------------------------- */

/**
 * applyDamageTint()'s ladder (d_a_alink.cpp:19524-19583) WITHOUT its register-1 half:
 * setWaterDropColor sits inside `if (!checkWolf())` (:19585-19606), and setDamageReg1's material
 * lists are the human outfits', which would write into the wolf body's materials. The light-drop
 * branch above the freeze (field_0x346c) is not on the wire and is skipped (spec section 5).
 */
void daRemotePlayer_c::wolfDamageTint() {
    tevStr.TevColor.r = 0;
    tevStr.TevColor.g = 0;
    tevStr.TevColor.b = 0;
    tevStr.TevKColor.r = 0;
    tevStr.TevKColor.g = 0;
    tevStr.TevKColor.b = 0;

    const daAlinkHIO_damage_c1& hio = daAlinkHIO_damage_c0::m;
    const u8 kind = mNetDmgTint;
    switch (kind) {
    case dusk::mp::kPlayerDmgTintFreeze:
        tevStr.TevColor.r = hio.mFreezeR;
        tevStr.TevColor.g = hio.mFreezeG;
        tevStr.TevColor.b = hio.mFreezeB;
        break;
    case dusk::mp::kPlayerDmgTintDamage: {
        const int colorTimer = mNetDmgFlash;
        int r, g, b;
        if (colorTimer > 32) {
            r = hio.mDamageR0;
            g = hio.mDamageG0;
            b = hio.mDamageB0;
        } else if (colorTimer > 16) {
            r = hio.mDamageR1;
            g = hio.mDamageG1;
            b = hio.mDamageB1;
        } else {
            r = hio.mDamageR2;
            g = hio.mDamageG2;
            b = hio.mDamageB2;
        }
        const f32 pulse = std::fabs(cM_ssin(static_cast<s16>(colorTimer * 0x800)));
        tevStr.TevKColor.r = static_cast<u8>(r * pulse);
        tevStr.TevKColor.g = static_cast<u8>(g * pulse);
        tevStr.TevKColor.b = static_cast<u8>(b * pulse);
        break;
    }
    case dusk::mp::kPlayerDmgTintIce: {
        const f32 initRatio = 1.0f - (mNetDmgFlash * (1.0f / hio.mFreezeTime));
        const f32 freezeRatio = 1.0f - initRatio;
        tevStr.TevColor.r =
            static_cast<s16>((initRatio * hio.mFreezeInitR) + (freezeRatio * hio.mFreezeR));
        tevStr.TevColor.g =
            static_cast<s16>((initRatio * hio.mFreezeInitG) + (freezeRatio * hio.mFreezeG));
        tevStr.TevColor.b =
            static_cast<s16>((initRatio * hio.mFreezeInitB) + (freezeRatio * hio.mFreezeB));
        break;
    }
    default:
        break;
    }

    if (kind != dusk::mp::kPlayerDmgTintNone && kind < 4 && !mLoggedDmgTint[kind]) {
        mLoggedDmgTint[kind] = true;
        Log.debug("Puppet {} drawing wolf {} tint: dmgFlash {}", mPlayerId,
            puppet_dmg_tint_name(kind), static_cast<unsigned>(mNetDmgFlash));
    }
}

/**
 * draw() for a wolf, from after its two guards (no local player; one entry per sim tick).
 *
 * daAlink_c's wolf draw (d_a_alink.cpp:19481-19642): tev type 9; into the DARK list; body; chain
 * while M_011; KColor cleared; sword + sheath, shield; back to the normal list. Every model is
 * entered once per pass (Hang 4's rule), and every one of them is this puppet's own.
 *
 * ★ Type 9 shares type 10's local-player dereferences (setLight_actor's checkNowWolfPowerUp, and
 * settingTevStruct_plightcol_plus's player reads, d_kankyo.cpp:3591 and on), which is why this is
 * reached only BELOW draw()'s early-out. The scene-wide crossfade state it writes is put back
 * exactly as draw() puts it back for type 10.
 */
int daRemotePlayer_c::drawWolf() {
    const u8 savedUseCol = g_env_light.UseCol;
    const u8 savedPrevCol = g_env_light.PrevCol;
    const f32 savedPatRatio = g_env_light.pat_ratio;
    const cXyz savedPlightNearPos = g_env_light.plight_near_pos;

    traceCalc("wolf draw: settingTevStruct");
    g_env_light.settingTevStruct(9, &current.pos, &tevStr);

    g_env_light.UseCol = savedUseCol;
    g_env_light.PrevCol = savedPrevCol;
    g_env_light.pat_ratio = savedPatRatio;
    g_env_light.plight_near_pos = savedPlightNearPos;

    wolfDamageTint();

    /* [piece C] EARLY-OUT POINT. The transform visuals go here: while (mNetForm &
     * kPlayerFormChanging) daAlink_c draws nothing but the black blob and returns
     * (d_a_alink.cpp:19505-19522), and the metamorphose TevColor tint (field_0x300e, :19531-19536)
     * overrides the ladder above. Both belong between the tint and the first model. */
    if (drawTransformBlob()) {
        return 1;
    }
    applyTransformTint();

    dComIfGd_setListDark();
    traceCalc("wolf draw: body");
    drawModel(model);

    if ((mNetForm & l_wolfFormChain) != 0 && mpWlChainModel[0] != NULL) {
        for (int i = 0; i < l_wolfChainNum; i++) {
            drawModel(mpWlChainModel[i]);
        }
        if (!mLoggedWolfChain) {
            mLoggedWolfChain = true;
            Log.debug("Puppet {} drawing the wolf's shackle chain (form 0x{:02x})", mPlayerId,
                mNetForm);
        }
    }

    // His tevStr.TevKColor = 0 after the chain: the damage pulse is the body's alone.
    clearDamageKColor();
    traceCalc("wolf draw: equipment");
    drawEquip();
    dComIfGd_setList();

    traceCalc("wolf draw: shadow");
    shadowDraw();
    traceCalc("wolf draw done");
    return 1;
}
