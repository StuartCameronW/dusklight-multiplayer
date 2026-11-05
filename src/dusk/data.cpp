#include "data.hpp"

#include "dusk/app_info.hpp"

#include <borealis/io.hpp>
#include <borealis/log.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace dusk::data {
namespace {

constexpr borealis::Log Log{"dusk::data"};

std::string status_message(const borealis::data::Status& status) {
    using enum borealis::data::ErrorCode;

    std::string message;
    switch (status.code) {
    case None:
        return {};
    case NotInitialized:
        message = "Data folders have not been initialized.";
        break;
    case PreferencePathUnavailable:
        message = "The system data folder is unavailable.";
        break;
    case EmptyPath:
        message = "Choose a folder.";
        break;
    case CreateDirectoryFailed:
        message = "The selected folder could not be created.";
        break;
    case NotDirectory:
        message = "The selected path is not a folder.";
        break;
    case WriteProbeFailed:
        message = "The selected folder is not writable.";
        break;
    case WriteProbeCleanupFailed:
        message = "The selected folder could not be validated.";
        break;
    case DescriptorWriteFailed:
        message = "Dusklight could not save the data folder setting.";
        break;
    case MigrationIncomplete:
        message = "Data migration could not be completed.";
        break;
    case OverrideActive:
        message = "The data folder is fixed by --user-dir for this session.";
        break;
    case Unsupported:
        message = "Changing the data folder is not supported on this platform.";
        break;
    case OpenFolderFailed:
        message = "The data folder could not be opened.";
        break;
    }

    if (!status.path.empty()) {
        message += " (" + borealis::io::fs_path_to_string(status.path) + ")";
    }
    if (status.systemError) {
        message += ": " + status.systemError.message();
    }
    return message;
}

bool operation_succeeded(const borealis::data::Status& status, std::string* errorOut = nullptr) {
    if (status) {
        return true;
    }
    const auto message = status_message(status);
    if (errorOut != nullptr) {
        *errorOut = message;
    }
    Log.warn("{}", message);
    return false;
}

/// Upstream's own ensure_data_directory() moved into borealis::data::Manager along with the rest
/// of path resolution; the lock code below still needs to create the numbered instance folders.
void ensure_data_directory(const std::filesystem::path& dataPath) {
    std::error_code ec;
    std::filesystem::create_directories(dataPath, ec);
    if (ec) {
        Log.fatal("Failed to create data directory '{}': {}",
            borealis::io::fs_path_to_string(dataPath), ec.message());
    }
}

/// Set by --data-dir. Unlike borealis's --user-dir, which moves only the user directory, this also
/// moves the cache directory (logs, dawn_cache.db) so a second instance shares NOTHING with the
/// first: a shared dawn_cache.db throttles the loser to ~5 Hz on SQLite lock contention.
std::optional<std::filesystem::path> sDataPathOverride;

/**
 * ★ THE SINGLE-INSTANCE DATA DIRECTORY LOCK (13-save-safety.md, P0).
 *
 * The problem: nothing stopped two Dusklight processes pointing at the same data directory, and a
 * single save is at least five independent open/seek/write/close cycles on one .gci with no lock,
 * no temp-file-and-rename and no fsync (13 §2). Two of those interleaving lands a torn save in both
 * the primary sector and its mirror. The accident that actually happens is mundane: double-clicking
 * the exe twice, or launching the installed build while a dev build is running.
 *
 * ★ WHAT IT DOES ON CONFLICT IS STUART'S CALL, 2026-08-13, and it is better than either option that
 * was put to him (refuse to launch / run with saving disabled): *"give it a separate folder, cant
 * you just rename it, so it doesnt overwrite my save files?"* So a second instance is **moved to a
 * numbered directory of its own**, seeded with a COPY of the first one's memory cards and config.
 * Nobody is refused a launch, nobody plays a crippled session, and the original .gci is never
 * opened by two processes. It is the same shape the two-instance test harness has used successfully
 * all along (`.claude/mp-test.ps1` seeds each instance from a copy), promoted from a script into
 * the program.
 *
 * ★ THE COST, NAMED. The second instance's progress goes somewhere else, and a player who does not
 * notice will look for it in the wrong place later. That is why is_secondary_instance() exists and
 * why the overlay raises a toast rather than this being a log line nobody reads.
 *
 * FAILURE MODES, all deliberately biased towards booting:
 *   - The lock is an OS lock on an OPEN HANDLE, never a pidfile. The OS releases it however the
 *     process dies, so a crash cannot leave the game permanently unstartable. Dusklight crashes are
 *     a first-class scenario (crash_handler.cpp exists), so the pidfile form was never an option.
 *   - "Could not create or open the lock file at all" counts as PERMITTED TO RUN. Network and
 *     cloud-synced data directories report locking unreliably in both directions, and an I/O error
 *     must never become a refusal to boot.
 *   - Only "someone else holds it" is treated as a conflict.
 *   - An explicit --data-dir / --user-dir is never redirected. The caller said exactly where to put
 *     things, and silently moving them would break the harness in the confusing direction.
 *
 * ★ WHEN THE DIRECTORY CANNOT BE MOVED, SAVING IS DISABLED INSTEAD (2026-11-04). Relocation covers
 * the default folder, but three cases leave a second process inside a directory another one holds:
 * an explicit --data-dir, an explicit --user-dir, and every numbered folder being taken. Those used
 * to log a warning and carry on writing, which is exactly the torn-.gci interleave this lock exists
 * to prevent. Now they run with saving refused for the life of the process, through the same
 * refusal machinery a co-op guest uses (multiplayer/session/save_guard.hpp — every card write,
 * format, Save Manager change and mod sidecar write already asks it, and each refusal COMPLETES
 * whatever the caller polls, so nothing softlocks). The game still boots and plays; the first
 * process keeps sole ownership of the card. Refusing the launch outright was the other option, and
 * it is worse for the one legitimate multi-instance user: a misconfigured harness run would vanish
 * instead of showing the mistake in both logs.
 */
constexpr std::string_view kDataLockFileName = ".dusklight.lock";

/// Give up after this many numbered directories rather than looping. Eight simultaneous copies of
/// Dusklight is far past any real use, and past it saving is disabled rather than shared.
constexpr int kMaxInstanceDirectories = 8;

/// A held lock is retried briefly before it counts as a conflict. Windows releases a dead process's
/// byte-range locks asynchronously ("the time it takes ... depends upon available system
/// resources"), so relaunching straight after a crash could otherwise see a lock nobody holds.
/// A live second instance holds it for its whole life, so the retry never hides a real one.
constexpr int kLockAttempts = 5;
constexpr std::chrono::milliseconds kLockRetryDelay{50};

bool sIsSecondaryInstance = false;
std::filesystem::path sSecondaryInstancePath;

/// Set when this process could not get a directory of its own; see is_data_directory_shared().
bool sDataDirectoryShared = false;
std::filesystem::path sSharedDataDirectoryPath;

/// What happened at startup, for log_data_lock_status() to repeat once file logging exists.
std::string sLockStatusLine;

/// Diagnostics written into the lock file by its holder: never part of the lock mechanism (the OS
/// lock on the open handle is), only so a refused instance can say WHICH process has the folder.
std::string lock_diagnostics() {
#if defined(_WIN32)
    const unsigned long pid = GetCurrentProcessId();
#else
    const long pid = static_cast<long>(::getpid());
#endif
    const auto started = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch())
                             .count();
    return "pid " + std::to_string(pid) + "\nstarted " + std::to_string(started) + "\n";
}

