/**
 * d_a_remote_player_held.inc
 * Remote player puppet: the item model in his hands, and the UPPER_2 overlay (wire block 3).
 *
 * Dusk-only, like the rest of the puppet. Included once, at the end of d_a_remote_player.cpp, so it
 * shares that file's helpers (init_model, own_archive_*, loadActionBck) and keeps this feature's
 * code out of the functions other work is editing.
 *
 * The rule throughout is daAlink_c's own placement, transcribed: every offset, joint and model flag
 * below is a literal from his code, cited where it is copied. The sender decides WHICH item and
 * WHICH hand (player_bridge.cpp fill_held_item); this file only reproduces where he puts it.
 */

namespace {

/// daAlink_c::setItemMatrix's lantern hip joint (d_a_alink.cpp:6105).
const u16 l_heldLanternHipJointNo = 0x10;

/// The clawshot root: where the tip sits on the gun at rest (d_a_alink_hook.inc:914, :1032-1034).
const Vec l_heldHookRoot = {0.0f, 0.0f, 23.5f};

/// daAlink_c::setUpperAnimeMorf's human joint range (field_0x30a8 / field_0x30aa,
/// d_a_alink_wolf.inc:545-546), and setUpperAnime's buffer (d_a_alink.cpp:7374).
const u16 l_upper2MorfStartJoint = 1;
const u16 l_upper2MorfEndJoint = 16;
const u32 l_upper2LoadSize = 0x2C00;

const u16 l_heldLogMax = 64;

/**
 * Headroom required on the current (actor solid) heap before a BMD is parsed into it.
 *
 * ★ Checked up front because the BMD loader is NOT safe against a failed allocation the way
 * mDoExt_J3DModel__create is: it dereferences what it allocates. So "allocation is non-fatal" has
 * to mean "refuse before starting", not "notice afterwards". The figure is a guess with margin, not
 * a measurement: twice the staging buffer (the loader's material/shape objects on PC are not
 * smaller than the GC file) plus 32 KB for the J3DModel instance and its display lists.
 */
bool held_heap_fits(u32 i_bufSize, const char* i_what) {
    JKRHeap* heap = JKRGetCurrentHeap();
    const u32 need = i_bufSize * 2 + 0x8000;
    if (heap == NULL || static_cast<u32>(heap->getFreeSize()) < need ||
        static_cast<u32>(heap->getMaxAllocatableSize(0x20)) < i_bufSize + 0x40)
    {
        Log.warn("Puppet held item '{}' skipped: heap has {} free (largest {}), wants {}", i_what,
            heap != NULL ? heap->getFreeSize() : 0,
            heap != NULL ? heap->getMaxAllocatableSize(0x20) : 0, need);
        return false;
    }
    return true;
}

/**
 * daAlink_c::loadAramBmd (d_a_alink.cpp:14321-14341), line for line, into the CURRENT heap — the
 * puppet's solid heap at createHeap time — with his per-model buffer literal. Plus the two refusals
 * a puppet needs and he does not: an index absent from the archive (a peer on another build) and a
 * heap too short to finish the load.
 */
J3DModelData* held_load_aram_bmd(u16 i_resIdx, u32 i_bufSize, const char* i_what) {
    JKRArchive* anmArchive = dComIfGp_getAnmArchive();
    if (anmArchive == NULL || anmArchive->findIdxResource(i_resIdx) == NULL) {
        Log.warn("Puppet held item '{}': BMD {} is not in the ARAM archive", i_what, i_resIdx);
        return NULL;
    }
    if (!held_heap_fits(i_bufSize, i_what)) {
        return NULL;
    }

    u8* tmpBuffer = JKR_NEW_ARRAY_ARGS(u8, i_bufSize, 0x20);
    if (tmpBuffer == NULL) {
        Log.warn("Puppet held item '{}': the {}-byte buffer did not allocate", i_what, i_bufSize);
        return NULL;
    }
    std::memset(tmpBuffer, 0, 4);
    if (JKRReadIdxResource(tmpBuffer, i_bufSize, i_resIdx, anmArchive) == 0 ||
        tmpBuffer[0] != 'J' || tmpBuffer[1] != '3' || tmpBuffer[2] != 'D')
    {
        Log.warn("Puppet held item '{}': BMD {} read nothing usable", i_what, i_resIdx);
        return NULL;
    }

    u32 type = 'BMWR';
    JKRArchive::SDIDirEntry* dir = anmArchive->mNodes;
    for (int i = 0; i < anmArchive->countDirectory(); i++, dir++) {
        if (i_resIdx >= dir->first_file_index &&
            i_resIdx < dir->first_file_index + dir->num_entries)
        {
            type = dir->type;
            break;
        }
    }

    return dRes_info_c::loaderBasicBmd(type, tmpBuffer);
}

/**
 * daAlink_c::loadAram (d_a_alink.cpp:14343-14351) with his 0x400 literal, refusing an absent index
 * or a loaded object of the wrong kind (5 = J3DAnmTevRegKey, 2 = J3DAnmTexPattern).
 */
J3DAnmBase* held_load_aram_anm(u16 i_resIdx, s32 i_kind, const char* i_what) {
    const u32 bufSize = 0x400;
    JKRArchive* archive = dComIfGp_getAnmArchive();
    if (archive == NULL || archive->findIdxResource(i_resIdx) == NULL) {
        Log.warn("Puppet held item '{}': anm {} is not in the ARAM archive", i_what, i_resIdx);
        return NULL;
    }
    JKRHeap* heap = JKRGetCurrentHeap();
    if (heap == NULL || static_cast<u32>(heap->getFreeSize()) < bufSize * 4) {
        Log.warn("Puppet held item '{}': no heap for anm {}", i_what, i_resIdx);
        return NULL;
    }
    u8* tmpBuffer = JKR_NEW_ARRAY_ARGS(u8, bufSize, 0x20);
    if (tmpBuffer == NULL) {
        return NULL;
    }
    std::memset(tmpBuffer, 0, 4);
    if (JKRReadIdxResource(tmpBuffer, bufSize, i_resIdx, archive) == 0 || tmpBuffer[0] != 'J' ||
        tmpBuffer[1] != '3' || tmpBuffer[2] != 'D')
    {
        Log.warn("Puppet held item '{}': anm {} read nothing usable", i_what, i_resIdx);
        return NULL;
    }
    J3DAnmBase* anm = J3DAnmLoaderDataBase::load(tmpBuffer);
    if (anm == NULL || anm->getKind() != i_kind) {
        Log.warn("Puppet held item '{}': anm {} is kind {}, wanted {}", i_what, i_resIdx,
            anm != NULL ? anm->getKind() : -1, i_kind);
        return NULL;
    }
    return anm;
}

/// The bottle's joint-1 trampoline: daAlink_bottleModelCallBack (d_a_alink_bottle.inc:23-35).
int daRemotePlayer_heldBottleModelCallBack(J3DJoint* i_joint, int i_pass) {
    UNUSED(i_joint);
    if (i_pass != 0) {
        return 1;
    }
    J3DModel* model = j3dSys.getModel();
    if (model == NULL) {
        return 1;
    }
    daRemotePlayer_c* puppet = reinterpret_cast<daRemotePlayer_c*>(model->getUserArea());
    if (puppet != NULL) {
        puppet->heldBottleCallBack();
    }
    return 1;
}

u8 held_kind(u8 i_kind) {
    return i_kind < dusk::mp::kPlayerHeldKindNum ? i_kind : dusk::mp::kPlayerHeldNone;
}

bool held_is_clawshot(u8 i_kind) {
    return i_kind == dusk::mp::kPlayerHeldClawshot || i_kind == dusk::mp::kPlayerHeldDoubleClawshot;
}

}  // namespace

