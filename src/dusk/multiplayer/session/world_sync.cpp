#include "world_sync.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../policy/mp_policy.hpp"
#include "../replication/world_bridge.hpp"
#include "../replication/world_state.hpp"
#include "dusk/logging.h"
#include "dusk/multiplayer.hpp"
#include "fmt/format.h"
#include "network_manager.hpp"
#include "presence.hpp"
#include "save_guard.hpp"

namespace dusk::mp::world_sync {
namespace {

constexpr borealis::Log Log{"dusk::mp"};

/// WorldDigest cadence, in sim ticks: every two seconds at 30 Hz (spec II.5).
constexpr std::uint64_t kDigestIntervalTicks = 60;

/// Guest: how long to wait for a WorldSync before asking again. The request is reliable, so this
/// only matters if the host dropped it on the floor (it had no world at the time and then lost the
/// guest's place in line), and it keeps a lost request from leaving a guest unsynced for good.
constexpr std::uint64_t kSyncRequestRetryTicks = 300;

/// Host: the least time between two WorldSyncs to one guest. A guest whose every proposal is
/// refused (a guest bug that keeps re-emitting the same flood) would otherwise get a 3.5 KB sync
/// per tick. Proposals refused inside the window are simply dropped; the owed sync goes out when
/// the window ends and resets that guest anyway.
constexpr std::uint64_t kMinSyncSpacingTicks = 60;

/// Guest: an engine view that differs from the shadow on this many digests in a row, while the
/// shadow itself matches the host, is treated as a failed engine write and resynced. One is not
/// enough: a local change made after the last diff would look the same for one tick.
constexpr int kEngineDriftDigestsBeforeResync = 2;

/// Guest: the most full installs (reload in place) in kInstallWindowTicks before giving up. An
/// install whose reload itself knocks the world back to Unsynced would otherwise reload forever.
constexpr int kMaxInstallsPerWindow = 3;
constexpr std::uint64_t kInstallWindowTicks = 900;

/// Why a guest asked for a WorldSync. Logged on both ends; values are append-only.
enum class RequestReason : std::uint8_t {
    Unsynced = 0,     ///< joined, or had no world and has one now
    Digest = 1,       ///< the host's digest disagreed with ours at the same worldSeq
    SelfRefused = 2,  ///< our own diff was over the host's limits, so we never sent it
    /* --- batch 7: policy --- */
    PolicyChange = 3,  ///< the host made quest flags or inventory Shared; its copy wins
};

const char* reason_text(std::uint8_t reason) {
    switch (static_cast<RequestReason>(reason)) {
    case RequestReason::Unsynced:
        return "unsynced";
    case RequestReason::Digest:
        return "digest mismatch";
    case RequestReason::SelfRefused:
        return "own diff over the limits";
    case RequestReason::PolicyChange:
        return "policy switch";
    }
    return "unknown";
}

/**
 * ★ Test knob (spec III, "Host protection"): DUSK_MP_TEST_WORLD_FLOOD=1 on a GUEST sends, once,
 * 90 ticks after it first syncs, a proposal that clears 200 saved-event bits — the shape of the
 * dSv_info_c::init-while-synced bug II.8 is about. The guest does NOT apply it locally. The host
 * must refuse it, log an ERROR and answer with a WorldSync; its own bits must not change. Read
 * once; never set in normal play.
 */
bool test_world_flood() {
    static const bool enabled = [] {
        const char* value = std::getenv("DUSK_MP_TEST_WORLD_FLOOD");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

struct PendingProposal {
    std::uint32_t seq = 0;
    /// (counter key, delta) for every counter op in the proposal.
    std::vector<std::pair<std::uint16_t, std::int32_t>> deltas;
};

/**
 * Temporary bits (Dan, Zone; spec II.7). Not saved world state: never in the WorldView, the
 * shadow or the digest. See world_state.hpp "temporary domains" for the model; this is who shares
 * them with whom.
 *
 * DESIGN (choices the spec left open, all deliberate):
 *  - ROUTING: Dan/Zone ops ride the same Propose/Commit stream as saved ops, so they keep one
 *    order with everything else (a guest's switch and the chest it opens arrive together). The
 *    host applies them to its MIRRORS and, when co-located, to its engine; never to the shadow, so
 *    they cannot reach the digest, a WorldSync view, or the save (mDan/mZone are outside
 *    mSavedata anyway). judge_proposal counts them separately (kMaxProposalTempBits).
 *  - DIGEST: excludes them entirely. A mismatch can therefore never resync temp bits, and
 *    players in different stages can never trigger a resync through them. The test oracle for
 *    them is `worldlog` / `templog` on both ends.
 *  - OCCUPANCY: a table is occupied when this machine stands in it (its live table, or the last
 *    one while mid stage change), when a partner's Presence last named it (a partner who is
 *    loading still counts for the table he was last in), or when a partner's op for it arrived in
 *    the last kTempOpGraceTicks (an op can overtake his Presence). A mirror is kept exactly while
 *    its table is occupied, so "the bits reset only when everyone has left" holds.
 *  - H6 (after dComIfGs_initDan): a mirror exists AND a partner occupies the table -> mDan's bits
 *    := the mirror. Otherwise the stage entry is vanilla, and the next rebase announces this
 *    machine's dan as mirror-only ops, which overwrites any stale mirror elsewhere.
 *  - H7 (a zone was just created): a partner STANDS in that room -> seed the persistent zone
 *    bytes from the mirror (one-zone bits start clear: vanilla clears them on every room change,
 *    so every visit starts clear). Otherwise announce the fresh zone, mirror-only.
 *  - H8 (zoneCountCheck and the room-kill clear in d_s_room): flush the diff before, rebase the
 *    shadow after. Those clears are the LOCAL player's room lifecycle and are never sent.
 *  - LATE JOIN: the host's WorldSync carries all its mirrors after the view (write_temp_mirrors),
 *    so a joiner entering an occupied table is seeded by H6 like anyone else.
 */
struct TempState {
    world::TempMirrors mirrors;
    /// Per table: the tick a partner's op for it last arrived (see OCCUPANCY).
    std::unordered_map<std::uint8_t, std::uint64_t> lastRemoteOpTick;
    /// Per partner: the last stage table his Presence named (a loading partner reports -1; this
    /// keeps the last real one).
    std::unordered_map<std::uint32_t, std::int8_t> remoteTbl;
    /// The last local table capture_temp reported; stands in while mMemory belongs to no table.
    int lastLocalTbl = -1;

    world::TempView shadow;
    bool shadowValid = false;
    std::uint32_t shadowEpoch = 0;
    world::TempView scratch;

    /// H6 entered `danAnnounceTbl` without seeding: announce its dan at the next rebase.
    bool danAnnouncePending = false;
    int danAnnounceTbl = -1;
    /// Ops produced inside decomp hooks (H7 announcements, H8 flushes), sent with the next tick's
    /// diff and before it.
    std::vector<world::WorldOp> pending;
};

/// How long a partner's op for a table keeps its mirror alive without a Presence naming it.
constexpr std::uint64_t kTempOpGraceTicks = 150;
/// Hook-produced ops held for a tick at most; past this something is not draining them.
constexpr std::size_t kMaxTempPending = 4096;

struct GuestState {
    std::uint32_t lastProposeSeq = 0;
    bool owedSync = false;
    bool everSent = false;
    std::uint64_t lastSyncTick = 0;
};

struct State {
    bool synced = false;
    bool haveShadow = false;
    world::WorldView shadow;
    /// apply_op writes two views; the first one is scratch here, only the shadow is kept.
    world::WorldView scratch;
    world::WorldView cur;
    std::vector<world::WorldOp> ops;

    /// Packets that arrived while this end's world was away (a stage change), kept in order and
    /// handled once it is back: guest proposals on the host, host commits on a guest. Dropping
    /// them and resyncing instead lost the change for good: a chest the guest opened while the
    /// host walked through a door came back closed (and openable again), and the resync also
    /// overwrote anything this end's own game set during its load (stage-entry scenes set flags).
    struct Deferred {
        std::uint32_t from = 0;
        std::vector<std::uint8_t> bytes;
    };
    std::deque<Deferred> deferred;

    bool haveEpoch = false;
    std::uint32_t epoch = 0;

    // --- host
    std::uint32_t worldSeq = 0;
    bool hostEverSynced = false;
    std::unordered_map<std::uint32_t, GuestState> guests;
    /// II.8 d: saved-event clears applied from guests this session. A bad case shows up in the log.
    std::uint64_t guestEventClears = 0;
    std::uint32_t refusals = 0;

    // --- guest
    std::uint32_t lastApplied = 0;
    std::uint32_t proposeSeq = 0;
    std::deque<PendingProposal> pending;
    /// Sum of the unacked proposed deltas per counter: a commit writes hostAbs + this.
    std::unordered_map<std::uint16_t, std::int32_t> pendingDelta;
    bool installedThisEpoch = false;
    bool requestOutstanding = false;
    std::uint64_t requestTick = 0;
    std::uint64_t syncedAtTick = 0;
    bool floodSent = false;
    int engineDriftDigests = 0;
    std::deque<std::uint64_t> installTicks;
    bool installGaveUp = false;
    /* --- batch 7: policy --- */
    /// A switch made quest flags / inventory Shared: ask for the host's world at the next tick
    /// this end has one (guest_tick), and say "policy switch" rather than "RESYNC" when it lands.
    bool policySyncOwed = false;
    bool policySyncInFlight = false;

    // --- both: Dan/Zone temp bits
    TempState temp;
};

State& st() {
    static State instance;
    return instance;
}

std::uint16_t counter_key(const world::WorldOp& op) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(op.domain) << 8 | op.tbl);
}

bool tbl_ok(std::uint8_t tbl, std::size_t count) {
    return tbl < count;
}

/// The value @p op's target holds in @p v: the byte for a bit domain, the counter for a counter
/// domain. What world_bridge::apply_to_engine gets as its `value`.
std::int32_t view_value(const world::WorldView& v, const world::WorldOp& op) {
    using world::Domain;
    switch (op.domain) {
    case Domain::Event:
        return op.index < v.event.size() ? v.event[op.index] : 0;
    case Domain::MemBit:
        return tbl_ok(op.tbl, v.mem.size()) && op.index < world::kMemBitBytes ?
                   v.mem[op.tbl][op.index] :
                   0;
    case Domain::Mem2:
        return tbl_ok(op.tbl, v.mem2.size()) && op.index < world::kMem2Bytes ?
                   v.mem2[op.tbl][op.index] :
                   0;
    case Domain::FirstGet:
        return op.index < v.firstGet.size() ? v.firstGet[op.index] : 0;
    case Domain::Items:
        return op.index < v.player.size() ? v.player[op.index] : 0;
    case Domain::Rupee:
        return v.rupee;
    case Domain::KeyNum:
        return tbl_ok(op.tbl, v.mem.size()) ? v.key_num(op.tbl) : 0;
    case Domain::MaxLife:
        return v.maxLife;
    case Domain::Poh:
        return v.poh;
    default:
        return 0;
    }
}

/**
 * The digest both ends compare. Stage table 6's key count is forced to 0 or 2 by every warp on each
 * machine separately (world_state.hpp kLocalOnlyKeyTbl) and is never replicated, so it would make
 * two agreeing worlds disagree. Masked here, identically on both ends, whatever digest() does.
 */
std::uint32_t masked_digest(const world::WorldView& v) {
    world::WorldView copy = v;
    copy.mem[world::kLocalOnlyKeyTbl][world::kMemKeyNumOffset] = 0;
    return world::digest(copy);
}

std::string describe_ops(const std::vector<world::WorldOp>& ops, std::size_t max = 6) {
    std::string out;
    const std::size_t n = std::min(ops.size(), max);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& op = ops[i];
        if (world::is_counter(op.domain)) {
            fmt::format_to(std::back_inserter(out), " d{}/t{}:{:+}={}", static_cast<int>(op.domain),
                static_cast<int>(op.tbl), op.delta, op.resultAbs);
        } else {
            fmt::format_to(std::back_inserter(out), " d{}/t{}/i{}:+{:02X}-{:02X}",
                static_cast<int>(op.domain), static_cast<int>(op.tbl), op.index, op.set, op.clr);
        }
    }
    if (ops.size() > n) {
        fmt::format_to(std::back_inserter(out), " (+{} more)", ops.size() - n);
    }
    return out;
}

std::uint64_t now_tick() {
    return network_manager().sim_tick();
}

bool is_host() {
    return network_manager().role() == Role::Host;
}

// --- temp bits (Dan, Zone) -----------------------------------------------------------------------

/// Hooks run with multiplayer inactive too; this keeps them no-ops then. The host keeps sharing
/// temp bits through its own stage changes (it is Unsynced while its world is away but keeps the
/// shadow); a guest only while Synced.
bool temp_active() {
    const NetworkManager& net = network_manager();
    if (!net.is_active() || !net.session_live()) {
        return false;
    }
    const State& s = st();
    if (net.role() == Role::Host) {
        return s.haveShadow;
    }
    return net.role() == Role::Client && s.synced;
}

int local_tbl() {
    const int live = world_bridge::live_tbl();
    return live >= 0 ? live : st().temp.lastLocalTbl;
}

/// Presence -> remoteTbl: the last real stage table each partner named.
void refresh_remote_tbls() {
    TempState& t = st().temp;
    const std::vector<presence::Info> peers = presence::peers();
    for (auto it = t.remoteTbl.begin(); it != t.remoteTbl.end();) {
        const bool present = std::any_of(peers.begin(), peers.end(),
            [&](const presence::Info& p) { return p.valid && p.playerId == it->first; });
        it = present ? std::next(it) : t.remoteTbl.erase(it);
    }
    for (const presence::Info& p : peers) {
        if (p.valid && p.saveTbl >= 0 && p.saveTbl < static_cast<int>(world::kMemTbls)) {
            t.remoteTbl[p.playerId] = p.saveTbl;
        }
    }
}

bool remote_occupies(int tbl) {
    const TempState& t = st().temp;
    for (const auto& entry : t.remoteTbl) {
        if (entry.second == tbl) {
            return true;
        }
    }
    const auto it = t.lastRemoteOpTick.find(static_cast<std::uint8_t>(tbl));
    return it != t.lastRemoteOpTick.end() && now_tick() - it->second < kTempOpGraceTicks;
}

/// A partner whose Link stands in `room` of `tbl` right now (not loading). His zone for that room
/// is live, so the mirror's bytes for it are current.
bool remote_in_room(int tbl, int room) {
    for (const presence::Info& p : presence::peers()) {
        if (p.valid && (p.flags & presence::kFlagLoading) == 0 && p.saveTbl == tbl &&
            p.room == room)
        {
            return true;
        }
    }
    return false;
}

/// Drop every mirror whose table nobody occupies any more (vanilla resets temps when you leave;
/// co-op does when EVERYONE has).
void drop_unoccupied_mirrors() {
    TempState& t = st().temp;
    const int local = local_tbl();
    for (auto it = t.mirrors.begin(); it != t.mirrors.end();) {
        const int tbl = it->first;
        if (tbl == local || remote_occupies(tbl)) {
            ++it;
            continue;
        }
        Log.info("[mp-world] temp: nobody is in stage table {} any more; its dan/zone mirror is "
                 "dropped (the next player in starts with vanilla-reset bits)",
            tbl);
        it = t.mirrors.erase(it);
    }
}

/// This machine's own table: its engine is the truth for the dan bits (H6 seeded it from this very
/// mirror, or nobody else is in the table), and for any room the mirror has never heard of.
/// Rooms the mirror already has are left alone: their one-zone bytes may have been cleared here by
/// this player's own room lifecycle (H8), which is not news for anyone else.
void adopt_local_into_mirror(const world::TempView& cur) {
    if (cur.tbl < 0) {
        return;
    }
    world::TempMirror& m = st().temp.mirrors[static_cast<std::uint8_t>(cur.tbl)];
    m.dan = cur.dan;
    const std::uint64_t unknown = cur.rooms & ~m.rooms;
    for (std::size_t room = 0; room < world::kZoneRooms; ++room) {
        if ((unknown >> room & 1) != 0) {
            m.zone[room] = cur.zone[room];
        }
    }
    m.rooms |= cur.rooms;
}

/// Before a WorldSync goes out: make sure it carries the sender's own table as it is right now
/// (the first sync can leave before the first temp capture of the session).
void write_sync_trailer(Writer& w) {
    TempState& t = st().temp;
    if (world_bridge::capture_temp(t.scratch)) {
        adopt_local_into_mirror(t.scratch);
    }
    world::write_temp_mirrors(w, t.mirrors);
}

/// An incoming temp op (a commit on a guest, an accepted proposal on the host): the mirror always;
/// the engine (and the temp shadow, so it is not sent back) when this machine holds that table
/// and, for Zone, that room. Mirror-only ops stop at the mirror.
void temp_receive(const world::WorldOp& op, std::uint32_t origin) {
    TempState& t = st().temp;
    world::apply_temp_op(t.mirrors[op.tbl], op);
    if (origin != network_manager().local_player_id()) {
        t.lastRemoteOpTick[op.tbl] = now_tick();
    }
    if (world::temp_mirror_only(op.index)) {
        return;
    }
    if (world_bridge::apply_temp_to_engine(op) && t.shadowValid) {
        world::apply_temp_op(t.shadow, op);
    }
}

/// This tick's local temp changes, appended to `out` after anything the decomp hooks queued.
/// Rebases (no ops) whenever the temp bits changed owner wholesale: a stage change, a reset, the
/// first capture, or a capture that failed meanwhile.
void temp_collect(std::vector<world::WorldOp>& out) {
    State& s = st();
    TempState& t = s.temp;
    refresh_remote_tbls();

    std::vector<world::WorldOp> diffs;
    world::TempView& cur = t.scratch;
    if (!world_bridge::capture_temp(cur)) {
        t.shadowValid = false;
    } else {
        const auto tbl = static_cast<std::uint8_t>(cur.tbl);
        const std::uint32_t epoch = world_bridge::temp_epoch();
        t.lastLocalTbl = cur.tbl;
        world::TempMirror& mirror = t.mirrors[tbl];
        if (!t.shadowValid || t.shadowEpoch != epoch || t.shadow.tbl != cur.tbl) {
            // Lifecycle, never ops. This machine's dan for its own table is the truth here: either
            // H6 just seeded it from this very mirror, or nobody else is in the table.
            t.shadow = cur;
            t.shadowValid = true;
            t.shadowEpoch = epoch;
            adopt_local_into_mirror(cur);
            const bool announce = t.danAnnouncePending && t.danAnnounceTbl == cur.tbl;
            if (announce) {
                world::announce_dan(tbl, cur.dan, t.pending);
            }
            t.danAnnouncePending = false;
            Log.info("[mp-world] temp: tracking stage table {} (epoch {}, {} zone(s) here){}", tbl,
                epoch, std::popcount(cur.rooms),
                announce ? "; announcing this machine's fresh dan to everyone's mirror" : "");
        } else {
            world::diff_temp(t.shadow, cur, diffs);
            for (const world::WorldOp& op : diffs) {
                world::apply_temp_op(mirror, op);
            }
            t.shadow = cur;
        }
    }

    drop_unoccupied_mirrors();

    out.insert(out.end(), t.pending.begin(), t.pending.end());
    t.pending.clear();
    out.insert(out.end(), diffs.begin(), diffs.end());
}

void temp_queue(std::vector<world::WorldOp>&& ops) {
    TempState& t = st().temp;
    if (t.pending.size() + ops.size() > kMaxTempPending) {
        Log.error("[mp-world] temp: {} hook ops waiting to be sent; dropping {} more (nothing is "
                  "draining them)",
            t.pending.size(), ops.size());
        return;
    }
    t.pending.insert(t.pending.end(), ops.begin(), ops.end());
}

/// Guest: the host's mirrors arrived with a WorldSync. The host's merged copy wins, except for
/// this end's own table when the host has none for it yet (our Presence has not reached it).
/// `reloading`: an install, whose reload in place runs H6 against these mirrors. Otherwise (a
/// resync) the shared dan goes straight into the engine when a partner is here, and the shadow
/// is rebased so the overwrite is not sent back.
void install_temp_mirrors(world::TempMirrors&& mirrors, bool reloading) {
    TempState& t = st().temp;
    refresh_remote_tbls();
    const int local = local_tbl();
    if (local >= 0 && mirrors.count(static_cast<std::uint8_t>(local)) == 0) {
        const auto own = t.mirrors.find(static_cast<std::uint8_t>(local));
        if (own != t.mirrors.end()) {
            mirrors[own->first] = own->second;
        }
    }
    t.mirrors = std::move(mirrors);
    Log.info("[mp-world] guest: {} temp mirror(s) from the host ({})", t.mirrors.size(),
        reloading ? "install" : "resync");
    if (reloading || local < 0) {
        return;
    }
    const auto it = t.mirrors.find(static_cast<std::uint8_t>(local));
    if (it != t.mirrors.end() && remote_occupies(local) && world_bridge::capture_temp(t.scratch) &&
        t.scratch.tbl == local)
    {
        world_bridge::seed_dan(it->second.dan);
    }
    t.shadowValid = false;
}

/// Beyond this many held packets something is stuck; fall back to a resync.
constexpr std::size_t kMaxDeferred = 512;

/// Keeps the rest of `r` to be handled once this end's world is back. False when the queue is full.
bool defer_packet(Reader& r, std::uint32_t from) {
    State& s = st();
    if (s.deferred.size() >= kMaxDeferred) {
        Log.error("[mp-world] {}: {} packets held while the world is away; giving up on holding",
            is_host() ? "host" : "guest", s.deferred.size());
        return false;
    }
    State::Deferred d;
    d.from = from;
    d.bytes.assign(r.cursor(), r.cursor() + r.remaining());
    s.deferred.push_back(std::move(d));
    if (s.deferred.size() == 1) {
        Log.info("[mp-world] {}: world away (stage change); holding packets until it is back",
            is_host() ? "host" : "guest");
    }
    return true;
}

void drain_deferred();

void go_unsynced(const char* why) {
    State& s = st();
    if (s.synced) {
        Log.info("[mp-world] {}: unsynced ({})", is_host() ? "host" : "guest", why);
    }
    s.synced = false;
    s.engineDriftDigests = 0;
}

// --- host
// ------------------------------------------------------------------------------------------

void host_send_sync(std::uint32_t playerId, const char* why) {
    State& s = st();
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::WorldSync));
    w.write_u32(s.worldSeq);
    world::write_view(w, s.shadow);
    write_sync_trailer(w);
    network_manager().send_to_player(playerId, w, kChannelControl, true);
    GuestState& g = s.guests[playerId];
    g.owedSync = false;
    g.everSent = true;
    g.lastSyncTick = now_tick();
    Log.info("[mp-world] host: WorldSync seq={} digest=0x{:08X} -> player {} ({}, {} bytes)",
        s.worldSeq, masked_digest(s.shadow), playerId, why, w.size());
}

