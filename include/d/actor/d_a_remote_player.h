#ifndef D_A_REMOTE_PLAYER_H
#define D_A_REMOTE_PLAYER_H

#include "d/d_resorce.h"
#include "f_op/f_op_actor.h"
#include "m_Do/m_Do_ext.h"

class daNpcF_MatAnm_c;

/**
 * Remote player puppet — a Dusk-only actor, not part of the original game.
 *
 * One of these stands in for each other player in a multiplayer session. It is deliberately NOT a
 * daAlink_c and is NEVER installed in the player-0 slot: the camera, input, targeting and the
 * save/quest systems all key off that slot, so a second occupant would fight the local player
 * rather than accompany it.
 *
 * The pose is pushed in from the network layer via setNetworkPose() once per sim tick, already
 * interpolated (src/dusk/multiplayer/replication/state_buffer.hpp). This actor never predicts,
 * never reads input and never runs Link's state machine — driving daAlink_PROC properly is M3.
 */

class daRemotePlayer_c : public fopAc_ac_c {
public:
    ~daRemotePlayer_c();

    int create();
    int createHeap();
    int execute();
    int draw();

    /// Push an interpolated pose in from the network layer, before the actor pass runs.
    void setNetworkPose(const cXyz& i_pos, s16 i_angleY, f32 i_speed);

    u32 getPlayerId() const { return mPlayerId; }
    /* Networked speed, which is NOT mirrored into speedF: nothing moves this actor locally, so the
     * inherited field would read as a permanent zero and misreport the puppet as standing still. */
    f32 getNetSpeed() const { return mNetSpeed; }
    bool hasPose() const { return mHasPose; }
    /* Resource index of the gait currently playing. Exposed for --mp-trace: which animation a
     * puppet picked is otherwise only checkable by looking at the other player's screen. */
    u16 getCurrentAnm() const { return mCurrentAnm; }

private:
    void setMatrix();
    void selectAnimation();
    /// Refresh the floor colour and room the puppet is lit by. Must run every tick.
    void setRoomInfo();
    /// Attach the blink texture animations to the face's eye materials. Once, at createHeap time.
    bool setupFaceAnimation();
    /// Advance (or start) a blink. Must run every tick.
    void playFaceTextureAnime();
    /// Aim the eyes at the local player. Must run every tick, AFTER setMatrix().
    void setEyeMove();

    /* Hat and hair sway. setHatAngle() integrates the angles once per tick after setMatrix(); the
     * joint callback then applies them during the head model's calc() on the NEXT tick, which is
     * the same one-frame lag daAlink_c lives with (d_a_alink.cpp:18530-18538). */
    /// Hook the sway callback onto the head model's joints. Once, at createHeap time.
    void setupHeadSway();
    void setHatAngle();
    void setHairAngle(cXyz* i_apparentWind, f32 i_sinYaw, f32 i_cosYaw);
    void calcHairAngle(s16* o_angle);
    /// Rotate a joint's world matrix about the actor's yaw frame. daAlink_c::setMatrixWorldAxisRot
    /// (d_a_alink.cpp:2098) minus the magne-boot frame, which a puppet never wears.
    void setJointWorldAxisRot(MtxP i_mtx, s16 i_rotX, s16 i_rotY, s16 i_rotZ);

public:
    /// Applies the sway to one head-model joint. Public only because the static J3D callback
    /// trampoline has to reach it; nothing else should call it.
    int headModelCallBack(int i_jointNo);

private:
    /// This puppet's OWN random stream — deliberately not cM_rnd(). See the .cpp for why.
    f32 ownRnd();
    /// ownRnd() scaled, matching cM_rndF's contract.
    f32 ownRndF(f32 i_max);
    /// Drive the private archive mount forward; returns a cPhs_* step for create() to hand back.
    int mountOwnArchive();
    /// Draw one sub-model, lit like the body. Null-tolerant, so a missing part costs a part.
    void drawModel(J3DModel* i_model);

    /* Which replicated player this puppet represents; arrives as the create parameter. */
    u32 mPlayerId;
    /* Index into the outfit table in the .cpp. Latched once, so a clothes change on the local
     * player cannot make create() and createHeap() disagree about which body to load. */
    int mOutfit;
    /* Horizontal speed from the network. Picks the gait, against the same thresholds daAlink_c
     * uses; it does NOT rate-scale a single cycle. */
    f32 mNetSpeed;
    /* Resource index of the animation currently playing, so setAnm only fires on a real change. */
    u16 mCurrentAnm;
    /* False until the first network pose lands, so the puppet is never drawn at its spawn pose. */
    bool mHasPose;
    /* Latches the outfit choice, so create() being re-entered while the mount completes cannot
     * change which body we are loading half way through. */
    bool mOutfitChosen;
    /* Latches the one-shot mount request, so re-entering create() polls rather than re-mounting. */
    bool mResRequested;
    /* Latches the one-shot pointer dump on the first calc(), so it stays one line per puppet. */
    bool mLoggedFirstCalc;
    /* Same idea for the first blink — the one observable that separates "attached and running" from
     * "attached and quietly doing nothing". */
    bool mLoggedFirstBlink;
    /* And for the first time the gaze engages. Prints both eyes' offsets because the bug that
     * produced the "peeling" was a SIGN disagreement between them, which no other measure sees. */
    bool mLoggedFirstGaze;
    /* And for the first tick the cap has actually bent, which is the only thing that separates a
     * live sway from an attached-but-dead one. */
    bool mLoggedFirstSway;
    /* Hair gets its own latch: it is driven by a different mechanism from the cap, so one moving is
     * no evidence at all about the other. */
    bool mLoggedFirstHair;

