#include "trace.hpp"

#include <chrono>
#include <fstream>

#include "../replication/player_bridge.hpp"
#include "../replication/replication_manager.hpp"
#include "dusk/logging.h"

namespace dusk::mp::trace {
namespace {

constexpr borealis::Log Log{"dusk::mp"};

std::ofstream sFile;
std::string sRole;
std::chrono::steady_clock::time_point sStart;
bool sEnabled = false;

/// Milliseconds since the trace opened. Wall time rather than tick count, because the two ends
/// have independent tick counters and only a clock lets their rows be lined up.
long long elapsed_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - sStart)
        .count();
}

void write_row(std::uint64_t tick, const char* kind, std::uint32_t playerId,
    const PlayerState& state, double delayTicks, std::uint32_t starvations, std::uint32_t snaps,
    std::size_t buffered, unsigned anm = 0, const DrawnJointAim& drawn = DrawnJointAim{}) {
    sFile << tick << ',' << elapsed_ms() << ',' << sRole << ',' << kind << ',' << playerId << ','
          << state.posX << ',' << state.posY << ',' << state.posZ << ',' << state.angleY << ','
          << state.moveRate << ',' << delayTicks << ',' << starvations << ',' << snaps << ','
          << buffered << ',' << anm << ',' << static_cast<unsigned>(state.flags) << ','
          << static_cast<unsigned>(state.equip) << ',' << static_cast<unsigned>(state.idleKind)
          << ',' << static_cast<unsigned>(state.atnKind) << ',' << state.neckX << ',' << state.neckY
          << ',' << state.bodyX << ',' << state.bodyTwist << ',' << state.bodyZ << ','
          << state.bankZ << ',' << drawn.headYawRel << ',' << drawn.headPitch << ','
          << drawn.torsoYawRel << ',' << drawn.armFires[0] << ',' << drawn.armFires[1] << ','
          << drawn.armFires[2] << ',' << drawn.clamps << '\n';
}

}  // namespace

void open(const std::string& path, const char* role) {
    sFile.open(path, std::ios::out | std::ios::trunc);
    if (!sFile) {
        Log.error("Could not open trace file '{}'; continuing without a trace", path);
        return;
    }

    sRole = role;
    sStart = std::chrono::steady_clock::now();
    sEnabled = true;
    // `moveRate` was called `speed` until protocol 6, and it is genuinely a different quantity now
    // — the sender's own getMoveGroundAngleSpeedRate(), roughly 0..1 — so the column is renamed
    // rather than quietly refilled. An analyzer written against the old name fails loudly.
    sFile << "tick,ms,role,kind,playerId,x,y,z,angleY,moveRate,delayTicks,starvations,snaps,"
             "buffered,anm,flags,equip,idleKind,atnKind,neckX,neckY,bodyX,bodyTwist,bodyZ,bankZ,"
             "headYawRel,headPitch,torsoYawRel,fireJ1,fireJ2,fireJ4,aimClamps\n";
    Log.info("Tracing poses to '{}'", path);
}

bool enabled() {
    return sEnabled;
}

void write_tick(std::uint64_t tick, std::uint32_t localPlayerId) {
    if (!sEnabled) {
        return;
    }

    PlayerState local;
    if (capture_local_player(local)) {
        /* The anm column on a local row is the sender's under slot 0 BCK, i.e. what his legs are
         * actually DRAWING. It was always 0 before. That's the value a puppet's own `actor` anm
         * has to be checked against. A plain-gait puppet reports its DOMINANT half, so the two
         * only line up exactly inside the attention set, where both report slot 0. */
        std::uint16_t localAnm = 0;
        read_local_anim(localAnm);
        // The DRAWN head and torso, off his body model's joints: the sender half of the
        // drawn-vs-drawn comparison (.claude/mp_head_compare.py).
        DrawnJointAim drawn;
        read_local_joint_aim(drawn);
        write_row(tick, "local", localPlayerId, local, 0.0, 0, 0, 0, localAnm, drawn);
    } else {
        // Written rather than skipped: a missing row is ambiguous between "loading" and "the trace
        // stopped", and the first probe run cost real time to that ambiguity.
        write_row(tick, "noworld", localPlayerId, PlayerState{}, 0.0, 0, 0, 0);
    }

    for (const auto& entry : replication_manager().players()) {
        const RemotePlayer& player = entry.second;
        write_row(tick, "applied", player.playerId, player.lastApplied, player.buffer.delay_ticks(),
            player.buffer.starvation_count(), player.buffer.snap_count(),
            player.buffer.sample_count());

        PlayerState actual;
        if (read_puppet_pose(player.playerId, actual)) {
            std::uint16_t anm = 0;
            read_puppet_anim(player.playerId, anm);
            DrawnJointAim drawn;
            read_puppet_joint_aim(player.playerId, drawn);
            write_row(tick, "actor", player.playerId, actual, 0.0, 0, 0, 0, anm, drawn);
        }
    }

    // Flushed every tick on purpose: these runs are usually ended by killing the process, and a
    // buffered tail would lose exactly the part worth reading.
    sFile.flush();
}

void close() {
    if (sEnabled) {
        sFile.flush();
        sFile.close();
        sEnabled = false;
    }
}

}  // namespace dusk::mp::trace
