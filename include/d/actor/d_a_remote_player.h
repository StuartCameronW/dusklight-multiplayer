#ifndef D_A_REMOTE_PLAYER_H
#define D_A_REMOTE_PLAYER_H

#include "d/actor/d_a_player.h"
#include "d/d_bg_s_gnd_chk.h"
#include "d/d_bg_s_lin_chk.h"
#include "d/d_resorce.h"
#include "f_op/f_op_actor.h"
#include "m_Do/m_Do_ext.h"

class daNpcF_MatAnm_c;
class J3DShape;

/**
 * One animation as the puppet holds it: the two BCKs daAlink_c's table row for it names.
 *
 * ★ Link's body plays TWO animations at once. The root and the legs take an "under" animation while
 * the torso, arms and head take an "upper" one, and every row of daAlink_c::m_anmDataTable carries
 * both ids (daAlink_BckData::m_underID / m_upperID, d_a_alink.h:200-206).
 *
 * mpUpper is NULL when a row names the same resource twice. That is daAlink_c's own convention for
 * "this animation has no separate upper half" (getUnderUpperAnime, d_a_alink.cpp:6987-6994), and it
 * is load-bearing rather than an optimisation: a J3DAnmTransform holds its own current frame, so
 * one object cannot be at two frames at once. Both halves then point at the same object and only
 * one frame controller may step it — exactly the test allAnimePlay makes (d_a_alink.cpp:7274-7280).
 */
struct daRemotePlayer_anm_c {
    J3DAnmTransform* mpUnder;
    J3DAnmTransform* mpUpper;
};

/**
 * How many animations may be blended onto each half of the body at once — daAlink_c's own three
 * (d_a_alink.cpp:4277-4283), kept the same so the machinery below is his machinery.
 *
 * Slot 0 is the animation currently playing and slot 1 the one it is cross-fading with — together
 * they are the gait blend, wait into walk into run (commonDoubleAnime / setDoubleAnimeBlendRatio).
 * Slot 2 is daAlink_c's upper-body OVERLAY: drawing, equipping and putting away items play into
 * UPPER_2 over whatever the legs are doing. The puppet has no path to that one until equipment is
 * replicated, and it is left in place rather than trimmed because that is where it will go.
 */
const int daRemotePlayer_anmSlotNum = 3;

/**
 * Per-leg state for the foot IK. daAlink_c::daAlink_footData_c (d_a_alink.h:181), with its
 * field_0xNN names resolved to what the code actually does with them.
 *
 * Held here rather than reusing daAlink_footData_c so this header does not have to pull in
 * d_a_alink.h — the same reason mCurrentAnm is a u16. The layout does not have to match; nothing
 * passes one of these to the game.
 */
