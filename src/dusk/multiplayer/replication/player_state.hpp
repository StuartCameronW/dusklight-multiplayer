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
};

/// 19 bytes on the wire. Sent unreliably at the sim rate, so it has to stay small.
struct PlayerState {
    float posX = 0.0f;
    float posY = 0.0f;
    float posZ = 0.0f;
    /// Y rotation in the engine's s16 binary-angle units. Wraps — interpolate on the shortest arc.
    std::int16_t angleY = 0;
    /// Horizontal speed in engine units per tick, used to choose the puppet's animation.
    float speed = 0.0f;
    std::uint8_t flags = 0;

    bool in_world() const { return (flags & kPlayerStateInWorld) != 0; }

    void write(Writer& w) const {
        w.write_f32(posX);
        w.write_f32(posY);
        w.write_f32(posZ);
        w.write_s16(angleY);
        w.write_f32(speed);
        w.write_u8(flags);
    }

    bool read(Reader& r) {
        return r.read_f32(posX) && r.read_f32(posY) && r.read_f32(posZ) && r.read_s16(angleY) &&
               r.read_f32(speed) && r.read_u8(flags);
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
    // Flags are discrete: take the newer sample's, never a blend of two bitfields.
    out.flags = b.flags;
    return out;
}

}  // namespace dusk::mp
