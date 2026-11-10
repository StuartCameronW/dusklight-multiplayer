/**
 * d_a_remote_player_footfx.inc
 * Remote player puppet: the ground-contact effects — footstep dust / grass / water kick-up, the
 * landing, roll and slide dust, water ripples, the splash on entering water, the run splash, and
 * wet footprints. Everything daAlink_c::setEffect (d_a_alink_effect.inc:332-654) does through his
 * field_0x2e54 (a dPaPoF_c), plus setRunSplash (:1597-1621).
 *
 * Dusk-only, like the rest of the puppet. Included once, at the end of d_a_remote_player.cpp.
 * NO wire bytes: everything here is derived from what the puppet already has.
 *
 * How daAlink_c does it, and so how this does:
 *
 *   - THE MACHINE. Every tick, setEffect calls field_0x2e54.setEffectFour (d_particle_copoly.cpp
 *     :181) with his position and up to five contact points — centre, two hands, two feet — each
 *     either a point or NULL. A point gets the effect its POLYGON asks for (dKy_pol_efftype_get /
 *     _efftype2_get: dust, grass, water, sand, snow, in-water smoke), on the ground polygon or, in
 *     shallow water, the water polygon (dPa_control_c::setCommonPoly, d_particle.cpp:1838). The
 *     same call also runs checkWaterIn every tick (d_particle_copoly.cpp:20-59): ripples while the
 *     water is less than his height above the feet, and a fopKyM_createWpillar splash on the tick
 *     he enters water deeper than mWaterSurfaceEffectHeight. The puppet owns ANOTHER dPaPoF_c and
 *     calls the same function with the same arguments; the effect tables, colours and offsets are
 *     the engine's, not copied.
 *   - WHICH POINTS. His field_0x2f9d, a per-tick bit set (reset at d_a_alink.cpp:17942): 0x04 the
 *     centre, 0x20 the left foot, 0x40 the right. Locomotion sets a foot on the tick his run BCK
 *     crosses frame 15 (left) or 3 (right) (setBlendMoveAnime, d_a_alink.cpp:7813-7817); the
 *     landing, roll and slide procs set the centre or both feet from their own init / frame
 *     windows. The puppet plays the same BCKs at the same frames, so it asks its OWN frame
 *     controller the same checkPass question, and reads the proc off the override BCK it is
 *     drawing (each one below is played by exactly one proc).
 *   - HOW. His mEffProc (setFootEffectProcType, d_a_alink_effect.inc:303-310) picks the flag words
 *     setCommonPoly is called with (ring vs plain ids) and the per-type emitter tweaks applied
 *     after it (m_fEffParamProc, :16-23 and :59-256). Transcribed below, human branches only.
 *   - THE FLOOR. His dPaPoF_c reads mLinkAcch (m_gnd, m_wtr, GetGroundH, ChkWaterHit). The puppet
 *     owns a dBgS_LinkAcch (Link's own pass-through flags) that it never CrrPos's — a CrrPos
 *     writes the position and calls the floor's RideCallBack with this actor, neither of which a
 *     replay may do — and fills it once a tick with CrrPos's own ground query and water query
 *     (d_bg_s_acch.cpp:124-164 and :408-453), in his water mode 2 (d_a_alink.cpp:4546).
 *
 * NOT reproduced, and why:
 *   - Sand and snow FOOTPRINTS (setFootMark kinds 1 and 2) and their "sunk" ripples: both key on
 *     mSinkShapeOffset, which is not on the wire. Wet footprints (kind 0) are reproduced.
 *   - The landing that goes straight into a run (checkLandAction's procMoveInit branch,
 *     d_a_alink.cpp:10760-10770): it has no BCK of its own, so the puppet cannot tell it from any
 *     other end of a jump. Likewise the sword-move landings in d_a_alink_cut.inc, the guard and
 *     damage procs, and sumo. Each would need the sender's field_0x2f9d — see the report.
 *   - The swim wake (ZI_J_HIKINAMI_A), the bubbles (LK_ABUKU), the vine-climb dust and the board:
 *     swim / climb / board state is not on the wire as such.
 *   - The centre-point nudges keyed on dComIfGp_checkPlayerStatus0/1 (:487-506): those are the
 *     LOCAL player's statuses, not the puppet's.
 *   - Wolf: the puppet has no wolf.
 *   - Footstep dust while wading in Iron Boots (FLG0_WATER_IN_MOVE, :7802-7808): that flag is set
 *     only in a heavy state underwater, whose clamped gait never leaves band 1 anyway.
 */

#include "d/d_bg_s_acch.h"
#include "d/d_k_wmark.h"
#include "d/d_particle_copoly.h"

