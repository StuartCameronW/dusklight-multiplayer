#pragma once

#include <cstdint>
#include <string>

#include "../net/serializer.hpp"
#include "../policy/mp_policy.hpp"

/**
 * Batch 7: the host's MultiplayerPolicy is the session's.
 *
 * Until v24 every machine ran its own compiled-in g_mpPolicy, so the two ends could disagree
 * about whether something was shared at all, and nothing would say so: a host on "wallet: per
 * player" and a guest on "shared" would each leave the other's wallet out of its own diff and
 * never notice. Now the host sends its policy at admission (before any WorldSync, on the same
 * ordered channel) and again on every change, and a guest takes it as is. The guest's own values
 * are kept and put back when the session ends.
 *
 * NOT synced, on purpose:
 *   - renderRemotePlayers / showNameTags: presentation, each player's own.
 *   - multiplayer.friendlyFire (a cvar, not a policy field): victim-side, each player decides
 *     whether HE can be hit (d_a_remote_player.cpp initFriendlyFire).
 *
 * Wire layout, PacketId::PolicySync (26), host -> guest, reliable, kChannelControl:
 *   [u8 layout = 1][u8 n][u8 field]*n [s32 hostWallet]
 * Fields by index, APPEND-ONLY (see Field). A reader takes the first min(n, kFieldCount) it knows,
 * skips any it does not, and keeps its own value for any the sender did not send, so a field can be
 * added without breaking older readers of the same protocol. Anything after hostWallet is for later
 * layouts and is ignored. hostWallet is world_sync::host_wallet(): the shared wallet a guest
 * installs when rupees have just become Shared, or -1.
 */

namespace dusk::mp::policy_sync {

/// Wire indices of the PolicySync fields. Never renumber; only append.
enum class Field : std::uint8_t {
    Rupees = 0,
    Inventory = 1,
    Equipment = 2,
    Ammo = 3,
    Bottles = 4,
    Oil = 5,
    Magic = 6,
    QuestFlags = 7,
    SceneRoom = 8,
    TimeOfDay = 9,
    TimeAdvance = 10,
    AllowJoinPartner = 11,
    Health = 12,
    Transform = 13,
    OnDeath = 14,
};
inline constexpr std::uint8_t kFieldCount = 15;
inline constexpr std::uint8_t kLayout = 1;

/// Host: at session start, before anyone can join. Applies the persisted MP settings to
/// g_mpPolicy (the wallet toggle, multiplayer.sharedWallet) and logs the policy hosted with.
void on_host_started();

/// Host: the newly admitted player gets the session's policy. Call at admission, before anything
/// of the world can be sent to it.
void send_to(std::uint32_t playerId);

/// Host: make @p next the session's policy: rebases the world sync's shadow (so the switch emits
/// no world ops) and sends PolicySync to every guest. Outside a session it just sets g_mpPolicy.
/// A guest may not: logged and refused (returns false).
bool host_apply(const MultiplayerPolicy& next, const char* why);

/// Host convenience for the wallet toggle (UI, autopilot).
bool host_set_rupees(Ownership rupees, const char* why);

/// Guest: PolicySync from the host.
void on_packet(Reader& r);

/// Session over: a guest gets its own policy back. Safe to call on any role.
void reset();

/// True while this machine is a guest running the host's policy.
bool from_host();

/// "rupees=Shared inventory=Shared ..." — every synced field, for logs and expectations.
std::string describe(const MultiplayerPolicy& p);

const char* ownership_name(Ownership o);

}  // namespace dusk::mp::policy_sync
