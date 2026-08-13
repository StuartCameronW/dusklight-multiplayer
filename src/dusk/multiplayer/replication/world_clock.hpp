#pragma once

#include <cstddef>
#include <cstdint>

/**
 * The shared world clock: time of day and date, host-authoritative.
 *
 * ★ WHY THIS EXISTS. Reported by Stuart, 2026-08-13: *"time isnt sync across both games."* It was
 * never synced at all. Time and date live in `dSv_player_status_b_c` — save state inside
 * `g_dComIfG_gameInfo` (`d_com_inf_game.h:1395-1409`) — so each instance starts from ITS OWN save's
 * clock and then advances it independently. The two are divergent from tick zero and drift from
 * there; there is no mechanism that could ever have brought them together.
 *
 * `mp_policy.hpp` already declared `timeOfDay = Ownership::Shared`. Nothing read it. This is the
 * thing that reads it.
 *
 * ★ WHY IT IS SAFE TO WRITE THE HOST'S TIME INTO A GUEST'S SAVE STRUCT. Because that struct is now
 * quarantined: a guest never writes its own memory card again (`session/save_guard.hpp`). Without
 * that guard this feature would be the first working example of exactly the corruption the guard
 * was built for — the host's world state, applied through a named setter, autosaved onto the
 * guest's own file. Landing save ownership first was not incidental ordering.
 *
 * ★ TWO DIFFERENT THINGS, AND CONFLATING THEM IS THE TRAP. The clock TICKS
 * (`daytime += time_change_rate`, `d_kankyo.cpp:1550`) and the clock is SET (a cutscene, a stage's
 * entry time, sleeping, a time-control tag). Only the tick is governed by
 * `MultiplayerPolicy::timeAdvance`. A set must always take effect, in both directions:
 *
 *   - Host sets the time -> every guest mirrors it on the next snapshot. Free.
 *   - Guest sets the time -> detected here and pushed UP as a `TimeOverride`, which the host adopts
 *     and rebroadcasts. Without this, "completing Arbiter's Grounds sets it to night" would work
 *     only if the host happened to be the one who did it. Stuart raised exactly this case when he
 *     picked the rule, and it is the reason the packet exists.
 *
 * Detection is by COMPARISON rather than by hooking the setters: the passive advance is one line in
 * one function, so anything else that moved the clock since the last tick was a set — including the
 * ones nobody enumerated. Hooking each of the ~20 `dComIfGs_setTime` call sites would be a list
 * that silently goes out of date.
 *
 * ★ TIME-CONTROL TAGS STAY LOCAL. `kytag11` forces a time for a region and restores it afterwards
 * (`d_a_kytag11.cpp:52-80`), raising `using_time_control_tag`. That is a LOCAL lighting effect, not
 * a statement about the world clock, so while the tag is up this class neither overwrites the local
 * time nor reports the tag's value upward. Getting this wrong would have had one player walking
 * through a tagged region and dragging everybody else's sun with them.
 */

namespace dusk::mp {

class WorldClock {
public:
    /**
     * Should the passive advance run this tick? @p i_localWouldAdvance is vanilla's own answer for
     * this machine.
     *
     * Outside a session, and whenever the policy makes time Individual, this returns its argument —
     * so single player is bit-for-bit unchanged.
     */
    bool should_advance(bool i_localWouldAdvance) const;

    /**
     * Reconcile local time with the shared clock, called once per tick just before `setDaytime`
     * writes back. On a client this either adopts the host's value or, if something local set the
     * clock since last tick, keeps the local value and pushes it up. On a host it records the value
     * to broadcast. No-op outside a session.
     *
     * @p io_daytime and @p io_date are `dScnKy_env_light_c`'s working copies, modified in place.
     * @p i_timeControlTag is `dScnKy_env_light_c::using_time_control_tag` — non-zero means a local
     * region is driving the time and the shared clock must keep out of it.
     */
    void reconcile(float& io_daytime, std::uint16_t& io_date, std::uint8_t i_timeControlTag);

