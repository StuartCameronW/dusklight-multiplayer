/**
 * d_a_remote_player_hookshot.inc
 * Remote player puppet: the clawshot's fired tips, their chains and the guns' own animation (wire
 * v20 clawshot block, while heldItem is kPlayerHeldClawshot or kPlayerHeldDoubleClawshot).
 *
 * Dusk-only, like the rest of the puppet. Included once, at the end of d_a_remote_player.cpp,
 * after d_a_remote_player_held.inc (whose loaders, BCK table and setHeldItemBck it uses) and
 * d_a_remote_player_ironball.inc (whose chain-link model data it shares).
 *
 * How daAlink_c does it, and so how this does:
 *
 *   - He keeps TWO clawshot sets even for the single clawshot (setHookshotModel,
 *     d_a_alink_hook.inc:427-476): mHeldItemModel + mpHookTipModel, and field_0x0710 +
 *     field_0x0714 built on the same model data. Here they are "slot 0" and "slot 1"
 *     (kPlayerClawTipOut0 in dusk/player_held.hpp). field_0x3020 (kPlayerHeldClawRight) puts slot
 *     0 in the right hand; for the single clawshot his draw then swaps in slot 1, the left-hand
 *     one (changeHookshotDrawModel, :346-356), so a single clawshot is always drawn left-handed.
 *   - Each gun is placed off an item joint (setHookshotPos, :848-870). Slot 1's gun is posed at
 *     HS_SPIN frame 0 (:911-912), slot 0's at field_0x33dc (setItemMatrix,
 *     d_a_alink.cpp:6083-6091) — the latter is wire block A's heldBck / heldBckFrame.
 *   - Each tip sits on its gun's muzzle (base * hookRoot) until it is fired / hung from, and then
 *     at a world position: mHookshotTopPos for slot 0, mIronBallBgChkPos for slot 1
 *     (:1195-1243). Each is posed at its own HS_TIP_OPEN frame (:1208, :1240).
 *   - The CHAINS are no model: hsChainShape_c::draw (:147-245), an immediate packet entered into
 *     the dark opaque list (d_a_alink.cpp:19793-19798), lays al_hs_kusari links from each gun's
 *     muzzle to its tip — slot 0's with a decaying wobble, slot 1's straight.
 *
 * What crosses the wire is only each tip's position and rotation while it is off its gun, the
 * tip-open frames, and the flags. The muzzles are this puppet's own, so a chain never lags the arm
 * that holds it; the chain is re-laid here by his own loop.
 */

#include "JSystem/J3DGraphBase/J3DDrawBuffer.h"

namespace {

/// hsChainShape_c::draw's TARGET_PC cap on one chain's links (d_a_alink_hook.inc:17), for the
/// same reason as his: a chain whose wobble turns a link sideways advances by almost nothing.
const int l_clawChainLinksMax = 600;
/// His hsVec, one link's advance (d_a_alink_hook.inc:197).
const Vec l_clawChainVec = {0.0f, 0.0f, 5.0f};
/// His HS_TIP_OPEN read: loadAram(..., 0x800) (d_a_alink_hook.inc:448).
const u32 l_clawTipBckBufSize = 0x800;
const u16 l_clawLogMax = 64;

/// daAlink_c::hsChainShape_c (d_a_alink.h:1426-1431): an immediate packet whose draw is the chain.
class daRemotePlayer_clawChain_c : public J3DPacket {
public:
    daRemotePlayer_clawChain_c() {}
    virtual void draw() {
        daRemotePlayer_c* puppet = static_cast<daRemotePlayer_c*>(getUserArea());
        if (puppet != NULL) {
            puppet->drawClawshotChain();
        }
    }
    virtual ~daRemotePlayer_clawChain_c() {}
};

}  // namespace

