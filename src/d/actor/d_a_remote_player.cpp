/**
 * d_a_remote_player.cpp
 * Remote Player Puppet (Dusk multiplayer — not part of the original game)
 */

#include "d/dolzel_rel.h"  // IWYU pragma: keep

#include <cstring>

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

/* Below this the puppet is standing still. Link's speedF is in units per tick. */
const f32 l_idleSpeedThreshold = 0.5f;
/* speedF at which the walk cycle plays at its authored rate; faster input speeds it up. */
const f32 l_walkReferenceSpeed = 6.0f;
const f32 l_minAnmRate = 0.5f;
const f32 l_maxAnmRate = 2.5f;

/**
 * Pick a body archive the LOCAL player is not currently wearing.
 *
 * This is not cosmetic. daAlink_c installs joint callbacks on the shared J3DModelData
 * (d_a_alink_swindow.inc:171-173), and daAlink_modelCallBack dereferences
 * J3DModel::getUserArea() (d_a_alink.cpp:2449) — which is zero on any model we create. Calling
 * calc() on a model built from the archive Link is wearing is therefore an immediate null
 * dereference. Taking the other outfit sidesteps the whole problem, and doubles as an at-a-glance
 * cue for which Link is you.
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
 */
bool model_data_claimed(J3DModelData* i_modelData) {
    if (i_modelData == NULL || i_modelData->getJointNum() == 0) {
        return false;
    }
    return i_modelData->getJointNodePointer(0)->getCallBack() != NULL;
}

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
    if (mpIdleAnm == NULL || mpWalkAnm == NULL) {
        return 0;
    }

    mpModelMorf = JKR_NEW mDoExt_McaMorfSO(
        modelData, NULL, NULL, mpIdleAnm, J3DFrameCtrl::EMode_LOOP, 1.0f, 0, -1, NULL, 0, 0);
    if (mpModelMorf == NULL || mpModelMorf->getModel() == NULL) {
        return 0;
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

void daRemotePlayer_c::selectAnimation() {
    const bool moving = mNetSpeed > l_idleSpeedThreshold;
    const u16 wanted = moving ? l_walkAnmIdx : l_idleAnmIdx;

    f32 rate = 1.0f;
    if (moving) {
        // Rate-scale the walk cycle by the replicated speed so the feet roughly match the ground
        // the puppet is covering. Coarse by design — real action states are M3.
        rate = mNetSpeed / l_walkReferenceSpeed;
        if (rate < l_minAnmRate) {
            rate = l_minAnmRate;
        } else if (rate > l_maxAnmRate) {
            rate = l_maxAnmRate;
        }
    }

    if (wanted != mCurrentAnm) {
        // A short blend, so switching between idle and walk doesn't pop.
        mpModelMorf->setAnm(
            moving ? mpWalkAnm : mpIdleAnm, J3DFrameCtrl::EMode_LOOP, 5.0f, rate, 0.0f, -1.0f);
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
        Log.debug("Puppet {} first calc: morf={:#x} model={:#x} idle={:#x}/{:#x} walk={:#x}/{:#x}",
            mPlayerId, reinterpret_cast<uintptr_t>(mpModelMorf), reinterpret_cast<uintptr_t>(model),
            reinterpret_cast<uintptr_t>(mpIdleAnm),
            mpIdleAnm != NULL ? *reinterpret_cast<const uintptr_t*>(mpIdleAnm) : 0,
            reinterpret_cast<uintptr_t>(mpWalkAnm),
            mpWalkAnm != NULL ? *reinterpret_cast<const uintptr_t*>(mpWalkAnm) : 0);
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
    setMatrix();
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
