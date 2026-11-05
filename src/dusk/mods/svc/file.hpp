#pragma once

namespace dusk::mods::svc {

// Test hooks for DUSK_MP_TEST_SAVE_GUARD_PROBE (multiplayer/session/save_guard.cpp). They drive the
// mod file API's own per-handle guard, without a mod: open a write handle on save storage while
// the guard is off, then write, flush and close it later and log whether each was refused.
void file_probe_open_stale_save_handle();
void file_probe_use_stale_save_handle();

}  // namespace dusk::mods::svc
