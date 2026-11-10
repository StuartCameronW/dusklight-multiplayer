#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>

#include "dusk/player_atn.hpp"
#include "dusk/player_equip.hpp"
#include "dusk/player_held.hpp"
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

/// v19: the three sword capsules of PlayerState::atMask.
constexpr std::uint8_t kPlayerAtCapsuleMask = 0x07;

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
    /**
     * The sender's legs are on the iron-boots gait: `checkUnderMove1BckNoArc(ANM_WALK_HEAVY)`, read
     * off his animation heap like fill_idle_kind() and fill_atn_kind() read theirs.
     *
     * ★ Not the same fact as kPlayerGearHeavyBoots, which is why it has a bit of its own.
     * setBlendMoveAnime only takes ANM_WALK_HEAVY under `checkBootsMoveAnime(1) &&
     * !enableFastIronBoots` (d_a_alink.cpp:7579, :7626-7627). Booted Link in water walks the
     * ordinary gait, and so does every booted Link whose OWN settings have fast iron boots on,
     * which the receiver cannot see. Deriving the gait from the boots bit gets both wrong.
     *
     * No protocol bump: a sender that predates this bit leaves it clear, and the puppet then plays
     * the ordinary gait, which is what it did before.
     */
    kPlayerGearHeavyGait = 1 << 1,
    /**
     * The sender's legs are on the iron-ball carry gait:
     * `checkUnderMove1BckNoArc(ANM_IRONBALL_WALK)`, read off his animation heap exactly as
     * kPlayerGearHeavyGait is.
     *
     * setBlendMoveAnime takes ANM_IRONBALL_WAIT / ANM_IRONBALL_WALK under `checkBootsMoveAnime(1)
     * && !checkBootsMoveAnime(0)` and not enableFastIronBoots (d_a_alink.cpp:7579, :7641-7649):
     * the ball's IBWAIT upper overlay makes him heavy for the gait but not for the boots branch.
     * Both iron-ball rows sit in slot 1 for the whole of band 1, which is the only band the branch
     * can reach, and the standing pivot replaces slot 1, so the bit is clear exactly when his legs
     * are not on the carry gait. Deriving it from heldItem would get the fast-iron-boots setting
     * and the pivot wrong, as deriving the boots gait from the boots bit would.
     */
    kPlayerGearIronBallGait = 1 << 2,
};

/**
 * v19 iron ball block: where the ball is, in units of 1/kPlayerIronBallPosScale relative
 * to the sender's own position (posX/Y/Z). 4 steps a unit is +-8191 units, and the ball never gets
 * further from him than its chain allows: setIronBallPos clamps it to 1000 units from his hand
 * (d_a_alink_ironball.inc:579-580), and at most 100 links of 10 units can be paid out (:563-564).
 */
constexpr float kPlayerIronBallPosScale = 4.0f;

/**
 * v20 clawshot block: where a fired tip is, in units of 1/kPlayerClawPosScale relative to the
 * sender's own position. +-8191 units against a chain that stops at HIO mMaxLength - 15 from his
 * gun (2000, or mBossMaxLength 3000 in D_MN07A; setHookshotPos, d_a_alink_hook.inc:924-1104).
 * Only the "super clawshot" cheat (69420, :933-937) can pass it; that is sent clamped.
 */
constexpr float kPlayerClawPosScale = 4.0f;

/**
 * v20 effects byte (PlayerState::fxFlags). Each bit is owned by one feature; a bit nobody sets is 0.
 *   bit 0       the lantern is lit (its glow, flame and light).
 *   bit 1       a sword trail is live (m_swordBlur).
 *   bit 2       a spin attack's effect is live.
 *   bit 3       the sword is charging (agent F: his m_nSwordBtk frame > 0, see below).
 *   bits 4-5    PlayerState::dmgFlash's tint kind (kPlayerDmgTint*).
 *   bits 6-7    agent F: the sword-fx variant, read with bit 1 or bit 2 (kPlayerFxVariant*).
 */