/// Every guest that has asked for (or been refused into) a WorldSync and is past the spacing.
void host_send_owed_syncs() {
    State& s = st();
    const std::uint64_t tick = now_tick();
    for (auto& [playerId, g] : s.guests) {
        if (!g.owedSync) {
            continue;
        }
        if (g.everSent && tick - g.lastSyncTick < kMinSyncSpacingTicks) {
            continue;
        }
        host_send_sync(playerId, "owed");
    }
}

/// Broadcast @p ops as WorldCommits, split so no message exceeds kMaxOpsPerMessage. An empty list
/// still sends one commit: it is how a guest learns its proposal was handled.
void host_commit(std::uint32_t originPlayer, std::uint32_t ackProposeSeq,
    const std::vector<world::WorldOp>& ops) {
    State& s = st();
    std::size_t i = 0;
    do {
        const std::size_t n = std::min(world::kMaxOpsPerMessage, ops.size() - i);
        const std::vector<world::WorldOp> chunk(ops.begin() + static_cast<std::ptrdiff_t>(i),
            ops.begin() + static_cast<std::ptrdiff_t>(i + n));
        ++s.worldSeq;
        Writer w;
        w.write_u8(static_cast<std::uint8_t>(PacketId::WorldCommit));
        w.write_u32(s.worldSeq);
        w.write_u32(originPlayer);
        w.write_u32(ackProposeSeq);
        world::write_ops(w, chunk);
        network_manager().send_to_session(w, kChannelControl, true);
        Log.info("[mp-world] host: commit seq={} origin={} ack={} ops={}{}", s.worldSeq,
            originPlayer, ackProposeSeq, chunk.size(), describe_ops(chunk));
        i += n;
    } while (i < ops.size());
}

