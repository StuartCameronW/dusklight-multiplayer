/**
 * d_a_remote_player.cpp
 * Remote Player Puppet (Dusk multiplayer — not part of the original game)
 */

#include "d/dolzel_rel.h"  // IWYU pragma: keep

#include <cmath>
#include <cstring>

#include "JSystem/J3DGraphAnimator/J3DJoint.h"
#include "JSystem/J3DGraphAnimator/J3DMaterialAnm.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphLoader/J3DAnmLoader.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_mirror.h"
#include "d/actor/d_a_npc4.h"
#include "d/actor/d_a_remote_player.h"
#include "d/d_bg_s.h"
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

/**
 * Matches daAlink_c::initModel (d_a_alink.cpp:4105-4137), warp-material branch included.
 *
 * ★ An earlier version of this skipped that branch with the comment "only fires for the midna-warp
 * texture and cannot apply to these". That was an ASSUMPTION, not a finding, and it was wrong in
 * the way that matters: the puppet loads the SAME BMDs the local player does, so whatever is true
 * of his models is true of ours. The check is a pointer comparison against the shared warp texture
 * in the Always archive, and if it matches, the model must be built with one extra TEV stage and
 * texgen and with its own copies of both blocks (0x2000400) — otherwise the puppet's materials are
 * shaped differently from the local player's for the same mesh.
 *
 * The on/off pair brackets creation on purpose: onWarpMaterial bumps the counts on the shared model
 * DATA, mDoExt_J3DModel__create copies them into the model's own blocks, and offWarpMaterial puts
 * the data back. Skipping the restore would leave the count raised for anything else using that
 * data, so the pairing is not optional.
 *
 * i_warpTexData comes from the local player rather than from the Always archive directly, because
 * dRes_ID_ALWAYS_BTI_WARP_TEX_e lives in a per-region header that cannot be included here without
 * colliding with the outfit headers. It is the identical pointer — daAlink_c::createHeap builds it
 * the same way (d_a_alink.cpp:4183-4184) — and NULL simply means "no local player yet", in which
 * case the branch cannot fire and we behave as before.
 */
/**
 * Does this model carry the twilight-dissolve material, and is it currently ON?
 *
 * ★ THIS IS THE A5 FIX, and the mistake it replaces is worth spelling out because the whole bug
 * came from reading `addWarpMaterial` too quickly.
 *
 * `addWarpMaterial` does not merely *append* a dormant stage — it **raises both counts**
 * (`d_resorce.cpp:157`, `:167`). So every `BMWR` model, which is all four of Link's
 * (`Kmdl.h`), arrives out of the loader with the dissolve **already enabled**. That reframes
 * `daAlink_c::initModel`'s bracket completely: `onWarpMaterial` hits its "already on" `break` and
 * does nothing, and the real work is the **trailing `offWarpMaterial`, which turns the dissolve
 * OFF**. Turning it off is not cleanup after the model is built; it is the entire point.
 *
 * The puppet used to decide whether to run that bracket by comparing the model's last texture
 * pointer against `daAlink_c::mpWarpTexData`, copying the shape of `initModel`. Measured on
 * 2026-08-12, that comparison never matched for the puppet's privately-mounted copies, so the
 * bracket never ran, so the dissolve was never switched off — and all four models drew it. Its UVs
 * come from world position through a camera-dependent matrix nothing drives for a puppet
 * (`d_resorce.cpp:212-225`), and the same function hard-cuts any pixel at or below alpha 0x80. That
 * is precisely Stuart's *"turns into warp particles (then disappears)... then when he walks away
 * its fine again"*.
 *
 * So ask the state directly instead of inferring it from pointer identity. "The last counted TEV
 * stage samples texmap 3" is the *same predicate* `onWarpMaterial` and `offWarpMaterial` use to
 * decide what to do (`:187`, `:203`), it is what actually governs rendering, and it cannot be
 * defeated by a pointer that means something different on PC than it did on GameCube.
 */
bool has_warp_material(J3DModelData* i_modelData) {
    if (i_modelData == NULL || i_modelData->getMaterialNum() == 0) {
        return false;
    }

    J3DTevBlock* tevBlock = i_modelData->getMaterialNodePointer(0)->getTevBlock();
    const u8 stageNum = tevBlock->getTevStageNum();
    return stageNum > 0 && tevBlock->getTevOrder(stageNum - 1)->getTexMap() == 3;
}

/**
 * One line describing a model's material state, for the puppet-vs-local-player comparison.
 *
 * ★ Written because the warp-particle bug has now survived two rounds of reasoning that produced
 * OPPOSITE predictions about which models should break, which is the point at which guessing has to
 * stop. Everything here is a property the local player's identical mesh also has, so any difference
 * between the two lines is a difference this actor introduced and nothing else.
 *
 * The number that matters is `lastTexMap`. addWarpMaterial appends the dissolve stage at TEV stage
 * 3 / texmap 3 and leaves it DISABLED by not counting it; onWarpMaterial enables it purely by
 * raising the stage count (d_resorce.cpp:181-195). So "the last counted stage reads from texmap 3"
 * is precisely "this model is currently drawing the twilight dissolve", which is what the bug looks
 * like. A puppet reporting 3 where the local player reports something else is the whole answer.
 */
/// The two numbers that decide whether a model is drawing the dissolve. Packed so a per-model
/// "has this changed since last tick" test is one comparison. 0xFFFF means "nothing sampled yet".
u16 material_signature(J3DModelData* i_modelData) {
    if (i_modelData == NULL || i_modelData->getMaterialNum() == 0) {
        return 0xFFFF;
    }

    J3DTevBlock* tevBlock = i_modelData->getMaterialNodePointer(0)->getTevBlock();
    const u8 stageNum = tevBlock->getTevStageNum();
    const int lastTexMap = stageNum > 0 ? tevBlock->getTevOrder(stageNum - 1)->getTexMap() : 0xFF;
    return (u16)((stageNum << 8) | (u8)lastTexMap);
}

void log_material_state(const char* i_who, const char* i_what, J3DModelData* i_modelData) {
    if (i_modelData == NULL) {
        Log.debug("  {} {}: <null>", i_who, i_what);
        return;
    }

    const u16 matNum = i_modelData->getMaterialNum();
    if (matNum == 0) {
        Log.debug("  {} {}: no materials", i_who, i_what);
        return;
    }

    J3DMaterial* material = i_modelData->getMaterialNodePointer(0);
    J3DTevBlock* tevBlock = material->getTevBlock();
    const u8 stageNum = tevBlock->getTevStageNum();
    const u32 texGenNum = material->getTexGenBlock()->getTexGenNum();
    J3DTexture* tex = i_modelData->getTexture();

    int lastTexMap = -1;
    if (stageNum > 0) {
        lastTexMap = tevBlock->getTevOrder(stageNum - 1)->getTexMap();
    }

    Log.debug("  {} {}: mats {} | stages {} | texgens {} | lastTexMap {} | textures {}{}", i_who,
        i_what, matNum, stageNum, texGenNum, lastTexMap, tex != NULL ? tex->getNum() : 0,
        lastTexMap == 3 ? "  <<< WARP MATERIAL IS ON" : "");
}

