#include "enemy_sync.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "../net/packet.hpp"
#include "../net/serializer.hpp"
#include "../session/network_manager.hpp"
#include "../session/trace.hpp"
#include "dusk/achievements.h"
#include "dusk/logging.h"
#include "dusk/settings.h"
#include "enemy_internal.hpp"
#include "player_bridge.hpp"
#include "player_state.hpp"
#include "replication_manager.hpp"

#include "d/d_cc_d.h"
#include "d/d_cc_s.h"
#include "d/d_cc_uty.h"
#include "d/d_com_inf_game.h"
#include "d/d_s_play.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_player.h"
#include "d/d_resorce.h"
#include "f_op/f_op_actor.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "m_Do/m_Do_ext.h"

/**
 * M6 phase 1 core (agent E2). The design and the wire layouts are in enemy_sync.hpp; the target
 * stand-in and EnemyTargetScope are in enemy_target.cpp. Log lines start "[mp-enemy]" on both
 * ends, so one grep covers a two-instance run.
 */

namespace dusk::mp::enemies::detail {

/* ---- The adapter table. ★ Plain, constant-initialised storage: the adapters register from
 * file-scope static initialisers in the decomp translation units (d_a_e_<x>_mp.inc), which may run
 * before this file's own dynamic initialisers. Zero-initialised static PODs are ready before any
 * of them, so enemy_register_adapter is safe to call before main(). Nothing here may log. ---- */
struct AdapterSlot {
    short prof;
    const EnemyNetAdapter* adapter;
};
constexpr int kMaxAdapters = 96;
static AdapterSlot s_adapterSlots[kMaxAdapters];
static int s_adapterCount;

/// Latched by pre_tick, cleared by reset(). A plain bool, read for every actor every tick.
static bool s_enabled;
/* --- batch 10B: death and drops --- set by EnemyTargetScope around an authority enemy's
 * execute. */
static std::uint32_t s_execProc = kNoProc;

std::uint32_t exec_proc() {
    return s_execProc;
}

void set_exec_proc(std::uint32_t proc) {
    s_execProc = proc;
}

const EnemyNetAdapter* find_adapter(short prof) {
    for (int i = 0; i < s_adapterCount; i++) {
        if (s_adapterSlots[i].prof == prof) {
            return s_adapterSlots[i].adapter;
        }
    }
    return nullptr;
}

bool enabled() {
    return s_enabled;
}

namespace {

constexpr borealis::Log Log{"dusk::mp"};

/// A ghost that has had no state from its authority this long since it became a ghost drops to
/// local-only on both ends (§3: "hidden for up to 1 s, then local-only"; a little longer here so a
/// handoff over a slow link does not trip it).
constexpr std::uint64_t kNoSampleDegradeTicks = 45;
/// Rule 1 (§2.3): the host takes over a guest's room at the earliest this many ticks after it
/// loaded it (so its own fresh copies have adopted the guest's state as ghosts first)...
constexpr std::uint64_t kHostArrivalMinHoldTicks = 15;
/// ...and while any of its ghosts there was engaged in the last kEngagedWindowTicks, but never for
/// longer than this (spec: "if that hasn't happened after 10 s, hand off anyway").
constexpr std::uint64_t kHostArrivalMaxHoldTicks = 300;
constexpr std::uint64_t kEngagedWindowTicks = 90;
/// Samples kept per ghost; well past the 12-tick interpolation ceiling.
constexpr std::size_t kMaxSamples = 64;
/// A played tick this far outside a ghost's samples is a different timeline (the authority's pose
/// stream reset by a load): fall back to the newest sample.
constexpr std::uint64_t kTimelineSlack = 45;
/// EnemyUpdate packets are split before they pass this, to stay under the ~1200-byte safe MTU.
constexpr std::size_t kMaxUpdateBytes = 1100;
/// The cross-check hash rides on the first this-many updates after a record becomes authority,
/// then once every this-many ticks.
constexpr std::uint32_t kCheckEvery = 30;
constexpr std::uint32_t kStatsEvery = 150;

constexpr std::uint8_t kEntryCheck = 1;
constexpr std::uint8_t kEntryEngaged = 2;
constexpr std::uint8_t kEntryAnm = 4;

constexpr std::uint8_t kWireNoPlayer = 0xFF;

struct RoomSet {
    std::uint32_t stageHash = 0;
    /// (room, layer), room -1 = the stage itself.
    std::vector<std::pair<std::int8_t, std::int8_t>> rooms;

    bool operator==(const RoomSet& o) const { return stageHash == o.stageHash && rooms == o.rooms; }
    bool operator!=(const RoomSet& o) const { return !(*this == o); }
};

struct Owner {
    std::uint32_t owner = kNoPlayer;
    std::uint16_t epoch = 0;

    bool operator==(const Owner& o) const { return owner == o.owner && epoch == o.epoch; }
    bool operator!=(const Owner& o) const { return !(*this == o); }
};

struct State {
    std::unordered_map<std::uint32_t, Rec> recs;
    std::unordered_map<std::uint32_t, std::uint32_t> byKey;
    std::uint64_t tick = 0;
    bool adaptersLogged = false;

    RoomSet lastSentRooms;
    bool haveSentRooms = false;
    bool forceRooms = false;
    std::vector<std::uint32_t> pendingDegrade;
    /// Host: every player's rooms, its own under kHostPlayerId. Guest: the others' (logs only).
    std::unordered_map<std::uint32_t, RoomSet> peerRooms;
    /// Host: players that have been sent the table at least once.
    std::unordered_set<std::uint32_t> tableSentTo;

    std::map<std::uint64_t, Owner> table;
    /// Host: the last epoch handed out per room, for the whole session.
    std::unordered_map<std::uint64_t, std::uint16_t> epochs;
    /// Host: the tick the host first held a room a guest still owns (Rule 1).
    std::unordered_map<std::uint64_t, std::uint64_t> hostArrivedAt;
};

State& S() {
    static State s;
    return s;
}

std::uint64_t room_key(std::uint32_t stageHash, std::int8_t layer, std::int8_t room) {
    return (static_cast<std::uint64_t>(stageHash) << 16) |
           (static_cast<std::uint64_t>(static_cast<std::uint8_t>(layer)) << 8) |
           static_cast<std::uint64_t>(static_cast<std::uint8_t>(room));
}

std::uint64_t room_key(const Rec& rec) {
    return room_key(rec.stageHash, rec.layer, rec.room);
}

std::uint32_t key_stage(std::uint64_t k) {
    return static_cast<std::uint32_t>(k >> 16);
}
int key_layer(std::uint64_t k) {
    return static_cast<std::int8_t>(static_cast<std::uint8_t>(k >> 8));
}
int key_room(std::uint64_t k) {
    return static_cast<std::int8_t>(static_cast<std::uint8_t>(k));
}

std::string pid_str(std::uint32_t id) {
    return id == kNoPlayer ? std::string("none") : std::to_string(id);
}

std::uint8_t pid_to_wire(std::uint32_t id) {
    return id == kNoPlayer || id >= kWireNoPlayer ? kWireNoPlayer : static_cast<std::uint8_t>(id);
}

std::uint32_t pid_from_wire(std::uint8_t id) {
    return id == kWireNoPlayer ? kNoPlayer : static_cast<std::uint32_t>(id);
}

struct Fnv {
    std::uint32_t h = 2166136261u;
    void byte(std::uint8_t b) {
        h ^= b;
        h *= 16777619u;
    }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; i++) {
            byte(static_cast<std::uint8_t>(v >> (i * 8)));
        }
    }
    std::uint32_t done() const { return h != 0 ? h : 1u; }
};

/// The layer the stage loader itself uses (d_stage.cpp:2737-2793: getLayerNo(0), evaluated for the
/// stay room, or the start room before there is one). [unverified] A save flag that changes the
/// answer mid-stage would make this disagree with the layer the loaded actors were made for; both
/// ends would then see "no authority" and keep their enemies local, which is the safe failure.
std::int8_t current_layer() {
    return static_cast<std::int8_t>(dComIfG_play_c::getLayerNo(0));
}

std::uint32_t current_stage_hash() {
    const char* stage = dComIfGp_getStartStageName();
    return stage != nullptr ? location_stage_hash(stage) : 0;
}

std::uint32_t check_hash(fopAc_ac_c* actor) {
    Fnv f;
    const std::uint16_t prof = static_cast<std::uint16_t>(fopAcM_GetProfName(actor));
    f.byte(static_cast<std::uint8_t>(prof));
    f.byte(static_cast<std::uint8_t>(prof >> 8));
    f.u32(fopAcM_GetParam(actor));
    f.u32(static_cast<std::uint32_t>(std::lround(actor->home.pos.x)));
    f.u32(static_cast<std::uint32_t>(std::lround(actor->home.pos.y)));
    f.u32(static_cast<std::uint32_t>(std::lround(actor->home.pos.z)));
    return f.done();
}

fopAc_ac_c* actor_of(const Rec& rec) {
    fopAc_ac_c* actor = nullptr;
    // The two-argument form reports null while the actor is still mid-create.
    fopAcM_SearchByID(rec.proc, &actor);
    return actor;
}

