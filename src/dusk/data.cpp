#include "data.hpp"

#include "dusk/app_info.hpp"
#include "dusk/io.hpp"
#include "dusk/logging.h"
#include "dusk/main.h"

#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_stdinc.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "nlohmann/json.hpp"

namespace dusk::data {
namespace {

aurora::Module Log{"dusk::data"};

constexpr auto kLocationDescriptorName = "data_location.json";

constexpr std::array<std::string_view, 4> kUserDataDirectories = {
    "texture_replacements",
    "USA",
    "EUR",
    "JAP",
};
constexpr std::array<std::string_view, 7> kUserDataFiles = {
    "achievements.json",
    "config.json",
    "controller_ports.dat",
    "gamecontrollerdb.txt",
    "imgui.ini",
    "keyboard_bindings.dat",
    "states.json",
};

enum class LocationMode {
    Default,
    Portable,
    Custom,
};

struct LocationDescriptor {
    LocationMode mode = LocationMode::Default;
    std::filesystem::path customPath;
    std::filesystem::path previousPath;
};

struct LocatedDescriptor {
    LocationDescriptor descriptor;
    std::filesystem::path path;
};

struct MigrationStats {
    std::uintmax_t directoriesCreated = 0;
    std::uintmax_t filesCopied = 0;
    std::uintmax_t symlinksCopied = 0;
    std::uintmax_t sourcesRemoved = 0;
    std::uintmax_t emptyDirectoriesRemoved = 0;
    std::uintmax_t skippedExistingTargets = 0;
    std::uintmax_t skippedDescriptorFiles = 0;
    std::uintmax_t skippedNestedTargets = 0;
    std::uintmax_t skippedUnsupportedEntries = 0;
    std::uintmax_t failures = 0;
};

std::optional<std::filesystem::path> sConfiguredDataPath;
std::optional<std::filesystem::path> sActiveDescriptorPath;
std::optional<std::filesystem::path> sActivePrefPath;
/// Set by --data-dir. Deliberately bypasses the descriptor file rather than writing one: the point
/// is a throwaway sandbox for a second instance, and it must not leave the real install pointing
/// somewhere else if it crashes.
std::optional<std::filesystem::path> sDataPathOverride;

std::filesystem::path path_from_utf8(std::string_view value) {
    return std::filesystem::path{
        reinterpret_cast<const char8_t*>(value.data()),
        reinterpret_cast<const char8_t*>(value.data() + value.size()),
    };
}

std::filesystem::path legacy_path_for_pref_path(const std::filesystem::path& prefPath) {
    if (std::string_view{LegacyAppName}.empty() || prefPath.empty()) {
        return {};
    }

    auto normalizedPrefPath = prefPath;
    if (normalizedPrefPath.filename().empty()) {
        normalizedPrefPath = normalizedPrefPath.parent_path();
    }

    const auto parentPath = normalizedPrefPath.parent_path();
    if (parentPath.empty()) {
        return {};
    }

    return parentPath / LegacyAppName;
}

std::filesystem::path get_pref_path() {
    char* prefPath = SDL_GetPrefPath(OrgName, AppName);
    if (!prefPath) {
        Log.fatal("Unable to get PrefPath: {}", SDL_GetError());
    }

    std::filesystem::path result = path_from_utf8(prefPath);
    SDL_free(prefPath);
    return result;
}

std::filesystem::path active_pref_path() {
    if (sActivePrefPath) {
        return *sActivePrefPath;
    }
    return get_pref_path();
}

std::filesystem::path base_path_relative(const std::filesystem::path& path) {
    const auto* basePath = SDL_GetBasePath();
    if (!basePath) {
        return path;
    }
    return path_from_utf8(basePath) / path;
}

std::filesystem::path default_data_path(const std::filesystem::path& prefPath) {
#ifdef __APPLE__
#if TARGET_OS_IOS && !TARGET_OS_TV
    const char* documentsPath = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    if (!documentsPath) {
        Log.fatal("Unable to get iOS Documents path: {}", SDL_GetError());
    }

    return reinterpret_cast<const char8_t*>(documentsPath);
#endif
#endif

    return prefPath;
}

std::filesystem::path portable_data_path() {
    return base_path_relative("data");
}

std::vector<std::filesystem::path> descriptor_paths(const std::filesystem::path& prefPath) {
    std::vector<std::filesystem::path> paths;
    if (const auto basePath = base_path_relative(kLocationDescriptorName); !basePath.empty()) {
        paths.push_back(basePath);
    }
    paths.push_back(prefPath / kLocationDescriptorName);
    return paths;
}

std::optional<LocationDescriptor> read_location_descriptor_file(const std::filesystem::path& path) {
    if (path.empty()) {
        return std::nullopt;
    }
    if (std::error_code ec; !std::filesystem::exists(path, ec)) {
        return std::nullopt;
    }

    try {
        const auto bytes = io::FileStream::ReadAllBytes(path);
        const auto json = nlohmann::json::parse(bytes);
        if (!json.is_object()) {
            Log.warn("Ignoring data location descriptor '{}': root is not an object",
                io::fs_path_to_string(path));
            return std::nullopt;
        }

        LocationDescriptor descriptor;
        const auto mode = json.value<std::string>("mode", "default");
        if (mode == "portable") {
            descriptor.mode = LocationMode::Portable;
        } else if (mode == "custom") {
            descriptor.mode = LocationMode::Custom;
        } else if (mode != "default") {
            Log.warn("Ignoring unknown data location mode '{}'", mode);
        }

        if (const auto customPath = json.find("customPath");
            customPath != json.end() && customPath->is_string())
        {
            descriptor.customPath = path_from_utf8(customPath->get<std::string>());
        }
        if (const auto previousPath = json.find("previousPath");
            previousPath != json.end() && previousPath->is_string())
        {
            descriptor.previousPath = path_from_utf8(previousPath->get<std::string>());
        }

        return descriptor;
    } catch (const std::exception& e) {
        Log.warn(
            "Ignoring data location descriptor '{}': {}", io::fs_path_to_string(path), e.what());
        return std::nullopt;
    }
}

std::optional<LocatedDescriptor> read_location_descriptor(const std::filesystem::path& prefPath) {
    for (const auto& path : descriptor_paths(prefPath)) {
        if (auto descriptor = read_location_descriptor_file(path)) {
            return LocatedDescriptor{
                .descriptor = *descriptor,
                .path = path,
            };
        }
    }
    return std::nullopt;
}

std::filesystem::path resolve_data_path(
    const std::filesystem::path& prefPath, const LocationDescriptor* descriptor) {
    if (!descriptor) {
        return default_data_path(prefPath);
    }

    switch (descriptor->mode) {
    case LocationMode::Default:
        return default_data_path(prefPath);
    case LocationMode::Portable:
        return portable_data_path();
    case LocationMode::Custom:
        if (!descriptor->customPath.empty()) {
            return descriptor->customPath;
        }
        Log.warn("Data location descriptor requested custom mode without a path");
        return default_data_path(prefPath);
    }

    return default_data_path(prefPath);
}

const char* location_mode_id(LocationMode mode) {
    switch (mode) {
    case LocationMode::Default:
        return "default";
    case LocationMode::Portable:
        return "portable";
    case LocationMode::Custom:
        return "custom";
    }

    return "default";
}

std::filesystem::path normalized_path(const std::filesystem::path& path) {
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return normalized;
    }