void host_tick(bool ready) {
    State& s = st();
    if (!ready) {
        go_unsynced("no world");
        return;
    }

    if (!s.synced) {
        // Same reset epoch and a shadow already: keep it, so anything that changed while the world
        // was away still diffs out as commits. A new epoch (another file loaded) is a different
        // world, and every guest is told so with a WorldSync.
        const bool fresh = !s.haveShadow;
        if (fresh) {
            world_bridge::capture(s.shadow);
            s.haveShadow = true;
        }
        s.synced = true;
        Log.info("[mp-world] host: synced at seq={} digest=0x{:08X} ({}, liveTbl={})", s.worldSeq,
            masked_digest(s.shadow), fresh ? "fresh capture" : "kept shadow",
            world_bridge::live_tbl());
        if (fresh && s.hostEverSynced) {
            Log.warn("[mp-world] host: this machine's world was replaced; resending it to every "
                     "guest");
            Writer w;
            w.write_u8(static_cast<std::uint8_t>(PacketId::WorldSync));
            w.write_u32(s.worldSeq);
            world::write_view(w, s.shadow);
            write_sync_trailer(w);
            network_manager().send_to_session(w, kChannelControl, true);
            for (auto& entry : s.guests) {
                GuestState& g = entry.second;
                g.owedSync = false;
                g.everSent = true;
                g.lastSyncTick = now_tick();
            }
        }
        s.hostEverSynced = true;
    }

    drain_deferred();
    host_send_owed_syncs();

    world_bridge::capture(s.cur);
    s.ops.clear();
    world::diff(s.shadow, s.cur, s.ops);
    // Dan/Zone ride along; they never touch the shadow (temp_collect keeps its own).
    temp_collect(s.ops);
    if (!s.ops.empty()) {
        // diff(shadow, cur) applied to the shadow IS cur.
        s.shadow = s.cur;
        host_commit(kHostPlayerId, 0, s.ops);
    }

    if (now_tick() % kDigestIntervalTicks == 0) {
        Writer w;
        w.write_u8(static_cast<std::uint8_t>(PacketId::WorldDigest));
        w.write_u32(s.worldSeq);
        w.write_u32(masked_digest(s.shadow));
        network_manager().send_to_session(w, kChannelControl, true);
    }
}

