#include "enemy_internal.hpp"

#include <cmath>
#include <cstdint>
#include <iterator>
#include <memory>
#include <unordered_map>

#include <fmt/format.h>

#include "../session/network_manager.hpp"
#include "../session/presence.hpp"
#include "dusk/logging.h"
#include "player_bridge.hpp"
#include "player_state.hpp"
#include "replication_manager.hpp"

#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "m_Do/m_Do_mtx.h"

/**
 * Nearest-player aggro (mp-m6-enemies.md §5.6), agent E2.
 *
 * On the AUTHORITY, around one synced enemy's subclass execute, EnemyTargetScope picks the enemy's
 * target and, when that is a remote player, points player slot 0 (mPlayerInfo[0].mpPlayer, what
 * dComIfGp_getPlayer(0) and daPy_getPlayerActorClass() read) at that player's TargetStandIn for
 * the duration of the execute. ~98% of enemy reads of "the player" go through that one field
 * (§5.6.1), so the enemy measures, faces, chases and swings at the remote player's position.
 *
 * ★ Only slot 0. mPlayerPtr[LINK_PTR] (dComIfGp_getLinkPlayer, daAlink_getAlinkActorClass) is
 * never touched: Dusk code and the LinkPlayer-slot enemy reads (wolf-mouth matrices) use it to mean
 * the real local Link, and this file uses it for the same thing. Nothing here reads slot 0 to find
 * Link.
 *
 * The stand-in is a daPy_py_c, not a process: never in an actor list, process id invalid, so
 * fopAcM_SearchByID on it finds nothing. Writes enemies make into "the player" (changeDemoMode,
 * setPlayerPosAndAngle, setDkCaught...) land on it and are absorbed (counted, logged), so they
 * reach neither Link. It is rebuilt from the remote player's interpolated pose every pre_tick.
 */

namespace dusk::mp::enemies::detail {
namespace {

constexpr borealis::Log Log{"dusk::mp"};

/// §5.6.4 hysteresis (placeholders to tune by eye): a challenger must be nearer by at least 25 %
/// AND 150 units, for 30 consecutive executes, and never while the enemy is engaged.
constexpr float kSwitchRatio = 0.75f;
constexpr float kSwitchMinGain = 150.0f;
constexpr int kSwitchTicks = 30;
/// Absorbed-write log lines per enemy before they are only counted.
constexpr std::uint32_t kAbsorbLogLimit = 24;

/// Fallback offsets from his feet, used only while there is no local Link to measure (his own
/// eyePos / attention / head-top are re-read off the local Link every refill otherwise:
/// reuse-the-game's-own-values).
constexpr float kFallbackEyeY = 150.0f;
constexpr float kFallbackHeadTopY = 170.0f;

class TargetStandIn : public daPy_py_c {
public:
    explicit TargetStandIn(std::uint32_t playerId);

    f32 getGroundY() override { return mGroundY; }
    BOOL checkPlayerGuard() const override { return mGuard ? TRUE : FALSE; }

    /* Writes an enemy can make into "the player". Absorbed: counted for the scope's log. */
    void setPlayerPosAndAngle(cXyz const*, s16, int) override { absorb("setPlayerPosAndAngle"); }
    void setPlayerPosAndAngle(cXyz const*, csXyz const*) override {
        absorb("setPlayerPosAndAngle");
    }
    void setPlayerPosAndAngle(f32 (*)[4]) override { absorb("setPlayerPosAndAngle(mtx)"); }
    bool setThrowDamage(s16, f32, f32, int, int, int) override {
        absorb("setThrowDamage");
        return false;
    }
    void setDkCaught(fopAc_ac_c*) override { absorb("setDkCaught"); }
    void setOutPower(f32, s16, int) override { absorb("setOutPower"); }
    void onPressedDamage(cXyz const&, s16) override { absorb("onPressedDamage"); }

    void absorb(const char* what) {
        mWrites++;
        mLastWrite = what;
    }

    /// Rebuild from the remote player's last interpolated pose. `local` is the real local Link
    /// (LINK_PTR slot) or null; only his offsets and actor type are read.
    void refill(const RemotePlayer& player, daPy_py_c* local);

