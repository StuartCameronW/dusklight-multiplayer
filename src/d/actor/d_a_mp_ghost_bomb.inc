/* Bomb, water bomb, bombling (0x03-0x05): the ghost side. Included at the end of d_a_mp_ghost.cpp.
 * Spec: .claude/future-plans/mp-object-channel.md §2.2 (bombs), §3 (friendly fire).
 *
 * The ghost draws another player's bomb with daNbomb_c's own model, lighting, fuse flash, sparks,
 * sizzle and bounce, from that player's stream (object_capture_bomb.cpp; blob layouts in
 * object_kind_bomb.hpp). It is never a daNbomb_c: a real one would decrement the LOCAL Link's
 * bomb count in its destructor (d_a_nbomb.cpp:397-403), take the local Link's yaw (bombling,
 * :306-308) and drop out of a carry the local Link is not doing (:797-838). It decides nothing:
 * where it is, how its fuse burns and when it goes off all come from the owner.
 *
 * The explosion is the exception. On the owner's Explode event this end creates the game's own
 * explosion-only bomb (daMpGhost_spawnRemoteExplosion, also called by the arrow kind for bomb
 * arrows), masked in d_a_nbomb.cpp procExplodeInit so it hurts only the local Link and only with
 * this end's friendly fire on. Its world effects are the owner's, never applied here (§2.2). */

#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "SSystem/SComponent/c_counter.h"
#include "SSystem/SComponent/c_m3d_g_pla.h"
#include "SSystem/SComponent/c_math.h"
#include "Z2AudioLib/Z2AudioMgr.h"
#include "d/actor/d_a_mirror.h"
#include "d/actor/d_a_nbomb.h"
#include "d/d_bg_s_gnd_chk.h"
#include "d/d_particle.h"
#include "dusk/game_clock.h"
#include "dusk/multiplayer/replication/object_kind_bomb.hpp"
#include "dusk/settings.h"

namespace {

/// "Alink" bmd per bomb type, daNbomb_c::createHeap's bmdIdx (d_a_nbomb.cpp:126): normal, water,
/// bombling.
const u16 l_bombBmd[3] = {0x001E, 0x0027, 0x0026};
/// The bombling's BCKs in "Alink": held / falling (d_a_nbomb.cpp:144, 775) and walking (:1175).
const u16 l_bombBckHeld = 0x15;
const u16 l_bombBckWalk = 0x14;
/// The bombling's size (d_a_nbomb.cpp:315), until the stream says otherwise.
const f32 l_bombBomblingScale = 0.6f;
/// Ground probe start above the bomb, as the puppet's (d_a_remote_player.cpp groundCheck).
const f32 l_bombGndProbe = 50.0f;

/// Remote explosions alive on this end, so a friendly-fire hit can name whose bomb it was.
struct bomb_ExplosionOwner {
    fpc_ProcID id;
    u8 owner;
};
const int l_bombExplosionSlots = 8;
bomb_ExplosionOwner l_bombExplosionOwners[l_bombExplosionSlots];
int l_bombExplosionNext = 0;
int l_bombFfHitLogs = 0;
const int l_bombFfHitLogMax = 64;

int bomb_type(u8 i_kind) {
    if (i_kind == l_kindWaterBomb) {
        return 1;
    }
    return i_kind == l_kindBombling ? 2 : 0;
}

/**
 * daNbomb_c::draw's fuse flash (d_a_nbomb.cpp:1588-1609), for a fuse of `i_fuse`: the red in the
 * bomb's tev colour, pulsing faster as it burns down. Zero when there is no Link to read the HIO
 * fuse length from (no ghost exists then anyway).
 */
s16 bomb_flash_red(s16 i_fuse) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == NULL) {
        return 0;
    }
    const s16 explode_time = link->getBombExplodeTime();
    if (FAST_DIV(explode_time, 8) == 0) {
        return 0;  // a degenerate HIO value would divide by zero below
    }
    f32 brightness;
    if (i_fuse > explode_time) {
        brightness = 1.0f - fabsf(cM_fcos(((f32)(i_fuse - explode_time) * M_PI) /
                                          (f32)FAST_DIV(explode_time, 2)));
    } else if (i_fuse > FAST_DIV(explode_time, 2)) {
        brightness = 1.0f - fabsf(cM_fcos(((f32)(i_fuse - FAST_DIV(explode_time, 2)) * M_PI) /
                                          (f32)FAST_DIV(explode_time, 4)));
    } else if (i_fuse > FAST_DIV(explode_time, 4)) {
        brightness = fabsf(cM_fsin(((f32)(i_fuse - FAST_DIV(explode_time, 4)) * M_PI) /
                                   (f32)FAST_DIV(explode_time, 7)));
    } else {
        brightness = fabsf(cM_fsin(((f32)(i_fuse - FAST_DIV(explode_time, 7)) * M_PI) /
                                   (f32)FAST_DIV(explode_time, 8)));
    }
    return (s16)(brightness * 15.0f) & 0xFF;
}

}  // namespace