    normalized = std::filesystem::absolute(path, ec);
    if (!ec) {
        return normalized.lexically_normal();
    }

    return path.lexically_normal();
}

std::filesystem::path absolute_path(const std::filesystem::path& path) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        return path;
    }
    return absolute.lexically_normal();
}

std::filesystem::path rename_legacy_pref_path(
    const std::filesystem::path& legacyPath, const std::filesystem::path& prefPath) {
    if (legacyPath.empty() || prefPath.empty() ||
        normalized_path(legacyPath) == normalized_path(prefPath))
    {
        return prefPath;
    }

    std::error_code ec;
    if (!std::filesystem::exists(legacyPath, ec)) {
        if (ec) {
            Log.warn("Failed to inspect legacy data directory '{}': {}",
                io::fs_path_to_string(legacyPath), ec.message());
        }
        return prefPath;
    }

    const bool prefExists = std::filesystem::exists(prefPath, ec);
    if (ec) {
        Log.warn("Failed to inspect data directory '{}': {}", io::fs_path_to_string(prefPath),
            ec.message());
        return prefPath;
    }
    if (prefExists) {
        if (!std::filesystem::is_directory(prefPath, ec) ||
            !std::filesystem::is_empty(prefPath, ec))
        {
            if (ec) {
                Log.warn("Failed to inspect data directory '{}': {}",
                    io::fs_path_to_string(prefPath), ec.message());
            } else {
                Log.info("Skipping legacy data directory rename because '{}' is not empty",
                    io::fs_path_to_string(prefPath));
            }
            return prefPath;
        }

        std::filesystem::remove(prefPath, ec);
        if (ec) {
            Log.warn("Failed to remove empty data directory '{}' before legacy rename: {}",
                io::fs_path_to_string(prefPath), ec.message());
            return prefPath;
        }
    }

    std::filesystem::rename(legacyPath, prefPath, ec);
    if (ec) {
        Log.warn("Failed to rename legacy data directory '{}' to '{}': {}",
            io::fs_path_to_string(legacyPath), io::fs_path_to_string(prefPath), ec.message());
        ec.clear();
        if (!std::filesystem::exists(prefPath, ec) && !ec) {
            Log.info("Using legacy data directory '{}' because the new data directory is absent",
                io::fs_path_to_string(legacyPath));
            return legacyPath;
        }
        return prefPath;
    }

    Log.info("Renamed legacy data directory '{}' to '{}'", io::fs_path_to_string(legacyPath),
        io::fs_path_to_string(prefPath));
    return prefPath;
}

bool is_same_or_inside(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto normalizedRoot = normalized_path(root);
    const auto normalizedPath = normalized_path(path);
    const auto relativePath = normalizedPath.lexically_relative(normalizedRoot);
    if (relativePath.empty()) {
        return normalizedPath == normalizedRoot;
    }
    if (relativePath == ".") {
        return true;
    }
    if (relativePath.is_absolute()) {
        return false;
    }

    const auto it = relativePath.begin();
    return it == relativePath.end() || *it != "..";
}

bool should_skip_migration_path(const std::filesystem::path& path,
    const std::filesystem::path& from, const std::filesystem::path& to, MigrationStats& stats) {
    if (is_same_or_inside(to, path)) {
        ++stats.skippedNestedTargets;
        return true;
    }

    const auto relativePath = path.lexically_relative(from);
    if (relativePath == kLocationDescriptorName) {
        ++stats.skippedDescriptorFiles;
        return true;
    }

    return false;
}

