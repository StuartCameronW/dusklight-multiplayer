#pragma once

#include <cstdint>

#include "../net/serializer.hpp"
#include "object_kinds.hpp"

/**
 * Wire blobs for the arrow and slingshot-pellet kinds (ObjKind::Arrow 0x01, ObjKind::Pellet 0x02).
 * Spec: .claude/future-plans/mp-object-channel.md §2.1.
 *
 * Shared by the two ends of phase-1 agent P1a and nothing else:
 *   owner     src/dusk/multiplayer/replication/object_capture_arrow.cpp
 *   receiver  src/d/actor/d_a_mp_ghost_arrow.inc
 * Kept out of object_kinds.hpp so the four phase-1 kinds never edit the same lines.
 *
 * Every number here is read straight off the owner's daArrow_c (include/d/actor/d_a_arrow.h), so
 * the ghost decides nothing the real arrow did not already decide.
 */

namespace dusk::mp::objects {

/// daArrow_c::mArrowType values the channel carries (d_a_arrow.h ArrowType_e). Type 2 (light,
/// Horseback Zelda's) is never captured.
inline constexpr std::uint8_t kArrowTypeNormal = 0;
inline constexpr std::uint8_t kArrowTypeBomb = 1;
inline constexpr std::uint8_t kArrowTypeSling = 4;

/// daArrow_c's own param byte, which IS its state (d_a_arrow.cpp:1147 and every fopAcM_SetParam).
inline constexpr std::uint8_t kArrowStateNocked = 0;
inline constexpr std::uint8_t kArrowStateShot = 1;
inline constexpr std::uint8_t kArrowStateCharged = 2;
inline constexpr std::uint8_t kArrowStateActorStop = 3;
inline constexpr std::uint8_t kArrowStateBgStop = 4;
inline constexpr std::uint8_t kArrowStateReturn = 5;
inline constexpr std::uint8_t kArrowStateEnemyHeld = 6;
inline constexpr std::uint8_t kArrowStateSlingHit = 8;

/// ArrowBlob::bits.
inline constexpr std::uint8_t kArrowBitFrozen = 0x01;  ///< field_0x943: ice hit or ice polygon
inline constexpr std::uint8_t kArrowBitWater = 0x02;   ///< field_0x945: entered water

/**
 * Spawn init blob AND state blob, 5 bytes: [u8 type][u8 state][u8 bits][u8 scale][u8 flash].
 *
 * The pose itself (ObjPose) is the arrow MODEL's base matrix, not current.pos/shape_angle: the
 * model is what the owner draws, and it already folds in every special case (the -95 offset of a
 * stuck arrow, d_a_arrow.cpp:384-389 and :440; the joint-relative pose of one stuck in an actor,
 * :437-441; the vibration after it lands, :439 and :944). The ghost only has to put its model
 * there.
 */
struct ArrowBlob {
    std::uint8_t type = kArrowTypeNormal;
    std::uint8_t state = kArrowStateShot;
    std::uint8_t bits = 0;
    /// scale.x * 255. The arrow shrinks to nothing when it sinks too deep (d_a_arrow.cpp:781-789).
    std::uint8_t scale = 255;
    /// Bomb arrow only: the red TEV value daArrow_c::draw computes from its fuse (0-50,
    /// d_a_arrow.cpp:1095-1106). Computed on the owner, so the ghost needs no fuse or HIO of its
    /// own.
    std::uint8_t flash = 0;
};

inline void write_arrow_blob(Writer& w, const ArrowBlob& b) {
    w.write_u8(b.type);
    w.write_u8(b.state);
    w.write_u8(b.bits);
    w.write_u8(b.scale);
    w.write_u8(b.flash);
}

inline bool read_arrow_blob(Reader& r, ArrowBlob& b) {
    return r.read_u8(b.type) && r.read_u8(b.state) && r.read_u8(b.bits) && r.read_u8(b.scale) &&
           r.read_u8(b.flash);
}

/**
 * ObjEventId::Stuck blob, 21 bytes: [u8 state][ObjPose][u8 polySound].
 * `state` is what it stopped as: kArrowStateBgStop (in a wall), kArrowStateActorStop (in an
 * actor; phase 1 has no Placed ref for enemies, so the pose is a fixed world pose and the stream
 * keeps it on the owner's actor), or kArrowStateSlingHit (a pellet hit and is gone).
 * `polySound` is dKy_pol_sound_get of the hit polygon, the map info the game's stick SE uses
 * (d_a_arrow.cpp:769); 0 when not from a polygon.
 */
struct ArrowStuckEvent {
    std::uint8_t state = kArrowStateBgStop;
    ObjPose pose;
    std::uint8_t polySound = 0;
};

inline void write_arrow_stuck(Writer& w, const ArrowStuckEvent& e) {
    w.write_u8(e.state);
    write_pose(w, e.pose);
    w.write_u8(e.polySound);
}

inline bool read_arrow_stuck(Reader& r, ArrowStuckEvent& e) {
    return r.read_u8(e.state) && read_pose(r, e.pose) && r.read_u8(e.polySound);
}

/**
 * ObjEventId::Explode blob, 13 bytes: [f32 x3 pos][u8 water]. A bomb arrow went off: `pos` is where
 * the owner's own explosion-only bomb was created (dBomb_c::createNormalBombExplode,
 * d_a_arrow.cpp:339-342), read off that bomb, not guessed.
 */
struct ArrowExplodeEvent {
    float pos[3] = {0.0f, 0.0f, 0.0f};
    std::uint8_t water = 0;
};

inline void write_arrow_explode(Writer& w, const ArrowExplodeEvent& e) {
    w.write_f32(e.pos[0]);
    w.write_f32(e.pos[1]);
    w.write_f32(e.pos[2]);
    w.write_u8(e.water);
}

inline bool read_arrow_explode(Reader& r, ArrowExplodeEvent& e) {
    return r.read_f32(e.pos[0]) && r.read_f32(e.pos[1]) && r.read_f32(e.pos[2]) &&
           r.read_u8(e.water);
}

}  // namespace dusk::mp::objects
