/**
 * d_a_remote_player.cpp
 * Remote Player Puppet (Dusk multiplayer — not part of the original game)
 */

#include "d/dolzel_rel.h"  // IWYU pragma: keep

#include <cstring>

#include "JSystem/J3DGraphAnimator/J3DJoint.h"
#include "JSystem/J3DGraphLoader/J3DAnmLoader.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_remote_player.h"
#include "dusk/logging.h"
#include "f_op/f_op_actor_mng.h"
// AlAnm.h (the animation indices) arrives via d_a_alink.h. The per-outfit headers are deliberately
// NOT included: Bmdl.h and Kmdl.h define the same joint enums, so they cannot coexist in one
// translation unit. The body model is resolved by name below instead, which sidesteps that
// entirely and reads better than a pair of magic indices.

namespace {

aurora::Module Log{"dusk::mp"};

/* Two interchangeable body archives. Their skeletons are joint-for-joint identical (compare AL_JNT
 * in Kmdl.h with BL_JNT in Bmdl.h), so every AlAnm animation drives either one.
 */
const char l_bArcName[] = "Bmdl";  // Ordon / casual clothes
const char l_kArcName[] = "Kmdl";  // hero's clothes

const char l_bBodyResName[] = "bl.bmd";
const char l_kBodyResName[] = "al.bmd";

/* Link's animations do not live in the body archive — they are in AlAnm, mounted in ARAM at boot
 * and streamed by index.
 */
const u16 l_idleAnmIdx = dRes_ID_ALANM_BCK_WAITS_e;
const u16 l_walkAnmIdx = dRes_ID_ALANM_BCK_WALKS_e;
/* Pairs with WALKS in daAlink_c::m_anmDataTable (ANM_WALK / ANM_RUN, d_a_alink.cpp:301-302), so
 * this is the cycle the local player runs on — not a faster playback of the walk.
 */
const u16 l_runAnmIdx = dRes_ID_ALANM_BCK_DASHS_e;

/* Below this the puppet is standing still. Link's speedF is in units per tick. Separate from the
 * HIO rates below: this one is about network noise, not about gait.
 */
const f32 l_idleSpeedThreshold = 0.5f;

/* Widens the walk/run crossover into a dead band. daAlink_c does not need this because it blends
 * the two cycles continuously; we switch outright, and mNetSpeed is an interpolated value that
 * jitters, so without hysteresis a puppet held near the crossover flips gait every tick.
 */
const f32 l_gaitHysteresis = 0.05f;

/**
 * Pick a body archive the LOCAL player is not currently wearing.
 *
 * This is not cosmetic. daAlink_c installs joint callbacks on the shared J3DModelData
 * (d_a_alink_swindow.inc:171-173), and daAlink_modelCallBack dereferences
 * J3DModel::getUserArea() (d_a_alink.cpp:2449) — which is zero on any model we create. Calling
 * calc() on a model built from the archive Link is wearing is therefore an immediate null
 * dereference. Taking the other outfit sidesteps the whole problem.
 *
 * ★ This is a WORKAROUND, not a design, and it has a visible cost: the puppet can never wear the
 * same clothes as the local player, so on a save where both players would be in the hero's tunic
 * the remote one shows up in Ordon clothes. Confirmed on screen 2026-08-11. It reads as an
 * at-a-glance cue for which Link is you, but that is a consolation, not the reason — do not treat
 * the mismatch as intended behaviour.
 *
 * Proper fix (M3): give the puppet its OWN copy of the J3DModelData rather than sharing Link's, so
 * daAlink_c's joint callbacks and getUserArea() never apply to it. Outfit then becomes a
 * replicated property of the wearer instead of a collision-avoidance choice.
 */
const char* select_arc_name() {
    const daAlink_c* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (link != NULL && link->mArcName != NULL && std::strcmp(link->mArcName, l_bArcName) == 0) {
        return l_kArcName;
    }
    return l_bArcName;
}

const char* other_arc_name(const char* i_arcName) {
    return i_arcName == l_bArcName ? l_kArcName : l_bArcName;
}

J3DModelData* body_model_data(const char* i_arcName) {
    const char* resName = i_arcName == l_kArcName ? l_kBodyResName : l_bBodyResName;
    const int idx = dComIfG_getObjctResName2Index(i_arcName, resName);
    if (idx < 0) {
        return NULL;
    }
    return static_cast<J3DModelData*>(dComIfG_getObjectRes(i_arcName, static_cast<u16>(idx)));
}

/**
 * True when daAlink_c has installed its joint callbacks on this model data, i.e. the local player
 * is wearing this outfit. Those callbacks read J3DModel::getUserArea(), which is zero for any
 * model we create, so calling calc() on it is an immediate null dereference.
 *
 * ★ Only valid while Link is in the world. daAlink_c::initStatusWindow sets FLG2_STATUS_WINDOW_DRAW
 * and then calls changeModelDataDirect(0), which takes the branch that sets all 35 body callbacks
 * to NULL (d_a_alink_swindow.inc:195-197); resetStatusWindow puts them back (:368-375). So for as
 * long as the pause menu is open this reports "unclaimed" for the outfit the local player is
 * standing in. Do not use it as the ONLY guard against sharing — see ScopedJointIsolation below,
 * which does not care what state the callbacks are in.
 */
bool model_data_claimed(J3DModelData* i_modelData) {
    if (i_modelData == NULL || i_modelData->getJointNum() == 0) {
        return false;
    }
    return i_modelData->getJointNodePointer(0)->getCallBack() != NULL;
}

/**
 * Borrow a J3DModelData that another actor may also be using, for exactly one calc().
 *
 * A J3DModelData loaded from an archive is shared by every J3DModel built from it, and the joint
 * tree — which is where per-actor hooks get stored — belongs to the DATA, not to the model
 * (J3DModel has no joint array at all). daAlink_c parks two kinds of pointer-to-itself there:
 *
 *   - joint callbacks on joints 0..34 (d_a_alink_swindow.inc:171-173). J3DJoint::recursiveCalc
 *     fires these unconditionally (J3DJoint.cpp:218-221) — there is no per-model gate — and
 *     daAlink_modelCallBack immediately does `(daAlink_c*)j3dSys.getModel()->getUserArea()` with no
 *     null check (d_a_alink.cpp:2449), so it faults on any model that is not Link's.
 *   - mtx calculators on joints 0, 1 and 16 (swindow.inc:167-169), which are the local player's own
 *     animation blend tables. Those do not crash us; they would quietly drive OUR root and spine
 *     from the local player's animation.
 *
 * There is traffic in the other direction too, and it is the one that would be easy to miss:
 * mDoExt_McaMorfSO::modelCalc writes ITSELF onto joint 0 as the mtx calc every single frame
 * (m_Do_ext.cpp:1804). Sharing without restoring would therefore leave the local player's root
 * joint driven by a puppet's animation.
 *
 * So: save every joint hook, clear it, calc, put it back exactly as it was. The engine's own idiom
 * (mDoExt_bckAnm::entry, m_Do_ext.cpp:239-242) is already "write your state onto the shared joint
 * immediately before your calc"; this is that, with the restore that a second Link makes necessary.
 * d_a_e_bg.cpp:1177-1187 and d_a_e_oct_bg.cpp:212-219 do the same install/calc/uninstall bracket.
 *
 * Why not give the puppet its own J3DModelData instead? Because on PC the model loader byte-swaps
 * the archive buffer IN PLACE (J3DModelLoader.cpp:342-350 and :555, J3DShapeFactory.cpp:32-39, all
 * under TARGET_LITTLE_ENDIAN). J3DModelData::getRawData() hands back those already-swapped bytes,
 * so a second J3DModelLoaderDataBase::load on them would double-swap and destroy the original model
 * as well. A private copy means mounting a second archive, or making the endian fixups idempotent
 * inside libs/JSystem — a lot of blast radius, and neither buys anything this does not.
 *
 * RAII rather than a begin/end pair on purpose: leaving the local player's callbacks cleared
 * because something returned early would break LINK, several frames later and nowhere near here.
 */
class ScopedJointIsolation {
public:
    ScopedJointIsolation(J3DModelData* i_modelData, J3DJointCallBack* i_callBacks,
        J3DMtxCalc** i_mtxCalcs, u16 i_jointNum)
        : mpModelData(i_modelData), mpCallBacks(i_callBacks), mpMtxCalcs(i_mtxCalcs),
          mJointNum(i_jointNum) {
        if (mpModelData == NULL || mpCallBacks == NULL || mpMtxCalcs == NULL) {
            mJointNum = 0;
            return;
        }
        for (u16 i = 0; i < mJointNum; i++) {
            J3DJoint* joint = mpModelData->getJointNodePointer(i);
            mpCallBacks[i] = joint->getCallBack();
            mpMtxCalcs[i] = joint->getMtxCalc();
            joint->setCallBack(NULL);
            joint->setMtxCalc(NULL);
        }
    }

