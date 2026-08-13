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

#include "d/d_com_inf_game.h"
#include "dusk/logging.h"
#include "dusk/multiplayer/replication/player_bridge.hpp"

/**
 * A tiny interpreter over aurora's virtual pad.
 *
 * The unit of time is one call to mDoCPd_c::read(), i.e. one sim tick rather than one rendered
 * frame — the two differ, and the sim rate is the stable one (~30 Hz). Scripts are written in
 * ticks and the elapsed wall time is logged periodically so they can be calibrated by eye.
 */

namespace dusk::autopilot {
namespace {

aurora::Module Log{"dusk::autopilot"};

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
    Repeat,
    End,
    Say,
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
    std::string text;
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
        const int value = std::stoi(token, &consumed);
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
