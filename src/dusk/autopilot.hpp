#pragma once

#include <filesystem>

namespace dusk::autopilot {

/**
 * Scripted controller input, for driving the game without a human at the keyboard.
 *
 * Feeds aurora's virtual pad (the same one the touch controls use), so it works regardless of
 * which window has focus — two instances can be driven at once while the desktop is in use.
 *
 * Development aid only: nothing runs unless --autopilot was passed.
 */

/// Load a pad script. A malformed script logs and disables autopilot rather than failing the boot.
void load_script(const std::filesystem::path& path);

bool active();

/// Publish this tick's virtual pad state. Called from mDoCPd_c::read(), which is the one place
/// guaranteed to run immediately before JUTGamePad::read() picks the status up.
void tick();

}  // namespace dusk::autopilot
