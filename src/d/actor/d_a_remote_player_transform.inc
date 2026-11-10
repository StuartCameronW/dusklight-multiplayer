/**
 * d_a_remote_player_transform.inc
 * Remote player puppet: the human <-> wolf transformation's visuals (wolf piece C), on BOTH puppet
 * kinds. Spec: .claude/future-plans/puppet-wolf.md sections 3 and 6 C.
 *
 * Dusk-only. Included once, at the end of d_a_remote_player.cpp, after the foot fx: it uses
 * init_model / own_archive_idx / load_aram_anm from that file, held_load_aram_bmd from
 * d_a_remote_player_held.inc, and the emitter API and setDamageReg1 from d_a_remote_player_fx.inc.
 *
 * What daAlink_c does during PROC_METAMORPHOSE (d_a_alink.cpp:17421-17696), and what is rebuilt
 * here from protocol v21's form / metaTint / furFrame (player_state.hpp kPlayerFormMetaPhaseMask):
 *
 *   - While mClothesChangeWaitTimer != 0 he draws NOTHING but mpWlChangeModel, the black blob,
 *     at MAJI colour +/-255 (draw :19505-19522), posed on his base matrix (+30 Z as a human,
 *     :18648-18655). The wire's kPlayerFormChanging is exactly that window.
 *   - Otherwise his tint ladder's PROC_METAMORPHOSE rung: TevColor = field_0x300e on every channel
 *     (:19531-19536), which also takes the damage pulse's place and, as a human, goes into the
 *     outfit's register-1 materials (setWaterDropColor(&TevColor), :19587-19598).
 *   - The human's fur overlay: AL_WF on head joint 4 with WFCHANGEATOW (turning into a wolf) or
 *     WFCHANGEWTOA (just turned back), played at field_0x33dc (setMetamorphoseModel :14128-14147,
 *     placed :6040-6041, posed :6083-6089). The wire carries that frame as furFrame.
 *   - setMetamorphoseEffect's particles (d_a_alink_effect.inc:1557-1595), metaPhase x wolf picking
 *     the list. ZI_J_ATOW_C is NOT reproduced: it is placed at cXyz::Zero and its PC fix scales it
 *     by the screen aspect (:1566-1572), i.e. it is HIS screen's overlay, not something in the
 *     world. Reasoned from the code, not seen.
 *
 * The rebuild: the bridge recreates the puppet when the wolf bit flips (spec section 3), which the
 * sender does inside the changing window. The old puppet can still be alive after the window ends
 * (phase 2); it keeps the blob up rather than show the wrong body for the gap, and logs how long
 * that lasted, which is the measured rebuild gap.
 *
 * Not reproduced: his hat's X squash during the proc (field_0x347c, headModelCallBack :2494-2496),
 * which lives in the head callback this file does not own.
 *
 * Cost (heap): the blob is an instance on data already resident in the private "Alink" mount; the
 * fur is a 0x6000-staged BMD plus two BCKs, human puppet only. setupTransformFx logs the bytes.
 */

namespace {

/* v21 form byte bits (dusk::mp::kPlayerFormMetaPhaseMask / kPlayerFormChanging / kPlayerFormFur,
 * player_state.hpp), restated because this file does not include the net layer. */
const u8 l_tfPhaseMask = 0x03;
const u8 l_tfChanging = 1 << 2;
const u8 l_tfFur = 1 << 3;

/// "No particle in this slot this tick".
const u16 l_tfNoFx = 0xFFFF;

/// His loadAramBmd literal for AL_WF (d_a_alink.cpp:14138).
const u32 l_tfFurBmdBufSize = 0x6000;

/// setMetamorphoseEffect's joint (mpLinkModel->getAnmMtx(2), d_a_alink_effect.inc:1585-1590).
const u16 l_tfFxJointNo = 2;

/// WL_CHANGE's offset on a human (d_a_alink.cpp:18650-18652).
const f32 l_tfBlobHumanZ = 30.0f;

const u16 l_tfLogMax = 64;

const char* tf_fx_name(u16 i_name) {
    switch (i_name) {
    case ID_ZI_J_ATOW_A:
        return "ATOW_A";
    case ID_ZI_J_ATOW_B:
        return "ATOW_B";
    case ID_ZI_J_WTOA_A:
        return "WTOA_A";
    case ID_ZI_J_WTOA_B:
        return "WTOA_B";
    default:
        return "-";
    }
}

s32 tf_heap_free() {
    JKRHeap* heap = JKRGetCurrentHeap();
    return heap != NULL ? heap->getFreeSize() : 0;
}

}  // namespace