struct daRemotePlayer_footData_c {
    /// The floor probe under this foot hit something usable this tick (field_0x0).
    u8 mOnGround;
    /// Counts down 5 → 0 while a standing foot stays put; at 0 the probe point FREEZES (field_0x1).
    /// This is what stops a standing puppet's feet buzzing between two neighbouring polygons.
    u8 mFreezeTicks;
    /// Ankle pitch, toward the slope the foot is standing on (field_0x2). Applied to two joints.
    s16 mAnkleAngle;
    /// Knee bend (field_0x4).
    s16 mKneeAngle;
    /// Hip pitch (field_0x6).
    s16 mHipAngle;
    /// Last accepted probe point, in world space (field_0x8). The freeze above re-uses it.
    cXyz mLastSample;
    /// The hip/knee/ankle joint matrices as the animation produced them, BEFORE this tick's IK was
    /// written over them (field_0x14). Captured in setFootMatrix and read by setLegAngle on the
    /// NEXT tick — solving against already-solved matrices would compound the rotation every frame.
    Mtx mJointMtx[3];
};

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
    ///
    /// i_idleKind is the wire's PlayerIdleKind — which idle the SENDER is genuinely playing, read
    /// off his own animation heap rather than re-derived from his flags. A plain byte here for the
    /// same reason i_equip is: the .cpp is the one place that maps a wire value to a daAlink_c
    /// animation id, and this header deliberately does not include the wire layout.
    void setNetworkPose(const cXyz& i_pos, s16 i_angleY, f32 i_moveRate, bool i_sharpTurn,
        bool i_zeroSpeed, bool i_modeIdle, bool i_footIkOff, u8 i_equip, u8 i_idleKind);

    u32 getPlayerId() const { return mPlayerId; }
    /* Index into the outfit table of the archive this puppet ACTUALLY mounted, which is not
     * necessarily the byte that arrived on the wire — see daRemotePlayer_outfitFromWire. The
     * network layer compares against this to notice a clothes change. */
    int getOutfit() const { return mOutfit; }

    /* ★ Set on the LAST line of create(), so it is true only for a puppet that got all the way
     * through. The network layer needs this because "the actor framework can find this id" does
     * NOT mean creation succeeded — measured: with createHeap forced to fail, create() logs its
     * failure and returns cPhs_ERROR_e, and fopAcM_SearchByID still hands the actor back on the
     * following tick. Inferring success from findability therefore marked a permanently failing
     * puppet as "it was alive once", which classified every subsequent failure as a vanish and
     * respawned it at full speed forever — the exact bug the spawn policy exists to stop. */
    bool createComplete() const { return mCreateComplete; }
    /* The sender's gait rate, which is NOT mirrored into speedF: it is not a speed, and nothing
     * moves this actor locally anyway, so the inherited field would read as a permanent zero and
     * misreport the puppet as standing still. */
    f32 getNetMoveRate() const { return mNetMoveRate; }
    /* The sender's "standing" bit as this puppet received it. Exposed for --mp-trace so the gait
     * check can be exact: the rate alone cannot distinguish standing from crawling, and the two
     * take different branches. */
    bool getNetZeroSpeed() const { return mNetZeroSpeed; }
    /// The equipment byte as this puppet received it. Exposed for --mp-trace and for the
    /// both-ends comparison a wire change has to pass; see PlayerEquipFlags for the layout.
    u8 getNetEquip() const { return mNetEquip; }
    /// The idle byte as this puppet received it, for --mp-trace and for the both-ends comparison
    /// every wire change has to pass: the trace's `actor` row is only meaningful next to the
    /// `local` row that produced it, and a byte that never arrived looks exactly like one that
    /// arrived as 0. See PlayerIdleKind for the values.
    u8 getNetIdleKind() const { return mNetIdleKind; }
    bool hasPose() const { return mHasPose; }
    /* BCK resource index of the gait currently playing. Exposed for --mp-trace: which animation a
     * puppet picked is otherwise only checkable by looking at the other player's screen.
     *
     * Out of line because it is DERIVED — mCurrentAnm holds daAlink_c's animation id and the
     * resource index is looked up in the game's own m_anmDataTable, so the two can never disagree.
     * The value handed back is unchanged, and .claude/scripts' analyzers still match on it. */
    u16 getCurrentAnm() const;

