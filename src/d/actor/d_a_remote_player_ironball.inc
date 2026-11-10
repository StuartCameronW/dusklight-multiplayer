/**
 * d_a_remote_player_ironball.inc
 * Remote player puppet: the Ball and Chain (wire v19 block B, heldItem kPlayerHeldIronBall), and
 * the carry gait's resources (gear kPlayerGearIronBallGait, played by selectAnimation()).
 *
 * Dusk-only, like the rest of the puppet. Included once, at the end of d_a_remote_player.cpp and
 * after d_a_remote_player_held.inc, whose loaders it uses.
 *
 * How daAlink_c does it, and so how this does:
 *
 *   - The BALL is mHeldItemModel (al_ib, setIronBallModel, d_a_alink_ironball.inc:90-170), so here
 *     it is mpHeldModel[kPlayerHeldIronBall] and the held-item code draws it. It is not in his
 *     hand: setIronBallPos (:465-810) builds its base matrix in world space from
 *     mIronBallChainPos[0] / mIronBallChainAngle[0] (:768-770).
 *   - The CHAIN is no model at all. al_hs_kusari's model data supplies one link's shape, and
 *     hsChainShape_c::draw (d_a_alink_hook.inc:58-147), an immediate packet entered into the dark
 *     opaque list (d_a_alink.cpp:19793-19798), draws that shape once per link at 102 positions
 *     that setIronBallChainPos (d_a_alink_ironball.inc:198-449) relaxes every tick between his
 *     right hand and the ball, plus two gap-filling runs at the ends.
 *
 * What crosses the wire is the ball (its chain end, its rotation), the paid-out link count and the
 * throw state. The chain is re-solved here between THIS puppet's hand and that ball, with his
 * relaxation, so the hand end can never lag the puppet's own arm. While he carries the ball
 * (state 0) the ball itself is placed off the puppet's own left item joint, as he places his.
 */

#include "JSystem/J3DGraphBase/J3DDrawBuffer.h"

namespace {

/// His chain arrays' length (setIronBallModel, d_a_alink_ironball.inc:97-99).
const int l_ironBallChainNum = 102;
/// The longest chain he pays out: checkIronBallReturnChange's 0x64 and setIronBallPos's 100
/// (d_a_alink_ironball.inc:454, :563-564).
const s16 l_ironBallLinksMax = 100;
/// The carried chain (setIronBallPos, d_a_alink_ironball.inc:518).
const s16 l_ironBallLinksCarried = 6;
/// d_a_alink.cpp:211 and :213, which are file-static there.
const Vec l_ironBallChainVec = {0.0f, 0.0f, 10.0f};
const Vec l_ironBallCenterVec = {0.0f, 0.0f, 42.0f};
const u16 l_ironBallLogMax = 64;
/* A cap on each end's gap run in drawIronBallChain, which his draw does not have — his ball cannot
 * be further than its chain allows. Ours comes off the wire, and a bad sample must cost a short
 * chain, not thousands of draws. The clawshot chain's own TARGET_PC cap (d_a_alink_hook.inc:17). */
const int l_ironBallGapLinksMax = 600;

/// daAlink_c::hsChainShape_c (d_a_alink.h:1426-1431): an immediate packet whose draw is the chain.
class daRemotePlayer_ironBallChain_c : public J3DPacket {
public:
    daRemotePlayer_ironBallChain_c() {}
    virtual void draw() {
        daRemotePlayer_c* puppet = static_cast<daRemotePlayer_c*>(getUserArea());
        if (puppet != NULL) {
            puppet->drawIronBallChain();
        }
    }
    virtual ~daRemotePlayer_ironBallChain_c() {}
};

/// daAlink_c::setIronBallGroundVec (d_a_alink_ironball.inc:172-196), line for line: lift a link
/// that has sunk into the floor back onto it, bending its direction rather than shortening it.
int ironball_ground_vec(const cXyz* i_pos, cXyz* io_vec) {
    cXyz sp3C(i_pos->x, i_pos->y + 50.0f, i_pos->z);

    if (fopAcM_gc_c::gndCheck(&sp3C)) {
        f32 var_f30 = (fopAcM_gc_c::getGroundY() + 4.0f) - i_pos->y;
        if (var_f30 > 0.0f) {
            f32 var_f31 = io_vec->absXZ();

            io_vec->y += var_f30;

            if (var_f31 < 0.1f) {
                io_vec->z += var_f30;
            } else {
                var_f31 = var_f30 / var_f31;
                io_vec->x += io_vec->x * var_f31;
                io_vec->z += io_vec->z * var_f31;
            }

            io_vec->normalizeZP();
            return 1;
        }
    }

    return 0;
}

}  // namespace

