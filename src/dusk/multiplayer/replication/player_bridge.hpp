#pragma once

#include <cstdint>

#include "player_state.hpp"

/**
 * The ONLY seam between multiplayer code and the game engine.
 *
 * Everything else under src/dusk/multiplayer/ is engine-free: no fopAc_ac_c, no cXyz, no actor
 * headers. That is what keeps the protocol and the interpolation testable on their own and stops
 * engine types leaking into the network layer (layering rule, 04-architecture.md).
 *
 * Exactly one translation unit — player_bridge.cpp — includes decomp headers and implements these.
 * Every function must be safe to call when there is no Link, no scene, or no puppet: the tick
 * hooks keep firing through loads, and are skipped entirely during DVD-error and shutdown states.
 */

namespace dusk::mp {

/// True when the game is in real, playable gameplay — not a menu, a load, a cutscene, or the
/// title screen's attract demo. Exported rather than kept private because the autopilot needs the
/// identical test: when the two definitions drifted apart, scripts believed they were in the world
/// several hundred ticks before replication agreed, and the resulting traces were nonsense.
bool world_is_playable();

/// True from the moment this end queues a scene change (exit, warp, death, void-out) until the new
/// scene's Link exists. The sender stops claiming to be in the world for that whole stretch.
bool local_scene_change_queued();

/// Which event the local Link is in, read off HIS proc and demo type (the answers his own
/// setDemoData / checkDemoAction chose). The numbering matches the planned v21 wire byte
/// (.claude/future-plans/puppet-events.md §6), but nothing puts it on the wire yet.
enum class LocalEventKind : std::uint8_t {
    None = 0,      ///< no event, no Link, a Link still being created, or the title's attract demo
    Talk = 1,      ///< PROC_TALK: an NPC, a sign, Midna
    Chest = 2,     ///< PROC_OPEN_TREASURE
    GetItem = 3,   ///< PROC_GET_ITEM
    Door = 4,      ///< PROC_DOOR_OPEN
    InPlace = 5,   ///< DEMO_TYPE_SPECIAL_e: bottle, oil, whistle, boomerang catch, death
    Scripted = 6,  ///< any other event proc that draws ALANM (the stage-entry walk-in, warps)
    Cutscene = 7,  ///< a TOOL demo / PROC_TOOL_DEMO, or a demo-archive BCK during an event
};

LocalEventKind local_event_kind();
const char* local_event_kind_name(LocalEventKind kind);

/// True when the player actor that exists now is the one the last successful capture read. False
/// when there is no Link, or when a different one exists, such as a new scene's Link that is not
/// capturable yet. A held pose from the old Link must not then claim to be in the world.
bool local_player_is_last_captured();

/// Read the local player's pose. False when Link doesn't currently exist (loading, title screen),
/// in which case nothing should be sent this tick.
bool capture_local_player(PlayerState& out);

/// v21: rewrite a HELD pose's form block (form, metaTint, furFrame) from the local Link as he is
/// now. For wire_local_state while capture is gated off — above all the model swap of a
/// transformation (mClothesChangeWaitTimer != 0), which is exactly when kPlayerFormChanging must
/// reach the far end. Reads scalars on daAlink_c only, never a model: his model heap is freed for
/// part of that window. No-op unless the Link that exists is the one the pose was captured from.
void refresh_held_form(PlayerState& held);

/// Where a remote sender is, relative to what this instance is drawing (v16 location fields).
enum class RemoteLocation : std::uint8_t {
    /// Same stage, and in a room this end displays (or the sender has not said where he is).
    Here = 0,
    /// A different stage. His puppet has nothing to stand on here.
    OtherStage = 1,
    /// Same stage, but in a room this end is not displaying (checkRoomDisp), so not loaded or
    /// not drawn: his puppet would float over nothing.
    RoomNotShown = 2,
};

/// Classify a remote pose's location against the local scene. Safe with no scene: answers Here,
/// so the existing "no playable world" gates stay the only ones that apply then.
RemoteLocation remote_location(const PlayerState& pose);

/// Create the puppet actor for `playerId` if it isn't alive yet, wearing the outfit the sender
/// reported. Returns false when creation isn't possible right now (no scene loaded, allocation
/// refused) — callers just retry next tick.
///
/// Calling it every tick is correct and cheap: it owns the whole spawn policy, including backing
/// off after a failed creation and eventually disabling a puppet that will not come up. A false
/// return therefore does NOT mean "try harder"; it means there is no puppet this tick, for a reason
/// the log already carries. See the failure-policy block at the top of player_bridge.cpp.
///
/// v21: `wolf` (the pose's kPlayerStateWolf) builds a wolf puppet instead: the create parameter's
/// top byte is then 0xFE, and `outfit` (still his human clothes) is not used for this build.
bool ensure_puppet(std::uint32_t playerId, std::uint32_t colorRgb, std::uint8_t outfit, bool wolf);

/// Destroy `playerId`'s puppet if it is wearing an outfit — or, since v21, a FORM (human/wolf) —
/// the sender is no longer in, so the next ensure_puppet() rebuilds it right. Returns true when it
/// did — the caller should skip the rest of this tick for that player. No-op when the puppet is
/// absent, still being created, or already correct. A wolf puppet's outfit is not compared: it has
/// no clothes.
bool reconcile_puppet_outfit(std::uint32_t playerId, std::uint8_t outfit, bool wolf);

/// Push an interpolated pose onto an existing puppet. No-op if the puppet isn't alive.
void apply_puppet_state(std::uint32_t playerId, const PlayerState& state);

/// Read back where the puppet actor actually is. False when it doesn't exist. Distinct from the
/// pose we last pushed: if the two disagree, the puppet is not honouring what it was given.
bool read_puppet_pose(std::uint32_t playerId, PlayerState& out);

/// Read back which gait animation the puppet chose, as an AlAnm resource index. Kept out of
/// PlayerState, which is a wire type: this is local presentation, not replicated state.
bool read_puppet_anim(std::uint32_t playerId, std::uint16_t& out);

/// The BCK resource index in the LOCAL player's under slot 0, i.e. what he is drawing on his legs
/// right now, every substitution applied. For --mp-trace, so a puppet's choice can be compared
/// against what its sender actually played. 0 (false) when there is no playable Link.
bool read_local_anim(std::uint16_t& out);

/**
 * The head turn and torso twist as they are DRAWN, for --mp-trace (worklist 14).
 *
 * Measured off the body model's joint matrices after calc, not off the wire values that feed them,
 * so a sign or axis mistake in the puppet's joint callbacks shows up as a number. Comparing wire to
 * wire would pass a mirrored head. All angles in degrees, relative to the actor's shape_angle.y.
 *
 *  - headYawRel / headPitch: the yaw and pitch of joint 4's X axis — the same "which way the head
 *    points" axis the hat sway reads (d_a_alink.cpp:5596-5599).
 *  - torsoYawRel: the yaw of joint 2's Y axis. Joint 2's X runs up the spine (daAlink_c's cap code
 *    treats it as such, :2706-2713), so Y is the horizontal one. It carries an anatomical constant
 *    that is the same on both ends; only drawn-vs-drawn differences are meaningful.
 *
 * `armFires` is the puppet's per-arm callback counter (joint 1, 2, 4) — zero on a local row.
 * `clamps` is the sender's running count of joint-aim values clamped to the wire range — zero on a
 * puppet row. Both exist so "attached but never fires" and "fires but wraps" are numbers.
 */
struct DrawnJointAim {
    float headYawRel = 0.0f;
    float headPitch = 0.0f;
    float torsoYawRel = 0.0f;
    std::uint32_t armFires[3] = {0, 0, 0};
    std::uint32_t clamps = 0;
};

/// The local player's drawn head/torso. False when there is no playable Link.
bool read_local_joint_aim(DrawnJointAim& out);
/// A puppet's drawn head/torso. False when the puppet does not exist.
bool read_puppet_joint_aim(std::uint32_t playerId, DrawnJointAim& out);

/// World position of joint 0 (the root) as the LOCAL player's body was last calc'd, for
/// --mp-trace's root-motion check (puppet-action-states.md §4.5). False when there is no playable
/// Link.
bool read_local_root(float& x, float& y, float& z);

/// The same for a puppet: joint 0 as its own calc left it. False when it doesn't exist.
bool read_puppet_root(std::uint32_t playerId, float& x, float& y, float& z);

/// The stride phase (0..1) the LOCAL player is drawing, unquantised, for --mp-trace's `locPhase`
/// column: his under slot 0 frame over its end, while a blended pair is playing. -1 (and false)
/// when it isn't, or there is no playable Link.
bool read_local_loc_phase(float& out);
/// The same off a puppet's own animation object. -1 (and false) outside blended locomotion.
bool read_puppet_loc_phase(std::uint32_t playerId, float& out);
/// Despawn one puppet (peer disconnected).
void destroy_puppet(std::uint32_t playerId);

/// Despawn every puppet (session ended, or the scene is being torn down).
void destroy_all_puppets();

}  // namespace dusk::mp
