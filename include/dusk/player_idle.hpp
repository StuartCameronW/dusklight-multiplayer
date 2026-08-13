#pragma once

#include <cstdint>

/**
 * The replicated idle byte — which of daAlink_c's standing animations the sender actually chose.
 *
 * ★ Here for exactly the reason `player_equip.hpp` is here: it is a part of the wire format that
 * BOTH sides of the layering rule have to agree on. The protocol layer
 * (multiplayer/replication/player_state.hpp) packs the byte; the puppet actor
 * (d/actor/d_a_remote_player.cpp) unpacks it to pick an animation. Those two are otherwise kept
 * apart on purpose — the protocol layer is engine-free so it stays unit-testable headless, and only
 * player_bridge.cpp may include engine headers.
 *
 * It was written twice before it was written once: the receiver landed with its own mirrored copy
 * of these values, which compiles perfectly and plays the WRONG ANIMATION the day either side is
 * renumbered, with nothing to catch it. One definition, no drift.
 *
 * Enumeration only: no engine types, no includes beyond <cstdint>, so including this changes
 * nothing about what either side can reach.
 */

namespace dusk::mp {

/**
 * Which idle daAlink_c is actually playing.
 *
 * Not a set of independent flags, and that is the whole design. `setBlendMoveAnime` picks exactly
 * ONE (d_a_alink.cpp:7585-7598, :7732-7737), and `procWait` / `procTiredWait` can replace it
 * outright (:15624-15625, :15637-15643). Spending a bit each on states that cannot co-occur would
 * put illegal combinations on the wire for the receiver to rank — and ranking them is precisely the
 * re-derivation this field exists to stop.
 *
 * The comment beside each name is the `daAlink_c::daAlink_ANM` enumerator it stands for. The ANM
 * ids themselves are NOT sent: they are 16-bit, and they are an engine detail this layer is not
 * allowed to know, so this byte is the stable wire name for them and the puppet is the single place
 * that maps it back.
 *
 * A byte with 248 spare values means finishing the set costs no protocol bump, so every kind worth
 * having is named here now even where neither end handles it yet:
 *   - `kPlayerIdleInsect` is named but not sampled — the sender's chain does test for it, but no
 *     receiver plays it.
 *   - `kPlayerIdleAtnLeft` / `Right` are the lock-on stance, deliberately deferred to job D3b:
 *     shipping the stance without the matching ATN strafe set would trade a wrong-but-stable pose
 *     for one that pops the instant its owner moves.
 *
 * `kPlayerIdleWait` is the default and the safe reading — the ordinary standing idle, which is what
 * the puppet played before this field existed.
 */
enum PlayerIdleKind : std::uint8_t {
    kPlayerIdleWait = 0,      ///< ANM_WAIT 0x19 — WAITS, hands 4/10
    kPlayerIdleWaitB = 1,     ///< ANM_WAIT_B 0x1A — WAITB, hands 1/6
    kPlayerIdleTired = 2,     ///< ANM_WAIT_TIRED 0xB6 — WAITD; also swaps the walk floor to 0.4
    kPlayerIdleService = 3,   ///< ANM_SERVICE_WAIT 0x90 — SWAITA, the idle fidget
    kPlayerIdleWind = 4,      ///< ANM_WAIT_WIND 0xFF — WAITWIND
    kPlayerIdleInsect = 5,    ///< ANM_WAIT_INSECT 0x185
    kPlayerIdleAtnLeft = 6,   ///< ANM_ATN_WAIT_LEFT 0x10 — the lock-on stance (D3b)
    kPlayerIdleAtnRight = 7,  ///< ANM_ATN_WAIT_RIGHT 0x11 — the lock-on stance (D3b)
};

}  // namespace dusk::mp
