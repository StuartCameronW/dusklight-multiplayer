#pragma once

#include <cmath>
#include <cstdint>

#include "dusk/player_atn.hpp"
#include "dusk/player_equip.hpp"
#include "dusk/player_idle.hpp"

#include "../net/serializer.hpp"

/**
 * The per-player pose that travels over the wire every sim tick.
 *
 * M1 replicates a COARSE pose deliberately (see 02/04): position, facing, and gait rate. The puppet
 * picks its blend of idle/walk/run from that rate rather than mirroring Link's animation ids,
 * because driving the real daAlink_PROC state machine is M3's job and would sink M1 in engine
 * detail.
 *
 * Engine-free by design so the protocol stays unit-testable headless (layering rule, 04).
 */

namespace dusk::mp {

/// Bit flags in PlayerState::flags.
enum PlayerStateFlags : std::uint8_t {
    /// Link exists and is in the world this tick. Clear during loads/cutscenes, when the puppet
    /// must hide rather than freeze at a stale position.
    kPlayerStateInWorld = 1 << 0,
    /// Reserved for M3 transform sync; sent as 0 for now so the bit is already allocated.
    kPlayerStateWolf = 1 << 1,
    /**
     * The sender is skidding — daAlink_c::PROC_SLIP, the sharp turn you get by flicking the stick
     * back at speed (d_a_alink.cpp:12413-12417 selects it, :16673 plays ANM_SLIP).
     *
     * ★ This is a WIRE bit rather than something the puppet derives from the yaw it already
     * receives, and the reason is not bandwidth — it is that derivation gives the wrong answer, and
     * gives it backwards:
     *
     *   - During the skid itself daAlink_c::procSlip (d_a_alink.cpp:16685-16726) only decelerates
     *     mNormalSpeed. It does NOT touch shape_angle.y until the slide has already STOPPED
     *     (:16688-16691, inside the checkZeroSpeedF branch). So the one state that HAS a distinct
     *     animation has a yaw rate of ZERO, and a yaw-rate detector would never fire on it.
     *   - The state with the largest yaw rate is PROC_MOVE_TURN, where shape_angle chases
     *     current.angle at up to mMaxTurnAngle*2 = 9000 units/tick (:15829-15831) — and
     *     procMoveTurnInit plays setBlendMoveAnime (:15809), i.e. the ORDINARY GAIT. There is no
     *     turn animation there to propagate, so a yaw-rate detector would fire hardest exactly
     *     where playing one is wrong.
     *
     * A local derivation would therefore invert the truth: silent on the skid, loud on the walk.
     * (The usual secondary objections apply too — lerp_state resamples the yaw so the puppet sees
     * the sender's average rate rather than its peak, StateBuffer holds the last pose while starved
     * so the derived rate collapses to zero, and a snap manufactures a one-tick spike — but those
     * are refinements. The inversion above is on its own decisive.)
     *
     * It rides in `flags` rather than as a new field because it is a boolean discrete state, which
     * is what this byte is for: six bits were spare, so the wire cost is ZERO bytes, and lerp_state
     * already takes `flags` whole from the newer sample, so the "never blend discrete state" rule
     * is satisfied with no new interpolation code to get wrong.
     */
    kPlayerStateSharpTurn = 1 << 2,
    /**
     * The sender was STANDING — `checkModeFlg(MODE_IDLE) || checkZeroSpeedF()`, which is the exact
     * predicate `setBlendMoveAnime` branches on (d_a_alink.cpp:7656).
     *
     * ★ This is a wire bit and not a threshold on `moveRate` for one reason: the branch it selects
     * is a DISCONTINUITY, not a taper. On the false side Link's walk weight is remapped to start at
     * mMinWalkRate — 0.7 — so he crosses from "no walk at all" to "seven tenths of a walk" in a
     * single tick (:7759). A receiver testing a threshold of its own therefore does not merely
     * round the answer, it picks the wrong side of a step, and it picks it wrong exactly where the
     * value hovers: at the start and end of every walk. The old receiver-side constant was
     * `speed <= 0.5f`, invented here rather than read from the game, and it was the second of two
     * bugs in a row caused by re-deriving a decision daAlink_c had already made.
     *
     * MODE_IDLE is the other half and cannot be derived at all: it is a mode flag the state machine
     * sets, and it is true in states that still carry speed.
     *
     * Free on the wire — the flags byte had five spare bits — and safe through lerp_state, which
     * takes `flags` whole from the newer sample rather than blending them.
     */
    kPlayerStateZeroSpeed = 1 << 3,
    /**
     * The sender's `checkModeFlg(MODE_IDLE)` on its own — he is at rest, as his state machine sees
     * it, not merely as his speed sees it.
     *
     * Sent alongside kPlayerStateZeroSpeed rather than folded into it because the two are used for
     * different jobs and the OR is only correct for one of them. `setBlendMoveAnime` wants the OR
     * (d_a_alink.cpp:7656); `footBgCheck` (:3883, :3931, :3970) wants MODE_IDLE alone, and it uses
     * it three separate times — to freeze the floor probe, to sink the body onto the lower foot,
     * and to pitch each foot onto its slope. Handing those the OR would run all three for the tick
     * or two at the end of every stop where Link's speed has reached zero but his mode flag has not
     * yet been set, which reads as the body dipping as he halts.
     */
    kPlayerStateModeIdle = 1 << 4,
    /**
     * The sender is in a state where he does no foot IK at all — `d_a_alink.cpp:3872`: not touching
     * the ground, wearing magne boots, sinking into sand, or in any of jumping / climbing /
     * swimming / rope-walking / riding / no-collision.
     *
     * ★ Replicated rather than derived, and this is the case where deriving is most tempting: the
     * plan for this feature originally said the receiver could test its own ground height against a
     * tolerance. That answers a different question. The puppet's position is interpolated between
     * two samples and delivered two ticks late, so a height test tells you where the puppet's
     * REPLAY is, not what the sender was doing — and it cannot distinguish "swimming at floor
     * level" or "riding" from "standing" at any tolerance. One bit gets all of it, exactly, on the
     * sender's own timeline, which is the timeline the puppet is replaying.
     */
    kPlayerStateNoFootIk = 1 << 5,
    /**
     * The sender is somewhere the world's clock is allowed to run — the room's TimePass flag
     * (`dComIfGp_roomControl_getTimePass()`, set from stage data at `d_stage.cpp:1511`), with no
     * time-control tag active and not in the twilight.
     *
     * ★ Deliberately the STAGE-level fact and not the whole of `setDaytime`'s condition. That
     * condition also folds in "an event is running" and "a message box is open"
     * (`d_kankyo.cpp:1538-1545`), and those are transient, local, and nobody else's business: under
     * the AllPlayers rule, one player reading a signpost must not stop the sun for everyone. What
     * this bit answers is "am I standing somewhere the world clock may run", which is the question
     * the rule is actually about.
     *
     * Free on the wire — bits 6 and 7 of the flags byte were spare — and safe through lerp_state,
     * which takes `flags` whole from the newer sample.
     */
    kPlayerStateTimeCanPass = 1 << 6,
    /**
     * `daAlink_c::checkUpperGuardAnime()`, verbatim: the shield is raised in the stance and gait
     * (setShieldGuard's FLG2_UNK_8000000, minus events and the guard-break/reverse/attack procs).
     * While it holds, getMainBckData plays m_mainBckShield for every row below ANM_SWIM_WAIT, and
     * the receiver makes the same substitution (d_a_remote_player.cpp, loadGuardAnimations).
     *
     * The sender's ANSWER, not the inputs to it — lock-on target group, battle stage, shield
     * owned — for the same reason every other bit here is an answer.
     *
     * The last free bit of the flags byte. Safe through lerp_state, which takes `flags` whole.
     */
    kPlayerStateUpperGuard = 1 << 7,
};

/**
 * Bit flags in PlayerState::gear — worn equipment that is neither the sword nor the shield.
 *
 * A byte of its own because `flags` and `equip` were both full (protocol 14). Seven bits are spare,
 * so the magne-boot stick, the magic armour's heavy state and the like can follow at no size cost.
 */
enum PlayerGearFlags : std::uint8_t {
    /**
     * The sender is wearing the iron boots: `daPy_py_c::checkEquipHeavyBoots()`, i.e.
     * FLG0_EQUIP_HVY_BOOTS, verbatim.
     *
     * ★ That flag and nothing derived from it, because it is the one value both consumers of
     * "boots on" in daAlink_c read. setHeavyBoots sets it and calls
     * `mZ2Link.setLinkBootsType(1)` in the same two lines, and clears it with
     * `setLinkBootsType(0)` (d_a_alink_hvyboots.inc:43-45, :74-76); the footstep code swaps
     * WALK/JUMP_DUMMY for the _HEAVY sounds on that boots type (Z2LinkMgr.cpp:585-597). The draw
     * code gates the boot models on the same predicate (d_a_alink.cpp:5995, :19832).
     *
     * NOT checkBootsOrArmorHeavy(): that also folds in the magic armour at zero rupees and the
     * iron-ball wait, which are heavy for the physics but are not boots and do not change the
     * footstep.
     */
    kPlayerGearHeavyBoots = 1 << 0,
};

/**
 * The joint-aim fields' wire scale: one s8 step is 128 binary-angle units, 0.70 degrees.
 *
 * The range this buys is +-16256 units (+-89 degrees). Every on-foot human value fits with room to
 * spare: the neck is at most ~10000 (half of HIO mHeadMaxTurnHorizontal = 20000), the slope pitch
 * 0x2800, the turn lead 0xC00 and the run bank 0x600. The sender clamps to kJointAimWireMax before
 * it gets here and COUNTS the clamps (fill_joint_aim), so a range mistake shows up as a number in
 * the trace instead of as a silent wrap. write() clamps again defensively, without counting.
 */
inline constexpr int kJointAimWireScale = 128;
inline constexpr int kJointAimWireMax = 127 * kJointAimWireScale;

/// s16 binary angle -> the s8 that goes on the wire. Rounds to nearest; saturates at +-127.
inline std::uint8_t joint_aim_to_wire(std::int16_t v) {
    int q = (v >= 0 ? v + kJointAimWireScale / 2 : v - kJointAimWireScale / 2) / kJointAimWireScale;
    if (q > 127) {
        q = 127;
    } else if (q < -127) {
        q = -127;
    }
    return static_cast<std::uint8_t>(static_cast<std::int8_t>(q));
}

/// The inverse: the wire's s8 back to an s16 binary angle.
inline std::int16_t joint_aim_from_wire(std::uint8_t v) {
    return static_cast<std::int16_t>(static_cast<std::int8_t>(v) * kJointAimWireScale);
}

/**
 * The action animation override (block 2). `actUnder == kPlayerActNone` means "no override": the
 * puppet plays its own locomotion reconstruction, exactly as before this block existed.
 *
 * Fixed-point scales, chosen so every field fits its type over the range Link actually uses:
 * frames at 1/8 (u16 -> 8191 frames), rate at 1/32 (s8 -> +-3.97), root translation at 1/8 unit
 * (s16 -> +-4096 units, a body length is ~150).
 */
constexpr std::uint16_t kPlayerActNone = 0xFFFF;
constexpr float kPlayerActFrameScale = 8.0f;
constexpr float kPlayerActRateScale = 32.0f;
constexpr float kPlayerActRootScale = 8.0f;
/// The hand-pose sentinel for "the body's own hand" — daAlink_c's 0xFE (setDrawHand).
constexpr std::uint8_t kPlayerActBodyHand = 0xFE;

/// Saturating float -> fixed point, rounded to nearest. Engine-free so both ends share it.
inline std::uint16_t act_encode_u16(float value, float scale) {
    const float scaled = value * scale + 0.5f;
    if (!(scaled > 0.0f)) {
        return 0;
    }
    if (scaled >= 65535.0f) {
        return 65535;
    }
    return static_cast<std::uint16_t>(scaled);
}

inline std::int16_t act_encode_s16(float value, float scale) {
    const float scaled = value * scale;
    if (!(scaled > -32768.0f)) {
        return -32768;
    }
    if (scaled >= 32767.0f) {
        return 32767;
    }
    return static_cast<std::int16_t>(scaled < 0.0f ? scaled - 0.5f : scaled + 0.5f);
}

inline std::int8_t act_encode_s8(float value, float scale) {
    const float scaled = value * scale;
    if (!(scaled > -128.0f)) {
        return -128;
    }
    if (scaled >= 127.0f) {
        return 127;
    }
    return static_cast<std::int8_t>(scaled < 0.0f ? scaled - 0.5f : scaled + 0.5f);
}

/// 49 bytes on the wire: 23 base, 6 head/torso (block 1), 18 action animation (block 2), the
/// 1-byte `gear` (offset 47), then the protocol-15 fields, in order: `locPhase` (offset 48). Sent
/// unreliably at the sim rate, so it has to stay small.
struct PlayerState {
    float posX = 0.0f;
    float posY = 0.0f;
    float posZ = 0.0f;
    /// Y rotation in the engine's s16 binary-angle units. Wraps — interpolate on the shortest arc.
    std::int16_t angleY = 0;
    /**
     * The sender's own gait rate: `daAlink_c::getMoveGroundAngleSpeedRate()`, verbatim.
     *
     * ★ NOT a speed, despite occupying the four bytes one used to. It is the dimensionless value
     * daAlink_c feeds his own gait blend — `fabsf(mNormalSpeed * cM_scos(groundAngle) /
     * mMaxSpeed)`, roughly 0..1 — and it is sent instead of a speed so the receiver has nothing
     * left to derive. Three separate divergences were arithmetic the receiver was doing for itself
     * and getting subtly wrong:
     *
     *   - it divided by the HIO constant 23.0, but mMaxSpeed is a daAlink_c MEMBER and is the
     *     lock-on maximum (or a flat 13.0) while targeting (d_a_alink.cpp:7867-7871);
     *   - it ignored the ground-angle cosine, so a sender climbing a slope kept a flat walk when
     *     Link himself had shifted toward the slower gait;
     *   - it read speedF, Link's root-motion-blended TRANSLATION speed, which barely moves across
     *     the whole walk band (that one was visible: every puppet's legs ran slow at a walk).
     *
     * Sending the answer instead of the inputs retires all three at once and costs no bytes. The
     * value is already absolute — daAlink_c takes fabsf — so it is never negative.
     */
    float moveRate = 0.0f;
    /// Which outfit the SENDER is wearing, as an index into the puppet actor's outfit table.
    ///
    /// Appearance is owned by the wearer, so this travels with the pose rather than being guessed
    /// at the receiver. It stays an OPAQUE byte here on purpose: the table lives in
    /// d_a_remote_player.cpp, and duplicating it into the engine-free layer would give "which
    /// outfit is 2?" two answers that could drift. daRemotePlayer_outfitFromWire() is the single
    /// place that resolves it, and it substitutes the hero's clothes for anything it does not
    /// recognise — 0xFF "nobody reported one" included.
    std::uint8_t outfit = 0xFF;
    std::uint8_t flags = 0;
    /// Sword and shield: which, and where. See PlayerEquipFlags for the bit layout.
    ///
    /// Discrete like `flags` and `outfit`, and taken whole from the newer sample for the same
    /// reason — blending the two kind fields would name a model neither player is holding.
    std::uint8_t equip = 0;
    /**
     * Which idle the sender is actually playing. See PlayerIdleKind.
     *
     * ★ An ENUMERATION rather than more bits in `flags`, and that is the shape of the thing rather
     * than a preference: the variants are mutually exclusive by construction, because
     * setBlendMoveAnime picks exactly one and procWait/procTiredWait replace it outright. Spending
     * a flag bit each on states that can never co-occur would put illegal combinations on the wire
     * for the receiver to have to rank, and ranking them is precisely the re-derivation this field
     * exists to stop.
     *
     * kPlayerIdleInsect is NAMED but not yet sampled — the sender's chain in fill_idle_kind() does
     * not test for it. It is listed here so the wire value is fixed now rather than being invented
     * later, since a byte with 248 spare values means finishing the set costs no protocol bump.
     *
     * Discrete like `flags`, `outfit` and `equip`, and taken whole from the newer sample for the
     * same reason: blending 0 and 2 would name idle 1 — a different animation — for the crossover.
     *
     * kPlayerIdleWait is the default and the safe reading: it is the ordinary standing idle, which
     * is what the puppet already played before this field existed.
     */
    std::uint8_t idleKind = kPlayerIdleWait;
    /**
     * Which attention (Z-target / lock-on) move set the sender is playing, and which way. See
     * PlayerAtnKind. Added in protocol 12 (D3b).
     *
     * This is a byte of its own, not more values in `idleKind`. An attention set isn't an idle:
     * it covers the stance AND the strafe walk and run, and the receiver blends it across the
     * whole speed range with its own HIO table. The stance and the strafe are one set
     * (12-d3-idle-spec.md §1), so one value has to name all of it.
     *
     * Discrete, so lerp_state takes it whole from the newer sample, like `idleKind`.
     */
    std::uint8_t atnKind = kPlayerAtnNone;

