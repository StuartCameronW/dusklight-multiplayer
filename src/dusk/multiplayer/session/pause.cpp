#include "pause.hpp"

#include <cstring>
#include <string>
#include <unordered_map>

#include <fmt/format.h>

#include "../replication/replication_manager.hpp"
#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_meter2_info.h"
#include "dusk/logging.h"
#include "dusk/multiplayer.hpp"
#include "f_op/f_op_overlap_mng.h"
#include "m_Do/m_Do_controller_pad.h"
#include "network_manager.hpp"

/**
 * Batch 8: the Dying Light pause model. Design: .claude/future-plans/mp-pause.md; Stuart's calls:
 * .claude/plan/17-decisions-2026-11-20.md (the item ring is a menu like any other; the Collect
 * screen's Link doll is hidden in co-op and equipment changes apply on close; the hearts flash over
 * the menu when he is hurt behind it; timers keep counting).
 *
 * Every function the decomp calls answers "vanilla" unless a session is live (sSessionLive, set
 * once per tick). With no session, world_frozen() is dComIfGp_isPauseFlag() and nothing else here
 * does anything.
 *
 * Log prefix "[mp-pause]" on every line, so one grep reads the whole feature off both ends.
 */

namespace dusk::mp {
namespace {

constexpr borealis::Log Log{"dusk::mp"};

/// How long the hearts keep flashing after the last hit behind a menu, and the length of one
/// half-period (shown / hidden). At the 30 Hz sim rate: 2.5 s, ~2 blinks a second.
constexpr int kHeartsFlashTicks = 75;
constexpr int kHeartsFlashHalfPeriod = 8;

/* --- once-per-tick state (pre_actor_tick) --- */

/// A session is live (host, or a client the host has accepted). False: everything is vanilla.
bool sSessionLive = false;
/// No session, or every in-world partner reports a menu: the menu flag may freeze the world.
bool sPeersAllowFreeze = true;
int sCountedPeers = 0;
int sPausedPeers = 0;
/// Each partner's last logged menu bit, so a change is logged once on the receiving end.
std::unordered_map<std::uint32_t, bool> sPeerBits;

bool sWasFrozen = false;
bool sWasInMenu = false;
bool sWasSessionLive = false;

/// What was already going on when the local menu opened. A force-close fires only on something
/// that STARTED behind the menu: several menus open during an event on purpose (Agitha's insect
/// menu, the warp map from Midna's talk, a forced item-ring tutorial).
struct OpenState {
    bool valid = false;
    bool event = false;
    bool nextStage = false;
    bool overlap = false;
    bool demo = false;
};
OpenState sAtOpen;

/* --- input isolation (PauseWorldExecScope) --- */

int sScopeDepth = 0;
interface_of_controller_pad sSavedPad;
int sIsolatedExecs = 0;
int sIsolatedExecsLastTick = 0;
bool sIsolationLogged = false;

/* --- P2: item assignments made in a menu apply on close --- */

/// The three places an X/Y assignment lives: the save's slot indices, its combo (mix) indices and
/// the play-side item cache dComIfGp_setSelectItem fills.
struct ItemSelection {
    u8 select[4];
    u8 mix[4];
    u8 play[4];
};
bool sHaveSelection = false;
/// What the world sees while the menu is open: the assignment as it was when the menu opened,
/// plus anything an actor itself wrote since (used up, newly got).
ItemSelection sWorldSelection;
/// The menu's own copy, swapped out for the length of one world execute.
ItemSelection sMenuSelection;

/* --- hearts flash --- */

u16 sLastLife = 0;
bool sHaveLastLife = false;
int sFlashTicks = 0;
int sHeartsFlashes = 0;

/* --- force-close --- */

int sForceCloses = 0;
int sLastForceReason = kPauseForceCloseNone;
int sForceWaitLogs = 0;

/* --- P1: the Collect screen's Link doll --- */

/// The current dMenu_Collect3D_c was created with its doll hidden (latched at create).
bool sDollHidden = false;
/// A doll-less Collect screen existed during this menu session: apply the deferred equipment on
/// close. Survives the Collect screen being deleted and re-created around its sub-windows (Save,
/// Options, Letters, ...), which happens several times inside one menu session.
bool sHiddenThisMenu = false;
bool sDeferClothes = false;
bool sDeferShield = false;
/// A deferred shield reload is running in the world: daAlink_c::execute holds until it lands.
bool sEquipHold = false;

dSv_player_status_a_c& status_a() {
    return g_dComIfG_gameInfo.info.getPlayer().getPlayerStatusA();
}

void read_selection(ItemSelection& out) {
    dSv_player_status_a_c& status = status_a();
    for (int i = 0; i < 4; i++) {
        out.select[i] = status.getSelectItemIndex(i);
        out.mix[i] = status.getMixItemIndex(i);
        out.play[i] = g_dComIfG_gameInfo.play.getSelectItem(i);
    }
}

void write_selection(const ItemSelection& in) {
    dSv_player_status_a_c& status = status_a();
    for (int i = 0; i < 4; i++) {
        status.setSelectItemIndex(i, in.select[i]);
        status.setMixItemIndex(i, in.mix[i]);
        g_dComIfG_gameInfo.play.setSelectItem(i, in.play[i]);
    }
}

/// An actor wrote a byte of the world's copy during its execute: that write wins in both copies.
void merge_byte(u8 now, u8& world, u8& menu) {
    if (now != world) {
        world = now;
        menu = now;
    }
}

void enter_world_exec() {
    std::memcpy(&sSavedPad, &mDoCPd_c::m_cpadInfo[PAD_1], sizeof(sSavedPad));
    std::memset(&mDoCPd_c::m_cpadInfo[PAD_1], 0, sizeof(mDoCPd_c::m_cpadInfo[PAD_1]));
    ++sIsolatedExecs;

    if (!sHaveSelection) {
        read_selection(sWorldSelection);
        sHaveSelection = true;
    }
    read_selection(sMenuSelection);
    write_selection(sWorldSelection);
}

void leave_world_exec() {
    ItemSelection now;
    read_selection(now);
    for (int i = 0; i < 4; i++) {
        merge_byte(now.select[i], sWorldSelection.select[i], sMenuSelection.select[i]);
        merge_byte(now.mix[i], sWorldSelection.mix[i], sMenuSelection.mix[i]);
        merge_byte(now.play[i], sWorldSelection.play[i], sMenuSelection.play[i]);
    }
    write_selection(sMenuSelection);

    std::memcpy(&mDoCPd_c::m_cpadInfo[PAD_1], &sSavedPad, sizeof(sSavedPad));
}

std::uint64_t now_tick() {
    return network_manager().sim_tick();
}

std::string describe_peers() {
    std::string out;
    for (const auto& entry : replication_manager().players()) {
        const RemotePlayer& player = entry.second;
        if (!out.empty()) {
            out += ' ';
        }
        if (!player.hasLatest) {
            out += fmt::format("{}:none", entry.first);
            continue;
        }
        out += fmt::format("{}:{}{}", entry.first,
            (player.latest.gear & kPlayerGearInMenu) != 0 ? "menu" : "play",
            player.latest.in_world() ? "" : "(not-in-world)");
    }
    return out.empty() ? std::string("none") : out;
}

/// The deferred equipment of a doll-less Collect screen, on the menu's close (P1).
void apply_deferred_equipment() {
    const bool clothes = sDeferClothes;
    const bool shield = sDeferShield;
    sHiddenThisMenu = false;
    sDeferClothes = false;
    sDeferShield = false;

    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr) {
        Log.info("[mp-pause] collect close: no Link to apply equipment to (clothes {} shield {}) "
                 "tick={}",
            clothes, shield, now_tick());
        return;
    }
    if (clothes) {
        // Processed by his own execute (loadModelDVD is its first line), the in-world path the
        // wolf transformation and the autopilot's `clothes` already take.
        link->setClothesChange(0);
    }
    if (shield) {
        // Only statusWindowExecute and daAlink_Delete pump this reload in vanilla. In the world it
        // nulls mShieldModel while his execute runs (measured crash, autopilot.cpp `shield`), so
        // his execute holds until it lands: see pause_equip_hold().
        link->setShieldChange();
        sEquipHold = true;
    }
    // The sword (and the sword/shield/clothes bookkeeping): what resetStatusWindow ends with
    // (d_a_alink_swindow.inc:369-389).
    link->setSelectEquipItem(FALSE);
    /* setSelectEquipItem starts the 5-tick sword change wait when the model changed, and only
     * statusWindowExecute counts it down (d_a_alink_swindow.inc:302-311). Left at 5 the sword
     * would never draw and the next Collect screen could not close (dMw_isPush_S_Button waits for
     * 0). Vanilla's doll has always counted it to 0 by the time the menu closes; 100 ("no sword")
     * is what vanilla leaves, so keep that. */
    if (link->mSwordChangeWaitTimer < 30) {
        link->mSwordChangeWaitTimer = 0;
    }
    Log.info("[mp-pause] collect close: deferred equipment applied (clothes {} shield {}; "
             "sword model refreshed) tick={}",
        clothes, shield, now_tick());
}

}  // namespace

