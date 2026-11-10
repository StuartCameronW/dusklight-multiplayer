#include "object_capture.hpp"
#include "object_drops.hpp"

#include <cmath>
#include <cstdint>
#include <unordered_map>

#include "../session/network_manager.hpp"
#include "d/actor/d_a_obj_item.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "dusk/logging.h"
#include "dusk/multiplayer.hpp"
#include "f_op/f_op_actor_mng.h"
#include "item_claims.hpp"
#include "object_channel.hpp"

/**
 * Shared drops, batch 7. Design, rules and the pickup protocol: object_drops.hpp. This file is
 * every engine-touching piece of it: the owner-side capture (tick_drop), the receiver's real
 * daItem_c copy (engine::create_drop / look_up_drop / apply_drop, the seam object_channel.hpp
 * declares), the daItem_c hooks (dusk/multiplayer.hpp) and the DropTaken packet.
 *
 * One table, keyed by the local daItem_c's process id, holds both the drops this end announced
 * (owner) and the copies of the partners' ones (remote). An entry outlives its pickup ("consumed")
 * until its actor is gone, so a hook that fires during the pickup animation still knows not to ask
 * again.
 */

namespace dusk::mp::objects {
namespace {

constexpr borealis::Log Log{"dusk::mp"};

constexpr fpc_ProcID kNoProc = fpcM_ERROR_PROCESS_ID_e;
/// A noted item whose create has not finished after this long is forgotten (it was cancelled).
constexpr std::uint64_t kCreatedForgetTicks = 300;

struct Drop {
    ObjRef ref;
    /// A copy of a partner's drop (true) or one this end announced (false).
    bool remote = false;
    std::uint8_t itemNo = 0;
    /// Picked up here, or taken by someone else: nothing more is sent, applied or asked.
    bool consumed = false;
    /// A pickup claim is out; drops_execute keeps asking until it resolves.
    bool claimPending = false;
    bool loggedPending = false;
    /* Owner side. */
    KindBlob blob;
    bool haveSent = false;
    ObjPose sent;
    /* Receiver side. */
    bool flagsSet = false;
    bool haveApplied = false;
    ObjPose applied;
};

struct State {
    std::unordered_map<fpc_ProcID, Drop> drops;
    /// Items the creation helpers made at run time and tick_drop has not looked at yet -> the tick
    /// they were noted.
    std::unordered_map<fpc_ProcID, std::uint64_t> created;
};

State& S() {
    static State s;
    return s;
}

std::uint64_t now_tick() {
    return network_manager().sim_tick();
}

std::uint8_t local8() {
    return static_cast<std::uint8_t>(network_manager().local_player_id());
}

bool in_force() {
    return item_claims::active();
}

item_claims::ClaimTarget target_of(const ObjRef& ref) {
    item_claims::ClaimTarget t;
    t.kind = item_claims::ClaimKind::Drop;
    t.tbl = ref.owner;
    t.no = ref.serial;
    return t;
}

enum class Look : std::uint8_t {
    Gone,
    Creating,
    Alive,
};

/// The live daItem_c behind `id`. Name-checked so a non-item can never be cast.
daItem_c* find_item(fpc_ProcID id, Look& look) {
    fopAc_ac_c* actor = NULL;
    const bool known = fopAcM_SearchByID(id, &actor) != 0;
    if (!known) {
        look = Look::Gone;
        return NULL;
    }
    if (actor == NULL) {
        look = Look::Creating;
        return NULL;
    }
    if (fopAcM_GetName(actor) != fpcNm_ITEM_e) {
        look = Look::Gone;
        return NULL;
    }
    look = Look::Alive;
    return static_cast<daItem_c*>(actor);
}

daItem_c* find_item(fpc_ProcID id) {
    Look look = Look::Gone;
    return find_item(id, look);
}

/// The consumables a breakable or an enemy drops. Keys, heart pieces and everything else with a
/// save bit or a get demo of its own are not drops.
bool shareable_item(u8 itemNo) {
    switch (itemNo) {
    case dItemNo_HEART_e:
    case dItemNo_GREEN_RUPEE_e:
    case dItemNo_BLUE_RUPEE_e:
    case dItemNo_YELLOW_RUPEE_e:
    case dItemNo_RED_RUPEE_e:
    case dItemNo_PURPLE_RUPEE_e:
    case dItemNo_ORANGE_RUPEE_e:
    case dItemNo_SILVER_RUPEE_e:
    case dItemNo_ARROW_10_e:
    case dItemNo_ARROW_20_e:
    case dItemNo_ARROW_30_e:
    case dItemNo_ARROW_1_e:
    case dItemNo_PACHINKO_SHOT_e:
        return true;
    default:
        return false;
    }
}

/// daItem_prm::getType values for an item that falls out of something and lies there. Not
/// SIMPLE_GET / FORCE_GET (handed straight to the player) and not BOOM_HIT (hidden until a
/// boomerang finds it: placed, not dropped).
bool shareable_type(u8 type) {
    switch (type) {
    case daItem_c::TYPE_LAUNCH_e:
    case daItem_c::TYPE_FIXED_PLACE_e:
    case daItem_c::TYPE_UNK2_e:
    case daItem_c::TYPE_WAIT_e:
    case daItem_c::TYPE_LAUNCH_NO_RND_e:
    case daItem_c::TYPE_LAUNCH_SMALL_e:
    case daItem_c::TYPE_LAUNCH_FROM_PLAYER_e:
        return true;
    default:
        return false;
    }
}

/// Null when the item may be shared, else why not (for the log).
const char* not_shareable(daItem_c* item) {
    if (item->mItemBitNo != 0xFF) {
        return "has a save bit (the floor-item claim's)";
    }
    if (!shareable_item(item->m_itemNo)) {
        return "not a consumable drop";
    }
    if (!shareable_type(daItem_prm::getType(item))) {
        return "not a dropped type";
    }
    if (item->getStatus() != daItem_c::STATUS_NORMAL_e) {
        return "not lying free (switch wait, boomerang wait or a get)";
    }
    if (item->mItemOverridden || item->mOriginalItemNo == dItemNo_BOOMERANG_e) {
        return "an overridden item";
    }
    if (item->checkFlag(daItem_c::FLAG_INIT_GET_ITEM_e) ||
        item->checkFlag(daItem_c::FLAG_DELETE_ITEM_e))
    {
        return "already being picked up or removed";
    }
    return NULL;
}

/// The owner's pose for the stream. Once the drop stops moving its yaw is held at the last moving
/// value: rupees spin forever at rest, and the receiver's copy spins itself (its RotateYBase runs
/// under FLAG_NO_MOVE_e), so a spinning yaw would only stream 30 entries a second for nothing.
ObjPose owner_pose(daItem_c* item, const Drop& d) {
    ObjPose p;
    p.pos[0] = item->current.pos.x;
    p.pos[1] = item->current.pos.y;
    p.pos[2] = item->current.pos.z;
    p.angle[0] = item->shape_angle.x;
    p.angle[1] = item->shape_angle.y;
    p.angle[2] = item->shape_angle.z;
    // The flashing before it times out (procMainNormal changeDraw) shows on the copy too.
    p.flags = item->chkDraw() ? 0 : kObjPoseHidden;
    if (d.haveSent) {
        bool still = true;
        for (int i = 0; i < 3; i++) {
            if (std::fabs(p.pos[i] - d.sent.pos[i]) > 0.01f) {
                still = false;
            }
        }
        if (still) {
            for (int i = 0; i < 3; i++) {
                p.angle[i] = d.sent.angle[i];
            }
        }
    }
    return p;
}

void announce(daItem_c* item, fpc_ProcID id) {
    Writer w;
    write_drop_blob(w, DropBlob{item->m_itemNo, 0});
    Drop d;
    d.blob.assign(w);
    d.itemNo = item->m_itemNo;
    const ObjPose pose = owner_pose(item, d);
    ObjRef ref;
    if (!spawn_transient(ObjKind::Drop, pose, d.blob, ref)) {
        Log.warn("[mp-drop] proc={} item=0x{:02X}: not shared (the channel refused the spawn; no "
                 "local location yet?) tick={}",
            id, item->m_itemNo, now_tick());
        return;
    }
    d.ref = ref;
    d.haveSent = true;
    d.sent = pose;
    S().drops[id] = d;
    Log.info(
        "[mp-drop] shared {} proc={} item=0x{:02X} at ({:.0f}, {:.0f}, {:.0f}) room {} tick={}",
        ref.str(), id, item->m_itemNo, pose.pos[0], pose.pos[1], pose.pos[2],
        static_cast<int>(fopAcM_GetRoomNo(item)), now_tick());
}

/// Gone from this end for good: the owner says so to everyone, a copy stops being played back.
void consume(Drop& d) {
    d.consumed = true;
    d.claimPending = false;
    if (!d.remote) {
        despawn_owned(d.ref, ObjDespawnReason::PickedUp);
    } else {
        forget_remote(d.ref, false);
    }
}

/// Drop T<owner>:<serial> went to `taker`. Every end but the taker's removes its copy.
void apply_taken(std::uint8_t owner, std::uint16_t serial, std::uint32_t taker) {
    if (taker == network_manager().local_player_id()) {
        return;  // my own pickup: drops_claim_pickup did the rest
    }
    const ObjRef ref = ObjRef::transient(owner, serial);
    for (auto& entry : S().drops) {
        Drop& d = entry.second;
        if (d.ref != ref || d.consumed) {
            continue;
        }
        daItem_c* item = find_item(entry.first);
        if (item != NULL && item->checkFlag(daItem_c::FLAG_INIT_GET_ITEM_e)) {
            // Only a claim grant starts a pickup, and the host made exactly one: unreachable.
            Log.error("[mp-drop] {} granted to player {}, but I am already picking it up: "
                      "DUPLICATE (a claim was granted twice) tick={}",
                ref.str(), taker, now_tick());
            consume(d);
            return;
        }
        Log.info("[mp-drop] {} taken by player {}: removing my {} proc={} tick={}", ref.str(),
            taker, d.remote ? "copy" : "drop", entry.first, now_tick());
        consume(d);
        if (item != NULL) {
            item->hide();
            item->deleteItem();
        }
        return;
    }
    if (owner != local8()) {
        // No copy here (not created yet, or never: another room). Make sure none is made later.
        forget_remote(ref, true);
    }
}

}  // namespace

/* ---- the capture interface (object_capture.hpp) --------------------------------------------- */

namespace capture {

void tick_drop() {
    State& s = S();
    if (!in_force()) {
        if (!s.drops.empty() || !s.created.empty()) {
            Log.info("[mp-drop] claims no longer in force: forgetting {} drop(s)", s.drops.size());
            s = State();
        }
        return;
    }
    const std::uint64_t now = now_tick();

    // Items the helpers created since last tick, once their create has finished.
    for (auto it = s.created.begin(); it != s.created.end();) {
        Look look = Look::Gone;
        daItem_c* item = find_item(it->first, look);
        if (look == Look::Creating) {
            if (now - it->second > kCreatedForgetTicks) {
                it = s.created.erase(it);
            } else {
                ++it;
            }
            continue;
        }
        const fpc_ProcID id = it->first;
        it = s.created.erase(it);
        if (item == NULL || s.drops.find(id) != s.drops.end()) {
            continue;
        }
        const char* why = not_shareable(item);
        if (why != NULL) {
            Log.debug("[mp-drop] proc={} item=0x{:02X} bit=0x{:X} type={} stays local: {}", id,
                item->m_itemNo, item->mItemBitNo, static_cast<int>(daItem_prm::getType(item)), why);
            continue;
        }
        announce(item, id);
    }

    for (auto it = s.drops.begin(); it != s.drops.end();) {
        Drop& d = it->second;
        Look look = Look::Gone;
        daItem_c* item = find_item(it->first, look);
        if (look == Look::Gone) {
            if (!d.remote && !d.consumed) {
                // Timed out (procMainNormal deleteItem), fell in lava, or a room unload took it.
                Log.info("[mp-drop] {} proc={} gone here unpicked: despawning tick={}", d.ref.str(),
                    it->first, now);
                despawn_owned(d.ref, ObjDespawnReason::Expired);
            }
            it = s.drops.erase(it);
            continue;
        }
        if (!d.remote && !d.consumed && item != NULL) {
            const ObjPose pose = owner_pose(item, d);
            update_owned(d.ref, pose, d.blob);
            d.sent = pose;
            d.haveSent = true;
        }
        ++it;
    }
}

void reset_drop() {
    State& s = S();
    if (!s.drops.empty()) {
        Log.info("[mp-drop] reset: forgetting {} drop(s)", s.drops.size());
    }
    s = State();
}

}  // namespace capture

/* ---- the engine seam for ObjKind::Drop (object_channel.hpp) -------------------------------- */

namespace engine {

std::uint32_t create_drop(const ObjRef& ref, const ObjPose& pose, const KindBlob& blob) {
    fopAc_ac_c* link = dComIfGp_getPlayer(0);
    if (link == NULL) {
        return kNoProc;
    }
    if (!in_force()) {
        // Without the claim a copy would be a second, freely collectable rupee.
        Log.warn("[mp-drop] copy of {} refused: claims are not in force on this end", ref.str());
        return kNoProc;
    }
    DropBlob b;
    Reader r = blob.reader();
    if (!read_drop_blob(r, b) || !shareable_item(b.itemNo)) {
        Log.warn("[mp-drop] copy of {} refused: bad drop blob (len {}, item 0x{:02X})", ref.str(),
            blob.len, b.itemNo);
        return kNoProc;
    }
    // A plain launched drop with no save bit and no switch, as fopAcM_createItem builds one
    // (MAKE_ITEM_PARAMS, f_op_actor_mng.cpp): it grows in like the owner's. Its own launch speed
    // never applies: drops_execute switches its movement off before its first move.
    const u32 params = static_cast<u32>(b.itemNo) | (0xFFu << 8) | (0xFFu << 16) |
                       ((static_cast<u32>(daItem_c::TYPE_LAUNCH_e) & 0xF) << 24);
    const cXyz pos(pose.pos[0], pose.pos[1], pose.pos[2]);
    const csXyz angle(0, pose.angle[1], 0);
    /* In the LOCAL Link's room, as every ghost is (d_a_mp_ghost.cpp create_ghost): that room is
     * loaded by definition. A copy a room unload takes is rebuilt by the channel. Not through
     * fopAcM_createItem: that would note it as a run-time drop of OURS and announce it back. */
    const fpc_ProcID id =
        fopAcM_create(fpcNm_ITEM_e, params, &pos, fopAcM_GetRoomNo(link), &angle, NULL, -1);
    if (id == kNoProc) {
        return kNoProc;
    }
    Drop d;
    d.ref = ref;
    d.remote = true;
    d.itemNo = b.itemNo;
    S().drops[id] = d;
    Log.info("[mp-drop] copy create {} proc={} item=0x{:02X} at ({:.0f}, {:.0f}, {:.0f}) tick={}",
        ref.str(), id, b.itemNo, pose.pos[0], pose.pos[1], pose.pos[2], now_tick());
    return id;
}

GhostLookup look_up_drop(std::uint32_t procId) {
    Look look = Look::Gone;
    find_item(procId, look);
    switch (look) {
    case Look::Alive:
        return GhostLookup::Alive;
    case Look::Creating:
        return GhostLookup::Creating;
    case Look::Gone:
        break;
    }
    return GhostLookup::Gone;
}

void apply_drop(std::uint32_t procId, const ObjPose& pose, bool visible) {
    auto it = S().drops.find(procId);
    if (it == S().drops.end() || !it->second.remote || it->second.consumed) {
        return;
    }
    daItem_c* item = find_item(procId);
    if (item == NULL || item->checkFlag(daItem_c::FLAG_INIT_GET_ITEM_e) ||
        item->checkFlag(daItem_c::FLAG_DELETE_ITEM_e))
    {
        return;  // its pickup animation (or its removal) owns it now
    }
    if (item->getStatus() != daItem_c::STATUS_NORMAL_e || fopAcM_checkHookCarryNow(item)) {
        return;  // the local boomerang or hookshot is bringing it to this Link: theirs to move
    }
    Drop& d = it->second;
    bool moved = !d.haveApplied;
    for (int i = 0; i < 3 && !moved; i++) {
        moved = std::fabs(pose.pos[i] - d.applied.pos[i]) > 0.01f ||
                pose.angle[i] != d.applied.angle[i];
    }
    item->current.pos.set(pose.pos[0], pose.pos[1], pose.pos[2]);
    if (moved) {
        // At rest the owner holds the yaw, so the copy's own spin is left alone.
        item->shape_angle.set(pose.angle[0], pose.angle[1], pose.angle[2]);
    }
    d.applied = pose;
    d.haveApplied = true;
    if (visible) {
        item->show();
    } else {
        item->hide();
    }
    // Its matrix was built in its execute, before this; rebuild it so this frame draws it here.
    item->setBaseMtx();
}

}  // namespace engine

/* ---- host grant and DropTaken (object_drops.hpp) --------------------------------------------- */

namespace drops {

void host_on_granted(std::uint8_t owner, std::uint16_t serial, std::uint32_t taker) {
    Log.info("[mp-drop] host: T{}:{} goes to player {}; telling every guest tick={}",
        static_cast<int>(owner), serial, taker, now_tick());
    apply_taken(owner, serial, taker);
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::DropTaken));
    w.write_u8(owner);
    w.write_u16(serial);
    w.write_u8(static_cast<std::uint8_t>(taker));
    network_manager().send_to_session(w, kChannelObjects, true);
}