    ~ScopedJointIsolation() {
        for (u16 i = 0; i < mJointNum; i++) {
            J3DJoint* joint = mpModelData->getJointNodePointer(i);
            joint->setCallBack(mpCallBacks[i]);
            joint->setMtxCalc(mpMtxCalcs[i]);
        }
    }

private:
    J3DModelData* mpModelData;
    J3DJointCallBack* mpCallBacks;
    J3DMtxCalc** mpMtxCalcs;
    u16 mJointNum;
};

/**
 * Load one animation out of the ARAM archive.
 *
 * Sizing the staging buffer is the whole job here, and getting it wrong is not loud. `data_size` is
 * the size of the entry AS STORED, which for these animations is Yaz0-compressed — the expanded
 * animation is several times larger. Both archive backends clamp the read to the buffer they are
 * handed (JKRAramArchive rounds the destination DOWN to 0x20 and truncates the DMA; JKRMemArchive
 * clamps the Yaz0 expand size) and then report the clamped figure as the size read, so a small
 * buffer produces a silently truncated animation. J3DAnmLoaderDataBase::load parses that into an
 * object whose internals point at nothing, and the failure finally surfaces as an access violation
 * inside J3DJoint::recursiveCalc on the first calc() — a long way from the mistake.
 *
 * So: start from the stored size (exact when the entry is uncompressed), give compressed entries
 * room to expand, and treat "the read exactly filled the buffer" as the signature of a clamp
 * rather than of a lucky fit. daAlink_c avoids all of this by hardcoding a generous literal per
 * resource (0x400 to 0x6000 across d_a_alink*), which works only because someone checked each one.
 */
J3DAnmTransform* load_aram_anm(u16 i_resIdx) {
    JKRArchive* archive = dComIfGp_getAnmArchive();
    if (archive == NULL) {
        return NULL;
    }

    JKRArchive::SDIFileEntry* entry = archive->findIdxResource(i_resIdx);
    if (entry == NULL) {
        Log.warn("Animation resource {} not present in the ARAM archive", i_resIdx);
        return NULL;
    }

    const u32 storedSize = static_cast<u32>(entry->data_size);
    if (storedSize == 0) {
        return NULL;
    }

    const JKRCompression compression =
        JKRConvertAttrToCompressionType(entry->type_flags_and_name_offset >> 24);
    /* Typical Yaz0 ratio on animation data is 2-3x; 4x means the first attempt almost always fits
     * and the loop below is only a backstop. */
    const u32 headroom = compression == COMPRESSION_NONE ? 1 : 4;
    u32 size = ALIGN_NEXT(storedSize * headroom, 0x20);

    for (int attempt = 0; attempt < 4; attempt++) {
        u8* buffer = JKR_NEW_ARRAY_ARGS(u8, size, 0x20);
        if (buffer == NULL) {
            Log.warn("Out of heap for animation {} ({} bytes)", i_resIdx, size);
            return NULL;
        }

        const u32 read = JKRReadIdxResource(buffer, size, i_resIdx, archive);
        if (read == 0) {
            Log.warn("Animation {} read nothing into a {}-byte buffer", i_resIdx, size);
            return NULL;
        }

        if (read >= size) {
            // Exactly filled: assume it was clamped and there is more animation we did not get.
            size *= 2;
            continue;
        }

        // Compared byte-wise rather than as a u32 so this does not depend on which way round the
        // header is on this platform.
        if (buffer[0] != 'J' || buffer[1] != '3' || buffer[2] != 'D' || buffer[3] != '1') {
            Log.warn("Animation {} is not a J3D binary after reading {} bytes", i_resIdx, read);
            return NULL;
        }

        J3DAnmTransform* anm = static_cast<J3DAnmTransform*>(J3DAnmLoaderDataBase::load(buffer));
        if (anm == NULL) {
            Log.warn("J3D loader rejected animation {} ({} bytes)", i_resIdx, read);
            return NULL;
        }

        Log.info("Loaded animation {}: {} stored -> {} expanded, {}-byte buffer", i_resIdx,
            storedSize, read, size);
        return anm;
    }

    Log.warn("Animation {} kept filling the buffer up to {} bytes; giving up", i_resIdx, size);
    return NULL;
}

}  // namespace