void host_on_propose(Reader& r, std::uint32_t fromPlayerId) {
    State& s = st();
    std::uint32_t proposeSeq = 0;
    std::vector<world::WorldOp> ops;
    GuestState& g = s.guests[fromPlayerId];
    if (!r.read_u32(proposeSeq) || !world::read_ops(r, ops) || !r.ok()) {
        Log.error("[mp-world] host: malformed WorldPropose from player {}; answering with a "
                  "WorldSync",
            fromPlayerId);
        g.owedSync = true;
        return;
    }
    if (proposeSeq <= g.lastProposeSeq) {
        Log.warn("[mp-world] host: proposal #{} from player {} is not newer than #{}; dropped",
            proposeSeq, fromPlayerId, g.lastProposeSeq);
        return;
    }
    g.lastProposeSeq = proposeSeq;

    if (!s.synced || !world_bridge::world_ready()) {
        // Nowhere to apply it. The guest's pending prediction is cleared by the sync it is owed,
        // which goes out as soon as this end has a world again.
        Log.warn(
            "[mp-world] host: proposal #{} from player {} arrived with no host world; dropped, "
            "guest will be resynced",
            proposeSeq, fromPlayerId);
        g.owedSync = true;
        return;
    }

    const world::ProposalVerdict verdict = world::judge_proposal(ops);
    if (!verdict.accept) {
        ++s.refusals;
        Log.error("[mp-world] host: REFUSED proposal #{} from player {}: {} ops, {} bits touched "
                  "(max {}), {} saved-event clears (max {}), {} bad ops. Not applied; sending the "
                  "guest a WorldSync. Refusals this session: {}",
            proposeSeq, fromPlayerId, ops.size(), verdict.bitsTouched, world::kMaxProposalBits,
            verdict.eventClears, world::kMaxProposalEventClears, verdict.badOps, s.refusals);
        g.owedSync = true;
        host_send_owed_syncs();
        return;
    }

    std::vector<world::WorldOp> committed;
    committed.reserve(ops.size());
    /* --- batch 7: policy --- */
    std::size_t individualOps = 0;
    for (world::WorldOp op : ops) {
        if (!world::is_temp(op.domain) && !world_bridge::domain_shared(op.domain)) {
            // Proposed before this guest heard of a switch that made the domain Individual (a
            // pickup crossing "wallet: per player" in flight). It is the guest's own now: its
            // engine already has it, and it must not reach the shared shadow or this wallet.
            ++individualOps;
            continue;
        }
        if (op.domain == world::Domain::KeyNum && op.tbl == world::kLocalOnlyKeyTbl) {
            // Never replicated (world_state.hpp); an honest guest's diff never emits it.
            Log.warn(
                "[mp-world] host: dropped a key-count op for local-only table {} from player {}",
                op.tbl, fromPlayerId);
            continue;
        }
        if (world::is_temp(op.domain)) {
            // Not saved state: mirrors (+ this engine when co-located), never the shadow.
            temp_receive(op, fromPlayerId);
            committed.push_back(op);
            continue;
        }
        // Counters as deltas onto the host's own value: absolutes would lose one of two pickups
        // that cross in flight (world_state.hpp).
        // The shadow first: apply_op adds a counter delta onto its FIRST view, and the scratch
        // view is stale (it held 0 rupees, so a guest's +30 set the shared wallet to 30).
        if (!world::apply_op(s.shadow, s.scratch, op, false)) {
            Log.warn("[mp-world] host: op{} from player {} did not apply; dropped",
                describe_ops({op}), fromPlayerId);
            continue;
        }
        const std::int32_t value = view_value(s.shadow, op);
        if (world::is_counter(op.domain)) {
            op.resultAbs = value;
        }
        world_bridge::apply_to_engine(op, value);
        committed.push_back(op);
    }

    if (individualOps != 0) {
        Log.info(
            "[mp-policy] host: dropped {} op(s) of proposal #{} from player {} for domains the "
            "policy makes Individual (crossed a policy switch in flight)",
            individualOps, proposeSeq, fromPlayerId);
    }

    if (verdict.eventClears > 0) {
        s.guestEventClears += verdict.eventClears;
        Log.warn("[mp-world] host: applied {} saved-event bit clear(s) from player {} (proposal "
                 "#{}); session total {}",
            verdict.eventClears, fromPlayerId, proposeSeq, s.guestEventClears);
    }

    host_commit(fromPlayerId, proposeSeq, committed);
}

// --- guest
// -----------------------------------------------------------------------------------------

void guest_send_request(RequestReason reason) {
    State& s = st();
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::WorldSyncRequest));
    w.write_u32(s.lastApplied);
    w.write_u8(static_cast<std::uint8_t>(reason));
    network_manager().send_to_player(kHostPlayerId, w, kChannelControl, true);
    s.requestOutstanding = true;
    s.requestTick = now_tick();
    Log.info("[mp-world] guest: WorldSyncRequest ({}, last applied seq={})",
        reason_text(static_cast<std::uint8_t>(reason)), s.lastApplied);
}

void guest_ack(std::uint32_t ackProposeSeq) {
    State& s = st();
    while (!s.pending.empty() && s.pending.front().seq <= ackProposeSeq) {
        for (const auto& [key, delta] : s.pending.front().deltas) {
            const auto it = s.pendingDelta.find(key);
            if (it == s.pendingDelta.end()) {
                continue;
            }
            it->second -= delta;
            if (it->second == 0) {
                s.pendingDelta.erase(it);
            }
        }
        s.pending.pop_front();
    }
}

void guest_clear_pending() {
    State& s = st();
    s.pending.clear();
    s.pendingDelta.clear();
}

void guest_send_flood() {
    State& s = st();
    std::vector<world::WorldOp> ops;
    for (std::uint16_t i = 0; i < 25; ++i) {
        world::WorldOp op;
        op.domain = world::Domain::Event;
        op.tbl = world::kNoTbl;
        op.index = i;
        op.clr = 0xFF;
        ops.push_back(op);
    }
    const std::uint32_t seq = ++s.proposeSeq;
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::WorldPropose));
    w.write_u32(seq);
    world::write_ops(w, ops);
    network_manager().send_to_player(kHostPlayerId, w, kChannelControl, true);
    // Pending, so the digest check stands down until the host's answer (a WorldSync) clears it.
    s.pending.push_back({seq, {}});
    s.floodSent = true;
    Log.warn("[mp-world] TEST FLOOD (DUSK_MP_TEST_WORLD_FLOOD): sent proposal #{} clearing 200 "
             "saved-event bits; NOT applied locally. The host must refuse it.",
        seq);
}

