/**
 * @file d_a_e_oc_mp.inc
 * Dusklight multiplayer, M6 enemy sync: the Bokoblin (E_OC) net adapter.
 *
 * Included at the bottom of d_a_e_oc.cpp under #if TARGET_PC, so it lives in the same translation
 * unit and can use the file-statics (l_HIO, E_OC_ACTIONS) and every daE_OC_c member. Nothing in
 * here changes what the original game does: these functions only run when dusk::mp's ghost tick
 * or capture path calls them through the adapter table (dusk/mp_enemy.hpp).
 *
 * Design: .claude/future-plans/mp-m6-enemies.md, sections 2.2 (ghost mode), 4 (replicated state),
 * 5.6 (nearest-player aggro).
 *
 * EnemyExtra byte layout (kE_OC_ExtraLen = 18 bytes, s16 values little-endian):
 *
 *   [0]      mActionMode            (E_OC_ACTIONS, 0..16)
 *   [1]      mOcState               (the action's sub-state, 0..8)
 *   [2]      visibility/collider bits
 *              0x01 mHide           (draw() returns early: hidden until the Forest Temple demo)
 *              0x02 field_0x6e8     (death fade on: draw() darkens the materials by field_0x6e4)
 *              0x04 mSphs_cc[0] TG set   0x08 mSphs_cc[0] CO set
 *              0x10 mSphs_cc[1] TG set   0x20 mSphs_cc[1] CO set
 *              0x40 mSphs_at[0] AT set   0x80 mSphs_at[1] AT set
 *   [3]      state bits
 *              0x01 fopEn_flag_Down      (set/cleared by E_OC itself; Link's ending blow reads it)
 *              0x02 fopEn_flag_HeadLock  (set/cleared by E_OC itself; Link's head jump reads it)
 *              0x04 attention_info.flags == 0 (damage_check drops lock-on when health <= 1)
 *   [4]      s8  field_0x6e4, truncated toward zero (death fade, 0 -> -20). draw() assigns the
 *                float to an s16 tev colour, which truncates the same way, so this is exact.
 *   [5]      u8  field_0x6c0, clamped to 0..255 (the action timer: other Bokoblins read it through
 *                checkBeforeDeath(), and a handoff mid-death needs it to finish on time)
 *   [6..7]   s16 field_0x6d2    head yaw, applied in ctrlJoint (joint 0x11) during modelCalc
 *   [8..9]   s16 field_0x6d0    club-arm yaw, applied in ctrlJoint (joint 0xc) during modelCalc
 *   [10..11] s16 field_0x688.x  ground tilt, mtx_set
 *   [12..13] s16 field_0x688.z  ground tilt, mtx_set
 *   [14..15] s16 field_0x6d8    extra body yaw (the big-damage spin), mtx_set
 *   [16..17] s16 shape_angle.x  pitch (big-damage flip, water death); the generic block only
 *                               carries shape_angle.y, so the adapter sends x itself
 *
 * Bytes 18..23 are free.
 */

#include "dusk/mp_enemy.hpp"

/* --- batch 10A: ghost AT --- */
/// Phase 3, spec 5.3 (victim-decided): the ghost's AT spheres follow the authority's set bits
/// (byte [2], 0x40/0x80). present() -> cc_set() positions and registers them off the ghost's own
/// joints, so in this machine's collision pass they hit THIS machine's Link, who then takes damage,
/// guards and knockback through his own TG path. at_sph_src is AT_SPRM 0xD (set | vs player |
/// vs other): it can only meet a TG flagged IS_PLAYER or IS_OTHER, and the remote-player puppet
/// registers no TG at all (only its friendly-fire AT capsules), so a ghost never hits a puppet and
/// the authority's real swing never hits one either. Only the local Link can take the blow.
/// Set false to go back to the phase 1 "no combat" ghost.
static const bool kE_OC_GhostAtLive = true;
/* --- end batch 10A --- */

static const unsigned char kE_OC_ExtraLen = 18;

static void daE_OC_mp_put16(unsigned char* p, s16 v) {
    u16 u = (u16)v;
    p[0] = (unsigned char)(u & 0xFF);
    p[1] = (unsigned char)(u >> 8);
}

static s16 daE_OC_mp_get16(const unsigned char* p) {
    return (s16)(u16)(p[0] | (p[1] << 8));
}

/// The action() switch sets field_0x566 (the "searching Link" byte handed to the sound object and
/// read by d_a_npc_ks) from the action alone; derive it the same way for a ghost.
static s8 daE_OC_mp_linkSearch(int action) {
    switch (action) {
    case E_OC_ACTION_FIND:
    case E_OC_ACTION_ATTACK:
    case E_OC_ACTION_DEMO_MASTER:
    case E_OC_ACTION_DEMO_CHILD:
    case E_OC_ACTION_FIND_STAY:
    case E_OC_ACTION_MOVE_OUT:
        return 1;
    default:
        return 0;
    }
}

