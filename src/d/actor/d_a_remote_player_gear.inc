/**
 * d_a_remote_player_gear.inc
 * Remote player puppet: worn gear that changes how the outfit or the shield LOOKS
 * (PlayerState::gear bits 3-6, see PlayerGearFlags in player_state.hpp):
 *
 *   - the magic armour's BRK glow and its drained look (bits 3-4),
 *   - the Zora helmet's diving mask (bits 3, 5),
 *   - the wooden shield burning: its burnt-wood tint, its two fire emitters and the burn-out puff
 *     (bit 6).
 *
 * Dusk-only, like the rest of the puppet. Included once, last, at the end of
 * d_a_remote_player.cpp: it uses own_archive_res and l_outfits from that file, and the emitter API
 * and damage tint from d_a_remote_player_fx.inc.
 *
 * Same rule as the other puppet files: daAlink_c's own code, transcribed and cited. The sender
 * sends the value his draw code reads (field_0x2fd7, checkZoraWearMaskDraw(), field_0x2fcb != 0),
 * never an input to re-derive it from.
 *
 * Cost: no heap at all. The BRKs are resources already loaded with this puppet's own outfit mount,
 * the Zora shapes are pointers into its own body data, and the diff flag the armour body is built
 * with (0x1000000, d_a_alink_wolf.inc:344-346) is already part of the 0x11000084 that init_model
 * ORs into every model, as daAlink_c::initModel does (d_a_alink.cpp:4144). So passing it as he
 * does would change nothing, and the body stays built as it was.
 */

namespace {

/// His field_0x2fcb start value: both places that light the shield write 120
/// (d_a_alink_guard.inc:108, d_a_alink_damage.inc:667), and his tint is scaled by the same 120
/// (d_a_alink.cpp:19749).
const int l_shieldBurnTime = 120;

/// setMagicArmorBrk's tables (d_a_alink.cpp:12721-12731), statuses 0 and 1. Status 2
/// (power_up_b) has no caller anywhere in the tree, so it is not loaded.
const char* const l_armorBodyBrkName[2] = {"ml_body_power_down.brk", "ml_body_power_up_a.brk"};
const char* const l_armorHeadBrkName[2] = {"ml_head_power_down.brk", "ml_head_power_up_a.brk"};

/// J3DAnmTevRegKey::getKind() (J3DAnimation.h), the type check for a BRK out of the archive.
const s32 l_brkKind = 5;

/// The Zora outfit's field_0x06e4 and field_0x06f0 (d_a_alink_wolf.inc:446-450).
const u16 l_zoraMaskMatNo = 9;
const u16 l_zoraHoodMatNo = 11;

const u16 l_gearFxLogMax = 48;

const char* armor_brk_name(s8 i_status) {
    switch (i_status) {
    case 0:
        return "power_down";
    case 1:
        return "power_up_a";
    default:
        return "none";
    }
}

/// Fetch one BRK out of the puppet's mount and point it at the model it animates, as
/// setMagicArmorBrk does (searchUpdateMaterialID before entry).
J3DAnmTevRegKey* gear_load_brk(dRes_info_c& i_res, const char* i_name, J3DModelData* i_data) {
    if (i_data == NULL) {
        return NULL;
    }
    J3DAnmBase* anm = static_cast<J3DAnmBase*>(own_archive_res(i_res, i_name));
    if (anm == NULL || anm->getKind() != l_brkKind) {
        return NULL;
    }
    J3DAnmTevRegKey* brk = static_cast<J3DAnmTevRegKey*>(anm);
    brk->searchUpdateMaterialID(i_data);
    return brk;
}

/// The burning shield's second emitter. The same level-emitter call setPuppetEmitter makes, on a
/// handle of its own because the fire is two emitters and the API gave this feature one slot.
void gear_set_emitter_mtx(u32& io_handle, u16 i_name, CMtxP i_mtx, dKy_tevstr_c* i_tevStr) {
    cXyz pos;
    mDoMtx_multVecZero(i_mtx, &pos);
    io_handle = dComIfGp_particle_set(
        io_handle, i_name, &pos, i_tevStr, NULL, NULL, 0xFF, NULL, -1, NULL, NULL, NULL);
    JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(io_handle);
    if (emitter != NULL) {
        emitter->playDrawParticle();
        emitter->setGlobalRTMatrix(i_mtx);
    }
}

void gear_stop_emitter(u32& io_handle) {
    if (io_handle == 0) {
        return;
    }
    JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(io_handle);
    if (emitter != NULL) {
        emitter->stopDrawParticle();
    }
    io_handle = 0;
}

}  // namespace

