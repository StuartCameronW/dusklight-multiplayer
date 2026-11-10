#include "object_channel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <tuple>
#include <vector>

#include "../session/network_manager.hpp"
#include "../session/trace.hpp"
#include "dusk/logging.h"
#include "object_capture.hpp"
#include "player_bridge.hpp"
#include "player_state.hpp"
#include "replication_manager.hpp"

/**
 * Phase 0 of the replicated-object channel. Spec: .claude/future-plans/mp-object-channel.md.
 * Wire layouts are documented in object_channel.hpp; every log line starts with "[mp-obj]" so a
 * two-instance run can be compared with one grep per end.
 */

namespace dusk::mp::objects {
namespace {

constexpr borealis::Log Log{"dusk::mp"};

/// How long a despawned ref stays known, so an ObjState that arrives after its ObjDespawn (the two
/// travel on different channels with no ordering between them) cannot resurrect it (spec §1.2).
constexpr std::uint64_t kTombstoneTicks = 60;
/// Entries per ObjState datagram, and the byte budget that also bounds it (spec §1.3, ~1200 MTU).
constexpr std::size_t kMaxStateEntries = 40;
constexpr std::size_t kStateDatagramBudget = 1100;
/// Retained playback samples per remote object. Well over kMaxDelayTicks of history.
constexpr std::size_t kMaxObjSamples = 64;
/// Local ticks a received despawn waits for playback to reach its tick before it is applied
/// anyway: an authority whose pose stream stalled must not leave the ghost up forever.
constexpr std::uint64_t kDespawnGraceTicks = 90;
/// Ghost creation backoff, and the attempts after which an object is left without a ghost.
constexpr std::uint64_t kGhostRetryTicks = 30;
constexpr int kGhostMaxFailures = 5;
/// A ghost that vanished after being alive is recreated. Only a vanish within
/// kGhostQuickVanishTicks of coming up counts toward giving up (a create-die loop); a scene load
/// here is normal.
constexpr int kGhostMaxVanishes = 10;
constexpr std::uint64_t kGhostQuickVanishTicks = 60;
constexpr std::uint32_t kNoGhost = 0xFFFFFFFFu;

/// The debug object (kind 0x7F): 150 units out, at chest height, about one orbit per 2.8 s.
constexpr float kDebugRadius = 150.0f;
constexpr float kDebugHeight = 100.0f;
constexpr std::uint16_t kDebugOrbitStep = 0x300;
constexpr int kDebugMaxTicks = 30 * 60 * 5;

constexpr double kS16ToRad = 3.14159265358979323846 / 32768.0;

/* ---- wire helpers ---------------------------------------------------------------------------- */

struct Header {
    ObjRef ref;
    std::uint8_t authority = 0;
    std::uint8_t kind = 0;
    std::uint32_t stageHash = 0;
    std::int8_t room = -1;
    std::uint8_t ownerEpoch = 0;
};

void write_header(Writer& w, const Header& h) {
    h.ref.write(w);
    w.write_u8(h.authority);
    w.write_u8(h.kind);
    w.write_u32(h.stageHash);
    w.write_u8(static_cast<std::uint8_t>(h.room));
    w.write_u8(h.ownerEpoch);
}

bool read_header(Reader& r, Header& h) {
    std::uint8_t room = 0;
    if (!h.ref.read(r) || !r.read_u8(h.authority) || !r.read_u8(h.kind) ||
        !r.read_u32(h.stageHash) || !r.read_u8(room) || !r.read_u8(h.ownerEpoch))
    {
        return false;
    }
    h.room = static_cast<std::int8_t>(room);
    return true;
}

void append(Writer& to, const Writer& from) {
    for (std::uint8_t b : from.data()) {
        to.write_u8(b);
    }
}

/* ---- state ----------------------------------------------------------------------------------- */

struct OwnedObj {
    ObjRef ref;
    ObjKind kind = ObjKind::None;
    std::uint32_t stageHash = 0;
    std::int8_t room = -1;
    std::uint8_t epoch = 0;
    /// What the owner says the object is now, and what last went on the wire.
    ObjPose pose;
    KindBlob blob;
    ObjPose sentPose;
    KindBlob sentBlob;
    std::uint64_t sentTick = 0;
    std::uint64_t spawnTick = 0;
    /* Debug kind only. */
    bool debug = false;
    std::uint16_t age = 0;
    std::uint16_t lifetime = 0;
    std::uint16_t orbitStart = 0;
};

struct Sample {
    std::uint64_t tick = 0;
    ObjPose pose;
    KindBlob blob;
};

struct PendingEvent {
    std::uint64_t tick = 0;
    std::uint8_t event = 0;
    KindBlob blob;
};

struct RemoteObj {
    ObjRef ref;
    std::uint8_t authority = 0;
    std::uint8_t kind = 0;
    std::uint32_t stageHash = 0;
    std::int8_t room = -1;
    std::uint8_t ownerEpoch = 0;
    std::uint64_t spawnTick = 0;
    std::deque<Sample> samples;
    std::deque<PendingEvent> events;

    bool despawnPending = false;
    std::uint64_t despawnTick = 0;
    std::uint8_t despawnReason = 0;
    std::uint64_t despawnSeenLocal = 0;

    std::uint32_t ghost = kNoGhost;
    bool ghostEverAlive = false;
    int ghostFailures = 0;
    int ghostVanishes = 0;
    std::uint64_t ghostAliveSince = 0;
    std::uint64_t ghostRetryAt = 0;
    bool ghostGaveUp = false;
    bool reachedLogged = false;
    RemoteLocation lastLoc = RemoteLocation::Here;
};

struct Claim {
    std::uint32_t holder = kNoClaim;
    ClaimKind kind = ClaimKind::Release;
};

struct State {
    std::map<ObjRef, OwnedObj> owned;
    std::map<ObjRef, RemoteObj> remote;
    /// ref -> local tick it expires on.
    std::map<ObjRef, std::uint64_t> tombstones;
    std::map<ObjRef, Claim> claims;
    std::uint16_t nextSerial = 1;
    std::uint64_t localTick = 0;

    /// The local Link's location as the far ends see it in OUR PlayerState: read with the same
    /// capture the pose stream uses, so the epoch stamped on a spawn is the very value the
    /// receivers compare it against.
    bool haveLocal = false;
    std::uint32_t localStage = 0;
    std::int8_t localRoom = -1;
    std::uint8_t localEpoch = 0;