bool matches_name(std::string_view name, const auto& names) {
    return std::ranges::find(names, name) != names.end();
}

bool should_migrate_user_data_path(
    const std::filesystem::path& sourcePath, const std::filesystem::path& from) {
    const auto relativePath = sourcePath.lexically_relative(from);
    if (relativePath.empty() || relativePath.is_absolute()) {
        return false;
    }

    auto it = relativePath.begin();
    if (it == relativePath.end() || *it == "..") {
        return false;
    }

    const auto first = io::fs_path_to_string(*it);
    if (matches_name(first, kUserDataDirectories)) {
        return true;
    }

    ++it;
    if (it != relativePath.end()) {
        return false;
    }

    const auto filename = io::fs_path_to_string(relativePath.filename());
    if (matches_name(filename, kUserDataFiles)) {
        return true;
    }

    return relativePath.extension() == ".controller" || relativePath.extension() == ".gci" ||
           (filename.starts_with("MemoryCard") && filename.ends_with(".raw"));
}

std::filesystem::path current_data_path() {
    if (!ConfigPath.empty()) {
        return ConfigPath;
    }
    const auto prefPath = active_pref_path();
    const auto descriptor = read_location_descriptor(prefPath);
    if (descriptor) {
        sActiveDescriptorPath = descriptor->path;
    }
    return resolve_data_path(prefPath, descriptor ? &descriptor->descriptor : nullptr);
}

std::vector<std::filesystem::path> descriptor_write_paths(const std::filesystem::path& prefPath) {
    if (sActiveDescriptorPath && !sActiveDescriptorPath->empty()) {
        return {*sActiveDescriptorPath};
    }

    std::vector<std::filesystem::path> paths;
#if defined(_WIN32)
    if (const auto basePath = base_path_relative(kLocationDescriptorName); !basePath.empty()) {
        paths.push_back(basePath);
    }
#endif
    paths.push_back(prefPath / kLocationDescriptorName);
    return paths;
}

bool write_descriptor_json(const std::filesystem::path& path, const nlohmann::json& json) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        Log.warn("Failed to create data location descriptor directory '{}': {}",
            io::fs_path_to_string(path.parent_path()), ec.message());
        return false;
    }
    try {
        io::FileStream::WriteAllText(path, json.dump(4));
    } catch (const std::exception& e) {
        Log.warn("Failed to write data location descriptor '{}': {}", io::fs_path_to_string(path),
            e.what());
        return false;
    }
    return true;
}

bool write_location_descriptor(LocationMode mode, const std::filesystem::path& targetPath) {
    LocationDescriptor descriptor;
    descriptor.mode = mode;
    if (mode == LocationMode::Custom) {
        descriptor.customPath = absolute_path(targetPath);
    }

    const auto currentPath = current_data_path();
    const auto resolvedTargetPath =
        mode == LocationMode::Custom ? descriptor.customPath : targetPath;
    if (!currentPath.empty() && normalized_path(currentPath) != normalized_path(resolvedTargetPath))
    {
        descriptor.previousPath = currentPath;
    }

    nlohmann::json json;
    json["version"] = 1;
    json["mode"] = location_mode_id(descriptor.mode);
    if (descriptor.mode == LocationMode::Custom && !descriptor.customPath.empty()) {
        json["customPath"] = io::fs_path_to_string(descriptor.customPath);
    }
    if (!descriptor.previousPath.empty()) {
        json["previousPath"] = io::fs_path_to_string(descriptor.previousPath);
    }

    const auto prefPath = active_pref_path();
    for (const auto& path : descriptor_write_paths(prefPath)) {
        if (write_descriptor_json(path, json)) {
            sActiveDescriptorPath = path;
            sConfiguredDataPath = resolvedTargetPath;
            return true;
        }
    }

    return false;
}

void set_error(std::string* errorOut, std::string error) {
    if (errorOut != nullptr) {
        *errorOut = std::move(error);
    }
}

bool validate_writable_data_path(const std::filesystem::path& path, std::string* errorOut) {
    if (path.empty()) {
        set_error(errorOut, "Choose a folder.");
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        set_error(errorOut, fmt::format("{} could not create the selected folder.", AppName));
        Log.warn("Failed to create custom data folder '{}': {}", io::fs_path_to_string(path),
            ec.message());
        return false;
    }

    if (!std::filesystem::is_directory(path, ec)) {
        set_error(errorOut, "The selected path is not a folder.");
        if (ec) {
            Log.warn("Failed to inspect custom data folder '{}': {}", io::fs_path_to_string(path),
                ec.message());
        }
        return false;
    }

    const auto probePath = path / fmt::format(".write-probe-{}.tmp",
                                      std::chrono::steady_clock::now().time_since_epoch().count());
    try {
        io::FileStream::WriteAllText(probePath, "dusk");
    } catch (const std::exception& e) {
#if defined(__ANDROID__)
        set_error(
            errorOut, fmt::format("{} could not write to the selected folder. On Android, allow "
                                  "\"All files access\" for Dusklight and try again.",
                          AppName));
#else
        set_error(errorOut, fmt::format("{} could not write to the selected folder.", AppName));
#endif
        Log.warn("Failed write probe for custom data folder '{}': {}", io::fs_path_to_string(path),
            e.what());
        return false;
    }

    std::filesystem::remove(probePath, ec);
    if (ec) {
        set_error(
            errorOut, fmt::format("{} could write to the selected folder, but could not remove "
                                  "the test file it created.",
                          AppName));
        Log.warn("Failed to remove custom data folder write probe '{}': {}",
            io::fs_path_to_string(probePath), ec.message());
        return false;
    }

    return true;
}