    std::uint32_t mPlayerId;
    f32 mGroundY = 0.0f;
    bool mGuard = false;
    /// He is in the world and in a place this end shows: may be a target this tick.
    bool mValid = false;
    /// A copy of his PlayerState as last refilled (event kind, location), for the selector.
    PlayerState mPose;
    Mtx mMtx;
    std::uint32_t mWrites = 0;
    const char* mLastWrite = nullptr;
    bool mHavePrev = false;
};

TargetStandIn::TargetStandIn(std::uint32_t playerId) : mPlayerId(playerId) {
    /* fopAc_ac_c's constructor is empty (f_op_actor.cpp:207), so every field an enemy can read is
     * given a value here. Not a process: no id, no name, no profile. */
    id = fpcM_ERROR_PROCESS_ID_e;
    name = -1;
    profname = -1;
    parameters = 0;
    actor_type = 0;
    setID = 0xFFFF;
    group = 0;
    argument = 0;
    actor_status = 0;
    actor_condition = 0;
    parentActorID = fpcM_ERROR_PROCESS_ID_e;
    const cXyz zero(0.0f, 0.0f, 0.0f);
    home.pos = zero;
    home.angle.set(0, 0, 0);
    home.roomNo = -1;
    home.field_0x13 = 0;
    old = home;
    current = home;
    shape_angle.set(0, 0, 0);
    scale.set(1.0f, 1.0f, 1.0f);
    speed = zero;
    MTXIdentity(mMtx);
    cullMtx = mMtx;
    model = nullptr;
    jntCol = nullptr;
    speedF = 0.0f;
    gravity = 0.0f;
    maxFallSpeed = 0.0f;
    eyePos = zero;
    for (int i = 0; i < fopAc_attn_MAX_e; i++) {
        attention_info.distances[i] = 0;
    }
    attention_info.field_0xa = 0;
    attention_info.position = zero;
    attention_info.flags = 0;
    field_0x560 = 0;
    health = 0;
#if TARGET_PC
    mItemGiveTag = 0;
    mItemGiveOriginalNo = 0;
#endif
    mCutType = 0;
    mComboCutCount = 0;
    mMode = 0;
    mDamageTimer = 0;
    mSwordUpTimer = 0;
    mNoResetFlg0 = mNoResetFlg1 = mNoResetFlg2 = mNoResetFlg3 = 0;
    mResetFlg0 = mResetFlg1 = 0;
    mEndResetFlg0 = mEndResetFlg1 = mEndResetFlg2 = 0;
    mMaxSpeed = 0.0f;
    mHeight = kFallbackHeadTopY;
    mBodyAngle.set(0, 0, 0);
    mHeadTopPos = mItemPos = mSwordTopPos = zero;
    mLeftHandPos = mRightHandPos = mLeftFootPos = mRightFootPos = mViewerCurrentPos = zero;
    mDemo.setDemoType(0);
    mDemo.setDemoMode(0);
    mDemo.setMoveAngle(0);
    mDemo.setTimer(0);
    mDemo.setParam0(0);
    mDemo.setParam1(0);
    mDemo.setParam2(0);
    mDemo.setStick(0.0f);
    mDemo.setPos0(&zero);
}

void TargetStandIn::refill(const RemotePlayer& player, daPy_py_c* local) {
    const PlayerState& st = player.lastApplied;
    mValid = player.hasLatest && st.in_world() && player.hiddenBy == RemoteLocation::Here &&
             remote_location(st) == RemoteLocation::Here;
    if (!mValid) {
        mHavePrev = false;
        return;
    }
    mPose = st;

    const cXyz pos(st.posX, st.posY, st.posZ);
    old = current;
    if (!mHavePrev) {
        old.pos = pos;
    }
    current.pos = pos;
    current.angle.set(0, st.angleY, 0);
    shape_angle = current.angle;
    current.roomNo = st.roomNo >= 0 ? st.roomNo : static_cast<s8>(dComIfGp_roomControl_getStayNo());
    // Bokoblin's searchPlayer reads speedF > 12 as "running at me" (d_a_e_oc.cpp:311). The pose
    // carries a gait RATE, not a speed, so the speed is what he actually moved since last tick.
    const cXyz delta = current.pos - old.pos;
    speed = delta;
    speedF = std::sqrt(delta.x * delta.x + delta.z * delta.z);
    mHavePrev = true;

    // Heights off the local Link, his own numbers, when there is one in the same form.
    float eyeY = kFallbackEyeY;
    float headTopY = kFallbackHeadTopY;
    float attnY = kFallbackEyeY;
    if (local != nullptr && ((local->checkWolf() != 0) == st.wolf())) {
        eyeY = local->eyePos.y - local->current.pos.y;
        headTopY = local->mHeadTopPos.y - local->current.pos.y;
        attnY = local->attention_info.position.y - local->current.pos.y;
        mHeight = local->mHeight;
        actor_type = local->actor_type;
    }
    eyePos.set(pos.x, pos.y + eyeY, pos.z);
    attention_info.position.set(pos.x, pos.y + attnY, pos.z);
    mHeadTopPos.set(pos.x, pos.y + headTopY, pos.z);
    mGroundY = pos.y;

    // The §5.6.3 table. Every flag word is rebuilt from scratch each tick, so a bit an enemy set on
    // the stand-in (a grab, a demo request) lasts at most until the next refill.
    mNoResetFlg0 = 0;
    mNoResetFlg1 = 0;
    mNoResetFlg2 = 0;
    mNoResetFlg3 = 0;
    mResetFlg0 = mResetFlg1 = 0;
    mEndResetFlg0 = mEndResetFlg1 = mEndResetFlg2 = 0;
    if (st.heavy_boots()) {
        mNoResetFlg0 |= FLG0_EQUIP_HVY_BOOTS;
    }
    if (st.cut_at()) {
        mNoResetFlg0 |= FLG0_CUT_AT_FLG;
    }
    if (st.wolf()) {
        mNoResetFlg1 |= FLG1_IS_WOLF;
    }
    mGuard = st.upper_guard();
    mCutType = st.cutType;
    mComboCutCount = st.cutCount;
    // ≈: the sender's damage-flash timer (his mDamageTimer + mDamageColorTime while hurt).
    mDamageTimer = static_cast<s16>(st.dmgFlash);
    mDemo.setDemoType(0);
    mDemo.setDemoMode(0);
    mDemo.setParam0(0);
    mDemo.setParam1(0);
    mDemo.setParam2(0);

    mDoMtx_stack_c::transS(pos.x, pos.y, pos.z);
    mDoMtx_stack_c::YrotM(st.angleY);
    MTXCopy(mDoMtx_stack_c::get(), mMtx);
    cullMtx = mMtx;
}

std::unordered_map<std::uint32_t, std::unique_ptr<TargetStandIn>>& standins() {
    static std::unordered_map<std::uint32_t, std::unique_ptr<TargetStandIn>> s;
    return s;
}

TargetStandIn* standin_for(std::uint32_t playerId) {
    auto& map = standins();
    const auto it = map.find(playerId);
    return it != map.end() ? it->second.get() : nullptr;
}

/// The real local Link, from the LINK_PTR slot. Null while there is none or it is being created.
daPy_py_c* real_link() {
    daPy_py_c* link = dComIfGp_getLinkPlayer();
    if (link == nullptr || fpcM_IsCreating(fopAcM_GetID(link))) {
        return nullptr;
    }
    return link;
}

float distance3(const fopAc_ac_c* a, const cXyz& b) {
    const float dx = b.x - a->current.pos.x;
    const float dy = b.y - a->current.pos.y;
    const float dz = b.z - a->current.pos.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

/// §5.6.4: may this remote player be targeted by an enemy of `rec`'s room right now?
bool remote_eligible(const Rec& rec, const TargetStandIn& si) {
    if (!si.mValid) {
        return false;
    }
    // A peer in a local event is out: his own ghosts are frozen by his own event gate, so an attack
    // could never land. (A pause menu does not exclude him: the Dying Light rule.)
    if (si.mPose.evt_kind() != kPlayerEvtNone) {
        return false;
    }
    if (si.mPose.stageHash != rec.stageHash) {
        return false;
    }
    if (si.mPose.roomNo >= 0 && !local_room_loaded(si.mPose.roomNo)) {
        return false;
    }
    if (const presence::Info* info = presence::get(si.mPlayerId)) {
        if ((info->flags &
                (presence::kFlagInEvent | presence::kFlagDead | presence::kFlagLoading)) != 0)
        {
            return false;
        }
        // Another layer of the same stage is another enemy world (§2.1).
        if (info->valid && info->layer != rec.layer) {
            return false;
        }
    }
    return true;
}

struct Candidate {
    std::uint32_t id = kNoPlayer;
    float dist = 0.0f;
};

void choose_target(Rec& rec, fopAc_ac_c* actor, const EnemyNetAdapter* adapter) {
    const std::uint32_t me = network_manager().local_player_id();
    Candidate cands[kMaxPlayers + 1];
    int count = 0;
    bool localOk = false;
    if (daPy_py_c* link = real_link()) {
        cands[count].id = me;
        cands[count].dist = distance3(actor, link->current.pos);
        count++;
        localOk = true;
    }
    if (adapter->remoteTargetOk) {
        for (const auto& [id, si] : standins()) {
            if (count >= static_cast<int>(kMaxPlayers + 1)) {
                break;
            }
            if (id == me || !remote_eligible(rec, *si)) {
                continue;
            }
            cands[count].id = id;
            cands[count].dist = distance3(actor, si->current.pos);
            count++;
        }
    }
    if (count == 0) {
        if (rec.target != kNoPlayer) {
            set_target(rec, kNoPlayer, "no candidate", -1.0f, rec.targetDist);
        }
        return;
    }
    int nearest = 0;
    int current = -1;
    for (int i = 0; i < count; i++) {
        if (cands[i].dist < cands[nearest].dist) {
            nearest = i;
        }
        if (cands[i].id == rec.target) {
            current = i;
        }
    }
    if (current < 0) {
        // No target yet, or the one it had is gone, in an event or elsewhere: decide now, whatever
        // the enemy is doing. A brand-new decision starts on the LOCAL Link (E3's DEMO_MASTER
        // writes into slot 0, so a fresh enemy must never begin on a stand-in).
        const bool initial = rec.target == kNoPlayer;
        const int pick = initial && localOk ? 0 : nearest;
        set_target(rec, cands[pick].id, initial ? "initial" : "target invalid", cands[pick].dist,
            rec.targetDist);
        return;
    }
    rec.targetDist = cands[current].dist;
    if (nearest == current) {
        rec.challenger = kNoPlayer;
        rec.challengerTicks = 0;
        return;
    }
    const float dCur = cands[current].dist;
    const float dNew = cands[nearest].dist;
    const bool nearer = dNew <= kSwitchRatio * dCur && dCur - dNew >= kSwitchMinGain;
    if (!nearer) {
        rec.challenger = kNoPlayer;
        rec.challengerTicks = 0;
        return;
    }
    if (rec.challenger != cands[nearest].id) {
        rec.challenger = cands[nearest].id;
        rec.challengerTicks = 0;
        Log.info("[mp-enemy] challenger key={:08X} player {} at {:.0f} vs target {} at {:.0f}: "
                 "switching after {} ticks unless that changes tick={}",
            rec.key, cands[nearest].id, dNew, rec.target, dCur, kSwitchTicks,
            network_manager().sim_tick());
    }
    rec.challengerTicks++;
    if (rec.challengerTicks < kSwitchTicks) {
        return;
    }
    if (adapter->engaged != nullptr && adapter->engaged(actor)) {
        return;  // mid-swing or mid-grab: keep counting, switch the first tick it is free
    }
    set_target(rec, cands[nearest].id, "nearer", dNew, dCur);
}

/* ---- The one active swap. Executes do not nest, but a nested scope is refused anyway. ---- */
struct ActiveScope {
    bool active = false;
    std::uint32_t proc = 0;
    TargetStandIn* standIn = nullptr;
    std::uint32_t writesBefore = 0;
    std::uint32_t demoModeBefore = 0;
    int demoTypeBefore = 0;
    std::uint32_t flg0Before = 0;
    std::uint32_t flg1Before = 0;
    daPy_py_c* link = nullptr;
    std::uint32_t linkDemoBefore = 0;
    cXyz linkPosBefore;
    std::uint32_t linkFlg0Before = 0;
    std::uint32_t linkFlg1Before = 0;
};

ActiveScope& active_scope() {
    static ActiveScope s;
    return s;
}

}  // namespace

void set_target(Rec& rec, std::uint32_t target, const char* why, float dist, float oldDist) {
    Log.info(
        "[mp-enemy] target key={:08X} {} -> {} ({}) dist {:.0f} (old target at {:.0f}) tick={}",
        rec.key, rec.target == kNoPlayer ? std::string("none") : std::to_string(rec.target),
        target == kNoPlayer ? std::string("none") : std::to_string(target), why, dist, oldDist,
        network_manager().sim_tick());
    if (rec.target != kNoPlayer && target != kNoPlayer) {
        rec.targetSwitches++;
    }
    rec.target = target;
    rec.targetDist = dist;
    rec.challenger = kNoPlayer;
    rec.challengerTicks = 0;
}

void standins_refresh() {
    auto& map = standins();
    const auto& players = replication_manager().players();
    for (auto it = map.begin(); it != map.end();) {
        it = players.count(it->first) == 0 ? map.erase(it) : std::next(it);
    }
    daPy_py_c* local = real_link();
    for (const auto& [id, player] : players) {
        std::unique_ptr<TargetStandIn>& si = map[id];
        if (!si) {
            si = std::make_unique<TargetStandIn>(id);
            Log.info("[mp-enemy] stand-in created for player {}", id);
        }
        si->refill(player, local);
    }
}

void standins_forget(std::uint32_t playerId) {
    check_slot0("standins_forget");
    standins().erase(playerId);
}

void standins_reset() {
    check_slot0("standins_reset");
    active_scope() = ActiveScope{};
    standins().clear();
}

bool is_standin(const fopAc_ac_c* actor) {
    if (actor == nullptr) {
        return false;
    }
    for (const auto& [id, si] : standins()) {
        if (static_cast<const fopAc_ac_c*>(si.get()) == actor) {
            return true;
        }
    }
    return false;
}

void check_slot0(const char* where) {
    fopAc_ac_c* slot = dComIfGp_getPlayer(0);
    if (slot == nullptr || !is_standin(slot)) {
        return;
    }
    daPy_py_c* link = dComIfGp_getLinkPlayer();
    g_dComIfG_gameInfo.play.setPlayer(0, link);
    Log.error("[mp-enemy] slot 0 still held a stand-in at {}: put Link back (a scope did not "
              "restore)",
        where);
    active_scope() = ActiveScope{};
}

bool player_position(std::uint32_t playerId, float out[3]) {
    if (playerId == kNoPlayer) {
        return false;
    }
    if (playerId == network_manager().local_player_id()) {
        daPy_py_c* link = real_link();
        if (link == nullptr) {
            return false;
        }
        out[0] = link->current.pos.x;
        out[1] = link->current.pos.y;
        out[2] = link->current.pos.z;
        return true;
    }
    const auto& players = replication_manager().players();
    const auto it = players.find(playerId);
    if (it == players.end() || !it->second.hasLatest || !it->second.lastApplied.in_world()) {
        return false;
    }
    out[0] = it->second.lastApplied.posX;
    out[1] = it->second.lastApplied.posY;
    out[2] = it->second.lastApplied.posZ;
    return true;
}

}  // namespace dusk::mp::enemies::detail

namespace dusk::mp {

EnemyTargetScope::EnemyTargetScope(fopAc_ac_c* actor) : mSavedPlayer(nullptr), mActive(false) {
    using namespace enemies::detail;
    // ★ Runs for every actor that passes the execute gate, every tick: bail before any map lookup.
    if (!enabled() || actor == nullptr) {
        return;
    }
    const EnemyNetAdapter* adapter = find_adapter(fopAcM_GetProfName(actor));
    if (adapter == nullptr) {
        return;
    }
    Rec* rec = find_rec(fopAcM_GetID(actor));
    if (rec == nullptr) {
        return;
    }
    if (rec->mode == enemies::Mode::Ghost) {
        // Counted here and in the ghost gate: the difference is executes a ghost ran for real.
        rec->gateAsGhost++;
        return;
    }
    if (rec->mode != enemies::Mode::Authority) {
        return;
    }
    choose_target(*rec, actor, adapter);
    const std::uint32_t me = network_manager().local_player_id();
    if (rec->target == kNoPlayer || rec->target == me) {
        return;
    }
    ActiveScope& scope = active_scope();
    if (scope.active) {
        return;
    }
    TargetStandIn* si = standin_for(rec->target);
    if (si == nullptr || !si->mValid) {
        return;
    }
    // Only Link is ever swapped out: whatever else is in slot 0 (nothing, during a load) stays.
    daPy_py_c* link = dComIfGp_getLinkPlayer();
    fopAc_ac_c* slot = dComIfGp_getPlayer(0);
    if (slot == nullptr || slot != static_cast<fopAc_ac_c*>(link)) {
        return;
    }
    scope.active = true;
    scope.proc = rec->proc;
    scope.standIn = si;
    scope.writesBefore = si->mWrites;
    scope.demoModeBefore = si->mDemo.getDemoMode();
    scope.demoTypeBefore = si->mDemo.getDemoType();
    scope.flg0Before = si->mNoResetFlg0;
    scope.flg1Before = si->mNoResetFlg1;
    scope.link = link;
    scope.linkDemoBefore = link->mDemo.getDemoMode();
    scope.linkPosBefore = link->current.pos;
    scope.linkFlg0Before = link->mNoResetFlg0;
    scope.linkFlg1Before = link->mNoResetFlg1;
    mSavedPlayer = slot;
    mActive = true;
    g_dComIfG_gameInfo.play.setPlayer(0, si);
    rec->swapTicks++;
}

EnemyTargetScope::~EnemyTargetScope() {
    using namespace enemies::detail;
    if (!mActive) {
        return;
    }
    ActiveScope& scope = active_scope();
    fopAc_ac_c* now = dComIfGp_getPlayer(0);
    if (now == static_cast<fopAc_ac_c*>(scope.standIn) || is_standin(now)) {
        g_dComIfG_gameInfo.play.setPlayer(0, static_cast<fopAc_ac_c*>(mSavedPlayer));
    } else {
        // Someone else rewrote slot 0 during the execute (Link re-created?). Theirs wins.
        Log.error("[mp-enemy] slot 0 changed during a swapped execute (proc {}): left as it is",
            scope.proc);
    }

    Rec* rec = find_rec(scope.proc);
    TargetStandIn* si = scope.standIn;
    if (rec != nullptr && si != nullptr) {
        std::uint32_t absorbed = si->mWrites - scope.writesBefore;
        const char* what = si->mLastWrite;
        if (si->mDemo.getDemoMode() != scope.demoModeBefore ||
            si->mDemo.getDemoType() != scope.demoTypeBefore)
        {
            absorbed++;
            what = "changeDemoMode / demo type";
        }
        if (si->mNoResetFlg0 != scope.flg0Before || si->mNoResetFlg1 != scope.flg1Before) {
            absorbed++;
            what = "player flag write";
        }
        if (absorbed != 0) {
            if (rec->absorbedWrites < kAbsorbLogLimit) {
                Log.info(
                    "[mp-enemy] stand-in absorbed key={:08X} target={} write={} demoMode {} -> "
                    "{} tick={}",
                    rec->key, si->mPlayerId, what != nullptr ? what : "?", scope.demoModeBefore,
                    si->mDemo.getDemoMode(), network_manager().sim_tick());
            }
            rec->absorbedWrites += absorbed;
        }
    }
    daPy_py_c* link = scope.link;
    if (rec != nullptr && link != nullptr && link == dComIfGp_getLinkPlayer()) {
        const bool moved = link->current.pos.x != scope.linkPosBefore.x ||
                           link->current.pos.y != scope.linkPosBefore.y ||
                           link->current.pos.z != scope.linkPosBefore.z;
        if (moved || link->mDemo.getDemoMode() != scope.linkDemoBefore ||
            link->mNoResetFlg0 != scope.linkFlg0Before ||
            link->mNoResetFlg1 != scope.linkFlg1Before)
        {
            rec->realLinkWrites++;
            Log.error(
                "[mp-enemy] REAL LINK WRITTEN during a swapped execute key={:08X} target={}: "
                "moved={} demoMode {} -> {} flg0 {:08X} -> {:08X} flg1 {:08X} -> {:08X} tick={}",
                rec->key, rec->target, moved ? 1 : 0, scope.linkDemoBefore,
                link->mDemo.getDemoMode(), scope.linkFlg0Before, link->mNoResetFlg0,
                scope.linkFlg1Before, link->mNoResetFlg1, network_manager().sim_tick());
        }
    }
    scope = ActiveScope{};
}

}  // namespace dusk::mp
