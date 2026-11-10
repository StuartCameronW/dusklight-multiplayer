/**
 * d_a_remote_player_swordfx.inc
 * Remote player puppet: the sword trail, the spin attack's particles and the sword charge glow
 * (protocol v20 fxFlags bits 1-3 and 6-7; the bits' meaning is under kPlayerFxKnownMask in
 * src/dusk/multiplayer/replication/player_state.hpp).
 *
 * Dusk-only, like the rest of the puppet. Included once, at the end of d_a_remote_player.cpp.
 *
 * How daAlink_c does each one, and so how this does:
 *
 *   - TRAIL. m_swordBlur is a daAlink_blur_c, a J3DPacket whose draw() is the whole effect: a strip
 *     of quads between two 60-point arrays (blade tip, blade root), entered into the Z-sorted XLU
 *     list from his draw (d_a_alink.cpp:19856-19858). setAtCollision extends it on every attack
 *     tick with copyBlur(tip, root, sweep direction) and otherwise shrinks it with decSwordBlur,
 *     which also carries the remaining points along with his movement (traceBlur,
 *     d_a_alink.cpp:6267-6274). The puppet owns ANOTHER daAlink_blur_c and drives it with the same
 *     three calls, fed from its own sword model's base matrix as setSwordPos computes his
 *     (d_a_alink.cpp:5866-5886) — so the trail leaves the blade the puppet draws, not one
 *     reconstructed from the hitbox points. The wire only says WHEN to extend (bit 1) and the two
 *     things the blade cannot tell us: the alpha (dash or not) and the sweep sign.
 *   - SPIN. setCutTurnEffect (d_a_alink_effect.inc:944-1096) re-sets two to six level emitters at
 *     joint 1 every tick of the spin proc; clearCutTurnEffectID hides them on the next proc change
 *     (d_a_alink.cpp:15218). Same here, with the list off the wire and the turn direction off the
 *     action override's own BCK (ANM_CUT_TURN_LEFT is the one that takes the local rotations).
 *   - CHARGE. setSwordChargeEffect (d_a_alink_effect.inc:806-873) reads m_nSwordBtk's frame — the
 *     ordon sword's BTK, which he advances once a tick only while charging and zeroes on every proc
 *     change — and puts KIRARI_A on the sword's matrix, KIRARI_B moving up the blade with the
 *     frame, and KIRARI_C at the point once the frame reaches 14. The puppet plays ITS copy of that
 *     BTK (mpOrdonSwordBtk, entered on its private ordon sword data by setupEquipModels) from the
 *     tick the wire's charge bit rises, so the blade's own shine and the particle frame are the
 *     same number, as they are for him. Not played anywhere else; see playEquipAnm().
 *
 * Every emitter is a level emitter: the particle manager kills one the tick after it stops being
 * re-set (dPa_control_c::level_c::execute, d_particle.cpp:971-984), so an effect that ends simply
 * stops being set, and the hide calls below only make the end as abrupt as his. Nothing here
 * hands the particle system a pointer it keeps (setGlobalRTMatrix copies), so a puppet deleted
 * mid-effect leaves at most one tick of orphaned emitters, which the manager reaps itself.
 *
 * NOT reproduced, deliberately or for now:
 *   - Sounds: Z2SE_SWORD_POWER_COME while the charge builds (:848-853) — the puppet's sound is its
 *     own feature (future-plans/puppet-sound.md).
 *   - The magnetised spin: with the Iron Boots on a magnetic surface he rotates the spin emitters
 *     onto mMagneBootMtx (:1066-1078). The puppet has no magnetised frame to give them.
 *   - The Zora-armour LARGE spin underwater: sent as the water list, without its scale/volume
 *     tweak (:1086-1093) — two wire bits buy four of the five lists.
 *   - The horse spin (setHorseCutTurnEffect) and the large jump landing: the puppet does not ride
 *     and is not sent those procs.
 */

#include "JSystem/JParticle/JPAEmitter.h"