namespace {

/// His field_0x30bc / field_0x30be, the foot joints mLeftFootPos / mRightFootPos are read from
/// (d_a_alink.cpp:5614-5615), human values (d_a_alink_wolf.inc:569-570).
const u16 l_footFxLeftJointNo = 21;
const u16 l_footFxRightJointNo = 26;

/// setBlendMoveAnime's foot-down frames on framectrl1, as literals there (d_a_alink.cpp:7622-7623,
/// :7639-7640, :7647-7648, :7731-7737): var_f27 is the LEFT foot, var_f26 the right.
const f32 l_footFxRunLeftFrame = 15.0f;
const f32 l_footFxRunRightFrame = 3.0f;
const f32 l_footFxHeavyLeftFrame = 0.0f;
const f32 l_footFxHeavyRightFrame = 23.0f;
const f32 l_footFxBallLeftFrame = 18.0f;
const f32 l_footFxBallRightFrame = 3.0f;
const f32 l_footFxStepNear = 8.0f;
const f32 l_footFxStepFar = 19.0f;

/// field_0x2f9d's bits (d_a_alink_effect.inc:570-572).
const u8 l_footFxPtCenter = 0x04;
const u8 l_footFxPtLeftFoot = 0x20;
const u8 l_footFxPtRightFoot = 0x40;

/// field_0x2fc4: ticks a footprint stays wet after leaving water (d_a_alink_effect.inc:605).
const u8 l_footFxWetTicks = 0x96;
/// His field_0x3420 for the human form (d_a_alink_wolf.inc:543): the water pillar's scale.
const f32 l_footFxPillarScale = 1.0f;
/// procSlip's "still sliding" speed (d_a_alink.cpp:16727), and procFrontRoll's / procSideRoll's
/// frame windows (:16310-16319, :16541-16546), literals there.
const f32 l_footFxSlipMinSpeed = 2.5f;
const f32 l_footFxFrontRollFrom = 6.0f;
const f32 l_footFxFrontRollTo = 16.0f;
const f32 l_footFxSideRollTo = 18.0f;

/// One-shot logging, per kind of effect.
enum {
    FOOTFX_LOG_RUN_STEP,
    FOOTFX_LOG_LAND,
    FOOTFX_LOG_ROLL,
    FOOTFX_LOG_SLIDE,
    FOOTFX_LOG_SKID,
    FOOTFX_LOG_RIPPLE,
    FOOTFX_LOG_PILLAR,
    FOOTFX_LOG_RUN_SPLASH,
    FOOTFX_LOG_WET_MARK,
    FOOTFX_LOG_NUM,
};
const u8 l_footFxLogMax = 4;

/// dPa_control_c::setCommonPoly's effect types (d_particle.cpp:1842-1851), 16 the water ring
/// dPaPo_c::setEffectCenter substitutes (d_particle_copoly.cpp:123-126), 0xFF nothing.
const char* footfx_type_name(u8 i_type) {
    switch (i_type) {
    case 0:
        return "dust";
    case 1:
        return "grass";
    case 2:
        return "water";
    case 3:
        return "sand";
    case 4:
        return "snow";
    case 5:
        return "underwater-smoke";
    case 16:
        return "water-ring";
    case 0xFF:
        return "none";
    default:
        return "?";
    }
}

/// Is the override BCK i_under the one daAlink_c's table plays for i_anm? These ids are all outside
/// getMainBckData's sword and shield ranges (ANM_ATN_WAIT_LEFT..ANM_STEP_TURN, < ANM_SWIM_WAIT), so
/// the table row is the only BCK the sender can be playing for them.
bool footfx_is_anm(u16 i_under, daAlink_c::daAlink_ANM i_anm) {
    return i_under == daAlink_c::m_anmDataTable[i_anm].m_bckData.m_underID;
}

/**
 * J3DFrameCtrl::checkPass(i_pass) (J3DAnimation.cpp:23-112), asked of the step a controller has
 * JUST taken rather than the one it is about to take.
 *
 * daAlink_c asks inside his proc, before allAnimePlay steps the controller; the puppet asks from
 * execute() after animePlay(), so it rewinds the step first: the controller was seated at
 * frame - rate (setDoubleAnm / selectActionAnimation both seat it so) and update() then added the
 * rate and wrapped a loop by (end - loop). Only the forward cases are needed — every BCK asked here
 * plays at a positive rate.
 */
bool footfx_passed(const daPy_frameCtrl_c& i_ctrl, f32 i_pass) {
    const f32 rate = i_ctrl.getRate();
    if (rate <= 0.0f) {
        return false;
    }
    const f32 end = i_ctrl.getEnd();
    const f32 loop = i_ctrl.getLoop();
    f32 seat = i_ctrl.getFrame() - rate;
    f32 next = seat + rate;

    if (i_ctrl.getAttribute() == J3DFrameCtrl::EMode_LOOP) {
        const f32 span = end - loop;
        if (seat < loop && span > 0.0f) {
            seat += span;
            next = seat + rate;
        }
        if (next >= end && span > 0.0f) {
            while (next >= end) {
                next -= span;
            }
            return (seat <= i_pass && i_pass < end) || (loop <= i_pass && i_pass < next);
        }
        return seat <= i_pass && i_pass < next;
    }

    if (next >= end) {
        next = end - 0.001f;
    }
    return seat <= i_pass && i_pass < next;
}

/// daAlink_c::setEmitterPolyColor (d_a_alink_effect.inc:33-40) with the tevStr passed in.
JPABaseEmitter* footfx_emit_poly(u32* io_id, u16 i_name, cBgS_PolyInfo& i_poly, const cXyz* i_pos,
    const dKy_tevstr_c* i_tevStr, const csXyz* i_rot) {
    *io_id = dComIfGp_particle_setPolyColor(
        *io_id, i_name, i_poly, i_pos, i_tevStr, i_rot, NULL, 0, NULL, -1, NULL);
    dComIfGp_particle_levelEmitterOnEventMove(*io_id);
    return dComIfGp_particle_getEmitter(*io_id);
}

}  // namespace