/* --- the network side ------------------------------------------------------------------------ */

void daRemotePlayer_c::setNetworkGearFx(
    bool i_reported, bool i_armorPowered, bool i_zoraMask, bool i_shieldBurn) {
    if (i_reported != mNetGearReported && mGearFxLogCount < l_gearFxLogMax) {
        mGearFxLogCount++;
        Log.info("Puppet {} gear fx: sender {} bits 3-6", mPlayerId,
            i_reported ? "now reports" : "no longer reports");
    }
    mNetGearReported = i_reported;
    mNetArmorPowered = i_armorPowered;
    mNetZoraMask = i_zoraMask;
    mNetShieldBurn = i_shieldBurn;
}

/* --- setup and teardown ---------------------------------------------------------------------- */

/**
 * From createHeap(), after setupHeavyBootsModel(). Finds what this outfit needs and changes
 * nothing: until bit 3 arrives the puppet must look as it did before these bits existed, so the BRK
 * is not entered and no shape is touched here.
 *
 * Non-fatal on every path, and all-or-nothing per feature: a missing body BRK costs the glow, a
 * missing shape costs the mask, and neither ever costs the puppet.
 */
void daRemotePlayer_c::setupGearFx() {
    mNetGearReported = false;
    mNetArmorPowered = false;
    mNetZoraMask = false;
    mNetShieldBurn = false;
    for (int i = 0; i < 2; i++) {
        mpArmorBodyBrk[i] = NULL;
        mpArmorHeadBrk[i] = NULL;
    }
    mArmorBrkStatus = -1;
    mpArmorBodyBrkIn = NULL;
    mpArmorHeadBrkIn = NULL;
    mpZoraMaskShape = NULL;
    mpZoraHoodShape = NULL;
    mZoraShapeState = ZORA_SHAPES_NONE;
    mShieldBurnTimer = 0;
    mShieldBurnEmitterB = 0;
    mpShieldBurnModel = NULL;
    mGearFxLogCount = 0;

    if (model == NULL || mOutfit < 0 || mOutfit >= l_outfitNum) {
        return;
    }
    const char* arcName = l_outfits[mOutfit].arcName;
    J3DModelData* bodyData = model->getModelData();

    if (std::strcmp(arcName, "Mmdl") == 0) {
        J3DModelData* headData = mpHeadModel != NULL ? mpHeadModel->getModelData() : NULL;
        for (int i = 0; i < 2; i++) {
            mpArmorBodyBrk[i] = gear_load_brk(mOwnRes, l_armorBodyBrkName[i], bodyData);
            mpArmorHeadBrk[i] = gear_load_brk(mOwnRes, l_armorHeadBrkName[i], headData);
        }
        const bool bodyOk = mpArmorBodyBrk[0] != NULL && mpArmorBodyBrk[1] != NULL;
        const bool headOk = mpArmorHeadBrk[0] != NULL && mpArmorHeadBrk[1] != NULL;
        if (!bodyOk) {
            for (int i = 0; i < 2; i++) {
                mpArmorBodyBrk[i] = NULL;
                mpArmorHeadBrk[i] = NULL;
            }
        } else if (!headOk) {
            mpArmorHeadBrk[0] = NULL;
            mpArmorHeadBrk[1] = NULL;
        }
        Log.info("Puppet {} gear fx setup: magic armour BRKs body {} head {}", mPlayerId,
            bodyOk ? "ok" : "MISSING (no glow)", headOk ? "ok" : "MISSING");
    } else if (std::strcmp(arcName, "Zmdl") == 0) {
        if (bodyData->getMaterialNum() > l_zoraHoodMatNo) {
            mpZoraMaskShape = bodyData->getMaterialNodePointer(l_zoraMaskMatNo)->getShape();
            mpZoraHoodShape = bodyData->getMaterialNodePointer(l_zoraHoodMatNo)->getShape();
        }
        // The material NAMES, so the two indices can be checked against the mesh from a log, as
        // the boots' are: a wrong index hides a visible piece of the body and says nothing.
        JUTNameTab* names = bodyData->getMaterialName();
        const char* maskName =
            mpZoraMaskShape != NULL && names != NULL ? names->getName(l_zoraMaskMatNo) : NULL;
        const char* hoodName =
            mpZoraHoodShape != NULL && names != NULL ? names->getName(l_zoraHoodMatNo) : NULL;
        Log.info("Puppet {} gear fx setup: Zora mask mat {} ('{}'), mat {} ('{}'){}", mPlayerId,
            l_zoraMaskMatNo, maskName != NULL ? maskName : "-", l_zoraHoodMatNo,
            hoodName != NULL ? hoodName : "-",
            mpZoraMaskShape != NULL ? "" : " | OUT OF RANGE, mask not driven");
    }
}

