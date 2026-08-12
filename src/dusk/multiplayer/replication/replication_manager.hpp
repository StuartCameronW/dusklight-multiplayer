#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "player_state.hpp"
#include "state_buffer.hpp"

/**
 * Owns "who exists and where are they", separate from NetworkManager's "who is connected".
 *
 * The split matters: connection identity (peers, sockets, handshakes) and world identity (players,
 * poses, puppets) have different lifetimes. A peer can be connected while its player has no pose
 * yet, and the host's own player has no peer at all.
 */

namespace dusk::mp {

/// One replicated player as seen locally. Never the local player.
struct RemotePlayer {
    std::uint32_t playerId = 0;
    std::string nickname;
    std::uint32_t color = 0xFFFFFF;

    /// Interpolation history used to drive the puppet smoothly.
    StateBuffer buffer;

    /// Most recent pose received, kept verbatim for the host to rebroadcast. Distinct from what
    /// the buffer plays back, which is deliberately delayed.
    PlayerState latest;
    /// The tick `latest` was captured on, in the SENDER's own clock. Rebroadcast alongside the pose
    /// so a third player interpolates this player on the timeline they actually moved on, rather
    /// than on whenever the host happened to relay it. Also what makes a re-sent unchanged pose
    /// identifiable as a duplicate instead of looking like a genuine "stood still for a tick".
    std::uint64_t latestTick = 0;
    bool hasLatest = false;

    /// True once the puppet actor is confirmed alive, so we stop retrying creation every tick.
    bool puppetAlive = false;

    /// The last interpolated pose actually pushed onto the puppet. Logged periodically, because
    /// "packets are arriving" and "the puppet is moving" are different claims and only the second
    /// one is the milestone.
    PlayerState lastApplied;
};

class ReplicationManager {
public:
    void add_player(std::uint32_t playerId, const std::string& nickname, std::uint32_t color);
    void remove_player(std::uint32_t playerId);
    /// Drop every player and despawn their puppets. Used when a session ends.
    void clear();

    /// Feed a received pose into a player's interpolation buffer.
    void record_remote(std::uint32_t playerId, std::uint64_t tick, const PlayerState& state);

    /// Read the local player's pose via the bridge. False when Link isn't in the world.
    bool capture_local(PlayerState& out) const;

    /// Advance every buffer one sim tick and drive the puppets. Called before the actor pass so
    /// puppets are already in position when actors execute.
    void drive_puppets();

    /// Forget interpolation history without forgetting the players — for scene changes, where
    /// blending from a pose in the old room to one in the new room would be meaningless.
    void reset_buffers();

    const std::unordered_map<std::uint32_t, RemotePlayer>& players() const { return mPlayers; }
    bool empty() const { return mPlayers.empty(); }

    /// Aggregate interpolation health, for the periodic diagnostic log line.
    struct Diagnostics {
        double worstDelayTicks = 0.0;
        std::uint32_t totalStarvations = 0;
        std::uint32_t totalSnaps = 0;
        /// Pose of an arbitrary remote player, as evidence the puppet is being driven.
        PlayerState samplePose;
    };
    Diagnostics diagnostics() const;

private:
    std::unordered_map<std::uint32_t, RemotePlayer> mPlayers;
};

ReplicationManager& replication_manager();

}  // namespace dusk::mp