std::uintmax_t remove_empty_directories(const std::filesystem::path& root, bool includeRoot) {
    std::error_code ec;
    std::vector<std::filesystem::path> directories;
    for (std::filesystem::recursive_directory_iterator it(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
        it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec) {
            Log.warn("Failed to scan empty directories under '{}': {}", io::fs_path_to_string(root),
                ec.message());
            return 0;
        }
        const auto status = it->symlink_status(ec);
        if (ec) {
            Log.warn("Failed to inspect '{}' while pruning empty directories: {}",
                io::fs_path_to_string(it->path()), ec.message());
            ec.clear();
            continue;
        }
        if (std::filesystem::is_directory(status)) {
            directories.push_back(it->path());
        }
    }

    std::uintmax_t removed = 0;
    for (auto& dir : std::views::reverse(directories)) {
        if (!std::filesystem::is_empty(dir, ec)) {
            ec.clear();
            continue;
        }
        if (std::filesystem::remove(dir, ec)) {
            ++removed;
        } else if (ec) {
            Log.warn("Failed to remove empty migrated source directory '{}': {}",
                io::fs_path_to_string(dir), ec.message());
        }
        ec.clear();
    }

    if (includeRoot) {
        if (std::filesystem::is_empty(root, ec)) {
            if (std::filesystem::remove(root, ec)) {
                ++removed;
            } else if (ec) {
                Log.warn("Failed to remove empty migrated source root '{}': {}",
                    io::fs_path_to_string(root), ec.message());
            }
        }
        ec.clear();
    }

    return removed;
}

bool ensure_parent_directory(const std::filesystem::path& targetPath, MigrationStats& stats) {
    std::error_code ec;
    std::filesystem::create_directories(targetPath.parent_path(), ec);
    if (ec) {
        ++stats.failures;
        Log.warn("Failed to create migration target parent '{}': {}",
            io::fs_path_to_string(targetPath.parent_path()), ec.message());
        return false;
    }
    return true;
}

bool remove_migrated_source(const std::filesystem::path& sourcePath, MigrationStats& stats) {
    std::error_code ec;
    std::filesystem::remove(sourcePath, ec);
    if (ec) {
        ++stats.failures;
        Log.warn("Migrated '{}' but failed to remove source: {}", io::fs_path_to_string(sourcePath),
            ec.message());
        return false;
    }

    ++stats.sourcesRemoved;
    return true;
}

bool try_rename_migration_entry(
    const std::filesystem::path& sourcePath, const std::filesystem::path& targetPath) {
    std::error_code ec;
    if (std::filesystem::exists(targetPath, ec) || std::filesystem::is_symlink(targetPath, ec)) {
        return false;
    }
    ec.clear();

    if (!std::filesystem::exists(sourcePath, ec)) {
        return false;
    }
    ec.clear();

    std::filesystem::create_directories(targetPath.parent_path(), ec);
    if (ec) {
        Log.debug("Could not create migration target parent '{}' before rename: {}",
            io::fs_path_to_string(targetPath.parent_path()), ec.message());
        return false;
    }

    std::filesystem::rename(sourcePath, targetPath, ec);
    if (ec) {
        Log.debug("Could not rename migration entry '{}' to '{}': {}",
            io::fs_path_to_string(sourcePath), io::fs_path_to_string(targetPath), ec.message());
        return false;
    }

    return true;
}

void migrate_symlink(const std::filesystem::path& sourcePath,
    const std::filesystem::path& targetPath, MigrationStats& stats) {
    std::error_code ec;
    if (std::filesystem::exists(targetPath, ec) || std::filesystem::is_symlink(targetPath, ec)) {
        ++stats.skippedExistingTargets;
        return;
    }
    ec.clear();

    const auto linkTarget = std::filesystem::read_symlink(sourcePath, ec);
    if (ec) {
        ++stats.failures;
        Log.warn("Failed to read migration symlink '{}': {}", io::fs_path_to_string(sourcePath),
            ec.message());
        return;
    }

    if (!ensure_parent_directory(targetPath, stats)) {
        return;
    }

    const bool targetIsDirectory = std::filesystem::is_directory(sourcePath, ec);
    if (ec) {
        Log.debug("Could not resolve symlink target type for '{}': {}",
            io::fs_path_to_string(sourcePath), ec.message());
        ec.clear();
    }

    if (targetIsDirectory) {
        std::filesystem::create_directory_symlink(linkTarget, targetPath, ec);
    } else {
        std::filesystem::create_symlink(linkTarget, targetPath, ec);
    }
    if (ec) {
        ++stats.failures;
        Log.warn("Failed to migrate symlink '{}' -> '{}' to '{}': {}",
            io::fs_path_to_string(sourcePath), io::fs_path_to_string(linkTarget),
            io::fs_path_to_string(targetPath), ec.message());
        return;
    }

    ++stats.symlinksCopied;
    remove_migrated_source(sourcePath, stats);
}

