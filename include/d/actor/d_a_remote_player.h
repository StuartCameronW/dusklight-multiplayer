#ifndef D_A_REMOTE_PLAYER_H
#define D_A_REMOTE_PLAYER_H

#include "d/d_bg_s_lin_chk.h"
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
    void setNetworkPose(const cXyz& i_pos, s16 i_angleY, f32 i_speed, bool i_sharpTurn);

    u32 getPlayerId() const { return mPlayerId; }
    /* Index into the outfit table of the archive this puppet ACTUALLY mounted, which is not
     * necessarily the byte that arrived on the wire — see daRemotePlayer_outfitFromWire. The
     * network layer compares against this to notice a clothes change. */
    int getOutfit() const { return mOutfit; }
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
    /// Report any change to the puppet's material state. Diagnostic for A5; must run every tick.
    void checkMaterialDrift();
    /* ★ TEMPORARY — Hang 4 bisection. One line per step of the first few calcs, so the last line in
     * a frozen host's log names the call that is spinning. Justified by two facts: the logger
     * fflushes every line (src/dusk/logging.cpp:240), so the last line written really is the last
     * one executed; and the hang reproduces in 14 of 14 SCRIPTED runs, which are drivable without a
     * human. That combination makes a checkpoint trace a substitute for the minidump this machine
     * cannot take (no debugger, and comsvcs MiniDump needs SeDebugPrivilege). Delete once found. */
    void traceCalc(const char* i_step);
    void setHatAngle();
    /// How much of the ambient wind actually reaches the puppet, 0 (fully sheltered) to 1 (open).
    /// daAlink_c::checkWindWallRate (d_a_alink.cpp:5461-5476), cast from the PUPPET's own position.
    f32 checkWindWallRate(const cXyz& i_windDir);
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
    /// Register the puppet's projected shadow for this frame. Must run every draw, and last, since
    /// it wants the models already posed. No one-time setup to pair with it: the shadow slots are
    /// static in the draw list and mShadowKey starts at 0 courtesy of fopAcM_ct.
    void shadowDraw();

    /* Which replicated player this puppet represents; arrives as the create parameter. */
    u32 mPlayerId;
    /* Index into the outfit table in the .cpp. Latched once, so a clothes change on the local
     * player cannot make create() and createHeap() disagree about which body to load. */
    int mOutfit;
    /* Horizontal speed from the network. Picks the gait, against the same thresholds daAlink_c
     * uses; it does NOT rate-scale a single cycle. */
    f32 mNetSpeed;
    /* True on the ticks the SENDER was in daAlink_c::PROC_SLIP. Replicated rather than derived from
     * the yaw — see kPlayerStateSharpTurn in player_state.hpp for why deriving it inverts the
     * truth.
     */
    bool mNetSharpTurn;
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
    /* The skid. Deliberately OPTIONAL — NULL just costs the turn pose, it does not fail createHeap,
     * because a createHeap failure puts the puppet into a permanent full-speed respawn loop. */
    J3DAnmTransform* mpSlipAnm;

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
    /* Counts out the wind breakdown samples, which run on their own schedule and only accumulate
     * while there is actually wind — see setHatAngle() for why they are not gated with the cap
     * comparison above. */
    u16 mWindLogTicks;
    u16 mWindLogCount;
    /* Latches the first room that actually has cap-bending wind, so the search for one stops being
     * guesswork. See setHatAngle(). */
    bool mLoggedWindArea;
    /* Per-tick cap-Y drivers since the last wind log, for the puppet and for the local player
     * alongside. Sampled every tick; see setHatAngle() for why a periodic sample of a per-tick
     * delta would be meaningless.
     *
     * ★ Reported as MEAN as well as peak, and the mean is the one that matters. The yaw kick is
     * subtracted straight into the accumulated angle with no damping on it, so it is a SUSTAINED
     * kick over consecutive ticks that walks the cap out to the clamp; a lone spike decays back
     * within about five ticks. A peak alone cannot tell those two apart, and reading matching
     * peaks as "the inputs agree" is exactly the mistake that sent this the wrong way once. */
    s16 mYawKickPeak;
    s16 mLinkYawKickPeak;
    s32 mYawKickSum;
    s32 mLinkYawKickSum;
    u16 mKickTicks;
    u16 mLinkKickTicks;
    /* Raw magnitude of cap-anchor motion, and — separately — that motion PROJECTED onto the head's
     * sideways axis, which is the term the Y target is actually built from (d_a_alink.cpp:2730).
     * The two diverge whenever the anchor moves along the look direction, so the raw figure
     * agreeing says nothing about the projected one. */
    f32 mLateralMovePeak;
    f32 mLinkLateralMovePeak;
    f32 mProjLateralSum;
    f32 mLinkProjLateralSum;
    s32 mCapYSum;
    s32 mLinkCapYSum;
    u16 mProjTicks;
    /* (The standing-still fire counters that used to live here did their job and were removed. They
     * came back 120/120 on BOTH sides, which killed that theory and sent the search one branch
     * further up, to the FLG0_SWIM_UP guard where the actual difference was. See setHatAngle().) */
    s16 mPrevLinkHeadYaw;
    bool mPrevLinkHeadYawValid;
    /* True once the local player has been sampled in the current window. Without it a wolf's
     * never-running cap would report peaks of 0 and read as a measured "his does not swing either".
     */
    bool mLinkSampled;
    /* Last-seen material signature per watched model, in the order body/head/hands/face. 0xFFFF
     * until the first sample. See checkMaterialDrift(). */
    u16 mMaterialSig[4];
    /* ★ TEMPORARY — Hang 4. How many calcs have been step-traced so far. See traceCalc(). */
    u16 mCalcTraced;

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
    /* The puppet's own line check for the wind-shelter test. Its own, not a borrow of Link's: the
     * whole point is to cast from where the PUPPET stands.
     *
     * ★ dBgS_LinkLinChk, not the plain dBgS_LinChk this started as. The subclass's only job is to
     * call SetLink() (d_bg_s_lin_chk.cpp:69-71), which makes the check PASS THROUGH every polygon
     * flagged link-through (d_bg_w_kcol.cpp:205). With the base class the puppet's upwind ray
     * stopped on collision that Link's ray walks straight past, so the shelter rate collapsed and
     * the wind was killed in exactly the open outdoor areas where it should be strongest. Link
     * casts his through mLinkLinChk (d_a_alink.h:4027) for this reason. */
    dBgS_LinkLinChk mWindLinChk;
    /* What the last shelter cast actually did, kept only so the wind can be logged as a breakdown
     * rather than a single opaque magnitude. Diagnosing this by adjusting the scale and asking how
     * it looked went wrong twice, in opposite directions. */
    f32 mWindWallRate;
    f32 mWindChkDist;
    bool mWindChkHit;
    /* Smoothed wind push, built the way daAlink_c::setWindSpeed builds his — but at the PUPPET's
     * position and from the game's own HIO constant, so it is right whatever form the local player
     * is in. See setHatAngle() for the two wrong answers this replaces. */
    cXyz mWindPush;
    /* False until mCapAnchorPrev holds a real sample. Without it the first tick reads the whole
     * distance from the world origin as one frame of velocity and flings the cap. */
    bool mSwayInited;

    /* Handle for this puppet's entry in the global real-shadow list, exactly
     * daAlink_c::field_0x31a4 (d_a_alink.cpp:19246) and daCow_c::mShadowKey. It is re-issued every
     * frame — the list is reset wholesale each frame (dDlst_list_c::reset, d_drawlist.cpp:1940) and
     * setReal hands out a fresh id (:1761-1762) — so this is really "the id valid for the frame
     * being drawn", used to hang the head, face and hand models off the same shadow the body
     * opened. Kept as a member rather than a local because that is the shape every caller in the
     * tree uses, and because dComIfGd_setShadow still takes the previous key as its first argument
     * even though the current decomp ignores it. Zero at spawn via fopAcM_ct's zeroing of the
     * actor. */
    u32 mShadowKey;
};