/// mBattleOn is reset every action() and set by the combat actions. Other (authority) Bokoblins
/// read it through isBattleOn() in s_other_oc, so a ghost keeps it the same way.
static bool daE_OC_mp_battleOn(int action, int state) {
    switch (action) {
    case E_OC_ACTION_FIND:
        return state == 4 || state == 5 || state == 6;
    case E_OC_ACTION_ATTACK:
    case E_OC_ACTION_DAMAGE:
    case E_OC_ACTION_BIG_DAMAGE:
    case E_OC_ACTION_DEATH:
    case E_OC_ACTION_FIND_STAY:
    case E_OC_ACTION_MOVE_OUT:
        return true;
    default:
        return false;
    }
}

static mDoExt_McaMorfSO* daE_OC_mp_morf(fopAc_ac_c* actor) {
    return static_cast<daE_OC_c*>(actor)->mpMorf;
}

static void daE_OC_mp_captureExtra(fopAc_ac_c* actor, dusk::mp::EnemyExtra& out) {
    daE_OC_c* oc = static_cast<daE_OC_c*>(actor);
    unsigned char* d = out.data;

    d[0] = (unsigned char)oc->mActionMode;
    d[1] = (unsigned char)oc->mOcState;

    unsigned char bits = 0;
    if (oc->mHide) {
        bits |= 0x01;
    }
    if (oc->field_0x6e8 != 0) {
        bits |= 0x02;
    }
    if (oc->mSphs_cc[0].ChkTgSet()) {
        bits |= 0x04;
    }
    if (oc->mSphs_cc[0].ChkCoSet()) {
        bits |= 0x08;
    }
    if (oc->mSphs_cc[1].ChkTgSet()) {
        bits |= 0x10;
    }
    if (oc->mSphs_cc[1].ChkCoSet()) {
        bits |= 0x20;
    }
    if (oc->mSphs_at[0].ChkAtSet()) {
        bits |= 0x40;
    }
    if (oc->mSphs_at[1].ChkAtSet()) {
        bits |= 0x80;
    }
    d[2] = bits;

    unsigned char state = 0;
    if (oc->mFlags & fopEn_enemy_c::fopEn_flag_Down) {
        state |= 0x01;
    }
    if (oc->mFlags & fopEn_enemy_c::fopEn_flag_HeadLock) {
        state |= 0x02;
    }
    if (oc->attention_info.flags == 0) {
        state |= 0x04;
    }
    d[3] = state;

    int fade = (int)oc->field_0x6e4;
    if (fade < -128) {
        fade = -128;
    } else if (fade > 127) {
        fade = 127;
    }
    d[4] = (unsigned char)(s8)fade;

    int timer = oc->field_0x6c0;
    if (timer < 0) {
        timer = 0;
    } else if (timer > 255) {
        timer = 255;
    }
    d[5] = (unsigned char)timer;

    daE_OC_mp_put16(d + 6, oc->field_0x6d2);
    daE_OC_mp_put16(d + 8, oc->field_0x6d0);
    daE_OC_mp_put16(d + 10, oc->field_0x688.x);
    daE_OC_mp_put16(d + 12, oc->field_0x688.z);
    daE_OC_mp_put16(d + 14, oc->field_0x6d8);
    daE_OC_mp_put16(d + 16, oc->shape_angle.x);

    out.len = kE_OC_ExtraLen;
}