void migrate_regular_file(const std::filesystem::path& sourcePath,
    const std::filesystem::path& targetPath, MigrationStats& stats) {
    std::error_code ec;
    if (std::filesystem::exists(targetPath, ec)) {
        ++stats.skippedExistingTargets;
        return;
    }
    ec.clear();

    if (try_rename_migration_entry(sourcePath, targetPath)) {
        ++stats.filesCopied;
        ++stats.sourcesRemoved;
        return;
    }

    if (!ensure_parent_directory(targetPath, stats)) {
        return;
    }

    std::filesystem::copy_file(
        sourcePath, targetPath, std::filesystem::copy_options::skip_existing, ec);
    if (ec) {
        ++stats.failures;
        Log.warn("Failed to migrate file '{}' to '{}': {}", io::fs_path_to_string(sourcePath),
            io::fs_path_to_string(targetPath), ec.message());
        return;
    }

    ++stats.filesCopied;
    remove_migrated_source(sourcePath, stats);
}

void migrate_directory(const std::filesystem::path& from, const std::filesystem::path& to,
    const std::filesystem::path& prefPath) {
    if (from.empty() || to.empty() || normalized_path(from) == normalized_path(to)) {
        Log.debug("Skipping data migration from '{}' to '{}'", io::fs_path_to_string(from),
            io::fs_path_to_string(to));
        return;
    }

    MigrationStats stats;

    std::error_code ec;
    if (!std::filesystem::exists(from, ec)) {
        if (ec) {
            Log.warn("Failed to inspect migration source '{}': {}", io::fs_path_to_string(from),
                ec.message());
        } else {
            Log.debug("Migration source '{}' does not exist", io::fs_path_to_string(from));
        }
        return;
    }

    std::filesystem::create_directories(to, ec);
    if (ec) {
        ++stats.failures;
        Log.warn("Failed to create data directory '{}' for migration: {}",
            io::fs_path_to_string(to), ec.message());
        return;
    }

    std::filesystem::recursive_directory_iterator it(
        from, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        Log.warn("Failed to begin migration scan for '{}': {}", io::fs_path_to_string(from),
            ec.message());
        return;
    }

    const std::filesystem::recursive_directory_iterator end;
    while (it != end) {
        if (ec) {
            ++stats.failures;
            Log.warn(
                "Migration scan error under '{}': {}", io::fs_path_to_string(from), ec.message());
            ec.clear();
        }

        const auto sourcePath = it->path();
        const auto status = it->symlink_status(ec);
        if (ec) {
            ++stats.failures;
            Log.warn("Failed to inspect migration source '{}': {}",
                io::fs_path_to_string(sourcePath), ec.message());
            ec.clear();
            it.increment(ec);
            continue;
        }

        if (should_skip_migration_path(sourcePath, from, to, stats)) {
            if (std::filesystem::is_directory(status)) {
                it.disable_recursion_pending();
            }
            ec.clear();
            it.increment(ec);
            continue;
        }

        if (!should_migrate_user_data_path(sourcePath, from)) {
            ++stats.skippedUnsupportedEntries;
            if (std::filesystem::is_directory(status)) {
                it.disable_recursion_pending();
            }
            ec.clear();
            it.increment(ec);
            continue;
        }

        const auto relativePath = sourcePath.lexically_relative(from);
        if (relativePath.empty() || relativePath.is_absolute()) {
            ++stats.failures;
            Log.warn("Failed to calculate migration relative path for '{}'",
                io::fs_path_to_string(sourcePath));
            it.increment(ec);
            continue;
        }

        const auto targetPath = to / relativePath;
        if (std::filesystem::is_symlink(status)) {
            migrate_symlink(sourcePath, targetPath, stats);
        } else if (std::filesystem::is_directory(status)) {
            if (try_rename_migration_entry(sourcePath, targetPath)) {
                ++stats.directoriesCreated;
                ++stats.sourcesRemoved;
                it.disable_recursion_pending();
            } else {
                std::filesystem::create_directories(targetPath, ec);
                if (ec) {
                    ++stats.failures;
                    Log.warn("Failed to create migration target directory '{}': {}",
                        io::fs_path_to_string(targetPath), ec.message());
                    ec.clear();
                    it.disable_recursion_pending();
                } else {
                    ++stats.directoriesCreated;
                }
            }
        } else if (std::filesystem::is_regular_file(status)) {
            migrate_regular_file(sourcePath, targetPath, stats);
        } else {
            ++stats.skippedUnsupportedEntries;
        }

        it.increment(ec);
    }

    const bool includeRoot = normalized_path(from) != normalized_path(prefPath);
    stats.emptyDirectoriesRemoved = remove_empty_directories(from, includeRoot);

    const bool migratedAnything = stats.filesCopied > 0 || stats.symlinksCopied > 0 ||
                                  stats.sourcesRemoved > 0 || stats.emptyDirectoriesRemoved > 0 ||
                                  stats.failures > 0;
    if (migratedAnything) {
        Log.info(
            "Finished data migration from '{}' to '{}': {} files copied, {} symlinks copied, {} "
            "sources removed, {} empty directories removed, {} existing targets skipped, {} "
            "descriptor files skipped, {} nested destination paths skipped, {} unsupported entries "
            "skipped, {} failures",
            io::fs_path_to_string(from), io::fs_path_to_string(to), stats.filesCopied,
            stats.symlinksCopied, stats.sourcesRemoved, stats.emptyDirectoriesRemoved,
            stats.skippedExistingTargets, stats.skippedDescriptorFiles, stats.skippedNestedTargets,
            stats.skippedUnsupportedEntries, stats.failures);
    }
}