/**
 * Once, lazily, from the first updateFootFx(): the puppet's dPaPoF_c on its own acch, with his
 * offsets — mWaterSurfaceEffectHeight and his mHeight (d_a_alink.cpp:4672, d_a_alink_wolf.inc
 * :539-540) — and the acch in his water mode (d_a_alink.cpp:4540-4546).
 */
void daRemotePlayer_c::setupFootFx() {
    STATIC_ASSERT(FOOTFX_LOG_NUM == FOOT_FX_LOG_NUM);
    mFootFxReady = true;
    mFootAcch.SetActorPid(fopAcM_GetID(this));
    mFootAcch.ClrWaterNone();
    mFootAcch.SetWaterCheckOffset(10000.0f);
    mFootAcch.SetWtrChkMode(2);
    mFootPoly.init(&mFootAcch, daAlinkHIO_basic_c0::m.mWaterSurfaceEffectHeight, l_linkHeight);
    mFootFxProc = daAlink_c::EFFPROC_NONE;
    mFootFxProcSet = daAlink_c::EFFPROC_NONE;
    mFootFxWetTimer = 0;
    mFootFxPrevActUnder = l_actNone;
    mFootFxPrevSlip = false;
    mFootFxPrevPosValid = false;
    mFootFxRippling = false;
    for (int i = 0; i < FOOTFX_LOG_NUM; i++) {
        mFootFxLogCount[i] = 0;
    }
    Log.info("Puppet {} foot fx ready: water-effect height {:.1f}, ripple height {:.1f}", mPlayerId,
        daAlinkHIO_basic_c0::m.mWaterSurfaceEffectHeight, l_linkHeight);
}

/**
 * The two queries of dBgS_Acch::CrrPos that his dPaPoF_c reads the results of, on mFootAcch, at
 * the puppet's position:
 *
 *   - the ground: GroundCheckInit + GroundCheck's query, raised by m_gnd_chk_offset (60) with the
 *     acch's Link pass flags (d_bg_s_acch.cpp:124-164). Not its writes — no pos.y snap, no
 *     RideCallBack — and not the thin-ceiling clamp, which Link turns off himself
 *     (SetGndThinCellingOff, d_a_alink.cpp:4545).
 *   - the water: CrrPos's water block, mode 2 (an underwater-roof cap), verbatim (:408-453).
 *
 * One GroundCross, one RoofChk and one WaterChk a tick.
 */
void daRemotePlayer_c::footFxBgCheck() {
    dBgS& bgs = dComIfG_Bgsp();

    mFootAcch.m_gnd.SetExtChk(mFootAcch);
    cXyz probe = current.pos;
    probe.y += mFootAcch.m_gnd_chk_offset;
    mFootAcch.m_gnd.SetPos(&probe);
    mFootAcch.m_ground_h = bgs.GroundCross(&mFootAcch.m_gnd);
    if (mFootAcch.m_ground_h != -G_CM3D_F_INF) {
        mFootAcch.SetGroundHit();
    } else {
        mFootAcch.ClrGroundHit();
    }

    mFootAcch.ClrWaterHit();
    mFootAcch.ClrWaterIn();
    mFootAcch.m_wtr.SetHeight(-G_CM3D_F_INF);

    const f32 bottom =
        mFootAcch.m_ground_h == -G_CM3D_F_INF ? current.pos.y - 50.0f : mFootAcch.m_ground_h;
    dBgS_RoofChk roofChk;
    roofChk.SetUnderwaterRoof();
    roofChk.SetPos(current.pos);
    f32 top = bgs.RoofChk(&roofChk);
    if (top == G_CM3D_F_INF) {
        top = current.pos.y + 1000000.0f;
    }

    cXyz ground = current.pos;
    ground.y = bottom;
    mFootAcch.m_wtr.Set(ground, top);
    mFootAcch.m_wtr.SetPassChkInfo(mFootAcch);
    if (bgs.WaterChk(&mFootAcch.m_wtr)) {
        mFootAcch.SetWaterHit();
        if (mFootAcch.m_wtr.GetHeight() > current.pos.y) {
            mFootAcch.SetWaterIn();
        }
    }
}

/// setFootEffectProcType (d_a_alink_effect.inc:303-310): a real change of kind drops the old
/// kind's emitters.
void daRemotePlayer_c::setFootFxProcType(u8 i_type) {
    if (i_type != mFootFxProcSet) {
        mFootPoly.clearFourAllID();
    }
    mFootFxProc = i_type;
    mFootFxProcSet = i_type;
}

