#pragma once

#include <cstdint>
#include <deque>

#include "player_state.hpp"

/**
 * ★ Adaptive snapshot interpolation buffer — one per remote player.
 *
 * This is where Dusklight beats the Dolphin mod (see 08, "do better" #4). TPOnline exposes a
 * manual `/buffer N` knob where each unit is a fixed 33 ms of added delay: every player pays the
 * worst player's latency, and a wrong setting either stutters or adds delay for nothing.
 *
 * Here the delay is measured and adjusted continuously instead:
 *  - Snapshots are held with the sender's sim tick and played back on a cursor that trails the
 *    newest sample by an adaptive `delay_ticks()`.
 *  - Running out of future data (a "starvation") widens the buffer immediately, because stutter
 *    is far more visible than latency.
 *  - A long clean stretch narrows it again slowly, so a brief network hiccup doesn't permanently
 *    tax the session.
 *  - Corrections are applied as a small time-warp (at most a quarter tick per tick) rather than a
 *    jump, so re-converging is invisible.
 *
 * Everything is measured in SIM TICKS, never milliseconds or rendered frames: the pump runs on
 * the 30 Hz sim tick and the measured RTT floor is one tick (00-status.md).
 */

namespace dusk::mp {

class StateBuffer {
public:
    /// Never trail by less than this — one tick of slack absorbs ordinary arrival jitter.
    static constexpr double kMinDelayTicks = 2.0;
    /// Beyond this the puppet feels laggy enough that more buffering is the wrong trade.
    static constexpr double kMaxDelayTicks = 12.0;
    /// A gap larger than this means a warp, load or long stall — re-seat instead of time-warping
    /// across it, which would send the puppet gliding across the map.
    ///
    /// Sized generously (~1.5 s) on purpose. Sim ticks are NOT paced in real time: fapGm_Execute
    /// runs 0, 1 or N times per rendered frame, so snapshots arrive in bursts and the newest tick
    /// can jump 20+ at once. An earlier, tighter value read those bursts as discontinuities and
    /// snapped constantly, throwing away good buffered data. Only a genuine warp exceeds this.
    static constexpr double kSnapTicks = 45.0;
    /// Ceiling on the per-tick playback-rate correction. Small enough to stay imperceptible, large
    /// enough to absorb a burst within a couple of seconds.
    static constexpr double kMaxTimeWarp = 0.35;
    /// Proportional gain on the trailing-distance error.
    static constexpr double kCatchupGain = 0.10;
    /// Clean ticks required before the buffer dares to narrow again (~5 s at 30 Hz).
    static constexpr std::uint32_t kShrinkWindowTicks = 150;
    /// Consecutive starved ticks after which we stop treating this as jitter. A long dry spell is
    /// the sender stalling (a load, a hitch); widening the buffer for that adds permanent latency
    /// and fixes nothing, so past this point we hold the pose without inflating further.
    static constexpr std::uint32_t kStallStarvationTicks = 5;
    /// Hard cap on retained samples, so a flood can't grow the deque without bound.
    static constexpr std::size_t kMaxSamples = 64;

    /// Insert a received snapshot. Tolerates duplicates and out-of-order arrival (it is sent on
    /// an unreliable channel), and ignores samples the cursor has already passed.
    void push(std::uint64_t tick, const PlayerState& state);

    /// Advance the playback cursor one sim tick and produce the pose to display.
    /// Returns false while nothing has been received yet — the caller should not draw a puppet.
    bool advance(PlayerState& out);

    /// Drop all history and re-seat on the next sample. Use on scene changes and reconnects.
    void reset();

    bool has_data() const { return !mSamples.empty(); }
    std::size_t sample_count() const { return mSamples.size(); }
    double delay_ticks() const { return mDelayTicks; }
    std::uint32_t starvation_count() const { return mStarvations; }
    std::uint32_t snap_count() const { return mSnaps; }

private:
    struct Sample {
        std::uint64_t tick = 0;
        PlayerState state;
    };

    void on_starved();
    void on_clean();

    std::deque<Sample> mSamples;
    double mPlaybackTick = 0.0;
    double mDelayTicks = kMinDelayTicks;
    bool mStarted = false;
    /// True once the cursor has sat between two real samples. Until then a starvation means the
    /// stream has not started, not that the link is bad, and must not widen the buffer.
    bool mEverInterpolated = false;

    std::uint32_t mTicksSinceStarvation = 0;
    std::uint32_t mConsecutiveStarvations = 0;
    std::uint32_t mStarvations = 0;
    std::uint32_t mSnaps = 0;
};

}  // namespace dusk::mp