    /* --- Head turn and torso lean (worklist 14, protocol 14) -----------------------------------
     *
     * The ANSWERS daAlink_c::jointControll consumes for joints 4, 1 and 2
     * (d_a_alink.cpp:2192-2212), not the inputs to them. The sender has already run setNeckAngle's
     * whole target chain (the NPC he is glancing at, Midna, the boomerang, the turn lead, the slope
     * pitch) and its HIO clamps; the receiver could not re-derive any of it, because the look
     * targets are HIS actors.
     *
     * All six are s16 binary angles HERE, quantised to s8 x 128 only in write()/read(), so
     * lerp_state blends them at full precision. Zero for a wolf sender (his arms are different,
     * :2150-2191) and for "not in the world". Six bytes, appended after atnKind. */
    /// field_0x3124.x — the neck's pitch, applied about joint 4's local Z.
    std::int16_t neckX = 0;
    /// field_0x3124.y — the neck's yaw, applied about joint 4's local Y. Sent un-negated; the
    /// receiver negates it exactly as jointControll does (:2193).
    std::int16_t neckY = 0;
    /// jointControll's `sp8` for joint 1 (:2199-2204): mBodyAngle.x, or mBodyAngle.x -
    /// shape_angle.x when rein-riding. The sender evaluates that predicate; the receiver never sees
    /// it.
    std::int16_t bodyX = 0;
    /// field_0x30c8 — half the torso twist, applied at joint 1 AND again at joint 2.
    std::int16_t bodyTwist = 0;
    /// mBodyAngle.z — the half of the run-turn bank that joint 1 carries (setMoveSlantAngle).
    std::int16_t bodyZ = 0;
    /// shape_angle.z — the other half, which tilts the whole model in setMatrix (:5776). Outside
    /// jointControll, but bodyZ without it is half a lean.
    std::int16_t bankZ = 0;

