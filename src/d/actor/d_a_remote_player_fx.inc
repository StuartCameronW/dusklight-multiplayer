/**
 * d_a_remote_player_fx.inc
 * Remote player puppet: the damage / freeze / ice body tint, the lantern's glow and flame, and the
 * puppet's particle emitters (protocol v20 effects block).
 *
 * Dusk-only, like the rest of the puppet. Included once, at the end of d_a_remote_player.cpp, after
 * the held-item file whose lantern model and loaders it uses.
 *
 * Same rule as the held-item file: daAlink_c's own code, transcribed and cited. The sender decides
 * WHICH tint and WHETHER the flame burns (player_bridge.cpp fill_damage_flash / fill_lantern_fx);
 * every colour, offset and rate below is his, read from his HIO tables rather than copied.
 *
 * Not reproduced: the lantern's LIGHT. daAlink_c::setLight lights the scene with
 * dKy_WolfEyeLight_set (d_a_alink.cpp:14980), which writes g_env_light.field_0x0c18[0] — ONE
 * global slot (d_kankyo.cpp:10178-10210) that the local player's own lantern and wolf senses also
 * write every frame. A puppet calling it would steal the local player's light, so it does not.
 * Also not reproduced: the swing flame (ZI_J_KANTERA_SWINGFIRE and its dPa_levelEcallBack), since
 * the swing proc is not on the wire.
 */

#include "d/d_camera.h"
#include "d/d_particle.h"
#include "d/d_particle_name.h"
#include "dusk/player_fx.hpp"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_lib.h"

namespace {

/// One material whose TEV colour register 1 daAlink_c::setWaterDropColor writes. mModel 0 is the
/// body (his field_0x064C), 1 the head (his mpLinkHatModel).
struct DmgReg1Mat {
    u8 mModel;
    u8 mMat;
};

/// setWaterDropColor's list for one outfit (d_a_alink.cpp:19404-19470), and the TARGET_PC bounds
/// guard that branch carries on the body's material count.
struct DmgReg1List {
    u16 mBodyMatMin;
    u8 mNum;
    DmgReg1Mat mMats[9];
};

/* In l_outfits order. Bmdl is his checkCasualWearFlg branch, Kmdl the last (hero's clothes)
 * branch, Zmdl checkZoraWearAbility and Mmdl checkMagicArmorWearAbility. For the freeze and ice
 * tints his i_color and var_r31 are the same colour, so the two halves of each list are one. */
const DmgReg1List l_dmgReg1[] = {
    {8, 3, {{0, 7}, {1, 0}, {0, 5}}},
    {18, 9, {{0, 17}, {0, 9}, {0, 0}, {0, 1}, {0, 2}, {1, 0}, {0, 16}, {0, 15}, {0, 14}}},
    {14, 4, {{0, 13}, {0, 0}, {0, 1}, {1, 1}}},
    {12, 7, {{0, 11}, {0, 10}, {0, 9}, {0, 8}, {0, 6}, {1, 2}, {1, 1}}},
};
const int l_dmgReg1Num = sizeof(l_dmgReg1) / sizeof(l_dmgReg1[0]);

const u16 l_fxLogMax = 64;

const char* puppet_dmg_tint_name(u8 i_kind) {
    switch (i_kind) {
    case dusk::mp::kPlayerDmgTintDamage:
        return "damage";
    case dusk::mp::kPlayerDmgTintFreeze:
        return "freeze";
    case dusk::mp::kPlayerDmgTintIce:
        return "ice";
    default:
        return "none";
    }
}

/// The lantern's joint-1 trampoline: daAlink_kandelaarModelCallBack
/// (d_a_alink_kandelaar.inc:141-151).
int daRemotePlayer_lanternModelCallBack(J3DJoint* i_joint, int i_pass) {
    UNUSED(i_joint);
    if (i_pass != 0) {
        return 1;
    }
    J3DModel* lantern = j3dSys.getModel();
    if (lantern == NULL) {
        return 1;
    }
    daRemotePlayer_c* puppet = reinterpret_cast<daRemotePlayer_c*>(lantern->getUserArea());
    if (puppet != NULL) {
        puppet->lanternModelCallBack();
    }
    return 1;
}

}  // namespace

/* --- the network side ------------------------------------------------------------------------ */