/**
 * The tip-open BCK, the chain link's model data and the chain packet. From createHeap, right after
 * setupHeldItems() — which built the guns, the tips and (in its BCK table) the gun's HS_SPIN — so a
 * short heap costs these and never a body model. All optional: no tip BCK draws the tips closed, no
 * chain draws fired tips with nothing behind them.
 *
 * Each is daAlink_c's own resource with his own literal: HS_TIP_OPEN with loadAram's 0x800 on an
 * mDoExt_bckAnm initialised as his mHookTipBck (init FALSE, attr 2; d_a_alink_hook.inc:448-451),
 * and AL_HS_KUSARI 0x1000 as model DATA only, his mpItemModelData (:440) — the iron ball's copy
 * when that loaded, since it is the same resource read the same way.
 */
void daRemotePlayer_c::setupClawshot() {
    mpClawTipBckAnm = NULL;
    mClawTipBckReady = false;
    mpClawChainData = NULL;
    mpClawChainPacket = NULL;
    for (int s = 0; s < 2; s++) {
        mClawRoot[s] = cXyz::Zero;
        mClawTop[s] = cXyz::Zero;
        mNetClawTipRel[s] = cXyz::Zero;
        mNetClawTipFrame[s] = 0;
    }
    for (int i = 0; i < 4; i++) {
        mNetClawTipAng[i] = 0;
    }
    mNetClawFlags = 0;
    mClawStopTime = 0;
    mClawPosedKind = dusk::mp::kPlayerHeldNone;
    mClawDrawnFlags = 0;
    mClawLoggedFlags = 0xFF;
    mClawLogCount = 0;
    mLoggedClawChain = false;

    if (mpHeldModel[dusk::mp::kPlayerHeldClawshot] == NULL) {
        Log.info("Puppet {} has no clawshot model; no clawshot chain or tip BCK built", mPlayerId);
        return;
    }

    JKRHeap* heap = JKRGetCurrentHeap();
    const s32 freeBefore = heap != NULL ? heap->getFreeSize() : 0;

    if (mpHeldClawTip[0] != NULL) {
        mpClawTipBckAnm = static_cast<J3DAnmTransform*>(held_load_aram_anm(
            dRes_ID_ALANM_BCK_HS_TIP_OPEN_e, 8, "clawshot tip bck", l_clawTipBckBufSize));
        if (mpClawTipBckAnm != NULL) {
            mClawTipBckReady = mClawTipBck.init(mpClawTipBckAnm, FALSE, 2, 1.0f, 0, -1, false) != 0;
        }
    }

    const bool shared = mpIronBallChainData != NULL;
    mpClawChainData = shared ? mpIronBallChainData :
                               held_load_aram_bmd(dRes_ID_ALANM_BMD_AL_HS_KUSARI_e, 0x1000,
                                   "clawshot chain");
    if (mpClawChainData != NULL && mpClawChainData->getMaterialNum() > 0) {
        mpClawChainPacket = JKR_NEW daRemotePlayer_clawChain_c();
        if (mpClawChainPacket != NULL) {
            mpClawChainPacket->setUserArea((uintptr_t)this);
        }
    } else {
        mpClawChainData = NULL;
    }

    // The measured cost, which the heap budget in create() is sized from.
    Log.info("Puppet {} clawshot: tip-open BCK {}, chain {} ({}), gun spin BCK {}; {} bytes of "
             "heap ({} -> {} free)",
        mPlayerId, mClawTipBckReady ? "ok" : "MISSING",
        mpClawChainPacket != NULL ? "ok" : "MISSING", shared ? "shared with the ball" : "own",
        mHeldItemBckReady && mpHeldItemBckAnm[dusk::mp::kPlayerHeldBckClawSpin] != NULL ?
            "ok" :
            "MISSING",
        heap != NULL ? freeBefore - heap->getFreeSize() : 0, freeBefore,
        heap != NULL ? heap->getFreeSize() : 0);
}

