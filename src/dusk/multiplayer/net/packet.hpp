#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Packet identifiers and protocol constants.
 *
 * Every packet is [u8 PacketId][payload...]. Milestone 0 only defines the handshake and
 * heartbeat; player state, world events and scene lockstep are added at M1/M4/M5.
 */

namespace dusk::mp {

/// Bumped on ANY incompatible wire change. Peers with a mismatched version are rejected at
/// handshake rather than being allowed to desync in confusing ways later.
/* 4: PlayerState grew the sender's outfit byte (19 -> 20) and claimed flag bit 2 for the sharp
 * turn. The size change is the reason this MUST be bumped: a v3 peer's read() under-runs on the
 * trailing flags byte, returns false, and every pose packet is silently discarded — a session that
 * connects fine and then shows nobody moving.
 *
 * 5: PlayerState::speed changed MEANING — it is the sender's daAlink_c::mNormalSpeed now, his
 * intended speed, where it used to be speedF, his root-motion-blended translation speed. Same size,
 * same type, so a v4 peer would parse it perfectly and simply animate wrong: every remote player
 * would walk visibly slower than the person driving them. Bumped anyway, deliberately. A rejected
 * handshake says what is wrong in one line; "everyone's legs are a bit off" is the kind of thing
 * that gets diagnosed twice and blamed on something else both times.
 *
 * 6: the same four bytes stopped being a speed at all. They now carry the sender's own
 * getMoveGroundAngleSpeedRate() — the dimensionless value daAlink_c blends his gaits from — and
 * flag bit 3 carries his "standing" predicate. Still 20 bytes, so once again a v5 peer would parse
 * it perfectly: it would read a number between 0 and 1 as a speed in engine units, decide every
 * remote player is standing still, and never animate one again. Rejecting the handshake turns a
 * silent, total misbehaviour into one line.
 *
 * 7: flag bits 4 and 5, for the foot IK — the sender's MODE_IDLE and his own "no foot IK this
 * tick" test. Same 20 bytes again. A v6 peer would leave both clear, which is the safe reading
 * ("moving, IK allowed") but the wrong one half the time: standing puppets would not settle onto
 * the lower foot and airborne ones would try to plant their feet on the floor below them.
 *
 * 8: PlayerState grew an equipment byte (20 -> 21) — which sword and shield, and whether each is in
 * a hand or on the back. A SIZE change, so this is the mandatory kind of bump rather than the
 * cautious kind: a v7 peer's read() under-runs on the new trailing byte and discards every pose
 * packet, which looks like a session that connects and then shows nobody moving. The byte is filled
 * completely from this version, including the shield half the puppet does not draw yet, so
 * finishing the shield will not need a second bump.
 *
 * 9: PlayerState grew an idle byte (21 -> 22) — which of daAlink_c's idle animations the sender is
 * actually playing, sampled off the animation heap rather than re-derived from his flags. Another
 * SIZE change, so this is the mandatory kind of bump: a v8 peer's read() under-runs on the new
 * trailing byte, returns false, and discards every pose packet — the same "connects fine, nobody
 * moves" failure as versions 4 and 8. It is an enumeration and not more flag bits because the
 * variants are mutually exclusive by construction (setBlendMoveAnime picks exactly one), and a byte
 * has room for all 256 of them where only eight are named so far — so the remaining idle variants,
 * and the rest of D3, cost no second bump.
 *
 * 10: the shared world clock. Two changes at once, and only one of them would have been caught by a
 * size check. WorldSnapshot grew a trailing [f32 daytime][u16 date] — a SIZE change, the mandatory
 * kind of bump. PlayerState claimed flag bit 6 for "time can pass where I am" at NO size cost,
 * which is the dangerous kind: a v9 peer parses the packet perfectly and simply leaves the bit
 * clear, and under the AllPlayers rule a permanently-clear bit means the sun never moves again for
 * anybody. Silent, total, and it would read as "time sync doesn't work" rather than as a version
 * mismatch. Also adds TimeOverride (client -> host), which an older host would drop as an unknown
 * id — so a guest's cutscene would set the time for itself and be quietly overwritten a tick later.
 */
inline constexpr std::uint32_t kProtocolVersion = 10;

/// Default UDP port. Chosen to sit clear of common web-dev ports.
inline constexpr std::uint16_t kDefaultPort = 7777;

/// Channel 0 is reliable/ordered control traffic; channel 1 is unreliable state snapshots
/// (a dropped snapshot is always superseded by the next one, so resending it is wasted latency).
inline constexpr std::uint8_t kChannelControl = 0;
inline constexpr std::uint8_t kChannelState = 1;
inline constexpr std::size_t kChannelCount = 2;

enum class PacketId : std::uint8_t {
    Invalid = 0,