/// The first line of the holder's diagnostics ("pid 1234"), or a placeholder. Best effort only.
std::string describe_lock_holder(const std::filesystem::path& dataPath) {
    std::ifstream file(dataPath / kDataLockFileName);
    std::string line;
    if (!file || !std::getline(file, line) || line.empty()) {
        return "pid unknown";
    }
    return line;
}

#if defined(_WIN32)

/// Held for the life of the process. Never closed on purpose — the OS closes it, including on a
/// crash, which is the entire argument for a handle lock over a pidfile. Not inheritable (null
/// security attributes), so a child process cannot keep the lock alive after we die.
void* sDataLockHandle = nullptr;

/// One attempt. Returns true when this process now owns the directory. `o_conflict` distinguishes
/// "someone else has it" (try the next directory) from "could not lock for any other reason" (run
/// anyway).
bool try_lock_data_directory_once(const std::filesystem::path& dataPath, bool& o_conflict) {
    o_conflict = false;
    const auto lockPath = dataPath / kDataLockFileName;

    // ★ Opened SHARED, and locked with LockFileEx. The first version opened with dwShareMode = 0,
    // which made the open itself the lock — but that also fails if an antivirus scanner, the search
    // indexer or a backup tool merely has the file open at that instant, and a false conflict now
    // costs a session with saving disabled. A byte-range lock is only contended by another lock.
    // No FILE_SHARE_DELETE, deliberately: while we hold it, the file cannot be deleted out from
    // under us and recreated by a second instance that would then lock a different file.
    HANDLE handle = CreateFileW(lockPath.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        // A sharing violation is what an older build (dwShareMode = 0) holding the file looks like,
        // so it is still a conflict. Anything else — access denied on a read-only folder, a
        // pending delete — is an I/O problem, and an I/O problem must never stop the game booting.
        o_conflict = GetLastError() == ERROR_SHARING_VIOLATION;
        return false;
    }

    // One byte at 4 GiB rather than at 0: locking past end-of-file is allowed, and it leaves the
    // diagnostics at the start of the file readable by the instance that is refused.
    OVERLAPPED region{};
    region.Offset = 0;
    region.OffsetHigh = 1;
    if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &region))
    {
        const DWORD error = GetLastError();
        CloseHandle(handle);
        o_conflict = error == ERROR_LOCK_VIOLATION;
        return false;
    }

    const std::string text = lock_diagnostics();
    LARGE_INTEGER start{};
    DWORD written = 0;
    if (SetFilePointerEx(handle, start, nullptr, FILE_BEGIN) && SetEndOfFile(handle)) {
        WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    }

    sDataLockHandle = handle;
    return true;
}

#else

/// Held for the life of the process; the kernel drops the flock when the last descriptor closes,
/// however the process dies. O_CLOEXEC matters: flock belongs to the open file description, so a
/// child that inherited the descriptor (the file manager open_data_path() launches, say) would
/// otherwise keep the folder locked after we exit.
int sDataLockFd = -1;

