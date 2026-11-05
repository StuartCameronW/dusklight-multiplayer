#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dusk::mp {

class Reader;
class Writer;

/**
 * Which mods each end is running, compared at the handshake so two players never share a world
 * that the two machines are building differently.
 *
 * ★ WHY THIS EXISTS. A gameplay mod that only one player has does not fail loudly. A randomizer
 * puts different items in the same chest; a rebalance mod changes what a hit does; a mod that
 * replaces a stage file moves a door. Both ends connect, both see each other walking around, and
 * the session quietly desyncs in ways nobody can diagnose from the outside. So the host refuses the
 * join and the guest is told, by name, which mods differ — the same principle as the protocol
 * version check, applied to the other thing that changes what the game does.
 *
 * ★ COSMETIC VS GAMEPLAY. The mod manifest has no category or "cosmetic" flag, so the split is
 * derived from what the bundle can actually DO, which the loader already decides for itself: a mod
 * with no code (no native library under lib/, no external native lib, no script runtime) can
 * only act through two directories — textures/ (texture replacements) and overlay/ (disc file
 * replacements). Texture replacement cannot change game logic. A disc overlay can: it may replace
 * a stage, a message table or an actor archive, and there is no cheap way to tell which. So:
 *
 *   gameplay  = active, and has code (native or runtime) OR ships lib/ OR ships any overlay/ file
 *   cosmetic  = active, and none of the above (textures/, icons, mod.json, inert files)
 *
 * The rule is read off the BUNDLE, not off what loaded on this machine, so both ends classify the
 * same package the same way even when one of them could not load its native library (a mod with
 * only a Windows DLL is still a code mod on the Mac that could not load it). It errs towards
 * "gameplay": a pure model-swap overlay is refused rather than allowed.
 *
 * Inactive mods (disabled, or failed to activate) are not listed at all: they change nothing.
 */
struct ModEntry {
    std::string id;
    std::string version;
};

struct ModFingerprint {
    /// FNV-1a over EVERY gameplay mod's id and version, sorted by id, so a difference past the
    /// listing cap still shows up as a mismatch.
    std::uint32_t gameplayHash = 0;
    /// True totals, which may exceed what is listed.
    std::uint16_t gameplayTotal = 0;
    std::uint16_t cosmeticTotal = 0;
    /// Sorted by id, at most kMaxListedMods each, strings capped at kMaxModStringBytes.
    std::vector<ModEntry> gameplay;
    std::vector<ModEntry> cosmetic;
};

/// Per-list cap on what is put on the wire. Past it the list is truncated (sorted by id, so the
/// same mods are dropped on both ends), the true total still travels, and the hash still covers
/// everything — so a difference beyond the cap is still refused, just described less precisely.
inline constexpr std::size_t kMaxListedMods = 32;
/// Cap on each id / version string on the wire. Longer ones are cut (the hash uses the full one).
inline constexpr std::size_t kMaxModStringBytes = 64;

/// This machine's active mods, classified, plus anything the test knobs inject. Cheap enough to
/// call once per Hello; it lists bundle file names but reads no file contents.
ModFingerprint local_mod_fingerprint();

/// Wire form, appended to Hello and HelloAck:
/// [u32 gameplayHash][u16 gameplayTotal][u8 n]{string id, string version}*n
/// [u16 cosmeticTotal][u8 m]{string id, string version}*m
void write_mod_fingerprint(Writer& w, const ModFingerprint& fp);
/// False on a malformed or truncated block, or one whose listed count exceeds kMaxListedMods.
bool read_mod_fingerprint(Reader& r, ModFingerprint& out);

/// The comparison, from "ours" vs "theirs". Each line is one human-readable difference.
struct ModDiff {
    /// True if the gameplay sets differ. This is the only thing that refuses a join.
    bool gameplayMismatch = false;
    /// e.g. "The host has Foo 1.2, you have 1.1" — phrased with the two labels.
    std::vector<std::string> gameplayLines;
    std::vector<std::string> cosmeticLines;
};

/// @p ourLabel is a plural-verb pronoun for this end ("you", "we"); @p theirLabel a singular name
/// for the other end ("the host", or the guest's nickname). A mod counts as a gameplay difference
/// when EITHER side classifies it as gameplay.
ModDiff diff_mod_fingerprints(const ModFingerprint& ours, const ModFingerprint& theirs,
    const std::string& ourLabel, const std::string& theirLabel);

/// One line for the log: "2 gameplay [a 1.0, b 2.1], 1 cosmetic [c 1.0], hash 0x1234ABCD".
std::string describe_mod_fingerprint(const ModFingerprint& fp);

/// Toast-sized summary of a gameplay mismatch: the first few lines, then "and N more".
std::string summarize_mod_mismatch(const ModDiff& diff);

}  // namespace dusk::mp
