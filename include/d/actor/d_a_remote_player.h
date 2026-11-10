#ifndef D_A_REMOTE_PLAYER_H
#define D_A_REMOTE_PLAYER_H

#include "Z2AudioLib/Z2Creature.h"
#include "Z2AudioLib/Z2SoundStarter.h"
#include "d/actor/d_a_player.h"
#include "d/d_cc_d.h"
#include "d/d_bg_s_gnd_chk.h"
#include "d/d_bg_s_lin_chk.h"
#include "d/d_resorce.h"
#include "dusk/interp/samples.h"
#include "f_op/f_op_actor.h"
#include "m_Do/m_Do_ext.h"

class daNpcF_MatAnm_c;
class J3DPacket;
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
    /* The under BCK's own sound-animation ("BAS") block — the footstep notes — or NULL when the
     * BCK carries none (JUTDataFileHeader::mSeAnmOffset == 0xFFFFFFFF). It points INTO mpUnder's
     * load buffer, which lives as long as the animation does, so unlike daAlink_c (who has to copy
     * it out of a reused anm heap, d_a_alink.cpp:14025-14030) nothing is copied. Only the under
     * half's block is kept, because the under heap is the only one setBasAnime ever reads. */
    const void* mpUnderBas;
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
 * Puppet sound, the iron-boots part of slice 2 (.claude/future-plans/puppet-sound.md §2): the
 * footstep swap from Z2LinkSoundStarter::startSound (Z2LinkMgr.cpp:578-601), keyed on the
 * SENDER's boots rather than on Z2GetLink(), which is the local player.
 *
 * ★ Constructed with `false`, as Z2LinkSoundStarter is (Z2LinkMgr.cpp:528). `true` would register
 * this as the global starter and take every sound in the game through a puppet.
 *
 * Not reproduced from that function: the magnetised mapinfo (magne-boot stick, not on the wire),
 * the water, sinking-pitch and equipment-jingle decoration, and the foot-miss voice.
 */
struct daRemotePlayer_soundStarter_c : public Z2SoundStarter {
    daRemotePlayer_soundStarter_c() : Z2SoundStarter(false), mHeavyBoots(false) {}
    using Z2SoundStarter::startSound;
    virtual bool startSound(JAISoundID i_soundID, JAISoundHandle* i_handle,
        const JGeometry::TVec3<f32>* i_pos, u32 i_mapinfo, f32 i_fxMix, f32 i_pitch, f32 i_volume,
        f32 i_pan, f32 i_dolby, u32 i_moveSteps);

