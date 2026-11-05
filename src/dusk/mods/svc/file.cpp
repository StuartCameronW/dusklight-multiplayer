#include "file.hpp"
#include "registry.hpp"

#include "internal.hpp"

#include <aurora/lib/window.hpp>
#include <borealis/file_select.hpp>
#include <borealis/io.hpp>
#include <borealis/log.hpp>
#include "dusk/config.hpp"
#include "dusk/main.h"
#include "dusk/mods/loader/loader.hpp"
#include "dusk/multiplayer/session/save_guard.hpp"
#include "dusk/utilities.hpp"
#include "mods/svc/file.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dusk::mods::svc {
namespace {

constexpr borealis::Log Log{"dusk::mods::file"};

static_assert(static_cast<int>(FILE_OPEN_READ) == static_cast<int>(borealis::io::File::Mode::Read));
static_assert(
    static_cast<int>(FILE_OPEN_TRUNCATE) == static_cast<int>(borealis::io::File::Mode::Truncate));
static_assert(
    static_cast<int>(FILE_OPEN_APPEND) == static_cast<int>(borealis::io::File::Mode::Append));

/**
 * An open mod file stream. The save guard is checked when a write handle is opened AND on every
 * write, flush and close, because a handle opened before this process joined a session would
 * otherwise keep writing into save storage after it.
 */
struct Stream {
    borealis::io::File file;
    /// Writable and under save storage. Decided once at open (resolving a location is not cheap),
    /// so the per-operation check is this bool plus the guard's two flag reads.
    bool saveStorage = false;
    /// Set by the first refused operation and never cleared. A guest join that is abandoned turns
    /// saving back on, but a file that has already lost writes must not carry on as if it had not.
    bool refused = false;
};

SlotMap<Stream> s_streams;
std::unordered_map<void*, LoadedMod*> s_buffers;
std::unordered_map<LoadedMod*, std::string> s_joinResults;
ConfigVar<std::string> s_pickerOverride{"file.pickerOverride", ""};

struct PendingPick {
    LoadedMod* owner = nullptr;
    FilePickFn callback = nullptr;
    void* userData = nullptr;
    std::optional<std::string> overrideLocation;
    std::optional<std::string> exportSource;
};

std::shared_ptr<PendingPick> s_pendingPick;

ModResult map_status(borealis::io::Status status) {
    switch (status) {
    case borealis::io::Status::Ok:
        return MOD_OK;
    case borealis::io::Status::NotFound:
        return MOD_UNAVAILABLE;
    case borealis::io::Status::Unsupported:
        return MOD_UNSUPPORTED;
    case borealis::io::Status::AlreadyExists:
        return MOD_CONFLICT;
    case borealis::io::Status::Failed:
    default:
        return MOD_ERROR;
    }
}

ModResult map_picker_status(borealis::file_select::Status status) {
    switch (status) {
    case borealis::file_select::Status::Selected:
        return MOD_OK;
    case borealis::file_select::Status::Canceled:
        return MOD_UNAVAILABLE;
    case borealis::file_select::Status::Unsupported:
        return MOD_UNSUPPORTED;
    case borealis::file_select::Status::Busy:
        return MOD_CONFLICT;
    case borealis::file_select::Status::Failed:
    default:
        return MOD_ERROR;
    }
}

void invoke_pick(const std::shared_ptr<PendingPick>& pending, ModResult status,
    const std::vector<std::string>& locations, const std::string& error) {
    if (s_pendingPick == pending) {
        s_pendingPick.reset();
    }
    auto* owner = pending->owner;
    const auto callback = pending->callback;
    if (owner == nullptr || callback == nullptr || !owner->active) {
        return;
    }

    std::vector<const char*> rawLocations;
    rawLocations.reserve(locations.size());
    for (const auto& location : locations) {
        rawLocations.push_back(location.c_str());
    }
    try {
        callback(owner->context.get(), status, rawLocations.empty() ? nullptr : rawLocations.data(),
            static_cast<uint32_t>(rawLocations.size()), error.c_str(), pending->userData);
    } catch (const std::exception& exception) {
        fail_mod(*owner, MOD_ERROR,
            std::string{"exception in file picker callback: "} + exception.what());
    } catch (...) {
        fail_mod(*owner, MOD_ERROR, "unknown exception in file picker callback");
    }
}

bool valid_pick_options(const FilePickOptions* options) {
    if (options == nullptr || options->struct_size < sizeof(FilePickOptions) ||
        (options->filter_count != 0 && options->filters == nullptr))
    {
        return false;
    }
    for (uint32_t i = 0; i < options->filter_count; ++i) {
        if (options->filters[i].name == nullptr || options->filters[i].pattern == nullptr) {
            return false;
        }
    }
    return true;
}

ModResult begin_pick(ModContext* context, const FilePickOptions* options, FilePickFn callback,
    void* userData, bool folder) {
    auto* mod = mod_from_context(context);
    if (mod == nullptr || callback == nullptr || !valid_pick_options(options)) {
        return MOD_INVALID_ARGUMENT;
    }
    if (s_pendingPick != nullptr || borealis::file_select::busy()) {
        return MOD_CONFLICT;
    }

    auto pending = std::make_shared<PendingPick>(PendingPick{
        .owner = mod,
        .callback = callback,
        .userData = userData,
    });
    const auto& overrideLocation = s_pickerOverride.getValue();
    if (!overrideLocation.empty()) {
        pending->overrideLocation = overrideLocation;
        s_pendingPick = std::move(pending);
        return MOD_OK;
    }

    s_pendingPick = pending;
    const std::string defaultLocation =
        options->default_location != nullptr ? options->default_location : "";
    if (folder) {
        borealis::file_select::open_folder(
            {
                .parentWindow = aurora::window::get_sdl_window(),
                .defaultLocation = defaultLocation,
            },
            [pending](borealis::file_select::Result result) {
                invoke_pick(
                    pending, map_picker_status(result.status), result.locations, result.message);
            });
        return MOD_OK;
    }

    std::vector<borealis::file_select::Filter> filters;
    filters.reserve(options->filter_count);
    for (uint32_t i = 0; i < options->filter_count; ++i) {
        filters.push_back({options->filters[i].name, options->filters[i].pattern});
    }
    borealis::file_select::open_file(
        {
            .parentWindow = aurora::window::get_sdl_window(),
            .filters = std::move(filters),
            .defaultLocation = defaultLocation,
        },
        [pending](borealis::file_select::Result result) {
            invoke_pick(
                pending, map_picker_status(result.status), result.locations, result.message);
        });
    return MOD_OK;
}

ModResult begin_export(ModContext* context, const char* sourceLocation, const char* suggestedName,
    FilePickFn callback, void* userData) {
    auto* mod = mod_from_context(context);
    const std::string_view name = suggestedName != nullptr ? suggestedName : "";
    if (mod == nullptr || sourceLocation == nullptr || callback == nullptr ||
        !utils::is_safe_path_component(name))
    {
        return MOD_INVALID_ARGUMENT;
    }
    if (s_pendingPick != nullptr || borealis::file_select::busy()) {
        return MOD_CONFLICT;
    }
    const auto available = borealis::io::check(sourceLocation);
    if (available != borealis::io::Status::Ok) {
        return map_status(available);
    }

    auto pending = std::make_shared<PendingPick>(PendingPick{
        .owner = mod,
        .callback = callback,
        .userData = userData,
    });
    const auto& overrideLocation = s_pickerOverride.getValue();
    if (!overrideLocation.empty()) {
        pending->overrideLocation = overrideLocation;
        pending->exportSource = sourceLocation;
        s_pendingPick = std::move(pending);
        return MOD_OK;
    }

    borealis::file_select::ExportOptions options{
        .parentWindow = aurora::window::get_sdl_window(),
        .sourceLocation = sourceLocation,
        .suggestedName = suggestedName,
    };
    s_pendingPick = pending;
    try {
        borealis::file_select::export_file(
            std::move(options), [pending](borealis::file_select::Result result) {
                invoke_pick(
                    pending, map_picker_status(result.status), result.locations, result.message);
            });
    } catch (...) {
        if (s_pendingPick == pending) {
            s_pendingPick.reset();
        }
        throw;
    }
    return MOD_OK;
}

ModResult file_pick_file(
    ModContext* context, const FilePickOptions* options, FilePickFn callback, void* userData) {
    return begin_pick(context, options, callback, userData, false);
}

ModResult file_pick_folder(
    ModContext* context, const FilePickOptions* options, FilePickFn callback, void* userData) {
    return begin_pick(context, options, callback, userData, true);
}

ModResult file_export_file(ModContext* context, const char* sourceLocation,
    const char* suggestedName, FilePickFn callback, void* userData) {
    try {
        return begin_export(context, sourceLocation, suggestedName, callback, userData);
    } catch (...) {
        return MOD_ERROR;
    }
}

ModResult file_display_name(
    ModContext* context, const char* location, char* buffer, uint32_t bufferSize) {
    if (mod_from_context(context) == nullptr || location == nullptr || buffer == nullptr ||
        bufferSize == 0)
    {
        return MOD_INVALID_ARGUMENT;
    }
    const std::string name = borealis::io::display_name(location);
    if (name.size() + 1 > bufferSize) {
        return MOD_INVALID_ARGUMENT;
    }
    std::memcpy(buffer, name.c_str(), name.size() + 1);
    return MOD_OK;
}

ModResult file_check(ModContext* context, const char* location) {
    if (mod_from_context(context) == nullptr || location == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    return map_status(borealis::io::check(location));
}

ModResult file_open(
    ModContext* context, const char* location, FileOpenMode mode, FileStreamHandle* outHandle) {
    if (outHandle != nullptr) {
        *outHandle = 0;
    }
    auto* mod = mod_from_context(context);
    if (mod == nullptr || location == nullptr || outHandle == nullptr || mode < FILE_OPEN_READ ||
        mode > FILE_OPEN_APPEND)
    {
        return MOD_INVALID_ARGUMENT;
    }
    try {
        const auto ioMode = static_cast<borealis::io::File::Mode>(mode);
        // Classified even while the guard is off, so the handle can be refused later if this
        // process joins a session while it is still open.
        const bool saveStorage = mode != FILE_OPEN_READ && mp::is_save_storage_location(location);
        if (mode != FILE_OPEN_READ) {
            if (saveStorage && mp::refuse_save_file_write("mod file API: open for writing")) {
                return MOD_UNAVAILABLE;
            }
            const auto available = borealis::io::check(location);
            if (available != borealis::io::Status::Ok) {
                return map_status(available);
            }
        }
        auto result = borealis::io::open(location, ioMode);
        if (result.status != borealis::io::Status::Ok) {
            Log.warn("[{}] open '{}' failed: {}", mod->metadata.id,
                borealis::io::display_name(location), result.message);
            return map_status(result.status);
        }
        *outHandle = s_streams.emplace(*mod, std::move(result.file), saveStorage);
        return MOD_OK;
    } catch (...) {
        return MOD_ERROR;
    }
}

/**
 * The per-operation half of the save guard. Cheap when it matters: a handle outside save storage
 * costs one bool, and one inside it two flag reads. Only the first refusal logs and toasts (through
 * refuse_save_file_write, the same as the open path), so a mod writing in a loop cannot flood the
 * log.
 */
bool refuse_stream_operation(Stream& stream, const char* what) {
    if (!stream.saveStorage) {
        return false;
    }
    if (stream.refused) {
        return true;
    }
    if (!mp::refuse_save_file_write(what)) {
        return false;
    }
    stream.refused = true;
    return true;
}

ModResult stream_write(Stream& stream, std::span<const std::byte> bytes) {
    if (refuse_stream_operation(stream, "mod file API: write on a handle opened earlier")) {
        return MOD_UNAVAILABLE;
    }
    return stream.file.write(bytes) ? MOD_OK : MOD_ERROR;
}

ModResult stream_flush(Stream& stream) {
    if (refuse_stream_operation(stream, "mod file API: flush on a handle opened earlier")) {
        return MOD_UNAVAILABLE;
    }
    return stream.file.flush() ? MOD_OK : MOD_ERROR;
}

/**
 * Close always releases the handle: refusing to close would leak it until the mod unloads. The
 * mod API has no temp-and-rename commit to withhold; a Truncate open already emptied the file and
 * every write since the guard engaged was dropped, so a refused close reports MOD_UNAVAILABLE to
 * say the file is not what the mod wrote. Any bytes the platform still buffers from writes accepted
 * BEFORE the guard engaged are flushed by the close (SDL offers no discard).
 */
ModResult stream_close(Stream& stream) {
    const bool refused =
        stream.file.writable() &&
        refuse_stream_operation(stream, "mod file API: close on a handle opened earlier");
    const bool closed = stream.file.close();
    if (refused) {
        return MOD_UNAVAILABLE;
    }
    return closed ? MOD_OK : MOD_ERROR;
}

Stream* find_stream(ModContext* context, FileStreamHandle handle) {
    auto* mod = mod_from_context(context);
    if (mod == nullptr) {
        return nullptr;
    }
    auto* entry = s_streams.find_owned(handle, *mod);
    return entry != nullptr ? &entry->value : nullptr;
}

ModResult file_size(ModContext* context, FileStreamHandle handle, uint64_t* outSize) {
    if (outSize == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    *outSize = 0;
    auto* stream = find_stream(context, handle);
    if (stream == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    *outSize = stream->file.size();
    return MOD_OK;
}

ModResult file_read(ModContext* context, FileStreamHandle handle, void* buffer, uint64_t length,
    uint64_t* outRead) {
    if (outRead == nullptr || (buffer == nullptr && length != 0)) {
        return MOD_INVALID_ARGUMENT;
    }
    *outRead = 0;
    auto* stream = find_stream(context, handle);
    if (stream == nullptr || stream->file.writable()) {
        return MOD_INVALID_ARGUMENT;
    }
    *outRead = stream->file.read(buffer, length);
    return stream->file.error().empty() ? MOD_OK : MOD_ERROR;
}

ModResult file_seek(ModContext* context, FileStreamHandle handle, uint64_t offset) {
    auto* stream = find_stream(context, handle);
    if (stream == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    return stream->file.seek(offset) ? MOD_OK : MOD_ERROR;
}

ModResult file_close(ModContext* context, FileStreamHandle handle) {
    auto* mod = mod_from_context(context);
    if (mod == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    auto entry = s_streams.take_owned(handle, *mod);
    if (!entry.has_value()) {
        return MOD_INVALID_ARGUMENT;
    }
    return stream_close(entry->value);
}

ModResult file_write(
    ModContext* context, FileStreamHandle handle, const void* buffer, uint64_t length) {
    if ((buffer == nullptr && length != 0) || length > std::numeric_limits<size_t>::max()) {
        return MOD_INVALID_ARGUMENT;
    }
    auto* stream = find_stream(context, handle);
    if (stream == nullptr || !stream->file.writable()) {
        return MOD_INVALID_ARGUMENT;
    }
    const auto bytes =
        std::span{static_cast<const std::byte*>(buffer), static_cast<size_t>(length)};
    return stream_write(*stream, bytes);
}

ModResult file_flush(ModContext* context, FileStreamHandle handle) {
    auto* stream = find_stream(context, handle);
    if (stream == nullptr || !stream->file.writable()) {
        return MOD_INVALID_ARGUMENT;
    }
    return stream_flush(*stream);
}

ModResult file_write_all(ModContext* context, const char* location, const void* data, size_t size) {
    if (mod_from_context(context) == nullptr || location == nullptr ||
        (data == nullptr && size != 0))
    {
        return MOD_INVALID_ARGUMENT;
    }
    try {
        if (mp::refuse_save_location_write(location, "mod file API: write_all")) {
            return MOD_UNAVAILABLE;
        }
        const auto available = borealis::io::check(location);
        if (available != borealis::io::Status::Ok) {
            return map_status(available);
        }
        auto opened = borealis::io::open(location, borealis::io::File::Mode::Truncate);
        if (opened.status != borealis::io::Status::Ok) {
            return map_status(opened.status);
        }
        if (!opened.file.write(std::span{static_cast<const std::byte*>(data), size})) {
            return MOD_ERROR;
        }
        return opened.file.close() ? MOD_OK : MOD_ERROR;
    } catch (...) {
        return MOD_ERROR;
    }
}

ModResult file_read_all(ModContext* context, const char* location, FileBuffer* outBuffer) {
    if (outBuffer == nullptr || outBuffer->struct_size < sizeof(FileBuffer)) {
        return MOD_INVALID_ARGUMENT;
    }
    outBuffer->data = nullptr;
    outBuffer->size = 0;
    auto* mod = mod_from_context(context);
    if (mod == nullptr || location == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    try {
        auto opened = borealis::io::open(location);
        if (opened.status != borealis::io::Status::Ok) {
            return map_status(opened.status);
        }
        const uint64_t expectedSize = opened.file.size();
        if (expectedSize > std::numeric_limits<size_t>::max()) {
            return MOD_ERROR;
        }
        std::vector<unsigned char> bytes;
        bytes.resize(static_cast<size_t>(expectedSize));
        uint64_t total = 0;
        while (total < expectedSize) {
            const uint64_t count = opened.file.read(bytes.data() + total, expectedSize - total);
            if (count == 0) {
                return MOD_ERROR;
            }
            total += count;
        }
        if (expectedSize == 0) {
            unsigned char chunk[64 * 1024];
            while (true) {
                const uint64_t count = opened.file.read(chunk, sizeof(chunk));
                if (count == 0) {
                    if (!opened.file.error().empty()) {
                        return MOD_ERROR;
                    }
                    break;
                }
                bytes.insert(bytes.end(), chunk, chunk + count);
            }
        }
        if (bytes.empty()) {
            return MOD_OK;
        }
        std::unique_ptr<void, decltype(&std::free)> data{std::malloc(bytes.size()), &std::free};
        if (!data) {
            return MOD_ERROR;
        }
        std::memcpy(data.get(), bytes.data(), bytes.size());
        s_buffers.emplace(data.get(), mod);
        outBuffer->data = data.release();
        outBuffer->size = bytes.size();
        return MOD_OK;
    } catch (...) {
        return MOD_ERROR;
    }
}

void file_free(ModContext* context, FileBuffer* buffer) {
    if (buffer == nullptr || buffer->struct_size < sizeof(FileBuffer) || buffer->data == nullptr) {
        return;
    }
    auto* mod = mod_from_context(context);
    const auto found = s_buffers.find(buffer->data);
    if (mod == nullptr || found == s_buffers.end() || found->second != mod) {
        Log.error("[{}] file free: buffer is not owned by this mod", mod_id_from_context(context));
        return;
    }
    s_buffers.erase(found);
    std::free(buffer->data);
    buffer->data = nullptr;
    buffer->size = 0;
}

ModResult file_list(
    ModContext* context, const char* folderLocation, FileListFn callback, void* userData) {
    auto* mod = mod_from_context(context);
    if (mod == nullptr || folderLocation == nullptr || callback == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    borealis::io::ListResult result;
    try {
        result = borealis::io::list(folderLocation);
    } catch (...) {
        return MOD_ERROR;
    }
    if (result.status != borealis::io::Status::Ok) {
        return map_status(result.status);
    }
    try {
        for (const auto& entry : result.entries) {
            const FileEntry raw{
                .name = entry.name.c_str(),
                .location = entry.location.c_str(),
                .is_directory = entry.isDirectory,
            };
            callback(mod->context.get(), &raw, userData);
            if (!mod->active) {
                return MOD_ERROR;
            }
        }
        callback(mod->context.get(), nullptr, userData);
    } catch (const std::exception& exception) {
        fail_mod(
            *mod, MOD_ERROR, std::string{"exception in file list callback: "} + exception.what());
        return MOD_ERROR;
    } catch (...) {
        fail_mod(*mod, MOD_ERROR, "unknown exception in file list callback");
        return MOD_ERROR;
    }
    return MOD_OK;
}

ModResult file_join(ModContext* context, const char* folderLocation, const char* relativePath,
    const char** outLocation) {
    if (outLocation != nullptr) {
        *outLocation = nullptr;
    }
    auto* mod = mod_from_context(context);
    if (mod == nullptr || folderLocation == nullptr || relativePath == nullptr ||
        outLocation == nullptr)
    {
        return MOD_INVALID_ARGUMENT;
    }
    auto result = borealis::io::join(folderLocation, relativePath);
    if (result.status != borealis::io::Status::Ok) {
        return map_status(result.status);
    }
    auto& saved = s_joinResults[mod];
    saved = std::move(result.location);
    *outLocation = saved.c_str();
    return MOD_OK;
}

ModResult file_create_child(
    ModContext* context, const char* folderLocation, const char* name, const char** outLocation) {
    if (outLocation != nullptr) {
        *outLocation = nullptr;
    }
    auto* mod = mod_from_context(context);
    if (mod == nullptr || folderLocation == nullptr || name == nullptr || outLocation == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    try {
        // A child of a save folder is save storage too, so the folder is the thing to check.
        if (mp::refuse_save_location_write(folderLocation, "mod file API: create_child")) {
            return MOD_UNAVAILABLE;
        }
        auto result = borealis::io::create_child(folderLocation, name);
        if (result.status != borealis::io::Status::Ok) {
            return map_status(result.status);
        }
        auto& saved = s_joinResults[mod];
        saved = std::move(result.location);
        *outLocation = saved.c_str();
        return MOD_OK;
    } catch (...) {
        return MOD_ERROR;
    }
}

void file_initialize() {
    config::Register(s_pickerOverride);
}

bool file_available() {
    const auto capabilities = borealis::file_select::capabilities();
    return capabilities.canOpenFile || capabilities.canOpenFolder || capabilities.canExportFile;
}

ModResult copy_picker_override(
    std::string_view source, std::string_view destination, std::string& error) {
    try {
        if (source == destination) {
            return MOD_OK;
        }
        if (mp::refuse_save_location_write(destination, "mod file API: export")) {
            error = "Saves can't be changed after joining a co-op session as a guest.";
            return MOD_UNAVAILABLE;
        }
        auto input = borealis::io::open(source);
        if (input.status != borealis::io::Status::Ok) {
            error = std::move(input.message);
            return map_status(input.status);
        }
        auto output = borealis::io::open(destination, borealis::io::File::Mode::Truncate);
        if (output.status != borealis::io::Status::Ok) {
            error = std::move(output.message);
            return map_status(output.status);
        }
        std::array<std::byte, 64 * 1024> buffer{};
        while (true) {
            const uint64_t read = input.file.read(buffer.data(), buffer.size());
            if (read == 0) {
                if (!input.file.error().empty()) {
                    error = input.file.error();
                    return MOD_ERROR;
                }
                break;
            }
            if (!output.file.write(std::span{buffer.data(), static_cast<size_t>(read)})) {
                error = output.file.error();
                return MOD_ERROR;
            }
        }
        if (!output.file.close()) {
            error = output.file.error();
            return MOD_ERROR;
        }
        return MOD_OK;
    } catch (const std::exception& exception) {
        error = exception.what();
        return MOD_ERROR;
    } catch (...) {
        error = "Unable to copy export source";
        return MOD_ERROR;
    }
}

void file_frame_begin() {
    auto pending = s_pendingPick;
    if (pending == nullptr || !pending->overrideLocation.has_value()) {
        return;
    }
    if (!pending->exportSource.has_value()) {
        invoke_pick(pending, MOD_OK, {*pending->overrideLocation}, "");
        return;
    }
    std::string error;
    const ModResult result =
        copy_picker_override(*pending->exportSource, *pending->overrideLocation, error);
    invoke_pick(pending, result,
        result == MOD_OK ? std::vector<std::string>{*pending->overrideLocation} :
                           std::vector<std::string>{},
        error);
}

void file_remove_mod(LoadedMod& mod) {
    const size_t streams = s_streams.erase_all(mod);
    size_t buffers = 0;
    std::erase_if(s_buffers, [&](const auto& entry) {
        if (entry.second != &mod) {
            return false;
        }
        std::free(entry.first);
        ++buffers;
        return true;
    });
    s_joinResults.erase(&mod);
    if (s_pendingPick != nullptr && s_pendingPick->owner == &mod) {
        s_pendingPick->owner = nullptr;
        s_pendingPick->callback = nullptr;
        s_pendingPick.reset();
    }
    if (streams != 0 || buffers != 0) {
        Log.warn("[{}] reclaimed {} open stream(s) and {} file buffer(s)", mod.metadata.id, streams,
            buffers);
    }
}

void file_shutdown() {
    config::unregister(s_pickerOverride);
    s_pendingPick.reset();
}

constexpr FileService s_fileService{
    .header = SERVICE_HEADER(FileService, FILE_SERVICE_MAJOR, FILE_SERVICE_MINOR),
    .pick_file = file_pick_file,
    .pick_folder = file_pick_folder,
    .export_file = file_export_file,
    .display_name = file_display_name,
    .check = file_check,
    .open = file_open,
    .size = file_size,
    .read = file_read,
    .write = file_write,
    .seek = file_seek,
    .flush = file_flush,
    .close = file_close,
    .read_all = file_read_all,
    .write_all = file_write_all,
    .free = file_free,
    .list = file_list,
    .join = file_join,
    .create_child = file_create_child,
};

// DUSK_MP_TEST_SAVE_GUARD_PROBE only. A stray file in the data root that is_save_storage_path
// counts as save storage (the "MemoryCard" prefix) but that the card code never opens.
std::optional<Stream> s_probeStream;

std::filesystem::path probe_stream_path() {
    return dusk::ConfigPath / "MemoryCardProbe.stale-handle.bin";
}

const char* probe_verdict(ModResult result) {
    return result == MOD_OK ? "ALLOWED" : result == MOD_UNAVAILABLE ? "REFUSED" : "FAILED";
}

}  // namespace

void file_probe_open_stale_save_handle() {
    const std::string location = borealis::io::fs_path_to_string(probe_stream_path());
    const bool saveStorage = mp::is_save_storage_location(location);
    auto opened = borealis::io::open(location, borealis::io::File::Mode::Truncate);
    if (opened.status != borealis::io::Status::Ok) {
        Log.warn("SAVE-GUARD PROBE: stale handle: open failed ({})", opened.message);
        return;
    }
    s_probeStream.emplace(Stream{std::move(opened.file), saveStorage});
    // Both must read as below for the later write to test anything.
    Log.info("SAVE-GUARD PROBE: stale handle opened, save storage {}, guard {}",
        saveStorage ? "yes" : "NO (probe invalid)",
        mp::saving_disabled() ? "ENGAGED (probe invalid)" : "not engaged");
}

void file_probe_use_stale_save_handle() {
    if (!s_probeStream.has_value()) {
        Log.info("SAVE-GUARD PROBE: stale handle was never opened");
        return;
    }
    const std::array<std::byte, 4> bytes{
        std::byte{'P'}, std::byte{'R'}, std::byte{'O'}, std::byte{'B'}};
    Log.info("SAVE-GUARD PROBE: stale handle write -> {}",
        probe_verdict(stream_write(*s_probeStream, bytes)));
    Log.info(
        "SAVE-GUARD PROBE: stale handle flush -> {}", probe_verdict(stream_flush(*s_probeStream)));
    Log.info(
        "SAVE-GUARD PROBE: stale handle close -> {}", probe_verdict(stream_close(*s_probeStream)));
    s_probeStream.reset();

    // The second witness: what reached the disk. 0 bytes on a guest, 4 on the host.
    std::error_code error;
    const auto path = probe_stream_path();
    const auto size = std::filesystem::file_size(path, error);
    Log.info("SAVE-GUARD PROBE: stale handle file holds {} byte(s) on disk ({})",
        error ? std::uintmax_t{0} : size,
        error ? error.message() : "0 = nothing written after the guard engaged");
    // Test-only cleanup of the probe's own artifact, so it is not carried into startup backups.
    std::filesystem::remove(path, error);
}

constinit const ServiceModule g_fileModule{
    .id = FILE_SERVICE_ID,
    .majorVersion = FILE_SERVICE_MAJOR,
    .minorVersion = FILE_SERVICE_MINOR,
    .service = &s_fileService,
    .available = file_available,
    .initialize = file_initialize,
    .modDetached = file_remove_mod,
    .frameBegin = file_frame_begin,
    .shutdown = file_shutdown,
};

}  // namespace dusk::mods::svc
