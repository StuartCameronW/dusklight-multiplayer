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

/// How long a requested puppet may resolve to NULL before it is presumed dead rather than still
/// loading. Creation is multi-phase and hits the DVD, so this has to be generous; ~4 s at 30 Hz.
constexpr int kCreateGraceTicks = 120;

/// Floor on how often an outfit change may respawn one player's puppet; ~1 s at 30 Hz.
///
/// A real clothes change happens in the pause menu and is rare, so a second of latency on it costs
/// nothing. What this buys is a bound on the pathological case: poses arrive unreliably and
/// unauthenticated, so a single flipping byte — or a peer genuinely rummaging through the menu —
/// must not be able to destroy and rebuild an actor (archive mount included) every tick.
constexpr int kOutfitRespawnCooldownTicks = 30;

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
    // Sampled every tick rather than on a change event, because there is no change event to hook:
    // daAlink_c::setArcName just overwrites mArcName during the pause menu's model rebuild. Reading
    // it is a pointer compare against four names, so the cost of doing it per tick is nil and the
    // alternative — caching it and missing an unhooked path that changes clothes — is not worth it.
    out.outfit = daRemotePlayer_localOutfitToWire();
    out.flags = kPlayerStateInWorld;
    return true;
}

bool ensure_puppet(std::uint32_t playerId, std::uint32_t colorRgb, std::uint8_t outfit) {
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

    // The outfit rides in the top byte of the create parameter. The actor needs it at its first
    // create() entry, before the multi-phase archive mount starts, and this is the only channel
    // that exists that early — see the packing note in d_a_remote_player.h.
    const std::uint32_t param =
        (static_cast<std::uint32_t>(outfit) << daRemotePlayer_outfitParamShift) |
        (playerId & daRemotePlayer_playerIdMask);

    const fpc_ProcID id = fopAcM_create(
        fpcNm_REMOTE_PLAYER_e, param, &pos, fopAcM_GetRoomNo(link), &angle, nullptr, 0xFF);

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
    puppet->setNetworkPose(pos, state.angleY, state.speed);
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
    // The outfit the puppet actually mounted, not the byte we last received, for the same reason
    // the rest of this function reads the actor rather than the network: this is the "did the
    // instruction land?" side of the comparison.
    out.outfit = static_cast<std::uint8_t>(puppet->getOutfit());
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

/**
 * Make a mid-session clothes change reach the puppet — by respawning it.
 *
 * ★ This is the deliberate trade-off, and it is not merely the easy option: an in-place re-mount is
 * not available. Everything the puppet is made of — the four J3DModels, the McaMorfSO, the material
 * anms — is allocated inside the actor's own JKRSolidHeap (fopAcM_entrySolidHeap, 0x20000). A solid
 * heap cannot free an individual allocation; the only way to reclaim it is to destroy the heap, and
 * destroying an actor's heap is exactly what fopAcM_delete does. So "swap the archive under a live
 * actor" would mean re-running the whole create phase in place, on machinery designed to run once,
 * for no gain over letting the actor die and be rebuilt by the path that already handles a room
 * unload taking a puppet away.
 *
 * What it costs is honest to state: the puppet is ABSENT for the tick it takes to notice plus the
 * frames the DVD mount of the new archive takes, and it loses its accumulated local presentation
 * state (cap and hair sway, blink phase, eye smoothing). None of that is replicated, all of it
 * re-converges within a second, and the wearer is standing in a pause menu while it happens.
 *
 * Two rules keep it from turning into a respawn storm:
 *  - The comparison is between RESOLVED indices, not raw bytes. If the actor clamped a byte it did
 *    not recognise to the hero's clothes and we compared against the raw byte, every single tick
 *    would read as "still wearing the wrong thing" and rebuild the actor forever.
 *  - A cooldown, measured on the puppet's own age (ticksSinceRequest is zeroed at create and
 *    stepped once per tick), so even a genuinely flapping outfit costs one respawn per second at
 *    worst. It defers a change, never drops one: the sender keeps reporting the new outfit, so the
 *    mismatch is still there when the cooldown expires.
 */
bool reconcile_puppet_outfit(std::uint32_t playerId, std::uint8_t outfit) {
    const auto it = s_puppets.find(playerId);
    if (it == s_puppets.end()) {
        return false;
    }

    daRemotePlayer_c* puppet = resolve_puppet(playerId);
    if (puppet == nullptr) {
        // Still mounting its archive, or already destroyed by a room unload. In the first case the
        // outfit it will wear is already latched and tearing down a half-created actor is the one
        // thing worth not doing; in the second, ensure_puppet() is about to rebuild it anyway.
        return false;
    }

    const int wanted = daRemotePlayer_outfitFromWire(outfit);
    if (puppet->getOutfit() == wanted) {
        return false;
    }
    if (it->second.ticksSinceRequest < kOutfitRespawnCooldownTicks) {
        return false;
    }

    Log.info("Player {} changed outfit ({} -> {}); rebuilding their puppet", playerId,
        puppet->getOutfit(), wanted);
    destroy_puppet(playerId);
    return true;
}

void destroy_all_puppets() {
    for (const auto& entry : s_puppets) {
        fopAcM_delete(entry.second.id);
    }
    s_puppets.clear();
}

}  // namespace dusk::mp