void guest_tick(bool ready) {
    State& s = st();
    if (!ready) {
        // A stage change, not a new world (that bumps the reset epoch, handled in tick()): stay
        // synced, stop diffing, and hold the host's commits until the world is back (deferred).
        return;
    }

    if (!s.synced) {
        // Hook ops from before we fell out of sync describe a world the coming WorldSync replaces.
        s.temp.pending.clear();
        if (s.installGaveUp) {
            return;
        }
        if (!s.requestOutstanding || now_tick() - s.requestTick >= kSyncRequestRetryTicks) {
            guest_send_request(RequestReason::Unsynced);
        }
        return;
    }

    drain_deferred();

    /* --- batch 7: policy --- */
    if (s.policySyncOwed && !s.requestOutstanding) {
        s.policySyncOwed = false;
        s.policySyncInFlight = true;
        guest_send_request(RequestReason::PolicyChange);
    }

    if (test_world_flood() && !s.floodSent && now_tick() - s.syncedAtTick >= 90) {
        guest_send_flood();
    }

    world_bridge::capture(s.cur);
    s.ops.clear();
    world::diff(s.shadow, s.cur, s.ops);
    temp_collect(s.ops);
    if (s.ops.empty()) {
        return;
    }

    // The host would refuse this anyway; don't send what would be refused. Something on this
    // machine changed far more of the world than gameplay does in one tick (II.8): ask for the
    // host's world instead, which puts back whatever just happened here.
    const world::ProposalVerdict verdict = world::judge_proposal(s.ops);
    if (!verdict.accept) {
        Log.error("[mp-world] guest: own diff of {} ops ({} bits, {} saved-event clears, {} bad "
                  "ops) is over the host's limits; NOT proposing it, resyncing instead.{}",
            s.ops.size(), verdict.bitsTouched, verdict.eventClears, verdict.badOps,
            describe_ops(s.ops));
        go_unsynced("own diff over the limits");
        guest_send_request(RequestReason::SelfRefused);
        return;
    }

    const std::uint32_t seq = ++s.proposeSeq;
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::WorldPropose));
    w.write_u32(seq);
    world::write_ops(w, s.ops);
    network_manager().send_to_player(kHostPlayerId, w, kChannelControl, true);

    PendingProposal proposal;
    proposal.seq = seq;
    for (const auto& op : s.ops) {
        if (world::is_counter(op.domain) && op.delta != 0) {
            proposal.deltas.emplace_back(counter_key(op), op.delta);
            s.pendingDelta[counter_key(op)] += op.delta;
        }
    }
    s.pending.push_back(std::move(proposal));
    s.shadow = s.cur;
    Log.info("[mp-world] guest: propose #{} ops={}{}", seq, s.ops.size(), describe_ops(s.ops));
}

void guest_on_sync(Reader& r) {
    State& s = st();
    std::uint32_t seq = 0;
    world::WorldView view;
    world::TempMirrors mirrors;
    if (!r.read_u32(seq) || !world::read_view(r, view) ||
        !world::read_temp_mirrors(r, mirrors) || !r.ok())
    {
        Log.error("[mp-world] guest: malformed WorldSync; asking again");
        s.requestOutstanding = false;
        return;
    }

    // ★ Defence in depth for the one rule that must never slip: the host's world only ever goes
    // into a game whose save is latched off. The bridge checks it as well.
    if (!save_would_be_contaminated()) {
        Log.error("[mp-world] guest: REFUSING WorldSync seq={}: the save latch is not engaged, so "
                  "the host's world could end up in this machine's own save",
            seq);
        return;
    }
    if (!world_bridge::world_ready()) {
        Log.info("[mp-world] guest: WorldSync seq={} arrived with no world here; will ask again "
                 "when there is one",
            seq);
        s.requestOutstanding = false;
        return;
    }

    if (!s.installedThisEpoch) {
        const std::uint64_t tick = now_tick();
        while (!s.installTicks.empty() && tick - s.installTicks.front() > kInstallWindowTicks) {
            s.installTicks.pop_front();
        }
        if (static_cast<int>(s.installTicks.size()) >= kMaxInstallsPerWindow) {
            Log.error("[mp-world] guest: {} installs in {} ticks; the install keeps knocking this "
                      "end back to unsynced. Giving up on the shared world for this session.",
                s.installTicks.size(), kInstallWindowTicks);
            s.installGaveUp = true;
            return;
        }
        if (!world_bridge::install_host_world(view)) {
            Log.error("[mp-world] guest: install_host_world refused WorldSync seq={}; staying "
                      "unsynced",
                seq);
            s.requestOutstanding = false;
            return;
        }
        s.installTicks.push_back(tick);
        s.installedThisEpoch = true;
        Log.info("[mp-world] guest: installed host world seq={} digest=0x{:08X} (reload in place)",
            seq, masked_digest(view));
        install_temp_mirrors(std::move(mirrors), true);
    } else {
        world_bridge::overwrite_world(view);
        if (s.policySyncInFlight) {
            // Asked for on purpose (batch 7): a group just became Shared and the host's copy wins.
            Log.info("[mp-policy] guest: host world installed for the policy switch at seq={} "
                     "digest=0x{:08X}",
                seq, masked_digest(view));
        } else {
            Log.warn("[mp-world] guest: RESYNC — overwrote the world with the host's at seq={} "
                     "digest=0x{:08X} (was seq={} digest=0x{:08X}). A resync means a bug "
                     "somewhere.",
                seq, masked_digest(view), s.lastApplied,
                s.haveShadow ? masked_digest(s.shadow) : 0u);
        }
        install_temp_mirrors(std::move(mirrors), false);
    }
    s.policySyncInFlight = false;
    s.policySyncOwed = false;

    s.shadow = view;
    s.haveShadow = true;
    s.lastApplied = seq;
    guest_clear_pending();
    s.requestOutstanding = false;
    s.engineDriftDigests = 0;
    if (!s.synced) {
        s.syncedAtTick = now_tick();
    }
    s.synced = true;
}

void guest_on_commit(Reader& r) {
    State& s = st();
    std::uint32_t seq = 0;
    std::uint32_t origin = 0;
    std::uint32_t ack = 0;
    std::vector<world::WorldOp> ops;
    if (!r.read_u32(seq) || !r.read_u32(origin) || !r.read_u32(ack) || !world::read_ops(r, ops) ||
        !r.ok())
    {
        Log.error("[mp-world] guest: malformed WorldCommit; resyncing");
        go_unsynced("malformed commit");
        return;
    }
    if (!s.synced) {
        // The WorldSync this end is waiting for already contains it.
        return;
    }
    if (!world_bridge::world_ready()) {
        go_unsynced("no world for a commit");
        return;
    }
    if (seq <= s.lastApplied) {
        return;
    }
    if (seq != s.lastApplied + 1) {
        Log.warn("[mp-world] guest: commit seq={} after {}; a gap on an ordered channel", seq,
            s.lastApplied);
    }

    const std::uint32_t me = network_manager().local_player_id();
    if (origin == me) {
        guest_ack(ack);
    }

    std::uint32_t bad = 0;
    std::uint32_t individual = 0;
    for (world::WorldOp op : ops) {
        if (world::is_temp(op.domain)) {
            if (!world::op_in_range(op)) {
                ++bad;
                continue;
            }
            // Re-applied for our own ops too, for the same reason as the bits below: the host's
            // order is the one every end must end up with.
            temp_receive(op, origin);
            continue;
        }
        /* --- batch 7: policy --- */
        if (!world_bridge::domain_shared(op.domain)) {
            // The host never commits one under the policy it sent us; this is one that was
            // committed before a switch and held here (deferred) past the PolicySync. Keeping it
            // out of the shadow is what the host's own rebase did to its copy.
            ++individual;
            continue;
        }
        if (world::is_counter(op.domain)) {
            // The host's value plus whatever this end has proposed and not yet heard back about, so
            // a pickup never visibly bounces (II.4).
            const auto it = s.pendingDelta.find(counter_key(op));
            if (it != s.pendingDelta.end()) {
                op.resultAbs += it->second;
            }
        }
        // Bit ops from this end's own proposals are no-ops here; applied anyway, because after a
        // resync they are what puts this end's accepted changes back.
        if (!world::apply_op(s.scratch, s.shadow, op, true)) {
            ++bad;
            continue;
        }
        world_bridge::apply_to_engine(op, view_value(s.shadow, op));
    }
    s.lastApplied = seq;
    Log.info("[mp-world] guest: applied commit seq={} origin={}{} ops={}{}", seq, origin,
        origin == me ? fmt::format(" (own, ack #{})", ack) : std::string{}, ops.size(),
        describe_ops(ops));
    if (bad != 0) {
        Log.error("[mp-world] guest: {} op(s) in commit seq={} did not apply", bad, seq);
    }
    if (individual != 0) {
        Log.info("[mp-policy] guest: skipped {} op(s) in commit seq={} for domains the policy now "
                 "makes Individual",
            individual, seq);
    }
}