/**
 * m_fEffParamProc[mEffProc] (d_a_alink_effect.inc:16-23), the human branches of each:
 * setEffectFrontRollParam (:59-74), setEffectSlipParam (:76-139), setEffectSmallLandParam
 * (:205-256), setEffectRunParam (:141-203). setEffectLandParam is empty and sumo is not
 * reproduced. They re-tune the emitters setEffectFour just set, by the type each point got.
 */
void daRemotePlayer_c::setFootFxParam() {
    static Vec const slipSmokeScale = {0.69999999f, 0.69999999f, 0.69999999f};
    static Vec const smallLandSmokeScale = {0.8f, 0.8f, 0.8f};
    static Vec const runWaterScale = {0.57999998f, 0.57999998f, 0.57999998f};
    static Vec const runWaterCenterScale = {1.2f, 1.2f, 1.2f};
    static Vec const runWaterSmokeScale = {0.69999999f, 0.69999999f, 0.69999999f};

    JGeometry::TVec3<f32> trans;
    JPABaseEmitter* emitter;
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 2; j++) {
            const u8 type = mFootPoly.getTypeFour(i, j);
            switch (mFootFxProc) {
            case daAlink_c::EFFPROC_FRONT_ROLL:
                if (type == 0) {
                    emitter = mFootPoly.getEmitterFour(i, j, 0);
                    if (emitter != NULL) {
                        emitter->setRate(2.0f);
                    }
                }
                break;
            case daAlink_c::EFFPROC_SLIP:
                if (type == 0) {
                    emitter = mFootPoly.getEmitterFour(i, j, 0);
                    if (emitter != NULL) {
                        emitter->setAwayFromCenterSpeed(0.0f);
                        emitter->setRandomDirectionSpeed(0.0f);
                        emitter->setSpread(0.1f);
                        emitter->setGlobalParticleScale(slipSmokeScale);
                    }
                } else if (type == 1) {
                    emitter = mFootPoly.getEmitterFour(i, j, 0);
                    if (emitter != NULL) {
                        emitter->setRate(0.5f);
                        emitter->setDirectionalSpeed(12.0f);
                    }
                } else if (type == 16) {
                    for (int k = 0; k < 4; k++) {
                        emitter = mFootPoly.getEmitterFour(i, j, k);
                        if (emitter == NULL) {
                            break;
                        }
                        emitter->getGlobalTranslation(&trans);
                        trans.x += cM_ssin(current.angle.y) * 35.0f;
                        trans.z += cM_scos(current.angle.y) * 35.0f;
                        emitter->setGlobalTranslation(trans);
                    }
                } else if (type == 3) {
                    emitter = mFootPoly.getEmitterFour(i, j, 0);
                    if (emitter != NULL) {
                        emitter->setRate(1.0f);
                    }
                } else if (type == 4) {
                    for (int k = 0; k < 4; k++) {
                        emitter = mFootPoly.getEmitterFour(i, j, k);
                        if (emitter != NULL) {
                            emitter->setRate(2.0f);
                        }
                    }
                }
                break;
            case daAlink_c::EFFPROC_SMALL_LAND:
                if (type == 0) {
                    emitter = mFootPoly.getEmitterFour(i, j, 0);
                    if (emitter != NULL) {
                        emitter->setRate(15.0f);
                        emitter->setVolumeSize(25);
                        emitter->setDirectionalSpeed(0.0f);
                        emitter->setGlobalParticleScale(smallLandSmokeScale);
                    }
                } else if (type == 1) {
                    emitter = mFootPoly.getEmitterFour(i, j, 0);
                    if (emitter != NULL) {
                        emitter->setRate(8.0f);
                        emitter->setVolumeSize(25);
                        emitter->setDirectionalSpeed(0.0f);
                        emitter->setAwayFromAxisSpeed(8.0f);
                    }
                }
                break;
            case daAlink_c::EFFPROC_RUN:
                if (type == 0) {
                    emitter = mFootPoly.getEmitterFour(i, j, 0);
                    if (emitter != NULL) {
                        emitter->setRate(3.0f);
                    }
                } else if (type == 1) {
                    emitter = mFootPoly.getEmitterFour(i, j, 0);
                    if (emitter != NULL) {
                        emitter->setRate(4.0f);
                    }
                } else if (type == 2) {
                    for (int k = 0; k < 4; k++) {
                        emitter = mFootPoly.getEmitterFour(i, j, k);
                        if (emitter == NULL) {
                            break;
                        }
                        emitter->setGlobalScale(runWaterScale);
                    }
                } else if (type == 5) {
                    emitter = mFootPoly.getEmitterFour(i, j, 0);
                    if (emitter != NULL) {
                        emitter->setRate(10.0f);
                        emitter->setAwayFromAxisSpeed(1.0f);
                        emitter->setGlobalScale(runWaterSmokeScale);
                    }
                } else if (type == 16) {
                    for (int k = 0; k < 4; k++) {
                        emitter = mFootPoly.getEmitterFour(i, j, k);
                        if (emitter == NULL) {
                            break;
                        }
                        emitter->setGlobalScale(runWaterCenterScale);
                    }
                } else if (type == 3) {
                    emitter = mFootPoly.getEmitterFour(i, j, 0);
                    if (emitter != NULL) {
                        emitter->setRate(5.0f);
                    }
                }
                break;
            default:
                break;
            }
        }
    }
}

