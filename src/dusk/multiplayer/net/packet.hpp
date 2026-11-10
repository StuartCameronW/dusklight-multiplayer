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
 *
 * 11: every hang-up now carries a DisconnectReason in ENet's disconnect data, and a host refuses
 * a client it will not admit (wrong version, session full) by hanging up on it with the reason
 * instead of leaving it connected and half-admitted. No packet changed size, so nothing would fail
 * to parse — a v10 peer would simply read every reason as 0, "connection lost", and tell its
 * player the wrong thing. Bumped so that never happens silently.
 *
 * 12: PlayerState grew a trailing `atnKind` byte (22 -> 23 bytes): the sender's attention /
 * lock-on move set and its direction, so the puppet can play the ATN stance and strafe (D3b). A
 * size change, so the bump is mandatory. A v11 peer's read() would under-run and drop every pose.
 *
 * 13: flags bit 7 is now kPlayerStateUpperGuard (the sender's checkUpperGuardAnime()), so the
 * puppet raises its shield in the stance and strafe. No size change, so a v12 peer would parse
 * fine and simply never show a guard. Bumped so the mismatch is refused instead of silent.
 * 14: PlayerState grew six head-turn / torso-lean bytes after `atnKind` (23 -> 29): neckX, neckY,
 * bodyX, bodyTwist, bodyZ, bankZ, each an s8 of (angle / 128).
 * In the same version, block (2): 18 bytes of action-animation override after those six
 * (actUnder, actUpper, actFrame, actUpperFrame, actRate, actMorf, actHandL, actHandR,
 * actRootX/Y/Z), so the puppet plays the sender's own BCK during swings, rolls and jumps. 23 -> 47
 * bytes so far.
 *     Then a trailing `gear` byte after everything else (offset 47), bit 0 the sender's
 *     checkEquipHeavyBoots() (iron boots on), because `flags` and `equip` were both full.
 *     23 -> 48 bytes in all. Still 14: none of it has shipped.
 *
 * 15: three changes, each enough on its own. Hello and HelloAck each grew a trailing
 * ModFingerprint block, and DisconnectReason gained ModMismatch = 5: a v14 host would ignore the
 * block and admit a guest with different gameplay mods, and a v14 client would report the refusal
 * as "unknown reason". PlayerState grew 48 -> 70 bytes, all after `gear`, in this order:
 * `locPhase` (offset 48, the sender's stride phase), block (4) action slot 1 / swim (12 bytes:
 * the second animation of a double anime, the joint-0 rotation, the climb yaw offset and the
 * whole-model pitch), then block (3) held item (9 bytes: the item model in his hands and its
 * UPPER_2 arm overlay). Block (2)'s actRoot* also changed meaning in this version: it is now
 * measured against the base the RECEIVER builds, not Link's own, so a v14 puppet would misplace
 * the body even if the sizes matched.
 */
/*
 * 16: PlayerState grew 70 -> 76 bytes, appended after the held-item block: u32 stageHash
 * (offset 70, FNV-1a of the sender's stage name), s8 roomNo (74, his Link's room) and u8
 * locEpoch (75, bumped whenever his Link actor is re-created, i.e. every scene load). The
 * receiver resets the interpolation buffer on an epoch change, so a puppet snaps to the new spawn
 * instead of gliding across the map, and despawns the puppet while the sender is in a stage or a
 * room this end is not drawing. A size change: a v15 peer's read() would under-run and drop
 * every pose.
 */
/*
 * 17: no size change (still 76 bytes), but block (2)'s actHandL / actHandR change meaning. They
 * used to be filled only while the action override was on (kPlayerActBodyHand otherwise). Now
 * they carry the hand shapes the sender is SHOWING on every tick, and kPlayerHandUnknown (0xFF)
 * while his status window is open. The receiver draws them in every state. A v16 sender would
 * send 0xFE outside actions, and a v17 puppet would take that as "the body's plain hands" and
 * drop the sword and shield grips.
 */
/*
 * 18: the ModFingerprint block (Hello, HelloAck) grew a trailing session-value list,
 * [u8 k]{string modId, string label, u8 known, string value}*k — per-save state a mod declares
 * must match, the randomizer's seed hash first among them. A v17 peer would stop reading before it
 * and admit two players on different seeds. Also adds ModsUpdate (11, either direction), which
 * re-sends the fingerprint when it changes mid-session (a save loaded, a mod toggled) so the host
 * can re-check an admitted guest; a v17 peer would log it as an unknown id and never re-check.
 */
/*
 * 19: PlayerState grew a trailing attack block after locEpoch: u8 atMask, and while it is non-zero
 * u8 atRadius, u8 atSpl and 9 x s16 points (the sender's live sword hitbox, relative to his
 * position), so 77 bytes idle and 97 mid-swing. The receiver turns it into a hitbox on the puppet
 * that only the local player's hurtbox can touch: friendly fire. A v18 peer's read() would stop a
 * byte short and drop every pose.
 *   Then 2 bytes for the bow, always: u8 heldBck (bits 0-3 which BCK animates the bow / slingshot
 * model itself, daAlink_c's mItemBck; bit 4 "the nocked arrow is a bomb arrow") and u8 heldBckFrame
 * (that BCK's entered frame / frameMax, x255). heldFlags bit 7 (kPlayerHeldArrowNocked, previously
 * always 0) now means an arrow is nocked and drawn on the left item joint.
 *   Then, while heldItem is kPlayerHeldIronBall (9, new in v19), 12 more bytes: the ball's chain end relative to the sender (3 x s16 at 1/4 unit), its model rotation (2 x s16),
 * the paid-out link count and the throw state (u8 each). gear bit 2 is kPlayerGearIronBallGait,
 * the carry gait read off his slot 1.
 */
/*
 * 20: PlayerState grew a 2-byte effects block after the iron ball block, always on the wire: u8
 * fxFlags (lantern lit, sword trail, spin, sword charging, damage tint kind) and u8 dmgFlash (the
 * damage-flash timer). A v19 peer's read() would stop two bytes short and drop every pose.
 *   Then, while heldItem is a clawshot (4 or 5), the clawshot block: u8 clawFlags (which tips are
 * off their guns, wait, roof hang, chain-wobble stop time), u8 x 2 tip-open frames, and per tip
 * that is out 3 x s16 position relative to the sender (1/4 unit) + 2 x s16 rotation: 3, 13 or 23
 * bytes. heldBck id 10 is the gun's HS_SPIN, and heldFlags' kPlayerHeldClawRight is now also sent
 * for the single clawshot.
 */
/*
 * 21: PlayerState grew two blocks after the clawshot block, both always on the wire. The event
 * block: u8 evt (the sender's event kind, a stand-in bit, a hidden bit), plus u8 evtItem and u8
 * evtItemScale while the kind is an item get. The form block: u8 form (wolf transform phase, the
 * reload blob, the fur overlay, the shackle chain, Midna reserved), plus s8 metaTint and u8
 * furFrame while a transformation runs. flags' kPlayerStateWolf is now set for a wolf sender.
 * 2 bytes idle; a v20 peer's read() would stop two bytes short and drop every pose.
 */
inline constexpr std::uint32_t kProtocolVersion = 21;

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
    /// [ModFingerprint] — the sender's active mods (session/mod_check.hpp), so the host can refuse
    /// a guest whose gameplay mods differ from its own. Only parsed when the version matches.
    Hello = 1,
    /// Host -> client. The host assigns the colour authoritatively so no two players collide,
    /// and includes its own identity so both ends know each other after one exchange.
    /// [u8 accepted][u32 protocolVersion][u32 assignedPlayerId][u32 assignedColorRgb]
    /// [string hostNickname][u32 hostColorRgb][ModFingerprint]
    /// The host's own mod list rides along whether or not the guest was accepted, so a refused
    /// guest can name the differences itself, and an accepted one can log cosmetic differences.
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

    /// Either direction, reliable, only between admitted ends. [ModFingerprint]
    /// Sent when this end's fingerprint changes after the handshake: a save was loaded (which is
    /// when a randomizer seed becomes known), or a mod was enabled or disabled. The host re-checks
    /// the guest against its own and, on a gameplay difference, sends its own ModsUpdate (so the
    /// guest can name what differs) and hangs up with ModMismatch. A guest only records the host's.
    ModsUpdate = 11,
};

