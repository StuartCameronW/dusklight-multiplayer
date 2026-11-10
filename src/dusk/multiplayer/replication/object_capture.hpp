#pragma once

/**
 * Owner-side capture for the phase-1 object kinds (.claude/future-plans/mp-object-channel.md §2, §6).
 *
 * Each kind's file scans the local game for the real actors this machine simulates (its own
 * arrows, bombs, boomerang, rod ball), and drives the owner API in object_channel.hpp for them:
 * spawn_transient when one appears, update_owned every tick it lives, send_event for its
 * one-off moments, despawn_owned when it goes.
 *
 * object_channel::tick() calls every tick_*() once per sim tick while a session is active,
 * BEFORE the channel streams owned objects (so a spawn captured this tick goes out this tick).
 * object_channel::reset() calls every reset_*(): forget all tracked actors, send nothing.
 *
 * Each pair is owned by exactly one file: object_capture_<kind>.cpp.
 */

namespace dusk::mp::objects::capture {

/// Arrows and slingshot pellets (ObjKind::Arrow, ObjKind::Pellet). object_capture_arrow.cpp.
void tick_arrow();
void reset_arrow();

/// Bombs, water bombs, bomblings (ObjKind::Bomb, WaterBomb, Bombling) and their explosion event.
/// object_capture_bomb.cpp.
void tick_bomb();
void reset_bomb();

/// The Gale Boomerang (ObjKind::Boomerang). object_capture_boom.cpp.
void tick_boomerang();
void reset_boomerang();

/// The Dominion Rod ball (ObjKind::RodBall). object_capture_crod.cpp.
void tick_rodball();
void reset_rodball();

}  // namespace dusk::mp::objects::capture