void daRemotePlayer_c::setNetworkClawshot(
    u8 i_flags, const u8* i_tipFrame, const cXyz* i_tipRel, const s16* i_tipAng) {
    mNetClawFlags = i_flags;
    for (int s = 0; s < 2; s++) {
        mNetClawTipFrame[s] = i_tipFrame[s];
        mNetClawTipRel[s] = i_tipRel[s];
    }
    for (int i = 0; i < 4; i++) {
        mNetClawTipAng[i] = i_tipAng[i];
    }
}

/**
 * Pose both clawshot sets: guns, tips, and the chains' four ends. From setHeldItemMatrix's
 * clawshot case — after the body's calc, as setItemMatrix calls setHookshotPos — and BEFORE the
 * caller calc()s i_held, which is slot 0's gun for the double clawshot and the drawn gun for the
 * single one.
 *
 * setHookshotPos, minus everything that is not drawing (the shot, its bg and attack checks, the
 * carry, the sounds and the mode machine — the sender's simulation, whose result is the wire).
 */
void daRemotePlayer_c::setClawshotMatrix(J3DModel* i_held, u8 i_kind) {
    const bool dbl = i_kind == dusk::mp::kPlayerHeldDoubleClawshot;
    const bool right = (mNetHeldFlags & dusk::mp::kPlayerHeldClawRight) != 0;

    // His two placements (d_a_alink_hook.inc:849-870): [0] the left hand, [1] the right.
    Mtx hand[2];
    mDoMtx_stack_c::copy(model->getAnmMtx(l_leftItemJointNo));
    mDoMtx_stack_c::transM(-2.0f, 1.0f, 1.0f);
    mDoMtx_stack_c::XYZrotM(cM_deg2s(5.7f), cM_deg2s(162.0f), 0);
    mDoMtx_copy(mDoMtx_stack_c::get(), hand[0]);
    mDoMtx_stack_c::copy(model->getAnmMtx(l_rightItemJointNo));
    mDoMtx_stack_c::transM(-2.0f, 0.0f, 1.0f);
    mDoMtx_stack_c::XYZrotM(cM_deg2s(-78.0f), cM_deg2s(182.0f), cM_deg2s(-99.0f));
    mDoMtx_copy(mDoMtx_stack_c::get(), hand[1]);

    // Slot 0 is in the left hand unless field_0x3020 says otherwise; slot 1 in the other.
    const int slotHand[2] = {right ? 1 : 0, right ? 0 : 1};

    /* Which of the puppet's models stands for which slot. The double clawshot draws both sets as
     * they are; the single one draws ONE gun and ONE tip, his swapped-in slot 1 when right is set
     * (changeHookshotDrawModel), and that is always the left-hand set. */
    J3DModel* gun[2] = {NULL, NULL};
    J3DModel* tip[2] = {NULL, NULL};
    if (dbl) {
        gun[0] = i_held;
        gun[1] = mpHeldClaw2Model;
        tip[0] = mpHeldClawTip[0];
        tip[1] = mpHeldClawTip[1];
    } else {
        const int shown = right ? 1 : 0;
        gun[shown] = i_held;
        tip[shown] = mpHeldClawTip[0];
    }

    // mHeldItemRootPos and field_0x3810: each gun's muzzle (:913-916), the chains' hand ends.
    for (int s = 0; s < 2; s++) {
        mDoMtx_multVec(hand[slotHand[s]], &l_heldHookRoot, &mClawRoot[s]);
        if (gun[s] != NULL) {
            gun[s]->setBaseTRMtx(hand[slotHand[s]]);
        }
    }

    /* The guns' HS_SPIN. Both share one model data, so the animation is entered per calc, in his
     * order: slot 1's gun at frame 0 and calc'd (:911-912), then slot 0's at the wire's frame,
     * which the caller calc()s (setItemMatrix, d_a_alink.cpp:6083-6091). That leaves the
     * animation object on the wire's frame, which is what getHeldItemBckDrawnFrame reports. */
    if (dbl && gun[1] != NULL) {
        J3DAnmTransform* spin =
            mHeldItemBckReady ? mpHeldItemBckAnm[dusk::mp::kPlayerHeldBckClawSpin] : NULL;
        if (spin != NULL) {
            if (mHeldItemBck.getBckAnm() != spin) {
                mHeldItemBck.changeBckOnly(spin);
            }
            mHeldItemBck.entry(gun[1]->getModelData(), 0.0f);
        } else if (mHeldItemBckReady) {
            mHeldItemBck.remove(gun[1]->getModelData());
        }
        gun[1]->calc();
    }
    setHeldItemBck(i_held, dusk::mp::kPlayerHeldClawshot);

    // The tips (:1031-1036, :1195-1243), and the chains' far ends.
    const f32 tipFrameMax = mClawTipBckReady ? mpClawTipBckAnm->getFrameMax() : 0.0f;
    for (int s = 0; s < 2; s++) {
        if ((mNetClawFlags & (dusk::mp::kPlayerClawTipOut0 << s)) != 0) {
            // Off its gun: where the sender's own simulation put it.
            mClawTop[s] = current.pos + mNetClawTipRel[s];
            mDoMtx_stack_c::transS(mClawTop[s]);
            mDoMtx_stack_c::ZXYrotM(mNetClawTipAng[s * 2 + 0], mNetClawTipAng[s * 2 + 1], 0);
        } else {
            // On its gun: base * hookRoot, so it rides THIS puppet's hand.
            mClawTop[s] = mClawRoot[s];
            mDoMtx_stack_c::copy(hand[slotHand[s]]);
            mDoMtx_stack_c::transM(l_heldHookRoot.x, l_heldHookRoot.y, l_heldHookRoot.z);
        }
        if (tip[s] == NULL) {
            continue;
        }
        if (mClawTipBckReady) {
            f32 frame = static_cast<f32>(mNetClawTipFrame[s]);
            if (frame > tipFrameMax) {
                frame = tipFrameMax;
            }
            mClawTipBck.entry(tip[s]->getModelData(), frame);
        }
        tip[s]->setBaseTRMtx(mDoMtx_stack_c::get());
        tip[s]->calc();
    }

    mClawStopTime = static_cast<u8>((mNetClawFlags & dusk::mp::kPlayerClawStopMask) >>
                                    dusk::mp::kPlayerClawStopShift);
    mClawPosedKind = i_kind;

    const u8 logKey = static_cast<u8>(mNetClawFlags & ~dusk::mp::kPlayerClawStopMask);
    if (logKey != mClawLoggedFlags && mClawLogCount < l_clawLogMax) {
        mClawLogCount++;
        Log.debug("Puppet {} clawshot 0x{:02x} -> 0x{:02x}: kind {}, right {}, tip0 {:.1f} from "
                  "its gun (frame {}), tip1 {:.1f} from its gun (frame {})",
            mPlayerId, static_cast<unsigned>(mClawLoggedFlags), static_cast<unsigned>(logKey),
            static_cast<unsigned>(i_kind), right ? 1 : 0, mClawTop[0].abs(mClawRoot[0]),
            static_cast<unsigned>(mNetClawTipFrame[0]), mClawTop[1].abs(mClawRoot[1]),
            static_cast<unsigned>(mNetClawTipFrame[1]));
    }
    mClawLoggedFlags = logKey;
}