bool try_lock_data_directory_once(const std::filesystem::path& dataPath, bool& o_conflict) {
    o_conflict = false;
    const auto lockPath = dataPath / kDataLockFileName;

    const int fd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }

    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
        const std::string text = lock_diagnostics();
        if (::ftruncate(fd, 0) == 0) {
            if (::pwrite(fd, text.data(), text.size(), 0) < 0) {
                // Diagnostics only; the lock is already held.
            }
        }
        sDataLockFd = fd;
        return true;
    }

    const int error = errno;
    ::close(fd);
    if (error == EWOULDBLOCK) {
        o_conflict = true;
    }
    return false;
}

#endif

bool try_lock_data_directory(const std::filesystem::path& dataPath, bool& o_conflict) {
    for (int attempt = 1;; ++attempt) {
        if (try_lock_data_directory_once(dataPath, o_conflict)) {
            return true;
        }
        if (!o_conflict || attempt >= kLockAttempts) {
            return false;
        }
        std::this_thread::sleep_for(kLockRetryDelay);
    }
}

void note_lock_taken(const std::filesystem::path& dataPath) {
    sLockStatusLine = "Data lock: this process holds '" +
                      borealis::io::fs_path_to_string(dataPath / kDataLockFileName) +
                      "'; it is the only Dusklight writing saves there.";
    Log.info("{}", sLockStatusLine);
}

void note_lock_unavailable(const std::filesystem::path& dataPath) {
    sLockStatusLine = "Data lock: could not lock '" + borealis::io::fs_path_to_string(dataPath) +
                      "' (not held by anyone else — an unlockable filesystem, e.g. a network or "
                      "cloud-synced folder). Continuing WITHOUT a single-instance guard.";
    Log.warn("{}", sLockStatusLine);
}

/**
 * Another process holds @p dataPath and this one cannot move. Run, but never write a save there.
 * The refusal itself is enforced by multiplayer/session/save_guard, which asks
 * is_data_directory_shared(); this only records the fact and says so.
 */
void disable_saving_shared_directory(const std::filesystem::path& dataPath, std::string_view why) {
    sDataDirectoryShared = true;
    sSharedDataDirectoryPath = dataPath;
    sLockStatusLine = "Data lock: REFUSED — another Dusklight (" + describe_lock_holder(dataPath) +
                      ") already holds '" + borealis::io::fs_path_to_string(dataPath) + "' and " +
                      std::string(why) +
                      ". Saving is DISABLED for this process so the two cannot tear one memory "
                      "card; the other instance keeps sole ownership of the saves.";
    Log.warn("{}", sLockStatusLine);
}

/// Lock an explicitly chosen directory (--data-dir, --user-dir): never relocated, so a conflict
/// means saving is disabled rather than shared.
void lock_explicit_data_directory(const std::filesystem::path& dataPath, std::string_view flag) {
    bool conflict = false;
    if (try_lock_data_directory(dataPath, conflict)) {
        note_lock_taken(dataPath);
    } else if (conflict) {
        disable_saving_shared_directory(dataPath,
            std::string(flag) + " is an explicit choice, so this instance was not moved elsewhere");
    } else {
        note_lock_unavailable(dataPath);
    }
}

/// Where the Nth instance's data lives: the same directory with a numbered suffix, so it sits
/// beside the original and is obvious in a file manager rather than hidden somewhere else.
std::filesystem::path instance_directory(const std::filesystem::path& dataPath, int index) {
    auto normalized = dataPath;
    if (normalized.filename().empty()) {
        normalized = normalized.parent_path();
    }
    return normalized.parent_path() /
           (normalized.filename().string() + "-" + std::to_string(index));
}

/// Left in an instance directory whose seed did not finish (a source file never held still long
/// enough to copy, or an I/O error). While it exists the next relocated launch seeds again, filling
/// in only what is missing, instead of treating the folder as "used before" and never retrying.
constexpr std::string_view kSeedIncompleteMarker = ".dusklight-seed-incomplete";

/// How long a source file must have gone unmodified before it is copied. Seeding only ever happens
/// while the source instance is RUNNING (that is why this process was relocated), and one save is
/// five open/write/close cycles spread over milliseconds; a second of quiet means none is underway.
constexpr std::chrono::milliseconds kSeedQuietWindow{1000};
/// Gap between the two reads that must agree byte for byte.
constexpr std::chrono::milliseconds kSeedRereadDelay{50};
/// Attempts per file before giving up on it. Bounds a constantly-writing source to ~10 s per file.
constexpr int kSeedAttempts = 8;

enum class SeedResult { Copied, AlreadyThere, SourceGone, GaveUp };