    std::uint32_t droppedStates = 0;
};

State& S() {
    static State s;
    return s;
}

std::uint8_t local8() {
    return static_cast<std::uint8_t>(network_manager().local_player_id());
}

std::uint64_t now_tick() {
    return network_manager().sim_tick();
}

bool is_host() {
    return network_manager().role() == Role::Host;
}

/// Send through the session: client -> host, host -> every admitted client.
void send_packet(const Writer& w, std::uint8_t channel, bool reliable) {
    NetworkManager& nm = network_manager();
    if (!nm.session_live()) {
        return;
    }
    nm.send_to_session(w, channel, reliable);
}

bool refresh_local() {
    State& s = S();
    PlayerState st;
    if (!replication_manager().capture_local(st) || st.stageHash == 0) {
        return false;
    }
    s.haveLocal = true;
    s.localStage = st.stageHash;
    s.localRoom = st.roomNo;
    s.localEpoch = st.locEpoch;
    return true;
}

const char* location_name(RemoteLocation loc) {
    switch (loc) {
    case RemoteLocation::Here:
        return "here";
    case RemoteLocation::OtherStage:
        return "other-stage";
    case RemoteLocation::RoomNotShown:
        return "room-not-shown";
    }
    return "?";
}

bool pose_changed(const ObjPose& a, const ObjPose& b) {
    for (int i = 0; i < 3; i++) {
        if (std::fabs(a.pos[i] - b.pos[i]) > 0.01f || a.angle[i] != b.angle[i]) {
            return true;
        }
    }
    return (a.flags & ~kObjPoseResume) != (b.flags & ~kObjPoseResume);
}

/// The owner may speak for `ref`: a Transient it owns, or a Placed ref it holds the claim on.
bool authority_ok(const ObjRef& ref, std::uint8_t authority, std::uint32_t from) {
    if (authority != static_cast<std::uint8_t>(from)) {
        return false;
    }
    if (ref.tag == RefTag::Transient) {
        return ref.owner == static_cast<std::uint8_t>(from);
    }
    return claim_holder(ref) == from;
}

void tombstone(const ObjRef& ref) {
    State& s = S();
    s.tombstones[ref] = s.localTick + kTombstoneTicks;
}

void drop_remote(std::map<ObjRef, RemoteObj>::iterator it) {
    if (it->second.ghost != kNoGhost) {
        engine::destroy_ghost(it->second.ghost);
    }
    tombstone(it->first);
    S().remote.erase(it);
}

/* ---- owner side ------------------------------------------------------------------------------ */

Header owned_header(const OwnedObj& o) {
    Header h;
    h.ref = o.ref;
    h.authority = local8();
    h.kind = static_cast<std::uint8_t>(o.kind);
    h.stageHash = o.stageHash;
    h.room = o.room;
    h.ownerEpoch = o.epoch;
    return h;
}

/// The debug object's pose around a Link at `link`, `age` ticks in.
void debug_pose(const float link[3], std::uint16_t orbitStart, std::uint16_t age, ObjPose& out) {
    const std::uint16_t orbit = static_cast<std::uint16_t>(orbitStart + age * kDebugOrbitStep);
    const double rad = static_cast<std::int16_t>(orbit) * kS16ToRad;
    // TP yaw 0 faces +Z; sin drives X (cM_ssin / cM_scos convention).
    out.pos[0] = link[0] + static_cast<float>(std::sin(rad)) * kDebugRadius;
    out.pos[1] = link[1] + kDebugHeight;
    out.pos[2] = link[2] + static_cast<float>(std::cos(rad)) * kDebugRadius;
    out.angle[0] = 0;
    // Pointing along the orbit, so the arrow model reads as flying round him.
    out.angle[1] = static_cast<std::int16_t>(static_cast<std::uint16_t>(orbit + 0x4000));
    out.angle[2] = 0;
    out.flags = 0;
}

KindBlob debug_blob(std::uint16_t age, std::uint16_t lifetime) {
    Writer w;
    write_debug_blob(w, DebugBlob{age, lifetime});
    KindBlob b;
    b.assign(w);
    return b;
}

std::uint16_t allocate_serial() {
    State& s = S();
    for (int guard = 0; guard < 0x10000; guard++) {
        const std::uint16_t serial = s.nextSerial++;
        if (s.nextSerial == 0) {
            s.nextSerial = 1;  // 0 is never handed out, so a zeroed ref can never alias a real one
        }
        if (serial != 0 && s.owned.find(ObjRef::transient(local8(), serial)) == s.owned.end()) {
            return serial;
        }
    }
    return 0;
}

void step_debug_objects(std::vector<ObjRef>& expired) {
    State& s = S();
    float link[3] = {};
    std::int16_t yaw = 0;
    const bool haveLink = engine::local_link_pose(link, yaw);
    for (auto& entry : s.owned) {
        OwnedObj& o = entry.second;
        if (!o.debug) {
            continue;
        }
        o.age++;
        if (haveLink) {
            debug_pose(link, o.orbitStart, o.age, o.pose);
        }
        o.blob = debug_blob(o.age, o.lifetime);
        if (o.age == o.lifetime / 2) {
            send_event(o.ref, ObjEventId::DebugPing, KindBlob{});
        }
        if (o.age >= o.lifetime) {
            expired.push_back(o.ref);
        }
    }
}

/// Drop every owned object a scene change destroyed. No message: the receivers see the epoch
/// advance on the owner's own pose stream and despawn on their own (spec §1.4), which also covers
/// the case where we are about to disconnect.
void drop_stale_owned() {
    State& s = S();
    for (auto it = s.owned.begin(); it != s.owned.end();) {
        if (it->second.epoch != s.localEpoch || it->second.stageHash != s.localStage) {
            Log.info("[mp-obj] owner epoch drop {} kind={} epoch {} -> {} tick={}", it->first.str(),
                kind_name(static_cast<std::uint8_t>(it->second.kind)), it->second.epoch,
                s.localEpoch, now_tick());
            it = s.owned.erase(it);
        } else {
            ++it;
        }
    }
}

void flush_state_batch(Writer& body, std::size_t& count) {
    if (count == 0) {
        return;
    }
    State& s = S();
    Writer pkt;
    pkt.write_u8(static_cast<std::uint8_t>(PacketId::ObjState));
    pkt.write_u8(local8());
    pkt.write_u64(now_tick());
    pkt.write_u32(s.localStage);
    pkt.write_u8(s.localEpoch);
    pkt.write_u8(static_cast<std::uint8_t>(count));
    append(pkt, body);
    send_packet(pkt, kChannelState, false);
    body.clear();
    count = 0;
}

void stream_owned() {
    State& s = S();
    const std::uint64_t now = now_tick();
    Writer body;
    std::size_t count = 0;
    for (auto& entry : s.owned) {
        OwnedObj& o = entry.second;
        const ObjKindInfo* info = kind_info(static_cast<std::uint8_t>(o.kind));
        const std::uint64_t sendEvery =
            info != nullptr && info->sendEvery > 0 ? info->sendEvery : 1;
        const std::uint64_t forceEvery = info != nullptr ? info->forceEvery : 15;
        const std::uint64_t since = now - o.sentTick;
        const bool changed = pose_changed(o.pose, o.sentPose) || o.blob != o.sentBlob;
        if (since == 0 ||
            !((changed && since >= sendEvery) || (forceEvery != 0 && since >= forceEvery)))
        {
            continue;
        }
        ObjPose wire = o.pose;
        wire.flags = static_cast<std::uint8_t>(wire.flags & ~kObjPoseResume);
        if (changed && since > sendEvery) {
            wire.flags |= kObjPoseResume;
        }
        Writer e;
        o.ref.write(e);
        e.write_u8(static_cast<std::uint8_t>(o.kind));
        e.write_u8(static_cast<std::uint8_t>(o.room));
        write_pose(e, wire);
        write_blob(e, o.blob);
        // 15 bytes of packet header.
        if (count >= kMaxStateEntries || 15 + body.size() + e.size() > kStateDatagramBudget) {
            flush_state_batch(body, count);
        }
        append(body, e);
        count++;
        o.sentPose = o.pose;
        o.sentBlob = o.blob;
        o.sentTick = now;
    }
    flush_state_batch(body, count);
}

void trace_owned() {
    if (!trace::enabled()) {
        return;
    }
    for (const auto& entry : S().owned) {
        const OwnedObj& o = entry.second;
        Log.info("[mp-obj] own {} tick={} pos=({:.2f}, {:.2f}, {:.2f}) yaw={}", o.ref.str(),
            now_tick(), o.pose.pos[0], o.pose.pos[1], o.pose.pos[2], o.pose.angle[1]);
    }
}

void owner_tick() {
    State& s = S();
    if (s.owned.empty()) {
        return;
    }
    /* A queued scene change destroys every actor this end owns, exactly as vanilla deletes them.
     * Dropped silently: the far end sees our epoch advance on the pose stream and despawns them
     * itself ("epoch despawn"), which also holds if this end disconnects mid-load. */
    if (local_scene_change_queued()) {
        for (const auto& entry : s.owned) {
            Log.info("[mp-obj] owner scene-change drop {} kind={} tick={}", entry.first.str(),
                kind_name(static_cast<std::uint8_t>(entry.second.kind)), now_tick());
        }
        s.owned.clear();
        return;
    }
    if (refresh_local()) {
        drop_stale_owned();
    }
    std::vector<ObjRef> expired;
    step_debug_objects(expired);
    stream_owned();
    trace_owned();
    for (const ObjRef& ref : expired) {
        despawn_owned(ref, ObjDespawnReason::Expired);
    }
}

/* ---- receiver side --------------------------------------------------------------------------- */

const RemotePlayer* find_authority(std::uint8_t authority) {
    for (const auto& entry : replication_manager().players()) {
        if (static_cast<std::uint8_t>(entry.first) == authority) {
            return &entry.second;
        }
    }
    return nullptr;
}

void insert_sample(RemoteObj& o, Sample sample) {
    const bool resume = (sample.pose.flags & kObjPoseResume) != 0;
    sample.pose.flags = static_cast<std::uint8_t>(sample.pose.flags & ~kObjPoseResume);
    auto at = std::lower_bound(o.samples.begin(), o.samples.end(), sample.tick,
        [](const Sample& a, std::uint64_t t) { return a.tick < t; });
    if (at != o.samples.end() && at->tick == sample.tick) {
        return;  // duplicate
    }
    if (at == o.samples.begin() && !o.samples.empty() && o.samples.front().tick > sample.tick &&
        o.samples.size() >= kMaxObjSamples)
    {
        return;  // older than everything retained: playback is past it
    }
    /* The object sat still before this sample (nothing was sent while it did not change): hold the
     * previous sample until the tick before, so playback does not glide across the whole gap. */
    if (resume && at != o.samples.begin()) {
        const Sample& prev = *(at - 1);
        if (prev.tick + 1 < sample.tick) {
            Sample hold = prev;
            hold.tick = sample.tick - 1;
            at = o.samples.insert(at, hold) + 1;
        }
    }
    o.samples.insert(at, sample);
    while (o.samples.size() > kMaxObjSamples) {
        o.samples.pop_front();
    }
}

/// The sample at authority tick `played`: interpolated between the two around it, held at the
/// newest when playback has run past the stream (objects at rest send nothing). False before the
/// first sample, i.e. before the spawn on the authority's timeline.
bool sample_at(const RemoteObj& o, std::uint64_t played, ObjPose& out, KindBlob& blob) {
    if (o.samples.empty() || played < o.samples.front().tick) {
        return false;
    }
    std::size_t i = 0;
    while (i + 1 < o.samples.size() && o.samples[i + 1].tick <= played) {
        i++;
    }
    const Sample& a = o.samples[i];
    out = a.pose;
    blob = a.blob;
    if (i + 1 >= o.samples.size() || a.tick == played) {
        return true;
    }
    const Sample& b = o.samples[i + 1];
    const double t = static_cast<double>(played - a.tick) / static_cast<double>(b.tick - a.tick);
    for (int k = 0; k < 3; k++) {
        out.pos[k] = static_cast<float>(a.pose.pos[k] + (b.pose.pos[k] - a.pose.pos[k]) * t);
        const std::int16_t d =
            static_cast<std::int16_t>(static_cast<std::uint16_t>(b.pose.angle[k]) -
                                      static_cast<std::uint16_t>(a.pose.angle[k]));
        out.angle[k] = static_cast<std::int16_t>(
            static_cast<std::uint16_t>(a.pose.angle[k] + static_cast<int>(std::lround(d * t))));
    }
    return true;
}

void prune_samples(RemoteObj& o, std::uint64_t played) {
    while (o.samples.size() >= 2 && o.samples[1].tick <= played) {
        o.samples.pop_front();
    }
}

void upkeep_ghost(RemoteObj& o, bool reached, RemoteLocation loc, bool playable,
    const ObjPose& pose, const KindBlob& blob, std::uint64_t played) {
    State& s = S();
    if (loc == RemoteLocation::OtherStage) {
        if (o.ghost != kNoGhost) {
            engine::destroy_ghost(o.ghost);
            o.ghost = kNoGhost;
            o.ghostEverAlive = false;
        }
        return;
    }
    const ObjKindInfo* info = kind_info(o.kind);
    if (o.ghost == kNoGhost) {
        if (info == nullptr || !info->implemented || !reached || loc != RemoteLocation::Here ||
            !playable || o.ghostGaveUp || s.localTick < o.ghostRetryAt)
        {
            return;
        }
        o.ghost = engine::create_ghost(o.kind, o.authority, pose);
        if (o.ghost == kNoGhost) {
            o.ghostFailures++;
            o.ghostRetryAt = s.localTick + kGhostRetryTicks;
            Log.warn("[mp-obj] ghost create refused {} (attempt {})", o.ref.str(), o.ghostFailures);
        } else {
            Log.info("[mp-obj] ghost create {} kind={} proc={} played={}", o.ref.str(),
                kind_name(o.kind), o.ghost, played);
        }
    } else {
        switch (engine::look_up_ghost(o.ghost)) {
        case engine::GhostLookup::Alive:
            if (!o.ghostEverAlive) {
                o.ghostEverAlive = true;
                o.ghostAliveSince = s.localTick;
                Log.info("[mp-obj] ghost alive {} proc={} played={}", o.ref.str(), o.ghost, played);
            }
            engine::apply_ghost(o.ghost, pose,
                reached && loc == RemoteLocation::Here && (pose.flags & kObjPoseHidden) == 0, blob);
            break;
        case engine::GhostLookup::Creating:
            break;
        case engine::GhostLookup::Gone:
            if (o.ghostEverAlive) {
                /* A scene load or room unload on THIS end deletes every actor, ghosts included;
                 * that is normal and the ghost is simply rebuilt. Only a ghost that keeps dying
                 * soon after it came up is a loop worth stopping. */
                const bool quick = s.localTick - o.ghostAliveSince < kGhostQuickVanishTicks;
                if (quick) {
                    o.ghostVanishes++;
                }
                Log.info("[mp-obj] ghost vanished {} proc={} after {} ticks{}", o.ref.str(),
                    o.ghost, s.localTick - o.ghostAliveSince, quick ? " (quick: counted)" : "");
                if (o.ghostVanishes >= kGhostMaxVanishes) {
                    o.ghostGaveUp = true;
                    Log.error("[mp-obj] giving up on a ghost for {}: it died {} times right after "
                              "creation",
                        o.ref.str(), o.ghostVanishes);
                }
            } else {
                o.ghostFailures++;
                o.ghostRetryAt = s.localTick + kGhostRetryTicks;
                Log.warn("[mp-obj] ghost create failed {} proc={} (attempt {})", o.ref.str(),
                    o.ghost, o.ghostFailures);
            }
            o.ghost = kNoGhost;
            o.ghostEverAlive = false;
            break;
        }
    }
    if (o.ghostFailures >= kGhostMaxFailures && !o.ghostGaveUp) {
        o.ghostGaveUp = true;
        Log.error("[mp-obj] giving up on a ghost for {} after {} failed creates", o.ref.str(),
            o.ghostFailures);
    }
}

/**
 * Deliver every queued event with an origin tick <= `upTo` to the object's ghost, in order.
 * Returns false (and keeps the rest queued) while the ghost exists but its create() has not
 * finished: an event that reaches playback the tick its ghost is requested (a point-blank bomb
 * arrow, a bomb arrow exploding in the hand) must wait for it, not be dropped. With no ghost at
 * all (hidden room, unimplemented kind) the events are logged and dropped, as before.
 */
bool deliver_events(RemoteObj& o, std::uint64_t upTo, std::uint64_t played) {
    while (!o.events.empty() && o.events.front().tick <= upTo) {
        const PendingEvent& ev = o.events.front();
        const engine::GhostLookup look =
            o.ghost != kNoGhost ? engine::look_up_ghost(o.ghost) : engine::GhostLookup::Gone;
        if (look == engine::GhostLookup::Creating) {
            return false;
        }
        Log.info("[mp-obj] event {} id={} originTick={} played={}{}", o.ref.str(), ev.event,
            ev.tick, played, look == engine::GhostLookup::Alive ? "" : " (no ghost)");
        if (look == engine::GhostLookup::Alive) {
            engine::ghost_event(o.ghost, ev.event, ev.blob);
        }
        o.events.pop_front();
    }
    return true;
}

void receiver_tick() {
    State& s = S();
    const bool playable = world_is_playable();
    for (auto it = s.remote.begin(); it != s.remote.end();) {
        RemoteObj& o = it->second;
        const RemotePlayer* owner = find_authority(o.authority);
        const bool timeline = owner != nullptr && owner->buffer.has_data();
        /* ★ The owner-timeline rule: the object is shown at the tick its authority's puppet is
         * being shown at. Without a pose stream (none yet, or reset by a load) fall back to the
         * newest sample, which is at worst the delay ahead. */
        const std::uint64_t played = timeline ?
                                         owner->buffer.played_tick() :
                                         (o.samples.empty() ? o.spawnTick : o.samples.back().tick);

        // A scene change on the authority's machine destroyed its transients: no message needed.
        if (timeline && o.ref.tag == RefTag::Transient && owner->lastPlayed.stageHash != 0 &&
            played >= o.spawnTick && owner->lastPlayed.locEpoch != o.ownerEpoch)
        {
            Log.info("[mp-obj] epoch despawn {} kind={} ownerEpoch={} now={} played={}",
                o.ref.str(), kind_name(o.kind), o.ownerEpoch, owner->lastPlayed.locEpoch, played);
            auto dead = it++;
            drop_remote(dead);
            continue;
        }

        const bool despawnGraceOver = o.despawnPending &&
                                      s.localTick - o.despawnSeenLocal > kDespawnGraceTicks;
        if (o.despawnPending && (played >= o.despawnTick || despawnGraceOver) &&
            // Every event comes before its object's despawn on the authority's timeline, so play
            // them all first (an explode sent on the despawn tick used to be dropped unseen). A
            // ghost still being created holds the despawn until it can take them, within grace.
            (deliver_events(o, o.despawnTick, played) || despawnGraceOver))
        {
            Log.info("[mp-obj] despawn {} kind={} originTick={} played={} reason={}{}", o.ref.str(),
                kind_name(o.kind), o.despawnTick, played, o.despawnReason,
                played >= o.despawnTick ? "" : " (grace expired before playback reached it)");
            auto dead = it++;
            drop_remote(dead);
            continue;
        }

        ObjPose pose;
        KindBlob blob;
        const bool reached = sample_at(o, played, pose, blob);
        if (reached && !o.reachedLogged) {
            o.reachedLogged = true;
            Log.info("[mp-obj] playback reached {} spawnTick={} played={}{}", o.ref.str(),
                o.spawnTick, played, timeline ? "" : " (no authority timeline, newest sample)");
        }

        PlayerState probe;
        probe.stageHash = o.stageHash;
        probe.roomNo = o.room;
        const RemoteLocation loc = remote_location(probe);
        if (loc != o.lastLoc) {
            Log.info("[mp-obj] location {} {} -> {}", o.ref.str(), location_name(o.lastLoc),
                location_name(loc));
            o.lastLoc = loc;
        }

        upkeep_ghost(o, reached, loc, playable, pose, blob, played);

        deliver_events(o, played, played);

        if (reached && trace::enabled()) {
            Log.info("[mp-obj] ghost {} played={} pos=({:.2f}, {:.2f}, {:.2f}) yaw={} loc={} "
                     "ghost={}",
                o.ref.str(), played, pose.pos[0], pose.pos[1], pose.pos[2], pose.angle[1],
                location_name(loc), o.ghost != kNoGhost ? "yes" : "no");
        }

        prune_samples(o, played);
        ++it;
    }
}

void expire_tombstones() {
    State& s = S();
    for (auto it = s.tombstones.begin(); it != s.tombstones.end();) {
        if (s.localTick >= it->second) {
            it = s.tombstones.erase(it);
        } else {
            ++it;
        }
    }
}

/* ---- packet handlers ------------------------------------------------------------------------- */

void handle_spawn(Reader& r, std::uint32_t from) {
    Header h;
    std::uint64_t tick = 0;
    ObjPose pose;
    KindBlob blob;
    bool oversized = false;
    if (!read_header(r, h) || !r.read_u64(tick) || !read_pose(r, pose) ||
        !read_blob(r, blob, oversized))
    {
        Log.warn("[mp-obj] malformed ObjSpawn from player {}", from);
        return;
    }
    if (is_host() && !authority_ok(h.ref, h.authority, from)) {
        Log.warn(
            "[mp-obj] refused ObjSpawn {} from player {}: not its authority", h.ref.str(), from);
        return;
    }
    if (h.authority == local8()) {
        return;  // our own, echoed
    }
    if (oversized) {
        Log.warn("[mp-obj] ObjSpawn {} kind={} has an init blob over {} bytes; ignored",
            h.ref.str(), h.kind, kMaxKindBlob);
        return;
    }
    State& s = S();
    // A reliable spawn is authoritative: it ends any tombstone (a Placed object can be spawned
    // again by its next claimant within the tombstone window).
    s.tombstones.erase(h.ref);
    if (s.remote.find(h.ref) != s.remote.end()) {
        Log.debug("[mp-obj] duplicate ObjSpawn {}", h.ref.str());
        return;
    }
    RemoteObj o;
    o.ref = h.ref;
    o.authority = h.authority;
    o.kind = h.kind;
    o.stageHash = h.stageHash;
    o.room = h.room;
    o.ownerEpoch = h.ownerEpoch;
    o.spawnTick = tick;
    o.despawnSeenLocal = s.localTick;
    Sample first;
    first.tick = tick;
    first.pose = pose;
    first.blob = blob;
    insert_sample(o, first);
    PlayerState probe;
    probe.stageHash = h.stageHash;
    probe.roomNo = h.room;
    o.lastLoc = remote_location(probe);
    Log.info("[mp-obj] spawn recv {} kind={} ({}) tick={} authority={} stage={:08X} room={} "
             "epoch={} pos=({:.1f}, {:.1f}, {:.1f}) loc={}{}",
        h.ref.str(), h.kind, kind_name(h.kind), tick, h.authority, h.stageHash,
        static_cast<int>(h.room), h.ownerEpoch, pose.pos[0], pose.pos[1], pose.pos[2],
        location_name(o.lastLoc), kind_info(h.kind) != nullptr ? "" : " (unknown kind: no ghost)");
    s.remote.emplace(h.ref, std::move(o));
}

void handle_state(Reader& r, std::uint32_t from) {
    State& s = S();
    std::uint8_t authority = 0;
    std::uint64_t tick = 0;
    std::uint32_t stageHash = 0;
    std::uint8_t epoch = 0;
    std::uint8_t n = 0;
    if (!r.read_u8(authority) || !r.read_u64(tick) || !r.read_u32(stageHash) || !r.read_u8(epoch) ||
        !r.read_u8(n))
    {
        return;
    }
    if (authority == local8()) {
        return;
    }
    for (std::uint8_t i = 0; i < n; i++) {
        ObjRef ref;
        std::uint8_t kind = 0;
        std::uint8_t room = 0;
        Sample sample;
        bool oversized = false;
        if (!ref.read(r) || !r.read_u8(kind) || !r.read_u8(room) || !read_pose(r, sample.pose) ||
            !read_blob(r, sample.blob, oversized))
        {
            return;  // truncated: what parsed so far was applied, the rest is unreadable
        }
        if (is_host() && !authority_ok(ref, authority, from)) {
            continue;
        }
        auto it = s.remote.find(ref);
        if (it == s.remote.end() || oversized) {
            // Unknown (its spawn has not arrived yet, or it is tombstoned): dropped, and nothing is
            // lost — the spawn carries the initial state (spec §1.2).
            s.droppedStates++;
            continue;
        }
        RemoteObj& o = it->second;
        if (o.ownerEpoch != epoch || o.kind != kind || (o.despawnPending && tick > o.despawnTick)) {
            s.droppedStates++;
            continue;
        }
        o.room = static_cast<std::int8_t>(room);
        sample.tick = tick;
        insert_sample(o, sample);
    }
    (void)stageHash;
}

void handle_event(Reader& r, std::uint32_t from) {
    Header h;
    std::uint64_t tick = 0;
    std::uint8_t event = 0;
    KindBlob blob;
    bool oversized = false;
    if (!read_header(r, h) || !r.read_u64(tick) || !r.read_u8(event) ||
        !read_blob(r, blob, oversized))
    {
        Log.warn("[mp-obj] malformed ObjEvent from player {}", from);
        return;
    }
    if (is_host() && !authority_ok(h.ref, h.authority, from)) {
        Log.warn(
            "[mp-obj] refused ObjEvent {} from player {}: not its authority", h.ref.str(), from);
        return;
    }
    if (h.authority == local8()) {
        return;
    }
    State& s = S();
    auto it = s.remote.find(h.ref);
    if (it == s.remote.end()) {
        Log.info("[mp-obj] event recv for unknown {} id={} tick={}", h.ref.str(), event, tick);
        return;
    }
    PendingEvent ev;
    ev.tick = tick;
    ev.event = event;
    if (!oversized) {
        ev.blob = blob;
    }
    auto& events = it->second.events;
    auto at = std::upper_bound(events.begin(), events.end(), tick,
        [](std::uint64_t t, const PendingEvent& e) { return t < e.tick; });
    events.insert(at, ev);
    Log.info("[mp-obj] event recv {} id={} tick={}", h.ref.str(), event, tick);
}

void handle_despawn(Reader& r, std::uint32_t from) {
    Header h;
    std::uint64_t tick = 0;
    std::uint8_t reason = 0;
    if (!read_header(r, h) || !r.read_u64(tick) || !r.read_u8(reason)) {
        Log.warn("[mp-obj] malformed ObjDespawn from player {}", from);
        return;
    }
    if (is_host() && !authority_ok(h.ref, h.authority, from)) {
        Log.warn(
            "[mp-obj] refused ObjDespawn {} from player {}: not its authority", h.ref.str(), from);
        return;
    }
    if (h.authority == local8()) {
        return;
    }
    State& s = S();
    auto it = s.remote.find(h.ref);
    if (it == s.remote.end()) {
        tombstone(h.ref);
        Log.info(
            "[mp-obj] despawn recv for unknown {} tick={} reason={}", h.ref.str(), tick, reason);
        return;
    }
    it->second.despawnPending = true;
    it->second.despawnTick = tick;
    it->second.despawnReason = reason;
    it->second.despawnSeenLocal = s.localTick;
    Log.info("[mp-obj] despawn recv {} kind={} tick={} reason={}", h.ref.str(),
        kind_name(it->second.kind), tick, reason);
}

void broadcast_claim_result(const ObjRef& ref, std::uint32_t holder, ClaimKind kind) {
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::ObjClaimResult));
    ref.write(w);
    w.write_u8(holder == kNoClaim ? 0xFF : static_cast<std::uint8_t>(holder));
    w.write_u8(static_cast<std::uint8_t>(kind));
    send_packet(w, kChannelObjects, true);
}

