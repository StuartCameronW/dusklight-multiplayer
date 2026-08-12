#pragma once

#include <cstdint>

#include "../net/serializer.hpp"

/**
 * The per-player pose that travels over the wire every sim tick.
 *
 * M1 replicates a COARSE pose deliberately (see 02/04): position, facing, and speed. The puppet
 * picks idle/walk/run from the speed rather than mirroring Link's animation ids, because driving
 * the real daAlink_PROC state machine is M3's job and would sink M1 in engine detail.
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
};

/// 20 bytes on the wire. Sent unreliably at the sim rate, so it has to stay small.
struct PlayerState {
    float posX = 0.0f;
    float posY = 0.0f;
    float posZ = 0.0f;
    /// Y rotation in the engine's s16 binary-angle units. Wraps — interpolate on the shortest arc.
    std::int16_t angleY = 0;
    /// Horizontal speed in engine units per tick, used to choose the puppet's animation.
    float speed = 0.0f;
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

    bool in_world() const { return (flags & kPlayerStateInWorld) != 0; }
    bool sharp_turn() const { return (flags & kPlayerStateSharpTurn) != 0; }

    void write(Writer& w) const {
        w.write_f32(posX);
        w.write_f32(posY);
        w.write_f32(posZ);
        w.write_s16(angleY);
        w.write_f32(speed);
        w.write_u8(outfit);
        w.write_u8(flags);
    }

    bool read(Reader& r) {
        return r.read_f32(posX) && r.read_f32(posY) && r.read_f32(posZ) && r.read_s16(angleY) &&
               r.read_f32(speed) && r.read_u8(outfit) && r.read_u8(flags);
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
    out.speed = a.speed + (b.speed - a.speed) * t;
    // The outfit is discrete for the same reason the flags are: it is an index, not a quantity.
    // Blending 1 and 3 would name outfit 2 — a third, unrelated archive — for as long as the
    // crossover lasted, so the newer sample is taken whole.
    out.outfit = b.outfit;
    // Flags are discrete: take the newer sample's, never a blend of two bitfields.
    out.flags = b.flags;
    return out;
}

}  // namespace dusk::mp
