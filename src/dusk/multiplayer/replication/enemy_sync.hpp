#pragma once

#include <cstdint>
#include <vector>

#include "../net/packet.hpp"
#include "../net/serializer.hpp"

/**
 * M6 enemy sync, phase 1 (batch 8, agent E2): the Dusk core behind dusk/mp_enemy.hpp.
 * Spec: .claude/future-plans/mp-m6-enemies.md (§2 authority, §3 identity, §4 state, §5.6 aggro).
 *
 * The session layer (network_manager.cpp) drives it; the decomp seams (f_op_actor.cpp) call the
 * mp_enemy.hpp functions; per-enemy adapters (src/d/actor/d_a_e_<x>_mp.inc) register themselves.
 * Engine-free header: the .cpp files read the engine directly, like presence.cpp.
 *
 * Everything here is inert outside a live session or with the cvar multiplayer.enemySync off: the
 * registry still records adapted enemies as they are created (a map insert, invisible), so a
 * session that starts mid-stage finds them, but no enemy changes behaviour.
 *
 * ---- Identity ----
 * An enemy's network identity is its PLACEMENT KEY, enemy_placement_key(): a hash of (stage,
 * layer, room, chunk tag, index in chunk), the same on every machine that created the same stage
 * entry. It is sent whole (u32) instead of a 2-byte netId: there is then nothing to allocate or
 * agree on. Each record also carries a CHECK hash of (profName, parameters, home.pos rounded); a
 * ghost whose check differs from the authority's is degraded to local-only on both ends (§3).
 * Runtime-spawned enemies (key 0) are not synced in phase 1.
 *
 * ---- Authority (§2.1, per (stageHash, layer, room)) ----
 * Every peer reports its loaded rooms (RoomsLoaded). Only the host computes the table: the host if
 * it has the room loaded, else the player who already owns it while he still has it (sticky), else
 * the lowest player id that has it. Rule 1 (§2.3): when the host loads a room a guest owns, the
 * switch to the host waits while any of the host's ghosts there was reported engaged in the last
 * 90 ticks, for at most 300 ticks. Rule 2: an owner who no longer has the room loaded (warped,
 * died, left) is replaced at once. Every ownership change bumps that room's epoch (kept per room
 * for the whole session, so a room that empties and refills never reuses one).
 *
 * ---- Wire layouts (each packet is [u8 PacketId][payload]) ----
 *   EnemyAuthority 28, host -> all, reliable kChannelObjects. The whole table:
 *       [u8 n] n x {[u32 stageHash][s8 layer][s8 room][u8 owner][u16 epoch]}
 *   EnemyUpdate 29, authority -> host -> others, unreliable kChannelState. One room, one tick:
 *       [u8 authority][u64 originTick][u32 stageHash][s8 layer][s8 room][u16 epoch][u8 n]
 *       n x entry:
 *         [u32 key][u8 eflags]                    eflags: 1 check present, 2 engaged, 4 anm valid
 *         [u32 check]                             only with eflags & 1 (first 30 sends, then 1/30)
 *         [f32 x][f32 y][f32 z]                   current.pos, absolute
 *         [s16 shapeX][s16 shapeY][s16 shapeZ][s16 angleY]   shape_angle, current.angle.y
 *         [u16 anm][u16 frame*16][s16 rate*256][u8 attr][u8 morph]   only with eflags & 4
 *         [s16 health][u16 mFlags][u8 targetPeer] targetPeer 0xFF = none
 *         [u8 extraLen][extraLen bytes]           the adapter's EnemyExtra (<= 24)
 *       39 bytes + extra per enemy without the check and with an anm; split at ~1100 bytes.
 *   RoomsLoaded 30, peer -> host (relayed to the others), host -> all; reliable kChannelObjects:
 *       [u32 playerId][u32 stageHash][u8 n] n x {[s8 room][s8 layer]} [u8 m] m x {[u32 key]}
 *       Room -1 (stage-level actors) is listed whenever the sender is in a stage. The m keys are
 *       enemies the sender degraded to local-only since its last RoomsLoaded; the receiver
 *       degrades its own copy too ("on both ends").
 *   --- batch 9 (phase 2): hits. Both go attacker -> the room's authority, reliable
 *   kChannelObjects: direct when the host is one end, else guest -> host -> the other guests (only
 *   the addressed authority acts; the host relays only a guest speaking for itself). ---
 *   EnemyHit 31:
 *       [u8 attacker][u8 authority][u32 key][u16 epoch][u8 tgIndex]
 *       [u32 atType][u8 atp][u8 atSpl][u8 atMtrl][u8 atSe][u8 atHitMark]
 *       [f32 x][f32 y][f32 z]                   the cross (hit) position
 *       [u8 hitType][u32 hitBit][s8 hitStatus]  at_power_check on the attacker's machine
 *       [u8 attackerFlags][u8 cutType][s16 shapeAngleY][s16 attackerLife]
 *       attackerFlags: 1 master sword, 2 wood sword, 4 wolf, 8 sword-up, 0x10 head jump,
 *                      0x20 cut-jump-cancel turn, 0x40 riding
 *       The authority validates (epoch, TG set, alive, distance), then injects it before its next
 *       actor pass as a proxy AT collider owned by the attacker's puppet (§5.1).
 *   EnemyFlagHit 32:
 *       [u8 attacker][u8 authority][u32 key][u16 epoch][u16 bit]   bit: CutDownHit or WolfBite
 *
 * ---- Playback ----
 * A ghost plays the authority's samples on the authority's own pose timeline: the tick its puppet
 * is being shown at (StateBuffer::played_tick, the object channel's rule), so the enemy and the
 * player it fights stay in step on the screen of whoever watches both. Position and angles are
 * interpolated; animation index, frame, HP, flags and the extra are discrete. The anm is SET on a
 * change and its frame corrected when it is more than one frame off (§4.1).
 */