/* The wire's PlayerHeldItem values index mpHeldModel, which the header sizes as a bare 9 for the
 * same reason mpSwordModel is a bare 3. STATIC_ASSERT is a no-op off the GC target (see the shield
 * one); held_kind() is the real guard. */
STATIC_ASSERT(dusk::mp::kPlayerHeldKindNum == 9);

/**
 * Build every handheld item model the puppet might need, once, at createHeap time. Called LAST in
 * createHeap — after the action override's buffer — so a short heap costs held items, never a body
 * model, a gait or the override.
 *
 * Each model is daAlink_c's own: the same ARAM resource, his buffer literal, and his init path and
 * flags (initModel = init_model, initModelEnv = init_model_env):
 *
 *   - bottle: AL_BOTTLE 0x5C00, initModelEnv 0x1020200, + BTP/BRK (d_a_alink_bottle.inc:52-54)
 *   - bow: AL_BOW 0x4C00, initModel 0 (d_a_alink_bow.inc:515)
 *   - slingshot: AL_PACHI 0x2C00, initModel 0 (d_a_alink_bow.inc:59)
 *   - clawshot: AL_HS 0x5C00 + AL_HS_TIP 0x3800, initModel 0, and the second pair built from the
 *     same data (d_a_alink_hook.inc:435-438)
 *   - copy rod: AL_CROD 0x5400, initModel 0x1000000, + BRK (d_a_alink_copyrod.inc:110-111)
 *   - horse call: AL_PEND 0x3000, initModel 0 (d_a_alink_whistle.inc:57)
 *   - boomerang: Alink al_boom (0x1F), 0x80000 / 0x11000084 (d_a_boomerang.cpp:1406)
 *   - lantern: al_kantera.bmd from the OUTFIT archive, initModelEnv 0 (d_a_alink_wolf.inc:405)
 *
 * NOT built: the bottle's slosh/drink BTKs, the fairy/worm/bee contents models, the item BCKs (bow
 * string, clawshot spin, tip open), the lantern glow, the clawshot chain. The model is in the hand;
 * those are its motion. Every load is individually non-fatal.
 */