void daRemotePlayer_c::setNetworkDamageLantern(u8 i_tint, u8 i_flash, bool i_lanternLit) {
    const u8 tint = i_tint <= dusk::mp::kPlayerDmgTintIce ? i_tint : dusk::mp::kPlayerDmgTintNone;
    if (tint != mNetDmgTint && mDmgTintLogCount < l_fxLogMax) {
        mDmgTintLogCount++;
        Log.debug("Puppet {} damage tint {} -> {} (dmgFlash {})", mPlayerId,
            puppet_dmg_tint_name(mNetDmgTint), puppet_dmg_tint_name(tint),
            static_cast<unsigned>(i_flash));
    }
    if (i_lanternLit != mNetLanternLit && mLanternFxLogCount < l_fxLogMax) {
        mLanternFxLogCount++;
        Log.debug("Puppet {} told lantern {} (lantern {})", mPlayerId, i_lanternLit ? "LIT" : "out",
            (mNetHeldFlags & dusk::mp::kPlayerHeldLanternShown) != 0 ? "shown" : "not shown");
    }
    mNetDmgTint = tint;
    mNetDmgFlash = i_flash;
    mNetLanternLit = i_lanternLit;
}

/* --- damage / freeze / ice tint -------------------------------------------------------------- */

/**
 * daAlink_c::draw's tint ladder (d_a_alink.cpp:19530-19583), the three branches the wire carries,
 * onto this puppet's tevStr. From draw(), right after settingTevStruct and before the first model:
 * setLightTevColorType_MAJI copies tevStr.TevColor / TevKColor into every material's register 0 as
 * each model is entered (d_kankyo.cpp:4323-4326), so what is in tevStr at that moment is the tint.
 *
 *   - damage: his colour_timer is the wire's dmgFlash. Above 32 / above 16 / else picks
 *     mDamage{R,G,B}{0,1,2}, scaled by |sin(colour_timer * 0x800)| into TevKColor. The
 *     Magic-Armor-absorbed hit never arrives here: the sender sends None for it.
 *   - freeze: a flat mFreeze{R,G,B} into TevColor.
 *   - ice: mFreezeInit* blended with mFreeze* by mIceDamageWaitTimer / mFreezeTime into TevColor.
 *
 * TevColor (freeze, ice) is left in place for everything drawn after the body — sword, shield,
 * held items, lantern, boots — as his is. TevKColor (damage) is the body's alone; see
 * clearDamageKColor(). Both are zeroed first, which is initTevCustomColor's colour half
 * (:19456-19463): type 10's settingTevStruct does not touch either (d_kankyo.cpp:3897-3910 is types
 * 12/13 only), and dKy_tevstr_init left them 0, so with no tint this changes nothing.
 */
void daRemotePlayer_c::applyDamageTint() {
    tevStr.TevColor.r = 0;
    tevStr.TevColor.g = 0;
    tevStr.TevColor.b = 0;
    tevStr.TevKColor.r = 0;
    tevStr.TevKColor.g = 0;
    tevStr.TevKColor.b = 0;

    const daAlinkHIO_damage_c1& hio = daAlinkHIO_damage_c0::m;
    bool reg1 = false;
    const u8 kind = mNetDmgTint;
    switch (kind) {
    case dusk::mp::kPlayerDmgTintFreeze:
        tevStr.TevColor.r = hio.mFreezeR;
        tevStr.TevColor.g = hio.mFreezeG;
        tevStr.TevColor.b = hio.mFreezeB;
        reg1 = true;
        break;
    case dusk::mp::kPlayerDmgTintDamage: {
        const int colorTimer = mNetDmgFlash;
        int r, g, b;
        if (colorTimer > 32) {
            r = hio.mDamageR0;
            g = hio.mDamageG0;
            b = hio.mDamageB0;
        } else if (colorTimer > 16) {
            r = hio.mDamageR1;
            g = hio.mDamageG1;
            b = hio.mDamageB1;
        } else {
            r = hio.mDamageR2;
            g = hio.mDamageG2;
            b = hio.mDamageB2;
        }
        const f32 pulse = std::fabs(cM_ssin(static_cast<s16>(colorTimer * 0x800)));
        tevStr.TevKColor.r = static_cast<u8>(r * pulse);
        tevStr.TevKColor.g = static_cast<u8>(g * pulse);
        tevStr.TevKColor.b = static_cast<u8>(b * pulse);
        break;
    }
    case dusk::mp::kPlayerDmgTintIce: {
        const f32 initRatio = 1.0f - (mNetDmgFlash * (1.0f / hio.mFreezeTime));
        const f32 freezeRatio = 1.0f - initRatio;
        tevStr.TevColor.r =
            static_cast<s16>((initRatio * hio.mFreezeInitR) + (freezeRatio * hio.mFreezeR));
        tevStr.TevColor.g =
            static_cast<s16>((initRatio * hio.mFreezeInitG) + (freezeRatio * hio.mFreezeG));
        tevStr.TevColor.b =
            static_cast<s16>((initRatio * hio.mFreezeInitB) + (freezeRatio * hio.mFreezeB));
        reg1 = true;
        break;
    }
    default:
        break;
    }

    setDamageReg1(reg1);

    if (kind != dusk::mp::kPlayerDmgTintNone && kind < 4 && !mLoggedDmgTint[kind]) {
        mLoggedDmgTint[kind] = true;
        Log.debug("Puppet {} drawing {} tint: dmgFlash {} -> TevColor ({}, {}, {}) TevKColor ({}, "
                  "{}, {}), reg-1 materials {}",
            mPlayerId, puppet_dmg_tint_name(kind), static_cast<unsigned>(mNetDmgFlash),
            static_cast<int>(tevStr.TevColor.r), static_cast<int>(tevStr.TevColor.g),
            static_cast<int>(tevStr.TevColor.b), static_cast<unsigned>(tevStr.TevKColor.r),
            static_cast<unsigned>(tevStr.TevKColor.g), static_cast<unsigned>(tevStr.TevKColor.b),
            mDmgReg1On ? "tinted" : "untouched");
    }
}