void migrate_data(const std::filesystem::path& prefPath, const std::filesystem::path& dataPath,
    const LocationDescriptor* descriptor) {
    if (descriptor && !descriptor->previousPath.empty()) {
        migrate_directory(descriptor->previousPath, dataPath, prefPath);
    }
}

void ensure_data_directory(const std::filesystem::path& dataPath) {
    std::error_code ec;
    std::filesystem::create_directories(dataPath, ec);
    if (ec) {
        Log.fatal("Failed to create data directory '{}': {}", io::fs_path_to_string(dataPath),
            ec.message());
    }
}

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
 *   - An explicit --data-dir is never redirected. The caller said exactly where to put things, and
 *     silently moving them would break the harness in the confusing direction. It warns instead.
 */
constexpr std::string_view kDataLockFileName = ".dusklight.lock";

/// Give up after this many numbered directories rather than looping. Eight simultaneous copies of
/// Dusklight is far past any real use, and past it the honest answer is to run unlocked.
constexpr int kMaxInstanceDirectories = 8;

bool sIsSecondaryInstance = false;
std::filesystem::path sSecondaryInstancePath;

#if defined(_WIN32)

/// Held for the life of the process. Never closed on purpose — the OS closes it, including on a
/// crash, which is the entire argument for a handle lock over a pidfile.
void* sDataLockHandle = nullptr;

/// Returns true when this process now owns the directory. `o_conflict` distinguishes "someone else
/// has it" (try the next directory) from "could not lock for any other reason" (run anyway).
bool try_lock_data_directory(const std::filesystem::path& dataPath, bool& o_conflict) {
    o_conflict = false;
    const auto lockPath = dataPath / kDataLockFileName;

    HANDLE handle = CreateFileW(lockPath.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
        0 /* no sharing: this IS the lock */, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);

    if (handle != INVALID_HANDLE_VALUE) {
        sDataLockHandle = handle;
        return true;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ||
        error == ERROR_ACCESS_DENIED)
    {
        // ERROR_ACCESS_DENIED is included deliberately even though it is ambiguous — it is what an
        // antivirus or indexer holding the handle looks like, and treating it as a conflict costs a
        // numbered directory where treating it as success costs a shared memory card.
        o_conflict = true;
        return false;
    }

    return false;
}

#else

int sDataLockFd = -1;

