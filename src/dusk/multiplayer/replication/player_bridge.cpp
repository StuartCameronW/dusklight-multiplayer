#include "player_bridge.hpp"

#include <unordered_map>

#include "d/actor/d_a_remote_player.h"
#include "d/d_com_inf_game.h"
#include "dusk/logging.h"
#include "f_op/f_op_actor_mng.h"

/**
 * The one translation unit allowed to include engine headers (see player_bridge.hpp).
 *
 * Everything here has to tolerate being called when there is no scene, no Link and no puppet: the
 * tick hooks fire during loads, and are skipped entirely during DVD-error and shutdown states, so
 * this code sees discontinuities rather than a tidy lifecycle.
 */

namespace dusk::mp {
namespace {

aurora::Module Log{"dusk::mp"};

/// playerId -> the puppet's process id. Process ids are re-resolved to pointers every tick via
/// fopAcM_SearchByID and never cached: the actor can be destroyed by a room unload at any time,
/// and a stale fopAc_ac_c* would be a use-after-free.
std::unordered_map<std::uint32_t, fpc_ProcID> s_puppets;

daRemotePlayer_c* resolve_puppet(std::uint32_t playerId) {
    const auto it = s_puppets.find(playerId);
    if (it == s_puppets.end()) {
        return nullptr;
    }

    fopAc_ac_c* actor = nullptr;
    // The two-argument form reports NULL while the actor is still mid-create, where the inline
    // one-argument overload would hand back a half-constructed process.
    fopAcM_SearchByID(it->second, &actor);
    return static_cast<daRemotePlayer_c*>(actor);
}

}  // namespace

bool capture_local_player(PlayerState& out) {
    fopAc_ac_c* link = dComIfGp_getPlayer(0);
    if (link == nullptr) {
        return false;
    }

    out.posX = link->current.pos.x;
    out.posY = link->current.pos.y;
    out.posZ = link->current.pos.z;
    // shape_angle is the visual facing that gets baked into the model matrix; current.angle is the
    // logical one and the two diverge while turning. The puppet is a visual, so mirror the visual.
    out.angleY = link->shape_angle.y;
    out.speed = link->speedF;
    out.flags = kPlayerStateInWorld;
    return true;
}

bool ensure_puppet(std::uint32_t playerId, std::uint32_t colorRgb) {
    if (s_puppets.count(playerId) != 0) {
        // Already requested. Creation is asynchronous, so "requested" and "alive" differ for a
        // frame or two; resolve_puppet() returning NULL in that window is expected, not an error.
        return true;
    }

    fopAc_ac_c* link = dComIfGp_getPlayer(0);
    if (link == nullptr) {
        // No scene yet. Spawning now would put the puppet in a room that is about to be unloaded.
        return false;
    }

    // Spawn on top of the local player: the first network pose arrives within a tick or two and
    // moves it, and starting at the origin would show a puppet sliding in from the map corner.
    const cXyz pos = link->current.pos;
    const csXyz angle(0, link->shape_angle.y, 0);

    const fpc_ProcID id = fopAcM_create(
        fpcNm_REMOTE_PLAYER_e, playerId, &pos, fopAcM_GetRoomNo(link), &angle, nullptr, 0xFF);

    if (id == fpcM_ERROR_PROCESS_ID_e) {
        Log.warn("Failed to create puppet for player {}", playerId);
        return false;
    }

    s_puppets[playerId] = id;
    Log.info("Spawned puppet for player {} (#{:06X}) as process {}", playerId, colorRgb, id);
    return true;
}

void apply_puppet_state(std::uint32_t playerId, const PlayerState& state) {
    daRemotePlayer_c* puppet = resolve_puppet(playerId);
    if (puppet == nullptr) {
        return;
    }

    cXyz pos(state.posX, state.posY, state.posZ);
    puppet->setNetworkPose(pos, state.angleY, state.speed);
}

void destroy_puppet(std::uint32_t playerId) {
    const auto it = s_puppets.find(playerId);
    if (it == s_puppets.end()) {
        return;
    }
    // The fpc_ProcID overload copes with the actor already being gone, which it will be whenever a
    // room unload beat us to it.
    fopAcM_delete(it->second);
    s_puppets.erase(it);
}

void destroy_all_puppets() {
    for (const auto& entry : s_puppets) {
        fopAcM_delete(entry.second);
    }
    s_puppets.clear();
}

}  // namespace dusk::mp
