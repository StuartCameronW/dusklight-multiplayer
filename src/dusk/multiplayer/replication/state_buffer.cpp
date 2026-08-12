#include "state_buffer.hpp"

#include <algorithm>
#include <cmath>

namespace dusk::mp {

void StateBuffer::push(std::uint64_t tick, const PlayerState& state) {
    // Ticks are the SENDER's, so the timeline stays uniform however jittered arrival was. The cost
    // is that the origin is not ours: a peer that restarts its session restarts its tick counter,
    // and every sample would then look hopelessly stale and be dropped forever — a puppet frozen
    // for the rest of the session. A sample this far behind the cursor is a new origin rather than
    // a late packet, so start over on it.
    if (mStarted && static_cast<double>(tick) + kSnapTicks < mPlaybackTick) {
        reset();
    }

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
    mStarveNewestAtStart = 0.0;
    mStarveWidened = 0.0;
    // Also cleared: after a warp the stream restarts from nothing, so the first starvations are
    // the sender loading the new scene, not a degraded link.
    mEverInterpolated = false;
    // Deliberately keep mDelayTicks: the link's jitter is a property of the connection, not of the
    // scene, so a warp shouldn't throw away what we learned about it.
}

void StateBuffer::on_starved(double newest) {
    ++mStarvations;
    if (mConsecutiveStarvations == 0) {
        // Opening a new run: remember where the sender's clock had got to, so on_clean() can work
        // out afterwards whether it kept running through the gap.
        mStarveNewestAtStart = newest;
        mStarveWidened = 0.0;
    }
    ++mConsecutiveStarvations;
    mTicksSinceStarvation = 0;

    if (!mEverInterpolated) {
        // The stream has not started properly yet — one or two poses have arrived and playback
        // trivially outruns them. Widening for that would charge the session permanent latency for
        // the other end's loading screen, which is the "everyone pays the worst player" trap this
        // buffer exists to avoid.
        //
        // Honest scope: this guard only ever covered the cold case before the first successful
        // interpolation. The bursts that actually dominated two-instance runs arrived AFTER
        // interpolation had begun, so they took the normal path below and ratcheted the delay to
        // its 12-tick ceiling. That is no longer this guard's problem to solve, because the cause
        // was upstream — senders went silent whenever they had no pose to report, and silence is
        // indistinguishable from loss. NetworkManager::wire_local_state() now keeps the stream
        // running, which took the same 200 s two-instance run from 162 starvations pinned at 12.0
        // ticks down to 2 starvations peaking at 3.0.
        return;
    }

    if (mConsecutiveStarvations > kStallStarvationTicks) {
        // The sender has stalled rather than jittered. Widening the buffer cannot bridge a stall,
        // it just adds latency that outlives the stall, so stop growing and ride it out.
        return;
    }

    // Widen fast. A visible stutter costs more than a tick of extra latency.
    const double before = mDelayTicks;
    mDelayTicks = std::min(kMaxDelayTicks, mDelayTicks + 1.0);
    mStarveWidened += mDelayTicks - before;
}

void StateBuffer::on_clean(double newest) {
    if (mConsecutiveStarvations > 0) {
        // ★ The stream just resumed, so the gap can finally be diagnosed — and widening is the
        // right answer to only one of the two things it might have been.
        //
        // If the LINK hiccuped, the sender went on producing a tick per tick throughout and the
        // packets merely arrived late, in a clump. Its counter will have advanced by roughly the
        // number of ticks we sat waiting. That is genuine jitter and a deeper buffer really does
        // absorb the next one, so the widening is earned and stays.
        //
        // If the SENDER stalled — loading a room, a shader compile, a frame hitch — its counter
        // barely moved, because the data we were waiting for was never produced. No buffer depth
        // can conjure that, so the widening bought precisely nothing, and left alone it would be
        // charged to the session as latency for the next minute. Give it back.
        //
        // Note this degrades in the safe direction: real packet LOSS leaves the sender's counter
        // racing ahead of what we received, which reads as jitter and keeps the buffer wide.
        const double produced = newest - mStarveNewestAtStart;
        if (produced * 2.0 < static_cast<double>(mConsecutiveStarvations)) {
            mDelayTicks = std::max(kMinDelayTicks, mDelayTicks - mStarveWidened);
        }
        mStarveWidened = 0.0;
    }

    mConsecutiveStarvations = 0;
    ++mTicksSinceStarvation;
    if (mTicksSinceStarvation >= kShrinkWindowTicks && mDelayTicks > kMinDelayTicks) {
        // Still asymmetric — widening is 30x faster than this — but on a timescale a player can
        // actually feel the end of, so one hiccup no longer taxes the rest of the session.
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
        on_clean(newest);
        return true;
    }

    if (mPlaybackTick >= newest) {
        // Nothing left to interpolate towards: the sender's data has not arrived. Hold the last
        // known pose. Extrapolating here is what makes puppets skate into walls under packet loss.
        out = mSamples.back().state;
        on_starved(newest);
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

    on_clean(newest);
    return true;
}

}  // namespace dusk::mp