/**
 * The ball model, the chain link's model data, the chain packet and the carry gait's two BCKs.
 * Called from setupHeldItems() — at the end of createHeap — so a short heap costs these and never
 * a body model. Returns the ball, which the caller files as mpHeldModel[kPlayerHeldIronBall].
 *
 * Each is daAlink_c's own resource with his own buffer literal and init path:
 *   - ball: AL_IB 0x2800, initModelEnv 0 (setIronBallModel, d_a_alink_ironball.inc:93)
 *   - chain link: AL_HS_KUSARI 0x1000, model DATA only, as his mpItemModelData (:94)
 *   - gait: ANM_IRONBALL_WAIT {IBWAIT, IBWAIT} and ANM_IRONBALL_WALK {IBWALK, IBWALK}
 *     (d_a_alink.cpp:648-649), one BCK each since each row names one resource for both halves
 *
 * All optional. No ball: the puppet draws empty-handed (the held-item warning names it). No chain:
 * a ball with no chain. No gait: the ordinary gait, as before this existed.
 */
J3DModel* daRemotePlayer_c::setupIronBall() {
    for (int i = 0; i < 2; i++) {
        mIronBallGaitAnm[i].mpUnder = NULL;
        mIronBallGaitAnm[i].mpUpper = NULL;
        mIronBallGaitAnm[i].mpUnderBas = NULL;
    }
    mpIronBallChainData = NULL;
    mpIronBallChainPacket = NULL;
    mIronBallLinks = l_ironBallLinksCarried;
    mIronBallState = 0;
    mIronBallChainLive = false;
    mIronBallSolvedTick = 0;
    mIronBallShadowKey = 0;
    mNetIronBallGait = false;
    mNetIronBallRel = cXyz::Zero;
    mNetIronBallAngX = 0;
    mNetIronBallAngY = 0;
    mNetIronBallLinks = 0;
    mNetIronBallState = 0;
    mGaitPrevIronBall = false;
    mLoggedIronBallGait = false;
    mLoggedIronBallDraw = false;
    mIronBallLogCount = 0;

    JKRHeap* heap = JKRGetCurrentHeap();
    const s32 freeBefore = heap != NULL ? heap->getFreeSize() : 0;

    const daAlink_c::daAlink_ANM gaitID[2] = {
        daAlink_c::ANM_IRONBALL_WAIT, daAlink_c::ANM_IRONBALL_WALK};
    bool gaitOk = true;
    for (int i = 0; i < 2; i++) {
        if (!load_gait_anm(&mIronBallGaitAnm[i], gaitID[i], 0)) {
            gaitOk = false;
        }
    }
    // A pair or nothing: half of it would put a WAITS cycle against an IBWALK one in band 1.
    if (!gaitOk) {
        for (int i = 0; i < 2; i++) {
            mIronBallGaitAnm[i].mpUnder = NULL;
            mIronBallGaitAnm[i].mpUpper = NULL;
            mIronBallGaitAnm[i].mpUnderBas = NULL;
        }
        Log.warn("Puppet {} has no iron-ball carry gait; a player carrying the ball will walk the "
                 "ordinary gait",
            mPlayerId);
    }

    J3DModel* ball = init_model_env(
        held_load_aram_bmd(dRes_ID_ALANM_BMD_AL_IB_e, 0x2800, "iron ball"), 0, "held iron ball");
    if (ball != NULL) {
        mpIronBallChainData =
            held_load_aram_bmd(dRes_ID_ALANM_BMD_AL_HS_KUSARI_e, 0x1000, "iron ball chain");
        if (mpIronBallChainData != NULL && mpIronBallChainData->getMaterialNum() > 0) {
            mpIronBallChainPacket = JKR_NEW daRemotePlayer_ironBallChain_c();
            if (mpIronBallChainPacket != NULL) {
                mpIronBallChainPacket->setUserArea((uintptr_t)this);
            }
        } else {
            mpIronBallChainData = NULL;
        }
    }

    // The measured cost, which the heap budget in create() is sized from.
    Log.info("Puppet {} iron ball: ball {}, chain {}, carry gait {}; {} bytes of heap ({} -> {} "
             "free)",
        mPlayerId, ball != NULL ? "ok" : "MISSING",
        mpIronBallChainPacket != NULL ? "ok" : "MISSING", gaitOk ? "ok" : "MISSING",
        heap != NULL ? freeBefore - heap->getFreeSize() : 0, freeBefore,
        heap != NULL ? heap->getFreeSize() : 0);
    return ball;
}

