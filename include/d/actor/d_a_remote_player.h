#ifndef D_A_REMOTE_PLAYER_H
#define D_A_REMOTE_PLAYER_H

#include "JSystem/J3DGraphAnimator/J3DJoint.h"
#include "f_op/f_op_actor.h"
#include "m_Do/m_Do_ext.h"

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
    /// True when the local player has taken over our body archive, which makes calling calc() on
    /// our model a null dereference. See the comment in selectArcName().
    bool modelDataOwnedByPlayer() const;

    /* Which replicated player this puppet represents; arrives as the create parameter. */
    u32 mPlayerId;
    /* Body archive we loaded. Deliberately NOT the one the local player is wearing. */
    const char* mArcName;
    /* Horizontal speed from the network. Picks the gait, against the same thresholds daAlink_c
     * uses; it does NOT rate-scale a single cycle. */
    f32 mNetSpeed;
    /* Resource index of the animation currently playing, so setAnm only fires on a real change. */
    u16 mCurrentAnm;
    /* False until the first network pose lands, so the puppet is never drawn at its spawn pose. */
    bool mHasPose;
    /* Latches the "archive stolen by the local player" warning to one log line. */
    bool mReportedArcConflict;
    /* Guards the one-time archive switch in create(), so a wrong guess can't loop forever. */
    bool mSwitchedArc;
    /* Latches the one-shot pointer dump on the first calc(), so it stays one line per puppet. */
    bool mLoggedFirstCalc;

    request_of_phase_process_class mPhaseReq;
    mDoExt_McaMorfSO* mpModelMorf;
    J3DAnmTransform* mpIdleAnm;
    J3DAnmTransform* mpWalkAnm;
    J3DAnmTransform* mpRunAnm;

    /* Scratch space for ScopedJointIsolation, which has to put back every joint hook it clears.
     * Held per puppet rather than on the stack so the size follows the model's real joint count
     * instead of a cap that a future outfit could quietly exceed. Allocated in createHeap, so they
     * live and die with the actor's solid heap. */
    u16 mJointNum;
    J3DJointCallBack* mpSavedCallBacks;
    J3DMtxCalc** mpSavedMtxCalcs;
};

#endif /* D_A_REMOTE_PLAYER_H */
