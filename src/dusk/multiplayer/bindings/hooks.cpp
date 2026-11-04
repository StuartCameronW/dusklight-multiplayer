#include "dusk/multiplayer.hpp"

#include <string>

#include "../session/network_manager.hpp"
#include "dusk/logging.h"

/**
 * The thin glue the decomp core loop calls into.
 *
 * Keeping this separate from NetworkManager means f_ap_game.cpp only ever sees the tiny
 * dependency-free include/dusk/multiplayer.hpp — no ENet, no STL containers, nothing that could
 * bleed into decomp translation units. Keep this file trivial.
 */

namespace dusk::mp {
namespace {

aurora::Module Log{"dusk::mp"};

/// What the command line asked for, kept so a later lobby request can be merged under it.
struct CommandLineRequest {
    bool host = false;
    std::string connect;
    unsigned short port = 0;
    std::string nickname;
    std::string color;
    std::string trace;
};

CommandLineRequest sCommandLine;

/// See defer_session_start(). While true the tick hooks are skipped entirely, which keeps the
/// session from being opened before the player has chosen Host, Join or Play.
bool sSessionStartHeld = false;

std::string copy_or_empty(const char* value) {
    return value != nullptr ? std::string(value) : std::string();
}

/// `primary` if it is set, otherwise `fallback`. Null when neither is.
const char* prefer(const std::string& primary, const char* fallback) {
    if (!primary.empty()) {
        return primary.c_str();
    }
    return (fallback != nullptr && fallback[0] != '\0') ? fallback : nullptr;
}

}  // namespace

void apply_startup_options(const StartupOptions& options) {
    sCommandLine.host = options.host;
    sCommandLine.connect = copy_or_empty(options.connect);
    sCommandLine.port = options.port;
    sCommandLine.nickname = copy_or_empty(options.nickname);
    sCommandLine.color = copy_or_empty(options.color);
    sCommandLine.trace = copy_or_empty(options.trace);
    network_manager().set_startup_options(options);
}

void defer_session_start() {
    sSessionStartHeld = true;
}

void begin_deferred_session(const StartupOptions* lobby) {
    if (lobby != nullptr) {
        StartupOptions merged;
        const bool commandLineChoseSession = sCommandLine.host || !sCommandLine.connect.empty();
        if (commandLineChoseSession) {
            if (lobby->host || (lobby->connect != nullptr && lobby->connect[0] != '\0')) {
                Log.warn("The command line already chose a session (--mp-host/--mp-connect); "
                         "ignoring the Multiplayer menu's choice");
            }
            merged.host = sCommandLine.host;
            merged.connect = prefer(sCommandLine.connect, nullptr);
        } else {
            merged.host = lobby->host;
            merged.connect = prefer(sCommandLine.connect, lobby->connect);
        }
        merged.port = sCommandLine.port != 0 ? sCommandLine.port : lobby->port;
        merged.nickname = prefer(sCommandLine.nickname, lobby->nickname);
        merged.color = prefer(sCommandLine.color, lobby->color);
        merged.trace = prefer(sCommandLine.trace, lobby->trace);
        network_manager().set_startup_options(merged);
    }
    sSessionStartHeld = false;
}

void pre_actor_tick() {
    if (sSessionStartHeld) {
        return;
    }
    network_manager().pre_actor_tick();
}

void post_actor_tick() {
    if (sSessionStartHeld) {
        return;
    }
    network_manager().post_actor_tick();
}

bool is_active() {
    return network_manager().is_active();
}

}  // namespace dusk::mp
