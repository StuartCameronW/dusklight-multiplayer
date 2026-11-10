/**
 * @file d_a_mp_ghost.cpp
 *
 * Dusk-only: the receiver-side ghost of a replicated object (fpcNm_MP_GHOST_e), and the engine seam
 * the object channel calls through (dusk::mp::objects::engine, declared in object_channel.hpp).
 * Spec: .claude/future-plans/mp-object-channel.md §2 and §6 (phase 0).
 */

#include "d/dolzel_rel.h"  // IWYU pragma: keep

#include <algorithm>
#include <cstring>
#include <tuple>
#include <vector>

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_mp_ghost.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "dusk/logging.h"
#include "dusk/multiplayer/replication/object_channel.hpp"
#include "dusk/multiplayer/replication/player_bridge.hpp"
#include "dusk/multiplayer/replication/player_state.hpp"
#include "f_op/f_op_actor_mng.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_mtx.h"

namespace {

constexpr borealis::Log Log{"dusk::mp"};

namespace objects = dusk::mp::objects;

constexpr u8 l_kindDebug = static_cast<u8>(objects::ObjKind::Debug);
constexpr u8 l_kindArrow = static_cast<u8>(objects::ObjKind::Arrow);
constexpr u8 l_kindPellet = static_cast<u8>(objects::ObjKind::Pellet);
constexpr u8 l_kindBomb = static_cast<u8>(objects::ObjKind::Bomb);
constexpr u8 l_kindWaterBomb = static_cast<u8>(objects::ObjKind::WaterBomb);
constexpr u8 l_kindBombling = static_cast<u8>(objects::ObjKind::Bombling);
constexpr u8 l_kindBoomerang = static_cast<u8>(objects::ObjKind::Boomerang);
constexpr u8 l_kindRodBall = static_cast<u8>(objects::ObjKind::RodBall);

/// "Alink" resource indices (d_a_arrow.cpp:27-42): the normal arrow.
constexpr int l_arrowNormalBmd = 0x1c;

static_assert(daMpGhost_c::BLOB_MAX == static_cast<int>(objects::kMaxKindBlob),
    "the ghost must hold any blob the channel accepts");

}  // namespace

daMpGhost_c::daMpGhost_c()
    : mpModel(NULL), mKind(0), mAuthority(0), mNetFlags(0), mVisible(false), mHasPose(false),
      mCreateComplete(false), mBlobLen(0), mExecuteCount(0), mEventCount(0) {
    std::memset(mBlob, 0, sizeof(mBlob));
}

daMpGhost_c::~daMpGhost_c() {
    // Every model lives in this actor's solid heap, which the framework frees after this body.
}

/* ---- create / heap -----------------------------------------------------------------------------
 */

static int daMpGhost_createHeap(fopAc_ac_c* i_this) {
    return static_cast<daMpGhost_c*>(i_this)->createHeap();
}

int daMpGhost_c::createHeap() {
    switch (mKind) {
    case l_kindDebug:
        return createHeapDebug();
    case l_kindArrow:
    case l_kindPellet:
        return createHeapArrow();
    case l_kindBomb:
    case l_kindWaterBomb:
    case l_kindBombling:
        return createHeapBomb();
    case l_kindBoomerang:
        return createHeapBoom();
    case l_kindRodBall:
        return createHeapCrod();
    default:
        return FALSE;
    }
}