/* ====================================================================================
 * dusk/multiplayer.hpp — the decomp-facing half
 * ==================================================================================== */

bool world_frozen() {
    return dComIfGp_isPauseFlag() != 0 && sPeersAllowFreeze;
}

bool pause_isolates_world() {
    return sSessionLive && pause::local_in_menu() && !world_frozen();
}

bool pause_world_input_blocked() {
    return sScopeDepth > 0;
}

PauseWorldExecScope::PauseWorldExecScope() : mActive(false) {
    if (!sSessionLive || !pause_isolates_world()) {
        return;
    }
    mActive = true;
    if (sScopeDepth++ == 0) {
        enter_world_exec();
    }
}

PauseWorldExecScope::~PauseWorldExecScope() {
    if (mActive && --sScopeDepth == 0) {
        leave_world_exec();
    }
}

int pause_menu_force_close_reason() {
    if (!sSessionLive || !sAtOpen.valid || world_frozen()) {
        return kPauseForceCloseNone;
    }
    // Death: d_gameover.cpp:133-138 waits for the menu's pause flag and 2-D heap lock to clear.
    if (dComIfGs_getLife() == 0) {
        return kPauseForceCloseDeath;
    }
    if (!sAtOpen.event && dComIfGp_event_runCheck()) {
        return kPauseForceCloseEvent;
    }
    if ((!sAtOpen.nextStage && dComIfGp_isEnableNextStage()) ||
        (!sAtOpen.overlap && fopOvlpM_IsDoingReq()))
    {
        return kPauseForceCloseScene;
    }
    // An enemy grab or swallow puts Link into a demo without necessarily starting an event.
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (!sAtOpen.demo && link != nullptr && link->checkPlayerDemoMode()) {
        return kPauseForceCloseDemo;
    }
    return kPauseForceCloseNone;
}

