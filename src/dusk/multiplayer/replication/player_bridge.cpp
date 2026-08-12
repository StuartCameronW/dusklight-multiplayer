#include "player_bridge.hpp"

#include <unordered_map>

#include "d/actor/d_a_alink.h"
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

/// How long a requested puppet may resolve to NULL before it is presumed dead rather than still
/// loading. Creation is multi-phase and hits the DVD, so this has to be generous; ~4 s at 30 Hz.
constexpr int kCreateGraceTicks = 120;

/*
 * ★ Do NOT add a "wait N settled ticks before creating the puppet" gate here.
 *
 * That was tried on 2026-08-12 as a mitigation for a hang, on the theory that creating a puppet on
 * the first playable tick after an event ends was too early. It is a reasonable-sounding theory and
 * it is wrong: with a 15-tick delay the joining instance hung on the puppet's first calc() in two
 * runs out of two, and with the delay removed it completed 1,076 in-world ticks. Delaying creation
 * did not avoid a bad moment, it moved creation INTO one.
 *
 * The useful part of that result is the clue: puppet creation is sensitive to where in the room's
 * lifecycle it lands, which is worth knowing when the real hang is finally chased. Whatever the
 * eventual fix is, it will not be a timer.
 */

struct PuppetRef {
    fpc_ProcID id = 0;
    /// True once the actor has resolved at least once. Before that, a NULL resolve means "still
    /// being created"; after it, the same NULL means "destroyed by a room unload".
    bool everResolved = false;
    int ticksSinceRequest = 0;
};

/// playerId -> the puppet's process id. Process ids are re-resolved to pointers every tick via
/// fopAcM_SearchByID and never cached: the actor can be destroyed by a room unload at any time,
/// and a stale fopAc_ac_c* would be a use-after-free.
std::unordered_map<std::uint32_t, PuppetRef> s_puppets;

daRemotePlayer_c* resolve_puppet(std::uint32_t playerId) {
    const auto it = s_puppets.find(playerId);
    if (it == s_puppets.end()) {
        return nullptr;
    }

    fopAc_ac_c* actor = nullptr;
    // The two-argument form reports NULL while the actor is still mid-create, where the inline
    // one-argument overload would hand back a half-constructed process.
    fopAcM_SearchByID(it->second.id, &actor);
    if (actor != nullptr && !it->second.everResolved) {
        it->second.everResolved = true;
        // One line, once, on the tick the actor first becomes resolvable. The hang on 2026-08-11
        // left a log ending at "using body archive" with the actor's own first-calc dump absent,
        // which narrowed it to "after create() returned, before the first execute() did any work"
        // and no further — because nothing was logged in between. This is that missing line.
        Log.debug("Puppet for player {} resolved after {} tick(s); entering the actor pass",
            playerId, it->second.ticksSinceRequest);
    }
    return static_cast<daRemotePlayer_c*>(actor);
}

}  // namespace

/**
 * "There is a real, playable scene" — the gate for both sending our pose and spawning puppets.
 *
 * getPlayer(0) alone is NOT enough, and this took three measured attempts to get right.
 *
 * The title screen runs an attract demo in which Link is a live actor walking real stages, so the
 * null check passes several seconds before a save is even loaded, and passes again whenever the
 * game idles back to the title. Two bugs came out of trusting it: a puppet created into the demo
 * scene (which is about to be torn down) hung the game outright, and demo poses went out on the
 * wire as gameplay, so every remote puppet re-enacted the attract sequence and then teleported
 * ~44,000 units when the real save finally loaded.
 *
 * Adding !event_runCheck() was still not enough — the demo has stretches with no event running,
 * which a trace caught as a stray in-world sample at the demo's start pose. The decisive test is
 * whether the title actor exists at all: it owns the attract sequence, so while it is alive
 * nothing on screen is gameplay no matter what the player actor says.
 *
 * The event check stays because it additionally keeps puppets out of cutscenes and warp
 * transitions, which is wanted in its own right.
 */
bool world_is_playable() {
    return dComIfGp_getPlayer(0) != nullptr && !dComIfGp_event_runCheck() &&
           fopAcM_SearchByName(fpcNm_TITLE_e) == NULL;
}

