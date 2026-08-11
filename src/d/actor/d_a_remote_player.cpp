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

/* Every human outfit daAlink_c::setArcName can select (d_a_alink_swindow.inc:14-26), with the body
 * model inside each. The skeletons are joint-for-joint identical across all of them (compare AL_JNT
 * in Kmdl.h with BL_JNT in Bmdl.h), so every AlAnm animation drives any of them.
 *
 * Wmdl — wolf — is deliberately absent. It has a different skeleton and a different animation set,
 * and transform is per-player by policy (mp_policy.hpp), so a puppet whose owner is a wolf needs a
 * genuinely different model rather than a different entry here. Until that exists we fall back to
 * the hero's clothes; see outfit_arc_name().
 */
struct OutfitArc {
    const char* arcName;
    const char* bodyResName;
    const char* headResName;
    const char* handsResName;
    const char* faceResName;
};

/* Link is four models, not one — body, head, hands and face — and the outfits do not name them
 * consistently (Zora and magic armour reuse al_hands.bmd; only Zora has its own face). Taken from
 * the outfit branches of daAlink_c::setLinkModel, d_a_alink_wolf.inc:328-377.
 */
const OutfitArc l_outfits[] = {
    // arc      body       head            hands            face
    {"Bmdl", "bl.bmd", "bl_head.bmd", "bl_hands.bmd", "al_face.bmd"},  // Ordon / casual clothes
    {"Kmdl", "al.bmd", "al_head.bmd", "al_hands.bmd", "al_face.bmd"},  // hero's clothes
    {"Zmdl", "zl.bmd", "zl_head.bmd", "al_hands.bmd", "zl_face.bmd"},  // Zora armour
    {"Mmdl", "ml.bmd", "ml_head.bmd", "al_hands.bmd", "al_face.bmd"},  // magic armour
};

const int l_outfitNum = sizeof(l_outfits) / sizeof(l_outfits[0]);

/* Index into l_outfits used when we have nothing better — the hero's clothes. */
const int l_defaultOutfit = 1;

/* Where object archives live on disc. dRes_info_c::set builds "<path><name>.arc" from these
 * (d_resorce.cpp:58-62); it is the same string dRes_control_c::setObjectRes passes
 * (d_resorce.h:96).
 */
const char l_objectPath[] = "/res/Object/";

/* Body joints the sub-models hang off. daAlink_c uses these same three literals: the head and face
 * both ride joint 4 (d_a_alink.cpp:5968-5970) and the hands model's own joints 1 and 2 are
 * overwritten with the body's hand joints after its calc (d_a_alink.cpp:19013-19014).
 */
const u16 l_headJointNo = 4;
const u16 l_leftHandJointNo = 9;
const u16 l_rightHandJointNo = 0xE;

/* The hands model holds ELEVEN alternative hand poses as separate shapes and shows exactly one per
 * hand (daAlink_c::setDrawHand, d_a_alink.cpp:18928-19057); draw them all and Link sprouts a
 * bouquet of hands. daAlink_c picks per frame from what he is holding. A puppet has no replicated
 * equipment yet, so it takes the pair the pause menu uses as its neutral
 * (d_a_alink.cpp:18936-18946) and holds them. Item-dependent hands arrive with equipment
 * replication.
 */
const u16 l_handShapeNum = 11;
const u16 l_leftHandShape = 0;
const u16 l_rightHandShape = 6;

/* Matches daAlink_c::initModel (d_a_alink.h:3732 -> d_a_alink.cpp:4131), minus the warp-material
 * branch, which only fires for the midna-warp texture and cannot apply to these.
 */
J3DModel* init_model(J3DModelData* i_modelData, u32 i_diffFlags) {
    if (i_modelData == NULL) {
        return NULL;
    }
    return mDoExt_J3DModel__create(i_modelData, 0x80000, i_diffFlags | 0x11000084);
}

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
 * Which outfit archive a puppet should wear.
 *
 * ★ Placeholder: this currently mirrors the LOCAL player. Stuart's rule (2026-08-12) is that
 * appearance is owned by the wearer — whatever P2 is wearing on their own screen is what P1 should
 * see — so this becomes a replicated property of the sender. It is not one yet; PlayerState does
 * not carry the outfit. Matching the local player is the right placeholder because it is correct in
 * the common case (both players in the same clothes) and, unlike the arrangement it replaces, it no
 * longer deliberately shows the WRONG clothes.
 *
 * Wolf is not an outfit swap. Wmdl has its own skeleton and animation set, so a wolf owner falls
 * back to the hero's clothes and looks like a human Link until transform replication exists.
 */
int outfit_index_for_local_player() {
    const daAlink_c* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (link != NULL && link->mArcName != NULL) {
        for (int i = 0; i < l_outfitNum; i++) {
            if (std::strcmp(link->mArcName, l_outfits[i].arcName) == 0) {
                return i;
            }
        }
    }
    return l_defaultOutfit;
}

