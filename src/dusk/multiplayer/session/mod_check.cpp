#include "mod_check.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <map>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "../net/serializer.hpp"
#include "dusk/logging.h"
#include "dusk/mod_loader.hpp"
#include "dusk/mods/loader/loader.hpp"
#include "dusk/mods/svc/save.hpp"
#include "fmt/format.h"

namespace dusk::mp {
namespace {

constexpr borealis::Log Log{"dusk::mp"};

/// How many difference lines a toast shows before "and N more". The log always gets all of them.
constexpr std::size_t kToastLines = 3;

/// A save blob whose current-slot value must match on both ends (see SessionValue).
struct SessionBlobDecl {
    std::string blob;
    std::string label;
};

/**
 * Built-in declarations for mods whose mod.json predates the "multiplayer" key. The randomizer
 * keeps the active seed's hash in its "seed_hash" blob (session.cpp: written on new save and on
 * every save write, read back on load to re-activate the seed), and that hash names the seed
 * directory whose seed.dat holds the settings too — so one string covers seed and settings.
 */
struct BuiltinDecl {
    std::string_view modId;
    std::string_view blob;
    std::string_view label;
};
constexpr BuiltinDecl kBuiltinSessionBlobs[] = {
    {"dev.twilitrealm.randomizer", "seed_hash", "Randomizer seed"},
};

/// What the bundle says about a mod, which only changes when the bundle does. Cached because the
/// fingerprint is rebuilt about once a second in a session (NetworkManager::check_mod_changes)
/// and getFileNames() can walk a directory on disk.
struct BundleFacts {
    std::uint32_t generation = 0;
    std::string version;
    bool gameplayFiles = false;
    std::vector<SessionBlobDecl> sessionBlobs;
};
std::map<std::string, BundleFacts> s_bundleFacts;

bool scan_gameplay_files(mods::LoadedMod& mod) {
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

std::vector<SessionBlobDecl> read_session_blobs(mods::LoadedMod& mod) {
    std::vector<SessionBlobDecl> out;
    if (mod.bundle != nullptr) {
        try {
            const auto bytes = mod.bundle->readFile("mod.json");
            const auto j = nlohmann::json::parse(bytes.begin(), bytes.end());
            const auto mp = j.find("multiplayer");
            if (mp != j.end() && mp->is_object()) {
                const auto blobs = mp->find("session_blobs");
                if (blobs != mp->end() && blobs->is_array()) {
                    for (const auto& entry : *blobs) {
                        if (!entry.is_object()) {
                            continue;
                        }
                        SessionBlobDecl decl;
                        decl.blob = entry.value("blob", "");
                        const std::string label = entry.value("label", decl.blob);
                        const std::string& name =
                            mod.metadata.name.empty() ? mod.metadata.id : mod.metadata.name;
                        decl.label = fmt::format("{} {}", name, label);
                        if (!decl.blob.empty()) {
                            out.push_back(std::move(decl));
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            Log.warn("Mod check: could not read the multiplayer block of {}'s mod.json: {}",
                mod.metadata.id, e.what());
        }
    }
    if (out.empty()) {
        for (const auto& builtin : kBuiltinSessionBlobs) {
            if (builtin.modId == mod.metadata.id) {
                out.push_back({std::string(builtin.blob), std::string(builtin.label)});
            }
        }
    }
    return out;
}

const BundleFacts& bundle_facts(mods::LoadedMod& mod) {
    auto& facts = s_bundleFacts[mod.metadata.id];
    if (facts.generation != mod.cacheGeneration + 1 || facts.version != mod.metadata.version) {
        // +1 so a default-constructed entry never matches generation 0.
        facts.generation = mod.cacheGeneration + 1;
        facts.version = mod.metadata.version;
        facts.gameplayFiles = scan_gameplay_files(mod);
        facts.sessionBlobs = read_session_blobs(mod);
    }
    return facts;
}

bool is_gameplay_mod(mods::LoadedMod& mod) {
    return mod.native != nullptr || mod.runtime.has_value() || bundle_facts(mod).gameplayFiles;
}

std::uint32_t fnv1a(std::uint32_t h, std::string_view bytes);

/// Short printable ASCII travels as itself (a randomizer hash is shown to the player as text);
/// anything else as a stable digest, identical on both ends for identical bytes.
std::string session_value_text(const std::vector<std::uint8_t>& bytes) {
    const bool printable = bytes.size() <= kMaxModStringBytes &&
                           std::all_of(bytes.begin(), bytes.end(),
                               [](std::uint8_t c) { return c >= 0x20 && c < 0x7F; });
    if (printable) {
        return std::string(bytes.begin(), bytes.end());
    }
    const std::string_view view{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    return fmt::format("#{:08X}", fnv1a(2166136261u, view));
}

/**
 * ★ Test knob: DUSK_MP_TEST_FAKE_SESSION = "modId|label|value[|always][,...]".
 *
 * Adds a session value, or overrides a real one with the same modId and label, so a seed
 * mismatch can be forced between two instances without generating two randomizer seeds. Like a
 * real one it is known only while a save slot is active, so it exercises the mid-session
 * ModsUpdate path; "|always" makes it known from boot, for the refusal at Hello. Only the
 * handshake's view changes; no mod sees it. Logged loudly when set.
 */
bool s_loggedFakeSession = false;

void inject_fake_session(std::vector<SessionValue>& out) {
    const char* env = std::getenv("DUSK_MP_TEST_FAKE_SESSION");
    if (env == nullptr || env[0] == '\0') {
        return;
    }
    std::string_view rest{env};
    while (!rest.empty()) {
        const auto comma = rest.find(',');
        const std::string_view item = rest.substr(0, comma);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        const auto bar1 = item.find('|');
        const auto bar2 = bar1 == std::string_view::npos ? bar1 : item.find('|', bar1 + 1);
        if (bar2 == std::string_view::npos) {
            continue;
        }
        SessionValue v;
        v.modId = std::string(item.substr(0, bar1));
        v.label = std::string(item.substr(bar1 + 1, bar2 - bar1 - 1));
        std::string_view value = item.substr(bar2 + 1);
        bool always = false;
        if (value.ends_with("|always")) {
            value.remove_suffix(std::string_view{"|always"}.size());
            always = true;
        }
        v.value = std::string(value);
        // An empty mod id names no mod, so this answers only "is a save slot active".
        std::vector<std::uint8_t> unused;
        v.known = always || mods::svc::peek_current_slot_blob({}, "x", unused);
        if (v.modId.empty() || v.label.empty()) {
            continue;
        }
        // Once per process: the fingerprint is rebuilt every second in a session.
        if (!s_loggedFakeSession) {
            Log.warn(
                "TEST: DUSK_MP_TEST_FAKE_SESSION sets {} '{}' = '{}'", v.modId, v.label, v.value);
        }
        std::erase_if(
            out, [&](const SessionValue& s) { return s.modId == v.modId && s.label == v.label; });
        out.push_back(std::move(v));
    }
    s_loggedFakeSession = true;
}

/**
 * ★ Test knobs: DUSK_MP_TEST_FAKE_MOD / DUSK_MP_TEST_FAKE_COSMETIC_MOD = "id:version[,id:version]".
 *
 * Injects mods that do not exist into this machine's list, so a mismatch can be forced on one of
 * two otherwise identical instances without building and installing a real mod. Only the list sent
 * in the handshake changes; nothing is loaded. Never set in normal play; logged loudly when set.
 */
void inject_fake_mods(const char* envName, std::vector<ModEntry>& out, bool& logged) {
    const char* env = std::getenv(envName);
    if (env == nullptr || env[0] == '\0') {
        return;
    }
    // Once per process: the fingerprint is rebuilt every second in a session.
    const bool log = !logged;
    logged = true;
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
        if (log) {
            Log.warn("TEST: {} injects fake mod '{}' v{}", envName, entry.id, entry.version);
        }
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

    std::vector<SessionValue> session;
    std::vector<std::uint8_t> blob;

    for (auto& mod : mods::ModLoader::instance().active_mods()) {
        ModEntry entry{mod.metadata.id, mod.metadata.version};
        (is_gameplay_mod(mod) ? gameplay : cosmetic).push_back(std::move(entry));
        for (const auto& decl : bundle_facts(mod).sessionBlobs) {
            SessionValue v;
            v.modId = clamp_string(mod.metadata.id);
            v.label = clamp_string(decl.label);
            v.known = mods::svc::peek_current_slot_blob(mod.metadata.id, decl.blob, blob);
            if (v.known) {
                v.value = session_value_text(blob);
            }
            session.push_back(std::move(v));
        }
    }
    static bool s_loggedFakeMod = false;
    static bool s_loggedFakeCosmetic = false;
    inject_fake_mods("DUSK_MP_TEST_FAKE_MOD", gameplay, s_loggedFakeMod);
    inject_fake_mods("DUSK_MP_TEST_FAKE_COSMETIC_MOD", cosmetic, s_loggedFakeCosmetic);
    inject_fake_session(session);
    std::sort(session.begin(), session.end(), [](const SessionValue& a, const SessionValue& b) {
        return a.modId != b.modId ? a.modId < b.modId : a.label < b.label;
    });
    if (session.size() > kMaxListedMods) {
        session.resize(kMaxListedMods);
    }
    fp.session = std::move(session);

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
    const std::size_t k = std::min(fp.session.size(), kMaxListedMods);
    w.write_u8(static_cast<std::uint8_t>(k));
    for (std::size_t i = 0; i < k; ++i) {
        const auto& v = fp.session[i];
        w.write_string(clamp_string(v.modId));
        w.write_string(clamp_string(v.label));
        w.write_u8(v.known ? 1 : 0);
        w.write_string(clamp_string(v.value));
    }
}

bool read_mod_fingerprint(Reader& r, ModFingerprint& out) {
    if (!r.read_u32(out.gameplayHash) || !read_list(r, out.gameplay, out.gameplayTotal) ||
        !read_list(r, out.cosmetic, out.cosmeticTotal))
    {
        return false;
    }
    std::uint8_t k = 0;
    if (!r.read_u8(k) || k > kMaxListedMods) {
        return false;
    }
    out.session.clear();
    out.session.reserve(k);
    for (std::uint8_t i = 0; i < k; ++i) {
        SessionValue v;
        std::uint8_t known = 0;
        if (!r.read_string(v.modId) || !r.read_string(v.label) || !r.read_u8(known) ||
            !r.read_string(v.value) || v.modId.size() > kMaxModStringBytes ||
            v.label.size() > kMaxModStringBytes || v.value.size() > kMaxModStringBytes)
        {
            return false;
        }
        v.known = known != 0;
        out.session.push_back(std::move(v));
    }
    return r.ok();
}

bool same_mod_fingerprint(const ModFingerprint& a, const ModFingerprint& b) {
    Writer wa;
    Writer wb;
    write_mod_fingerprint(wa, a);
    write_mod_fingerprint(wb, b);
    return wa.data() == wb.data();
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

    // Session values, after the mod lines (a missing mod is the more basic difference). Only where
    // BOTH ends have a save loaded: before that there is no seed to disagree about, and refusing
    // would kick every guest the moment the host reached its file select first. An entry only one
    // side lists belongs to a mod the other lacks, which the lines above already name.
    bool sessionDiffers = false;
    for (const auto& o : theirs.session) {
        for (const auto& m : ours.session) {
            if (m.modId != o.modId || m.label != o.label || !m.known || !o.known ||
                m.value == o.value)
            {
                continue;
            }
            sessionDiffers = true;
            diff.gameplayLines.push_back(capitalize(fmt::format("{} differs: {} has {}, {} have {}",
                o.label, theirLabel, o.value.empty() ? "none" : o.value, ourLabel,
                m.value.empty() ? "none" : m.value)));
        }
    }

    diff.gameplayMismatch =
        !diff.gameplayLines.empty() || hashDiffers || totalsDiffer || sessionDiffers;
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
    std::string session;
    for (const auto& v : fp.session) {
        if (!session.empty()) {
            session += ", ";
        }
        session += v.known ? fmt::format("{} '{}'", v.label, v.value) :
                             fmt::format("{} (no save loaded)", v.label);
    }
    return fmt::format("{} gameplay [{}{}], {} cosmetic [{}{}], hash 0x{:08X}, session [{}]",
        fp.gameplayTotal, join_entries(fp.gameplay),
        static_cast<std::size_t>(fp.gameplayTotal) > fp.gameplay.size() ? ", ..." : "",
        fp.cosmeticTotal, join_entries(fp.cosmetic),
        static_cast<std::size_t>(fp.cosmeticTotal) > fp.cosmetic.size() ? ", ..." : "",
        fp.gameplayHash, session);
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