/* ---- the explosion (global: the arrow kind calls it for bomb arrows) --------------------------
 */

/**
 * Replay another player's explosion here: the game's own explosion-only bomb (dBomb_c::
 * createNormalBombExplode / createWaterBombExplode, d_bomb.h), so particles, light, wind, sound,
 * AI noise and the distance-scaled rumble are all the game's (d_a_nbomb.cpp procExplodeInit).
 *
 * What it does NOT do, by construction:
 *  - touch any bomb count: it has no FLG0_PLAYER_MAKE, so ~daNbomb_c decrements nothing;
 *  - touch the local Link's bomb state: an explosion-only bomb never carries or grabs;
 *  - break or trigger world things (rocks, bombable walls, webs, Beamos, switches, enemies):
 *    its attack is cut to hit group 0x4 (DUSK_MP_REMOTE_EXPLODE), because the owner's real bomb
 *    already did that on the owner's machine and the result reaches here through world sync.
 *    Applying it here too would decide the same world change twice (spec §2.2). Until M5 syncs a
 *    given flag, a rock the partner blew up stays standing here: a known, visible gap.
 *  - chain-detonate local bombs: its TG is off (vanilla procExplodeInit already turns it off).
 * Friendly fire is victim-judged (§3): this end's own multiplayer.friendlyFire decides whether
 * the blast can hurt the local Link at all (DUSK_MP_FRIENDLY_FIRE).
 *
 * Known leak, not masked: a few actors find explosions by NAME and state rather than by attack
 * (dBomb_c::checkStateExplode within a radius: d_a_obj_toby.cpp:115, d_a_e_nest.cpp:44, and some
 * enemies via checkExplodeNow). They react to a replayed explosion as to a local one.
 */
void daMpGhost_spawnRemoteExplosion(const cXyz& i_pos, bool i_water, u8 i_owner) {
    if (daAlink_getAlinkActorClass() == NULL) {
        // procExplodeInit reads the local Link unchecked (scale, attack radius, rumble distance).
        Log.warn("[mp-obj] remote explosion at ({:.1f}, {:.1f}, {:.1f}) water={} owner={} "
                 "skipped: no local Link",
            i_pos.x, i_pos.y, i_pos.z, i_water ? 1 : 0, i_owner);
        return;
    }
    const bool ff = dusk::getSettings().multiplayer.friendlyFire.getValue();
    u32 param = i_water ? dBomb_c::PRM_WATER_BOMB_EXPLODE : dBomb_c::PRM_NORMAL_BOMB_EXPLODE;
    param |= daNbomb_c::DUSK_MP_REMOTE_EXPLODE;
    if (ff) {
        param |= daNbomb_c::DUSK_MP_FRIENDLY_FIRE;
    }
    cXyz pos = i_pos;
    // The same call createNormalBombExplode makes, with the Dusk bits in the parameter.
    fopAc_ac_c* bomb =
        fopAcM_fastCreate(fpcNm_NBOMB_e, param, &pos, -1, NULL, NULL, -1, NULL, NULL);
    const fpc_ProcID id = bomb != NULL ? fopAcM_GetID(bomb) : fpcM_ERROR_PROCESS_ID_e;
    Log.info("[mp-obj] remote explosion at ({:.1f}, {:.1f}, {:.1f}) water={} owner={} ff={} "
             "proc={}{}",
        i_pos.x, i_pos.y, i_pos.z, i_water ? 1 : 0, i_owner, ff ? 1 : 0, id,
        bomb != NULL ? "" : " (create FAILED)");
    if (bomb != NULL) {
        l_bombExplosionOwners[l_bombExplosionNext].id = id;
        l_bombExplosionOwners[l_bombExplosionNext].owner = i_owner;
        l_bombExplosionNext = (l_bombExplosionNext + 1) % l_bombExplosionSlots;
    }
}