void daRemotePlayer_c::setupHeldItems() {
    for (int i = 0; i < dusk::mp::kPlayerHeldKindNum; i++) {
        mpHeldModel[i] = NULL;
        mLoggedHeldMissing[i] = false;
        mLoggedHeldDraw[i] = false;
    }
    mpHeldClaw2Model = NULL;
    mpHeldClawTip[0] = NULL;
    mpHeldClawTip[1] = NULL;
    mpHeldLanternModel = NULL;
    mpHeldBottleBrk = NULL;
    mpHeldBottleBtp = NULL;
    mpHeldCopyRodBrk = NULL;
    mNetHeldKind = dusk::mp::kPlayerHeldNone;
    mNetHeldFlags = 0;
    mNetHeldMat = 0;
    mNetUpper2Idx = l_actNone;
    mNetUpper2Frame = 0.0f;
    mNetUpper2Ratio = 0.0f;
    mNetUpper2Morf = 0.0f;
    mHeldDrawnKind = dusk::mp::kPlayerHeldNone;
    mHeldDrawnFlags = 0;
    mHeldDrawnMat = 0;
    mUpper2Shown = l_actNone;
    mpUpper2Bck = NULL;
    mUpper2Failed = l_actNone;
    mUpper2AppliedMorf = 0;
    mHeldLogCount = 0;
    mLoggedLantern = false;

    JKRHeap* heap = JKRGetCurrentHeap();
    const s32 freeBefore = heap != NULL ? heap->getFreeSize() : 0;

    // Bottle, with its contents colour and stopper pattern entered as loadAramItemBrk/Btp do
    // (d_a_alink.cpp:14353-14384): frame 0, searchUpdateMaterialID, entry*Animator.
    J3DModel* bottle =
        init_model_env(held_load_aram_bmd(dRes_ID_ALANM_BMD_AL_BOTTLE_e, 0x5C00, "bottle"),
            0x1020200, "held bottle");
    mpHeldModel[dusk::mp::kPlayerHeldBottle] = bottle;
    if (bottle != NULL) {
        J3DModelData* data = bottle->getModelData();
        mpHeldBottleBtp = static_cast<J3DAnmTexPattern*>(
            held_load_aram_anm(dRes_ID_ALANM_BTP_AL_BOTTLE_e, 2, "bottle btp"));
        if (mpHeldBottleBtp != NULL) {
            mpHeldBottleBtp->setFrame(0.0f);
            mpHeldBottleBtp->searchUpdateMaterialID(data);
            data->entryTexNoAnimator(mpHeldBottleBtp);
        }
        mpHeldBottleBrk = static_cast<J3DAnmTevRegKey*>(
            held_load_aram_anm(dRes_ID_ALANM_BRK_AL_BOTTLE_e, 5, "bottle brk"));
        if (mpHeldBottleBrk != NULL) {
            mpHeldBottleBrk->setFrame(0.0f);
            mpHeldBottleBrk->searchUpdateMaterialID(data);
            data->entryTevRegAnimator(mpHeldBottleBrk);
        }
        /* The cork: his joint-1 callback, installed as setBottleModel installs it for a
         * non-pouring bottle (d_a_alink_bottle.inc:95-98). It acts only while the wire says the
         * cork is out (kPlayerHeldBottleCork), which the sender never sets while pouring — the
         * case in which he removes the callback. */
        if (data->getJointNum() > 1) {
            bottle->setUserArea((uintptr_t)this);
            data->getJointNodePointer(1)->setCallBack(daRemotePlayer_heldBottleModelCallBack);
        }
    }

    mpHeldModel[dusk::mp::kPlayerHeldBow] =
        init_model(held_load_aram_bmd(dRes_ID_ALANM_BMD_AL_BOW_e, 0x4C00, "bow"), 0, "held bow");
    mpHeldModel[dusk::mp::kPlayerHeldSlingshot] = init_model(
        held_load_aram_bmd(dRes_ID_ALANM_BMD_AL_PACHI_e, 0x2C00, "slingshot"), 0, "held slingshot");

    J3DModel* claw = init_model(
        held_load_aram_bmd(dRes_ID_ALANM_BMD_AL_HS_e, 0x5C00, "clawshot"), 0, "held clawshot");
    if (claw != NULL) {
        mpHeldClawTip[0] =
            init_model(held_load_aram_bmd(dRes_ID_ALANM_BMD_AL_HS_TIP_e, 0x3800, "clawshot tip"), 0,
                "held clawshot tip");
        // The second pair is two more J3DModels on the SAME data, as his field_0x0710/0714 are.
        if (held_heap_fits(0, "second clawshot")) {
            mpHeldClaw2Model = init_model(claw->getModelData(), 0, "held clawshot 2");
            if (mpHeldClawTip[0] != NULL) {
                mpHeldClawTip[1] =
                    init_model(mpHeldClawTip[0]->getModelData(), 0, "held clawshot tip 2");
            }
        }
    }
    mpHeldModel[dusk::mp::kPlayerHeldClawshot] = claw;
    // The same instance: only one kind is drawn per pass, so it can never be entered twice.
    mpHeldModel[dusk::mp::kPlayerHeldDoubleClawshot] = claw;

    J3DModel* rod = init_model(
        held_load_aram_bmd(dRes_ID_ALANM_BMD_AL_CROD_e, 0x5400, "copy rod"), 0x1000000, "held rod");
    mpHeldModel[dusk::mp::kPlayerHeldCopyRod] = rod;
    if (rod != NULL) {
        mpHeldCopyRodBrk = static_cast<J3DAnmTevRegKey*>(
            held_load_aram_anm(dRes_ID_ALANM_BRK_AL_CROD_CHANGE_COLOR_e, 5, "copy rod brk"));
        if (mpHeldCopyRodBrk != NULL) {
            mpHeldCopyRodBrk->setFrame(0.0f);
            mpHeldCopyRodBrk->searchUpdateMaterialID(rod->getModelData());
            rod->getModelData()->entryTevRegAnimator(mpHeldCopyRodBrk);
        }
    }

    mpHeldModel[dusk::mp::kPlayerHeldHorseCall] =
        init_model(held_load_aram_bmd(dRes_ID_ALANM_BMD_AL_PEND_e, 0x3000, "horse call"), 0,
            "held horse call");

    /* Out of the puppet's PRIVATE Alink mount and its outfit mount: resident data, so only the
     * J3DModel instance is new. init_model passes 0x80000 / 0|0x11000084, which is exactly
     * daBoomerang_c's raw create; its warp bracket is taken only for a model that carries the
     * dissolve, which a BMDR (al_boom) does not. */
    if (held_heap_fits(0, "boomerang")) {
        mpHeldModel[dusk::mp::kPlayerHeldBoomerang] = init_model(
            static_cast<J3DModelData*>(own_archive_idx(mAlinkRes, dRes_INDEX_ALINK_BMD_AL_BOOM_e)),
            0, "held boomerang");
    }
    if (held_heap_fits(0, "lantern")) {
        mpHeldLanternModel =
            init_model_env(static_cast<J3DModelData*>(own_archive_res(mOwnRes, "al_kantera.bmd")),
                0, "held lantern");
    }

    int built = 0;
    for (int i = 1; i < dusk::mp::kPlayerHeldKindNum; i++) {
        if (mpHeldModel[i] != NULL) {
            built++;
        } else {
            Log.warn("Puppet {} has no held-item model for kind {}; it will appear empty-handed "
                     "while holding that item",
                mPlayerId, i);
        }
    }
    // The measured cost, which the heap budget in create() is sized from.
    Log.info("Puppet {} held items: {}/{} models (claw2 {}, tips {}/{}, lantern {}, bottle brk/btp "
             "{}/{}, rod brk {}), {} bytes of heap ({} -> {} free)",
        mPlayerId, built, dusk::mp::kPlayerHeldKindNum - 1,
        mpHeldClaw2Model != NULL ? "ok" : "MISSING", mpHeldClawTip[0] != NULL ? "ok" : "MISSING",
        mpHeldClawTip[1] != NULL ? "ok" : "MISSING", mpHeldLanternModel != NULL ? "ok" : "MISSING",
        mpHeldBottleBrk != NULL ? "ok" : "MISSING", mpHeldBottleBtp != NULL ? "ok" : "MISSING",
        mpHeldCopyRodBrk != NULL ? "ok" : "MISSING",
        heap != NULL ? freeBefore - heap->getFreeSize() : 0, freeBefore,
        heap != NULL ? heap->getFreeSize() : 0);
}