    /// Host: remember what a peer last reported about its own room. Cleared when the peer leaves.
    void set_peer_time_can_pass(std::uint32_t i_playerId, bool i_canPass, bool i_inWorld);
    void forget_peer(std::uint32_t i_playerId);

    /// Host: the clock to put in the outgoing snapshot.
    float daytime() const { return mDaytime; }
    std::uint16_t date() const { return mDate; }

    /// Client: the clock from the newest snapshot.
    void receive(float i_daytime, std::uint16_t i_date);

    /// Host: a client reported that something set its clock. Adopted wholesale — the guest's event
    /// is as authoritative as the host's would have been.
    void receive_override(std::uint32_t i_playerId, float i_daytime, std::uint16_t i_date);

    /**
     * Client: take the pending upward override, if reconcile() found one. Returns false when there
     * is nothing to send.
     *
     * The clock does not send it itself, deliberately: `NetworkManager` owns the transport, and a
     * replication object reaching down to it would invert the layering the whole subsystem is built
     * on (04-architecture.md). So the clock decides, and the session sends.
     */
    bool consume_pending_override(float& o_daytime, std::uint16_t& o_date);

    /// Drop all peer reporting and the received clock. Called on session teardown so a later
    /// session cannot inherit a stale roster and stall its clock forever.
    void clear();

    /// Periodic both-ends diagnostic: what time this machine thinks it is, and who is holding the
    /// clock still. Called every tick; logs occasionally.
    void report();

private:
    bool all_peers_allow_advance() const;

    /// The authoritative clock: what the host holds, or what a client last received.
    float mDaytime = 0.0f;
    std::uint16_t mDate = 0;
    bool mHaveRemoteClock = false;

    /// What we wrote into the save struct last tick, so a change we did not make is detectable.
    float mLastAppliedDaytime = 0.0f;
    std::uint16_t mLastAppliedDate = 0;
    bool mHaveApplied = false;

    /// Set by reconcile() on a client when a local set was detected; drained by the session.
    bool mOverridePending = false;
    float mOverrideDaytime = 0.0f;
    std::uint16_t mOverrideDate = 0;

    /// The last override actually sent, so the load-time fight between the save and the incoming
    /// snapshot does not send the same reliable packet several frames running.
    bool mSentOverride = false;
    float mSentOverrideDaytime = 0.0f;
    std::uint16_t mSentOverrideDate = 0;

    struct PeerClockState {
        std::uint32_t playerId = 0;
        bool canPass = false;
        /// A peer that is loading does not get a vote — otherwise a slow stage transition on one
        /// machine freezes the world for everyone else until it finishes.
        bool inWorld = false;
    };

    static constexpr std::size_t kMaxTrackedPeers = 8;
    PeerClockState mPeers[kMaxTrackedPeers];
    std::size_t mPeerCount = 0;

    /// ~5 s at 30 Hz. Often enough to watch two logs converge, rare enough not to bury them.
    static constexpr std::uint32_t kReportIntervalTicks = 150;
    std::uint32_t mReportCounter = 0;
};

/// The process-wide clock.
WorldClock& world_clock();

/**
 * ★ Test knob: DUSK_MP_TEST_TIME_BLOCK=1 makes this instance report that time cannot pass where it
 * is standing, wherever it actually is.
 *
 * The AllPlayers rule has exactly one branch worth testing — "somebody says no, so the sun stops" —
 * and a scripted run cannot reach it: every stage the harness can boot into and warp between is
 * outdoors, and guessing at an interior stage name is how a test run turns into a hang. This knob
 * reaches the branch directly, which is the same trade `DUSK_MP_TEST_SAVE_REFUSAL` makes for the
 * save prompt, and for the same reason: the alternative is a comment claiming it works.
 *
 * Set it on the GUEST. On the host it proves nothing — the host's own room is already consulted by
 * vanilla's own predicate, so blocking there is not the path under test.
 */
bool test_time_blocked();

}  // namespace dusk::mp