/// From daNbomb_c::procExplode (TARGET_PC): a remote explosion's masked attack hit something.
void daMpGhost_onRemoteExplosionHit(daNbomb_c* i_bomb, fopAc_ac_c* i_hitActor) {
    const fpc_ProcID id = fopAcM_GetID(i_bomb);
    int owner = -1;
    for (int i = 0; i < l_bombExplosionSlots; i++) {
        if (l_bombExplosionOwners[i].id == id) {
            owner = l_bombExplosionOwners[i].owner;
            break;
        }
    }
    if (l_bombFfHitLogs >= l_bombFfHitLogMax) {
        return;
    }
    l_bombFfHitLogs++;
    if (i_hitActor != NULL && i_hitActor == dComIfGp_getPlayer(0)) {
        // The life read here is from BEFORE the damage: Link applies it in his own execute.
        Log.info("Friendly fire: player {}'s explosion hit the local player (proc {}, local life "
                 "before damage {})",
            owner, id, static_cast<unsigned>(dComIfGs_getLife()));
    } else {
        Log.warn("Friendly fire: player {}'s explosion hit actor {} (proc {}), not the local player",
            owner, i_hitActor != NULL ? fopAcM_getProcNameString(i_hitActor) : "(none)", id);
    }
}

/* ---- the ghost --------------------------------------------------------------------------------
 */

daMpGhost_c::BombFx::BombFx() : mPos(cXyz::Zero), mLastPos(cXyz::Zero), mTrace(cXyz::Zero) {
    for (int i = 0; i < 5; i++) {
        mEmitterIds[i] = 0;
    }
}

daMpGhost_c::BombFx::~BombFx() {
    // ~daNbomb_c (d_a_nbomb.cpp:409-412): the spark emitter outlives us by a few frames, and its
    // trace callback reads mTrace. Every ghost kind has this member; only a bomb ever sets an id.
    if (mEmitterIds[0] == 0) {
        return;
    }
    JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(mEmitterIds[0]);
    if (emitter != NULL) {
        emitter->setParticleCallBackPtr(NULL);
    }
}

int daMpGhost_c::createHeapBomb() {
    mpBombBck = NULL;
    mBombFuse = 0;
    mBombFuseSeq = 0;
    mBombBounceSeq = 0;
    mBombNetFlags = 0;
    mBombSynced = false;
    mBombWalking = false;
    mBombExploded = false;
    mBombHasDrawnTick = false;
    mBombDrawnTick = 0;
    mBombHasGround = false;
    mBombGroundH = 0.0f;
    mBombGroundNorm = cXyz::BaseY;

    // The resident "Alink" archive, as daNbomb_c loads player bombs (no resLoad below
    // TYPE_FLOWER, d_a_nbomb.cpp:205). Checked, as the debug kind checks it.
    const char* arc = daAlink_c::getAlinkArcName();
    if (dComIfG_getObjectResInfo(arc) == NULL) {
        Log.warn("[mp-obj] ghost: archive '{}' is not resident; no bomb ghost", arc);
        return FALSE;
    }
    const int type = bomb_type(mKind);
    J3DModelData* modelData = static_cast<J3DModelData*>(dComIfG_getObjectRes(arc, l_bombBmd[type]));
    if (modelData == NULL) {
        return FALSE;
    }
    mpModel = mDoExt_J3DModel__create(modelData, 0x80000, 0x11000084);
    if (mpModel == NULL) {
        return FALSE;
    }

    if (type == 2) {
        J3DAnmTransform* trans =
            static_cast<J3DAnmTransform*>(dComIfG_getObjectRes(arc, l_bombBckHeld));
        if (trans == NULL) {
            return FALSE;
        }
        mpBombBck = JKR_NEW mDoExt_bckAnm();
        if (mpBombBck == NULL ||
            !mpBombBck->init(trans, TRUE, J3DFrameCtrl::EMode_NONE, 1.0f, 0, -1, false))
        {
            return FALSE;
        }
        scale.set(l_bombBomblingScale, l_bombBomblingScale, l_bombBomblingScale);
    } else {
        scale.set(1.0f, 1.0f, 1.0f);
    }
    mpModel->setBaseScale(scale);

    mBombSound.init(&current.pos, 2);

    // daNbomb_c::create's cull box (d_a_nbomb.cpp:345-347).
    fopAcM_SetMin(this, -36.0f, 0.0f, -36.0f);
    fopAcM_SetMax(this, 36.0f, 66.0f, 36.0f);
    fopAcM_setCullSizeFar(this, 10.0f);

    Log.info("[mp-obj] bomb ghost ready kind={} authority={}", objects::kind_name(mKind),
        mAuthority);
    return TRUE;
}

