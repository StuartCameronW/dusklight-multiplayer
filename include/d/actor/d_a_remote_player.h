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
    /// This puppet's OWN random stream — deliberately not cM_rnd(). See the .cpp for why.
    f32 ownRnd();
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
};

#endif /* D_A_REMOTE_PLAYER_H */