/// d_a_alink.cpp:19731-19733: he zeroes TevKColor after the face, so the damage pulse is on the
/// body, head, face and hands only. From draw(), right before drawEquip().
void daRemotePlayer_c::clearDamageKColor() {
    tevStr.TevKColor.r = 0;
    tevStr.TevKColor.g = 0;
    tevStr.TevKColor.b = 0;
}

/**
 * The register-1 half of the freeze and ice tints. For those two his ladder leaves var_r31 TRUE and
 * calls setWaterDropColor(&tevStr.TevColor) (d_a_alink.cpp:19587-19598), which puts the tint into
 * register 1 of a per-outfit list of body and head materials as well.
 *
 * Every other tick he writes the water-drop colour there instead, which the puppet does not model:
 * nothing else on the puppet writes these registers, so they hold the BMD's own values, and those
 * are what is put back when the tint ends. Saved once, before the first write; the head and body
 * are private copies out of the puppet's own archive mount, so nobody else can change them.
 */
void daRemotePlayer_c::setDamageReg1(bool i_on) {
    if (!i_on && !mDmgReg1On) {
        return;
    }
    if (mOutfit < 0 || mOutfit >= l_dmgReg1Num || model == NULL) {
        mDmgReg1On = false;
        return;
    }

    const DmgReg1List& list = l_dmgReg1[mOutfit];
    J3DModelData* body = model->getModelData();
    J3DModelData* head = mpHeadModel != NULL ? mpHeadModel->getModelData() : NULL;
    if (body->getMaterialNum() < list.mBodyMatMin) {
        mDmgReg1On = false;
        return;
    }

    for (int i = 0; i < list.mNum; i++) {
        J3DModelData* data = list.mMats[i].mModel == 0 ? body : head;
        if (data == NULL || list.mMats[i].mMat >= data->getMaterialNum()) {
            continue;
        }
        J3DMaterial* mat = data->getMaterialNodePointer(list.mMats[i].mMat);
        if (!mDmgReg1Saved) {
            const J3DGXColorS10* own = mat->getTevColor(1);
            if (own != NULL) {
                mDmgReg1Orig[i] = *own;
            } else {
                mDmgReg1Orig[i].r = 0;
                mDmgReg1Orig[i].g = 0;
                mDmgReg1Orig[i].b = 0;
                mDmgReg1Orig[i].a = 0;
            }
        }
        mat->setTevColor(
            1, i_on ? (J3DGXColorS10*)&tevStr.TevColor : (J3DGXColorS10*)&mDmgReg1Orig[i]);
    }
    mDmgReg1Saved = true;
    mDmgReg1On = i_on;
}

/* --- lantern glow + flame -------------------------------------------------------------------- */