void daMpGhost_c::setBombAnime(bool i_walking) {
    mBombWalking = i_walking;
    if (mpBombBck == NULL) {
        return;
    }
    J3DAnmTransform* anm = static_cast<J3DAnmTransform*>(dComIfG_getObjectRes(
        daAlink_c::getAlinkArcName(), i_walking ? l_bombBckWalk : l_bombBckHeld));
    if (anm == NULL) {
        return;
    }
    // procInsectMoveInit plays the walk at 3x (d_a_nbomb.cpp:1175); procCarryInit the hold at 1x.
    mpBombBck->init(anm, TRUE, -1, i_walking ? 3.0f : 1.0f, 0, -1, true);
}

/// daNbomb_c::setEffect (d_a_nbomb.cpp:484-520): the fuse sparks, refreshed every tick (a level
/// emitter that is not refreshed is let go by dPa_control_c::level_c::execute).
void daMpGhost_c::setBombEffect() {
    static cXyz effectScale(1.8f, 1.8f, 1.8f);
    const bool water = mKind == l_kindWaterBomb;
    if (mpBombBck != NULL || water) {
        static u16 const bombID[] = {0xA0D, 0xA0E, 0xA0F, 0xA10, 0xA11};
        mDoMtx_stack_c::copy(mpModel->getAnmMtx(0));
        if (water) {
            mDoMtx_stack_c::ZXYrotM(0, 0x4000, 0x4000);
        }
        for (int i = 0; i < 5; i++) {
            mBombFx.mEmitterIds[i] =
                dComIfGp_particle_set(mBombFx.mEmitterIds[i], bombID[i], &current.pos, &tevStr);
            JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(mBombFx.mEmitterIds[i]);
            if (emitter != NULL) {
                emitter->setGlobalRTMatrix(mDoMtx_stack_c::get());
            }
        }
    } else {
        mBombFx.mEmitterIds[0] =
            dComIfGp_particle_set(mBombFx.mEmitterIds[0], 0x1DF, &mBombFx.mPos, &tevStr, NULL,
                &effectScale, 0xFF, NULL, -1, NULL, NULL, NULL);
        JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(mBombFx.mEmitterIds[0]);
        if (emitter != NULL) {
            emitter->setParticleCallBackPtr(dPa_control_c::getParticleTracePCB());
            emitter->setUserWork((uintptr_t)&mBombFx.mTrace);
        }
        mBombFx.mEmitterIds[1] =
            dComIfGp_particle_set(mBombFx.mEmitterIds[1], 0x1DE, &mBombFx.mPos, &tevStr, NULL,
                &effectScale, 0xFF, NULL, -1, NULL, NULL, NULL);
    }
}