void on_packet(PacketId id, Reader& r, std::uint32_t fromPlayerId) {
    if (id != PacketId::DropTaken) {
        return;
    }
    if (network_manager().role() != Role::Client || fromPlayerId != kHostPlayerId) {
        Log.error("[mp-drop] DropTaken from player {} not from the host; ignored", fromPlayerId);
        return;
    }
    std::uint8_t owner = 0;
    std::uint16_t serial = 0;
    std::uint8_t taker = 0;
    if (!r.read_u8(owner) || !r.read_u16(serial) || !r.read_u8(taker)) {
        Log.warn("[mp-drop] truncated DropTaken; dropped");
        return;
    }
    Log.info("[mp-drop] DropTaken T{}:{} -> player {} tick={}", static_cast<int>(owner), serial,
        static_cast<int>(taker), now_tick());
    apply_taken(owner, serial, taker);
}

bool view(std::uint32_t procId, DropView& out) {
    auto it = S().drops.find(procId);
    if (it == S().drops.end()) {
        return false;
    }
    out.shared = true;
    out.remote = it->second.remote;
    out.consumed = it->second.consumed;
    out.claimPending = it->second.claimPending;
    out.ref = it->second.ref;
    return true;
}

}  // namespace drops

}  // namespace dusk::mp::objects