void daRemotePlayer_c::setNetworkHeldItem(u8 i_kind, u8 i_flags, u8 i_mat, u16 i_upper2Idx,
    f32 i_upper2Frame, f32 i_upper2Ratio, f32 i_upper2Morf) {
    mNetHeldKind = held_kind(i_kind);
    mNetHeldFlags = i_flags;
    mNetHeldMat = i_mat;
    mNetUpper2Idx = i_upper2Idx;
    mNetUpper2Frame = i_upper2Frame;
    mNetUpper2Ratio = i_upper2Ratio;
    mNetUpper2Morf = i_upper2Morf;
}

/// daAlink_c::bottleModelCallBack (d_a_alink_bottle.inc:13-21): the cork, joint 1, rides the right
/// item joint. Runs inside the bottle's calc, after the body's, so the joint matrix is final.
int daRemotePlayer_c::heldBottleCallBack() {
    J3DModel* bottle = mpHeldModel[dusk::mp::kPlayerHeldBottle];
    if ((mNetHeldFlags & dusk::mp::kPlayerHeldBottleCork) != 0 && model != NULL && bottle != NULL) {
        mDoMtx_stack_c::copy(model->getAnmMtx(l_rightItemJointNo));
        mDoMtx_stack_c::transM(-3.0f, -1.0f, 0.3f);
        mDoMtx_stack_c::XYZrotM(cM_deg2s(102.0f), cM_deg2s(-0.5f), cM_deg2s(-122.0f));
        mDoMtx_copy(mDoMtx_stack_c::get(), bottle->getAnmMtx(1));
    }
    return 1;
}