/// Host: first claim heard wins; a release only counts from the holder.
void arbitrate(const ObjRef& ref, std::uint32_t claimant, ClaimKind kind) {
    State& s = S();
    auto it = s.claims.find(ref);
    if (kind == ClaimKind::Release) {
        if (it != s.claims.end() && it->second.holder == claimant) {
            s.claims.erase(it);
            Log.info("[mp-obj] claim released {} by player {}", ref.str(), claimant);
            broadcast_claim_result(ref, kNoClaim, ClaimKind::Release);
        }
        return;
    }
    if (it == s.claims.end()) {
        s.claims[ref] = Claim{claimant, kind};
        Log.info("[mp-obj] claim granted {} to player {} kind={}", ref.str(), claimant,
            static_cast<int>(kind));
        broadcast_claim_result(ref, claimant, kind);
    } else if (it->second.holder == claimant) {
        broadcast_claim_result(ref, claimant, it->second.kind);
    } else {
        Log.info("[mp-obj] claim refused {} to player {}: held by player {}", ref.str(), claimant,
            it->second.holder);
        broadcast_claim_result(ref, it->second.holder, it->second.kind);
    }
}

void handle_claim(Reader& r, std::uint32_t from) {
    Header h;
    std::uint64_t tick = 0;
    std::uint8_t kind = 0;
    if (!read_header(r, h) || !r.read_u64(tick) || !r.read_u8(kind)) {
        Log.warn("[mp-obj] malformed ObjClaim from player {}", from);
        return;
    }
    if (!is_host()) {
        return;  // host-bound only
    }
    if (h.ref.tag == RefTag::Transient) {
        return;  // transients are never claimed: their owner is their authority
    }
    arbitrate(h.ref, from, static_cast<ClaimKind>(kind));
}