/**
 * createHeap(), last. Every part is non-fatal: a missing blob costs the blob (the puppet then draws
 * nothing during the changing window, which is what he shows apart from the blob), a missing fur
 * costs the fur.
 */
void daRemotePlayer_c::setupTransformFx() {
    mpWlChangeModel = NULL;
    mpTransformFurModel = NULL;
    mpTransformFurBck[0] = NULL;
    mpTransformFurBck[1] = NULL;
    mTransformFurBckReady = false;

    const bool wolf = isWolf();
    const s32 free0 = tf_heap_free();

    // His initModel(dRes_ID_ALINK_BMD_WL_CHANGE_e, 0) (d_a_alink.cpp:4248) = (0x80000, 0), out of
    // this puppet's own Alink copy: the instance is the only new memory.
    J3DModelData* blobData =
        static_cast<J3DModelData*>(own_archive_idx(mAlinkRes, dRes_INDEX_ALINK_BMD_WL_CHANGE_e));
    if (blobData == NULL) {
        Log.warn("Puppet {} transform fx: WL_CHANGE is not in the private Alink mount ({}); the "
                 "transform blob will not be drawn",
            mPlayerId, mAlinkRes.getArchive() != NULL ? "index missing" : "not mounted");
    } else {
        mpWlChangeModel = init_model(blobData, 0, "transform blob");
    }
    const s32 free1 = tf_heap_free();

    // The fur is a human's only: he loads it in setMetamorphoseModel, which only a human calls
    // (procCoMetamorphoseInit :17478, procCoMetamorphose :17581).
    if (!wolf) {
        // initModel(loadAramBmd(AL_WF, 0x6000), 0) (:14138).
        mpTransformFurModel = init_model(
            held_load_aram_bmd(dRes_ID_ALANM_BMD_AL_WF_e, l_tfFurBmdBufSize, "transform fur"), 0,
            "transform fur");
    }
    const s32 free2 = tf_heap_free();

    if (mpTransformFurModel != NULL) {
        const u16 bckIdx[2] = {dRes_ID_ALANM_BCK_WFCHANGEATOW_e, dRes_ID_ALANM_BCK_WFCHANGEWTOA_e};
        for (int i = 0; i < 2; i++) {
            mpTransformFurBck[i] =
                static_cast<J3DAnmTransform*>(load_aram_anm(bckIdx[i], ANM_FAMILY_TRANSFORM));
            // mItemBck.init(bck, FALSE, 2, 1.0f, 0, -1, false) (:14140): one calculator, made
            // here in the solid heap; the other BCK is swapped in with changeBckOnly.
            if (mpTransformFurBck[i] != NULL && !mTransformFurBckReady) {
                mTransformFurBckReady =
                    mTransformFurBck.init(mpTransformFurBck[i], FALSE, 2, 1.0f, 0, -1, false) != 0;
            }
        }
    }
    const s32 free3 = tf_heap_free();

    Log.info("Puppet {} transform fx setup ({} puppet): blob {} ({} B), fur model {} ({} B), fur "
             "BCK ATOW {} WTOA {} calc {} ({} B); {} B in all, {} free after",
        mPlayerId, wolf ? "wolf" : "human", mpWlChangeModel != NULL ? "ok" : "MISSING",
        free0 - free1, wolf ? "n/a" : (mpTransformFurModel != NULL ? "ok" : "MISSING"),
        free1 - free2, mpTransformFurBck[0] != NULL ? "ok" : "MISSING",
        mpTransformFurBck[1] != NULL ? "ok" : "MISSING", mTransformFurBckReady ? "ok" : "none",
        free2 - free3, free0 - free3, free3);
}

