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
#include "dusk/frame_interpolation.h"
#include "dusk/logging.h"
#include "dusk/player_equip.hpp"
#include "f_op/f_op_actor_mng.h"
// The sword and sheath resource indices. Safe to include even though the per-outfit headers are
// not: Alink.h's joint enums are all prefixed by their own model (AL_SWA_JNT, AL_PODM_JNT), so
// nothing in it collides the way AL_JNT in Kmdl.h collides with BL_JNT in Bmdl.h.
#include "res/Object/Alink.h"
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
    /* BODY-model materials whose shapes are this outfit's plain hands — daAlink_c's field_0x06d8
     * and field_0x06dc, which an animation selects by asking for hand index 0xFE. They differ per
     * outfit and are NOT on the hands model; see setDrawHand(). */
    u16 defaultHandMatNo[2];
    /* One BODY-model material daAlink_c hides as it builds the model and never shows again for a
     * player in this state, or l_noBodyMat for none. Two outfits have one: the hero's clothes hide
     * material 16 outright (d_a_alink_wolf.inc:471), and the Zora armour hides material 9 — its
     * mask — which only comes back for checkZoraWearMaskDraw(), a Zora-swimming state a puppet has
     * no way to be in yet (:447-449, :488-490). */
    u16 hiddenBodyMatNo;
};

/* "This outfit hides nothing." Not 0 — material 0 is a real material on every one of these. */
const u16 l_noBodyMat = 0xFFFF;

/* Link is four models, not one — body, head, hands and face — and the outfits do not name them
 * consistently (Zora and magic armour reuse al_hands.bmd; only Zora has its own face). Taken from
 * the outfit branches of daAlink_c::setLinkModel, d_a_alink_wolf.inc:328-377.
 */
/* The per-outfit numbers on the right are the four wear branches of daAlink_c::setLinkModel
 * (d_a_alink_wolf.inc:434-477), in the same order the branches test for them. The first branch
 * there — checkNoResetFlg2(FLG2_UNK_80000) — is deliberately not represented: it is a transient
 * state of the LOCAL player, not one of the four archives, and a puppet is never in it. */
const OutfitArc l_outfits[] = {
    // arc      body       head            hands            face             hands  hide
    {"Bmdl", "bl.bmd", "bl_head.bmd", "bl_hands.bmd", "al_face.bmd", {3, 4}, l_noBodyMat},
    {"Kmdl", "al.bmd", "al_head.bmd", "al_hands.bmd", "al_face.bmd", {11, 12}, 16},
    {"Zmdl", "zl.bmd", "zl_head.bmd", "al_hands.bmd", "zl_face.bmd", {4, 5}, 9},
    {"Mmdl", "ml.bmd", "ml_head.bmd", "al_hands.bmd", "al_face.bmd", {4, 5}, l_noBodyMat},
};

const int l_outfitNum = sizeof(l_outfits) / sizeof(l_outfits[0]);

/* Index into l_outfits used when we have nothing better — the hero's clothes. */
const int l_defaultOutfit = 1;

/* Where object archives live on disc. dRes_info_c::set builds "<path><name>.arc" from these
 * (d_resorce.cpp:58-62); it is the same string dRes_control_c::setObjectRes passes
 * (d_resorce.h:96).
 */
const char l_objectPath[] = "/res/Object/";

/* Link's permanent archive — the swords and both sheaths live here. Mounted once at boot beside
 * "Always" (d_s_logo.cpp:1484) and never freed while a scene exists, which is what makes it safe to
 * read directly instead of mounting privately; see setupEquipModels() for the full argument.
 *
 * Spelled out rather than taken from daAlink_c::getArcName() because that returns the OUTFIT
 * archive's name (d_a_alink.cpp:100-102) — a different string that happens to be reachable through
 * a similarly-named accessor. */
const char l_alinkArcName[] = "Alink";

/* The wooden sword, which is the one piece of equipment that does NOT live in "Alink". daAlink_c
 * loads it from the outfit archive (d_a_alink_wolf.inc:415), so the puppet takes it from its own
 * private mount of that archive. By name, for the same reason the body is: the outfit headers
 * cannot be included here. */
const char l_woodSwordResName[] = "al_SWB.bmd";

/* Body joints the sub-models hang off. daAlink_c uses these same three literals: the head and face
 * both ride joint 4 (d_a_alink.cpp:5968-5970) and the hands model's own joints 1 and 2 are
 * overwritten with the body's hand joints after its calc (d_a_alink.cpp:19013-19014).
 */
const u16 l_headJointNo = 4;
const u16 l_leftHandJointNo = 9;
const u16 l_rightHandJointNo = 0xE;

/* Where Link's body is split between its two animation halves, straight out of
 * daAlink_c::changeModelDataDirect (d_a_alink_swindow.inc:167-169): joint 0 and joint 16 take the
 * UNDER animation, joint 1 takes the UPPER one.
 *
 * Three assignments cover all thirty-five joints because a joint with no calculator of its own
 * inherits the nearest ancestor's — J3DJoint::recursiveCalc installs a joint's calculator as the
 * current one, walks its children, then puts the previous one back (J3DJoint.cpp:195-235). So joint
 * 1 hands the upper animation to the whole torso-and-arms subtree, and joint 16 takes the legs back
 * off it.
 *
 * These are the same three numbers for every human outfit: changeModelDataDirect runs on whichever
 * body is currently mounted and does not branch on which one it is.
 */
const u16 l_underRootJointNo = 0;
const u16 l_upperRootJointNo = 1;
const u16 l_underLegJointNo = 16;

/* How many alternative hand poses the hands model holds, as separate shapes 0..10. Exactly the
 * bound daAlink_c::setLinkModel hides at build time (d_a_alink_wolf.inc:495-498) before setDrawHand
 * starts showing one of them per hand. */
const u16 l_handShapeNum = 11;
/* The hand index meaning "not one of those eleven — use the BODY model's own plain hand". It is
 * daAlink_c's own sentinel, tested by name at d_a_alink.cpp:19017 and :19038 and appearing all over
 * m_anmDataTable's hand columns. */
const u8 l_defaultHandIdx = 0xFE;

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

/**
 * The one place a puppet turns model data into a model, for BOTH mdlFlags values.
 *
 * ★ The warp bracket is the whole reason this is shared rather than written out twice. It is
 * subtle, it has already produced one visible bug, and the two entry points below differ ONLY in
 * the mdlFlags they pass — so a second copy of the bracket is a second chance to get it wrong.
 *
 * i_what names the model in the log line. It is not decoration: the bracket's effect is invisible
 * from the model data afterwards, because the trailing offWarpMaterial restores exactly what the
 * leading test read. Without a line saying which models took it, "is the sheath the one answering
 * wrong" is unanswerable from a log, and answering it by reasoning is what produced the last two
 * wrong predictions about this material.
 */
J3DModel* init_model_common(
    J3DModelData* i_modelData, u32 i_mdlFlags, u32 i_diffFlags, const char* i_what) {
    if (i_modelData == NULL) {
        return NULL;
    }

    const bool warpMaterial = has_warp_material(i_modelData);
    const u16 sigBefore = material_signature(i_modelData);

    if (warpMaterial) {
        dRes_info_c::onWarpMaterial(i_modelData);
        i_diffFlags |= 0x2000400;
    }

    J3DModel* model = mDoExt_J3DModel__create(i_modelData, i_mdlFlags, i_diffFlags | 0x11000084);
    const u16 sigAtCreate = material_signature(i_modelData);

    if (warpMaterial) {
        dRes_info_c::offWarpMaterial(i_modelData);
    }

    /* sigAtCreate is the one that matters: it is the material state the model was BUILT from, and
     * with 0x2000400 the model keeps its own copy of it. sigBefore says what the shared data
     * happened to be in when we got here, which depends on who created a model from it first. */
    Log.debug("Puppet init_model {}: warp bracket {} | data sig 0x{:04x} -> built from 0x{:04x} "
              "-> restored 0x{:04x}",
        i_what, warpMaterial ? "TAKEN" : "not taken", sigBefore, sigAtCreate,
        material_signature(i_modelData));

    return model;
}

J3DModel* init_model(J3DModelData* i_modelData, u32 i_diffFlags, const char* i_what) {
    return init_model_common(i_modelData, 0x80000, i_diffFlags, i_what);
}

/**
 * init_model with mdlFlags 0 instead of 0x80000 — daAlink_c::initModelEnv (d_a_alink.h:3736).
 *
 * The difference is not cosmetic and the pairing is not free choice: he builds the master sword and
 * its sheath this way and the ordon sword and sheath the other, and each model is authored for the
 * flags it is given. Kept as a separate entry point with the original's name so the call sites read
 * as a diff against his.
 */
J3DModel* init_model_env(J3DModelData* i_modelData, u32 i_diffFlags, const char* i_what) {
    return init_model_common(i_modelData, 0, i_diffFlags, i_what);
}

/* The four of daAlink_c's animations the puppet can be in.
 *
 * ★ These are ANIMATION IDS, not BCK resource indices, and that is the point. daAlink_c's
 * m_anmDataTable row for an id carries everything about how that animation is meant to be drawn:
 * which BCK to stream (m_bckData.m_underID) AND which hand pose belongs on each hand
 * (m_handIndexL/R). The puppet used to hold the resource indices directly and pick its hands
 * separately, which is exactly how it ended up walking around with the pause menu's sword-and-
 * shield grips — two lists that could disagree, and did. Going through the id means there is one
 * table describing a gait and it is the game's own.
 *
 * Link's animations do not live in the body archive — they are in AlAnm, mounted in ARAM at boot
 * and streamed by the index this table hands back.
 */
const daAlink_c::daAlink_ANM l_idleAnm = daAlink_c::ANM_WAIT;
const daAlink_c::daAlink_ANM l_walkAnm = daAlink_c::ANM_WALK;
/* Pairs with ANM_WALK in m_anmDataTable (d_a_alink.cpp:301-302) — its own cycle, so this is what
 * the local player runs on, not a faster playback of the walk.
 *
 * ★ It is also the ONE animation of these four whose two halves differ: {DASHS, DASHA}, where the
 * other three name a single resource twice. That matters because it is the bug Stuart reported —
 * "the running animation on the puppet is for link when he holds a sword and shield (but he has
 * nothing equipped)" — and the table says exactly that in his favour. With a sword equipped
 * daAlink_c does not take this row at all: getMainBckData redirects ANM_RUN to m_mainBckSword[3],
 * which is {DASHS, DASHS} (d_a_alink.cpp:246, :6939-6942). So DASHS *is* the sword-carrying dash
 * and DASHA is the empty-handed one, and a puppet that played only the under half was playing the
 * sword-carrying arms on a character holding nothing. Source-derived, not an eyeball.
 */
const daAlink_c::daAlink_ANM l_runAnm = daAlink_c::ANM_RUN;
/* The sharp turn: the skid Link plants when the stick is flicked back at speed. This is ANM_SLIP's
 * m_underID in the same table (index 0x028, d_a_alink.cpp:323), reached from
 * daAlink_c::procSlipInit (d_a_alink.cpp:16673-16675).
 *
 * ★ There is no left/right variant to get backwards, and that is worth stating because two other
 * "turn" animations in this game DO have one and are MISNAMED in the decomp header: ANM_STEP_TURN
 * (STEPL) and ANM_SMALL_GUARD (STEPR) are the left and right halves of one standing pivot — the
 * game picks between them purely on the sign of the yaw delta (d_a_alink.cpp:7709-7713 and
 * :17776-17779) and tests them as a pair (:15539). ANM_SLIP has no such twin: its table entry is
 * {SLIP, SLIP}, because the skid is a straight-ahead brake. Link's actual 180 happens AFTERWARDS,
 * in PROC_MOVE_TURN, and that is carried by shape_angle.y, which the puppet already replicates
 * exactly (player_bridge.cpp samples link->shape_angle.y). So the pose and the yaw cannot disagree
 * about direction here — there is only one direction the pose can mean.
 */
const daAlink_c::daAlink_ANM l_slipAnm = daAlink_c::ANM_SLIP;

/* An animation's row in daAlink_c's table — the one place the puppet learns anything about a gait.
 * The table is a public static of the class (d_a_alink.h:3919) with constant initialisers, so this
 * is a plain array read with no ordering or lifetime concerns. */
const daAlink_AnmData& anm_data(daAlink_c::daAlink_ANM i_anm) {
    return daAlink_c::m_anmDataTable[i_anm];
}

/**
 * The pair of BCK resources to stream for an animation, given what the sender is holding.
 *
 * ★ daAlink_c does NOT read m_anmDataTable's row directly; he goes through getMainBckData
 * (d_a_alink.cpp:6918-6947), which substitutes a different pair when he is holding something. This
 * is that function, restricted to the substitution a puppet can currently be subject to, and the
 * restriction is stated rather than assumed:
 *
 *   - The sword branch (:6939-6942) fires on `mEquipItem == 0x103` over the id range
 *     ANM_ATN_WAIT_LEFT..ANM_SWIM_WAIT. `mEquipItem == 0x103` is precisely what the sender puts in
 *     kPlayerEquipSwordInHand (player_bridge.cpp), so this is his answer, not a guess at it.
 *   - The kandelaar, shield-guard and fishing-rod branches are all keyed on state that is not on
 *     the wire, so they cannot fire here yet. Each is a separate byte away, not a redesign.
 *
 * Of the four animations a puppet can be in, exactly ONE row differs between the two tables:
 *
 *     ANM_WAIT  0x19  outside the sword range entirely
 *     ANM_WALK  0x12  {WALKS, WALKS} in both  (d_a_alink.cpp:696 and :245)
 *     ANM_RUN   0x13  {DASHS, DASHA} plain -> {DASHS, DASHS} armed  (:697 and :246)
 *     ANM_SLIP  0x28  outside the sword range entirely
 *
 * So the whole of the visible fix is ANM_RUN's UPPER half. That is exactly the thing Stuart could
 * see — "the puppet ... [isn't] using the sword running animation" — because the under half, the
 * legs, was already DASHS either way and only the arms were wrong.
 */
const daAlink_BckData& anm_bck_data(daAlink_c::daAlink_ANM i_anm, u8 i_equip) {
    if ((i_equip & dusk::mp::kPlayerEquipSwordInHand) != 0 &&
        i_anm >= daAlink_c::ANM_ATN_WAIT_LEFT && i_anm < daAlink_c::ANM_STEP_TURN)
    {
        return daAlink_c::m_mainBckSword[i_anm - daAlink_c::ANM_ATN_WAIT_LEFT];
    }

    return anm_data(i_anm).m_bckData;
}

u16 anm_bck_idx(daAlink_c::daAlink_ANM i_anm, u8 i_equip) {
    return anm_bck_data(i_anm, i_equip).m_underID;
}

u16 anm_bck_upper_idx(daAlink_c::daAlink_ANM i_anm, u8 i_equip) {
    return anm_bck_data(i_anm, i_equip).m_upperID;
}

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

/* How far ABOVE the puppet's feet to ask the bg system for the floor.
 *
 * Not a number picked for this actor: it is dBgS_Acch's own m_gnd_chk_offset default
 * (d_bg_s_acch.cpp:65), i.e. the raise every grounded actor in the game already queries from. The
 * ground test rejects a floor at exactly the query height (cBgW::RwgGroundCheckCommon's strict
 * `cy < y`, d_bg_w.cpp:606-625) and grounded actors stand at exactly floor height, so querying
 * from the feet finds nothing at all. See the mGndChk comment in the header. */
const f32 l_gndCheckOffset = 60.0f;

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
 * out when the puppet is still. Not gravity in any physical sense — it is the bias that decides the
 * rest pose, and it is also the denominator the sideways swing is measured against, so its value
 * changes how far the cap can be blown out.
 *
 * ★ 5.0f, the `else` at d_a_alink.cpp:2673 — NOT the 2.0f at :2669. An earlier version of this file
 * had 2.0f and called it "the only branch a puppet can be in", which was backwards. See the
 * FLG0_SWIM_UP note in setHatAngle(): that flag is ON while standing on dry land, so a walking Link
 * takes the :2673 branch and 2.0f is the UNDERWATER value. Getting this wrong shrinks the swing
 * twice over — once directly, and once by suppressing the wind term that shares the branch. */
const f32 l_capGravity = 5.0f;
/* Human Link's own height (d_a_alink_wolf.inc:528). The wind-shelter line check is cast from half
 * of it, which is what daAlink_c feeds checkWindWallRate as mHeight. */
const f32 l_linkHeight = 180.0f;

/* ★ TEMPORARY — Hang 4. How many of a puppet's calcs get the step-by-step trace. The hang is
 * always on the first calc, so a few is plenty and the log stays readable. See traceCalc(). */
const u16 l_calcTraceNum = 3;

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
/* Models watched by checkMaterialDrift(), in the order body / head / hands / face, then the three
 * swords and the two sheaths.
 *
 * ★ The equipment half is not symmetry for its own sake — it is the only way to see the one hazard
 * the body half does not have. The body, head, hands and face are PRIVATE copies out of the
 * puppet's own archive mount, so nothing but this actor can change their material state. The
 * swords and sheaths from "Alink" are the SAME J3DModelData the local player draws, and
 * daAlink_c::changeWarpMaterial (d_a_alink.cpp:14998-15007) toggles the twilight dissolve straight
 * onto that shared data for his sword, shield and sheath. When he warps, the puppet's blade and
 * scabbard go with him — and the puppet drives none of the UVs that make the effect look like
 * anything (d_resorce.cpp:212-225 needs setWarpSRT every frame). This watch is what turns "the
 * sheath has a strange particle effect" into a timestamped line saying which model and when. */
const int l_watchedModelNum = 9;
const u16 l_capComparePeriod = 60;
const u16 l_capCompareCount = 8;
/* The wind breakdown samples on its own schedule, and only while there is wind, because unlike the
 * cap comparison it has to survive the walk to somewhere windy. Forty samples two seconds apart is
 * over a minute of windy time — enough to cross a field and back — and it costs nothing anywhere
 * still, which is most places. */
