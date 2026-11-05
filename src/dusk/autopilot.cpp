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

#include "d/actor/d_a_player.h"
#include "d/d_camera.h"
#include "d/d_com_inf_game.h"
#include "d/d_meter2_info.h"
#include "dusk/logging.h"
#include "dusk/multiplayer/replication/player_bridge.hpp"
#include "dusk/settings.h"
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
    Quit,
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
    default:
        break;
    }
}

/// Runs every command that takes no time (say/repeat/end/quit), leaving sPc on a timed one.
/// Returns false once the program is over.
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
            Log.info("[{}] warping to stage '{}' point {} room {} layer {}", sTicks, command.text,
                command.point, command.room, command.layer);
            dComIfGp_setNextStage(command.text.c_str(), static_cast<s16>(command.point),
                static_cast<s8>(command.room), static_cast<s8>(command.layer));
            sPc++;
            continue;

        case Op::Give:
        case Op::Equip:
        case Op::Clothes:
            run_setup_command(command);
            sPc++;
            continue;

        case Op::Quit:
            Log.info("Script asked to quit after {} ticks", sTicks);
            request_quit();
            return false;

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
        } else if (verb == "QUIT") {
            command.op = Op::Quit;
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
