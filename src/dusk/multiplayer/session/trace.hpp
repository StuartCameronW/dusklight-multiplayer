#pragma once

#include <cstdint>
#include <string>

/**
 * Per-tick CSV of what every player's pose actually is, on this instance.
 *
 * The point is to make "does the puppet move correctly" a measurement rather than an opinion:
 * run both ends with --mp-trace, then compare the sender's own pose against the pose the receiver
 * ended up putting on the puppet. Position error, added latency and jitter all fall straight out
 * of the two files; smoothness does not have to be judged by eye.
 *
 * Rows are long-format (one row per subject per tick) because the number of players varies:
 *
 *   tick,ms,role,kind,playerId,x,y,z,angleY,speed,delayTicks,starvations,snaps,buffered,anm
 *
 * `kind` is `local` for this instance's own Link, `applied` for the pose pushed onto a puppet, and
 * `actor` for the pose read back off the puppet actor afterwards. `applied` and `actor` disagreeing
 * means the puppet is not honouring what it was given, which is a different bug from a bad pose.
 *
 * `anm` is set on `actor` rows only: the AlAnm resource index of the gait the puppet chose. It is
 * here because a wrong gait is otherwise only visible on the OTHER player's screen, which makes it
 * the one class of bug the harness could not catch on its own.
 */

namespace dusk::mp::trace {

/// Start writing. Failure to open logs and leaves tracing off rather than failing the session.
void open(const std::string& path, const char* role);

bool enabled();

/// Sample every player. Called after the actor pass, when the puppets have executed.
void write_tick(std::uint64_t tick, std::uint32_t localPlayerId);

void close();

}  // namespace dusk::mp::trace
