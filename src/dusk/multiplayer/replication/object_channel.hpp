#pragma once

#include <cstdint>
#include <string>

#include "../net/packet.hpp"
#include "../net/serializer.hpp"
#include "object_kinds.hpp"

/**
 * The replicated-object channel, phase 0 (.claude/future-plans/mp-object-channel.md §1, §6).
 *
 * Detached things — projectiles in flight, thrown pots, possessed statues, later enemies — each
 * have an identity (ObjRef), one authority that simulates them, reliable spawn/event/despawn
 * messages on kChannelObjects, an unreliable batched state stream on kChannelState, and stage
 * scoping. The receiver plays every object back on its AUTHORITY's interpolation clock: the
 * authority's StateBuffer::played_tick(), the same timeline as that player's puppet, which is what
 * lines an arrow up with the bow that released it.
 *
 * Engine-free, like the rest of replication/. Everything that touches an actor goes through the
 * `engine` seam at the bottom of this header, implemented in src/d/actor/d_a_mp_ghost.cpp.
 *
 * ---- Wire layouts (v22). Every packet is [u8 PacketId][payload]; the readers below are handed
 * the Reader positioned just AFTER the id byte. ----
 *
 *   ObjRef      [u8 tag] then  tag 0 Transient:  [u8 owner][u16 serial]
 *                              tag 1 Placed:     [u32 stageHash][s8 room][u16 setID]
 *                              tag 2 PlacedNoId: [u32 stageHash][s8 room][u16 procname]
 *                                                [s16 x3 home.pos, whole units]
 *   Header H    [ObjRef][u8 authority][u8 kind][u32 stageHash][s8 room][u8 ownerEpoch]
 *   ObjPose     [f32 x3 pos][s16 x3 angle][u8 flags]                         (19 bytes)
 *   Blob        [u8 len][len bytes]                                          (object_kinds.hpp)
 *
 *   ObjSpawn       20  H [u64 originTick][ObjPose][Blob init]
 *   ObjState       21  [u8 authority][u64 originTick][u32 stageHash][u8 ownerEpoch][u8 n]
 *                      n x {[ObjRef][u8 kind][s8 room][ObjPose][Blob state]}
 *   ObjEvent       22  H [u64 originTick][u8 event][Blob]
 *   ObjDespawn     23  H [u64 originTick][u8 reason]
 *   ObjClaim       24  H [u64 originTick][u8 claimKind]            (claimKind 0 = release)
 *   ObjClaimResult 25  [ObjRef][u8 grantedPlayerId (0xFF = released)][u8 claimKind]
 *
 * Deviations from the spec's table, each deliberate:
 *  - H carries an explicit `authority` byte. For a Transient it repeats ref.owner; for a Placed ref
 *    it is the claim holder, which a client otherwise could not name for a host-relayed packet
 *    (every packet a client receives comes FROM the host).
 *  - ObjState hoists stageHash and ownerEpoch to the packet header: one packet is one authority on
 *    one tick, which has exactly one stage and one epoch. Saves 5 bytes an entry, which is what
 *    keeps 40 entries under the ~1200-byte MTU.
 *  - ObjDespawn carries originTick, so the receiver removes the ghost when PLAYBACK reaches the
 *    despawn rather than ~delay ticks early, which would clip the end of every flight.
 *
 * ---- Direction policing (spec §1.2). ----
 * 20-23 are authority-originated: a client may send them only for Transient refs whose owner is
 * its own player id, and for Placed refs it holds a claim on. 24 is host-bound, 25 client-bound.
 */

namespace dusk::mp::objects {

enum class RefTag : std::uint8_t {
    Transient = 0,
    Placed = 1,
    PlacedNoId = 2,
};

/// Object identity (spec §1.1). Value type; ordered so it can key a std::map.
struct ObjRef {
    RefTag tag = RefTag::Transient;
    /// Transient only.
    std::uint8_t owner = 0;
    std::uint16_t serial = 0;
    /// Placed / PlacedNoId only.
    std::uint32_t stageHash = 0;
    std::int8_t room = -1;
    std::uint16_t setId = 0xFFFF;
    /// PlacedNoId only.
    std::uint16_t procName = 0;
    std::int16_t home[3] = {0, 0, 0};