void handle_claim_result(Reader& r, std::uint32_t from) {
    ObjRef ref;
    std::uint8_t granted = 0;
    std::uint8_t kind = 0;
    if (!ref.read(r) || !r.read_u8(granted) || !r.read_u8(kind)) {
        Log.warn("[mp-obj] malformed ObjClaimResult from player {}", from);
        return;
    }
    if (is_host()) {
        return;  // client-bound only; the host's own table is the authority
    }
    State& s = S();
    if (granted == 0xFF) {
        s.claims.erase(ref);
    } else {
        s.claims[ref] = Claim{granted, static_cast<ClaimKind>(kind)};
    }
    Log.info("[mp-obj] claim result {} -> {} kind={}", ref.str(),
        granted == 0xFF ? std::string("released") : "player " + std::to_string(granted),
        static_cast<int>(kind));
}

}  // namespace

/* ---- ObjRef ---------------------------------------------------------------------------------- */

bool ObjRef::operator<(const ObjRef& o) const {
    return std::tie(tag, owner, serial, stageHash, room, setId, procName, home[0], home[1],
               home[2]) < std::tie(o.tag, o.owner, o.serial, o.stageHash, o.room, o.setId,
                              o.procName, o.home[0], o.home[1], o.home[2]);
}

bool ObjRef::operator==(const ObjRef& o) const {
    return !(*this < o) && !(o < *this);
}