bool read_whole_file(const std::filesystem::path& path, std::vector<char>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

/// Test knob: DUSK_TEST_SEED_CHURN=N makes the first N stability checks of every seeded file
/// report "the source changed", exercising the re-copy (N < kSeedAttempts) and give-up
/// (N >= kSeedAttempts) paths without anything having to write to a real save.
int seed_test_churn() {
    static const int value = [] {
        const char* env = std::getenv("DUSK_TEST_SEED_CHURN");
        return env != nullptr ? std::atoi(env) : 0;
    }();
    return value;
}

/**
 * Copy one file out of a live instance without ever capturing it mid-write, and without ever
 * leaving a partial file behind.
 *
 * Accepted only when: the source has been quiet for kSeedQuietWindow, two reads kSeedRereadDelay
 * apart are byte-identical and match the size stat() reported, and size + mtime are unchanged
 * across the whole attempt. The bytes then go to a temp file beside the target and are renamed
 * into place, so a crash mid-seed leaves no half-written card under the real name. The source is
 * only ever opened for reading. (A content check was considered and rejected: a .gci header has no
 * checksum, and the game's own per-slot checksums already fall back to the mirror on load; the
 * stability check is what actually rules out a torn copy.)
 */
SeedResult seed_file_stably(const std::filesystem::path& src, const std::filesystem::path& dst) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(dst, ec)) {
        return SeedResult::AlreadyThere;
    }

    int changes = 0;
    for (int attempt = 1; attempt <= kSeedAttempts; ++attempt) {
        if (attempt > 1) {
            std::this_thread::sleep_for(kSeedRereadDelay);
        }
        const auto size1 = fs::file_size(src, ec);
        const auto time1 = ec ? fs::file_time_type{} : fs::last_write_time(src, ec);
        if (ec) {
            std::error_code existsEc;
            if (!fs::exists(src, existsEc)) {
                return SeedResult::SourceGone;
            }
            Log.warn("Seed: could not stat '{}': {}", borealis::io::fs_path_to_string(src),
                ec.message());
            ec.clear();
            ++changes;
            continue;
        }

        const auto age = fs::file_time_type::clock::now() - time1;
        if (age < kSeedQuietWindow) {
            // Written moments ago: a save may still be in flight. Wait out the window, re-check.
            const auto wait =
                std::chrono::duration_cast<std::chrono::milliseconds>(kSeedQuietWindow - age);
            std::this_thread::sleep_for(std::max(wait, kSeedRereadDelay));
            ++changes;
            continue;
        }

        std::vector<char> first;
        std::vector<char> second;
        const bool read1 = read_whole_file(src, first);
        std::this_thread::sleep_for(kSeedRereadDelay);
        const bool read2 = read_whole_file(src, second);
        const auto size2 = fs::file_size(src, ec);
        const auto time2 = ec ? fs::file_time_type{} : fs::last_write_time(src, ec);
        const bool stable = read1 && read2 && !ec && first == second && first.size() == size1 &&
                            size1 == size2 && time1 == time2 && attempt > seed_test_churn();
        ec.clear();
        if (!stable) {
            ++changes;
            continue;
        }

        auto tmp = dst;
        tmp += ".seed-tmp";
        fs::remove(tmp, ec);  // A leftover from a seed that crashed; never a real save.
        ec.clear();
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(first.data(), static_cast<std::streamsize>(first.size()));
            out.close();
            if (!out.good()) {
                Log.warn("Seed: could not write '{}'", borealis::io::fs_path_to_string(tmp));
                fs::remove(tmp, ec);
                return SeedResult::GaveUp;
            }
        }
        fs::rename(tmp, dst, ec);
        if (ec) {
            Log.warn("Seed: could not move '{}' into place: {}",
                borealis::io::fs_path_to_string(dst), ec.message());
            fs::remove(tmp, ec);
            return SeedResult::GaveUp;
        }
        if (changes > 0) {
            Log.warn("Seed: re-copied '{}' after {} unstable attempt(s) (the source instance was "
                     "writing it); the copy is a consistent snapshot.",
                borealis::io::fs_path_to_string(src), changes);
        } else {
            Log.info("Seed: copied '{}'", borealis::io::fs_path_to_string(src));
        }
        return SeedResult::Copied;
    }

    Log.warn("Seed: GAVE UP on '{}' — it changed on every one of {} attempts. It was NOT copied "
             "(a torn card is worse than none); this instance starts without it, and the copy is "
             "retried on its next launch.",
        borealis::io::fs_path_to_string(src), kSeedAttempts);
    return SeedResult::GaveUp;
}

/**
 * Give a freshly-created instance directory the first one's saves and settings.
 *
 * Copies, never moves or links: the whole point is that the original .gci is not touched. Only done
 * when the target does not already exist (or a previous seed left kSeedIncompleteMarker), so a
 * second instance that has been used before continues from where IT left off rather than being
 * reset to the primary's progress every launch. Existing target files are never overwritten.
 *
 * The source instance is necessarily running while this happens, so every file goes through
 * seed_file_stably(). On failure the file is left OUT rather than copied torn: the original is
 * untouched either way, and an absent card is recoverable (retried next launch) where a corrupt
 * one silently becomes this instance's progress.
 */