enum PlayerFx : std::uint8_t {
    kPlayerFxLanternLit = 1 << 0,
    kPlayerFxSwordTrail = 1 << 1,
    kPlayerFxSpin = 1 << 2,
    kPlayerFxSwordCharge = 1 << 3,
};
constexpr std::uint8_t kPlayerFxDmgTintShift = 4;
constexpr std::uint8_t kPlayerFxDmgTintMask = 0x30;
constexpr std::uint8_t kPlayerFxKnownMask = 0xFF;
/**
 * Agent F's bits (sword trail, spin attack, charge), set by fill_sword_fx (player_bridge.cpp) and
 * read by d_a_remote_player_swordfx.inc. What each one means on the SENDER:
 *
 *   bit 1  kPlayerFxSwordTrail: daAlink_c EXTENDED m_swordBlur this tick (copyBlur,
 *          d_a_alink.cpp:6626). Read off the blur's own point count (field_0x14): copyBlur is the
 *          only thing that raises it (+10, capped at 58; d_a_alink_effect.inc:2012-2015),
 *          decSwordBlur only lowers it and initBlur zeroes it. The decay, and the trail riding the
 *          body while it decays, are rebuilt on the receiver exactly as decSwordBlur does them.
 *   bit 2  kPlayerFxSpin: setCutTurnEffect emitted this tick (d_a_alink_effect.inc:944-1096):
 *          PROC_CUT_TURN / PROC_BOARD_CUT_TURN with his first spin emitter id live. clearCutTurn-
 *          EffectID zeroes those ids on every proc change (d_a_alink.cpp:15218), so a non-zero id
 *          in the spin proc means the emitter has been set in it; that already folds in the
 *          start-frame gate and the out-of-Zora-armour underwater early return.
 *   bit 3  kPlayerFxSwordCharge: m_nSwordBtk's frame > 0, setSwordChargeEffect's own test
 *          (d_a_alink_effect.inc:820-821). The frame is NOT on the wire: it is 1 on the first
 *          charging tick and rises by one a tick (simpleAnmPlay, d_a_alink.cpp:5845; reset to 0 by
 *          every commonProcInit, :15244), so the receiver plays its own copy of the BTK from the
 *          rising edge. Never set with the wooden sword (he does not play the BTK for it).
 *
 * Bits 6-7 are ONE two-bit field whose meaning depends on which of bits 1 and 2 is set. The two
 * never are at once: copyBlur is unreachable in the spin procs (setAtCollision sends PROC_CUT_TURN
 * and PROC_BOARD_CUT_TURN to decSwordBlur, d_a_alink.cpp:6574-6588), and fill_sword_fx gives the
 * spin precedence regardless. With neither set the field is 0.
 *   with bit 2: a PlayerFxSpinKind, the emitter list setCutTurnEffect chose (:1034-1061).
 *   with bit 1: kPlayerFxTrailDash (bit 6): the blur alpha is his mDashBlurAlpha, not mBlurAlpha
 *               (d_a_alink.cpp:6567-6572). kPlayerFxTrailReversed (bit 7): the blade sweeps along
 *               its local +Z rather than -Z, i.e. CUT_TYPE_TWIRL / CUT_TYPE_FINISH_RIGHT
 *               (setSwordPos, :5875-5879), read off the direction the blur actually stored.
 */
constexpr std::uint8_t kPlayerFxVariantShift = 6;
constexpr std::uint8_t kPlayerFxVariantMask = 0xC0;
constexpr std::uint8_t kPlayerFxTrailDash = 1 << 6;
constexpr std::uint8_t kPlayerFxTrailReversed = 1 << 7;
/// The spin's emitter list. A Zora-armour LARGE spin underwater has no value of its own: it goes as
/// kPlayerFxSpinWater, and the receiver loses only that case's particle scale/volume tweak.
enum PlayerFxSpinKind : std::uint8_t {
    kPlayerFxSpinNormal = 0,
    kPlayerFxSpinLight = 1,
    kPlayerFxSpinLarge = 2,
    kPlayerFxSpinWater = 3,
};
// The ironBallState byte's layout is in dusk/player_held.hpp, which the puppet can include.

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
/// "The sender's in-world hands are not known this tick": his status window is open, and
/// setDrawHand is showing the pause-menu grips (d_a_alink.cpp:18950-18961), not what he holds in
/// the world. The receiver falls back to its own table-plus-grip derivation. Never a hand-model
/// material index.
constexpr std::uint8_t kPlayerHandUnknown = 0xFF;
/// Block (4)'s unit-interval bytes (the slot-1 phase and blend weight): u8 x 255, so 0 and 1 are
/// both exact.
constexpr float kPlayerActUnitScale = 255.0f;

/// Saturating [0, 1] -> u8 x kPlayerActUnitScale, rounded to nearest.
inline std::uint8_t act_encode_unit(float value) {
    if (!(value > 0.0f)) {
        return 0;
    }
    if (value >= 1.0f) {
        return 255;
    }
    return static_cast<std::uint8_t>(value * kPlayerActUnitScale + 0.5f);
}

/// A full-circle binary angle -> one byte (256 units a step, 1.4 degrees), rounded to nearest and
/// wrapping. For the body pitch, which a flip carries all the way round.
inline std::uint8_t act_encode_turn(std::int16_t v) {
    return static_cast<std::uint8_t>(
        ((static_cast<std::uint32_t>(static_cast<std::uint16_t>(v)) + 0x80u) >> 8) & 0xFFu);
}
inline std::int16_t act_decode_turn(std::uint8_t v) {
    return static_cast<std::int16_t>(
        static_cast<std::uint16_t>(static_cast<std::uint32_t>(v) << 8));
}

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

/**
 * FNV-1a of a stage name (dStage_startStage_c::mName: a char[8], NUL-padded but not necessarily
 * NUL-terminated), so two ends can compare "which stage" in four bytes. Never 0: 0 on the wire
 * means "no location reported", which the receiver reads as "don't hide".
 */
inline std::uint32_t location_stage_hash(const char* name) {
    std::uint32_t hash = 2166136261u;
    for (int i = 0; i < 8 && name[i] != '\0'; i++) {
        hash ^= static_cast<std::uint8_t>(name[i]);
        hash *= 16777619u;
    }
    return hash != 0 ? hash : 1u;
}

