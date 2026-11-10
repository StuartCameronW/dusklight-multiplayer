#pragma once

#include <cstdint>

#include "../net/packet.hpp"
#include "../net/serializer.hpp"
#include "object_channel.hpp"

/**
 * Shared drops (batch 7, protocol 24; Stuart's decision in
 * .claude/plan/17-decisions-2026-11-10.md): a rupee, heart, arrow bundle or seed bag that falls out
 * of a pot, a tuft of grass, a rock or a barrel one player broke is visible to every player in the
 * room, and the FIRST to touch it gets it. Enemy kill drops (M6) reuse this unchanged: "one drop,
 * first come". Until then they stay local: fopAcM_createItemFromEnemyID (the enemy item tables,
 * also used by the death smoke) wraps its create in drops_enemy_scope, because the enemy itself is
 * not shared yet and a rupee would pop out of one still alive on the partner's screen. M6 deletes
 * that scope. An enemy that calls fopAcM_createItem directly for a fixed drop is NOT caught by it
 * [unverified: none checked].
 *
 * ---- Owner side (the machine whose world made the drop) ----
 * The item helpers that every breakable calls (fopAcM_createItem, fopAcM_fastCreateItem,
 * fopAcM_fastCreateItem2, f_op_actor_mng.cpp) report each daItem_c they create
 * (drops_note_created). That is what tells a run-time drop from a stage-placed item: a placed item
 * exists on both machines already and must never be announced. tick_drop then looks at each new
 * one once its create has finished and announces it as ObjKind::Drop when it is:
 *   - a consumable: heart, any rupee, arrows (1/10/20/30), slingshot seeds;
 *   - without a save bit (mItemBitNo 0xFF). An item with a bit is the chest/floor-item claim's
 *     business (item_claims, H5) and already exists on both ends;
 *   - a plain launched drop (types LAUNCH, FIXED_PLACE, WAIT, LAUNCH_NO_RND, LAUNCH_SMALL,
 *     LAUNCH_FROM_PLAYER), not waiting on a switch, not a forced or simple "get" (an item handed
 *     straight to the player), not a randomizer-overridden item.
 * Its pose streams every tick it moves (yaw frozen once it rests: the receiver spins its own copy),
 * kObjPoseHidden mirrors its pre-timeout flashing, and it despawns when it is picked up (by anyone)
 * or times out.
 *
 * ---- Receiver side ----
 * Not a ghost: the local copy is a real daItem_c (create_drop), so the local player can pick it
 * up. It is created only when the owner's timeline reaches the spawn (the pot breaks on the
 * partner's puppet, then the rupee pops out), in the local Link's room. Its own physics and
 * timers are switched off (FLAG_NO_MOVE_e | FLAG_NO_TIMER_e on its first execute): it is placed
 * on the played-back pose every tick (apply_drop), and goes when the owner's despawn reaches
 * playback. A copy that cannot be created (the room is not shown here, a "no heart drops" setting)
 * simply does not exist on that end.
 *
 * ---- Pickup: one grant per drop ----
 * Touching a shared drop on EITHER machine, the breaker's own included, asks item_claims for
 * {ClaimKind::Drop, tbl = owner id, no = serial} before anything is granted (drops_claim_pickup,
 * from daItem_c::itemGetNextExecute). The host's dropGrants table makes exactly one grant per drop,
 * forever, so two players touching it on the same tick cannot both collect it — which matters
 * because the wallet is shared: a rupee granted twice would be counted twice.
 *   - Granted: the normal pickup runs (a rupee goes into the wallet and the world sync carries the
 *     delta once; a heart heals the picker only). The owner despawns its drop (PickedUp); a
 *     receiver forgets its copy, which finishes its pickup animation on its own.
 *   - Pending: the item stays. The touch is remembered and asked again every tick until it
 *     resolves, so a player who ran through it still gets it once the grant arrives.
 *   - Denied (someone else has it): the copy vanishes without effect.
 *   - A guest's claim that timed out (host silent for 3 s) is NOT treated as taken: the item stays
 *     and is asked for again. Nothing is granted on a timeout either.
 * When the host grants a drop it tells every guest (DropTaken, 27): the owner removes its drop and
 * despawns it, everyone else removes their copy at once instead of waiting for the despawn.
 *
 * Outside a live session with claims in force (item_claims::active()) nothing here runs: vanilla.
 * Every log line starts "[mp-drop]".
 */

namespace dusk::mp::objects::drops {

/// Host, from item_claims' judge: drop T<owner>:<serial> was just granted to `taker`. Applies it
/// here and tells every guest (DropTaken).
void host_on_granted(std::uint8_t owner, std::uint16_t serial, std::uint32_t taker);

/// DropTaken on a guest. `r` is positioned just after the PacketId byte.
void on_packet(PacketId id, Reader& r, std::uint32_t fromPlayerId);

/// What this end knows about one daItem_c, for the autopilot's dropnear / expectdrops.
struct DropView {
    /// Announced by this end (owner) or a copy of a partner's (remote).
    bool shared = false;
    bool remote = false;
    /// Picked up or taken away; the actor may still be finishing its pickup animation.
    bool consumed = false;
    /// A pickup claim is out and unresolved.
    bool claimPending = false;
    ObjRef ref;
};

/// False for a procId this end does not track (a local-only item, or not an item at all).
bool view(std::uint32_t procId, DropView& out);

}  // namespace dusk::mp::objects::drops