/// From the destructor, after deletePuppetEmitters() (which takes BURN_A's slot with it).
void daRemotePlayer_c::deleteGearFx() {
    if (g_dComIfG_gameInfo.play.getParticle() == NULL) {
        mShieldBurnEmitterB = 0;
        return;
    }
    gear_stop_emitter(mShieldBurnEmitterB);
}

/* --- per tick -------------------------------------------------------------------------------- */

/// From execute(), after setMatrix(): the shield's matrix is this tick's by then.
void daRemotePlayer_c::updateGearFx() {
    updateMagicArmorBrk();
    updateZoraMask();
    updateShieldBurn();
}

/**
 * The magic armour's BRK frame and which pair is playing.
 *
 *   - Bit 4 changes while reported: his setMagicArmorBrk on a rupee change (d_a_alink.cpp:
 *     18753-18760). The new pair starts at frame 0, so the puppet plays the same power-down or
 *     power-up as he does.
 *   - First reported tick: his setLinkModel, which puts the pair at its LAST frame
 *     (d_a_alink_wolf.inc:361-362). A puppet that spawns, or a sender who just put the armour on,
 *     is already in that state and must not play the transition.
 *   - Every tick: simpleAnmPlay on both, as setSelectEquipItem does (d_a_alink.cpp:4452-4453).
 *
 * The frames live on this puppet's own anm objects, out of its own mount, so nothing else steps
 * them (the double-frame-controller trap does not apply).
 */
void daRemotePlayer_c::updateMagicArmorBrk() {
    if (mpArmorBodyBrk[0] == NULL) {
        return;
    }
    const s8 want = mNetGearReported ? (mNetArmorPowered ? 1 : 0) : -1;
    if (want != mArmorBrkStatus) {
        const bool fresh = mArmorBrkStatus < 0;
        if (want >= 0) {
            J3DAnmTevRegKey* body = mpArmorBodyBrk[want];
            J3DAnmTevRegKey* head = mpArmorHeadBrk[want];
            body->setFrame(fresh ? body->getFrameMax() : 0.0f);
            if (head != NULL) {
                head->setFrame(fresh ? head->getFrameMax() : 0.0f);
            }
        }
        if (mGearFxLogCount < l_gearFxLogMax) {
            mGearFxLogCount++;
            Log.info("Puppet {} magic armour BRK: {} -> {}{}", mPlayerId,
                armor_brk_name(mArmorBrkStatus), armor_brk_name(want),
                want < 0 ? "" : (fresh ? " (at its last frame)" : " (from frame 0)"));
        }
        mArmorBrkStatus = want;
    }
    if (mArmorBrkStatus >= 0) {
        daAlink_c::simpleAnmPlay(mpArmorBodyBrk[mArmorBrkStatus]);
        daAlink_c::simpleAnmPlay(mpArmorHeadBrk[mArmorBrkStatus]);
    }
}