    // block (2): action anim — after the head/torso block (1)
    /**
     * What Link is DRAWING when he is in a proc the puppet does not reconstruct — a sword swing, a
     * roll, a jump, a hurt reaction. 18 bytes, all of them the sender's answers rather than inputs
     * (.claude/future-plans/puppet-action-states.md §2-3):
     *
     *   actUnder / actUpper  mUnderAnmHeap[0] / mUpperAnmHeap[0].getIdx(), ALANM resource indices.
     *                        actUnder == kPlayerActNone: no override. actUpper == kPlayerActNone:
     *                        the upper half plays the under BCK (daAlink_c's own encoding,
     *                        getUnderUpperAnime resets the upper idx to 0xFFFF in that case).
     *   actFrame / actUpperFrame  the two frame controllers' frames, x kPlayerActFrameScale.
     *   actRate              the under controller's rate, x kPlayerActRateScale. Used by the
     *                        receiver only while starved, to keep a swing moving.
     *   actMorf              the cross-fade length in frames Link started on the tick actUnder
     *                        last changed, latched.
     *   actHandL / actHandR  the hand shapes he is SHOWING (field_0x06d0/06d4 resolved back to a
     *                        hand-model material index), kPlayerActBodyHand for the body's own.
     *   actRootX/Y/Z         joint 0's final model-space translation after jointControll, x
     *                        kPlayerActRootScale — the root-motion clear, as an answer.
     *
     * Discrete except the frames and the root translation — see lerp_state for the rule.
     */
    std::uint16_t actUnder = kPlayerActNone;
    std::uint16_t actUpper = kPlayerActNone;
    std::uint16_t actFrame = 0;
    std::uint16_t actUpperFrame = 0;
    std::int8_t actRate = 0;
    std::uint8_t actMorf = 0;
    std::uint8_t actHandL = kPlayerActBodyHand;
    std::uint8_t actHandR = kPlayerActBodyHand;
    std::int16_t actRootX = 0;
    std::int16_t actRootY = 0;
    std::int16_t actRootZ = 0;