/**
 * How the puppet's fopAcM_create parameter is packed: the player id in the low 24 bits, the wire
 * outfit byte in the high 8.
 *
 * The parameter is used because create() needs the outfit at its VERY FIRST entry — the archive
 * mount it kicks off is multi-phase and re-entered over several frames, so there is no later moment
 * at which the choice could still be made. The create parameter is the only channel already in
 * place then; a setter on the actor would arrive after the mount had started, and a "next puppet
 * wears X" static would race with a second player joining on the same tick.
 *
 * 24 bits is not a practical limit: player ids come from a monotonic counter starting at 1
 * (network_manager.hpp) and a session holds at most kMaxPlayers of them.
 */
const u32 daRemotePlayer_playerIdMask = 0x00FFFFFF;
const u8 daRemotePlayer_outfitParamShift = 24;

/**
 * Resolve a replicated outfit byte to an index into the puppet's outfit table.
 *
 * The table is file-static in d_a_remote_player.cpp and stays there — this is the one accessor both
 * ends of the engine seam call, so "which outfit is 2?" has exactly one answer and the network
 * layer never grows a second copy of the list.
 *
 * Anything the table does not name resolves to the hero's clothes rather than refusing to draw:
 * 0xFF ("nobody has reported an outfit"), a peer built against a longer table, or a byte mangled in
 * an unreliable packet. Callers that compare a wire byte against a live puppet's outfit MUST
 * compare the resolved values — see dusk::mp::reconcile_puppet_outfit.
 */
int daRemotePlayer_outfitFromWire(u8 i_wireOutfit);

/**
 * The local player's current outfit, as the byte to put on the wire.
 *
 * Wolf is deliberately not an outfit: Wmdl has its own skeleton and animation set, so it is absent
 * from the table and a wolf reports the hero's clothes. A transformed player therefore looks like a
 * human Link to everyone else until transform replication exists — the same fallback the puppet has
 * always had, just now decided by the wearer.
 */
u8 daRemotePlayer_localOutfitToWire();

#endif /* D_A_REMOTE_PLAYER_H */