/// The anm pointer -> resource index reverse lookup (§4.1): walk the arc's resource table until the
/// pointer matches. Cached per pointer and re-validated on every hit, because an archive that is
/// re-mounted moves its resources and a stale pointer could otherwise name a different one.
int anm_index(const char* arc, const void* anm) {
    if (arc == nullptr || anm == nullptr) {
        return -1;
    }
    dRes_info_c* info = dComIfG_getObjectResInfo(arc);
    if (info == nullptr || info->getArchive() == nullptr || info->mRes == nullptr) {
        return -1;
    }
    const int count = info->getResNum();
    static std::unordered_map<const void*, int> s_cache;
    const auto hit = s_cache.find(anm);
    if (hit != s_cache.end() && hit->second >= 0 && hit->second < count &&
        info->mRes[hit->second] == anm)
    {
        return hit->second;
    }
    for (int i = 0; i < count; i++) {
        if (info->mRes[i] == anm) {
            s_cache[anm] = i;
            return i;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------------------------------------
 * Rooms
 * ------------------------------------------------------------------------------------------- */

bool local_in_stage() {
    daPy_py_c* link = dComIfGp_getLinkPlayer();
    if (link == nullptr || fpcM_IsCreating(fopAcM_GetID(link))) {
        return false;
    }
    if (local_scene_change_queued() || fopAcM_SearchByName(fpcNm_TITLE_e) != nullptr) {
        return false;
    }
    return dComIfGp_getStartStageName() != nullptr;
}

RoomSet capture_local_rooms() {
    RoomSet out;
    if (!local_in_stage()) {
        return out;
    }
    out.stageHash = current_stage_hash();
    const std::int8_t layer = current_layer();
    out.rooms.emplace_back(static_cast<std::int8_t>(-1), layer);
    for (int room = 0; room < 64; room++) {
        if (local_room_loaded(room)) {
            out.rooms.emplace_back(static_cast<std::int8_t>(room), layer);
        }
    }
    return out;
}

std::string describe_rooms(const RoomSet& rs) {
    if (rs.rooms.empty()) {
        return "none";
    }
    std::string out =
        fmt::format("stage {:08X} layer {} rooms", rs.stageHash, rs.rooms.front().second);
    for (const auto& r : rs.rooms) {
        out += fmt::format(" {}", static_cast<int>(r.first));
    }
    return out;
}

void write_rooms_payload(Writer& w, std::uint32_t playerId, const RoomSet& rs,
    const std::vector<std::uint32_t>& degraded) {
    w.write_u32(playerId);
    w.write_u32(rs.stageHash);
    const std::size_t n = std::min<std::size_t>(rs.rooms.size(), 255);
    w.write_u8(static_cast<std::uint8_t>(n));
    for (std::size_t i = 0; i < n; i++) {
        w.write_u8(static_cast<std::uint8_t>(rs.rooms[i].first));
        w.write_u8(static_cast<std::uint8_t>(rs.rooms[i].second));
    }
    const std::size_t m = std::min<std::size_t>(degraded.size(), 255);
    w.write_u8(static_cast<std::uint8_t>(m));
    for (std::size_t i = 0; i < m; i++) {
        w.write_u32(degraded[i]);
    }
}

void degrade(Rec& rec, const char* why, bool tellOthers) {
    if (rec.degraded) {
        return;
    }
    Log.warn("[mp-enemy] degraded key={:08X} prof=0x{:03X} proc={} mode={} owner={}: {}; it stays "
             "local-only{}",
        rec.key, static_cast<unsigned>(static_cast<std::uint16_t>(rec.prof)), rec.proc,
        mode_name(rec.mode), pid_str(rec.authority), why,
        tellOthers ? " on both ends (reported in the next RoomsLoaded)" : "");
    rec.degraded = true;
    rec.mode = Mode::Local;
    rec.samples.clear();
    rec.havePlayed = false;
    if (tellOthers) {
        S().pendingDegrade.push_back(rec.key);
    }
}

void apply_remote_degrades(const std::vector<std::uint32_t>& keys, std::uint32_t from) {
    State& s = S();
    for (const std::uint32_t key : keys) {
        const auto k = s.byKey.find(key);
        if (k == s.byKey.end()) {
            Log.info("[mp-enemy] player {} degraded key={:08X}; not alive here", from, key);
            continue;
        }
        if (Rec* rec = find_rec(k->second)) {
            degrade(*rec, fmt::format("player {} degraded it", from).c_str(), false);
        }
    }
}

void send_rooms_if_changed() {
    State& s = S();
    NetworkManager& net = network_manager();
    const RoomSet now = capture_local_rooms();
    if (net.role() == Role::Host) {
        s.peerRooms[kHostPlayerId] = now;
    }
    if (s.haveSentRooms && now == s.lastSentRooms && s.pendingDegrade.empty() && !s.forceRooms) {
        return;
    }
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::RoomsLoaded));
    write_rooms_payload(w, net.local_player_id(), now, s.pendingDegrade);
    if (net.role() == Role::Host) {
        net.send_to_session(w, kChannelObjects, true);
    } else {
        net.send_to_player(kHostPlayerId, w, kChannelObjects, true);
    }
    if (!s.haveSentRooms || now != s.lastSentRooms || s.forceRooms) {
        Log.info("[mp-enemy] rooms out: {}{}", describe_rooms(now),
            s.pendingDegrade.empty() ?
                "" :
                fmt::format(" (+{} degraded key(s))", s.pendingDegrade.size()));
    }
    s.lastSentRooms = now;
    s.haveSentRooms = true;
    s.forceRooms = false;
    s.pendingDegrade.clear();
}

/* ---------------------------------------------------------------------------------------------
 * The authority table
 * ------------------------------------------------------------------------------------------- */

void log_table_diff(
    const std::map<std::uint64_t, Owner>& before, const std::map<std::uint64_t, Owner>& after) {
    for (const auto& [k, o] : after) {
        const auto it = before.find(k);
        if (it != before.end() && it->second == o) {
            continue;
        }
        Log.info("[mp-enemy] table stage={:08X} layer={} room={} owner {} -> {} epoch={}",
            key_stage(k), key_layer(k), key_room(k),
            it != before.end() ? pid_str(it->second.owner) : std::string("none"), pid_str(o.owner),
            o.epoch);
    }
    for (const auto& [k, o] : before) {
        if (after.find(k) == after.end()) {
            Log.info("[mp-enemy] table stage={:08X} layer={} room={} owner {} -> none (nobody has "
                     "it loaded) epoch={}",
                key_stage(k), key_layer(k), key_room(k), pid_str(o.owner), o.epoch);
        }
    }
}

void write_table(Writer& w) {
    const State& s = S();
    w.write_u8(static_cast<std::uint8_t>(PacketId::EnemyAuthority));
    const std::size_t n = std::min<std::size_t>(s.table.size(), 255);
    w.write_u8(static_cast<std::uint8_t>(n));
    std::size_t i = 0;
    for (const auto& [k, o] : s.table) {
        if (i++ >= n) {
            break;
        }
        w.write_u32(key_stage(k));
        w.write_u8(static_cast<std::uint8_t>(key_layer(k)));
        w.write_u8(static_cast<std::uint8_t>(key_room(k)));
        w.write_u8(pid_to_wire(o.owner));
        w.write_u16(o.epoch);
    }
}

/// Rule 1's hold condition for room `k` on the host: one of its own ghosts there has not been
/// given the owner's state yet, or was engaged recently.
bool host_hold_room(std::uint64_t k) {
    const State& s = S();
    for (const auto& [proc, rec] : s.recs) {
        if (rec.mode != Mode::Ghost || room_key(rec) != k) {
            continue;
        }
        if (!rec.everSampled) {
            return true;
        }
        if (rec.everEngaged && s.tick - rec.lastEngagedAt <= kEngagedWindowTicks) {
            return true;
        }
    }
    return false;
}

void host_recompute_table() {
    State& s = S();
    std::map<std::uint64_t, std::vector<std::uint32_t>> holders;
    for (const auto& [pid, rs] : s.peerRooms) {
        for (const auto& r : rs.rooms) {
            holders[room_key(rs.stageHash, r.second, r.first)].push_back(pid);
        }
    }
    std::map<std::uint64_t, Owner> next;
    for (auto& [k, ids] : holders) {
        std::sort(ids.begin(), ids.end());
        const bool hostHas = ids.front() == kHostPlayerId;
        const auto prev = s.table.find(k);
        const bool prevHolds = prev != s.table.end() && prev->second.owner != kNoPlayer &&
                               std::find(ids.begin(), ids.end(), prev->second.owner) != ids.end();
        std::uint32_t owner = kNoPlayer;
        if (hostHas && prevHolds && prev->second.owner != kHostPlayerId) {
            // Rule 1: the host arrived in a room a guest is simulating. Defer.
            const auto arrived = s.hostArrivedAt.try_emplace(k, s.tick).first->second;
            const std::uint64_t held = s.tick - arrived;
            // The minimum hold is the cvar (default 15 = kHostArrivalMinHoldTicks). A test may widen
            // it past the 300-tick cap, which then moves with it (capped at 3600 ticks).
            const int holdCvar = dusk::getSettings().multiplayer.enemyHostHoldTicks.getValue();
            const std::uint64_t minHold =
                holdCvar < 0 ? kHostArrivalMinHoldTicks : std::min<std::uint64_t>(holdCvar, 3600);
            const std::uint64_t maxHold = std::max(kHostArrivalMaxHoldTicks, minHold);
            const bool hold = held < minHold || (held < maxHold && host_hold_room(k));
            owner = hold ? prev->second.owner : kHostPlayerId;
            if (!hold) {
                Log.info("[mp-enemy] host takes over stage={:08X} room={} from player {} after {} "
                         "tick(s){}",
                    key_stage(k), key_room(k), prev->second.owner, held,
                    held >= maxHold ? " (hold cap reached)" : "");
            }
        } else if (hostHas) {
            owner = kHostPlayerId;
        } else if (prevHolds) {
            // Sticky among guests: no handoff while the owner still has the room.
            owner = prev->second.owner;
        } else {
            owner = ids.front();
        }
        if (owner == kHostPlayerId) {
            s.hostArrivedAt.erase(k);
        }
        Owner o;
        o.owner = owner;
        if (prev != s.table.end() && prev->second.owner == owner) {
            o.epoch = prev->second.epoch;
        } else {
            o.epoch = ++s.epochs[k];
        }
        next[k] = o;
    }
    for (auto it = s.hostArrivedAt.begin(); it != s.hostArrivedAt.end();) {
        it = next.count(it->first) == 0 ? s.hostArrivedAt.erase(it) : std::next(it);
    }
    if (next == s.table) {
        return;
    }
    log_table_diff(s.table, next);
    s.table = std::move(next);
    Writer w;
    write_table(w);
    network_manager().send_to_session(w, kChannelObjects, true);
}

/* ---------------------------------------------------------------------------------------------
 * Modes
 * ------------------------------------------------------------------------------------------- */

void transition(Rec& rec, Mode want, std::uint32_t owner, std::uint16_t epoch) {
    State& s = S();
    Log.info("[mp-enemy] mode key={:08X} prof=0x{:03X} proc={} {} -> {} owner {} -> {} epoch {} -> "
             "{} tick={}",
        rec.key, static_cast<unsigned>(static_cast<std::uint16_t>(rec.prof)), rec.proc,
        mode_name(rec.mode), mode_name(want), pid_str(rec.authority), pid_str(owner), rec.epoch,
        epoch, network_manager().sim_tick());
    if (want == Mode::Ghost) {
        if (rec.mode != Mode::Ghost) {
            rec.havePlayed = false;
            rec.everSampled = false;
            rec.ghostSince = s.tick;
            rec.ghostAnm = 0xFFFF;
            rec.everEngaged = false;
            rec.ghostFlagsValid = false;  // batch 9: no mFlags written by this ghost yet
        }
        // A different owner or epoch is a different stream: its samples start over. The last
        // played sample is kept, so the ghost holds still instead of snapping home meanwhile.
        rec.samples.clear();
        rec.splitBrainLogged = false;
    }
    if (want == Mode::Authority && rec.mode != Mode::Authority) {
        // Rule 2: the AI takes over from the last state the ghost was given. A fresh target
        // decision, which starts on the local Link (enemy_target.cpp).
        rec.target = kNoPlayer;
        rec.challenger = kNoPlayer;
        rec.challengerTicks = 0;
        rec.updatesSent = 0;
        if (rec.mode == Mode::Ghost && rec.adapter != nullptr &&
            rec.adapter->onBecomeAuthority != nullptr)
        {
            if (fopAc_ac_c* actor = actor_of(rec)) {
                rec.adapter->onBecomeAuthority(actor);
            }
        }
    }
    rec.mode = want;
    rec.authority = owner;
    rec.epoch = epoch;
}

void update_modes() {
    State& s = S();
    const std::uint32_t me = network_manager().local_player_id();
    for (auto& [proc, rec] : s.recs) {
        Mode want = Mode::Local;
        std::uint32_t owner = kNoPlayer;
        std::uint16_t epoch = 0;
        if (!rec.degraded) {
            const auto t = s.table.find(room_key(rec));
            if (t != s.table.end() && t->second.owner != kNoPlayer) {
                owner = t->second.owner;
                epoch = t->second.epoch;
                want = owner == me ? Mode::Authority : Mode::Ghost;
            }
        }
        if (want == rec.mode && owner == rec.authority && epoch == rec.epoch) {
            continue;
        }
        transition(rec, want, owner, epoch);
    }
}

/* ---------------------------------------------------------------------------------------------
 * Ghost playback
 * ------------------------------------------------------------------------------------------- */

void play_ghost(Rec& rec) {
    if (rec.samples.empty()) {
        return;  // keep showing the last played sample
    }
    std::uint64_t played = rec.samples.back().tick;
    const auto& players = replication_manager().players();
    const auto it = players.find(rec.authority);
    if (it != players.end() && it->second.buffer.has_data()) {
        // ★ The owner-timeline rule (object_channel.cpp receiver_tick): show the enemy at the
        // tick its authority's puppet is being shown at.
        const std::uint64_t p = it->second.buffer.played_tick();
        if (p + kTimelineSlack >= rec.samples.front().tick &&
            p <= rec.samples.back().tick + kTimelineSlack)
        {
            played = p;
        }
    }
    std::size_t i = 0;
    while (i + 1 < rec.samples.size() && rec.samples[i + 1].tick <= played) {
        i++;
    }
    const Sample& a = rec.samples[i];
    Sample out = a;
    if (played > a.tick && i + 1 < rec.samples.size()) {
        const Sample& b = rec.samples[i + 1];
        const float t = static_cast<float>(played - a.tick) / static_cast<float>(b.tick - a.tick);
        for (int k = 0; k < 3; k++) {
            out.pos[k] = a.pos[k] + (b.pos[k] - a.pos[k]) * t;
            out.shape[k] = lerp_angle(a.shape[k], b.shape[k], t);
        }
        out.angleY = lerp_angle(a.angleY, b.angleY, t);
    }
    rec.played = out;
    rec.havePlayed = true;
    while (rec.samples.size() > 2 && rec.samples[1].tick <= played) {
        rec.samples.pop_front();
    }
}

/* ---------------------------------------------------------------------------------------------
 * EnemyUpdate
 * ------------------------------------------------------------------------------------------- */

Sample capture_sample(Rec& rec, fopAc_ac_c* actor) {
    Sample s;
    s.pos[0] = actor->current.pos.x;
    s.pos[1] = actor->current.pos.y;
    s.pos[2] = actor->current.pos.z;
    s.shape[0] = actor->shape_angle.x;
    s.shape[1] = actor->shape_angle.y;
    s.shape[2] = actor->shape_angle.z;
    s.angleY = actor->current.angle.y;
    s.health = actor->health;
    // Every adapted enemy is an fopEn_enemy_c or embeds one at offset 0 (mp-m6-enemies.md §1.2).
    s.flags = static_cast<fopEn_enemy_c*>(actor)->mFlags;
    const EnemyNetAdapter* ad = rec.adapter;
    mDoExt_McaMorfSO* morf = ad->morf != nullptr ? ad->morf(actor) : nullptr;
    if (morf != nullptr && morf->getAnm() != nullptr) {
        const int idx = anm_index(ad->arc, morf->getAnm());
        if (idx >= 0) {
            s.anmValid = true;
            s.anm = static_cast<std::uint16_t>(idx);
            s.frame = morf->getFrame();
            s.rate = morf->getPlaySpeed();
            s.attr = static_cast<std::uint8_t>(morf->getPlayMode());
            // setMorf (m_Do_ext.cpp:1289-1299) keeps 1 / morf frames in field_0x34 (negative for
            // the eased form). Recovered as whole frames for the ghost's own setAnm.
            const float step = std::fabs(morf->field_0x34);
            const float frames = step > 0.0f ? 1.0f / step : 0.0f;
            s.morph = static_cast<std::uint8_t>(std::clamp(std::lround(frames), 0l, 255l));
        }
    }
    s.engaged = ad->engaged != nullptr && ad->engaged(actor);
    if (ad->captureExtra != nullptr) {
        ad->captureExtra(actor, s.extra);
        if (s.extra.len > sizeof(s.extra.data)) {
            s.extra.len = sizeof(s.extra.data);
        }
    }
    s.targetPeer = pid_to_wire(rec.target);
    return s;
}

void write_entry(Writer& w, const Rec& rec, const Sample& s, bool withCheck) {
    w.write_u32(rec.key);
    std::uint8_t eflags = 0;
    if (withCheck) {
        eflags |= kEntryCheck;
    }
    if (s.engaged) {
        eflags |= kEntryEngaged;
    }
    if (s.anmValid) {
        eflags |= kEntryAnm;
    }
    w.write_u8(eflags);
    if (withCheck) {
        w.write_u32(rec.check);
    }
    w.write_f32(s.pos[0]);
    w.write_f32(s.pos[1]);
    w.write_f32(s.pos[2]);
    w.write_s16(s.shape[0]);
    w.write_s16(s.shape[1]);
    w.write_s16(s.shape[2]);
    w.write_s16(s.angleY);
    if (s.anmValid) {
        w.write_u16(s.anm);
        w.write_u16(
            static_cast<std::uint16_t>(std::clamp(std::lround(s.frame * 16.0f), 0l, 65535l)));
        w.write_s16(
            static_cast<std::int16_t>(std::clamp(std::lround(s.rate * 256.0f), -32768l, 32767l)));
        w.write_u8(s.attr);
        w.write_u8(s.morph);
    }
    w.write_s16(s.health);
    w.write_u16(s.flags);
    w.write_u8(s.targetPeer);
    w.write_u8(s.extra.len);
    for (unsigned i = 0; i < s.extra.len; i++) {
        w.write_u8(s.extra.data[i]);
    }
}

bool read_entry(
    Reader& r, std::uint32_t& key, std::uint8_t& eflags, std::uint32_t& check, Sample& s) {
    if (!(r.read_u32(key) && r.read_u8(eflags))) {
        return false;
    }
    check = 0;
    if ((eflags & kEntryCheck) != 0 && !r.read_u32(check)) {
        return false;
    }
    if (!(r.read_f32(s.pos[0]) && r.read_f32(s.pos[1]) && r.read_f32(s.pos[2]) &&
            r.read_s16(s.shape[0]) && r.read_s16(s.shape[1]) && r.read_s16(s.shape[2]) &&
            r.read_s16(s.angleY)))
    {
        return false;
    }
    s.anmValid = (eflags & kEntryAnm) != 0;
    if (s.anmValid) {
        std::uint16_t frame = 0;
        std::int16_t rate = 0;
        if (!(r.read_u16(s.anm) && r.read_u16(frame) && r.read_s16(rate) && r.read_u8(s.attr) &&
                r.read_u8(s.morph)))
        {
            return false;
        }
        s.frame = static_cast<float>(frame) / 16.0f;
        s.rate = static_cast<float>(rate) / 256.0f;
    }
    std::uint8_t len = 0;
    if (!(r.read_s16(s.health) && r.read_u16(s.flags) && r.read_u8(s.targetPeer) && r.read_u8(len)))
    {
        return false;
    }
    s.extra.len = 0;
    for (unsigned i = 0; i < len; i++) {
        std::uint8_t b = 0;
        if (!r.read_u8(b)) {
            return false;
        }
        if (i < sizeof(s.extra.data)) {
            s.extra.data[i] = b;
        }
    }
    s.extra.len = static_cast<unsigned char>(std::min<unsigned>(len, sizeof(s.extra.data)));
    s.engaged = (eflags & kEntryEngaged) != 0;
    for (int k = 0; k < 3; k++) {
        if (!std::isfinite(s.pos[k])) {
            return false;
        }
    }
    return true;
}

void send_updates() {
    State& s = S();
    NetworkManager& net = network_manager();
    // One batch per room. The header names the room; every entry of a batch is in it.
    std::map<std::uint64_t, std::vector<std::vector<std::uint8_t>>> batches;
    for (auto& [proc, rec] : s.recs) {
        if (rec.mode != Mode::Authority) {
            continue;
        }
        fopAc_ac_c* actor = actor_of(rec);
        if (actor == nullptr) {
            continue;
        }
        const Sample sample = capture_sample(rec, actor);
        const bool withCheck = rec.updatesSent < kCheckEvery || s.tick % kCheckEvery == 0;
        Writer entry;
        write_entry(entry, rec, sample, withCheck);
        rec.updatesSent++;
        batches[room_key(rec)].push_back(entry.data());
    }
    for (const auto& [k, entries] : batches) {
        const auto t = s.table.find(k);
        const std::uint16_t epoch = t != s.table.end() ? t->second.epoch : 0;
        std::size_t i = 0;
        while (i < entries.size()) {
            Writer w;
            w.write_u8(static_cast<std::uint8_t>(PacketId::EnemyUpdate));
            w.write_u8(pid_to_wire(net.local_player_id()));
            w.write_u64(net.sim_tick());
            w.write_u32(key_stage(k));
            w.write_u8(static_cast<std::uint8_t>(key_layer(k)));
            w.write_u8(static_cast<std::uint8_t>(key_room(k)));
            w.write_u16(epoch);
            // How many fit, decided before the count byte is written.
            std::size_t n = 0;
            std::size_t bytes = w.size() + 1;
            while (i + n < entries.size() && n < 255 &&
                   (n == 0 || bytes + entries[i + n].size() <= kMaxUpdateBytes))
            {
                bytes += entries[i + n].size();
                n++;
            }
            w.write_u8(static_cast<std::uint8_t>(n));
            for (std::size_t j = 0; j < n; j++) {
                for (const std::uint8_t b : entries[i + j]) {
                    w.write_u8(b);
                }
            }
            i += n;
            if (net.role() == Role::Host) {
                net.send_to_session(w, kChannelState, false);
            } else {
                net.send_to_player(kHostPlayerId, w, kChannelState, false);
            }
        }
    }
}

void insert_sample(Rec& rec, const Sample& sample) {
    auto at = std::lower_bound(rec.samples.begin(), rec.samples.end(), sample.tick,
        [](const Sample& a, std::uint64_t t) { return a.tick < t; });
    if (at != rec.samples.end() && at->tick == sample.tick) {
        return;  // duplicate
    }
    rec.samples.insert(at, sample);
    while (rec.samples.size() > kMaxSamples) {
        rec.samples.pop_front();
    }
}

bool on_update(Reader& r, std::uint32_t fromPlayerId) {
    State& s = S();
    NetworkManager& net = network_manager();
    std::uint8_t authorityWire = 0;
    std::uint64_t tick = 0;
    std::uint32_t stageHash = 0;
    std::uint8_t layer = 0;
    std::uint8_t room = 0;
    std::uint16_t epoch = 0;
    std::uint8_t n = 0;
    if (!(r.read_u8(authorityWire) && r.read_u64(tick) && r.read_u32(stageHash) &&
            r.read_u8(layer) && r.read_u8(room) && r.read_u16(epoch) && r.read_u8(n)))
    {
        return false;
    }
    const std::uint32_t authority = pid_from_wire(authorityWire);
    const std::uint64_t k =
        room_key(stageHash, static_cast<std::int8_t>(layer), static_cast<std::int8_t>(room));
    const auto t = s.table.find(k);
    const bool ownerMatches = t != s.table.end() && t->second.owner == authority;
    if (net.role() == Role::Host && (authority != fromPlayerId || !ownerMatches)) {
        // A guest speaks only for rooms the host's table gives it.
        static int s_warned = 0;
        if (s_warned++ < 8) {
            Log.warn(
                "[mp-enemy] dropping EnemyUpdate from player {}: authority {} room {} owner {}",
                fromPlayerId, pid_str(authority), static_cast<int>(static_cast<std::int8_t>(room)),
                t != s.table.end() ? pid_str(t->second.owner) : std::string("none"));
        }
        return false;
    }
    for (std::uint8_t i = 0; i < n; i++) {
        std::uint32_t key = 0;
        std::uint8_t eflags = 0;
        std::uint32_t check = 0;
        Sample sample;
        if (!read_entry(r, key, eflags, check, sample)) {
            Log.warn("[mp-enemy] malformed EnemyUpdate from player {} (entry {} of {})",
                fromPlayerId, i, n);
            return false;
        }
        sample.tick = tick;
        if (!ownerMatches) {
            continue;
        }
        const auto byKey = s.byKey.find(key);
        if (byKey == s.byKey.end()) {
            continue;  // not created here (yet), or killed here already
        }
        Rec* rec = find_rec(byKey->second);
        if (rec == nullptr || rec->degraded) {
            continue;
        }
        if (rec->mode == Mode::Authority) {
            if (!rec->splitBrainLogged) {
                rec->splitBrainLogged = true;
                Log.error(
                    "[mp-enemy] split brain: key={:08X} is mine, but player {} sent its state "
                    "(epoch {}, mine {})",
                    key, pid_str(authority), epoch, rec->epoch);
            }
            continue;
        }
        if (rec->mode != Mode::Ghost || rec->authority != authority || rec->epoch != epoch) {
            continue;  // a stale epoch, or a table change still in flight
        }
        if ((eflags & kEntryCheck) != 0 && check != rec->check) {
            degrade(*rec,
                fmt::format("cross-check mismatch: mine {:08X}, player {}'s {:08X}", rec->check,
                    authority, check)
                    .c_str(),
                true);
            continue;
        }
        if (!rec->everSampled) {
            Log.info("[mp-enemy] first state key={:08X} from player {} epoch {} after {} tick(s)",
                key, authority, epoch, s.tick - rec->ghostSince);
        }
        rec->everSampled = true;
        if (sample.engaged) {
            rec->everEngaged = true;
            rec->lastEngagedAt = s.tick;
        }
        insert_sample(*rec, sample);
    }
    return net.role() == Role::Host;
}

bool on_authority(Reader& r) {
    State& s = S();
    if (network_manager().role() != Role::Client) {
        return false;
    }
    std::uint8_t n = 0;
    if (!r.read_u8(n)) {
        return false;
    }
    std::map<std::uint64_t, Owner> next;
    for (std::uint8_t i = 0; i < n; i++) {
        std::uint32_t stageHash = 0;
        std::uint8_t layer = 0;
        std::uint8_t room = 0;
        std::uint8_t owner = 0;
        std::uint16_t epoch = 0;
        if (!(r.read_u32(stageHash) && r.read_u8(layer) && r.read_u8(room) && r.read_u8(owner) &&
                r.read_u16(epoch)))
        {
            Log.warn("[mp-enemy] malformed EnemyAuthority");
            return false;
        }
        Owner o;
        o.owner = pid_from_wire(owner);
        o.epoch = epoch;
        next[room_key(stageHash, static_cast<std::int8_t>(layer), static_cast<std::int8_t>(room))] =
            o;
    }
    if (next != s.table) {
        log_table_diff(s.table, next);
        s.table = std::move(next);
    }
    return false;
}

bool on_rooms(Reader& r, std::uint32_t fromPlayerId) {
    State& s = S();
    NetworkManager& net = network_manager();
    std::uint32_t playerId = 0;
    RoomSet rs;
    std::uint8_t n = 0;
    if (!(r.read_u32(playerId) && r.read_u32(rs.stageHash) && r.read_u8(n))) {
        return false;
    }
    for (std::uint8_t i = 0; i < n; i++) {
        std::uint8_t room = 0;
        std::uint8_t layer = 0;
        if (!(r.read_u8(room) && r.read_u8(layer))) {
            return false;
        }
        rs.rooms.emplace_back(static_cast<std::int8_t>(room), static_cast<std::int8_t>(layer));
    }
    std::uint8_t m = 0;
    std::vector<std::uint32_t> degraded;
    if (!r.read_u8(m)) {
        return false;
    }
    for (std::uint8_t i = 0; i < m; i++) {
        std::uint32_t key = 0;
        if (!r.read_u32(key)) {
            return false;
        }
        degraded.push_back(key);
    }
    if (net.role() == Role::Host && playerId != fromPlayerId) {
        Log.warn("[mp-enemy] RoomsLoaded from player {} claims to be player {}; dropped",
            fromPlayerId, playerId);
        return false;
    }
    if (playerId == net.local_player_id()) {
        return false;
    }
    const auto old = s.peerRooms.find(playerId);
    if (old == s.peerRooms.end() || old->second != rs) {
        Log.info("[mp-enemy] rooms in: player {} {}", playerId, describe_rooms(rs));
    }
    s.peerRooms[playerId] = rs;
    apply_remote_degrades(degraded, playerId);
    if (net.role() == Role::Host) {
        if (s.tableSentTo.insert(playerId).second) {
            // Its first word: it has never heard the table (a later change is broadcast anyway).
            Writer w;
            write_table(w);
            net.send_to_player(playerId, w, kChannelObjects, true);
        }
        return true;
    }
    return false;
}

/* =============================================================================================
 * --- batch 9: enemy hits (M6 phase 2, mp-m6-enemies.md §5.1, §5.2, §5.6.5 items 3 and 7) ---
 * A player hits a GHOST in his own collision pass; the ghost tick reads that hit off the ghost's
 * TG and sends it (EnemyHit) to the enemy's authority, which validates it and injects it before its
 * next actor pass as a proxy AT collider owned by the attacker's puppet. The enemy's own
 * damage_check then runs on it: its own reaction, invulnerability timer and HP loss. The ghost
 * never changes its own HP. Flag bits the attacker's Link sets directly (CutDownHit, wolf bite)
 * travel as EnemyFlagHit.
 * =========================================================================================== */

/// mFlags bits only the PLAYER sets on an enemy (§5.1 item 8); a ghost forwards these.
constexpr std::uint16_t kForwardFlagBits =
    fopEn_enemy_c::fopEn_flag_CutDownHit | fopEn_enemy_c::fopEn_flag_WolfBiteDamage;
/// Injected hits per tick (the rest wait a tick). Each needs its own proxy collider, which must
/// outlive the tick: the enemy's TG keeps pointing at it until the next collision pass.
constexpr int kMaxProxies = 4;
/// Highest TG index a ghost looks for (tgByIndex answers null past its last one).
constexpr int kMaxTgIndex = 16;
/// Validation: attacker-to-enemy distance. Melee (sword, a Link hit type) vs everything else
/// (arrows, bombs, boomerang). Generous: the puppet pose the authority sees lags the attacker.
constexpr float kMaxMeleeHitDist = 1200.0f;
constexpr float kMaxRangedHitDist = 6000.0f;

constexpr std::uint8_t kHitMaster = 0x01;
constexpr std::uint8_t kHitWood = 0x02;
constexpr std::uint8_t kHitWolf = 0x04;
constexpr std::uint8_t kHitSwordUp = 0x08;
constexpr std::uint8_t kHitHeadJump = 0x10;
constexpr std::uint8_t kHitCutJumpCancel = 0x20;
constexpr std::uint8_t kHitHorse = 0x40;
/* --- batch 10B: death and drops --- the attacker's Link was in a rollstab finish (the
 * rollstab_kill achievement is the killer's). */
constexpr std::uint8_t kHitRollstab = 0x80;

struct PendingHit {
    std::uint32_t attacker = kNoPlayer;
    std::uint32_t key = 0;
    std::uint16_t epoch = 0;
    std::uint8_t tgIndex = 0;
    std::uint32_t atType = 0;
    std::uint8_t atp = 0;
    std::uint8_t atSpl = 0;
    std::uint8_t atMtrl = 0;
    std::uint8_t atSe = 0;
    std::uint8_t atHitMark = 0;
    float pos[3] = {0.0f, 0.0f, 0.0f};
    std::uint8_t hitType = 0;
    std::uint32_t hitBit = 0;
    std::int8_t hitStatus = 0;
    std::uint8_t flags = 0;
    std::uint8_t cutType = 0;
    std::int16_t angleY = 0;
    std::int16_t life = 0;
    /* --- batch 10B --- */
    std::int16_t maxLife = 0;
};

struct PendingFlag {
    std::uint32_t attacker = kNoPlayer;
    std::uint32_t key = 0;
    std::uint16_t epoch = 0;
    std::uint16_t bit = 0;
};

struct Proxy {
    dCcD_Stts stts;
    dCcD_Sph sph;
    EnemyHitOverride ov;
};

/// Constructed on first use (not at static-init time: dCcD objects have real constructors).
Proxy* proxies() {
    static Proxy p[kMaxProxies];
    return p;
}
/// Which proxies carry an override. Plain storage, read by enemy_hit_override for every hit.
bool s_proxyLive[kMaxProxies];

std::vector<PendingHit> s_pendingHits;
std::vector<PendingFlag> s_pendingFlags;
std::uint32_t s_hitsUnknownKey = 0;

const EnemyHitOverride* hit_override(const cCcD_Obj* atObj) {
    if (atObj == nullptr) {
        return nullptr;
    }
    for (int i = 0; i < kMaxProxies; i++) {
        if (s_proxyLive[i] && static_cast<const cCcD_Obj*>(&proxies()[i].sph) == atObj) {
            return &proxies()[i].ov;
        }
    }
    return nullptr;
}

Rec* find_rec_by_key(std::uint32_t key) {
    State& s = S();
    const auto k = s.byKey.find(key);
    if (k == s.byKey.end()) {
        return nullptr;
    }
    return find_rec(k->second);
}

void send_to_authority(Writer& w, std::uint32_t authority) {
    NetworkManager& net = network_manager();
    if (net.role() == Role::Host) {
        net.send_to_player(authority, w, kChannelObjects, true);
    } else {
        // A guest only talks to the host; the host relays it when the authority is another guest.
        net.send_to_player(kHostPlayerId, w, kChannelObjects, true);
    }
}

/// The attacker's machine: this ghost's TGs were hit in the last collision pass. Send each hit made
/// by this end's own Link (or his bomb, arrow, boomerang) to the authority, and give the attacker
/// the hit-stop his own hit would have caused (§5.6.5 item 7; the authority suppresses its own).
void ghost_scan_hits(Rec& rec, fopAc_ac_c* actor) {
    if (rec.adapter == nullptr || rec.adapter->tgByIndex == nullptr || rec.authority == kNoPlayer ||
        rec.despawnPending)
    {
        return;
    }
    daPy_py_c* link = dComIfGp_getLinkPlayer();
    if (link == nullptr) {
        return;
    }
    NetworkManager& net = network_manager();
    for (int i = 0; i < kMaxTgIndex; i++) {
        cCcD_Obj* tg = rec.adapter->tgByIndex(actor, i);
        if (tg == nullptr) {
            break;
        }
        dCcD_GObjInf* tgInf = dCcD_GetGObjInf(tg);
        if (tgInf == nullptr || !tgInf->ChkTgHit()) {
            continue;
        }
        cCcD_Obj* atObj = tgInf->GetTgHitObj();
        dCcD_GObjInf* atInf = tgInf->GetTgHitGObj();
        if (atObj == nullptr || atInf == nullptr) {
            continue;
        }
        fopAc_ac_c* owner = atObj->GetAc();
        if (owner == nullptr) {
            continue;
        }
        const s16 ownerName = fopAcM_GetName(owner);
        const bool byLink = owner == static_cast<fopAc_ac_c*>(link);
        if (!byLink && ownerName != fpcNm_NBOMB_e && ownerName != fpcNm_BOOMERANG_e &&
            ownerName != fpcNm_ARROW_e)
        {
            continue;  // not this player's: another enemy, a puppet, the world
        }

        // Classify it exactly as this end's own damage code would (cut bits from the local Link).
        dCcU_AtInfo info;
        info.mpCollider = atObj;
        info.mpActor = nullptr;
        info.mpSound = nullptr;
        info.mHitDirection.set(0, 0, 0);
        info.mHitBit = 0;
        info.field_0x18 = 0;
        info.mAttackPower = 0;
        info.mPowerType = 0;
        info.mHitStatus = 0;
        info.mHitType = 0;
        at_power_check(&info);

        std::uint8_t flags = 0;
        if (daPy_py_c::checkMasterSwordEquip()) {
            flags |= kHitMaster;
        }
        if (daPy_py_c::checkWoodSwordEquip()) {
            flags |= kHitWood;
        }
        if (daPy_py_c::checkNowWolf()) {
            flags |= kHitWolf;
        }
        if (link->getSwordAtUpTime() != 0) {
            flags |= kHitSwordUp;
        }
        if (link->getCutType() == daPy_py_c::CUT_TYPE_HEAD_JUMP) {
            flags |= kHitHeadJump;
        }
        if (link->checkCutJumpCancelTurn()) {
            flags |= kHitCutJumpCancel;
        }
        if (link->checkHorseRide()) {
            flags |= kHitHorse;
        }
        /* --- batch 10B: death and drops --- */
        if (const daAlink_c* alink = daAlink_getAlinkActorClass();
            alink != nullptr && alink->mProcID == daAlink_c::PROC_CUT_FINISH && alink->mIsRollstab)
        {
            flags |= kHitRollstab;
        }
        cXyz hitPos = *tgInf->GetTgHitPosP();

        Writer w;
        w.write_u8(static_cast<std::uint8_t>(PacketId::EnemyHit));
        w.write_u8(pid_to_wire(net.local_player_id()));
        w.write_u8(pid_to_wire(rec.authority));
        w.write_u32(rec.key);
        w.write_u16(rec.epoch);
        w.write_u8(static_cast<std::uint8_t>(i));
        w.write_u32(atObj->GetAtType());
        w.write_u8(atObj->GetAtAtp());
        w.write_u8(static_cast<std::uint8_t>(atInf->GetAtSpl()));
        w.write_u8(atInf->GetAtMtrl());
        w.write_u8(atInf->GetAtSe());
        w.write_u8(static_cast<std::uint8_t>(atInf->GetAtHitMark()));
        w.write_f32(hitPos.x);
        w.write_f32(hitPos.y);
        w.write_f32(hitPos.z);
        w.write_u8(info.mHitType);
        w.write_u32(info.mHitBit);
        w.write_u8(static_cast<std::uint8_t>(info.mHitStatus));
        w.write_u8(flags);
        w.write_u8(link->getCutType());
        w.write_u16(static_cast<std::uint16_t>(link->shape_angle.y));
        // Link's hearts live in the save data, not in fopAc_ac_c::health (always 0 for him).
        w.write_u16(dComIfGs_getLife());
        /* --- batch 10B: death and drops --- the kill drop rolls on the killer's hearts. */
        w.write_u16(dComIfGs_getMaxLife());
        send_to_authority(w, rec.authority);
        rec.hitsSent++;
        Log.info("[mp-enemy] hit sent key={:08X} n={} tg={} atType=0x{:X} atp={} spl={} "
                 "hitType={} bit=0x{:X} status={} flags=0x{:02X} cut={} by={} to={} tick={}",
            rec.key, rec.hitsSent, i, atObj->GetAtType(), atObj->GetAtAtp(),
            static_cast<int>(atInf->GetAtSpl()), info.mHitType, info.mHitBit,
            static_cast<int>(info.mHitStatus), flags, link->getCutType(),
            byLink ? "link" : "projectile", pid_str(rec.authority), net.sim_tick());

        // The attacker's hit-stop (cc_at_check's rule, d_cc_uty.cpp; the ghost skips
        // damage_check, so nothing else gives it). HP and death are the authority's: the death
        // pause uses this end's (streamed) HP as an estimate.
        int power = info.mAttackPower;
        std::int8_t status = info.mHitStatus;
        if (info.mHitType == HIT_TYPE_LINK_NORMAL_ATTACK) {
            if (!(flags & kHitWolf)) {
                if (flags & kHitMaster) {
                    power *= 2;
                }
                if (flags & kHitWood) {
                    power /= 2;
                }
            }
            if (flags & kHitSwordUp) {
                power *= 2;
                status = 1;
            }
        }
        if (power != 0 && actor->health - power <= 0) {
            status = 2;
        }
        s8 pause = status != 0 ? 5 : (power > 1 ? 2 : 0);
        if (atObj->ChkAtType(AT_TYPE_MIDNA_LOCK) || (flags & kHitHorse)) {
            pause = 0;
        }
        if ((info.mHitType == HIT_TYPE_LINK_NORMAL_ATTACK || atObj->ChkAtType(AT_TYPE_THROW_OBJ)) &&
            !(flags & kHitCutJumpCancel))
        {
            if (atObj->ChkAtType(AT_TYPE_THROW_OBJ)) {
                pause = 4;
            }
            dScnPly_c::setPauseTimer(pause);
        }
    }
}

/// The attacker's machine: bits in kForwardFlagBits that appeared on this ghost since its last
/// tick were set by this end's Link (the ghost writes mFlags from the stream every tick).
void ghost_send_flag_hit(Rec& rec, std::uint16_t added) {
    if (rec.authority == kNoPlayer) {
        return;
    }
    NetworkManager& net = network_manager();
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::EnemyFlagHit));
    w.write_u8(pid_to_wire(net.local_player_id()));
    w.write_u8(pid_to_wire(rec.authority));
    w.write_u32(rec.key);
    w.write_u16(rec.epoch);
    w.write_u16(added);
    send_to_authority(w, rec.authority);
    rec.flagHitsSent++;
    Log.info("[mp-enemy] flag hit sent key={:08X} bit=0x{:04X} n={} to={} tick={}", rec.key, added,
        rec.flagHitsSent, pid_str(rec.authority), net.sim_tick());
}