    /// Client -> host, first thing after connecting. colorExplicit tells the host whether the
    /// player actually chose the colour (and so must keep it) or is just carrying a default.
    /// [u32 protocolVersion][string nickname][u8 colorExplicit][u32 preferredColorRgb]
    Hello = 1,
    /// Host -> client. The host assigns the colour authoritatively so no two players collide,
    /// and includes its own identity so both ends know each other after one exchange.
    /// [u8 accepted][u32 protocolVersion][u32 assignedPlayerId][u32 assignedColorRgb]
    /// [string hostNickname][u32 hostColorRgb]
    HelloAck = 2,
    /// Either direction. [u32 sequence][u64 senderTick]
    Heartbeat = 3,
    /// Echoes the sequence and tick it is replying to. [u32 sequence][u64 senderTick]
    HeartbeatAck = 4,

    /// Client -> host, every sim tick on the unreliable channel. The player's own pose only;
    /// the host is the one that fans state out. [u64 senderTick][PlayerState]
    PlayerStateUpdate = 5,
    /// Host -> clients, every sim tick on the unreliable channel. Carries every player including
    /// the recipient's own entry (which the recipient skips) so the packet is identical for all
    /// peers and can be serialized once and broadcast.
    ///
    /// Each entry carries its OWN originTick — the tick that player captured the pose on, in their
    /// own clock — rather than being described by the snapshot's hostTick. Poses only interpolate
    /// correctly on the clock they were produced on; re-stamping them on relay distorts speed.
    ///
    /// The trailing clock is the shared world time, host-authoritative. It rides here rather than
    /// in its own periodic packet because it is small, because it is exactly the state the snapshot
    /// already exists to fan out, and because sending it at the sim rate means a guest that misses
    /// one is corrected on the next tick instead of drifting until a timer fires.
    /// [u64 hostTick][u8 count][{u32 playerId, u64 originTick, PlayerState} * count]
    /// [f32 daytime][u16 date]
    WorldSnapshot = 6,

    /// Host -> client, reliable. The full roster at join time, so a client learns about players
    /// who were already connected — not just the host.
    /// [u8 count][{u32 playerId, string nickname, u32 colorRgb} * count]
    PeerList = 7,
    /// Host -> clients, reliable. [u32 playerId][string nickname][u32 colorRgb]
    PeerJoined = 8,
    /// Host -> clients, reliable. [u32 playerId]
    PeerLeft = 9,

    /// Client -> host, reliable. "Something on my machine SET the clock — adopt it."
    /// [f32 daytime][u16 date]
    ///
    /// ★ The point of this packet is that a shared clock must not be a one-way broadcast. Plenty of
    /// the game sets the time outright rather than letting it tick: a cutscene
    /// (`d_a_demo00.cpp:1198`), a stage's own entry time (`d_kankyo.cpp:1481`), sleeping
    /// (`:9621-9625`). If the HOST does one of those it propagates for free, because the host's
    /// absolute time is what everyone mirrors. If a GUEST does one, the very next snapshot would
    /// stomp it — so "completing Arbiter's Grounds sets the time to night" would silently fail for
    /// anyone but the host. This is the path back up.
    ///
    /// Reliable, because these are rare, discrete, and losing one loses the event's whole point.
    TimeOverride = 10,
};

/// The host occupies a fixed player id so both ends can name it without a lookup.
inline constexpr std::uint32_t kHostPlayerId = 0;

/// Ceiling on players in one snapshot. Keeps WorldSnapshot inside a single unfragmented datagram
/// (8 * 32 bytes + header is comfortably under the ~1200-byte safe MTU).
inline constexpr std::size_t kMaxPlayers = 8;

}  // namespace dusk::mp