int daMpGhost_c::create() {
    fopAcM_ct(this, daMpGhost_c);

    const u32 param = fopAcM_GetParam(this);
    mKind = static_cast<u8>(param & 0xFF);
    mAuthority = static_cast<u8>((param >> 8) & 0xFF);

    const objects::ObjKindInfo* info = objects::kind_info(mKind);
    if (info == NULL || !info->implemented || info->ghostHeap == 0) {
        Log.warn("[mp-obj] ghost: kind 0x{:02X} has no ghost in this build", mKind);
        return cPhs_ERROR_e;
    }
    // Default cull box, set before the heap so a kind's createHeap can replace it.
    // Generous: the arrow model runs ~110 units along +Z from its origin (d_a_arrow.cpp:1180).
    fopAcM_SetMin(this, -30.0f, -30.0f, -30.0f);
    fopAcM_SetMax(this, 30.0f, 30.0f, 130.0f);
    if (!fopAcM_entrySolidHeap(this, daMpGhost_createHeap, info->ghostHeap)) {
        Log.warn("[mp-obj] ghost: heap/model setup failed for kind 0x{:02X} ({}), heap 0x{:X}",
            mKind, info->name, info->ghostHeap);
        return cPhs_ERROR_e;
    }

    if (mpModel != NULL) {
        fopAcM_SetMtx(this, mpModel->getBaseTRMtx());
    }
    // Not something to lock on to, talk to or lift.
    attention_info.flags = 0;
    updateMatrix();

    // Last line: the channel's only trustworthy "creation succeeded" signal.
    mCreateComplete = true;
    return cPhs_COMPLEATE_e;
}

static int daMpGhost_Create(fopAc_ac_c* i_this) {
    return static_cast<daMpGhost_c*>(i_this)->create();
}

static int daMpGhost_Delete(daMpGhost_c* i_this) {
    i_this->~daMpGhost_c();
    return 1;
}

/* ---- execute / draw ----------------------------------------------------------------------------
 */

void daMpGhost_c::updateMatrix() {
    if (mpModel == NULL) {
        return;
    }
    mDoMtx_stack_c::transS(current.pos.x, current.pos.y, current.pos.z);
    mDoMtx_stack_c::ZXYrotM(shape_angle.x, shape_angle.y, shape_angle.z);
    mpModel->setBaseTRMtx(mDoMtx_stack_c::get());
}

void daMpGhost_c::setNetworkState(const cXyz& i_pos, const csXyz& i_angle, u8 i_flags,
    bool i_visible, const u8* i_blob, u8 i_blobLen) {
    current.pos = i_pos;
    old.pos = i_pos;
    shape_angle = i_angle;
    current.angle = i_angle;
    eyePos = i_pos;
    mNetFlags = i_flags;
    mVisible = i_visible;
    mHasPose = true;
    mBlobLen = i_blobLen > BLOB_MAX ? 0 : i_blobLen;
    if (mBlobLen != 0 && i_blob != NULL) {
        std::memcpy(mBlob, i_blob, mBlobLen);
    }
    updateMatrix();
}

void daMpGhost_c::onNetworkEvent(u8 i_event, const u8* i_blob, u8 i_blobLen) {
    mEventCount++;
    switch (mKind) {
    case l_kindDebug:
        Log.info("[mp-obj] ghost proc {} (debug) event {} (#{})", fopAcM_GetID(this), i_event,
            mEventCount);
        break;
    case l_kindArrow:
    case l_kindPellet:
        eventArrow(i_event, i_blob, i_blobLen);
        break;
    case l_kindBomb:
    case l_kindWaterBomb:
    case l_kindBombling:
        eventBomb(i_event, i_blob, i_blobLen);
        break;
    case l_kindBoomerang:
        eventBoom(i_event, i_blob, i_blobLen);
        break;
    case l_kindRodBall:
        eventCrod(i_event, i_blob, i_blobLen);
        break;
    default:
        break;
    }
}

int daMpGhost_c::execute() {
    mExecuteCount++;
    if (!mHasPose) {
        return 1;
    }
    switch (mKind) {
    case l_kindDebug:
        executeDebug();
        break;
    case l_kindArrow:
    case l_kindPellet:
        executeArrow();
        break;
    case l_kindBomb:
    case l_kindWaterBomb:
    case l_kindBombling:
        executeBomb();
        break;
    case l_kindBoomerang:
        executeBoom();
        break;
    case l_kindRodBall:
        executeCrod();
        break;
    default:
        break;
    }
    updateMatrix();
    return 1;
}