/// Common header of EnemyHit / EnemyFlagHit. Returns false on a malformed packet. `relay` is set
/// on the host for a guest's event addressed to another guest (the caller returns it).
bool read_hit_header(Reader& r, std::uint32_t fromPlayerId, const char* what,
    std::uint32_t& attacker, std::uint32_t& authority, std::uint32_t& key, std::uint16_t& epoch,
    bool& forMe, bool& relay) {
    NetworkManager& net = network_manager();
    std::uint8_t attackerWire = 0;
    std::uint8_t authorityWire = 0;
    if (!(r.read_u8(attackerWire) && r.read_u8(authorityWire) && r.read_u32(key) &&
            r.read_u16(epoch)))
    {
        Log.warn("[mp-enemy] malformed {} from player {}", what, fromPlayerId);
        return false;
    }
    attacker = pid_from_wire(attackerWire);
    authority = pid_from_wire(authorityWire);
    forMe = false;
    relay = false;
    if (net.role() == Role::Host && fromPlayerId != attacker) {
        // A guest speaks only for itself.
        Log.warn("[mp-enemy] {} key={:08X} from player {} claims attacker {}: dropped", what, key,
            fromPlayerId, pid_str(attacker));
        return true;
    }
    if (authority == net.local_player_id()) {
        forMe = true;
    } else if (net.role() == Role::Host) {
        relay = true;
    }
    return true;
}

