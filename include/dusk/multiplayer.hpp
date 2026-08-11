#pragma once

/**
 * Dusklight multiplayer — the only surface the game loop touches.
 *
 * Everything else lives under src/dusk/multiplayer/. These two functions are called from
 * f_ap_game.cpp around the actor pass; see .claude/plan/04-architecture.md for the design and
 * .claude/plan/07-milestones.md for the build order.
 *
 * Deliberately dependency-free: this header is included by decomp code, so it must not drag in
 * ENet, the STL, or any engine type.
 */

namespace dusk::mp {

/**
 * A session asked for on the command line.
 *
 * Plain C types on purpose: this header is included by decomp translation units, so it must not
 * pull in <string>. The strings are copied when the options are applied, so the caller keeps
 * ownership of them.
 */
struct StartupOptions {
    /// --mp-host: open a session and wait for players.
    bool host = false;
    /// --mp-connect: "address" or "address:port". Null or empty means no join was requested.
    const char* connect = nullptr;
    /// --mp-port: 0 leaves the port at its default.
    unsigned short port = 0;
    /// --mp-name: nickname shown to the other players.
    const char* nickname = nullptr;
    /// --mp-color: "RRGGBB" hex. Null or empty leaves the colour to the automatic palette.
    const char* color = nullptr;
    /// --mp-trace: development pose trace, written per sim tick. See multiplayer/session/trace.hpp.
    const char* trace = nullptr;
};

/**
 * Record what the command line asked for. Must be called before the first tick; the session is
 * actually opened lazily on that tick, once the game's own subsystems are up.
 *
 * These win over the DUSK_MP_* environment variables, which stay supported because they survive
 * a process restart and because the two-instance test script uses them.
 */
void apply_startup_options(const StartupOptions& options);

/// Called from fapGm_Before, immediately BEFORE the actor pass runs.
/// Drains the transport and applies inbound state so actors execute against fresh data.
/// Cheap and safe to call when multiplayer is inactive.
void pre_actor_tick();

/// Called from fapGm_AfterRecord, immediately AFTER the actor pass runs.
/// Captures local state, sends it, and flushes the transport.
/// Cheap and safe to call when multiplayer is inactive.
void post_actor_tick();

/// True once a session is live (hosting, or connected to a host).
bool is_active();

}  // namespace dusk::mp