/**
 * Pose the held item and the lantern off the body's item joints. Every tick, from setMatrix(),
 * AFTER the body's calc and setEquipMatrix() — daAlink_c's setItemMatrix order
 * (d_a_alink.cpp:6039-6111).
 *
 * Only the model the wire names is posed, and it is posed before it is drawn on the same tick, so a
 * model that has just been switched to never draws with a stale matrix.
 */
void daRemotePlayer_c::setHeldItemMatrix() {
    const u8 kind = mNetHeldKind;
    J3DModel* held = mpHeldModel[kind];

    if (held != NULL) {
        switch (kind) {
        case dusk::mp::kPlayerHeldBottle: {
            if ((mNetHeldFlags & dusk::mp::kPlayerHeldBottlePour) != 0) {
                // checkOilBottleItemNotGet (d_a_alink.cpp:6042-6046).
                mDoMtx_stack_c::copy(model->getAnmMtx(l_rightItemJointNo));
                mDoMtx_stack_c::transM(1.5f, -7.5f, -1.0f);
                mDoMtx_stack_c::XYZrotM(cM_deg2s(183.0f), cM_deg2s(176.0f), cM_deg2s(167.0f));
            } else {
                // checkBottleItem (:6047-6051).
                mDoMtx_stack_c::copy(model->getAnmMtx(l_leftItemJointNo));
                mDoMtx_stack_c::transM(-10.0f, -0.5f, -5.5f);
                mDoMtx_stack_c::XYZrotM(cM_deg2s(174.0f), cM_deg2s(-47.0f), cM_deg2s(94.0f));
            }
            held->setBaseTRMtx(mDoMtx_stack_c::get());

            // The sender's material frames, on this puppet's own anm objects and data.
            if (mpHeldBottleBrk != NULL) {
                mpHeldBottleBrk->setFrame(
                    static_cast<f32>(mNetHeldMat & dusk::mp::kPlayerHeldMatBottleBrkMask));
            }
            if (mpHeldBottleBtp != NULL) {
                mpHeldBottleBtp->setFrame(
                    static_cast<f32>((mNetHeldMat & dusk::mp::kPlayerHeldMatBottleBtpMask) >>
                                     dusk::mp::kPlayerHeldMatBottleBtpShift));
            }
            J3DModelData* data = held->getModelData();
            if (data->getMaterialNum() > 0) {
                J3DShape* shape = data->getMaterialNodePointer(0)->getShape();
                if ((mNetHeldMat & dusk::mp::kPlayerHeldMatBottleMat0Hidden) != 0) {
                    shape->hide();
                } else {
                    shape->show();
                }
            }
            break;
        }
        case dusk::mp::kPlayerHeldBow:
        case dusk::mp::kPlayerHeldSlingshot:
            // checkBowAndSlingItem (:6058-6066).
            if ((mNetHeldFlags & dusk::mp::kPlayerHeldBowLeftHand) != 0) {
                mDoMtx_stack_c::copy(model->getAnmMtx(l_leftItemJointNo));
                mDoMtx_stack_c::transM(-1.3f, 0.0f, -3.0f);
                mDoMtx_stack_c::XYZrotM(cM_deg2s(-74.0f), cM_deg2s(43.6f), cM_deg2s(1.9f));
                held->setBaseTRMtx(mDoMtx_stack_c::get());
            } else {
                held->setBaseTRMtx(model->getAnmMtx(l_rightItemJointNo));
            }
            break;
        case dusk::mp::kPlayerHeldClawshot:
        case dusk::mp::kPlayerHeldDoubleClawshot: {
            /* setHookshotPos (d_a_alink_hook.inc:848-870): a left-hand and a right-hand placement,
             * and field_0x3020 decides which model takes which. The single clawshot is always DRAWN
             * in the left hand — changeHookshotDrawModel swaps the right-hand one out before the
             * draw (:346-356) — so it takes the left placement here unconditionally. */
            const bool primaryRight = kind == dusk::mp::kPlayerHeldDoubleClawshot &&
                                      (mNetHeldFlags & dusk::mp::kPlayerHeldClawRight) != 0;
            J3DModel* left = primaryRight ? mpHeldClaw2Model : held;
            J3DModel* right = primaryRight ? held : mpHeldClaw2Model;
            if (kind == dusk::mp::kPlayerHeldClawshot) {
                right = NULL;
            }
            if (left != NULL) {
                mDoMtx_stack_c::copy(model->getAnmMtx(l_leftItemJointNo));
                mDoMtx_stack_c::transM(-2.0f, 1.0f, 1.0f);
                mDoMtx_stack_c::XYZrotM(cM_deg2s(5.7f), cM_deg2s(162.0f), 0);
                left->setBaseTRMtx(mDoMtx_stack_c::get());
            }
            if (right != NULL) {
                mDoMtx_stack_c::copy(model->getAnmMtx(l_rightItemJointNo));
                mDoMtx_stack_c::transM(-2.0f, 0.0f, 1.0f);
                mDoMtx_stack_c::XYZrotM(cM_deg2s(-78.0f), cM_deg2s(182.0f), cM_deg2s(-99.0f));
                right->setBaseTRMtx(mDoMtx_stack_c::get());
            }
            break;
        }
        case dusk::mp::kPlayerHeldCopyRod:
            // The default branch (:6069-6080), with the glow frame the sender's rod is showing.
            if (mpHeldCopyRodBrk != NULL) {
                const f32 frameMax = mpHeldCopyRodBrk->getFrameMax();
                f32 frame = frameMax * static_cast<f32>(mNetHeldMat) / 255.0f;
                if (frame > frameMax - 0.001f) {
                    frame = frameMax - 0.001f;
                }
                mpHeldCopyRodBrk->setFrame(frame < 0.0f ? 0.0f : frame);
            }
            held->setBaseTRMtx(model->getAnmMtx(l_leftItemJointNo));
            break;
        case dusk::mp::kPlayerHeldBoomerang:
            // daBoomerang_c::setKeepMatrix (d_a_boomerang.cpp:686-692), off getLeftItemMatrix.
            mDoMtx_stack_c::copy(model->getAnmMtx(l_leftItemJointNo));
            mDoMtx_stack_c::transM(32.0f, -5.0f, -6.0f);
            mDoMtx_stack_c::XYZrotM(cM_deg2s(-4.0f), cM_deg2s(39.0f), cM_deg2s(-9.0f));
            held->setBaseTRMtx(mDoMtx_stack_c::get());
            break;
        default:
            // The horse call: the default branch, plain left item joint (:6080).
            held->setBaseTRMtx(model->getAnmMtx(l_leftItemJointNo));
            break;
        }

        held->calc();

        if (held_is_clawshot(kind)) {
            /* The tip at rest on its gun: base * transM(hookRoot), the placement his second tip
             * takes when it is not flying (:1230-1233) and the point his first one fires from
             * (:1032-1034). A fired tip is NOT followed — that is a world position. */
            J3DModel* guns[2] = {held, mpHeldClaw2Model};
            const int gunNum = kind == dusk::mp::kPlayerHeldDoubleClawshot ? 2 : 1;
            for (int i = 0; i < gunNum; i++) {
                if (guns[i] == NULL) {
                    continue;
                }
                if (i == 1) {
                    guns[i]->calc();
                }
                if (mpHeldClawTip[i] != NULL) {
                    mDoMtx_stack_c::copy(guns[i]->getBaseTRMtx());
                    mDoMtx_stack_c::transM(l_heldHookRoot.x, l_heldHookRoot.y, l_heldHookRoot.z);
                    mpHeldClawTip[i]->setBaseTRMtx(mDoMtx_stack_c::get());
                    mpHeldClawTip[i]->calc();
                }
            }
        }
    }

    // The lantern (:6093-6111): in the left hand, or on the hip joint 0x10.
    if ((mNetHeldFlags & dusk::mp::kPlayerHeldLanternShown) != 0 && mpHeldLanternModel != NULL) {
        if ((mNetHeldFlags & dusk::mp::kPlayerHeldLanternInHand) != 0) {
            mDoMtx_stack_c::copy(model->getAnmMtx(l_leftItemJointNo));
            mDoMtx_stack_c::transM(-2.0f, -0.1f, -0.7f);
            mDoMtx_stack_c::XYZrotM(cM_deg2s(100.0f), cM_deg2s(9.3f), cM_deg2s(183.0f));
        } else {
            mDoMtx_stack_c::copy(model->getAnmMtx(l_heldLanternHipJointNo));
            mDoMtx_stack_c::transM(-1.0f, 4.5f, 9.0f);
            mDoMtx_stack_c::XYZrotM(cM_deg2s(-75.0f), cM_deg2s(62.0f), cM_deg2s(89.0f));
        }
        mpHeldLanternModel->setBaseTRMtx(mDoMtx_stack_c::get());
        mpHeldLanternModel->calc();
    }
}

