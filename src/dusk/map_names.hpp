#pragma once

namespace dusk {

/// The player-facing name of a stage file ("F_SP103" -> "Ordon Village"), from the warp menu's
/// table (map_loader_definitions.h). nullptr for an empty, unknown or null name. The returned
/// string is static.
const char* lookup_map_name(const char* mapFile);

}  // namespace dusk
