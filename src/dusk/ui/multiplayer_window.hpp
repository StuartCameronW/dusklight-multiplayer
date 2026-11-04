#pragma once

#include "window.hpp"

#include "dusk/multiplayer.hpp"

#include <functional>

namespace dusk::ui {

/**
 * Pre-game co-op lobby: host a session, join one by address, and set the nickname, port and
 * colour the session layer will use. Direct IP / LAN only.
 *
 * The window does not open the session itself. It hands a filled-in StartupOptions to `launch`,
 * which belongs to the prelaunch menu and boots the disc exactly the way Play does. Connection
 * failures are reported by the session layer once the game is running, never here, so nothing in
 * this window can block on the network.
 */
class MultiplayerWindow : public Window {
public:
    /// Boots the disc with `options` applied. Returns false when it could not, for instance
    /// because no disc image is selected yet, in which case this window stays open. `self` is
    /// this window, so the launcher can close it before closing the prelaunch menu under it.
    using LaunchCallback = std::function<bool(Document& self, const mp::StartupOptions& options)>;

    explicit MultiplayerWindow(LaunchCallback launch);

    void update() override;

private:
    void start(bool host);

    LaunchCallback mLaunch;
};

}  // namespace dusk::ui