/**
 * execute(), after setMatrix() and the other fx: this tick's joints exist. Decides the blob, poses
 * it and the fur, refreshes the emitters, and logs the edges.
 */
void daRemotePlayer_c::updateTransformFx() {
    const u8 phase = static_cast<u8>(mNetForm & l_tfPhaseMask);
    const bool changing = (mNetForm & l_tfChanging) != 0;
    const bool fur = (mNetForm & l_tfFur) != 0;
    const bool wolf = isWolf();
    J3DModel* body = model;
    const u16 jointNum = body != NULL ? body->getModelData()->getJointNum() : 0;

    /* The stale-body hold. Within one puppet's life, the pre-swap half followed by phase 2 means
     * the swap happened on the sender and this puppet is the OLD body awaiting its rebuild. The
     * rebuilt puppet is created inside the changing window (the wolf bit flips at the swap, timer
     * 1), so it never sees the pre-swap half and is never held. */
    if (phase != 2 && mTransformStale) {
        if (mTransformLogCount < l_tfLogMax) {
            mTransformLogCount++;
            Log.info("Puppet {} transform fx: stale-body hold ended after {} ticks without a "
                     "rebuild (phase now {})",
                mPlayerId, mTransformStaleTicks, phase);
        }
        mTransformStale = false;
    }
    if (phase == 0) {
        mTransformSawPreSwap = false;
        mTransformFxPosValid = false;
    } else if (phase == 1 && !changing) {
        mTransformSawPreSwap = true;
    } else if (phase == 2 && mTransformSawPreSwap && !mTransformStale) {
        mTransformStale = true;
        mTransformStaleTicks = 0;
    }
    if (mTransformStale && mTransformStaleTicks < 0xFFFF) {
        mTransformStaleTicks++;
    }

    // The blob: his field_0x300e > 0 ? 255 : -255, read while it is his to read.
    if (changing) {
        mTransformBlobSign = mNetMetaTint > 0 ? 1 : -1;
    }
    mTransformBlobOn = changing || mTransformStale;
    mTransformBlobPosed = false;
    if (mTransformBlobOn && mpWlChangeModel != NULL && body != NULL) {
        mDoMtx_stack_c::copy(body->getBaseTRMtx());
        if (!wolf) {
            mDoMtx_stack_c::transM(0.0f, 0.0f, l_tfBlobHumanZ);
        }
        mpWlChangeModel->setBaseTRMtx(mDoMtx_stack_c::get());
        mpWlChangeModel->calc();
        mTransformBlobPosed = true;
    }

    /* The fur: setItemMatrix's 0x106 branch, base on head joint 4 then the BCK entered at his
     * frame, BEFORE the calc (memory: post-calc joint writes are invisible). Phase 1 on a human is
     * human -> wolf (WFCHANGEATOW), phase 2 is wolf -> human (WFCHANGEWTOA). The BCK objects are
     * this puppet's own and this is their only frame driver. */
    u8 furShown = 0;
    if (!wolf && fur && phase != 0 && !mTransformBlobOn && mpTransformFurModel != NULL &&
        mTransformFurBckReady && jointNum > l_headJointNo)
    {
        const int idx = phase == 1 ? 0 : 1;
        J3DAnmTransform* bck = mpTransformFurBck[idx];
        if (bck != NULL) {
            mpTransformFurModel->setBaseTRMtx(body->getAnmMtx(l_headJointNo));
            if (mTransformFurBck.getBckAnm() != bck) {
                mTransformFurBck.changeBckOnly(bck);
            }
            const f32 frameMax = bck->getFrameMax();
            f32 frame = frameMax * static_cast<f32>(mNetFurFrame) / 255.0f;
            // His clamp (:6084-6086): never frameMax itself.
            if (frame >= frameMax) {
                frame = frameMax - 0.001f;
            }
            if (frame < 0.0f) {
                frame = 0.0f;
            }
            mTransformFurBck.entry(mpTransformFurModel->getModelData(), frame);
            mpTransformFurModel->calc();
            furShown = static_cast<u8>(idx + 1);
        }
    }
    mTransformFurShown = furShown;

    /* The particles, setMetamorphoseEffect's lists (d_a_alink_effect.inc:1574-1595):
     *   phase 1 human : field_0x37c8 = joint 2, ATOW_A + ATOW_B there   (+ ATOW_C, his screen's)
     *   phase 1 wolf  : WTOA_B on joint 2's RT matrix, field_0x37c8 = joint 2
     *   phase 2 wolf  : ATOW_A + ATOW_B at field_0x37c8                (+ ATOW_C)
     *   phase 2 human : WTOA_A at field_0x37c8
     * His field_0x37c8 in phase 2 is the OLD body's last joint 2; this puppet is the rebuilt one
     * and never saw it, so it takes its own joint 2 on its first phase-2 tick and freezes it. A
     * stale (held) body emits nothing: the rebuilt puppet owns phase 2. */
    u16 names[2] = {l_tfNoFx, l_tfNoFx};
    bool onJoint = false;
    MtxP fxMtx = NULL;
    if (phase != 0 && !mTransformStale && body != NULL && jointNum > l_tfFxJointNo) {
        fxMtx = body->getAnmMtx(l_tfFxJointNo);
        if (phase == 1) {
            mDoMtx_multVecZero(fxMtx, &mTransformFxPos);
            mTransformFxPosValid = true;
            if (wolf) {
                names[0] = ID_ZI_J_WTOA_B;
                onJoint = true;
            } else {
                names[0] = ID_ZI_J_ATOW_A;
                names[1] = ID_ZI_J_ATOW_B;
            }
        } else {
            if (!mTransformFxPosValid) {
                mDoMtx_multVecZero(fxMtx, &mTransformFxPos);
                mTransformFxPosValid = true;
            }
            if (wolf) {
                names[0] = ID_ZI_J_ATOW_A;
                names[1] = ID_ZI_J_ATOW_B;
            } else {
                names[1] = ID_ZI_J_WTOA_A;
            }
        }
    }
    for (int i = 0; i < 2; i++) {
        const int slot = PUPPET_FX_TRANSFORM_FIRST + i;
        if (names[i] == l_tfNoFx) {
            /* Not refreshed: forget the handle WITHOUT hiding it, so the particle manager cuts it
             * on its next pass and its particles finish, as his do when setMetamorphoseEffect
             * stops naming a slot (level_c::execute, d_particle.cpp:971-984). */
            mFxEmitter[slot] = 0;
            continue;
        }
        const bool atJoint = i == 0 && onJoint;
        // His setEmitter(&slot, name, pos, NULL): set, then levelEmitterOnEventMove
        // (d_a_alink_effect.inc:25-31), so a running local event does not freeze it.
        JPABaseEmitter* emitter =
            setPuppetEmitter(slot, names[i], atJoint ? &current.pos : &mTransformFxPos);
        if (emitter != NULL) {
            dComIfGp_particle_levelEmitterOnEventMove(mFxEmitter[slot]);
            if (atJoint) {
                emitter->setGlobalRTMatrix(fxMtx);
            }
        }
    }

    // Latched edge log: phase / changing / fur, plus the stale hold and what is drawn for them.
    const u16 state =
        static_cast<u16>(0x8000 | (mNetForm & (l_tfPhaseMask | l_tfChanging | l_tfFur)) |
                         (mTransformStale ? 0x100 : 0) | (furShown << 9));
    if (state != mTransformLoggedState && mTransformLogCount < l_tfLogMax) {
        mTransformLogCount++;
        const bool had = mTransformLoggedState != 0;
        const u16 prev = mTransformLoggedState;
        Log.info("Puppet {} transform fx: phase {}->{} changing {}->{} fur {}->{} stale {} | {} "
                 "puppet: blob {} (sign {}), fur {} frame {}/255, fx A {} B {}, tint {}",
            mPlayerId, had ? static_cast<int>(prev & l_tfPhaseMask) : -1, phase,
            had ? ((prev & l_tfChanging) != 0 ? 1 : 0) : -1, changing ? 1 : 0,
            had ? ((prev & l_tfFur) != 0 ? 1 : 0) : -1, fur ? 1 : 0, mTransformStale ? 1 : 0,
            wolf ? "wolf" : "human",
            !mTransformBlobOn ? "off" : (mTransformBlobPosed ? "DRAWN" : "MISSING (draws nothing)"),
            mTransformBlobSign > 0 ? "+255" : "-255",
            furShown == 1 ? "ATOW" :
                            (furShown == 2 ? "WTOA" :
                                             (fur && !wolf && phase != 0 && !mTransformBlobOn ?
                                                     "NOT SHOWN (fur model/BCK missing)" :
                                                     "none")),
            static_cast<unsigned>(mNetFurFrame), tf_fx_name(names[0]), tf_fx_name(names[1]),
            static_cast<int>(mNetMetaTint));
        mTransformLoggedState = state;
    }
}