void seed_instance_directory(const std::filesystem::path& from, const std::filesystem::path& to) {
    namespace fs = std::filesystem;
    std::error_code ec;
    bool incomplete = false;
    int copied = 0;

    const auto seed = [&](const fs::path& src, const fs::path& dst) {
        switch (seed_file_stably(src, dst)) {
        case SeedResult::Copied:
            ++copied;
            break;
        case SeedResult::GaveUp:
            incomplete = true;
            break;
        case SeedResult::AlreadyThere:
        case SeedResult::SourceGone:
            break;
        }
    };

    const auto markerPath = to / kSeedIncompleteMarker;
    {
        // Written FIRST, removed last: a crash mid-seed also counts as incomplete.
        std::ofstream marker(markerPath, std::ios::trunc);
    }

    const auto configSrc = from / "config.json";
    if (fs::exists(configSrc, ec)) {
        seed(configSrc, to / "config.json");
    }
    ec.clear();

    // Memory cards only — deliberately NOT kUserDataDirectories, which also lists
    // texture_replacements. That can be gigabytes, and duplicating it per instance would turn a
    // safety measure into a disk-filling one.
    for (const std::string_view region : {"USA", "EUR", "JAP"}) {
        const auto src = from / region;
        if (!fs::is_directory(src, ec)) {
            ec.clear();
            continue;
        }
        for (auto it = fs::recursive_directory_iterator(src, ec);
            !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            const auto dst = to / region / it->path().lexically_relative(src);
            std::error_code entryEc;
            if (it->is_directory(entryEc)) {
                fs::create_directories(dst, entryEc);
            } else if (it->is_regular_file(entryEc) && it->path().extension() != ".seed-tmp") {
                fs::create_directories(dst.parent_path(), entryEc);
                seed(it->path(), dst);
            }
        }
        if (ec) {
            Log.warn("Seed: could not list '{}': {}", borealis::io::fs_path_to_string(src),
                ec.message());
            incomplete = true;
            ec.clear();
        }
    }

    if (incomplete) {
        Log.warn("Seed of '{}' from '{}' is INCOMPLETE ({} file(s) copied); left '{}' so the next "
                 "launch into this folder retries the missing files.",
            borealis::io::fs_path_to_string(to), borealis::io::fs_path_to_string(from), copied,
            kSeedIncompleteMarker);
    } else {
        fs::remove(markerPath, ec);
        Log.info("Seed of '{}' from '{}' complete: {} file(s) copied.",
            borealis::io::fs_path_to_string(to), borealis::io::fs_path_to_string(from), copied);
    }
}

/**
 * Take the lock, or move this process to a directory of its own. Returns the directory to actually
 * use, which is @p dataPath in the overwhelmingly common single-instance case.
 */
std::filesystem::path lock_or_relocate_data_directory(const std::filesystem::path& dataPath) {
    bool conflict = false;
    if (try_lock_data_directory(dataPath, conflict)) {
        note_lock_taken(dataPath);
        return dataPath;
    }

    if (!conflict) {
        // Could not lock, and not because anyone else holds it. Run in the normal directory rather
        // than inventing a problem: an unlockable filesystem is not evidence of a second instance.
        note_lock_unavailable(dataPath);
        return dataPath;
    }

    for (int index = 2; index < 2 + kMaxInstanceDirectories; ++index) {
        const auto candidate = instance_directory(dataPath, index);

        std::error_code ec;
        const bool existed = std::filesystem::exists(candidate, ec);
        ensure_data_directory(candidate);

        bool candidateConflict = false;
        const bool candidateLocked = try_lock_data_directory(candidate, candidateConflict);
        if (!candidateLocked) {
            if (candidateConflict) {
                continue;  // A third instance already has this one.
            }
            Log.warn("Could not lock '{}' either — continuing there unguarded.",
                borealis::io::fs_path_to_string(candidate));
        }

        // Re-seed a folder whose first seed did not finish; it only fills in missing files.
        const bool seedIncomplete =
            existed && std::filesystem::exists(candidate / kSeedIncompleteMarker, ec);
        if (!existed || seedIncomplete) {
            seed_instance_directory(dataPath, candidate);
        }

        sIsSecondaryInstance = true;
        sSecondaryInstancePath = candidate;
        sLockStatusLine =
            "Data lock: another Dusklight (" + describe_lock_holder(dataPath) +
            ") already holds '" + borealis::io::fs_path_to_string(dataPath) +
            "', so this one moved to '" + borealis::io::fs_path_to_string(candidate) + "' (" +
            (candidateLocked ? "locked" : "unlockable, unguarded") + "). Its saves were " +
            (existed ? "already there from a previous run" : "copied from the original") +
            " and anything saved from now on goes THERE, not to the original.";
        Log.warn("{}", sLockStatusLine);
        return candidate;
    }

    // Nowhere left to go. Sharing the original would be the torn-card race, so stay here and
    // refuse to write instead.
    disable_saving_shared_directory(dataPath, "every numbered instance folder (-2 to -" +
                                                  std::to_string(kMaxInstanceDirectories + 1) +
                                                  ") is in use too");
    return dataPath;
}

/// Where rolling memory card backups live, relative to the data directory.
constexpr std::string_view kSaveBackupDirName = "save-backups";

/// How many backups to keep per card file. 32 KB each, so ten of them cost a third of a megabyte.
constexpr std::size_t kSaveBackupsKept = 10;

bool files_have_same_contents(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    const auto sizeA = std::filesystem::file_size(a, ec);
    if (ec) {
        return false;
    }
    const auto sizeB = std::filesystem::file_size(b, ec);
    if (ec || sizeA != sizeB) {
        return false;
    }

    std::ifstream streamA(a, std::ios::binary);
    std::ifstream streamB(b, std::ios::binary);
    if (!streamA || !streamB) {
        return false;
    }

    std::array<char, 4096> bufferA{};
    std::array<char, 4096> bufferB{};
    while (streamA && streamB) {
        streamA.read(bufferA.data(), bufferA.size());
        streamB.read(bufferB.data(), bufferB.size());
        const auto readA = streamA.gcount();
        if (readA != streamB.gcount()) {
            return false;
        }
        if (std::memcmp(bufferA.data(), bufferB.data(), static_cast<std::size_t>(readA)) != 0) {
            return false;
        }
    }

    return true;
}