/**
 * The lantern's glow model and its BTK, built as daAlink_c builds his (d_a_alink_wolf.inc:407-413):
 * ef_ktGlow.bmd from the outfit archive with initModel(data, 0x200), and ef_ktGlow.btk at frame 0,
 * matched to the data and entered as its texture-matrix animator. Both resources are resident in
 * the puppet's PRIVATE outfit mount, so the only heap this costs is the J3DModel instance, and the
 * BTK is this puppet's alone to step.
 *
 * Plus his swing: the lantern model's joint-1 callback (d_a_alink_wolf.inc:506), installed on the
 * puppet's own copy of the lantern data. It is what computes mKandelaarFlamePos, the point the
 * flame and the glow are placed at, and it swings the lantern body as his swings.
 *
 * From setupHeldItems(), right after the lantern model. Every part is non-fatal: no lantern model,
 * no glow or no BTK costs that part only. Also resets the emitter slots, which other features use.
 */
void daRemotePlayer_c::setupLanternFx() {
    mpLanternGlowModel = NULL;
    mpLanternGlowBtk = NULL;
    mLanternSwingHooked = false;
    mLanternSwingLive = false;
    mLanternGlowScale = 0.0f;
    mLanternGlowPeek = 0;
    mLanternGlowZ = -1.0f;
    mLanternGlowPosed = false;
    for (int i = 0; i < PUPPET_FX_NUM; i++) {
        mFxEmitter[i] = 0;
        mLoggedFxFail[i] = false;
    }

    if (mpHeldLanternModel == NULL) {
        Log.info("Puppet {} lantern fx: no lantern model, so no glow, flame or swing", mPlayerId);
        return;
    }

    JKRHeap* heap = JKRGetCurrentHeap();
    const s32 freeBefore = heap != NULL ? heap->getFreeSize() : 0;

    J3DModelData* lanternData = mpHeldLanternModel->getModelData();
    if (lanternData->getJointNum() > 1) {
        mpHeldLanternModel->setUserArea((uintptr_t)this);
        lanternData->getJointNodePointer(1)->setCallBack(daRemotePlayer_lanternModelCallBack);
        mLanternSwingHooked = true;
    }

    J3DModelData* glowData = static_cast<J3DModelData*>(own_archive_res(mOwnRes, "ef_ktGlow.bmd"));
    if (glowData != NULL && held_heap_fits(0, "lantern glow")) {
        mpLanternGlowModel = init_model(glowData, 0x200, "lantern glow");
    }
    if (mpLanternGlowModel != NULL) {
        J3DAnmTextureSRTKey* btk =
            static_cast<J3DAnmTextureSRTKey*>(own_archive_res(mOwnRes, "ef_ktGlow.btk"));
        if (btk != NULL) {
            btk->setFrame(0.0f);
            btk->searchUpdateMaterialID(glowData);
            glowData->entryTexMtxAnimator(btk);
            mpLanternGlowBtk = btk;
        }
    }

    Log.info("Puppet {} lantern fx: glow {} (btk {}), swing hook {} ({} joints), {} bytes of heap",
        mPlayerId, mpLanternGlowModel != NULL ? "ok" : "MISSING",
        mpLanternGlowBtk != NULL ? "ok" : "MISSING", mLanternSwingHooked ? "ok" : "NONE",
        lanternData->getJointNum(), heap != NULL ? freeBefore - heap->getFreeSize() : 0);
}

/**
 * daAlink_c::kandelaarModelCallBack (d_a_alink_kandelaar.inc:98-139), on the puppet's lantern:
 * the flame hangs 17 units below joint 1 and trails it as a damped pendulum, the lantern body is
 * turned to match, and the result is mLanternFlamePos. Runs inside the lantern's calc, from
 * setHeldItemMatrix(), so the body joints it seeds from are final.
 *
 * Two branches of his are not taken: the OPEN_TREASURE / GET_ITEM exception (a proc the puppet
 * does not have) and the ERFLG1_UNK_8 one (setKandelaarMtx, a cutscene's lantern — never the one
 * the wire shows).
 */