    /// The sender's kPlayerGearHeavyBoots, i.e. his Z2CreatureLink boots type == 1.
    bool mHeavyBoots;
};

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
    /// D3b. The sender's attention byte (PlayerAtnKind), pushed in alongside setNetworkPose() every
    /// tick. A separate call so the outfit/model work and this one don't collide in one
    /// signature. Stored whole; selectAtnAnimation() treats an unknown value as "none".
    void setNetworkAtn(u8 i_atnKind) { mNetAtnKind = i_atnKind; }
    u8 getNetAtnKind() const { return mNetAtnKind; }
    /// The sender's daAlink_c::checkUpperGuardAnime(), pushed in every tick like the attention
    /// byte. While set, the gait and attention rows below ANM_SWIM_WAIT play getMainBckData's
    /// shield table (m_mainBckShield) instead of the plain or sword pair.
    void setNetworkUpperGuard(bool i_guard) { mNetUpperGuard = i_guard; }
    bool getNetUpperGuard() const { return mNetUpperGuard; }
    /// The sender's daPy_py_c::checkEquipHeavyBoots() (kPlayerGearHeavyBoots), pushed in every tick
    /// like the guard bit. It drives the footstep swap WALK/JUMP_DUMMY -> _HEAVY, which is
    /// Z2LinkSoundStarter's test on the same flag's boots type (Z2LinkMgr.cpp:578-601); see
    /// daRemotePlayer_soundStarter_c. It does NOT pick the gait. The gait is mNetHeavyGait, a
    /// separate fact (kPlayerGearHeavyGait). It also draws the boot models (daAlink_c's
    /// mpLinkBootModels, d_a_alink.cpp:5995/:19832); see setupHeavyBootsModel().
    void setNetworkHeavyBoots(bool i_heavyBoots) {
        mNetHeavyBoots = i_heavyBoots;
        mSoundStarter.mHeavyBoots = i_heavyBoots;
    }
    bool getNetHeavyBoots() const { return mNetHeavyBoots; }
    /// kPlayerGearHeavyGait: the sender's slot 1 is ANM_WALK_HEAVY, so his legs are on
    /// setBlendMoveAnime's booted branch. selectAnimation() plays that branch while it is set.
    void setNetworkHeavyGait(bool i_heavyGait) { mNetHeavyGait = i_heavyGait; }
    bool getNetHeavyGait() const { return mNetHeavyGait; }
    /// The sender's locomotion phase (wire `locPhase`): where in the blended stride his under slot
    /// 0 is, 0..255 for one cycle. i_valid is false while the action override is on, when the byte
    /// means nothing. Pushed every tick alongside setNetworkPose(); setDoubleAnm() locks onto it.
    void setNetworkLocPhase(u8 i_phase, bool i_valid);
    /// The stride phase the puppet is DRAWING, 0..1, read off the animation object in under slot 0
    /// — or -1 when no blended locomotion pair is on the body (override, skid, fidget). For
    /// --mp-trace's `locPhase` column.
    f32 getDrawnLocPhase();
    /// Worklist 14. The sender's head turn and torso lean, as jointControll's answers: the neck
    /// (field_0x3124.x/.y), joint 1's pitch (`sp8`), the twist (field_0x30c8), joint 1's roll
    /// (mBodyAngle.z), and the whole-model bank (shape_angle.z). Pushed every tick alongside
    /// setNetworkPose(); the joint callbacks on joints 1, 2 and 4 read them during calc.
    void setNetworkJointAim(
        s16 i_neckX, s16 i_neckY, s16 i_bodyX, s16 i_bodyTwist, s16 i_bodyZ, s16 i_bankZ);
    s16 getNetNeckX() const { return mNetNeckX; }
    s16 getNetNeckY() const { return mNetNeckY; }
    s16 getNetBodyX() const { return mNetBodyX; }
    s16 getNetBodyTwist() const { return mNetBodyTwist; }
    s16 getNetBodyZ() const { return mNetBodyZ; }
    /// How many times the joint-aim callback has run for arm 0 (joint 1), 1 (joint 2), 2 (joint 4).
    /// For --mp-trace: zero means the hook is not attached, which looks exactly like "no head
    /// turn".
    u32 getJointAimFires(int i_arm) const {
        return i_arm >= 0 && i_arm < 3 ? mJointAimFires[i_arm] : 0;
    }
    bool hasPose() const { return mHasPose; }
    /* Where the floating name tag hangs from (src/dusk/imgui/ImGuiNameTags.cpp): the body's head
     * joint as it is DRAWN this frame. On an interpolated frame that is frame_interp's lerped copy
     * of the joint matrix — the one J3DShapeMtx renders the head with — so a tag placed from it
     * cannot drift against the model the way one placed from current.pos (a sim-tick value) would.
     * False before the first pose arrives, or without a body model. */
    bool getNameTagAnchor(cXyz* o_pos);
    /* BCK resource index of the gait currently playing. Exposed for --mp-trace: which animation a
     * puppet picked is otherwise only checkable by looking at the other player's screen.
     *
     * Out of line because it is DERIVED — mCurrentAnm holds daAlink_c's animation id and the
     * resource index is looked up in the game's own m_anmDataTable, so the two can never disagree.
     * The value handed back is unchanged, and .claude/scripts' analyzers still match on it. */
    u16 getCurrentAnm() const;

    /* --- Action animation override (wire block 2; .claude/future-plans/puppet-action-states.md).
     *
     * Pushed in every tick from the network layer alongside setNetworkPose(). i_under == 0xFFFF
     * means "no override" and the puppet plays its own locomotion reconstruction. Plain values,
     * already decoded from the wire's fixed point, for the same reason setNetworkPose takes plain
     * values: this header does not include the wire layout. */
    void setNetworkAction(u16 i_under, u16 i_upper, f32 i_frame, f32 i_upperFrame, f32 i_rate,
        f32 i_morf, u8 i_handL, u8 i_handR, const cXyz& i_rootTrans);
    /* What the puppet is actually DRAWING for the override, for --mp-trace's actor row — the heap
     * idx installed in the packs and the animation objects' own frames, not what was received.
     * 0xFFFF when no override is playing. */
    u16 getActDrawnUnder() const;
    u16 getActDrawnUpper() const;
    f32 getActDrawnFrame() const;
    f32 getActDrawnUpperFrame() const;
    f32 getActDrawnRate() const;
    f32 getActDrawnMorf() const { return mActAppliedMorf; }
    /* Block (4): the second animation of a double anime (i_phase1 is its phase, 0..1),
     * jointControll's extra joint-0 rotation (field_0x3080 / field_0x3082), and setMatrix's yaw
     * offset (field_0x308c) and pitch (shape_angle.x). Only applied while the block-2 override is
     * playing. i_under1 == 0xFFFF: single anime. */
    void setNetworkActionSlot1(u16 i_under1, u16 i_upper1, f32 i_phase1, f32 i_ratio1,
        s16 i_rootRotX, s16 i_rootRotZ, s16 i_yawOff, s16 i_pitch);
    /// What the puppet DRAWS for block (4), for --mp-trace's actor row.
    u16 getActDrawnUnder1() const;
    u16 getActDrawnUpper1() const;
    f32 getActDrawnFrame1() const;
    f32 getActDrawnPhase1() const;
    f32 getActDrawnRatio1() const;
    s16 getActDrawnRootRotX() const { return mActAppliedRootRotX; }
    s16 getActDrawnRootRotZ() const { return mActAppliedRootRotZ; }
    s16 getActDrawnYawOff() const { return mActAppliedYawOff; }
    s16 getActDrawnPitch() const { return mActAppliedPitch; }
    /// The hand-model material the puppet is showing on hand i (0 left, 1 right), or 0xFE for the
    /// body's own hand. Resolved back from the shape pointer, which is what is drawn.
    u8 getDrawnHand(int i_hand) const;
    /// Joint 0's translation as drawn: model space (o_local) and world space (o_world). False with
    /// no model or no pose.
    bool getDrawnRoot(cXyz* o_local, cXyz* o_world);

    /* --- block (3): held item (d_a_remote_player_held.inc).
     *
     * Pushed in every tick alongside setNetworkPose(). i_kind / i_flags / i_mat are the wire's
     * PlayerHeldItem / PlayerHeldFlags / heldMat bytes (dusk/player_held.hpp); i_upper2Idx 0xFFFF
     * means no UPPER_2 overlay. Plain values, decoded by the bridge. */
    void setNetworkHeldItem(u8 i_kind, u8 i_flags, u8 i_mat, u16 i_upper2Idx, f32 i_upper2Frame,
        f32 i_upper2Ratio, f32 i_upper2Morf);
    /* --- v19: friendly fire. The sender's live sword hitbox in world space: capsule 0 is
     * i_points[0]-[1], 1 is [2]-[1], 2 is [0]-[2]; bit i of i_mask = capsule i is live. */
    void setNetworkAttack(u8 i_mask, f32 i_radius, u8 i_spl, const cXyz* i_points);
    /* What the puppet DREW for --mp-trace's actor row: the kind/flags/mat of the models it entered
     * last pass (none if the model failed to load), and the overlay installed in upper slot 2 with
     * that animation object's own frame and the pack's ratio. */
    u8 getHeldDrawnKind() const { return mHeldDrawnKind; }
    u8 getHeldDrawnFlags() const { return mHeldDrawnFlags; }
    u8 getHeldDrawnMat() const { return mHeldDrawnMat; }
    u16 getUpper2DrawnIdx() const { return mUpper2Shown; }
    f32 getUpper2DrawnFrame() const;
    f32 getUpper2DrawnRatio();
    u8 getUpper2DrawnMorf() const { return mUpper2AppliedMorf; }
    /// The bottle's joint-1 callback body (daAlink_c::bottleModelCallBack). Public for the static
    /// trampoline only.
    int heldBottleCallBack();

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
    /// D3b. Play daAlink_c's attention (Z-target / lock-on) stance and strafe set when the wire
    /// says the sender is in it. setBlendAtnMoveAnime / setBlendAtnBackMoveAnime's bands, read off
    /// the same HIO tables. True if it set this tick's animation. selectAnimation() then returns.
    bool selectAtnAnimation();
    /// D3b. Load the attention set into the solid heap. Non-fatal, once, from createHeap().
    void loadAtnAnimations();
    /// The shield-guard variants of every row m_mainBckShield covers. Non-fatal, once, from
    /// createHeap(), after loadAtnAnimations() because it reuses what that loaded.
    void loadGuardAnimations();
    /// The standing pivot (STEPL / STEPR and their tired rows). Non-fatal, once, from createHeap(),
    /// after setupHeldItems() so a short heap costs only this. See selectAnimation().
    void loadStepTurnAnimations();
    /// i_plain, or its shield-guard variant when the wire says he is guarding and that variant is
    /// resident. getMainBckData's guard branch, which beats the sword one.
    const daRemotePlayer_anm_c& guardOr(u16 i_anmID, const daRemotePlayer_anm_c& i_plain) const;
    /// Show exactly one hand shape per hand, the pair the current animation asks for. Must run
    /// every tick, after selectAnimation() — the choice is per-ANIMATION, not per-actor.
    void setDrawHand();
    /// Action override: allocate the six Link-identical anm heaps and hook joint 0. LAST in
    /// createHeap, non-fatal — on any failure the override stays off and locomotion plays as
    /// before.
    void setupActionAnimation();
    /// Action override: play the sender's BCK if the wire says so. True if it set this tick's
    /// animation, in which case selectAnimation() returns without choosing a gait.
    bool selectActionAnimation();
    /// Action override: leave it, handing the body back to selectAnimation() with a cross-fade.
    void exitActionAnimation(const char* i_why);
    bool copyActionBas();
    /// Action override: load one BCK into one of the action heaps, the way daAlink_c's
    /// getAnimeResource does. Returns the cached animation when the heap already holds i_idx.
    J3DAnmTransform* loadActionBck(daPy_anmHeap_c& io_heap, u16 i_idx, u32 i_bufSize,
        J3DAnmTransform* i_current, bool* o_reloaded);
    /// Action override: the root-motion overwrite, from the joint-0 callback (inside calc).
    void applyActionRoot();
    /// Action override, block (4): install / seat / clear the slot-1 blend. Called every tick from
    /// selectActionAnimation() after slot 0 is installed and seated.
    void selectActionSlot1();
    /// Drop slot 1 from the packs and restore slot 0's full weight.
    void clearActionSlot1();
    /// Forget what the slot-1 heaps hold: a slot-0 load can overwrite their regions of the shared
    /// buffer.
    void invalidateActionSlot1();
    /// Refresh the floor colour and room the puppet is lit by. Must run every tick.
    void groundCheck();
    void setRoomInfo();

    /* Puppet sound, slice 1: the footsteps (and any other note) baked into the BCKs it plays.
     * See .claude/future-plans/puppet-sound.md. All three are NEW and touch nothing else.
     *
     * setBasSource() — daAlink_c::setBasAnime + initBasAnime (d_a_alink.cpp:13988-14060), with the
     *                  BAS block pointer standing in for his (anm heap idx, arc no) identity.
     *                  Call AFTER the animation is in its pack: i_slot is the under slot whose
     *                  frame controller drives the notes, and a refused set is detected from the
     *                  pack rather than trusted.
     * resetBasSource() — daAlink_c::resetBasAnime.
     * soundFramework() — per sim tick, after groundCheck()/setRoomInfo(): framework + updateAnime,
     *                    in daAlink_c's order (d_a_alink.cpp:18514-18524).
     * initSound()     — last thing create() does before mCreateComplete.
     * deleteSound()   — FIRST thing the destructor does: no handle may outlive the BCK buffers its
     *                   sounds point into. Idempotent; ~Z2Creature repeats it harmlessly. */
    void setBasSource(const daRemotePlayer_anm_c& i_anm, int i_slot);
    void resetBasSource();
    void soundFramework();
    void initSound();
    void deleteSound();

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
    /// 26 runs setFootMatrix; 27 and 29 apply each leg's hip angle to the joint that follows it
    /// (daAlink_c::jointControll's hip-follow arms). The torso and neck arms are
    /// setupJointControl()'s.
    void setupFootIk();
    /// Worklist 14. Hook the body-model joint callback onto joints 1, 2 and 4 — daAlink_c's
    /// jointControll arms for the torso lean/twist and the neck. Once, at createHeap time.
    void setupJointControl();
    /// The joint 1 / 2 / 4 arms themselves, from bodyModelCallBack, i.e. INSIDE the body's calc.
    void jointAimControl(int i_jointNo);
    /// Joint 4's arm: jointControll's quaternion tail (d_a_alink.cpp:2260-2336) against mpOldFrame.
    void neckJointControl();
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
    /// Advance the puppet's own sword material animations; daAlink_c's setItemMatrix tail.
    void playEquipAnm();
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

    /* Poll this puppet's private archive mounts and report whether it is safe to run the
     * destructor. Public only because the profile's file-static Delete method has to reach it;
     * nothing else should call it.
     *
     * ★ This exists because ~dRes_info_c does NOT cancel an in-flight mount. With a DVD command
     * still queued or executing it takes its FIRST branch and calls mDMCommand->destroy(), which is
     * JKR_DELETE on a node the DVD thread still owns (d_resorce.cpp:37-51); ~mDoDvdThd_command_c
     * only prints "a catastrophe will surely occur" and there is no abort API — the command exposes
     * sync() and destroy() and nothing else (m_Do_dvd_thread.h:19-29, m_Do_dvd_thread.cpp:114-118).
     * A puppet mounts FOUR archives at once, and a create request cancelled by a room teardown
     * reaches the destructor with all four potentially still in flight. So the Delete method holds
     * the destructor off, exactly as daAlink_Delete holds his off while a clothes or shield change
     * is loading (d_a_alink.cpp:19906-19919) — but with a tick cap he does not need, because his
     * wait is bounded by a timer and this one is bounded by a DVD thread. */
    bool readyForDelete();

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
    /// How many "the sender's hands overrode the puppet's own pick" lines have been printed
    /// (capped, on change), and the last (fallback, wire) pair printed. Zeroed by fopAcM_ct. See
    /// setDrawHand().
    u16 mLoggedWireHands;
    u32 mLastWireHandState;
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
    /* How many times the Delete method has already refused to destruct because a private mount was
     * still in flight, and the latch for the one-shot report that it happened at all. See
     * readyForDelete(); zero at spawn via fpcBs_Create's sBs_ClearArea of the whole process
     * (f_pc_base.cpp:158), which is what makes them safe to read on a puppet whose create() was
     * cancelled before it ever ran. */
    u16 mDeleteWaitTicks;
    bool mLoggedDeleteWait;
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
     * ~dRes_info_c unmounts and frees everything when the actor dies.
     *
     * ★ There is EXACTLY ONE teardown ordering to get wrong, and this comment used to claim there
     * was none. ~dRes_info_c is only a clean unmount for a mount that has FINISHED: while the DVD
     * command is still live it takes the other branch and destroys the command instead, without
     * cancelling it and without unmounting anything. The actor must therefore not be destructed
     * while this is loading — see readyForDelete(), which is what enforces it. */
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
    /* The wire's locomotion phase (setNetworkLocPhase), this tick's and last tick's. Last tick's is
     * kept only to recognise a STARVED buffer, which hands back the same sample twice; setDoubleAnm
     * then lets the stride run free instead of slewing it toward a frozen target. Zero-initialised
     * by fopAcM_ct like the rest of the actor, so the first pose arrives with mNetLocPhaseValid
     * false until the network layer says otherwise. */
    u8 mNetLocPhase;
    u8 mNetLocPhasePrev;
    bool mNetLocPhaseValid;
    /// Capped count of the phase-lock snaps logged so far, so a session's log shows them without
    /// flooding.
    u16 mLocPhaseSnapLogCount;
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

    /* The standing pivot: setBlendMoveAnime's step-turn branch (d_a_alink.cpp:7706-7745) puts one
     * of these in slot 1 over the wait while Link turns on the spot, and slot 1 then owns the
     * notes, so these are where his turn-in-place footsteps come from. Indexed by
     * anm - ANM_STEP_TURN: STEPL, STEPR (misnamed ANM_SMALL_GUARD), and the two tired rows, which
     * share the plain rows' under objects (never both in the packs) and one WAITD upper of their
     * own (slot 0 holds mTiredWaitAnm's at the same time). A NULL mpUnder means not resident, and
     * the puppet then stands through the turn as before. */
    daRemotePlayer_anm_c mStepTurnAnm[4];
    /// shape_angle.y as of last tick's selectAnimation(): the puppet's mPrevAngleY.
    s16 mStepTurnPrevYaw;
    /// Whether mStepTurnPrevYaw holds a real sample yet. False at spawn via fopAcM_ct's zeroing.
    bool mStepTurnYawValid;
    /// 1 + the mStepTurnAnm index played last tick, or 0 for none. A change is a change of loaded
    /// pair, which setDoubleAnime cross-fades.
    u8 mStepTurnPrev;

    /* --- D3b: the attention (Z-target / lock-on) set. Kept in one block on purpose. ---------
     *
     * Indexed by daAlink_c's own animation id, 0x00 (ANM_ATN_RETURN_FROM_WALK) through 0x11
     * (ANM_ATN_WAIT_RIGHT). Two rows are not attention rows but iron-boots ones and live here
     * because the ids do: 0x0E (ANM_WALK_HEAVY) is the booted gait selectAnimation() plays under
     * mNetHeavyGait, and 0x0F (ANM_ATN_BACK_WALK_HEAVY) is kPlayerAtnBackHeavy's walk and run.
     * Every entry is OPTIONAL, on the terms the alert idles are: selectAtnAnimation() falls back to
     * the ordinary gait for any kind whose pair is missing, and selectAnimation() to the plain walk
     * if 0x0E is.
     *
     * Each row is loaded as its own object, including the two stances' shared upper half AT
     * (0x0E). A J3DAnmTransform holds its own frame, so two rows sharing one object would have two
     * controllers stepping it at once. That costs one extra small BCK and makes the hazard
     * impossible rather than "safe because only one is in the packs". */
    daRemotePlayer_anm_c mAtnAnm[0x12];
    /* The two stances with a sword drawn. getMainBckData's sword table names {ATL, ATL} and
     * {ATR, ATR} for 0x10/0x11 (d_a_alink.cpp:256-262). These share the plain row's under object
     * and hold no second resource; see load_gait_anm_sword. */
    daRemotePlayer_anm_c mAtnWaitSwordAnm[2];
    /// The sender's PlayerAtnKind, whole. Zero (none) at spawn via fopAcM_ct's zeroing.
    u8 mNetAtnKind;
    /* The shield-guard (m_mainBckShield) variant of each row 0x00-0x13, indexed by daAlink_c's
     * animation id. A NULL mpUnder means "not resident", and guardOr() then plays the plain row.
     * Only two resources are new, ATLS and ATRS; every other half is the plain row's own object.
     * See loadGuardAnimations(). */
    daRemotePlayer_anm_c mGuardAnm[0x14];
    /// kPlayerStateUpperGuard as received. False at spawn via fopAcM_ct's zeroing.
    bool mNetUpperGuard;
    /// Last ordinary-gait tick's guard state, so a guard flip in the gait cross-fades as
    /// daAlink_c's setDoubleAnime does when the loaded pair changes.
    bool mGaitPrevGuard;
    /// The same for the heavy gait: boots on or off mid-walk swaps the walk slot's resource
    /// (WALKS <-> WALKH), which setDoubleAnime cross-fades rather than snapping.
    bool mGaitPrevHeavy;
    /// Last attention tick's guard state, for the change log.
    bool mAtnPrevGuard;
    /// Whether last tick's animation came from selectAtnAnimation(). Entering or leaving the set
    /// is a change of pair, which daAlink_c cross-fades with mBasicInterpolation.
    bool mAtnActive;
    /// Set when the set is left, consumed by the ordinary gait's morf on the next blend.
    bool mAtnExitMorf;
    /// Last tick's attention pair, as the animation objects themselves. A change of either one is
    /// a change of loaded resource, the condition daAlink_c morfs on. That includes the stance's
    /// sword variant swapping in, which the animation ids alone can't see.
    const daRemotePlayer_anm_c* mpAtnPrevA;
    const daRemotePlayer_anm_c* mpAtnPrevB;
    /// The kind last played, for the change log both ends' logs are compared on.
    u8 mAtnPrevKind;

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
     * and frees each of them when the actor dies.
     *
     * ★ And subject to exactly the same one ordering rule, three more times over. These three are
     * what turned a one-command hazard into a four-command one: they are started on the SAME tick
     * as the outfit mount, so a puppet deleted mid-spawn has up to four commands in flight at once.
     * readyForDelete() waits for all four. */
    dRes_info_c mShieldRes[3];

    /* This puppet's PRIVATE mount of "Alink", for the ordon and master swords and both sheaths.
     * Until M3 those four were the local player's own J3DModelData, and everything daAlink_c writes
     * onto model data leaked onto the puppet: changeWarpMaterial's dissolve, the blade shape he
     * hides on a sheathe, and the material animators below (whose frame was HIS). A private copy
     * has one writer. Same teardown rule as mOwnRes and mShieldRes: readyForDelete() waits for it.
     */
    dRes_info_c mAlinkRes;

    /* The sword material animations daAlink_c enters on his own copies (d_a_alink.cpp:4583-4599):
     * the master sword's shimmer BTK + BRK, played every frame from setItemMatrix (:6118-6119), and
     * the ordon sword's spin-charge BTK, which sits at frame 0 outside a spin. Out of mAlinkRes, so
     * each puppet owns its anm objects and their frames — never Link's (see the memory on J3D anms
     * holding their own frame). Any may be NULL; that costs the shimmer, not the sword. */
    J3DAnmTextureSRTKey* mpOrdonSwordBtk;
    J3DAnmTextureSRTKey* mpMasterSwordBtk;
    J3DAnmTevRegKey* mpMasterSwordBrk;

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
    /// kPlayerGearHeavyBoots as received. False at spawn via fopAcM_ct's zeroing. See
    /// setNetworkHeavyBoots().
    bool mNetHeavyBoots;
    /// kPlayerGearHeavyGait as received. False at spawn via fopAcM_ct's zeroing. See
    /// setNetworkHeavyGait().
    bool mNetHeavyGait;
    /// Latches the one-shot "the heavy gait is being drawn" line; see selectAnimation().
    bool mLoggedHeavyGait;
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
    /// Latches the one-shot "drew nothing because the local player is gone" line. See draw(): that
    /// state is a stage transition in progress, it lasts a handful of frames, and without the latch
    /// it would fill the log on every room change for the rest of the session.
    bool mLoggedNoLocalPlayer;
    /// Latches the one-shot "the idle fidget started, and at which frame" line. Separate from
    /// mLoggedIdleKind because the fidget no longer goes through the blend at all — it is a single
    /// animation taken before the bands — and the frame it starts on is the whole measurement: 0.0
    /// is daAlink_c's own entry, anything else is the phase carry-over that froze it.
    bool mLoggedServiceWait;
    /// Latches the one-shot "equipment is being drawn, and here is what" line; see drawEquip().
    bool mLoggedEquip;

    /// And the same for the shield, which needs its OWN latch rather than sharing mLoggedEquip: the
    /// two halves of the equipment byte are drawn independently, so a session in which the sword
    /// line appears and the shield line does not is a real and interesting state. Zero at spawn via
    /// fopAcM_ct's zeroing of the actor, like every other latch here.
    bool mLoggedShield;

    /* --- Sound (slice 1) ------------------------------------------------------------------- */

    /* ★ A plain Z2Creature, NEVER a Z2CreatureLink. Z2CreatureLink's constructor takes over the
     * global Z2GetLink() pointer and its init() moves the listener's ears onto its position
     * (Z2LinkMgr.cpp:50-54, :96) — a second one would steal both from the local player. This has
     * no globals; Horse and Midna drive their BCK notes through exactly this class.
     *
     * Constructed by fopAcM_ct's placement new; init()'d only on the last lines of create(), so a
     * puppet whose create failed never owns a sound handle. Handles come from the global
     * Z2SoundHandlePool allocator, not the solid heap. */
    /* The starter mSound's objects start their notes through (installed in initSound()). Declared
     * BEFORE mSound so it is destroyed after it: ~Z2Creature stops the handles while the starter
     * they were started through still exists. */
    daRemotePlayer_soundStarter_c mSoundStarter;
    Z2Creature mSound;
    /// The BAS block currently driving mSound (daAlink_c field_0x2d80), and the frame controller
    /// that drives it (field_0x2d7c). mBasCtrl NULL means "no notes": nothing calls updateAnime.
    const void* mBasData;
    daPy_frameCtrl_c* mBasCtrl;
    /// The rate the BAS was started at (field_0x33d4). A sign flip forces a restart, as for Link.
    f32 mBasRate;
    /// The room's reverb, daAlink_c's mVoiceReverbIntensity, refreshed in setRoomInfo().
    s8 mReverb;
    /// mSound.init() has run. False before create() completes and after the destructor starts.
    bool mSoundReady;
    /// Latches the one-shot "a BAS is actually driving the puppet's sound" line.
    bool mLoggedSound;

    /* Hang 4's guard. The sim tick this puppet last ENTERED the draw list on, and whether it ever
     * has. A J3DModel may be entered once per pass; a second entry builds a self-referential shape
     * chain and the renderer never terminates. See draw() for the measurement that pinned it. */
    u64 mDrawnSimTick;
    bool mHasDrawnSimTick;
    /// Latches the one-shot "a second entry-draw was skipped" line. Its ABSENCE from a session log
    /// is meaningful: it means the double draw did not happen in that session, not that the guard
    /// is broken.
    bool mLoggedDoubleDraw;

    /* --- Worklist 14: head turn and torso lean ------------------------------------------------
     *
     * The sender's jointControll answers, whole, as setNetworkJointAim() received them. Constant
     * across a calc: they are written before execute(), and only the joint callbacks read them.
     * Zero at spawn via fopAcM_ct, which is "no turn" — every arm is a no-op at zero. */
    s16 mNetNeckX;
    s16 mNetNeckY;
    s16 mNetBodyX;
    s16 mNetBodyTwist;
    s16 mNetBodyZ;
    /* How far joint 4's arm turned the head's forward axis on the last calc, in yaw and pitch.
     * MEASURED off the joint matrix before and after the rotation, not computed from the wire
     * values. setEyeMove() subtracts it, so the eyes aim relative to the head the neck is now
     * turning instead of double-counting the turn. Zero whenever the arm did nothing. */
    s16 mNeckYawDelta;
    s16 mNeckPitchDelta;
    /// Callback invocations per arm (joint 1, 2, 4). See getJointAimFires().
    u32 mJointAimFires[3];
    /// One-shot "this arm applied a non-zero rotation" latches, so "fires with zeros" and "fires
    /// and turns" are distinguishable in the log as well as in the trace.
    bool mLoggedJointAim[3];
    /* --- Action animation override (wire block 2). See setupActionAnimation() in the .cpp.
     *
     * The six heaps are laid out exactly as daAlink_c::initModel lays out his
     * (d_a_alink.cpp:4307, :4555-4563): one 0x10800 buffer on the actor heap, under[i] at
     * +i*0x2C00, upper[0] at +0x8400 and upper[i] at +0x8400+i*0x2C00. Only slot 0 is played in
     * slice 1; 1 and 2 exist so slice 2 (double anime) inherits the same geometry.
     *
     * ★ daPy_anmHeap_c's constructor never initialises mAnimeHeap/mBuffer, but its destructor
     * destroys a non-NULL mAnimeHeap. These members are only safe because the actor's memory is
     * zeroed before construction (fpcBs_Create -> sBs_ClearArea); setupActionAnimation() NULLs
     * them explicitly as well before anything can fail. */
    daPy_anmHeap_c mActUnderHeap[3];
    daPy_anmHeap_c mActUpperHeap[3];
    /// The animations currently loaded in mActUnderHeap[0] / mActUpperHeap[0]. They point INTO the
    /// heaps' buffers, so they are invalid the moment that heap reloads.
    J3DAnmTransform* mpActUnderBck;
    J3DAnmTransform* mpActUpperBck;
    bool mActHeapReady;
    bool mActActive;
    /// The ids currently installed in the ratio packs (0xFFFF none).
    u16 mActShownUnder;
    u16 mActShownUpper;
    f32 mActAppliedMorf;
    /// A pair that failed to load, so it is not re-read from ARAM every tick. Cleared by a change.
    u16 mActFailedUnder;
    u16 mActFailedUpper;
    // The wire's values, as pushed by setNetworkAction().
    u16 mNetActUnder;
    u16 mNetActUpper;
    f32 mNetActFrame;
    f32 mNetActUpperFrame;
    f32 mNetActRate;
    f32 mNetActMorf;
    u8 mNetActHand[2];
    cXyz mNetActRoot;
    /// The previous tick's wire under id and frame, for spotting a held (starved) sample.
    u16 mActPrevNetUnder;
    f32 mActPrevNetFrame;
    bool mActStarved;
    u16 mActLogCount;
    u16 mActFailLogCount;
    bool mLoggedActLive;
    bool mLoggedActRoot;
    /* The override BCK's sound block, COPIED out — daAlink_c's field_0x2d78 (d_a_alink.cpp:4312,
     * :14025-14030). Unlike the locomotion BCKs, the override's buffer is reloaded on every change,
     * so its notes cannot be used in place. NULL if the copy buffer did not allocate (the override
     * then plays silent). mActBasValid: the copy holds the BCK in mActUnderHeap[0] and it has
     * notes. */
    u8* mpActBas;
    bool mActBasValid;
    bool mLoggedActSound;

    /* --- Block (4): action slot 1 / swim. See selectActionSlot1() in the .cpp. Slot 1 loads into
     * mActUnderHeap[1] / mActUpperHeap[1], daAlink_c's own slot-1 geometry. */
    /// The animations loaded in mActUnderHeap[1] / mActUpperHeap[1]; invalid once that heap or
    /// the slot-0 heaps (which can overlap them) reload.
    J3DAnmTransform* mpActUnder1Bck;
    J3DAnmTransform* mpActUpper1Bck;
    /// The slot-1 ids installed in the packs (0xFFFF none), and a pair that failed to load.
    u16 mActShownUnder1;
    u16 mActShownUpper1;
    u16 mActFailedUnder1;
    u16 mActFailedUpper1;
    u16 mNetActUnder1;
    u16 mNetActUpper1;
    f32 mNetActPhase1;
    f32 mNetActRatio1;
    s16 mNetActRootRotX;
    s16 mNetActRootRotZ;
    s16 mNetActYawOff;
    s16 mNetActPitch;
    /// What applyActionRoot() and setMatrix() last applied, for the trace.
    s16 mActAppliedRootRotX;
    s16 mActAppliedRootRotZ;
    s16 mActAppliedYawOff;
    s16 mActAppliedPitch;
    bool mLoggedActSlot1;
    bool mLoggedActRootRot;

    /* --- block (3): held item. See d_a_remote_player_held.inc.
     *
     * Every handheld model is PRELOADED at createHeap (Stuart, 2026-11-05: no on-demand loading),
     * each from the archive and with the flags daAlink_c uses, and each optional: a model that did
     * not load is NULL and costs that item, never the puppet. Indexed by PlayerHeldItem; [0] (none)
     * is always NULL. The double clawshot reuses the single's two models as its primary pair. */
    void setupHeldItems();
    void setHeldItemMatrix();
    void drawHeldItems();
    void addHeldItemShadow();
    void applyUpper2Overlay();
    void clearUpper2Overlay(f32 i_morf);
    J3DModel* mpHeldModel[10];
    /// The second clawshot and the two claw tips (daAlink_c's field_0x0710 / mpHookTipModel /
    /// field_0x0714).
    J3DModel* mpHeldClaw2Model;
    J3DModel* mpHeldClawTip[2];
    /// mpKanteraModel: from this puppet's outfit archive, as he takes his from mArcName.
    J3DModel* mpHeldLanternModel;
    /// The bottle's contents colour (BRK) and stopper pattern (BTP), and the Dominion Rod's glow
    /// (BRK). This puppet's own anm objects, entered on its own model data.
    J3DAnmTevRegKey* mpHeldBottleBrk;
    J3DAnmTexPattern* mpHeldBottleBtp;
    J3DAnmTevRegKey* mpHeldCopyRodBrk;
    // The wire's values, as pushed by setNetworkHeldItem().
    u8 mNetHeldKind;
    u8 mNetHeldFlags;
    u8 mNetHeldMat;
    u16 mNetUpper2Idx;
    f32 mNetUpper2Frame;
    f32 mNetUpper2Ratio;
    f32 mNetUpper2Morf;
    // What was drawn (trace).
    u8 mHeldDrawnKind;
    u8 mHeldDrawnFlags;
    u8 mHeldDrawnMat;
    /// The UPPER_2 overlay currently in mAnmPackUpper[2] (0xFFFF none), its animation object (it
    /// points into mActUpperHeap[2]'s buffer), and a load that failed (not retried until it
    /// changes).
    u16 mUpper2Shown;
    J3DAnmTransform* mpUpper2Bck;
    u16 mUpper2Failed;
    u8 mUpper2AppliedMorf;
    u16 mHeldLogCount;
    bool mLoggedHeldMissing[10];
    bool mLoggedHeldDraw[10];
    bool mLoggedLantern;

    // --- v19: friendly fire (coordinator) ---
    void initFriendlyFire();
    void registerFriendlyFire();
    dCcD_Stts mFfStts;
    dCcD_Cps mFfCps[3];
    cXyz mFfPoints[3];
    f32 mFfRadius;
    u8 mFfMask;
    u8 mFfSpl;
    /// Set once a swing has hit, so one slash costs one hit; cleared when the swing ends.
    bool mFfSwingHit;
    u16 mFfHitLogs;
    // --- iron boots (agent C) ---
    /* daAlink_c's mpLinkBootModels and field_0x06e0, driven by mNetHeavyBoots. See the block
     * comment above setupHeavyBootsModel() in d_a_remote_player.cpp. All zeroed by fopAcM_ct. */
    void setupHeavyBootsModel();
    void setHeavyBootsMatrix();
    void drawHeavyBoots();
    void addHeavyBootsShadow();
    /// One per foot, both from this puppet's outfit archive's al_bootsH.bmd. Both or neither.
    J3DModel* mpHeavyBootsModel[2];
    /// The body shape the outfit's own boots are (field_0x06e0), or NULL when the outfit has none.
    J3DShape* mpHeavyBootsBodyShape;
    /// This tick's boots state as latched by setHeavyBootsMatrix(); draw reads this, not the wire.
    bool mHeavyBootsOn;
    /// What mpHeavyBootsBodyShape's hide flag currently says, so it is only written on a change.
    bool mHeavyBootsBodyHidden;
    bool mLoggedHeavyBootsDraw;
    u16 mHeavyBootsLogCount;
    // --- iron ball (agent B) ---
    /* The ball and chain, and the carry gait. See d_a_remote_player_ironball.inc.
     *
     * The ball model rides in mpHeldModel[kPlayerHeldIronBall] so the held-item code poses, draws
     * and shadows it like any other held model; everything here is what daAlink_c keeps for it
     * beyond the model (setIronBallModel, d_a_alink_ironball.inc:90-170). */