/**
 * Recursive content comparison for a sidecar directory. Same file set, same bytes; an unreadable
 * tree compares unequal, so the worst case is one redundant backup rather than a missed one.
 */
bool directories_have_same_contents(
    const std::filesystem::path& a, const std::filesystem::path& b) {
    const auto collect = [](const std::filesystem::path& root,
                             std::vector<std::filesystem::path>& out) {
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
            !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (it->is_regular_file()) {
                out.push_back(it->path().lexically_relative(root));
            }
        }
        std::sort(out.begin(), out.end());
        return !ec;
    };

    std::vector<std::filesystem::path> filesA;
    std::vector<std::filesystem::path> filesB;
    if (!collect(a, filesA) || !collect(b, filesB) || filesA != filesB) {
        return false;
    }
    for (const auto& relative : filesA) {
        if (!files_have_same_contents(a / relative, b / relative)) {
            return false;
        }
    }
    return true;
}

/**
 * One thing worth backing up. A memory card is not just the .gci any more: upstream added raw card
 * images (`<dataPath>/MemoryCardA.EUR.raw`, up to 16 MB) and per-save mod sidecars
 * (`<Card A>/<stem>.mods/` and `<dataPath>/MemoryCardA.EUR.raw.mods/`), which the Save Manager
 * treats as part of the save. A backup that restores the .gci and loses the randomizer's seed data
 * is not a backup of the save.
 */
struct SaveArtifact {
    std::filesystem::path path;
    /// Backups are named `<key>.<YYYYmmdd-HHMMSS><extension>`.
    std::string key;
    std::string extension;
    bool directory = false;
    std::size_t kept = kSaveBackupsKept;
};

/// How many backups to keep per raw card image: they are up to 512 times the size of a .gci.
constexpr std::size_t kRawBackupsKept = 5;

constexpr std::size_t kStampLength = 15;  // YYYYmmdd-HHMMSS

/**
 * True when @p name is a backup of @p artifact. Parsed exactly rather than by prefix, because the
 * keys nest: "MemoryCardA.EUR" is a prefix of "MemoryCardA.EUR.raw.mods", and a .gci stem is a
 * prefix of its own ".mods" directory. A prefix match would let one artifact's pruning delete
 * another's backups.
 */
bool is_backup_of(const std::string& name, const SaveArtifact& artifact) {
    const std::string prefix = artifact.key + ".";
    if (name.size() != prefix.size() + kStampLength + artifact.extension.size() ||
        !name.starts_with(prefix) || !name.ends_with(artifact.extension))
    {
        return false;
    }
    for (std::size_t i = 0; i < kStampLength; ++i) {
        const char c = name[prefix.size() + i];
        if (i == 8 ? c != '-' : (c < '0' || c > '9')) {
            return false;
        }
    }
    return true;
}

/**
 * Collect the save artifacts under a data directory.
 *
 * Deliberately NOT a recursive_directory_iterator over the whole tree: in --data-dir mode the data
 * directory also holds caches and logs, and walking those to find a 32 KB file would be paying a
 * lot for nothing. Aurora lays cards out as <dataPath>/<region>/Card A (GCI folder) or
 * <dataPath>/MemoryCardA.<region>.raw (raw image) (lib/dolphin/card.cpp get_card_full_path), so
 * this walks exactly two levels and matches the "Card" / "MemoryCard" prefixes rather than
 * hardcoding a region list — a new region then needs no change here.
 */
std::vector<SaveArtifact> find_save_artifacts(const std::filesystem::path& dataPath) {
    std::vector<SaveArtifact> artifacts;
    std::error_code ec;

    for (const auto& topEntry : std::filesystem::directory_iterator(dataPath, ec)) {
        if (ec) {
            continue;
        }
        const auto topName = topEntry.path().filename().string();
        if (topName == kSaveBackupDirName) {
            continue;
        }

        if (topName.starts_with("MemoryCard")) {
            if (topEntry.is_regular_file() && topEntry.path().extension() == ".raw") {
                artifacts.push_back({
                    .path = topEntry.path(),
                    .key = topEntry.path().stem().string(),
                    .extension = ".raw",
                    .kept = kRawBackupsKept,
                });
            } else if (topEntry.is_directory() && topName.ends_with(".raw.mods")) {
                artifacts.push_back({.path = topEntry.path(), .key = topName, .directory = true});
            }
            continue;
        }
        if (!topEntry.is_directory()) {
            continue;
        }

        std::error_code slotEc;
        for (const auto& slotEntry : std::filesystem::directory_iterator(topEntry.path(), slotEc)) {
            if (slotEc || !slotEntry.is_directory()) {
                continue;
            }
            if (!slotEntry.path().filename().string().starts_with("Card")) {
                continue;
            }

            std::error_code fileEc;
            for (const auto& fileEntry :
                std::filesystem::directory_iterator(slotEntry.path(), fileEc))
            {
                if (fileEc) {
                    continue;
                }
                const auto fileName = fileEntry.path().filename().string();
                if (fileEntry.is_regular_file() && fileEntry.path().extension() == ".gci") {
                    artifacts.push_back({
                        .path = fileEntry.path(),
                        .key = fileEntry.path().stem().string(),
                        .extension = ".gci",
                    });
                } else if (fileEntry.is_directory() && fileName.ends_with(".mods")) {
                    artifacts.push_back(
                        {.path = fileEntry.path(), .key = fileName, .directory = true});
                }
            }
        }
    }

    return artifacts;
}

