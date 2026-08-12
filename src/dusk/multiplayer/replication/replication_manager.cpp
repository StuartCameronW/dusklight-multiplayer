#include "replication_manager.hpp"

#include <algorithm>

#include "../policy/mp_policy.hpp"
#include "player_bridge.hpp"

namespace dusk::mp {

void ReplicationManager::add_player(
    std::uint32_t playerId, const std::string& nickname, std::uint32_t color) {
    RemotePlayer& player = mPlayers[playerId];
    player.playerId = playerId;
    player.nickname = nickname;
    player.color = color;
    // The puppet is created lazily in drive_puppets(): at join time there may be no scene loaded
    // yet, and a failed spawn here would be indistinguishable from a real error.
}

void ReplicationManager::remove_player(std::uint32_t playerId) {
    const auto it = mPlayers.find(playerId);
    if (it == mPlayers.end()) {
        return;
    }
    if (it->second.puppetAlive) {
        destroy_puppet(playerId);
    }
    mPlayers.erase(it);
}

void ReplicationManager::clear() {
    destroy_all_puppets();
    mPlayers.clear();
}

void ReplicationManager::record_remote(
    std::uint32_t playerId, std::uint64_t tick, const PlayerState& state) {
    const auto it = mPlayers.find(playerId);
    if (it == mPlayers.end()) {
        // A pose for someone we haven't been told about yet. Snapshots are unreliable and the
        // roster is reliable, so they can legitimately race; dropping it is correct — the next
        // snapshot after the roster arrives will have it.
        return;
    }
    it->second.buffer.push(tick, state);
    it->second.latest = state;
    it->second.latestTick = tick;
    it->second.hasLatest = true;
}

bool ReplicationManager::capture_local(PlayerState& out) const {
    return capture_local_player(out);
}

void ReplicationManager::reset_buffers() {
    for (auto& entry : mPlayers) {
        entry.second.buffer.reset();
    }
}

void ReplicationManager::drive_puppets() {
    for (auto& entry : mPlayers) {
        RemotePlayer& player = entry.second;

        PlayerState pose;
        if (!player.buffer.advance(pose)) {
            // Nothing received yet — don't spawn a puppet that would stand at the origin.
            continue;
        }

        // tpmp's "optimization level 3" equivalent: stop drawing other players entirely to claw
        // back frames. Checked here rather than at the draw site so the actor isn't even alive.
        const bool wanted = g_mpPolicy.renderRemotePlayers && pose.in_world();

        if (!wanted) {
            if (player.puppetAlive) {
                destroy_puppet(player.playerId);
                player.puppetAlive = false;
            }
            continue;
        }

        // Appearance is owned by the wearer, so a clothes change on the sender has to reach the
        // puppet. It does so by rebuilding the actor — see reconcile_puppet_outfit() for why an
        // in-place re-mount is not on offer. Driven off `pose` rather than off the newest sample
        // received so the change lands on the same delayed timeline as the movement it accompanies.
        if (reconcile_puppet_outfit(player.playerId, pose.outfit)) {
            // The puppet is gone for this tick; ensure_puppet() rebuilds it in the new clothes on
            // the next one. Deliberately not the same tick: it keeps the old actor's archive and
            // solid heap from having to coexist with the new one's.
            player.puppetAlive = false;
            continue;
        }

        // Asked every tick rather than cached in puppetAlive: the actor can be destroyed under us
        // by a room unload, and a cached "alive" would leave us applying poses to nothing forever.
        // ensure_puppet() owns the whole question, including respawning after a scene change.
        if (!ensure_puppet(player.playerId, player.color, pose.outfit)) {
            player.puppetAlive = false;
            continue;
        }
        player.puppetAlive = true;

        apply_puppet_state(player.playerId, pose);
        player.lastApplied = pose;
    }
}

ReplicationManager::Diagnostics ReplicationManager::diagnostics() const {
    Diagnostics diag;
    for (const auto& entry : mPlayers) {
        const StateBuffer& buffer = entry.second.buffer;
        diag.worstDelayTicks = std::max(diag.worstDelayTicks, buffer.delay_ticks());
        diag.totalStarvations += buffer.starvation_count();
        diag.totalSnaps += buffer.snap_count();
        diag.samplePose = entry.second.lastApplied;
    }
    return diag;
}

ReplicationManager& replication_manager() {
    static ReplicationManager instance;
    return instance;
}

}  // namespace dusk::mp