namespace {

/// d_a_alink.cpp:207 and :209, which are file-static there: the blade tip in the sword's own frame,
/// ordon/wooden and master.
const Vec l_swordFxTopLocalN = {100.0f, 0.0f, 0.0f};
const Vec l_swordFxTopLocalM = {120.0f, 0.0f, 0.0f};
/// setSwordPos's sweep directions (d_a_alink.cpp:5867-5868): Local1 for the twirl and the right
/// finisher, Local0 for every other cut.
const Vec l_swordFxMoveLocal0 = {0.0f, 0.0f, -1.0f};
const Vec l_swordFxMoveLocal1 = {0.0f, 0.0f, 1.0f};
/// setSwordChargeEffect's statics (d_a_alink_effect.inc:807-808).
const Vec l_swordFxLightAScale = {1.0f, 1.171f, 1.0f};
const Vec l_swordFxLightATrans = {68.0f, 0.0f, 0.0f};
/// Where setSwordChargeEffect clamps the BTK frame, and the frame its full glow starts on
/// (:838-846).
const f32 l_swordFxChargeFull = 14.0f;
const u16 l_swordFxLogMax = 64;

/* setCutTurnEffect's tables, verbatim (d_a_alink_effect.inc:953-1022). Function statics there, so
 * copied. The rotations are NOT const for the same reason his are not: the emitter API takes them
 * through a JGeometry::TVec3<s16> cast, exactly as his loop does. */
const u16 l_swordSpinNameNormal[] = {
    ID_ZI_J_KAITENGIRI_A,
    ID_ZI_J_KAITENGIRI_B,
};
// clang-format off
s16 l_swordSpinRotNormal[] = {
    cM_deg2s(180), cM_deg2s(45), cM_deg2s(13), cM_deg2s(180), cM_deg2s(45), cM_deg2s(13),
};
// clang-format on
const Vec l_swordSpinTransNormal[] = {
    {0.0f, 0.0f, 0.0f},
    {0.0f, 30.0f, 0.0f},
};

const u16 l_swordSpinNameLight[] = {
    ID_ZI_J_KAITENGIRIL_A,
    ID_ZI_J_KAITENGIRIL_B,
    ID_ZI_J_KAITENGIRIL_C,
    ID_ZI_J_KAITENGIRIL_D,
};
// clang-format off
s16 l_swordSpinRotLight[] = {
    cM_deg2s(180), cM_deg2s(100), cM_deg2s(13), cM_deg2s(180), cM_deg2s(80), cM_deg2s(13),
    cM_deg2s(180), cM_deg2s(80),  cM_deg2s(13), cM_deg2s(180), cM_deg2s(80), cM_deg2s(13),
};
// clang-format on
const Vec l_swordSpinTransLight[] = {
    {0.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f},
    {0.0f, 35.0f, 0.0f},
    {0.0f, 0.0f, 0.0f},
};

const u16 l_swordSpinNameLarge[] = {
    ID_ZI_J_KAITENGIRID_A,
    ID_ZI_J_KAITENGIRID_B,
    ID_ZI_J_KAITENGIRID_C,
    ID_ZI_J_KAITENGIRID_D,
    ID_ZI_J_KAITENGIRID_E,
    ID_ZI_J_KAITENGIRID_F,
};
// clang-format off
s16 l_swordSpinRotLarge[] = {
    cM_deg2s(180), cM_deg2s(45), cM_deg2s(13), cM_deg2s(180), cM_deg2s(45), cM_deg2s(13),
    cM_deg2s(180), cM_deg2s(60), cM_deg2s(13), cM_deg2s(180), cM_deg2s(60), cM_deg2s(13),
    cM_deg2s(180), cM_deg2s(60), cM_deg2s(13), cM_deg2s(180), cM_deg2s(60), cM_deg2s(13),
};
// clang-format on
const Vec l_swordSpinTransLarge[] = {
    {0.0f, 0.0f, 0.0f},
    {0.0f, 35.0f, 0.0f},
    {0.0f, 0.0f, 0.0f},
    {0.0f, 45.0f, 0.0f},
    {0.0f, 30.0f, 0.0f},
    {0.0f, 50.0f, 0.0f},
};

const u16 l_swordSpinNameWater[] = {
    ID_ZI_J_KAITENGIRI_INWTR_A,
    ID_ZI_J_KAITENGIRI_INWTR_B,
};
// clang-format off
s16 l_swordSpinRotWater[] = {
    cM_deg2s(-90), cM_deg2s(0), cM_deg2s(180), cM_deg2s(0), cM_deg2s(0), cM_deg2s(180),
};
// clang-format on
const Vec l_swordSpinTransWater[] = {
    {0.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f},
};

/// One of the four lists, indexed by daRemotePlayer_c::SwordSpinKind_e.
struct SwordSpinList {
    const u16* mNames;
    s16* mRot;
    const Vec* mTrans;
    int mNum;
    const char* mWhat;
};
const SwordSpinList l_swordSpinLists[4] = {
    {l_swordSpinNameNormal, l_swordSpinRotNormal, l_swordSpinTransNormal, 2, "normal"},
    {l_swordSpinNameLight, l_swordSpinRotLight, l_swordSpinTransLight, 4, "light"},
    {l_swordSpinNameLarge, l_swordSpinRotLarge, l_swordSpinTransLarge, 6, "large"},
    {l_swordSpinNameWater, l_swordSpinRotWater, l_swordSpinTransWater, 2, "water"},
};

/* ---- Particle emitter interface (agent F) -------------------------------------------------------
 * The puppet's first particles. Kept to these three helpers so they can be folded into agent D's
 * shared emitter helper (d_a_remote_player_fx.inc) without touching the effect code below.
 *
 * swordfx_emit is daAlink_c::setEmitter (d_a_alink_effect.inc:25-31) with the tevStr passed in:
 * re-setting an id that is still live moves the SAME emitter (dPa_control_c::set, d_particle.cpp
 * :1733-1771) and keeps it alive another tick; a dead or zero id gets a new one. */
bool swordfx_particles_ready() {
    return g_dComIfG_gameInfo.play.getParticle() != NULL;
}

JPABaseEmitter* swordfx_emit(
    u32* io_id, u16 i_name, const cXyz* i_pos, const dKy_tevstr_c* i_tevStr, const csXyz* i_rot) {
    *io_id = dComIfGp_particle_set(
        *io_id, i_name, i_pos, i_tevStr, i_rot, NULL, 0xFF, NULL, 0xFF, NULL, NULL, NULL);
    dComIfGp_particle_levelEmitterOnEventMove(*io_id);
    return dComIfGp_particle_getEmitter(*io_id);
}

/// daAlink_c::stopDrawParticle (d_a_alink_effect.inc:51-57): hide what is already out. The id is
/// left alone; the emitter dies by itself once nobody re-sets it.
void swordfx_stop(u32 i_id) {
    JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(i_id);
    if (emitter != NULL) {
        emitter->stopDrawParticle();
    }
}
/* ---- end of the emitter interface ------------------------------------------------------------ */

}  // namespace