const u16 l_windLogPeriod = 120;
const u16 l_windLogCount = 40;

/* Dead band on the REPORTED gait, and on nothing else. What the puppet draws is a continuous blend
 * of two animations now, so there is no longer a moment where it switches; but mCurrentAnm still
 * names one of them, for the hand poses and for --mp-trace, and mNetMoveRate is an interpolated
 * value that jitters. Without this a puppet held near a band's midpoint would flip the reported
 * gait every tick and make the trace unreadable while looking identical on screen.
 */
const f32 l_gaitHysteresis = 0.05f;

/* Frames to cross-fade over when leaving the skid and rejoining the gait blend. Not one of
 * daAlink_c's numbers — his equivalent is whatever setSingleAnime was given — and it is the one
 * place the puppet still needs an explicit morf, because the skid is a single animation and the
 * blend cannot fade out of something it is not part of. */
const f32 l_gaitMorf = 5.0f;

/**
 * Which outfit archive the LOCAL player is wearing, as an index into l_outfits.
 *
 * This is now a SENDER-side question only. It used to decide what a puppet wore, which was a
 * placeholder: it was right whenever both players happened to be dressed alike and wrong the moment
 * they were not. Appearance is owned by the wearer, so the answer is sampled here, put on the wire
 * (PlayerState::outfit), and the puppet is dressed from the sender's byte instead.
 *
 * daAlink_c::setArcName (d_a_alink_swindow.inc:14-25) picks the archive name from the wear flags,
 * so his own mArcName is the authoritative answer and cannot drift from what is on his screen.
 *
 * Wolf is not an outfit swap. mArcName is Wmdl then, which is deliberately absent from l_outfits
 * because Wmdl has its own skeleton and animation set, so it falls through to the hero's clothes
 * and a wolf looks like a human Link to everyone else until transform replication exists.
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

/**
 * Load both halves of one animation row into the puppet's own heap.
 *
 * Returns false only if the UNDER half is missing — an animation with no legs is not an animation.
 * A missing upper half degrades to "both halves play the under BCK", which is what a row that names
 * one resource twice means anyway, so the puppet keeps moving with slightly wrong arms.
 *
 * ★ The second load happens only when the row names two different resources, and that test is not
 * an optimisation. It is what daAlink_c does (getUnderUpperAnime, d_a_alink.cpp:6987-6994) and the
 * reason is that a J3DAnmTransform carries its own current frame: two halves sharing one object
 * must share one frame, so exactly one frame controller may step it.
 */
bool load_gait_anm(daRemotePlayer_anm_c* o_anm, daAlink_c::daAlink_ANM i_anmID, u8 i_equip) {
    const daAlink_BckData& bck = anm_bck_data(i_anmID, i_equip);

    o_anm->mpUnder =
        static_cast<J3DAnmTransform*>(load_aram_anm(bck.m_underID, ANM_FAMILY_TRANSFORM));
    o_anm->mpUpper = NULL;

    if (o_anm->mpUnder == NULL) {
        return false;
    }

    if (bck.m_upperID != bck.m_underID) {
        o_anm->mpUpper =
            static_cast<J3DAnmTransform*>(load_aram_anm(bck.m_upperID, ANM_FAMILY_TRANSFORM));
        if (o_anm->mpUpper == NULL) {
            Log.warn("Animation {} has no upper half ({}); its arms will play the lower body's",
                static_cast<int>(i_anmID), bck.m_upperID);
        }
    }

    return true;
}

/**
 * The sword-drawn variant of a gait, reusing whatever the empty-handed one already loaded.
 *
 * daAlink_c does not need this: getUnderUpperAnime loads each half out of its own animation heap
 * every time the pair changes (d_a_alink.cpp:6960-6999), so a sword being drawn simply streams a
 * different BCK. The puppet holds its animations open for its whole life instead — it has no
 * proc-driven moment at which to reload, and a puppet whose animation is streamed on the frame the
 * wire says "sword out" would stall on the ARAM read in the middle of a run. So both variants are
 * resident and the choice is a pointer swap.
 *
 * ★ Reuse is not an optimisation here, it is a correctness rule. Two frame controllers stepping ONE
 * J3DAnmTransform advance it at double speed, because the frame lives on the animation object
 * (animePlay's pointer comparison is the guard for the same hazard). Reusing i_plain.mpUnder is
 * safe only because a gait and its sword variant are never both in the ratio packs at once —
 * selectAnimation picks one of the two and passes it to both slots when the pair collapses.
 *
 * For today's tables this loads NOTHING: ANM_RUN's two rows share DASHS as the under half, and the
 * armed upper half IS that same DASHS, which is the mpUpper == NULL case meaning "one resource on
 * both halves". The general path is written out anyway because the shield and kandelaar rows will
 * take it, and getting it wrong there would be a silent double-speed animation.
 */
bool load_gait_anm_sword(daRemotePlayer_anm_c* o_anm, const daRemotePlayer_anm_c& i_plain,
    daAlink_c::daAlink_ANM i_anmID) {
    const daAlink_BckData& plain = anm_bck_data(i_anmID, 0);
    const daAlink_BckData& sword = anm_bck_data(i_anmID, dusk::mp::kPlayerEquipSwordInHand);

    if (sword.m_underID == plain.m_underID && sword.m_upperID == plain.m_upperID) {
        // ANM_WAIT, ANM_WALK and ANM_SLIP: the two tables agree, so there is no second variant.
        *o_anm = i_plain;
        return true;
    }

    if (sword.m_underID != plain.m_underID) {
        // Nothing to share — both halves are somebody else's resources.
        return load_gait_anm(o_anm, i_anmID, dusk::mp::kPlayerEquipSwordInHand);
    }

    o_anm->mpUnder = i_plain.mpUnder;
    o_anm->mpUpper = NULL;

    if (sword.m_upperID != sword.m_underID) {
        o_anm->mpUpper =
            static_cast<J3DAnmTransform*>(load_aram_anm(sword.m_upperID, ANM_FAMILY_TRANSFORM));
        if (o_anm->mpUpper == NULL) {
            Log.warn("Armed animation {} has no upper half ({}); its arms will play the lower "
                     "body's",
                static_cast<int>(i_anmID), sword.m_upperID);
        }
    }

    return true;
}

/* Aim one frame controller at one animation. daAlink_c::commonSingleAnime's per-half half
 * (d_a_alink.cpp:7154-7200), minus the water and Zora speed scaling, which needs equipment state a
 * puppet does not have.
 *
 * The animation is wound to the starting frame here as well as in the controller: the blend
 * calculators read the pose straight off the J3DAnmTransform, so an animation that has been swapped
 * in but not wound would draw one frame of wherever it was left last time. */
void set_gait_frame_ctrl(daPy_frameCtrl_c* o_ctrl, J3DAnmTransform* i_anm, u8 i_attr, f32 i_rate,
    f32 i_startF, s16 i_endF) {
    const s16 endFrame = i_endF < 0 ? i_anm->getFrameMax() : i_endF;
    const f32 frame = i_rate < 0.0f ? static_cast<f32>(endFrame) : i_startF;

    o_ctrl->setFrameCtrl(i_attr, static_cast<s16>(i_startF), endFrame, i_rate, frame);
    i_anm->setFrame(frame);
}

/**
 * Aim one frame controller at one animation of a BLENDED pair.
 *
 * daAlink_c::commonDoubleAnime's per-animation half (d_a_alink.cpp:7025-7057), and the two things
 * it does that are easy to leave out are the two that matter:
 *
 *  - Every cycle is wound to the same NORMALISED phase (i_phase, 0..1) rather than the same frame
 *    number. Walk and run are different lengths, so equal frame numbers would put them at different
 *    points in the stride.
 *  - The rate is given per unit of phase and multiplied back up by each animation's own length, so
 *    a long cycle and a short one still complete together. Feed both their authored rates instead
 *    and they drift apart, which is a blend of two feet in different places.
 */
void set_blend_frame_ctrl(
    daPy_frameCtrl_c* o_ctrl, J3DAnmTransform* i_anm, f32 i_phaseRate, f32 i_phase) {
    const f32 frameMax = i_anm->getFrameMax();

    o_ctrl->setFrameCtrl(i_anm->getAttribute(), 0, static_cast<s16>(frameMax),
        i_phaseRate * frameMax, i_phase * frameMax);
    i_anm->setFrame(o_ctrl->getFrame());
}

/* One animation stepped by one frame controller. daAlink_c::animePlay (d_a_alink.cpp:7255-7260),
 * verbatim — the frame lives on the animation object, so advancing the controller is only half of
 * it and forgetting the second half leaves the pose frozen with no other symptom. */
void gait_anime_play(J3DAnmTransform* i_anm, daPy_frameCtrl_c* i_frameCtrl) {
    if (i_anm != NULL) {
        i_frameCtrl->updateFrame();
        i_anm->setFrame(i_frameCtrl->getFrame());
    }
}

}  // namespace

