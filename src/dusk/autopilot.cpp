#include "dusk/autopilot.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <SDL3/SDL_events.h>
#include <dolphin/pad.h>
#include <fmt/format.h>

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_player.h"
#include "d/d_camera.h"
#include "d/d_com_inf_game.h"
#include "d/d_meter2_info.h"
#include "dusk/logging.h"
#include "dusk/mods/item.hpp"
#include "dusk/multiplayer/replication/object_channel.hpp"
#include "dusk/multiplayer/replication/player_bridge.hpp"
#include "dusk/multiplayer/replication/world_bridge.hpp"
#include "dusk/multiplayer/session/network_manager.hpp"
#include "dusk/multiplayer/session/presence.hpp"
#include "dusk/multiplayer/session/world_sync.hpp"
#include "dusk/settings.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_camera_mng.h"

/**
 * A tiny interpreter over aurora's virtual pad.
 *
 * The unit of time is one call to mDoCPd_c::read(), i.e. one sim tick rather than one rendered
 * frame — the two differ, and the sim rate is the stable one (~30 Hz). Scripts are written in
 * ticks and the elapsed wall time is logged periodically so they can be calibrated by eye.
 */

namespace dusk::autopilot {
namespace {

constexpr borealis::Log Log{"dusk::autopilot"};

constexpr u32 kPort = 0;
/// Comfortably past fapGmHIO_getLROnValue(), so a scripted L or R reads as fully pressed.
constexpr u8 kTriggerAnalog = 200;
/// Bail out of a script whose loops contain no timed command, rather than hanging the game thread.
constexpr int kInstantCommandLimit = 1000;
/// Mash cadence. Long enough apart that menus with an open/close animation still register each
/// press as a separate one.
constexpr int kMashHoldTicks = 6;
constexpr int kMashGapTicks = 20;

enum class Op {
    Wait,
    WaitWorld,
    Tap,
    Mash,
    Hold,
    Stick,
    StickTo,
    Repeat,
    End,
    Say,
    Warp,
    Give,
    Equip,
    Clothes,
    Time,
    Arrows,
    Oil,
    Rupees,
    Shield,
    ShieldBurn,
    Quit,
    Nearest,
    TalkNear,
    GetItem,
    ObjTest,
    Transform,
    // Shared-world (M5) test verbs: see run_world_command.
    EvBit,
    Switch,
    SwitchOff,
    StageSwitch,
    Tbox,
    Keys,
    RupeesAdd,
    WorldLog,
    JoinPartner,
    ExpectBit,
    ExpectKeys,
    ExpectRupees,
    WaitEvent,
    /* --- batch 6: temp flags --- */
    DanSwitch,
    ZoneSwitch,
    ExpectDan,
    ExpectZone,
    TempLog,
    ZoneCountCheck,
};

struct Command {
    Op op = Op::Wait;
    /// Duration in ticks; for Repeat the iteration count; for WaitWorld the timeout (0 = forever).
    int frames = 0;
    /// Tap only: released ticks after the press, so the game sees a clean button edge.
    int gap = 0;
    u32 buttons = 0;
    s8 stickX = 0;
    s8 stickY = 0;
    /// StickTo only: the world XZ point to steer Link toward.
    double targetX = 0.0;
    double targetZ = 0.0;
    double magnitude = 1.0;
    std::string text;
    /// Warp only: spawn point, room and layer. The destination stage travels in `text`.
    int point = 0;
    int room = 0;
    int layer = 0;
    /// Repeat -> index of its End, and vice versa.
    std::size_t pair = 0;
    int line = 0;
    /// Nearest / TalkNear only: how far from Link to look, world units.
    double radius = 0.0;
};

struct LoopFrame {
    std::size_t repeatPc = 0;
    int remaining = 0;
};

std::vector<Command> sProgram;
std::vector<LoopFrame> sLoops;
std::size_t sPc = 0;
int sElapsed = 0;
/// StickTo: reached the target during the current command; stays let go until the next one.
bool sStickToArrived = false;
/// Transform: which way the current command is going (true = human -> wolf), fixed at its first
/// tick.
bool sTransformToWolf = false;
bool sActive = false;
bool sFinished = false;
bool sSawWorld = false;
std::uint64_t sTicks = 0;

struct ButtonName {
    const char* name;
    u32 mask;
};

constexpr ButtonName kButtons[] = {
    {"A", PAD_BUTTON_A},
    {"B", PAD_BUTTON_B},
    {"X", PAD_BUTTON_X},
    {"Y", PAD_BUTTON_Y},
    {"Z", PAD_TRIGGER_Z},
    {"L", PAD_TRIGGER_L},
    {"R", PAD_TRIGGER_R},
    {"START", PAD_BUTTON_START},
    {"UP", PAD_BUTTON_UP},
    {"DOWN", PAD_BUTTON_DOWN},
    {"LEFT", PAD_BUTTON_LEFT},
    {"RIGHT", PAD_BUTTON_RIGHT},
};

std::string to_upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

/// Accepts "A" or "A+B+START" so a script can express a button combination in one token.
bool parse_buttons(const std::string& token, u32& out) {
    out = 0;
    std::size_t start = 0;
    while (start <= token.size()) {
        const std::size_t plus = token.find('+', start);
        const std::string name = to_upper(
            token.substr(start, plus == std::string::npos ? std::string::npos : plus - start));
        if (name.empty()) {
            return false;
        }

        bool matched = false;
        for (const auto& button : kButtons) {
            if (name == button.name) {
                out |= button.mask;
                matched = true;
                break;
            }
        }
        if (!matched) {
            return false;
        }

        if (plus == std::string::npos) {
            break;
        }
        start = plus + 1;
    }
    return out != 0;
}

s8 stick_value(double value) {
    return static_cast<s8>(std::clamp(std::lround(value * 127.0), -127l, 127l));
}

bool parse_int(const std::string& token, int& out) {
    try {
        std::size_t consumed = 0;
        // Decimal, or hex with an explicit 0x (item numbers read better that way). Not base 0,
        // which would quietly read a leading-zero number as octal.
        const bool hex =
            token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X');
        const int value = std::stoi(token, &consumed, hex ? 16 : 10);
        if (consumed != token.size()) {
            return false;
        }
        out = value;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parse_double(const std::string& token, double& out) {
    try {
        std::size_t consumed = 0;
        const double value = std::stod(token, &consumed);
        if (consumed != token.size()) {
            return false;
        }
        out = value;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

/// Matches every Repeat with its End. A script that fails this is rejected whole: half-executing a
/// broken loop is worse than not running at all.
bool link_loops() {
    std::vector<std::size_t> open;
    for (std::size_t i = 0; i < sProgram.size(); i++) {
        if (sProgram[i].op == Op::Repeat) {
            open.push_back(i);
        } else if (sProgram[i].op == Op::End) {
            if (open.empty()) {
                Log.error("Line {}: 'end' without a matching 'repeat'", sProgram[i].line);
                return false;
            }
            sProgram[i].pair = open.back();
            sProgram[open.back()].pair = i;
            open.pop_back();
        }
    }

    if (!open.empty()) {
        Log.error("Line {}: 'repeat' without a matching 'end'", sProgram[open.back()].line);
        return false;
    }
    return true;
}

/**
 * "Link exists and the player is actually in control."
 *
 * Deliberately delegates to the multiplayer bridge rather than repeating the test. The two were
 * separate for one round of work and immediately drifted: the script believed it was in the world
 * during the title attract demo while replication did not, and every trace from that run had to be
 * thrown away. One definition, used by both.
 */
bool in_world() {
    return mp::world_is_playable();
}

/// Logged on the transition into the world, because "both instances are in the same stage" is a
/// precondition for every multiplayer test and is otherwise invisible.
void report_world_reached() {
    const char* stage = dComIfGp_getStartStageName();
    Log.info("In world after {} ticks, stage '{}' room {}", sTicks, stage != nullptr ? stage : "?",
        dComIfGp_getStartStageRoomNo());
}

void request_quit() {
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
}

void publish(const PADStatus& status) {
    PADSetVirtualStatus(kPort, &status);
}

/**
 * Test setup that no button sequence can reach: the item and outfit a script needs, so a run can
 * exercise a held item or the Zora-armor dive on any save. The same calls the Editor makes
 * (dusk/ui/editor.cpp: the item-slot picker, Equip X / Y, the clothes picker). They change the
 * save in memory, so they belong in test scripts only; a script run's save is a scratch copy.
 */
void run_setup_command(const Command& command) {
    if (dComIfGs_getSaveData() == nullptr || daPy_getPlayerActorClass() == nullptr) {
        Log.error(
            "[{}] line {}: give / equip / clothes need the world; skipped", sTicks, command.line);
        return;
    }
    switch (command.op) {
    case Op::Give:
        dComIfGs_setItem(command.point, static_cast<u8>(command.room));
        dComIfGs_onItemFirstBit(static_cast<u8>(command.room));
        Log.info("[{}] gave item 0x{:02X} in slot {}", sTicks, command.room, command.point);
        break;
    case Op::Equip:
        dComIfGs_setSelectItemIndex(command.point, static_cast<u8>(command.room));
        dComIfGp_setSelectItem(command.point);
        Log.info("[{}] equipped slot {} on {} (item 0x{:02X})", sTicks, command.room,
            command.point == 0 ? "X" : "Y", dComIfGp_getSelectItem(command.point));
        break;
    case Op::Clothes:
        dMeter2Info_setCloth(static_cast<u8>(command.room), false);
        daPy_getPlayerActorClass()->setClothesChange(0);
        Log.info("[{}] wearing outfit item 0x{:02X}", sTicks, command.room);
        break;
    case Op::Time:
        // 15 degrees an hour, as the save editor writes it. On a host the world clock sees this
        // as a set and sends it on; on a guest it is a TimeOverride.
        dComIfGs_setTime(static_cast<f32>(command.room) * 15.0f);
        Log.info("[{}] set the time to {:02}:00", sTicks, command.room);
        break;
    case Op::Arrows:
        // What item_func_BOW does on top of the slot: a bow with no arrows cannot nock one.
        if (dComIfGs_getArrowMax() < command.room) {
            dComIfGs_setArrowMax(static_cast<u8>(command.room));
        }
        dComIfGs_setArrowNum(static_cast<u8>(command.room));
        Log.info("[{}] arrows set to {}", sTicks, command.room);
        break;
    case Op::Oil:
        // What item_func_KANTERA does on top of the slot: an empty lantern will not light.
        if (dComIfGs_getMaxOil() < command.room) {
            dComIfGs_setMaxOil(static_cast<u16>(command.room));
        }
        dComIfGs_setOil(static_cast<u16>(command.room));
        Log.info("[{}] lantern oil set to {}", sTicks, command.room);
        break;
    case Op::Rupees:
        // The wallet, for the magic armour's drain: 0 is its power-down, any other its power-up.
        dComIfGs_setRupee(static_cast<u16>(command.room));
        Log.info("[{}] rupees set to {}", sTicks, command.room);
        break;
    case Op::Shield:
        // The collection menu's own three calls (d_menu_collect.cpp:1376-1406).
        dComIfGs_onItemFirstBit(static_cast<u8>(command.room));
        dMeter2Info_setShield(static_cast<u8>(command.room), false);
        daAlink_getAlinkActorClass()->setShieldChange();
        /* His shield reload (loadShieldModelDVD) only runs from statusWindowExecute, while the
         * collection menu pauses him (d_a_alink_swindow.inc:300-306). Outside the menu the swap
         * never lands, so checkShieldDraw() stays false and no shield is drawn. Do NOT pump it
         * from here: it nulls mShieldModel while his execute still runs, and setCollision then
         * dereferences it (measured crash). Issue `shield` BEFORE a `warp` instead: the new Link
         * loads the shield normally and the old one finishes the reload in daAlink_Delete. */
        Log.info("[{}] wearing shield item 0x{:02X}", sTicks, command.room);
        break;
    case Op::ShieldBurn: {
        // A fire hit on the wooden shield: his own guard-side write (d_a_alink_guard.inc:107-108),
        // so the timer, the tint, the fire and the burn-out all run as they do in play.
        daAlink_c* alink = daAlink_getAlinkActorClass();
        if (alink != nullptr && daPy_py_c::checkWoodShieldEquip() && alink->field_0x2fcb == 0) {
            alink->field_0x2fcb = 120;
            Log.info("[{}] wooden shield set alight", sTicks);
        } else {
            Log.error("[{}] line {}: shieldburn needs a wooden shield that is not already burning",
                sTicks, command.line);
        }
        break;
    }
    default:
        break;
    }
}

/* --- nearest / talknear / getitem: drive the sender's events for the puppet-event tests ------ */

/// TalkNear: the NPC picked on the command's first tick.
fpc_ProcID sTalkTarget = fpcM_ERROR_PROCESS_ID_e;

struct ActorHit {
    fopAc_ac_c* actor = nullptr;
    double distance = 0.0;
};

struct ActorSearch {
    const fopAc_ac_c* from = nullptr;
    /// Upper-cased stage name ("BOU", "KANBAN"...), "*" for any actor, or "TALK" for any actor that
    /// can be talked to right now.
    std::string name;
    /// A profile name typed as a number ("0x0FB"), or -1.
    int profName = -1;
    double radius = 0.0;
    std::vector<ActorHit> hits;
};

/**
 * "Can be talked to right now", as the game asks it: the NPC base's own test for its talk target
 * (d_a_npc.cpp:2053) is SPEAK or TALK in attention_info.flags. NPCs clear the flags whenever they
 * cannot talk (d_a_npc_moi.cpp:624, d_a_npc_bou.cpp:661).
 */
bool actor_is_talkable(const fopAc_ac_c* actor) {
    return (actor->attention_info.flags & (fopAc_AttnFlag_SPEAK_e | fopAc_AttnFlag_TALK_e)) != 0;
}

void* collect_actor(void* actorPtr, void* data) {
    auto* search = static_cast<ActorSearch*>(data);
    auto* actor = static_cast<fopAc_ac_c*>(actorPtr);
    if (actor == search->from || !fopAcM_IsExecuting(fopAcM_GetID(actor))) {
        return nullptr;
    }
    const s16 prof = fopAcM_GetProfName(actor);
    // Never ourselves or a partner's puppet: neither is something to walk up to and talk to.
    if (prof == fpcNm_ALINK_e || prof == fpcNm_REMOTE_PLAYER_e) {
        return nullptr;
    }
    bool match;
    if (search->name == "TALK") {
        match = actor_is_talkable(actor);
    } else if (search->name == "*") {
        match = true;
    } else if (search->profName >= 0) {
        match = prof == search->profName;
    } else {
        match = to_upper(fopAcM_getProcNameString(actor)) == search->name;
    }
    if (!match) {
        return nullptr;
    }
    const double dx = actor->current.pos.x - search->from->current.pos.x;
    const double dy = actor->current.pos.y - search->from->current.pos.y;
    const double dz = actor->current.pos.z - search->from->current.pos.z;
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance <= search->radius) {
        search->hits.push_back(ActorHit{actor, distance});
    }
    return nullptr;  // keep iterating: the whole list is wanted, sorted afterwards
}

/// Every matching actor within the radius, nearest first.
std::vector<ActorHit> find_actors(const std::string& name, double radius) {
    ActorSearch search;
    search.from = dComIfGp_getPlayer(0);
    if (search.from == nullptr) {
        return {};
    }
    search.name = to_upper(name);
    int number = 0;
    if (parse_int(name, number)) {
        search.profName = number;
    }
    search.radius = radius;
    fopAcM_Search(collect_actor, &search);
    std::sort(search.hits.begin(), search.hits.end(),
        [](const ActorHit& a, const ActorHit& b) { return a.distance < b.distance; });
    return search.hits;
}

void log_actor_hit(const char* what, const ActorHit& hit) {
    const fopAc_ac_c* actor = hit.actor;
    Log.info("[{}] {} '{}' prof 0x{:03X} id {} at ({:.0f}, {:.0f}, {:.0f}) room {}, {:.0f} units "
             "away, attention flags 0x{:08X}{}",
        sTicks, what, fopAcM_getProcNameString(actor),
        static_cast<unsigned>(static_cast<u16>(fopAcM_GetProfName(actor))), fopAcM_GetID(actor),
        actor->current.pos.x, actor->current.pos.y, actor->current.pos.z,
        static_cast<int>(fopAcM_GetRoomNo(actor)), hit.distance, actor->attention_info.flags,
        actor_is_talkable(actor) ? " (talkable)" : "");
}

/**
 * `nearest <name|*|talk|0xPROF> [radius]`: log up to eight matching actors, nearest first, and the
 * `stickto` line that walks to the nearest. Read-only: nothing in the game changes.
 */
void run_nearest(const Command& command) {
    if (!in_world()) {
        Log.error("[{}] line {}: nearest needs the world; skipped", sTicks, command.line);
        return;
    }
    const std::vector<ActorHit> hits = find_actors(command.text, command.radius);
    if (hits.empty()) {
        Log.warn(
            "[{}] nearest '{}': nothing within {:.0f} units", sTicks, command.text, command.radius);
        return;
    }
    for (std::size_t i = 0; i < hits.size() && i < 8; i++) {
        log_actor_hit(i == 0 ? "nearest" : "  also", hits[i]);
    }
    Log.info("[{}] nearest '{}': {} found; walk there with: stickto {:.0f} {:.0f} 120", sTicks,
        command.text, hits.size(), hits[0].actor->current.pos.x, hits[0].actor->current.pos.z);
}

/**
 * `getitem <itemNo>`: the real item-get event, fanfare and all, through Dusk's own give queue:
 * the same path a randomizer's forced give takes (mods/item_gives.cpp dispatch_demo_give: the
 * DEFAULT_GETITEM event with setGtItm). The queue waits until Link is in a plain wait or move
 * proc with no event running (safe_to_dispatch), so this line returns at once and the event
 * starts a few ticks later. Follow it with a `wait` and `tap A` to page out the text box.
 *
 * Like `give`, it grants the item into THIS instance's save in memory. It runs only on the
 * instance whose script has it. The partner's puppet spawns nothing (puppet-events.md §5.4).
 * A unique check name per command keeps two queued gives from sharing one commit.
 */
void run_getitem(const Command& command) {
    if (!in_world()) {
        Log.error("[{}] line {}: getitem needs the world; skipped", sTicks, command.line);
        return;
    }
    static int s_getItemCount = 0;
    const std::string name = "autopilot:getitem:" + std::to_string(++s_getItemCount);
    dusk::mods::item_check_enqueue_deferred(name.c_str(), static_cast<u8>(command.room));
    Log.info("[{}] queued item get 0x{:02X} ('{}'); the event starts once Link is free", sTicks,
        command.room, name);
}

/* ---- Shared-world (M5) test verbs -------------------------------------------------------------
 * Flag and counter writes plus PASS/FAIL reads, for two-instance world-sync tests
 * (future-plans/mp-m4-m5-world.md Part III). Writes go through the same dComIfGs_* / dComIfGp_*
 * calls actors use, so the world diff sees them exactly as it sees gameplay. Reads go to the
 * engine directly, never through the multiplayer layer, so a PASS means the flag is really set
 * where the game will look for it. Like `give`, every write changes the save in memory; a script
 * run's save is a scratch copy. */

/// The `tbl` argument value meaning "the stage table I am standing in, via the room-aware path".
constexpr int kLiveTbl = -1;
/// Number of save stage tables (dSv_save_c::STAGE_MAX).
constexpr int kStageTables = 32;
/// Switch numbers dSv_info_c::onSwitch routes (memory 0x00-0x7F, dan, zone, one-zone: to 0xEF).
constexpr int kSwitchLimit = dSv_info_c::MEMORY_SWITCH + dSv_info_c::DAN_SWITCH +
                             dSv_info_c::ZONE_SWITCH + dSv_info_c::ONEZONE_SWITCH;
/// Item bits dSv_info_c::onItem routes (dan 0x00-0x7F, memory 0x80-0x9F, zone, one-zone).
constexpr int kItemLimit = dSv_info_c::MEMORY_ITEM + dSv_info_c::DAN_ITEM + dSv_info_c::ZONE_ITEM +
                           dSv_info_c::ONEZONE_ITEM;
/// dSv_memBit_c::mTbox[2].
constexpr int kTboxLimit = 64;

int sExpectPass = 0;
int sExpectFail = 0;

/// An event flag as the decomp writes them (d_save_bit_labels.inc): byte << 8 | mask, in hex,
/// with or without the 0x.
bool parse_event_flag(const std::string& token, int& out) {
    try {
        std::size_t consumed = 0;
        const int value = std::stoi(token, &consumed, 16);
        if (consumed != token.size() || value < 0 || value > 0xFFFF || (value & 0xFF) == 0) {
            return false;
        }
        out = value;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

/// A stage table: 0-31, or `live` for the one this instance is standing in.
bool parse_stage_table(const std::string& token, int& out) {
    if (to_upper(token) == "LIVE") {
        out = kLiveTbl;
        return true;
    }
    return parse_int(token, out) && out >= 0 && out < kStageTables;
}

/// The stage table mMemory belongs to now, or -1 when there is no stage (title, mid-load).
int live_stage_table() {
    stage_stag_info_class* info = dComIfGp_getStageStagInfo();
    return info != nullptr ? dStage_stagInfo_GetSaveTbl(info) : -1;
}

int current_room() {
    return dComIfGp_roomControl_getStayNo();
}

/// Reads one bit for `expectbit`. Returns false (and says why) for a read that cannot be made.
bool read_world_bit(const Command& command, bool& got, const char*& source) {
    const std::string domain = to_upper(command.text);
    const int tbl = command.room;
    const int no = command.point;
    const int live = live_stage_table();
    dSv_save_c& save = g_dComIfG_gameInfo.info.getSavedata();

    if (domain == "EVENT") {
        got = dComIfGs_isEventBit(static_cast<u16>(no)) != FALSE;
        source = "mEvent";
        return true;
    }
    if (domain == "SWITCH") {
        if (tbl == kLiveTbl) {
            if (live < 0) {
                return false;
            }
            got = dComIfGs_isSwitch(no, current_room()) != FALSE;
            source = "live";
            return true;
        }
        // The game's own other-stage reader: mMemory if tbl is the live one, else mSave[tbl].
        got = dComIfGs_isStageSwitch(tbl, no) != FALSE;
        source = tbl == live ? "mMemory" : "mSave";
        return true;
    }
    if (domain == "TBOX") {
        if (tbl == kLiveTbl) {
            if (live < 0) {
                return false;
            }
            got = dComIfGs_isTbox(no) != FALSE;
            source = "mMemory";
            return true;
        }
        got = dComIfGs_isStageTbox(tbl, no) != FALSE;
        source = tbl == live ? "mMemory" : "mSave";
        return true;
    }
    if (domain == "ITEM") {
        if (tbl == kLiveTbl) {
            if (live < 0) {
                return false;
            }
            got = dComIfGs_isItem(no, current_room());
            source = "live";
            return true;
        }
        // A numbered table holds only the saved item bits (0x80-0x9F); the parser enforces it.
        const int bit = no - dSv_info_c::MEMORY_ITEM;
        if (tbl == live) {
            got = dComIfGs_isSaveItem(bit) != FALSE;
            source = "mMemory";
        } else {
            got = save.getSave(tbl).getBit().isItem(bit) != FALSE;
            source = "mSave";
        }
        return true;
    }
    return false;
}

/// One grep-able line per expectation: "[autopilot] <verb> PASS|FAIL <detail> ...".
void log_expect(bool pass, const char* verb, const std::string& detail) {
    (pass ? sExpectPass : sExpectFail)++;
    if (pass) {
        Log.info("[autopilot] {} PASS {} tick={} (pass {} fail {} so far)", verb, detail, sTicks,
            sExpectPass, sExpectFail);
    } else {
        Log.error("[autopilot] {} FAIL {} tick={} (pass {} fail {} so far)", verb, detail, sTicks,
            sExpectPass, sExpectFail);
    }
}

/**
 * Every shared-world verb. All are instant (no ticks of their own); a script `wait`s around them.
 *
 * Counters (`keys`, `rupeesadd`) go into the pending play counters, as an item get does
 * (dComIfGp_setItem*Count); dMeter2 commits them into the save a few ticks later (moveKey,
 * moveRupee), clamped. `expectkeys` / `expectrupees` read the COMMITTED value, so a script should
 * wait ~30 ticks after a counter write before it expects on it.
 */
void run_world_command(const Command& command) {
    switch (command.op) {
    case Op::EvBit:
        if (command.layer != 0) {
            dComIfGs_onEventBit(static_cast<u16>(command.point));
        } else {
            dComIfGs_offEventBit(static_cast<u16>(command.point));
        }
        Log.info(
            "[{}] event flag 0x{:04X} {}", sTicks, command.point, command.layer ? "on" : "off");
        break;

    case Op::Switch:
    case Op::SwitchOff: {
        if (live_stage_table() < 0) {
            Log.error("[{}] line {}: switch needs a stage; skipped", sTicks, command.line);
            break;
        }
        const int room = command.room >= 0 ? command.room : current_room();
        if (command.op == Op::Switch) {
            dComIfGs_onSwitch(command.point, room);
        } else {
            dComIfGs_offSwitch(command.point, room);
        }
        Log.info("[{}] switch 0x{:02X} {} (room {}, stage table {})", sTicks, command.point,
            command.op == Op::Switch ? "on" : "off", room, live_stage_table());
        break;
    }

    case Op::StageSwitch:
        // Writes mSave[tbl] always and mMemory too when tbl is the live table
        // (d_com_inf_game.cpp:1518). Aimed at a table you are NOT in, it is the other-stage path.
        dComIfGs_onStageSwitch(command.room, command.point);
        Log.info("[{}] stage switch 0x{:02X} on in stage table {} (live table {})", sTicks,
            command.point, command.room, live_stage_table());
        break;

    case Op::Tbox:
        if (live_stage_table() < 0) {
            Log.error("[{}] line {}: tbox needs a stage; skipped", sTicks, command.line);
            break;
        }
        dComIfGs_onTbox(command.point);
        Log.info("[{}] chest {} marked open in stage table {}", sTicks, command.point,
            live_stage_table());
        break;

    case Op::Keys:
        Log.info("[{}] small keys {:+} queued (now {} in stage table {}; dMeter2 commits it)",
            sTicks, command.point, dComIfGs_getKeyNum(), live_stage_table());
        dComIfGp_setItemKeyNumCount(static_cast<s16>(command.point));
        break;

    case Op::RupeesAdd:
        Log.info("[{}] rupees {:+} queued (now {}; dMeter2 commits it)", sTicks, command.point,
            dComIfGs_getRupee());
        dComIfGp_setItemRupeeCount(static_cast<s32>(command.point));
        break;

    case Op::WorldLog: {
        const char* tag = command.text.empty() ? "autopilot" : command.text.c_str();
        Log.info("[{}] worldlog '{}': rupees {}, keys {}, stage table {}, room {}", sTicks, tag,
            dComIfGs_getRupee(), dComIfGs_getKeyNum(), live_stage_table(), current_room());
        mp::world_sync::log_state(tag);
        break;
    }

    case Op::JoinPartner:
        Log.info("[{}] joinpartner: asking presence to warp to the first remote player", sTicks);
        mp::presence::join_partner(mp::presence::first_remote_player_id());
        break;

    case Op::ExpectBit: {
        bool got = false;
        const char* source = "?";
        const std::string tblName = command.text == "EVENT"  ? std::string("-") :
                                    command.room == kLiveTbl ? std::string("live") :
                                                               std::to_string(command.room);
        if (!read_world_bit(command, got, source)) {
            log_expect(false, "expectbit",
                fmt::format("domain={} tbl={} no=0x{:X} want={} got=? (no stage to read from)",
                    command.text, tblName, command.point, command.layer));
            break;
        }
        log_expect(got == (command.layer != 0), "expectbit",
            fmt::format("domain={} tbl={} no=0x{:X} want={} got={} src={} live={}", command.text,
                tblName, command.point, command.layer, got ? 1 : 0, source, live_stage_table()));
        break;
    }

    case Op::ExpectKeys: {
        const int got = dComIfGs_getKeyNum();
        log_expect(got == command.point, "expectkeys",
            fmt::format("want={} got={} tbl={}", command.point, got, live_stage_table()));
        break;
    }

    case Op::ExpectRupees: {
        const int got = dComIfGs_getRupee();
        log_expect(got == command.point, "expectrupees",
            fmt::format("want={} got={}", command.point, got));
        break;
    }

    default:
        break;
    }
}

/* --- batch 6: temp flags ---
 * Dungeon bits (mDan, game switches 0x80-0xBF) and room bits (mZone, 0xC0-0xEF) for the W-B
 * temp-bit tests (spec II.7). Numbers are relative to their range: `danswitch 5` is game switch
 * 0x85; `zoneswitch N` is 0xC0+N, so N 0-31 is a zone switch (lives as long as the zone) and
 * N 32-47 a one-zone switch (cleared by every room change of the player who holds it). Zone verbs
 * act on the room Link stands in. Writes go through dComIfGs_onSwitch like an actor's; reads go
 * to the engine, never the multiplayer layer. */

/// Dan switches (dSv_danBit_c::mSwitch[2]) and zone + one-zone switches.
constexpr int kDanSwitchCount = dSv_info_c::DAN_SWITCH;
constexpr int kZoneSwitchCount = dSv_info_c::ZONE_SWITCH + dSv_info_c::ONEZONE_SWITCH;
constexpr int kDanSwitchBase = dSv_info_c::MEMORY_SWITCH;
constexpr int kZoneSwitchBase = dSv_info_c::MEMORY_SWITCH + dSv_info_c::DAN_SWITCH;

void run_temp_command(const Command& command) {
    const int tbl = live_stage_table();
    const int room = current_room();
    const int zoneNo = mp::world_bridge::zone_no(room);
    switch (command.op) {
    case Op::DanSwitch:
        if (tbl < 0) {
            Log.error("[{}] line {}: danswitch needs a stage; skipped", sTicks, command.line);
            break;
        }
        if (command.layer != 0) {
            dComIfGs_onSwitch(kDanSwitchBase + command.point, room);
        } else {
            dComIfGs_offSwitch(kDanSwitchBase + command.point, room);
        }
        Log.info("[{}] dan switch {} (0x{:02X}) {} (stage table {}, mDan stage {})", sTicks,
            command.point, kDanSwitchBase + command.point, command.layer ? "on" : "off", tbl,
            mp::world_bridge::dan_stage_no());
        break;

    case Op::ZoneSwitch:
        // dSv_info_c::onSwitch asserts a zone exists and would index mZone[-1] without one.
        if (tbl < 0 || zoneNo < 0) {
            Log.error("[{}] line {}: zoneswitch needs a room with a zone (room {}, zone {}); "
                      "skipped",
                sTicks, command.line, room, zoneNo);
            break;
        }
        if (command.layer != 0) {
            dComIfGs_onSwitch(kZoneSwitchBase + command.point, room);
        } else {
            dComIfGs_offSwitch(kZoneSwitchBase + command.point, room);
        }
        Log.info("[{}] zone switch {} (0x{:02X}, {}) {} (room {}, zone {}, stage table {})",
            sTicks, command.point, kZoneSwitchBase + command.point,
            command.point < dSv_info_c::ZONE_SWITCH ? "zone" : "one-zone",
            command.layer ? "on" : "off", room, zoneNo, tbl);
        break;

    case Op::ExpectDan: {
        if (tbl < 0) {
            log_expect(false, "expectdan",
                fmt::format("no={} want={} got=? (no stage)", command.point, command.layer));
            break;
        }
        const bool got = dComIfGs_isSwitch(kDanSwitchBase + command.point, room) != FALSE;
        log_expect(got == (command.layer != 0), "expectdan",
            fmt::format("no={} want={} got={} tbl={} danStage={}", command.point, command.layer,
                got ? 1 : 0, tbl, mp::world_bridge::dan_stage_no()));
        break;
    }

    case Op::ExpectZone: {
        if (tbl < 0 || zoneNo < 0) {
            log_expect(false, "expectzone",
                fmt::format("no={} want={} got=? (room {} has no zone here)", command.point,
                    command.layer, room));
            break;
        }
        const bool got = dComIfGs_isSwitch(kZoneSwitchBase + command.point, room) != FALSE;
        log_expect(got == (command.layer != 0), "expectzone",
            fmt::format("no={} want={} got={} tbl={} room={} zone={}", command.point,
                command.layer, got ? 1 : 0, tbl, room, zoneNo));
        break;
    }

    case Op::TempLog: {
        const char* tag = command.text.empty() ? "autopilot" : command.text.c_str();
        Log.info("[{}] templog '{}': stage table {}, room {}", sTicks, tag, tbl, room);
        mp::world_sync::log_temp(tag);
        break;
    }

    case Op::ZoneCountCheck:
        /* What a room change does to the zones (dStage_RoomCheck -> zoneCountCheck), aimed at the
         * room Link already stands in: every zone's one-zone bits are cleared. That is the H8
         * lifecycle path; reaching it by walking needs a room boundary the script cannot find.
         * In a non-field stage it also counts every zone down once (2 -> 1 after a fresh load), so
         * one call never removes a zone; don't call it twice in a row there. */
        if (tbl < 0) {
            Log.error("[{}] line {}: zonecountcheck needs a stage; skipped", sTicks, command.line);
            break;
        }
        Log.info("[{}] zonecountcheck: room-change lifecycle on room {} (zone {})", sTicks, room,
            zoneNo);
        dComIfGp_roomControl_zoneCountCheck(room);
        break;

    default:
        break;
    }
}

/// Steer toward a world XZ point: the same mapping as the StickTo case in tick().
void steer_toward(PADStatus& status, double targetX, double targetZ, double maxMagnitude) {
    fopAc_ac_c* link = dComIfGp_getPlayer(0);
    auto* camera = dComIfGp_getCamera(dComIfGp_getPlayerCameraID(0));
    if (link == nullptr || camera == nullptr) {
        return;
    }
    const double dx = targetX - link->current.pos.x;
    const double dz = targetZ - link->current.pos.z;
    const double distance = std::sqrt(dx * dx + dz * dz);
    const double kToRad = 3.14159265358979 / 32768.0;
    double stickAngle = std::atan2(dx, dz) - dCam_getControledAngleY(camera) * kToRad;
    if (dusk::getSettings().game.enableMirrorMode) {
        stickAngle = -stickAngle;
    }
    const double magnitude = std::min(maxMagnitude, std::max(0.45, distance / 100.0));
    status.stickX = stick_value(-std::sin(stickAngle) * magnitude);
    status.stickY = stick_value(std::cos(stickAngle) * magnitude);
}

/// TalkNear: close enough to press A. NPC talk ranges are well past this; under it Link would
/// mostly be pushing against the NPC's collider.
constexpr double kTalkReach = 120.0;

/// Runs every command that takes no time (say/repeat/end/quit), leaving sPc on a timed one.
/// Returns false once the program is over.
/// Ticks a `warp` has been held so far, waiting for a guest's world install (see Op::Warp).
int sWarpHeld = 0;
constexpr int kWarpHoldLimit = 1800;

/// A guest's scripted warp waits until the host's world is installed. The install reloads the
/// current stage in place (world_bridge reload_in_place), and the game starts that stage change on
/// the next tick; a warp issued meanwhile lands mid-fade and is dropped, so the guest silently
/// stays where it booted and every check after it runs in the wrong stage. Measured 2026-11-10:
/// both batch-5 pair tests lost the guest's Ordon warp this way. A player can't hit this (no one
/// warps within a second of joining), so it is held here, in the harness, not in the game.
bool warp_must_wait_for_world_sync() {
    const mp::NetworkManager& net = mp::network_manager();
    const bool settled =
        mp::world_sync::synced() && !mp::world_bridge::reload_pending() && in_world();
    if (net.role() != mp::Role::Client || settled) {
        if (sWarpHeld > 0) {
            Log.info("[{}] warp released after {} tick(s): the host's world is installed", sTicks,
                sWarpHeld);
        }
        sWarpHeld = 0;
        return false;
    }
    if (sWarpHeld == 0) {
        Log.info("[{}] warp held: waiting for the host's world to be installed first", sTicks);
    }
    if (++sWarpHeld > kWarpHoldLimit) {
        Log.warn("[{}] warp: still no world sync after {} ticks; warping anyway", sTicks,
            kWarpHoldLimit);
        sWarpHeld = 0;
        return false;
    }
    return true;
}

bool settle_program_counter() {
    for (int guard = 0; guard < kInstantCommandLimit; guard++) {
        if (sPc >= sProgram.size()) {
            return false;
        }

        const Command& command = sProgram[sPc];
        switch (command.op) {
        case Op::Say:
            Log.info("[{}] {}", sTicks, command.text);
            sPc++;
            continue;

        case Op::Repeat:
            if (command.frames <= 0) {
                sPc = command.pair + 1;
            } else {
                sLoops.push_back(LoopFrame{sPc, command.frames});
                sPc++;
            }
            continue;

        case Op::End:
            if (!sLoops.empty() && sLoops.back().repeatPc == command.pair) {
                if (--sLoops.back().remaining > 0) {
                    sPc = command.pair + 1;
                    continue;
                }
                sLoops.pop_back();
            }
            sPc++;
            continue;

        case Op::Warp:
            /* The same one call the warp window makes (dusk/ui/warp.cpp:325) — everything else in
             * that file is the picker. Exposed to scripts because a STAGE CHANGE at a known tick
             * is otherwise unreachable from the harness: the only other way to leave a stage is to
             * walk into a load zone, which needs map knowledge the script does not have and
             * timing it cannot control.
             *
             * That matters beyond convenience. A stage change is what cancels outstanding actor
             * create requests, and "a puppet deleted while its private archive mount is still in
             * flight" is a real use-after-free in ~dRes_info_c that no amount of ordinary play
             * reproduces here, because every mount on this machine lands within one tick. With
             * this verb and DUSK_DVD_MOUNT_DELAY_MS the window is seconds wide and the timing is
             * a script line.
             *
             * Not gated on in_world(): a warp issued before the world exists is the game's own
             * problem to reject, and pretending otherwise would hide a real ordering bug. */
            if (warp_must_wait_for_world_sync()) {
                return true;  // the timed switch holds it (case Op::Warp) and settle retries
            }
            Log.info("[{}] warping to stage '{}' point {} room {} layer {}", sTicks, command.text,
                command.point, command.room, command.layer);
            dComIfGp_setNextStage(command.text.c_str(), static_cast<s16>(command.point),
                static_cast<s8>(command.room), static_cast<s8>(command.layer));
            sPc++;
            continue;

        case Op::Give:
        case Op::Equip:
        case Op::Clothes:
        case Op::Time:
        case Op::Arrows:
        case Op::Oil:
        case Op::Rupees:
        case Op::Shield:
        case Op::ShieldBurn:
            run_setup_command(command);
            sPc++;
            continue;

        case Op::Quit:
            Log.info("Script asked to quit after {} ticks", sTicks);
            request_quit();
            return false;

        case Op::ObjTest:
            if (!dusk::mp::objects::start_debug_object(command.frames)) {
                Log.error("[{}] line {}: objtest: no debug object (no session, or no Link)", sTicks,
                    command.line);
            }
            sPc++;
            continue;

        case Op::Nearest:
            run_nearest(command);
            sPc++;
            continue;

        case Op::GetItem:
            run_getitem(command);
            sPc++;
            continue;

        case Op::EvBit:
        case Op::Switch:
        case Op::SwitchOff:
        case Op::StageSwitch:
        case Op::Tbox:
        case Op::Keys:
        case Op::RupeesAdd:
        case Op::WorldLog:
        case Op::JoinPartner:
        case Op::ExpectBit:
        case Op::ExpectKeys:
        case Op::ExpectRupees:
            run_world_command(command);
            sPc++;
            continue;

        /* --- batch 6: temp flags --- */
        case Op::DanSwitch:
        case Op::ZoneSwitch:
        case Op::ExpectDan:
        case Op::ExpectZone:
        case Op::TempLog:
        case Op::ZoneCountCheck:
            run_temp_command(command);
            sPc++;
            continue;

        default:
            return true;
        }
    }

    Log.error("Script made no progress in {} commands; stopping", kInstantCommandLimit);
    return false;
}

}  // namespace

void load_script(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        Log.error("Could not open autopilot script '{}'", path.string());
        return;
    }

    sProgram.clear();
    std::string line;
    int lineNumber = 0;
    bool ok = true;

    while (std::getline(file, line)) {
        lineNumber++;

        // Trailing \r, because these scripts get edited on Windows and read on everything.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (const std::size_t comment = line.find_first_of("#;"); comment != std::string::npos) {
            line.erase(comment);
        }

        std::istringstream tokens(line);
        std::string verb;
        if (!(tokens >> verb)) {
            continue;
        }
        verb = to_upper(verb);

        Command command;
        command.line = lineNumber;
        std::string a;
        std::string b;
        std::string c;
        std::string d;
        tokens >> a >> b >> c >> d;

        if (verb == "WAIT") {
            command.op = Op::Wait;
            ok = parse_int(a, command.frames) && command.frames > 0;
        } else if (verb == "WAITWORLD") {
            command.op = Op::WaitWorld;
            // No argument means wait forever, which is what a boot script usually wants.
            ok = a.empty() || parse_int(a, command.frames);
        } else if (verb == "TAP") {
            command.op = Op::Tap;
            command.frames = 4;
            command.gap = 10;
            ok = parse_buttons(a, command.buttons) && (b.empty() || parse_int(b, command.frames)) &&
                 (c.empty() || parse_int(c, command.gap));
        } else if (verb == "MASH") {
            command.op = Op::Mash;
            // Second argument is a TIMEOUT, not a duration: this ends when the world appears.
            command.frames = 3600;
            ok = parse_buttons(a, command.buttons) && (b.empty() || parse_int(b, command.frames)) &&
                 command.frames > 0;
        } else if (verb == "HOLD") {
            command.op = Op::Hold;
            ok = parse_buttons(a, command.buttons) && parse_int(b, command.frames) &&
                 command.frames > 0;
        } else if (verb == "STICK") {
            command.op = Op::Stick;
            double x = 0.0;
            double y = 0.0;
            // The optional fourth argument is a button combination held for the whole push. Every
            // tick's PADStatus is built from one command, so without this there is no way to
            // express "guard while walking" — the two halves would have to be separate commands
            // and would run one after the other.
            ok = parse_double(a, x) && parse_double(b, y) && parse_int(c, command.frames) &&
                 command.frames > 0 && (d.empty() || parse_buttons(d, command.buttons));
            command.stickX = stick_value(x);
            command.stickY = stick_value(y);
        } else if (verb == "STICKTO") {
            // `stickto X Z frames [buttons]`: steer toward a world point, re-aimed every tick.
            // Plain `stick` is camera-relative, so a swinging camera curves the path; ledge and
            // edge tests need to arrive at one spot.
            // The fourth argument is either a stick magnitude (0..1, for walking off an edge
            // slowly) or a button combination, told apart by whether it parses as a number.
            command.op = Op::StickTo;
            ok = parse_double(a, command.targetX) && parse_double(b, command.targetZ) &&
                 parse_int(c, command.frames) && command.frames > 0;
            if (ok && !d.empty() && !parse_double(d, command.magnitude)) {
                command.magnitude = 1.0;
                ok = parse_buttons(d, command.buttons);
            }
        } else if (verb == "REPEAT") {
            command.op = Op::Repeat;
            ok = parse_int(a, command.frames);
        } else if (verb == "END") {
            command.op = Op::End;
        } else if (verb == "SAY") {
            command.op = Op::Say;
            // Everything after the verb, comment already stripped. Found by hand rather than with
            // the token stream so the message keeps its internal spacing.
            const std::size_t verbStart = line.find_first_not_of(" \t");
            const std::size_t verbEnd = line.find_first_of(" \t", verbStart);
            const std::size_t textStart = verbEnd == std::string::npos ?
                                              std::string::npos :
                                              line.find_first_not_of(" \t", verbEnd);
            command.text = textStart == std::string::npos ? "" : line.substr(textStart);
        } else if (verb == "WARP") {
            command.op = Op::Warp;
            // "warp F_SP115 0 0 0" — stage, spawn point, room, layer. The layer is optional and
            // defaults to 0, which is the ordinary one; the rest are not, because a warp with a
            // guessed room number lands somewhere the script did not mean to be.
            command.text = a;
            ok = !a.empty() && parse_int(b, command.point) && parse_int(c, command.room) &&
                 (d.empty() || parse_int(d, command.layer));
        } else if (verb == "GIVE") {
            // "give 9 0x44": item number into an inventory slot (d_save.h SLOT_*, dItemNo_*).
            command.op = Op::Give;
            ok = parse_int(a, command.point) && parse_int(b, command.room) && command.point >= 0 &&
                 command.point < 24 && command.room >= 0 && command.room <= 0xFF;
        } else if (verb == "EQUIP") {
            // "equip X 9": put the item in that slot on X (or Y).
            command.op = Op::Equip;
            const std::string button = to_upper(a);
            command.point = button == "X" ? 0 : (button == "Y" ? 1 : -1);
            ok = command.point >= 0 && parse_int(b, command.room) && command.room >= 0 &&
                 command.room < 24;
        } else if (verb == "CLOTHES") {
            // "clothes 0x31": wear that outfit item (dItemNo_WEAR_*, dItemNo_ARMOR_e).
            command.op = Op::Clothes;
            ok = parse_int(a, command.room) && command.room >= 0 && command.room <= 0xFF;
        } else if (verb == "TIME") {
            // "time 12": set the clock to that hour (0-23). Hyrule Field at night spawns
            // Stalchildren, which knock a scripted player about.
            command.op = Op::Time;
            ok = parse_int(a, command.room) && command.room >= 0 && command.room <= 23;
        } else if (verb == "ARROWS") {
            // "arrows 30": the quiver count (and its max, if lower). "give 4 0x43" alone leaves 0.
            command.op = Op::Arrows;
            ok = parse_int(a, command.room) && command.room >= 0 && command.room <= 0xFF;
        } else if (verb == "OIL") {
            // "oil 21600": lantern oil (and its max, if lower); 21600 is a full lantern.
            command.op = Op::Oil;
            ok = parse_int(a, command.room) && command.room >= 0 && command.room <= 0xFFFF;
        } else if (verb == "RUPEES") {
            // "rupees 5": the wallet, for the magic armour (a low count drains to 0 in seconds).
            command.op = Op::Rupees;
            ok = parse_int(a, command.room) && command.room >= 0 && command.room <= 0xFFFF;
        } else if (verb == "SHIELD") {
            // "shield 0x2A": wear that shield (dItemNo_WOOD_SHIELD_e 0x2A, _SHIELD_e 0x2B, ...).
            command.op = Op::Shield;
            ok = parse_int(a, command.room) && command.room >= 0x2A && command.room <= 0x2C;
        } else if (verb == "SHIELDBURN") {
            // "shieldburn": set the worn wooden shield alight, as a fire hit does.
            command.op = Op::ShieldBurn;
        } else if (verb == "QUIT") {
            command.op = Op::Quit;
        } else if (verb == "NEAREST") {
            // "nearest Bou 2000": log the nearest actors with that stage name ("*" = any, "talk" =
            // any NPC that can be talked to now, or a profile number like 0x0FB). Radius optional.
            command.op = Op::Nearest;
            command.text = a;
            command.radius = 3000.0;
            ok = !a.empty() && (b.empty() || parse_double(b, command.radius)) &&
                 command.radius > 0.0;
        } else if (verb == "TALKNEAR") {
            // "talknear 1500 300": walk to the nearest talkable NPC within the radius and press A
            // until an event starts, giving up after the timeout in ticks.
            command.op = Op::TalkNear;
            command.radius = 1500.0;
            command.frames = 300;
            ok = (a.empty() || parse_double(a, command.radius)) &&
                 (b.empty() || parse_int(b, command.frames)) && command.radius > 0.0 &&
                 command.frames > 0;
        } else if (verb == "GETITEM") {
            // "getitem 0x01": the real item-get event for that item (dItemNo_*). See run_getitem.
            command.op = Op::GetItem;
            ok = parse_int(a, command.room) && command.room >= 0 && command.room <= 0xFF;
        } else if (verb == "OBJTEST") {
            // "objtest 300": the object channel's phase-0 debug object. One transient spawned 150
            // units in front of Link orbits him for that many ticks, streamed to the other end,
            // then despawns. Instant: the script goes on at once, so a `stick` line right after it
            // tests a moving owner. Logs are "[mp-obj] ..." on both ends.
            command.op = Op::ObjTest;
            ok = parse_int(a, command.frames) && command.frames > 1;
        } else if (verb == "TRANSFORM") {
            // "transform [timeout]": human <-> wolf through the game's own request flag, falling
            // back to procCoMetamorphoseInit after the timeout (default 60). See the tick() case.
            command.op = Op::Transform;
            command.frames = 60;
            ok = (a.empty() || parse_int(a, command.frames)) && command.frames > 0;
        } else if (verb == "EVBIT") {
            // "evbit 6380 on": event flag (byte << 8 | mask, hex, d_save_bit_labels.inc) on or off.
            command.op = Op::EvBit;
            const std::string state = to_upper(b);
            command.layer = state == "ON" ? 1 : (state == "OFF" ? 0 : -1);
            ok = parse_event_flag(a, command.point) && command.layer >= 0;
        } else if (verb == "SWITCH" || verb == "SWITCHOFF") {
            // "switch 40 [room]": dComIfGs_onSwitch (or off). Room defaults to the current one; it
            // only matters for zone switches (0xC0+).
            command.op = verb == "SWITCH" ? Op::Switch : Op::SwitchOff;
            command.room = -1;
            ok = parse_int(a, command.point) && command.point >= 0 &&
                 command.point < kSwitchLimit &&
                 (b.empty() ||
                     (parse_int(b, command.room) && command.room >= 0 && command.room < 64));
        } else if (verb == "STAGESWITCH") {
            // "stageswitch 2 7": switch 7 in stage table 2 (dStage_SaveTbl_*), stage or not.
            command.op = Op::StageSwitch;
            ok = parse_int(a, command.room) && command.room >= 0 && command.room < kStageTables &&
                 parse_int(b, command.point) && command.point >= 0 &&
                 command.point < dSv_info_c::MEMORY_SWITCH;
        } else if (verb == "TBOX") {
            // "tbox 5": chest 5 of the live stage table marked open.
            command.op = Op::Tbox;
            ok = parse_int(a, command.point) && command.point >= 0 && command.point < kTboxLimit;
        } else if (verb == "KEYS" || verb == "RUPEESADD") {
            // "keys 1" / "rupeesadd -20": a counter DELTA through the pending play counter.
            command.op = verb == "KEYS" ? Op::Keys : Op::RupeesAdd;
            ok = parse_int(a, command.point) &&
                 (command.op == Op::Keys ? (command.point >= -99 && command.point <= 99) :
                                           (command.point >= -9999 && command.point <= 9999));
        } else if (verb == "WORLDLOG") {
            // "worldlog [tag]": dump the shared-world state so both ends' logs can be diffed.
            command.op = Op::WorldLog;
            command.text = a;
        } else if (verb == "JOINPARTNER") {
            command.op = Op::JoinPartner;
        } else if (verb == "EXPECTBIT") {
            // "expectbit <event|switch|tbox|item> <tbl|live|-> <no> <0|1>". For event, no is the
            // hex flag and tbl is ignored (write `-`). For item, a numbered tbl takes the saved
            // item bits 0x80-0x9F only; `live` takes anything onItem routes.
            command.op = Op::ExpectBit;
            command.text = to_upper(a);
            if (command.text == "EVENT") {
                command.room = 0;
                ok = parse_event_flag(c, command.point);
            } else {
                int limit = 0;
                if (command.text == "SWITCH") {
                    limit = kSwitchLimit;
                } else if (command.text == "TBOX") {
                    limit = kTboxLimit;
                } else if (command.text == "ITEM") {
                    limit = kItemLimit;
                }
                ok = limit > 0 && parse_stage_table(b, command.room) &&
                     parse_int(c, command.point) && command.point >= 0 && command.point < limit;
                if (ok && command.room != kLiveTbl) {
                    // A numbered table is a dSv_memBit_c: memory switches and saved item bits only.
                    if (command.text == "SWITCH") {
                        ok = command.point < dSv_info_c::MEMORY_SWITCH;
                    } else if (command.text == "ITEM") {
                        ok = command.point >= dSv_info_c::MEMORY_ITEM &&
                             command.point < dSv_info_c::MEMORY_ITEM + dSv_info_c::DAN_ITEM;
                    }
                }
            }
            ok = ok && (d == "0" || d == "1");
            command.layer = d == "1" ? 1 : 0;
        } else if (verb == "EXPECTKEYS" || verb == "EXPECTRUPEES") {
            // "expectkeys 1" / "expectrupees 150": the committed count, absolute.
            command.op = verb == "EXPECTKEYS" ? Op::ExpectKeys : Op::ExpectRupees;
            ok = parse_int(a, command.point) && command.point >= 0;
        } else if (verb == "WAITEVENT") {
            // "waitevent 6308 [timeout]": block until that event flag is set (default timeout
            // 1800 ticks). The rendezvous between two instances: one sets a flag, the world sync
            // carries it, and the other's script resumes within a few ticks of it.
            command.op = Op::WaitEvent;
            command.frames = 1800;
            ok = parse_event_flag(a, command.point) &&
                 (b.empty() || parse_int(b, command.frames)) && command.frames > 0;
        /* --- batch 6: temp flags --- */
        } else if (verb == "DANSWITCH" || verb == "ZONESWITCH") {
            // "danswitch 5 on" (0-63, game switch 0x80+N) / "zoneswitch 33 off" (0-47, 0xC0+N; 32+
            // are one-zone switches) in the room Link stands in.
            command.op = verb == "DANSWITCH" ? Op::DanSwitch : Op::ZoneSwitch;
            const int limit = command.op == Op::DanSwitch ? kDanSwitchCount : kZoneSwitchCount;
            const std::string state = to_upper(b);
            command.layer = state == "ON" ? 1 : (state == "OFF" ? 0 : -1);
            ok = parse_int(a, command.point) && command.point >= 0 && command.point < limit &&
                 command.layer >= 0;
        } else if (verb == "EXPECTDAN" || verb == "EXPECTZONE") {
            // "expectdan 5 1" / "expectzone 33 0": same numbering as danswitch / zoneswitch.
            command.op = verb == "EXPECTDAN" ? Op::ExpectDan : Op::ExpectZone;
            const int limit = command.op == Op::ExpectDan ? kDanSwitchCount : kZoneSwitchCount;
            ok = parse_int(a, command.point) && command.point >= 0 && command.point < limit &&
                 (b == "0" || b == "1");
            command.layer = b == "1" ? 1 : 0;
        } else if (verb == "ZONECOUNTCHECK") {
            // "zonecountcheck": run the room-change zone lifecycle on the current room (H8 test).
            command.op = Op::ZoneCountCheck;
        } else if (verb == "TEMPLOG") {
            // "templog [tag]": dan bits of this stage table and zone bits of this room, engine and
            // shared copy (worldlog prints the same lines at its end).
            command.op = Op::TempLog;
            command.text = a;
        } else {
            Log.error("Line {}: unknown command '{}'", lineNumber, verb);
            ok = false;
        }

        // Four argument positions are read above, and every verb uses at most four. A fifth token
        // would otherwise be dropped in silence — and the shape of mistake that produces is
        // `stick 0 1 60 R Z` for "guard while walking with Z held", where the correct form is
        // `R+Z` and the silent version does something subtly different for the whole run.
        //
        // SAY is exempt, and not as a special case: it does not use the token stream at all. It
        // re-reads the raw line so its message keeps its internal spacing, so for SAY the tokens
        // above are just the first four words of prose and a fifth is expected.
        if (ok && command.op != Op::Say) {
            std::string extra;
            if (tokens >> extra) {
                Log.error(
                    "Line {}: unexpected extra argument '{}' for '{}'", lineNumber, extra, verb);
                ok = false;
            }
        }

        if (!ok) {
            Log.error("Line {}: bad arguments for '{}'", lineNumber, verb);
            sProgram.clear();
            return;
        }

        sProgram.push_back(std::move(command));
    }

    if (sProgram.empty()) {
        Log.error("Autopilot script '{}' has no commands", path.string());
        return;
    }
    if (!link_loops()) {
        sProgram.clear();
        return;
    }

    sActive = true;
    Log.info("Autopilot loaded {} commands from '{}'", sProgram.size(), path.string());
}

bool active() {
    return sActive;
}

void tick() {
    if (!sActive || sFinished) {
        return;
    }

    sTicks++;

    if (!settle_program_counter()) {
        sFinished = true;
        PADClearVirtualStatus(kPort);
        Log.info("Autopilot finished after {} ticks", sTicks);
        return;
    }

    const Command& command = sProgram[sPc];

    PADStatus status{};
    status.err = PAD_ERR_NONE;

    int duration = command.frames;

    switch (command.op) {
    case Op::WaitWorld: {
        if (in_world()) {
            if (!sSawWorld) {
                sSawWorld = true;
                report_world_reached();
            }
            duration = sElapsed + 1;
        } else if (command.frames <= 0) {
            duration = sElapsed + 2;  // never satisfied by elapsed time alone
        } else if (sElapsed + 1 >= command.frames) {
            // Loud, because everything after this in the script is now running against a menu.
            Log.warn("Line {}: still not in the world after {} ticks; continuing anyway",
                command.line, command.frames);
        }
        break;
    }

    case Op::Tap:
        if (sElapsed < command.frames) {
            status.button = static_cast<u16>(command.buttons);
        }
        duration = command.frames + command.gap;
        break;

    case Op::Mash:
        // Menu depth is not knowable up front, so mash on a condition rather than a count.
        // Stopping the instant control begins is what stops the presses leaking into the game as
        // pause menus and sword swings.
        if (in_world()) {
            if (!sSawWorld) {
                sSawWorld = true;
                report_world_reached();
            }
            duration = sElapsed + 1;
        } else {
            if (sElapsed % (kMashHoldTicks + kMashGapTicks) < kMashHoldTicks) {
                status.button = static_cast<u16>(command.buttons);
            }
            if (sElapsed + 1 >= command.frames) {
                Log.warn("Line {}: mashed for {} ticks without reaching the world", command.line,
                    command.frames);
            }
        }
        break;

    case Op::Hold:
        status.button = static_cast<u16>(command.buttons);
        break;

    case Op::Stick:
        status.stickX = command.stickX;
        status.stickY = command.stickY;
        status.button = static_cast<u16>(command.buttons);
        break;

    case Op::StickTo: {
        status.button = static_cast<u16>(command.buttons);
        fopAc_ac_c* link = dComIfGp_getPlayer(0);
        auto* camera = dComIfGp_getCamera(dComIfGp_getPlayerCameraID(0));
        if (link == nullptr || camera == nullptr) {
            break;
        }
        const double dx = command.targetX - link->current.pos.x;
        const double dz = command.targetZ - link->current.pos.z;
        const double distance = std::sqrt(dx * dx + dz * dz);
        if (sElapsed == 0) {
            sStickToArrived = false;
        }
        // Latched: momentum carries him back out of the radius, and re-aiming from there made him
        // orbit the point (measured, +-110 units for ~130 ticks at full stick).
        if (sStickToArrived || distance < 15.0) {
            sStickToArrived = true;
            break;
        }
        /* Inverse of daAlink_c's own mapping (d_a_alink.cpp:9468-9472): moveAngle = stickAngle +
         * camera yaw, stickAngle = JUT angle + 0x8000, JUT angle = atan2(x, -y). Mirror mode
         * negates the JUT angle first (m_Do_controller_pad.cpp:29); negating the wanted angle
         * here yields exactly the stick that survives that. */
        const double kToRad = 3.14159265358979 / 32768.0;
        const double moveAngle = std::atan2(dx, dz);
        double stickAngle = moveAngle - dCam_getControledAngleY(camera) * kToRad;
        if (dusk::getSettings().game.enableMirrorMode) {
            stickAngle = -stickAngle;
        }
        // Ease off over the last 100 units, but never below 0.45: under ~0.4 Link only turns on
        // the spot (measured; the game's own walk threshold).
        const double magnitude = std::min(command.magnitude, std::max(0.45, distance / 100.0));
        status.stickX = stick_value(-std::sin(stickAngle) * magnitude);
        status.stickY = stick_value(std::cos(stickAngle) * magnitude);
        break;
    }

    case Op::TalkNear: {
        /* Ends the tick an event starts, whatever started it: the point is to put Link into
         * PROC_TALK for the partner to watch, and the script's next lines page the text out. */
        if (dComIfGp_event_runCheck()) {
            if (sElapsed > 0) {
                Log.info("[{}] talknear: an event started after {} ticks", sTicks, sElapsed);
            }
            duration = sElapsed + 1;
            break;
        }
        fopAc_ac_c* link = dComIfGp_getPlayer(0);
        if (link == nullptr) {
            break;
        }
        if (sElapsed == 0) {
            sTalkTarget = fpcM_ERROR_PROCESS_ID_e;
            const std::vector<ActorHit> hits = find_actors("talk", command.radius);
            if (hits.empty()) {
                Log.warn("[{}] line {}: talknear: no talkable NPC within {:.0f} units", sTicks,
                    command.line, command.radius);
                duration = 1;
                break;
            }
            log_actor_hit("talknear: heading for", hits[0]);
            sTalkTarget = fopAcM_GetID(hits[0].actor);
        }
        fopAc_ac_c* target = fopAcM_SearchByID(sTalkTarget);
        if (target == nullptr) {
            Log.warn("[{}] talknear: the NPC is gone", sTicks);
            duration = sElapsed + 1;
            break;
        }
        const double dx = target->current.pos.x - link->current.pos.x;
        const double dz = target->current.pos.z - link->current.pos.z;
        if (std::sqrt(dx * dx + dz * dz) > kTalkReach) {
            steer_toward(status, target->current.pos.x, target->current.pos.z, 1.0);
        } else if (std::fabs(link->speedF) < 0.5f) {
            // Standing, so A cannot become a roll. 4 ticks on, 8 off: one clean edge per press.
            if (sElapsed % 12 < 4) {
                status.button = PAD_BUTTON_A;
            }
        }
        if (sElapsed + 1 >= command.frames) {
            Log.warn("[{}] line {}: talknear: no event after {} ticks", sTicks, command.line,
                command.frames);
        }
        break;
    }

    case Op::Transform: {
        /* Human <-> wolf, by the game's own request flags rather than a mid-frame proc call
         * (puppet-wolf.md "Autopilot test"). Human: onForceWolfChange (d_a_player.h:1091), which
         * checkGroundSpecialMode consumes on the ground (d_a_alink.cpp:12275-12279) - the path the
         * twilight tag uses (d_a_kytag12.cpp:398). Wolf: ERFLG0_UNK_1, consumed by
         * checkWolfGroundSpecialMode (d_a_alink_wolf.inc:1418-1423); it has no named setter. Both
         * are END-reset flags, cleared at the end of his execute (d_a_alink.cpp:18897), so the
         * request is re-raised every tick until the proc starts, as kytag12 does.
         *
         * Not Dusk's quick transform (d_a_alink_dusk.cpp:67-147): that needs M_077 and Midna
         * riding, and the test save may have neither. These flags need neither.
         *
         * If the proc has not started by the timeout (a proc that never asks, such as a swim), it
         * falls back to procCoMetamorphoseInit(), which is what the console `transform` does
         * (dusk/commands.cpp:809-814). The log says which path ran. */
        daAlink_c* alink = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
        if (sElapsed == 0) {
            if (alink == nullptr || !in_world()) {
                Log.error("[{}] line {}: transform needs the world; skipped", sTicks, command.line);
                duration = 1;
                break;
            }
            sTransformToWolf = !alink->checkWolf();
            Log.info("[{}] transform: requesting {} via the game's own flag ({}), proc 0x{:X}",
                sTicks, sTransformToWolf ? "human->wolf" : "wolf->human",
                sTransformToWolf ? "ERFLG0_FORCE_WOLF_CHANGE" : "ERFLG0_UNK_1",
                static_cast<unsigned>(alink->mProcID));
        }
        if (alink == nullptr) {
            Log.warn("[{}] transform: Link is gone; giving up", sTicks);
            duration = sElapsed + 1;
            break;
        }
        if (alink->mProcID == daAlink_c::PROC_METAMORPHOSE) {
            Log.info("[{}] transform: PROC_METAMORPHOSE started after {} tick(s) via the request "
                     "flag",
                sTicks, sElapsed);
            duration = sElapsed + 1;
            break;
        }
        if (sElapsed + 1 >= command.frames) {
            const unsigned procBefore = static_cast<unsigned>(alink->mProcID);
            const int started = alink->procCoMetamorphoseInit();
            Log.warn("[{}] transform: the request flag was not consumed in {} ticks (proc 0x{:X}); "
                     "fell back to procCoMetamorphoseInit() as the console 'transform' does: "
                     "returned {}, proc now 0x{:X}{}",
                sTicks, command.frames, procBefore, started, static_cast<unsigned>(alink->mProcID),
                alink->mProcID == daAlink_c::PROC_METAMORPHOSE ? " (fallback path ran)" :
                                                                 " (DID NOT START)");
            break;
        }
        if (sTransformToWolf) {
            alink->onForceWolfChange();
        } else {
            alink->onEndResetFlg0(daPy_py_c::ERFLG0_UNK_1);
        }
        break;
    }

    case Op::WaitEvent:
        if (dComIfGs_isEventBit(static_cast<u16>(command.point))) {
            Log.info(
                "[{}] waitevent 0x{:04X}: set after {} tick(s)", sTicks, command.point, sElapsed);
            duration = sElapsed + 1;
        } else if (sElapsed + 1 >= command.frames) {
            // Loud: everything after this runs without the rendezvous it was written against.
            Log.error("[autopilot] waitevent FAIL flag=0x{:04X} not set after {} ticks; "
                      "continuing anyway",
                command.point, command.frames);
        }
        break;

    case Op::Warp:
        // Only reached while warp_must_wait_for_world_sync() holds it: never advance from here;
        // settle_program_counter() performs the warp once the guest is synced.
        duration = sElapsed + 2;
        break;

    default:
        break;
    }

    if ((status.button & PAD_TRIGGER_L) != 0) {
        status.triggerLeft = kTriggerAnalog;
    }
    if ((status.button & PAD_TRIGGER_R) != 0) {
        status.triggerRight = kTriggerAnalog;
    }

    publish(status);

    sElapsed++;
    if (sElapsed >= duration) {
        sElapsed = 0;
        sPc++;
    }
}

}  // namespace dusk::autopilot