/**
 * Why a peer hung up, carried in ENet's 32-bit disconnect data — the one field that survives even a
 * protocol mismatch, because it is part of the transport's own goodbye rather than a packet either
 * side has to parse. That is what lets a rejected client tell its player WHY in their own words.
 *
 * 0 is what ENet reports for a timeout, so it means "the line went dead" and is never sent on
 * purpose. Values are append-only.
 */
enum class DisconnectReason : std::uint32_t {
    /// Nothing was said: a timeout, a crash, a pulled cable, or a peer that predates v11.
    ConnectionLost = 0,
    /// A deliberate leave: the player chose to, or quit the game. From the host, the session ended.
    Left = 1,
    /// Host -> client: protocol versions differ. HelloAck said so too, if the client could read it.
    VersionMismatch = 2,
    /// Host -> client: every player slot is taken.
    SessionFull = 3,
    /// Host -> client: connected but never introduced itself with a Hello.
    HandshakeTimeout = 4,
    /// Host -> client: the two ends run different gameplay mods (session/mod_check.hpp). The
    /// HelloAck before the hang-up carries the host's list, so the client can say which ones.
    /// Since v18 also sent mid-session, after a ModsUpdate, when the two ends stop matching.
    ModMismatch = 5,
};

/// The host occupies a fixed player id so both ends can name it without a lookup.
inline constexpr std::uint32_t kHostPlayerId = 0;

/// Ceiling on players in one snapshot. Keeps WorldSnapshot inside a single unfragmented datagram
/// (8 * 32 bytes + header is comfortably under the ~1200-byte safe MTU).
inline constexpr std::size_t kMaxPlayers = 8;

}  // namespace dusk::mp