int daRemotePlayer_c::createHeap() {
    J3DModelData* modelData =
        static_cast<J3DModelData*>(own_archive_res(mOwnRes, l_outfits[mOutfit].bodyResName));
    if (modelData == NULL) {
        return 0;
    }

    if (!load_gait_anm(&mIdleAnm, l_idleAnm, 0) || !load_gait_anm(&mWalkAnm, l_walkAnm, 0) ||
        !load_gait_anm(&mRunAnm, l_runAnm, 0))
    {
        return 0;
    }

    /* The armed run. Fatal on failure alongside the three above, because it is not an extra
     * animation the puppet could do without: for today's tables it loads nothing at all and simply
     * aliases mRunAnm's DASHS onto both halves, so a failure here means anm_bck_data disagrees with
     * daAlink_c's tables about which resources exist — which would make every armed gait wrong, not
     * just this one. */
    if (!load_gait_anm_sword(&mRunSwordAnm, mRunAnm, l_runAnm)) {
        return 0;
    }

    /* The skid, loaded on the SAME family-checked path as the gaits (see anm_kind_matches: a BCK
     * arrives as J3DAnmTransformKey/TransformFull/TransformFullWithLerp, never as a bare
     * J3DAnmTransform, so this must be a family test).
     *
     * ★ Deliberately NOT joined to the fatal check above. A puppet that cannot play the turn pose
     * is a puppet with one animation missing; a puppet that fails createHeap is deleted and
     * respawned every couple of ticks forever, mounting an archive each time. Trading the first
     * failure for the second would be a bad bargain, so this one degrades to the gait instead. */
    if (!load_gait_anm(&mSlipAnm, l_slipAnm, 0)) {
        Log.warn("Puppet has no sharp-turn animation; turns will play the gait instead");
    }

    /* The body model, built on exactly the terms the other three sub-models are — init_model
     * applies the warp-material bracket and the (0x80000, 0x11000084) pair that daAlink_c's own
     * initModel uses (d_a_alink_wolf.inc:364-366).
     *
     * ★ This used to be a mDoExt_McaMorfSO, which owned the model AND drove the animation. It
     * cannot stay, and the reason is structural rather than a matter of taste: McaMorfSO holds
     * exactly ONE J3DAnmTransform, and every modelCalc() re-installs itself as joint 0's matrix
     * calculator (m_Do_ext.cpp:1797-1805). There is no seam in it where a second animation could be
     * given to the torso. See setupAnimation() below for what replaces it.
     *
     * The two flags are unchanged by that swap. They matter and are worth keeping written down: the
     * body was once created as (data, 0, 0) while its own head, hands and face went through
     * init_model, and a body shaded on different terms from the head bolted onto it is exactly the
     * seam Stuart reported at the neck. The warp bracket matters even more — al.bmd is a BMWR
     * resource, so dRes_info_c's loader hands it over with the twilight dissolve already ENABLED,
     * and it is offWarpMaterial that switches it back off (d_resorce.cpp:127-178, :291-293). Both
     * of those live inside init_model now, which is the point of routing through it. */
    model = init_model(modelData, 0, "body");
    if (model == NULL) {
        return 0;
    }

    if (!setupAnimation(modelData)) {
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

    mpHeadModel = init_model(
        static_cast<J3DModelData*>(own_archive_res(mOwnRes, outfit.headResName)), 0, "head");
    mpHandModel = init_model(
        static_cast<J3DModelData*>(own_archive_res(mOwnRes, outfit.handsResName)), 0, "hands");
    mpFaceModel = init_model(
        static_cast<J3DModelData*>(own_archive_res(mOwnRes, outfit.faceResName)), 0x20200, "face");

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
    setupFootIk();
    setupEquipModels();

    /* Hands, and the one body shape this outfit is built with hidden. Both are the tail of
     * daAlink_c::setLinkModel (d_a_alink_wolf.inc:471-498) and both were missing; see setDrawHand()
     * for what that looked like on screen. */
    if (outfit.hiddenBodyMatNo != l_noBodyMat &&
        outfit.hiddenBodyMatNo < modelData->getMaterialNum())
    {
        modelData->getMaterialNodePointer(outfit.hiddenBodyMatNo)->getShape()->hide();
    }

    for (int i = 0; i < 2; i++) {
        mpDefaultHandShape[i] =
            outfit.defaultHandMatNo[i] < modelData->getMaterialNum() ?
                modelData->getMaterialNodePointer(outfit.defaultHandMatNo[i])->getShape() :
                NULL;
    }

    u16 hiddenShapeNum = 0;
    if (mpHandModel != NULL) {
        J3DModelData* handData = mpHandModel->getModelData();
        hiddenShapeNum = handData->getMaterialNum() < l_handShapeNum ? handData->getMaterialNum() :
                                                                       l_handShapeNum;
        for (u16 i = 0; i < hiddenShapeNum; i++) {
            handData->getMaterialNodePointer(i)->getShape()->hide();
        }
    }

    /* Start where daAlink_c starts, with the body's own pair showing (d_a_alink_wolf.inc:492-493).
     * setDrawHand() replaces them on the first tick that has a pose. */
    mpShownHandShape[0] = mpDefaultHandShape[0];
    mpShownHandShape[1] = mpDefaultHandShape[1];

    /* Latched and permanent. Every way this can be wrong is silent: a body material index that is
     * out of range for this outfit's model leaves the default hand NULL and setDrawHand() then
     * draws nothing for that hand, and hiding fewer than eleven alternates leaves a spare pair on
     * screen — which is the bug being fixed, and it left no trace in any log. */
    Log.debug("Puppet {} hands: '{}' body defaults mat {}/{} ({}/{}), {} of {} alternates hidden",
        mPlayerId, outfit.arcName, outfit.defaultHandMatNo[0], outfit.defaultHandMatNo[1],
        mpDefaultHandShape[0] != NULL ? "ok" : "MISSING",
        mpDefaultHandShape[1] != NULL ? "ok" : "MISSING", hiddenShapeNum, l_handShapeNum);

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
    log_material_state("puppet", "body", model->getModelData());
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

    // What is LEFT, not what was asked for. A heap that is merely nearly full does not announce
    // itself: every allocation here still succeeds and the damage, if any, shows up later and
    // somewhere else. One line makes the margin a number instead of a guess — and it earned its
    // keep, because the guess it replaced was wrong. Before DASHA was added this read 112528 free
    // of 0x20000 (131072), i.e. ~18 KB used, against a comment here that claimed ~35 KB.
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
 * Give the body the two-animations-at-once rig Link's body has.
 *
 * ★ This is the fix for what Stuart reported as the puppet running "the animation for link when he
 * holds a sword and shield". Link's body is driven by TWO animations simultaneously, not one: the
 * root and legs by an "under" BCK, the torso and arms by an "upper" one. Of the four animations the
 * puppet can be in, ANM_RUN is the only row whose halves differ — {DASHS, DASHA} — and a puppet
 * playing only the under half was therefore wearing DASHS's arms. That is not a near-miss: with a
 * sword equipped, daAlink_c plays m_mainBckSword[3] = {DASHS, DASHS} (d_a_alink.cpp:246), so
 * DASHS's arms ARE the sword-carrying arms. The puppet was playing the armed run while holding
 * nothing.
 *
 * The mechanism is daAlink_c's own (d_a_alink.cpp:4271-4283 and d_a_alink_swindow.inc:167-169), and
 * it is deliberately not a simpler stand-in:
 *
 *   - Two mDoExt_MtxCalcAnmBlendTblOld, one per half, each reading a small table of
 *     (animation, ratio) pairs. Blending a table rather than a single animation is what will make
 *     the walk/run cross-fade fall out for free later; today only slot 0 is filled.
 *   - Installed on joints 0, 1 and 16 — the two calculators between them cover thirty-five joints
 *     because J3D propagates a joint's calculator down its subtree until another one replaces it.
 *   - Both share ONE mDoExt_MtxCalcOldFrame. That object is what makes an animation CHANGE
 *     cross-fade instead of snap, by keeping the previous frame's pose per joint and lerping out of
 *     it, and sharing it is what keeps the arms and legs morfing in step. It also explains the two
 *     per-joint arrays: they are that stored pose.
 *
 * Returns false on any allocation failure, which fails createHeap. That is the right call here even
 * though a failing createHeap is expensive (the puppet is deleted and respawned): a body with no
 * matrix calculator installed does not degrade, it draws in the bind pose forever.
 */
bool daRemotePlayer_c::setupAnimation(J3DModelData* i_modelData) {
    mBodyJointNum = i_modelData->getJointNum();

    /* The upper half's root has to exist for the split to mean anything. Link's body has 35 joints;
     * this is checked rather than assumed because the count comes out of a binary asset. */
    if (mBodyJointNum <= l_upperRootJointNo) {
        Log.warn("Puppet body has only {} joints — no torso to animate separately", mBodyJointNum);
        return false;
    }

    mpOldTransInfo = JKR_NEW_ARRAY(J3DTransformInfo, mBodyJointNum);
    mpOldQuat = JKR_NEW_ARRAY(Quaternion, mBodyJointNum);
    if (mpOldTransInfo == NULL || mpOldQuat == NULL) {
        return false;
    }

    /* Zeroed for the same reason daAlink_c zeroes his (d_a_alink.cpp:4265-4269, under TARGET_PC):
     * the very first calc lerps OUT of whatever is in here, so garbage would be a garbage pose. */
    std::memset(mpOldTransInfo, 0, sizeof(J3DTransformInfo) * mBodyJointNum);
    std::memset(mpOldQuat, 0, sizeof(Quaternion) * mBodyJointNum);

    mpOldFrame = JKR_NEW mDoExt_MtxCalcOldFrame(mpOldTransInfo, mpOldQuat);
    if (mpOldFrame == NULL) {
        return false;
    }

    mpUnderCalc =
        JKR_NEW mDoExt_MtxCalcAnmBlendTblOld(mpOldFrame, daRemotePlayer_anmSlotNum, mAnmPackUnder);
    mpUpperCalc =
        JKR_NEW mDoExt_MtxCalcAnmBlendTblOld(mpOldFrame, daRemotePlayer_anmSlotNum, mAnmPackUpper);
    if (mpUnderCalc == NULL || mpUpperCalc == NULL) {
        return false;
    }

    /* Slot 0 has to be filled before anything calcs — the calculators dereference it without a null
     * check (m_Do_ext.cpp:1157). Nothing calcs before the first network pose arrives, but the pose
     * to start from is still the idle one rather than the bind pose. A negative morf means "do not
     * cross-fade into this", which is right for the first animation there is nothing to fade from.
     */
    setAnm(
        mIdleAnm, J3DFrameCtrl::EMode_LOOP, -1.0f, daAlinkHIO_move_c0::m.mWaitAnmSpeed, 0.0f, -1);

    i_modelData->getJointNodePointer(l_underRootJointNo)->setMtxCalc(mpUnderCalc);
    i_modelData->getJointNodePointer(l_upperRootJointNo)->setMtxCalc(mpUpperCalc);
    if (l_underLegJointNo < mBodyJointNum) {
        i_modelData->getJointNodePointer(l_underLegJointNo)->setMtxCalc(mpUnderCalc);
    } else {
        // Not fatal — the legs would simply follow the torso's animation — but it means this is not
        // the skeleton the joint numbers were read off, and every pose will be subtly wrong.
        Log.warn("Puppet body has {} joints; no joint {} to hand the legs back to", mBodyJointNum,
            l_underLegJointNo);
    }

    return true;
}

/**
 * Put one animation on both halves of the body.
 *
 * daAlink_c::commonSingleAnime (d_a_alink.cpp:7149-7206) plus the morf that setSingleAnime does
 * after it (:7238-7240). "Single" is his word for one animation across both halves as opposed to
 * two blended together — it still sets up two frame controllers when the row names two BCKs.
 */
void daRemotePlayer_c::setAnm(const daRemotePlayer_anm_c& i_anm, u8 i_attr, f32 i_morf, f32 i_rate,
    f32 i_startF, s16 i_endF) {
    if (i_anm.mpUnder == NULL) {
        return;
    }

    /* Clear every other slot. Only slot 0 is driven today, but leaving a stale pointer in slot 1
     * would keep blending a previous animation into the pose forever, at whatever ratio it was last
     * given — a bug with no symptom other than a body that looks slightly wrong. */
    for (int i = 1; i < daRemotePlayer_anmSlotNum; i++) {
        mAnmPackUnder[i].setAnmTransform(NULL);
        mAnmPackUpper[i].setAnmTransform(NULL);
        mAnmPackUnder[i].setRatio(0.0f);
        mAnmPackUpper[i].setRatio(0.0f);
    }
    mAnmPackUnder[0].setRatio(1.0f);
    mAnmPackUpper[0].setRatio(1.0f);

    mAnmPackUnder[0].setAnmTransform(i_anm.mpUnder);
    set_gait_frame_ctrl(&mUnderFrameCtrl[0], i_anm.mpUnder, i_attr, i_rate, i_startF, i_endF);

    if (i_anm.mpUpper != NULL) {
        mAnmPackUpper[0].setAnmTransform(i_anm.mpUpper);
        set_gait_frame_ctrl(&mUpperFrameCtrl[0], i_anm.mpUpper, i_attr, i_rate, i_startF, i_endF);
    } else {
        /* One resource on both halves, pointed at by both packs — daAlink_c's own answer
         * (d_a_alink.cpp:7202-7204). animePlay() then steps it once, through the under controller
         * only, because the frame lives on the shared object. */
        mAnmPackUpper[0].setAnmTransform(i_anm.mpUnder);
    }

    if (i_morf >= 0.0f) {
        // Whole skeleton, as daAlink_c does with his literal 35 (d_a_alink.cpp:7239).
        mpOldFrame->initOldFrameMorf(i_morf, 0, mBodyJointNum);
    }

    mDoubleAnmSet = false;
}

/**
 * Play TWO animations at once and cross-fade between them by weight.
 *
 * This is how the local player's gait actually works, and it is why a puppet that switched outright
 * never quite read as another player: daAlink_c never swaps walk for run, he runs both and slides
 * the weight across the band (commonDoubleAnime, d_a_alink.cpp:7009-7066). The rig for it was
 * already here — the ratio packs setupAnimation() builds are exactly this — with slot 1 empty.
 *
 * Called every tick rather than on a change, as daAlink_c calls it, because the ratio has to track
 * the speed continuously. Passing i_morf < 0 means "no cross-fade", which is the normal case: there
 * is nothing to fade when the pair is unchanged and only the weight moved.
 *
 * ★ One case daAlink_c does not have to handle and this does: the two animations being the SAME
 * object. Above the run threshold he calls this with ANM_RUN on both sides, which is harmless for
 * him because getUnderUpperAnime loads each slot out of its own heap and he gets two separate
 * copies of DASHS. The puppet holds one copy of each animation, and two frame controllers stepping
 * one object would advance it at double speed — a run cycle at 2x with no other symptom. So an
 * identical pair collapses to the single-animation case instead.
 */
void daRemotePlayer_c::setDoubleAnm(const daRemotePlayer_anm_c& i_anmA,
    const daRemotePlayer_anm_c& i_anmB, f32 i_blendRatio, f32 i_speedA, f32 i_speedB, f32 i_morf) {
    if (i_anmA.mpUnder == NULL || i_anmB.mpUnder == NULL) {
        return;
    }

    const bool onePair = i_anmA.mpUnder == i_anmB.mpUnder;

    /* Where in the stride we are, 0..1, carried over from the pair that was playing. Without this a
     * band change would restart both cycles at frame 0 and the feet would jump — and it is read
     * BEFORE mDoubleAnmSet is updated, because after a single animation (the skid) there is no
     * blended phase to carry and daAlink_c restarts from 0 there too (his field_0x2f8c,
     * d_a_alink.cpp:7016-7021). */
    f32 phase = 0.0f;
    if (mDoubleAnmSet && mUnderFrameCtrl[0].getEnd() != 0) {
        phase = mUnderFrameCtrl[0].getFrame() / mUnderFrameCtrl[0].getEnd();
    }

    if (onePair) {
        i_blendRatio = 0.0f;
    }

    mAnmPackUnder[0].setRatio(1.0f - i_blendRatio);
    mAnmPackUpper[0].setRatio(1.0f - i_blendRatio);
    mAnmPackUnder[1].setRatio(i_blendRatio);
    mAnmPackUpper[1].setRatio(i_blendRatio);

    /* The blended rate, expressed per unit of phase. daAlink_c writes it on animation A's timeline
     * and divides through by A's length for the others (:7028-7033); dividing once here says the
     * same thing and makes the four calls below identical. */
    const f32 maxA = i_anmA.mpUnder->getFrameMax();
    const f32 maxB = i_anmB.mpUnder->getFrameMax();
    const f32 rateA = maxA != 0.0f ? i_speedA / maxA : 0.0f;
    const f32 rateB = maxB != 0.0f ? i_speedB / maxB : 0.0f;
    const f32 phaseRate = rateA + i_blendRatio * (rateB - rateA);

    mAnmPackUnder[0].setAnmTransform(i_anmA.mpUnder);
    set_blend_frame_ctrl(&mUnderFrameCtrl[0], i_anmA.mpUnder, phaseRate, phase);

    if (i_anmA.mpUpper != NULL) {
        mAnmPackUpper[0].setAnmTransform(i_anmA.mpUpper);
        set_blend_frame_ctrl(&mUpperFrameCtrl[0], i_anmA.mpUpper, phaseRate, phase);
    } else {
        mAnmPackUpper[0].setAnmTransform(i_anmA.mpUnder);
    }

    if (onePair) {
        mAnmPackUnder[1].setAnmTransform(NULL);
        mAnmPackUpper[1].setAnmTransform(NULL);
    } else {
        mAnmPackUnder[1].setAnmTransform(i_anmB.mpUnder);
        set_blend_frame_ctrl(&mUnderFrameCtrl[1], i_anmB.mpUnder, phaseRate, phase);

        if (i_anmB.mpUpper != NULL) {
            mAnmPackUpper[1].setAnmTransform(i_anmB.mpUpper);
            set_blend_frame_ctrl(&mUpperFrameCtrl[1], i_anmB.mpUpper, phaseRate, phase);
        } else {
            mAnmPackUpper[1].setAnmTransform(i_anmB.mpUnder);
        }
    }

    if (i_morf >= 0.0f) {
        mpOldFrame->initOldFrameMorf(i_morf, 0, mBodyJointNum);
    }

    mDoubleAnmSet = true;
}

/* See the header. daAlink_c::allAnimePlay (d_a_alink.cpp:7262-7290) with everything the puppet has
 * no path to removed — the wolf voice, the demo hand animations and the sub-animations behind
 * FLG1_UNK_10. What is kept exactly is the guard on the upper half. */
void daRemotePlayer_c::animePlay() {
    for (int i = 0; i < daRemotePlayer_anmSlotNum; i++) {
        gait_anime_play(mAnmPackUnder[i].getAnmTransform(), &mUnderFrameCtrl[i]);
    }

    for (int i = 0; i < daRemotePlayer_anmSlotNum; i++) {
        J3DAnmTransform* upper = mAnmPackUpper[i].getAnmTransform();
        /* Pointer comparison, not a null check, and it is the whole reason this loop is separate:
         * an animation with no distinct upper half has BOTH packs pointing at one object, and
         * stepping it twice would advance it at double speed. */
        if (upper != mAnmPackUnder[i].getAnmTransform()) {
            gait_anime_play(upper, &mUpperFrameCtrl[i]);
        }
    }

    /* ★ Latched and permanent. A split rig and the single-animation one it replaced look the same
     * from everywhere else — same model, same joints, same morf, same trace columns — so without
     * this line "the torso is running its own animation now" is unfalsifiable from a log. It fires
     * only on ANM_RUN, the one row of the puppet's four whose halves differ.
     *
     * What it prints is chosen carefully, because the obvious thing to print proves nothing: the
     * two FRAME NUMBERS are supposed to be equal. DASHS and DASHA are the two halves of one run
     * cycle and have to stay in phase, so equal frames is the correct answer, not evidence of
     * anything. The two things that do discriminate are the two BCK ids — different resources, so
     * different arms — and the calculator actually sitting on joint 1 when the model is read back.
     * That last one is the whole split: if it is not mpUpperCalc, joint 1's subtree inherited joint
     * 0's and the torso is back on the under animation with nothing else looking wrong. */
    if (!mLoggedSplitAnm && mAnmPackUpper[0].getAnmTransform() != NULL &&
        mAnmPackUpper[0].getAnmTransform() != mAnmPackUnder[0].getAnmTransform())
    {
        mLoggedSplitAnm = true;

        J3DModelData* bodyData = model->getModelData();
        const J3DMtxCalc* onUnderRoot =
            bodyData->getJointNodePointer(l_underRootJointNo)->getMtxCalc();
        const J3DMtxCalc* onUpperRoot =
            bodyData->getJointNodePointer(l_upperRootJointNo)->getMtxCalc();
        const J3DMtxCalc* onLegs =
            l_underLegJointNo < mBodyJointNum ?
                bodyData->getJointNodePointer(l_underLegJointNo)->getMtxCalc() :
                NULL;

        Log.debug("Puppet {} body split live: anim {} under bck {} / upper bck {} (frames {:.1f} / "
                  "{:.1f}, equal is correct) | joints {}/{}/{} -> {}/{}/{}",
            mPlayerId, mCurrentAnm,
            anm_bck_idx(static_cast<daAlink_c::daAlink_ANM>(mCurrentAnm), mNetEquip),
            anm_bck_upper_idx(static_cast<daAlink_c::daAlink_ANM>(mCurrentAnm), mNetEquip),
            mUnderFrameCtrl[0].getFrame(), mUpperFrameCtrl[0].getFrame(), l_underRootJointNo,
            l_upperRootJointNo, l_underLegJointNo, onUnderRoot == mpUnderCalc ? "under" : "WRONG",
            onUpperRoot == mpUpperCalc ? "UPPER" : "WRONG",
            onLegs == mpUnderCalc ? "under" : "WRONG");
    }
}

/* --- The outfit seam. These two are the only way the network layer touches l_outfits; the table
 * itself stays file-static so there is exactly one copy of it in the program. */

int daRemotePlayer_outfitFromWire(u8 i_wireOutfit) {
    if (i_wireOutfit >= l_outfitNum) {
        // Not a name we know. Reasons range from benign (0xFF, nobody has reported an outfit yet)
        // to hostile (a corrupted byte out of an unreliable packet), and all of them are better
        // answered with a Link in the hero's clothes than with an out-of-bounds table read.
        return l_defaultOutfit;
    }
    return i_wireOutfit;
}

u8 daRemotePlayer_localOutfitToWire() {
    return static_cast<u8>(outfit_index_for_local_player());
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

/* Read and cleared by the network layer's spawn backoff; see d_a_remote_player.h. */
daRemotePlayer_createFail_c g_daRemotePlayer_lastCreateFail = {0, NULL};

int daRemotePlayer_c::create() {
    fopAcM_ct(this, daRemotePlayer_c);

    /* Both halves of the create parameter, read up front. mPlayerId used to be assigned after the
     * heap pass; it is set here instead so the mount, and every log line before that point, can
     * name the player it belongs to. fopAcM_ct only constructs once (it is gated on
     * fopAcCnd_INIT_e), so re-entry while the archive mounts re-reads the same parameter. */
    const u32 param = fopAcM_GetParam(this);
    mPlayerId = param & daRemotePlayer_playerIdMask;

    if (!mOutfitChosen) {
        /* ★ The SENDER's outfit, carried in the create parameter — not the local player's, which is
         * what this used to copy. Appearance is owned by the wearer: if the other player is in the
         * Zora armour, that is what we mount, whatever we happen to be wearing ourselves.
         *
         * Still latched. create() is re-entered every frame until the mount finishes, and letting
         * the target change half way through would leave mOwnRes mounting one archive while
         * createHeap() looked up model names from another. A clothes change that lands mid-mount is
         * picked up afterwards, by respawning the actor — see dusk::mp::reconcile_puppet_outfit. */
        mOutfit = daRemotePlayer_outfitFromWire(
            static_cast<u8>(param >> daRemotePlayer_outfitParamShift));
        mOutfitChosen = true;
    }

    const int mountPhase = mountOwnArchive();
    if (mountPhase != cPhs_COMPLEATE_e) {
        /* cPhs_LOADING_e here is the normal case and must NOT be reported as a failure — the mount
         * legitimately takes many frames. Only the error step is worth recording. */
        if (mountPhase == cPhs_ERROR_e) {
            g_daRemotePlayer_lastCreateFail.mPlayerId = mPlayerId;
            g_daRemotePlayer_lastCreateFail.mReason = "the private archive mount failed";
        }
        return mountPhase;
    }

    if (!fopAcM_entrySolidHeap(this, daRemotePlayer_createHeap, 0x20000)) {
        // Either the heap estimate was too small or a resource was missing. Say so: a silent
        // cPhs_ERROR_e here surfaces later as a puppet that simply never appears.
        Log.warn("Puppet heap/model setup failed for the remote player actor");
        /* ★ mPlayerId, NOT fopAcM_GetParam(this). The create parameter is no longer the player id
         * on its own — it carries the outfit in its top byte — so keying the reason on the raw
         * parameter made every lookup miss and the give-up line read "not reported by the actor".
         * Measured; the two halves of that change landed together and neither could see the other.
         */
        g_daRemotePlayer_lastCreateFail.mPlayerId = mPlayerId;
        g_daRemotePlayer_lastCreateFail.mReason =
            "the 0x20000 solid heap, or one of the models/animations createHeap() loads into it";
        return cPhs_ERROR_e;
    }

    // `model` itself was set by createHeap, which is where it is built; setupAnimation() has
    // already put the idle animation on it.
    mCurrentAnm = static_cast<u16>(l_idleAnm);

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
    // Last line, deliberately: this is the network layer's ONLY trustworthy "creation succeeded"
    // signal. See createComplete() in the header for why findability is not one.
    mCreateComplete = true;
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

void daRemotePlayer_c::setNetworkPose(const cXyz& i_pos, s16 i_angleY, f32 i_moveRate,
    bool i_sharpTurn, bool i_zeroSpeed, bool i_modeIdle, bool i_footIkOff, u8 i_equip) {
    current.pos = i_pos;
    shape_angle.y = i_angleY;
    // The logical angle is kept in step so anything that reads current.angle (audio, effects) sees
    // a sane value, even though only shape_angle drives the model matrix.
    current.angle.y = i_angleY;
    mNetMoveRate = i_moveRate;
    // Written every tick before execute() can reach selectAnimation() — execute() returns early
    // until mHasPose, which is set right below — so these need no separate initialisation.
    mNetSharpTurn = i_sharpTurn;
    mNetZeroSpeed = i_zeroSpeed;
    mNetModeIdle = i_modeIdle;
    mNetFootIkOff = i_footIkOff;
    mNetEquip = i_equip;
    mHasPose = true;
}

/**
 * Choose the gait from the sender's own gait rate, the way the local player chooses it.
 *
 * daAlink_c crosses over at two fixed fractions of that rate (d_a_alink.cpp:7653/7784): wait blends
 * into walk below mWalkChangeRate, walk into run below mRunChangeRate, run alone above it. Each
 * cycle plays at its own authored rate — the game never speeds a walk up to stand in for a run.
 *
 * Those numbers are read from daAlinkHIO_move_c0::m rather than copied, so the puppet cannot drift
 * out of step with the player if the table is ever corrected.
 *
 * ★ Nothing here scales, normalises or thresholds the replicated value, and that is the point. The
 * rate arrives as getMoveGroundAngleSpeedRate()'s own output and the "is he standing" answer
 * arrives as a bit beside it, because every time this function derived one of those for itself it
 * got a different answer from the player it was copying — three times, each silent, each looking
 * like a tuning problem: the wrong speed field, then the missing mMinWalkRate floor, then an idle
 * threshold invented here. Read the sender's answer; do not recompute it.
 *
 * ★ The blend is the point, and it used to be missing. daAlink_c never swaps one gait for another:
 * he plays BOTH sides of a band at once and slides the weight across it. The puppet quantised to
 * whichever side was dominant and morfed over the switch, which is the wrong shape of behaviour
 * rather than a slightly worse version of the same one — a real player crossing from a walk into a
 * run passes through every mixture between them, and the puppet stepped over that in one frame.
 *
 * ★ The ground-angle cosine used to be a knowing divergence here — daAlink_c shifts toward the
 * slower gait when climbing at the same speed — and it is gone for free, because it is applied
 * inside getMoveGroundAngleSpeedRate() before the rate is sent. So is the lock-on case, where his
 * mMaxSpeed is a member that drops to the targeting maximum rather than the HIO constant this
 * function used to divide by.
 *
 * ★ What remains is equipment, and it is job D: getMainBckData swaps the whole animation pair for a
 * drawn sword, a raised shield, the kandelaar or the fishing rod, and heavy boots or the iron ball
 * pin the rate into band 1 outright (d_a_alink.cpp:7620-7641). The SWORD half of that is done — see
 * anm_bck_data() and runAnm(), which follow his own `mEquipItem == 0x103` test off the wire. The
 * other three are each one more byte of replicated state away, and none of them changes the band
 * arithmetic below; they only change which pair of resources the chosen band plays.
 */
void daRemotePlayer_c::selectAnimation() {
    const daAlinkHIO_move_c1& hio = daAlinkHIO_move_c0::m;

    /* First thing, and before either return path below: from here on mCurrentAnm is a choice rather
     * than create()'s seed, which is what getCurrentAnm() reports on. */
    mAnmChosen = true;

    /* The sharp turn wins over the gait outright, exactly as it does for the local player:
     * PROC_SLIP is a proc of its own and setSingleAnimeParam replaces the move blend wholesale, it
     * does not layer over it (d_a_alink.cpp:16675). Taken FIRST and returned from, so the gait code
     * below cannot then overwrite the play speed with a walk/run rate.
     *
     * Every playback parameter is daAlink_c's own mSlideAnm, read from daAlinkHIO_move_c0::m rather
     * than copied — the same single-source rule the gait thresholds already follow. That is start
     * frame 3.0, end frame 11, rate 0.7 and a 4.0-frame morf in (d_a_alink_HIO_data.inc:34-40), so
     * the puppet's blend INTO the skid is the player's own interpolation rather than the 5.0f the
     * gait switch uses, and cannot fight it.
     *
     * ★ One deliberate divergence: the play mode is pinned to EMode_NONE (play once, hold the last
     * frame) instead of taking the BCK's own attribute the way commonSingleAnime does
     * (d_a_alink.cpp:7180). daAlink_c can afford the asset's mode because his state machine leaves
     * PROC_SLIP within about ten ticks under its own power; the puppet has no state machine and
     * leaves only when the wire says so — and StateBuffer HOLDS the last sample while starved
     * (state_buffer.cpp:164-169). If that held sample had the bit set and the animation looped, a
     * lagging puppet would skid on the spot over and over, which reads as a bug rather than as the
     * lag it is. Holding the final frame degrades to a puppet frozen mid-skid, which reads as lag.
     */
    if (mNetSharpTurn && mSlipAnm.mpUnder != NULL) {
        if (mCurrentAnm != l_slipAnm) {
            const daAlinkHIO_anm_c& slide = hio.mSlideAnm;
            setAnm(mSlipAnm, J3DFrameCtrl::EMode_NONE, slide.mInterpolation, slide.mSpeed,
                slide.mStartFrame, slide.mEndFrame);
            mCurrentAnm = static_cast<u16>(l_slipAnm);
        }
        // The idle-frame probe below counts consecutive IDLE ticks; a skid is not one of them.
        mIdleTicks = 0;
        return;
    }

    /* The three bands daAlink_c crosses over in, and the pair he blends inside each
     * (setBlendMoveAnime, d_a_alink.cpp:7749-7800). Below mWalkChangeRate he mixes wait into walk,
     * below mRunChangeRate walk into run, and above it runs alone. The thresholds come from
     * daAlinkHIO_move_c0::m rather than being copied, so the puppet cannot drift out of step with
     * the player if the table is ever corrected. */
    /* No scaling, no absolute value, no threshold of our own: this IS the sender's own
     * getMoveGroundAngleSpeedRate(), which has already divided by his mMaxSpeed, already applied
     * the ground-angle cosine, and already taken fabsf (d_a_alink.cpp:7555). Every one of those
     * steps used to happen here instead, and each was a way to disagree with him. */
    const f32 fraction = mNetMoveRate;

    /* Standing, decided by the SENDER and carried on the wire, because the branch it picks has a
     * step in it rather than a taper — see the remap in band 1 below, and kPlayerStateZeroSpeed.
     *
     * The `<= 0` half is belt and braces for a rate that arrives at exactly zero with the bit
     * somehow clear: the rate is an absolute value, so zero means the sender was not moving, and
     * without this that combination would land on the moving side of the step and walk a
     * motionless puppet on the spot. It costs a compare and removes a class of wire bug. */
    const bool zeroSpeed = mNetZeroSpeed || fraction <= 0.0f;

    const daRemotePlayer_anm_c* anmA;
    const daRemotePlayer_anm_c* anmB;
    daAlink_c::daAlink_ANM idA;
    daAlink_c::daAlink_ANM idB;
    f32 speedA;
    f32 speedB;
    f32 blend;

    if (fraction < hio.mWalkChangeRate) {
        blend = fraction / hio.mWalkChangeRate;
        /* ★ Link's walk weight never drops below mMinWalkRate — 0.7 — once he is genuinely moving
         * (d_a_alink.cpp:7752-7759, the else of the standing branch). Band 2 has no equivalent
         * remap, which is exactly why walking was the only gait Stuart saw glide: "no still
         * gliding, puppet is too slow (its animation) — but this is only on walking", 2026-08-13.
         * At a slow walk the raw ratio is about 0.25, so Link was at 0.775 and the puppet at 0.25 —
         * roughly a third of the walk weight over a body translating at the full speed.
         *
         * ⚠ Do NOT smooth the resulting 0 -> 0.7 jump. Link has the same discontinuity, in the same
         * tick, for the same reason: he crosses from the standing branch to this one whole. A ramp
         * here would be a divergence invented to hide a divergence.
         *
         * mMinTiredWalkRate (0.4) is the low-HP variant and belongs with ANM_WAIT_TIRED, which the
         * puppet cannot play until the wire carries the sender's health — job D. */
        if (!zeroSpeed) {
            blend = hio.mMinWalkRate + blend * (1.0f - hio.mMinWalkRate);
        }
        /* Nothing pins the standing case to a hard zero, and deliberately: daAlink_c's standing
         * branch keeps the raw ratio too, it simply never reaches the remap. The ratio is already
         * zero to five decimal places whenever the bit is set — the bit means the sender's speed is
         * under 0.001 — so pinning it would only be inventing a rule the game does not have. */
        anmA = &mIdleAnm;
        anmB = &mWalkAnm;
        idA = l_idleAnm;
        idB = l_walkAnm;
        speedA = hio.mWaitAnmSpeed;
        speedB = hio.mWalkAnmSpeed;
    } else if (fraction < hio.mRunChangeRate) {
        blend = (fraction - hio.mWalkChangeRate) / (hio.mRunChangeRate - hio.mWalkChangeRate);
        anmA = &mWalkAnm;
        anmB = &runAnm();
        idA = l_walkAnm;
        idB = l_runAnm;
        speedA = hio.mWalkAnmSpeed;
        speedB = hio.mRunAnmSpeed;
    } else {
        /* daAlink_c passes ANM_RUN on both sides here with a weight of 1; setDoubleAnm() collapses
         * an identical pair rather than stepping one animation object twice. */
        blend = 1.0f;
        anmA = &runAnm();
        anmB = &runAnm();
        idA = l_runAnm;
        idB = l_runAnm;
        speedA = hio.mRunAnmSpeed;
        speedB = hio.mRunAnmSpeed;
    }

    /* ★ The standing test is applied in band 1 ONLY, which is where daAlink_c applies it: bands 2
     * and 3 do not consult it at all (d_a_alink.cpp:7784-7798). The receiver used to pin every band
     * to pure idle below a speed threshold of its own, which is a rule the game does not have — it
     * only ever looked harmless because MODE_IDLE with a run-speed rate is rare, not impossible. */

    /* Which gait the puppet is mostly in. This no longer decides what is drawn — the blend does —
     * but it is still the animation the hand poses are read from and the value --mp-trace reports,
     * so it keeps the hysteresis: a puppet held near a band's midpoint would otherwise flip the
     * REPORTED gait every tick and make the trace unreadable. */
    daAlink_c::daAlink_ANM wanted;
    if (mCurrentAnm == static_cast<u16>(idB)) {
        wanted = blend > 0.5f - l_gaitHysteresis ? idB : idA;
    } else {
        wanted = blend > 0.5f + l_gaitHysteresis ? idB : idA;
    }

    /* Stuart reported the puppet as having "no idle body animation". The animation it plays IS the
     * one the local player plays — ANM_WAIT resolves to WAITS (d_a_alink.cpp:308) and it is set to
     * loop at mWaitAnmSpeed, which is 1.0 (d_a_alink_HIO_data.inc:33) — so the question is whether
     * the frame is actually ADVANCING, which nothing else in the trace can answer. One latched line
     * once the puppet has been idle a while; a frame near the animation's end means it is running.
     */
    if (wanted == l_idleAnm) {
        if (mIdleTicks < 0xFFFF) {
            mIdleTicks++;
        }
        if (mIdleTicks == l_idleFrameLogTick && !mLoggedIdleFrame) {
            mLoggedIdleFrame = true;
            Log.debug(
                "Puppet {} idle body anim after {} ticks: frame {:.1f} of {:.1f}, rate {:.2f}, "
                "mode {}",
                mPlayerId, mIdleTicks, mUnderFrameCtrl[0].getFrame(),
                static_cast<f32>(mUnderFrameCtrl[0].getEnd()), mUnderFrameCtrl[0].getRate(),
                static_cast<int>(mUnderFrameCtrl[0].getAttribute()));
        }
    } else {
        mIdleTicks = 0;
    }

    mCurrentAnm = static_cast<u16>(wanted);

    /* No morf between gaits — the blend IS the transition, and asking for one on top would fade
     * from a pose to itself. The only thing there is to fade out of is the skid, which is a single
     * animation, and mDoubleAnmSet is exactly "the last thing set was a blended pair". */
    setDoubleAnm(*anmA, *anmB, blend, speedA, speedB, mDoubleAnmSet ? -1.0f : l_gaitMorf);

    /* ★ Latched and permanent. The armed run and the empty-handed one share their LEGS — both are
     * DASHS under — so the trace's animation id, the frame numbers and the blend are all identical
     * between them and none of them can tell the two apart. The one value that can is the upper
     * pack's animation POINTER: armed, both packs hold the same object (that is what "one resource
     * on both halves" means and why animePlay must not step it twice); empty-handed, the upper pack
     * holds DASHA and they differ. So this prints the pointer comparison and the two table rows it
     * is supposed to follow from, and if they disagree the substitution did not take. */
    if (!mLoggedSwordRun && wanted == l_runAnm &&
        (mNetEquip & dusk::mp::kPlayerEquipSwordInHand) != 0)
    {
        mLoggedSwordRun = true;
        Log.debug("Puppet {} armed run live: equip 0x{:02x} -> bck {}/{} (empty-handed row is "
                  "{}/{}), packs {} -> arms are {}",
            mPlayerId, mNetEquip, anm_bck_idx(l_runAnm, mNetEquip),
            anm_bck_upper_idx(l_runAnm, mNetEquip), anm_bck_idx(l_runAnm, 0),
            anm_bck_upper_idx(l_runAnm, 0),
            mAnmPackUpper[0].getAnmTransform() == mAnmPackUnder[0].getAnmTransform() ? "shared" :
                                                                                       "split",
            mAnmPackUpper[0].getAnmTransform() == mAnmPackUnder[0].getAnmTransform() ?
                "sword-carrying" :
                "WRONG (empty-handed)");
    }

    /* ★ Latched and permanent, for the same reason every other one-shot in this file is: a blend
     * that never leaves 0 or 1 is indistinguishable from the outright switch it replaced, from
     * everywhere except someone's eyes. The condition is deliberately strict — two DIFFERENT
     * animations, both actually carrying weight — because either half alone is the old behaviour.
     *
     * The two frame numbers being different is expected and is NOT the claim: these are cycles of
     * different lengths held at the same normalised phase, which is the whole job of
     * set_blend_frame_ctrl(). The check worth making by eye on this line is that frame/end comes
     * out about equal for both; if it does not, the two halves of the stride have drifted apart. */
    if (!mLoggedBlend && idA != idB && blend > 0.05f && blend < 0.95f) {
        mLoggedBlend = true;
        Log.debug("Puppet {} gait blend live: {:.2f} from anim {} bck {} (frame {:.1f} of {}) into "
                  "anim {} bck {} (frame {:.1f} of {}) at rate {:.3f}",
            mPlayerId, blend, static_cast<int>(idA), anm_bck_idx(idA, mNetEquip),
            mUnderFrameCtrl[0].getFrame(), mUnderFrameCtrl[0].getEnd(), static_cast<int>(idB),
            anm_bck_idx(idB, mNetEquip), mUnderFrameCtrl[1].getFrame(), mUnderFrameCtrl[1].getEnd(),
            mNetMoveRate);
    }

    /* ★ Latched and permanent, and it proves the one thing a screenshot cannot: that the walk
     * weight is on the RIGHT SIDE of Link's step. A puppet using the raw ratio and a puppet using
     * the remap look like the same bug at a glance — both walk — and the difference between them
     * is the entire gliding report. Fires the first time the sender is genuinely moving inside
     * band 1, and states the raw ratio next to what it became. Anything below mMinWalkRate on the
     * "remapped" number means the remap is not live. */
    if (!mLoggedWalkFloor && !zeroSpeed && fraction < hio.mWalkChangeRate) {
        mLoggedWalkFloor = true;
        Log.debug("Puppet {} walk floor live: rate {:.3f} -> raw ratio {:.2f} -> remapped {:.2f} "
                  "(floor mMinWalkRate {:.2f}); sender says standing = {}",
            mPlayerId, fraction, fraction / hio.mWalkChangeRate, blend, hio.mMinWalkRate,
            mNetZeroSpeed ? "yes" : "no");
    }
}

/* See the header. Out of line so it can go through the same table everything else here does. */
u16 daRemotePlayer_c::getCurrentAnm() const {
    /* ★ 0 until selectAnimation has actually run once, and that is a correctness fix, not padding.
     * create() seeds mCurrentAnm with ANM_WAIT so the hysteresis has a starting value, and there is
     * a window between create() finishing and the actor joining the execute pass in which the
     * network layer can already resolve this puppet and read that seed back. Reporting it says
     * "standing" for a puppet that has not chosen anything yet.
     *
     * That window is normally invisible because both players spawn each other while standing still.
     * Measured on sword-run.txt, where the guest loads its world while the host is already at full
     * speed: eight consecutive actor rows claiming the idle animation against a rate ramping
     * 0.66 -> 1.00, which mp_analyze correctly scored as OUT OF BAND. Nothing was wrong on screen —
     * the first draw comes after the first execute, so the puppet was not being drawn at all — but
     * an analyzer that reports a failure for a puppet that does not exist yet is an analyzer we
     * learn to ignore.
     *
     * 0 is the sentinel the trace and mp_analyze already agree on: no BCK has index 0 and the
     * analyzer filters those rows out. */
    if (!mAnmChosen) {
        return 0;
    }
    return anm_bck_idx(static_cast<daAlink_c::daAlink_ANM>(mCurrentAnm), mNetEquip);
}

/* See the header. Out of line so anm_bck_data's table read stays in one translation unit with the
 * rest of the animation code. */
const daRemotePlayer_anm_c& daRemotePlayer_c::runAnm() const {
    return (mNetEquip & dusk::mp::kPlayerEquipSwordInHand) != 0 ? mRunSwordAnm : mRunAnm;
}

/**
 * Show exactly one hand shape per hand, the pair the current animation asks for.
 *
 * ★ Link wears TWO pairs of hands, and the puppet was drawing both. This is not obvious from any
 * one place in daAlink_c, so it is worth writing down in full:
 *
 *   - The BODY model carries a plain pair as ordinary shapes. Which materials they are depends on
 *     the outfit, and setLinkModel picks them per wear branch into field_0x06d8 / field_0x06dc
 *     (d_a_alink_wolf.inc:426-477) — off field_0x064C, which IS mpLinkModel->getModelData()
 *     (d_a_alink_swindow.inc:163). They are shapes on the body, so they are always drawn unless
 *     something hides them.
 *   - The SEPARATE al_hands.bmd holds eleven alternative poses as shapes 0-10, and setLinkModel
 *     hides all eleven (:495-498).
 *   - setDrawHand then hides whichever two are currently up and shows exactly one per hand
 *     (d_a_alink.cpp:18928-19057) — taken from EITHER model, because hand index 0xFE means "the
 *     body's plain one".
 *
 * The puppet used to hide the eleven and then show hand-model shapes 0 and 6, and never touch the
 * body's pair. So it had a spare set of hands hanging off its wrists — Stuart's report — and the
 * visible pair was wrong as well: 0 and 6 are not neutral poses, they are the grips daAlink_c uses
 * ONLY on the pause-menu branch, "0 if he owns a sword, 6 if he owns a shield"
 * (d_a_alink.cpp:18936-18946). Hence a puppet walking around clenched around a sword and shield
 * that were not there.
 *
 * ★ The pose is a property of the ANIMATION, not of the actor, and that is the whole fix: the hand
 * indices sit in m_anmDataTable next to the BCK id (m_handIndexL/R, d_a_alink.h:212-213). Walking,
 * running and standing all ask for 4 and 10 (d_a_alink.cpp:301, :302, :308); the skid asks for
 * 0xFE/0xFE (:323) and so shows the body's own hands. Reading them from there rather than choosing
 * a "neutral" pair means the puppet cannot be holding something the animation is not shaped for,
 * and it stays right for free when equipment replication starts driving other animations.
 *
 * What is NOT reproduced is daAlink_c's item override chain (field_0x2f94-0x2f97, the 0x64 and 0xFB
 * demo cases). Those are all "the hand is holding a specific item", and a puppet has no replicated
 * equipment yet; every one of them falls through to mLeftHandIndex/mRightHandIndex, which is
 * exactly what this reads.
 */
void daRemotePlayer_c::setDrawHand() {
    for (int i = 0; i < 2; i++) {
        if (mpShownHandShape[i] != NULL) {
            mpShownHandShape[i]->hide();
        }
    }

    const daAlink_AnmData& anmData = anm_data(static_cast<daAlink_c::daAlink_ANM>(mCurrentAnm));
    const u8 handIdx[2] = {anmData.m_handIndexL, anmData.m_handIndexR};

    J3DModelData* handData = mpHandModel != NULL ? mpHandModel->getModelData() : NULL;
    for (int i = 0; i < 2; i++) {
        /* Anything this cannot honour — the 0xFE sentinel, an item pose, a hands model that failed
         * to load — resolves to the body's plain hand, which is also daAlink_c's own answer for
         * 0xFE. The range checks are not defensive padding: the shape count comes out of a binary
         * asset, so it is not verifiable from source for every outfit. */
        const bool useBodyHand = handIdx[i] == l_defaultHandIdx || handData == NULL ||
                                 handIdx[i] >= l_handShapeNum ||
                                 handIdx[i] >= handData->getMaterialNum();

        if (useBodyHand) {
            mpShownHandShape[i] = mpDefaultHandShape[i];
        } else {
            mpShownHandShape[i] = handData->getMaterialNodePointer(handIdx[i])->getShape();
        }

        if (mpShownHandShape[i] != NULL) {
            mpShownHandShape[i]->show();
        }
    }

    /* One line, latched, the first time a pose actually comes off the hands model. Without it
     * "the hands are right now" is unfalsifiable from a trace: a table read that silently resolved
     * to the body's hands every tick would look identical to the old bug minus one pair, and the
     * animation id is the thing worth seeing because it is what selects the pair. */
    if (!mLoggedHands && (handIdx[0] != l_defaultHandIdx || handIdx[1] != l_defaultHandIdx)) {
        mLoggedHands = true;
        Log.debug("Puppet {} hands: anim {} (bck {}) asks for L={} R={}", mPlayerId, mCurrentAnm,
            getCurrentAnm(), handIdx[0], handIdx[1]);
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
    } else if (mNetZeroSpeed) {
        /* Nobody worth watching and standing still, so look about. daAlink_c::setEyeMove's idle
         * branch (d_a_alink.cpp:3337-3358), which is what stops a waiting Link from staring dead
         * ahead. His version gates on mProcID == PROC_WAIT and friends; a puppet has no proc, and
         * the sender's own "standing" bit is the closest thing to it that exists here — closer
         * than the invented speed threshold this used to compare against.
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
        /* No pivot: a hair strand turns about its own root, and rewriting only its own matrix is
         * what keeps the strands independent of one another (the param_4 == 0 form of the
         * original). The foot IK is the caller that passes one. */
        setMatrixWorldAxisRot(mpHeadModel->getAnmMtx(i_jointNo), mSwayAngleX[i_jointNo], 0,
            mSwayAngleY[i_jointNo], NULL);
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
        model != NULL ? model->getModelData() : NULL,
        mpHeadModel != NULL ? mpHeadModel->getModelData() : NULL,
        mpHandModel != NULL ? mpHandModel->getModelData() : NULL,
        mpFaceModel != NULL ? mpFaceModel->getModelData() : NULL,
        mpSwordModel[0] != NULL ? mpSwordModel[0]->getModelData() : NULL,
        mpSwordModel[1] != NULL ? mpSwordModel[1]->getModelData() : NULL,
        mpSwordModel[2] != NULL ? mpSwordModel[2]->getModelData() : NULL,
        mpSheathModel[0] != NULL ? mpSheathModel[0]->getModelData() : NULL,
        mpSheathModel[1] != NULL ? mpSheathModel[1]->getModelData() : NULL,
    };
    static const char* const names[l_watchedModelNum] = {"body", "head", "hands", "face",
        "sword ordon", "sword master", "sword wood", "sheath PODA", "sheath PODM"};

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

    /* ★ Cap-Y inputs, sampled EVERY tick, because the thing being measured is a per-tick delta and
     * reading it once every 120 ticks would compare the puppet's one-tick turn against 120 ticks of
     * Link's.
     *
     * ★ MEAN as well as peak, and the mean is the one that decides this. The yaw kick is subtracted
     * straight into the accumulated angle with nothing damping it, so it is a kick SUSTAINED over
     * consecutive ticks that walks the cap out to the ±0x2800 clamp and holds it there; a single
     * spike is pulled back to zero within about five ticks by the addCalc. Peaks alone cannot tell
     * a spike from a sustained turn, so matching peaks — which is what the first pass measured —
     * are not evidence that the inputs agree. Means are.
     *
     * Link's equivalents are reconstructed from public members — field_0x3062 is his head yaw and
     * field_0x34c8 his previous cap anchor — so this is like-for-like, not inference. */
    {
        const s16 yawKickNow = (s16)(mHeadYaw - prevYaw);
        if (abs(yawKickNow) > abs(mYawKickPeak)) {
            mYawKickPeak = yawKickNow;
        }
        mYawKickSum += abs(yawKickNow);
        mKickTicks++;

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
                mLinkYawKickSum += abs(linkKick);
                mLinkKickTicks++;
            }
            mPrevLinkHeadYaw = tickLink->field_0x3062;
            mPrevLinkHeadYawValid = true;
            mLinkSampled = true;

            cXyz linkAnchor;
            mDoMtx_multVecZero(tickLink->mpLinkHatModel->getAnmMtx(l_capRootJointNo), &linkAnchor);
            const cXyz linkWind = tickLink->field_0x34c8 - linkAnchor;
            const f32 linkLateral = linkWind.absXZ();
            if (linkLateral > mLinkLateralMovePeak) {
                mLinkLateralMovePeak = linkLateral;
            }

            /* ★ Link's anchor motion PROJECTED onto his sideways axis — the term his Y target is
             * really built from. His frame is the gaze direction, and field_0x3062 is precisely
             * that direction's yaw (d_a_alink.cpp:2604), so sin/cos of it reproduce his var_f29 and
             * var_f28 without needing his locals. Accumulated here so the puppet's equivalent,
             * which can only be formed further down once the apparent wind exists, has something to
             * be compared against over the same window. */
            const f32 linkSin = cM_ssin(tickLink->field_0x3062);
            const f32 linkCos = cM_scos(tickLink->field_0x3062);
            mLinkProjLateralSum += fabsf(linkWind.x * linkCos - linkWind.z * linkSin);
            mLinkCapYSum += abs(tickLink->field_0x3040[l_capRootJointNo]);

            /* ★ Link's standing-still test used to be counted here, and its answer is what cracked
             * this: it fired 120/120 on BOTH sides, which killed the theory that the puppet was
             * zeroing where he was not, and forced a reading of the branch in front of that test —
             * where the real difference turned out to be. Removed now that the puppet has no
             * corresponding branch to compare against, and because counting the inner test on its
             * own was misleading regardless: the FLG0_SWIM_UP guard means Link never reaches it on
             * dry land however still he stands. */
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

    /* ★★ THE term that makes a cap stream sideways in wind, and the one this actor was missing.
     *
     * The original is an if/else-if (d_a_alink.cpp:2652-2657): when FLG0_SWIM_UP is set it adds the
     * ambient wind scaled by the flutter energy and does NOT zero anything; only in the `else` does
     * the standing-still test throw the horizontal part away. An earlier version of this function
     * transcribed the second branch alone, on the reasoning that a puppet is never swimming.
     *
     * ★ That reasoning inverted the flag. FLG0_SWIM_UP is turned ON in Link's create
     * (d_a_alink.cpp:4597) and is only ever turned OFF inside d_a_alink_swim.inc and
     * d_a_alink_hvyboots.inc — i.e. it means "head above water" and is TRUE while standing on dry
     * land. The name reads like "is swimming upward" and it is a trap. So a walking Link takes the
     * FIRST branch, always; the standing-still zeroing is the UNDERWATER case and had no business
     * running here at all.
     *
     * Measured rather than argued: with both characters parked in the same spot, Link's cap Y sat
     * pinned at the -0x2800 clamp (mean 10233 over 12 samples) while the puppet's sat at 7, and the
     * arithmetic of this branch reproduces his number exactly — flutter power 0.60 gives an energy
     * of 25*0.6^2 = 9.0 horizontally against the 5.0 downward bias, and atan2s(9, 5) is about 61
     * degrees, which clamps to the 45 degrees that IS 10240.
     *
     * ★ Note this is NOT the teach-wind path. field_0x35b8 (`windPush` below) is gated by
     * dKy_TeachWind_existence_chk and measured at 0.00 for both players here; the wind still
     * reaches the cap ANGLE through this term, which is ungated. That is why the room reads `teach
     * 0` and why the flutter — which also ignores the gate — was the only thing visibly working. */
    apparentWind += flutterWindDir * (25.0f * (windPower * windPower));

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

    const f32 projLateral = apparentWind.x * cosYaw - apparentWind.z * sinYaw;

    const s16 wantY = cLib_minMaxLimit<s16>(
        cM_atan2s(-projLateral, JMAFastSqrt(SQUARE(forward) + SQUARE(apparentWind.y))), -0x2800,
        0x2800);

    cLib_addCalcAngleS2(angY, wantY, 5, 0x400);
    *angY = cLib_minMaxLimit<s16>(*angY + *velY, -0x2800, 0x2800);

    /* ★ The projected term itself, not the magnitude of the motion it came from. The first pass
     * measured |anchor motion| and found it matched Link's, but the Y target uses only the part of
     * that motion lying across the look direction — a cap anchor moving straight down the gaze
     * contributes nothing to Y however fast it moves. Sampled here rather than with the other
     * inputs above because it cannot be formed until the apparent wind and the head frame exist. */
    mProjLateralSum += fabsf(projLateral);
    mCapYSum += abs(*angY);
    mProjTicks++;

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
            /* Non-const purely so the comparison below can ask Link for his own gait rate;
             * getMoveGroundAngleSpeedRate() is a read but is not declared const in the decomp. */
            daAlink_c* windLink = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
            f32 linkRaw = -1.0f;
            if (windLink != NULL) {
                cXyz linkPos = windLink->current.pos;
                cXyz linkWindDir;
                dKyw_get_AllWind_vec(&linkPos, &linkWindDir, &linkRaw);
            }
            /* ★ Both GAIT RATES go on this line, and they are not decoration. The cap's lateral
             * swing is driven by how far the cap anchor moved, so a walking character's cap swings
             * and a standing one's hangs — by design, and confirmed by Stuart when the idle hang
             * was added. A comparison taken while one side walks and the other stands therefore
             * shows a large difference that has NOTHING to do with wind, and that is exactly what
             * the first human-form comparison caught: P1 walking, puppet parked. Trust the cap
             * angles only when these two numbers are close. */
            Log.debug("Puppet {} wind #{}: bend raw {:.2f} (P1 raw {:.2f}) flutter {:.2f} | "
                      "teach {} | rate {:.2f} (hit {}, dist {:.0f}) | push {:.2f} vs P1 {:.2f} | "
                      "gait rate {:.3f} vs P1 {:.3f}",
                mPlayerId, mWindLogCount, bendWindPower, linkRaw, windPower, teachWind,
                mWindWallRate, mWindChkHit ? "yes" : "no", mWindChkDist,
                JMAFastSqrt(mWindPush.abs2()),
                windLink != NULL ? JMAFastSqrt(windLink->field_0x35b8.abs2()) : -1.0f, mNetMoveRate,
                windLink != NULL ? windLink->getMoveGroundAngleSpeedRate() : -1.0f);

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
            const f32 meanKick = mKickTicks != 0 ? (f32)mYawKickSum / mKickTicks : 0.0f;
            const f32 meanLinkKick =
                mLinkKickTicks != 0 ? (f32)mLinkYawKickSum / mLinkKickTicks : 0.0f;
            const f32 meanProj = mProjTicks != 0 ? mProjLateralSum / mProjTicks : 0.0f;
            const f32 meanLinkProj =
                mLinkKickTicks != 0 ? mLinkProjLateralSum / mLinkKickTicks : 0.0f;
            const f32 meanCapY = mProjTicks != 0 ? (f32)mCapYSum / mProjTicks : 0.0f;
            const f32 meanLinkCapY =
                mLinkKickTicks != 0 ? (f32)mLinkCapYSum / mLinkKickTicks : 0.0f;

            if (!mLinkSampled) {
                Log.debug("Puppet {} capY inputs #{}: yaw kick/tick mean {:.0f} peak {} | proj "
                          "lateral/tick mean {:.2f} (raw peak {:.2f}) | capY mean {:.0f} now {} | "
                          "NO P1 COMPARISON (wolf or no local player)",
                    mPlayerId, mWindLogCount, meanKick, mYawKickPeak, meanProj, mLateralMovePeak,
                    meanCapY, mSwayAngleY[l_capRootJointNo]);
            } else {
                Log.debug("Puppet {} capY inputs #{}: yaw kick/tick mean {:.0f} vs P1 {:.0f} (peak "
                          "{} vs {}) | proj lateral/tick mean {:.2f} vs P1 {:.2f} (raw peak {:.2f} "
                          "vs {:.2f}) | capY mean {:.0f} vs P1 {:.0f} (now {} vs {})",
                    mPlayerId, mWindLogCount, meanKick, meanLinkKick, mYawKickPeak,
                    mLinkYawKickPeak, meanProj, meanLinkProj, mLateralMovePeak,
                    mLinkLateralMovePeak, meanCapY, meanLinkCapY, mSwayAngleY[l_capRootJointNo],
                    windLink != NULL ? windLink->field_0x3040[l_capRootJointNo] : 0);
            }

            mYawKickPeak = 0;
            mLinkYawKickPeak = 0;
            mLateralMovePeak = 0.0f;
            mLinkLateralMovePeak = 0.0f;
            mYawKickSum = 0;
            mLinkYawKickSum = 0;
            mKickTicks = 0;
            mLinkKickTicks = 0;
            mProjLateralSum = 0.0f;
            mLinkProjLateralSum = 0.0f;
            mCapYSum = 0;
            mLinkCapYSum = 0;
            mProjTicks = 0;
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
    setHairAngle(&apparentWind, sinYaw, cosYaw);
}

/* --- Foot IK ------------------------------------------------------------------------------- */

/* Both legs, joint by joint. daAlink_c's footJointTable (d_a_alink.cpp:3626) is {0x12, 0x17} and
 * each leg is FOUR consecutive joints from there: hip, knee, ankle, toe. The probe points come off
 * the ankle and the toe, which is why 0x14/0x15 and 0x19/0x1A appear separately in footBgCheck —
 * they are hip+2 and hip+3 for each leg, not a second table. Same numbers for every human outfit;
 * the wolf's are different and the puppet has no wolf model. */
const u16 l_legRootJointNo[2] = {0x12, 0x17};

/* Where setFootMatrix is applied from, and it is NOT an arbitrary hook point.
 *
 * daAlink_c calls it from joint 26 (d_a_alink.cpp:2430-2433) and 26 is 0x1A — the second leg's toe,
 * i.e. the LAST joint the solve writes. J3DJoint::recursiveCalc fires the pass-0 callback after
 * that joint's own matrix is built and before it descends (J3DJoint.cpp:218-221), so by the time
 * this runs, all eight leg joints are final and none of the joints above them have been touched. */
const u16 l_footIkCallBackJointNo = 26;

/**
 * J3D calls this for every hooked joint of the BODY model, twice per joint. Same shape as
 * daRemotePlayer_headModelCallBack, and the same reasons: first pass only, actor resolved from the
 * model rather than a global.
 */
static int daRemotePlayer_bodyModelCallBack(J3DJoint* i_joint, int i_pass) {
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

    return puppet->bodyModelCallBack(i_joint->getJntNo());
}

int daRemotePlayer_c::bodyModelCallBack(int i_jointNo) {
    if (i_jointNo == l_footIkCallBackJointNo) {
        setFootMatrix();
    }

    return 1;
}

/**
 * Hook the foot IK onto the body model's joint 26. Once, at createHeap time.
 *
 * ★ This is the fix for the first attempt, which called setFootMatrix() straight after
 * model->calc() and produced a solve that was correct in the logs and invisible on screen.
 * J3DModel::calc() runs calcAnmMtx() and then calcWeightEnvelopeMtx()
 * (J3DModel.cpp:452-453): the second builds the skinning matrices FROM the joint matrices the
 * first left. Posing a leg after calc() returns writes into buffers that have already been consumed
 * — and on top of that, the TARGET_PC block at the end of calc() records the joint matrices for
 * frame interpolation (J3DModel.cpp:463-471), so every interpolated frame would redraw the
 * un-IK'd pose regardless. A joint callback is upstream of both. The legs are envelope-skinned, so
 * nothing at all reached the screen.
 *
 * The reasoning that produced the bug is worth keeping: every leg joint is numbered below 26, so
 * post-calc looked equivalent. That answers "are the joints final?" — the question is "has anything
 * read them yet?"
 *
 * Same archive precondition as setupHeadSway(): setCallBack lives on the model DATA, so this is
 * only safe because the puppet mounts its own copy. Against a shared tree this would install puppet
 * code on the joints the LOCAL PLAYER's body is drawn from. The private mount is the precondition,
 * not an optimisation — the same one that makes the blend matrix calculators safe to install.
 */
void daRemotePlayer_c::setupFootIk() {
    J3DModelData* modelData = model->getModelData();
    if (modelData == NULL) {
        return;
    }

    /* Guarded because an outfit with a shorter skeleton would index off the end of the joint array,
     * and the failure mode of getting this wrong is silence: the hook never fires and the feet look
     * exactly like they did before the IK existed. Logged either way for that reason. */
    const u16 jointNum = modelData->getJointNum();
    if (jointNum <= l_footIkCallBackJointNo) {
        Log.warn("Puppet body model has only {} joints; no foot IK (needs joint {})", jointNum,
            l_footIkCallBackJointNo);
        return;
    }

    model->setUserArea((uintptr_t)this);
    modelData->getJointNodePointer(l_footIkCallBackJointNo)
        ->setCallBack(daRemotePlayer_bodyModelCallBack);

    /* The envelope count is logged because it is the measurement that the callback is NEEDED, not
     * merely tidier. calcWeightEnvelopeMtx() does nothing at all when it is zero
     * (J3DModel.cpp:406), and on such a model the old post-calc write would have been visible — a
     * count of zero here would mean the invisible-IK diagnosis was wrong and the fault lay
     * somewhere else entirely. */
    Log.info("Puppet foot IK attached to body joint {} of {} ({} envelope matrices)",
        l_footIkCallBackJointNo, jointNum, modelData->getWEvlpMtxNum());

    /* Useful property of hanging it here: mFootDataValid is set by setFootMatrix and nothing else,
     * and footBgCheck refuses to run until it is true. So a callback that is attached but never
     * fires shows up as the "foot IK probing" line never appearing at all — the two failure modes
     * stay distinguishable in the log instead of both reading as flat ground. */
}

/* Where on each foot the floor is probed, in that joint's own space (d_a_alink.cpp:3846-3849). The
 * ankle point sits behind and to the side, the toe point ahead — the pair is averaged, so the probe
 * follows the middle of the foot rather than either end of it. */
const Vec l_localFootOffset[2] = {{-3.0f, 13.0f, 0.0f}, {-3.0f, -13.0f, 0.0f}};
const Vec l_localToeOffset[2] = {{10.0f, 5.0f, 0.0f}, {10.0f, -5.0f, 0.0f}};

/* Bone lengths, from setFootMatrix (d_a_alink.cpp:3627-3629). These are the model's, not a guess:
 * the solver walks the chain by them to find where each joint ends up. */
const Vec l_thighVec = {30.0f, 0.0f, 0.0f};
const Vec l_shinVec = {39.363499f, 0.0f, 0.0f};
const Vec l_footVec = {14.18f, 0.0f, 0.0f};

/* How far above the actor's origin the floor probe starts, and how far below it may find a floor.
 *
 * ★ daAlink_c keeps these in file-static `l_autoUpHeight`/`l_autoDownHeight` (d_a_alink.cpp:1468),
 * which are not reachable from here — but they are not constants either. They are ASSIGNED from the
 * HIO table when Link becomes human (`mpHIO->mWallHang.m.auto_walk_height + 0.01f`,
 * d_a_alink_wolf.inc:532), so the table is the source and the static is the cache. Read the table.
 * The 30.01 the static is initialised with is only what it holds before that assignment runs.
 *
 * The down height is the negation of the up height, exactly as :533 sets it. */
static f32 foot_ik_up_height() {
    return daAlinkHIO_wallHang_c0::m.auto_walk_height + 0.01f;
}

/**
 * The slope angle of the polygon a check object last hit, relative to a facing.
 *
 * daAlink_c::getGroundAngle (d_a_alink.cpp:8466-8477). Transcribed rather than called because it is
 * a member of daAlink_c, and it is four lines. The two rejections matter and are not decoration: an
 * unsafe polygon index means the collision entry has been recycled since the query, and a normal
 * that fails cBgW_CheckBGround is a wall or a ceiling, which has an "angle" that would tip a foot
 * straight over.
 */
static s16 ground_angle(const cBgS_PolyInfo& i_polyInfo, s16 i_angle) {
    if (!dComIfG_Bgsp().ChkPolySafe(i_polyInfo)) {
        return 0;
    }

    cM3dGPla plane;
    if (!dComIfG_Bgsp().GetTriPla(i_polyInfo, &plane) || !cBgW_CheckBGround(plane.mNormal.y)) {
        return 0;
    }

    return fopAcM_getPolygonAngle(&plane, i_angle);
}

/**
 * Rotate one joint's world matrix about an axis through a pivot, in place.
 *
 * daAlink_c::setMatrixWorldAxisRot (d_a_alink.cpp:2098-2120), minus two things it does not need
 * here: the magne-boot matrices, which are identity unless Link is walking on a ceiling, and the
 * copy into J3DSys::mCurrentMtx, which neither caller asks for.
 *
 * Both callers are here: the foot IK walks down each leg passing the end of the bone above as the
 * pivot, and the hair passes none, which pivots each strand on itself. A null pivot is the
 * original's own param_5 == 0 case, not a puppet-side shortcut — one transcription, two uses.
 *
 * The Y rotations either side of the axis rotation are what make it a WORLD-axis rotation: the
 * joint is turned back into the model's facing, rotated about the model's own X, then turned out
 * again.
 */
void daRemotePlayer_c::setMatrixWorldAxisRot(
    MtxP io_mtx, s16 i_rotX, s16 i_rotY, s16 i_rotZ, const cXyz* i_pivot) {
    cXyz jointPos;
    mDoMtx_multVecZero(io_mtx, &jointPos);
    if (i_pivot != NULL) {
        mDoMtx_stack_c::transS(*i_pivot);
    } else {
        mDoMtx_stack_c::transS(jointPos);
    }

    mDoMtx_stack_c::YrotM(shape_angle.y);
    mDoMtx_stack_c::ZXYrotM(i_rotX, i_rotY, i_rotZ);
    mDoMtx_stack_c::YrotM(-shape_angle.y);
    mDoMtx_stack_c::transM(-jointPos.x, -jointPos.y, -jointPos.z);
    mDoMtx_stack_c::concat(io_mtx);
    mDoMtx_copy(mDoMtx_stack_c::get(), io_mtx);
}

/**
 * Lower (or raise) the whole model, smoothed, and keep the two derived matrices in step.
 *
 * daAlink_c::setMatrixOffset (d_a_alink.cpp:3684-3697) for its field_0x2b94 caller — the leg-length
 * one. His other caller passes &mSinkShapeOffset and skips the smoothing; the puppet does not sink
 * into sand, so only this branch exists here.
 *
 * The three writes go together. Moving the base matrix without moving mInvMtx would leave
 * setLegAngle solving in a space offset from the one the legs are drawn in, and the legs would
 * chase the body by exactly the sink each tick.
 */
void daRemotePlayer_c::setBodySinkOffset(f32 i_target) {
    cLib_addCalc(&mBodySinkOffset, i_target, 0.5f, 7.5f, 2.5f);

    model->getBaseTRMtx()[1][3] += mBodySinkOffset;
    mInvMtx[1][3] -= mBodySinkOffset;

    mDoMtx_stack_c::XrotS(shape_angle.x);
    mDoMtx_stack_c::concat(mInvMtx);
    mDoMtx_copy(mDoMtx_stack_c::get(), mFootLocalMtx);
}

/**
 * Two-bone IK for one leg: how much to pitch the hip and bend the knee so the ankle moves by
 * i_heightDelta.
 *
 * daAlink_c::setLegAngle (d_a_alink.cpp:3699-3843), the `param_4 != 0` branch. The other branch
 * solves in a different plane for the ARMS (handBgCheck), which the puppet does not do, and the
 * wolf and dismount special cases inside this branch are gone for the same reason.
 *
 * It is the standard circle-intersection solve: place the knee on the circle where the two bone
 * spheres meet, then report each joint's rotation as the angle between its old bone and its new
 * one. The three early rejections are all real cases, not paranoia — a target the leg cannot reach
 * (`thigh + shin <= distance`) would otherwise produce a NaN, and a target ABOVE the hip means the
 * floor came out over Link's waist, which is a bad probe rather than a leg pose.
 *
 * Returns false when it declines to solve; every caller must then leave the angles at zero rather
 * than reusing the previous ones.
 */
bool daRemotePlayer_c::setLegAngle(
    f32 i_heightDelta, daRemotePlayer_footData_c& io_foot, s16* o_hipAngle, s16* o_kneeAngle) {
    if (fabsf(i_heightDelta) < 0.1f) {
        return false;
    }

    /* The three joints in MODEL space, taken from last tick's un-IK'd matrices. X is flattened
     * because the solve is two-dimensional: the leg swings in the model's YZ plane and letting the
     * sideways component in would tip the knee outward. */
    cXyz hipPos;
    cXyz kneePos;
    cXyz anklePos;
    cMtx_concat(mFootLocalMtx, io_foot.mJointMtx[0], mDoMtx_stack_c::get());
    mDoMtx_stack_c::multVecZero(&hipPos);
    cMtx_concat(mFootLocalMtx, io_foot.mJointMtx[1], mDoMtx_stack_c::get());
    mDoMtx_stack_c::multVecZero(&kneePos);
    cMtx_concat(mFootLocalMtx, io_foot.mJointMtx[2], mDoMtx_stack_c::get());
    mDoMtx_stack_c::multVecZero(&anklePos);
    hipPos.x = 0.0f;
    kneePos.x = 0.0f;
    anklePos.x = 0.0f;

    const cXyz thigh = kneePos - hipPos;
    const cXyz shin = anklePos - kneePos;

    cXyz target(anklePos);
    target.y += i_heightDelta;
    if (target.y >= hipPos.y) {
        return false;
    }

    const cXyz toTarget = target - hipPos;
    const f32 distSq = toTarget.abs2();
    if (cM3d_IsZero(distSq)) {
        return false;
    }

    const f32 thighSq = thigh.abs2();
    const f32 shinSq = shin.abs2();
    if (JMAFastSqrt(thighSq) + JMAFastSqrt(shinSq) <= JMAFastSqrt(distSq)) {
        return false;
    }

    // Along the hip→target line, where the plane of the intersection circle sits; then how far off
    // that line the knee has to be.
    const f32 along = ((distSq + thighSq) - shinSq) / (2.0f * distSq);
    const cXyz mid = hipPos + (toTarget * along);

    f32 offset = thighSq - (along * (distSq * along));
    if (offset < 0.0f) {
        offset = 0.0f;
    }
    offset = JMAFastSqrt(offset);

    // Perpendicular to hip→target, in the solve plane, on the side the knee bends towards.
    cXyz perp;
    perp.set(0.0f, toTarget.z, -toTarget.y);
    f32 perpLen = perp.abs();
    if (cM3d_IsZero(perpLen)) {
        return false;
    }
    perpLen = offset / perpLen;

    const cXyz newKnee = mid + (perp * perpLen);
    const cXyz newThigh = newKnee - hipPos;
    const cXyz newShin = target - newKnee;

    const s16 hipDir = cM_atan2s(newThigh.y, newThigh.z);
    s16 kneeDir = cM_atan2s(newShin.y, newShin.z);

    /* Clamp the knee to bending one way only, and no further than 0x7000. A knee is a hinge; the
     * solve has no opinion about which of the two circle intersections is anatomically possible, so
     * this is what stops the leg folding backwards on an awkward floor. */
    const s16 bend = kneeDir - hipDir;
    if (bend > 0) {
        kneeDir = hipDir;
    } else if (bend < -0x7000) {
        kneeDir = hipDir - 0x7000;
    }

    *o_hipAngle = cM_atan2s(thigh.y, thigh.z) - hipDir;
    *o_kneeAngle = cM_atan2s(shin.y, shin.z) - kneeDir;
    return true;
}

/**
 * Find the floor under each foot and work out what the legs have to do about it.
 *
 * daAlink_c::footBgCheck (d_a_alink.cpp:3845-3977). Runs BEFORE the body's calc, on the joint
 * matrices the PREVIOUS calc left behind — which is the same one-frame lag the local player has,
 * and is why nothing here needs the model to be calc'd twice.
 *
 * What the puppet leaves out, and why each is safe:
 *   - `setSandShapeOffset` / `mSinkShapeOffset` / `setSandDownBgCheckWallH` — sinking into sand and
 *     snow. That is a whole system the puppet does not have; the sender's part of it is folded into
 *     the replicated "no foot IK" bit, so a sender sinking in sand simply switches the IK off here.
 *   - `mProcID == PROC_SERVICE_WAIT` and friends — the puppet has no state machine, and the states
 *     concerned are demos and the Ganon fight.
 *   - `mGroundCode != 8` on the foot pitch — a floor property the puppet does not read. Code 8 is
 *     the one getMoveGroundAngleSpeedRate also refuses to take an angle from, so the effect of
 *     leaving it out is a foot that pitches on a surface Link's would keep flat.
 */
void daRemotePlayer_c::footBgCheck() {
    /* Nothing to solve against until the model has been calc'd at least once and setFootMatrix has
     * captured a set of joint matrices. Before then getAnmMtx returns whatever the model was
     * allocated with, and the probe points would be taken from a skeleton that has never been
     * posed — a floor query at the world origin, on the first tick of every puppet's life. */
    if (!mFootDataValid) {
        return;
    }

    const f32 upHeight = foot_ik_up_height();
    const f32 downHeight = -upHeight;

    /* Both gates come off the wire. See kPlayerStateNoFootIk and kPlayerStateModeIdle — deriving
     * either from the puppet's own interpolated position answers a different question. */
    const bool ikOff = mNetFootIkOff;
    const bool modeIdle = mNetModeIdle;

    cXyz anklePos[2];
    cXyz toePos[2];
    f32 footGroundY[2];
    s16 footGroundAngle[2];
    for (int i = 0; i < 2; i++) {
        mDoMtx_multVec(
            model->getAnmMtx(l_legRootJointNo[i] + 2), &l_localFootOffset[i], &anklePos[i]);
        mDoMtx_multVec(model->getAnmMtx(l_legRootJointNo[i] + 3), &l_localToeOffset[i], &toePos[i]);
        footGroundAngle[i] = 0;
    }

    for (int i = 0; i < 2; i++) {
        daRemotePlayer_footData_c& foot = mFootData[i];
        cXyz sample = (toePos[i] + anklePos[i]) * 0.5f;

        /* The anti-buzz latch, and it is load-bearing rather than a refinement. A standing foot
         * still moves a hair every tick (breathing, the idle cycle), and two neighbouring polygons
         * can differ in height; without this the foot flickers between them forever. Five ticks of
         * "has barely moved" and the probe point freezes where it was. Only while standing —
         * a walking foot must follow the ground it is walking onto. */
        if (ikOff) {
            foot.mFreezeTicks = 5;
        } else {
            const cXyz moved = sample - foot.mLastSample;
            if (moved.abs2XZ() < 100.0f && modeIdle) {
                if (foot.mFreezeTicks != 0) {
                    foot.mFreezeTicks--;
                } else {
                    sample = foot.mLastSample;
                }
            } else {
                foot.mFreezeTicks = 5;
            }
        }
        foot.mLastSample = sample;

        /* Raised to the actor's origin plus the up height before probing DOWN — the same
         * strictly-below trap the puppet's shadow hit. A query started at the foot's own height is
         * already level with the floor it is standing on and finds nothing at all. */
        cXyz probe(sample.x, current.pos.y + upHeight, sample.z);
        mFootGndChk.SetPos(&probe);

        const f32 groundY = dComIfG_Bgsp().GroundCross(&mFootGndChk);
        cM3dGPla plane;
        if (groundY != -G_CM3D_F_INF) {
            dComIfG_Bgsp().GetTriPla(mFootGndChk, &plane);
        }

        if (groundY != -G_CM3D_F_INF && cBgW_CheckBGround(plane.mNormal.y) &&
            probe.y - groundY < upHeight - downHeight)
        {
            footGroundY[i] = groundY;
            foot.mOnGround = 1;
            footGroundAngle[i] = ground_angle(mFootGndChk, shape_angle.y);
        } else {
            // No usable floor under this foot: fall back to the actor's own height, which leaves
            // the leg where the animation put it.
            footGroundY[i] = current.pos.y;
            foot.mOnGround = 0;
        }
    }

    /* Which foot the body hangs from: the one on the LOWER floor. Standing across a step, that is
     * the foot on the tread below, and the whole model sinks to it so the upper leg bends instead
     * of the lower one stretching. 2 means neither. */
    int plantedFoot;
    f32 sink = 0.0f;
    if (ikOff) {
        plantedFoot = 2;
    } else {
        plantedFoot = footGroundY[1] > footGroundY[0] ? 0 : 1;
        sink = footGroundY[plantedFoot];
    }

    if (ikOff || !modeIdle) {
        sink = 0.0f;
    } else {
        sink -= current.pos.y;
    }
    setBodySinkOffset(sink);

    const f32 bodyY = model->getBaseTRMtx()[1][3];
    for (int i = 0; i < 2; i++) {
        daRemotePlayer_footData_c& foot = mFootData[i];

        s16 hipAngle = 0;
        s16 kneeAngle = 0;
        if (!ikOff) {
            f32 delta = footGroundY[i] - bodyY;
            if (delta > upHeight) {
                delta = upHeight;
            }

            /* While MOVING, only the planted foot is pulled DOWN — the other leg is in the air and
             * belongs to the animation. Standing, both legs solve. */
            if ((plantedFoot != i && !(delta > 0.0f) && !modeIdle) ||
                !setLegAngle(delta, foot, &hipAngle, &kneeAngle))
            {
                hipAngle = 0;
                kneeAngle = 0;
            }
        }

        /* Unwrap before smoothing. Two angles of opposite sign that are more than half a turn apart
         * are actually adjacent across the seam, and interpolating between them the short way round
         * would swing the hip through the body. */
        if ((hipAngle * foot.mHipAngle) < 0 && abs(hipAngle - foot.mHipAngle) >= 0x8000) {
            if (hipAngle >= 0) {
                ANGLE_SUB(hipAngle, 0x4000);
            } else {
                ANGLE_ADD(hipAngle, 0x4000);
            }
        }

        // Eased in, never applied raw: the target jumps whenever a probe crosses a polygon edge.
        cLib_addCalcAngleS(&foot.mHipAngle, hipAngle, 2, 0x1800, 0x10);
        cLib_addCalcAngleS(&foot.mKneeAngle, kneeAngle, 2, 0x1800, 0x10);

        // Pitching the foot ONTO the slope, so a standing puppet's sole lies flat on a ramp rather
        // than meeting it at an angle. Standing only — a walking foot keeps the animation's pitch.
        s16 ankleAngle = 0;
        if (plantedFoot != 2 && foot.mOnGround != 0 && modeIdle) {
            ankleAngle += footGroundAngle[i];
        }
        cLib_addCalcAngleS(&foot.mAnkleAngle, ankleAngle, 2, 0x1800, 0x10);
    }

    /* ★ TWO latched lines, and the pair is the point. On flat ground the correct output of all of
     * the above is ZERO — both feet find the same floor, the height delta is under setLegAngle's
     * 0.1 threshold, and it declines. That is indistinguishable from the IK not existing, so the
     * first line records that the code RAN and what it saw, and the second that it actually bent a
     * leg. A log with the first and not the second says "ran, ground was flat"; a log with neither
     * says the feature never executed, which is a different bug entirely. */
    if (!mLoggedFootProbe && !ikOff) {
        mLoggedFootProbe = true;
        Log.debug("Puppet {} foot IK probing: floors {:.1f}/{:.1f} (hit {}/{}) vs body {:.1f}, "
                  "deltas {:.2f}/{:.2f} | up height {:.2f} | sender standing {}",
            mPlayerId, footGroundY[0], footGroundY[1], mFootData[0].mOnGround,
            mFootData[1].mOnGround, bodyY, footGroundY[0] - bodyY, footGroundY[1] - bodyY, upHeight,
            modeIdle ? "yes" : "no");
    }

    /* The second one: a genuinely bent leg — a hip angle worth more than a degree or so, 0x0100
     * being about 1.4 degrees. Zero angles on flat ground are the right answer and prove nothing.
     */
    if (!mLoggedFootIk &&
        (abs(mFootData[0].mHipAngle) > 0x0100 || abs(mFootData[1].mHipAngle) > 0x0100))
    {
        mLoggedFootIk = true;
        Log.debug("Puppet {} foot IK live: hip {}/{} knee {}/{} ankle {}/{} | floors {:.1f}/{:.1f} "
                  "vs body {:.1f} | planted {} sink {:.2f} | sender standing {} ik off {}",
            mPlayerId, mFootData[0].mHipAngle, mFootData[1].mHipAngle, mFootData[0].mKneeAngle,
            mFootData[1].mKneeAngle, mFootData[0].mAnkleAngle, mFootData[1].mAnkleAngle,
            footGroundY[0], footGroundY[1], bodyY, plantedFoot, mBodySinkOffset,
            modeIdle ? "yes" : "no", ikOff ? "yes" : "no");
    }
}

/**
 * Write this tick's leg angles onto the skeleton, and save the pose they were solved against.
 *
 * daAlink_c::setFootMatrix (d_a_alink.cpp:3625-3682), run from the same place he runs it: the joint
 * callback at joint 26, i.e. from INSIDE the body's calc. See setupFootIk() for why anywhere else
 * is a solve nothing draws.
 *
 * It re-poses all four joints of each leg itself rather than relying on J3D to propagate down the
 * chain — the children of these joints were calc'd before the callback fired, so nothing is going
 * to inherit these rotations for it.
 *
 * The save has to come FIRST. field_0x14 is the un-IK'd pose, and next tick's solve starts from it;
 * capturing it after the rotations were applied would feed each tick's answer back into the next
 * one and the legs would wind up.
 */
void daRemotePlayer_c::setFootMatrix() {
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 3; j++) {
            cMtx_copy(model->getAnmMtx(j + l_legRootJointNo[i]), mFootData[i].mJointMtx[j]);
        }
    }

    // Only now is there something for footBgCheck to solve against.
    mFootDataValid = true;

    for (int i = 0; i < 2; i++) {
        const daRemotePlayer_footData_c& foot = mFootData[i];
        u16 joint = l_legRootJointNo[i];
        cXyz pivot;

        /* Down the chain, each joint rotated about the END of the bone above it. The pivot comes
         * out of the matrix stack that setMatrixWorldAxisRot leaves behind, so these four calls are
         * a sequence and cannot be reordered.
         *
         * The ankle angle is applied TWICE, to the ankle and to the toe. That is not a copy/paste
         * slip in the original: pitching the ankle alone would leave the toe hinged, and the foot
         * has to stay rigid as it lies down on the slope. */
        setMatrixWorldAxisRot(model->getAnmMtx(joint), foot.mHipAngle, 0, 0, NULL);
        mDoMtx_stack_c::multVec(&l_thighVec, &pivot);
        joint++;

        setMatrixWorldAxisRot(model->getAnmMtx(joint), foot.mKneeAngle, 0, 0, &pivot);
        mDoMtx_stack_c::multVec(&l_shinVec, &pivot);
        joint++;

        setMatrixWorldAxisRot(model->getAnmMtx(joint), foot.mAnkleAngle, 0, 0, &pivot);
        mDoMtx_stack_c::multVec(&l_footVec, &pivot);
        joint++;

        setMatrixWorldAxisRot(model->getAnmMtx(joint), foot.mAnkleAngle, 0, 0, &pivot);
    }
}

/* --- Sword and sheath ------------------------------------------------------------------------ */

/* Where each piece hangs, from daAlink_c's human branch (d_a_alink_wolf.inc:563-571). The puppet
 * already uses 9 and 14 for the hand JOINTS; these are the ITEM joints, one further down each hand,
 * and the back joint the sheath and the stowed sword ride.
 *
 * ★ These are daAlink_c MEMBERS there, not constants, because the wolf uses a different skeleton
 * (19/24/2 at :277-285). A puppet is never a wolf — kPlayerStateWolf is reserved and always sent
 * clear — so the human values are inlined here rather than replicated. When the wolf arrives, this
 * is one of the places that has to grow a branch rather than a new number. */
const u16 l_leftItemJointNo = 10;
const u16 l_rightItemJointNo = 15;
const u16 l_backJointNo = 5;

/* Where the sheathed sword sits relative to the back joint, exactly as daAlink_c places it
 * (d_a_alink.cpp:5898-5901). Read as: take the back joint's matrix, walk to the small of the back,
 * and tip the hilt out so it clears the shoulder. */
const f32 l_stowedSwordOffsetX = -18.5f;
const f32 l_stowedSwordOffsetY = 0.14f;
const f32 l_stowedSwordOffsetZ = 12.2f;
const f32 l_stowedSwordYawDeg = 33.1f;

/**
 * Which sword model this tick's equipment byte selects, and the sheath that goes with it.
 *
 * Returns NULL for "none", which is the honest answer in two different cases and they are worth
 * keeping apart in the reader's head: the sender is not drawing a sword at all (the usual one), or
 * the model failed to load when the puppet was built (an outfit archive with no al_SWB in it). Both
 * end as an unarmed puppet, which is the right failure.
 */
J3DModel* daRemotePlayer_c::currentSword(J3DModel** o_sheath) const {
    *o_sheath = NULL;

    if ((mNetEquip & dusk::mp::kPlayerEquipSwordDraw) == 0) {
        return NULL;
    }

    const u8 kind = static_cast<u8>(
        (mNetEquip & dusk::mp::kPlayerEquipSwordKindMask) >> dusk::mp::kPlayerEquipSwordKindShift);
    if (kind >= dusk::mp::kPlayerEquipSwordKindNum) {
        // The wire has a fourth value that means nothing. Unarmed rather than indexed off the end.
        return NULL;
    }

    *o_sheath = mpSheathModel[kind];
    return mpSwordModel[kind];
}

/**
 * Build every sword and sheath the puppet might need, once, at createHeap time.
 *
 * ★ The ordon and master swords and both sheaths come from the "Alink" archive, which the puppet
 * READS DIRECTLY rather than mounting privately — the one place it does that, and it needs its
 * justification stated because mountOwnArchive() argues at length for the opposite.
 *
 * The argument there is about LIFETIME and about the JOINT TREE, and neither applies here:
 *
 *   - "Alink" is mounted once at boot, beside "Always" (d_s_logo.cpp:1478-1485), and is excluded
 *     from the per-stage size accounting exactly as "Always" and "Midna" are
 * (d_s_play.cpp:517-523). Nothing frees it while a scene exists. The OUTFIT archive is the opposite
 * case — Link loads it into his own heap and calls freeAll() on a clothes change — which is what
 * that comment is about.
 *   - Sharing model DATA is only dangerous when something per-actor is written onto it: a joint
 *     callback, a matrix calculator, a hidden shape. A sword has none of the first two. It does
 * have the third, and that one is real — see drawEquip(), which brackets it.
 *
 * The wooden sword is the exception and comes from the puppet's OWN outfit archive, because that is
 * where daAlink_c gets it (d_a_alink_wolf.inc:415, from mArcName rather than l_arcName).
 *
 * The flag pairs are daAlink_c's, verbatim (d_a_alink.cpp:4239-4253). They are not decorative:
 * initModelEnv is mdlFlags 0 where init_model is 0x80000, and the master sword additionally carries
 * 0x1000200. A sword built on the wrong terms is the neck seam bug again, one model further out.
 */
void daRemotePlayer_c::setupEquipModels() {
    for (int i = 0; i < dusk::mp::kPlayerEquipSwordKindNum; i++) {
        mpSwordModel[i] = NULL;
        mpSheathModel[i] = NULL;
    }

    J3DModelData* swaData = static_cast<J3DModelData*>(
        dComIfG_getObjectRes(l_alinkArcName, dRes_ID_ALINK_BMD_AL_SWA_e));
    J3DModelData* swmData = static_cast<J3DModelData*>(
        dComIfG_getObjectRes(l_alinkArcName, dRes_ID_ALINK_BMD_AL_SWM_e));
    J3DModelData* podaData = static_cast<J3DModelData*>(
        dComIfG_getObjectRes(l_alinkArcName, dRes_ID_ALINK_BMD_AL_PODA_e));
    J3DModelData* podmData = static_cast<J3DModelData*>(
        dComIfG_getObjectRes(l_alinkArcName, dRes_ID_ALINK_BMD_AL_PODM_e));

    mpSwordModel[dusk::mp::kPlayerEquipSwordOrdon] = init_model(swaData, 0x200, "sword ordon");
    mpSwordModel[dusk::mp::kPlayerEquipSwordMaster] =
        init_model_env(swmData, 0x1000200, "sword master");
    mpSwordModel[dusk::mp::kPlayerEquipSwordWood] = init_model(
        static_cast<J3DModelData*>(own_archive_res(mOwnRes, l_woodSwordResName)), 0, "sword wood");

    // Two sheaths, three swords: the wooden sword carries the master sword's (d_a_alink.cpp:4356).
    J3DModel* podaModel = init_model(podaData, 0, "sheath PODA");
    J3DModel* podmModel = init_model_env(podmData, 0, "sheath PODM");
    mpSheathModel[dusk::mp::kPlayerEquipSwordOrdon] = podaModel;
    mpSheathModel[dusk::mp::kPlayerEquipSwordMaster] = podmModel;
    mpSheathModel[dusk::mp::kPlayerEquipSwordWood] = podmModel;

    /* Not fatal, on the same terms as a missing head: an unarmed puppet is a far better failure
     * than no puppet. Logged per kind, because "which one is missing" is the whole diagnosis — a
     * missing wooden sword means the outfit archive, a missing ordon sword means "Alink" was not
     * where it is supposed to be, which would be a much stranger thing to be true. */
    for (int i = 0; i < dusk::mp::kPlayerEquipSwordKindNum; i++) {
        if (mpSwordModel[i] == NULL || mpSheathModel[i] == NULL) {
            Log.warn("Puppet has no sword model for kind {} (sword {}, sheath {}); it will appear "
                     "unarmed while that one is equipped",
                i, mpSwordModel[i] != NULL ? "ok" : "missing",
                mpSheathModel[i] != NULL ? "ok" : "missing");
        }
    }
}

/**
 * Hang the sword and its sheath off the body's joints. Every tick, AFTER the body's calc.
 *
 * daAlink_c::setItemMatrix (d_a_alink.cpp:5883-5906), keeping the two placements that exist for a
 * player who is simply carrying a sword. What is left out is all one thing — his `param_0`, the
 * status-window pose, which forces the sword into the hand for the pause menu's rotating model.
 *
 * The sheath is unconditional and the sword is not: the sheath rides the back whatever the sword is
 * doing, which is what makes a drawn sword read as drawn.
 */
void daRemotePlayer_c::setEquipMatrix() {
    J3DModel* sheath = NULL;
    J3DModel* sword = currentSword(&sheath);
    if (sword == NULL) {
        return;
    }

    if (sheath != NULL) {
        sheath->setBaseTRMtx(model->getAnmMtx(l_backJointNo));
        sheath->calc();
    }

    if ((mNetEquip & dusk::mp::kPlayerEquipSwordInHand) != 0) {
        sword->setBaseTRMtx(model->getAnmMtx(l_leftItemJointNo));
    } else {
        mDoMtx_stack_c::copy(model->getAnmMtx(l_backJointNo));
        mDoMtx_stack_c::transM(l_stowedSwordOffsetX, l_stowedSwordOffsetY, l_stowedSwordOffsetZ);
        mDoMtx_stack_c::XYZrotM(0, cM_deg2s(l_stowedSwordYawDeg), 0);
        sword->setBaseTRMtx(mDoMtx_stack_c::get());
    }

    sword->calc();
}

/**
 * Draw the sword and its sheath.
 *
 * ★ The bracket around the blade is the whole reason this is its own function.
 *
 * daAlink_c hides part of the sword model when it is sheathed and shows it when it is drawn —
 * material 0 for a real sword, material 1 for the wooden one (d_a_alink.cpp:4385-4395). He does it
 * on the MODEL DATA, which the puppet shares with him for the two swords that come out of "Alink".
 * So the flag is one variable serving two actors that can disagree about it, and a puppet with a
 * sheathed sword standing next to a local player with a drawn one would otherwise fight over it
 * every frame.
 *
 * Bracketing works because the flag is consumed at ENTRY time, not at draw time: J3DJoint::entryIn
 * tests it as it builds the packet (J3DJoint.cpp:164-165), and mDoExt_modelEntryDL is what runs
 * that. So setting it, entering, and putting it back leaves the local player's own state exactly as
 * it was by the time he enters his.
 *
 * The wooden sword needs no bracket — it comes from the puppet's private outfit archive — but gets
 * one anyway, because the alternative is a rule that is true for two of three cases.
 */
void daRemotePlayer_c::drawEquip() {
    J3DModel* sheath = NULL;
    J3DModel* sword = currentSword(&sheath);
    if (sword == NULL) {
        return;
    }

    const bool inHand = (mNetEquip & dusk::mp::kPlayerEquipSwordInHand) != 0;
    const u8 kind = static_cast<u8>(
        (mNetEquip & dusk::mp::kPlayerEquipSwordKindMask) >> dusk::mp::kPlayerEquipSwordKindShift);
    /* The material differs by sword AND the sense is inverted between them: the wooden sword hides
     * its material 1 when drawn, every other sword shows its material 0. Both are transcribed from
     * the same four lines rather than unified, because unifying them is how the inversion gets
     * lost. */
    const u16 bladeMatNo = kind == dusk::mp::kPlayerEquipSwordWood ? 1 : 0;
    const bool bladeVisible = kind == dusk::mp::kPlayerEquipSwordWood ? !inHand : inHand;

    J3DModelData* swordData = sword->getModelData();
    J3DShape* blade = NULL;
    bool bladeWasVisible = false;
    if (swordData != NULL && bladeMatNo < swordData->getMaterialNum()) {
        blade = swordData->getMaterialNodePointer(bladeMatNo)->getShape();
        // J3DShpFlag_Visible SET means hidden — hide() turns it on (J3DShape.h:171-172). Read the
        // flag rather than remembering what we last wrote: the local player writes it too.
        bladeWasVisible = !blade->checkFlag(J3DShpFlag_Visible);
        if (bladeVisible) {
            blade->show();
        } else {
            blade->hide();
        }
    }

    drawModel(sword);
    drawModel(sheath);

    if (blade != NULL) {
        if (bladeWasVisible) {
            blade->show();
        } else {
            blade->hide();
        }
    }

    if (!mLoggedEquip) {
        mLoggedEquip = true;
        Log.debug("Puppet {} drawing equipment: sword kind {} {} (blade material {} {}), sheath {} "
                  "| equip byte 0x{:02x}",
            mPlayerId, kind, inHand ? "in hand" : "on the back", bladeMatNo,
            bladeVisible ? "shown" : "hidden", sheath != NULL ? "yes" : "no", mNetEquip);
    }
}

void daRemotePlayer_c::setMatrix() {
    mDoMtx_stack_c::transS(current.pos);
    mDoMtx_stack_c::YrotM(shape_angle.y);
    model->setBaseTRMtx(mDoMtx_stack_c::get());

    /* The world→model matrices the foot IK solves in, built here for the same reason daAlink_c
     * builds his in the same place (d_a_alink.cpp:5781-5785): they are derived from the base
     * transform and would go stale the moment it changed. setBodySinkOffset then keeps all three in
     * step when it lowers the body. */
    mDoMtx_inverse(mDoMtx_stack_c::get(), mInvMtx);
    mDoMtx_stack_c::XrotS(shape_angle.x);
    mDoMtx_stack_c::concat(mInvMtx);
    mDoMtx_copy(mDoMtx_stack_c::get(), mFootLocalMtx);

    // Before calc: it reads the PREVIOUS calc's joint matrices and it moves the base transform.
    footBgCheck();
    /* Plain J3DModel::calc(), where this used to be mDoExt_McaMorfSO::modelCalc(). That call did
     * three things and only one of them is gone: it pushed the frame controller's frame into the
     * animation (animePlay() does that now, from execute(), for every slot on both halves), it
     * re-installed itself as joint 0's calculator every single frame (setupAnimation() installs the
     * two blend calculators once, and nothing removes them), and then it calc'd the model. */
    /* setFootMatrix() lands DURING this call, from the joint callback setupFootIk() installed on
     * joint 26 — not after it. calc() consumes the joint matrices before it returns. */
    model->calc();

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

    // Last, and after the body's calc for the same reason as the three above: the sword and sheath
    // hang off body joints, which are not final until calc has run.
    setEquipMatrix();
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
/// ★ This was written to mirror daMidna_c::setRoomInfo (d_a_midna.cpp:1171-1182), on the reasoning
/// that Midna is the game's own answer for a companion actor with no ground check of its own. That
/// was the WRONG template and it silently did nothing: Midna's query works only because she HOVERS,
/// so her origin is above the floor. Measured on a standing puppet, the fopAcM_gc_c query it used
/// failed 800 times out of 800 and this function took its fallback branch for the puppet's whole
/// life. It now reads daAlink_c's version instead — the collision result out of the actor's own
/// ground-check member — which is what every grounded actor in the tree does. Midna's reverb line
/// is still dropped: a puppet makes no sound of its own.
/**
 * Find the floor under the puppet, into the actor's OWN check object.
 *
 * Run once per tick from execute(), so the draw pass reads a settled answer instead of asking the
 * bg system a second time — and so mGndChk still describes THIS puppet's floor when the shadow
 * wants it. See the mGndChk comment in the header for why the query is raised and why the shared
 * fopAcM_gc_c static cannot do this job.
 */
void daRemotePlayer_c::groundCheck() {
    cXyz probe = current.pos;
    probe.y += l_gndCheckOffset;
    mGndChk.SetPos(&probe);
    mGroundHeight = dComIfG_Bgsp().GroundCross(&mGndChk);
    mGroundValid = mGroundHeight != -G_CM3D_F_INF;
}

void daRemotePlayer_c::setRoomInfo() {
    int room_no;
    if (mGroundValid) {
        room_no = dComIfG_Bgsp().GetRoomId(mGndChk);
        tevStr.YukaCol = dComIfG_Bgsp().GetPolyColor(mGndChk);
    } else {
        // Over a hole, mid-warp, or handed a pose with no floor under it. Keep the last floor
        // colour and fall back to the room the local player is in, which is the room the puppet is
        // being drawn into anyway.
        room_no = dComIfGp_roomControl_getStayNo();
    }
    tevStr.room_no = room_no;
    fopAcM_SetRoomNo(this, room_no);
}

/* ★ TEMPORARY — Hang 4 bisection. See the declaration for why a trace stands in for a minidump
 * here. Traces only the first few calcs because the hang is always on the FIRST one (14 of 14), so
 * a handful of lines is enough and the log stays readable. */
void daRemotePlayer_c::traceCalc(const char* i_step) {
    if (mCalcTraced < l_calcTraceNum) {
        Log.debug("Puppet {} calc {}: -> {}", mPlayerId, mCalcTraced, i_step);
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
        Log.debug("Puppet {} first calc: model={:#x} under/upper calc={:#x}/{:#x} idle={:#x}/{:#x} "
                  "walk={:#x}/{:#x} run={:#x}/{:#x} runUpper={:#x}/{:#x}",
            mPlayerId, reinterpret_cast<uintptr_t>(model), reinterpret_cast<uintptr_t>(mpUnderCalc),
            reinterpret_cast<uintptr_t>(mpUpperCalc), reinterpret_cast<uintptr_t>(mIdleAnm.mpUnder),
            mIdleAnm.mpUnder != NULL ? *reinterpret_cast<const uintptr_t*>(mIdleAnm.mpUnder) : 0,
            reinterpret_cast<uintptr_t>(mWalkAnm.mpUnder),
            mWalkAnm.mpUnder != NULL ? *reinterpret_cast<const uintptr_t*>(mWalkAnm.mpUnder) : 0,
            reinterpret_cast<uintptr_t>(mRunAnm.mpUnder),
            mRunAnm.mpUnder != NULL ? *reinterpret_cast<const uintptr_t*>(mRunAnm.mpUnder) : 0,
            reinterpret_cast<uintptr_t>(mRunAnm.mpUpper),
            mRunAnm.mpUpper != NULL ? *reinterpret_cast<const uintptr_t*>(mRunAnm.mpUpper) : 0);
    }

    // No guard against the local player owning this model data, and none needed: the puppet's
    // J3DModelData came out of its own private archive mount, so daAlink_c's joint callbacks and
    // mtx calculators are not on it and cannot be.
    traceCalc("selectAnimation");
    selectAnimation();
    // Immediately after, and never before: the hand pose is a column of the animation's own table
    // row, so it can only be right once the animation for this tick has been chosen.
    traceCalc("setDrawHand");
    setDrawHand();
    traceCalc("animePlay");
    animePlay();
    // Independent of the body: the puppet blinks while standing still as much as while running,
    // which is the whole point — a face frozen mid-stare is what reads as "not a real player".
    traceCalc("playFaceTextureAnime");
    playFaceTextureAnime();
    // Before setMatrix, so the ground check runs against the pose the puppet is about to be drawn
    // at rather than the previous tick's.
    traceCalc("groundCheck");
    groundCheck();
    traceCalc("setRoomInfo");
    setRoomInfo();
    traceCalc("setMatrix");
    setMatrix();
    // After setMatrix, not before: the aim is measured from the head joint's world matrix, which
    // only exists once modelCalc() has run.
    traceCalc("setEyeMove");
    setEyeMove();
    // Same reason, and in the same place daAlink_c puts it (d_a_alink.cpp:18530-18538): the sway is
    // integrated from how far this tick's matrices moved, and the joint callback applies the result
    // during the NEXT tick's calc.
    traceCalc("setHatAngle");
    setHatAngle();
    // Diagnostic for A5, silent unless the material state actually moves. Last, so it reports the
    // state the draw pass is about to use.
    traceCalc("checkMaterialDrift");
    checkMaterialDrift();
    traceCalc("execute done");
    if (mCalcTraced < l_calcTraceNum) {
        mCalcTraced++;
    }
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

/// Cast the puppet's shadow onto the floor, the same way the local player casts his.
///
/// Without this the puppet is the only character in the scene standing on nothing, which reads as
/// "pasted on top of the world" far more strongly than any shading difference does. daAlink_c does
/// it in `shadowDraw()` (d_a_alink.cpp:19201-19362) and this is that function with everything
/// player-specific removed — no horse, no boar, no canoe, no Midna, no held item, no boots.
///
/// **This is the REAL (projected) shadow, not the simple blob.** That was a deliberate choice
/// between the two APIs, and the blob was the tempting one because it is cheaper and cannot fail:
///
///  - `dComIfGd_setSimpleShadow` draws a flat round texture on the ground plane. A round blob
///    parked next to the local player's crisp silhouette would itself be the tell we are trying to
///    remove — it would say "that one is not a real character" every bit as loudly as no shadow.
///  - `dComIfGd_setShadow` (d_com_inf_game.cpp:2277-2288) projects the actual models. Checked
///    before committing to it: nothing on that path touches the local player. It forwards to
///    `dDlst_shadowControl_c::setReal` -> `dDlst_shadowReal_c::set` -> `setShadowRealMtx`
///    (d_drawlist.cpp:1692, :1308, :1245), and the only place that path reaches for player-ish
///    global state is `dKy_plight_near_pos()` at d_drawlist.cpp:1318 — which is the `tevStr ==
///    NULL` branch, and we pass ours. There is no `daPy_getPlayerActorClass()` anywhere in
///    d_drawlist.cpp (grepped), so the unguarded deref that 00-status.md warns about in the
///    type-9/10 lighting path has no counterpart here. This adds no new exposure to a teardown
///    window in which the puppet draws and Link does not; `draw()` already calls
///    `settingTevStruct(10, ...)`, which is the call that carries that risk, and this changes
///    nothing about it.
///
/// The cost is one of the **eight** global real-shadow slots (`dDlst_shadowControl_c::mReal[8]`,
/// d_drawlist.h:314), so two players spend two. When all eight are taken `setReal` returns 0 and
/// the newest caller simply gets no shadow that frame (d_drawlist.cpp:1738-1741) — a graceful
/// degradation the game already lives with. The local player can never be the one to lose out:
/// daAlink_c is created before any puppet, so at equal draw priority he registers first.
///
/// Zero heap cost. Every slot is in the statically allocated draw list; the only new storage is
/// `mShadowKey`, four bytes in the actor struct, which is not on the 0x20000 solid heap at all.
void daRemotePlayer_c::shadowDraw() {
    if (model == NULL) {
        return;
    }

    /* The ground under the puppet's feet, found during execute() by groundCheck() into this
     * actor's own mGndChk. Deliberately NOT re-queried here: the shared fopAcM_gc_c static that
     * this used to call cannot answer the question at all for a grounded actor (header comment),
     * and a per-actor member is exactly what daAlink_c reads at the same point
     * (mLinkAcch.m_gnd, d_a_alink.cpp:19246), so there is no stale-static hazard to dodge. */
    if (!mGroundValid) {
        // Over a hole, mid-warp, or handed a pose with no floor under it. No ground, no shadow —
        // and this is dComIfGd_setShadow's own guard restated (d_com_inf_game.cpp:2280), so we are
        // only declining a call that would have declined itself.
        mShadowKey = 0;
        return;
    }

    const f32 groundY = mGroundHeight;

    /* Shadow centre = the body model's ROOT JOINT in world space, NOT current.pos. daAlink_c feeds
     * `field_0x3834` (d_a_alink.cpp:19226), which is exactly this quantity (:5605). It matters
     * because the root joint carries the animation's own translation, so the shadow leans and
     * slides with the pose instead of staying pinned under the actor origin. Valid by this point:
     * setMatrix() ran modelCalc() during execute(), and draw() has already returned early if no
     * network pose has landed. */
    cXyz shadowPos;
    mDoMtx_multVecZero(model->getAnmMtx(0), &shadowPos);

    /* Argument by argument against daAlink_c's call (d_a_alink.cpp:19246):
     *
     *  - water flag 0. Link computes `mWaterY > groundH`, and on dry land that is 0 for him too —
     *    including while airborne, since mWaterY is -inf with no water — so 0 reproduces his
     *    behaviour exactly everywhere except over water, where his shadow fades with height and
     *    ours would not. Water state is not replicated at all yet, and faking a bg water query here
     *    would buy a fade nobody can see under a swimming puppet. Revisit when water is on the
     *    wire.
     *  - 800.0f radius and the trailing 0, 1.0f, simple-texture arguments are Link's verbatim, so
     *    the puppet's shadow is sized and shaped like his rather than merely present.
     *  - current.pos.y as the caster height. Link passes the lower of his two target cylinders'
     *    centres, which `setCollisionPos` places at foot level (d_a_alink.cpp:6721-6738), so the
     *    height-above-ground this resolves to is ~0 while grounded and grows in the air. The actor
     *    origin is at the feet, so current.pos.y is that same quantity. daCow_c passes exactly this
     *    pair for the same reason (d_a_cow.cpp:3256-3258).
     *  - &tevStr, not NULL. Non-null is what keeps `dDlst_shadowReal_c::set` on its own light
     *    direction (mLightPosWorld, filled by the settingTevStruct(10) call above) instead of
     *    falling through to the scene-global one — and, incidentally, out of the branch at
     *    d_drawlist.cpp:1324 that writes through a pointer it has just proven to be NULL. */
    mShadowKey = dComIfGd_setShadow(mShadowKey, 0, model, &shadowPos, 800.0f, 0.0f, current.pos.y,
        groundY, mGndChk, &tevStr, 0, 1.0f, dDlst_shadowControl_c::getSimpleTex());

    /* One line, once, the first time a shadow is actually granted. This feature fails SILENTLY —
     * setReal declines for several reasons and simply returns 0, and the puppet then looks exactly
     * as it did when it had no shadow code at all. That is how the original ground-check bug
     * survived: the code was present, compiled, called every frame, and never once succeeded. If
     * this line is absent from a session's log, the puppet had no shadow for the whole session. */
    if (mShadowKey != 0 && !mLoggedShadow) {
        mLoggedShadow = true;
        Log.debug("Puppet {} shadow registered (key {}, ground {:.1f}, {:.1f} above it)", mPlayerId,
            mShadowKey, groundY, current.pos.y - groundY);
    }

    if (mShadowKey != 0) {
        // The other three models cast into the SAME shadow, so the silhouette has a head and hands
        // rather than being a decapitated torso. Null-tolerant by the API's own contract:
        // dDlst_shadowReal_c::add returns false for a NULL model (d_drawlist.cpp:1355-1357), so a
        // missing sub-model costs that part of the outline and nothing else. Same set, same order,
        // as daAlink_c (d_a_alink.cpp:19277-19279).
        dComIfGd_addRealShadow(mShadowKey, mpHeadModel);
        dComIfGd_addRealShadow(mShadowKey, mpFaceModel);
        dComIfGd_addRealShadow(mShadowKey, mpHandModel);
    }
}

int daRemotePlayer_c::draw() {
    if (!mHasPose) {
        return 1;
    }

    /* ★★ HANG 4. This is the fix, and it is the whole of it.
     *
     * A J3DModel may be ENTERED into the draw list at most once per pass. Enter it twice and the
     * mat packet's shape chain is made to point at itself (J3DJoint::entryIn's setShapePacket,
     * J3DJoint.cpp:164-180), and the renderer then walks that list forever — which is what the
     * Aurora FIFO guard catches as "command stream reached 268435456 bytes".
     *
     * With frame interpolation on, every actor is legitimately drawn TWICE per presentation frame
     * (m_Do_main.cpp:298-322): once inside fapGm_Execute with frame_interp::is_sim_frame() true,
     * and once from fpcM_DrawIterater with it false. That is safe, because mDoExt_modelEntryDL
     * early-outs to J3DModel::diff() on the second (m_Do_ext.cpp) and never re-enters.
     *
     * The puppet was getting TWO draws with is_sim_frame() TRUE, both on the SAME sim tick. It is
     * measured rather than reasoned — the two ends of one run, at the puppet's first draw:
     *
     *   host  : simFrame YES simTickSeq 725 / simFrame YES simTickSeq 725   -> FATAL, every time
     *   guest : simFrame YES simTickSeq 554 / simFrame YES simTickSeq 555   -> healthy, every time
     *
     * That is the whole of the host/guest asymmetry that made this look intermittent for two
     * sessions: whether the puppet's two entry-draws land inside one sim tick or straddle two is a
     * matter of exactly when in the frame the network layer got the actor created, which is why it
     * came and went with timing and never with code.
     *
     * So: one entry per sim tick, enforced here. The second call is a DUPLICATE, not an
     * interpolated frame, and skipping it is the correct handling rather than a mitigation — the
     * model is already in the list, and shadowDraw() would otherwise burn a second of the eight
     * global real-shadow slots on the same puppet. Interpolated frames (is_sim_frame() false) are
     * never skipped: they carry the diff() that makes the puppet move between sim ticks.
     *
     * Deliberately fixed HERE and not in the actor framework. The framework drawing a
     * just-created actor twice may well be general, but every other actor in the game is created
     * during a scene load rather than from a network event mid-session, and this is Dusk-authored
     * code that cannot affect the local player. If it turns out to bite a stock actor too, the
     * framework is the place — see 00-status.md. */
    if (dusk::frame_interp::is_sim_frame()) {
        const u64 simTick = dusk::frame_interp::sim_tick_seq();
        if (mHasDrawnSimTick && mDrawnSimTick == simTick) {
            if (!mLoggedDoubleDraw) {
                mLoggedDoubleDraw = true;
                Log.debug("Puppet {} skipped a SECOND entry-draw on sim tick {} — this is Hang 4's "
                          "cycle, caught",
                    mPlayerId, simTick);
            }
            return 1;
        }
        mDrawnSimTick = simTick;
        mHasDrawnSimTick = true;
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

    traceCalc("draw: settingTevStruct");
    g_env_light.settingTevStruct(10, &current.pos, &tevStr);

    g_env_light.UseCol = savedUseCol;
    g_env_light.PrevCol = savedPrevCol;
    g_env_light.pat_ratio = savedPatRatio;
    g_env_light.plight_near_pos = savedPlightNearPos;
    traceCalc("draw: body");
    drawModel(model);
    // Same order daAlink_c draws them in, which matters for the face: it is drawn after the head so
    // it wins the depth fight at the eyes rather than being buried inside it.
    drawModel(mpHeadModel);
    drawModel(mpFaceModel);
    drawModel(mpHandModel);
    // After the body, exactly where daAlink_c draws them (d_a_alink.cpp:19610-19617): they are
    // separate models rather than geometry on the body, and they read their own visibility.
    traceCalc("draw: equipment");
    drawEquip();
    // Last, exactly where daAlink_c puts it (d_a_alink.cpp:19849-19853): the shadow projects the
    // models, so it wants them posed and their tevStr settled, and it renders in its own later pass
    // rather than into the draw list we have just filled.
    traceCalc("draw: shadow");
    shadowDraw();
    traceCalc("draw done");
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