/// daAlink_c::setFootMark (d_a_alink_effect.inc:326-330) on the puppet's foot joint.
void daRemotePlayer_c::setFootFxMark(cXyz* i_pos, u16 i_jointNo, int i_kind) {
    cXyz axis;
    mDoMtx_multVecSR(model->getAnmMtx(i_jointNo), &cXyz::BaseX, &axis);
    dkWmark_c::setFootMark(i_pos, axis.atan2sX_Z(), i_kind);
}

/**
 * Once a sim tick, from execute() after setMatrix(): the foot joints are this tick's then, as his
 * are when setEffect runs after modelCalc and setBodyPartPos (d_a_alink.cpp:18632-18645). See the
 * top of the file for the whole story.
 */
void daRemotePlayer_c::updateFootFx() {
    if (!mFootFxReady) {
        setupFootFx();
    }

    /* The same guard as the sword fx: particle colour is computed from this actor's tevStr against
     * the scene, and during a stage change there is neither a scene nor, briefly, a particle
     * manager or a local Link (engine statics assume one). Nothing is set; the manager reaps what
     * is out, and the next real tick starts clean. */
    if (g_dComIfG_gameInfo.play.getParticle() == NULL || daAlink_getAlinkActorClass() == NULL ||
        daPy_getPlayerActorClass() == NULL || model == NULL ||
        model->getModelData()->getJointNum() <= l_footFxRightJointNo)
    {
        mFootFxPrevPosValid = false;
        return;
    }

    const u32 tick = g_Counter.mCounter0;
    footFxBgCheck();

    // setWaterY (d_a_alink.cpp:8933-8946): his mWaterY.
    const bool waterHit = mFootAcch.ChkWaterHit();
    const f32 waterY = waterHit ? mFootAcch.m_wtr.GetHeight() : -G_CM3D_F_INF;
    // execute's field_0x2fc4 countdown (d_a_alink.cpp:18068-18070), before setEffect.
    if (mFootFxWetTimer != 0) {
        mFootFxWetTimer--;
    }

    /* ---- Which proc he is in, and what it asked for this tick ------------------------------- */
    const bool inAct = mActActive;
    const u16 under = inAct ? mActShownUnder : l_actNone;
    const bool actEntry = inAct && under != mFootFxPrevActUnder;
    const bool slip = !inAct && mNetSharpTurn && mCurrentAnm == static_cast<u16>(l_slipAnm);
    const bool locomotion =
        !inAct && !slip && mDoubleAnmSet && mNetAtnKind == dusk::mp::kPlayerAtnNone;
    /* checkDashAnime (d_a_alink.cpp:5470-5472): ANM_RUN in under slot 0. The puppet's band 3 is the
     * collapsed run pair (setDoubleAnm, onePair), which is exactly when his slot 0 holds the run.
     */
    const bool dash = locomotion && mCurrentAnm == static_cast<u16>(l_runAnm) &&
                      mAnmPackUnder[1].getAnmTransform() == NULL;

    u8 effProc = daAlink_c::EFFPROC_NONE;
    u8 points = 0;
    bool markLeft = false;
    bool markRight = false;
    bool slide = false;
    int logKind = -1;

    if (locomotion) {
        /* setBlendMoveAnime (d_a_alink.cpp:7571-7851). PROC_MOVE sets EFFPROC_RUN every tick
         * (:15680); PROC_WAIT leaves commonProcInit's NONE. The foot-down test is on framectrl1,
         * which the puppet's collapsed band-3 pair keeps in slot 0. */
        effProc = mNetZeroSpeed ? daAlink_c::EFFPROC_NONE : daAlink_c::EFFPROC_RUN;
        const daPy_frameCtrl_c& ctrl =
            mAnmPackUnder[1].getAnmTransform() != NULL ? mUnderFrameCtrl[1] : mUnderFrameCtrl[0];

        f32 leftFrame = l_footFxRunLeftFrame;
        f32 rightFrame = l_footFxRunRightFrame;
        if (mGaitPrevHeavy) {
            leftFrame = l_footFxHeavyLeftFrame;
            rightFrame = l_footFxHeavyRightFrame;
        } else if (mGaitPrevIronBall) {
            leftFrame = l_footFxBallLeftFrame;
            rightFrame = l_footFxBallRightFrame;
        }
        if (mStepTurnPrev != 0) {
            // The pivot (:7731-7737): turning left (sp8 > 0) is the even mStepTurnAnm index.
            const bool turnLeft = ((mStepTurnPrev - 1) & 1) == 0;
            leftFrame = turnLeft ? l_footFxStepNear : l_footFxStepFar;
            rightFrame = turnLeft ? l_footFxStepFar : l_footFxStepNear;
        }

        /* The footprints (RFLG1_UNK_10/20) need field_0x2f8c != 2, the slot that owns the notes
         * (:7830-7844): 2 only while standing without a pivot, which is selectAnimation's own
         * basOnA test. In bands 2 and 3 the sender is never standing. */
        if (!mNetZeroSpeed || mStepTurnPrev != 0) {
            if (footfx_passed(ctrl, leftFrame)) {
                markLeft = true;
            } else if (footfx_passed(ctrl, rightFrame)) {
                markRight = true;
            }
        }
        // The dust (field_0x2f9d), only at a full run (:7813-7817), on the same two frames.
        if (dash) {
            if (markLeft) {
                points = l_footFxPtLeftFoot;
            } else if (markRight) {
                points = l_footFxPtRightFoot;
            }
            if (points != 0) {
                logKind = FOOTFX_LOG_RUN_STEP;
            }
        }
    } else if (slip) {
        /* procSlipInit (:16689-16698) sets the right foot once; procSlip (:16701-16727) every tick
         * while mNormalSpeed > 2.5. His speed is not on the wire, but his rate is
         * |mNormalSpeed cos(ground)| / mMaxSpeed (getMoveGroundAngleSpeedRate, :7559-7569), so on
         * level ground at the human mMaxSpeed it inverts exactly. */
        effProc = daAlink_c::EFFPROC_SLIP;
        if (!mFootFxPrevSlip || (!mNetZeroSpeed && mNetMoveRate * daAlinkHIO_move_c0::m.mMaxSpeed >
                                                       l_footFxSlipMinSpeed))
        {
            points = l_footFxPtRightFoot;
            logKind = FOOTFX_LOG_SKID;
        }
    } else if (inAct) {
        /* The override's own frame, as his proc reads it — before this tick's step. */
        const f32 frame = mUnderFrameCtrl[0].getFrame() - mUnderFrameCtrl[0].getRate();
        if (footfx_is_anm(under, daAlink_c::ANM_JUMP_LAND)) {
            // procLandInit (:17219-17239): the centre and both footprints, once.
            effProc = daAlink_c::EFFPROC_LAND;
            if (actEntry) {
                points = l_footFxPtCenter;
                markLeft = true;
                markRight = true;
                logKind = FOOTFX_LOG_LAND;
            }
        } else if (footfx_is_anm(under, daAlink_c::ANM_BACKFLIP_LAND)) {
            // procBackJumpLandInit (:16643-16652).
            effProc = daAlink_c::EFFPROC_SMALL_LAND;
            if (actEntry) {
                points = l_footFxPtCenter;
                logKind = FOOTFX_LOG_LAND;
            }
        } else if (footfx_is_anm(under, daAlink_c::ANM_BACK_JUMP_LAND) ||
                   footfx_is_anm(under, daAlink_c::ANM_SIDE_JUMP_LEFT_LAND) ||
                   footfx_is_anm(under, daAlink_c::ANM_SIDE_JUMP_RIGHT_LAND))
        {
            // procSideStepLandInit (:15935-15973), with its RFLG1_UNK_30.
            effProc = daAlink_c::EFFPROC_SMALL_LAND;
            if (actEntry) {
                points = l_footFxPtCenter;
                markLeft = true;
                markRight = true;
                logKind = FOOTFX_LOG_LAND;
            }
        } else if (footfx_is_anm(under, daAlink_c::ANM_FRONT_ROLL)) {
            // procFrontRoll (:16229-16321): frames 6..16, unless past the cancel frame.
            effProc = daAlink_c::EFFPROC_FRONT_ROLL;
            if (frame > l_footFxFrontRollFrom && frame <= l_footFxFrontRollTo &&
                frame <= daAlinkHIO_frontRoll_c0::m.mRollAnm.mCancelFrame)
            {
                points = l_footFxPtCenter;
                logKind = FOOTFX_LOG_ROLL;
            }
        } else if (footfx_is_anm(under, daAlink_c::ANM_SIDE_ROLL_LEFT) ||
                   footfx_is_anm(under, daAlink_c::ANM_SIDE_ROLL_RIGHT))
        {
            // procSideRollInit (:16476-16477) once, then procSideRoll (:16541-16546) below 18.
            effProc = daAlink_c::EFFPROC_FRONT_ROLL;
            if (actEntry || (frame < l_footFxSideRollTo &&
                                frame <= daAlinkHIO_turnMove_c0::m.mTurnAnm.mCancelFrame))
            {
                points = l_footFxPtCenter;
                logKind = FOOTFX_LOG_ROLL;
            }
        } else if (footfx_is_anm(under, daAlink_c::ANM_SLIDE_FORWARD)) {
            // procSlideInit / procSlide's forward slide (:16020-16031, :16066-16069).
            effProc = daAlink_c::EFFPROC_FRONT_ROLL;
            slide = true;
            points = l_footFxPtCenter;
            logKind = FOOTFX_LOG_SLIDE;
        } else if (footfx_is_anm(under, daAlink_c::ANM_SLIDE_BACKWARD)) {
            // ...and its backward slide (:16032-16037, :16070-16072): both feet.
            effProc = daAlink_c::EFFPROC_SLIP;
            slide = true;
            points = l_footFxPtLeftFoot | l_footFxPtRightFoot;
            logKind = FOOTFX_LOG_SLIDE;
        }
    }

    // commonProcInit leaves NONE without clearing (d_a_alink.cpp:15216); a kind is set as his
    // procs set it.
    if (effProc == daAlink_c::EFFPROC_NONE) {
        mFootFxProc = daAlink_c::EFFPROC_NONE;
    } else {
        setFootFxProcType(effProc);
    }

    /* ---- setEffect's flag words (d_a_alink_effect.inc:509-550, human branches) --------------- */
    u32 ringFlags = 0;
    u32 skipFlags = 0;
    switch (mFootFxProc) {
    case daAlink_c::EFFPROC_LAND:
        ringFlags |= 0x3B;
        break;
    case daAlink_c::EFFPROC_FRONT_ROLL:
        if (slide) {
            skipFlags |= 4;
        }
        ringFlags |= 4;
        skipFlags |= 0x10000;
        break;
    case daAlink_c::EFFPROC_SLIP:
        skipFlags |= 0x10000;
        break;
    case daAlink_c::EFFPROC_SMALL_LAND:
        ringFlags |= 0x38;
        break;
    default:
        break;
    }

    cXyz center = current.pos;
    cXyz waterPos = current.pos;
    cXyz leftFoot;
    cXyz rightFoot;
    mDoMtx_multVecZero(model->getAnmMtx(l_footFxLeftJointNo), &leftFoot);
    mDoMtx_multVecZero(model->getAnmMtx(l_footFxRightJointNo), &rightFoot);

    /* His gate on the points (:565-567): on the ground and in none of the airborne / hanging /
     * swimming modes. Every proc that sets a point above is a ground proc, so what is left to ask
     * is whether there is a floor under the puppet at all. */
    const bool onGround = mFootAcch.ChkGroundHit();
    const f32 moveDist = mFootFxPrevPosValid ? mFootFxPrevPos.absXZ(current.pos) : 0.0f;

    // For the logs: checkWaterIn's own tests (d_particle_copoly.cpp:22-45), read before the call.
    const bool waterUsable = waterHit && dComIfG_Bgsp().ChkPolySafe(mFootAcch.m_wtr) &&
                             dComIfG_Bgsp().GetPolyAtt0(mFootAcch.m_wtr) != 6;
    const f32 depth = waterUsable ? waterY - current.pos.y : -1.0f;
    const bool wasIn = mFootPoly.field_0x37 != 0;

    mFootPoly.setEffectFour(&tevStr, &waterPos, ringFlags, skipFlags,
        onGround && (points & l_footFxPtCenter) != 0 ? &center : NULL, NULL, NULL,
        onGround && (points & l_footFxPtLeftFoot) != 0 ? &leftFoot : NULL,
        onGround && (points & l_footFxPtRightFoot) != 0 ? &rightFoot : NULL, &current.angle, NULL,
        fopAcM_GetRoomNo(this), l_footFxPillarScale, moveDist);

    if (mFootFxProc != daAlink_c::EFFPROC_NONE) {
        setFootFxParam();
    }

    /* ---- logs: one line per kind, capped ---------------------------------------------------- */
    if (logKind >= 0 && onGround && mFootFxLogCount[logKind] < l_footFxLogMax) {
        mFootFxLogCount[logKind]++;
        static const char* const kindName[] = {"run step", "landing", "roll", "slide", "skid"};
        Log.info("Puppet {} foot fx: {} at tick {} — points 0x{:02x}, effproc {}, centre {}/{}, "
                 "left foot {}/{}, right foot {}/{}, floor att0 {} at ({:.0f}, {:.0f}, {:.0f})",
            mPlayerId, kindName[logKind], tick, static_cast<unsigned>(points),
            static_cast<unsigned>(mFootFxProc), footfx_type_name(mFootPoly.getTypeFour(0, 0)),
            footfx_type_name(mFootPoly.getTypeFour(0, 1)),
            footfx_type_name(mFootPoly.getTypeFour(3, 0)),
            footfx_type_name(mFootPoly.getTypeFour(3, 1)),
            footfx_type_name(mFootPoly.getTypeFour(4, 0)),
            footfx_type_name(mFootPoly.getTypeFour(4, 1)),
            dComIfG_Bgsp().ChkPolySafe(mFootAcch.m_gnd) ?
                dComIfG_Bgsp().GetPolyAtt0(mFootAcch.m_gnd) :
                -1,
            current.pos.x, current.pos.y, current.pos.z);
    }

    const bool rippling = depth >= 0.0f && depth < l_linkHeight;
    if (rippling && !mFootFxRippling && mFootFxLogCount[FOOTFX_LOG_RIPPLE] < l_footFxLogMax) {
        mFootFxLogCount[FOOTFX_LOG_RIPPLE]++;
        Log.info("Puppet {} foot fx: ripples ON at tick {} — water {:.1f} above the feet "
                 "(ripples below {:.0f}), HAMON emitter {}",
            mPlayerId, tick, depth, l_linkHeight,
            dComIfGp_particle_getEmitter(mFootPoly.field_0x0[1]) != NULL ? "live" : "NONE");
    }
    mFootFxRippling = rippling;

    if (!wasIn && depth >= 0.0f &&
        mFootAcch.m_wtr.GetHeight() - mFootAcch.GetGroundH() >= mFootPoly.mWaterOffset &&
        mFootFxLogCount[FOOTFX_LOG_PILLAR] < l_footFxLogMax)
    {
        mFootFxLogCount[FOOTFX_LOG_PILLAR]++;
        Log.info("Puppet {} foot fx: water-entry splash (wpillar) at tick {} — water {:.1f} deep "
                 "at the feet, {:.1f} over the floor",
            mPlayerId, tick, depth, mFootAcch.m_wtr.GetHeight() - mFootAcch.GetGroundH());
    }

    /* ---- wet footprints (d_a_alink_effect.inc:604-647, human, kind 0 only) ------------------- */
    if (waterY > current.pos.y) {
        mFootFxWetTimer = l_footFxWetTicks;
    }
    if ((markLeft || markRight) && current.pos.y > waterY && mFootFxWetTimer != 0) {
        if (markLeft) {
            setFootFxMark(&leftFoot, l_footFxLeftJointNo, 0);
        }
        if (markRight) {
            setFootFxMark(&rightFoot, l_footFxRightJointNo, 0);
        }
        if (mFootFxLogCount[FOOTFX_LOG_WET_MARK] < l_footFxLogMax) {
            mFootFxLogCount[FOOTFX_LOG_WET_MARK]++;
            Log.info("Puppet {} foot fx: wet footprint{} at tick {} ({} wet tick(s) left)",
                mPlayerId, markLeft && markRight ? "s" : (markLeft ? " left" : " right"), tick,
                static_cast<unsigned>(mFootFxWetTimer));
        }
    }

    /* ---- setRunSplash (d_a_alink_effect.inc:1597-1621), on the puppet's three slots ---------- */
    if (dash && waterHit && depth >= daAlinkHIO_basic_c0::m.mWaterSurfaceEffectHeight &&
        depth < l_linkHeight && dKy_pol_efftype_get(&mFootAcch.m_wtr) == 2)
    {
        cXyz pos(current.pos.x, waterY, current.pos.z);
        JPABaseEmitter* emitter = footfx_emit_poly(&mFxEmitter[PUPPET_FX_FOOT_SPLASH_A],
            ID_ZI_J_DASHWTRA_A, mFootAcch.m_wtr, &pos, &tevStr, &current.angle);
        if (emitter != NULL) {
            emitter->setLifeTime(10);
            emitter->setDirectionalSpeed(5.0f);
        }

        pos.set(current.pos.x + (70.0f * cM_ssin(shape_angle.y)), 20.0f + waterY,
            current.pos.z + (70.0f * cM_scos(shape_angle.y)));
        footfx_emit_poly(&mFxEmitter[PUPPET_FX_FOOT_SPLASH_B], ID_ZI_J_DASHWTRA_B, mFootAcch.m_wtr,
            &pos, &tevStr, &current.angle);
        pos.y = waterY;

        emitter = footfx_emit_poly(&mFxEmitter[PUPPET_FX_FOOT_SPLASH_C], ID_ZI_J_DASHWTRA_C,
            mFootAcch.m_wtr, &pos, &tevStr, &current.angle);
        if (emitter != NULL) {
            emitter->setLifeTime(20);
            emitter->setDirectionalSpeed(4.5f);
        }

        if (mFootFxLogCount[FOOTFX_LOG_RUN_SPLASH] < l_footFxLogMax) {
            mFootFxLogCount[FOOTFX_LOG_RUN_SPLASH]++;
            Log.info("Puppet {} foot fx: run splash (DASHWTRA A/B/C) at tick {} — water {:.1f} "
                     "deep, emitters {}/{}/{}",
                mPlayerId, tick, depth,
                getPuppetEmitter(PUPPET_FX_FOOT_SPLASH_A) != NULL ? "live" : "NONE",
                getPuppetEmitter(PUPPET_FX_FOOT_SPLASH_B) != NULL ? "live" : "NONE",
                getPuppetEmitter(PUPPET_FX_FOOT_SPLASH_C) != NULL ? "live" : "NONE");
        }
    }

    // His field_0x3798 = old.pos (d_a_alink.cpp:17925), and this file's own edge memory.
    mFootFxPrevPos = current.pos;
    mFootFxPrevPosValid = true;
    mFootFxPrevActUnder = under;
    mFootFxPrevSlip = slip;
}