int daRemotePlayer_c::createHeap() {
    J3DModelData* modelData = body_model_data(mArcName);
    if (modelData == NULL) {
        return 0;
    }

    mpIdleAnm = load_aram_anm(l_idleAnmIdx);
    mpWalkAnm = load_aram_anm(l_walkAnmIdx);
    mpRunAnm = load_aram_anm(l_runAnmIdx);
    if (mpIdleAnm == NULL || mpWalkAnm == NULL || mpRunAnm == NULL) {
        return 0;
    }

    mpModelMorf = JKR_NEW mDoExt_McaMorfSO(
        modelData, NULL, NULL, mpIdleAnm, J3DFrameCtrl::EMode_LOOP, 1.0f, 0, -1, NULL, 0, 0);
    if (mpModelMorf == NULL || mpModelMorf->getModel() == NULL) {
        return 0;
    }

    // Sized from the model rather than from daAlink_c's hardcoded 35 (d_a_alink_swindow.inc:171):
    // the wolf skeleton runs to 40, and a bound that is right for one outfit and short for another
    // would leave joints un-isolated, which is precisely the crash this exists to prevent.
    mJointNum = modelData->getJointNum();
    if (mJointNum != 0) {
        mpSavedCallBacks = JKR_NEW_ARRAY(J3DJointCallBack, mJointNum);
        mpSavedMtxCalcs = JKR_NEW_ARRAY(J3DMtxCalc*, mJointNum);
        if (mpSavedCallBacks == NULL || mpSavedMtxCalcs == NULL) {
            return 0;
        }
    }

    // What is LEFT, not what was asked for. Adding the run animation grew the animation footprint
    // from ~20 KB to ~35 KB inside a fixed 0x20000 solid heap, and a heap that is merely nearly
    // full does not announce itself: every allocation here still succeeds and the damage, if any,
    // shows up later and somewhere else. One line makes the margin a number instead of a guess.
    JKRHeap* heap = JKRGetCurrentHeap();
    if (heap != NULL) {
        Log.info("Puppet heap after setup: {} bytes free, largest block {}", heap->getFreeSize(),
            heap->getMaxAllocatableSize(0x20));
    }

    return 1;
}

