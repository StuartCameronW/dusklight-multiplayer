#ifndef D_A_MP_GHOST_H
#define D_A_MP_GHOST_H

#include "f_op/f_op_actor.h"
#include "m_Do/m_Do_ext.h"

class J3DModel;

/**
 * Dusk-only actor (fpcNm_MP_GHOST_e, 0x319): the receiver's picture of ONE replicated object that
 * another player's machine simulates — an arrow in flight, a bomb, the boomerang, the Dominion Rod
 * ball. Spec: .claude/future-plans/mp-object-channel.md §2.
 *
 * ★ Never the game's own actor. Every player projectile reads the LOCAL Link each tick (the arrow
 * re-aims from the local camera, the bomb's destructor decrements the local bomb count, the
 * boomerang changes the local equipment), and enemies find the boomerang BY NAME
 * (fopAcM_SearchByName(fpcNm_BOOMERANG_e)). So a ghost has its own procname, draws the game's own
 * model out of the resident "Alink" archive, and has no AT, TG or CO (friendly fire may add an AT
 * later, group 0x4 only, spec §3).
 *
 * The object channel (src/dusk/multiplayer/replication/object_channel.cpp) owns its lifetime and
 * pushes a pose every tick through setNetworkState(); the ghost itself decides nothing.
 *
 * One class, a per-kind switch in createHeap / execute / draw / onNetworkEvent. Phase-1 kinds each
 * add one `d_a_mp_ghost_<kind>.inc`, included at the end of d_a_mp_ghost.cpp, plus their own
 * member block at the end of this class.
 *
 * Create parameter: bits 0-7 the kind byte (object_kinds.hpp ObjKind), bits 8-15 the authority's
 * player id (low byte).
 */
class daMpGhost_c : public fopAc_ac_c {
public:
    /// Longest kind blob a ghost keeps (object_kinds.hpp kMaxKindBlob).
    static const int BLOB_MAX = 48;

    daMpGhost_c();
    ~daMpGhost_c();

    int create();
    int createHeap();
    int execute();
    int draw();

    /// One played-back sample from the channel. Also sets the model matrix immediately, so the
    /// pose is drawn this frame whichever side of the actor pass the channel ran on.
    void setNetworkState(const cXyz& i_pos, const csXyz& i_angle, u8 i_flags, bool i_visible,
        const u8* i_blob, u8 i_blobLen);
    /// A reliable event (ObjEventId) reached on the authority's timeline.
    void onNetworkEvent(u8 i_event, const u8* i_blob, u8 i_blobLen);

    /// The end of create() was reached. The ONLY trustworthy "creation succeeded" signal: a create
    /// that returned cPhs_ERROR_e can still be found by fopAcM_SearchByID for a tick (measured on
    /// the puppet, player_bridge.cpp look_up_puppet).
    bool createComplete() const { return mCreateComplete; }
    u8 getKind() const { return mKind; }
    u8 getAuthority() const { return mAuthority; }

    static u32 makeParam(u8 i_kind, u8 i_authority) {
        return static_cast<u32>(i_kind) | (static_cast<u32>(i_authority) << 8);
    }

private:
    void updateMatrix();

    /* Debug kind (0x7F): the arrow model, nothing else. */
    int createHeapDebug();
    void executeDebug();
    int drawDebug();

    /// The kind's main model, in this actor's own solid heap. NULL for a kind that draws nothing.
    J3DModel* mpModel;
    u8 mKind;
    u8 mAuthority;
    /// Last flags byte pushed (kind bits 0-5, kObjPoseHidden).
    u8 mNetFlags;
    bool mVisible;
    bool mHasPose;
    bool mCreateComplete;
    u8 mBlobLen;
    u8 mBlob[BLOB_MAX];
    u32 mExecuteCount;
    u32 mEventCount;

    /* --- per-kind blocks. Each is owned by one d_a_mp_ghost_<kind>.inc; add that kind's members
     * inside its own block only. --- */