private:
    void setMatrix();
    /// Build the two-halves animation rig on the body model. Once, at createHeap time.
    bool setupAnimation(J3DModelData* i_modelData);
    /// Put one animation on both halves of the body, cross-fading out of whatever was playing.
    /// daAlink_c::commonSingleAnime + the morf half of setSingleAnime (d_a_alink.cpp:7149-7245).
    void setAnm(const daRemotePlayer_anm_c& i_anm, u8 i_attr, f32 i_morf, f32 i_rate, f32 i_startF,
        s16 i_endF);
    /// Play two animations at once and cross-fade them by weight — the gait blend the local player
    /// moves on. daAlink_c::commonDoubleAnime (d_a_alink.cpp:7009-7066).
    void setDoubleAnm(const daRemotePlayer_anm_c& i_anmA, const daRemotePlayer_anm_c& i_anmB,
        f32 i_blendRatio, f32 i_speedA, f32 i_speedB, f32 i_morf);
    /// Step every frame controller and push its frame into the animation it drives. Must run every
    /// tick, BEFORE the body's calc. daAlink_c::allAnimePlay (d_a_alink.cpp:7262-7290).
    void animePlay();
    void selectAnimation();
    /// Which run animation the puppet is entitled to — the armed one when the wire says a sword is
    /// in hand, which is getMainBckData's own test (d_a_alink.cpp:6939). Out of line.
    const daRemotePlayer_anm_c& runAnm() const;
    /// Which IDLE the puppet is entitled to — the one the sender says he is playing, or the plain
    /// wait for any kind this build has no animation for. Reports the animation id alongside, as a
    /// u16 for the same reason mCurrentAnm is one: this header does not include d_a_alink.h.
    ///
    /// Both halves of the answer have to travel together. The id is what daAlink_c's own tables are
    /// keyed on — the hand pair, the BCK pair, the tired special cases — so returning the animation
    /// without it would leave every caller re-deriving which one it got.
    const daRemotePlayer_anm_c& idleAnm(u16* o_anmID) const;
    /// Show exactly one hand shape per hand, the pair the current animation asks for. Must run
    /// every tick, after selectAnimation() — the choice is per-ANIMATION, not per-actor.
    void setDrawHand();
    /// Refresh the floor colour and room the puppet is lit by. Must run every tick.
    void groundCheck();
    void setRoomInfo();

    /* Foot IK — planting the feet on the floor they are actually over, instead of on the flat plane
     * the animation was authored against. Transcribed from daAlink_c; see the block comment above
     * footBgCheck() in the .cpp for the ordering, which is the part that is easy to get wrong.
     *
     * The three run in this order around the body's calc:
     *   footBgCheck()   — BEFORE calc, from setMatrix(). Probes the floor under each foot,
     *                     integrates the joint angles, and sinks the whole body to the lower foot.
     *   model->calc()
     *   setFootMatrix() — INSIDE calc, from the joint callback on joint 26. Saves the un-IK'd leg
     *                     matrices for the next tick, then rewrites the four joints of each leg
     *                     with this tick's angles.
     *
     * ★ "Inside calc" is the whole of it, and it is not a detail. J3DModel::calc() builds the
     * SKINNING matrices from the joint matrices before it returns, so a leg posed after calc() is
     * posed into buffers nothing reads again. See setupFootIk().
     */
    void footBgCheck();
    void setFootMatrix();
    /// Hook the body-model joint callback onto joints 26, 27 and 29. Once, at createHeap time.
    /// 26 runs setFootMatrix; 27 and 29 apply each leg's hip angle to the joint that follows it,
    /// which is daAlink_c::jointControll's only two arms a puppet can currently reproduce. See the
    /// .cpp for the four it cannot and why.
    void setupFootIk();
    /// Solve one leg for a height delta. daAlink_c::setLegAngle (d_a_alink.cpp:3699), the
    /// param_4 != 0 branch — the other branch is the arms, which the puppet does not IK.
    bool setLegAngle(
        f32 i_heightDelta, daRemotePlayer_footData_c& io_foot, s16* o_hipAngle, s16* o_kneeAngle);
    /// Rotate one joint about a world axis, in place. daAlink_c::setMatrixWorldAxisRot (:2098).
    /// Used by the foot IK (with a pivot, walking down the leg) and by the hair (without one, which
    /// pivots on the joint itself) — one transcription of the original, not two.
    void setMatrixWorldAxisRot(
        MtxP io_mtx, s16 i_rotX, s16 i_rotY, s16 i_rotZ, const cXyz* i_pivot);
    /// Sink the whole model by an offset, smoothed. daAlink_c::setMatrixOffset (:3684) for
    /// field_0x2b94 — the leg-length branch, not the sand branch.
    void setBodySinkOffset(f32 i_target);
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

    /* Sword, sheath and shield. daAlink_c draws them as separate models hung off body joints, so
     * the puppet does too — none of them is part of the body skeleton and none can be animated onto
     * it.
     *
     *   setupEquipModels() — once, at createHeap time. Builds all of them up front, as daAlink_c
     *                        does, so a change of sword is a change of pointer rather than a load.
     *   setEquipMatrix()   — every tick, AFTER the body's calc: every model hangs off body joints,
     *                        which are not final until then.
     *   drawEquip()        — from draw(), inside the sword-blade bracket. See the .cpp.
     *
     * ★ The sword half and the shield half are INDEPENDENT in all three, and that is a correction
     * rather than a style: carrying a shield with no sword drawn is the ordinary state for most of
     * the game, so the two early-outs that used to give up on "no sword" now branch instead. Gating
     * the shield behind the sword makes a shield that appears only while a blade is out — which
     * looks entirely correct in any screenshot taken with one.
     */
    void setupEquipModels();
    void setEquipMatrix();
    void drawEquip();
    /// The sword model this tick's equipment byte selects, or NULL for none. Also reports which
    /// sheath goes with it, since the pairing is not one-to-one.
    J3DModel* currentSword(J3DModel** o_sheath) const;
    /// The shield model this tick's equipment byte selects, or NULL for none. Separate from
    /// currentSword() rather than a second out-parameter on it, because the two selections are
    /// genuinely independent and pairing them is how one ends up gating the other.
    J3DModel* currentShield() const;
    /// Whether the WIRE says the wooden sword is equipped — this puppet's
    /// daPy_py_c::checkWoodSwordEquip(), and named after it so the two read alike at the call sites
    /// that use it: the sheath's draw and the sheath's shadow, which are the two places daAlink_c
    /// spells `!checkWoodSwordEquip()` (d_a_alink.cpp:19725, :19256). A function rather than a
    /// third and fourth open-coded mask/shift, because the two must never be allowed to disagree —
    /// a sheath drawn but not shadowed, or the reverse, is a worse artefact than either bug alone.
    /// (drawEquip's blade-material lines test the same kind value inline; they have already decoded
    /// `kind` for the material number, and re-deriving it through here would read as if it were a
    /// different question.)
    /// Defined in the .cpp because this header deliberately does not include the wire layout.
    bool checkWoodSwordEquip() const;
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