void daRemotePlayer_c::setNetworkIronBall(
    bool i_gait, const cXyz& i_ballRel, s16 i_angX, s16 i_angY, u8 i_links, u8 i_state) {
    mNetIronBallGait = i_gait;
    mNetIronBallRel = i_ballRel;
    mNetIronBallAngX = i_angX;
    mNetIronBallAngY = i_angY;
    mNetIronBallLinks = i_links;
    mNetIronBallState = i_state;
}

bool daRemotePlayer_c::ironBallInHands() const {
    return mIronBallState == 0 || mIronBallState == 8;
}

/**
 * Pose the ball and solve the chain. From setHeldItemMatrix(), i.e. from setMatrix() after the
 * body's calc, which is where daAlink_c's setItemMatrix calls setIronBallPos
 * (d_a_alink.cpp:6070-6071). The caller calc()s the ball afterwards.
 *
 * setIronBallPos, in its own order: the hand end off his right item joint, then the ball, then
 * setIronBallChainPos, then the ball's base matrix and centre. What it does NOT do is everything
 * that is not drawing: the ball's flight, its bg and attack checks, the hit sound and effects, and
 * the state machine. Those ran on the sender; their result is the wire's ball.
 */
void daRemotePlayer_c::setIronBallMatrix(J3DModel* i_ball) {
    const u32 tick = g_Counter.mCounter0;
    const u8 state = mNetIronBallState & dusk::mp::kPlayerIronBallStateMask;

    /* The chain's hand end (d_a_alink_ironball.inc:473-484). Joint 15 is l_rightItemJointNo. In
     * state 0 the extra transM/XrotM turns the attachment round for the carried pose. */
    mDoMtx_stack_c::copy(model->getAnmMtx(l_rightItemJointNo));
    mDoMtx_stack_c::transM(-1.0f, -6.0f, -3.6f);
    mDoMtx_stack_c::XYZrotM(cM_deg2s(150.0f), cM_deg2s(-81.0f), cM_deg2s(111.0f));
    if (state == 0) {
        mDoMtx_stack_c::transM(0.0f, 0.0f, 10.0f);
        mDoMtx_stack_c::XrotM(0x7FFF);
    }
    mDoMtx_stack_c::multVec(&l_ironBallChainVec, &mIronBallHandTop);
    mDoMtx_stack_c::multVecZero(&mIronBallHandRoot);
    mDoMtx_MtxToRot(mDoMtx_stack_c::get(), &mIronBallHandRootAngle);

    /* (Re)start the chain the way setIronBallModel starts his (:124-134): every link on the left
     * hand, the link roll stepping 0x3000, no velocity. Whenever the ball was not posed on the
     * previous tick — it has just come out, or the puppet just spawned while it was out. */
    if (!mIronBallChainLive || tick != mIronBallSolvedTick + 1) {
        cXyz leftHand;
        mDoMtx_multVecZero(model->getAnmMtx(l_leftHandJointNo), &leftHand);
        csXyz* angle = mIronBallChainAngle;
        for (int i = 0; i < l_ironBallChainNum; i++, angle++) {
            if (i < 2) {
                angle->set(0, shape_angle.y, static_cast<s16>(i * 0x3000));
            } else {
                angle->set(0, shape_angle.y, static_cast<s16>(angle[-1].z + 0x3000));
            }
            mIronBallChainVel[i] = cXyz::Zero;
            mIronBallChainPos[i] = leftHand;
        }
        if (mIronBallLogCount < l_ironBallLogMax) {
            mIronBallLogCount++;
            Log.debug("Puppet {} iron ball out: chain (re)started on the left hand, state {}",
                mPlayerId, static_cast<unsigned>(state));
        }
    }

    if (state == 0) {
        /* Carried (:506-518): off this puppet's own left item joint, so the ball sits in its
         * hands exactly as his sits in his. The rotation is his facing, or in his two aiming
         * procs the joint's own (the wire's kPlayerIronBallJointRot). */
        mDoMtx_stack_c::copy(model->getAnmMtx(l_leftItemJointNo));
        mDoMtx_stack_c::transM(-35.3f, -9.5f, -16.0f);
        mDoMtx_stack_c::XYZrotM(cM_deg2s(61.5f), cM_deg2s(-2.5f), cM_deg2s(50.3f));
        mDoMtx_stack_c::multVecZero(mIronBallChainPos);
        if ((mNetIronBallState & dusk::mp::kPlayerIronBallJointRot) != 0) {
            mDoMtx_MtxToRot(mDoMtx_stack_c::get(), mIronBallChainAngle);
        } else {
            mIronBallChainAngle->set(-0x4000, shape_angle.y, 0);
        }
        mIronBallLinks = l_ironBallLinksCarried;
    } else {
        // Out of his hands: where the sender's own simulation put it.
        mIronBallChainPos[0] = current.pos + mNetIronBallRel;
        mIronBallChainAngle[0].set(mNetIronBallAngX, mNetIronBallAngY, 0);
        s16 links = mNetIronBallLinks;
        if (links < 1) {
            links = 1;
        } else if (links > l_ironBallLinksMax) {
            links = l_ironBallLinksMax;
        }
        mIronBallLinks = links;
    }

    if (state != mIronBallState && mIronBallLogCount < l_ironBallLogMax) {
        mIronBallLogCount++;
        Log.debug("Puppet {} iron ball state {} -> {}: {} links, ball {:.1f} from the hand",
            mPlayerId, static_cast<unsigned>(mIronBallState), static_cast<unsigned>(state),
            mIronBallLinks, mIronBallChainPos[0].abs(mIronBallHandTop));
    }
    mIronBallState = state;

    setIronBallChainPos();

    // The ball's base matrix and centre (:768-775).
    mDoMtx_stack_c::transS(mIronBallChainPos[0]);
    mDoMtx_stack_c::ZXYrotM(mIronBallChainAngle[0]);
    i_ball->setBaseTRMtx(mDoMtx_stack_c::get());
    mDoMtx_stack_c::multVec(&l_ironBallCenterVec, &mIronBallCenter);
    mIronBallBgChkPos.set(mIronBallCenter.x, mIronBallCenter.y - 32.0f, mIronBallCenter.z);

    mIronBallChainLive = true;
    mIronBallSolvedTick = tick;
}