    /* ==== Arrow + pellet (ObjKind Arrow 0x01, Pellet 0x02): d_a_mp_ghost_arrow.inc ==== */
    int createHeapArrow();
    void executeArrow();
    int drawArrow();
    void eventArrow(u8 i_event, const u8* i_blob, u8 i_blobLen);
    /// The friendly-fire collider (dCcD_Stts + dCcD_Cps). Defined in d_a_mp_ghost_arrow.inc and
    /// allocated in this actor's own solid heap, so this header needs no collision include.
    struct ArrowFf;
    void arrowReadBlob();
    void arrowSyncModel(bool i_drawOnly);
    void arrowInitSound();
    void arrowFriendlyFire(bool i_armed);
    /* All set in createHeapArrow (the shared constructor does not know them). */
    ArrowFf* mpArrowFf;
    /// The normal-arrow (Arrow kind) or pellet (Pellet kind) model; mpModel points at it or at
    /// mpArrowBombModel, whichever the blob's type says.
    J3DModel* mpArrowPlainModel;
    /// Arrow kind only: the bomb arrow. Both are loaded because the type arrives in the blob, after
    /// the heap is made.
    J3DModel* mpArrowBombModel;
    Z2SoundObjArrow mArrowSound;
    /// Model origin at the last execute: the tail of the friendly-fire sweep.
    cXyz mArrowPrevPos;
    cXyz mArrowSmokePos;
    csXyz mArrowSmokeAngle;
    u32 mArrowBlurId;
    u32 mArrowSmokeId[2];
    /// From the latest blob (object_kind_arrow.hpp ArrowBlob).
    u8 mArrowType;
    u8 mArrowState;
    u8 mArrowBits;
    u8 mArrowScale;
    u8 mArrowFlash;
    /// mArrowState as of the previous execute, for the stream-carried cues (rebound SE).
    u8 mArrowPrevState;
    bool mArrowSoundReady;
    bool mArrowHavePrev;
    /// A Stuck event arrived.
    bool mArrowStuck;
    /// Exploded, a pellet that hit, or an arrow that struck the local player: draws nothing more.
    bool mArrowGone;
    /// Friendly fire already hit with this projectile (one hit per projectile, spec §3).
    bool mArrowFfHit;
    bool mArrowLoggedUp;
    /* ==== end arrow ==== */

    /* ==== Bomb, water bomb, bombling (0x03-0x05): d_a_mp_ghost_bomb.inc ==== */
    int createHeapBomb();
    void executeBomb();
    int drawBomb();
    void eventBomb(u8 i_event, const u8* i_blob, u8 i_blobLen);
    void setBombAnime(bool i_walking);
    void setBombEffect();

    /// The fuse sparks' particle emitters (daNbomb_c::setEffect). A member with its own
    /// destructor because the spark emitter's trace callback points INTO this actor
    /// (setUserWork(&mTrace)): the destructor unhooks it, as ~daNbomb_c does
    /// (d_a_nbomb.cpp:409-412), whichever way the ghost dies.
    struct BombFx {
        BombFx();
        ~BombFx();
        u32 mEmitterIds[5];
        /// The fuse tip (daNbomb_c::setSmokePos), this tick and last.
        cXyz mPos;
        cXyz mLastPos;
        /// The trace callback's user work (daNbomb_c::field_0xbe4).
        cXyz mTrace;
    };
    BombFx mBombFx;
    /// Fuse sizzle and bounce, as daNbomb_c::mSound.
    Z2SoundObjSimple mBombSound;
    /// Bombling only: walk (0x14) / held (0x15) BCK, in this actor's solid heap.
    mDoExt_bckAnm* mpBombBck;
    /// The local fuse countdown, re-synced from BombBlob whenever its fuseSeq changes.
    s16 mBombFuse;
    u8 mBombFuseSeq;
    u8 mBombBounceSeq;
    /// The last BombBlob flags (object_kind_bomb.hpp kBomb*).
    u8 mBombNetFlags;
    bool mBombSynced;
    /// The floor under it, probed once a tick: blob shadow and lighting room.
    bool mBombHasGround;
    f32 mBombGroundH;
    cXyz mBombGroundNorm;
    bool mBombWalking;
    /// The Explode event arrived: draw nothing more (the replayed explosion takes over).
    bool mBombExploded;
    /// Hang 4 guard (d_a_remote_player.cpp draw()): the sim tick (g_Counter.mCounter0) of the
    /// last entry-draw.
    bool mBombHasDrawnTick;
    u32 mBombDrawnTick;
    /* ==== end bomb ==== */

    /* ==== Gale Boomerang (0x07): d_a_mp_ghost_boom.inc ==== */
    int createHeapBoom();
    void executeBoom();
    int drawBoom();
    void eventBoom(u8 i_event, const u8* i_blob, u8 i_blobLen);
    /// The blade's matrix (daBoomerang_c::setMoveMatrix's second half) from the pose and the spin.
    void setBoomBladeMatrix();
    /// Calc the tornado model with this ghost's own bck frame and joint-4 callback, putting the
    /// SHARED model data's root mtxCalc, joint-4 callback and bck frame back afterwards.
    void calcBoomShippu();
    /// The trace and wind particles (daBoomerang_c::setEffect's thrown branch, minus the BG ones).
    void setBoomEffect();

