#include "mod_check.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <string_view>

#include "../net/serializer.hpp"
#include "dusk/logging.h"
#include "dusk/mod_loader.hpp"
#include "dusk/mods/loader/loader.hpp"
#include "fmt/format.h"

namespace dusk::mp {
namespace {

constexpr borealis::Log Log{"dusk::mp"};

/// How many difference lines a toast shows before "and N more". The log always gets all of them.
constexpr std::size_t kToastLines = 3;

bool has_gameplay_files(mods::LoadedMod& mod) {
    if (mod.bundle == nullptr) {
        return false;
    }
    for (const auto& file : mod.bundle->getFileNames()) {
        // lib/: native code, whether or not THIS machine could load it (see the header).
        // overlay/: disc file replacement, which can be a stage, a message table, an actor.
        if (file.starts_with("lib/") || file.starts_with("overlay/")) {
            return true;
        }
    }
    return false;
}

bool is_gameplay_mod(mods::LoadedMod& mod) {
    return mod.native != nullptr || mod.runtime.has_value() || has_gameplay_files(mod);
}

/**
 * ★ Test knobs: DUSK_MP_TEST_FAKE_MOD / DUSK_MP_TEST_FAKE_COSMETIC_MOD = "id:version[,id:version]".
 *
 * Injects mods that do not exist into this machine's list, so a mismatch can be forced on one of
 * two otherwise identical instances without building and installing a real mod. Only the list sent
 * in the handshake changes; nothing is loaded. Never set in normal play; logged loudly when set.
 */
void inject_fake_mods(const char* envName, std::vector<ModEntry>& out) {
    const char* env = std::getenv(envName);
    if (env == nullptr || env[0] == '\0') {
        return;
    }
    std::string_view rest{env};
    while (!rest.empty()) {
        const auto comma = rest.find(',');
        const std::string_view item = rest.substr(0, comma);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        if (item.empty()) {
            continue;
        }
        const auto colon = item.find(':');
        ModEntry entry;
        entry.id = std::string(item.substr(0, colon));
        entry.version = colon == std::string_view::npos ? std::string("0") :
                                                          std::string(item.substr(colon + 1));
        if (entry.id.empty()) {
            continue;
        }
        Log.warn("TEST: {} injects fake mod '{}' v{}", envName, entry.id, entry.version);
        out.push_back(std::move(entry));
    }
}

void sort_and_dedupe(std::vector<ModEntry>& list) {
    std::sort(list.begin(), list.end(), [](const ModEntry& a, const ModEntry& b) {
        return a.id != b.id ? a.id < b.id : a.version < b.version;
    });
    list.erase(std::unique(list.begin(), list.end(),
                   [](const ModEntry& a, const ModEntry& b) { return a.id == b.id; }),
        list.end());
}

std::uint32_t fnv1a(std::uint32_t h, std::string_view bytes) {
    for (const char c : bytes) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 16777619u;
    }
    // A NUL terminator byte, so ("ab","c") and ("a","bc") hash differently.
    h *= 16777619u;
    return h;
}

std::string clamp_string(const std::string& s) {
    return s.size() > kMaxModStringBytes ? s.substr(0, kMaxModStringBytes) : s;
}

void write_list(Writer& w, const std::vector<ModEntry>& list, std::uint16_t total) {
    w.write_u16(total);
    const std::size_t n = std::min(list.size(), kMaxListedMods);
    w.write_u8(static_cast<std::uint8_t>(n));
    for (std::size_t i = 0; i < n; ++i) {
        w.write_string(clamp_string(list[i].id));
        w.write_string(clamp_string(list[i].version));
    }
}

bool read_list(Reader& r, std::vector<ModEntry>& list, std::uint16_t& total) {
    std::uint8_t n = 0;
    if (!r.read_u16(total) || !r.read_u8(n) || n > kMaxListedMods) {
        return false;
    }
    list.clear();
    list.reserve(n);
    for (std::uint8_t i = 0; i < n; ++i) {
        ModEntry entry;
        if (!r.read_string(entry.id) || !r.read_string(entry.version) ||
            entry.id.size() > kMaxModStringBytes || entry.version.size() > kMaxModStringBytes)
        {
            return false;
        }
        list.push_back(std::move(entry));
    }
    return r.ok();
}

std::string capitalize(std::string s) {
    if (!s.empty()) {
        s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    }
    return s;
}

struct Side {
    std::string version;
    bool gameplay = false;
};

std::map<std::string, Side> index_entries(const ModFingerprint& fp) {
    std::map<std::string, Side> out;
    for (const auto& m : fp.cosmetic) {
        out[m.id] = Side{m.version, false};
    }
    // Gameplay wins if a broken peer lists one id in both.
    for (const auto& m : fp.gameplay) {
        out[m.id] = Side{m.version, true};
    }
    return out;
}

std::string join_entries(const std::vector<ModEntry>& list) {
    std::string out;
    for (const auto& m : list) {
        if (!out.empty()) {
            out += ", ";
        }
        out += fmt::format("{} {}", m.id, m.version);
    }
    return out;
}

}  // namespace

