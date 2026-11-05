#include "replication_manager.hpp"

#include <algorithm>

#include "../policy/mp_policy.hpp"
#include "dusk/logging.h"
#include "player_bridge.hpp"

namespace dusk::mp {
namespace {

constexpr borealis::Log Log{"dusk::mp"};

/// How long a LIVE puppet's sender must stay in a room this end is not displaying before the
/// puppet is despawned (~0.5 s at 30 Hz). Despawning costs a respawn (an archive mount and a
/// 0x20000 solid heap), so a sender pacing along a room boundary must not churn it every tick. A
/// puppet that is not alive yet is held back at once: there is nothing to churn.
constexpr std::uint32_t kRoomHideDebounceTicks = 15;

/// A freshly created Link reports room -1 for its first few ticks (~4 measured) after a warp.
/// remote_location() can only answer Here for that, and spawning on it put the puppet up for
/// 4 ticks + the debounce above before the real room hid it again: a 17-tick flash and a full
/// respawn on every far-room warp (measured, D_MN05). So an unknown room keeps the last
/// decision and does not spawn a puppet, for at most this many SENDER ticks; a stage that never
/// reports a room still gets its puppet. Counted in the sender's ticks, not ours: a sender
/// crawling after a big load took 1.6 s over its 4 room -1 ticks, which a starved receiver
/// replayed for ~64 of its own ticks (measured, F_SP121 with the PC shared).
constexpr std::uint64_t kRoomUnknownHoldTicks = 10;

const char* location_name(RemoteLocation where) {
    switch (where) {
    case RemoteLocation::Here:
        return "here";
    case RemoteLocation::OtherStage:
        return "another stage";
    case RemoteLocation::RoomNotShown:
        return "a room not displayed here";
    }
    return "?";
}

}  // namespace

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
    RemotePlayer& player = it->second;
    /* v16: a scene load on the sender. Reset the buffer rather than interpolate from the old room's
     * last pose to the new spawn: that blend is the glide across the map (measured 1,034 units).
     * After the reset the first new sample is held until the cursor reaches it, i.e. a snap.
     *
     * Judged only on a sample NEWER than the one the current epoch was learned from, so a reordered
     * packet from before the load does not count as a second load. A tick far behind is a sender
     * that restarted its counter (StateBuffer::push treats it as a new origin too), and counts. */
    const bool newer =
        !player.epochKnown || tick > player.epochTick ||
        static_cast<double>(tick) + StateBuffer::kSnapTicks < static_cast<double>(player.epochTick);
    if (newer) {
        if (player.epochKnown && state.locEpoch != player.epoch) {
            player.buffer.reset();
            player.epochResets++;
            Log.info("Player {} location epoch {} -> {} (stage hash {:08X}, room {}): "
                     "interpolation reset, the puppet snaps",
                playerId, static_cast<unsigned>(player.epoch),
                static_cast<unsigned>(state.locEpoch), state.stageHash,
                static_cast<int>(state.roomNo));
        }
        player.epoch = state.locEpoch;
        player.epochTick = tick;
        player.epochKnown = true;
    }
    player.buffer.push(tick, state);
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

        // No puppet and nowhere to put one (booting, title demo, stage load): hold the buffer
        // empty instead of playing it. Played here, it starves against the sender's 30 Hz while
        // this end ticks faster at boot, and ratchets the delay up before the puppet ever appears
        // (measured 2 -> 7 ticks). An alive puppet is still driven through cutscenes.
        if (!player.puppetAlive && !world_is_playable()) {
            player.buffer.reset();
            continue;
        }

        PlayerState pose;
        if (!player.buffer.advance(pose)) {
            // Nothing received yet — don't spawn a puppet that would stand at the origin.
            continue;
        }

        player.lastPlayed = pose;

        /* v16: is the sender somewhere this end is drawing? A puppet for a player in another stage
         * stood at the last place we saw him, and one in an unloaded room floated over nothing.
         * Hidden by despawning, the same way as the not-in-world case below, which takes the name
         * tag, the shadow and the sounds with it (all three hang off the actor). It comes back
         * through ensure_puppet() at the pose of the moment: a snap, never a slide. */
        const RemoteLocation where = remote_location(pose);
        bool roomUnknown = false;
        if (where == RemoteLocation::Here && pose.stageHash != 0 && pose.roomNo < 0) {
            if (!player.roomUnknownActive) {
                player.roomUnknownActive = true;
                player.roomUnknownSince = player.buffer.played_tick();
            }
            roomUnknown =
                player.buffer.played_tick() - player.roomUnknownSince < kRoomUnknownHoldTicks;
            if (roomUnknown) {
                player.roomWasUnknown = true;
            }
        } else if (pose.roomNo >= 0) {
            player.roomUnknownActive = false;
        }
        RemoteLocation hide = RemoteLocation::Here;
        if (roomUnknown) {
            hide = player.hiddenBy;
        } else if (where == RemoteLocation::RoomNotShown) {
            player.roomHiddenTicks++;
            if (!player.puppetAlive || player.roomWasUnknown ||
                player.hiddenBy == RemoteLocation::RoomNotShown ||
                player.roomHiddenTicks >= kRoomHideDebounceTicks)
            {
                hide = where;
            }
        } else {
            player.roomHiddenTicks = 0;
            hide = where;
        }
        if (!roomUnknown && pose.roomNo >= 0) {
            player.roomWasUnknown = false;
        }
        if (hide != player.hiddenBy) {
            if (hide == RemoteLocation::Here) {
                Log.info("Player {}'s puppet shown again: sender is here (stage hash {:08X}, room "
                         "{}, epoch {})",
                    player.playerId, pose.stageHash, static_cast<int>(pose.roomNo),
                    static_cast<unsigned>(pose.locEpoch));
            } else {
                Log.info("Player {}'s puppet hidden: sender is in {} (stage hash {:08X}, room {}, "
                         "epoch {})",
                    player.playerId, location_name(hide), pose.stageHash,
                    static_cast<int>(pose.roomNo), static_cast<unsigned>(pose.locEpoch));
            }
            player.hiddenBy = hide;
        }

        // tpmp's "optimization level 3" equivalent: stop drawing other players entirely to claw
        // back frames. Checked here rather than at the draw site so the actor isn't even alive.
        const bool wanted = g_mpPolicy.renderRemotePlayers && pose.in_world() &&
                            player.hiddenBy == RemoteLocation::Here &&
                            !(roomUnknown && !player.puppetAlive);

        if (!wanted) {
            if (player.puppetAlive) {
                destroy_puppet(player.playerId);
                player.puppetAlive = false;
            }
            continue;
        }

        // Room not known yet (a warp or load just re-created the sender's Link): a live puppet
        // stays where the sender was last seen instead of jumping to a spot that may be in a room
        // we don't draw. The first pose with a known room either snaps it there or hides it.
        if (roomUnknown && player.puppetAlive) {
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
