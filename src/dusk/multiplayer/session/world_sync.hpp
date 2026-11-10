#pragma once

#include <cstdint>

#include "../net/packet.hpp"
#include "../net/serializer.hpp"

/**
 * The M5 shared-world sync loop: the network half of replication/world_state.hpp.
 * Spec: .claude/future-plans/mp-m4-m5-world.md II.4, II.5 and II.8.
 *
 * ★ The host owns the world. Its own diffs become commits at once. A guest's diffs are only
 * proposals: the host judges them (world::judge_proposal), applies the accepted ones to its own
 * game, and broadcasts each one as a WorldCommit, in worldSeq order, to every guest including the
 * proposer. A refused proposal is answered with a fresh WorldSync instead of being saved. That is
 * the host's protection against a guest bug that would otherwise wipe its story flags (II.8 b).
 *
 * States, per process: Unsynced (no world, a reset, or not yet installed) and Synced (diffing every
 * tick). A guest only diffs, proposes and applies commits while Synced. It gets there with a
 * WorldSync from the host, which it requests itself as soon as it has a world to put it in.
 *
 * Wire layouts (all reliable, kChannelControl):
 *   WorldSync         host -> guest  [u32 worldSeq][WorldView]
 *   WorldPropose      guest -> host  [u32 proposeSeq][ops]
 *   WorldCommit       host -> all    [u32 worldSeq][u32 originPlayer][u32 ackProposeSeq][ops]
 *   WorldSyncRequest  guest -> host  [u32 lastAppliedSeq][u8 reason]
 *   WorldDigest       host -> all    [u32 worldSeq][u32 digest]           every 60 ticks
 *
 * Everything runs on the game thread: on_packet from NetworkManager::handle_packet (pre-actor),
 * tick() from NetworkManager::post_actor_tick.
 */

namespace dusk::mp::world_sync {

/// Once per sim tick, after the actor pass, while a session exists. Diffs, commits or proposes,
/// sends the periodic digest, and moves between Unsynced and Synced.
void tick();

/// Packets 13-16 and 19. @p fromPlayerId is the sender's world identity (the host is
/// kHostPlayerId).
void on_packet(PacketId id, Reader& r, std::uint32_t fromPlayerId);

/// Host: forget a guest's proposal sequence and any WorldSync it was owed.
void on_peer_left(std::uint32_t playerId);

/// True while this end is diffing against a world the host agrees with.
bool synced();

/// Host: the last worldSeq it committed. Guest: the last one it applied.
std::uint32_t world_seq();

/// The digest of the last synced view (the shadow), with the local-only stage-table-6 key count
/// masked out. 0 while there is no shadow.
std::uint32_t current_digest();

/// The autopilot `worldlog` verb. Writes grep-able lines, each starting with "[mp-world] <tag>":
///   - role=host|guest seq=N synced=0|1 digest=0x........ liveTbl=T src=engine|shadow|none
///   - counters rupee=R maxLife=L poh=P keyNum=K (K for liveTbl, -1 when none)
///   - event: i i i ...
///   - membit tbl=T[ live]: i i i ... (one line per table with a bit set, and the live one)
///   - mem2 tbl=T: i i ... (one line per table with a bit set)
///   - firstget: i i ...
///   - items: i i ...
/// A bit index is byte * 8 + bit, bit 0 being mask 0x01. For a little-endian u32 switch array that
/// is the switch number itself. The keyNum byte is left out of membit.
void log_state(const char* tag);

/// Session over: back to Unsynced with nothing remembered.
void reset();

}  // namespace dusk::mp::world_sync