    bool act_active() const { return actUnder != kPlayerActNone; }
    float act_frame() const { return static_cast<float>(actFrame) / kPlayerActFrameScale; }
    float act_upper_frame() const {
        return static_cast<float>(actUpperFrame) / kPlayerActFrameScale;
    }
    float act_rate() const { return static_cast<float>(actRate) / kPlayerActRateScale; }
    float act_root_x() const { return static_cast<float>(actRootX) / kPlayerActRootScale; }
    float act_root_y() const { return static_cast<float>(actRootY) / kPlayerActRootScale; }
    float act_root_z() const { return static_cast<float>(actRootZ) / kPlayerActRootScale; }

    /// Worn equipment other than the sword and shield. See PlayerGearFlags. Protocol 14. Discrete,
    /// so lerp_state takes it whole from the newer sample. Offset 47, after block 2; the
    /// protocol-15 fields follow it.
    std::uint8_t gear = 0;

    /* --- Protocol 15, block (1): locomotion phase. Offset 48, directly after `gear`. --- */
    /**
     * Where the sender is in his blended stride: `mUnderFrameCtrl[0].getFrame() / getEnd()`,
     * 0..1 for one cycle, x256 and wrapped, so 256 would be 0. Every slot of daAlink_c's blended
     * pair sits at this same normalised phase (commonDoubleAnime, d_a_alink.cpp:7029-7070), so one
     * value names the whole stride.
     *
     * The sender's ANSWER, and it fixes a lag no input could. The puppet used to carry its own
     * phase from pair to pair (daAlink_c's rule) and so started every walk from wherever its own
     * idle happened to be. Measured: a constant +75 ms torso-sway lag over a 19 s run.
     *
     * 0 whenever it means nothing. That is while the action override is on (act_active()) and
     * while Link plays a single animation (field_0x2f8c == 0). Zero is not a sentinel. It is
     * Link's own restart point: commonDoubleAnime starts the next blended pair from phase 0 after a
     * single animation, so a 0 on the sample before that pair begins lerps into it correctly.
     *
     * Continuous, so lerp_state blends it. The blend goes the short way round the cycle, and only
     * when both samples are locomotion; otherwise it takes the newer sample. */
    std::uint8_t locPhase = 0;