/**
 * Built at the end of createHeap, after everything else, so a short heap costs the trail and
 * nothing more. The blur texture comes out of the puppet's own "Alink" mount, the same resource
 * daAlink_c gives his (d_a_alink.cpp:4581), and lives exactly as long as this actor does.
 *
 * Cost: one daAlink_blur_c, 0x5D8 bytes of points plus the PC texture object (logged).
 */
void daRemotePlayer_c::setupSwordFx() {
    mpSwordBlurPacket = NULL;

    ResTIMG* tex = static_cast<ResTIMG*>(own_archive_idx(mAlinkRes, dRes_INDEX_ALINK_BTI_BLUR_e));
    if (tex == NULL) {
        Log.warn(
            "Puppet {} has no sword-trail texture (al blur.bti); it will swing without a trail",
            mPlayerId);
        return;
    }

    daAlink_blur_c* blur = JKR_NEW daAlink_blur_c();
    if (blur == NULL) {
        Log.warn("Puppet {} could not fit its sword trail ({} bytes) on its heap; it will swing "
                 "without one",
            mPlayerId, sizeof(daAlink_blur_c));
        return;
    }

    blur->m_blurTex = tex;
    blur->field_0x14 = 0;
    blur->field_0x1c = 0;
    blur->field_0x20 = daAlinkHIO_cut_c0::m.mBlurAlpha;
    blur->field_0x24 = 0.0f;
    blur->field_0x2c = cXyz::Zero;
    mpSwordBlurPacket = blur;
    Log.debug(
        "Puppet {} sword fx: trail packet ready ({} bytes)", mPlayerId, sizeof(daAlink_blur_c));
}

void daRemotePlayer_c::setNetworkSwordFx(bool i_trail, bool i_trailDash, bool i_trailReversed,
    bool i_spin, u8 i_spinKind, bool i_charge) {
    mNetSwordTrail = i_trail;
    mNetSwordTrailDash = i_trailDash;
    mNetSwordTrailReversed = i_trailReversed;
    mNetSwordSpin = i_spin;
    mNetSwordSpinKind = i_spinKind < 4 ? i_spinKind : static_cast<u8>(SWORD_SPIN_NORMAL_e);
    mNetSwordCharge = i_charge;
}