void guest_on_digest(Reader& r) {
    State& s = st();
    std::uint32_t seq = 0;
    std::uint32_t hostDigest = 0;
    if (!r.read_u32(seq) || !r.read_u32(hostDigest) || !r.ok()) {
        return;
    }
    // Only a settled view is comparable: synced, everything we proposed answered, and at exactly
    // the host's seq.
    if (!s.synced || !s.pending.empty() || seq != s.lastApplied || !world_bridge::world_ready()) {
        return;
    }

    const std::uint32_t shadowDigest = masked_digest(s.shadow);
    world_bridge::capture(s.cur);
    const std::uint32_t engineDigest = masked_digest(s.cur);

    if (shadowDigest != hostDigest) {
        Log.error("[mp-world] guest: DIGEST MISMATCH at seq={}: host 0x{:08X}, ours 0x{:08X} "
                  "(engine 0x{:08X}); requesting a resync",
            seq, hostDigest, shadowDigest, engineDigest);
        if (!s.requestOutstanding) {
            guest_send_request(RequestReason::Digest);
        }
        return;
    }
    if (engineDigest != shadowDigest) {
        // Either a local change since the last diff (proposed next tick, harmless) or an engine
        // write that did not land. Only the second one persists.
        ++s.engineDriftDigests;
        Log.warn("[mp-world] guest: digest matches the host at seq={} (0x{:08X}) but the engine "
                 "reads 0x{:08X} ({} in a row)",
            seq, hostDigest, engineDigest, s.engineDriftDigests);
        if (s.engineDriftDigests >= kEngineDriftDigestsBeforeResync && !s.requestOutstanding) {
            guest_send_request(RequestReason::Digest);
        }
        return;
    }
    s.engineDriftDigests = 0;
    Log.info("[mp-world] digest match seq={} 0x{:08X}", seq, hostDigest);
}

// --- logging -------------------------------------------------------------------------------------

void append_bits(std::string& out, const std::uint8_t* bytes, std::size_t count,
    std::size_t skipByte = static_cast<std::size_t>(-1)) {
    for (std::size_t i = 0; i < count; ++i) {
        if (i == skipByte) {
            continue;
        }
        for (int bit = 0; bit < 8; ++bit) {
            if ((bytes[i] & (1u << bit)) != 0) {
                fmt::format_to(
                    std::back_inserter(out), " {}", i * 8 + static_cast<std::size_t>(bit));
            }
        }
    }
}

bool any_bits(const std::uint8_t* bytes, std::size_t count, std::size_t skipByte) {
    for (std::size_t i = 0; i < count; ++i) {
        if (i != skipByte && bytes[i] != 0) {
            return true;
        }
    }
    return false;
}

void drain_deferred() {
    State& s = st();
    if (s.deferred.empty()) {
        return;
    }
    std::deque<State::Deferred> queue;
    queue.swap(s.deferred);
    Log.info("[mp-world] {}: world back; handling {} held packet(s)", is_host() ? "host" : "guest",
        queue.size());
    for (const State::Deferred& d : queue) {
        Reader rr(d.bytes.data(), d.bytes.size());
        if (is_host()) {
            host_on_propose(rr, d.from);
        } else {
            guest_on_commit(rr);
        }
    }
}

}  // namespace

void tick() {
    NetworkManager& net = network_manager();
    if (!net.is_active() || !net.session_live()) {
        return;
    }
    State& s = st();

    const bool ready = world_bridge::world_ready();
    const std::uint32_t epoch = world_bridge::reset_epoch();
    if (s.haveEpoch && epoch != s.epoch) {
        Log.warn("[mp-world] {}: reset epoch {} -> {}; this machine's world was replaced",
            is_host() ? "host" : "guest", s.epoch, epoch);
        go_unsynced("reset epoch changed");
        s.installedThisEpoch = false;
        // Held packets belong to the old world. The host resends its new world to everyone; a
        // guest asks for it.
        s.deferred.clear();
        // So do the temp mirrors: the host's WorldSync brings the session's back.
        s.temp = TempState{};
        if (is_host()) {
            // A different world: recapture and resend it rather than diff old against new.
            s.haveShadow = false;
        } else {
            guest_clear_pending();
            s.requestOutstanding = false;
        }
    }
    s.epoch = epoch;
    s.haveEpoch = true;

    if (net.role() == Role::Host) {
        host_tick(ready);
    } else if (net.role() == Role::Client) {
        guest_tick(ready);
    }
}

void on_packet(PacketId id, Reader& r, std::uint32_t fromPlayerId) {
    const Role role = network_manager().role();
    const bool hostBound = id == PacketId::WorldPropose || id == PacketId::WorldSyncRequest;
    if ((hostBound && role != Role::Host) || (!hostBound && role != Role::Client)) {
        // ★ A host never takes a WorldSync: that would put someone else's world into the one save
        // this session writes. NetworkManager already polices direction; this is the second lock.
        Log.error("[mp-world] ignoring packet {} from player {}: wrong direction for this role",
            static_cast<int>(id), fromPlayerId);
        return;
    }

    switch (id) {
    case PacketId::WorldPropose: {
        // The host's world is away (a stage change) but it is the same world: hold the proposal
        // rather than drop it. With no shadow at all (never had a world, or a new one) the old
        // path stands: dropped, and the guest gets a WorldSync.
        State& s = st();
        const bool away = !s.synced || !world_bridge::world_ready() || !s.deferred.empty();
        if (away && s.haveShadow && defer_packet(r, fromPlayerId)) {
            break;
        }
        host_on_propose(r, fromPlayerId);
        break;
    }
    case PacketId::WorldSyncRequest: {
        std::uint32_t lastApplied = 0;
        std::uint8_t reason = 0;
        // Lenient: the reason is only for the log.
        r.read_u32(lastApplied);
        r.read_u8(reason);
        GuestState& g = st().guests[fromPlayerId];
        g.owedSync = true;
        Log.info("[mp-world] host: player {} asks for a WorldSync ({}, last applied seq={}){}",
            fromPlayerId, reason_text(reason), lastApplied,
            st().synced ? "" : "; queued until this end has a world");
        if (st().synced) {
            host_send_owed_syncs();
        }
        break;
    }
    case PacketId::WorldSync:
        guest_on_sync(r);
        break;
    case PacketId::WorldCommit: {
        State& s = st();
        if (s.synced && (!world_bridge::world_ready() || !s.deferred.empty())) {
            if (!defer_packet(r, fromPlayerId)) {
                s.deferred.clear();
                go_unsynced("too many held commits");
            }
            break;
        }
        guest_on_commit(r);
        break;
    }
    case PacketId::WorldDigest:
        guest_on_digest(r);
        break;
    default:
        Log.warn("[mp-world] packet {} is not a world packet", static_cast<int>(id));
        break;
    }
}

void on_peer_left(std::uint32_t playerId) {
    st().guests.erase(playerId);
}

bool synced() {
    return st().synced;
}

std::uint32_t world_seq() {
    const State& s = st();
    return network_manager().role() == Role::Host ? s.worldSeq : s.lastApplied;
}

std::uint32_t current_digest() {
    const State& s = st();
    return s.haveShadow ? masked_digest(s.shadow) : 0;
}