static void daE_OC_mp_applyExtra(fopAc_ac_c* actor, const dusk::mp::EnemyExtra& in) {
    if (in.len < kE_OC_ExtraLen) {
        return;
    }

    daE_OC_c* oc = static_cast<daE_OC_c*>(actor);
    const unsigned char* d = in.data;

    int action = d[0];
    int state = d[1];

    // One-shot swing voices are started by action() on the tick the swing begins, which a ghost
    // skips. Start them on entry into the swing state instead (the sub-state, not the anm, so a
    // dropped packet that skips ATTACK/0 still gets its voice).
    if (action == E_OC_ACTION_ATTACK && (state == 1 || state == 2) &&
        (oc->mActionMode != action || oc->mOcState != state))
    {
        if (state == 1) {
            oc->mSound.startCreatureVoice(Z2SE_EN_OC_V_ATTACK_B, -1);
        } else {
            oc->mSound.startCreatureVoice(Z2SE_EN_OC_V_ATTACK_C, -1);
        }
    }

    if (oc->mActionMode != action) {
        oc->mOldActionMode = oc->mActionMode;
    }
    oc->mActionMode = action;
    oc->mOcState = state;

    unsigned char bits = d[2];
    oc->mHide = (bits & 0x01) != 0;
    oc->field_0x6e8 = (bits & 0x02) ? 1 : 0;

    if (bits & 0x04) {
        oc->mSphs_cc[0].OnTgSetBit();
    } else {
        oc->mSphs_cc[0].OffTgSetBit();
    }
    if (bits & 0x08) {
        oc->mSphs_cc[0].OnCoSetBit();
    } else {
        oc->mSphs_cc[0].OffCoSetBit();
    }
    if (bits & 0x10) {
        oc->mSphs_cc[1].OnTgSetBit();
    } else {
        oc->mSphs_cc[1].OffTgSetBit();
    }
    if (bits & 0x20) {
        oc->mSphs_cc[1].OnCoSetBit();
    } else {
        oc->mSphs_cc[1].OffCoSetBit();
    }
    if (kE_OC_GhostAtLive && (bits & 0x40)) {
        oc->mSphs_at[0].OnAtSetBit();
    } else {
        oc->mSphs_at[0].OffAtSetBit();
    }
    if (kE_OC_GhostAtLive && (bits & 0x80)) {
        oc->mSphs_at[1].OnAtSetBit();
    } else {
        oc->mSphs_at[1].OffAtSetBit();
    }

    // Only the two mFlags bits the Bokoblin owns. Every other bit (CutDownHit, wolf bite...) is
    // written by the local Link and must survive for the phase 2 EnemyFlagHit diff.
    unsigned char st = d[3];
    if (st & 0x01) {
        oc->onDownFlg();
    } else {
        oc->mFlags &= (u16)~fopEn_enemy_c::fopEn_flag_Down;
    }
    if (st & 0x02) {
        oc->onHeadLockFlg();
    } else {
        oc->offHeadLockFlg();
    }
    // The original only ever sets BATTLE at create and zeroes it in damage_check.
    oc->attention_info.flags = (st & 0x04) ? 0u : (u32)fopAc_AttnFlag_BATTLE_e;

    oc->field_0x6e4 = (f32)(s8)d[4];
    oc->field_0x6c0 = d[5];
    oc->field_0x6d2 = daE_OC_mp_get16(d + 6);
    oc->field_0x6d0 = daE_OC_mp_get16(d + 8);
    oc->field_0x688.x = daE_OC_mp_get16(d + 10);
    oc->field_0x688.z = daE_OC_mp_get16(d + 12);
    oc->field_0x6d8 = daE_OC_mp_get16(d + 14);
    oc->shape_angle.x = daE_OC_mp_get16(d + 16);

    // Derived the way action() derives them.
    oc->mBattleOn = daE_OC_mp_battleOn(action, state);
    oc->mIsHomeWait = false;
    oc->field_0x566 = daE_OC_mp_linkSearch(action);
    oc->mSound.setLinkSearch(oc->field_0x566);

    // A ghost never pairs up for a chat: s_other_oc lets an authority Bokoblin call
    // setActionMode(TALK) on whoever names it as mpTalk, and a ghost's mpTalk is left over from
    // the one execute create() ran.
    oc->mpTalk = NULL;
}

/// Sound and particle lines from action() that depend only on the action, the anm and its frame:
/// no player, no cM_rnd, no state change. Cosmetic; drop this call if it ever misbehaves.
static void daE_OC_mp_ghostCosmetics(daE_OC_c* oc) {
    oc->setFootNoteSound();

    if (oc->mActionMode != E_OC_ACTION_ATTACK) {
        return;
    }

    if (oc->mOcState == 2) {
        MtxP mtx = oc->mpMorf->getModel()->getAnmMtx(0);
        cXyz pos(mtx[0][3], mtx[1][3], mtx[2][3]);
        oc->mParticleKeys[0] =
            dComIfGp_particle_set(oc->mParticleKeys[0], dPa_RM(ID_ZM_S_ORCATTACKBLUR00), &pos,
                &oc->tevStr, &oc->shape_angle, NULL, -1, NULL, -1, NULL, NULL, NULL);
        oc->mParticleKeys[1] =
            dComIfGp_particle_set(oc->mParticleKeys[1], dPa_RM(ID_ZM_S_ORCATTACKBLUR01), &pos,
                &oc->tevStr, &oc->shape_angle, NULL, -1, NULL, -1, NULL, NULL, NULL);
    }

    mDoExt_McaMorfSO* morf = oc->mpMorf;
    if (oc->mOcState == 1) {
        if (morf->checkFrame(10.0f)) {
            oc->mSound.startCreatureVoice(Z2SE_EN_OC_V_ATTACK_B2, -1);
        } else if (morf->checkFrame(12.5f)) {
            oc->mSound.startCreatureSound(Z2SE_EN_OC_ATTACK_B, 0, -1);
        } else if (morf->checkFrame(19.0f)) {
            if (strcmp(oc->mName, "E_OC") == 0) {
                oc->mSound.startCreatureSound(Z2SE_EN_OC_CLUB_HIT, 0, -1);
            } else {
                oc->mSound.startCreatureSound(Z2SE_EN_OC_NATA_HIT, 0, -1);
            }
        }
    } else if (oc->mOcState == 2) {
        if (morf->checkFrame(7.0f)) {
            oc->mSound.startCreatureVoice(Z2SE_EN_OC_V_ATTACK_C2, -1);
        } else if (morf->checkFrame(11.5f)) {
            oc->mSound.startCreatureSound(Z2SE_EN_OC_ATTACK_C, 0, -1);
        }
    }
}

