#pragma once

#include <cstdint>

/**
 * The replicated damage-tint values (PlayerState v20 effects block): which of daAlink_c's body
 * tints the sender is showing, and what PlayerState::dmgFlash means for each.
 *
 * Public and alone for the same reason as player_equip.hpp and player_held.hpp: the protocol layer
 * packs these and the puppet actor (d/actor/d_a_remote_player_fx.inc) unpacks them, and neither may
 * include the other. Enumerations only.
 *
 * Every value is the sender's ANSWER: the branch daAlink_c::draw took in its tint ladder
 * (d_a_alink.cpp:19530-19583) and the timer that branch reads. The receiver turns them back into a
 * colour with the same HIO constants (daAlinkHIO_damage_c0::m), so the colours are never on the
 * wire and the two ends cannot disagree about them.
 */

namespace dusk::mp {

/**
 * The tint kind, in PlayerState::fxFlags bits 4-5 (kPlayerFxDmgTintShift / kPlayerFxDmgTintMask).
 *
 * Only the three branches of his ladder that are "being hurt" are sent. The ones above them —
 * field_0x06f4's BRK, the transform / dungeon-warp fade, the wolf light-drop — and the sword-raise
 * glow below them are other features, and the sender sends None while one of those owns the colour.
 */
enum PlayerDmgTint : std::uint8_t {
    kPlayerDmgTintNone = 0,
    /// The red pulse after a hit: `mDamageTimer != 0 && !checkMagicArmorNoDamage()`. dmgFlash is
    /// his `mDamageTimer + mDamageColorTime`, clamped to 0..255 — the colour_timer the ladder and
    /// the |sin| pulse are computed from (TevKColor, body/head/face/hands only).
    kPlayerDmgTintDamage = 1,
    /// Frozen solid: checkFreezeDamage(). dmgFlash is 0. A flat mFreezeR/G/B (TevColor).
    kPlayerDmgTintFreeze = 2,
    /// Thawing out of a freeze: `mIceDamageWaitTimer != 0`. dmgFlash is that timer, clamped to
    /// 0..255; the colour blends mFreezeInit* to mFreeze* by timer / mFreezeTime (TevColor).
    kPlayerDmgTintIce = 3,
};

}  // namespace dusk::mp