void log_state(const char* tag) {
    const State& s = st();
    const char* t = tag != nullptr ? tag : "";
    const NetworkManager& net = network_manager();
    const char* role = net.role() == Role::Host   ? "host" :
                       net.role() == Role::Client ? "guest" :
                                                    "none";
    const int liveTbl = world_bridge::live_tbl();

    world::WorldView view;
    const char* src = "none";
    if (world_bridge::world_ready()) {
        world_bridge::capture(view);
        src = "engine";
    } else if (s.haveShadow) {
        view = s.shadow;
        src = "shadow";
    }

    Log.info("[mp-world] {} role={} seq={} synced={} digest=0x{:08X} liveTbl={} src={} pending={} "
             "guestEventClears={} refusals={}",
        t, role, world_seq(), s.synced ? 1 : 0, current_digest(), liveTbl, src, s.pending.size(),
        s.guestEventClears, s.refusals);
    if (src[0] == 'n') {
        return;
    }

    const bool liveOk = liveTbl >= 0 && liveTbl < static_cast<int>(world::kMemTbls);
    Log.info("[mp-world] {} counters rupee={} maxLife={} poh={} keyNum={}", t, view.rupee,
        view.maxLife, view.poh, liveOk ? view.key_num(static_cast<std::uint8_t>(liveTbl)) : -1);

    std::string line;
    append_bits(line, view.event.data(), view.event.size());
    Log.info("[mp-world] {} event:{}", t, line);

    for (std::size_t tbl = 0; tbl < world::kMemTbls; ++tbl) {
        const auto& bytes = view.mem[tbl];
        const bool live = liveOk && static_cast<std::size_t>(liveTbl) == tbl;
        if (!live && !any_bits(bytes.data(), bytes.size(), world::kMemKeyNumOffset)) {
            continue;
        }
        line.clear();
        append_bits(line, bytes.data(), bytes.size(), world::kMemKeyNumOffset);
        Log.info("[mp-world] {} membit tbl={}{}:{}", t, tbl, live ? " live" : "", line);
    }

    for (std::size_t tbl = 0; tbl < world::kMem2Tbls; ++tbl) {
        const auto& bytes = view.mem2[tbl];
        if (!any_bits(bytes.data(), bytes.size(), static_cast<std::size_t>(-1))) {
            continue;
        }
        line.clear();
        append_bits(line, bytes.data(), bytes.size());
        Log.info("[mp-world] {} mem2 tbl={}:{}", t, tbl, line);
    }

    line.clear();
    append_bits(line, view.firstGet.data(), view.firstGet.size());
    Log.info("[mp-world] {} firstget:{}", t, line);

    line.clear();
    append_bits(line, view.player.data(), view.player.size());
    Log.info("[mp-world] {} items:{}", t, line);

    log_temp(tag);
}

namespace {

/// "sw: 0 5 | item: 3" for the dan bytes (host-native u32s: byte * 8 + bit is the number).
std::string describe_dan(const std::array<std::uint8_t, world::kDanBytes>& dan) {
    std::string sw;
    std::string item;
    append_bits(sw, dan.data(), world::kDanSwitchBytes);
    append_bits(
        item, dan.data() + world::kDanSwitchBytes, world::kDanBytes - world::kDanSwitchBytes);
    return fmt::format("sw:{} | item:{}", sw, item);
}

/// The six BE u16 fields of a zone, decoded: zone switches 0-31, one-zone switches 0-15, zone
/// items 0-31, one-zone items 0-15. In `zoneswitch N` numbering, N < 32 is "sw N" and N >= 32 is
/// "one N-32".
std::string describe_zone(const std::array<std::uint8_t, world::kZoneBytes>& z) {
    static constexpr const char* kNames[6] = {"sw", "sw", "one", "item", "item", "oneitem"};
    std::string out;
    for (int f = 0; f < 6; ++f) {
        if (f == 0 || f == 2 || f == 3 || f == 5) {
            fmt::format_to(std::back_inserter(out), "{}{}:", f == 0 ? "" : " | ", kNames[f]);
        }
        const unsigned value = static_cast<unsigned>(z[2 * f]) << 8 | z[2 * f + 1];
        const int base = (f == 1 || f == 4) ? 16 : 0;
        for (int bit = 0; bit < 16; ++bit) {
            if ((value >> bit & 1) != 0) {
                fmt::format_to(std::back_inserter(out), " {}", base + bit);
            }
        }
    }
    return out;
}

}  // namespace

void log_temp(const char* tag) {
    State& s = st();
    TempState& t = s.temp;
    const char* tg = tag != nullptr ? tag : "";
    refresh_remote_tbls();

    std::string occupants;
    for (const auto& [id, tbl] : t.remoteTbl) {
        fmt::format_to(std::back_inserter(occupants), " {}:{}", id, static_cast<int>(tbl));
    }
    std::string mirrorList;
    for (const auto& entry : t.mirrors) {
        fmt::format_to(std::back_inserter(mirrorList), " {}", static_cast<int>(entry.first));
    }
    const int room = world_bridge::stay_room();
    world::TempView cur;
    const bool live = world_bridge::capture_temp(cur);
    Log.info("[mp-world] {} temp localTbl={} live={} danStageNo={} room={} zoneNo={} active={} "
             "shadow={} mirrors=[{} ] partners=[{} ] pending={}",
        tg, local_tbl(), live ? 1 : 0, world_bridge::dan_stage_no(), room,
        world_bridge::zone_no(room), temp_active() ? 1 : 0, t.shadowValid ? 1 : 0, mirrorList,
        occupants, t.pending.size());

    const int tbl = live ? cur.tbl : local_tbl();
    const auto mit = tbl >= 0 ? t.mirrors.find(static_cast<std::uint8_t>(tbl)) : t.mirrors.end();
    Log.info("[mp-world] {} dan tbl={} engine {} || mirror {}", tg, tbl,
        live ? describe_dan(cur.dan) : std::string("(not live)"),
        mit != t.mirrors.end() ? describe_dan(mit->second.dan) : std::string("(none)"));

    const bool haveZone = live && room >= 0 && room < static_cast<int>(world::kZoneRooms) &&
                          (cur.rooms >> room & 1) != 0;
    const bool mirrorZone = mit != t.mirrors.end() && room >= 0 &&
                            room < static_cast<int>(world::kZoneRooms) &&
                            (mit->second.rooms >> room & 1) != 0;
    Log.info("[mp-world] {} zone tbl={} room={} engine {} || mirror {}", tg, tbl, room,
        haveZone ? describe_zone(cur.zone[room]) : std::string("(no zone)"),
        mirrorZone ? describe_zone(mit->second.zone[room]) : std::string("(none)"));
}

void reset() {
    st() = State{};
}

/* --- batch 7: policy --- */

namespace {

/// The view groups a MultiplayerPolicy decides, as world_bridge's capture() reads them.
struct PolicyGroups {
    bool quest = false;      ///< questFlags: event, mem (keyNum included), mem2
    bool inventory = false;  ///< inventory: firstGet, player bytes, maxLife, poh
    bool rupees = false;     ///< rupees: the wallet
    bool any() const { return quest || inventory || rupees; }
};

PolicyGroups changed_groups(const MultiplayerPolicy& before) {
    PolicyGroups g;
    g.quest = before.questFlags != g_mpPolicy.questFlags;
    g.inventory = before.inventory != g_mpPolicy.inventory;
    g.rupees = before.rupees != g_mpPolicy.rupees;
    return g;
}

/// The fields of the groups in @p g, from @p from into @p to; the rest of @p to is left alone.
void copy_groups(const PolicyGroups& g, const world::WorldView& from, world::WorldView& to) {
    if (g.quest) {
        to.event = from.event;
        to.mem = from.mem;
        to.mem2 = from.mem2;
    }
    if (g.inventory) {
        to.firstGet = from.firstGet;
        to.player = from.player;
        to.maxLife = from.maxLife;
        to.poh = from.poh;
    }
    if (g.rupees) {
        to.rupee = from.rupee;
    }
}

const char* ownership_name(Ownership o) {
    return o == Ownership::Shared ? "Shared" : "Individual";
}

}  // namespace