    float loc_phase() const { return static_cast<float>(locPhase) / 256.0f; }

    bool in_world() const { return (flags & kPlayerStateInWorld) != 0; }
    bool sharp_turn() const { return (flags & kPlayerStateSharpTurn) != 0; }
    bool zero_speed() const { return (flags & kPlayerStateZeroSpeed) != 0; }
    bool mode_idle() const { return (flags & kPlayerStateModeIdle) != 0; }
    bool no_foot_ik() const { return (flags & kPlayerStateNoFootIk) != 0; }
    bool time_can_pass() const { return (flags & kPlayerStateTimeCanPass) != 0; }
    bool upper_guard() const { return (flags & kPlayerStateUpperGuard) != 0; }

    bool sword_draw() const { return (equip & kPlayerEquipSwordDraw) != 0; }
    bool sword_in_hand() const { return (equip & kPlayerEquipSwordInHand) != 0; }
    std::uint8_t sword_kind() const {
        return static_cast<std::uint8_t>(
            (equip & kPlayerEquipSwordKindMask) >> kPlayerEquipSwordKindShift);
    }
    bool shield_draw() const { return (equip & kPlayerEquipShieldDraw) != 0; }
    bool shield_in_hand() const { return (equip & kPlayerEquipShieldInHand) != 0; }
    std::uint8_t shield_kind() const {
        return static_cast<std::uint8_t>(
            (equip & kPlayerEquipShieldKindMask) >> kPlayerEquipShieldKindShift);
    }
    bool heavy_boots() const { return (gear & kPlayerGearHeavyBoots) != 0; }

