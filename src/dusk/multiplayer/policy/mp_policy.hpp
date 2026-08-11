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

    /// The one v1 exception — each client keeps its own mLife/mMaxLife.
    Ownership health = Ownership::Individual;

    /// Wolf/human. tpmp made this Individual and that is probably better; one flag to change.
    Ownership transform = Ownership::Shared;

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