void ObjRef::write(Writer& w) const {
    w.write_u8(static_cast<std::uint8_t>(tag));
    switch (tag) {
    case RefTag::Transient:
        w.write_u8(owner);
        w.write_u16(serial);
        break;
    case RefTag::Placed:
        w.write_u32(stageHash);
        w.write_u8(static_cast<std::uint8_t>(room));
        w.write_u16(setId);
        break;
    case RefTag::PlacedNoId:
        w.write_u32(stageHash);
        w.write_u8(static_cast<std::uint8_t>(room));
        w.write_u16(procName);
        w.write_s16(home[0]);
        w.write_s16(home[1]);
        w.write_s16(home[2]);
        break;
    }
}

bool ObjRef::read(Reader& r) {
    *this = ObjRef();
    std::uint8_t rawTag = 0;
    if (!r.read_u8(rawTag)) {
        return false;
    }
    std::uint8_t rawRoom = 0;
    switch (rawTag) {
    case 0:
        tag = RefTag::Transient;
        return r.read_u8(owner) && r.read_u16(serial);
    case 1:
        tag = RefTag::Placed;
        if (!r.read_u32(stageHash) || !r.read_u8(rawRoom) || !r.read_u16(setId)) {
            return false;
        }
        room = static_cast<std::int8_t>(rawRoom);
        return true;
    case 2:
        tag = RefTag::PlacedNoId;
        if (!r.read_u32(stageHash) || !r.read_u8(rawRoom) || !r.read_u16(procName) ||
            !r.read_s16(home[0]) || !r.read_s16(home[1]) || !r.read_s16(home[2]))
        {
            return false;
        }
        room = static_cast<std::int8_t>(rawRoom);
        return true;
    default:
        return false;  // an unknown tag cannot be skipped: its length is unknown
    }
}