public:
    /// v19 block B and gear bit 2, decoded by the bridge. i_ballRel is mIronBallChainPos[0]
    /// relative to the sender's position; i_state is the wire's ironBallState byte.
    void setNetworkIronBall(bool i_gait, const cXyz& i_ballRel, s16 i_angX, s16 i_angY, u8 i_links,
        u8 i_state);
    /// kPlayerGearIronBallGait as received.
    bool getNetIronBallGait() const { return mNetIronBallGait; }
    /// daAlink_c::hsChainShape_c::draw's iron-ball half, from the chain packet at paint time.
    void drawIronBallChain();

private:
    J3DModel* setupIronBall();
    void setIronBallMatrix(J3DModel* i_ball);
    void setIronBallChainPos();
    void drawIronBall();
    /// The ball is in his hands (states 0 and 8): it casts into the body's real shadow then, and
    /// into a shadow of its own otherwise (d_a_alink.cpp:19310-19321).
    bool ironBallInHands() const;
    /// ANM_IRONBALL_WAIT / ANM_IRONBALL_WALK: setBlendMoveAnime's carry-gait pair
    /// (d_a_alink.cpp:7642-7643).
    daRemotePlayer_anm_c mIronBallGaitAnm[2];
    /// al_hs_kusari, the chain link: model DATA only, as his mpItemModelData
    /// (d_a_alink_ironball.inc:94).
    J3DModelData* mpIronBallChainData;
    /// The chain's immediate packet (his mpHookChain), on the solid heap.
    J3DPacket* mpIronBallChainPacket;
    /* His mIronBallChainPos / mIronBallChainAngle / field_0x3848 (pos, rotation, velocity), all 102
     * of them (d_a_alink_ironball.inc:97-99). [0] is the ball's chain end, [1..mIronBallLinks] the
     * paid-out links, [mIronBallLinks + 1] his hand, and the rest are parked on the hand. */
    cXyz mIronBallChainPos[102];
    csXyz mIronBallChainAngle[102];
    cXyz mIronBallChainVel[102];
    /// His mHookshotTopPos (the chain's hand end), mHeldItemRootPos and field_0x316c.
    cXyz mIronBallHandTop;
    cXyz mIronBallHandRoot;
    csXyz mIronBallHandRootAngle;
    /// His mIronBallCenterPos and mIronBallBgChkPos.
    cXyz mIronBallCenter;
    cXyz mIronBallBgChkPos;
    /// His mItemMode while the ball is out, and the state the chain was last solved for.
    s16 mIronBallLinks;
    u8 mIronBallState;
    /// The chain has been initialised and solved on the previous sim tick (g_Counter.mCounter0).
    bool mIronBallChainLive;
    u32 mIronBallSolvedTick;
    /// His field_0x32d4: the ball's own real-shadow slot while it is out of his hands.
    u32 mIronBallShadowKey;
    /// Frame interpolation of the chain, as AlinkInterp's ib_pos/ib_angle/ib_hand
    /// (d_a_alink.cpp:67-72).
    dusk::interp::Samples<cXyz> mIronBallPosSamples;
    dusk::interp::Samples<csXyz> mIronBallAngleSamples;
    dusk::interp::Samples<cXyz> mIronBallHandSamples;
    // The wire's values, as pushed by setNetworkIronBall().
    bool mNetIronBallGait;
    cXyz mNetIronBallRel;
    s16 mNetIronBallAngX;
    s16 mNetIronBallAngY;
    u8 mNetIronBallLinks;
    u8 mNetIronBallState;
    bool mGaitPrevIronBall;
    bool mLoggedIronBallGait;
    bool mLoggedIronBallDraw;
    u16 mIronBallLogCount;
    // --- held items: bottle/bow (agent A) ---
    /* The bow / slingshot model's own animation and the nocked arrow (protocol v19 block A). See
     * d_a_remote_player_held.inc.
     *
     * mpHeldItemBckAnm: every BCK daAlink_c can put in his mItemBck for a bow or slingshot,
     * PRELOADED (indexed by PlayerHeldItemBck; [0] NULL), each read with his mAnmHeap9 buffer size.
     * mHeldItemBck: his mDoExt_bckAnm, built once on the first BCK that loaded and switched with
     * changeBckOnly — the ONLY driver of those animation objects' frames.
     * mpHeldArrowModel: AL_ARROW / AL_ARROWB from the private Alink mount, built as
     * daArrow_c::createHeap builds them. mHeldArrowTevStr: the arrow's own type-0 light, as the
     * arrow actor keeps its own. */
