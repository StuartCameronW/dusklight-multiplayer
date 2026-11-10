#include "map_names.hpp"

#include <dolphin/types.h>

#include <cstring>
#include <vector>

#include "map_loader_definitions.h"

namespace dusk {

const char* lookup_map_name(const char* mapFile) {
    if (!mapFile || mapFile[0] == '\0')
        return nullptr;
    for (const auto& region : gameRegions) {
        for (const auto& map : region.maps) {
            if (map.mapFile && strcmp(mapFile, map.mapFile) == 0) {
                return map.mapName;
            }
        }
    }
    return nullptr;
}

}  // namespace dusk