static int daRemotePlayer_createHeap(fopAc_ac_c* i_this) {
    return static_cast<daRemotePlayer_c*>(i_this)->createHeap();
}

int daRemotePlayer_c::create() {
    fopAcM_ct(this, daRemotePlayer_c);

    if (mArcName == NULL) {
        mArcName = select_arc_name();
    }

    int phase = dComIfG_resLoad(&mPhaseReq, mArcName);
    if (phase != cPhs_COMPLEATE_e) {
        return phase;
    }

    // VERIFY the archive choice rather than trusting it. select_arc_name() reads the local Link's
    // mArcName, which is only a hint — during the intro demo it can be unset or stale while Link is
    // in fact wearing that outfit. Confirm against the model data itself, and switch once if we
    // guessed wrong. Getting this wrong is not cosmetic: it is an access violation on the first
    // calc().
    if (!mSwitchedArc && model_data_claimed(body_model_data(mArcName))) {
        const char* rejected = mArcName;
        mArcName = other_arc_name(rejected);
        mSwitchedArc = true;
        dComIfG_resDelete(&mPhaseReq, rejected);
        cPhs_Reset(&mPhaseReq);
        Log.info("Archive '{}' belongs to the local player; puppet switching to '{}'", rejected,
            mArcName);
        return cPhs_INIT_e;
    }

    if (!fopAcM_entrySolidHeap(this, daRemotePlayer_createHeap, 0x20000)) {
        // Either the heap estimate was too small or a resource was missing. Say so: a silent
        // cPhs_ERROR_e here surfaces later as a puppet that simply never appears.
        Log.warn("Puppet heap/model setup failed for the remote player actor");
        return cPhs_ERROR_e;
    }

    mPlayerId = fopAcM_GetParam(this);
    mCurrentAnm = l_idleAnmIdx;
    model = mpModelMorf->getModel();

    // Deliberately NOT calling setMatrix() here. It ends in modelCalc(), and calc'ing before the
    // first network pose has arrived is both pointless (we'd be posing at the spawn point) and the
    // exact place the original crash happened.
    Log.info("Puppet for player {} using body archive '{}'", mPlayerId, mArcName);
    return cPhs_COMPLEATE_e;
}

static int daRemotePlayer_Create(fopAc_ac_c* i_this) {
    return static_cast<daRemotePlayer_c*>(i_this)->create();
}