/**
 * Enter the held item and the lantern. From draw(), right after drawEquip() — his order: sword,
 * sheath, shield, then mHeldItemModel and its tip(s), then the lantern (d_a_alink.cpp:19720-19825).
 *
 * ★ Hang 4: each model is entered at most once here, and draw() carries the per-sim-tick guard.
 * The single and double clawshot share one J3DModel, but only one kind is drawn per pass.
 */
void daRemotePlayer_c::drawHeldItems() {
    const u8 kind = mNetHeldKind;
    J3DModel* held = mpHeldModel[kind];

    mHeldDrawnKind = dusk::mp::kPlayerHeldNone;
    mHeldDrawnFlags = 0;
    mHeldDrawnMat = 0;

    if (held != NULL) {
        drawModel(held);
        if (held_is_clawshot(kind)) {
            drawModel(mpHeldClawTip[0]);
            if (kind == dusk::mp::kPlayerHeldDoubleClawshot &&
                (mNetHeldFlags & dusk::mp::kPlayerHeldClawBoth) != 0)
            {
                drawModel(mpHeldClaw2Model);
                drawModel(mpHeldClawTip[1]);
            }
        }
        mHeldDrawnKind = kind;
        mHeldDrawnFlags = static_cast<u8>(mNetHeldFlags & ~(dusk::mp::kPlayerHeldLanternShown |
                                                              dusk::mp::kPlayerHeldLanternInHand));
        mHeldDrawnMat = mNetHeldMat;

        if (!mLoggedHeldDraw[kind]) {
            mLoggedHeldDraw[kind] = true;
            Log.debug("Puppet {} drawing held item kind {} | flags 0x{:02x} mat 0x{:02x}",
                mPlayerId, static_cast<unsigned>(kind), static_cast<unsigned>(mNetHeldFlags),
                static_cast<unsigned>(mNetHeldMat));
        }
    } else if (kind != dusk::mp::kPlayerHeldNone && !mLoggedHeldMissing[kind]) {
        mLoggedHeldMissing[kind] = true;
        Log.warn("Puppet {} told to hold item kind {} but has no model for it; empty hand",
            mPlayerId, static_cast<unsigned>(kind));
    }

    if ((mNetHeldFlags & dusk::mp::kPlayerHeldLanternShown) != 0 && mpHeldLanternModel != NULL) {
        drawModel(mpHeldLanternModel);
        mHeldDrawnFlags |= static_cast<u8>(mNetHeldFlags & (dusk::mp::kPlayerHeldLanternShown |
                                                               dusk::mp::kPlayerHeldLanternInHand));
        if (!mLoggedLantern) {
            mLoggedLantern = true;
            Log.debug("Puppet {} drawing lantern ({})", mPlayerId,
                (mNetHeldFlags & dusk::mp::kPlayerHeldLanternInHand) != 0 ? "in hand" : "on hip");
        }
    }
}