int daRemotePlayer_c::lanternModelCallBack() {
    if (mpHeldLanternModel == NULL) {
        return 1;
    }

    cXyz jointPos;
    mDoMtx_multVecZero(J3DSys::mCurrentMtx, &jointPos);

    if (!mLanternSwingLive) {
        // setKandelaarModel's reset as the lantern comes out (d_a_alink_kandelaar.inc:208-221).
        if (model != NULL) {
            mDoMtx_multVecZero(model->getAnmMtx(l_leftItemJointNo), &mLanternFlamePos);
        } else {
            mLanternFlamePos = jointPos;
        }
        mLanternFlamePos.y -= 30.0f;
        mLanternFlamePrev = mLanternFlamePos;
        mLanternFlameVel = cXyz::Zero;
        mLanternGlowPeek = 0;
        mLanternGlowScale = 0.0f;
        mLanternGlowZ = -1.0f;
        mLanternSwingLive = true;
    }

    mLanternFlamePrev = mLanternFlamePos;

    cXyz swing = (mLanternFlamePos - jointPos) + mLanternFlameVel;
    swing.y -= 3.0f;

    cXyz front;
    mDoMtx_multVec(J3DSys::mCurrentMtx, &cXyz::BaseZ, &front);
    const s16 yaw = front.atan2sX_Z();
    mDoMtx_stack_c::YrotS(-yaw);
    mDoMtx_stack_c::multVec(&swing, &swing);

    const s16 rotX = cLib_minMaxLimit<s16>(cM_atan2s(-swing.z, -swing.y), -0x1800, 0x1800);
    const s16 rotZ = cLib_minMaxLimit<s16>(
        cM_atan2s(swing.x, JMAFastSqrt(SQUARE(swing.y) + SQUARE(swing.z))), -0x1800, 0x1800);

    mDoMtx_stack_c::transS(jointPos);
    mDoMtx_stack_c::ZXYrotM(rotX, yaw, rotZ);
    static const Vec flameOffset = {0.0f, -17.0f, 0.0f};
    mDoMtx_stack_c::multVec(&flameOffset, &mLanternFlamePos);

    mLanternFlameVel = (mLanternFlamePos - mLanternFlamePrev) * 0.9f;

    const f32 scale =
        JMAFastSqrt(SQUARE(J3DSys::mCurrentMtx[0][0]) + SQUARE(J3DSys::mCurrentMtx[1][0]) +
                    SQUARE(J3DSys::mCurrentMtx[2][0]));
    mDoMtx_stack_c::transS(
        J3DSys::mCurrentMtx[0][3], J3DSys::mCurrentMtx[1][3], J3DSys::mCurrentMtx[2][3]);
    mDoMtx_stack_c::ZXYrotM(rotX, yaw, rotZ);
    mDoMtx_stack_c::scaleM(scale, scale, scale);
    mpHeldLanternModel->setAnmMtx(1, mDoMtx_stack_c::get());
    cMtx_copy(mDoMtx_stack_c::get(), J3DSys::mCurrentMtx);
    return 1;
}

/**
 * The flame and the glow's pose, every tick from execute(), right after setMatrix() (whose lantern
 * calc has just run the swing callback). His setLight (d_a_alink.cpp:14862-14909) and the glow half
 * of setItemMatrix (:6093-6114):
 *
 *   - lit: ZI_J_KANTERA_FIRE refreshed at the flame, with his shape_angle and the puppet's tevStr,
 *     on PUPPET_FX_LANTERN_FIRE. Not lit, or the lantern put away: stopped, which ends it (see
 *     stopPuppetEmitter).
 *   - the glow eases its scale toward 1 while lit and in view, toward 0 otherwise, at his rates
 *     (cLib_addCalc 0.5 / 0.3 / 0.1). "In view" is his peekZ test: the glow hides once the depth
 *     buffer at the flame is nearer than the flame (drawLanternGlow takes the reading).
 *   - the BTK steps one frame (simpleAnmPlay), and the glow sits at the flame, unrotated.
 *
 * Put away, it also re-arms setKandelaarModel's reset for the next time the lantern comes out.
 */