bool on_hit(Reader& r, std::uint32_t fromPlayerId) {
    std::uint32_t attacker = kNoPlayer;
    std::uint32_t authority = kNoPlayer;
    PendingHit h;
    bool forMe = false;
    bool relay = false;
    if (!read_hit_header(r, fromPlayerId, "EnemyHit", attacker, authority, h.key, h.epoch, forMe,
            relay))
    {
        return false;
    }
    std::uint8_t status = 0;
    std::uint16_t angle = 0;
    std::uint16_t life = 0;
    std::uint16_t maxLife = 0;
    if (!(r.read_u8(h.tgIndex) && r.read_u32(h.atType) && r.read_u8(h.atp) && r.read_u8(h.atSpl) &&
            r.read_u8(h.atMtrl) && r.read_u8(h.atSe) && r.read_u8(h.atHitMark) &&
            r.read_f32(h.pos[0]) && r.read_f32(h.pos[1]) && r.read_f32(h.pos[2]) &&
            r.read_u8(h.hitType) && r.read_u32(h.hitBit) && r.read_u8(status) &&
            r.read_u8(h.flags) && r.read_u8(h.cutType) && r.read_u16(angle) && r.read_u16(life) &&
            r.read_u16(maxLife)))
    {
        Log.warn("[mp-enemy] malformed EnemyHit from player {}", fromPlayerId);
        return false;
    }
    if (!forMe) {
        return relay;
    }
    h.attacker = attacker;
    h.hitStatus = static_cast<std::int8_t>(status);
    h.angleY = static_cast<std::int16_t>(angle);
    h.life = static_cast<std::int16_t>(life);
    h.maxLife = static_cast<std::int16_t>(maxLife);
    s_pendingHits.push_back(h);
    return relay;
}