    /* Blink cursor, exactly daAlink_c::field_0x2fea: 0 means eyes open, anything else is the frame
     * of a blink in progress. Per-puppet rather than shared, so two puppets never blink in unison.
     */
    s16 mBlinkFrame;
    /* State of this puppet's private Wichmann-Hill stream (the same generator cM_rnd() uses, with
     * its own seeds). Private so that blinking a puppet cannot perturb the global game RNG. */
    s32 mRndSeed[3];

    /* ★ This puppet's PRIVATE copy of the outfit archive - not a reference into the global,
     * name-keyed table that dComIfG_resLoad uses. It has to be private because the local player
     * frees his own outfit archive's heap wholesale on a clothes change, without consulting the
     * reference count (d_a_alink_swindow.inc:80-87). Being a plain member is the point:
     * ~dRes_info_c unmounts and frees everything when the actor dies, with no teardown ordering to
     * get wrong. */
    dRes_info_c mOwnRes;
    mDoExt_McaMorfSO* mpModelMorf;
    J3DAnmTransform* mpIdleAnm;
    J3DAnmTransform* mpWalkAnm;
    J3DAnmTransform* mpRunAnm;

    /* Link is four models. The body is the one the animation drives; these three are posed off its
     * joints every frame in setMatrix(). Any of them may be NULL — a puppet missing a head is a
     * better failure than no puppet at all. */
    J3DModel* mpHeadModel;
    J3DModel* mpHandModel;
    J3DModel* mpFaceModel;

    /* The blink. BTP swaps the eyelid texture, BTK slides the texture matrix; they are played in
     * lock-step on the same frame number. Both are this puppet's own copies out of the ARAM
     * archive, so setting a frame here cannot disturb the local player's eyes. */
    J3DAnmTexPattern* mpBlinkBtp;
    J3DAnmTextureSRTKey* mpBlinkBtk;
    /* One per eye material, [0] left and [1] right. They must exist BEFORE the animators are
     * entered — see the .cpp. daNpcF_MatAnm_c rather than a plain J3DMaterialAnm because it also
     * aims the eyes, and rather than daAlink_matAnm_c because its state is per-instance. */
    daNpcF_MatAnm_c* mpEyeMatAnm[2];
    /* Smoothed eye texture offsets, [eye][0]=X [eye][1]=Y. Held here rather than read back out of
     * the material anm because daNpcF_MatAnm_c exposes setters only. */
    f32 mEyeOffset[2][2];
    /* True while the material anm is overriding the BTK's translation with our aim. Dropped only
     * once the offsets have smoothed to centre, so handing control back does not pop. */
    bool mEyeMoveOn;
    /* Ticks left on the current idle glance, and the direction of it as a -1..1 pair
     * (daAlink_c::field_0x2fa7 / field_0x3418 / field_0x341c). Both are rewritten every tick and
     * read through copies; see setEyeMove(). */
    u8 mIdleGazeTimer;
    f32 mIdleGaze[2];
    /* Consecutive ticks spent in the idle animation, and the latch for the one-shot report on
     * whether its frame is actually advancing. */
    u16 mIdleTicks;
    bool mLoggedIdleFrame;
    /* Latches the first strong-wind tick, so a windy area leaves evidence in the log without
     * anyone having to be there watching. */
    bool mLoggedStrongWind;
    /* And the first idle glance — the one eye path the tracking log cannot reach. */
    bool mLoggedIdleGaze;
    /* Counts out the puppet-vs-local-player cap comparison samples. */
    u16 mCapCompareTicks;

    /* --- Hat and hair sway. One array per axis, indexed by HEAD-MODEL joint number, exactly
     * daAlink_c::field_0x302c / field_0x3040 (d_a_alink.h:4273-4274). Joints 1-5 are hair strands,
     * 7-9 the cap's three segments; joint 6 borrows segment 7's angle halved. */
    s16 mSwayAngleX[10];
    s16 mSwayAngleY[10];
    /* Per-cap-segment angular velocity, carried between ticks so the chain overshoots and settles
     * instead of tracking rigidly (field_0x3054 / field_0x305a). */
    s16 mCapVelX[3];
    s16 mCapVelY[3];
    /* Where the HEAD is pointing, in world terms. The cap chain is expressed relative to these, and
     * the hair is rotated in a yaw frame built from the second (field_0x3060 / field_0x3062). */
    s16 mHeadPitch;
    s16 mHeadYaw;
    /* Free-running phase for the cap's flutter, and the three per-segment offsets it produces
     * (field_0x3064 / field_0x3066). This is what keeps the cap alive when nothing else moves. */
    s16 mFlutterPhase;
    s16 mFlutterAngle[3];
    /* Four independent phases driving the hair, at deliberately unrelated rates so strands never
     * move in unison (field_0x3070 / 0x3072 / 0x3074 / 0x3076). */
    s16 mHairPhase[4];
    /* The cap anchor's world position last tick. The apparent wind is measured from how far it
     * moved, so this is the single most important piece of state here (field_0x34c8). */
    cXyz mCapAnchorPrev;
    /* Smoothed wind push, built the way daAlink_c::setWindSpeed builds his — but at the PUPPET's
     * position and from the game's own HIO constant, so it is right whatever form the local player
     * is in. See setHatAngle() for the two wrong answers this replaces. */
    cXyz mWindPush;
    /* False until mCapAnchorPrev holds a real sample. Without it the first tick reads the whole
     * distance from the world origin as one frame of velocity and flings the cap. */
    bool mSwayInited;
};

#endif /* D_A_REMOTE_PLAYER_H */