daRemotePlayer_c::~daRemotePlayer_c() {
    if (mArcName != NULL) {
        dComIfG_resDelete(&mPhaseReq, mArcName);
    }
}

static int daRemotePlayer_Delete(daRemotePlayer_c* i_this) {
    i_this->~daRemotePlayer_c();
    return 1;
}

void daRemotePlayer_c::setNetworkPose(const cXyz& i_pos, s16 i_angleY, f32 i_speed) {
    current.pos = i_pos;
    shape_angle.y = i_angleY;
    // The logical angle is kept in step so anything that reads current.angle (audio, effects) sees
    // a sane value, even though only shape_angle drives the model matrix.
    current.angle.y = i_angleY;
    mNetSpeed = i_speed;
    mHasPose = true;
}

bool daRemotePlayer_c::modelDataOwnedByPlayer() const {
    if (mpModelMorf == NULL || mpModelMorf->getModel() == NULL) {
        return true;
    }
    return model_data_claimed(mpModelMorf->getModel()->getModelData());
}

/**
 * Choose the gait from the replicated speed, the way the local player chooses it.
 *
 * daAlink_c normalises speedF against its top ground speed and crosses over at two fixed fractions
 * (d_a_alink.cpp:7653/7784): wait blends into walk below mWalkChangeRate, walk into run below
 * mRunChangeRate, run alone above it. Each cycle plays at its own authored rate — the game never
 * speeds a walk up to stand in for a run.
 *
 * Those numbers are read from daAlinkHIO_move_c0::m rather than copied, so the puppet cannot drift
 * out of step with the player if the table is ever corrected.
 *
 * ★ What we do NOT reproduce is the blend. daAlink_c drives two animations at once through
 * daPy_frameCtrl_c pairs (commonDoubleAnime, m_Do_ext.cpp has the same idea in mDoExt_McaMorf2) and
 * cross-fades by weight; mDoExt_McaMorfSO holds a single animation, so we quantise to whichever
 * side of the blend is dominant and morf across the switch. Visible difference is confined to the
 * walk/run transition; the endpoints match the player exactly. Proper blending is M3, alongside
 * real action states.
 */
void daRemotePlayer_c::selectAnimation() {
    const daAlinkHIO_move_c1& hio = daAlinkHIO_move_c0::m;

    // Midpoint of the band daAlink_c cross-fades over, i.e. where its blend weight passes 0.5.
    const f32 runFraction = 0.5f * (hio.mWalkChangeRate + hio.mRunChangeRate);
    const f32 fraction = mNetSpeed / hio.mMaxSpeed;

    u16 wanted;
    if (mNetSpeed <= l_idleSpeedThreshold) {
        wanted = l_idleAnmIdx;
    } else if (mCurrentAnm == l_runAnmIdx) {
        wanted = fraction < runFraction - l_gaitHysteresis ? l_walkAnmIdx : l_runAnmIdx;
    } else {
        wanted = fraction > runFraction + l_gaitHysteresis ? l_runAnmIdx : l_walkAnmIdx;
    }

    J3DAnmTransform* anm;
    f32 rate;
    if (wanted == l_runAnmIdx) {
        anm = mpRunAnm;
        rate = hio.mRunAnmSpeed;
    } else if (wanted == l_walkAnmIdx) {
        anm = mpWalkAnm;
        rate = hio.mWalkAnmSpeed;
    } else {
        anm = mpIdleAnm;
        rate = hio.mWaitAnmSpeed;
    }

    if (wanted != mCurrentAnm) {
        // A short morf, so changing gait doesn't pop. This is the one place we are standing in for
        // the player's cross-fade, so it is doing more work here than a plain animation change.
        mpModelMorf->setAnm(anm, J3DFrameCtrl::EMode_LOOP, 5.0f, rate, 0.0f, -1.0f);
        mCurrentAnm = wanted;
    } else {
        mpModelMorf->setPlaySpeed(rate);
    }
}

void daRemotePlayer_c::setMatrix() {
    mDoMtx_stack_c::transS(current.pos);
    mDoMtx_stack_c::YrotM(shape_angle.y);
    model->setBaseTRMtx(mDoMtx_stack_c::get());
    mpModelMorf->modelCalc();
}

