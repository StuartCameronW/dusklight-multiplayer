#include "world_clock.hpp"

#include <cmath>
#include <cstdlib>

#include "../policy/mp_policy.hpp"
#include "../session/network_manager.hpp"
#include "dusk/logging.h"

namespace dusk::mp {
namespace {

aurora::Module Log{"dusk::mp"};

/**
 * How far the clock has to move in one tick before it counts as somebody SETTING it rather than it
 * ticking.
 *
 * The passive advance is `time_change_rate` per tick, which is 0.012 for most of the day and 1.0
 * across dawn and dusk (`d_kankyo.cpp:1550-1558`) — so 1.0 is a legitimate one-tick step and cannot
 * be treated as a set. Two degrees is comfortably above the largest honest tick and far below any
 * real event, which jump by tens or hundreds (a cutscene sets an absolute hour, sleeping advances a
 * whole day). Time is in degrees: 360 to a day, so 1 degree is four minutes of game time.
 */
constexpr float kSetDetectionThreshold = 2.0f;

/// Shortest distance between two points on a 360-degree clock, so a set across midnight (359 -> 1)
/// reads as 2 rather than as 358.
float clock_delta(float a, float b) {
    float d = std::fabs(a - b);
    if (d > 180.0f) {
        d = 360.0f - d;
    }
    return d;
}

}  // namespace

WorldClock& world_clock() {
    static WorldClock instance;
    return instance;
}

bool test_time_blocked() {
    static const bool blocked = [] {
        const char* value = std::getenv("DUSK_MP_TEST_TIME_BLOCK");
        return value != nullptr && value[0] == '1';
    }();
    return blocked;
}

bool WorldClock::should_advance(bool i_localWouldAdvance) const {
    const NetworkManager& net = network_manager();
    if (!net.is_active() || g_mpPolicy.timeOfDay != Ownership::Shared) {
        return i_localWouldAdvance;
    }

    // A client never ticks its own clock. It is not that the answer would be wrong — it is that
    // there would be two clocks, and the host's would overwrite this one a moment later anyway.
    // Advancing as well would just make the guest's sky stutter forward and be dragged back.
    if (net.role() == Role::Client) {
        return false;
    }

    if (!i_localWouldAdvance) {
        return false;
    }

    switch (g_mpPolicy.timeAdvance) {
    case TimeAdvanceRule::HostRoom:
        return true;
    case TimeAdvanceRule::AnyPlayer:
        // The host is one of the players, and it already said yes.
        return true;
    case TimeAdvanceRule::AllPlayers:
        return all_peers_allow_advance();
    }

    return true;
}

bool WorldClock::all_peers_allow_advance() const {
    for (std::size_t i = 0; i < mPeerCount; ++i) {
        // Still loading: no vote. Otherwise one machine's slow stage transition freezes the world
        // for everyone until it finishes, which reads as the sun randomly sticking.
        if (!mPeers[i].inWorld) {
            continue;
        }
        if (!mPeers[i].canPass) {
            return false;
        }
    }
    return true;
}

void WorldClock::reconcile(
    float& io_daytime, std::uint16_t& io_date, std::uint8_t i_timeControlTag) {
    const NetworkManager& net = network_manager();
    if (!net.is_active() || g_mpPolicy.timeOfDay != Ownership::Shared) {
        mHaveApplied = false;
        return;
    }

    // A time-control tag is a LOCAL region forcing a local look (d_a_kytag11.cpp:52-80). Neither
    // adopt the shared clock over it nor mistake it for an event worth telling everyone about.
    if (i_timeControlTag != 0) {
        mHaveApplied = false;
        return;
    }

    if (net.role() == Role::Host) {
        mDaytime = io_daytime;
        mDate = io_date;
        mHaveRemoteClock = true;
        mLastAppliedDaytime = io_daytime;
        mLastAppliedDate = io_date;
        mHaveApplied = true;
        report();
        return;
    }

    // --- Client ---

    // Something on this machine moved the clock since we last wrote it: a cutscene, the stage's own
    // entry time, sleeping. That is an event, not drift, and it has to travel UP rather than be
    // silently overwritten by the next snapshot.
    if (mHaveApplied && (clock_delta(io_daytime, mLastAppliedDaytime) > kSetDetectionThreshold ||
                            io_date != mLastAppliedDate))
    {
        // ★ Do not re-send the same value. Measured 2026-08-13: while a guest's save is loading,
        // the local clock and the host's snapshot genuinely fight for a handful of frames — the
        // save sets the time, the snapshot overwrites it, the save sets it again — and each round
        // trips this branch. The fight settles on its own within a few frames (the run that found
        // it produced 4 detections total and then none for the remaining 150 s), so the thing worth
        // fixing is not the fight but the three identical RELIABLE packets it was sending, and the
        // three identical log lines that made it look like an oscillation that never ended.
        const bool sameAsLast = mSentOverride &&
                                clock_delta(io_daytime, mSentOverrideDaytime) <= 0.001f &&
                                io_date == mSentOverrideDate;
        if (!sameAsLast) {
            mOverridePending = true;
            mOverrideDaytime = io_daytime;
            mOverrideDate = io_date;
            mSentOverride = true;
            mSentOverrideDaytime = io_daytime;
            mSentOverrideDate = io_date;
            Log.info(
                "World clock: something local set the time to {:.1f} (day {}) — telling the host, "
                "which adopts it for everyone. Was {:.1f} (day {}).",
                io_daytime, io_date, mLastAppliedDaytime, mLastAppliedDate);
        }

        mDaytime = io_daytime;
        mDate = io_date;
        mLastAppliedDaytime = io_daytime;
        mLastAppliedDate = io_date;
        return;
    }

    if (!mHaveRemoteClock) {
        // No snapshot yet. Leave the local clock alone rather than slamming it to zero.
        mLastAppliedDaytime = io_daytime;
        mLastAppliedDate = io_date;
        mHaveApplied = true;
        return;
    }

    io_daytime = mDaytime;
    io_date = mDate;
    mLastAppliedDaytime = io_daytime;
    mLastAppliedDate = io_date;
    mHaveApplied = true;
    report();
}

void WorldClock::report() {
    if (++mReportCounter < kReportIntervalTicks) {
        return;
    }
    mReportCounter = 0;

    const NetworkManager& net = network_manager();
    if (!net.is_active()) {
        return;
    }

    // ★ Both ends log the same three numbers in the same units, which is the only way this feature
    // can be checked at all: "the host's clock advanced" and "the two games agree on the time" are
    // different claims, and a protocol change is only verified when both logs are read (CLAUDE.md).
    // The hour is included because degrees are not something anyone can eyeball against a screen.
    std::size_t blocking = 0;
    for (std::size_t i = 0; i < mPeerCount; ++i) {
        if (mPeers[i].inWorld && !mPeers[i].canPass) {
            ++blocking;
        }
    }

    Log.info("World clock [{}]: daytime {:.2f} ({:02d}:{:02d}), day {}, {} peer(s) tracked, {} "
             "holding time still",
        net.role() == Role::Host ? "host" : "guest", mLastAppliedDaytime,
        static_cast<int>(mLastAppliedDaytime / 15.0f),
        static_cast<int>(std::fmod(mLastAppliedDaytime, 15.0f) * 4.0f), mLastAppliedDate,
        mPeerCount, blocking);
}

void WorldClock::set_peer_time_can_pass(std::uint32_t i_playerId, bool i_canPass, bool i_inWorld) {
    for (std::size_t i = 0; i < mPeerCount; ++i) {
        if (mPeers[i].playerId == i_playerId) {
            mPeers[i].canPass = i_canPass;
            mPeers[i].inWorld = i_inWorld;
            return;
        }
    }

    if (mPeerCount >= kMaxTrackedPeers) {
        return;
    }

    mPeers[mPeerCount].playerId = i_playerId;
    mPeers[mPeerCount].canPass = i_canPass;
    mPeers[mPeerCount].inWorld = i_inWorld;
    ++mPeerCount;
}

void WorldClock::forget_peer(std::uint32_t i_playerId) {
    for (std::size_t i = 0; i < mPeerCount; ++i) {
        if (mPeers[i].playerId != i_playerId) {
            continue;
        }
        // ★ A peer that leaves MUST be forgotten. Under the AllPlayers rule a stale entry left
        // behind by someone who quit inside a dungeon would stop the sun for the rest of the
        // session with nobody able to see why.
        mPeers[i] = mPeers[mPeerCount - 1];
        --mPeerCount;
        return;
    }
}

void WorldClock::receive(float i_daytime, std::uint16_t i_date) {
    mDaytime = i_daytime;
    mDate = i_date;
    mHaveRemoteClock = true;
}

void WorldClock::receive_override(std::uint32_t i_playerId, float i_daytime, std::uint16_t i_date) {
    Log.info("World clock: player {} reports its game set the time to {:.1f} (day {}); adopting it "
             "for the session.",
        i_playerId, i_daytime, i_date);
    mDaytime = i_daytime;
    mDate = i_date;
    mHaveRemoteClock = true;
}

bool WorldClock::consume_pending_override(float& o_daytime, std::uint16_t& o_date) {
    if (!mOverridePending) {
        return false;
    }

    mOverridePending = false;
    o_daytime = mOverrideDaytime;
    o_date = mOverrideDate;
    return true;
}

void WorldClock::clear() {
    mPeerCount = 0;
    mHaveRemoteClock = false;
    mHaveApplied = false;
    mOverridePending = false;
    mSentOverride = false;
}

}  // namespace dusk::mp