public:
    /// Pushed every tick right after setNetworkHeldItem(): PlayerState::heldBck / heldBckFrame.
    void setNetworkHeldItemBck(u8 i_bck, u8 i_frame);
    /// What was drawn (trace / logs): the item BCK id installed last tick (0 none), and the arrow
    /// model entered last pass (0 normal, 1 bomb, 0xFF none).
    u8 getHeldItemBckShown() const { return mHeldItemBckShown; }
    /// That BCK's own frame / frameMax, x255 (0 with none installed).
    u8 getHeldItemBckDrawnFrame() const;
    u8 getHeldArrowDrawn() const { return mHeldArrowDrawn; }

private:
    void setHeldItemBck(J3DModel* i_held, u8 i_kind);
    void setHeldArrowMatrix();
    void drawHeldArrow();
    J3DAnmTransform* mpHeldItemBckAnm[14];
    mDoExt_bckAnm mHeldItemBck;
    bool mHeldItemBckReady;
    u8 mHeldItemBckShown;
    J3DModel* mpHeldArrowModel[2];
    dKy_tevstr_c mHeldArrowTevStr;
    /// The bomb arrow's fuse, daArrow_c::field_0x950: starts at getBombExplodeTime() when it is
    /// nocked and counts down one per tick (d_a_arrow.cpp:1038-1041); drives its material-1 pulse.
    s16 mHeldArrowFuse;
    /// A BOMB arrow was nocked last tick (the fuse only runs on one).
    bool mHeldArrowWasNocked;
    // The wire's v19 block A bytes, as pushed by setNetworkHeldItemBck().
    u8 mNetHeldBck;
    u8 mNetHeldBckFrame;
    // What was drawn (log / trace): the arrow model entered last pass, 0xFF none.
    u8 mHeldArrowDrawn;
    /// The validated wire BCK id last tick, so a change is logged once even if it cannot be shown.
    u8 mHeldItemBckWanted;
    bool mLoggedArrowMissing;
    u16 mHeldBckLogCount;
    // --- damage flash + lantern fx (agent D) ---
    /* The damage / freeze / ice body tint, the lantern's glow model and flame, and the puppet's
     * particle emitters. See d_a_remote_player_fx.inc. All zeroed by fopAcM_ct, and 0 is "nothing
     * live" for every member below (a level-emitter handle of 0 names no emitter). */