void host_on_policy_changed(const MultiplayerPolicy& before) {
    State& s = st();
    const PolicyGroups g = changed_groups(before);
    if (!g.any()) {
        return;
    }
    if (!s.haveShadow) {
        Log.info("[mp-policy] host: no shared world yet; the first capture is taken under the new "
                 "policy");
        return;
    }
    const std::uint32_t was = masked_digest(s.shadow);
    // Under the NEW policy: zero for every group now Individual, this engine's values for every
    // group now Shared. Read from the save struct even mid stage change (mMemory belongs to no
    // table then, and capture reads mSave, which putSave has just written); a reset is handled by
    // the epoch check in tick(), which drops this shadow anyway.
    world_bridge::capture(s.cur);
    copy_groups(g, s.cur, s.shadow);
    Log.info("[mp-policy] host: shadow rebased for the switch (rupees {}, inventory {}, questFlags "
             "{}; shared wallet {}); digest 0x{:08X} -> 0x{:08X}, no ops emitted{}",
        ownership_name(g_mpPolicy.rupees), ownership_name(g_mpPolicy.inventory),
        ownership_name(g_mpPolicy.questFlags), s.shadow.rupee, was, masked_digest(s.shadow),
        world_bridge::world_ready() ? "" : " (world away: rebased from the saved tables)");
}

std::int32_t host_wallet() {
    const State& s = st();
    return g_mpPolicy.rupees == Ownership::Shared && s.haveShadow ? s.shadow.rupee : -1;
}

void guest_on_policy_changed(const MultiplayerPolicy& before, std::int32_t hostWallet) {
    State& s = st();
    const PolicyGroups g = changed_groups(before);
    if (!g.any()) {
        return;
    }
    if (g.rupees) {
        // Predictions for the wallet belong to the policy that made them. Shared -> Individual:
        // the host drops those ops (host_on_propose), so they would never be answered with a
        // value. Individual -> Shared: there are none (an Individual wallet is never proposed).
        s.pendingDelta.erase(static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(world::Domain::Rupee) << 8 | world::kNoTbl));
    }
    if (!s.haveShadow || !s.synced) {
        Log.info("[mp-policy] guest: not synced yet; the coming WorldSync is installed under the "
                 "new policy");
        return;
    }

    // Under the NEW policy, from this end's own engine: zero for groups now Individual, our own
    // values for groups now Shared, so nothing diffs. The host's values go in below.
    world_bridge::capture(s.cur);
    const std::int32_t ownWallet = s.cur.rupee;
    copy_groups(g, s.cur, s.shadow);

    if (g.rupees && g_mpPolicy.rupees == Ownership::Individual) {
        Log.info("[mp-policy] guest: wallet now per player; keeping this end's own (nothing sent)");
    } else if (g.rupees) {
        if (hostWallet >= 0) {
            world::WorldOp op;
            op.domain = world::Domain::Rupee;
            op.tbl = world::kNoTbl;
            world_bridge::apply_to_engine(op, hostWallet);
            s.shadow.rupee = std::min<std::int32_t>(hostWallet, 9999);
            Log.info("[mp-policy] guest: wallet now Shared; installed the host's wallet {} over "
                     "ours {}",
                hostWallet, ownWallet);
        } else {
            Log.info("[mp-policy] guest: wallet now Shared but the host sent no wallet; asking for "
                     "its world");
            s.policySyncOwed = true;
        }
    }
    if ((g.quest && g_mpPolicy.questFlags == Ownership::Shared) ||
        (g.inventory && g_mpPolicy.inventory == Ownership::Shared))
    {
        Log.info("[mp-policy] guest: quest flags or inventory now Shared; asking for the host's "
                 "world");
        s.policySyncOwed = true;
    }
    s.engineDriftDigests = 0;
}

}  // namespace dusk::mp::world_sync

// --- decomp hooks (declared in dusk/multiplayer.hpp) ---------------------------------------------

namespace dusk::mp {

using world_sync::st;

void world_on_dan_init(int stageNo) {
    world_sync::TempState& t = st().temp;
    t.danAnnouncePending = false;
    if (!world_sync::temp_active() || stageNo < 0 ||
        stageNo >= static_cast<int>(world::kMemTbls))
    {
        return;
    }
    world_sync::refresh_remote_tbls();
    const auto it = t.mirrors.find(static_cast<std::uint8_t>(stageNo));
    const bool partner = world_sync::remote_occupies(stageNo);
    if (it != t.mirrors.end() && partner) {
        // initDan has run: mDan belongs to stageNo, with its bits reset (we came from another
        // table) or kept (same table). Either way the shared copy is the truth now. The carry
        // save flag is left as vanilla set it: clrSaveFlag on a reset, so iron balls use their
        // default positions (their saved positions are static data this machine never wrote for
        // this visit); setSaveFlag only when this machine never left the table.
        world_bridge::seed_dan(it->second.dan);
        world_sync::Log.info("[mp-world] temp: H6 entered stage table {} where a partner is; dan "
                             "seeded from the shared copy ({})",
            stageNo, world_sync::describe_dan(it->second.dan));
        return;
    }
    if (partner) {
        // Should not happen (an occupied table always has a mirror). Keep our bits, and do not
        // announce them: that would overwrite the partner's in everyone's mirror.
        world_sync::Log.warn("[mp-world] temp: H6 stage table {} has a partner but no mirror here; "
                             "keeping vanilla dan bits, not announcing",
            stageNo);
        return;
    }
    t.danAnnouncePending = true;
    t.danAnnounceTbl = stageNo;
    world_sync::Log.info("[mp-world] temp: H6 entered stage table {} with nobody else in it; dan "
                         "is vanilla's, to be announced",
        stageNo);
}

void world_on_zone_created(int roomNo) {
    if (!world_sync::temp_active() || roomNo < 0 ||
        roomNo >= static_cast<int>(world::kZoneRooms))
    {
        return;
    }
    const int tbl = world_bridge::live_tbl();
    if (tbl < 0) {
        return;
    }
    world_sync::TempState& t = st().temp;
    const bool partnerHere = world_sync::remote_in_room(tbl, roomNo);
    const auto it = t.mirrors.find(static_cast<std::uint8_t>(tbl));
    if (partnerHere && it != t.mirrors.end() && (it->second.rooms >> roomNo & 1) != 0) {
        world_bridge::seed_zone(roomNo, it->second.zone[roomNo], true);
        world_sync::Log.info("[mp-world] temp: H7 room {} (table {}) zone created where a partner "
                             "stands; persistent bits seeded ({})",
            roomNo, tbl, world_sync::describe_zone(it->second.zone[roomNo]));
        return;
    }
    if (partnerHere) {
        // His zone for this room has never been reported (nothing set in it yet): fresh is equal.
        return;
    }
    std::array<std::uint8_t, world::kZoneBytes> bytes{};
    if (!world_bridge::read_zone(roomNo, bytes)) {
        return;
    }
    std::vector<world::WorldOp> ann;
    world::announce_zone(static_cast<std::uint8_t>(tbl), static_cast<std::uint8_t>(roomNo), bytes,
        ann);
    world::TempMirror& mirror = t.mirrors[static_cast<std::uint8_t>(tbl)];
    for (const world::WorldOp& op : ann) {
        world::apply_temp_op(mirror, op);
    }
    world_sync::temp_queue(std::move(ann));
}

void world_on_zone_lifecycle_begin() {
    if (!world_sync::temp_active()) {
        return;
    }
    world_sync::TempState& t = st().temp;
    world::TempView& cur = t.scratch;
    if (!t.shadowValid || !world_bridge::capture_temp(cur) || cur.tbl != t.shadow.tbl ||
        world_bridge::temp_epoch() != t.shadowEpoch)
    {
        return;
    }
    // Gameplay changes made earlier this tick, before the lifecycle clear can hide them.
    std::vector<world::WorldOp> diffs;
    world::diff_temp(t.shadow, cur, diffs);
    if (!diffs.empty()) {
        world::TempMirror& mirror = t.mirrors[static_cast<std::uint8_t>(cur.tbl)];
        for (const world::WorldOp& op : diffs) {
            world::apply_temp_op(mirror, op);
        }
        world_sync::temp_queue(std::move(diffs));
    }
    t.shadow = cur;
}

void world_on_zone_lifecycle_end() {
    if (!world_sync::temp_active()) {
        return;
    }
    world_sync::TempState& t = st().temp;
    world::TempView& cur = t.scratch;
    if (!t.shadowValid || !world_bridge::capture_temp(cur) || cur.tbl != t.shadow.tbl ||
        world_bridge::temp_epoch() != t.shadowEpoch)
    {
        return;
    }
    // The clears (and any zone removal) are this player's room lifecycle: adopt them silently.
    t.shadow = cur;
}

}  // namespace dusk::mp
