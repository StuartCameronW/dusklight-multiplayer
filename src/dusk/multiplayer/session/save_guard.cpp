#include "save_guard.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include <aurora/card.h>
#include <borealis/io.hpp>

#include "dusk/data.hpp"
#include "dusk/logging.h"
#include "dusk/main.h"
#include "dusk/mods/svc/file.hpp"
#include "dusk/save_manager.hpp"
#include "dusk/ui/ui.hpp"
#include "dusk/version.hpp"
#include "m_Do/m_Do_MemCard.h"

namespace dusk::mp {
namespace {

constexpr borealis::Log Log{"dusk::mp"};

/**
 * ★ Test knob: DUSK_MP_TEST_SAVE_REFUSAL=1 exercises the refusal path without a session.
 *
 * The dangerous half of this feature is not "does the write get dropped" — that is one branch and
 * an md5 proves it. It is whether a DROPPED write leaves a caller waiting forever for a card
 * command that no longer exists (see consume_refused_card_write in the header). That path lives
 * behind the in-game save prompt, which a scripted run cannot reach, so without this knob the
 * softlock question could only ever be reasoned about rather than run.
 *
 * It forces only refuse_card_write(), NOT save_would_be_contaminated() — so on an instance that has
 * not joined a session (the host, or a single instance) the autosave predicate still passes and the
 * write is then refused at the choke point. That is exactly the sequence the save prompt would
 * produce, driven by a state machine that also polls SaveSync (autosave.cpp:112). Autosave fires on
 * every stage entry and every shutter door, so a run under this knob refuses many writes and would
 * wedge visibly if the completion latch were wrong. Setting it on a guest proves nothing: there the
 * predicate short-circuits first, which is the whole point of the predicate.
 *
 * Read once. Never set in normal play; a run with it on writes no save at all.
 */
bool test_refusal_forced() {
    static const bool forced = [] {
        const char* value = std::getenv("DUSK_MP_TEST_SAVE_REFUSAL");
        return value != nullptr && value[0] == '1';
    }();
    return forced;
}

/**
 * Sticky for the life of the process. See the header for why this is deliberately never cleared on
 * disconnect: the contamination is in g_dComIfG_gameInfo, and that outlives the socket.
 */
bool sContaminated = false;

/**
 * True while sContaminated comes ONLY from a join attempt the host has not yet accepted. Nothing of
 * the host's can have been applied then: every host packet is dropped until the handshake completes
 * (network_manager.cpp). So if the attempt fails, the world in memory is still this machine's own
 * and saving can safely come back. Any other cause, or an accepted handshake, makes it sticky.
 */
bool sGuestJoinUnconfirmed = false;

/**
 * Rate limit for the player-facing toast. A save that is refused tends to be retried — the in-game
 * save prompt is a menu the player can mash — and a toast per attempt would bury the message it is
 * trying to deliver.
 */
constexpr std::chrono::seconds kToastCooldown{10};

std::chrono::steady_clock::time_point sLastToast{};
bool sToastedOnce = false;

/// Set by refuse_card_write, consumed by SaveSync so the caller's wait terminates. See the header.
bool sRefusedWritePending = false;

/// Set by refuse_card_format, consumed by FormatSync. Same reason as sRefusedWritePending.
bool sRefusedFormatPending = false;

/// One notification per process when the session ends, not one per reconnect attempt.
bool sNotifiedSessionEnded = false;

/**
 * True when every save write must be refused, for either reason: this process joined a co-op
 * session as a guest (sContaminated), or another Dusklight holds this data directory and this one
 * could not be moved (dusk/data.cpp, the P0 lock). Both are sticky for the life of the process.
 */
bool saving_refused() {
    return sContaminated || dusk::data::is_data_directory_shared();
}

/// Only the shared directory is the cause; the guest wording wins when both apply.
bool refused_for_shared_directory() {
    return !sContaminated && dusk::data::is_data_directory_shared();
}

/// Why, for the log. The guest wording is the original one and is kept verbatim for the harness.
const char* refusal_cause_for_log() {
    return refused_for_shared_directory() ?
               "another Dusklight holds this data directory, so this process may not write saves "
               "there" :
               "this process joined a co-op session as a guest";
}

/**
 * The one player-facing toast for every refused write, card or file. Shared rate limit, so a Save
 * Manager click straight after a refused autosave does not stack two identical notices.
 */
void toast_refusal() {
    const auto now = std::chrono::steady_clock::now();
    if (sToastedOnce && now - sLastToast < kToastCooldown) {
        return;
    }
    sToastedOnce = true;
    sLastToast = now;
    dusk::ui::push_toast({
        .title = "Saving is disabled",
        .content = refused_for_shared_directory() ?
                       "Another copy of Dusklight is using this save folder and owns its saves. "
                       "Close it and restart this one to save." :
                       "The host owns this world's save. Your own file is untouched.",
        .duration = std::chrono::seconds(6),
    });
}

/// Generic separators, and lower case everywhere: Windows and macOS filesystems are
/// case-insensitive by default, and over-matching "card" on Linux costs nothing.
std::string comparable(const std::filesystem::path& i_path) {
    std::string text = borealis::io::fs_path_to_generic_string(i_path);
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::filesystem::path absolute_normal(const std::filesystem::path& i_path) {
    std::error_code ec;
    auto result = std::filesystem::absolute(i_path, ec);
    if (ec) {
        result = i_path;
    }
    return result.lexically_normal();
}

}  // namespace

bool save_would_be_contaminated() {
    return sContaminated;
}

bool saving_disabled() {
    return saving_refused();
}

const char* saving_disabled_explanation() {
    if (sContaminated) {
        return "This run joined a co-op session as a guest, so saves can be viewed and exported "
               "but not changed. Restart Dusklight to manage them.";
    }
    if (dusk::data::is_data_directory_shared()) {
        return "Another copy of Dusklight is using this save folder, so this one can view and "
               "export saves but not change them. Close the other copy and restart this one to "
               "manage them.";
    }
    return nullptr;
}

void mark_save_contaminated(const char* i_reason) {
    if (sContaminated) {
        return;
    }

    sContaminated = true;
    sGuestJoinUnconfirmed = false;
    Log.warn("Save ownership: this machine will not write its own memory card again this run — {}. "
             "The host owns the save for a co-op session; a guest's own .gci is left untouched "
             "rather than overwritten with the host's world. Restart Dusklight to save normally.",
        i_reason != nullptr ? i_reason : "reason not given");
}

void begin_guest_join() {
    if (sContaminated) {
        return;
    }
    sContaminated = true;
    sGuestJoinUnconfirmed = true;
    Log.info("Save ownership: saving is off while joining a co-op session. It stays off for this "
             "run once the host accepts us, and comes back if the join fails.");
}

void confirm_guest_join() {
    if (!sGuestJoinUnconfirmed) {
        return;
    }
    sGuestJoinUnconfirmed = false;
    Log.warn("Save ownership: this machine will not write its own memory card again this run — "
             "joined a co-op session as a guest. The host owns the save for a co-op session; a "
             "guest's own .gci is left untouched rather than overwritten with the host's world. "
             "Restart Dusklight to save normally.");
}

void abandon_guest_join() {
    if (!sGuestJoinUnconfirmed) {
        return;
    }
    sGuestJoinUnconfirmed = false;
    sContaminated = false;
    Log.info(
        "Save ownership: the join never completed, so nothing of the host's reached this game. "
        "Saving is back on.");
    dusk::ui::push_toast({
        .title = "Saving is back on",
        .content = "You never joined the session, so your own game and save are as you left them.",
        .duration = std::chrono::seconds(5),
    });
}

bool refuse_card_write() {
    if (!saving_refused() && !test_refusal_forced()) {
        return false;
    }

    // Whoever asked is waiting on SaveSync; make sure that wait can end.
    sRefusedWritePending = true;

    if (!sToastedOnce || std::chrono::steady_clock::now() - sLastToast >= kToastCooldown) {
        if (refused_for_shared_directory()) {
            Log.warn("Refused a memory card write: another Dusklight holds this data directory "
                     "('{}'), and two writers would tear the card. Nothing was written.",
                borealis::io::fs_path_to_string(dusk::data::shared_data_directory_path()));
        } else {
            Log.warn("Refused a memory card write: this process joined a co-op session, so its "
                     "game state belongs to the host. Nothing was written.");
        }
    }
    toast_refusal();
    return true;
}

bool refuse_card_format() {
    if (!saving_refused()) {
        return false;
    }

    // Both callers poll FormatSync every frame; see consume_refused_card_format.
    sRefusedFormatPending = true;
    Log.warn(
        "Refused a memory card FORMAT: {}. The card was not touched.", refusal_cause_for_log());
    toast_refusal();
    return true;
}

bool consume_refused_card_format() {
    if (!sRefusedFormatPending) {
        return false;
    }

    sRefusedFormatPending = false;
    return true;
}

bool refuse_save_file_write(const char* i_what) {
    if (!saving_refused()) {
        return false;
    }

    Log.warn("Refused a save file operation ({}): {}, so nothing on this machine's card or save "
             "folders may change until a restart.",
        i_what != nullptr ? i_what : "unnamed", refusal_cause_for_log());
    toast_refusal();
    return true;
}

bool is_save_storage_path(
    const std::filesystem::path& i_path, const std::filesystem::path& i_dataRoot) {
    if (i_path.empty() || i_dataRoot.empty()) {
        return false;
    }

    const std::string path = comparable(absolute_normal(i_path));
    std::string root = comparable(absolute_normal(i_dataRoot));
    if (!root.empty() && root.back() != '/') {
        root += '/';
    }
    if (!std::string_view{path}.starts_with(root)) {
        return false;
    }

    // The first two components below the root are all the layout needs.
    const std::string_view rest = std::string_view{path}.substr(root.size());
    const auto slash = rest.find('/');
    const std::string_view first = rest.substr(0, slash);
    std::string_view second;
    if (slash != std::string_view::npos) {
        second = rest.substr(slash + 1);
        second = second.substr(0, second.find('/'));
    }

    // <root>/MemoryCardA.EUR.raw, its temporaries, and <root>/MemoryCardA.EUR.raw.mods/...
    // (extern/aurora/lib/dolphin/card.cpp get_card_full_path).
    if (first.starts_with("memorycard")) {
        return true;
    }
    // Startup backups (src/dusk/data.cpp), raw-mode Save Manager backups
    // (save_manager.cpp backup_directory), and the legacy mod sidecar (mods/svc/save.cpp).
    if (first == "save-backups" || first == "backups" || first.starts_with("mod_saves.json")) {
        return true;
    }
    // <root>/<region>/Card A/... — the GCI folder, its .mods sidecars and its backups/. Exact
    // names, so a mod's own <root>/mods/cards/ is not caught.
    return second == "card a" || second == "card b";
}

bool is_save_storage_location(std::string_view i_location) {
    if (i_location.empty()) {
        return false;
    }

    // Locations are opaque (a content:// URI on Android, a security-scoped grant on Apple). One
    // that does not resolve to a filesystem path cannot be the app's own card folder, which always
    // is one, so it is let through.
    const auto access = borealis::io::access_path(i_location);
    return access && is_save_storage_path(access.path(), dusk::ConfigPath);
}

bool refuse_save_location_write(std::string_view i_location, const char* i_what) {
    if (!saving_refused() || !is_save_storage_location(i_location)) {
        return false;
    }
    return refuse_save_file_write(i_what);
}

void notify_save_disabled_after_session_end(SessionEnd i_how) {
    if (!sContaminated || sNotifiedSessionEnded) {
        return;
    }

    sNotifiedSessionEnded = true;
    dusk::ui::push_toast({
        .title = "Saving is still disabled",
        .content = "This world belongs to the host. Restart Dusklight to play your own save.",
        .duration = std::chrono::seconds(8),
    });
    Log.warn("{} Saving stays disabled for this process: the in-memory game state is still the "
             "host's world, so writing it to this machine's card would overwrite the player's own "
             "progress. A restart loads their save from card and clears the flag.",
        i_how == SessionEnd::LeftByChoice ? "You left the session." : "The host is gone.");
}

bool consume_refused_card_write() {
    if (!sRefusedWritePending) {
        return false;
    }

    sRefusedWritePending = false;
    return true;
}

namespace {

std::uint64_t probe_frame() {
    static const std::uint64_t frame = [] {
        const char* value = std::getenv("DUSK_MP_TEST_SAVE_GUARD_PROBE");
        return value != nullptr ? std::strtoull(value, nullptr, 10) : 0ull;
    }();
    return frame;
}

void log_probe(const char* i_path, const save_manager::Result& i_result) {
    Log.info("SAVE-GUARD PROBE: {} -> {} ({})", i_path, i_result.ok ? "ALLOWED" : "REFUSED",
        i_result.message);
}

void run_save_guard_probe() {
    const bool guarded = saving_refused();
    Log.info("SAVE-GUARD PROBE: start, guard {}", guarded ? "ENGAGED" : "not engaged");

    const auto& diskId = dusk::version::getDiskID();
    const std::string game{diskId.gameName, sizeof(diskId.gameName)};
    const std::string maker{diskId.company, sizeof(diskId.company)};
    const char* fileName = mDoMemCd_GetFileName();
    const save_manager::SaveIdentity identity{
        .maker = maker,
        .game = game,
        .saveName = fileName != nullptr ? fileName : "",
    };
    const auto cardType = aurora_card_get_type(0);
    const auto storage = save_manager::resolve_storage(game,
        cardType == AURORA_CARD_RAW_IMAGE ? save_manager::StorageKind::RawImage :
                                            save_manager::StorageKind::GciDirectory,
        0);
    if (!storage) {
        Log.warn("SAVE-GUARD PROBE: no card storage ({}); Save Manager paths not probed",
            storage.result.message);
    }

    if (!guarded) {
        // Prove the guard does not over-block: the one non-destructive Save Manager write.
        if (storage) {
            log_probe("save_manager::create_backup",
                save_manager::create_backup(storage.value, identity));
        }
        // The stale handle must still work here: the guard does not over-block.
        dusk::mods::svc::file_probe_use_stale_save_handle();
        Log.info("SAVE-GUARD PROBE: done");
        return;
    }

    // 1. Save Manager. Each of these really writes or deletes if the guard fails, which is the
    // point: the sandbox card's md5 is the second witness.
    if (storage) {
        const std::string gciLocation = borealis::io::fs_path_to_string(
            storage.value.path /
            (save_manager::card_file_stem(maker, game, identity.saveName) + ".gci"));
        if (auto artifact = save_manager::read_artifact(gciLocation)) {
            log_probe("save_manager::import_artifact",
                save_manager::import_artifact(
                    storage.value, identity, artifact.value, save_manager::ModDataAction::Keep));
        } else {
            Log.info("SAVE-GUARD PROBE: no .gci to re-import ({})", artifact.result.message);
        }
        log_probe(
            "save_manager::create_backup", save_manager::create_backup(storage.value, identity));
        if (auto backups = save_manager::list_backups(storage.value, identity);
            backups && !backups.value.empty())
        {
            log_probe("save_manager::restore_backup",
                save_manager::restore_backup(storage.value, identity, backups.value.front().path));
            log_probe("save_manager::delete_backup",
                save_manager::delete_backup(storage.value, backups.value.front().path));
        } else {
            Log.info("SAVE-GUARD PROBE: no Save Manager backup to restore or delete");
        }
        log_probe("save_manager::delete_mod_data",
            save_manager::delete_mod_data(storage.value, identity, "dev.twilitrealm.probe"));
        log_probe("save_manager::delete_save", save_manager::delete_save(storage.value, identity));

        // 3. Mod file API: the predicate its entry points call, on a save path and on a mod path.
        Log.info("SAVE-GUARD PROBE: mod file write to the .gci -> {}",
            refuse_save_location_write(gciLocation, "probe: mod file write") ? "REFUSED" :
                                                                               "ALLOWED");
    }
    const std::string modLocation =
        borealis::io::fs_path_to_string(dusk::ConfigPath / "mods" / "probe-own-data.json");
    Log.info("SAVE-GUARD PROBE: mod file write to its own folder -> {}",
        refuse_save_location_write(modLocation, "probe: mod own file") ? "REFUSED" : "ALLOWED");

    // 4. A mod file API handle opened before the session started, used now that the guard is on.
    dusk::mods::svc::file_probe_use_stale_save_handle();

    // 2. The card thread: a write and a format, each followed by the poll its callers make. A
    // return of 0 here would be the softlock.
    static u8 sProbeBuffer[4] = {};
    g_mDoMemCd_control.save(sProbeBuffer, sizeof(sProbeBuffer), 0);
    Log.info("SAVE-GUARD PROBE: card save -> SaveSync returned {} (2 = refused and completed)",
        g_mDoMemCd_control.SaveSync());
    g_mDoMemCd_control.command_format();
    Log.info("SAVE-GUARD PROBE: card format -> FormatSync returned {} (2 = refused and completed)",
        g_mDoMemCd_control.FormatSync());

    Log.info("SAVE-GUARD PROBE: done");
}

}  // namespace

void prepare_save_guard_probe_if_requested() {
    static bool sPrepared = false;
    if (probe_frame() == 0 || sPrepared) {
        return;
    }
    sPrepared = true;
    dusk::mods::svc::file_probe_open_stale_save_handle();
}

void run_save_guard_probe_if_requested() {
    static std::uint64_t sCalls = 0;
    if (probe_frame() == 0 || ++sCalls != probe_frame()) {
        return;
    }
    run_save_guard_probe();
}

}  // namespace dusk::mp