    void write(Writer& w) const {
        w.write_f32(posX);
        w.write_f32(posY);
        w.write_f32(posZ);
        w.write_s16(angleY);
        w.write_f32(moveRate);
        w.write_u8(outfit);
        w.write_u8(flags);
        w.write_u8(equip);
        w.write_u8(idleKind);
        w.write_u8(atnKind);
        w.write_u8(joint_aim_to_wire(neckX));
        w.write_u8(joint_aim_to_wire(neckY));
        w.write_u8(joint_aim_to_wire(bodyX));
        w.write_u8(joint_aim_to_wire(bodyTwist));
        w.write_u8(joint_aim_to_wire(bodyZ));
        w.write_u8(joint_aim_to_wire(bankZ));
        // block (2): action anim — after the head/torso block (1)
        w.write_u16(actUnder);
        w.write_u16(actUpper);
        w.write_u16(actFrame);
        w.write_u16(actUpperFrame);
        w.write_u8(static_cast<std::uint8_t>(actRate));
        w.write_u8(actMorf);
        w.write_u8(actHandL);
        w.write_u8(actHandR);
        w.write_s16(actRootX);
        w.write_s16(actRootY);
        w.write_s16(actRootZ);
        // After block (2): `gear`, then the protocol-15 fields in order.
        w.write_u8(gear);
        // v15 block (1): locomotion phase
        w.write_u8(locPhase);
    }