/**
 * daAlink_c::setIronBallChainPos (d_a_alink_ironball.inc:198-449), for the two branches that
 * only lay links between a hand and a ball:
 *
 *   - state 2, swung overhead: a straight line out of the ball towards the hand (:221-235);
 *   - every other state: his last branch (:364-420), the two-pass relaxation — out of the ball
 *     with gravity and the links' velocity, then back from the hand, each link lifted off the
 *     floor — then the link rotations and the parked tail, which every branch shares (:421-449).
 *
 * ★ His states 1, 3 and 4 (the wind-up and the launch) use a third branch that simulates the BALL
 * as the chain's last link, and state 7 (pull-back) runs the ball's bg check inside the chain
 * pass. Both exist to MOVE the ball, which the sender has already done; with the ball pinned at
 * the wire's position they reduce to laying the links between the two ends, which is what the
 * last branch does. That is a reasoned equivalence, not a measured one: the links of a winding-up
 * chain may hang slightly differently from his. Any gap at the ball end is filled by the draw's
 * own gap run (drawIronBallChain), as his is.
 */
void daRemotePlayer_c::setIronBallChainPos() {
    const daAlinkHIO_ironBall_c1& hio = daAlinkHIO_ironBall_c0::m;
    const s16 links = mIronBallLinks;
    cXyz sp2A4;
    cXyz sp28C;
    cXyz sp280;

    mIronBallChainPos[links + 1] = mIronBallHandRoot;
    mIronBallChainAngle[links + 1] = mIronBallHandRootAngle;

    if (mIronBallState == 2) {
        cXyz* var_r29 = mIronBallChainPos + 1;
        cXyz* var_r27 = var_r29 - 1;
        cXyz* var_r26 = mIronBallChainVel + 1;

        sp2A4 = mIronBallHandTop - *mIronBallChainPos;
        sp2A4.normalizeZP();

        for (int i = 0; i < links; i++, var_r29++, var_r26++) {
            sp28C = *var_r29;
            *var_r29 = *var_r27 + (sp2A4 * 10.0f);

            *var_r26 = (*var_r29 - sp28C) * hio.mChainSpeedRate;
            var_r27 = var_r29;
        }
    } else {
        // His own expression; 4 cannot reach this branch on his side, 5 is the throw.
        const f32 var_f30 = (mIronBallState == 4 || mIronBallState == 5) ? hio.mThrowChainGravity :
                                                                           hio.mChainGravity;

        cXyz* var_r29 = mIronBallChainPos + 1;
        cXyz* var_r27 = var_r29 - 1;
        cXyz* var_r26 = mIronBallChainVel + 1;

        for (int i = 0; i < links; i++, var_r29++, var_r26++) {
            sp2A4 = (*var_r29 - *var_r27) + *var_r26;
            sp2A4.y += var_f30;
            sp2A4.normalizeZP();

            *var_r29 = *var_r27 + (sp2A4 * 10.0f);
            var_r27 = var_r29;
        }

        var_r29 = &mIronBallChainPos[links];
        var_r26 = &mIronBallChainVel[links];
        var_r27 = var_r29 + 1;

        /* The carried ball's sway on the walk's heel strike (:388-393): his slot 1 playing IBWALK
         * at more than 0.8 of the blend, within frames 24..end or 0..3. Read off THIS puppet's own
         * slot 1, which plays the same BCK at the same phase while the carry gait is on. */
        const J3DAnmTransform* walk = mIronBallGaitAnm[1].mpUnder;
        const BOOL sp2C = mIronBallState == 0 && walk != NULL &&
                          mAnmPackUnder[1].getAnmTransform() == walk &&
                          mAnmPackUnder[1].getRatio() > 0.8f &&
                          (mUnderFrameCtrl[1].getFrame() >= 24.0f ||
                              mUnderFrameCtrl[1].getFrame() <= 3.0f);

        f32 var_f31 = 5.0f * (5.0f * (mAnmPackUnder[1].getRatio() - 0.8f));
        const f32 var_f28 = var_f31 * cM_scos(shape_angle.y);
        var_f31 *= cM_ssin(shape_angle.y);

        for (int i = links; i > 0; i--, var_r29--, var_r26--) {
            sp28C = *var_r29;

            if (i == links) {
                *var_r29 = mIronBallHandTop;
            } else {
                sp2A4 = (*var_r29 - *var_r27) + *var_r26;
                if (sp2C) {
                    sp2A4.x += var_f31;
                    sp2A4.z += var_f28;
                }
                sp2A4.y += var_f30;

                sp280 = sp2A4;
                sp2A4.normalizeZP();

                *var_r29 = *var_r27 + (sp2A4 * 10.0f);
                if (ironball_ground_vec(var_r29, &sp280)) {
                    *var_r29 = *var_r27 + (sp280 * 10.0f);
                }
            }

            var_r27 = var_r29;
            *var_r26 = (*var_r29 - sp28C) * hio.mChainSpeedRate;
        }
    }

    // The link rotations, each facing back down the chain, its roll stepping 0x3000 (:421-442).
    csXyz* sp3C = mIronBallChainAngle + 1;
    cXyz* var_r29 = mIronBallChainPos + 1;
    cXyz* var_r27 = var_r29 - 1;
    s16 spE = static_cast<s16>(mIronBallChainAngle->z + 0x3000);
    for (int i = 0; i < links; i++, sp3C++, var_r29++, spE = static_cast<s16>(spE + 0x3000)) {
        sp2A4 = (*var_r27 - *var_r29);

        s16 sp10;
        if (sp2A4.absXZ() < 1.0f) {
            sp10 = sp3C[-1].y;
        } else {
            sp10 = sp2A4.atan2sX_Z();
        }

        sp3C->set(sp2A4.atan2sY_XZ(), sp10, spE);
        var_r27 = var_r29;
    }

    // Everything past the hand is parked on it (:444-449).
    for (int i = links + 2; i < l_ironBallChainNum; i++) {
        mIronBallChainPos[i] = mIronBallChainPos[links + 1];
        mIronBallChainAngle[i] = mIronBallChainAngle[links + 1];
        mIronBallChainVel[i] = cXyz::Zero;
    }
}