/**
 * Enter the chain packet. From drawHeldItems(), right after the guns and tips, which is where
 * daAlink_c enters his (d_a_alink.cpp:19771-19798), and under his own condition: either set's tip
 * more than 1 unit off its muzzle.
 *
 * ★ Hang 4: the packet is entered once per pass. draw() has already returned on a second
 * entry-draw within one sim tick, drawHeldItems runs once per draw(), and this packet is not the
 * iron ball's, which is only entered while the ball is the held kind.
 */
void daRemotePlayer_c::drawClawshot() {
    mClawDrawnFlags = mNetClawFlags;
    if (mClawPosedKind != mNetHeldKind || mpClawChainPacket == NULL) {
        return;
    }
    if (!(mClawRoot[0].abs(mClawTop[0]) > 1.0f || mClawRoot[1].abs(mClawTop[1]) > 1.0f)) {
        return;
    }

    dComIfGd_getOpaListDark()->entryImm(mpClawChainPacket, 0);
    // AlinkInterp's hs_chain capture (:19810-19815): the packet draws at paint time from these.
    const cXyz anchors[4] = {mClawTop[0], mClawRoot[0], mClawRoot[1], mClawTop[1]};
    mClawChainSamples.capture(anchors, 4);

    if (!mLoggedClawChain) {
        mLoggedClawChain = true;
        Log.debug("Puppet {} drawing the clawshot chain: tip0 {:.1f}, tip1 {:.1f} from the guns, "
                  "stop {}",
            mPlayerId, mClawTop[0].abs(mClawRoot[0]), mClawTop[1].abs(mClawRoot[1]),
            static_cast<unsigned>(mClawStopTime));
    }
}