bool on_flag_hit(Reader& r, std::uint32_t fromPlayerId) {
    std::uint32_t attacker = kNoPlayer;
    std::uint32_t authority = kNoPlayer;
    PendingFlag f;
    bool forMe = false;
    bool relay = false;
    if (!read_hit_header(r, fromPlayerId, "EnemyFlagHit", attacker, authority, f.key, f.epoch,
            forMe, relay))
    {
        return false;
    }
    if (!r.read_u16(f.bit)) {
        Log.warn("[mp-enemy] malformed EnemyFlagHit from player {}", fromPlayerId);
        return false;
    }
    if (forMe) {
        f.attacker = attacker;
        s_pendingFlags.push_back(f);
    }
    return relay;
}

/// Why this end may not apply a hit on `rec` now (null: it may). `actor` is its live actor.
const char* hit_reject_reason(const Rec* rec, fopAc_ac_c* actor, std::uint16_t epoch) {
    if (rec == nullptr || actor == nullptr) {
        return "unknown key";
    }
    if (rec->mode != Mode::Authority) {
        return "not authority";
    }
    if (rec->epoch != epoch) {
        return "stale epoch";
    }
    if (actor->health <= 0 ||
        (static_cast<fopEn_enemy_c*>(actor)->mFlags & fopEn_enemy_c::fopEn_flag_Dead))
    {
        return "dead";
    }
    return nullptr;
}

/// pre_tick on the authority, before this tick's actor pass (so before this tick's damage_check,
/// §1.4): apply the queued flag bits and inject the queued hits.
void authority_apply_hits() {
    NetworkManager& net = network_manager();
    for (const PendingFlag& f : s_pendingFlags) {
        Rec* rec = find_rec_by_key(f.key);
        fopAc_ac_c* actor = rec != nullptr ? actor_of(*rec) : nullptr;
        const char* why = hit_reject_reason(rec, actor, f.epoch);
        const std::uint16_t bit = f.bit & kForwardFlagBits;
        if (why == nullptr && bit == 0) {
            why = "bit not forwardable";
        }
        if (why != nullptr) {
            Log.info("[mp-enemy] flag hit recv key={:08X} bit=0x{:04X} from={} rejected({})", f.key,
                f.bit, pid_str(f.attacker), why);
            continue;
        }
        static_cast<fopEn_enemy_c*>(actor)->mFlags |= bit;
        rec->flagHitsApplied++;
        Log.info("[mp-enemy] flag hit recv key={:08X} bit=0x{:04X} from={} applied n={} tick={}",
            f.key, bit, pid_str(f.attacker), rec->flagHitsApplied, net.sim_tick());
    }
    s_pendingFlags.clear();

    int used = 0;
    std::size_t consumed = 0;
    for (; consumed < s_pendingHits.size(); consumed++) {
        if (used >= kMaxProxies) {
            break;  // the rest next tick
        }
        const PendingHit& h = s_pendingHits[consumed];
        Rec* rec = find_rec_by_key(h.key);
        fopAc_ac_c* actor = rec != nullptr ? actor_of(*rec) : nullptr;
        if (rec != nullptr) {
            rec->hitsRecv++;
        } else {
            s_hitsUnknownKey++;
        }
        std::string reason;
        const char* why = hit_reject_reason(rec, actor, h.epoch);
        cCcD_Obj* tg = nullptr;
        fopAc_ac_c* puppet = nullptr;
        if (why == nullptr) {
            tg = rec->adapter != nullptr && rec->adapter->tgByIndex != nullptr ?
                     rec->adapter->tgByIndex(actor, h.tgIndex) :
                     nullptr;
            if (tg == nullptr) {
                why = "no such TG";
            } else if (!tg->ChkTgSet()) {
                why = "TG not set (invulnerable here)";
            }
        }
        if (why == nullptr) {
            puppet = static_cast<fopAc_ac_c*>(puppet_actor_ptr(h.attacker));
            if (puppet == nullptr) {
                why = "no puppet for attacker";
            }
        }
        if (why == nullptr) {
            float ap[3];
            if (!player_position(h.attacker, ap)) {
                why = "attacker position unknown";
            } else {
                const float dx = ap[0] - actor->current.pos.x;
                const float dy = ap[1] - actor->current.pos.y;
                const float dz = ap[2] - actor->current.pos.z;
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                const bool melee = h.hitType == HIT_TYPE_LINK_NORMAL_ATTACK ||
                                   h.hitType == HIT_TYPE_LINK_HEAVY_ATTACK;
                if (d > (melee ? kMaxMeleeHitDist : kMaxRangedHitDist)) {
                    reason = fmt::format("too far: {:.0f}", d);
                    why = reason.c_str();
                }
            }
        }
        if (why != nullptr) {
            if (rec != nullptr) {
                rec->hitsRejected++;
            }
            Log.info("[mp-enemy] hit recv key={:08X} from={} tg={} rejected({}) tick={}", h.key,
                pid_str(h.attacker), h.tgIndex, why, net.sim_tick());
            continue;
        }

        Proxy& px = proxies()[used];
        s_proxyLive[used] = false;
        px.stts.Init(0xFF, 0xFF, puppet);
        static const dCcD_SrcSph kProxySrc = {
            {
                {0, {{0, 0, 0x1}, {0, 0}, 0}},
                {dCcD_SE_NONE, 0, 0, 0, {0}},
                {dCcD_SE_NONE, 0, 0, 0, {0}},
                {0},
            },
            {{
                {0.0f, 0.0f, 0.0f},
                10.0f,
            }},
        };
        px.sph.Set(kProxySrc);
        px.sph.SetStts(&px.stts);
        px.sph.SetAtType(h.atType);
        px.sph.SetAtAtp(h.atp);
        px.sph.SetAtSpl(static_cast<dCcG_At_Spl>(h.atSpl));
        px.sph.SetAtMtrl(h.atMtrl);
        px.sph.SetAtSe(h.atSe);
        px.sph.SetAtHitMark(h.atHitMark);
        cXyz hitPos(h.pos[0], h.pos[1], h.pos[2]);
        px.sph.SetC(hitPos);
        px.sph.SetR(10.0f);
        px.ov.hitType = h.hitType;
        px.ov.hitBit = h.hitBit;
        px.ov.hitStatus = h.hitStatus;
        px.ov.masterSword = (h.flags & kHitMaster) != 0;
        px.ov.woodSword = (h.flags & kHitWood) != 0;
        px.ov.wolf = (h.flags & kHitWolf) != 0;
        px.ov.swordUp = (h.flags & kHitSwordUp) != 0;
        px.ov.headJump = (h.flags & kHitHeadJump) != 0;
        px.ov.cutJumpCancelTurn = (h.flags & kHitCutJumpCancel) != 0;
        px.ov.horseRide = (h.flags & kHitHorse) != 0;
        px.ov.shapeAngleY = h.angleY;
        /* --- batch 10B: death and drops --- */
        px.ov.attacker = h.attacker;
        px.ov.life = h.life;
        px.ov.maxLife = h.maxLife;
        px.ov.rollstab = (h.flags & kHitRollstab) != 0;
        s_proxyLive[used] = true;
        used++;

        dComIfG_Ccsp()->SetAtTgCommonHitInf(&px.sph, tg, &hitPos);
        if (!dCcD_GetGObjInf(tg)->ChkTgHit()) {
            rec->hitsRejected++;
            Log.info("[mp-enemy] hit recv key={:08X} from={} tg={} rejected(collision refused it: "
                     "no-con-hit / shield) tick={}",
                h.key, pid_str(h.attacker), h.tgIndex, net.sim_tick());
            continue;
        }
        rec->hitsInjected++;
        /* --- batch 10B: death and drops --- the killer, if this enemy dies some other way. */
        rec->lastAttacker = h.attacker;
        rec->lastAttackerLife = h.life;
        rec->lastAttackerMaxLife = h.maxLife;
        if (!rec->injectedThisTick) {
            rec->injectedThisTick = true;
            rec->hpBeforeInject = actor->health;
        }
        Log.info("[mp-enemy] hit recv key={:08X} from={} tg={} injected n={} atType=0x{:X} atp={} "
                 "hitType={} bit=0x{:X} flags=0x{:02X} life={} hp={} tick={}",
            h.key, pid_str(h.attacker), h.tgIndex, rec->hitsInjected, h.atType, h.atp, h.hitType,
            h.hitBit, h.flags, h.life, actor->health, net.sim_tick());
    }
    s_pendingHits.erase(s_pendingHits.begin(), s_pendingHits.begin() + consumed);
}

/// post_tick on the authority: what the enemy's own damage_check made of this tick's injections.
void authority_log_hit_results() {
    for (auto& [proc, rec] : S().recs) {
        if (!rec.injectedThisTick) {
            continue;
        }
        rec.injectedThisTick = false;
        fopAc_ac_c* actor = actor_of(rec);
        if (actor == nullptr) {
            continue;
        }
        if (actor->health != rec.hpBeforeInject) {
            Log.info("[mp-enemy] hit applied key={:08X} hp {} -> {} tick={}", rec.key,
                rec.hpBeforeInject, actor->health, network_manager().sim_tick());
        } else {
            Log.info("[mp-enemy] hit applied key={:08X} hp {} unchanged (its damage_check ignored "
                     "it: invulnerable, guarding or not reading damage now) tick={}",
                rec.key, rec.hpBeforeInject, network_manager().sim_tick());
        }
    }
}

void hits_reset() {
    s_pendingHits.clear();
    s_pendingFlags.clear();
    for (int i = 0; i < kMaxProxies; i++) {
        s_proxyLive[i] = false;
    }
}

/* --- end batch 9 --- */