/**
 * draw(), right after settingTevStruct and before any tint or model. True: the caller draws
 * nothing else, as his draw returns right after the blob (d_a_alink.cpp:19505-19522). The caller's
 * one-entry-per-sim-tick guard runs before this, so the blob is entered at most once per pass.
 */
bool daRemotePlayer_c::drawTransformBlob() {
    if (!mTransformBlobOn) {
        return false;
    }
    if (mTransformBlobPosed) {
        const s16 colour = mTransformBlobSign > 0 ? 255 : -255;
        tevStr.TevColor.r = colour;
        tevStr.TevColor.g = colour;
        tevStr.TevColor.b = colour;
        // His is 0 here: every full draw of his zeroes it after the body (:19620, :19732).
        tevStr.TevKColor.r = 0;
        tevStr.TevKColor.g = 0;
        tevStr.TevKColor.b = 0;
        g_env_light.setLightTevColorType_MAJI(mpWlChangeModel, &tevStr);
        mDoExt_modelEntryDL(mpWlChangeModel);
    }
    return true;
}

/**
 * draw(), right after applyDamageTint(): his ladder's PROC_METAMORPHOSE rung (:19531-19536) sits
 * above freeze and damage, so it overwrites what applyDamageTint wrote, including the damage
 * pulse's KColor. As a human he also puts it in the outfit's register-1 materials (:19587-19598,
 * var_r31 stays TRUE); a wolf does not (:19599). applyDamageTint restores those on the next draw.
 */