    static ObjRef transient(std::uint8_t owner, std::uint16_t serial) {
        ObjRef r;
        r.tag = RefTag::Transient;
        r.owner = owner;
        r.serial = serial;
        return r;
    }
    static ObjRef placed(std::uint32_t stageHash, std::int8_t room, std::uint16_t setId) {
        ObjRef r;
        r.tag = RefTag::Placed;
        r.stageHash = stageHash;
        r.room = room;
        r.setId = setId;
        return r;
    }

    bool operator<(const ObjRef& o) const;
    bool operator==(const ObjRef& o) const;
    bool operator!=(const ObjRef& o) const { return !(*this == o); }

    void write(Writer& w) const;
    bool read(Reader& r);
    /// "T0:12" / "P1A2B3C4D/1/37" / "N1A2B3C4D/1/0x2FC@(x,y,z)", for the [mp-obj] log lines.
    std::string str() const;
};

/* ---- The interface network_manager drives (agent N wires these). ---- */

/// One object packet (ids 20-25). `r` is positioned just after the PacketId byte.
/// `fromPlayerId` is the player id of the peer the packet came from: on a client that is always
/// the host's, even for a packet the host relayed from another client, so ownership checks are only
/// made on the host (where it is the true originator).
void on_packet(PacketId id, Reader& r, std::uint32_t fromPlayerId);

/// Once per sim tick, AFTER drive_puppets() in the same tick (it reads every authority's
/// played_tick()). Owner side: steps the debug object, drops objects a scene change destroyed,
/// streams ObjState batches. Receiver side: owner-timeline playback, scoping, ghost upkeep,
/// tombstone expiry, and the Placed-identity census. Recommended spot: post_actor_tick, before
/// trace::write_tick and the flush, so the state sent is the one this tick ended at.
void tick();

/// A player left: despawn every object they were authority over and release their claims (the
/// host also broadcasts the releases). Logged as "[mp-obj] peerleft despawn".
void on_peer_left(std::uint32_t playerId);

/// Session over: destroy every ghost and forget everything, owned and remote.
void reset();

/**
 * Host only, for a packet from a client: may it be RELAYED verbatim to the other clients?
 *  - 20-23: true iff the sender is the object's authority (Transient owner == sender; Placed ref
 *    claimed by the sender). The host should then both on_packet() it and forward it, same
 *    channel and reliability (21 unreliable on kChannelState, the rest reliable on
 *    kChannelObjects). False: drop it, do not apply it either (on_packet re-checks anyway).
 *  - 24 (ObjClaim): false. It is host-bound; the host on_packet()s it and answers with a
 *    host-originated 25, it is never forwarded.
 *  - 25 from a client: false, and the host must not apply it (on_packet ignores it on the host).
 * `r` by value so it can be peeked without consuming the caller's reader.
 */
bool relay_allowed(PacketId id, Reader r, std::uint32_t fromPlayerId);

/* ---- Owner-side API, for the phase-1 capture files. ---- */

/// Start the phase-0 debug object: one transient 150 units in front of the local Link that orbits
/// him for `ticks` sim ticks and then despawns. False (and logged) when there is no session or no
/// Link to orbit. The autopilot's `objtest <ticks>`.
bool start_debug_object(int ticks);

/// Register a new transient this machine owns, and send its ObjSpawn. Returns false (no ref) when
/// there is no session, no local location yet, or the init blob is too long.
bool spawn_transient(ObjKind kind, const ObjPose& pose, const KindBlob& init, ObjRef& out);

/// The owned object's state this tick. tick() decides whether it goes on the wire (changed, rate
/// class, forced sample). Unknown refs are ignored.
void update_owned(const ObjRef& ref, const ObjPose& pose, const KindBlob& state);

/// A reliable event on an owned object (stuck, explode, rest...).
void send_event(const ObjRef& ref, ObjEventId event, const KindBlob& blob);

/// The owned object is gone: reliable ObjDespawn, then forget it.
void despawn_owned(const ObjRef& ref, ObjDespawnReason reason);

/* ---- Claims (Placed objects). Plumbing only in phase 0; the policy is phase 2's. ---- */

inline constexpr std::uint32_t kNoClaim = 0xFFFFFFFFu;

/// Ask for authority over a Placed object. On the host this is arbitrated at once; on a client it
/// goes to the host and the answer arrives as an ObjClaimResult. First claim the host hears wins.
void request_claim(const ObjRef& ref, ClaimKind kind, std::uint32_t stageHash, std::int8_t room);
/// Give a claim up (ObjClaim with ClaimKind::Release).
void release_claim(const ObjRef& ref, std::uint32_t stageHash, std::int8_t room);
/// Who holds `ref` as far as this end knows, or kNoClaim.
std::uint32_t claim_holder(const ObjRef& ref);

/* --- batch 7: drops --- */

/// Receiver side: stop playing back the remote object `ref` and forget it at once (tombstoned, so
/// a late state cannot bring it back; a late despawn is logged as unknown). For a drop whose local
/// copy was just picked up or taken away: without this the channel would see its actor vanish and
/// rebuild it. `destroyActor` false leaves the actor alone (it is already on its way out, e.g. in
/// its pickup animation). No-op for an unknown ref.
void forget_remote(const ObjRef& ref, bool destroyActor);

/* ---- The engine seam. Declared here, implemented in src/d/actor/d_a_mp_ghost.cpp, the one
 * translation unit on this path that includes actor headers. Every function is safe with no
 * scene, no Link and no ghost. ---- */
namespace engine {

/// The local Link's position and shape yaw. False when there is no Link.
bool local_link_pose(float pos[3], std::int16_t& yaw);

enum class GhostLookup : std::uint8_t {
    Gone = 0,      ///< no such process (never made, deleted, or its create failed)
    Creating = 1,  ///< the process exists but create() has not finished
    Alive = 2,
};

/// Request a ghost actor (fpcNm_MP_GHOST_e) for one object, created in the local Link's room.
/// Returns the process id, or 0xFFFFFFFF when the actor manager refused or there is no Link.
std::uint32_t create_ghost(std::uint8_t kind, std::uint32_t authority, const ObjPose& pose);

GhostLookup look_up_ghost(std::uint32_t procId);

/// Push one played-back sample onto a live ghost. `visible` false hides it (room not shown, not
/// yet reached on the timeline, kObjPoseHidden). Sets its matrix at once, so the ghost is drawn
/// where it was put on THIS frame whichever side of the actor pass the caller runs on.
void apply_ghost(std::uint32_t procId, const ObjPose& pose, bool visible, const KindBlob& blob);

/// Deliver an event to a live ghost when playback reaches it.
void ghost_event(std::uint32_t procId, std::uint8_t event, const KindBlob& blob);

void destroy_ghost(std::uint32_t procId);

/// The Placed-identity census (spec §6 phase 0): once per scene load, a few ticks after the world
/// becomes playable, log every daObjCarry_c, daCstatue_c and horse with its (stage, room, setID,
/// procname, home). Called from tick(); keeps its own edge state.
void census_tick();

/* --- batch 7: drops ---
 * ObjKind::Drop has no ghost: the receiver's copy is a real daItem_c the local player can pick up.
 * The channel calls these instead of create_ghost / look_up_ghost / apply_ghost for that kind
 * (destroy_ghost is generic). Implemented in replication/object_capture_drop.cpp. */

/// Create the local copy of a remote drop (a daItem_c in the local Link's room). `blob` is the
/// played-back DropBlob. Returns the process id, or 0xFFFFFFFF.
std::uint32_t create_drop(const ObjRef& ref, const ObjPose& pose, const KindBlob& blob);

GhostLookup look_up_drop(std::uint32_t procId);

/// Put the copy where the owner's drop is on the played-back timeline. Ignored once the local
/// copy is being picked up or removed.
void apply_drop(std::uint32_t procId, const ObjPose& pose, bool visible);

}  // namespace engine

}  // namespace dusk::mp::objects