    /* mpModel is the tornado ("shippu") model: updateMatrix() gives it exactly the matrix
     * setMoveMatrix does (trans * ZXYrot(shape_angle)). The blade has its own model. */
    J3DModel* mpBoomBlade;
    /// "Alink" btk 0x48, the tornado's scrolling texture. An archive resource SHARED with the local
    /// player's own daBoomerang_c: this ghost sets its own frame around its own draw only.
    J3DAnmTextureSRTKey* mpBoomWindBtk;
    /// "Alink" bck 0x13 on the tornado. Owns its J3DMtxCalc (this ghost's heap); the transform
    /// itself is the shared archive resource.
    mDoExt_bckAnm mBoomShippuBck;
    /// The tornado's level sound and the blade's whoosh. Its destructor (deleteObject) is the
    /// teardown, run by ~daMpGhost_c with every other member.
    Z2SoundObjSimple mBoomSound;
    f32 mBoomShippuFrame;
    f32 mBoomWindBtkFrame;
    f32 mBoomShippuSize;
    /// m_modelRotY: -0x1F00 a tick from 0 at the throw, run here rather than sent.
    s16 mBoomSpin;
    u16 mBoomAge;
    u8 mBoomBits;
    u8 mBoomLocks;
    bool mBoomCaught;
    bool mBoomLogged;
    /// Level-emitter ids (0x1FE, 0x1FF, 0x24C). A level emitter that is not refreshed for one
    /// frame is cut by dPa_control_c::level_c::execute, so hiding or deleting the ghost ends them
    /// the way the real boomerang's do; these are zeroed whenever they stop being refreshed.
    u32 mBoomFx[3];
    /// mExecuteCount at the last tornado calc: once per sim tick, not once per draw call.
    u32 mBoomCalcTick;
    /* ==== end boom ==== */

    /* ==== Dominion Rod ball (0x08): d_a_mp_ghost_crod.inc ==== */
public:
    /**
     * The rod ball's wire blobs, ONE definition for both ends: the owner side
     * (src/dusk/multiplayer/replication/object_capture_crod.cpp) writes them, the ghost reads them.
     *
     * Spawn init blob and state blob: [u8 phase][u8 bck], CROD_STATE_LEN bytes.
     *   phase: the owner's daCrod_c param, CROD_PHASE_FLY or CROD_PHASE_RETURN (d_a_crod.cpp:187-356).
     *   bck:   "Alink" resource index of the BCK the owner's ball is playing, CROD_BCK_FIRST..LAST.
     * Event blobs (event ids are ObjEventId values, used per this kind):
     *   Stuck  (1) = possessed a statue: [u16 BE procname of the statue][u8 boss type], 3 bytes.
     *   Hit    (6) = hit something and turned back: [u16 BE procname of what it hit, 0 = none].
     *   Caught (5) = back in Link's hand: empty.
     */
    enum {
        CROD_PHASE_FLY = 3,
        CROD_PHASE_RETURN = 5,
        CROD_STATE_PHASE = 0,
        CROD_STATE_BCK = 1,
        CROD_STATE_LEN = 2,
        CROD_EVENT_PROC_HI = 0,
        CROD_EVENT_PROC_LO = 1,
        CROD_EVENT_BOSS = 2,
        CROD_EVENT_HIT_LEN = 2,
        CROD_EVENT_POSSESS_LEN = 3,
    };
    /// "Alink" resource indices of the ball (d_a_crod.cpp:14-25).
    enum {
        CROD_RES_BMD = 0x22,
        CROD_RES_BRK = 0x40,
        CROD_RES_BTK = 0x46,
        CROD_BCK_FIRST = 13,     ///< ANM_REVIVE_DEMO
        CROD_BCK_WAIT_A = 14,    ///< ANM_WAIT_A: daCrod_c::createHeap's first animation
        CROD_BCK_WAIT_A_T = 15,  ///< ANM_WAIT_A_T: what a ball in flight normally plays
        CROD_BCK_LAST = 18,      ///< ANM_WAIT_C
    };

private:
    int createHeapCrod();
    void executeCrod();
    int drawCrod();
    void eventCrod(u8 i_event, const u8* i_blob, u8 i_blobLen);
    void crodEnterMaterialAnm();

    /// The ball's own joint animation, in this actor's solid heap.
    mDoExt_bckAnm mCrodBck;
    /// The ball's BRK / BTK: shared "Alink" resources (the local Link's ball plays the same ones).
    J3DAnmTevRegKey* mpCrodBrk;
    J3DAnmTextureSRTKey* mpCrodBtk;
    u8 mCrodBckId;
    bool mCrodAnmEntered;
    bool mCrodDrawnLogged;
    /* ==== end crod ==== */
};

#endif /* D_A_MP_GHOST_H */