void prune_save_backups(const std::filesystem::path& backupDir, const SaveArtifact& artifact) {
    std::vector<std::filesystem::path> existing;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(backupDir, ec)) {
        if (ec || entry.is_directory() != artifact.directory) {
            continue;
        }
        if (is_backup_of(entry.path().filename().string(), artifact)) {
            existing.push_back(entry.path());
        }
    }

    if (existing.size() <= artifact.kept) {
        return;
    }

    // Names carry a sortable timestamp, so lexicographic order is chronological order. Cheaper and
    // steadier than asking the filesystem for mtimes, which a copy or a sync client can rewrite.
    std::sort(existing.begin(), existing.end());
    for (std::size_t i = 0; i + artifact.kept < existing.size(); ++i) {
        std::error_code removeEc;
        std::filesystem::remove_all(existing[i], removeEc);
    }
}

/**
 * ★ Rolling save backup, taken once at startup before the game can write anything.
 *
 * Every failure mode in .claude/plan/13-save-safety.md — a torn write from two processes on one
 * data directory (P0), a crash midway through the five open/seek/write/close cycles a single save
 * costs (P1), a read-back verify that reports success when it failed (P4) — has the same
 * consequence without this: the file is gone. With it, the consequence is losing one session. That
 * asymmetry is why this is worth more than its size, and why it runs unconditionally rather than
 * behind a setting.
 *
 * Covers .gci files, raw card images and both kinds of mod sidecar directory (see SaveArtifact).
 *
 * Best-effort by construction. Every error is swallowed with at most a warning: a backup that
 * cannot be written must never be a reason the game will not start, which is the same principle
 * that makes the P0 lock a harder call than this one.
 */
void backup_memory_cards(const std::filesystem::path& dataPath) {
    const auto artifacts = find_save_artifacts(dataPath);
    if (artifacts.empty()) {
        return;
    }

    const auto backupDir = dataPath / kSaveBackupDirName;
    std::error_code ec;
    std::filesystem::create_directories(backupDir, ec);
    if (ec) {
        Log.warn("Could not create save backup directory '{}': {} — continuing without backups",
            borealis::io::fs_path_to_string(backupDir), ec.message());
        return;
    }

    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::array<char, 32> stamp{};
    if (std::strftime(stamp.data(), stamp.size(), "%Y%m%d-%H%M%S", &local) != kStampLength) {
        return;
    }

    for (const auto& artifact : artifacts) {
        // Skip a launch that changed nothing: without this, ten backups of an untouched save push
        // the one backup that mattered out of the window.
        bool alreadyHaveIt = false;
        std::error_code scanEc;
        for (const auto& entry : std::filesystem::directory_iterator(backupDir, scanEc)) {
            if (scanEc || entry.is_directory() != artifact.directory ||
                !is_backup_of(entry.path().filename().string(), artifact))
            {
                continue;
            }
            if (artifact.directory ? directories_have_same_contents(artifact.path, entry.path()) :
                                     files_have_same_contents(artifact.path, entry.path()))
            {
                alreadyHaveIt = true;
                break;
            }
        }
        if (alreadyHaveIt) {
            continue;
        }

        const auto target = backupDir / (artifact.key + "." + stamp.data() + artifact.extension);
        std::error_code copyEc;
        if (artifact.directory) {
            std::filesystem::copy(artifact.path, target,
                std::filesystem::copy_options::recursive |
                    std::filesystem::copy_options::overwrite_existing,
                copyEc);
        } else {
            std::filesystem::copy_file(
                artifact.path, target, std::filesystem::copy_options::overwrite_existing, copyEc);
        }
        if (copyEc) {
            Log.warn("Could not back up '{}': {}", borealis::io::fs_path_to_string(artifact.path),
                copyEc.message());
            // A half-copied directory would later dedupe against nothing useful; drop it.
            std::error_code cleanupEc;
            std::filesystem::remove_all(target, cleanupEc);
            continue;
        }

        Log.info("Backed up '{}' to '{}'", borealis::io::fs_path_to_string(artifact.path),
            borealis::io::fs_path_to_string(target));
        prune_save_backups(backupDir, artifact);
    }
}

}  // namespace

borealis::data::Manager& manager() {
    static borealis::data::Manager instance{
        AppInfo,
        {
            .defaultPath = {.useDocumentsOnIOS = true},
            .portableRelativePath = "data",
            .legacyApps =
                {
                    {.orgName = "TwilitRealm", .appName = "Dusk"},
                },
            .migration =
                {
                    .directories =
                        {
                            "texture_replacements",
                            "USA",
                            "EUR",
                            "JAP",
                            "MemoryCardA.USA.raw.mods",
                            "MemoryCardA.EUR.raw.mods",
                            "MemoryCardA.JAP.raw.mods",
                            "MemoryCardB.USA.raw.mods",
                            "MemoryCardB.EUR.raw.mods",
                            "MemoryCardB.JAP.raw.mods",
                        },
                    .files =
                        {
                            "achievements.json",
                            "config.json",
                            "controller_ports.dat",
                            "gamecontrollerdb.txt",
                            "imgui.ini",
                            "keyboard_bindings.dat",
                            "states.json",
                        },
                    .extensions = {".controller", ".gci"},
                    .filenamePatterns =
                        {
                            {.prefix = "MemoryCard", .suffix = ".raw"},
                        },
                },
        },
    };
    return instance;
}