/**
 * Enter or remove the BRK, from draw() right after the damage tint, before the body: his draw()
 * does it at the same point (d_a_alink.cpp:19587-19604). He removes it while his tint ladder
 * overrides the colour (var_r31 TRUE) and enters it otherwise. Of the tints the wire carries,
 * freeze and ice leave var_r31 TRUE and the damage pulse clears it, so the BRK is removed for those
 * two only.
 *
 * Not reproduced: a hit the armour absorbs also leaves var_r31 TRUE (the `!checkMagicArmorNoDamage`
 * branch is skipped), so he drops the glow for his damage timer. The sender sends no tint for that
 * hit (d_a_remote_player_fx.inc), so the puppet keeps glowing.
 *
 * Entered every draw, as his is: it only writes the material anms' pointers. On the puppet's own
 * private body and head data, so no one else's materials are touched.
 */
void daRemotePlayer_c::applyMagicArmorBrk() {
    J3DAnmTevRegKey* body = NULL;
    J3DAnmTevRegKey* head = NULL;
    if (mArmorBrkStatus >= 0 && mNetDmgTint != dusk::mp::kPlayerDmgTintFreeze &&
        mNetDmgTint != dusk::mp::kPlayerDmgTintIce)
    {
        body = mpArmorBodyBrk[mArmorBrkStatus];
        head = mpArmorHeadBrk[mArmorBrkStatus];
    }

    if (model != NULL) {
        J3DModelData* bodyData = model->getModelData();
        if (mpArmorBodyBrkIn != NULL && mpArmorBodyBrkIn != body) {
            bodyData->removeTevRegAnimator(mpArmorBodyBrkIn);
        }
        if (body != NULL) {
            bodyData->entryTevRegAnimator(body);
        }
        mpArmorBodyBrkIn = body;
    }
    if (mpHeadModel != NULL) {
        J3DModelData* headData = mpHeadModel->getModelData();
        if (mpArmorHeadBrkIn != NULL && mpArmorHeadBrkIn != head) {
            headData->removeTevRegAnimator(mpArmorHeadBrkIn);
        }
        if (head != NULL) {
            headData->entryTevRegAnimator(head);
        }
        mpArmorHeadBrkIn = head;
    }
}

/**
 * The Zora outfit's mask (material 9) and material 11, setSelectEquipItem's block
 * (d_a_alink.cpp:4417-4449) on the sender's answer to checkZoraWearMaskDraw():
 *
 *   - drawn: material 11 shown, the mask shown unless the iron boots are on. setHeavyBoots hides
 *     the mask as they go on and shows it as they come off (d_a_alink_hvyboots.inc:47-84), and
 *     setSelectEquipItem only ever shows it without them, so "boots on" is the whole test. The
 *     boots are kPlayerGearHeavyBoots, his own checkEquipHeavyBoots().
 *   - not drawn: both hidden. That is his look on dry land, since FLG0_SWIM_UP is set everywhere
 *     but under water.
 *   - not reported (an older sender): the shapes as createHeap left them, the mask hidden and
 *     material 11 shown, which is what the puppet always drew before.
 *
 * His draw() also hides material 11 for the first-person camera (d_a_alink.cpp:19648-19707). That
 * is his own camera, which never looks at a puppet, so it is not reproduced.
 */