/* ---- decomp hooks (declared in dusk/multiplayer.hpp) ------------------------------------------
 */

namespace dusk::mp {

namespace {
constexpr borealis::Log HookLog{"dusk::mp"};
}  // namespace

namespace {
/// drops_enemy_scope depth. Not in State: a reset mid-scope must not unbalance it.
int sEnemyScope = 0;
}  // namespace

void drops_note_created(unsigned int procId) {
    if (procId == fpcM_ERROR_PROCESS_ID_e || sEnemyScope > 0 || !objects::in_force()) {
        return;
    }
    objects::S().created[procId] = objects::now_tick();
}

void drops_enemy_scope(bool enter) {
    sEnemyScope += enter ? 1 : -1;
    if (sEnemyScope < 0) {
        sEnemyScope = 0;
    }
}

bool drops_execute(daItem_c* item) {
    if (!objects::in_force()) {
        return false;
    }
    auto it = objects::S().drops.find(fopAcM_GetID(item));
    if (it == objects::S().drops.end()) {
        return false;
    }
    objects::Drop& d = it->second;
    if (d.remote && !d.consumed && item->getStatus() == daItem_c::STATUS_NORMAL_e &&
        !fopAcM_checkHookCarryNow(item))
    {
        /* A copy follows the owner's stream (apply_drop): no physics, no timeout of its own. Set
         * before its first procMainNormal, so its launch speed never moves it, and again after a
         * boomerang or hookshot that let go of it without a pickup (both clear NO_MOVE). */
        item->setFlag(daItem_c::FLAG_NO_MOVE_e);
        item->setFlag(daItem_c::FLAG_NO_TIMER_e);
        d.flagsSet = true;
    }
    if (d.consumed || !d.claimPending || item->checkFlag(daItem_c::FLAG_INIT_GET_ITEM_e) ||
        item->checkFlag(daItem_c::FLAG_DELETE_ITEM_e))
    {
        return false;
    }
    /* A touch is waiting on the host. Settle it whether or not the player is still touching: a
     * player who ran straight through a rupee still gets it once the grant lands. */
    const item_claims::ClaimTarget t = objects::target_of(d.ref);
    const item_claims::ClaimState st = item_claims::claim(t);
    if (st == item_claims::ClaimState::Granted) {
        HookLog.info(
            "[mp-drop] {} grant arrived: picking it up tick={}", d.ref.str(), objects::now_tick());
        item->itemGetNextExecute();  // asks again (Granted at once) and runs the pickup
        return false;
    }
    if (st == item_claims::ClaimState::Denied &&
        item_claims::last_reason(t) != item_claims::DenyReason::Timeout)
    {
        HookLog.info("[mp-drop] {} denied ({}): removing without effect tick={}", d.ref.str(),
            item_claims::reason_name(item_claims::last_reason(t)), objects::now_tick());
        objects::consume(d);
        item->hide();
        item->deleteItem();
        return true;
    }
    return false;
}

int drops_claim_pickup(daItem_c* item) {
    if (!objects::in_force()) {
        return kClaimGranted;
    }
    auto it = objects::S().drops.find(fopAcM_GetID(item));
    if (it == objects::S().drops.end() || it->second.consumed) {
        return kClaimGranted;  // not a shared drop: vanilla
    }
    objects::Drop& d = it->second;
    const item_claims::ClaimTarget t = objects::target_of(d.ref);
    switch (item_claims::claim(t)) {
    case item_claims::ClaimState::Granted:
        HookLog.info("[mp-drop] pickup granted {} ({} item=0x{:02X}) tick={}", d.ref.str(),
            d.remote ? "partner's" : "mine", d.itemNo, objects::now_tick());
        objects::consume(d);
        return kClaimGranted;
    case item_claims::ClaimState::Pending:
        break;
    case item_claims::ClaimState::Denied:
        if (item_claims::last_reason(t) == item_claims::DenyReason::Timeout) {
            // The host did not answer. Not a reason to throw a rupee away: keep it, ask again.
            break;
        }
        HookLog.info("[mp-drop] pickup denied {} ({}): removing without effect tick={}",
            d.ref.str(), item_claims::reason_name(item_claims::last_reason(t)),
            objects::now_tick());
        objects::consume(d);
        return kClaimDenied;
    }
    d.claimPending = true;
    if (!d.loggedPending) {
        d.loggedPending = true;
        HookLog.info("[mp-drop] pickup pending {}: asked the host tick={}", d.ref.str(),
            objects::now_tick());
    }
    return kClaimPending;
}

}  // namespace dusk::mp