/* =============================================================================================
 * --- batch 10B: death and drops (M6 phase 3, mp-m6-enemies.md §5.4) ---
 * The authority's enemy dies through its own code: death action -> fopAcM_createDisappear ->
 * fopAcM_delete. The disappear actor is fast-created inside that execute (EnemyTargetScope is
 * live), so its drop roll runs there too: on the KILLER's hearts (enemy_roll_life), and the item it
 * makes is shared as ONE batch 7 drop (enemy_drop_shared; first come through ClaimKind::Drop). The
 * deletion sends EnemyDespawn with the disappear args; the ghost replays the disappear with enemy
 * id 0xFF (the smoke, no roll, d_a_disappear.cpp) inside its own gate tick and deletes itself. The
 * killer's end signals the kill achievement (the authority does not for an injected hit).
 * =========================================================================================== */

constexpr std::uint8_t kDespawnKilled = 1;
constexpr std::uint8_t kDespawnHaveDisappear = 0x01;
constexpr std::uint8_t kDespawnRollstab = 0x02;
/// A despawned ghost no gate tick has removed after this long is deleted from pre_tick, without
/// the smoke (outside its own execute there is no process layer of its own to create it in).
constexpr std::uint64_t kDespawnFallbackTicks = 30;

std::uint32_t s_despawnsSent = 0;
std::uint32_t s_despawnsRecv = 0;

/// Who killed an authority enemy: the killing blow's dealer, else the last remote attacker whose
/// hit was injected (a death that did not come through cc_at_check), else this end's own Link.
/// `known` (with `life`/`maxLife`) only for a remote killer whose hearts came in his EnemyHit.
std::uint32_t resolved_killer(
    const Rec& rec, std::int16_t& life, std::int16_t& maxLife, bool& known) {
    const std::uint32_t me = network_manager().local_player_id();
    known = false;
    if (rec.killer != kNoPlayer) {
        if (rec.killer != me) {
            life = rec.killerLife;
            maxLife = rec.killerMaxLife;
            known = true;
        }
        return rec.killer;
    }
    if (rec.lastAttacker != kNoPlayer && rec.lastAttacker != me) {
        life = rec.lastAttackerLife;
        maxLife = rec.lastAttackerMaxLife;
        known = true;
        return rec.lastAttacker;
    }
    return me;
}

/// The record of the synced enemy whose execute is running now, if this end is its authority.
Rec* exec_authority_rec() {
    if (!s_enabled || s_execProc == kNoProc) {
        return nullptr;
    }
    Rec* rec = find_rec(s_execProc);
    return rec != nullptr && rec->mode == Mode::Authority ? rec : nullptr;
}

/// post_tick on the authority: remember a death while the actor can still be read (fopAc_Delete
/// runs after the subclass delete, so enemy_note_deleted must not read it).
void authority_note_dead() {
    for (auto& [proc, rec] : S().recs) {
        if (rec.mode != Mode::Authority || rec.deadSeen) {
            continue;
        }
        fopAc_ac_c* actor = actor_of(rec);
        if (actor == nullptr) {
            continue;
        }
        const bool deadFlag =
            (static_cast<fopEn_enemy_c*>(actor)->mFlags & fopEn_enemy_c::fopEn_flag_Dead) != 0;
        if (actor->health > 0 && !deadFlag) {
            rec.aliveSeen = true;
            continue;
        }
        // HP 0 counts only after it was ever above 0 here: an enemy that never uses HP is not dead.
        if (deadFlag || rec.aliveSeen) {
            rec.deadSeen = true;
            std::int16_t life = 0;
            std::int16_t maxLife = 0;
            bool known = false;
            Log.info("[mp-enemy] dead here key={:08X} hp={} deadFlag={} killer={} tick={}", rec.key,
                actor->health, deadFlag ? 1 : 0,
                pid_str(resolved_killer(rec, life, maxLife, known)), network_manager().sim_tick());
        }
    }
}

/// The authority, as its dead enemy is deleted: tell everyone else.
void send_despawn(const Rec& rec) {
    NetworkManager& net = network_manager();
    std::int16_t life = 0;
    std::int16_t maxLife = 0;
    bool known = false;
    const std::uint32_t killer = resolved_killer(rec, life, maxLife, known);
    std::uint8_t dflags = 0;
    if (rec.haveDisappear) {
        dflags |= kDespawnHaveDisappear;
    }
    if (rec.killer != kNoPlayer && rec.killerRollstab) {
        dflags |= kDespawnRollstab;
    }
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::EnemyDespawn));
    w.write_u8(pid_to_wire(net.local_player_id()));
    w.write_u32(rec.stageHash);
    w.write_u8(static_cast<std::uint8_t>(rec.layer));
    w.write_u8(static_cast<std::uint8_t>(rec.room));
    w.write_u32(rec.key);
    w.write_u16(rec.epoch);
    w.write_u8(kDespawnKilled);
    w.write_u8(pid_to_wire(killer));
    w.write_u8(dflags);
    w.write_u8(rec.disappearSize);
    w.write_u8(rec.disappearType);
    w.write_u8(rec.disappearEnemyId);
    w.write_f32(rec.disappearPos[0]);
    w.write_f32(rec.disappearPos[1]);
    w.write_f32(rec.disappearPos[2]);
    if (net.role() == Role::Host) {
        net.send_to_session(w, kChannelObjects, true);
    } else {
        net.send_to_player(kHostPlayerId, w, kChannelObjects, true);
    }
    s_despawnsSent++;
    Log.info("[mp-enemy] despawn sent key={:08X} n={} killer={} rollstab={} disappear={} size={} "
             "type={} enemyId=0x{:02X} epoch={} tick={}",
        rec.key, s_despawnsSent, pid_str(killer), (dflags & kDespawnRollstab) ? 1 : 0,
        rec.haveDisappear ? 1 : 0, rec.disappearSize, rec.disappearType, rec.disappearEnemyId,
        rec.epoch, net.sim_tick());
}

bool on_despawn(Reader& r, std::uint32_t fromPlayerId) {
    State& s = S();
    NetworkManager& net = network_manager();
    std::uint8_t authorityWire = 0;
    std::uint32_t stageHash = 0;
    std::uint8_t layer = 0;
    std::uint8_t room = 0;
    std::uint32_t key = 0;
    std::uint16_t epoch = 0;
    std::uint8_t reason = 0;
    std::uint8_t killerWire = 0;
    std::uint8_t dflags = 0;
    EnemyDespawn d;
    if (!(r.read_u8(authorityWire) && r.read_u32(stageHash) && r.read_u8(layer) &&
            r.read_u8(room) && r.read_u32(key) && r.read_u16(epoch) && r.read_u8(reason) &&
            r.read_u8(killerWire) && r.read_u8(dflags) && r.read_u8(d.disappearSize) &&
            r.read_u8(d.disappearType) && r.read_u8(d.disappearEnemyId) && r.read_f32(d.pos[0]) &&
            r.read_f32(d.pos[1]) && r.read_f32(d.pos[2])))
    {
        Log.warn("[mp-enemy] malformed EnemyDespawn from player {}", fromPlayerId);
        return false;
    }
    const std::uint32_t authority = pid_from_wire(authorityWire);
    const std::uint32_t killer = pid_from_wire(killerWire);
    const std::uint64_t k =
        room_key(stageHash, static_cast<std::int8_t>(layer), static_cast<std::int8_t>(room));
    const auto t = s.table.find(k);
    const bool ownerMatches = t != s.table.end() && t->second.owner == authority;
    if (net.role() == Role::Host && (authority != fromPlayerId || !ownerMatches)) {
        // As EnemyUpdate: a guest speaks only for rooms the host's table gives it.
        Log.warn("[mp-enemy] dropping EnemyDespawn key={:08X} from player {}: authority {} room {} "
                 "owner {}",
            key, fromPlayerId, pid_str(authority), static_cast<int>(static_cast<std::int8_t>(room)),
            t != s.table.end() ? pid_str(t->second.owner) : std::string("none"));
        return false;
    }
    s_despawnsRecv++;
    d.reason = reason;
    d.killerPeer = killerWire;
    d.haveDisappear = (dflags & kDespawnHaveDisappear) != 0;
    d.rollstab = (dflags & kDespawnRollstab) != 0;
    const std::uint32_t me = net.local_player_id();
    Log.info("[mp-enemy] despawn recv key={:08X} n={} from={} killer={}{} disappear={} size={} "
             "type={} epoch={} tick={}",
        key, s_despawnsRecv, pid_str(authority), pid_str(killer), killer == me ? " (me)" : "",
        d.haveDisappear ? 1 : 0, d.disappearSize, d.disappearType, epoch, net.sim_tick());

    // §5.4 item 7: the kill is the killer's. The authority did not signal it (cc_at_check skips an
    // injected hit), so the killer's own end does, here.
    if (killer == me && authority != me) {
        dusk::AchievementSystem::get().signal("enemy_killed");
        if (d.rollstab) {
            dusk::AchievementSystem::get().signal("rollstab_kill");
        }
        Log.info("[mp-enemy] kill credited here key={:08X} rollstab={} tick={}", key,
            d.rollstab ? 1 : 0, net.sim_tick());
    }

    Rec* rec = find_rec_by_key(key);
    if (rec == nullptr) {
        Log.info("[mp-enemy] despawn key={:08X}: no copy here (room not loaded)", key);
    } else if (rec->mode != Mode::Ghost) {
        Log.warn("[mp-enemy] despawn key={:08X} ignored: this end runs it as {} (epoch {} vs {})",
            key, mode_name(rec->mode), rec->epoch, epoch);
    } else if (!rec->despawnPending && !rec->despawnApplied) {
        rec->despawnPending = true;
        rec->despawnAt = s.tick;
        rec->despawn = d;
    }
    return net.role() == Role::Host;
}

/// The ghost's end: play the authority's disappear (with enemy id 0xFF: the smoke, and no drop
/// roll) and delete the ghost. `withSmoke` false outside the ghost's own gate tick.
void apply_despawn(Rec& rec, fopAc_ac_c* actor, bool withSmoke) {
    rec.despawnPending = false;
    rec.despawnApplied = true;
    if (rec.adapter != nullptr && rec.adapter->onGhostDeath != nullptr) {
        rec.adapter->onGhostDeath(actor, rec.despawn);
    }
    const bool smoke = withSmoke && rec.despawn.haveDisappear;
    if (smoke) {
        cXyz pos(rec.despawn.pos[0], rec.despawn.pos[1], rec.despawn.pos[2]);
        fopAcM_createDisappear(
            actor, &pos, rec.despawn.disappearSize, rec.despawn.disappearType, 0xFF);
    }
    fopAcM_delete(actor);
    Log.info("[mp-enemy] despawn applied key={:08X} smoke={} after {} tick(s) tick={}", rec.key,
        smoke ? 1 : 0, S().tick - rec.despawnAt, network_manager().sim_tick());
}

/// pre_tick: a despawn no gate tick has applied in time (the ghost is not executing).
void despawn_fallback() {
    State& s = S();
    for (auto& [proc, rec] : s.recs) {
        if (!rec.despawnPending || s.tick - rec.despawnAt <= kDespawnFallbackTicks) {
            continue;
        }
        fopAc_ac_c* actor = actor_of(rec);
        if (actor == nullptr) {
            continue;
        }
        Log.warn("[mp-enemy] despawn key={:08X}: no ghost tick in {} ticks, deleting it from "
                 "pre_tick",
            rec.key, kDespawnFallbackTicks);
        apply_despawn(rec, actor, false);
    }
}

/* --- end batch 10B --- */