int daRemotePlayer_c::execute() {
    if (!mHasPose) {
        // No network pose yet. Skipping calc keeps the puppet from flashing at its spawn point.
        return 1;
    }

    if (!mLoggedFirstCalc) {
        mLoggedFirstCalc = true;
        // A bad animation object does not announce itself here — it surfaces as an access violation
        // on mpAnm's vtable several frames deep inside J3DJoint::recursiveCalc, with no hint of
        // which pointer was wrong. One line before the first calc() makes that diagnosable from a
        // log alone. The two animations are both J3DAnmTransformKey, so their vtable pointers must
        // match each other and must look like an address in the executable.
        Log.debug("Puppet {} first calc: morf={:#x} model={:#x} idle={:#x}/{:#x} walk={:#x}/{:#x} "
                  "run={:#x}/{:#x}",
            mPlayerId, reinterpret_cast<uintptr_t>(mpModelMorf), reinterpret_cast<uintptr_t>(model),
            reinterpret_cast<uintptr_t>(mpIdleAnm),
            mpIdleAnm != NULL ? *reinterpret_cast<const uintptr_t*>(mpIdleAnm) : 0,
            reinterpret_cast<uintptr_t>(mpWalkAnm),
            mpWalkAnm != NULL ? *reinterpret_cast<const uintptr_t*>(mpWalkAnm) : 0,
            reinterpret_cast<uintptr_t>(mpRunAnm),
            mpRunAnm != NULL ? *reinterpret_cast<const uintptr_t*>(mpRunAnm) : 0);
    }

    if (modelDataOwnedByPlayer()) {
        // The local player changed clothes into our archive. Calling calc() now would dereference
        // a null user area, so stand down until they change back rather than take the crash.
        if (!mReportedArcConflict) {
            Log.warn(
                "Player {} puppet hidden: local Link took over archive '{}'", mPlayerId, mArcName);
            mReportedArcConflict = true;
        }
        return 1;
    }
    mReportedArcConflict = false;

    selectAnimation();
    mpModelMorf->play(0, 0);
    {
        // Only setMatrix() needs the bracket: it is the one that reaches modelCalc(), and neither
        // selectAnimation() nor play() writes to the model data (mDoExt_McaMorfSO::setAnm and
        // ::play touch only their own frame controller, m_Do_ext.cpp:1721 and :1765).
        ScopedJointIsolation isolation(
            model->getModelData(), mpSavedCallBacks, mpSavedMtxCalcs, mJointNum);
        setMatrix();
    }
    return 1;
}

static int daRemotePlayer_Execute(daRemotePlayer_c* i_this) {
    return i_this->execute();
}

int daRemotePlayer_c::draw() {
    if (!mHasPose || modelDataOwnedByPlayer()) {
        return 1;
    }

    // 10 is the light type daAlink_c uses for human Link (d_a_alink.cpp:19468), so the puppet is
    // lit consistently with the local player rather than as scenery.
    g_env_light.settingTevStruct(10, &current.pos, &tevStr);
    g_env_light.setLightTevColorType_MAJI(model, &tevStr);
    mDoExt_modelEntryDL(model);
    return 1;
}

static int daRemotePlayer_Draw(daRemotePlayer_c* i_this) {
    return i_this->draw();
}

static DUSK_CONST actor_method_class l_daRemotePlayer_Method = {
    (process_method_func)daRemotePlayer_Create,
    (process_method_func)daRemotePlayer_Delete,
    (process_method_func)daRemotePlayer_Execute,
    (process_method_func)NULL,
    (process_method_func)daRemotePlayer_Draw,
};

DUSK_PROFILE actor_process_profile_definition DUSK_CONST g_profile_REMOTE_PLAYER = {
    /* Layer ID     */ fpcLy_CURRENT_e,
    /* List ID      */ 7,
    /* List Prio    */ fpcPi_CURRENT_e,
    /* Proc Name    */ fpcNm_REMOTE_PLAYER_e,
    /* Proc SubMtd  */ &g_fpcLf_Method.base,
    /* Size         */ sizeof(daRemotePlayer_c),
    /* Size Other   */ 0,
    /* Parameters   */ 0,
    /* Leaf SubMtd  */ &g_fopAc_Method.base,
    /* Draw Prio    */ fpcDwPi_ALINK_e,
    /* Actor SubMtd */ &l_daRemotePlayer_Method,
    /* Status       */ fopAcStts_UNK_0x40000_e | fopAcStts_NOPAUSE_e,
    /* Group        */ fopAc_ACTOR_e,
    /* Cull Type    */ fopAc_CULLBOX_0_e,
};