/**
 * Once a sim tick, from execute() after setMatrix(): the sword's base matrix and joint 1 are this
 * tick's then, as his are when setCollision / setEffect run after his own setItemMatrix
 * (d_a_alink.cpp:18593-18645).
 */
void daRemotePlayer_c::updateSwordFx() {
    J3DModel* sheath = NULL;
    J3DModel* sword = currentSword(&sheath);
    if (sword != NULL && (mNetEquip & dusk::mp::kPlayerEquipSwordInHand) == 0) {
        // Sheathed. Every effect here belongs to a blade in his hand.
        sword = NULL;
    }
    const u8 kind = static_cast<u8>(
        (mNetEquip & dusk::mp::kPlayerEquipSwordKindMask) >> dusk::mp::kPlayerEquipSwordKindShift);
    const bool master = kind == dusk::mp::kPlayerEquipSwordMaster;

    /* Same guard as draw(): particle colour is computed from this actor's tevStr against the scene,
     * and during a stage change there is neither a scene to light against nor, briefly, a particle
     * manager. An effect that cannot be set this tick is simply not set; the manager reaps it. */
    const bool particlesOk = swordfx_particles_ready() && daAlink_getAlinkActorClass() != NULL &&
                             daPy_getPlayerActorClass() != NULL;

    updateSwordTrail(sword, master);
    updateSwordSpinFx(particlesOk);
    updateSwordChargeFx(sword, master, particlesOk);

    if (sword != NULL) {
        cMtx_copy(sword->getBaseTRMtx(), mSwordFxPrevMtx);
        mSwordFxPrevMtxValid = true;
    } else {
        mSwordFxPrevMtxValid = false;
    }
    mSwordFxPrevPos = current.pos;
    mSwordFxPrevAngleY = shape_angle.y;
    mSwordFxPrevPosValid = true;
}

/**
 * setAtCollision's blur half (d_a_alink.cpp:6560-6636), with the sender's decision taken off the
 * wire and his three blur calls made here on the puppet's blade.
 *
 * The one reconstruction: the sender's initBlur happens on the tick BEFORE his first copyBlur (the
 * attack's first tick, before FLG0_CUT_AT_FLG is on, :6604-6614), and leaves the point count at 0,
 * so it never shows on the wire — the bit only rises with that first copy. So on a rising edge
 * this does both: initBlur from LAST tick's blade, which is where his was, then this tick's copy.
 */
void daRemotePlayer_c::updateSwordTrail(J3DModel* i_sword, bool i_master) {
    daAlink_blur_c* blur = static_cast<daAlink_blur_c*>(mpSwordBlurPacket);
    if (blur == NULL) {
        return;
    }

    const u32 tick = g_Counter.mCounter0;
    const Vec* topLocal = i_master ? &l_swordFxTopLocalM : &l_swordFxTopLocalN;
    const Vec* moveLocal = mNetSwordTrailReversed ? &l_swordFxMoveLocal1 : &l_swordFxMoveLocal0;

    if (mNetSwordTrail && i_sword != NULL) {
        // setSwordPos, on our blade (d_a_alink.cpp:5873-5885).
        cXyz root;
        cXyz dir;
        cXyz tip;
        mDoMtx_multVecZero(i_sword->getBaseTRMtx(), &root);
        mDoMtx_multVecSR(i_sword->getBaseTRMtx(), moveLocal, &dir);
        mDoMtx_multVec(i_sword->getBaseTRMtx(), topLocal, &tip);

        blur->field_0x20 = mNetSwordTrailDash ? daAlinkHIO_cut_c0::m.mDashBlurAlpha :
                                                daAlinkHIO_cut_c0::m.mBlurAlpha;

        bool copy = true;
        if (!mSwordTrailOn) {
            if (mSwordFxPrevMtxValid) {
                cXyz prevRoot;
                cXyz prevDir;
                cXyz prevTip;
                mDoMtx_multVecZero(mSwordFxPrevMtx, &prevRoot);
                mDoMtx_multVecSR(mSwordFxPrevMtx, moveLocal, &prevDir);
                mDoMtx_multVec(mSwordFxPrevMtx, topLocal, &prevTip);
                blur->initBlur(0.0f, 0, &prevTip, &prevRoot, &prevDir);
            } else {
                // No blade last tick to start from: start here, and extend from the next tick.
                blur->initBlur(0.0f, 0, &tip, &root, &dir);
                copy = false;
            }
            mSwordTrailOn = true;
            mSwordTrailOnTick = tick;
            mLoggedSwordTrailDraw = false;
            if (mSwordFxLogCount < l_swordFxLogMax) {
                mSwordFxLogCount++;
                Log.debug(
                    "Puppet {} sword fx: trail ON at tick {} (dash {}, reversed {}, master {}, "
                    "init from {})",
                    mPlayerId, tick, mNetSwordTrailDash, mNetSwordTrailReversed, i_master,
                    copy ? "last tick's blade" : "this tick's blade");
            }
        }
        if (copy) {
            blur->copyBlur(&tip, &root, &dir);
        }
        return;
    }

    if (mSwordTrailOn) {
        mSwordTrailOn = false;
        if (mSwordFxLogCount < l_swordFxLogMax) {
            mSwordFxLogCount++;
            Log.debug("Puppet {} sword fx: trail OFF at tick {} after {} tick(s); {} points decay",
                mPlayerId, tick, tick - mSwordTrailOnTick, blur->field_0x14);
        }
    }

    // decSwordBlur (d_a_alink.cpp:6267-6274): his old.pos and mPrevAngleY are last tick's.
    if (blur->field_0x14 < 10) {
        blur->field_0x14 = 0;
    } else {
        blur->field_0x14 -= 10;
        if (mSwordFxPrevPosValid) {
            blur->traceBlur(&current.pos, &mSwordFxPrevPos,
                static_cast<s16>(shape_angle.y - mSwordFxPrevAngleY));
        }
    }
}