/// The held models cast into the puppet's real shadow, as his do (d_a_alink.cpp:19312-19345).
/// After shadowDraw(), from draw(); null-tolerant by the API's contract.
void daRemotePlayer_c::addHeldItemShadow() {
    if (mShadowKey == 0) {
        return;
    }
    const u8 kind = mHeldDrawnKind;
    if (kind != dusk::mp::kPlayerHeldNone) {
        dComIfGd_addRealShadow(mShadowKey, mpHeldModel[kind]);
        if (held_is_clawshot(kind)) {
            dComIfGd_addRealShadow(mShadowKey, mpHeldClawTip[0]);
            if ((mHeldDrawnFlags & dusk::mp::kPlayerHeldClawBoth) != 0 &&
                kind == dusk::mp::kPlayerHeldDoubleClawshot)
            {
                dComIfGd_addRealShadow(mShadowKey, mpHeldClaw2Model);
                dComIfGd_addRealShadow(mShadowKey, mpHeldClawTip[1]);
            }
        }
    }
    if ((mHeldDrawnFlags & dusk::mp::kPlayerHeldLanternShown) != 0) {
        dComIfGd_addRealShadow(mShadowKey, mpHeldLanternModel);
    }
}

/* --- The UPPER_2 overlay ---------------------------------------------------------------------- */