namespace dusk::mp::enemies {

/// Not a player: no owner, no target.
inline constexpr std::uint32_t kNoPlayer = 0xFFFFFFFFu;

/// Once per sim tick in pre_actor_tick, AFTER drive_puppets (the ghosts play on the authorities'
/// puppet timelines). Only while the session is live. Sends this end's RoomsLoaded on change, on
/// the host recomputes and broadcasts the authority table, updates every enemy's mode, plays the
/// ghosts' samples forward and refills the target stand-ins.
void pre_tick();

/// Once per sim tick in post_actor_tick while the session is live, before trace::write_tick: the
/// authority captures and sends its enemies' states, the trace rows and periodic stats are
/// written, and slot 0 is checked to be Link again.
void post_tick();

/// One of EnemyAuthority / EnemyUpdate / RoomsLoaded, `r` just after the id byte. True on the host
/// when the packet is valid and should be relayed verbatim to every other guest (same channel and
/// reliability it came on).
bool on_packet(PacketId id, Reader& r, std::uint32_t fromPlayerId);

/// Host: a guest was admitted (resend our RoomsLoaded so it hears it). Any end: a player left.
void on_peer_joined(std::uint32_t playerId);
void on_peer_left(std::uint32_t playerId);

/// Session over: every enemy back to local, every stand-in gone, slot 0 back on Link. The
/// registry of live enemies is kept (they still exist).
void reset();

/* ---- Read-only views, for the autopilot (enemynear / expectenemy / expecttarget ...). ---- */

enum class Mode : std::uint8_t {
    Local = 0,      ///< vanilla: not synced (no session, no authority known, degraded, cvar off)
    Authority = 1,  ///< this end runs its AI and streams it
    Ghost = 2,      ///< driven from the network, AI off
};

const char* mode_name(Mode mode);

struct EnemyView {
    std::uint32_t key = 0;
    std::uint32_t procId = 0;
    std::int16_t prof = 0;
    Mode mode = Mode::Local;
    bool degraded = false;
    std::uint32_t authority = kNoPlayer;
    std::uint16_t epoch = 0;
    /// The authority's chosen target as a player id: on the authority its own selection, on a
    /// ghost the targetPeer last received. kNoPlayer = none / not known.
    std::uint32_t target = kNoPlayer;
    float pos[3] = {0.0f, 0.0f, 0.0f};
    std::int16_t health = 0;
    /// The anm resource index the morf is drawing now (-1 = unknown), and its frame.
    int anm = -1;
    float frame = 0.0f;
    std::uint32_t ghostTicks = 0;
    /// Ticks this enemy was a ghost and still ran its own execute (must stay 0).
    std::uint32_t realExecWhileGhost = 0;
    std::uint32_t swapTicks = 0;
    std::uint32_t absorbedWrites = 0;
    std::uint32_t realLinkWrites = 0;
    /* batch 9: enemy hits. Ghost: sent. Authority: received / injected / rejected. */
    std::uint32_t hitsSent = 0;
    std::uint32_t hitsRecv = 0;
    std::uint32_t hitsInjected = 0;
    std::uint32_t hitsRejected = 0;
};

/// Every registered (adapted, placed) enemy, created and alive.
std::vector<EnemyView> views();

struct RoomOwnerView {
    std::uint32_t stageHash = 0;
    std::int8_t layer = 0;
    std::int8_t room = -1;
    std::uint32_t owner = kNoPlayer;
    std::uint16_t epoch = 0;
};

/// The authority table as this end knows it (host: computed; guest: last EnemyAuthority).
std::vector<RoomOwnerView> authority_table();

}  // namespace dusk::mp::enemies