bool try_lock_data_directory(const std::filesystem::path& dataPath, bool& o_conflict) {
    o_conflict = false;
    const auto lockPath = dataPath / kDataLockFileName;

    const int fd = ::open(lockPath.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return false;
    }

    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
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

/**
 * Give a freshly-created instance directory the first one's saves and settings.
 *
 * Copies, never moves or links: the whole point is that the original .gci is not touched. Only done
 * when the target does not already exist, so a second instance that has been used before continues
 * from where IT left off rather than being reset to the primary's progress every launch.
 */
void seed_instance_directory(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;

    const auto configSrc = from / "config.json";
    if (std::filesystem::exists(configSrc, ec)) {
        std::filesystem::copy_file(
            configSrc, to / "config.json", std::filesystem::copy_options::skip_existing, ec);
        if (ec) {
            Log.warn(
                "Could not copy config into '{}': {}", io::fs_path_to_string(to), ec.message());
            ec.clear();
        }
    }

    // Memory cards only — deliberately NOT kUserDataDirectories, which also lists
    // texture_replacements. That can be gigabytes, and duplicating it per instance would turn a
    // safety measure into a disk-filling one.
    for (const std::string_view region : {"USA", "EUR", "JAP"}) {
        const auto src = from / region;
        if (!std::filesystem::is_directory(src, ec)) {
            continue;
        }
        std::filesystem::copy(src, to / region,
            std::filesystem::copy_options::recursive | std::filesystem::copy_options::skip_existing,
            ec);
        if (ec) {
            Log.warn("Could not copy '{}' into '{}': {}", region, io::fs_path_to_string(to),
                ec.message());
            ec.clear();
        }
    }
}

/**
 * Take the lock, or move this process to a directory of its own. Returns the directory to actually
 * use, which is @p dataPath in the overwhelmingly common single-instance case.
 */
std::filesystem::path lock_or_relocate_data_directory(const std::filesystem::path& dataPath) {
    bool conflict = false;
    if (try_lock_data_directory(dataPath, conflict)) {
        return dataPath;
    }

    if (!conflict) {
        // Could not lock, and not because anyone else holds it. Run in the normal directory rather
        // than inventing a problem: an unlockable filesystem is not evidence of a second instance.
        Log.warn("Could not lock the data directory '{}' — continuing without a single-instance "
                 "guard. This is expected on some network and cloud-synced folders.",
            io::fs_path_to_string(dataPath));
        return dataPath;
    }

    for (int index = 2; index < 2 + kMaxInstanceDirectories; ++index) {
        const auto candidate = instance_directory(dataPath, index);

        std::error_code ec;
        const bool existed = std::filesystem::exists(candidate, ec);
        ensure_data_directory(candidate);

        bool candidateConflict = false;
        if (!try_lock_data_directory(candidate, candidateConflict)) {
            if (candidateConflict) {
                continue;  // A third instance already has this one.
            }
            Log.warn("Could not lock '{}' either — continuing there unguarded.",
                io::fs_path_to_string(candidate));
        }

        if (!existed) {
            seed_instance_directory(dataPath, candidate);
        }

        sIsSecondaryInstance = true;
        sSecondaryInstancePath = candidate;
        Log.warn("Another Dusklight is already using '{}', so this one is using '{}' instead. Its "
                 "saves were {} and anything saved from now on goes THERE, not to the original.",
            io::fs_path_to_string(dataPath), io::fs_path_to_string(candidate),
            existed ? "already there from a previous run" : "copied from the original");
        return candidate;
    }

    Log.warn("Every instance directory up to {} is in use — continuing in '{}' unguarded.",
        kMaxInstanceDirectories + 1, io::fs_path_to_string(dataPath));
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
 * Collect the memory cards under a data directory.
 *
 * Deliberately NOT a recursive_directory_iterator over the whole tree: in --data-dir mode the data
 * directory also holds caches and logs, and walking those to find a 32 KB file would be paying a
 * lot for nothing. Aurora lays cards out as <dataPath>/<region>/Card A (lib/dolphin/card.cpp:57),
 * so this walks exactly two levels and matches the "Card" prefix rather than hardcoding a region
 * list — a new region then needs no change here.
 */
std::vector<std::filesystem::path> find_memory_cards(const std::filesystem::path& dataPath) {
    std::vector<std::filesystem::path> cards;
    std::error_code ec;

    for (const auto& regionEntry : std::filesystem::directory_iterator(dataPath, ec)) {
        if (ec || !regionEntry.is_directory()) {
            continue;
        }
        if (regionEntry.path().filename() == kSaveBackupDirName) {
            continue;
        }

        std::error_code slotEc;
        for (const auto& slotEntry :
            std::filesystem::directory_iterator(regionEntry.path(), slotEc))
        {
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
                if (fileEc || !fileEntry.is_regular_file()) {
                    continue;
                }
                if (fileEntry.path().extension() == ".gci") {
                    cards.push_back(fileEntry.path());
                }
            }
        }
    }

    return cards;
}

void prune_save_backups(const std::filesystem::path& backupDir, const std::string& stem) {
    std::vector<std::filesystem::path> existing;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(backupDir, ec)) {
        if (ec || !entry.is_regular_file()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (name.starts_with(stem + ".") && entry.path().extension() == ".gci") {
            existing.push_back(entry.path());
        }
    }

    if (existing.size() <= kSaveBackupsKept) {
        return;
    }

    // Names carry a sortable timestamp, so lexicographic order is chronological order. Cheaper and
    // steadier than asking the filesystem for mtimes, which a copy or a sync client can rewrite.
    std::sort(existing.begin(), existing.end());
    for (std::size_t i = 0; i + kSaveBackupsKept < existing.size(); ++i) {
        std::error_code removeEc;
        std::filesystem::remove(existing[i], removeEc);
    }
}

/**
 * ★ Rolling memory card backup, taken once at startup before the game can write anything.
 *
 * Every failure mode in .claude/plan/13-save-safety.md — a torn write from two processes on one
 * data directory (P0), a crash midway through the five open/seek/write/close cycles a single save
 * costs (P1), a read-back verify that reports success when it failed (P4) — has the same
 * consequence without this: the file is gone. With it, the consequence is losing one session. That
 * asymmetry is why this is worth more than its size, and why it runs unconditionally rather than
 * behind a setting.
 *
 * Best-effort by construction. Every error is swallowed with at most a warning: a backup that
 * cannot be written must never be a reason the game will not start, which is the same principle
 * that makes the P0 lock a harder call than this one.
 */
void backup_memory_cards(const std::filesystem::path& dataPath) {
    const auto cards = find_memory_cards(dataPath);
    if (cards.empty()) {
        return;
    }

    const auto backupDir = dataPath / kSaveBackupDirName;
    std::error_code ec;
    std::filesystem::create_directories(backupDir, ec);
    if (ec) {
        Log.warn("Could not create save backup directory '{}': {} — continuing without backups",
            io::fs_path_to_string(backupDir), ec.message());
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
    if (std::strftime(stamp.data(), stamp.size(), "%Y%m%d-%H%M%S", &local) == 0) {
        return;
    }

    for (const auto& card : cards) {
        const auto stem = card.stem().string();

        // Skip a launch that changed nothing: without this, ten backups of an untouched save push
        // the one backup that mattered out of the window.
        bool alreadyHaveIt = false;
        std::error_code scanEc;
        for (const auto& entry : std::filesystem::directory_iterator(backupDir, scanEc)) {
            if (scanEc || !entry.is_regular_file()) {
                continue;
            }
            if (entry.path().filename().string().starts_with(stem + ".") &&
                files_have_same_contents(card, entry.path()))
            {
                alreadyHaveIt = true;
                break;
            }
        }
        if (alreadyHaveIt) {
            continue;
        }

        const auto target = backupDir / (stem + "." + stamp.data() + ".gci");
        std::error_code copyEc;
        std::filesystem::copy_file(
            card, target, std::filesystem::copy_options::overwrite_existing, copyEc);
        if (copyEc) {
            Log.warn("Could not back up memory card '{}': {}", io::fs_path_to_string(card),
                copyEc.message());
            continue;
        }

        Log.info("Backed up memory card '{}' to '{}'", io::fs_path_to_string(card),
            io::fs_path_to_string(target));
        prune_save_backups(backupDir, stem);
    }
}

}  // namespace

bool open_data_path() {
#if DUSK_CAN_OPEN_DATA_FOLDER
    std::error_code ec;
    std::filesystem::path path = std::filesystem::absolute(ConfigPath, ec);
    if (ec) {
        Log.warn("Failed to resolve absolute data folder path '{}': {}",
            io::fs_path_to_string(ConfigPath), ec.message());
        path = ConfigPath;
    }

#if defined(_WIN32)
    const std::string url = "file:///" + path.generic_string();
#else
    const std::string url = "file://" + path.generic_string();
#endif
    if (!SDL_OpenURL(url.c_str())) {
        Log.warn(
            "Failed to open data folder '{}': {}", io::fs_path_to_string(path), SDL_GetError());
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool set_custom_data_path(const std::filesystem::path& path, std::string* errorOut) {
    if (!validate_writable_data_path(path, errorOut)) {
        return false;
    }

    if (!write_location_descriptor(LocationMode::Custom, path)) {
        set_error(errorOut, fmt::format("{} could not save the data folder setting.", AppName));
        return false;
    }

    return true;
}

bool set_custom_data_path(const char* path, std::string* errorOut) {
    if (path == nullptr) {
        set_error(errorOut, "Choose a folder.");
        return false;
    }
    return set_custom_data_path(path_from_utf8(path), errorOut);
}

void set_data_path_override(const std::filesystem::path& path) {
    if (path.empty()) {
        return;
    }
    sDataPathOverride = path;
}

bool set_portable_data_path() {
    return write_location_descriptor(LocationMode::Portable, portable_data_path());
}

bool reset_data_path() {
    const auto prefPath = active_pref_path();
    return write_location_descriptor(LocationMode::Default, default_data_path(prefPath));
}

bool is_default_data_path() {
    const auto prefPath = active_pref_path();
    return normalized_path(configured_data_path()) == normalized_path(default_data_path(prefPath));
}

std::filesystem::path configured_data_path() {
    if (sConfiguredDataPath) {
        return *sConfiguredDataPath;
    }

    const auto prefPath = active_pref_path();
    const auto descriptor = read_location_descriptor(prefPath);
    if (descriptor) {
        sActiveDescriptorPath = descriptor->path;
    }
    sConfiguredDataPath =
        resolve_data_path(prefPath, descriptor ? &descriptor->descriptor : nullptr);
    return *sConfiguredDataPath;
}

std::filesystem::path cache_path() {
    if (!CachePath.empty()) {
        return CachePath;
    }
    return active_pref_path();
}

bool is_secondary_instance() {
    return sIsSecondaryInstance;
}

std::filesystem::path secondary_instance_path() {
    return sSecondaryInstancePath;
}

bool is_data_path_restart_pending() {
    if (ConfigPath.empty()) {
        return false;
    }

    return normalized_path(ConfigPath) != normalized_path(configured_data_path());
}

Paths initialize_data() {
    if (sDataPathOverride) {
        // Everything — save files, config, logs and caches — lands in the one directory, so a
        // second instance shares nothing with the first and can be deleted wholesale afterwards.
        sActivePrefPath = *sDataPathOverride;
        sConfiguredDataPath = *sDataPathOverride;
        sActiveDescriptorPath.reset();
        ensure_data_directory(*sDataPathOverride);
        // Locked but never relocated: --data-dir is an explicit instruction about where things go,
        // and silently moving them would break the two-instance harness in the most confusing
        // possible direction. A conflict here means someone pointed two processes at one sandbox,
        // which is worth a warning and is their business.
        {
            bool conflict = false;
            if (!try_lock_data_directory(*sDataPathOverride, conflict) && conflict) {
                Log.warn("Another Dusklight already holds '{}'. Honouring --data-dir anyway, but "
                         "the two processes now share one memory card and can corrupt it.",
                    io::fs_path_to_string(*sDataPathOverride));
            }
        }
        backup_memory_cards(*sDataPathOverride);

        return Paths{
            .userPath = *sDataPathOverride,
            .cachePath = *sDataPathOverride,
        };
    }

    const auto preferredPrefPath = get_pref_path();
    const auto prefPath =
        rename_legacy_pref_path(legacy_path_for_pref_path(preferredPrefPath), preferredPrefPath);
    sActivePrefPath = prefPath;

    const auto descriptor = read_location_descriptor(prefPath);
    if (descriptor) {
        sActiveDescriptorPath = descriptor->path;
    } else {
        sActiveDescriptorPath.reset();
    }
    const auto dataPath =
        resolve_data_path(prefPath, descriptor ? &descriptor->descriptor : nullptr);
    sConfiguredDataPath = dataPath;

    migrate_data(prefPath, dataPath, descriptor ? &descriptor->descriptor : nullptr);
    ensure_data_directory(dataPath);
    ensure_data_directory(prefPath);
    // ★ Claim the directory, or move to one of our own. AFTER the directory exists and any
    // migration has landed (there is nothing to lock or copy before that), and BEFORE the backup
    // and the first card write, so a second instance never opens the first one's .gci at all.
    const auto activePath = lock_or_relocate_data_directory(dataPath);
    if (activePath != dataPath) {
        sConfiguredDataPath = activePath;
    }

    // After the directory exists and any migration has landed, and before the game can write a
    // card.
    backup_memory_cards(activePath);

    return Paths{
        .userPath = activePath,
        .cachePath = prefPath,
    };
}

}  // namespace dusk::data