std::string ObjRef::str() const {
    char buf[96];
    switch (tag) {
    case RefTag::Transient:
        std::snprintf(buf, sizeof(buf), "T%u:%u", static_cast<unsigned>(owner),
            static_cast<unsigned>(serial));
        break;
    case RefTag::Placed:
        std::snprintf(buf, sizeof(buf), "P%08X/%d/%u", static_cast<unsigned>(stageHash),
            static_cast<int>(room), static_cast<unsigned>(setId));
        break;
    case RefTag::PlacedNoId:
        std::snprintf(buf, sizeof(buf), "N%08X/%d/0x%03X@(%d,%d,%d)",
            static_cast<unsigned>(stageHash), static_cast<int>(room),
            static_cast<unsigned>(procName), home[0], home[1], home[2]);
        break;
    default:
        std::snprintf(buf, sizeof(buf), "?");
        break;
    }
    return buf;
}

/* ---- the network_manager interface ----------------------------------------------------------- */

void on_packet(PacketId id, Reader& r, std::uint32_t fromPlayerId) {
    switch (id) {
    case PacketId::ObjSpawn:
        handle_spawn(r, fromPlayerId);
        break;
    case PacketId::ObjState:
        handle_state(r, fromPlayerId);
        break;
    case PacketId::ObjEvent:
        handle_event(r, fromPlayerId);
        break;
    case PacketId::ObjDespawn:
        handle_despawn(r, fromPlayerId);
        break;
    case PacketId::ObjClaim:
        handle_claim(r, fromPlayerId);
        break;
    case PacketId::ObjClaimResult:
        handle_claim_result(r, fromPlayerId);
        break;
    default:
        break;
    }
}