void daRemotePlayer_c::updateLanternFx() {
    mLanternGlowPosed = false;

    const bool shown = (mNetHeldFlags & dusk::mp::kPlayerHeldLanternShown) != 0 &&
                       mpHeldLanternModel != NULL && mLanternSwingHooked && mLanternSwingLive;
    if (!shown) {
        mLanternSwingLive = false;
        stopPuppetEmitter(PUPPET_FX_LANTERN_FIRE);
        return;
    }

    const bool lit = mNetLanternLit;
    if (lit) {
        setPuppetEmitter(
            PUPPET_FX_LANTERN_FIRE, ID_ZI_J_KANTERA_FIRE, &mLanternFlamePos, &shape_angle);
    } else {
        stopPuppetEmitter(PUPPET_FX_LANTERN_FIRE);
    }

    const f32 target = (mLanternGlowZ > (f32)mLanternGlowPeek || !lit) ? 0.0f : 1.0f;
    cLib_addCalc(&mLanternGlowScale, target, 0.5f, 0.3f, 0.1f);

    if (mpLanternGlowModel != NULL) {
        if (mpLanternGlowBtk != NULL) {
            daAlink_c::simpleAnmPlay(mpLanternGlowBtk);
        }
        Vec glowScale = {mLanternGlowScale, mLanternGlowScale, mLanternGlowScale};
        mpLanternGlowModel->setBaseScale(glowScale);
        mDoMtx_stack_c::transS(mLanternFlamePos);
        mpLanternGlowModel->setBaseTRMtx(mDoMtx_stack_c::get());
        mpLanternGlowModel->calc();
        mLanternGlowPosed = true;
    }
}

/**
 * preKandelaarDraw (d_a_alink_kandelaar.inc:167-209), then the glow, as daAlink_c draws them right
 * after the lantern (d_a_alink.cpp:19822-19825). From drawHeldItems(), inside the lantern block.
 *
 * The material colours are his HIO pair, set every draw as he sets them. The peek asks for the
 * depth buffer under the flame's screen position; dDlst_peekZ_c fills mLanternGlowPeek at the
 * start of the NEXT frame's fpcM_Management, before the delete handler runs
 * (f_pc_manager.cpp:52-56 vs :83-87), so the member is alive when it is written even if this
 * puppet is deleted that frame.
 */
void daRemotePlayer_c::drawLanternGlow() {
    if (!mLanternGlowPosed || mpLanternGlowModel == NULL) {
        return;
    }

    J3DMaterial* mat = mpLanternGlowModel->getModelData()->getMaterialNodePointer(0);
    const daAlinkHIO_kandelaar_c1& hio = daAlinkHIO_kandelaar_c0::m;
    J3DGXColorS10 color;
    color.r = hio.mColorReg1R;
    color.g = hio.mColorReg1G;
    color.b = hio.mColorReg1B;
    color.a = 255;
    mat->setTevColor(1, &color);
    color.r = hio.mColorReg2R;
    color.g = hio.mColorReg2G;
    color.b = hio.mColorReg2B;
    mat->setTevColor(2, &color);

    cXyz proj;
    mDoLib_project(&mLanternFlamePos, &proj);
    camera_process_class* camera = dComIfGp_getCamera(0);
    const f32 trimHeight = camera != NULL ? camera->mCamera.TrimHeight() : 0.0f;
    if (proj.x > 0.0f && proj.x < FB_WIDTH && proj.y > trimHeight &&
        proj.y < FB_HEIGHT - trimHeight)
    {
        dComIfGd_peekZ(static_cast<s16>(proj.x), static_cast<s16>(proj.y), &mLanternGlowPeek);
    } else {
        mLanternGlowPeek = 0;
    }

    view_class* view = dComIfGd_getView();
    if (view != NULL) {
        const f32 nearZ = view->near_;
        const f32 farZ = view->far_;
        mDoLib_pos2camera(&mLanternFlamePos, &proj);
        proj.z += 30.0f;
        if (proj.z > -0.01f) {
            proj.z = -0.01f;
        }
        mLanternGlowZ = ((nearZ + (farZ * nearZ) / proj.z) / (farZ - nearZ) + 1.0f) * 1.6777215E7f;
    }

    drawModel(mpLanternGlowModel);

    if (!mLoggedLanternGlow && mLanternGlowScale >= 0.5f) {
        mLoggedLanternGlow = true;
        Log.debug("Puppet {} drawing lantern glow: scale {:.2f}, flame ({:.1f}, {:.1f}, {:.1f}), "
                  "fire emitter {}",
            mPlayerId, mLanternGlowScale, mLanternFlamePos.x, mLanternFlamePos.y,
            mLanternFlamePos.z, getPuppetEmitter(PUPPET_FX_LANTERN_FIRE) != NULL ? "live" : "NONE");
    }
}

/* --- puppet particle emitters (see the header for the contract) ------------------------------ */