public:
    /// v20 effects block, decoded by the bridge: i_tint is PlayerDmgTint (dusk/player_fx.hpp),
    /// i_flash the wire's dmgFlash, i_lanternLit kPlayerFxLanternLit.
    void setNetworkDamageLantern(u8 i_tint, u8 i_flash, bool i_lanternLit);
    u8 getNetDmgTint() const { return mNetDmgTint; }
    bool getNetLanternLit() const { return mNetLanternLit; }
    /// The lantern's joint-1 callback body (daAlink_c::kandelaarModelCallBack). Public for the
    /// static trampoline only.
    int lanternModelCallBack();

    /* ★ PUPPET PARTICLE EMITTERS — the reusable part. Use these rather than dComIfGp_particle_set
     * directly, so every emitter a puppet owns is torn down with it.
     *
     * They are dPa_control_c "level" emitters, the kind daAlink_c keeps in field_0x31c4 and
     * friends: one handle per slot, and an emitter lives exactly as long as it is refreshed.
     *
     *   - setPuppetEmitter(slot, name, &pos, &rot, &scale): call it EVERY sim tick, from execute(),
     *     for as long as the effect should exist. The first call creates the emitter, every later
     *     one moves it (dPa_control_c::set, d_particle.cpp:1733-1765) and un-hides it. rot and
     *     scale may be NULL. Returns the emitter, or NULL if none could be made. Lit by the
     *     puppet's own tevStr, as his are by his.
     *   - setPuppetEmitterMtx(slot, name, mtx): the same, placed and rotated by a joint matrix
     *     (setGlobalRTMatrix) — for effects that ride a joint, like a sword's.
     *   - stopPuppetEmitter(slot): hides it now and forgets the handle. A level emitter that is not
     *     refreshed is cut by the particle manager on its next pass (level_c::execute,
     *     d_particle.cpp:971-984), so stopping IS deleting; no other call is needed.
     *   - deletePuppetEmitters(): every slot, from the destructor. Nothing else has to call it.
     *
     * Slots are owned per feature, below. Name a new one rather than sharing: two features on one
     * slot would each re-create the other's emitter every tick. */
    enum {
        /// ZI_J_KANTERA_FIRE, the lantern flame (agent D).
        PUPPET_FX_LANTERN_FIRE = 0,
        /// First of the slots reserved for the sword trail / spin / charge effects (agent F):
        /// PUPPET_FX_SWORD_FIRST .. PUPPET_FX_NUM - 1.
        PUPPET_FX_SWORD_FIRST = 1,
        PUPPET_FX_NUM = 8,
    };
    JPABaseEmitter* setPuppetEmitter(int i_slot, u16 i_name, const cXyz* i_pos,
        const csXyz* i_rot = NULL, const cXyz* i_scale = NULL);
    JPABaseEmitter* setPuppetEmitterMtx(int i_slot, u16 i_name, CMtxP i_mtx);
    void stopPuppetEmitter(int i_slot);
    JPABaseEmitter* getPuppetEmitter(int i_slot) const;
    void deletePuppetEmitters();