int daMpGhost_c::draw() {
    if (!mHasPose || !mVisible) {
        return 1;
    }
    switch (mKind) {
    case l_kindDebug:
        return drawDebug();
    case l_kindArrow:
    case l_kindPellet:
        return drawArrow();
    case l_kindBomb:
    case l_kindWaterBomb:
    case l_kindBombling:
        return drawBomb();
    case l_kindBoomerang:
        return drawBoom();
    case l_kindRodBall:
        return drawCrod();
    default:
        return 1;
    }
}

static int daMpGhost_Execute(daMpGhost_c* i_this) {
    return i_this->execute();
}

static int daMpGhost_Draw(daMpGhost_c* i_this) {
    return i_this->draw();
}

/* ---- debug kind (0x7F) -------------------------------------------------------------------------
 */

int daMpGhost_c::createHeapDebug() {
    /* The resident "Alink" archive, exactly as the game's own arrow loads it (d_a_arrow.cpp:24-46).
     * [unverified] that it stays mounted while the LOCAL player is a wolf; checked rather than
     * assumed, so a missing archive fails this create cleanly instead of dereferencing NULL. */
    const char* arc = daAlink_c::getAlinkArcName();
    if (dComIfG_getObjectResInfo(arc) == NULL) {
        Log.warn("[mp-obj] ghost: archive '{}' is not resident; no debug ghost", arc);
        return FALSE;
    }
    J3DModelData* modelData =
        static_cast<J3DModelData*>(dComIfG_getObjectRes(arc, l_arrowNormalBmd));
    if (modelData == NULL) {
        return FALSE;
    }
    mpModel = mDoExt_J3DModel__create(modelData, 0x80000, 0x11000084);
    return mpModel != NULL ? TRUE : FALSE;
}

void daMpGhost_c::executeDebug() {
    // Nothing to simulate: the pose is the stream's. The blob ([u16 age][u16 lifetime]) is kept for
    // the trace and for any later debug visual.
}

int daMpGhost_c::drawDebug() {
    // daArrow_c::draw's own lighting for a normal arrow (d_a_arrow.cpp:1073-1119).
    g_env_light.settingTevStruct(0, &current.pos, &tevStr);
    g_env_light.setLightTevColorType_MAJI(mpModel, &tevStr);
    mDoExt_modelUpdateDL(mpModel);
    return 1;
}

/* ---- profile -----------------------------------------------------------------------------------
 */

static DUSK_CONST actor_method_class l_daMpGhost_Method = {
    (process_method_func)daMpGhost_Create,
    (process_method_func)daMpGhost_Delete,
    (process_method_func)daMpGhost_Execute,
    (process_method_func)NULL,
    (process_method_func)daMpGhost_Draw,
};

DUSK_PROFILE actor_process_profile_definition DUSK_CONST g_profile_MP_GHOST = {
    /* Layer ID     */ fpcLy_CURRENT_e,
    /* List ID      */ 9,
    /* List Prio    */ fpcPi_CURRENT_e,
    /* Proc Name    */ fpcNm_MP_GHOST_e,
    /* Proc SubMtd  */ &g_fpcLf_Method.base,
    /* Size         */ sizeof(daMpGhost_c),
    /* Size Other   */ 0,
    /* Parameters   */ 0,
    /* Leaf SubMtd  */ &g_fopAc_Method.base,
    /* Draw Prio    */ fpcDwPi_ARROW_e,
    /* Actor SubMtd */ &l_daMpGhost_Method,
    /* Status       */ fopAcStts_UNK_0x40000_e | fopAcStts_NOPAUSE_e,
    /* Group        */ fopAc_ACTOR_e,
    /* Cull Type    */ fopAc_CULLBOX_CUSTOM_e,
};

/* ---- the engine seam (object_channel.hpp, namespace engine) ------------------------------------
 */