JPABaseEmitter* daRemotePlayer_c::setPuppetEmitter(
    int i_slot, u16 i_name, const cXyz* i_pos, const csXyz* i_rot, const cXyz* i_scale) {
    if (i_slot < 0 || i_slot >= PUPPET_FX_NUM || i_pos == NULL) {
        return NULL;
    }

    const u32 before = mFxEmitter[i_slot];
    // The level-emitter overload daAlink_c uses (d_a_alink.cpp:14887): no callback, full alpha.
    const u32 handle = dComIfGp_particle_set(
        before, i_name, i_pos, &tevStr, i_rot, i_scale, 0xFF, NULL, -1, NULL, NULL, NULL);
    mFxEmitter[i_slot] = handle;

    JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(handle);
    if (emitter == NULL) {
        if (!mLoggedFxFail[i_slot]) {
            mLoggedFxFail[i_slot] = true;
            Log.warn(
                "Puppet {} could not create particle 0x{:04x} in fx slot {} (the level-emitter "
                "table may be full); it will not be shown (logged once per slot)",
                mPlayerId, static_cast<unsigned>(i_name), i_slot);
        }
        return NULL;
    }

    // Undo an earlier stop on a handle the manager has not cut yet, as his draw() does for his
    // lantern flame every frame (d_a_alink.cpp:19496-19505).
    emitter->playDrawParticle();

    if (handle != before && mFxLogCount < l_fxLogMax) {
        mFxLogCount++;
        Log.debug("Puppet {} fx slot {}: particle 0x{:04x} started as emitter 0x{:x} at ({:.1f}, "
                  "{:.1f}, {:.1f})",
            mPlayerId, i_slot, static_cast<unsigned>(i_name), handle, i_pos->x, i_pos->y, i_pos->z);
    }
    return emitter;
}

JPABaseEmitter* daRemotePlayer_c::setPuppetEmitterMtx(int i_slot, u16 i_name, CMtxP i_mtx) {
    if (i_mtx == NULL) {
        return NULL;
    }
    cXyz pos;
    mDoMtx_multVecZero(i_mtx, &pos);
    JPABaseEmitter* emitter = setPuppetEmitter(i_slot, i_name, &pos);
    if (emitter != NULL) {
        emitter->setGlobalRTMatrix(i_mtx);
    }
    return emitter;
}

/**
 * Hide the slot's emitter now and forget its handle. Not refreshed again, the particle manager cuts
 * it on its next pass (dPa_control_c::level_c::execute, d_particle.cpp:971-984) — the same way
 * daAlink_c ends his flame (stopDrawParticle and no further set, d_a_alink.cpp:14889-14909). While
 * an event is running it is parked instead (calc stopped) and cut when the event ends; it stays
 * hidden throughout, and holds no pointer into this actor.
 */
void daRemotePlayer_c::stopPuppetEmitter(int i_slot) {
    if (i_slot < 0 || i_slot >= PUPPET_FX_NUM || mFxEmitter[i_slot] == 0) {
        return;
    }
    JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(mFxEmitter[i_slot]);
    if (emitter != NULL) {
        emitter->stopDrawParticle();
    }
    if (mFxLogCount < l_fxLogMax) {
        mFxLogCount++;
        Log.debug("Puppet {} fx slot {}: emitter 0x{:x} stopped{}", mPlayerId, i_slot,
            mFxEmitter[i_slot], emitter != NULL ? "" : " (already gone)");
    }
    mFxEmitter[i_slot] = 0;
}

JPABaseEmitter* daRemotePlayer_c::getPuppetEmitter(int i_slot) const {
    if (i_slot < 0 || i_slot >= PUPPET_FX_NUM || mFxEmitter[i_slot] == 0) {
        return NULL;
    }
    return dComIfGp_particle_getEmitter(mFxEmitter[i_slot]);
}

/// From the destructor. With no particle manager (the play scene is already gone) there is nothing
/// left to stop and nothing to ask, so the handles are just dropped.
void daRemotePlayer_c::deletePuppetEmitters() {
    if (g_dComIfG_gameInfo.play.getParticle() == NULL) {
        for (int i = 0; i < PUPPET_FX_NUM; i++) {
            mFxEmitter[i] = 0;
        }
        return;
    }
    for (int i = 0; i < PUPPET_FX_NUM; i++) {
        stopPuppetEmitter(i);
    }
}