J3DModel* init_model(J3DModelData* i_modelData, u32 i_diffFlags) {
    if (i_modelData == NULL) {
        return NULL;
    }

    const bool warpMaterial = has_warp_material(i_modelData);

    if (warpMaterial) {
        dRes_info_c::onWarpMaterial(i_modelData);
        i_diffFlags |= 0x2000400;
    }

    J3DModel* model = mDoExt_J3DModel__create(i_modelData, 0x80000, i_diffFlags | 0x11000084);

    if (warpMaterial) {
        dRes_info_c::offWarpMaterial(i_modelData);
    }

    return model;
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

/* Which pointer type a caller of load_aram_anm intends to downcast the result to. */
enum AnmFamily {
    ANM_FAMILY_TRANSFORM,    // J3DAnmTransform*   — the BCK gait animations
    ANM_FAMILY_TEX_PATTERN,  // J3DAnmTexPattern*  — a BTP
    ANM_FAMILY_TEX_SRT,      // J3DAnmTextureSRTKey* — a BTK
};

/**
 * Is a loaded animation safe to downcast to the family the caller asked for?
 *
 * ★ It has to be a family test, not an equality test on the base class's kind.
 * `J3DAnmBase::getKind()` reports the CONCRETE class, and J3DAnmLoaderDataBase always hands back a
 * concrete subclass: a BCK arrives as J3DAnmTransformKey (8), TransformFull (9) or
 * TransformFullWithLerp (16) — never as a bare J3DAnmTransform (0). Found the hard way; an equality
 * check against 0 rejected all three gait animations and left the puppet failing createHeap in a
 * respawn loop.
 */
bool anm_kind_matches(s32 i_kind, AnmFamily i_family) {
    switch (i_family) {
    case ANM_FAMILY_TRANSFORM:
        // J3DAnmTransform and its three descendants (J3DAnimation.h:518, :544, :562, :575).
        return i_kind == 0 || i_kind == 8 || i_kind == 9 || i_kind == 16;
    case ANM_FAMILY_TEX_PATTERN:
        return i_kind == 2;  // J3DAnmTexPattern, no subclasses (J3DAnimation.h:903).
    case ANM_FAMILY_TEX_SRT:
        return i_kind == 4;  // J3DAnmTextureSRTKey, no subclasses (J3DAnimation.h:595).
    }
    return false;
}

/* The blink — "mabataki" — is index [0] of daAlink_c::m_faceTexDataTable (d_a_alink.cpp:849-850).
 * BTP swaps the eyelid texture, BTK slides the texture matrix, and the two are stepped in lock-step
 * on one frame number. Both live in the same AlAnm ARAM archive the gait animations come from.
 */
const u16 l_blinkBtpIdx = dRes_ID_ALANM_BTP_FMABA01_e;
const u16 l_blinkBtkIdx = dRes_ID_ALANM_BTK_FMABA01_e;

/* Per-frame chance of starting a blink. daAlink_c holds this in field_0x3440 and sets it to 0.012
 * for the normal face when the MABA pattern is loaded (d_a_alink.cpp:8218) — about one blink every
 * 83 ticks, i.e. every ~2.8 s at 30 Hz.
 */
const f32 l_blinkChance = 0.012f;

/* The two eye materials on the face model. Confirmed for Link at d_a_alink_wolf.inc:501-502 — they
 * are the only two that ever receive a J3DMaterialAnm. Range-checked at use anyway: the material
 * count comes out of a binary asset, so it is not verifiable from source for every outfit.
 */
const u16 l_eyeMaterialNo[2] = {2, 3};

/* s16 angle -> the -1..1 the eye offset is expressed in. 1/0x2000, so ±45° is full deflection.
 * Same constant daAlink_c::setEyeMove (d_a_alink.cpp:3296) and daHoZelda_c (d_a_hozelda.cpp:711)
 * use; written as the literal they use rather than 1.0f/8192.0f to stay greppable against them.
 */
const f32 l_eyeAngleToOffset = 0.00012207031f;

/* How far the eye texture slides at full deflection, in UV units — daAlink_c::setEyeMove
 * (d_a_alink.cpp:3359-3372), i.e. LINK's numbers, because this is Link's face rig.
 *
 * ★ These MUST come from Link and not from daHoZelda_c, and the reason is the sign, not the
 * magnitude. Zelda's two eye materials are UV-mirrored, so she slides them by -0.2 and +0.2 to move
 * both pupils the same way on screen (d_a_hozelda.cpp:741-747). Link's are NOT mirrored: both his
 * eyes take the SAME sign, and the 0.25/0.15 split is inner-vs-outer — the eye nearer the direction
 * of gaze travels further. Copying Zelda's opposite signs onto Link's face makes the pupils diverge
 * instead of track, which reads as cross-eyed one way and wall-eyed the other depending on which
 * side the viewer stands. That was the "peeling" regression.
 */
const f32 l_eyeOffsetInner = 0.25f;
const f32 l_eyeOffsetOuter = 0.15f;
const f32 l_eyeOffsetUp = 0.2f;
const f32 l_eyeOffsetDown = 0.1f;

/* Head aim limits, from daHoZelda_c::setNeckAngle (d_a_hozelda.cpp:817-818) and matching
 * daAlink_c's. Asymmetric in X because looking down is easier than looking up.
 */
const s16 l_eyeLimitUp = -10000;
const s16 l_eyeLimitDown = 8000;
const s16 l_eyeLimitSide = 20000;

/* When the local player is worth looking at. Mirrors daHoZelda_c (d_a_hozelda.cpp:797-806): always
 * within a quarter turn, and a little past that if they are close. Beyond it the puppet faces
 * front rather than craning after someone behind it.
 */
const s16 l_eyeGateAngle = 0x4000;
const s16 l_eyeGateAngleNear = 0x5000;
const f32 l_eyeGateNearDistSq = 90000.0f;

/* Below this the eyes count as centred, so the override can be handed back to the BTK without a
 * visible jump. In UV units, against a full deflection of 0.2.
 */
const f32 l_eyeCentredEpsilon = 0.002f;

/* Head-model joint layout for the sway, read off daAlink_c::headModelCallBack
 * (d_a_alink.cpp:2477-2496). Joints 1-5 are hair strands, each rotated independently in a world
 * axis frame; 6-9 are the cap, a chain where 6 and 7 share segment 7's angle (halved, so the crown
 * of the cap moves half as far as its body) and 8, 9 trail behind it.
 *
 * ★ "Hat model" in daAlink_c is the HEAD model — mpLinkHatModel is loaded from al_head.bmd
 * (d_a_alink_wolf.inc:364-366). Hair and cap live on the same model and are driven by the same
 * callback, which is why B was always one job and not two.
 */
const u16 l_swayJointNum = 10;
const u16 l_capJointFirst = 6;
const u16 l_capRootJointNo = 7;

/* Constant downward pull added to the apparent wind, so the cap hangs rather than sticking straight
 * out when the puppet is still (d_a_alink.cpp:2669 — the not-swimming-up branch, which is the only
 * one a puppet can be in; the 5.0f at :2673 belongs to the swimming case). Not gravity in any
 * physical sense — it is the bias that decides the rest pose.
 */
const f32 l_capGravity = 2.0f;
/* Human Link's own height (d_a_alink_wolf.inc:528). The wind-shelter line check is cast from half
 * of it, which is what daAlink_c feeds checkWindWallRate as mHeight. */
const f32 l_linkHeight = 180.0f;

/* Above this the wind counts as "strong" and the cap flutters at a fixed hard rate rather than one
 * proportional to how fast the head is moving (d_a_alink.cpp:2568-2576, :2789-2793). Compared
 * against the SAME quantity daAlink_c compares, so the two caps switch modes together.
 */
const f32 l_strongWindSpeed = 10.0f;

/* Threshold for the one-shot "this is actually moving" traces below — about 11 degrees. Chosen to
 * sit ABOVE the ~6 degrees the cap reaches just settling under its own weight at spawn, so the log
 * reports motion rather than the puppet finishing standing up.
 */
const s16 l_swayLoggedAngle = 0x800;

/* When to report the idle animation's frame — long enough in that a stuck frame is unambiguous. */
const u16 l_idleFrameLogTick = 200;

/* How often, and how many times, to print the puppet's cap angles next to the local player's. Kept
 * short because a puppet's life in a scripted run is only a few hundred in-world ticks — an earlier
 * period of 150 yielded exactly one sample.
 */
/* Models watched by checkMaterialDrift(), in the order body / head / hands / face. */
const int l_watchedModelNum = 4;
const u16 l_capComparePeriod = 60;
const u16 l_capCompareCount = 8;
/* The wind breakdown samples on its own schedule, and only while there is wind, because unlike the
 * cap comparison it has to survive the walk to somewhere windy. Forty samples two seconds apart is
 * over a minute of windy time — enough to cross a field and back — and it costs nothing anywhere
 * still, which is most places. */
const u16 l_windLogPeriod = 120;
const u16 l_windLogCount = 40;

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
 *
 * Returns the loader's own J3DAnmBase* so this can serve BCK, BTP and BTK alike; callers downcast.
 * `i_family` is that downcast written down and checked, so handing this the wrong resource index is
 * a log line rather than a wrong-vtable call several frames later.
 */
J3DAnmBase* load_aram_anm(u16 i_resIdx, AnmFamily i_family) {
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

        J3DAnmBase* anm = J3DAnmLoaderDataBase::load(buffer);
        if (anm == NULL) {
            Log.warn("J3D loader rejected animation {} ({} bytes)", i_resIdx, read);
            return NULL;
        }

        if (!anm_kind_matches(anm->getKind(), i_family)) {
            Log.warn("Animation {} is kind {}, which is not family {} — refusing to hand it over",
                i_resIdx, anm->getKind(), static_cast<int>(i_family));
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

    mpIdleAnm = static_cast<J3DAnmTransform*>(load_aram_anm(l_idleAnmIdx, ANM_FAMILY_TRANSFORM));
    mpWalkAnm = static_cast<J3DAnmTransform*>(load_aram_anm(l_walkAnmIdx, ANM_FAMILY_TRANSFORM));
    mpRunAnm = static_cast<J3DAnmTransform*>(load_aram_anm(l_runAnmIdx, ANM_FAMILY_TRANSFORM));
    if (mpIdleAnm == NULL || mpWalkAnm == NULL || mpRunAnm == NULL) {
        return 0;
    }

    /* ★ The last two arguments are the model flag and the deferred-display-list flag, and they must
     * match what every other Link model is built with. They were 0, 0 — which made the BODY the
     * only model in the scene created as mDoExt_J3DModel__create(data, 0, 0), while its own head,
     * hands and face go through init_model at (0x80000, 0x11000084), and so does every one of
     * daAlink_c's models (initModel, d_a_alink_wolf.inc:364-366). A body shaded on different terms
     * from the head bolted onto it is exactly the seam Stuart reported at the neck: "the puppet's
     * body is a few shades darker, noticeable by the head, the skin colour to the neck is a bit
     * different."
     *
     * 0x80000 also matters on its own: mDoExt_McaMorfSO::create calls mDoExt_changeMaterial for any
     * model flag OTHER than 0x80000 (m_Do_ext.cpp:1579-1581), so the old value put the body through
     * a material rewrite the other three never saw.
     *
     * For reference, no actor in the tree passes 0 for the deferred flag: 43 McaMorfSO
     * constructions use 0x80000 with a 0x1100xxxx deferred flag, of which 0x11000084 — the value
     * init_model uses — is the most common. */
    /* ★ The body needs the SAME warp-material bracketing the other three models get, and until now
     * it was the one model that did not. daAlink_c builds his body with initModel like everything
     * else (d_a_alink_wolf.inc:364), and al.bmd is a BMWR resource (Kmdl.h) — so dRes_info_c's
     * loader has already run addWarpMaterial over it, appending a fourth TEV stage, a fourth texgen
     * and the shared warp texture, and permanently replacing the alpha compare with "discard
     * anything at or below 0x80" (d_resorce.cpp:127-178, :291-293).
     *
     * ★ And the bracket's trailing half is the one that matters: BMWR models come out of the loader
     * with the dissolve already ENABLED, so `offWarpMaterial` is what switches it off. Skipping the
     * bracket does not leave the body slightly mis-sized — it leaves the body **drawing the
     * twilight dissolve**, which is the A5 bug. See has_warp_material() for the full account.
     *
     * The leading half still earns its place: it makes mDoExt_J3DModel__create size the model's own
     * copies of the TEV and texgen blocks to hold the extra stage, which is what 0x2000400 asks
     * for, matching what daAlink_c's body gets from initModel (d_a_alink_wolf.inc:364). */
    const bool bodyWarpMaterial = has_warp_material(modelData);

    u32 bodyDiffFlags = 0x11000084;
    if (bodyWarpMaterial) {
        dRes_info_c::onWarpMaterial(modelData);
        bodyDiffFlags |= 0x2000400;
    }

    mpModelMorf = JKR_NEW mDoExt_McaMorfSO(modelData, NULL, NULL, mpIdleAnm,
        J3DFrameCtrl::EMode_LOOP, 1.0f, 0, -1, NULL, 0x80000, bodyDiffFlags);

    if (bodyWarpMaterial) {
        dRes_info_c::offWarpMaterial(modelData);
    }

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

    // Cosmetic and deliberately non-fatal: a puppet with open, staring eyes is a far better failure
    // than no puppet. setupFaceAnimation() logs its own reason for every way it can decline.
    setupFaceAnimation();

    setupHeadSway();

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

    /* ★ The warp-particle report (00-status.md A5), measured rather than argued about. Both sides
     * of the same four meshes, once per puppet.
     *
     * The local player's half is skipped in wolf form, and that is not a limitation worth working
     * around — a wolf has genuinely different models, so there is nothing to compare against. It is
     * spelled out in the log because the previous bug in this file was diagnosed backwards for
     * exactly this reason: zeroes from a system that was not running got read as a mismatch. */
    /* 0xFFFF, not 0: fopAcM_ct zeroes the actor, and a zero would read as a real previous sample
     * and make checkMaterialDrift() announce a change on its very first tick. */
    for (int i = 0; i < l_watchedModelNum; i++) {
        mMaterialSig[i] = 0xFFFF;
    }

    const daAlink_c* link = static_cast<const daAlink_c*>(dComIfGp_getLinkPlayer());

    Log.debug("Puppet {} material state:", mPlayerId);
    log_material_state("puppet", "body", mpModelMorf->getModel()->getModelData());
    log_material_state("puppet", "head", mpHeadModel != NULL ? mpHeadModel->getModelData() : NULL);
    log_material_state("puppet", "hands", mpHandModel != NULL ? mpHandModel->getModelData() : NULL);
    log_material_state("puppet", "face", mpFaceModel != NULL ? mpFaceModel->getModelData() : NULL);

    if (link == NULL) {
        Log.debug("  (no local player to compare against)");
    } else if (link->checkWolf()) {
        Log.debug("  (local player is a WOLF — different meshes, nothing to compare against)");
    } else {
        log_material_state(
            "P1", "body", link->mpLinkModel != NULL ? link->mpLinkModel->getModelData() : NULL);
        log_material_state("P1", "head",
            link->mpLinkHatModel != NULL ? link->mpLinkHatModel->getModelData() : NULL);
        log_material_state("P1", "hands",
            link->mpLinkHandModel != NULL ? link->mpLinkHandModel->getModelData() : NULL);
        log_material_state("P1", "face",
            link->mpLinkFaceModel != NULL ? link->mpLinkFaceModel->getModelData() : NULL);
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

    /* Seeded from the player id, so two puppets standing side by side do not blink in unison — the
     * giveaway that would make them read as copies of one puppet rather than two people. Odd
     * multipliers keep the three streams from lining up, and every seed stays well inside its
     * modulus and non-zero. */
    mRndSeed[0] = 12345 + static_cast<s32>(mPlayerId) * 331;
    mRndSeed[1] = 6789 + static_cast<s32>(mPlayerId) * 557;
    mRndSeed[2] = 23456 + static_cast<s32>(mPlayerId) * 173;

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

    /* Stuart reported the puppet as having "no idle body animation". The animation it plays IS the
     * one the local player plays — ANM_WAIT resolves to WAITS (d_a_alink.cpp:308) and it is set to
     * loop at mWaitAnmSpeed, which is 1.0 (d_a_alink_HIO_data.inc:33) — so the question is whether
     * the frame is actually ADVANCING, which nothing else in the trace can answer. One latched line
     * once the puppet has been idle a while; a frame near the animation's end means it is running.
     */
    if (wanted == l_idleAnmIdx) {
        if (mIdleTicks < 0xFFFF) {
            mIdleTicks++;
        }
        if (mIdleTicks == l_idleFrameLogTick && !mLoggedIdleFrame) {
            mLoggedIdleFrame = true;
            Log.debug(
                "Puppet {} idle body anim after {} ticks: frame {:.1f} of {:.1f}, rate {:.2f}, "
                "mode {}",
                mPlayerId, mIdleTicks, mpModelMorf->getFrame(), mpModelMorf->getEndFrame(),
                mpModelMorf->getPlaySpeed(), mpModelMorf->getPlayMode());
        }
    } else {
        mIdleTicks = 0;
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

/**
 * Attach the blink to the face model's eye materials. Runs once, inside createHeap.
 *
 * ★ ORDER IS LOAD-BEARING and its failure mode is silent. Both entry points look up
 * `J3DMaterial::getMaterialAnm()` on each material the animation targets and simply skip any that
 * has none — `entryTexNoAnimator` returns 1 having attached nothing to that material
 * (J3DMaterialAttach.cpp:206-215), and `entryTexMtxAnimator` bails out of `createTexMtxForAnimator`
 * before attaching anything at all (:152-155, :226-231). Nothing crashes; the eyes just never move.
 * So the J3DMaterialAnm objects have to exist on materials 2 and 3 FIRST. The return codes are
 * checked here for the same reason: this is a class of bug that otherwise only shows up as "it
 * looks the same as before".
 *
 * ★ daNpcF_MatAnm_c, deliberately NOT daAlink_matAnm_c. Link's subclass reaches for
 * `daAlink_getAlinkActorClass()->checkStatusWindowDraw()` in its calc (d_a_alink.cpp:2022) and
 * keeps its blend state in STATIC members shared by every instance (d_a_alink.h:60-61) — a puppet
 * using it would read and write the local player's eye state — and merely constructing one would
 * stomp it, since the constructor calls init() and init() zeroes those statics. daNpcF_MatAnm_c
 * (d_a_npc4.cpp:103-130) is the same calc with the flags as INSTANCE members and no singleton
 * lookup, which is what an actor that is not a singleton needs.
 *
 * ★ dEyeHL_c is NOT part of blinking, despite sitting next to it in daAlink_c::setLinkModel. It
 * only nudges texture LODBias by FOV during events with an HIO flag set (d_eye_hl.cpp:40-55), and
 * it hard-errors if the texture it is given is absent (:31) — which across outfits it may well be.
 */
bool daRemotePlayer_c::setupFaceAnimation() {
    if (mpFaceModel == NULL) {
        return false;
    }

    J3DModelData* faceData = mpFaceModel->getModelData();
    const u16 materialNum = faceData->getMaterialNum();
    for (int i = 0; i < 2; i++) {
        if (l_eyeMaterialNo[i] >= materialNum) {
            Log.warn(
                "Face model has {} materials; no eye material {}", materialNum, l_eyeMaterialNo[i]);
            return false;
        }
    }

    for (int i = 0; i < 2; i++) {
        mpEyeMatAnm[i] = JKR_NEW daNpcF_MatAnm_c();
        if (mpEyeMatAnm[i] == NULL) {
            Log.warn("Out of heap for the puppet's eye material anm {}", i);
            return false;
        }
        faceData->getMaterialNodePointer(l_eyeMaterialNo[i])->setMaterialAnm(mpEyeMatAnm[i]);
    }

    mpBlinkBtp =
        static_cast<J3DAnmTexPattern*>(load_aram_anm(l_blinkBtpIdx, ANM_FAMILY_TEX_PATTERN));
    mpBlinkBtk =
        static_cast<J3DAnmTextureSRTKey*>(load_aram_anm(l_blinkBtkIdx, ANM_FAMILY_TEX_SRT));
    if (mpBlinkBtp == NULL || mpBlinkBtk == NULL) {
        // Drop both, so playFaceTextureAnime() never has to reason about half a blink.
        mpBlinkBtp = NULL;
        mpBlinkBtk = NULL;
        return false;
    }

    /* Through the material table rather than J3DModelData, which is the same call with the return
     * code thrown away (J3DModelData.h:79, :81) — and the return code is the only way this failure
     * is visible at all. */
    J3DMaterialTable& faceMaterials = faceData->getMaterialTable();

    mpBlinkBtp->searchUpdateMaterialID(faceData);
    const int btpResult = faceMaterials.entryTexNoAnimator(mpBlinkBtp);

    mpBlinkBtk->searchUpdateMaterialID(faceData);
    const int btkResult = faceMaterials.entryTexMtxAnimator(mpBlinkBtk);

    mpBlinkBtp->setFrame(0.0f);
    mpBlinkBtk->setFrame(0.0f);
    mBlinkFrame = 0;

    if (btpResult != 0 || btkResult != 0) {
        // Non-zero is never fatal but is never right either: 1 means a target material had no
        // J3DMaterialAnm, 2 means the material table was locked, 4 means a texture matrix had to be
        // manufactured — and 4 makes entryTexMtxAnimator return before attaching anything.
        Log.warn("Puppet blink attached with warnings: BTP={} BTK={}", btpResult, btkResult);
        return false;
    }

    Log.info("Puppet blink attached to face materials {} and {}", l_eyeMaterialNo[0],
        l_eyeMaterialNo[1]);
    return true;
}

/**
 * This puppet's own random stream.
 *
 * ★ Deliberately not cM_rnd(). That is a single Wichmann-Hill generator over three FILE-STATIC
 * seeds (c_math.cpp:173-196) shared by every actor in the game. Rolling a blink from it once per
 * tick per puppet would advance the global sequence by a number of steps that depends on HOW MANY
 * OTHER PLAYERS ARE IN THE ROOM — so the host and each guest would draw different numbers for
 * everything else that uses it, and a co-op session would not even match single-player. Cosmetic
 * eyelids are not worth spending the game's shared randomness on.
 *
 * Same generator, private seeds. The three moduli are prime, so a seed that starts non-zero stays
 * non-zero and the stream cannot collapse.
 */
f32 daRemotePlayer_c::ownRnd() {
    mRndSeed[0] = (mRndSeed[0] * 171) % 30269;
    mRndSeed[1] = (mRndSeed[1] * 172) % 30307;
    mRndSeed[2] = (mRndSeed[2] * 170) % 30323;

    const f32 sum = mRndSeed[0] / 30269.0f + mRndSeed[1] / 30307.0f + mRndSeed[2] / 30323.0f;
    return fabsf(fmodf(sum, 1.0f));
}

/* cM_rndF's contract (c_math.cpp:198), on the puppet's own stream. */
f32 daRemotePlayer_c::ownRndF(f32 i_max) {
    return i_max * ownRnd();
}

/**
 * Blink, the way the local player blinks.
 *
 * daAlink_c::playFaceTextureAnime (d_a_alink.cpp:8383-8394) is three lines once the demo, hawk and
 * priority-animation branches are stripped away, and none of those apply to a puppet: roll each
 * frame to START a blink, then run the cursor to the longer of the two animations' frame counts and
 * drop back to zero. Both animations are set to the same frame, each clamped to its own maximum.
 *
 * ★ There is nothing to replicate here. The blink is a local, random, purely cosmetic roll on the
 * sender's machine too — it is not part of the sender's state and never crosses the wire, so a
 * puppet blinking on its own schedule is not an approximation of the original, it IS the original.
 */
void daRemotePlayer_c::playFaceTextureAnime() {
    if (mpBlinkBtp == NULL || mpBlinkBtk == NULL) {
        return;
    }

    const s16 btpFrameMax = mpBlinkBtp->getFrameMax();
    const s16 btkFrameMax = mpBlinkBtk->getFrameMax();
    const s16 blinkFrameMax = btpFrameMax > btkFrameMax ? btpFrameMax : btkFrameMax;

    if (mBlinkFrame != 0) {
        mBlinkFrame++;
        if (mBlinkFrame > blinkFrameMax) {
            mBlinkFrame = 0;
        }
    } else if (ownRnd() < l_blinkChance) {
        mBlinkFrame++;
        if (!mLoggedFirstBlink) {
            mLoggedFirstBlink = true;
            // One line, latched. "The eyes never move" is otherwise unfalsifiable from a trace:
            // every silent failure in setupFaceAnimation() and every dead texture animator produce
            // exactly the same still face. This says the timer ran and the animations are live.
            Log.debug("Puppet {} blinked for the first time ({} frames)", mPlayerId, blinkFrameMax);
        }
    }

    mpBlinkBtp->setFrame(mBlinkFrame > btpFrameMax ? btpFrameMax : mBlinkFrame);
    mpBlinkBtk->setFrame(mBlinkFrame > btkFrameMax ? btkFrameMax : mBlinkFrame);
}

/**
 * Aim the eyes at the local player.
 *
 * ★ Eyes in this engine do not rotate — nothing swivels an eyeball joint. The eye texture is SLID,
 * by overwriting the translation of the tex matrix the face's BTK animates. That is the whole
 * mechanism, and it is why this composes with the blink for free: the blink is the BTP (it swaps
 * the eyelid texture), so replacing the BTK's translation aims the eye without touching the lid.
 * daAlink_matAnm_c and daNpcF_MatAnm_c both do exactly this in their calc override.
 *
 * Two sources, split on purpose. The TARGETING — pick a target, gate it on whether it is worth
 * looking at, take the angle from the head to it and clamp it to what a head can manage — is
 * daHoZelda_c::setNeckAngle (d_a_hozelda.cpp:779-840), because she is a non-Link humanoid doing
 * exactly this to the player and daAlink_c's equivalent is tangled in his own state. The
 * ANGLE-TO-OFFSET step is daAlink_c::setEyeMove (d_a_alink.cpp:3355-3383), because that half is a
 * property of the face RIG rather than of the character, and this puppet wears Link's face.
 *
 * ★ Mixing those two the other way is what caused the "peeling" regression: Zelda's eye materials
 * are UV-mirrored and Link's are not, so her opposite-signed pair made the puppet's pupils diverge.
 * See the offset constants above.
 *
 * ★ One deliberate departure. Zelda halves the clamped angle (`>> 1`, d_a_hozelda.cpp:820-821)
 * because her NECK takes the other half; the eyes only ever carry the remainder. This puppet has no
 * joint callback, so its head is rigid and there is no other half — it uses the full clamped angle,
 * so the total gaze lands in roughly the right place instead of half way there. Turning the neck
 * too is the better answer and is a separate change.
 *
 * The local player's eyePos as the target is not a stand-in for something replicated: it is what
 * daHoZelda_c and daMidna_c both genuinely use. Aiming at what the REMOTE player is looking at
 * would need their target on the wire, and is only worth it once there is a target worth sending.
 */
void daRemotePlayer_c::setEyeMove() {
    if (mpEyeMatAnm[0] == NULL || mpEyeMatAnm[1] == NULL) {
        return;
    }

    /* The head joint's world position, taken from the matrix the head model is already posed off,
     * so this costs nothing and cannot disagree with where the head actually is. Valid only after
     * setMatrix()'s modelCalc(), which is why this runs at the end of execute(). */
    MtxP headMtx = model->getAnmMtx(l_headJointNo);
    cXyz headPos(headMtx[0][3], headMtx[1][3], headMtx[2][3]);

    daPy_py_c* player = daPy_getLinkPlayerActorClass();
    bool haveTarget = false;
    s16 angleX = 0;
    s16 angleY = 0;

    if (player != NULL) {
        const cXyz toPlayer = player->current.pos - current.pos;
        const int away = cLib_distanceAngleS(toPlayer.atan2sX_Z(), shape_angle.y);
        if (away <= l_eyeGateAngle ||
            (away <= l_eyeGateAngleNear && toPlayer.abs2XZ() < l_eyeGateNearDistSq))
        {
            const cXyz toTarget = player->eyePos - headPos;
            angleX = cLib_minMaxLimit<s16>(toTarget.atan2sY_XZ(), l_eyeLimitUp, l_eyeLimitDown);
            angleY = cLib_minMaxLimit<s16>(
                toTarget.atan2sX_Z() - shape_angle.y, -l_eyeLimitSide, l_eyeLimitSide);
            haveTarget = true;
        }
    }

    /* Link restarts the idle countdown and forgets the idle direction on EVERY call, whatever the
     * eyes end up doing (d_a_alink.cpp:3287-3291). Both are read back below through the copies
     * taken here, so a tick spent looking at something costs the wander its progress — the puppet
     * does not resume a half-finished glance after tracking a player. */
    const u8 prevIdleTimer = mIdleGazeTimer;
    const f32 prevIdleH = mIdleGaze[0];
    const f32 prevIdleV = mIdleGaze[1];

    mIdleGazeTimer = 75.0f + ownRndF(30.0f);
    mIdleGaze[0] = 0.0f;
    mIdleGaze[1] = 0.0f;

    f32 horizontal = 0.0f;
    f32 vertical = 0.0f;
    bool eyesActive = false;

    if (haveTarget) {
        vertical = cLib_minMaxLimit<f32>(l_eyeAngleToOffset * angleX, -1.0f, 1.0f);
        horizontal = cLib_minMaxLimit<f32>(l_eyeAngleToOffset * angleY, -1.0f, 1.0f);
        eyesActive = true;
    } else if (mNetSpeed < l_idleSpeedThreshold) {
        /* Nobody worth watching and standing still, so look about. daAlink_c::setEyeMove's idle
         * branch (d_a_alink.cpp:3337-3358), which is what stops a waiting Link from staring dead
         * ahead. His version gates on mProcID == PROC_WAIT and friends; a puppet has no proc, and
         * "not moving" is the same idea.
         *
         * Unlike the tracking path these are NOT angles — they are already the -1..1 deflection,
         * so an idle glance always goes to full travel. */
        if (prevIdleTimer != 0) {
            // Mid-glance: hold it and run the clock down.
            mIdleGazeTimer = prevIdleTimer - 1;
            mIdleGaze[0] = prevIdleH;
            mIdleGaze[1] = prevIdleV;
        } else if (prevIdleH != 0.0f || prevIdleV != 0.0f) {
            /* Was looking somewhere and the clock ran out: half the time settle back to centre,
             * half the time glance somewhere else.
             *
             * ★ Transcribed WITH an oddity in the original. It rotates "the current direction" via
             * cM_atan2s(field_0x3418, field_0x341c) — but both were zeroed at the top of the
             * function a few lines earlier and the copies were never written back, so the atan2 is
             * of (0, 0) and the result is always one of three fixed directions rather than an
             * offset from where the eyes already were. Kept, because looking like the original is
             * the point and the outcome is perfectly reasonable idle behaviour: down-left, down, or
             * down-right. Do not "fix" this into using the copies without deciding that
             * deliberately. */
            if (ownRnd() < 0.5f) {
                mIdleGaze[0] = 0.0f;
                mIdleGaze[1] = 0.0f;
            } else {
                s16 dir = cM_atan2s(mIdleGaze[0], mIdleGaze[1]);
                ANGLE_ADD(dir, ((int)ownRndF(3.0f) << 13) + 0x6000);
                mIdleGaze[0] = cM_ssin(dir);
                mIdleGaze[1] = cM_scos(dir);
            }
        } else {
            // Eyes were centred: pick one of eight directions around the clock face.
            const s16 dir = (s16)((int)ownRndF(8.0f) << 13);
            mIdleGaze[0] = cM_ssin(dir);
            mIdleGaze[1] = cM_scos(dir);
        }

        horizontal = mIdleGaze[0];
        vertical = mIdleGaze[1];
        eyesActive = true;

        /* Latched on the first idle glance that actually goes somewhere. The tracking log below
         * cannot cover this path by definition — it only fires when there IS a target — so without
         * this line the wander is the one eye behaviour with no evidence behind it. */
        if (!mLoggedIdleGaze && (horizontal != 0.0f || vertical != 0.0f)) {
            mLoggedIdleGaze = true;
            Log.debug("Puppet {} idle glance: {:.2f},{:.2f} held for {} ticks", mPlayerId,
                horizontal, vertical, mIdleGazeTimer);
        }
    }

    f32 wantX[2] = {0.0f, 0.0f};
    f32 wantY[2] = {0.0f, 0.0f};

    if (eyesActive) {
        /* Both eyes slide the SAME way; the asymmetry is which one leads. Whichever eye is on the
         * inside of the turn travels the full 0.25, the outer one 0.15 (d_a_alink.cpp:3359-3366).
         * Vertically Link looks up further than down, and the second eye is not smoothed
         * separately at all — it copies the first (:3368-3372, :3383). */
        if (horizontal > 0.0f) {
            wantX[0] = l_eyeOffsetInner * horizontal;
            wantX[1] = l_eyeOffsetOuter * horizontal;
        } else {
            wantX[0] = l_eyeOffsetOuter * horizontal;
            wantX[1] = l_eyeOffsetInner * horizontal;
        }

        wantY[0] = (vertical > 0.0f ? l_eyeOffsetUp : l_eyeOffsetDown) * vertical;
        wantY[1] = wantY[0];
    }

    /* ★ No radial clamp here, deliberately. daHoZelda_c shrinks a diagonal glance back inside the
     * unit circle (d_a_hozelda.cpp:752-762); daAlink_c does not, and this is Link's face. The clamp
     * was also written against Zelda's symmetric pair and does not transfer to an asymmetric one.
     *
     * Smoothing rates are Link's, and X and Y differ (d_a_alink.cpp:3380-3383). */
    cLib_addCalc(&mEyeOffset[0][0], wantX[0], 0.5f, 0.1f, 0.03f);
    cLib_addCalc(&mEyeOffset[1][0], wantX[1], 0.5f, 0.1f, 0.03f);
    cLib_addCalc(&mEyeOffset[0][1], wantY[0], 0.5f, 0.08f, 0.02f);
    mEyeOffset[1][1] = mEyeOffset[0][1];

    for (int i = 0; i < 2; i++) {
        mpEyeMatAnm[i]->setNowOffsetX(mEyeOffset[i][0]);
        mpEyeMatAnm[i]->setNowOffsetY(mEyeOffset[i][1]);
    }

    if (haveTarget && !mLoggedFirstGaze && fabsf(mEyeOffset[0][0]) > l_eyeCentredEpsilon) {
        mLoggedFirstGaze = true;
        // The two X offsets MUST share a sign; opposite signs are the peeling bug returning.
        Log.debug("Puppet {} gaze engaged: angle {},{} -> L {:.3f},{:.3f} R {:.3f},{:.3f}",
            mPlayerId, angleX, angleY, mEyeOffset[0][0], mEyeOffset[0][1], mEyeOffset[1][0],
            mEyeOffset[1][1]);
    }

    /* Handing control back to the BTK is the one place this can pop, because daNpcF_MatAnm_c has no
     * morf frame to cross-fade with (daAlink_c and daHoZelda_c both use one; the field is private
     * with no setter here). Instead the override is held ON until the offsets have smoothed to
     * centre, at which point dropping it changes nothing visible. */
    if (eyesActive) {
        mEyeMoveOn = true;
    } else if (mEyeMoveOn && fabsf(mEyeOffset[0][0]) < l_eyeCentredEpsilon &&
               fabsf(mEyeOffset[0][1]) < l_eyeCentredEpsilon &&
               fabsf(mEyeOffset[1][0]) < l_eyeCentredEpsilon &&
               fabsf(mEyeOffset[1][1]) < l_eyeCentredEpsilon)
    {
        mEyeMoveOn = false;
    }

    for (int i = 0; i < 2; i++) {
        if (mEyeMoveOn) {
            mpEyeMatAnm[i]->onEyeMoveFlag();
        } else {
            mpEyeMatAnm[i]->offEyeMoveFlag();
        }
    }
}

/**
 * J3D calls this for every joint of the head model, twice per joint. Shape matches
 * daAlink_headModelCallBack (d_a_alink.cpp:2505-2515): act on the first pass only, and resolve the
 * owning actor from the model rather than from any global, so it stays correct with several
 * puppets on screen.
 */
static int daRemotePlayer_headModelCallBack(J3DJoint* i_joint, int i_pass) {
    if (i_pass != 0) {
        return 1;
    }

    J3DModel* model = j3dSys.getModel();
    if (model == NULL || i_joint == NULL) {
        return 1;
    }

    daRemotePlayer_c* puppet = reinterpret_cast<daRemotePlayer_c*>(model->getUserArea());
    if (puppet == NULL) {
        return 1;
    }

    return puppet->headModelCallBack(i_joint->getJntNo());
}

/**
 * Hook the sway callback onto this puppet's head model. Once, at createHeap time.
 *
 * ★ setCallBack lives on the model DATA, i.e. on the shared joint tree — which is exactly why this
 * is only safe because the puppet mounts its OWN archive (see mountOwnArchive). Against the global
 * copy this would install a daRemotePlayer_c callback on the joint tree the LOCAL PLAYER's head is
 * drawn from, and the trampoline would then cast Link to a puppet. The private mount is not an
 * optimisation here; it is the precondition.
 *
 * The trampoline resolves the actor from J3DModel::getUserArea, which is per-MODEL rather than per
 * model data, so even a shared tree would dispatch to the right actor — but it would still be
 * dispatching Link's joints into puppet code. Both halves have to be private, and they are.
 *
 * Joint 0 is skipped, matching daAlink_c (d_a_alink_swindow.inc:175-177): it is the head model's
 * root and carries the whole head, so rotating it would sway the face and skull too.
 */
void daRemotePlayer_c::setupHeadSway() {
    if (mpHeadModel == NULL) {
        return;
    }

    J3DModelData* headData = mpHeadModel->getModelData();
    if (headData == NULL) {
        return;
    }

    const u16 jointNum = headData->getJointNum();
    if (jointNum <= l_capRootJointNo) {
        // Not a head model with a cap chain on it. Leave it rigid rather than index off the end.
        Log.warn("Puppet head model has only {} joints; no hat or hair sway", jointNum);
        return;
    }

    mpHeadModel->setUserArea((uintptr_t)this);

    const u16 last = jointNum < l_swayJointNum ? jointNum : l_swayJointNum;
    for (u16 i = 1; i < last; i++) {
        headData->getJointNodePointer(i)->setCallBack(daRemotePlayer_headModelCallBack);
    }

    Log.info("Puppet hat and hair sway attached to head joints 1-{}", last - 1);
}

/**
 * Rotate one joint's world matrix about the puppet's yaw frame, in place.
 *
 * daAlink_c::setMatrixWorldAxisRot (d_a_alink.cpp:2098-2120) with the magne-boot frame dropped —
 * a puppet never wears the iron boots, and concatMagneBootMtx is daAlink_c state. Deliberately
 * does NOT write J3DSys::mCurrentMtx, matching the param_4 == 0 form the hair uses: each hair
 * strand is rotated on its own and must not drag the rest of the head with it.
 */
void daRemotePlayer_c::setJointWorldAxisRot(MtxP i_mtx, s16 i_rotX, s16 i_rotY, s16 i_rotZ) {
    cXyz jointPos;
    mDoMtx_multVecZero(i_mtx, &jointPos);

    mDoMtx_stack_c::transS(jointPos);
    mDoMtx_stack_c::YrotM(shape_angle.y);
    mDoMtx_stack_c::ZXYrotM(i_rotX, i_rotY, i_rotZ);
    mDoMtx_stack_c::YrotM(-shape_angle.y);
    mDoMtx_stack_c::transM(-jointPos.x, -jointPos.y, -jointPos.z);
    mDoMtx_stack_c::concat(i_mtx);
    mDoMtx_copy(mDoMtx_stack_c::get(), i_mtx);
}

/**
 * Apply this tick's sway to one joint of the head model, during that model's calc().
 *
 * Transcribed from daAlink_c::headModelCallBack (d_a_alink.cpp:2477-2496), keeping only the branch
 * that runs during ordinary play: no demo BCK override, no status-window tilt, no metamorphose
 * squash, no Zora-helmet widening.
 *
 * The two halves work differently on purpose. The CAP (joints >= 6) multiplies into
 * J3DSys::mCurrentMtx and writes it back, so each segment's rotation is inherited by the next and
 * the chain bends cumulatively. The HAIR (joints < 6) rewrites only its own matrix, so strands
 * stay independent of one another.
 */
int daRemotePlayer_c::headModelCallBack(int i_jointNo) {
    if (mpHeadModel == NULL || i_jointNo <= 0 || i_jointNo >= l_swayJointNum) {
        return 1;
    }

    if (i_jointNo >= l_capJointFirst) {
        mDoMtx_stack_c::copy(J3DSys::mCurrentMtx);

        if (i_jointNo == l_capJointFirst) {
            mDoMtx_stack_c::XYZrotM(
                0, mSwayAngleY[l_capRootJointNo] >> 1, mSwayAngleX[l_capRootJointNo] >> 1);
        } else if (i_jointNo == l_capRootJointNo) {
            mDoMtx_stack_c::XYZrotM(0, mSwayAngleY[l_capRootJointNo] >> 1,
                (mSwayAngleX[l_capRootJointNo] >> 1) + mFlutterAngle[0]);
        } else {
            const int segment = i_jointNo - l_capRootJointNo;
            mDoMtx_stack_c::XYZrotM(
                0, mSwayAngleY[i_jointNo], mSwayAngleX[i_jointNo] + mFlutterAngle[segment]);
        }

        mpHeadModel->setAnmMtx(i_jointNo, mDoMtx_stack_c::get());
        cMtx_copy(mDoMtx_stack_c::get(), J3DSys::mCurrentMtx);
    } else {
        /* The rotation is applied in a yaw frame built from where the HEAD points, not where the
         * body does, so hair blows the same way whichever way the puppet has turned its head.
         * daAlink_c swaps shape_angle.y the same way for the same reason (d_a_alink.cpp:2488-2491)
         * — it is the only argument setMatrixWorldAxisRot does not take. Restored immediately;
         * nothing between the two lines can yield. */
        const s16 bodyYaw = shape_angle.y;
        shape_angle.y = mHeadYaw;
        setJointWorldAxisRot(
            mpHeadModel->getAnmMtx(i_jointNo), mSwayAngleX[i_jointNo], 0, mSwayAngleY[i_jointNo]);
        shape_angle.y = bodyYaw;
    }

    return 1;
}

/**
 * Decay one hair angle back to rest.
 *
 * daAlink_c::calcHairAngle (d_a_alink.cpp:2801-2803). Note the 400 — it is a decimal literal in the
 * original, not 0x400, and the difference is a factor of two and a half in how fast hair settles.
 */
void daRemotePlayer_c::calcHairAngle(s16* o_angle) {
    cLib_addCalcAngleS(o_angle, 0, 5, 400, 50);
}

/**
 * Blow the hair around.
 *
 * daAlink_c::setHairAngle (d_a_alink.cpp:2805-2878), transcribed whole. This is NOT a spring: it is
 * four free-running phase accumulators whose rates depend on how hard the apparent wind is blowing,
 * turned into 0..1 envelopes by `0.5 * (1 + cos)` and multiplied by the wind's lateral and forward
 * components. Each of the five strands gets its own amplitude, and the amplitudes differ by sign of
 * the wind so hair blown forward does not simply mirror hair blown back.
 *
 * ★ The one substantive change: cM_rndF becomes ownRndF. The original jitters all four phase rates
 * with the global random stream FOUR TIMES PER FRAME PER ACTOR. Puppets running that would advance
 * the shared Wichmann-Hill sequence by an amount depending on how many players are in the session,
 * so host and guest would draw different numbers for everything else in the game. Same hazard as
 * the blink, same fix, and it matters more here because the call rate is four times higher.
 */
void daRemotePlayer_c::setHairAngle(cXyz* i_apparentWind, f32 i_sinYaw, f32 i_cosYaw) {
    f32 strength = i_apparentWind->abs();
    f32 lateralLen = i_apparentWind->absXZ();

    if (strength < 1.0f || lateralLen < 1.0f) {
        for (int i = 1; i <= 5; i++) {
            calcHairAngle(&mSwayAngleX[i]);
            calcHairAngle(&mSwayAngleY[i]);
        }

        /* Parked at -0x8000 rather than 0 so cos() is -1 and every envelope starts at zero: the
         * hair picks back up from rest instead of snapping to mid-swing. */
        for (int i = 0; i < 4; i++) {
            mHairPhase[i] = -0x8000;
        }
        return;
    }

    strength *= 0.033333335f;
    if (strength > 1.0f) {
        strength = 1.0f;
    }
    strength = 0.15f + 0.85f * strength;

    ANGLE_ADD(mHairPhase[0], 1000.0f + ownRndF(500.0f) + strength * (3000.0f + ownRndF(1000.0f)));
    ANGLE_ADD(mHairPhase[1], 1000.0f + ownRndF(500.0f) + strength * (3000.0f + ownRndF(1000.0f)));
    ANGLE_ADD(mHairPhase[2], 1000.0f + ownRndF(500.0f) + strength * (5000.0f + ownRndF(1500.0f)));
    ANGLE_ADD(mHairPhase[3], 1000.0f + ownRndF(500.0f) + strength * (5000.0f + ownRndF(1500.0f)));

    lateralLen = 1.0f / lateralLen;
    i_apparentWind->x *= lateralLen;
    i_apparentWind->z *= lateralLen;

    // The wind, rotated into the head's frame: sideways component and front-to-back component.
    const f32 lateral = i_apparentWind->x * i_cosYaw - i_apparentWind->z * i_sinYaw;
    const f32 forward = i_apparentWind->x * i_sinYaw + i_apparentWind->z * i_cosYaw;

    const f32 envA = strength * (0.5f * (1.0f + cM_scos(mHairPhase[0])));
    const f32 envB = strength * (0.5f * (1.0f + cM_scos(mHairPhase[1])));
    const f32 envC = strength * (0.5f * (1.0f + cM_scos(mHairPhase[2])));
    const f32 envD = strength * (0.5f * (1.0f + cM_scos(mHairPhase[3])));

    if (lateral > 0.0f) {
        mSwayAngleY[1] = 6000.0f * envA * lateral;
        mSwayAngleY[2] = 8000.0f * envA * lateral;
        mSwayAngleY[3] = 2000.0f * envB * lateral;
        mSwayAngleY[4] = 7000.0f * envC * lateral;
        mSwayAngleY[5] = 2500.0f * envD * lateral;
    } else {
        mSwayAngleY[1] = 10000.0f * envA * lateral;
        mSwayAngleY[2] = 2000.0f * envA * lateral;
        mSwayAngleY[3] = 8000.0f * envB * lateral;
        mSwayAngleY[4] = 2500.0f * envC * lateral;
        mSwayAngleY[5] = 7000.0f * envD * lateral;
    }

    if (forward > 0.0f) {
        mSwayAngleX[1] = -9000.0f * envA * forward;
        mSwayAngleX[2] = -15000.0f * envA * forward;
        mSwayAngleX[3] = -15000.0f * envB * forward;
    } else {
        mSwayAngleX[1] = -1000.0f * envA * forward;
        mSwayAngleX[2] = -5000.0f * envA * forward;
        mSwayAngleX[3] = -5000.0f * envB * forward;
    }

    mSwayAngleX[4] = -7000.0f * envC * forward;
    mSwayAngleX[5] = -7000.0f * envD * forward;

    /* Separate latch from the cap's, because the two are driven differently and one working proves
     * nothing about the other: the cap settles under its own weight the moment the puppet spawns,
     * whereas the hair only ever moves when the apparent wind is over a unit long. All five strands
     * are printed — they take different amplitudes off the same envelopes, so a run of identical
     * values would mean the per-strand constants are not being applied. */
    if (!mLoggedFirstHair && abs(mSwayAngleX[1]) > l_swayLoggedAngle) {
        mLoggedFirstHair = true;
        Log.debug("Puppet {} hair moving: X {},{},{},{},{}  Y {},{},{},{},{}", mPlayerId,
            mSwayAngleX[1], mSwayAngleX[2], mSwayAngleX[3], mSwayAngleX[4], mSwayAngleX[5],
            mSwayAngleY[1], mSwayAngleY[2], mSwayAngleY[3], mSwayAngleY[4], mSwayAngleY[5]);
    }
}

/**
 * Integrate one tick of hat and hair motion.
 *
 * daAlink_c::setHatAngle (d_a_alink.cpp:2541-2799), which is the whole of Link's hat and hair
 * physics. Everything Link-only is dropped: magne boots, swimming, horse and goat riding, the
 * metamorphose stretch, the status-window pose, and the HIO tuning block. What is left is the
 * mechanism, transcribed rather than approximated.
 *
 * ★ THE PUPPET CAN RUN THE REAL ALGORITHM, and this is worth stating because 00-status.md offered
 * a spring driven off replicated velocity as the cheap alternative. It is not needed. The ONLY
 * motion input the original uses is how far the cap's anchor joint moved in world space since last
 * tick — which the puppet measures from its own head matrix, exactly as the local player does. That
 * measurement already contains the animation's head bob, the gait, and the network interpolation,
 * so it is strictly better information than a replicated velocity scalar would have been. Nothing
 * about hat physics needs to go on the wire.
 *
 * Runs at the end of execute(), after setMatrix(), so it reads matrices this tick's calc() produced
 * and the callback consumes its output on the NEXT calc(). daAlink_c has the same one-frame lag
 * (d_a_alink.cpp:18530-18538) — it is not a bug to fix.
 */
/**
 * daAlink_c::checkWindWallRate (d_a_alink.cpp:5461-5476), cast from the puppet's own position.
 *
 * Look upwind from chest height for the length the game considers wind can travel around an
 * obstacle. Nothing in the way: full strength. A wall closer than mNoWindInfluenceDist: no wind at
 * all. Between the two: linear. Wall code 0xA is excluded by the original, so it is excluded here.
 *
 * ★ The CLASS of the line check is load-bearing, and getting it wrong is why this term went in as a
 * fix and came back out as a bug. mWindLinChk is a dBgS_LinkLinChk, whose constructor calls
 * SetLink() (d_bg_s_lin_chk.cpp:69-71) so the cast passes through every link-through polygon
 * (d_bg_w_kcol.cpp:205). Given a plain dBgS_LinChk instead, the ray stops on collision that Link's
 * own ray ignores, the cross point comes back short, and the rate collapses toward zero — wind
 * killed OUTDOORS, which is the opposite of what this term is for and precisely what Stuart saw
 * standing south of Hyrule Castle. Link casts his through mLinkLinChk (d_a_alink.h:4027).
 *
 * The height is Link's own 180.0f (d_a_alink_wolf.inc:528) rather than a new invented number; the
 * puppet is the same character on the same rig, and mHeight is what daAlink_c feeds this.
 */
f32 daRemotePlayer_c::checkWindWallRate(const cXyz& i_windDir) {
    const f32 maxDist = daAlinkHIO_basic_c0::m.mMaxWindInfluenceDist;
    const f32 noWindDist = daAlinkHIO_basic_c0::m.mNoWindInfluenceDist;

    cXyz start(current.pos.x, current.pos.y + 0.5f * l_linkHeight, current.pos.z);
    cXyz end = start - i_windDir * maxDist;

    mWindLinChk.Set(&start, &end, this);
    if (!dComIfG_Bgsp().LineCross(&mWindLinChk) || dComIfG_Bgsp().GetWallCode(mWindLinChk) == 0xA) {
        mWindChkHit = false;
        mWindChkDist = -1.0f;
        mWindWallRate = 1.0f;
        return 1.0f;
    }

    f32 rate = (1.0f / (maxDist - noWindDist)) * (start.abs(mWindLinChk.GetCross()) - noWindDist);
    if (rate < 0.0f) {
        rate = 0.0f;
    }

    mWindChkHit = true;
    mWindChkDist = start.abs(mWindLinChk.GetCross());
    mWindWallRate = rate;
    return rate;
}

/**
 * Watch for the twilight dissolve switching itself on behind our back.
 *
 * ★ This exists because A5 — the puppet turning into black warp particles — could not be settled by
 * reading the code. Two separate chains of reasoning produced OPPOSITE predictions about which
 * models should break, and Stuart's report (body and head go, face and hands stay) matched neither
 * cleanly. A spawn-time snapshot would not settle it either: if something enables the dissolve
 * LATER, a snapshot taken in createHeap looks perfectly healthy.
 *
 * So sample every tick and speak only on a change. The signature is (stage count, last stage's
 * texmap); a last stage reading from texmap 3 IS the dissolve (d_resorce.cpp:127-195). Four reads
 * per frame, and silence unless something actually moves.
 *
 * Note the state being watched belongs to the shared model DATA, which is the point: if the local
 * player's warp — or anything else — is reaching across into the puppet's materials, this is where
 * it shows up, with a frame number attached.
 */
void daRemotePlayer_c::checkMaterialDrift() {
    J3DModelData* models[l_watchedModelNum] = {
        mpModelMorf != NULL && mpModelMorf->getModel() != NULL ?
            mpModelMorf->getModel()->getModelData() :
            NULL,
        mpHeadModel != NULL ? mpHeadModel->getModelData() : NULL,
        mpHandModel != NULL ? mpHandModel->getModelData() : NULL,
        mpFaceModel != NULL ? mpFaceModel->getModelData() : NULL,
    };
    static const char* const names[l_watchedModelNum] = {"body", "head", "hands", "face"};

    for (int i = 0; i < l_watchedModelNum; i++) {
        const u16 signature = material_signature(models[i]);
        if (signature == mMaterialSig[i]) {
            continue;
        }

        const u16 previous = mMaterialSig[i];
        mMaterialSig[i] = signature;
        if (previous == 0xFFFF) {
            continue;  // First sample is the baseline, not a change.
        }

        const u8 texMap = (u8)(signature & 0xFF);
        Log.warn("Puppet {} {} material CHANGED: stages {}->{}, lastTexMap {}->{}{}", mPlayerId,
            names[i], previous >> 8, signature >> 8, (u8)(previous & 0xFF), texMap,
            texMap == 3 ? "  <<< TWILIGHT DISSOLVE JUST TURNED ON" : "");
    }
}

void daRemotePlayer_c::setHatAngle() {
    if (mpHeadModel == NULL) {
        return;
    }

    J3DModelData* headData = mpHeadModel->getModelData();
    if (headData == NULL || headData->getJointNum() <= l_capRootJointNo) {
        return;
    }

    cXyz anchorPos;
    mDoMtx_multVecZero(mpHeadModel->getAnmMtx(l_capRootJointNo), &anchorPos);

    /* First tick: there is no previous sample, so the difference below would be the whole distance
     * from wherever the matrix happened to start. Seed it and let the next tick measure properly.
     */
    if (!mSwayInited) {
        mSwayInited = true;
        mCapAnchorPrev = anchorPos;
        mPrevPos = current.pos;
    }

    /* ★ TWO samples, at two different points, because the original takes two — and collapsing them
     * into one is a mistake that cannot be fixed by choosing the better point, only by splitting:
     *
     *   - The BEND (field_0x35b8) is sampled at current.pos in daAlink_c::setWindSpeed
     *     (d_a_alink.cpp:5521), then shelter-attenuated and multiplied by mMaxWindSpeed.
     *   - The FLUTTER energy is sampled at the HAT ANCHOR in setHatAngle itself (:2577) and used
     *     raw, 0..1 — never attenuated, never scaled.
     *
     * They are genuinely different quantities from different places, so one sample serving both
     * necessarily gets one of them wrong whichever point is picked. Outdoors on open ground the two
     * points see the same ambient wind and this changes nothing; near a local wind source it does.
     */
    cXyz bendWindDir;
    f32 bendWindPower;
    dKyw_get_AllWind_vec(&current.pos, &bendWindDir, &bendWindPower);

    cXyz flutterWindDir;
    f32 windPower;
    dKyw_get_AllWind_vec(&anchorPos, &flutterWindDir, &windPower);

    /* ★ Build the wind the way daAlink_c::setWindSpeed does (d_a_alink.cpp:5521-5539), using the
     * game's OWN scale constant. This went wrong twice before, in opposite directions, and both
     * mistakes are worth not repeating:
     *
     *   1. The first version invented the scale as 30.0f, on the belief that Link's
     *      mpHIO->mBasic.m.mMaxWindSpeed was unreachable. Stuart: "one step too windy".
     *   2. The second read daAlink_c::field_0x35b8 — Link's own already-scaled wind — reasoning
     *      that copying him exactly must be right. It is not, because setWindSpeed is not the only
     *      gate: setHatAngle itself only runs `if (!checkWolf())` (d_a_alink.cpp:18537), so with
     *      the local player in WOLF form there is no cap, no sway, and field_0x35b8 contributes
     *      nothing. Stuart, playing a wolf save: "the base wind amount is not enough now".
     *
     * The actual answer was in the tree the whole time. daAlinkHIO_basic_c0::m is a public static
     * const (d_a_alink.h:4650-4653) — the same pattern selectAnimation() already reads gait speeds
     * from — and mMaxWindSpeed is 20.0f (d_a_alink_HIO_data.inc:22). No guess, no dependence on
     * what form the local player happens to be in, and sampled at the PUPPET's position rather
     * than Link's, which is also more correct than reading his value ever was.
     *
     * The third mistake was quieter than the other two and is fixed just below: this dropped
     * checkWindWallRate, so a puppet sheltered by geometry still felt the full wind. That is why
     * the cap could still look wrong after the scale itself was right — indoors the local player's
     * wind is attenuated hard and the puppet's was not attenuated at all. */
    f32 windTargetPower = bendWindPower;
    const s32 teachWind = dKy_TeachWind_existence_chk();
    mWindWallRate = 1.0f;
    mWindChkHit = false;
    mWindChkDist = -1.0f;
    if (teachWind == 0 || windTargetPower < 0.3f) {
        windTargetPower = 0.0f;
    } else if (windTargetPower > 0.0f && teachWind != -1) {
        windTargetPower *= checkWindWallRate(bendWindDir);
    }
    windTargetPower *= daAlinkHIO_basic_c0::m.mMaxWindSpeed;

    /* ★ Latched, and loud, the first time this room has cap-bending wind at all.
     *
     * Three rounds of "the puppet's cap has no wind" were spent in rooms where `teach` is 0 — which
     * gates the bend off for the LOCAL PLAYER TOO (d_a_alink.cpp:5523), so there was never anything
     * to see and no amount of staring could have distinguished a bug from correct behaviour. The
     * flag comes from kytag02 actors placed per room (d_a_kytag02.cpp); rooms without one have no
     * teach-wind by design, and both test rooms so far had none. Walk until this line appears —
     * that is where a wind comparison actually means something. */
    if (!mLoggedWindArea && teachWind != 0) {
        mLoggedWindArea = true;
        Log.warn("Puppet {} WIND AREA: teach {} — this room HAS cap-bending wind (raw {:.2f}). A "
                 "cap comparison here is meaningful; one in a teach-0 room is not.",
            mPlayerId, teachWind, bendWindPower);
    }

    const cXyz windTarget = bendWindDir * windTargetPower;
    // Rises three times faster than it falls, so gusts arrive quickly and die away slowly.
    const f32 windRate = mWindPush.abs2() > windTargetPower * windTargetPower ? 3.0f : 1.0f;
    cLib_addCalcPos(&mWindPush, windTarget, 0.5f, windRate, 0.5f);

    const cXyz& windPush = mWindPush;
    const bool strongWind = windPush.abs2() > SQUARE(l_strongWindSpeed);

    /* Latched the first time the wind is strong enough to change the cap's behaviour. This is the
     * one number that was previously guessed, and the place Stuart saw it go wrong (the Forest
     * Temple's gusts) is somewhere autopilot cannot drive to. If the cap ever looks over-eager
     * again, this line settles the question immediately: a large magnitude here means the puppet is
     * faithfully copying a genuinely strong wind that the LOCAL player is feeling too, and anything
     * else means the fault is on this side. */
    if (strongWind && !mLoggedStrongWind) {
        mLoggedStrongWind = true;
        Log.debug("Puppet {} in strong wind: Link's push {:.1f},{:.1f},{:.1f} (len {:.1f}, "
                  "threshold {:.1f})",
            mPlayerId, windPush.x, windPush.y, windPush.z, JMAFastSqrt(windPush.abs2()),
            l_strongWindSpeed);
    }

    /* In a strong wind the original stops caring how hard the wind actually blows and pins the
     * flutter input to full (d_a_alink.cpp:2578-2582). Transcribed here because leaving it out was
     * the second half of "too windy": the flutter kept scaling past the point the original caps it.
     */
    if (strongWind) {
        windPower = 1.0f;
    }

    /* Which way the head is pointing. daAlink_c gets this as eyePos - field_0x34e0, but those are
     * head-joint-matrix * (12,-8,0) and * (0,-8,0) (d_a_alink.cpp:5596-5599), whose difference is
     * just the head joint's local X axis. Taking the axis directly says the same thing and needs no
     * body-part positions the puppet does not keep. */
    cXyz headFwd;
    mDoMtx_multVecSR(model->getAnmMtx(l_headJointNo), &cXyz::BaseX, &headFwd);

    const s16 prevPitch = mHeadPitch;
    const s16 prevYaw = mHeadYaw;

    mHeadYaw = headFwd.atan2sX_Z();
    if (cLib_distanceAngleS(mHeadYaw, shape_angle.y) > 0x7000) {
        // Head turned nearly backwards: measure the pitch the other way up or it reads inverted.
        mHeadPitch = cM_atan2s(-headFwd.y, -headFwd.absXZ());
    } else {
        mHeadPitch = headFwd.atan2sY_XZ();
    }

    /* ★ Cap-Y inputs, sampled EVERY tick and reported as peaks, because the thing being measured is
     * a per-tick delta and reading it once every 120 ticks would compare the puppet's one-tick turn
     * against 120 ticks of Link's. Peaks rather than means: the cap is thrown sideways by sharp
     * turns, and a mean over a straight run would hide exactly the events that matter.
     *
     * Link's equivalents are reconstructed from public members — field_0x3062 is his head yaw and
     * field_0x34c8 his previous cap anchor — so this is like-for-like, not inference. */
    {
        const s16 yawKickNow = (s16)(mHeadYaw - prevYaw);
        if (abs(yawKickNow) > abs(mYawKickPeak)) {
            mYawKickPeak = yawKickNow;
        }
        const f32 lateralNow = (mCapAnchorPrev - anchorPos).absXZ();
        if (lateralNow > mLateralMovePeak) {
            mLateralMovePeak = lateralNow;
        }

        const daAlink_c* tickLink = static_cast<const daAlink_c*>(dComIfGp_getLinkPlayer());
        if (tickLink != NULL && !tickLink->checkWolf() && tickLink->mpLinkHatModel != NULL) {
            if (mPrevLinkHeadYawValid) {
                const s16 linkKick = (s16)(tickLink->field_0x3062 - mPrevLinkHeadYaw);
                if (abs(linkKick) > abs(mLinkYawKickPeak)) {
                    mLinkYawKickPeak = linkKick;
                }
            }
            mPrevLinkHeadYaw = tickLink->field_0x3062;
            mPrevLinkHeadYawValid = true;
            mLinkSampled = true;

            cXyz linkAnchor;
            mDoMtx_multVecZero(tickLink->mpLinkHatModel->getAnmMtx(l_capRootJointNo), &linkAnchor);
            const f32 linkLateral = (tickLink->field_0x34c8 - linkAnchor).absXZ();
            if (linkLateral > mLinkLateralMovePeak) {
                mLinkLateralMovePeak = linkLateral;
            }
        }
    }

    f32 sinYaw;
    f32 cosYaw;
    f32 lateralLen = headFwd.absXZ();
    if (lateralLen < 0.01f) {
        sinYaw = cM_ssin(shape_angle.y);
        cosYaw = cM_scos(shape_angle.y);
    } else {
        lateralLen = 1.0f / lateralLen;
        sinYaw = headFwd.x * lateralLen;
        cosYaw = headFwd.z * lateralLen;
    }

    /* Half of however far the head turned this tick is fed back into the cap as inertia — the cap
     * lags the head rather than being welded to it (d_a_alink.cpp:2642-2648). The dead zone around
     * a quarter turn is the original's: near straight up or straight down, yaw is ill-conditioned
     * and feeding it in makes the cap spin. */
    const s16 pitchKick = (s16)(mHeadPitch - prevPitch) >> 1;
    s16 yawKick;
    if (abs(mHeadPitch) > 0x3000 && abs(mHeadPitch) < 0x5000) {
        yawKick = 0;
    } else {
        yawKick = (s16)(mHeadYaw - prevYaw) >> 1;
    }

    /* The apparent wind: how far the anchor moved, REVERSED (a head moving forward feels wind from
     * the front), plus the environment's wind, plus a constant downward bias so the cap hangs. */
    cXyz apparentWind = mCapAnchorPrev - anchorPos;

    /* ★ Standing still, the original throws the horizontal part of that away before adding the
     * environment's wind (d_a_alink.cpp:2654-2657), and this had been left out. It matters more
     * than it looks: the anchor term is measured off the head joint, so an idle animation's head
     * bob feeds a small sideways wobble into the cap every single frame even in dead calm. Link's
     * cap hangs still in a windless room; the puppet's was always faintly stirring. */
    if (mPrevPos.abs2XZ(current.pos) < 1.0f) {
        apparentWind.x = 0.0f;
        apparentWind.z = 0.0f;
    }

    apparentWind += windPush;
    apparentWind.y -= l_capGravity;

    // Kills the jitter a resting animation's sub-unit drift would otherwise put into the cap.
    if (fabsf(apparentWind.x) < 0.01f) {
        apparentWind.x = 0.0f;
    }
    if (fabsf(apparentWind.z) < 0.01f) {
        apparentWind.z = 0.0f;
    }

    /* A floor on how far the cap may swing back, taken from the body's own up axis rather than
     * world up, so it still holds when the puppet is on a slope (d_a_alink.cpp:2686-2699). Without
     * it the cap passes through the shoulders. */
    mDoMtx_stack_c::copy(model->getAnmMtx(2));
    cXyz bodyUp;
    cXyz bodyFwdPos;
    mDoMtx_stack_c::multVecSR(&cXyz::BaseY, &bodyUp);
    mDoMtx_stack_c::multVec(&cXyz::BaseX, &bodyFwdPos);

    s16 pitchFloor;
    if (bodyFwdPos.y < mDoMtx_stack_c::get()[1][3]) {
        pitchFloor = cM_atan2s(-bodyUp.y, -bodyUp.absXZ());
    } else {
        pitchFloor = bodyUp.atan2sY_XZ();
    }
    pitchFloor -= 0x3800;

    /* --- The cap chain. Three segments walked with parallel pointers, kept in the original's shape
     * because the trailing term reads each segment against the one in front of it. */
    s16* angX = &mSwayAngleX[l_capRootJointNo];
    s16* angY = &mSwayAngleY[l_capRootJointNo];
    s16* velX = &mCapVelX[0];
    s16* velY = &mCapVelY[0];

    *angX -= pitchKick;
    *angY -= yawKick;

    s16 beforeX = *angX;
    s16 beforeY = *angY;

    const f32 forward = apparentWind.z * cosYaw + apparentWind.x * sinYaw;

    // Aim the first segment down the apparent wind, but never more than 5/8 of a turn off the head.
    s16 want = cM_atan2s(apparentWind.y, -forward);
    int delta = cLib_minMaxLimit<int>(want - mHeadPitch, -0x3800, 0x3800);
    want = delta + mHeadPitch;
    if (want < pitchFloor) {
        want = pitchFloor;
    }
    delta = want - mHeadPitch;

    cLib_addCalcAngleS2(angX, delta, 5, 0x400);
    *angX = cLib_minMaxLimit<s16>(*angX + *velX, -0x3800, 0x3800);

    const s16 wantY =
        cLib_minMaxLimit<s16>(cM_atan2s(-(apparentWind.x * cosYaw - apparentWind.z * sinYaw),
                                  JMAFastSqrt(SQUARE(forward) + SQUARE(apparentWind.y))),
            -0x2800, 0x2800);

    cLib_addCalcAngleS2(angY, wantY, 5, 0x400);
    *angY = cLib_minMaxLimit<s16>(*angY + *velY, -0x2800, 0x2800);

    /* Velocity is a fifth of the distance just travelled, carried into next tick. This is what
     * makes the cap overshoot and settle rather than tracking the target rigidly. */
    *velX = 0.2f * (*angX - beforeX);
    *velY = 0.2f * (*angY - beforeY);

    s16 pitchSum = *angX + mHeadPitch;
    angX++;
    angY++;
    velX++;
    velY++;

    for (int i = 1; i < 3; i++, angX++, angY++, velX++, velY++) {
        // Half of whatever the segment in front just did, propagated down as a delayed tug.
        ANGLE_SUB_2(angX[0], ((s16)(angX[-1] - beforeX) >> 1));
        ANGLE_SUB_2(angY[0], ((s16)(angY[-1] - beforeY) >> 1));
        beforeX = angX[0];
        beforeY = angY[0];

        // Trailing segments have no target of their own; they relax toward straight.
        cLib_addCalcAngleS2(angX, 0, 5, 0x400);
        cLib_addCalcAngleS2(angY, 0, 5, 0x400);

        angX[0] = cLib_minMaxLimit<s16>(angX[0] + *velX, -0x1000, 0x1000);

        // The floor applies to the accumulated bend, not to each segment on its own.
        pitchSum += angX[0];
        if (pitchSum < pitchFloor) {
            ANGLE_ADD_2(angX[0], pitchFloor - pitchSum);
            pitchSum = pitchFloor;
        }

        angY[0] = cLib_minMaxLimit<s16>(angY[0] + *velY, -0x2000, 0x2000);

        *velX = 0.2f * (angX[0] - beforeX);
        *velY = 0.2f * (angY[0] - beforeY);
    }

    /* Flutter. A cosine running through the three segments at a phase offset, so a ripple travels
     * down the cap. Amplitude and rate both scale with how hard things are blowing, which is why a
     * standing puppet's cap still stirs slightly instead of freezing (d_a_alink.cpp:2778-2796). */
    const f32 windEnergy = 25.0f * (windPower * windPower);
    f32 rate = (windEnergy + 0.65f * mCapAnchorPrev.abs(anchorPos)) / 30.0f;
    if (rate > 1.0f) {
        rate = 1.0f;
    }

    f32 amplitude = rate;
    if (strongWind) {
        rate = 3.5f;
        amplitude = 1.0f;
    }

    const s16 step = 1500.0f + 4060.0f * rate;
    mFlutterPhase += step;

    for (int i = 0; i < 3; i++) {
        mFlutterAngle[i] =
            amplitude * cM_deg2s((i + 1) * 4) * cM_scos(mFlutterPhase - ((i + 3) * step));
    }

    /* One line, latched, on the first tick the cap has actually bent. Same reason as the blink and
     * the gaze: "the callback is attached" and "the callback is attached and every angle is still
     * zero" look identical from outside, and the second is what a wrong anchor joint or a wind term
     * stuck at zero would produce. Prints all three segments so a chain that moves only at the root
     * — i.e. one where the trailing term is broken — is distinguishable from one that works. */
    if (!mLoggedFirstSway && abs(mSwayAngleX[l_capRootJointNo]) > l_swayLoggedAngle) {
        mLoggedFirstSway = true;
        Log.debug("Puppet {} cap swaying: X {},{},{}  Y {},{},{}", mPlayerId, mSwayAngleX[7],
            mSwayAngleX[8], mSwayAngleX[9], mSwayAngleY[7], mSwayAngleY[8], mSwayAngleY[9]);
    }

    /* ★ The wind as a BREAKDOWN, not a magnitude. Every previous attempt at this read one number at
     * the end of the chain and adjusted a constant in front of it, and got the direction wrong
     * twice. These are the places the value can be lost — the ambient sample, the teach-wind gate,
     * the shelter cast, the smoothing — printed next to the same ambient sample taken at the LOCAL
     * player's own feet. If the two raw figures agree and the rates do not, the shelter cast is at
     * fault. If the raws disagree, the puppet is standing somewhere the wind genuinely differs. If
     * everything agrees and the pushes still do not, only then is the scale worth touching.
     *
     * ★ Gated separately from the cap comparison below, and that separation is the point. The cap
     * comparison is a spawn-time sanity check and stops a few seconds in; the wind question can
     * only be answered somewhere windy, and somewhere windy is somewhere the player has to WALK to.
     * This samples only while there is wind to describe, so a report from an open field arrives
     * with its own numbers attached instead of costing another trip out there. */
    if (mWindLogCount < l_windLogCount && (bendWindPower > 0.0f || mWindPush.abs2() > 0.0f)) {
        mWindLogTicks++;
        if (mWindLogTicks % l_windLogPeriod == 0) {
            mWindLogCount++;
            const daAlink_c* windLink = static_cast<const daAlink_c*>(dComIfGp_getLinkPlayer());
            f32 linkRaw = -1.0f;
            if (windLink != NULL) {
                cXyz linkPos = windLink->current.pos;
                cXyz linkWindDir;
                dKyw_get_AllWind_vec(&linkPos, &linkWindDir, &linkRaw);
            }
            /* ★ Both SPEEDS go on this line, and they are not decoration. The cap's lateral swing
             * is driven by how far the cap anchor moved, so a walking character's cap swings and a
             * standing one's hangs — by design, and confirmed by Stuart when the idle hang was
             * added. A comparison taken while one side walks and the other stands therefore shows a
             * large difference that has NOTHING to do with wind, and that is exactly what the first
             * human-form comparison caught: P1 walking, puppet parked. Trust the cap angles only
             * when these two numbers are close. */
            Log.debug("Puppet {} wind #{}: bend raw {:.2f} (P1 raw {:.2f}) flutter {:.2f} | "
                      "teach {} | rate {:.2f} (hit {}, dist {:.0f}) | push {:.2f} vs P1 {:.2f} | "
                      "speed {:.2f} vs P1 {:.2f}",
                mPlayerId, mWindLogCount, bendWindPower, linkRaw, windPower, teachWind,
                mWindWallRate, mWindChkHit ? "yes" : "no", mWindChkDist,
                JMAFastSqrt(mWindPush.abs2()),
                windLink != NULL ? JMAFastSqrt(windLink->field_0x35b8.abs2()) : -1.0f, mNetSpeed,
                windLink != NULL ? windLink->speedF : -1.0f);

            /* ★ The cap's SIDEWAYS swing, broken into its two inputs, because Stuart's report is
             * that the puppet's cap pitches like his but never gets thrown out to the side — and
             * the angle log confirms it (his Y pinned at the ±0x2800 clamp, the puppet's near 0).
             *
             * Cap Y has exactly two drivers, and they need separating before anything is changed:
             *   1. The per-tick HEAD YAW CHANGE, fed in as inertia (`spA`, d_a_alink.cpp:2646).
             *      Network rotation arrives interpolated, so the puppet's per-tick delta may simply
             *      be far smaller than a human yanking the stick — a replication-fidelity problem,
             *      not a hat one.
             *   2. The LATERAL component of cap-anchor motion, which is what `sp10` is built from
             *      (:2730). If the anchor barely moves sideways, there is no target to swing to.
             *
             * Both of Link's are reconstructable from public members — field_0x3062 is his head yaw
             * and field_0x34c8 his previous cap anchor — so this is a like-for-like comparison
             * rather than an inference. Whichever column is small on the puppet and large on his is
             * the one to fix; if BOTH match, the fault is downstream and the inputs are innocent.
             */
            /* ★ Say so when there was no local cap to measure, rather than printing zeroes. A wolf
             * never runs setHatAngle, so his peaks would sit at 0 and read as "Link's cap does not
             * swing either" — which is the exact shape of mistake (zeroes from a system that was
             * not running, taken as data) that sent an earlier wind fix the wrong way. */
            if (!mLinkSampled) {
                Log.debug("Puppet {} capY inputs #{}: peak yaw kick/tick {} | peak lateral anchor "
                          "move/tick {:.2f} | capY {} | NO P1 COMPARISON (wolf or no local player)",
                    mPlayerId, mWindLogCount, mYawKickPeak, mLateralMovePeak,
                    mSwayAngleY[l_capRootJointNo]);
            } else {
                Log.debug(
                    "Puppet {} capY inputs #{}: peak yaw kick/tick {} vs P1 {} | peak lateral "
                    "anchor move/tick {:.2f} vs P1 {:.2f} | capY {} vs P1 {}",
                    mPlayerId, mWindLogCount, mYawKickPeak, mLinkYawKickPeak, mLateralMovePeak,
                    mLinkLateralMovePeak, mSwayAngleY[l_capRootJointNo],
                    windLink != NULL ? windLink->field_0x3040[l_capRootJointNo] : 0);
            }

            mYawKickPeak = 0;
            mLinkYawKickPeak = 0;
            mLateralMovePeak = 0.0f;
            mLinkLateralMovePeak = 0.0f;
            mLinkSampled = false;
        }
    }

    /* ★ Side-by-side with the LOCAL player's own cap, which is the only way to answer "is the
     * puppet's sway right" without eyes on it. Both characters are Link on the same rig, so in the
     * two-instance test the HOST's own numbers and the GUEST's puppet-of-the-host numbers describe
     * the same character at the same moment and should agree. daAlink_c's members are public, so
     * this reads his angle arrays directly rather than inferring anything. */
    if (mCapCompareTicks < l_capCompareCount * l_capComparePeriod) {
        mCapCompareTicks++;
        if (mCapCompareTicks % l_capComparePeriod == 0) {
            const daAlink_c* link = static_cast<const daAlink_c*>(dComIfGp_getLinkPlayer());

            /* ★ Skip this entirely in wolf form. daAlink_c::setHatAngle only runs `if
             * (!checkWolf())` (d_a_alink.cpp:18537), so a wolf's angle arrays sit at zero and
             * comparing against them says nothing — reading them as a mismatch is what sent the
             * previous wind fix the wrong way. Print the form instead, so a run against a wolf save
             * is self-evidently not a comparison. */
            if (link == NULL) {
                Log.debug("Puppet {} cap @{}: X{} Y{} | wind {:.2f} | no local player", mPlayerId,
                    mCapCompareTicks, mSwayAngleX[l_capRootJointNo], mSwayAngleY[l_capRootJointNo],
                    JMAFastSqrt(mWindPush.abs2()));
            } else if (link->checkWolf()) {
                Log.debug("Puppet {} cap @{}: X{} Y{} | wind {:.2f} | P1 is a WOLF, no cap to "
                          "compare against",
                    mPlayerId, mCapCompareTicks, mSwayAngleX[l_capRootJointNo],
                    mSwayAngleY[l_capRootJointNo], JMAFastSqrt(mWindPush.abs2()));
            } else {
                Log.debug("Puppet {} cap vs P1 @{}: puppet X{} Y{} | P1 X{} Y{} | wind {:.2f} vs "
                          "{:.2f} | floor {} pitch {}",
                    mPlayerId, mCapCompareTicks, mSwayAngleX[l_capRootJointNo],
                    mSwayAngleY[l_capRootJointNo], link->field_0x302c[l_capRootJointNo],
                    link->field_0x3040[l_capRootJointNo], JMAFastSqrt(mWindPush.abs2()),
                    JMAFastSqrt(link->field_0x35b8.abs2()), pitchFloor, mHeadPitch);
            }
        }
    }

    mCapAnchorPrev = anchorPos;
    mPrevPos = current.pos;
    setHairAngle(&apparentWind, sinYaw, cosYaw);
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

/// Keep the puppet lit by the floor and the room it is actually standing on.
///
/// Both inputs otherwise sit at defaults that quietly mis-light it, which is what Stuart saw as
/// "the body is lit a bit differently than p1":
///
///  - `tevStr.YukaCol` is left at 0xFF by dKy_tevstr_init (d_kankyo.cpp:9507). At 0xFF,
///    settingTevStruct_colget_player takes its fallback branch and pins the light-influence ratio
///    to 1.0 (d_kankyo.cpp:3166-3170), where Link's is YukaCol/100 (:3160-3165). That ratio scales
///    both the actor ambient colour and the room light colours, so on any floor whose colour index
///    is not 100 the puppet is lit at a different intensity from Link on the very same tile.
///  - `tevStr.room_no` is seeded from home.roomNo at spawn (f_op_actor.cpp:493) and never updated,
///    so the puppet keeps its spawn room's six light vectors forever
///    (dKy_setLight_nowroom_actor, d_kankyo.cpp:8775).
///
/// Mirrors daMidna_c::setRoomInfo (d_a_midna.cpp:1171-1182) — the game's own answer for a companion
/// actor with no ground check of its own. Deliberately NOT daAlink_c's version, which reads the
/// collision result out of his own dBgS_LinkAcch. Midna's reverb line is dropped: a puppet makes no
/// sound of its own.
///
/// fopAcM_gc_c's ground check is static shared state, so the result has to be consumed in the same
/// breath as the check rather than cached (f_op_actor_mng.h:874-889).
void daRemotePlayer_c::setRoomInfo() {
    int room_no;
    if (fopAcM_gc_c::gndCheck(&current.pos)) {
        room_no = fopAcM_gc_c::getRoomId();
        tevStr.YukaCol = fopAcM_gc_c::getPolyColor();
    } else {
        // Over a hole, mid-warp, or handed a pose with no floor under it. Keep the last floor
        // colour and fall back to the room the local player is in, which is the room the puppet is
        // being drawn into anyway.
        room_no = dComIfGp_roomControl_getStayNo();
    }
    tevStr.room_no = room_no;
    fopAcM_SetRoomNo(this, room_no);
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
    // Independent of the body: the puppet blinks while standing still as much as while running,
    // which is the whole point — a face frozen mid-stare is what reads as "not a real player".
    playFaceTextureAnime();
    // Before setMatrix, so the ground check runs against the pose the puppet is about to be drawn
    // at rather than the previous tick's.
    setRoomInfo();
    setMatrix();
    // After setMatrix, not before: the aim is measured from the head joint's world matrix, which
    // only exists once modelCalc() has run.
    setEyeMove();
    // Same reason, and in the same place daAlink_c puts it (d_a_alink.cpp:18530-18538): the sway is
    // integrated from how far this tick's matrices moved, and the joint callback applies the result
    // during the NEXT tick's calc.
    setHatAngle();
    // Diagnostic for A5, silent unless the material state actually moves. Last, so it reports the
    // state the draw pass is about to use.
    checkMaterialDrift();
    return 1;
}

static int daRemotePlayer_Execute(daRemotePlayer_c* i_this) {
    return i_this->execute();
}

/// One sub-model, lit like the body. This is daAlink_c::modelDraw with its draw flag at 0
/// (d_a_alink.cpp:19375-19386) — which is the call Link uses for all four of HIS models
/// (`:19598`, `:19696`, `:19707`, `:19713`); `basicModelDraw` is only the lantern and the pause
/// menu.
///
/// The daMirror_c::entry line is what separates the two under that flag, and without it puppets are
/// simply **absent from mirrored panes** while the local player is reflected — a mirror showing one
/// player in a two-player room. It is safe to call unconditionally: the static returns 0 when no
/// mirror actor exists (d_com_static.cpp:400-405), and the packet drops entries past 64 rather than
/// overflowing (d_a_mirror.cpp:95-97), against a handful in use.
void daRemotePlayer_c::drawModel(J3DModel* i_model) {
    if (i_model == NULL) {
        return;
    }
    g_env_light.setLightTevColorType_MAJI(i_model, &tevStr);
    mDoExt_modelEntryDL(i_model);
    daMirror_c::entry(i_model);
}

int daRemotePlayer_c::draw() {
    if (!mHasPose) {
        return 1;
    }

    // 10 is the light type daAlink_c uses for human Link (d_a_alink.cpp:19470), so the puppet is
    // lit consistently with the local player rather than as scenery.
    //
    // That type carries a sting: the 9/10 path writes SCENE-WIDE state, not just our own tevStr.
    // settingTevStruct_colget_player drives the room-colour crossfade machine in g_env_light
    // (d_kankyo.cpp:3185-3197) and the 9/10 branch parks plight_near_pos (:4060-4062), which
    // dDlst_shadowReal_c::set reads back (d_drawlist.cpp:1318). Until now daAlink_c was the only
    // caller of type 9/10 in the whole tree; a puppet standing in a differently-coloured room makes
    // two callers per frame, which flips UseCol/pat_ratio back and forth and would disturb the
    // LOCAL player's lighting and shadow.
    //
    // So take the reading and put the scene back exactly as we found it. The puppet keeps the
    // tevStr the call computed for it; what it gives up is a vote in a crossfade that should be
    // driven by the local player alone. Cheap, and entirely contained in Dusk code.
    const u8 savedUseCol = g_env_light.UseCol;
    const u8 savedPrevCol = g_env_light.PrevCol;
    const f32 savedPatRatio = g_env_light.pat_ratio;
    const cXyz savedPlightNearPos = g_env_light.plight_near_pos;

    g_env_light.settingTevStruct(10, &current.pos, &tevStr);

    g_env_light.UseCol = savedUseCol;
    g_env_light.PrevCol = savedPrevCol;
    g_env_light.pat_ratio = savedPatRatio;
    g_env_light.plight_near_pos = savedPlightNearPos;
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