public:
    /// Applies the sway to one head-model joint. Public only because the static J3D callback
    /// trampoline has to reach it; nothing else should call it.
    int headModelCallBack(int i_jointNo);
    /// Applies the foot IK during the BODY model's calc. Public for the same reason, and for
    /// nothing else.
    int bodyModelCallBack(int i_jointNo);

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
    /* ★ The SENDER's own getMoveGroundAngleSpeedRate() — the dimensionless rate daAlink_c blends
     * his gaits from (d_a_alink.cpp:7546-7556, used at :7561) — and NOT a speed, despite what two
     * earlier versions of this member held. It is compared straight against mWalkChangeRate and
     * mRunChangeRate here, with no scaling of any kind, because every scaling step this side used
     * to perform was one the sender had already performed differently. See PlayerState::moveRate.
     *
     * Never negative: daAlink_c takes fabsf as the last thing it does (:7555). */
    f32 mNetMoveRate;
    /* True on the ticks the SENDER was in daAlink_c::PROC_SLIP. Replicated rather than derived from
     * the yaw — see kPlayerStateSharpTurn in player_state.hpp for why deriving it inverts the
     * truth.
     */
    bool mNetSharpTurn;
    /* True on the ticks the SENDER was standing — his own `checkModeFlg(MODE_IDLE) ||
     * checkZeroSpeedF()`, the predicate setBlendMoveAnime branches on (d_a_alink.cpp:7656).
     * Replicated because it selects a branch with a STEP in it, not a taper; see
     * kPlayerStateZeroSpeed. */
    bool mNetZeroSpeed;
    /* daAlink_c::daAlink_ANM id of the animation currently playing, so setAnm only fires on a real
     * change. Held as the ID rather than as the BCK resource index because the id is the key into
     * daAlink_c::m_anmDataTable, which owns BOTH the resource index AND the pair of hand poses that
     * animation is meant to be drawn with. Stored as u16 purely so this header does not have to
     * pull in d_a_alink.h; the .cpp casts it back. */
    u16 mCurrentAnm;

    /* --- Hands. Link wears TWO pairs and shows one shape per hand out of either model; see
     * setDrawHand() in the .cpp for the whole mechanism and for what the puppet used to get wrong.
     */
    /* The two shapes currently shown, so the next tick can hide exactly them — daAlink_c's
     * field_0x06d0 / field_0x06d4 (d_a_alink.cpp:18929-18930). Either may be NULL. */
    J3DShape* mpShownHandShape[2];
    /* The BODY model's own plain hands, which is what an animation asking for hand 0xFE means —
     * daAlink_c's field_0x06d8 / field_0x06dc, per outfit (d_a_alink_wolf.inc:426-477). */
    J3DShape* mpDefaultHandShape[2];
    /* False until the first network pose lands, so the puppet is never drawn at its spawn pose. */
    bool mHasPose;
    /* Latches the outfit choice, so create() being re-entered while the mount completes cannot
     * change which body we are loading half way through. */
    bool mOutfitChosen;
    /// See createComplete(). Zero at spawn via fopAcM_ct's zeroing of the actor.
    bool mCreateComplete;
    /// Latches the one-shot "the shadow was actually granted" line; see shadowDraw().
    bool mLoggedShadow;
    /// Same for the first hand pose actually taken off the hands model; see setDrawHand().
    bool mLoggedHands;
    /// And once for the first tick the puppet holds a drawn sword, printing its hand pair next to
    /// the local player's. Either alone proves nothing; the pair is only wrong relative to what he
    /// is showing on the same tick. See setDrawHand().
    u16 mLoggedGrip;
    /// Packed (local player proc id, his left hand index, his right hand index) as last printed, so
    /// the grip series samples on CHANGE rather than every tick.
    u32 mLastGripState;
    /// And for the first tick the body's two animation halves are actually running SEPARATE
    /// animations, which is the only observable that separates the split rig from the single one it
    /// replaced. Everything else about it — the model, the joints, the morf — looks identical.
    bool mLoggedSplitAnm;
    /// And for the first tick two DIFFERENT gaits are actually blended together at a weight that is
    /// neither end of the band. A blend that never leaves 0 or 1 is the outright switch it
    /// replaced.
    bool mLoggedBlend;
    /// And for the first tick the walk weight is remapped onto Link's mMinWalkRate floor. A puppet
    /// using the raw ratio and one using the floor both "walk"; the difference between them is the
    /// whole gliding report, and it is not visible in a screenshot.
    bool mLoggedWalkFloor;
    /// And for the first tick the puppet runs with a sword drawn. The armed run and the plain one
    /// share their legs, so the animation id, the frames and the blend are the same either way; the
    /// upper pack's pointer is the only thing that separates them.
    bool mLoggedSwordRun;
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

    /* --- The body's animation rig. This is daAlink_c's own mechanism, member for member
     * (d_a_alink.cpp:4271-4283); see setupAnimation() in the .cpp for why mDoExt_McaMorfSO, which
     * used to own the body here, cannot do this job.
     *
     * Nothing here is released by hand. Every pointer is allocated inside the actor's solid heap,
     * which the framework frees wholesale when the actor dies — the same reason the McaMorfSO
     * before it was never deleted either. */
    /* One ratio pack per half. The matrix calculators hold pointers INTO these arrays, so they must
     * outlive the calcs; being plain members of the actor is what guarantees that. */
    mDoExt_AnmRatioPack mAnmPackUnder[daRemotePlayer_anmSlotNum];
    mDoExt_AnmRatioPack mAnmPackUpper[daRemotePlayer_anmSlotNum];
    /* One frame controller per slot per half. daPy_frameCtrl_c rather than a bare J3DFrameCtrl
     * because that is what daAlink_c steps (animePlay, d_a_alink.cpp:7255-7260) and it carries the
     * end-of-animation flags a later action-state pass will want. */
    daPy_frameCtrl_c mUnderFrameCtrl[daRemotePlayer_anmSlotNum];
    daPy_frameCtrl_c mUpperFrameCtrl[daRemotePlayer_anmSlotNum];
    /* The two calculators themselves, installed on the body's joints in setupAnimation(). */
    mDoExt_MtxCalcAnmBlendTblOld* mpUnderCalc;
    mDoExt_MtxCalcAnmBlendTblOld* mpUpperCalc;
    /* ★ The cross-fade Link uses when an animation is REPLACED, as opposed to the ratio blend
     * between two animations playing together. It works by keeping the previous frame's pose for
     * every joint and lerping out of it over a few frames, which is why it needs one array of each
     * per joint. Shared by both calculators exactly as daAlink_c shares his (:4271-4283) — that is
     * what keeps the two halves morfing in step instead of at their own rates. */
    mDoExt_MtxCalcOldFrame* mpOldFrame;
    J3DTransformInfo* mpOldTransInfo;
    Quaternion* mpOldQuat;
    /* True when the last thing put on the body was a blended PAIR rather than a single animation.
     * Two jobs, both daAlink_c's field_0x2f8c (d_a_alink.cpp:7016-7021, :7065): it says whether
     * there is a stride phase worth carrying into the next pair, and whether the next pair needs a
     * cross-fade to get out of what is playing. */
    bool mDoubleAnmSet;
    /* Whether selectAnimation() has ever run. mCurrentAnm carries create()'s seed until it has, and
     * the network layer can resolve this puppet during that window — so getCurrentAnm() reports the
     * no-data sentinel rather than a gait nobody chose. See getCurrentAnm() for the measurement. */
    bool mAnmChosen;
    /* Joints on the body model, read from the model rather than assumed to be Link's 35. The morf
     * range is expressed in joints, and mDoExt_MtxCalcAnmBlendTblOld::calc does its per-frame
     * bookkeeping when it reaches the LAST one (m_Do_ext.cpp:1194-1206). */
    u16 mBodyJointNum;

    daRemotePlayer_anm_c mIdleAnm;
    daRemotePlayer_anm_c mWalkAnm;
    daRemotePlayer_anm_c mRunAnm;
    /* The run with a sword drawn. getMainBckData substitutes m_mainBckSword's row for ANM_RUN when
     * mEquipItem == 0x103 (d_a_alink.cpp:6939-6942), and that row is {DASHS, DASHS} where the
     * empty-handed one is {DASHS, DASHA} — the same legs, sword-carrying arms. So this SHARES
     * mRunAnm.mpUnder and holds no second resource; see load_gait_anm_sword for why sharing is safe
     * and why it is written as a general path rather than as that one aliasing. */
    daRemotePlayer_anm_c mRunSwordAnm;
    /* The skid. Deliberately OPTIONAL — a NULL mpUnder just costs the turn pose, it does not fail
     * createHeap, because a createHeap failure puts the puppet into a permanent full-speed respawn
     * loop. */
    daRemotePlayer_anm_c mSlipAnm;

    /* The three ALERT idles the wire can ask for, on top of the plain wait above. Each is a whole
     * animation of its own rather than a variation on mIdleAnm: daAlink_c reaches all three through
     * setBlendMoveAnime's wait slot, and which one he is in is a decision he has already made and
     * sent (PlayerIdleKind), never something to re-derive here.
     *
     *   mWaitBAnm       ANM_WAIT_B 0x1A       the braced/alert idle. Fires constantly in co-op — a
     *                                         lock-on, an enemy looked at in the last 0x50 ticks, a
     *                                         boss room, or heavy boots/armour alone. It is the one
     *                                         visible in a STILL frame, because its table row asks
     *                                         for hands 1/6 where every other idle asks 4/10.
     *   mServiceWaitAnm ANM_SERVICE_WAIT 0x90 the idle-fidget performance, after 10-15 s of
     *                                         unbroken standing. A partner in a menu hits this one
     *                                         constantly.
     *   mTiredWaitAnm   ANM_WAIT_TIRED 0xB6   the low-health idle. Gameplay INFORMATION rather than
     *                                         decoration: a partner is on his last heart.
     *
     * ★ All three are OPTIONAL, on exactly the terms mSlipAnm is: a NULL mpUnder costs one pose and
     * idleAnm() falls back to the plain wait, where a fatal createHeap would put the puppet into a
     * permanent respawn loop. They are also the three most likely to be squeezed out of the solid
     * heap, so the degrading path is the one that will actually be taken if anything is.
     *
     * ★ Every one of these three rows names ONE resource twice ({WAITB, WAITB}, {SWAITA, SWAITA},
     * {WAITD, WAITD} — d_a_alink.cpp:309, :427, :465), so load_gait_anm takes its
     * m_upperID == m_underID early-out and leaves mpUpper NULL. One animation object, one frame
     * controller, no aliasing to reason about. Verified against the table, not assumed. */
    daRemotePlayer_anm_c mWaitBAnm;
    daRemotePlayer_anm_c mServiceWaitAnm;
    daRemotePlayer_anm_c mTiredWaitAnm;

    /* Link is four models. The body is the one the animation drives; these three are posed off its
     * joints every frame in setMatrix(). Any of them may be NULL — a puppet missing a head is a
     * better failure than no puppet at all. */
    J3DModel* mpHeadModel;
    J3DModel* mpHandModel;
    J3DModel* mpFaceModel;

    /* Sword and sheath, one model per kind, indexed by the wire's PlayerEquipSword values. All
     * built at createHeap time and selected per tick, exactly as daAlink_c holds mpSwAModel /
     * mpSwMModel / mWoodSwordModel side by side (d_a_alink.cpp:4239-4253).
     *
     * Any of them may be NULL and that is survivable: currentSword() simply reports no sword, and
     * the puppet appears unarmed rather than failing to spawn. Only the wooden one is at real risk
     * — it lives in the outfit archive rather than the permanently-mounted Alink one. */
    J3DModel* mpSwordModel[3];
    /* Two sheaths for three swords: the wooden sword and the master sword share al_PODM, and only
     * the ordon sword has its own (d_a_alink.cpp:4354-4366). Indexed by PlayerEquipSword too, with
     * wood and master pointing at the same model, so the caller never has to know that. */
    J3DModel* mpSheathModel[3];

    /* One shield model per kind, indexed by the wire's PlayerEquipShield values.
     *
     * ★ These are the one piece of equipment that CANNOT borrow the local player's models, and the
     * reason is the same refcount-blind wipe that mOwnRes exists for. daAlink_c loads his shield
     * into a heap of his own (d_a_alink.cpp:4975-4981) and calls mpShieldArcHeap->freeAll() the
     * moment the shield changes (d_a_alink_swindow.inc:141), which would free the J3DModelData out
     * from under any puppet pointing into it. The swords escape this only because they live in
     * "Alink", which is mounted at boot and never freed. So each shield comes from this puppet's
     * OWN mount of that kind's archive, held below.
     *
     * All three are built up front, exactly as the swords are, so a shield change is a change of
     * pointer rather than an archive load: the puppet has no proc-driven moment at which to run
     * daAlink_c's four-tick reload (loadShieldModelDVD), and the NULL-model window in the middle of
     * it would show as the shield blinking out. Any of them may be NULL and that costs a shield
     * rather than a puppet, on the same terms as a missing head. */
    J3DModel* mpShieldModel[3];

    /* This puppet's PRIVATE mounts of the three shield archives — CWShd, SWShd and HyShd, one BMD
     * each at index 3. Plain members for exactly the reason mOwnRes is one: ~dRes_info_c unmounts
     * and frees each of them when the actor dies, with no teardown ordering to get wrong. */
    dRes_info_c mShieldRes[3];

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
    /* Last-seen material signature per watched model, in the order body/head/hands/face and then
     * the three swords, two sheaths and three shields. 0xFFFF until the first sample. See
     * checkMaterialDrift().
     *
     * ★ This length, l_watchedModelNum in the .cpp, and the names table beside it move TOGETHER.
     * Widening the count without widening these two arrays is a silent overrun of the actor struct
     * that the priming loop in createHeap() commits before anything is ever drawn. */
    u16 mMaterialSig[12];
    /* Last-seen COUNT of materials drawing the dissolve, same model order. Watched separately from
     * the signature above because the signature is material 0's, and the warp toggles break on the
     * first material already in the target state — so a model can sit with material 0 clean and
     * every other material dissolving without the signature ever moving. 0xFFFF until first
     * sampled. Same length and same model order as mMaterialSig above. */
    u16 mWarpMatCount[12];
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

    /* ★ The puppet's OWN ground check, and the height it last found.
     *
     * It cannot use fopAcM_gc_c::gndCheck(&current.pos), which is what it did before, and the
     * reason is worth writing down because the failure is silent and total:
     *
     * The bg ground test is "the highest floor strictly BELOW the query point", and the `strictly`
     * is literal — cBgW::RwgGroundCheckCommon compares `cy < pgndchk->GetPointP().y`
     * (d_bg_w.cpp:606-625), so a floor at EXACTLY the query height is rejected. Meanwhile every
     * grounded actor in the game finishes its tick standing at exactly the floor height, because
     * dBgS_Acch::GroundCheck writes pos.y = ground_h (d_bg_s_acch.cpp:164-167). The puppet
     * replicates the sender's already-snapped position verbatim, so asking from the feet is asking
     * for a floor strictly below a point that is ON the floor. Measured: 800 consecutive failures
     * for a standing puppet; the only successes were the settling frames right after a load, and
     * frames where the sender happened to be airborne.
     *
     * daAlink_c never hits this because he does not use fopAcM_gc_c at all for his own footing — he
     * reads mLinkAcch.m_gnd (d_a_alink.cpp:5134, :19246), filled by a query raised 60 units above
     * the feet. So do the same thing here, with the same 60: it is dBgS_Acch's own
     * m_gnd_chk_offset default (d_bg_s_acch.cpp:65) rather than a number invented for this actor.
     *
     * A full dBgS_Acch was the other candidate and is deliberately NOT used: CrrPos() WRITES
     * pos.y and zeroes speed.y (d_bg_s_acch.cpp:166-169), which would let local collision overrule
     * a position the wire owns, and its line check runs against an old_pos that means nothing for
     * an interpolated actor. The one thing given up by not using it is the thin-ceiling clamp
     * (:148-157), which matters only where a floor sits under 60 units below a ceiling. */
    dBgS_ObjGndChk mGndChk;
    f32 mGroundHeight;
    /* False when the puppet genuinely has no floor under it — over a pit, mid-warp, or airborne.
     * Distinct from "we failed to ask properly", which is what the old code could not tell apart.
     */
    bool mGroundValid;

    /* --- Foot IK --------------------------------------------------------------------------- */

    daRemotePlayer_footData_c mFootData[2];  // [0] left, [1] right, as daAlink_c orders them.
    /* A SECOND check object, and it has to be. mGndChk above is queried once per tick and then read
     * again by the shadow in draw() without being re-queried — deliberately, see its comment — so
     * running two more probes through it would hand the shadow whichever foot asked last. */
    dBgS_ObjGndChk mFootGndChk;
    /* Inverse of the body's base transform, and that transform composed into model space
     * (daAlink_c::mInvMtx and field_0x2be8, built in his setMatrix at d_a_alink.cpp:5781-5785).
     * setLegAngle works in model space, so it needs the world→local matrix; the body sink then
     * has to update BOTH of these and the base matrix together or the two disagree by the offset.
     */
    Mtx mInvMtx;
    Mtx mFootLocalMtx;
    /* How far the whole model is currently lowered so the lower foot can reach its floor
     * (field_0x2b94). Smoothed by cLib_addCalc, so it eases rather than snapping when the floor
     * under a foot changes. Without it, IK'd legs stretch instead of the body settling. */
    f32 mBodySinkOffset;
    /* True once setFootMatrix has run at least once, i.e. once mFootData[].mJointMtx holds real
     * joint matrices. Everything IK is skipped until then: on the first tick those matrices are
     * zeroed and the probe points would be taken from a model that has never been calc'd. */
    bool mFootDataValid;
    /* The SENDER's daAlink_c::checkModeFlg(MODE_IDLE). Three separate things in footBgCheck turn on
     * it — the probe freeze, the body sink, and pitching the foot to the slope — and all three are
     * "is he standing", which no amount of looking at an interpolated position can answer. */
    bool mNetModeIdle;
    /* The SENDER's own "no foot IK this tick" test — not grounded, magne boots, sinking in sand,
     * jumping, climbing, swimming, riding (d_a_alink.cpp:3872). Replicated whole rather than
     * approximated by a local height tolerance, which would have to re-derive every one of those
     * states from a position that arrives interpolated and two ticks late. */
    bool mNetFootIkOff;
    /// Latches the one-shot "the IK ran and here is what it saw" line; see footBgCheck().
    bool mLoggedFootProbe;
    /// And the one-shot "it actually bent a leg". Both are needed — on flat ground the first fires
    /// and the second correctly does not, and only the pair separates that from never running.
    bool mLoggedFootIk;
    /* The SENDER's equipment byte, whole. Every field in it is an answer he computed rather than a
     * fact about the world, so there is nothing here to re-derive; see PlayerEquipFlags. */
    u8 mNetEquip;
    /* The SENDER's idle byte, whole, for exactly the same reason: which idle he is in is not a fact
     * about the world at all. It is sampled off his own animation heap — what he is genuinely
     * PLAYING, every substitution already applied — so there is nothing here to re-derive and no
     * combination of position, rate and yaw that could have answered it. See PlayerIdleKind, and
     * idleAnm() in the .cpp for the subset this build can honour. Zero (the plain wait) at spawn
     * via fopAcM_ct's zeroing of the actor, which is the right reading before any pose arrives. */
    u8 mNetIdleKind;
    /* The idle animation id resolved LAST tick, so a change of idle can be cross-faded exactly once
     * rather than every tick. daAlink_c's own equivalent is the "did getUnderUpperAnime actually
     * swap anything" return that setDoubleAnime tests (d_a_alink.cpp:7085-7087) before it
     * substitutes a morf. Seeded in create() alongside mCurrentAnm; u16 for the same reason. */
    u16 mCurrentIdleAnm;
    /// Latches the one-shot "an alert idle is genuinely being drawn" line. Its whole job is to make
    /// the feature falsifiable: a fallback for a resource that failed to load looks identical to
    /// the feature not existing. See selectAnimation().
    bool mLoggedIdleKind;
    /// Latches the one-shot "equipment is being drawn, and here is what" line; see drawEquip().
    bool mLoggedEquip;
    /// And the same for the shield, which needs its OWN latch rather than sharing mLoggedEquip: the
    /// two halves of the equipment byte are drawn independently, so a session in which the sword
    /// line appears and the shield line does not is a real and interesting state. Zero at spawn via
    /// fopAcM_ct's zeroing of the actor, like every other latch here.
    bool mLoggedShield;

    /* Hang 4's guard. The sim tick this puppet last ENTERED the draw list on, and whether it ever
     * has. A J3DModel may be entered once per pass; a second entry builds a self-referential shape
     * chain and the renderer never terminates. See draw() for the measurement that pinned it. */
    u64 mDrawnSimTick;
    bool mHasDrawnSimTick;
    /// Latches the one-shot "a second entry-draw was skipped" line. Its ABSENCE from a session log
    /// is meaningful: it means the double draw did not happen in that session, not that the guard
    /// is broken.
    bool mLoggedDoubleDraw;
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

/**
 * Why the last puppet creation attempt failed, for the network layer's spawn backoff
 * (src/dusk/multiplayer/replication/player_bridge.cpp).
 *
 * The framework tells the bridge THAT a creation failed — it cancels the create request, which the
 * bridge sees as an id that is neither creating nor in the actor layer — but it cannot tell it why.
 * Without a reason the "gave up on this puppet" line would be exactly the kind of message that
 * hides a real bug behind a shrug, so create() leaves one here.
 *
 * A global rather than a call into dusk::mp, so the dependency keeps pointing the one way it
 * already does: the bridge includes this header and nothing under src/dusk/multiplayer is included
 * from here. The player id is carried alongside so a reason left behind by a DIFFERENT puppet's
 * failure is detectable rather than silently misattributed; the bridge clears the reason as it
 * takes it. mReason is always a string literal, so there is no lifetime to manage.
 */
struct daRemotePlayer_createFail_c {
    u32 mPlayerId;
    const char* mReason;
};

extern daRemotePlayer_createFail_c g_daRemotePlayer_lastCreateFail;

#endif /* D_A_REMOTE_PLAYER_H */
