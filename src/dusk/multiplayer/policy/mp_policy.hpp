#pragma once

/**
 * ★ Single source of truth for every "is this system shared or per-player?" decision.
 *
 * Replication code must ASK the policy — it must never hardcode a shared-vs-individual
 * assumption inline. If you ever find yourself writing `if (shared)` anywhere under
 * src/dusk/multiplayer/replication/, it has to read a field from here instead.
 *
 * Adding a new syncable system means adding a field here FIRST. This is what makes the model
 * flick-able (and UI-bindable later) rather than a scatter of assumptions.
 *
 * v1 defaults: one shared, host-authoritative world — EXCEPT health, which is per-player.
 * See .claude/plan/03-sync-targets.md and 04-architecture.md.
 */

namespace dusk::mp {

/// Shared = host-authoritative, one world. Individual = each player owns their own copy.
enum class Ownership {
    Shared,
    Individual,
};

/**
 * When the shared clock is allowed to ADVANCE, given that players can be in different places.
 *
 * Only matters when players are apart — with everyone in one room all three agree. This is about
 * the passive tick only (`daytime += time_change_rate`, d_kankyo.cpp:1550); an explicit time SET,
 * from a cutscene or a time-control tag, is a different thing entirely and is never gated by this.
 *
 * Vanilla decides per ROOM: `dComIfGp_roomControl_getTimePass()` reads a flag out of the stage's
 * room data (d_stage.cpp:1511), and it is off in dungeons, houses and several towns.
 */
enum class TimeAdvanceRule {
    /// Everyone must be somewhere time can pass. Stuart's call, 2026-08-13, and the most faithful
    /// to
    /// vanilla's "time does not move in here".
    ///
    /// ★ Known cost, stated because it was argued before it was chosen: one player idling indoors
    /// stops the clock for everyone, so this is the only rule under which a player can be prevented
    /// from ever reaching night. Players who are still loading do not count — otherwise a slow
    /// stage transition on one machine would freeze the world for the rest.
    AllPlayers,
    /// One player outdoors is enough. Never stalls; the cost is that your sky can change while you
    /// stand somewhere vanilla would have held it still.
    AnyPlayer,
    /// Guests simply mirror the host's clock and their own rooms are not consulted. Cheapest — no
    /// per-player reporting at all — but the host alone decides, and can stall everyone.
    HostRoom,
};

/// What happens when a player's hearts reach zero. Product decision, pending (see 00-status.md).
enum class DeathRule {
    ReloadRoom,
    RespawnAtCheckpoint,
    DownedUntilRevived,
};

struct MultiplayerPolicy {
    Ownership rupees = Ownership::Shared;
    Ownership inventory = Ownership::Shared;  ///< item slots / owned items
    Ownership equipment = Ownership::Shared;  ///< visible sword / shield / clothes
    Ownership ammo = Ownership::Shared;       ///< arrows, bombs
    Ownership magic = Ownership::Shared;
    Ownership questFlags = Ownership::Shared;  ///< story flags, chests, switches, dungeon items
    Ownership sceneRoom = Ownership::Shared;   ///< everyone in the same stage/room (lockstep)
    Ownership timeOfDay = Ownership::Shared;

    /// Only read when timeOfDay is Shared. See TimeAdvanceRule for what each one costs.
    TimeAdvanceRule timeAdvance = TimeAdvanceRule::AllPlayers;

    /// The one v1 exception — each client keeps its own mLife/mMaxLife.
    Ownership health = Ownership::Individual;

    /// Wolf/human. Decided by Stuart 2026-08-12: players may be in different forms at once, and
    /// that is intended rather than tolerated. Note this is a GAMEPLAY decision, not the same one
    /// as `equipment` above — a costume propagates from its wearer, but being a wolf changes what
    /// a player can do (doors, items, Midna), so M3 has to cope with the two forms diverging
    /// rather than assume a single world state. tpmp reached the same answer.
    Ownership transform = Ownership::Individual;

    DeathRule onDeath = DeathRule::ReloadRoom;

    // --- Presentation. Not ownership decisions, but they belong in the same settings object so
    // --- the UI binds to one place. tpmp bolted its settings menu on afterwards (08).

    /// tpmp's "optimization level 3" let players stop drawing other Links entirely to claw back
    /// frames. Read by the puppet actor at M1 rather than checked at the draw site.
    bool renderRemotePlayers = true;

    /// Floating name tag over each remote player (M2).
    bool showNameTags = true;
};

/// THE single source of truth. Hardcoded for now; the settings UI writes to it later.
inline MultiplayerPolicy g_mpPolicy{};

}  // namespace dusk::mp
