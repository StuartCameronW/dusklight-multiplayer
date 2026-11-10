#include "presence.hpp"

#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "../policy/mp_policy.hpp"
#include "../replication/player_bridge.hpp"
#include "../replication/player_state.hpp"
#include "../replication/replication_manager.hpp"
#include "dusk/logging.h"
#include "dusk/map_names.hpp"
#include "network_manager.hpp"

#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"

namespace dusk::mp::presence {
namespace {

constexpr borealis::Log Log{"dusk::mp"};

/// At most one Presence every this many sim ticks (~0.2 s at 30 Hz) unless forced. The event bit
/// can flicker for a few ticks around a conversation or a door; each change is a reliable packet,
/// and nobody reads a roster line at 30 Hz. A change held back here is sent on a later call.
constexpr std::uint32_t kMinTicksBetweenSends = 6;

/// How far behind the partner (against his facing) the join warp puts us, so the two bodies do not
/// spawn inside each other. Behind rather than beside: if he is walking, behind is ground he has
/// just stood on, which is the likeliest place in reach to be solid.
constexpr float kJoinBehindDistance = 100.0f;

/// The top byte of Link's parameters is his start event; 0xFF is "none", which is what an ordinary
/// spawn point carries (d_a_alink.cpp:4614-4653). Start mode 0 is a plain standing start.
constexpr int kJoinStartMode = 0;
constexpr int kJoinStartEvent = 0xFF;

std::unordered_map<std::uint32_t, Info> s_peers;
Info s_lastSent{};
bool s_haveSent = false;
bool s_forceSend = false;
std::uint32_t s_ticksSinceSend = std::numeric_limits<std::uint32_t>::max() / 2;

/// The 8-byte stage field as a C string. It need not be NUL-terminated (spec I.3).
struct StageName {
    char text[9] = {};
    explicit StageName(const char (&stage)[8]) { std::memcpy(text, stage, 8); }
};

bool same(const Info& a, const Info& b) {
    return a.playerId == b.playerId && std::memcmp(a.stage, b.stage, sizeof(a.stage)) == 0 &&
           a.room == b.room && a.layer == b.layer && a.point == b.point && a.saveTbl == b.saveTbl &&
           a.flags == b.flags;
}

/// Where the next log line can tell "moved" from "a flag flipped" without printing every flip.
bool same_place(const Info& a, const Info& b) {
    return std::memcmp(a.stage, b.stage, sizeof(a.stage)) == 0 && a.room == b.room;
}

/// The local Link, or nullptr when there is none or he is still being created (his flags and proc
/// are not meaningful yet; fpcM_IsCreating is the game's own test, d_a_alink_demo.inc:2541).
daAlink_c* live_alink() {
    daAlink_c* alink = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (alink == nullptr || fpcM_IsCreating(fopAcM_GetID(alink))) {
        return nullptr;
    }
    return alink;
}

bool local_is_loading() {
    return dComIfGp_getPlayer(0) == nullptr || fopAcM_SearchByName(fpcNm_TITLE_e) != nullptr ||
           local_scene_change_queued();
}

bool local_is_dead(daAlink_c* alink) {
    return alink != nullptr && (alink->mProcID == daAlink_c::PROC_DEAD || dComIfGs_getLife() == 0);
}

Info capture_local() {
    Info out{};
    out.playerId = network_manager().local_player_id();
    std::strncpy(out.stage, dComIfGp_getStartStageName(), sizeof(out.stage));
    out.room = dComIfGp_getStartStageRoomNo();
    out.layer = dComIfGp_getStartStageLayer();
    out.point = dComIfGp_getStartStagePoint();
    out.saveTbl = -1;
    out.flags = 0;
    out.valid = true;

    if (local_is_loading()) {
        // The stage data is only trustworthy once the new scene has its Link: before that,
        // stagInfo can still point into the previous stage's archive.
        out.flags |= kFlagLoading;
        return out;
    }

    fopAc_ac_c* link = dComIfGp_getPlayer(0);
    out.room = static_cast<std::int8_t>(fopAcM_GetRoomNo(link));
    out.layer = static_cast<std::int8_t>(dComIfG_play_c::getLayerNo(0));
    if (stage_stag_info_class* stag = dComIfGp_getStageStagInfo()) {
        out.saveTbl = static_cast<std::int8_t>(dStage_stagInfo_GetSaveTbl(stag));
        // dStage_isBossStage's own test (d_stage.cpp:70-84); that function is file-static.
        if (dStage_stagInfo_GetSTType(stag) == static_cast<u32>(ST_BOSS_ROOM)) {
            out.flags |= kFlagBossStage;
        }
    }
    if (!world_is_playable()) {
        out.flags |= kFlagInEvent;
    }
    daAlink_c* alink = live_alink();
    if (alink != nullptr && alink->checkWolf()) {
        out.flags |= kFlagWolf;
    }
    if (local_is_dead(alink)) {
        out.flags |= kFlagDead;
    }
    return out;
}

void write_payload(Writer& w, const Info& info) {
    w.write_u32(info.playerId);
    for (char c : info.stage) {
        w.write_u8(static_cast<std::uint8_t>(c));
    }
    w.write_u8(static_cast<std::uint8_t>(info.room));
    w.write_u8(static_cast<std::uint8_t>(info.layer));
    w.write_s16(info.point);
    w.write_u8(static_cast<std::uint8_t>(info.saveTbl));
    w.write_u8(info.flags);
}

}  // namespace

bool build_if_changed(Writer& w) {
    if (s_ticksSinceSend < std::numeric_limits<std::uint32_t>::max()) {
        ++s_ticksSinceSend;
    }
    const Info now = capture_local();
    if (!s_forceSend) {
        if (s_haveSent && same(now, s_lastSent)) {
            return false;
        }
        if (s_ticksSinceSend < kMinTicksBetweenSends) {
            return false;
        }
    }

    write_payload(w, now);
    if (!s_haveSent || !same_place(now, s_lastSent) || s_forceSend) {
        Log.info("Presence out: '{}' room {} layer {} point {} saveTbl {} flags {:#04x}{}",
            static_cast<const char*>(StageName(now.stage).text), now.room, now.layer, now.point,
            now.saveTbl, now.flags, s_forceSend ? " (resend)" : "");
    } else {
        Log.debug("Presence out: flags {:#04x}", now.flags);
    }
    s_lastSent = now;
    s_haveSent = true;
    s_forceSend = false;
    s_ticksSinceSend = 0;
    return true;
}

void on_packet(Reader& r, std::uint32_t fromPlayerId) {
    Info in{};
    std::uint8_t byte = 0;
    r.read_u32(in.playerId);
    for (char& c : in.stage) {
        r.read_u8(byte);
        c = static_cast<char>(byte);
    }
    r.read_u8(byte);
    in.room = static_cast<std::int8_t>(byte);
    r.read_u8(byte);
    in.layer = static_cast<std::int8_t>(byte);
    r.read_s16(in.point);
    r.read_u8(byte);
    in.saveTbl = static_cast<std::int8_t>(byte);
    r.read_u8(in.flags);
    if (!r.ok()) {
        Log.warn("Presence from player {}: truncated packet, dropped", fromPlayerId);
        return;
    }

    const NetworkManager& net = network_manager();
    if (net.role() == Role::Host) {
        // A guest only ever speaks for itself. The relay then carries this corrected id.
        if (in.playerId != fromPlayerId) {
            Log.warn("Presence from player {} claimed to be player {}; using {}", fromPlayerId,
                in.playerId, fromPlayerId);
            in.playerId = fromPlayerId;
        }
    }
    if (in.playerId == net.local_player_id()) {
        return;
    }
    in.valid = true;

    auto it = s_peers.find(in.playerId);
    const bool moved = it == s_peers.end() || !same_place(it->second, in);
    if (moved) {
        const StageName stage(in.stage);
        const char* mapName = lookup_map_name(stage.text);
        Log.info(
            "Presence in: player {} '{}' ({}) room {} layer {} point {} saveTbl {} flags {:#04x}",
            in.playerId, static_cast<const char*>(stage.text), mapName != nullptr ? mapName : "?",
            in.room, in.layer, in.point, in.saveTbl, in.flags);
    } else if (it->second.flags != in.flags) {
        Log.debug("Presence in: player {} flags {:#04x}", in.playerId, in.flags);
    }
    s_peers[in.playerId] = in;
}

bool write_stored(Writer& w, std::uint32_t playerId) {
    const Info* info = get(playerId);
    if (info == nullptr || !info->valid) {
        return false;
    }
    write_payload(w, *info);
    return true;
}

void on_peer_left(std::uint32_t playerId) {
    s_peers.erase(playerId);
}

void reset() {
    s_peers.clear();
    s_lastSent = Info{};
    s_haveSent = false;
    s_forceSend = false;
    s_ticksSinceSend = std::numeric_limits<std::uint32_t>::max() / 2;
}

void force_resend() {
    s_forceSend = true;
}

const Info* get(std::uint32_t playerId) {
    const auto it = s_peers.find(playerId);
    return it != s_peers.end() ? &it->second : nullptr;
}

std::uint32_t first_remote_player_id() {
    std::uint32_t best = kNoPlayer;
    for (const auto& [id, player] : replication_manager().players()) {
        if (id < best) {
            best = id;
        }
    }
    return best;
}

const char* join_partner_blocker(std::uint32_t playerId) {
    const NetworkManager& net = network_manager();
    if (!net.is_active()) {
        return "Not in a co-op session.";
    }
    if (!g_mpPolicy.allowJoinPartner) {
        return "Joining a partner is turned off for this session.";
    }
    if (playerId == kNoPlayer) {
        return "Nobody else is here.";
    }
    if (playerId == net.local_player_id()) {
        return "That is you.";
    }
    const auto& players = replication_manager().players();
    const auto playerIt = players.find(playerId);
    if (playerIt == players.end()) {
        return "No such player.";
    }

    // Me first: a warp requested mid-event or mid-load is the game's softlock, not theirs.
    if (local_is_loading()) {
        return "You are between areas.";
    }
    if (!world_is_playable()) {
        return "You are in an event.";
    }
    if (local_is_dead(live_alink())) {
        return "You are down.";
    }

    const Info* info = get(playerId);
    if (info == nullptr || !info->valid) {
        return "Their location is not known yet.";
    }
    if ((info->flags & kFlagLoading) != 0) {
        return "They are between areas.";
    }
    if ((info->flags & kFlagInEvent) != 0) {
        return "They are in an event.";
    }
    if ((info->flags & kFlagDead) != 0) {
        return "They are down.";
    }
    if ((info->flags & kFlagBossStage) != 0) {
        return "They are in a boss room.";
    }

    // The target is his last received pose, which must be from the stage Presence names: right
    // after he changes stage the newest pose can still be the old one, or a held one from the load.
    const RemotePlayer& player = playerIt->second;
    if (!player.hasLatest || (player.latest.flags & kPlayerStateInWorld) == 0) {
        return "Their position is not known yet.";
    }
    if (player.latest.stageHash != location_stage_hash(info->stage)) {
        return "Their position is not known yet.";
    }
    if (player.latest.roomNo < 0 && info->room < 0) {
        return "Their room is not known yet.";
    }
    return nullptr;
}

bool join_partner(std::uint32_t playerId) {
    if (const char* why = join_partner_blocker(playerId)) {
        Log.info("Join partner {}: refused: {}", playerId, why);
        return false;
    }
    const Info& info = s_peers.at(playerId);
    const PlayerState& pose = replication_manager().players().at(playerId).latest;

    const StageName stage(info.stage);
    const s8 room = pose.roomNo >= 0 ? pose.roomNo : info.room;
    const s16 angle = pose.angleY;
    const cXyz pos(pose.posX - kJoinBehindDistance * cM_ssin(angle), pose.posY,
        pose.posZ - kJoinBehindDistance * cM_scos(angle));

    /* Spec I.4: spawn at an arbitrary position through the restart room, the way
     * daAlink_c::dungeonReturnWarp does (d_a_alink_demo.inc:4390-4401). Point -1 makes
     * dStage_playerInit take the restart-room position, angle and parameters
     * (d_stage.cpp:1712-1715). Layer -1 lets the game pick the layer from the (shared) flags.
     *
     * i_setPoint is 0, as in dungeonReturnWarp and dStage_restartRoom: the 4-argument
     * setNextStage would also write -1 into the save's start point, which a later save would keep.
     *
     * The void-out restart point this overwrites is reset by Link himself on creation
     * (d_a_alink.cpp:5044), so it ends up being where we arrive. */
    dComIfGs_setRestartRoom(pos, angle, room);
    dComIfGp_setNextStage(stage.text, -1, room, -1, 0.0f, 0, 0, 0, 0, 1, 0);
    dComIfGs_setRestartRoomParam(daPy_py_c::setParamData(room, kJoinStartMode, kJoinStartEvent, 0));

    Log.info("Join partner {}: warping to '{}' room {} at ({:.0f}, {:.0f}, {:.0f}) angle {}",
        playerId, static_cast<const char*>(stage.text), room, pos.x, pos.y, pos.z, angle);
    return true;
}

std::string describe(std::uint32_t playerId) {
    const Info* info = get(playerId);
    if (info == nullptr || !info->valid) {
        return "location unknown";
    }
    const StageName stage(info->stage);
    const char* mapName = lookup_map_name(stage.text);
    std::string out = mapName != nullptr ? mapName : (stage.text[0] != '\0' ? stage.text : "?");
    if (info->room >= 0 && (info->flags & kFlagLoading) == 0) {
        out += fmt::format(", room {}", info->room);
    }

    std::vector<const char*> tags;
    if ((info->flags & kFlagLoading) != 0) {
        tags.push_back("loading");
    }
    if ((info->flags & kFlagInEvent) != 0) {
        tags.push_back("in event");
    }
    if ((info->flags & kFlagWolf) != 0) {
        tags.push_back("wolf");
    }
    if ((info->flags & kFlagDead) != 0) {
        tags.push_back("down");
    }
    if (!tags.empty()) {
        out += " (";
        for (std::size_t i = 0; i < tags.size(); ++i) {
            if (i != 0) {
                out += ", ";
            }
            out += tags[i];
        }
        out += ")";
    }
    return out;
}

}  // namespace dusk::mp::presence