void daRemotePlayer_c::updateZoraMask() {
    if (mpZoraMaskShape == NULL || mpZoraHoodShape == NULL) {
        return;
    }
    const u8 want = !mNetGearReported ? ZORA_SHAPES_AS_BUILT :
                                        (mNetZoraMask ? ZORA_SHAPES_DIVING : ZORA_SHAPES_DRY);
    const bool maskShown = want == ZORA_SHAPES_DIVING && !mNetHeavyBoots;
    const bool hoodShown = want != ZORA_SHAPES_DRY;
    // J3DShpFlag_Visible SET means hidden: hide() turns it on (J3DShape.h:173-174).
    const bool maskWasShown = !mpZoraMaskShape->checkFlag(J3DShpFlag_Visible);

    if (maskShown) {
        mpZoraMaskShape->show();
    } else {
        mpZoraMaskShape->hide();
    }
    if (hoodShown) {
        mpZoraHoodShape->show();
    } else {
        mpZoraHoodShape->hide();
    }

    if ((want != mZoraShapeState || maskShown != maskWasShown) && mGearFxLogCount < l_gearFxLogMax)
    {
        mGearFxLogCount++;
        Log.info("Puppet {} Zora mask: {} (mask mat {} {}, mat {} {})", mPlayerId,
            want == ZORA_SHAPES_DIVING ? "DIVING" : (want == ZORA_SHAPES_DRY ? "dry" : "as built"),
            l_zoraMaskMatNo, maskShown ? "shown" : "hidden", l_zoraHoodMatNo,
            hoodShown ? "shown" : "hidden");
    }
    mZoraShapeState = want;
}

/**
 * The wooden shield's burn, from bit 6.
 *
 * ★ Why a bit and the receiver's own copy of the timer, rather than the timer itself: his
 * field_0x2fcb is set to 120 on a fire hit and then, every tick, either counts down by one or is
 * cleared outright (d_a_alink.cpp:18566-18587); it is never held. So the rising edge says the
 * timer is 120, and counting ticks from there IS his timer, up to the snapshot latency (which only
 * delays the whole curve) and an off-by-one on the hit tick (the tint moves 32/120 of a unit a
 * tick). Sending the value would cost 7 bits for nothing the edge does not already say. The copy
 * stops at 1, because his only reaches 0 by burning out or being put out, and either way bit 6
 * falls with it.
 *
 * When it falls, it is a burn-out if the same packet says he has no shield: at 0 he calls
 * dMeter2Info_setShield(NONE), which empties checkShieldGet() on that tick, so fill_equip sends
 * kind 0 (the "no shield" fold) with the draw bit clear. Then the puppet plays
 * setWoodShieldBurnOutEffect's two one-shots (d_a_alink_effect.inc:1926-1944) on the shield it was
 * drawing. Put out in water, or a shield swap, leaves a shield on the wire, and there is no puff.
 *
 * The fire: setWoodShieldBurnEffect (d_a_alink_effect.inc:1898-1914), BURN_A and BURN_B refreshed
 * every tick on the shield's base matrix while the timer runs. BURN_A is PUPPET_FX_SHIELD_FIRE,
 * BURN_B has its own handle. Not reproduced: his Z2SE_AL_BURNING loop (:1847-1849); the puppet's
 * sound is a separate plan (.claude/future-plans/puppet-sound.md).
 */