/**
 * Fetch a resource out of a PRIVATELY mounted archive.
 *
 * dComIfG_getObjectRes cannot be used here: it looks the name up in the global 128-slot resource
 * table, which is exactly the shared entry we are avoiding. This is the same two steps
 * dRes_control_c::getRes performs once it has the dRes_info_c in hand (d_resorce.cpp:928-940).
 */
void* own_archive_res(dRes_info_c& i_res, const char* i_resName) {
    JKRArchive* archive = i_res.getArchive();
    if (archive == NULL) {
        return NULL;
    }

    JKRArchive::SDIFileEntry* entry = archive->findNameResource(i_resName);
    if (entry == NULL) {
        Log.warn("Resource '{}' is not in the puppet's private archive", i_resName);
        return NULL;
    }

    return i_res.getRes(entry - archive->mFiles);
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
    J3DModelData* modelData =
        static_cast<J3DModelData*>(own_archive_res(mOwnRes, l_outfits[mOutfit].bodyResName));
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

    /* Head, hands and face. Without these the puppet is a headless, handless body — which is
     * exactly what Stuart saw ("only ears") before this, because the archive carries them but
     * nothing attached them. They are separate models posed off the body's joints every frame in
     * setMatrix(), not extra geometry on the body skeleton.
     *
     * A missing sub-model is deliberately NOT fatal: an outfit whose head resource is named
     * differently should cost a head, not the whole puppet. own_archive_res() already logs which
     * name was missing. */
    const OutfitArc& outfit = l_outfits[mOutfit];
    mpHeadModel =
        init_model(static_cast<J3DModelData*>(own_archive_res(mOwnRes, outfit.headResName)), 0);
    mpHandModel =
        init_model(static_cast<J3DModelData*>(own_archive_res(mOwnRes, outfit.handsResName)), 0);
    mpFaceModel = init_model(
        static_cast<J3DModelData*>(own_archive_res(mOwnRes, outfit.faceResName)), 0x20200);

    /* Dusk already fixes Link's eyes vanishing on PC by clamping maxLOD on three face textures
     * (d_a_alink_wolf.inc:379-395). That fix is applied to the local player's face model data, and
     * our private mount is a different copy, so it has to be applied again here or puppets get the
     * bug the local player no longer has. Safe to write: nothing else shares these textures.
     */
    if (mpFaceModel != NULL) {
        J3DModelData* faceData = mpFaceModel->getModelData();
        J3DTexture* tex = faceData->getTexture();
        JUTNameTab* nameTab = faceData->getTextureName();
        if (tex != NULL && nameTab != NULL) {
            for (u16 i = 0; i < tex->getNum(); i++) {
                const char* texName = nameTab->getName(i);
                if (texName != NULL && (std::strcmp(texName, "al_eyeball") == 0 ||
                                           std::strcmp(texName, "highlight02") == 0 ||
                                           std::strcmp(texName, "eye_kage01") == 0))
                {
                    tex->getResTIMG(i)->maxLOD = 0;
                }
            }
        }
    }

    if (mpHandModel != NULL) {
        J3DModelData* handData = mpHandModel->getModelData();
        const u16 shapeNum = handData->getMaterialNum() < l_handShapeNum ?
                                 handData->getMaterialNum() :
                                 l_handShapeNum;
        for (u16 i = 0; i < shapeNum; i++) {
            handData->getMaterialNodePointer(i)->getShape()->hide();
        }
        if (l_leftHandShape < shapeNum) {
            handData->getMaterialNodePointer(l_leftHandShape)->getShape()->show();
        }
        if (l_rightHandShape < shapeNum) {
            handData->getMaterialNodePointer(l_rightHandShape)->getShape()->show();
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

/**
 * Mount this puppet's OWN copy of the outfit archive.
 *
 * ★ Deliberately NOT dComIfG_resLoad. That registers against a global, name-keyed,
 * reference-counted table (dRes_control_c::setRes, d_resorce.cpp:793-816), so a puppet asking for
 * the outfit the local player is wearing gets a second reference to Link's ONE archive. Two things
 * then go wrong, and both are fatal rather than cosmetic:
 *
 *   - The joint tree is shared, so daAlink_c's per-actor callbacks and mtx calculators apply to our
 *     model too (see the note on the M3 fix in 00-status.md).
 *   - Worse, the lifetime is not actually shared. Link loads his outfit into his own heap
 *     (d_a_alink.cpp:4971), and on a clothes change he calls dComIfG_resDelete and then
 *     mpArcHeap->freeAll() two lines later (d_a_alink_swindow.inc:80-87). freeAll knows nothing
 *     about the reference count — it wipes the heap regardless — so changing clothes would free the
 *     archive out from under any puppet still pointing into it. Note that setRes only honours the
 *     caller's heap for the FIRST loader (d_resorce.cpp:807 sits inside the `resInfo == NULL`
 *     branch), so a puppet cannot opt out by passing its own heap either.
 *
 * A private dRes_info_c sidesteps all of it: our own mount, our own J3DModelData with a clean joint
 * tree, our own lifetime, destroyed with the actor. It is the same machinery the global table uses
 * per entry — set() starts the async mount, setRes() polls it and then runs loadResource() — just
 * not registered anywhere, so nothing else can find it, share it, or free it.
 *
 * Cost is one extra archive per remote player (Link budgets 0xA2800 for his, d_a_alink.cpp:4969).
 * Passing NULL for the heap puts it where the puppet's archive already went before this change: the
 * global archive heap, which nothing ever calls freeAll on.
 */
int daRemotePlayer_c::mountOwnArchive() {
    if (!mResRequested) {
        if (!mOwnRes.set(
                l_outfits[mOutfit].arcName, l_objectPath, mDoDvd_MOUNT_DIRECTION_HEAD, NULL))
        {
            Log.warn("Puppet could not start a private mount of '{}'", l_outfits[mOutfit].arcName);
            return cPhs_ERROR_e;
        }
        mResRequested = true;
    }

    const int resState = mOwnRes.setRes();
    if (resState > 0) {
        return cPhs_LOADING_e;
    }
    if (resState < 0) {
        Log.warn("Puppet's private mount of '{}' failed", l_outfits[mOutfit].arcName);
        return cPhs_ERROR_e;
    }

    return cPhs_COMPLEATE_e;
}

int daRemotePlayer_c::create() {
    fopAcM_ct(this, daRemotePlayer_c);

    if (!mOutfitChosen) {
        mOutfit = outfit_index_for_local_player();
        mOutfitChosen = true;
    }

    const int mountPhase = mountOwnArchive();
    if (mountPhase != cPhs_COMPLEATE_e) {
        return mountPhase;
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
    Log.info("Puppet for player {} wearing '{}' from its own archive copy", mPlayerId,
        l_outfits[mOutfit].arcName);
    return cPhs_COMPLEATE_e;
}

static int daRemotePlayer_Create(fopAc_ac_c* i_this) {
    return static_cast<daRemotePlayer_c*>(i_this)->create();
}

daRemotePlayer_c::~daRemotePlayer_c() {
    // Nothing to release by hand. mOwnRes is a member, so ~dRes_info_c runs after this body and
    // unmounts the archive and frees its resources — the whole reason the private mount is a member
    // rather than a registration in the global table.
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

    /* Sub-models ride the body's joints, so they must be posed AFTER the body's calc. The head and
     * face take the head joint's matrix as their whole base transform; the hands take the body's
     * base transform and then have their own two joints overwritten with the body's hand joints
     * (daAlink_c does the same three things, d_a_alink.cpp:5968-5980 and :19007-19014). Note the
     * hand fix-up has to come after the hand model's own calc(), not before, or calc overwrites it.
     */
    if (mpHeadModel != NULL) {
        mpHeadModel->setBaseTRMtx(model->getAnmMtx(l_headJointNo));
        mpHeadModel->calc();
    }

    if (mpFaceModel != NULL) {
        mpFaceModel->setBaseTRMtx(model->getAnmMtx(l_headJointNo));
        mpFaceModel->calc();
    }

    if (mpHandModel != NULL) {
        mpHandModel->setBaseTRMtx(model->getBaseTRMtx());
        mpHandModel->calc();
        mpHandModel->setAnmMtx(1, model->getAnmMtx(l_leftHandJointNo));
        mpHandModel->setAnmMtx(2, model->getAnmMtx(l_rightHandJointNo));
    }
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

    // No guard against the local player owning this model data, and none needed: the puppet's
    // J3DModelData came out of its own private archive mount, so daAlink_c's joint callbacks and
    // mtx calculators are not on it and cannot be.
    selectAnimation();
    mpModelMorf->play(0, 0);
    setMatrix();
    return 1;
}

static int daRemotePlayer_Execute(daRemotePlayer_c* i_this) {
    return i_this->execute();
}

/// One sub-model, lit like the body. Mirrors daAlink_c::basicModelDraw (d_a_alink.cpp:19370).
void daRemotePlayer_c::drawModel(J3DModel* i_model) {
    if (i_model == NULL) {
        return;
    }
    g_env_light.setLightTevColorType_MAJI(i_model, &tevStr);
    mDoExt_modelEntryDL(i_model);
}

int daRemotePlayer_c::draw() {
    if (!mHasPose) {
        return 1;
    }

    // 10 is the light type daAlink_c uses for human Link (d_a_alink.cpp:19468), so the puppet is
    // lit consistently with the local player rather than as scenery.
    g_env_light.settingTevStruct(10, &current.pos, &tevStr);
    drawModel(model);
    // Same order daAlink_c draws them in, which matters for the face: it is drawn after the head so
    // it wins the depth fight at the eyes rather than being buried inside it.
    drawModel(mpHeadModel);
    drawModel(mpFaceModel);
    drawModel(mpHandModel);
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