bool capture_local_player(PlayerState& out) {
    if (!world_is_playable()) {
        // Nothing worth sending: no scene, or the pose belongs to the attract demo or a cutscene
        // rather than to the player. See world_is_playable().
        return false;
    }

    fopAc_ac_c* link = dComIfGp_getPlayer(0);

    out.posX = link->current.pos.x;
    out.posY = link->current.pos.y;
    out.posZ = link->current.pos.z;
    // shape_angle is the visual facing that gets baked into the model matrix; current.angle is the
    // logical one and the two diverge while turning. The puppet is a visual, so mirror the visual.
    out.angleY = link->shape_angle.y;
    out.speed = link->speedF;
    out.flags = kPlayerStateInWorld;

    /* The sharp turn. Read straight off the proc the state machine is IN rather than inferred from
     * anything: commonProcInit writes mProcID = the proc it is entering (d_a_alink.cpp:15144) and
     * procSlipInit enters PROC_SLIP and plays ANM_SLIP in the same two lines
     * (d_a_alink.cpp:16673-16675), so this bit is true for exactly the ticks Link is skidding.
     *
     * Fetched through the LINK_PTR slot rather than reusing `link` above because that is the slot
     * the rest of the puppet code reads Link from (d_a_remote_player.cpp:414), and it is the one
     * that is documented to hold a daAlink_c. Null-checked anyway — nothing here may assume a
     * scene. Wolf Link never reaches PROC_SLIP (he has his own PROC_WOLF_SLIP_TURN), so a wolf
     * sender simply sends the bit clear, which is the right answer while the puppet has no wolf
     * model to play it on.
     */
    const daAlink_c* alink = static_cast<const daAlink_c*>(dComIfGp_getLinkPlayer());
    if (alink != nullptr && alink->mProcID == daAlink_c::PROC_SLIP) {
        out.flags |= kPlayerStateSharpTurn;
    }

    return true;
}

bool ensure_puppet(std::uint32_t playerId, std::uint32_t colorRgb) {
    if (const auto it = s_puppets.find(playerId); it != s_puppets.end()) {
        it->second.ticksSinceRequest++;

        if (resolve_puppet(playerId) != nullptr) {
            return true;
        }
        if (!it->second.everResolved && it->second.ticksSinceRequest < kCreateGraceTicks) {
            // Still being created. Asynchronous, so "requested" and "alive" differ for a frame or
            // two; NULL in this window is expected, not an error.
            return true;
        }

        // The actor is gone — a room unload or stage change took it. Drop the dead process id so
        // the puppet is recreated below, otherwise it would stay permanently invisible while the
        // network layer went on cheerfully applying poses to nothing.
        Log.info("Puppet for player {} vanished; respawning", playerId);
        s_puppets.erase(it);
    }

    if (!world_is_playable()) {
        // No playable scene. Spawning now would put the puppet in the title demo or in a room
        // that is about to be unloaded.
        return false;
    }

    fopAc_ac_c* link = dComIfGp_getPlayer(0);

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

    PuppetRef& ref = s_puppets[playerId];
    ref.id = id;
    ref.everResolved = false;
    ref.ticksSinceRequest = 0;
    Log.info("Spawned puppet for player {} (#{:06X}) as process {}", playerId, colorRgb, id);
    return true;
}

void apply_puppet_state(std::uint32_t playerId, const PlayerState& state) {
    daRemotePlayer_c* puppet = resolve_puppet(playerId);
    if (puppet == nullptr) {
        return;
    }

    cXyz pos(state.posX, state.posY, state.posZ);
    puppet->setNetworkPose(pos, state.angleY, state.speed, state.sharp_turn());
}

bool read_puppet_pose(std::uint32_t playerId, PlayerState& out) {
    daRemotePlayer_c* puppet = resolve_puppet(playerId);
    if (puppet == nullptr) {
        return false;
    }

    out.posX = puppet->current.pos.x;
    out.posY = puppet->current.pos.y;
    out.posZ = puppet->current.pos.z;
    out.angleY = puppet->shape_angle.y;
    out.speed = puppet->getNetSpeed();
    out.flags = puppet->hasPose() ? kPlayerStateInWorld : 0;
    return true;
}

bool read_puppet_anim(std::uint32_t playerId, std::uint16_t& out) {
    daRemotePlayer_c* puppet = resolve_puppet(playerId);
    if (puppet == nullptr) {
        return false;
    }
    out = puppet->getCurrentAnm();
    return true;
}

void destroy_puppet(std::uint32_t playerId) {
    const auto it = s_puppets.find(playerId);
    if (it == s_puppets.end()) {
        return;
    }
    // The fpc_ProcID overload copes with the actor already being gone, which it will be whenever a
    // room unload beat us to it.
    fopAcM_delete(it->second.id);
    s_puppets.erase(it);
}

void destroy_all_puppets() {
    for (const auto& entry : s_puppets) {
        fopAcM_delete(entry.second.id);
    }
    s_puppets.clear();
}

}  // namespace dusk::mp