void daRemotePlayer_c::updateShieldBurn() {
    const u8 shieldKind = static_cast<u8>((mNetEquip & dusk::mp::kPlayerEquipShieldKindMask) >>
                                          dusk::mp::kPlayerEquipShieldKindShift);
    if (mNetShieldBurn) {
        if (mShieldBurnTimer == 0) {
            mShieldBurnTimer = l_shieldBurnTime;
            if (mGearFxLogCount < l_gearFxLogMax) {
                mGearFxLogCount++;
                Log.info("Puppet {} shield burn: started (timer {}, shield kind {} {})", mPlayerId,
                    l_shieldBurnTime, shieldKind, currentShield() != NULL ? "drawn" : "NOT drawn");
            }
        } else if (mShieldBurnTimer > 1) {
            mShieldBurnTimer--;
        }
    } else if (mShieldBurnTimer != 0) {
        const bool burntOut = (mNetEquip & dusk::mp::kPlayerEquipShieldDraw) == 0 &&
                              shieldKind == dusk::mp::kPlayerEquipShieldCarvingWood;
        if (burntOut && mpShieldBurnModel != NULL) {
            static const u16 burnOutName[2] = {ID_ZI_J_LK_SH_BURN_C, ID_ZI_J_LK_SH_BURN_D};
            for (int i = 0; i < 2; i++) {
                JPABaseEmitter* emitter = dComIfGp_particle_setColor(
                    burnOutName[i], &current.pos, &tevStr, NULL, NULL, 0.0f, 0xFF);
                if (emitter != NULL) {
                    emitter->setGlobalRTMatrix(mpShieldBurnModel->getBaseTRMtx());
                }
            }
        }
        if (mGearFxLogCount < l_gearFxLogMax) {
            mGearFxLogCount++;
            Log.info("Puppet {} shield burn: ended at timer {}, {}", mPlayerId, mShieldBurnTimer,
                burntOut ? "BURNT OUT (shield gone)" : "put out (shield kept)");
        }
        mShieldBurnTimer = 0;
    }

    J3DModel* shield = currentShield();
    if (mShieldBurnTimer != 0 && shield != NULL) {
        setPuppetEmitterMtx(PUPPET_FX_SHIELD_FIRE, ID_ZI_J_LK_SH_BURN_A, shield->getBaseTRMtx());
        gear_set_emitter_mtx(
            mShieldBurnEmitterB, ID_ZI_J_LK_SH_BURN_B, shield->getBaseTRMtx(), &tevStr);
        mpShieldBurnModel = shield;
    } else {
        stopPuppetEmitter(PUPPET_FX_SHIELD_FIRE);
        gear_stop_emitter(mShieldBurnEmitterB);
    }
}

/* --- draw ------------------------------------------------------------------------------------ */

/**
 * His burnt-wood tint (d_a_alink.cpp:19747-19759): while checkWoodShieldEquip() and the timer
 * runs, TevColor darkens from 0 to -32 over the 120 ticks. Kinds 0 and 1 are his two wooden shields
 * (checkWoodShieldEquip, d_a_player.cpp:481-484); kind 0 also means "none", but this is only
 * reached with the shield being drawn. His PROC_METAMORPHOSE exception is moot: a puppet does not
 * transform.
 */
bool daRemotePlayer_c::beginShieldBurnTint() {
    const u8 shieldKind = static_cast<u8>((mNetEquip & dusk::mp::kPlayerEquipShieldKindMask) >>
                                          dusk::mp::kPlayerEquipShieldKindShift);
    if (mShieldBurnTimer == 0 || (shieldKind != dusk::mp::kPlayerEquipShieldCarvingWood &&
                                     shieldKind != dusk::mp::kPlayerEquipShieldShopWood))
    {
        return false;
    }
    tevStr.TevColor.r =
        static_cast<s16>(-((l_shieldBurnTime - mShieldBurnTimer) * 32) / l_shieldBurnTime);
    tevStr.TevColor.g = tevStr.TevColor.r;
    tevStr.TevColor.b = tevStr.TevColor.r;
    return true;
}

/// His reset right after the shield (d_a_alink.cpp:19756-19760): zero, not the colour from before.
void daRemotePlayer_c::endShieldBurnTint(bool i_tinted) {
    if (!i_tinted) {
        return;
    }
    tevStr.TevColor.r = 0;
    tevStr.TevColor.g = 0;
    tevStr.TevColor.b = 0;
}
