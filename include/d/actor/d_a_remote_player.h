#ifndef D_A_REMOTE_PLAYER_H
#define D_A_REMOTE_PLAYER_H

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
    /* Horizontal speed from the network, used to pick and rate-scale the animation. */
    f32 mNetSpeed;
    /* Resource index of the animation currently playing, so setAnm only fires on a real change. */
    u16 mCurrentAnm;
    /* False until the first network pose lands, so the puppet is never drawn at its spawn pose. */
    bool mHasPose;
    /* Latches the "archive stolen by the local player" warning to one log line. */
    bool mReportedArcConflict;
    /* Guards the one-time archive switch in create(), so a wrong guess can't loop forever. */
    bool mSwitchedArc;

    request_of_phase_process_class mPhaseReq;
    mDoExt_McaMorfSO* mpModelMorf;
    J3DAnmTransform* mpIdleAnm;
    J3DAnmTransform* mpWalkAnm;
};

#endif /* D_A_REMOTE_PLAYER_H */
