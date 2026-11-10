#pragma once

#include <cstddef>
#include <cstdint>

#include "../net/serializer.hpp"

/**
 * The replicated-object kind table (.claude/future-plans/mp-object-channel.md §1.5).
 *
 * Engine-free, like the rest of replication/ except the bridges: the ghost actor
 * (d_a_mp_ghost.cpp) and the owner-side capture files read this table, the object channel itself
 * only ever needs the rate class and the "known kind" answer.
 *
 * ★ Every kind blob on the wire is length-prefixed (u8 len, then len bytes), so a receiver that
 * meets a kind it does not implement yet skips exactly that blob instead of desyncing the rest of
 * an ObjState batch. That is what lets the phase-1 agents land kinds independently. It does NOT
 * replace the protocol bump rule: every phase still bumps kProtocolVersion.
 */

namespace dusk::mp::objects {

/// Kind bytes, pre-assigned by the spec (§1.5). Values are wire format: never renumber.
enum class ObjKind : std::uint8_t {
    None = 0x00,
    Arrow = 0x01,
    Pellet = 0x02,
    Bomb = 0x03,
    WaterBomb = 0x04,
    Bombling = 0x05,
    Explosion = 0x06,  ///< event-only: never streamed, never a ghost of its own
    Boomerang = 0x07,
    RodBall = 0x08,
    Carry = 0x10,
    Statue = 0x11,
    Horse = 0x12,  ///< only if Q1 = one shared Epona
    EnemyFirst = 0x40,
    EnemyLast = 0x7E,
    /// Phase 0's debug object: one transient that orbits its owner's Link for N ticks. Its ghost
    /// draws the arrow model. Sits at the top of the 0x40-0x7F block the spec reserves.
    Debug = 0x7F,
};

/// Largest kind blob (spawn init blob, state blob or event blob) this build stores. A longer one
/// on the wire is skipped whole and the object treated as malformed, never truncated.
inline constexpr std::size_t kMaxKindBlob = 48;

/// Sent with every state entry, every spawn and every event, so the ghost can be posed without
/// knowing the kind. Kind-specific extras ride in the blob.
struct ObjPose {
    float pos[3] = {0.0f, 0.0f, 0.0f};
    /// shape_angle x/y/z, s16 binary angle units.
    std::int16_t angle[3] = {0, 0, 0};
    /// Bits 0-5 kind-defined; bit 7 kObjPoseHidden and bit 6 kObjPoseResume are common.
    std::uint8_t flags = 0;
};

/// The owner is not drawing it this tick (nocked, inside something, mid-transition). The ghost
/// keeps its record and its interpolation history but draws nothing.
inline constexpr std::uint8_t kObjPoseHidden = 0x80;
/// Wire-only, set by the channel itself (kinds never set it): "nothing was sent for this object
/// since my previous sample because it did not change". The receiver then holds the previous
/// sample until the tick before this one instead of gliding across the gap. Stripped on receipt.
inline constexpr std::uint8_t kObjPoseResume = 0x40;
/// The bits a kind may define for itself.
inline constexpr std::uint8_t kObjPoseKindMask = 0x3F;

/// 19 bytes: absolute f32 positions on purpose (spec §1.3: arrows fly thousands of units, past the
/// ±8192 an s16 quarter-unit offset can reach).
inline void write_pose(Writer& w, const ObjPose& p) {
    w.write_f32(p.pos[0]);
    w.write_f32(p.pos[1]);
    w.write_f32(p.pos[2]);
    w.write_s16(p.angle[0]);
    w.write_s16(p.angle[1]);
    w.write_s16(p.angle[2]);
    w.write_u8(p.flags);
}

inline bool read_pose(Reader& r, ObjPose& p) {
    return r.read_f32(p.pos[0]) && r.read_f32(p.pos[1]) && r.read_f32(p.pos[2]) &&
           r.read_s16(p.angle[0]) && r.read_s16(p.angle[1]) && r.read_s16(p.angle[2]) &&
           r.read_u8(p.flags);
}

/// One length-prefixed kind blob, held as raw bytes. The channel never interprets it; the kind's
/// own read_*_blob does, on whichever end it is implemented.
struct KindBlob {
    std::uint8_t len = 0;
    std::uint8_t bytes[kMaxKindBlob] = {};

    bool operator==(const KindBlob& o) const {
        if (len != o.len) {
            return false;
        }
        for (std::size_t i = 0; i < len; i++) {
            if (bytes[i] != o.bytes[i]) {
                return false;
            }
        }
        return true;
    }
    bool operator!=(const KindBlob& o) const { return !(*this == o); }

    /// Fill from a Writer's bytes. False (and empty) when they do not fit.
    bool assign(const Writer& w) {
        if (w.size() > kMaxKindBlob) {
            len = 0;
            return false;
        }
        len = static_cast<std::uint8_t>(w.size());
        for (std::size_t i = 0; i < w.size(); i++) {
            bytes[i] = w.data()[i];
        }
        return true;
    }

