#pragma once

#include "window.hpp"

#include "dusk/multiplayer.hpp"

#include <functional>
#include <string>

namespace dusk::ui {

/**
 * Pre-game co-op lobby: host a session, join one by join code or by address, and set the
 * nickname, port and colour the session layer will use.
 *
 * The join code itself only exists once the game is running and the session has registered with
 * the rendezvous, so it is shown on the in-game menu's Session page (menu_bar.cpp), with the
 * host's Allow / Deny prompt for each guest.
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
    enum class StartMode { Host, HostWithCode, JoinAddress, JoinCode };
    void start(StartMode mode);

    LaunchCallback mLaunch;
    /// The code typed into Join. Not saved: codes stop working when the host stops hosting.
    std::string mJoinCode;
};

}  // namespace dusk::ui