/**
 * Enter the chain and give the thrown ball its shadow. From draw(), after drawHeldItems() — which
 * drew the ball — and addHeldItemShadow(), mirroring daAlink_c's draw (d_a_alink.cpp:19771-19820)
 * and shadowDraw (:19310-19321).
 *
 * ★ Hang 4: the packet is entered once per pass. draw() has already returned on a second
 * entry-draw within one sim tick, and actors are not drawn on interpolated frames at all
 * (fpcLf_Draw), so this cannot prepend the packet onto a list it already heads or sits in.
 */
void daRemotePlayer_c::drawIronBall() {
    if (mHeldDrawnKind != dusk::mp::kPlayerHeldIronBall || !mIronBallChainLive) {
        return;
    }

    if (mpIronBallChainPacket != NULL && mpIronBallChainData != NULL) {
        dComIfGd_getOpaListDark()->entryImm(mpIronBallChainPacket, 0);
        // AlinkInterp's capture (:19800-19810): the packet draws at paint time from these.
        mIronBallPosSamples.capture(mIronBallChainPos, l_ironBallChainNum);
        mIronBallAngleSamples.capture(mIronBallChainAngle, l_ironBallChainNum);
        mIronBallHandSamples.capture(&mIronBallHandTop, 1);
    }

    // Out of his hands the ball has a shadow of its own, his field_0x32d4 (:19312-19320).
    J3DModel* ball = mpHeldModel[dusk::mp::kPlayerHeldIronBall];
    if (!ironBallInHands() && ball != NULL && fopAcM_gc_c::gndCheck(&mIronBallBgChkPos)) {
        mIronBallShadowKey = dComIfGd_setShadow(mIronBallShadowKey, 0, ball, &mIronBallCenter,
            150.0f, 32.0f, mIronBallBgChkPos.y, fopAcM_gc_c::getGroundY(),
            *fopAcM_gc_c::getGroundCheck(), &tevStr, 0, 1.0f,
            dDlst_shadowControl_c::getSimpleTex());
    }

    if (!mLoggedIronBallDraw) {
        mLoggedIronBallDraw = true;
        Log.debug("Puppet {} drawing the iron ball: chain {}, state {}, {} links, ball {:.1f} from "
                  "the hand",
            mPlayerId, mpIronBallChainPacket != NULL ? "entered" : "MISSING",
            static_cast<unsigned>(mIronBallState), mIronBallLinks,
            mIronBallChainPos[0].abs(mIronBallHandTop));
    }
}