private:
    /// draw(): his tint ladder onto tevStr (and the outfit's reg-1 materials), right after
    /// settingTevStruct; and his TevKColor reset, right before the equipment.
    void applyDamageTint();
    void clearDamageKColor();
    /// setWaterDropColor's material list for this outfit, reg 1, for the freeze and ice tints;
    /// restores the materials' own values when the tint ends.
    void setDamageReg1(bool i_on);
    /// The glow model, its BTK and the swing callback. From setupHeldItems, after the lantern.
    void setupLanternFx();
    /// execute(), after setMatrix: the glow's matrix and scale, and the flame emitter.
    void updateLanternFx();
    /// drawHeldItems(), right after the lantern: preKandelaarDraw, then the glow.
    void drawLanternGlow();
    // The wire's values, as pushed by setNetworkDamageLantern().
    u8 mNetDmgTint;
    u8 mNetDmgFlash;
    bool mNetLanternLit;
    /// Reg-1 tint state: written now, and the materials' own values saved before the first write.
    bool mDmgReg1On;
    bool mDmgReg1Saved;
    GXColorS10 mDmgReg1Orig[9];
    bool mLoggedDmgTint[4];
    u16 mDmgTintLogCount;
    /// ef_ktGlow.bmd / .btk from this puppet's outfit archive (his mpKanteraGlowModel /
    /// mpKanteraGlowBtk, d_a_alink_wolf.inc:407-413).
    J3DModel* mpLanternGlowModel;
    J3DAnmTextureSRTKey* mpLanternGlowBtk;
    /// His mKandelaarFlamePos, field_0x3624 (last tick's) and field_0x3618 (the swing velocity).
    cXyz mLanternFlamePos;
    cXyz mLanternFlamePrev;
    cXyz mLanternFlameVel;
    /// The swing has been seeded since the lantern came out (setKandelaarModel's reset).
    bool mLanternSwingLive;
    bool mLanternSwingHooked;
    /// His field_0x3448 (glow scale), field_0x32c8 (peeked depth) and field_0x344c (the flame's
    /// own depth). The peek is written by dDlst_peekZ_c at the start of the next frame.
    f32 mLanternGlowScale;
    u32 mLanternGlowPeek;
    f32 mLanternGlowZ;
    /// The glow was posed this tick, so draw may enter it.
    bool mLanternGlowPosed;
    bool mLoggedLanternGlow;
    u16 mLanternFxLogCount;
    u32 mFxEmitter[PUPPET_FX_NUM];
    bool mLoggedFxFail[PUPPET_FX_NUM];
    u16 mFxLogCount;
    // --- sword trail + spin/charge fx (agent F) ---
    /* daAlink_c's m_swordBlur (the trail), setCutTurnEffect (the spin attack's particles) and
     * setSwordChargeEffect (the charge glow, plus the ordon sword BTK it reads), rebuilt on the
     * puppet's own sword and joints from protocol v20 fxFlags bits 1-3 and 6-7. See
     * d_a_remote_player_swordfx.inc. All zeroed by fopAcM_ct. */