/// The tips cast into the body's real shadow under his own gates (d_a_alink.cpp:19321-19338): the
/// drawn primary tip only while checkHookshotWait(), the second gun always and its tip unless he
/// hangs from a roof. The caller has already added the drawn primary gun.
void daRemotePlayer_c::addClawshotShadow(u8 i_kind) {
    const bool roof = (mClawDrawnFlags & dusk::mp::kPlayerClawRoofHang) != 0;
    if ((mClawDrawnFlags & dusk::mp::kPlayerClawWait) != 0 && !roof) {
        dComIfGd_addRealShadow(mShadowKey, mpHeldClawTip[0]);
    }
    if (i_kind == dusk::mp::kPlayerHeldDoubleClawshot &&
        (mHeldDrawnFlags & dusk::mp::kPlayerHeldClawBoth) != 0)
    {
        dComIfGd_addRealShadow(mShadowKey, mpHeldClaw2Model);
        if (!roof) {
            dComIfGd_addRealShadow(mShadowKey, mpHeldClawTip[1]);
        }
    }
}

/**
 * daAlink_c::hsChainShape_c::draw's clawshot half (d_a_alink_hook.inc:147-245), at paint time:
 *
 *   - slot 0's chain, tip to muzzle, 5 units a link, each link pitched by a half-sine wobble of
 *     amplitude 2.5 x the stop time whose sign flips with it (setHookshotCatchNow's 5 counting
 *     down, so a caught tip's chain twangs and settles);
 *   - slot 1's chain, tip to muzzle, straight.
 *
 * Each link is al_hs_kusari at scale 1 (the iron ball's are x2), lit by this puppet's tevStr.
 */
