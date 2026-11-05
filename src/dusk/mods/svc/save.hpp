#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace dusk::save_manager {
struct Result;
struct Storage;
}  // namespace dusk::save_manager

namespace dusk::mods::svc {

void save_slot_new(uint32_t slot);
void save_slot_loaded(uint32_t slot);
void save_slot_written(uint32_t slot);
void save_slot_copied(uint32_t fromSlot, uint32_t toSlot);
void save_slot_erased(uint32_t slot);
void save_no_slot();
void invalidate_save(std::string_view saveName);

/**
 * Read-only peek at another mod's blob in the CURRENT slot, for the multiplayer mod check
 * (multiplayer/session/mod_check.cpp), which compares a few named blobs (e.g. the randomizer's
 * seed) between host and guest. Memory only: it never loads a sidecar from disk.
 *
 * Returns false when no slot is active (title screen, file select) or its store is not loaded, so
 * the caller can tell "no save yet" from "this save has no such blob". On true, @p out holds the
 * blob, or is empty if the mod stored none under @p name.
 */
bool peek_current_slot_blob(
    std::string_view modId, std::string_view name, std::vector<uint8_t>& out);

// Migrates the old mod_saves.json to the new per-file, per-mod JSON.
save_manager::Result migrate_legacy_sidecar(
    const save_manager::Storage& storage, std::string_view maker, std::string_view game);

}  // namespace dusk::mods::svc
