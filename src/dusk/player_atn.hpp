#pragma once

#include <cstdint>

/**
 * The replicated attention byte: which of daAlink_c's attention (Z-target / lock-on) move sets
 * the sender is playing, and in which direction.
 *
 * It lives here for the same reason `player_idle.hpp` and `player_equip.hpp` do. The protocol
 * layer packs it and the puppet actor unpacks it, and those two are otherwise kept apart (the
 * protocol layer is engine-free). One definition means the two sides can't drift.
 *
 * The values are mutually exclusive by construction. `setBlendAtnMoveAnime` picks exactly one
 * direction (`field_0x2f98`, d_a_alink.cpp:7870-7899). Inside that same call,
 * `mTargetedActor != NULL` picks the locked animations and the daAlinkHIO_atnMove_c0 table over
 * daAlinkHIO_noActAtnMove_c0.
 *
 * ★ "Forward" happens WITHOUT a target too. That was measured, and it contradicts a plausible
 * reading of the source. With no target, a stick pushed straight ahead takes the ordinary gait
 * (`sp3C`, :7932-7936). But field_0x2f98 keeps its 0 once the stick is released, so the next tick
 * plays ATN_RETURN_FROM_WALK on the no-target table (:7967-7970). The free and locked variants
 * share their animations and differ only in the band table.
 *
 * ★ The sender samples the answer off his own animation heap (fill_atn_kind, player_bridge.cpp).
 * It doesn't re-derive it from his input. The puppet maps each value back to the exact animation
 * pair and HIO table daAlink_c uses for it.
 */

namespace dusk::mp {

enum PlayerAtnKind : std::uint8_t {
    kPlayerAtnNone = 0,         ///< not in an attention move set; the ordinary gait applies
    kPlayerAtnForward = 1,      ///< ATN_WAIT_RIGHT + ATN_RETURN_FROM_WALK/RUN (0x11, 0x00, 0x01)
    kPlayerAtnForwardLock = 2,  ///< the same pair, on the locked-on band table
    kPlayerAtnBack = 3,         ///< ANM_WAIT + ATN_BACK_WALK/RUN (0x19, 0x02, 0x03)
    kPlayerAtnBackLock = 4,     ///< ATN_WAIT_RIGHT + ATN_LOCK_BACK_WALK/RUN (0x11, 0x04, 0x05)
    kPlayerAtnLeft = 5,         ///< ATN_WAIT_LEFT + ATN_WALK_LEFT/RUN_LEFT (0x10, 0x06, 0x0A)
    kPlayerAtnLeftLock = 6,     ///< ATN_WAIT_LEFT + ATN_LOCK_WALK_LEFT/RUN_LEFT (0x10, 0x07, 0x0B)
    kPlayerAtnRight = 7,        ///< ATN_WAIT_RIGHT + ATN_WALK_RIGHT/RUN_RIGHT (0x11, 0x08, 0x0C)
    kPlayerAtnRightLock = 8,  ///< ATN_WAIT_RIGHT + ATN_LOCK_WALK_RIGHT/RUN_RIGHT (0x11, 0x09, 0x0D)
    /// ATN_WAIT_RIGHT + ATN_BACK_WALK_HEAVY as both walk and run (0x11, 0x0F): backing off in iron
    /// boots, setBlendAtnBackMoveAnime's first branch (d_a_alink.cpp:8092-8102). A receiver that
    /// predates this value treats it as kPlayerAtnNone, which is what the sender used to send.
    kPlayerAtnBackHeavy = 9,
};

}  // namespace dusk::mp
