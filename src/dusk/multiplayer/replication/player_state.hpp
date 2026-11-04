#pragma once

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
};

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

/// 41 bytes on the wire (23 + 18, before the head/torso block is merged in ahead of block 2). Sent
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

    // block (2): action anim — goes after the head/torso block
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

    bool in_world() const { return (flags & kPlayerStateInWorld) != 0; }
    bool sharp_turn() const { return (flags & kPlayerStateSharpTurn) != 0; }
    bool zero_speed() const { return (flags & kPlayerStateZeroSpeed) != 0; }
    bool mode_idle() const { return (flags & kPlayerStateModeIdle) != 0; }
    bool no_foot_ik() const { return (flags & kPlayerStateNoFootIk) != 0; }
    bool time_can_pass() const { return (flags & kPlayerStateTimeCanPass) != 0; }

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
        // block (2): action anim — goes after the head/torso block
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
    }

    bool read(Reader& r) {
        std::uint8_t rate = 0;
        const bool ok = r.read_f32(posX) && r.read_f32(posY) && r.read_f32(posZ) &&
                        r.read_s16(angleY) && r.read_f32(moveRate) && r.read_u8(outfit) &&
                        r.read_u8(flags) && r.read_u8(equip) && r.read_u8(idleKind) &&
                        r.read_u8(atnKind) &&
                        // block (2): action anim — goes after the head/torso block
                        r.read_u16(actUnder) && r.read_u16(actUpper) && r.read_u16(actFrame) &&
                        r.read_u16(actUpperFrame) && r.read_u8(rate) && r.read_u8(actMorf) &&
                        r.read_u8(actHandL) && r.read_u8(actHandR) && r.read_s16(actRootX) &&
                        r.read_s16(actRootY) && r.read_s16(actRootZ);
        actRate = static_cast<std::int8_t>(rate);
        return ok;
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

    // block (2): action anim — goes after the head/torso block
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
    return out;
}

}  // namespace dusk::mp