void pause_note_force_close(int reason, int menuProc, const char* waitingFor) {
    if (waitingFor != nullptr) {
        if (sForceWaitLogs < 40) {
            sForceWaitLogs++;
            Log.info("[mp-pause] force-close {} pending: menu proc {} waits for {} tick={}",
                pause::force_close_reason_name(reason), menuProc, waitingFor, now_tick());
        }
        return;
    }
    sForceCloses++;
    sLastForceReason = reason;
    sForceWaitLogs = 0;
    Log.info("[mp-pause] force-close {}: menu closed from proc {} (life {}) tick={}",
        pause::force_close_reason_name(reason), menuProc, static_cast<int>(dComIfGs_getLife()),
        now_tick());
}

bool pause_collect_doll_begin() {
    // Latched for this Collect screen's whole life: the world can start running behind the menu
    // at any moment (a partner closes his), so "hidden only while running" would have to swap the
    // doll in and out of Link's real model mid-menu, which is the crash class this avoids.
    sDollHidden = sSessionLive || sHiddenThisMenu;
    if (sDollHidden) {
        if (!sHiddenThisMenu) {
            Log.info("[mp-pause] collect screen: Link doll hidden (co-op); equipment changes "
                     "apply on close tick={}",
                now_tick());
        }
        sHiddenThisMenu = true;
    }
    return sDollHidden;
}

bool pause_collect_doll_hidden() {
    return sDollHidden;
}

void pause_collect_doll_execute() {
    /* The doll's timer side of statusWindowExecute (d_a_alink_swindow.inc:300-311) without its
     * model side. Its sword wait counts down here and is capped like there, so the Collect screen
     * can close (dMw_isPush_S_Button waits for 0): a Link with no sword sits at 100 in the world,
     * and only the doll ever cleared it. */
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr) {
        return;
    }
    if (link->mSwordChangeWaitTimer != 0) {
        link->mSwordChangeWaitTimer--;
    }
    if (link->mSwordChangeWaitTimer >= 30) {
        link->mSwordChangeWaitTimer = 0;
    }
}