namespace dusk::mp::objects::engine {

bool local_link_pose(float pos[3], std::int16_t& yaw) {
    fopAc_ac_c* link = dComIfGp_getPlayer(0);
    if (link == NULL) {
        return false;
    }
    pos[0] = link->current.pos.x;
    pos[1] = link->current.pos.y;
    pos[2] = link->current.pos.z;
    yaw = link->shape_angle.y;
    return true;
}

std::uint32_t create_ghost(std::uint8_t kind, std::uint32_t authority, const ObjPose& pose) {
    fopAc_ac_c* link = dComIfGp_getPlayer(0);
    if (link == NULL) {
        return fpcM_ERROR_PROCESS_ID_e;
    }
    const cXyz pos(pose.pos[0], pose.pos[1], pose.pos[2]);
    const csXyz angle(pose.angle[0], pose.angle[1], pose.angle[2]);
    /* In the LOCAL Link's room, as the puppet is (player_bridge.cpp ensure_puppet): that room is
     * loaded by definition. A ghost a room unload takes anyway is recreated by the channel. */
    return fopAcM_create(fpcNm_MP_GHOST_e, daMpGhost_c::makeParam(kind, static_cast<u8>(authority)),
        &pos, fopAcM_GetRoomNo(link), &angle, NULL, -1);
}

namespace {
/// The live ghost behind `procId`, or NULL. Name-checked so a recycled id can never be cast.
daMpGhost_c* resolve(std::uint32_t procId, bool* o_known) {
    fopAc_ac_c* actor = NULL;
    const bool known = fopAcM_SearchByID(procId, &actor) != 0;
    if (o_known != NULL) {
        *o_known = known;
    }
    if (actor == NULL || fopAcM_GetName(actor) != fpcNm_MP_GHOST_e) {
        return NULL;
    }
    return static_cast<daMpGhost_c*>(actor);
}
}  // namespace

GhostLookup look_up_ghost(std::uint32_t procId) {
    bool known = false;
    daMpGhost_c* ghost = resolve(procId, &known);
    if (ghost == NULL) {
        return known ? GhostLookup::Creating : GhostLookup::Gone;
    }
    return ghost->createComplete() ? GhostLookup::Alive : GhostLookup::Creating;
}

void apply_ghost(std::uint32_t procId, const ObjPose& pose, bool visible, const KindBlob& blob) {
    daMpGhost_c* ghost = resolve(procId, NULL);
    if (ghost == NULL || !ghost->createComplete()) {
        return;
    }
    const cXyz pos(pose.pos[0], pose.pos[1], pose.pos[2]);
    const csXyz angle(pose.angle[0], pose.angle[1], pose.angle[2]);
    ghost->setNetworkState(pos, angle, pose.flags, visible, blob.bytes, blob.len);
}

void ghost_event(std::uint32_t procId, std::uint8_t event, const KindBlob& blob) {
    daMpGhost_c* ghost = resolve(procId, NULL);
    if (ghost == NULL || !ghost->createComplete()) {
        return;
    }
    ghost->onNetworkEvent(event, blob.bytes, blob.len);
}

void destroy_ghost(std::uint32_t procId) {
    if (procId == fpcM_ERROR_PROCESS_ID_e) {
        return;
    }
    // The fpc_ProcID overload copes with the actor already being gone (a room unload or a scene
    // change beat us to it), as destroy_puppet relies on.
    fopAcM_delete(procId);
}

/* ---- the Placed-identity census (spec §6 phase 0) ----------------------------------------------
 */

namespace {

constexpr borealis::Log Log{"dusk::mp"};

/// Playable ticks after a scene load before counting: stage actors are created over several
/// frames after Link appears.
const int l_censusDelayTicks = 60;

struct CensusEntry {
    s8 room;
    s8 homeRoom;
    u16 setId;
    s16 proc;
    fpc_ProcID id;
    cXyz home;
};

void* census_collect(void* i_actor, void* i_data) {
    fopAc_ac_c* actor = static_cast<fopAc_ac_c*>(i_actor);
    const s16 prof = fopAcM_GetProfName(actor);
    if (prof != fpcNm_Obj_Carry_e && prof != fpcNm_CSTATUE_e && prof != fpcNm_HORSE_e) {
        return NULL;
    }
    CensusEntry entry;
    entry.room = fopAcM_GetRoomNo(actor);
    entry.homeRoom = fopAcM_GetHomeRoomNo(actor);
    entry.setId = static_cast<u16>(fopAcM_GetSetId(actor));
    entry.proc = prof;
    entry.id = fopAcM_GetID(actor);
    entry.home = actor->home.pos;
    static_cast<std::vector<CensusEntry>*>(i_data)->push_back(entry);
    return NULL;  // keep iterating
}

const char* census_proc_name(s16 proc) {
    switch (proc) {
    case fpcNm_Obj_Carry_e:
        return "Obj_Carry";
    case fpcNm_CSTATUE_e:
        return "Cstatue";
    case fpcNm_HORSE_e:
        return "Horse";
    default:
        return "?";
    }
}

void run_census() {
    const char* stage = dComIfGp_getStartStageName();
    char name[9] = {};
    std::memcpy(name, stage, 8);
    const std::uint32_t hash = location_stage_hash(stage);

    std::vector<CensusEntry> entries;
    fopAcM_Search(census_collect, &entries);
    std::sort(entries.begin(), entries.end(), [](const CensusEntry& a, const CensusEntry& b) {
        return std::make_tuple(a.homeRoom, a.setId, a.proc, a.home.x, a.home.z) <
               std::make_tuple(b.homeRoom, b.setId, b.proc, b.home.x, b.home.z);
    });

    int duplicates = 0;
    for (std::size_t i = 0; i < entries.size(); i++) {
        const CensusEntry& e = entries[i];
        Log.info(
            "[mp-obj] census stage={} ({:08X}) room={} homeRoom={} setID={} proc=0x{:03X} ({}) "
            "home=({:.0f}, {:.0f}, {:.0f}) id={}",
            static_cast<const char*>(name), hash, static_cast<int>(e.room),
            static_cast<int>(e.homeRoom), e.setId, static_cast<unsigned>(static_cast<u16>(e.proc)),
            census_proc_name(e.proc), e.home.x, e.home.y, e.home.z, e.id);
        // 0xFFFF is "no id" (d_save.cpp:1735): those fall back to PlacedNoId and are not keys.
        if (i > 0 && e.setId != 0xFFFF && entries[i - 1].homeRoom == e.homeRoom &&
            entries[i - 1].setId == e.setId)
        {
            duplicates++;
            Log.warn(
                "[mp-obj] census DUPLICATE (homeRoom {}, setID {}): proc 0x{:03X} and 0x{:03X}",
                static_cast<int>(e.homeRoom), e.setId,
                static_cast<unsigned>(static_cast<u16>(entries[i - 1].proc)),
                static_cast<unsigned>(static_cast<u16>(e.proc)));
        }
    }
    Log.info("[mp-obj] census done stage={} ({:08X}) count={} duplicates={}",
        static_cast<const char*>(name), hash, entries.size(), duplicates);
}

}  // namespace

void census_tick() {
    static fpc_ProcID s_linkId = fpcM_ERROR_PROCESS_ID_e;
    static std::uint32_t s_stage = 0;
    static int s_wait = -1;

    fopAc_ac_c* link = dComIfGp_getPlayer(0);
    if (link == NULL) {
        return;
    }
    const std::uint32_t stage = location_stage_hash(dComIfGp_getStartStageName());
    const fpc_ProcID id = fopAcM_GetID(link);
    if (id != s_linkId || stage != s_stage) {
        // A new Link: every scene load (stage change, same-stage warp, respawn) rebuilds him.
        s_linkId = id;
        s_stage = stage;
        s_wait = l_censusDelayTicks;
    }
    if (s_wait < 0 || !world_is_playable()) {
        return;
    }
    if (--s_wait > 0) {
        return;
    }
    s_wait = -1;
    run_census();
}

}  // namespace dusk::mp::objects::engine

/* ---- per-kind ghosts (one file per phase-1 agent) ---------------------------------------------
 */

#include "d_a_mp_ghost_arrow.inc"
#include "d_a_mp_ghost_bomb.inc"
#include "d_a_mp_ghost_boom.inc"
#include "d_a_mp_ghost_crod.inc"