public:
    /// setCutTurnEffect's four emitter lists (d_a_alink_effect.inc:1034-1061), as decoded by the
    /// bridge from the wire's spin kind.
    enum SwordSpinKind_e {
        SWORD_SPIN_NORMAL_e,
        SWORD_SPIN_LIGHT_e,
        SWORD_SPIN_LARGE_e,
        SWORD_SPIN_WATER_e,
    };
    /// fxFlags bits 1-3 and 6-7, decoded by the bridge every tick (dusk::mp::apply_sword_fx).
    /// i_trail: the sender extended his trail this tick; i_spin: his spin emitted this tick;
    /// i_charge: his charge BTK frame is above 0.
    void setNetworkSwordFx(bool i_trail, bool i_trailDash, bool i_trailReversed, bool i_spin,
        u8 i_spinKind, bool i_charge);

private:
    void setupSwordFx();
    void updateSwordFx();
    void updateSwordTrail(J3DModel* i_sword, bool i_master);
    void updateSwordSpinFx(bool i_particlesOk);
    void updateSwordChargeFx(J3DModel* i_sword, bool i_master, bool i_particlesOk);
    void clearSwordSpinFx();
    void drawSwordFx();
    void deleteSwordFx();
    /// This puppet's own daAlink_blur_c (a J3DPacket subclass, typed as its base here so this
    /// header needs no d_a_alink.h), on the solid heap. NULL if it did not fit or the texture is
    /// missing: no trail, nothing else lost.
    J3DPacket* mpSwordBlurPacket;
    // The wire's bits, as pushed by setNetworkSwordFx().
    bool mNetSwordTrail;
    bool mNetSwordTrailDash;
    bool mNetSwordTrailReversed;
    bool mNetSwordSpin;
    u8 mNetSwordSpinKind;
    bool mNetSwordCharge;
    /// What was reproduced last tick: the trail extended, spin emitters set, charge running, and
    /// the charge reached its full-glow frame (14).
    bool mSwordTrailOn;
    bool mSwordSpinOn;
    bool mSwordChargeOn;
    bool mSwordChargeFull;
    /// Last tick's sword base matrix (valid only if a sword was in hand), so a trail that starts
    /// this tick can be initialised where the sender's initBlur was — one tick earlier.
    Mtx mSwordFxPrevMtx;
    bool mSwordFxPrevMtxValid;
    /// Last tick's position and facing: decSwordBlur's old.pos and mPrevAngleY.
    cXyz mSwordFxPrevPos;
    s16 mSwordFxPrevAngleY;
    bool mSwordFxPrevPosValid;
    /// His field_0x3204[6] (spin) and field_0x321c / 0x3220 / 0x3224 (charge A/B/C) level ids.
    u32 mSwordSpinEmitter[6];
    u32 mSwordChargeEmitter[3];
    /// g_Counter ticks each effect turned on, for the OFF lines' durations.
    u32 mSwordTrailOnTick;
    u32 mSwordSpinOnTick;
    u32 mSwordChargeOnTick;
    u16 mSwordFxLogCount;
    bool mLoggedSwordTrailDraw;
    // --- clawshot chain + tip (agent E) ---
    /* The clawshot's fired tips, their chains and the tip-open BCK (protocol v20 clawshot block).
     * See d_a_remote_player_hookshot.inc. The guns and tips themselves are the held-item models
     * (mpHeldModel[kPlayerHeldClawshot], mpHeldClaw2Model, mpHeldClawTip), and the guns' HS_SPIN is
     * wire block A's kPlayerHeldBckClawSpin on mHeldItemBck. "Slot" 0/1 is daAlink_c's
     * mHeldItemModel set / field_0x0710 set, not a hand. */