    Reader reader() const { return Reader(bytes, len); }
};

inline void write_blob(Writer& w, const KindBlob& b) {
    w.write_u8(b.len);
    for (std::size_t i = 0; i < b.len; i++) {
        w.write_u8(b.bytes[i]);
    }
}

/**
 * Read a [u8 len][len bytes] blob. A blob longer than kMaxKindBlob is SKIPPED (consumed, so the
 * rest of a batch still parses) and reported as `oversized`; the caller drops that one object.
 */
inline bool read_blob(Reader& r, KindBlob& b, bool& oversized) {
    oversized = false;
    std::uint8_t len = 0;
    if (!r.read_u8(len)) {
        return false;
    }
    std::uint8_t scratch = 0;
    b.len = 0;
    for (std::size_t i = 0; i < len; i++) {
        if (!r.read_u8(scratch)) {
            return false;
        }
        if (i < kMaxKindBlob) {
            b.bytes[i] = scratch;
        }
    }
    if (len > kMaxKindBlob) {
        oversized = true;
        return true;
    }
    b.len = len;
    return true;
}

/**
 * Per-kind facts both ends agree on.
 *
 * `sendEvery` is the rate class (spec §1.3): a CHANGED state is streamed at most every N sim ticks
 * (1 = 30 Hz). `forceEvery` is the loss-recovery sample sent even when nothing changed, while the
 * object is live; 0 means never (an object at rest sends nothing, its final pose rides a reliable
 * event). `ghostHeap` is the ghost actor's solid heap for this kind, from the game's own number for
 * the real actor (cited per row). `implemented` is THIS build's answer to "can I draw it"; a
 * receiver still registers, plays back and despawns an unimplemented kind, it just creates no
 * ghost for it.
 */
struct ObjKindInfo {
    ObjKind kind;
    const char* name;
    std::uint8_t sendEvery;
    std::uint8_t forceEvery;
    std::uint32_t ghostHeap;
    bool implemented;
};

inline constexpr ObjKindInfo kObjKinds[] = {
    // d_a_arrow.cpp:1149: 0x810, 0xEA0 for a bomb arrow. The type arrives in the blob after the
    // heap is made, so an Arrow ghost loads BOTH models (0x810 + 0xEA0) plus its friendly-fire
    // collider (~0x180, d_a_mp_ghost_arrow.inc ArrowFf), rounded up. A pellet: 0x810 + collider.
    {ObjKind::Arrow, "arrow", 1, 15, 0x1A00, true},
    {ObjKind::Pellet, "pellet", 1, 15, 0xC00, true},
    // d_a_nbomb.cpp:210: 0xEB0 / 0x17C0 (bombling).
    {ObjKind::Bomb, "bomb", 1, 15, 0xEB0, false},
    {ObjKind::WaterBomb, "waterbomb", 1, 15, 0xEB0, false},
    {ObjKind::Bombling, "bombling", 1, 15, 0x17C0, false},
    {ObjKind::Explosion, "explosion", 0, 0, 0, false},
    // d_a_boomerang.cpp:1476.
    {ObjKind::Boomerang, "boomerang", 1, 15, 0xC0D0, false},
    // d_a_crod.cpp:81.
    {ObjKind::RodBall, "rodball", 1, 15, 0x10C0, false},
    {ObjKind::Carry, "carry", 1, 15, 0, false},
    {ObjKind::Statue, "statue", 1, 15, 0, false},
    {ObjKind::Horse, "horse", 1, 15, 0, false},
    // The arrow model, d_a_arrow.cpp:1149.
    {ObjKind::Debug, "debug", 1, 15, 0x810, true},
};

/// The row for a kind byte, or nullptr for one this build has never heard of. Enemy kinds
/// (0x40-0x7E) have no rows until M6.
inline const ObjKindInfo* kind_info(std::uint8_t kind) {
    for (const ObjKindInfo& info : kObjKinds) {
        if (static_cast<std::uint8_t>(info.kind) == kind) {
            return &info;
        }
    }
    return nullptr;
}

inline const char* kind_name(std::uint8_t kind) {
    const ObjKindInfo* info = kind_info(kind);
    return info != nullptr ? info->name : "unknown";
}

/* --- per-kind blob layouts --------------------------------------------------------------------
 * One block per kind, each owned by that kind's agent. Phase 0 defines only the debug kind's. */

/// Debug kind (0x7F). Spawn init blob and state blob share the layout: [u16 age][u16 lifetime].
/// The age makes the blob change every tick, which exercises the length-prefixed path end to end.
struct DebugBlob {
    std::uint16_t age = 0;
    std::uint16_t lifetime = 0;
};

inline void write_debug_blob(Writer& w, const DebugBlob& b) {
    w.write_u16(b.age);
    w.write_u16(b.lifetime);
}

inline bool read_debug_blob(Reader& r, DebugBlob& b) {
    return r.read_u16(b.age) && r.read_u16(b.lifetime);
}

/* --- events and despawn reasons (ObjEvent's u8 event, ObjDespawn's u8 reason) --------------- */

enum class ObjEventId : std::uint8_t {
    Stuck = 1,         ///< pos, angle, optional target ObjRef (arrows)
    Explode = 2,       ///< pos, water (bombs, bomb arrows)
    Break = 3,         ///< pos (thrown pots)
    Rest = 4,          ///< final pos (anything that stops)
    Caught = 5,        ///< the boomerang / rod ball came back
    Hit = 6,           ///< M6: a non-authority hit on an enemy
    DebugPing = 0x7F,  ///< phase 0: sent once at the debug object's half-life
};

enum class ObjDespawnReason : std::uint8_t {
    Gone = 0,     ///< the owner's actor no longer exists
    Expired = 1,  ///< lifetime over (the debug object)
    Broken = 2,
    Caught = 3,
    Exploded = 4,
    Released = 5,  ///< a Placed object's claim ended; receivers resume local simulation
};

/// ObjClaim's u8 claimKind (and ObjClaimResult's). Release is a claim kind so one packet id
/// covers both directions of the claim's life (spec §2.5 "ObjClaim(release)").
enum class ClaimKind : std::uint8_t {
    Release = 0,
    Carry = 1,
    Possess = 2,
    Mount = 3,
    EnemyAuthority = 4,
};

}  // namespace dusk::mp::objects