/// daAlink_c::clearCutTurnEffectID (d_a_alink_effect.inc:937-942).
void daRemotePlayer_c::clearSwordSpinFx() {
    if (swordfx_particles_ready()) {
        for (int i = 0; i < 6; i++) {
            swordfx_stop(mSwordSpinEmitter[i]);
        }
    }
    for (int i = 0; i < 6; i++) {
        mSwordSpinEmitter[i] = 0;
    }
}

/**
 * setCutTurnEffect (d_a_alink_effect.inc:944-1096) minus the parts the sender already decided:
 * his start-frame gate and his "underwater without the Zora armour" early return are folded into
 * the wire bit (he sends it only on ticks he emitted on), and the list is the wire's.
 *
 * The turn direction is his mProcVar4.field_0x3010, 0 for ANM_CUT_TURN_LEFT and 1 for the right
 * turn (procCutTurnInit, d_a_alink_cut.inc:1736-1752). The puppet reads it off the BCK the action
 * override is playing, which is the same row of the same table: the under BCK of
 * m_anmDataTable[ANM_CUT_TURN_LEFT].
 */
void daRemotePlayer_c::updateSwordSpinFx(bool i_particlesOk) {
    const u32 tick = g_Counter.mCounter0;

    if (!mNetSwordSpin || !i_particlesOk) {
        if (mSwordSpinOn) {
            clearSwordSpinFx();
            mSwordSpinOn = false;
            if (mSwordFxLogCount < l_swordFxLogMax) {
                mSwordFxLogCount++;
                Log.debug("Puppet {} sword fx: spin OFF at tick {} after {} tick(s)", mPlayerId,
                    tick, tick - mSwordSpinOnTick);
            }
        }
        return;
    }

    const SwordSpinList& list = l_swordSpinLists[mNetSwordSpinKind];
    const bool left =
        mNetActUnder == daAlink_c::m_anmDataTable[daAlink_c::ANM_CUT_TURN_LEFT].m_bckData.m_underID;

    cXyz pos;
    mDoMtx_multVecZero(model->getAnmMtx(1), &pos);

    int created = 0;
    for (int i = 0; i < list.mNum; i++) {
        JPABaseEmitter* emitter =
            swordfx_emit(&mSwordSpinEmitter[i], list.mNames[i], &pos, &tevStr, &shape_angle);
        if (emitter == NULL) {
            continue;
        }
        created++;
        if (left) {
            emitter->setLocalRotation(((JGeometry::TVec3<s16>*)list.mRot)[i]);
            if (list.mTrans[i].y > 1.0f) {
                emitter->setLocalTranslation(list.mTrans[i]);
            }
        }
    }

    if (!mSwordSpinOn) {
        mSwordSpinOn = true;
        mSwordSpinOnTick = tick;
        if (mSwordFxLogCount < l_swordFxLogMax) {
            mSwordFxLogCount++;
            Log.debug(
                "Puppet {} sword fx: spin ON at tick {} ({} list, {} turn, {}/{} emitters set)",
                mPlayerId, tick, list.mWhat, left ? "left" : "right", created, list.mNum);
        }
    }
}