void daMpGhost_c::executeBomb() {
    objects::BombBlob blob;
    bool haveBlob = false;
    if (mBlobLen != 0) {
        dusk::mp::Reader r(mBlob, mBlobLen);
        haveBlob = objects::read_bomb_blob(r, blob);
    }
    const bool first = !mBombSynced;
    if (haveBlob) {
        mBombNetFlags = blob.flags;
        // The fuse: re-sync when the owner says it stopped burning one per tick, else burn it
        // here the way checkExplode does (d_a_nbomb.cpp:427-455). Cosmetic: only the flash.
        if (first || blob.fuseSeq != mBombFuseSeq) {
            mBombFuse = blob.fuse;
            mBombFuseSeq = blob.fuseSeq;
            mBombSynced = true;
        } else if (mBombFuse > 0 && (blob.flags & objects::kBombTimerStopped) == 0) {
            mBombFuse--;
        }
        // A landing on the owner's machine. Not on the first sample: a ghost made mid-life (a
        // room change here rebuilt it) would replay a bounce that happened long ago.
        if (blob.bounceSeq != mBombBounceSeq) {
            if (!first && mVisible && !mBombExploded) {
                // procWait's call (d_a_nbomb.cpp:1007-1008), with the owner's volume.
                Z2AudioMgr::getInterface()->seStart(Z2SE_OBJ_BOMB_BOUND, &current.pos, 0,
                    dComIfGp_getReverb(tevStr.room_no), 1.0f, blob.bouncePower, -1.0f, -1.0f, 0);
            }
            mBombBounceSeq = blob.bounceSeq;
        }
        const f32 s = blob.scale100 / 100.0f;
        scale.set(s, s, s);
        mpModel->setBaseScale(scale);
    }

    if (mpBombBck != NULL) {
        const bool walking = (mBombNetFlags & objects::kBombWalking) != 0;
        if (walking != mBombWalking) {
            setBombAnime(walking);
        }
        // procInsectMove stops the walk while frozen (d_a_nbomb.cpp:1305).
        if ((mBombNetFlags & objects::kBombFrozen) == 0) {
            mpBombBck->play();
        }
    }

    // The floor under it: the room it is lit as and its blob shadow (daNbomb_c::setRoomInfo and
    // draw use the bomb's own Acch; the ghost has none, so it asks once a tick, like the puppet).
    {
        dBgS_ObjGndChk gndChk;
        cXyz probe = current.pos;
        probe.y += l_bombGndProbe;
        gndChk.SetPos(&probe);
        const f32 groundH = dComIfG_Bgsp().GroundCross(&gndChk);
        mBombHasGround = groundH != -G_CM3D_F_INF;
        if (mBombHasGround) {
            mBombGroundH = groundH;
            cM3dGPla plane;
            if (dComIfG_Bgsp().GetTriPla(gndChk, &plane)) {
                mBombGroundNorm = plane.mNormal;
            } else {
                mBombGroundNorm = cXyz::BaseY;
            }
            tevStr.room_no = dComIfG_Bgsp().GetRoomId(gndChk);
            tevStr.YukaCol = dComIfG_Bgsp().GetPolyColor(gndChk);
        } else {
            tevStr.room_no = dComIfGp_roomControl_getStayNo();
        }
    }

    // daNbomb_c::setSmokePos (d_a_nbomb.cpp:473-482): the fuse tip, and half its movement for the
    // spark trace.
    mBombFx.mLastPos = mBombFx.mPos;
    static Vec const localOffset = {0.0f, 60.0f, 0.0f};
    mDoMtx_multVec(mpModel->getBaseTRMtx(), &localOffset, &mBombFx.mPos);
    mBombFx.mTrace = (mBombFx.mPos - mBombFx.mLastPos) * 0.5f;

    const s8 reverb = dComIfGp_getReverb(tevStr.room_no);
    // Sparks and sizzle while the fuse burns (daNbomb_c::execute :1547-1550). Not before the first
    // draw: the water bomb's and bombling's sparks hang off joint 0, which only a calc fills in.
    if (!mBombExploded && mVisible && mBombHasDrawnTick &&
        (mBombNetFlags & objects::kBombTimerStopped) == 0)
    {
        mBombSound.startLevelSound(Z2SE_OBJ_BOMB_IGNITION, 0, reverb);
        setBombEffect();
    }
    mBombSound.framework(0, reverb);
}