void write_trace_rows() {
    if (!trace::enabled()) {
        return;
    }
    static const char* kHeader =
        "key,prof,proc,mode,degraded,authority,epoch,target,x,y,z,shapeY,hp,anm,frame,mflags,"
        "engaged,tx,ty,tz,tdist,samples,ghostTicks,realExecWhileGhost,swapTicks,absorbed,"
        "realLinkWrites,targetSwitches,hitsSent,hitsRecv,hitsInjected,hitsRejected"
        /* --- batch 10A: ghost AT --- */ ",atHitsLocal,atShieldedLocal";
    const std::uint64_t tick = network_manager().sim_tick();
    for (auto& [proc, rec] : S().recs) {
        if (rec.mode == Mode::Local && !rec.degraded) {
            continue;
        }
        fopAc_ac_c* actor = actor_of(rec);
        if (actor == nullptr) {
            continue;
        }
        int anm = -1;
        float frame = 0.0f;
        mDoExt_McaMorfSO* morf = rec.adapter->morf != nullptr ? rec.adapter->morf(actor) : nullptr;
        if (morf != nullptr) {
            anm = anm_index(rec.adapter->arc, morf->getAnm());
            frame = morf->getFrame();
        }
        bool engaged = false;
        std::uint32_t target = kNoPlayer;
        if (rec.mode == Mode::Authority) {
            engaged = rec.adapter->engaged != nullptr && rec.adapter->engaged(actor);
            target = rec.target;
        } else if (rec.mode == Mode::Ghost && rec.havePlayed) {
            engaged = rec.played.engaged;
            target = pid_from_wire(rec.played.targetPeer);
        }
        float tp[3] = {0.0f, 0.0f, 0.0f};
        float tdist = -1.0f;
        if (target != kNoPlayer && player_position(target, tp)) {
            const float dx = tp[0] - actor->current.pos.x;
            const float dy = tp[1] - actor->current.pos.y;
            const float dz = tp[2] - actor->current.pos.z;
            tdist = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        const std::uint32_t realExec =
            rec.gateAsGhost > rec.ghostTicks ? rec.gateAsGhost - rec.ghostTicks : 0;
        trace::enemy_row(tick, kHeader,
            fmt::format("{:08X},0x{:03X},{},{},{},{},{},{},{:.2f},{:.2f},{:.2f},{},{},{},{:.2f},{},"
                        "{},{:.2f},{:.2f},{:.2f},{:.2f},{},{},{},{},{},{},{},{},{},{},{}"
                        /* --- batch 10A: ghost AT --- */ ",{},{}",
                rec.key, static_cast<unsigned>(static_cast<std::uint16_t>(rec.prof)), rec.proc,
                mode_name(rec.mode), rec.degraded ? 1 : 0, pid_str(rec.authority), rec.epoch,
                pid_str(target), actor->current.pos.x, actor->current.pos.y, actor->current.pos.z,
                actor->shape_angle.y, actor->health, anm, frame,
                static_cast<fopEn_enemy_c*>(actor)->mFlags, engaged ? 1 : 0, tp[0], tp[1], tp[2],
                tdist, rec.samples.size(), rec.ghostTicks, realExec, rec.swapTicks,
                rec.absorbedWrites, rec.realLinkWrites, rec.targetSwitches, rec.hitsSent,
                rec.hitsRecv, rec.hitsInjected, rec.hitsRejected,
                /* --- batch 10A: ghost AT --- */ rec.atHitsLocal, rec.atShieldedLocal));
    }
}

void log_stats() {
    State& s = S();
    if (s.tick % kStatsEvery != 0) {
        return;
    }
    for (auto& [proc, rec] : s.recs) {
        if (rec.mode == Mode::Local && !rec.degraded) {
            continue;
        }
        fopAc_ac_c* actor = actor_of(rec);
        if (actor == nullptr) {
            continue;
        }
        const std::uint32_t target = rec.mode == Mode::Authority ? rec.target :
                                     rec.havePlayed ? pid_from_wire(rec.played.targetPeer) :
                                                      kNoPlayer;
        const std::uint32_t realExec =
            rec.gateAsGhost > rec.ghostTicks ? rec.gateAsGhost - rec.ghostTicks : 0;
        Log.info(
            "[mp-enemy] stats key={:08X} mode={} owner={} epoch={} target={} hp={} pos=({:.0f}, "
            "{:.0f}, {:.0f}) ghostTicks={} realExecWhileGhost={} swaps={} absorbed={} "
            "realLinkWrites={} switches={} hitsSent={} hitsRecv={} hitsInjected={} "
            "hitsRejected={} tick={} atHitsLocal={} atShieldedLocal={}",
            rec.key, mode_name(rec.mode), pid_str(rec.authority), rec.epoch, pid_str(target),
            actor->health, actor->current.pos.x, actor->current.pos.y, actor->current.pos.z,
            rec.ghostTicks, realExec, rec.swapTicks, rec.absorbedWrites, rec.realLinkWrites,
            rec.targetSwitches, rec.hitsSent, rec.hitsRecv, rec.hitsInjected, rec.hitsRejected,
            network_manager().sim_tick(),
            /* --- batch 10A: ghost AT --- */ rec.atHitsLocal, rec.atShieldedLocal);
    }
}

void reset_session_state() {
    State& s = S();
    for (auto& [proc, rec] : s.recs) {
        if (rec.mode != Mode::Local) {
            Log.info("[mp-enemy] mode key={:08X} {} -> local (sync ended)", rec.key,
                mode_name(rec.mode));
        }
        rec.mode = Mode::Local;
        rec.authority = kNoPlayer;
        rec.epoch = 0;
        rec.samples.clear();
        rec.havePlayed = false;
        rec.target = kNoPlayer;
        rec.challenger = kNoPlayer;
        rec.challengerTicks = 0;
        rec.ghostFlagsValid = false;
        rec.injectedThisTick = false;
        /* --- batch 10B: death and drops --- */
        rec.haveDisappear = false;
        rec.killer = kNoPlayer;
        rec.lastAttacker = kNoPlayer;
        rec.despawnPending = false;
    }
    hits_reset();
    s.lastSentRooms = RoomSet{};
    s.haveSentRooms = false;
    s.forceRooms = false;
    s.pendingDegrade.clear();
    s.peerRooms.clear();
    s.tableSentTo.clear();
    s.table.clear();
    s.epochs.clear();
    s.hostArrivedAt.clear();
    s_enabled = false;
    standins_reset();
}

}  // namespace

Rec* find_rec(std::uint32_t procId) {
    State& s = S();
    const auto it = s.recs.find(procId);
    return it != s.recs.end() ? &it->second : nullptr;
}

std::uint64_t local_tick() {
    return S().tick;
}

bool local_room_loaded(int room) {
    if (room < 0) {
        return local_in_stage();
    }
    if (room >= 64) {
        return false;
    }
    // Flag 1: the room scene exists; 4: it is being torn down (dStage_roomControl_c::loadRoom,
    // d_stage.cpp:307-368).
    return dComIfGp_roomControl_checkStatusFlag(room, 0x01) != 0 &&
           dComIfGp_roomControl_checkStatusFlag(room, 0x04) == 0;
}

}  // namespace dusk::mp::enemies::detail

/* =============================================================================================
 * enemy_sync.hpp
 * =========================================================================================== */

namespace dusk::mp::enemies {

using namespace detail;

const char* mode_name(Mode mode) {
    switch (mode) {
    case Mode::Local:
        return "local";
    case Mode::Authority:
        return "authority";
    case Mode::Ghost:
        return "ghost";
    }
    return "?";
}

void pre_tick() {
    State& s = S();
    NetworkManager& net = network_manager();
    const bool want = net.session_live() && dusk::getSettings().multiplayer.enemySync.getValue();
    if (!want) {
        if (s_enabled) {
            Log.info("[mp-enemy] enemy sync off (cvar or session): every enemy back to local");
            reset_session_state();
        }
        return;
    }
    if (!s_enabled) {
        s_enabled = true;
        std::string list;
        for (int i = 0; i < s_adapterCount; i++) {
            list += fmt::format(" 0x{:03X}({})",
                static_cast<unsigned>(static_cast<std::uint16_t>(s_adapterSlots[i].prof)),
                s_adapterSlots[i].adapter != nullptr && s_adapterSlots[i].adapter->arc != nullptr ?
                    s_adapterSlots[i].adapter->arc :
                    "?");
        }
        Log.info(
            "[mp-enemy] enemy sync on as {} (player {}); {} adapter(s):{}; {} enemy(ies) alive",
            net.role() == Role::Host ? "host" : "guest", net.local_player_id(), s_adapterCount,
            list, s.recs.size());
    }
    ++s.tick;
    check_slot0("pre_tick");
    send_rooms_if_changed();
    if (net.role() == Role::Host) {
        host_recompute_table();
    }
    update_modes();
    for (auto& [proc, rec] : s.recs) {
        if (rec.mode != Mode::Ghost) {
            continue;
        }
        if (!rec.everSampled && s.tick - rec.ghostSince > kNoSampleDegradeTicks) {
            degrade(rec,
                fmt::format(
                    "no state from player {} within {} ticks", rec.authority, kNoSampleDegradeTicks)
                    .c_str(),
                true);
            continue;
        }
        play_ghost(rec);
    }
    standins_refresh();
    /* --- batch 9: enemy hits --- before this tick's actor pass, so before its damage_check. */
    authority_apply_hits();
    /* --- batch 10B: death and drops --- */
    despawn_fallback();
}

void post_tick() {
    if (!s_enabled) {
        return;
    }
    check_slot0("post_tick");
    /* --- batch 9: enemy hits --- */
    authority_log_hit_results();
    /* --- batch 10B: death and drops --- */
    authority_note_dead();
    send_updates();
    write_trace_rows();
    log_stats();
}

bool on_packet(PacketId id, Reader& r, std::uint32_t fromPlayerId) {
    if (!s_enabled) {
        // A peer with the cvar on talking to one with it off: nothing here acts on it, and the host
        // does not relay what it does not understand to act on.
        return false;
    }
    switch (id) {
    case PacketId::EnemyAuthority:
        return on_authority(r);
    case PacketId::EnemyUpdate:
        return on_update(r, fromPlayerId);
    case PacketId::RoomsLoaded:
        return on_rooms(r, fromPlayerId);
    /* --- batch 9: enemy hits --- */
    case PacketId::EnemyHit:
        return on_hit(r, fromPlayerId);
    case PacketId::EnemyFlagHit:
        return on_flag_hit(r, fromPlayerId);
    /* --- batch 10B: death and drops --- */
    case PacketId::EnemyDespawn:
        return on_despawn(r, fromPlayerId);
    default:
        return false;
    }
}

void on_peer_joined(std::uint32_t playerId) {
    S().forceRooms = true;
    Log.info("[mp-enemy] player {} joined: resending my rooms", playerId);
}

void on_peer_left(std::uint32_t playerId) {
    State& s = S();
    s.peerRooms.erase(playerId);
    s.tableSentTo.erase(playerId);
    standins_forget(playerId);
    Log.info("[mp-enemy] player {} left: rooms and stand-in forgotten", playerId);
}

void reset() {
    reset_session_state();
}

std::vector<EnemyView> views() {
    std::vector<EnemyView> out;
    for (auto& [proc, rec] : S().recs) {
        fopAc_ac_c* actor = actor_of(rec);
        if (actor == nullptr) {
            continue;
        }
        EnemyView v;
        v.key = rec.key;
        v.procId = rec.proc;
        v.prof = rec.prof;
        v.mode = rec.mode;
        v.degraded = rec.degraded;
        v.authority = rec.authority;
        v.epoch = rec.epoch;
        if (rec.mode == Mode::Authority) {
            v.target = rec.target;
        } else if (rec.mode == Mode::Ghost && rec.havePlayed) {
            v.target = pid_from_wire(rec.played.targetPeer);
        }
        v.pos[0] = actor->current.pos.x;
        v.pos[1] = actor->current.pos.y;
        v.pos[2] = actor->current.pos.z;
        v.health = actor->health;
        mDoExt_McaMorfSO* morf = rec.adapter->morf != nullptr ? rec.adapter->morf(actor) : nullptr;
        if (morf != nullptr) {
            v.anm = anm_index(rec.adapter->arc, morf->getAnm());
            v.frame = morf->getFrame();
        }
        v.ghostTicks = rec.ghostTicks;
        v.realExecWhileGhost =
            rec.gateAsGhost > rec.ghostTicks ? rec.gateAsGhost - rec.ghostTicks : 0;
        v.swapTicks = rec.swapTicks;
        v.absorbedWrites = rec.absorbedWrites;
        v.realLinkWrites = rec.realLinkWrites;
        v.hitsSent = rec.hitsSent;
        v.hitsRecv = rec.hitsRecv;
        v.hitsInjected = rec.hitsInjected;
        v.hitsRejected = rec.hitsRejected;
        /* --- batch 10A: ghost AT --- */
        v.atHitsLocal = rec.atHitsLocal;
        v.atShieldedLocal = rec.atShieldedLocal;
        /* --- end batch 10A --- */
        out.push_back(v);
    }
    return out;
}

std::vector<RoomOwnerView> authority_table() {
    std::vector<RoomOwnerView> out;
    for (const auto& [k, o] : S().table) {
        RoomOwnerView v;
        v.stageHash = key_stage(k);
        v.layer = static_cast<std::int8_t>(key_layer(k));
        v.room = static_cast<std::int8_t>(key_room(k));
        v.owner = o.owner;
        v.epoch = o.epoch;
        out.push_back(v);
    }
    return out;
}

}  // namespace dusk::mp::enemies

/* =============================================================================================
 * dusk/mp_enemy.hpp (all but EnemyTargetScope, which is in enemy_target.cpp)
 * =========================================================================================== */