/// daAlink_c::setUpperAnimeMorf (d_a_alink.cpp:7327-7342), with his human joint numbers.
static void held_upper2_morf(mDoExt_MtxCalcOldFrame* i_oldFrame, f32 i_morf, u16 i_jointNum) {
    if (i_oldFrame == NULL || !(i_morf > 0.0f)) {
        return;
    }
    if (i_oldFrame->getOldFrameRate() > 0.1f && i_oldFrame->getOldFrameStartJoint() == 0) {
        i_oldFrame->initOldFrameMorf(i_morf, 0, i_jointNum);
    } else {
        i_oldFrame->initOldFrameMorf(i_morf, l_upper2MorfStartJoint, l_upper2MorfEndJoint);
    }
}

void daRemotePlayer_c::clearUpper2Overlay(f32 i_morf) {
    // resetUpperAnime(UPPER_2, morf) (d_a_alink.cpp:7422-7440), minus everything that is not pose.
    mAnmPackUpper[2].setAnmTransform(NULL);
    mAnmPackUpper[2].setRatio(0.0f);
    mUpperFrameCtrl[2].init(0);
    mUpper2Shown = l_actNone;
    held_upper2_morf(mpOldFrame, i_morf, mBodyJointNum);
    mUpper2AppliedMorf = static_cast<u8>(i_morf > 0.0f ? (i_morf > 255.0f ? 255 : i_morf) : 0);
}

/**
 * Put the sender's UPPER_2 overlay into the puppet's upper slot 2, every tick. From execute(),
 * after selectAnimation() — whose setAnm() clears slots 1-2 — and before animePlay().
 *
 * The BCK loads into mActUpperHeap[2], which the action override builds and never uses: daAlink_c's
 * own slot for it, inside his own buffer layout (setUpperAnime, d_a_alink.cpp:7374, 0x2C00). That
 * layout overlaps under[0]'s 0x10800 read exactly as his does, so it inherits his guarantee that
 * the pairs he plays together fit — and the puppet only ever plays the pairs he sent together.
 *
 * The frame is the wire's, set on a controller with rate 0, so animePlay() steps it without moving
 * it — one driver per animation object (memory: J3D anms hold their own frame).
 */
void daRemotePlayer_c::applyUpper2Overlay() {
    const u16 want = mNetUpper2Idx;

    if (!mActHeapReady || want == l_actNone) {
        if (mUpper2Shown != l_actNone) {
            clearUpper2Overlay(mNetUpper2Morf);
        }
        if (want == l_actNone) {
            mUpper2Failed = l_actNone;
        } else if (mHeldLogCount < l_heldLogMax) {
            mHeldLogCount++;
            Log.debug("Puppet {} UPPER_2 overlay {} not played: the override heaps are not ready",
                mPlayerId, want);
        }
        return;
    }

    if (want != mUpper2Shown) {
        if (want == mUpper2Failed) {
            if (mUpper2Shown != l_actNone) {
                clearUpper2Overlay(mNetUpper2Morf);
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
                clearUpper2Overlay(mNetUpper2Morf);
            }
            if (mHeldLogCount < l_heldLogMax) {
                mHeldLogCount++;
                Log.warn("Puppet {} UPPER_2 overlay load failed idx={}", mPlayerId, want);
            }
            return;
        }

        if (mHeldLogCount < l_heldLogMax) {
            mHeldLogCount++;
            Log.debug("Puppet {} UPPER_2 overlay {} -> {} (morf {:.0f}, {})", mPlayerId,
                mUpper2Shown, want, mNetUpper2Morf, reloaded ? "loaded" : "resident");
        }
        mpUpper2Bck = bck;
        mUpper2Shown = want;
        mUpper2Failed = l_actNone;
        held_upper2_morf(mpOldFrame, mNetUpper2Morf, mBodyJointNum);
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
    mAnmPackUpper[2].setAnmTransform(mpUpper2Bck);
    mAnmPackUpper[2].setRatio(mNetUpper2Ratio);
    mUpperFrameCtrl[2].setFrameCtrl(
        mpUpper2Bck->getAttribute(), 0, static_cast<s16>(frameMax), 0.0f, frame);
    mpUpper2Bck->setFrame(frame);
}

f32 daRemotePlayer_c::getUpper2DrawnFrame() const {
    return mUpper2Shown != l_actNone && mpUpper2Bck != NULL ? mpUpper2Bck->getFrame() : 0.0f;
}

f32 daRemotePlayer_c::getUpper2DrawnRatio() {
    return mUpper2Shown != l_actNone ? mAnmPackUpper[2].getRatio() : 0.0f;
}
