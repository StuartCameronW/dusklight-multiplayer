#pragma once

#include <cstdint>

/**
 * The replicated equipment byte's layout — what a player has on his back and in his hands.
 *
 * ★ This lives in the PUBLIC dusk headers, alone, for one reason: it is the only part of the wire
 * format that BOTH sides of the layering rule have to agree on. The protocol layer
 * (multiplayer/replication/player_state.hpp) packs the byte; the puppet actor
 * (d/actor/d_a_remote_player.cpp) unpacks it to pick a model. Those two are otherwise deliberately
 * kept apart — the protocol layer is engine-free so it stays unit-testable headless, and only
 * player_bridge.cpp is allowed to include engine headers.
 *
 * The alternative was to decode the byte in player_bridge.cpp and pass six more arguments through
 * setNetworkPose, which is how the boolean flags are carried today. That does not scale to two
 * enumerations, and it would put "which value means the master sword" in two places — exactly the
 * kind of duplicate that drifts. Enumerations only: no engine types, no includes beyond <cstdint>,
 * so including this changes nothing about what either side can reach.
 */

namespace dusk::mp {

/**
 * Bit layout of PlayerState::equip.
 *
 * Every field is an ANSWER the sender already computed, never an input for the receiver to
 * recompute — the rule that retired three bugs in a row on the gait blend. `checkSwordDraw()` in
 * particular is not "does he own a sword": it also folds in the change-over timer that hides the
 * blade for five ticks while the model is swapped, and two no-draw flags.
 *
 * The KIND fields pick a model, so they are small enumerations rather than bit sets, and both have
 * an unused fourth value. They are meaningful only when the matching draw bit is set — but they are
 * reported regardless, because daAlink_c also selects a model and an archive before deciding
 * whether to draw them, and a receiver that only loaded a shield when one was currently visible
 * would have nothing ready at the moment it became visible.
 */
enum PlayerEquipFlags : std::uint8_t {
    /// daAlink_c::checkSwordDraw() (d_a_alink.cpp:19060) — draw the sword and its sheath at all.
    kPlayerEquipSwordDraw = 1 << 0,
    /// The sword is in his hand rather than on his back: `mEquipItem == 0x103`
    /// (d_a_alink.cpp:5894). The sheath stays on the back either way.
    kPlayerEquipSwordInHand = 1 << 1,
    /// Which sword, as a PlayerEquipSword value in bits 2-3.
    kPlayerEquipSwordKindShift = 2,
    kPlayerEquipSwordKindMask = 3 << 2,
    /// daAlink_c::checkShieldDraw() (d_a_alink.cpp:19065).
    kPlayerEquipShieldDraw = 1 << 4,
    /**
     * The shield is in his hand rather than on his back.
     *
     * ★ The single most necessary bit in this byte to replicate rather than derive. The sender's
     * condition is a seven-term disjunction over guard state, two demo procs, a guard-break proc, a
     * no-reset flag and a shield-on-backbone end flag (d_a_alink.cpp:5921-5930), and every term is
     * state a puppet has no access to whatsoever.
     */
    kPlayerEquipShieldInHand = 1 << 5,
    /// Which shield, as a PlayerEquipShield value in bits 6-7.
    kPlayerEquipShieldKindShift = 6,
    kPlayerEquipShieldKindMask = 3 << 6,
};

/// Sword models, in the order daAlink_c::setSelectEquipItem tests for them (d_a_alink.cpp:4354).
/// The sheath is not a separate choice: the wooden sword and the master sword share one (al_PODM),
/// and only the ordon sword has its own (al_PODA).
enum PlayerEquipSword : std::uint8_t {
    kPlayerEquipSwordOrdon = 0,
    kPlayerEquipSwordMaster = 1,
    kPlayerEquipSwordWood = 2,
    kPlayerEquipSwordKindNum = 3,
};

/// Shield models, in the order daAlink_c::setShieldArcName tests for them
/// (d_a_alink_swindow.inc:28-36). Each is a separate ARCHIVE, not a resource within one.
enum PlayerEquipShield : std::uint8_t {
    kPlayerEquipShieldCarvingWood = 0,
    kPlayerEquipShieldShopWood = 1,
    kPlayerEquipShieldHylian = 2,
    kPlayerEquipShieldKindNum = 3,
};

}  // namespace dusk::mp
