#include "dusk/multiplayer.hpp"

#include "../session/network_manager.hpp"

/**
 * The thin glue the decomp core loop calls into.
 *
 * Keeping this separate from NetworkManager means f_ap_game.cpp only ever sees the tiny
 * dependency-free include/dusk/multiplayer.hpp — no ENet, no STL containers, nothing that could
 * bleed into decomp translation units. Keep this file trivial.
 */

namespace dusk::mp {

void apply_startup_options(const StartupOptions& options) {
    network_manager().set_startup_options(options);
}

void pre_actor_tick() {
    network_manager().pre_actor_tick();
}

void post_actor_tick() {
    network_manager().post_actor_tick();
}

bool is_active() {
    return network_manager().is_active();
}

}  // namespace dusk::mp