/// The parts of execute() that make a correct pose, lock-on, push and hit volumes: no AI, no
/// damage_check, nothing that reads the player or cM_rnd.
static void daE_OC_mp_present(fopAc_ac_c* actor) {
    daE_OC_c* oc = static_cast<daE_OC_c*>(actor);

    // Pose: base TR (ground tilt, body yaw, scale) + modelCalc (head/arm yaw via ctrlJoint), and
    // the club on joint 0xc. Then eyePos, attention position and the four spheres.
    oc->mtx_set();
    oc->cc_set();

    // Lock-on anchors that action() refreshes: the head-jump target and, while down, the
    // ending-blow position.
    cXyz head_lock_pos(oc->eyePos);
    head_lock_pos.y += 300.0f + NREG_F(7);
    oc->setHeadLockPos(&head_lock_pos);
    if (oc->checkDownFlg()) {
        oc->setStabPos();
    }

    // draw() puts the shadow at mAcch's ground height and polygon, which only mAcch.CrrPos
    // (inside action) refreshes. Re-probe the ground under the ghost without moving it.
    cXyz probe(oc->current.pos.x, oc->current.pos.y + 50.0f, oc->current.pos.z);
    oc->mAcch.m_gnd.SetPos(&probe);
    oc->mAcch.m_ground_h = dComIfG_Bgsp().GroundCross(&oc->mAcch.m_gnd);

    daE_OC_mp_ghostCosmetics(oc);
}

/// Mid-swing, reeling from a hit, or in the scripted encounter: don't switch targets now.
/// The demo matters most: executeDemoMaster is the one place E_OC WRITES into slot 0
/// (setPlayerPosAndAngle, changeDemoMode), so it must keep the target it started with, which is
/// the local Link as long as the selector starts every enemy there.
static bool daE_OC_mp_engaged(fopAc_ac_c* actor) {
    switch (static_cast<daE_OC_c*>(actor)->mActionMode) {
    case E_OC_ACTION_ATTACK:
    case E_OC_ACTION_DAMAGE:
    case E_OC_ACTION_BIG_DAMAGE:
    case E_OC_ACTION_DEMO_MASTER:
    case E_OC_ACTION_DEMO_CHILD:
        return true;
    default:
        return false;
    }
}

/// Phase 2 helpers: which of the two TG spheres a hit landed on, and back. -1 = not ours.
static int daE_OC_mp_tgIndexOf(fopAc_ac_c* actor, cCcD_Obj* tg) {
    daE_OC_c* oc = static_cast<daE_OC_c*>(actor);
    for (int i = 0; i < 2; i++) {
        if (tg == static_cast<cCcD_Obj*>(&oc->mSphs_cc[i])) {
            return i;
        }
    }
    return -1;
}

static cCcD_Obj* daE_OC_mp_tgByIndex(fopAc_ac_c* actor, int index) {
    if (index < 0 || index >= 2) {
        return NULL;
    }
    return &static_cast<daE_OC_c*>(actor)->mSphs_cc[index];
}

/**
 * This machine just took over a Bokoblin it was only presenting. Its timers, speeds and per-action
 * scratch are whatever they were when it stopped running action(), so put the AI somewhere that
 * re-derives everything from the world on its next tick.
 */
