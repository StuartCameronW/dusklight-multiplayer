#pragma once

#include <cstdint>

#include "object_kinds.hpp"

/**
 * Blob layouts for the bomb kinds (ObjKind Bomb 0x03, WaterBomb 0x04, Bombling 0x05) and their
 * explode event. Spec: .claude/future-plans/mp-object-channel.md §2.2.
 *
 * Engine-free. Written by the owner's capture (object_capture_bomb.cpp), read by the ghost
 * (src/d/actor/d_a_mp_ghost_bomb.inc). In its own header rather than object_kinds.hpp so the four
 * phase-1 kinds never edit the same lines.
 *
 * ★ The fuse. Sending mExTime every tick would make the blob change every tick, and an object
 * whose blob changes is streamed every tick even while it sits still (object_channel.cpp
 * stream_owned). So the owner sends a fuse SYNC instead: `fuse` is mExTime as of the last time
 * it stopped counting down one per tick, and `fuseSeq` counts those syncs. While the fuse burns
 * normally the blob is constant and a bomb at rest sends nothing; the ghost counts down locally
 * from the last sync. An event pause, a freeze, or the boomerang's re-arm (d_a_nbomb.cpp:427-455,
 * 1119, 1144-1156) bumps fuseSeq, and the ghost re-syncs. The flash is cosmetic; detonation is the
 * explicit Explode event, never the ghost's own countdown.
 */

namespace dusk::mp::objects {

/// BombBlob::flags.
inline constexpr std::uint8_t kBombHeld = 0x01;          ///< in a Link's hands (carry or hookshot)
inline constexpr std::uint8_t kBombFrozen = 0x02;        ///< FLG0_FROZEN: freeze tint, no sparks
inline constexpr std::uint8_t kBombSinking = 0x04;       ///< FLG0_UNK_800: drowning, drawn black
inline constexpr std::uint8_t kBombTimerStopped = 0x08;  ///< checkTimerStop(): no sparks, no fizz
inline constexpr std::uint8_t kBombWalking = 0x10;       ///< bombling in procInsectMove (walk BCK)
inline constexpr std::uint8_t kBombUnderwater = 0x20;    ///< FLG0_UNDERWATER

/// Spawn init blob and state blob (same layout). 8 bytes.
struct BombBlob {
    std::uint8_t flags = 0;
    /// mExTime at the last fuse sync.
    std::int16_t fuse = 0;
    /// Bumped on every fuse sync; the ghost re-syncs when it changes.
    std::uint8_t fuseSeq = 0;
    /// scale.x * 100 (a bombling is 60; a drowning or thawing bomb shrinks to 0).
    std::uint8_t scale100 = 100;
    /// Bumped on every ground landing (mAcch.ChkGroundLanding), so the ghost plays the bounce.
    std::uint8_t bounceSeq = 0;
    /// The landing's volume, as procWait computes it (|fall speed| * 2, capped at 100).
    std::uint8_t bouncePower = 0;
    std::uint8_t reserved = 0;
};

inline void write_bomb_blob(Writer& w, const BombBlob& b) {
    w.write_u8(b.flags);
    w.write_s16(b.fuse);
    w.write_u8(b.fuseSeq);
    w.write_u8(b.scale100);
    w.write_u8(b.bounceSeq);
    w.write_u8(b.bouncePower);
    w.write_u8(b.reserved);
}

inline bool read_bomb_blob(Reader& r, BombBlob& b) {
    return r.read_u8(b.flags) && r.read_s16(b.fuse) && r.read_u8(b.fuseSeq) &&
           r.read_u8(b.scale100) && r.read_u8(b.bounceSeq) && r.read_u8(b.bouncePower) &&
           r.read_u8(b.reserved);
}

/// ObjEventId::Explode blob: where it went off, and whether it is the underwater explosion
/// (FLG0_UNDERWATER: the water-bomb effect set and sound, d_a_nbomb.cpp procExplodeInit). 13 bytes.
struct BombExplodeBlob {
    float pos[3] = {0.0f, 0.0f, 0.0f};
    std::uint8_t water = 0;
};

inline void write_bomb_explode_blob(Writer& w, const BombExplodeBlob& b) {
    w.write_f32(b.pos[0]);
    w.write_f32(b.pos[1]);
    w.write_f32(b.pos[2]);
    w.write_u8(b.water);
}

inline bool read_bomb_explode_blob(Reader& r, BombExplodeBlob& b) {
    return r.read_f32(b.pos[0]) && r.read_f32(b.pos[1]) && r.read_f32(b.pos[2]) &&
           r.read_u8(b.water);
}

}  // namespace dusk::mp::objects