void daRemotePlayer_c::applyTransformTint() {
    if ((mNetForm & l_tfPhaseMask) == 0) {
        return;
    }
    tevStr.TevColor.r = mNetMetaTint;
    tevStr.TevColor.g = mNetMetaTint;
    tevStr.TevColor.b = mNetMetaTint;
    tevStr.TevKColor.r = 0;
    tevStr.TevKColor.g = 0;
    tevStr.TevKColor.b = 0;
    if (!isWolf()) {
        setDamageReg1(true);
    }
    if (!mTransformLoggedTint) {
        mTransformLoggedTint = true;
        Log.debug("Puppet {} transform tint live: TevColor {} on all channels, reg-1 materials {}",
            mPlayerId, static_cast<int>(mNetMetaTint),
            isWolf() ? "untouched (wolf)" : (mDmgReg1On ? "tinted" : "not applied"));
    }
}

/// draw(), right after drawHeldItems(): where he draws mHeldItemModel (:19771-19773).
void daRemotePlayer_c::drawTransformFur() {
    if (mTransformFurShown != 0 && mpTransformFurModel != NULL) {
        drawModel(mpTransformFurModel);
    }
}

/// The destructor, before deletePuppetEmitters(). The old body's particles fade out on their own
/// across the rebuild; the new puppet starts its own list on its first tick.
void daRemotePlayer_c::releaseTransformFx() {
    if (mTransformStale) {
        Log.info("Puppet {} transform fx: stale body replaced after {} ticks of blob hold (the "
                 "rebuild gap)",
            mPlayerId, mTransformStaleTicks);
    }
    for (int slot = PUPPET_FX_TRANSFORM_FIRST; slot <= PUPPET_FX_TRANSFORM_LAST; slot++) {
        mFxEmitter[slot] = 0;
    }
}
