#pragma once

#include <cstdint>
#include <string>

#include "../net/serializer.hpp"

/**
 * Presence: where every player is, by NAME (stage, room, layer, spawn point, save table) rather
 * than by the pose's stage hash, plus a few coarse "what is he doing" bits. Spec:
 * .claude/future-plans/mp-m4-m5-world.md I.3 (packet) and I.4 (join-partner warp).
 *
 * Wire payload of PacketId::Presence (the id byte itself is written by NetworkManager):
 *
 *   [u32 playerId][char stage[8]][s8 room][s8 layer][s16 point][s8 saveTbl][u8 flags]
 *
 * Sent reliably on change, and once more after a peer joins (force_resend). The host relays it to
 * every other guest unchanged.
 *
 * The pose already carries a stage HASH, which is enough to hide a puppet but not to name a place
 * or to warp to it: resolving a hash would mean hashing every known stage. Presence is the
 * named, low-rate counterpart.
 *
 * Game thread only, like the rest of the session layer. Unlike most of src/dusk/multiplayer, the
 * .cpp reads the engine directly (capture and the warp both need it); nothing here exposes an
 * engine type.
 */

namespace dusk::mp::presence {

struct Info {
    std::uint32_t playerId;
    /// dStage_startStage_c::mName: NUL-padded, but NOT necessarily NUL-terminated (8 chars fit).
    char stage[8];
    /// The room his Link stands in; the start room while he has no Link. -1 = unknown.
    std::int8_t room;
    std::int8_t layer;
    std::int16_t point;
    /// dStage_stagInfo_GetSaveTbl of his stage: which dSv_info_c::mSave[] slot is live for him.
    /// -1 while loading.
    std::int8_t saveTbl;
    std::uint8_t flags;
    bool valid;
};

/// No Link, or a scene change is queued or in flight (local_scene_change_queued), or the title.
constexpr std::uint8_t kFlagLoading = 1;
/// A Link exists but the world is not playable: an event, a cutscene, a conversation.
constexpr std::uint8_t kFlagInEvent = 2;
constexpr std::uint8_t kFlagWolf = 4;
/// PROC_DEAD, or no hearts left: dying, game over, or about to respawn.
constexpr std::uint8_t kFlagDead = 8;
/// His stage is a boss room (stagInfo ST_BOSS_ROOM, the test dStage_isBossStage makes). Added
/// beyond the four spec bits because only the SENDER has that stage's data loaded: the joiner
/// cannot answer "is it a boss stage" for a stage it is not in.
constexpr std::uint8_t kFlagBossStage = 16;

/// No such player. Distinct from 0, which is kHostPlayerId.
constexpr std::uint32_t kNoPlayer = 0xFFFFFFFFu;

/// Captures the local presence; when it differs from the last one sent (or nothing was sent since
/// reset() / force_resend()), writes the payload (WITHOUT the packet id byte) and returns true.
/// Call it every sim tick: changes are rate-limited here, and a change held back by the limit
/// goes out on a later call rather than being lost.
bool build_if_changed(Writer& w);

/// Parse a Presence payload. On the host, the sender's identity is `fromPlayerId` (a guest cannot
/// speak for anyone else); on a guest, everything comes from the host, which relays the payload's
/// own playerId verbatim, so that one is used.
void on_packet(Reader& r, std::uint32_t fromPlayerId);

/// Host: write the payload of `playerId`'s stored presence (as on_packet corrected it), WITHOUT the
/// packet id byte; false when none is stored. Two uses: relaying a guest's Presence with the id
/// on_packet enforced rather than the raw bytes (which could name someone else), and telling a
/// newly joined guest where the OTHER guests already are, which force_resend() cannot do.
bool write_stored(Writer& w, std::uint32_t playerId);

void on_peer_left(std::uint32_t playerId);

/// Forget everything: every peer's presence and what we last sent. Session start/end.
void reset();

/// Make the next build_if_changed() send even if nothing changed, bypassing the rate limit. For
/// "a new peer joined": the host's and the other guests' presence must reach it once.
void force_resend();

/// A remote player's last presence, or nullptr when none has arrived.
const Info* get(std::uint32_t playerId);

/// The remote player with the lowest id (on a guest, the host), or kNoPlayer when there is none.
/// For the autopilot's `joinpartner` verb.
std::uint32_t first_remote_player_id();

/// Why join_partner(playerId) would be refused right now, in words for the player; nullptr when it
/// would be allowed. Cheap enough to call per frame (the UI disables the Join button with it).
const char* join_partner_blocker(std::uint32_t playerId);

/// The join-partner warp (spec I.4) to `playerId`; returns false + logs the reason when gated.
bool join_partner(std::uint32_t playerId);

/// "Ordon Village, room 1 (wolf)" for a remote player, "location unknown" when no presence has
/// arrived. For the session roster.
std::string describe(std::uint32_t playerId);

}  // namespace dusk::mp::presence