ModFingerprint local_mod_fingerprint() {
    ModFingerprint fp;
    std::vector<ModEntry> gameplay;
    std::vector<ModEntry> cosmetic;

    for (auto& mod : mods::ModLoader::instance().active_mods()) {
        ModEntry entry{mod.metadata.id, mod.metadata.version};
        (is_gameplay_mod(mod) ? gameplay : cosmetic).push_back(std::move(entry));
    }
    inject_fake_mods("DUSK_MP_TEST_FAKE_MOD", gameplay);
    inject_fake_mods("DUSK_MP_TEST_FAKE_COSMETIC_MOD", cosmetic);

    sort_and_dedupe(gameplay);
    sort_and_dedupe(cosmetic);

    // Over every gameplay mod, full strings, before any truncation.
    std::uint32_t h = 2166136261u;
    for (const auto& m : gameplay) {
        h = fnv1a(h, m.id);
        h = fnv1a(h, m.version);
    }
    fp.gameplayHash = gameplay.empty() ? 0u : h;
    fp.gameplayTotal = static_cast<std::uint16_t>(std::min<std::size_t>(gameplay.size(), 0xFFFF));
    fp.cosmeticTotal = static_cast<std::uint16_t>(std::min<std::size_t>(cosmetic.size(), 0xFFFF));

    if (gameplay.size() > kMaxListedMods) {
        gameplay.resize(kMaxListedMods);
    }
    if (cosmetic.size() > kMaxListedMods) {
        cosmetic.resize(kMaxListedMods);
    }
    for (auto& m : gameplay) {
        m.id = clamp_string(m.id);
        m.version = clamp_string(m.version);
    }
    for (auto& m : cosmetic) {
        m.id = clamp_string(m.id);
        m.version = clamp_string(m.version);
    }
    fp.gameplay = std::move(gameplay);
    fp.cosmetic = std::move(cosmetic);
    return fp;
}

void write_mod_fingerprint(Writer& w, const ModFingerprint& fp) {
    w.write_u32(fp.gameplayHash);
    write_list(w, fp.gameplay, fp.gameplayTotal);
    write_list(w, fp.cosmetic, fp.cosmeticTotal);
}

bool read_mod_fingerprint(Reader& r, ModFingerprint& out) {
    return r.read_u32(out.gameplayHash) && read_list(r, out.gameplay, out.gameplayTotal) &&
           read_list(r, out.cosmetic, out.cosmeticTotal) && r.ok();
}

ModDiff diff_mod_fingerprints(const ModFingerprint& ours, const ModFingerprint& theirs,
    const std::string& ourLabel, const std::string& theirLabel) {
    ModDiff diff;
    const auto mine = index_entries(ours);
    const auto other = index_entries(theirs);

    auto add = [&](bool gameplay, std::string line) {
        (gameplay ? diff.gameplayLines : diff.cosmeticLines).push_back(capitalize(std::move(line)));
    };

    for (const auto& [id, o] : other) {
        const auto it = mine.find(id);
        if (it == mine.end()) {
            add(o.gameplay,
                fmt::format("{} has {} {}, {} don't", theirLabel, id, o.version, ourLabel));
        } else if (it->second.version != o.version) {
            add(o.gameplay || it->second.gameplay,
                fmt::format("{} has {} {}, {} have {}", theirLabel, id, o.version, ourLabel,
                    it->second.version));
        }
    }
    for (const auto& [id, m] : mine) {
        if (other.find(id) == other.end()) {
            add(m.gameplay,
                fmt::format("{} have {} {}, {} doesn't", ourLabel, id, m.version, theirLabel));
        }
    }

    // The hash covers mods past the listing cap; the lists cover a (vanishingly unlikely) hash
    // collision. Either one differing is a mismatch.
    const bool totalsDiffer = ours.gameplayTotal != theirs.gameplayTotal;
    const bool hashDiffers = ours.gameplayHash != theirs.gameplayHash;
    diff.gameplayMismatch = !diff.gameplayLines.empty() || hashDiffers || totalsDiffer;
    if (diff.gameplayMismatch && diff.gameplayLines.empty()) {
        diff.gameplayLines.push_back(capitalize(
            fmt::format("{} gameplay mods on {} end and {} on {}'s, and they differ past the first "
                        "{} that can be compared by name",
                ours.gameplayTotal, ourLabel == "you" ? "your" : "our", theirs.gameplayTotal,
                theirLabel, kMaxListedMods)));
    }
    return diff;
}

std::string describe_mod_fingerprint(const ModFingerprint& fp) {
    return fmt::format("{} gameplay [{}{}], {} cosmetic [{}{}], hash 0x{:08X}", fp.gameplayTotal,
        join_entries(fp.gameplay),
        static_cast<std::size_t>(fp.gameplayTotal) > fp.gameplay.size() ? ", ..." : "",
        fp.cosmeticTotal, join_entries(fp.cosmetic),
        static_cast<std::size_t>(fp.cosmeticTotal) > fp.cosmetic.size() ? ", ..." : "",
        fp.gameplayHash);
}

std::string summarize_mod_mismatch(const ModDiff& diff) {
    std::string out;
    const std::size_t shown = std::min(diff.gameplayLines.size(), kToastLines);
    for (std::size_t i = 0; i < shown; ++i) {
        out += diff.gameplayLines[i];
        out += i + 1 < shown ? ". " : ".";
    }
    if (diff.gameplayLines.size() > shown) {
        out += fmt::format(" And {} more.", diff.gameplayLines.size() - shown);
    }
    return out;
}

}  // namespace dusk::mp