    bool read(Reader& r) {
        std::uint8_t aim[6] = {};
        std::uint8_t rate = 0;
        if (!(r.read_f32(posX) && r.read_f32(posY) && r.read_f32(posZ) && r.read_s16(angleY) &&
                r.read_f32(moveRate) && r.read_u8(outfit) && r.read_u8(flags) && r.read_u8(equip) &&
                r.read_u8(idleKind) && r.read_u8(atnKind) &&
                // block (1): head turn and torso lean
                r.read_u8(aim[0]) && r.read_u8(aim[1]) && r.read_u8(aim[2]) && r.read_u8(aim[3]) &&
                r.read_u8(aim[4]) && r.read_u8(aim[5]) &&
                // block (2): action anim
                r.read_u16(actUnder) && r.read_u16(actUpper) && r.read_u16(actFrame) &&
                r.read_u16(actUpperFrame) && r.read_u8(rate) && r.read_u8(actMorf) &&
                r.read_u8(actHandL) && r.read_u8(actHandR) && r.read_s16(actRootX) &&
                r.read_s16(actRootY) && r.read_s16(actRootZ) &&
                // after block (2): gear
                r.read_u8(gear) &&
                // v15 block (1): locomotion phase
                r.read_u8(locPhase)))
        {
            return false;
        }
        neckX = joint_aim_from_wire(aim[0]);
        neckY = joint_aim_from_wire(aim[1]);
        bodyX = joint_aim_from_wire(aim[2]);
        bodyTwist = joint_aim_from_wire(aim[3]);
        bodyZ = joint_aim_from_wire(aim[4]);
        bankZ = joint_aim_from_wire(aim[5]);
        actRate = static_cast<std::int8_t>(rate);
        // A non-finite float from the wire would poison the interpolation buffer and every pose
        // lerped from it, so reject the snapshot outright. An unusable rate only picks the gait,
        // so it is zeroed instead.
        if (!std::isfinite(posX) || !std::isfinite(posY) || !std::isfinite(posZ)) {
            return false;
        }
        if (!std::isfinite(moveRate)) {
            moveRate = 0.0f;
        }
        return true;
    }
};

/**
 * Shortest-arc interpolation between two binary angles.
 *
 * The subtraction is done in std::uint16_t so it wraps modulo 2^16; reinterpreting that as
 * std::int16_t yields the shortest signed delta automatically. This is why turning past the
 * 0/65535 seam doesn't make the puppet spin the long way round.
 */
inline std::int16_t lerp_angle(std::int16_t a, std::int16_t b, float t) {
    const std::int16_t delta =
        static_cast<std::int16_t>(static_cast<std::uint16_t>(b) - static_cast<std::uint16_t>(a));
    const std::int32_t stepped = static_cast<std::int32_t>(static_cast<float>(delta) * t);
    return static_cast<std::int16_t>(
        static_cast<std::uint16_t>(a) + static_cast<std::uint16_t>(stepped));
}

/// Linear blend of two poses. `t` is expected in [0, 1] but is not clamped here — callers that
/// extrapolate briefly past the newest sample pass t > 1 deliberately.
inline PlayerState lerp_state(const PlayerState& a, const PlayerState& b, float t) {
    // Never blend ACROSS the in-world boundary. A not-in-world sample carries no position — the
    // sender had no Link to read and sent zeroes — so interpolating out of one would walk the
    // puppet from the world origin towards the spawn point over a couple of ticks, and it would
    // visibly fly in from the horizon on the first tick of every session. Position is only
    // meaningful when both endpoints have it; when they disagree the discrete state wins and the
    // newer sample is taken whole.
    if (a.in_world() != b.in_world()) {
        return b;
    }

    PlayerState out;
    out.posX = a.posX + (b.posX - a.posX) * t;
    out.posY = a.posY + (b.posY - a.posY) * t;
    out.posZ = a.posZ + (b.posZ - a.posZ) * t;
    out.angleY = lerp_angle(a.angleY, b.angleY, t);
    out.moveRate = a.moveRate + (b.moveRate - a.moveRate) * t;
    // The outfit is discrete for the same reason the flags are: it is an index, not a quantity.
    // Blending 1 and 3 would name outfit 2 — a third, unrelated archive — for as long as the
    // crossover lasted, so the newer sample is taken whole.
    out.outfit = b.outfit;
    // Flags are discrete: take the newer sample's, never a blend of two bitfields.
    out.flags = b.flags;
    out.equip = b.equip;
    // Discrete for the same reason as the outfit: it is an index into a table of animations, not a
    // quantity. Halfway between the tired idle and the service idle is the plain one, which is
    // neither of the two the sender was ever in.
    out.idleKind = b.idleKind;
    // Discrete too. Halfway between "strafing left" and "backing off" is no animation at all.
    out.atnKind = b.atnKind;
    /* The joint aim angles are continuous, so they blend — but with t clamped to 1. The rest of the
     * pose extrapolates briefly past the newest sample when the buffer starves; these angles
     * reverse often (a head glancing left and back, the bank flipping between turns), and
     * extrapolating through a reversal overshoots, which reads as a twitch. Holding the newest
     * sample is the honest answer for a head. */
    const float aimT = t > 1.0f ? 1.0f : t;
    out.neckX = lerp_angle(a.neckX, b.neckX, aimT);
    out.neckY = lerp_angle(a.neckY, b.neckY, aimT);
    out.bodyX = lerp_angle(a.bodyX, b.bodyX, aimT);
    out.bodyTwist = lerp_angle(a.bodyTwist, b.bodyTwist, aimT);
    out.bodyZ = lerp_angle(a.bodyZ, b.bodyZ, aimT);
    out.bankZ = lerp_angle(a.bankZ, b.bankZ, aimT);

    // block (2): action anim — after the head/torso block (1)
    // Ids, rate, morf and hands are discrete: taken whole from the newer sample.
    out.actUnder = b.actUnder;
    out.actUpper = b.actUpper;
    out.actRate = b.actRate;
    out.actMorf = b.actMorf;
    out.actHandL = b.actHandL;
    out.actHandR = b.actHandR;
    out.actFrame = b.actFrame;
    out.actUpperFrame = b.actUpperFrame;
    out.actRootX = b.actRootX;
    out.actRootY = b.actRootY;
    out.actRootZ = b.actRootZ;
    /* Frames lerp ONLY across forward play of one animation: same id on both sides and b not
     * behind a. A loop seam, a restart or a reverse-played animation takes b whole, which is a
     * one-tick hold — invisible at 30 Hz, and it keeps this layer from needing frameMax. The upper
     * frame is held to the same rule on its own id (it can restart alone). */
    const bool sameUnder = a.act_active() && a.actUnder == b.actUnder;
    if (sameUnder && b.actFrame >= a.actFrame) {
        out.actFrame = act_encode_u16(
            (a.act_frame() + (b.act_frame() - a.act_frame()) * t), kPlayerActFrameScale);
    }
    if (sameUnder && a.actUpper == b.actUpper && b.actUpperFrame >= a.actUpperFrame) {
        out.actUpperFrame =
            act_encode_u16((a.act_upper_frame() + (b.act_upper_frame() - a.act_upper_frame()) * t),
                kPlayerActFrameScale);
    }
    // The root translation is continuous within one animation, and meaningless across two.
    if (sameUnder) {
        out.actRootX = act_encode_s16(
            a.act_root_x() + (b.act_root_x() - a.act_root_x()) * t, kPlayerActRootScale);
        out.actRootY = act_encode_s16(
            a.act_root_y() + (b.act_root_y() - a.act_root_y()) * t, kPlayerActRootScale);
        out.actRootZ = act_encode_s16(
            a.act_root_z() + (b.act_root_z() - a.act_root_z()) * t, kPlayerActRootScale);
    }

    // gear: a bit set, like `flags`: never blended.
    out.gear = b.gear;

    /* v15 block (1): the stride phase, the short way round the cycle. The subtraction wraps in u8
     * and reads back as s8, exactly as lerp_angle does at 16 bits. t is NOT clamped. The stride
     * only runs forward, so extrapolating a starved buffer keeps it walking rather than
     * overshooting a reversal. Only when both samples are locomotion: across an override boundary
     * one side is the meaningless 0 and the newer sample is the only answer. */
    out.locPhase = b.locPhase;
    if (!a.act_active() && !b.act_active()) {
        const int delta =
            static_cast<std::int8_t>(static_cast<std::uint8_t>(b.locPhase - a.locPhase));
        const float stepped = static_cast<float>(delta) * t;
        const int rounded = static_cast<int>(stepped < 0.0f ? stepped - 0.5f : stepped + 0.5f);
        out.locPhase = static_cast<std::uint8_t>(a.locPhase + rounded);
    }
    return out;
}

}  // namespace dusk::mp