void pause_collect_doll_end() {
    sDollHidden = false;
}

void pause_defer_clothes_change() {
    sDeferClothes = true;
    Log.info("[mp-pause] collect screen: outfit change deferred to close tick={}", now_tick());
}

void pause_defer_shield_change() {
    sDeferShield = true;
    Log.info("[mp-pause] collect screen: shield change deferred to close tick={}", now_tick());
}

bool pause_equip_hold() {
    return sEquipHold;
}

bool pause_defer_warp_start() {
    return sSessionLive && pause::local_in_menu();
}

bool pause_hearts_flash_on() {
    if (sFlashTicks <= 0 || !sSessionLive || !pause::local_in_menu()) {
        return false;
    }
    return ((sFlashTicks / kHeartsFlashHalfPeriod) % 2) == 1;
}

/* ====================================================================================
 * pause.hpp — the Dusk-side half
 * ==================================================================================== */

namespace pause {

bool local_in_menu() {
    return dMeter2Info_getWindowStatus() != 0 || dComIfGp_isPauseFlag() != 0;
}

void fill_wire_bit(PlayerState& state) {
    if (local_in_menu()) {
        state.gear |= kPlayerGearInMenu;
    } else {
        state.gear &= static_cast<std::uint8_t>(~kPlayerGearInMenu);
    }
}

void pre_actor_tick() {
    NetworkManager& net = network_manager();
    const bool live = net.is_active() && net.session_live();
    const std::uint64_t tick = net.sim_tick();
    if (live != sWasSessionLive) {
        Log.info("[mp-pause] pause model: {} tick={}",
            live ? "Dying Light (the world pauses only when every player is in a menu)" :
                   "vanilla (no live session)",
            tick);
        sWasSessionLive = live;
    }
    sSessionLive = live;

    /* Partners. Only one who is in the world (or in a menu, which can be open during an event)
     * counts toward "every player": one loading, on the title screen or not yet posed must neither
     * hold the world running nor keep it frozen (mp-pause.md §2.1). */
    sCountedPeers = 0;
    sPausedPeers = 0;
    if (live) {
        const auto& players = replication_manager().players();
        for (const auto& entry : players) {
            const RemotePlayer& player = entry.second;
            if (!player.hasLatest) {
                continue;
            }
            const bool inMenu = (player.latest.gear & kPlayerGearInMenu) != 0;
            auto seen = sPeerBits.find(entry.first);
            if (seen == sPeerBits.end() || seen->second != inMenu) {
                Log.info("[mp-pause] partner {} menu bit {} -> {} (in world {}) tick={}",
                    entry.first, seen == sPeerBits.end() ? "?" : (seen->second ? "1" : "0"),
                    inMenu ? 1 : 0, player.latest.in_world(), tick);
                sPeerBits[entry.first] = inMenu;
            }
            if (!player.latest.in_world() && !inMenu) {
                continue;
            }
            ++sCountedPeers;
            if (inMenu) {
                ++sPausedPeers;
            }
        }
        for (auto it = sPeerBits.begin(); it != sPeerBits.end();) {
            if (players.find(it->first) == players.end()) {
                it = sPeerBits.erase(it);
            } else {
                ++it;
            }
        }
    } else {
        sPeerBits.clear();
    }
    sPeersAllowFreeze = !live || sPausedPeers == sCountedPeers;

    const bool inMenu = local_in_menu();
    const int windowStatus = static_cast<int>(dMeter2Info_getWindowStatus());
    const bool frozen = world_frozen();

    if (inMenu != sWasInMenu) {
        if (inMenu) {
            daAlink_c* link = daAlink_getAlinkActorClass();
            sAtOpen.valid = true;
            sAtOpen.event = dComIfGp_event_runCheck() != 0;
            sAtOpen.nextStage = dComIfGp_isEnableNextStage() != 0;
            sAtOpen.overlap = fopOvlpM_IsDoingReq() != 0;
            sAtOpen.demo = link != nullptr && link->checkPlayerDemoMode();
            // P2: the world keeps the assignment it had when the menu opened.
            if (live) {
                read_selection(sWorldSelection);
                sHaveSelection = true;
            }
            sIsolationLogged = false;
            sFlashTicks = 0;
        } else {
            sAtOpen = OpenState();
            // P2: what the menu assigned is now what the world sees.
            sHaveSelection = false;
            sFlashTicks = 0;
            // P1: the doll-less Collect screen's equipment.
            if (sHiddenThisMenu) {
                apply_deferred_equipment();
            }
        }
        if (live) {
            Log.info("[mp-pause] local menu {} (window status {}, flag {}) world {} tick={}",
                inMenu ? "open" : "closed", windowStatus, dComIfGp_isPauseFlag() != 0 ? 1 : 0,
                frozen ? "frozen" : "running", tick);
        }
        sWasInMenu = inMenu;
    }
    if (!live) {
        sHaveSelection = false;
    }

    if (live && frozen != sWasFrozen) {
        Log.info("[mp-pause] world_frozen {} -> {}: local menu {} (status {}, flag {}), partners "
                 "{}/{} in a menu [{}] tick={}",
            sWasFrozen ? 1 : 0, frozen ? 1 : 0, inMenu ? 1 : 0, windowStatus,
            dComIfGp_isPauseFlag() != 0 ? 1 : 0, sPausedPeers, sCountedPeers, describe_peers(),
            tick);
    }
    sWasFrozen = frozen;

    sIsolatedExecsLastTick = sIsolatedExecs;
    sIsolatedExecs = 0;
    if (live && sIsolatedExecsLastTick > 0 && !sIsolationLogged) {
        sIsolationLogged = true;
        Log.info("[mp-pause] input isolated: {} actor/camera executes ran with the pad zeroed "
                 "while the menu has it tick={}",
            sIsolatedExecsLastTick, tick);
    }

    /* Hearts flash (Stuart, 2026-11-20): hurt behind an opaque menu, the life gauge is drawn over
     * it, flashing. dMeter2_c::moveLife applies a hit to the save life in one step, so a drop is
     * one hit. */
    const u16 life = dComIfGs_getLife();
    if (live && inMenu && !frozen && sHaveLastLife && life < sLastLife) {
        if (sFlashTicks == 0) {
            ++sHeartsFlashes;
        }
        sFlashTicks = kHeartsFlashTicks;
        Log.info("[mp-pause] hearts flash: life {} -> {} behind the menu (window status {}) "
                 "tick={}",
            static_cast<int>(sLastLife), static_cast<int>(life), windowStatus, tick);
    } else if (sFlashTicks > 0) {
        --sFlashTicks;
    }
    if (!inMenu) {
        sFlashTicks = 0;
    }
    sLastLife = life;
    sHaveLastLife = true;

    if (sEquipHold) {
        daAlink_c* link = daAlink_getAlinkActorClass();
        if (link == nullptr || link->getShieldChangeWaitTimer() == 0) {
            sEquipHold = false;
            Log.info("[mp-pause] deferred shield change {} tick={}",
                link == nullptr ? "dropped (no Link)" : "landed; Link released", tick);
        }
    }
}

Stats stats() {
    Stats out;
    out.sessionLive = sSessionLive;
    out.frozen = world_frozen();
    out.localInMenu = local_in_menu();
    out.windowStatus = static_cast<int>(dMeter2Info_getWindowStatus());
    out.countedPeers = sCountedPeers;
    out.pausedPeers = sPausedPeers;
    out.heartsFlashes = sHeartsFlashes;
    out.heartsFlashing = sFlashTicks > 0;
    out.forceCloses = sForceCloses;
    out.lastForceCloseReason = sLastForceReason;
    out.isolatedExecsLastTick = sIsolatedExecsLastTick;
    out.collectDollHidden = sDollHidden;
    return out;
}

void reset_counters() {
    sHeartsFlashes = 0;
    sForceCloses = 0;
    sLastForceReason = kPauseForceCloseNone;
}

const char* force_close_reason_name(int reason) {
    switch (reason) {
    case kPauseForceCloseNone:
        return "none";
    case kPauseForceCloseDeath:
        return "death";
    case kPauseForceCloseEvent:
        return "event";
    case kPauseForceCloseScene:
        return "scene-change";
    case kPauseForceCloseDemo:
        return "player-demo";
    default:
        return "?";
    }
}

}  // namespace pause

}  // namespace dusk::mp