static void daE_OC_mp_onBecomeAuthority(fopAc_ac_c* actor) {
    daE_OC_c* oc = static_cast<daE_OC_c*>(actor);

    // Never integrated on the ghost: stale from create, or from the last time we were authority.
    oc->speedF = 0.0f;
    oc->speed.set(0.0f, 0.0f, 0.0f);
    oc->field_0x6a0 = 0.0f;               // executeAttack's root-motion accumulator
    oc->field_0x6be = 0;                  // wall-avoid turn step
    oc->field_0x69c = oc->current.pos.y;  // checkFall's last ground height
    oc->mPrevShapeAngle = oc->shape_angle.y;

    switch (oc->mActionMode) {
    case E_OC_ACTION_DEATH:
    case E_OC_ACTION_WATER_DEATH:
    case E_OC_ACTION_FALL_DEAD:
    case E_OC_ACTION_FALL:
    case E_OC_ACTION_DAMAGE:
    case E_OC_ACTION_BIG_DAMAGE:
        // Self-completing: driven by the anm, the ground and field_0x6c0 (which is synced). A
        // dying Bokoblin finishes dying here, including its defeated switch.
        break;
    case E_OC_ACTION_FIND:
    case E_OC_ACTION_ATTACK:
    case E_OC_ACTION_FIND_STAY:
    case E_OC_ACTION_MOVE_OUT:
        // Combat: re-acquire from FIND/0 (wait_st, then chase) rather than resume a swing whose
        // timers and root-motion accumulator we never ran.
        oc->setActionMode(E_OC_ACTION_FIND, 0);
        break;
    case E_OC_ACTION_DEMO_MASTER:
    case E_OC_ACTION_DEMO_CHILD:
        // The monkey-cage encounter drives this machine's Link and camera; it was never ours.
        oc->mHide = false;
        oc->setActionMode(E_OC_ACTION_FIND, 0);
        break;
    default:
        // WAIT / WALK / TALK / WATCH / SOUND_WATCH: their targets (mpTalk, mWatchPos, home turn)
        // are stale.
        oc->setActionMode(E_OC_ACTION_WAIT, 0);
        break;
    }
}

/* --- batch 10A: ghost AT --- */
/// The ghost-side half of executeAttack's own hit test (d_a_e_oc.cpp, the ChkAtHit block after
/// the 14..22 swing window): which AT spheres met this machine's Link in the last collision pass.
/// Read-only: the enemy-side reaction (the guard recoil to mOcState 3) stays on the authority
/// (spec 5.3 "Gap"). Both spheres hitting in one pass count twice, as two colliders hit.
static int daE_OC_mp_ghostAtHitsLocal(fopAc_ac_c* actor, int* shielded) {
    daE_OC_c* oc = static_cast<daE_OC_c*>(actor);
    int hits = 0;
    int guarded = 0;
    for (int i = 0; i < 2; i++) {
        dCcD_Sph& at = oc->mSphs_at[i];
        if (!at.ChkAtHit() || at.GetAtHitObj() == NULL) {
            continue;
        }
        fopAc_ac_c* hit_actor = dCc_GetAc(at.GetAtHitObj()->GetAc());
        if (hit_actor == NULL || fopAcM_GetName(hit_actor) != fpcNm_ALINK_e) {
            continue;
        }
        hits++;
        if (at.ChkAtShieldHit()) {
            guarded++;
        }
    }
    if (shielded != NULL) {
        *shielded = guarded;
    }
    return hits;
}
/* --- end batch 10A --- */

static const dusk::mp::EnemyNetAdapter l_daE_OC_netAdapter = {
    "E_ocb",                      // arc: every setBck/checkBck resolves anms in E_ocb
    daE_OC_mp_morf,               // morf
    daE_OC_mp_captureExtra,       // captureExtra
    daE_OC_mp_applyExtra,         // applyExtra
    daE_OC_mp_present,            // present
    daE_OC_mp_engaged,            // engaged
    true,                         // remoteTargetOk: combat only reads slot 0 (spec 5.6.6)
    daE_OC_mp_tgIndexOf,          // tgIndexOf (phase 2)
    daE_OC_mp_tgByIndex,          // tgByIndex (phase 2)
    daE_OC_mp_onBecomeAuthority,  // onBecomeAuthority
    NULL,                         // onGhostDeath (phase 3)
    daE_OC_mp_ghostAtHitsLocal,   // ghostAtHitsLocal (batch 10A)
};

/// Registered at static-init time: the header promises the adapter table is plain storage, and
/// this TU is always linked because the profile list references g_profile_E_OC.
namespace {
struct daE_OC_mp_registrar {
    daE_OC_mp_registrar() { dusk::mp::enemy_register_adapter(fpcNm_E_OC_e, &l_daE_OC_netAdapter); }
};
daE_OC_mp_registrar l_daE_OC_mp_registrar;
}  // namespace