/**
 * setSwordChargeEffect (d_a_alink_effect.inc:806-873), on the puppet's blade and its own copy of
 * the BTK whose frame he reads.
 *
 * ★ The ordon sword BTK used to be deliberately left at frame 0 (playEquipAnm()), because the
 * charge was not on the wire. It is now: this is its only frame controller. He advances it once a
 * tick with simpleAnmPlay whenever a charge proc runs and the sword is not wooden
 * (d_a_alink_cut.inc:1921-1923, :1936-1938) and zeroes it in commonProcInit (d_a_alink.cpp:15244);
 * the wire bit is exactly "his frame is above 0", so playing ours from 0 on the rising edge lands
 * on his frame 1 on his first charging tick. With no BTK (a failed load) a plain counter stands in,
 * so the particles still build.
 */
void daRemotePlayer_c::updateSwordChargeFx(J3DModel* i_sword, bool i_master, bool i_particlesOk) {
    const u32 tick = g_Counter.mCounter0;
    J3DAnmTextureSRTKey* btk = mpOrdonSwordBtk;

    f32 frame = 0.0f;
    if (mNetSwordCharge) {
        if (!mSwordChargeOn) {
            mSwordChargeOn = true;
            mSwordChargeFull = false;
            mSwordChargeOnTick = tick;
            if (btk != NULL) {
                btk->setFrame(0.0f);
            }
            if (mSwordFxLogCount < l_swordFxLogMax) {
                mSwordFxLogCount++;
                Log.debug(
                    "Puppet {} sword fx: charge ON at tick {} (ordon btk {}, sword {}, wood {})",
                    mPlayerId, tick, btk != NULL ? "playing" : "MISSING, counting",
                    i_sword != NULL ? (i_master ? "master" : "ordon/wood") : "not in hand",
                    checkWoodSwordEquip());
            }
        }
        if (btk != NULL) {
            daAlink_c::simpleAnmPlay(btk);
            frame = btk->getFrame();
        } else {
            frame = static_cast<f32>(tick - mSwordChargeOnTick + 1);
        }
        if (!mSwordChargeFull && frame >= l_swordFxChargeFull) {
            mSwordChargeFull = true;
            if (mSwordFxLogCount < l_swordFxLogMax) {
                mSwordFxLogCount++;
                Log.debug(
                    "Puppet {} sword fx: charge FULL at tick {} (frame {:.0f}, {} tick(s) in; "
                    "KIRARI_C from here)",
                    mPlayerId, tick, frame, tick - mSwordChargeOnTick + 1);
            }
        }
    } else {
        if (btk != NULL && btk->getFrame() != 0.0f) {
            btk->setFrame(0.0f);
        }
        if (mSwordChargeOn) {
            mSwordChargeOn = false;
            if (mSwordFxLogCount < l_swordFxLogMax) {
                mSwordFxLogCount++;
                Log.debug("Puppet {} sword fx: charge OFF at tick {} after {} tick(s) (full {})",
                    mPlayerId, tick, tick - mSwordChargeOnTick, mSwordChargeFull);
            }
        }
    }

    if (!i_particlesOk) {
        return;
    }

    if (frame > 0.0f && i_sword != NULL) {
        if (!checkWoodSwordEquip()) {
            JPABaseEmitter* emitter = swordfx_emit(
                &mSwordChargeEmitter[0], ID_ZI_J_SWA_KIRARI_A, &current.pos, &tevStr, NULL);
            if (emitter != NULL) {
                emitter->setGlobalRTMatrix(i_sword->getBaseTRMtx());
                if (i_master) {
                    emitter->setGlobalParticleScale(l_swordFxLightAScale);
                    emitter->setLocalTranslation(l_swordFxLightATrans);
                }
            }
        }

        const f32 topX = i_master ? l_swordFxTopLocalM.x : l_swordFxTopLocalN.x;
        cXyz axis;
        cXyz base;
        mDoMtx_multVecSR(i_sword->getBaseTRMtx(), &cXyz::BaseX, &axis);
        mDoMtx_multVecZero(i_sword->getBaseTRMtx(), &base);

        f32 level = frame;
        if (level >= l_swordFxChargeFull) {
            level = l_swordFxChargeFull;
        }

        cXyz point = base + ((axis * (level + 1.0f)) * (topX / 15.0f));
        swordfx_emit(&mSwordChargeEmitter[1], ID_ZI_J_SWA_KIRARI_B, &point, &tevStr, NULL);
        if (level >= l_swordFxChargeFull) {
            swordfx_emit(&mSwordChargeEmitter[2], ID_ZI_J_SWA_KIRARI_C, &point, &tevStr, NULL);
        }
        return;
    }

    // The fade-out branch (d_a_alink_effect.inc:855-872): KIRARI_A loses 17 of alpha a tick, then
    // is let go.
    JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(mSwordChargeEmitter[0]);
    if (emitter != NULL) {
        const u8 alpha = emitter->getGlobalAlpha();
        if (alpha > 17) {
            emitter = swordfx_emit(
                &mSwordChargeEmitter[0], ID_ZI_J_SWA_KIRARI_A, &current.pos, &tevStr, NULL);
            if (emitter != NULL) {
                if (i_sword != NULL) {
                    emitter->setGlobalRTMatrix(i_sword->getBaseTRMtx());
                }
                emitter->setGlobalAlpha(alpha - 17);
            }
        } else {
            emitter->setGlobalAlpha(0);
        }
    }

    swordfx_stop(mSwordChargeEmitter[1]);
    swordfx_stop(mSwordChargeEmitter[2]);
}

