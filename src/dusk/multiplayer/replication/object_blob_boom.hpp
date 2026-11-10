#pragma once

#include <cstdint>

#include "../net/serializer.hpp"

/**
 * The Gale Boomerang's kind blob (ObjKind::Boomerang, 0x07). Spec: .claude/future-plans/
 * mp-object-channel.md §2.3. Header-only so the owner (object_capture_boom.cpp) and the ghost
 * (d_a_mp_ghost_boom.inc) read one definition; kept out of object_kinds.hpp so the phase-1 agents
 * never edit the same block.
 *
 *   State blob   [u16 age][u8 shippuSize x64][u8 bits]                                  4 bytes
 *   Init blob    the state blob, then [u8 n][n x ObjRef]                                <= 44
 *   Caught event [u16 age]
 *
 * - `age`: sim ticks since the throw, on the OWNER's clock. It makes the owner's "boomerang return
 *   age=N" and the ghost's "returning age=N" lines comparable across the two logs without either
 *   end knowing the other's tick.
 * - `shippuSize`: daBoomerang_c::m_shippuSize, the tornado's XZ scale (1.0 flying, eases to 2.0
 *   while it chases a lock target, d_a_boomerang.cpp:1092-1105). x64, so 1.0 = 64.
 * - `bits`: kBoomReturning = FLG0_1 (getReturnFlg, the turn for home), kBoomLockChase = the
 *   lock-chase stretch (field_0x957 != 0).
 * - The init blob's lock list: the throw's lock targets as Placed refs (stage, home room, setID),
 *   for a later phase that might draw the reticles (spec §2.3 recommends never drawing them; they
 *   are the thrower's UI). Targets with no setID (0xFFFF) and ground-point locks are left out.
 *
 * The blade's spin (m_modelRotY) is NOT sent: it is a fixed -0x1F00 a tick from 0 at the throw
 * (d_a_boomerang.cpp:725-732, :964), so the ghost runs it itself and a lost state packet can never
 * freeze it.
 */

namespace dusk::mp::objects {

inline constexpr std::uint8_t kBoomReturning = 0x01;
inline constexpr std::uint8_t kBoomLockChase = 0x02;

/// Most lock targets the init blob carries (BOOMERANG_LOCK_MAX).
inline constexpr std::uint8_t kBoomMaxLocks = 5;

struct BoomBlob {
    std::uint16_t age = 0;
    float shippuSize = 1.0f;
    std::uint8_t bits = 0;
};

inline std::uint8_t boom_quantise_size(float size) {
    const float scaled = size * 64.0f + 0.5f;
    if (scaled <= 0.0f) {
        return 0;
    }
    if (scaled >= 255.0f) {
        return 255;
    }
    return static_cast<std::uint8_t>(scaled);
}

inline void write_boom_blob(Writer& w, const BoomBlob& b) {
    w.write_u16(b.age);
    w.write_u8(boom_quantise_size(b.shippuSize));
    w.write_u8(b.bits);
}

/// Reads the 4-byte prefix shared by the state and init blobs.
inline bool read_boom_blob(Reader& r, BoomBlob& b) {
    std::uint8_t size = 0;
    if (!r.read_u16(b.age) || !r.read_u8(size) || !r.read_u8(b.bits)) {
        return false;
    }
    b.shippuSize = static_cast<float>(size) / 64.0f;
    return true;
}

}  // namespace dusk::mp::objects
