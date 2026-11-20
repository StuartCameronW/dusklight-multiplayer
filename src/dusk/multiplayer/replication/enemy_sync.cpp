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
#include "dusk/logging.h"
#include "dusk/settings.h"
#include "enemy_internal.hpp"
#include "player_bridge.hpp"
#include "player_state.hpp"
#include "replication_manager.hpp"

#include "d/d_com_inf_game.h"
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
            const bool hold = held < kHostArrivalMinHoldTicks ||
                              (held < kHostArrivalMaxHoldTicks && host_hold_room(k));
            owner = hold ? prev->second.owner : kHostPlayerId;
            if (!hold) {
                Log.info("[mp-enemy] host takes over stage={:08X} room={} from player {} after {} "
                         "tick(s){}",
                    key_stage(k), key_room(k), prev->second.owner, held,
                    held >= kHostArrivalMaxHoldTicks ? " (hold cap reached)" : "");
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

void write_trace_rows() {
    if (!trace::enabled()) {
        return;
    }
    static const char* kHeader =
        "key,prof,proc,mode,degraded,authority,epoch,target,x,y,z,shapeY,hp,anm,frame,mflags,"
        "engaged,tx,ty,tz,tdist,samples,ghostTicks,realExecWhileGhost,swapTicks,absorbed,"
        "realLinkWrites,targetSwitches";
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
                        "{},{:.2f},{:.2f},{:.2f},{:.2f},{},{},{},{},{},{},{}",
                rec.key, static_cast<unsigned>(static_cast<std::uint16_t>(rec.prof)), rec.proc,
                mode_name(rec.mode), rec.degraded ? 1 : 0, pid_str(rec.authority), rec.epoch,
                pid_str(target), actor->current.pos.x, actor->current.pos.y, actor->current.pos.z,
                actor->shape_angle.y, actor->health, anm, frame,
                static_cast<fopEn_enemy_c*>(actor)->mFlags, engaged ? 1 : 0, tp[0], tp[1], tp[2],
                tdist, rec.samples.size(), rec.ghostTicks, realExec, rec.swapTicks,
                rec.absorbedWrites, rec.realLinkWrites, rec.targetSwitches));
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
            "realLinkWrites={} switches={} tick={}",
            rec.key, mode_name(rec.mode), pid_str(rec.authority), rec.epoch, pid_str(target),
            actor->health, actor->current.pos.x, actor->current.pos.y, actor->current.pos.z,
            rec.ghostTicks, realExec, rec.swapTicks, rec.absorbedWrites, rec.realLinkWrites,
            rec.targetSwitches, network_manager().sim_tick());
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
    }
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
}

void post_tick() {
    if (!s_enabled) {
        return;
    }
    check_slot0("post_tick");
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

    if (!rec->havePlayed) {
        // No state yet (just became a ghost): hold where create put it, but still pose it, so its
        // model is never drawn with uncomputed joints (R1: present must survive a fresh enemy).
        if (adapter->present != nullptr) {
            adapter->present(actor);
        }
        rec->ghostTicks++;
        return true;
    }

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

}  // namespace dusk::mp