/**
 * From the END of draw(), so after its once-per-sim-tick guard (Hang 4): the blur is a J3DPacket
 * prepended into a Z-sort slot, and entering one twice in a pass makes it point at itself. That
 * guard already turns away a second sim-frame draw; the is_sim_frame test here keeps the packet
 * out of any presentation-only pass as well, since nothing resets the lists before one
 * (mDoGph_BeforeOfDraw runs only at the head of a sim tick's draw, f_pc_draw.cpp:42).
 *
 * daAlink_c's own entry, line for line (d_a_alink.cpp:19856-19858).
 */
void daRemotePlayer_c::drawSwordFx() {
    daAlink_blur_c* blur = static_cast<daAlink_blur_c*>(mpSwordBlurPacket);
    if (blur == NULL || blur->field_0x14 <= 0 || !dusk::game_clock::is_sim_frame()) {
        return;
    }

    dComIfGd_entryZSortXluList(blur, blur->field_0x308[0]);

    if (!mLoggedSwordTrailDraw && mSwordFxLogCount < l_swordFxLogMax) {
        mLoggedSwordTrailDraw = true;
        mSwordFxLogCount++;
        Log.debug("Puppet {} sword fx: trail drawn, {} points, alpha {}", mPlayerId,
            blur->field_0x14, blur->field_0x20);
    }
}

/**
 * From ~daRemotePlayer_c, which every teardown path goes through (a player leaving, a room or
 * stage unload, an outfit rebuild). Hides whatever this puppet still has out — his own effects end
 * abruptly on a proc change, so an abrupt end here matches — and releases the trail's PC texture
 * object, which a solid heap freed wholesale would otherwise leak (daAlink_c's is a member, and its
 * destructor does this for him).
 *
 * The packet itself cannot still be in a draw list: deletes run in fpcDt_Handler, before the next
 * draw pass resets the lists (f_pc_manager.cpp:86 and f_pc_draw.cpp:42).
 */
void daRemotePlayer_c::deleteSwordFx() {
    if (swordfx_particles_ready()) {
        for (int i = 0; i < 6; i++) {
            swordfx_stop(mSwordSpinEmitter[i]);
        }
        for (int i = 0; i < 3; i++) {
            swordfx_stop(mSwordChargeEmitter[i]);
        }
    }
    for (int i = 0; i < 6; i++) {
        mSwordSpinEmitter[i] = 0;
    }
    for (int i = 0; i < 3; i++) {
        mSwordChargeEmitter[i] = 0;
    }

    daAlink_blur_c* blur = static_cast<daAlink_blur_c*>(mpSwordBlurPacket);
    if (blur != NULL) {
        blur->field_0x14 = 0;
#if TARGET_PC
        blur->mBlurTexObj.reset();
        blur->mpCachedBlurTex = NULL;
#endif
    }
}