int daMpGhost_c::drawBomb() {
    if (mpModel == NULL || mBombExploded) {
        return 1;
    }
    /* Hang 4 (d_a_remote_player.cpp draw(), memory j3d-one-entry-per-pass): a ghost is created
     * from a network event mid-session, which is exactly when an actor can get two entry-draws on
     * one sim tick, and two entries of one J3DModel in one pass loop the renderer forever. One
     * per sim tick; interpolated frames (is_sim_frame() false) only diff() and are never skipped. */
    if (dusk::game_clock::is_sim_frame()) {
        const u32 simTick = g_Counter.mCounter0;
        if (mBombHasDrawnTick && mBombDrawnTick == simTick) {
            return 1;
        }
        mBombDrawnTick = simTick;
        mBombHasDrawnTick = true;
    }

    // daNbomb_c::draw (d_a_nbomb.cpp:1568-1668) for a player bomb, with the network's flags.
    g_env_light.settingTevStruct(0, &current.pos, &tevStr);

    daAlink_c* player = daAlink_getAlinkActorClass();
    J3DGXColorS10 bomb_color;
    bomb_color.g = 0;
    bomb_color.a = 0;
    const bool frozen = (mBombNetFlags & objects::kBombFrozen) != 0;
    if ((mBombNetFlags & objects::kBombSinking) != 0) {
        bomb_color.r = 0;
        bomb_color.b = 0;
    } else if (frozen && player != NULL) {
        bomb_color.r = player->getFreezeR();
        bomb_color.g = player->getFreezeG();
        bomb_color.b = player->getFreezeB();
    } else {
        bomb_color.r = bomb_flash_red(mBombFuse);
        bomb_color.b = 0;
    }
    if (frozen && player != NULL) {
        tevStr.TevColor.r = player->getFreezeR();
        tevStr.TevColor.g = player->getFreezeG();
        tevStr.TevColor.b = player->getFreezeB();
    }

    g_env_light.setLightTevColorType_MAJI(mpModel, &tevStr);
    J3DModelData* modelData = mpModel->getModelData();
    const bool water = mKind == l_kindWaterBomb;
    J3DMaterial* mat = modelData->getMaterialNodePointer(0);
    if (water) {
        mat->setTevColor(0, &bomb_color);
        mat = modelData->getMaterialNodePointer(1);
        mat->setTevColor(0, &bomb_color);
    } else {
        mat->setTevColor(1, &bomb_color);
    }

    if (mpBombBck != NULL) {
        mpBombBck->entry(modelData);
    }
    // calc + entry (entry only on a sim frame, m_Do_ext.cpp mDoExt_modelDiff).
    mDoExt_modelUpdateDL(mpModel);
    daMirror_c::entry(mpModel);

    bomb_color.r = 0;
    bomb_color.g = 0;
    bomb_color.b = 0;
    if (water) {
        mat->setTevColor(0, &bomb_color);
        modelData->getMaterialNodePointer(0)->setTevColor(0, &bomb_color);
    } else {
        mat->setTevColor(1, &bomb_color);
    }

    // No shadow while in a Link's hands, as daNbomb_c (fopAcM_checkCarryNow, :1658).
    if (mBombHasGround && (mBombNetFlags & objects::kBombHeld) == 0) {
        dComIfGd_setSimpleShadow(&current.pos, mBombGroundH, scale.x * 25.0f, &mBombGroundNorm, 0,
            1.0f, dDlst_shadowControl_c::getSimpleTex());
    }
    return 1;
}

void daMpGhost_c::eventBomb(u8 i_event, const u8* i_blob, u8 i_blobLen) {
    if (i_event != static_cast<u8>(objects::ObjEventId::Explode)) {
        Log.info("[mp-obj] bomb ghost proc={} kind={} authority={}: event {} ignored",
            fopAcM_GetID(this), objects::kind_name(mKind), mAuthority, i_event);
        return;
    }
    objects::BombExplodeBlob blob;
    dusk::mp::Reader r(i_blob, i_blobLen);
    if (i_blob == NULL || !objects::read_bomb_explode_blob(r, blob)) {
        // Still explode, where the ghost is: a lost position must not cost the blast.
        Log.warn("[mp-obj] bomb ghost proc={}: malformed explode blob ({} bytes); exploding at "
                 "the ghost",
            fopAcM_GetID(this), i_blobLen);
        blob.pos[0] = current.pos.x;
        blob.pos[1] = current.pos.y;
        blob.pos[2] = current.pos.z;
        blob.water = (mBombNetFlags & objects::kBombUnderwater) != 0 ? 1 : 0;
    }
    mBombExploded = true;
    Log.info("[mp-obj] bomb ghost proc={} kind={} authority={} explode pos=({:.1f}, {:.1f}, "
             "{:.1f}) water={} shown={} fuseLeft={}",
        fopAcM_GetID(this), objects::kind_name(mKind), mAuthority, blob.pos[0], blob.pos[1],
        blob.pos[2], static_cast<unsigned>(blob.water), mVisible ? 1 : 0, mBombFuse);
    if (!mVisible) {
        // Its room is not shown here (or playback has not reached it): nothing to see or hear.
        Log.info("[mp-obj] bomb ghost proc={}: explosion not replayed, not shown here",
            fopAcM_GetID(this));
        return;
    }
    daMpGhost_spawnRemoteExplosion(cXyz(blob.pos[0], blob.pos[1], blob.pos[2]), blob.water != 0,
        mAuthority);
}