namespace dusk::mp {

void enemy_register_adapter(short profName, const EnemyNetAdapter* adapter) {
    // ★ May run before main(): touches only the zero-initialised table, never logs.
    using namespace enemies::detail;
    for (int i = 0; i < s_adapterCount; i++) {
        if (s_adapterSlots[i].prof == profName) {
            s_adapterSlots[i].adapter = adapter;
            return;
        }
    }
    if (s_adapterCount < kMaxAdapters) {
        s_adapterSlots[s_adapterCount].prof = profName;
        s_adapterSlots[s_adapterCount].adapter = adapter;
        s_adapterCount++;
    }
}

unsigned int enemy_placement_key(int roomNo, unsigned int chunkTag, int entryIndex) {
    enemies::detail::Fnv f;
    const char* stage = dComIfGp_getStartStageName();
    for (int i = 0; stage != nullptr && i < 8 && stage[i] != '\0'; i++) {
        f.byte(static_cast<std::uint8_t>(stage[i]));
    }
    f.byte(0xFF);
    f.byte(static_cast<std::uint8_t>(enemies::detail::current_layer()));
    f.byte(static_cast<std::uint8_t>(static_cast<std::int8_t>(roomNo)));
    f.u32(chunkTag);
    f.u32(static_cast<std::uint32_t>(entryIndex));
    return f.done();
}

void enemy_note_created(fopAc_ac_c* actor, unsigned int placementKey) {
    using namespace enemies::detail;
    if (actor == nullptr || placementKey == 0) {
        return;
    }
    const short prof = fopAcM_GetProfName(actor);
    const EnemyNetAdapter* adapter = find_adapter(prof);
    if (adapter == nullptr) {
        return;
    }
    State& s = S();
    Rec rec;
    rec.proc = fopAcM_GetID(actor);
    rec.prof = prof;
    rec.adapter = adapter;
    rec.key = placementKey;
    rec.check = check_hash(actor);
    rec.stageHash = current_stage_hash();
    rec.room = fopAcM_GetRoomNo(actor);
    rec.layer = current_layer();
    const auto clash = s.byKey.find(placementKey);
    if (clash != s.byKey.end() && clash->second != rec.proc) {
        if (Rec* other = find_rec(clash->second)) {
            // Two live actors from one stage entry (a room reloaded before the old copy was
            // deleted?): neither can be told apart on the wire.
            degrade(*other, "placement key shared with another live actor", false);
            rec.degraded = true;
        }
    }
    const std::string line = fmt::format(
        "[mp-enemy] registered key={:08X} prof=0x{:03X} proc={} stage={:08X} layer={} room={} "
        "check={:08X} home=({:.0f}, {:.0f}, {:.0f}) params={:08X}{}",
        rec.key, static_cast<unsigned>(static_cast<std::uint16_t>(prof)), rec.proc, rec.stageHash,
        static_cast<int>(rec.layer), static_cast<int>(rec.room), rec.check, actor->home.pos.x,
        actor->home.pos.y, actor->home.pos.z, fopAcM_GetParam(actor),
        rec.degraded ? " (degraded: duplicate key)" : "");
    // Recorded in single player too (a session may start mid-stage), but only loud while syncing.
    if (s_enabled) {
        Log.info("{}", line);
    } else {
        Log.debug("{}", line);
    }
    s.byKey[placementKey] = rec.proc;
    s.recs[rec.proc] = std::move(rec);
}

void enemy_note_deleted(fopAc_ac_c* actor) {
    using namespace enemies::detail;
    if (actor == nullptr) {
        return;
    }
    State& s = S();
    const auto it = s.recs.find(fopAcM_GetID(actor));
    if (it == s.recs.end()) {
        return;
    }
    const Rec& rec = it->second;
    if (s_enabled) {
        Log.info("[mp-enemy] deleted key={:08X} proc={} mode={} ghostTicks={} swaps={}", rec.key,
            rec.proc, enemies::mode_name(rec.mode), rec.ghostTicks, rec.swapTicks);
        /* --- batch 10B: death and drops --- a dead authority enemy's deletion is its despawn. A
         * live one going (room unload, stage change) is not: the others keep their own copy. */
        if (rec.mode == enemies::Mode::Authority && (rec.haveDisappear || rec.deadSeen)) {
            send_despawn(rec);
        }
    }
    const auto k = s.byKey.find(rec.key);
    if (k != s.byKey.end() && k->second == rec.proc) {
        s.byKey.erase(k);
    }
    s.recs.erase(it);
}

bool enemy_ghost_execute(fopAc_ac_c* actor) {
    using namespace enemies::detail;
    // ★ Runs for every actor that passes the execute gate, every tick: bail before any map lookup.
    if (!s_enabled || actor == nullptr) {
        return false;
    }
    const EnemyNetAdapter* adapter = find_adapter(fopAcM_GetProfName(actor));
    if (adapter == nullptr) {
        return false;
    }
    Rec* rec = find_rec(fopAcM_GetID(actor));
    if (rec == nullptr || rec->mode != enemies::Mode::Ghost) {
        return false;
    }

    /* --- batch 10B: death and drops --- inside its own gate tick, so the smoke is created where
     * the authority's was (this actor's room and process layer). */
    if (rec->despawnApplied) {
        return true;
    }
    if (rec->despawnPending) {
        apply_despawn(*rec, actor, true);
        rec->ghostTicks++;
        return true;
    }

    if (!rec->havePlayed) {
        // No state yet (just became a ghost): hold where create put it, but still pose it, so its
        // model is never drawn with uncomputed joints (R1: present must survive a fresh enemy).
        if (adapter->present != nullptr) {
            adapter->present(actor);
        }
        rec->ghostTicks++;
        return true;
    }

    /* --- batch 9: enemy hits --- what this end's Link did to the ghost since its last tick: the
     * TG hits of the last collision pass, and flag bits set directly (the ghost rewrites mFlags
     * from the stream below, so a new bit there is the local player's). */
    ghost_scan_hits(*rec, actor);
    if (rec->ghostFlagsValid) {
        const std::uint16_t added = static_cast<fopEn_enemy_c*>(actor)->mFlags &
                                    static_cast<std::uint16_t>(~rec->ghostFlagsWritten) &
                                    kForwardFlagBits;
        if (added != 0) {
            ghost_send_flag_hit(*rec, added);
        }
    }

    /* --- batch 10A: ghost AT --- spec 5.3: the ghost's AT spheres (set from the authority's bits
     * by applyExtra below, placed by present) hit this end's Link in the collision pass. Read the
     * last pass's result before applyExtra/cc_set re-arm them; ChkAtTg clears it every pass. */
    if (adapter->ghostAtHitsLocal != nullptr) {
        int shielded = 0;
        const int hits = adapter->ghostAtHitsLocal(actor, &shielded);
        if (hits > 0) {
            rec->atHitsLocal += static_cast<std::uint32_t>(hits);
            rec->atShieldedLocal += static_cast<std::uint32_t>(shielded > 0 ? shielded : 0);
            Log.info("[mp-enemy] ghost-at key={:08X} owner={} hit local Link colliders={} "
                     "shielded={} life={} total={} tick={}",
                rec->key, pid_str(rec->authority), hits, shielded, dComIfGs_getLife(),
                rec->atHitsLocal, network_manager().sim_tick());
        }
    }
    /* --- end batch 10A --- */

    const Sample& s = rec->played;
    // §2.2 one ghost tick. 1: pose.
    actor->current.pos.set(s.pos[0], s.pos[1], s.pos[2]);
    actor->shape_angle.set(s.shape[0], s.shape[1], s.shape[2]);
    actor->current.angle.y = s.angleY;
    actor->speedF = 0.0f;
    // 2: HP and the enemy flags. WolfNoLock stays set on a ghost: a wolf grab on it would wait
    // for an enemy half that never runs here (§5.5 v1 rule).
    actor->health = s.health;
    static_cast<fopEn_enemy_c*>(actor)->mFlags =
        static_cast<u16>(s.flags | fopEn_enemy_c::fopEn_flag_WolfNoLock);
    rec->ghostFlagsWritten = static_cast<fopEn_enemy_c*>(actor)->mFlags;
    rec->ghostFlagsValid = true;
    // 3-5: the animation. Set on a change (the authority's own morph), played here at the sent
    // rate so its BAS sounds play locally, then corrected to the authority's frame when off by
    // more than one frame (the puppet lesson: frames are set, not lerped).
    mDoExt_McaMorfSO* morf = adapter->morf != nullptr ? adapter->morf(actor) : nullptr;
    if (morf != nullptr) {
        J3DAnmTransform* res = nullptr;
        if (s.anmValid && adapter->arc != nullptr) {
            res = static_cast<J3DAnmTransform*>(dComIfG_getObjectRes(adapter->arc, s.anm));
        }
        if (res != nullptr && (s.anm != rec->ghostAnm || morf->getAnm() != res)) {
            morf->setAnm(res, s.attr, static_cast<f32>(s.morph), s.rate, 0.0f, -1.0f);
            rec->ghostAnm = s.anm;
        }
        if (res != nullptr) {
            morf->setPlaySpeed(s.rate);
        }
        morf->play(0, dComIfGp_getReverb(fopAcM_GetRoomNo(actor)));
        if (res != nullptr && std::fabs(morf->getFrame() - s.frame) > 1.0f) {
            morf->setFrameF(s.frame);
        }
    }
    // 6: the adapter's own state, then its own matrices and colliders.
    if (adapter->applyExtra != nullptr) {
        adapter->applyExtra(actor, s.extra);
    }
    if (adapter->present != nullptr) {
        adapter->present(actor);
    }
    rec->ghostTicks++;
    return true;
}

/* --- batch 9: enemy hits --- */
bool enemy_sync_live() {
    return enemies::detail::s_enabled;
}

const EnemyHitOverride* enemy_hit_override(const cCcD_Obj* atObj) {
    // Called for every hit any enemy takes: bail before the proxy scan when sync is off.
    if (!enemies::detail::s_enabled) {
        return nullptr;
    }
    return enemies::detail::hit_override(atObj);
}

/* --- batch 10B: death and drops --- */
void enemy_note_disappear(const fopAc_ac_c* actor, float x, float y, float z, unsigned char size,
    unsigned char type, unsigned char enemyId) {
    using namespace enemies::detail;
    // Called by every disappear in the game (51 enemy files): bail before the map lookup.
    if (!s_enabled || actor == nullptr) {
        return;
    }
    Rec* rec = find_rec(fopAcM_GetID(const_cast<fopAc_ac_c*>(actor)));
    if (rec == nullptr || rec->mode != enemies::Mode::Authority) {
        return;
    }
    rec->haveDisappear = true;
    rec->disappearSize = size;
    rec->disappearType = type;
    rec->disappearEnemyId = enemyId;
    rec->disappearPos[0] = x;
    rec->disappearPos[1] = y;
    rec->disappearPos[2] = z;
    Log.info("[mp-enemy] disappear key={:08X} size={} type={} enemyId=0x{:02X} at ({:.0f}, {:.0f}, "
             "{:.0f}) tick={}",
        rec->key, size, type, enemyId, x, y, z, network_manager().sim_tick());
}

void enemy_note_killing_blow(const fopAc_ac_c* enemy, const EnemyHitOverride* mpHit) {
    using namespace enemies::detail;
    if (!s_enabled || enemy == nullptr) {
        return;
    }
    Rec* rec = find_rec(fopAcM_GetID(const_cast<fopAc_ac_c*>(enemy)));
    if (rec == nullptr || rec->mode != enemies::Mode::Authority || rec->killer != kNoPlayer) {
        return;
    }
    if (mpHit != nullptr) {
        rec->killer = mpHit->attacker;
        rec->killerLife = mpHit->life;
        rec->killerMaxLife = mpHit->maxLife;
        rec->killerRollstab = mpHit->rollstab;
    } else {
        rec->killer = network_manager().local_player_id();
        rec->killerRollstab = false;
    }
    Log.info("[mp-enemy] killing blow key={:08X} by={} ({}) life={}/{} rollstab={} tick={}",
        rec->key, pid_str(rec->killer), mpHit != nullptr ? "remote, injected" : "this end's Link",
        mpHit != nullptr ? mpHit->life : static_cast<short>(dComIfGs_getLife()),
        mpHit != nullptr ? mpHit->maxLife : static_cast<short>(dComIfGs_getMaxLife()),
        rec->killerRollstab ? 1 : 0, network_manager().sim_tick());
}

bool enemy_drop_shared() {
    return enemies::detail::exec_authority_rec() != nullptr;
}

void enemy_roll_life(int tableNo, int& life, int& maxLife) {
    using namespace enemies::detail;
    Rec* rec = exec_authority_rec();
    if (rec == nullptr) {
        return;
    }
    std::int16_t killerLife = 0;
    std::int16_t killerMaxLife = 0;
    bool known = false;
    const std::uint32_t killer = resolved_killer(*rec, killerLife, killerMaxLife, known);
    // maxLife below 5 would make the roll's hp_max 0 (a divide by zero): keep the local hearts.
    const bool use = known && killerMaxLife >= 5;
    if (use) {
        life = killerLife;
        maxLife = killerMaxLife;
    }
    Log.info("[mp-enemy] drop roll key={:08X} table={} killer={} life={}/{} ({}) tick={}", rec->key,
        tableNo, pid_str(killer), life, maxLife,
        use ? "the killer's" : (known ? "mine: the killer's max is unknown" : "mine"),
        network_manager().sim_tick());
}

void enemy_note_roll_table(int tableIn, int tableOut) {
    using namespace enemies::detail;
    Rec* rec = exec_authority_rec();
    if (rec == nullptr) {
        return;
    }
    Log.info("[mp-enemy] drop table key={:08X} {} -> {}{} tick={}", rec->key, tableIn, tableOut,
        tableOut != tableIn ? " (low-life variant)" : "", network_manager().sim_tick());
}

}  // namespace dusk::mp
