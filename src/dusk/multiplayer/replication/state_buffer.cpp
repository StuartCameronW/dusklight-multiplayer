#include "state_buffer.hpp"

#include <algorithm>
#include <cmath>

namespace dusk::mp {

void StateBuffer::push(std::uint64_t tick, const PlayerState& state) {
    // Already played past this one — it arrived too late to be useful, so dropping it is strictly
    // better than rewinding the puppet.
    if (mStarted && static_cast<double>(tick) < mPlaybackTick - 1.0) {
        return;
    }

    // Newest-first is the overwhelmingly common case on an ordered-enough link, so check the tail
    // before doing any searching.
    if (mSamples.empty() || tick > mSamples.back().tick) {
        mSamples.push_back(Sample{tick, state});
    } else {
        const auto it = std::lower_bound(mSamples.begin(), mSamples.end(), tick,
            [](const Sample& sample, std::uint64_t value) { return sample.tick < value; });
        if (it != mSamples.end() && it->tick == tick) {
            // Duplicate: keep the first arrival. A retransmit carries no newer information.
            return;
        }
        mSamples.insert(it, Sample{tick, state});
    }

    while (mSamples.size() > kMaxSamples) {
        mSamples.pop_front();
    }
}

void StateBuffer::reset() {
    mSamples.clear();
    mPlaybackTick = 0.0;
    mStarted = false;
    mTicksSinceStarvation = 0;
    mConsecutiveStarvations = 0;
    // Also cleared: after a warp the stream restarts from nothing, so the first starvations are
    // the sender loading the new scene, not a degraded link.
    mEverInterpolated = false;
    // Deliberately keep mDelayTicks: the link's jitter is a property of the connection, not of the
    // scene, so a warp shouldn't throw away what we learned about it.
}

void StateBuffer::on_starved() {
    ++mStarvations;
    ++mConsecutiveStarvations;
    mTicksSinceStarvation = 0;

    if (!mEverInterpolated) {
        // The stream has not started properly yet — one or two poses have arrived and playback
        // trivially outruns them. Widening for that would charge the session permanent latency for
        // the other end's loading screen, which is the "everyone pays the worst player" trap this
        // buffer exists to avoid.
        //
        // Honest scope: measurement showed this guard does NOT cover the startup burst actually
        // seen in two-instance runs (27 starvations right after the sender enters the world). By
        // then interpolation has genuinely begun, so those take the normal path and are capped by
        // kStallStarvationTicks instead — delay peaks near 7 ticks and decays back to ~3.5 over
        // the following minute. This guard covers only the colder case before the first successful
        // interpolation, which is real but was not the one that showed up in the trace.
        return;
    }

    if (mConsecutiveStarvations > kStallStarvationTicks) {
        // The sender has stalled rather than jittered. Widening the buffer cannot bridge a stall,
        // it just adds latency that outlives the stall, so stop growing and ride it out.
        return;
    }

    // Widen fast. A visible stutter costs more than a tick of extra latency.
    mDelayTicks = std::min(kMaxDelayTicks, mDelayTicks + 1.0);
}

void StateBuffer::on_clean() {
    mConsecutiveStarvations = 0;
    ++mTicksSinceStarvation;
    if (mTicksSinceStarvation >= kShrinkWindowTicks && mDelayTicks > kMinDelayTicks) {
        // Narrow slowly, and only after a long clean stretch, so one hiccup doesn't tax the whole
        // session but a genuinely improved link does get its responsiveness back.
        mDelayTicks = std::max(kMinDelayTicks, mDelayTicks - 0.5);
        mTicksSinceStarvation = 0;
    }
}

bool StateBuffer::advance(PlayerState& out) {
    if (mSamples.empty()) {
        return false;
    }

    const double newest = static_cast<double>(mSamples.back().tick);

    if (!mStarted) {
        mPlaybackTick = newest - mDelayTicks;
        mStarted = true;
    } else {
        const double target = newest - mDelayTicks;
        const double error = target - mPlaybackTick;
        if (std::fabs(error) > kSnapTicks) {
            // A warp, a load, or a long stall. Gliding across the gap would look far worse than
            // re-seating, so cut straight to the correct place.
            mPlaybackTick = target;
            ++mSnaps;
        } else {
            mPlaybackTick += 1.0 + std::clamp(error * kCatchupGain, -kMaxTimeWarp, kMaxTimeWarp);
        }
    }

    const double oldest = static_cast<double>(mSamples.front().tick);
    if (mPlaybackTick <= oldest) {
        // Still filling the buffer after a (re)start. Hold the first pose rather than extrapolate
        // from a single sample, which would be a guess with nothing behind it.
        out = mSamples.front().state;
        on_clean();
        return true;
    }

    if (mPlaybackTick >= newest) {
        // Nothing left to interpolate towards: the sender's data has not arrived. Hold the last
        // known pose. Extrapolating here is what makes puppets skate into walls under packet loss.
        out = mSamples.back().state;
        on_starved();
        return true;
    }

    // Reaching here means the cursor sits between two real samples, which is the definition of a
    // healthy stream. Latched so on_starved() can tell "never started" from "hiccuped".
    mEverInterpolated = true;

    // Find the pair bracketing the cursor. The deque is tick-ordered and short (a couple of dozen
    // entries at worst), so a linear scan from the back is cheaper than binary searching.
    std::size_t upper = mSamples.size() - 1;
    while (upper > 0 && static_cast<double>(mSamples[upper - 1].tick) > mPlaybackTick) {
        --upper;
    }

    const Sample& a = mSamples[upper - 1];
    const Sample& b = mSamples[upper];
    const double span = static_cast<double>(b.tick - a.tick);
    const double t = span > 0.0 ? (mPlaybackTick - static_cast<double>(a.tick)) / span : 0.0;
    out = lerp_state(a.state, b.state, static_cast<float>(t));

    // Keep one sample behind the cursor so the next tick still has something to interpolate from.
    while (mSamples.size() > 2 && static_cast<double>(mSamples[1].tick) < mPlaybackTick) {
        mSamples.pop_front();
    }

    on_clean();
    return true;
}

}  // namespace dusk::mp
