#pragma once

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

#if (defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_MACCATALYST)
#define DUSK_CAN_CHANGE_DATA_FOLDER 0
#else
#define DUSK_CAN_CHANGE_DATA_FOLDER 1
#endif

namespace dusk::data {

struct Paths {
    std::filesystem::path userPath;
    std::filesystem::path cachePath;
};

/// Point this process at its own data directory, ignoring the installed one entirely. Must be
/// called before initialize_data(). Exists so two instances can run side by side without sharing a
/// memory card, a config file or a log.
void set_data_path_override(const std::filesystem::path& path);

Paths initialize_data();
std::filesystem::path configured_data_path();
std::filesystem::path cache_path();
bool open_data_path();
bool set_custom_data_path(const char* path, std::string* errorOut);
bool set_custom_data_path(const std::filesystem::path& path, std::string* errorOut);
bool set_portable_data_path();
bool reset_data_path();
bool is_default_data_path();
bool is_data_path_restart_pending();

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

}  // namespace dusk::data