void daRemotePlayer_c::drawClawshotChain() {
    J3DModelData* modelData = mpClawChainData;
    if (modelData == NULL || modelData->getMaterialNum() == 0) {
        return;
    }
    J3DMaterial* material = modelData->getMaterialNodePointer(0);
    daAlink_hsChainLight_c* chainLight = (daAlink_hsChainLight_c*)&tevStr;

    j3dSys.setVtxPos(modelData->getVtxPosArray(), modelData->getVtxNum());
    j3dSys.setVtxNrm(modelData->getVtxNrmArray(), modelData->getNrmNum());
    j3dSys.setVtxCol(modelData->getVtxColorArray(0), modelData->getColNum());
    j3dSys.setTexture(modelData->getTexture());
    J3DShape::resetVcdVatCache();

    material->loadSharedDL();
    material->getShape()->loadPreDrawSetting();

    GXColor ambColor;
    ambColor.r = chainLight->AmbCol.r;
    ambColor.g = chainLight->AmbCol.g;
    ambColor.b = chainLight->AmbCol.b;
    ambColor.a = chainLight->AmbCol.a;

    GXSetChanAmbColor(GX_COLOR0A0, ambColor);
    GXSetChanMatColor(GX_COLOR0A0, g_whiteColor);

    dKy_setLight_again();
    dKy_GxFog_tevstr_set(chainLight);
    GXLoadLightObjImm(chainLight->getLightObj(), GX_LIGHT0);

    // The interpolated anchors, as his TARGET_PC block reads his (:148-154).
    const cXyz chainTopPos = mClawChainSamples.read(0, mClawTop[0]);
    const cXyz chainRootPos = mClawChainSamples.read(1, mClawRoot[0]);
    const cXyz subChainRootPos = mClawChainSamples.read(2, mClawRoot[1]);
    const cXyz subChainTopPos = mClawChainSamples.read(3, mClawTop[1]);

    cXyz maxDistance = chainRootPos - chainTopPos;
    f32 maxDistanceF = maxDistance.abs();
    f32 var_f30;
    cXyz sp98;
    csXyz sp6C;

    if (maxDistanceF > 1.0f) {
        maxDistance *= (1.0f / maxDistanceF);
        var_f30 = 0.0f;

        sp6C.set(maxDistance.atan2sY_XZ(), maxDistance.atan2sX_Z(), 0);
        sp98 = chainTopPos;

        csXyz sp64(sp6C);

        f32 sp34 = M_PI / maxDistanceF;

        f32 temp_f27;
        f32 var_f26 = 0.0f;
        f32 var_f28 = 2.5f * mClawStopTime;
        if (mClawStopTime & 1) {
            var_f28 *= -1.0f;
        }

        int chainLinks = 0;
        while (maxDistanceF > var_f30 && chainLinks < l_clawChainLinksMax) {
            temp_f27 = var_f28 * cM_fsin(sp34 * var_f30);
            s16 spC = cM_atan2s(temp_f27 - var_f26, 5.0f);
            sp64.x = static_cast<s16>(sp6C.x + spC);

            mDoMtx_stack_c::transS(sp98);
            mDoMtx_stack_c::ZXYrotM(sp64);
            mDoMtx_stack_c::multVec(&l_clawChainVec, &sp98);

            mDoMtx_stack_c::revConcat(j3dSys.getViewMtx());

            GXLoadPosMtxImm(mDoMtx_stack_c::get(), GX_PNMTX0);
            GXLoadNrmMtxImm(mDoMtx_stack_c::get(), GX_PNMTX0);

            material->getShape()->simpleDrawCache();

            sp64.z = static_cast<s16>(sp64.z + 0x3000);

            var_f26 = temp_f27;
            var_f30 += fabsf(cM_scos(spC)) * 5.0f;
            chainLinks++;
        }
    }

    maxDistance = subChainRootPos - subChainTopPos;
    maxDistanceF = maxDistance.abs();
    if (maxDistanceF > 1.0f) {
        maxDistance *= (1.0f / maxDistanceF);
        var_f30 = 0.0f;

        sp98 = subChainTopPos;
        sp6C.set(maxDistance.atan2sY_XZ(), maxDistance.atan2sX_Z(), 0);

        int subChainLinks = 0;
        while (maxDistanceF > var_f30 && subChainLinks < l_clawChainLinksMax) {
            mDoMtx_stack_c::copy(j3dSys.getViewMtx());
            mDoMtx_stack_c::transM(sp98);
            mDoMtx_stack_c::ZXYrotM(sp6C);

            GXLoadPosMtxImm(mDoMtx_stack_c::get(), GX_PNMTX0);
            GXLoadNrmMtxImm(mDoMtx_stack_c::get(), GX_PNMTX0);

            material->getShape()->simpleDrawCache();

            sp98 += maxDistance * 5.0f;
            sp6C.z = static_cast<s16>(sp6C.z + 0x3000);
            var_f30 += 5.0f;
            subChainLinks++;
        }
    }
}