void tick() {
    State& s = S();
    s.localTick++;
    if (!network_manager().is_active()) {
        return;
    }
    engine::census_tick();
    expire_tombstones();
    capture::tick_arrow();
    capture::tick_bomb();
    capture::tick_boomerang();
    capture::tick_rodball();
    owner_tick();
    receiver_tick();
    if (s.localTick % 300 == 0 && (s.droppedStates != 0 || !s.remote.empty() || !s.owned.empty())) {
        Log.info("[mp-obj] summary owned={} remote={} tombstones={} claims={} droppedStates={}",
            s.owned.size(), s.remote.size(), s.tombstones.size(), s.claims.size(), s.droppedStates);
    }
}

void on_peer_left(std::uint32_t playerId) {
    State& s = S();
    const std::uint8_t id8 = static_cast<std::uint8_t>(playerId);
    for (auto it = s.remote.begin(); it != s.remote.end();) {
        if (it->second.authority == id8) {
            Log.info("[mp-obj] peerleft despawn {} kind={} player={} localTick={}", it->first.str(),
                kind_name(it->second.kind), playerId, s.localTick);
            auto dead = it++;
            drop_remote(dead);
        } else {
            ++it;
        }
    }
    const bool host = is_host();
    for (auto it = s.claims.begin(); it != s.claims.end();) {
        if (it->second.holder == playerId) {
            Log.info("[mp-obj] claim released {} (player {} left)", it->first.str(), playerId);
            if (host) {
                broadcast_claim_result(it->first, kNoClaim, ClaimKind::Release);
            }
            it = s.claims.erase(it);
        } else {
            ++it;
        }
    }
}

void reset() {
    State& s = S();
    if (!s.owned.empty() || !s.remote.empty()) {
        Log.info("[mp-obj] reset: dropping {} owned and {} remote object(s)", s.owned.size(),
            s.remote.size());
    }
    for (auto& entry : s.remote) {
        if (entry.second.ghost != kNoGhost) {
            engine::destroy_ghost(entry.second.ghost);
        }
    }
    capture::reset_arrow();
    capture::reset_bomb();
    capture::reset_boomerang();
    capture::reset_rodball();
    const std::uint64_t localTick = s.localTick;
    s = State();
    s.localTick = localTick;
}

bool relay_allowed(PacketId id, Reader r, std::uint32_t fromPlayerId) {
    switch (id) {
    case PacketId::ObjSpawn:
    case PacketId::ObjEvent:
    case PacketId::ObjDespawn: {
        Header h;
        return read_header(r, h) && authority_ok(h.ref, h.authority, fromPlayerId);
    }
    case PacketId::ObjState: {
        std::uint8_t authority = 0;
        std::uint64_t tick = 0;
        std::uint32_t stageHash = 0;
        std::uint8_t epoch = 0;
        std::uint8_t n = 0;
        if (!r.read_u8(authority) || !r.read_u64(tick) || !r.read_u32(stageHash) ||
            !r.read_u8(epoch) || !r.read_u8(n) ||
            authority != static_cast<std::uint8_t>(fromPlayerId))
        {
            return false;
        }
        for (std::uint8_t i = 0; i < n; i++) {
            ObjRef ref;
            std::uint8_t kind = 0;
            std::uint8_t room = 0;
            ObjPose pose;
            KindBlob blob;
            bool oversized = false;
            if (!ref.read(r) || !r.read_u8(kind) || !r.read_u8(room) || !read_pose(r, pose) ||
                !read_blob(r, blob, oversized) || !authority_ok(ref, authority, fromPlayerId))
            {
                return false;
            }
        }
        return r.ok();
    }
    default:
        return false;
    }
}