/**
 * daAlink_c::hsChainShape_c::draw's iron-ball half (d_a_alink_hook.inc:58-147), at paint time:
 * one al_hs_kusari link per chain position, scaled x2, lit by this puppet's tevStr, then a run of
 * links to close each end's gap — hand root to the last solved link, and the ball to link 1.
 */
void daRemotePlayer_c::drawIronBallChain() {
    J3DModelData* modelData = mpIronBallChainData;
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

    // The interpolated chain, as his TARGET_PC block reads his (:59-67).
    cXyz chainPositions[l_ironBallChainNum];
    csXyz chainAngles[l_ironBallChainNum];
    for (int i = 0; i < l_ironBallChainNum; ++i) {
        chainPositions[i] = mIronBallPosSamples.read(i, mIronBallChainPos[i]);
        chainAngles[i] = mIronBallAngleSamples.read(i, mIronBallChainAngle[i]);
    }
    const cXyz chainHand = mIronBallHandSamples.read(0, mIronBallHandTop);

    cXyz* ppos = chainPositions + 1;
    csXyz* pangle = chainAngles + 1;

    int i;
    for (i = 0; i < l_ironBallChainNum - 1; i++, ppos++, pangle++) {
        mDoMtx_stack_c::copy(j3dSys.getViewMtx());
        mDoMtx_stack_c::transM(*ppos);
        mDoMtx_stack_c::ZXYrotM(*pangle);
        mDoMtx_stack_c::scaleM(2.0f, 2.0f, 2.0f);

        GXLoadPosMtxImm(mDoMtx_stack_c::get(), GX_PNMTX0);
        GXLoadNrmMtxImm(mDoMtx_stack_c::get(), GX_PNMTX0);

        material->getShape()->simpleDrawCache();
    }

    cXyz spC8;
    cXyz spBC;

    for (i = 0; i < 2; i++) {
        cXyz* sp48;
        csXyz* sp44;

        s16 sp14;
        s16 sp12;
        s16 sp10;
        s16 spE;

        if (i == 0) {
            sp48 = chainPositions + mIronBallLinks;
            spC8 = chainHand - *sp48;

            sp44 = chainAngles + mIronBallLinks;
            spE = 0x3000;
        } else {
            sp48 = chainPositions;
            mDoMtx_stack_c::transS(*(chainPositions + 1));
            mDoMtx_stack_c::ZXYrotM(*(chainAngles + 1));
            mDoMtx_stack_c::multVec(&l_ironBallChainVec, &spC8);
            spC8 -= *sp48;

            sp44 = chainAngles + 1;
            spE = -0x3000;
        }

        sp10 = (s16)sp44->y;
        sp14 = sp44->z + spE;
        f32 var_f29 = spC8.abs();

        spBC = *sp48;

        if (var_f29 > 5.0f) {
            f32 sp40 = spC8.absXZ();
            if (sp40 >= 1.0f) {
                sp10 = spC8.atan2sX_Z();
            }

            sp12 = spC8.atan2sY_XZ();
            cXyz spB0 = spC8 * (10.0f / var_f29);

            int gapLinks = 0;
            while (var_f29 > 5.0f && gapLinks < l_ironBallGapLinksMax) {
                gapLinks++;
                mDoMtx_stack_c::copy(j3dSys.getViewMtx());
                mDoMtx_stack_c::transM(spBC);
                mDoMtx_stack_c::ZXYrotM(sp12, sp10, sp14);
                mDoMtx_stack_c::scaleM(2.0f, 2.0f, 2.0f);

                GXLoadPosMtxImm(mDoMtx_stack_c::get(), GX_PNMTX0);
                GXLoadNrmMtxImm(mDoMtx_stack_c::get(), GX_PNMTX0);

                material->getShape()->simpleDrawCache();

                spBC += spB0;
                var_f29 -= 10.0f;
                sp14 = static_cast<s16>(sp14 + spE);
            }
        }
    }
}