void set_data_path_override(const std::filesystem::path& path) {
    if (path.empty()) {
        return;
    }
    sDataPathOverride = path;
}

Paths initialize_data(const std::filesystem::path& userDirectoryOverride) {
    if (sDataPathOverride) {
        // Everything - save files, config, logs and caches - lands in the one directory, so a
        // second instance shares nothing with the first and can be deleted wholesale afterwards.
        // The manager is told too, so the settings UI reports the folder as fixed for this session.
        const auto status = manager().initialize(*sDataPathOverride);
        if (!status) {
            Log.fatal("Failed to initialize data folders: {}", status_message(status));
        }
        const auto& userPath = manager().paths().userPath;
        // Locked but never relocated: --data-dir is an explicit instruction about where things go,
        // and silently moving them would break the two-instance harness in the most confusing
        // possible direction. A conflict here means someone pointed two processes at one sandbox,
        // so this one runs with saving disabled rather than sharing the card.
        lock_explicit_data_directory(userPath, "--data-dir");
        // The startup backup writes into save storage too, and would be reading a .gci the holder
        // may be halfway through writing. The holder made its own backup when it started.
        if (!sDataDirectoryShared) {
            backup_memory_cards(userPath);
        }

        Paths paths = manager().paths();
        paths.cachePath = userPath;
        return paths;
    }

    const auto status = manager().initialize(userDirectoryOverride);
    if (!status && status.code != borealis::data::ErrorCode::MigrationIncomplete) {
        Log.fatal("Failed to initialize data folders: {}", status_message(status));
    }
    if (!status) {
        Log.warn("{} Migration will be retried on the next launch.", status_message(status));
    }

    // ★ Claim the directory, or move to one of our own. AFTER the directory exists and any
    // migration has landed (there is nothing to lock or copy before that), and BEFORE the backup
    // and the first card write, so a second instance never opens the first one's .gci at all.
    // An explicit --user-dir is treated like --data-dir: locked, never relocated.
    const auto dataPath = manager().paths().userPath;
    if (manager().has_user_directory_override()) {
        lock_explicit_data_directory(dataPath, "--user-dir");
    } else {
        const auto activePath = lock_or_relocate_data_directory(dataPath);
        if (activePath != dataPath) {
            // Re-resolve through the manager so everything that asks it agrees on the folder. This
            // marks the location as overridden for the session, which also stops a relocated
            // instance from rewriting the real install's location descriptor.
            const auto relocated = manager().initialize(activePath);
            if (!relocated) {
                Log.fatal("Failed to initialize relocated data folder: {}",
                    status_message(relocated));
            }
        }
    }

    // After the directory exists and any migration has landed, and before the game can write a
    // card. Skipped when another process holds the directory (see the --data-dir branch).
    if (!sDataDirectoryShared) {
        backup_memory_cards(manager().paths().userPath);
    }

    return manager().paths();
}

std::filesystem::path base_path_relative(const std::filesystem::path& path) {
    return manager().base_path_relative(path);
}

std::filesystem::path configured_data_path() {
    return manager().configured_data_path();
}

std::filesystem::path cache_path() {
    if (sDataPathOverride) {
        return manager().paths().userPath;
    }
    return manager().paths().cachePath;
}

bool is_secondary_instance() {
    return sIsSecondaryInstance;
}

std::filesystem::path secondary_instance_path() {
    return sSecondaryInstancePath;
}

bool is_data_directory_shared() {
    return sDataDirectoryShared;
}

std::filesystem::path shared_data_directory_path() {
    return sSharedDataDirectoryPath;
}

void log_data_lock_status() {
    if (sLockStatusLine.empty()) {
        return;
    }
    if (sDataDirectoryShared || sIsSecondaryInstance) {
        Log.warn("{}", sLockStatusLine);
    } else {
        Log.info("{}", sLockStatusLine);
    }
}

bool open_data_path() {
    return operation_succeeded(manager().open_active_data_path());
}

bool set_custom_data_path(const std::filesystem::path& path, std::string* errorOut) {
    return operation_succeeded(manager().set_custom_data_path(path), errorOut);
}

bool set_custom_data_path(const char* path, std::string* errorOut) {
    if (path == nullptr) {
        if (errorOut != nullptr) {
            *errorOut = "Choose a folder.";
        }
        return false;
    }
    return set_custom_data_path(borealis::io::fs_path_from_utf8(path), errorOut);
}

bool set_portable_data_path() {
    return operation_succeeded(manager().set_portable_data_path());
}

bool reset_data_path() {
    return operation_succeeded(manager().reset_data_path());
}

bool is_default_data_path() {
    return manager().is_default_data_path();
}

bool is_data_path_restart_pending() {
    return manager().is_data_path_restart_pending();
}

std::filesystem::path user_home_path() {
    return manager().user_home_path();
}

std::filesystem::path normalized_display_path(const std::filesystem::path& path) {
    return manager().normalized_display_path(path);
}

std::string abbreviated_path_string(const std::filesystem::path& path) {
    return manager().abbreviated_path_string(path);
}

}  // namespace dusk::data