/* ---- owner-side API -------------------------------------------------------------------------- */

bool spawn_transient(ObjKind kind, const ObjPose& pose, const KindBlob& init, ObjRef& out) {
    NetworkManager& nm = network_manager();
    if (!nm.is_active() || !refresh_local()) {
        return false;
    }
    if (init.len > kMaxKindBlob) {
        return false;
    }
    State& s = S();
    const std::uint16_t serial = allocate_serial();
    if (serial == 0) {
        Log.error("[mp-obj] no free object serial");
        return false;
    }
    OwnedObj o;
    o.ref = ObjRef::transient(local8(), serial);
    o.kind = kind;
    o.stageHash = s.localStage;
    o.room = s.localRoom;
    o.epoch = s.localEpoch;
    o.pose = pose;
    o.blob = init;
    o.sentPose = pose;
    o.sentBlob = init;
    o.spawnTick = nm.sim_tick();
    o.sentTick = o.spawnTick;

    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::ObjSpawn));
    write_header(w, owned_header(o));
    w.write_u64(o.spawnTick);
    write_pose(w, pose);
    write_blob(w, init);
    send_packet(w, kChannelObjects, true);

    Log.info("[mp-obj] spawn {} kind=0x{:02X} ({}) tick={} stage={:08X} room={} epoch={} "
             "pos=({:.1f}, {:.1f}, {:.1f})",
        o.ref.str(), static_cast<unsigned>(kind), kind_name(static_cast<std::uint8_t>(kind)),
        o.spawnTick, o.stageHash, static_cast<int>(o.room), o.epoch, pose.pos[0], pose.pos[1],
        pose.pos[2]);
    out = o.ref;
    s.owned.emplace(o.ref, o);
    return true;
}

bool start_debug_object(int ticks) {
    if (!network_manager().is_active()) {
        Log.error("[mp-obj] objtest: no multiplayer session");
        return false;
    }
    float link[3] = {};
    std::int16_t yaw = 0;
    if (!engine::local_link_pose(link, yaw)) {
        Log.error("[mp-obj] objtest: no Link to orbit");
        return false;
    }
    const int lifetime = std::clamp(ticks, 2, kDebugMaxTicks);
    const std::uint16_t orbitStart = static_cast<std::uint16_t>(yaw);
    ObjPose pose;
    debug_pose(link, orbitStart, 0, pose);
    ObjRef ref;
    if (!spawn_transient(
            ObjKind::Debug, pose, debug_blob(0, static_cast<std::uint16_t>(lifetime)), ref))
    {
        Log.error("[mp-obj] objtest: spawn refused (no local location yet?)");
        return false;
    }
    OwnedObj& o = S().owned[ref];
    o.debug = true;
    o.lifetime = static_cast<std::uint16_t>(lifetime);
    o.orbitStart = orbitStart;
    Log.info("[mp-obj] objtest {} lifetime={} ticks", ref.str(), lifetime);
    return true;
}

void update_owned(const ObjRef& ref, const ObjPose& pose, const KindBlob& state) {
    auto it = S().owned.find(ref);
    if (it == S().owned.end()) {
        return;
    }
    it->second.pose = pose;
    it->second.blob = state;
}

void send_event(const ObjRef& ref, ObjEventId event, const KindBlob& blob) {
    auto it = S().owned.find(ref);
    if (it == S().owned.end()) {
        return;
    }
    const std::uint64_t tick = now_tick();
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::ObjEvent));
    write_header(w, owned_header(it->second));
    w.write_u64(tick);
    w.write_u8(static_cast<std::uint8_t>(event));
    write_blob(w, blob);
    send_packet(w, kChannelObjects, true);
    Log.info("[mp-obj] event {} id={} tick={}", ref.str(), static_cast<unsigned>(event), tick);
}

void despawn_owned(const ObjRef& ref, ObjDespawnReason reason) {
    State& s = S();
    auto it = s.owned.find(ref);
    if (it == s.owned.end()) {
        return;
    }
    const std::uint64_t tick = now_tick();
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::ObjDespawn));
    write_header(w, owned_header(it->second));
    w.write_u64(tick);
    w.write_u8(static_cast<std::uint8_t>(reason));
    send_packet(w, kChannelObjects, true);
    Log.info("[mp-obj] despawn {} kind={} tick={} reason={} spawnTick={}", ref.str(),
        kind_name(static_cast<std::uint8_t>(it->second.kind)), tick, static_cast<unsigned>(reason),
        it->second.spawnTick);
    s.owned.erase(it);
}

/* ---- claims ---------------------------------------------------------------------------------- */

namespace {
void send_claim(const ObjRef& ref, ClaimKind kind, std::uint32_t stageHash, std::int8_t room) {
    Header h;
    h.ref = ref;
    h.authority = local8();
    h.kind = static_cast<std::uint8_t>(ObjKind::None);
    h.stageHash = stageHash;
    h.room = room;
    h.ownerEpoch = S().localEpoch;
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::ObjClaim));
    write_header(w, h);
    w.write_u64(now_tick());
    w.write_u8(static_cast<std::uint8_t>(kind));
    send_packet(w, kChannelObjects, true);
}
}  // namespace

void request_claim(const ObjRef& ref, ClaimKind kind, std::uint32_t stageHash, std::int8_t room) {
    if (ref.tag == RefTag::Transient || !network_manager().is_active()) {
        return;
    }
    if (is_host()) {
        arbitrate(ref, network_manager().local_player_id(), kind);
    } else {
        send_claim(ref, kind, stageHash, room);
    }
}

void release_claim(const ObjRef& ref, std::uint32_t stageHash, std::int8_t room) {
    request_claim(ref, ClaimKind::Release, stageHash, room);
}

std::uint32_t claim_holder(const ObjRef& ref) {
    const auto& claims = S().claims;
    auto it = claims.find(ref);
    return it != claims.end() ? it->second.holder : kNoClaim;
}

}  // namespace dusk::mp::objects
