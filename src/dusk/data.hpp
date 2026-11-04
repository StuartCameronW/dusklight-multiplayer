#pragma once

#include <borealis/data.hpp>

#include <filesystem>
#include <string>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(_WIN32) ||                                                                             \
    (defined(__APPLE__) && !TARGET_OS_IOS && !TARGET_OS_TV && !TARGET_OS_MACCATALYST) ||           \
    (defined(__linux__) && !defined(__ANDROID__))
#define DUSK_CAN_OPEN_DATA_FOLDER 1
#else
#define DUSK_CAN_OPEN_DATA_FOLDER 0
#endif

namespace dusk::data {

using Paths = borealis::data::Paths;

borealis::data::Manager& manager();

/// Point this process at its own data directory, ignoring the installed one entirely. Must be
/// called before initialize_data(). Exists so two instances can run side by side without sharing a
/// memory card, a config file, a cache or a log (borealis's --user-dir moves only the first two).
void set_data_path_override(const std::filesystem::path& path);

Paths initialize_data(const std::filesystem::path& userDirectoryOverride = {});
std::filesystem::path base_path_relative(const std::filesystem::path& path);
std::filesystem::path configured_data_path();
std::filesystem::path cache_path();
bool open_data_path();
bool set_custom_data_path(const char* path, std::string* errorOut);
bool set_custom_data_path(const std::filesystem::path& path, std::string* errorOut);
bool set_portable_data_path();
bool reset_data_path();
bool is_default_data_path();
bool is_data_path_restart_pending();
std::filesystem::path user_home_path();
std::filesystem::path normalized_display_path(const std::filesystem::path& path);
std::string abbreviated_path_string(const std::filesystem::path& path);

/**
 * True when another Dusklight was already using the normal data directory, so this process was
 * moved to a numbered one of its own.
 *
 * ★ This MUST be surfaced to the player, not just logged. Silently sending someone's saves
 * somewhere else is the failure this whole mechanism exists to prevent, in a new costume: they play
 * for two hours, quit, relaunch, and their progress is not there. `secondary_instance_path()` is
 * the folder their saves actually went to.
 */
bool is_secondary_instance();
std::filesystem::path secondary_instance_path();

/**
 * True when another Dusklight already held this process's data directory and this one could not
 * be moved to a folder of its own — an explicit --data-dir or --user-dir, or every numbered
 * instance folder taken. Such a process runs, but must never write a save: the two would tear one
 * memory card. Sticky for the life of the process.
 *
 * Enforced by dusk::mp's save guard (multiplayer/session/save_guard.hpp), which every save write
 * path already asks, so this is one more reason that guard refuses rather than a second guard.
 * `shared_data_directory_path()` is the directory the other instance holds.
 */
bool is_data_directory_shared();
std::filesystem::path shared_data_directory_path();

/**
 * Repeat the startup lock decision into the log. initialize_data() runs before file logging
 * exists, so its own line reaches only the console; call this once file logging is up.
 */
void log_data_lock_status();

}  // namespace dusk::data