/// 76 bytes on the wire: 23 base, 6 head/torso (block 1), 18 action animation (block 2), the
/// 1-byte `gear` (offset 47), then the protocol-15 fields, in order: `locPhase` (offset 48),
/// 12 bytes of action slot 1 / swim (block 4, offset 49), 9 bytes of held item (block 3,
/// offset 61), then the protocol-16 location (offset 70: u32 stageHash, s8 roomNo at 74,
/// u8 locEpoch at 75). Protocol 19 appends after locEpoch; its block A (bow) is 2 bytes: u8
/// heldBck, u8 heldBckFrame. Sent unreliably at the sim rate, so it has to stay small.
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
     *                        Since protocol 17 they are sent on EVERY tick, not only during an
     *                        override: the output of his whole setDrawHand chain (held item,
     *                        grips, bow, canoe). kPlayerHandUnknown while his status window is up.
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

    // block (4): action slot 1 / swim
    /**
     * The rest of what daAlink_c draws while block 2 is active. Only meaningful while
     * act_active(); zero / kPlayerActNone otherwise. 12 bytes:
     *
     *   act1Under / act1Upper  mUnderAnmHeap[1] / mUpperAnmHeap[1].getIdx(): the SECOND animation
     * of a double anime (commonDoubleAnime, d_a_alink.cpp:7022-7079), which every setBlendMoveAnime
     * / setBlendAtnMoveAnime caller uses — the step-turn (PROC_WAIT_TURN), the bow / boomerang /
     * clawshot / copy-rod aim-walk, carrying. kPlayerActNone when he is playing a single anime
     *                          (commonSingleAnime resets the slot, :7163). act1Upper kPlayerActNone
     *                          = follows act1Under, the same encoding as actUpper.
     *   act1Phase              mUnderFrameCtrl[1].getFrame() / getEnd(), u8 x kPlayerActUnitScale
     *                          (0.12 frames on a 60-frame cycle). A PHASE, not a frame, so it fits
     *                          one byte. The slot-1 upper frame is not sent at all:
     *                          commonDoubleAnime winds every controller to one phase at
     *                          proportional rates (:7043-7066), so the receiver puts the upper
     *                          half on this phase too.
     *   act1Ratio              mNowAnmPackUnder[1].getRatio(), u8 x kPlayerActUnitScale — the blend
     *                          weight setDoubleAnimeBlendRatio gives slot 1 (:7015-7020).
     *   actRootRotX / Z        field_0x3080 / field_0x3082: the extra joint-0 rotation
     *                          jointControll applies in EVERY proc (`sp18.set(field_0x3080, 0,
     *                          field_0x3082)`, d_a_alink.cpp:2231). It is not in any BCK. The
     *                          underwater swim pitch (-0x3800, d_a_alink_swim.inc:828-845) and the
     *                          large-damage tumble / wall-hit tilt (d_a_alink_damage.inc:1499-1538)
     *                          live here. Full s16: the wall-hit values reach 0x4000 and beyond.
     *   actYawOff              field_0x308c: the yaw offset setMatrix adds to the whole model
     *                          (:5776) — the wall-hang / climb shape alignment
     *                          (setClimbShapeOffset, d_a_alink_hang.inc:1728). An s16 here,
     *                          quantised to s8 x kJointAimWireScale on the wire like block 1.
     *   actPitch               shape_angle.x: the whole-model pitch setMatrix applies (:5776) — the
     *                          roll-jump flip (procRollJump turns it a full circle,
     *                          d_a_alink.cpp:17048 and on), crawling, the clawshot wall-hang,
     *                          slopes under setShapeAngleOnGround. An s16 here, ONE byte on the
     * wire (x256, full circle, 1.4 degrees) because the flip goes all the way round.
     *
     * ★ Block 2's actRoot* changes meaning WITH this block: it is now joint 0 relative to the base
     * the RECEIVER builds — pos, then ZXYrot(actPitch, angleY + actYawOff, bankZ), each as
     * quantised as the puppet sees it — not relative to Link's own base. That folds the base Y
     * offsets setMatrix adds and the wire does not carry — the swim bob field_0x2b98
     * (d_a_alink_swim.inc:893), the stair offset field_0x2b9c, the sink field_0x2ba0
     * (d_a_alink.cpp:5774) — into the root, so the puppet's body bobs where his does.
     */
    std::uint16_t act1Under = kPlayerActNone;
    std::uint16_t act1Upper = kPlayerActNone;
    std::uint8_t act1Phase = 0;
    std::uint8_t act1Ratio = 0;
    std::int16_t actRootRotX = 0;
    std::int16_t actRootRotZ = 0;
    std::int16_t actYawOff = 0;
    std::int16_t actPitch = 0;

    bool act1_active() const { return act1Under != kPlayerActNone; }
    float act1_phase() const { return static_cast<float>(act1Phase) / kPlayerActUnitScale; }
    float act1_ratio() const { return static_cast<float>(act1Ratio) / kPlayerActUnitScale; }

    // block (3): held item
    /**
     * The item model the sender's Link is DRAWING in his hands, and the UPPER_2 overlay that poses
     * his arms around it. 9 bytes, all answers read after daAlink_c's own logic
     * (player_bridge.cpp fill_held_item):
     *
     *   heldItem      PlayerHeldItem: the mHeldItemModel he placed AND checkItemDraw() let through.
     *   heldFlags     PlayerHeldFlags: lantern, which hand, the bottle cork, the second clawshot,
     *                 and (v19) a nocked arrow.
     *   heldMat       the held model's material animation frames (see player_held.hpp).
     *   upper2Idx     mUpperAnmHeap[UPPER_2].getIdx(), the item hold/aim overlay BCK (ALANM only);
     *                 kPlayerActNone = no overlay.
     *   upper2Frame   mUpperFrameCtrl[2].getFrame() x kPlayerActFrameScale.
     *   upper2Ratio   mNowAnmPackUpper[2].getRatio() x 255.
     *   upper2Morf    the upper cross-fade length in frames, latched on the tick upper2Idx changed.
     *
     * Discrete except upper2Frame / upper2Ratio — see lerp_state.
     */
    std::uint8_t heldItem = kPlayerHeldNone;
    std::uint8_t heldFlags = 0;
    std::uint8_t heldMat = 0;
    std::uint16_t upper2Idx = kPlayerActNone;
    std::uint16_t upper2Frame = 0;
    std::uint8_t upper2Ratio = 0;
    std::uint8_t upper2Morf = 0;

    bool upper2_active() const { return upper2Idx != kPlayerActNone; }

    // v16: where the sender is
    /**
     * Which stage and room the sender's Link is in, and a counter of his scene loads. 6 bytes:
     *
     *   stageHash  location_stage_hash(dComIfGp_getStartStageName()). 0 = never reported (no
     *              pose yet), which the receiver treats as "show".
     *   roomNo     fopAcM_GetRoomNo(Link): the room he is standing in. -1 = unknown.
     *   locEpoch   +1 every time the sender's Link actor is RE-CREATED (its process id changes),
     *              which the engine does on every scene load: a stage change, a same-stage warp,
     *              a void-out respawn. Wraps; only inequality means anything. Walking across a
     *              room boundary does NOT bump it: Link is not rebuilt and his position is
     *              continuous, so there is nothing to snap.
     *
     * The receiver resets its interpolation buffer when the epoch changes, so the puppet snaps to
     * the new spawn instead of gliding there from the old room (measured once as a 1,034-unit
     * jump), and despawns the puppet while the sender is in a stage, or a room, this end is not
     * drawing. All three are discrete, and lerp_state never blends across an epoch change.
     */
    std::uint32_t stageHash = 0;
    std::int8_t roomNo = -1;
    std::uint8_t locEpoch = 0;

    // v19 attack block: the sender's live sword hitbox, for friendly fire
    /**
     * daAlink_c's sword hitbox is three capsules (mAtCps, set in setSwordAtCollision,
     * d_a_alink.cpp:6190-6247) over three points: A = the blade root, B = the tip, C = the tip's
     * position last frame. Capsule 0 is A-B, 1 is C-B, 2 is A-C, so three points carry all three.
     *
     *   atMask    bit i = capsule i is live this tick (kPlayerAtCapsuleMask). 0 = no attack, and
     *             then nothing else in the block is on the wire.
     *   atRadius  capsule radius, world units.
     *   atSpl     the attack's dCcG_At_Spl (normal, large, huge...), which picks the victim's
     *             reaction: a spin attack knocks him down where a slash only flinches him.
     *   atPt      A, B, C as x,y,z each, relative to (posX, posY, posZ), world units.
     *
     * Only sword attacks are sent (AT_TYPE_NORMAL_SWORD / AT_TYPE_MASTER_SWORD), not the hookshot
     * or the lantern, which reuse capsule 0 for other things.
     */
    std::uint8_t atMask = 0;
    std::uint8_t atRadius = 0;
    std::uint8_t atSpl = 0;
    std::int16_t atPt[9] = {};

    // v19 bow block (after the attack block)
    /**
     * The bow / slingshot model's own animation (daAlink_c's mItemBck), and the nocked arrow's
     * kind. 2 bytes, read after his setItemMatrix entered them (player_bridge.cpp fill_held_item):
     *
     *   heldBck       bits 0-3 PlayerHeldItemBck: which BCK is in mItemBck (0 = none / not a bow or
     *                 slingshot); bit 4 kPlayerHeldBckBombArrow: the arrow flagged by
     *                 kPlayerHeldArrowNocked is a bomb arrow.
     *   heldBckFrame  that BCK's entered frame / getFrameMax(), x255.
     *
     * The id and the arrow bit are discrete; the frame blends within one BCK — see lerp_state.
     */
    std::uint8_t heldBck = 0;
    std::uint8_t heldBckFrame = 0;

    std::uint8_t held_bck_id() const {
        return static_cast<std::uint8_t>(heldBck & kPlayerHeldBckIdMask);
    }
    float held_bck_frame_unit() const { return static_cast<float>(heldBckFrame) / 255.0f; }

    // v19 iron ball block (after the bow block)
    /**
     * Where the sender's ball is, while heldItem is kPlayerHeldIronBall. 12 bytes:
     *
     *   ironBallX/Y/Z     mIronBallChainPos[0] (the chain end of the ball, where its model's
     *                     origin is), relative to posX/Y/Z, x kPlayerIronBallPosScale.
     *   ironBallAngX/Y    mIronBallChainAngle[0].x / .y, the ball model's rotation. Its z is a roll
     *                     about the chain and is not sent.
     *   ironBallLinks     mItemMode: how many chain links are paid out (6 carried, up to 100).
     *   ironBallState     mItemVar0.field_0x3018 | kPlayerIronBallJointRot.
     *
     * All zero when the ball is not out. The chain itself is NOT sent (102 links): the receiver
     * re-runs setIronBallChainPos between its own hand and this ball, which is a deterministic
     * relaxation, not a physics state worth the bytes.
     */
    std::int16_t ironBallX = 0;
    std::int16_t ironBallY = 0;
    std::int16_t ironBallZ = 0;
    std::int16_t ironBallAngX = 0;
    std::int16_t ironBallAngY = 0;
    std::uint8_t ironBallLinks = 0;
    std::uint8_t ironBallState = 0;

    bool iron_ball_out() const { return heldItem == kPlayerHeldIronBall; }
    float iron_ball_x() const { return static_cast<float>(ironBallX) / kPlayerIronBallPosScale; }
    float iron_ball_y() const { return static_cast<float>(ironBallY) / kPlayerIronBallPosScale; }
    float iron_ball_z() const { return static_cast<float>(ironBallZ) / kPlayerIronBallPosScale; }

    // v20 effects block (after the iron ball block): 2 bytes, always on the wire.
    /**
     *   fxFlags   PlayerFx bits, see above.
     *   dmgFlash  the timer behind fxFlags' tint kind (PlayerDmgTint, dusk/player_fx.hpp):
     *             Damage = his mDamageTimer + mDamageColorTime, Ice = his mIceDamageWaitTimer,
     *             each clamped to 0..255; 0 for Freeze and None. Not interpolated: the |sin| pulse
     *             it drives is 16 ticks long, so the receiver uses each tick's value as sent.
     */
    std::uint8_t fxFlags = 0;
    std::uint8_t dmgFlash = 0;

    // v20 clawshot block (after the effects block): only while heldItem is a clawshot
    /**
     * His two clawshot sets ("slots", see kPlayerClawTipOut0 in dusk/player_held.hpp), read after
     * setHookshotPos placed them (player_bridge.cpp fill_clawshot). 3 bytes, plus 10 per tip that
     * is off its gun:
     *
     *   clawFlags        kPlayerClawTipOut0/1, kPlayerClawWait, kPlayerClawRoofHang, and the
     *                    chain-wobble stop time in bits 4-7.
     *   clawTipFrame[s]  the HS_TIP_OPEN frame slot s's tip was entered with: field_0x33e0 for
     *                    slot 0, 0 or 14 for slot 1 (d_a_alink_hook.inc:872-905, :1208,
     *                    :1212-1243).
     *   clawTipPos[s*3]  ONLY while slot s's tip is out: its anchor (mHookshotTopPos /
     *                    mIronBallBgChkPos) relative to posX/Y/Z, x kPlayerClawPosScale...
     *   clawTipAng[s*2]  ...and its model's rotation x / y, read off the tip's base matrix. His
     *                    out-of-gun placements never roll it (z = 0, :1195-1229).
     *
     * A tip on its gun is not sent at all: the receiver puts it on its own gun's muzzle, so it can
     * never lag the puppet's hand. Nor is the chain (up to 600 links): it is a pure function of its
     * two ends and the stop time, so the receiver draws it with hsChainShape_c::draw's own loop.
     */
    std::uint8_t clawFlags = 0;
    std::uint8_t clawTipFrame[2] = {};
    std::int16_t clawTipPos[6] = {};
    std::int16_t clawTipAng[4] = {};

    bool clawshot_out() const {
        return heldItem == kPlayerHeldClawshot || heldItem == kPlayerHeldDoubleClawshot;
    }
    bool claw_tip_out(int slot) const {
        return (clawFlags & (kPlayerClawTipOut0 << slot)) != 0;
    }
    float claw_tip_pos(int i) const {
        return static_cast<float>(clawTipPos[i]) / kPlayerClawPosScale;
    }
    float upper2_frame() const { return static_cast<float>(upper2Frame) / kPlayerActFrameScale; }
    float upper2_ratio() const { return static_cast<float>(upper2Ratio) / 255.0f; }

    bool attacking() const { return (atMask & kPlayerAtCapsuleMask) != 0; }
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
    bool heavy_gait() const { return (gear & kPlayerGearHeavyGait) != 0; }
    bool iron_ball_gait() const { return (gear & kPlayerGearIronBallGait) != 0; }

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
        // block (4): action slot 1 / swim
        w.write_u16(act1Under);
        w.write_u16(act1Upper);
        w.write_u8(act1Phase);
        w.write_u8(act1Ratio);
        w.write_s16(actRootRotX);
        w.write_s16(actRootRotZ);
        w.write_u8(joint_aim_to_wire(actYawOff));
        w.write_u8(act_encode_turn(actPitch));
        // block (3): held item
        w.write_u8(heldItem);
        w.write_u8(heldFlags);
        w.write_u8(heldMat);
        w.write_u16(upper2Idx);
        w.write_u16(upper2Frame);
        w.write_u8(upper2Ratio);
        w.write_u8(upper2Morf);
        // v16: location
        w.write_u32(stageHash);
        w.write_u8(static_cast<std::uint8_t>(roomNo));
        w.write_u8(locEpoch);
        // v19 attack block: one byte when idle, 21 while a swing is live
        w.write_u8(atMask);
        if (attacking()) {
            w.write_u8(atRadius);
            w.write_u8(atSpl);
            for (const std::int16_t v : atPt) {
                w.write_s16(v);
            }
        }
        // v19 bow block (after the attack block)
        w.write_u8(heldBck);
        w.write_u8(heldBckFrame);
        // v19 iron ball block: only while the ball is out
        if (iron_ball_out()) {
            w.write_s16(ironBallX);
            w.write_s16(ironBallY);
            w.write_s16(ironBallZ);
            w.write_s16(ironBallAngX);
            w.write_s16(ironBallAngY);
            w.write_u8(ironBallLinks);
            w.write_u8(ironBallState);
        }
        // v20 effects block
        w.write_u8(fxFlags);
        w.write_u8(dmgFlash);
        // v20 clawshot block: only while a clawshot is drawn, and each tip only while it is out
        if (clawshot_out()) {
            w.write_u8(clawFlags);
            w.write_u8(clawTipFrame[0]);
            w.write_u8(clawTipFrame[1]);
            for (int s = 0; s < 2; s++) {
                if (claw_tip_out(s)) {
                    w.write_s16(clawTipPos[s * 3 + 0]);
                    w.write_s16(clawTipPos[s * 3 + 1]);
                    w.write_s16(clawTipPos[s * 3 + 2]);
                    w.write_s16(clawTipAng[s * 2 + 0]);
                    w.write_s16(clawTipAng[s * 2 + 1]);
                }
            }
        }
    }

    bool read(Reader& r) {
        std::uint8_t aim[6] = {};
        std::uint8_t rate = 0;
        std::uint8_t yawOff = 0;
        std::uint8_t pitch = 0;
        std::uint8_t room = 0;
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
                r.read_u8(locPhase) &&
                // block (4): action slot 1 / swim
                r.read_u16(act1Under) && r.read_u16(act1Upper) && r.read_u8(act1Phase) &&
                r.read_u8(act1Ratio) && r.read_s16(actRootRotX) && r.read_s16(actRootRotZ) &&
                r.read_u8(yawOff) && r.read_u8(pitch) &&
                // block (3): held item
                r.read_u8(heldItem) && r.read_u8(heldFlags) && r.read_u8(heldMat) &&
                r.read_u16(upper2Idx) && r.read_u16(upper2Frame) && r.read_u8(upper2Ratio) &&
                r.read_u8(upper2Morf) &&
                // v16: location
                r.read_u32(stageHash) && r.read_u8(room) && r.read_u8(locEpoch) &&
                // v19 attack block
                r.read_u8(atMask)))
        {
            return false;
        }
        atMask &= kPlayerAtCapsuleMask;
        if (attacking()) {
            if (!(r.read_u8(atRadius) && r.read_u8(atSpl))) {
                return false;
            }
            for (std::int16_t& v : atPt) {
                if (!r.read_s16(v)) {
                    return false;
                }
            }
        } else {
            atRadius = 0;
            atSpl = 0;
            std::fill(std::begin(atPt), std::end(atPt), std::int16_t{0});
        }
        // v19 bow block (after the attack block)
        if (!(r.read_u8(heldBck) && r.read_u8(heldBckFrame))) {
            return false;
        }
        // v19 iron ball block: only while the ball is out
        if (iron_ball_out()) {
            if (!(r.read_s16(ironBallX) && r.read_s16(ironBallY) && r.read_s16(ironBallZ) &&
                    r.read_s16(ironBallAngX) && r.read_s16(ironBallAngY) &&
                    r.read_u8(ironBallLinks) && r.read_u8(ironBallState)))
            {
                return false;
            }
        } else {
            ironBallX = ironBallY = ironBallZ = 0;
            ironBallAngX = ironBallAngY = 0;
            ironBallLinks = ironBallState = 0;
        }
        // v20 effects block
        if (!(r.read_u8(fxFlags) && r.read_u8(dmgFlash))) {
            return false;
        }
        fxFlags &= kPlayerFxKnownMask;
        // v20 clawshot block: only while a clawshot is drawn, and each tip only while it is out
        if (clawshot_out()) {
            if (!(r.read_u8(clawFlags) && r.read_u8(clawTipFrame[0]) &&
                    r.read_u8(clawTipFrame[1])))
            {
                return false;
            }
            for (int s = 0; s < 2; s++) {
                if (claw_tip_out(s)) {
                    if (!(r.read_s16(clawTipPos[s * 3 + 0]) && r.read_s16(clawTipPos[s * 3 + 1]) &&
                            r.read_s16(clawTipPos[s * 3 + 2]) &&
                            r.read_s16(clawTipAng[s * 2 + 0]) && r.read_s16(clawTipAng[s * 2 + 1])))
                    {
                        return false;
                    }
                } else {
                    clawTipPos[s * 3 + 0] = clawTipPos[s * 3 + 1] = clawTipPos[s * 3 + 2] = 0;
                    clawTipAng[s * 2 + 0] = clawTipAng[s * 2 + 1] = 0;
                }
            }
        } else {
            clawFlags = 0;
            clawTipFrame[0] = clawTipFrame[1] = 0;
            std::fill(std::begin(clawTipPos), std::end(clawTipPos), std::int16_t{0});
            std::fill(std::begin(clawTipAng), std::end(clawTipAng), std::int16_t{0});
        }
        roomNo = static_cast<std::int8_t>(room);
        actYawOff = joint_aim_from_wire(yawOff);
        actPitch = act_decode_turn(pitch);
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
    // The same rule across a scene load on the sender (v16 locEpoch): the two samples are in
    // different places, and a blend between them is a glide through walls. The buffer is also
    // reset when the epoch changes (ReplicationManager::record_remote), so this only catches a
    // reordered packet.
    if (a.locEpoch != b.locEpoch) {
        return b;
    }
    // And out of a fresh Link's first few room -1 samples into his first known room. He is placed
    // before he settles, so those samples can sit well off the ground (measured 225 units below,
    // F_SP121 re-warp), and blending from one put the puppet ~110 units low for a tick. The
    // receiver holds the puppet over the room -1 stretch anyway, so the first known pose is taken
    // whole. A stage that never reports a room has -1 on both sides and still blends.
    if (a.roomNo < 0 && b.roomNo >= 0) {
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

    // block (4): action slot 1 / swim. Same rules as block 2: ids discrete, the frame lerps only
    // across forward play of one slot-1 animation under one slot-0 animation.
    out.act1Under = b.act1Under;
    out.act1Upper = b.act1Upper;
    out.act1Phase = b.act1Phase;
    out.act1Ratio = b.act1Ratio;
    out.actRootRotX = b.actRootRotX;
    out.actRootRotZ = b.actRootRotZ;
    out.actYawOff = b.actYawOff;
    out.actPitch = b.actPitch;
    const bool sameSlot1 = sameUnder && a.act1_active() && a.act1Under == b.act1Under;
    if (sameSlot1 && b.act1Phase >= a.act1Phase) {
        out.act1Phase = act_encode_unit(a.act1_phase() + (b.act1_phase() - a.act1_phase()) * t);
    }
    if (sameSlot1) {
        // The weight is continuous but bounded: t clamped like the joint aim, and the encoder
        // saturates, so a starved extrapolation cannot push it outside 0..1.
        out.act1Ratio = act_encode_unit(a.act1_ratio() + (b.act1_ratio() - a.act1_ratio()) * aimT);
    }
    /* The joint-0 rotation and the yaw offset are angles and continuous within one animation (the
     * swim pitch eases with cLib_addCalcAngleS, d_a_alink_swim.inc:845). Across an animation change
     * they can jump (a wall hit sets the tilt outright), so b is taken whole there. t clamped, as
     * for the joint aim: these reverse. */
    if (sameUnder) {
        out.actRootRotX = lerp_angle(a.actRootRotX, b.actRootRotX, aimT);
        out.actRootRotZ = lerp_angle(a.actRootRotZ, b.actRootRotZ, aimT);
        out.actYawOff = lerp_angle(a.actYawOff, b.actYawOff, aimT);
        out.actPitch = lerp_angle(a.actPitch, b.actPitch, aimT);
    }

    // block (3): held item. Kind, flags, material frames, overlay id and morf are discrete.
    out.heldItem = b.heldItem;
    out.heldFlags = b.heldFlags;
    out.heldMat = b.heldMat;
    out.upper2Idx = b.upper2Idx;
    out.upper2Frame = b.upper2Frame;
    out.upper2Ratio = b.upper2Ratio;
    out.upper2Morf = b.upper2Morf;
    /* The overlay's frame and ratio blend only within one overlay BCK, and the frame only forward —
     * block 2's rule, for the same reasons (a loop seam or restart takes b whole). t is clamped to
     * 1 for the ratio: it is a weight, and extrapolating it past the newest sample overshoots. */
    if (a.upper2_active() && a.upper2Idx == b.upper2Idx) {
        if (b.upper2Frame >= a.upper2Frame) {
            out.upper2Frame = act_encode_u16(
                a.upper2_frame() + (b.upper2_frame() - a.upper2_frame()) * t, kPlayerActFrameScale);
        }
        const float ratioT = t > 1.0f ? 1.0f : t;
        const float ratio = a.upper2_ratio() + (b.upper2_ratio() - a.upper2_ratio()) * ratioT;
        out.upper2Ratio = static_cast<std::uint8_t>(
            ratio <= 0.0f ? 0.0f : (ratio >= 1.0f ? 255.0f : ratio * 255.0f + 0.5f));
    }

    // v16: location. Discrete; the epochs are equal by the check at the top.
    out.stageHash = b.stageHash;
    out.roomNo = b.roomNo;
    out.locEpoch = b.locEpoch;

    // v19 attack block. Which capsules are live is discrete. The points blend only when both
    // samples have a live swing, so the hitbox follows the drawn blade between ticks instead of
    // jumping a tick ahead of it; t is clamped so a starved buffer never extrapolates a blade.
    out.atMask = b.atMask;
    out.atRadius = b.atRadius;
    out.atSpl = b.atSpl;
    if (a.attacking() && b.attacking()) {
        const float atT = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        for (int i = 0; i < 9; i++) {
            const float v =
                static_cast<float>(a.atPt[i]) + static_cast<float>(b.atPt[i] - a.atPt[i]) * atT;
            out.atPt[i] = static_cast<std::int16_t>(std::lround(v));
        }
    } else {
        std::copy(std::begin(b.atPt), std::end(b.atPt), std::begin(out.atPt));
    }

    // v19 bow block (after the attack block). The id and the arrow bit are discrete. The frame
    // blends only within one BCK and only forward — block 3's overlay rule: a release or reload
    // swaps the BCK (changeItemBck), and the wait free-runs into the end clamp, so anything else
    // takes b whole.
    out.heldBck = b.heldBck;
    out.heldBckFrame = b.heldBckFrame;
    if (a.held_bck_id() != kPlayerHeldBckNone && a.held_bck_id() == b.held_bck_id() &&
        b.heldBckFrame >= a.heldBckFrame)
    {
        const float frameT = t > 1.0f ? 1.0f : t;
        const float frame =
            a.held_bck_frame_unit() + (b.held_bck_frame_unit() - a.held_bck_frame_unit()) * frameT;
        out.heldBckFrame = static_cast<std::uint8_t>(
            frame <= 0.0f ? 0.0f : (frame >= 1.0f ? 255.0f : frame * 255.0f + 0.5f));
    }

    // v19 iron ball block (after the bow block)
    /* The link count and the state are discrete. The ball's position and rotation blend only while
     * BOTH samples have it out: the other side's zeroes mean "no ball", and blending out of them
     * would fly it in from his feet. t is clamped to 1, as for the joint aim: a swung ball circles
     * him (state 2) and extrapolating a circle overshoots it outward. */
    out.ironBallLinks = b.ironBallLinks;
    out.ironBallState = b.ironBallState;
    out.ironBallX = b.ironBallX;
    out.ironBallY = b.ironBallY;
    out.ironBallZ = b.ironBallZ;
    out.ironBallAngX = b.ironBallAngX;
    out.ironBallAngY = b.ironBallAngY;
    if (a.iron_ball_out() && b.iron_ball_out()) {
        out.ironBallX = act_encode_s16(
            a.iron_ball_x() + (b.iron_ball_x() - a.iron_ball_x()) * aimT, kPlayerIronBallPosScale);
        out.ironBallY = act_encode_s16(
            a.iron_ball_y() + (b.iron_ball_y() - a.iron_ball_y()) * aimT, kPlayerIronBallPosScale);
        out.ironBallZ = act_encode_s16(
            a.iron_ball_z() + (b.iron_ball_z() - a.iron_ball_z()) * aimT, kPlayerIronBallPosScale);
        out.ironBallAngX = lerp_angle(a.ironBallAngX, b.ironBallAngX, aimT);
        out.ironBallAngY = lerp_angle(a.ironBallAngY, b.ironBallAngY, aimT);
    }

    // v20 effects block: discrete, take b.
    out.fxFlags = b.fxFlags;
    out.dmgFlash = b.dmgFlash;

    // v20 clawshot block (after the effects block)
    /* The flags (which tips are out, the stop time) and the tip-open frames are discrete. A tip's
     * position and rotation blend only while BOTH samples have that tip out: the other side's
     * zeroes mean "on its gun", and blending out of them would fly it in from his feet. t is
     * clamped to 1, the iron ball's rule: a tip flying out at 100 units a tick and stopping dead on
     * a wall must not be extrapolated through it. */
    out.clawFlags = b.clawFlags;
    out.clawTipFrame[0] = b.clawTipFrame[0];
    out.clawTipFrame[1] = b.clawTipFrame[1];
    std::copy(std::begin(b.clawTipPos), std::end(b.clawTipPos), std::begin(out.clawTipPos));
    std::copy(std::begin(b.clawTipAng), std::end(b.clawTipAng), std::begin(out.clawTipAng));
    if (a.clawshot_out() && b.clawshot_out()) {
        for (int s = 0; s < 2; s++) {
            if (!(a.claw_tip_out(s) && b.claw_tip_out(s))) {
                continue;
            }
            for (int i = s * 3; i < s * 3 + 3; i++) {
                out.clawTipPos[i] = act_encode_s16(
                    a.claw_tip_pos(i) + (b.claw_tip_pos(i) - a.claw_tip_pos(i)) * aimT,
                    kPlayerClawPosScale);
            }
            for (int i = s * 2; i < s * 2 + 2; i++) {
                out.clawTipAng[i] = lerp_angle(a.clawTipAng[i], b.clawTipAng[i], aimT);
            }
        }
    }
    return out;
}

}  // namespace dusk::mp