public:
    /// The v20 clawshot block, decoded by the bridge: clawFlags, the two tip-open frames, each
    /// tip's position relative to the sender (zero while on its gun) and its rotation x/y pairs.
    void setNetworkClawshot(u8 i_flags, const u8* i_tipFrame, const cXyz* i_tipRel,
        const s16* i_tipAng);
    /// daAlink_c::hsChainShape_c::draw's clawshot half, from the chain packet at paint time.
    void drawClawshotChain();

private:
    void setupClawshot();
    void setClawshotMatrix(J3DModel* i_held, u8 i_kind);
    void drawClawshot();
    void addClawshotShadow(u8 i_kind);
    /// HS_TIP_OPEN on his mHookTipBck's terms (d_a_alink_hook.inc:448-451); both tips share data.
    J3DAnmTransform* mpClawTipBckAnm;
    mDoExt_bckAnm mClawTipBck;
    bool mClawTipBckReady;
    /// al_hs_kusari model data (the iron ball's when it loaded) and the chain's immediate packet.
    J3DModelData* mpClawChainData;
    J3DPacket* mpClawChainPacket;
    /// Each slot's muzzle (his mHeldItemRootPos / field_0x3810) and tip anchor (mHookshotTopPos /
    /// mIronBallBgChkPos), as posed this tick: the chains' ends.
    cXyz mClawRoot[2];
    cXyz mClawTop[2];
    /// getHookshotStopTime() as received: the slot-0 chain's wobble.
    u8 mClawStopTime;
    /// The kind the four anchors were last posed for, so the chain never draws stale ones.
    u8 mClawPosedKind;
    /// The flags the last pass drew with (shadow gates), and the last logged flags.
    u8 mClawDrawnFlags;
    u8 mClawLoggedFlags;
    /// Frame interpolation of the four anchors, as AlinkInterp's hs_chain (d_a_alink.cpp:64-71).
    dusk::interp::Samples<cXyz> mClawChainSamples;
    // The wire's values, as pushed by setNetworkClawshot().
    u8 mNetClawFlags;
    u8 mNetClawTipFrame[2];
    cXyz mNetClawTipRel[2];
    s16 mNetClawTipAng[4];
    u16 mClawLogCount;
    bool mLoggedClawChain;
    // --- bottle contents (agent H) ---
    /* What is IN the held bottle — daAlink_c's mpHookTipModel for a bottle: the fairy, worm or bee
     * larva — and the bottle's own texture animations, his three BTK slots. Wire: heldBck ids
     * 11-13 + bits 5-7, frame in heldBckFrame (dusk/player_held.hpp). See
     * d_a_remote_player_bottle.inc. Indexed by PlayerBottleContents ([0] unused) and by the
     * file's BTK order (YURE, D_YURE, D_NOMU, NUKU, DASU). */
public:
    /// read_puppet_pose: overwrite the heldBck / heldBckFrame echo with the bottle's BTK, set and
    /// contents as last DRAWN. Leaves both alone unless the bottle was drawn last pass.
    void getBottleDrawnWire(u8& io_bck, u8& io_frame) const;
    /// Sender side (fill_held_item): which of his three slots (field_0x0718 / 071c / 0720, in that
    /// order in i_slots) is ENTERED on i_bottle's materials, as a PlayerHeldItemBck id (0 none),
    /// and that animation object.
    static u8 bottleBtkEntered(
        J3DModel* i_bottle, J3DAnmTextureSRTKey* const* i_slots, J3DAnmTextureSRTKey** o_anm);

private:
    void setupBottleContents();
    void setBottleMatrix(J3DModel* i_bottle, bool i_inLeftHand);
    void resetBottleShown();
    void drawBottleContents();
    J3DModel* mpBottleContentsModel[4];
    J3DAnmTransform* mpBottleContentsBckAnm[4];
    mDoExt_bckAnm mBottleContentsBck[4];
    J3DAnmTextureSRTKey* mpBottleBtk[5];
    /// The BTK this puppet entered last (pointer guard: entry only on a change) and the set
    /// (0 none installed, 1 open, 2 drink, 0xFF no bottle since) whose setBottleModel entries
    /// were replayed.
    J3DAnmTextureSRTKey* mpBottleBtkEntered;
    u8 mBottleBtkSet;
    /// The wire's BTK id|set and contents last tick (change logs), the contents posed this tick,
    /// and what the last pass drew.
    u8 mBottleWantedBtk;
    u8 mBottleWantedContents;
    u8 mBottleContentsPosed;
    u8 mBottleContentsShown;
    u16 mBottleLogCount;

/**
 * protocol v19 block A: the wire id (PlayerHeldItemBck, dusk/player_held.hpp) of the item BCK
 * resource daAlink_c has in mAnmHeap9, for held kind i_kind (PlayerHeldItem). 0 when it is not one
 * the puppet reproduces for that kind. The table lives in d_a_remote_player_held.inc, once, and
 * the puppet maps the id back through it — so the two ends cannot disagree.
 */
u8 daRemotePlayer_heldItemBckToWire(u16 i_resIdx, u8 i_kind);

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
