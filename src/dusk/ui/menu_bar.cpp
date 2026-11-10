#include "menu_bar.hpp"

#include "achievements.hpp"
#include "editor.hpp"
#include "mod_updates.hpp"
#include "modal.hpp"
#include "mods_window.hpp"
#include "pane.hpp"
#include "prelaunch.hpp"
#include "settings.hpp"
#include "ui.hpp"
#include "warp.hpp"
#include "window.hpp"

#include "dusk/config.hpp"
#include "dusk/game_mode.hpp"
#include "dusk/livesplit.h"
#include "dusk/main.h"
#include "dusk/mods/svc/ui.hpp"
#include "dusk/multiplayer/session/network_manager.hpp"
#include "dusk/multiplayer/session/presence.hpp"
#include "dusk/settings.h"
#include "dusk/speedrun.h"

#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "m_Do/m_Do_audio.h"

#include <RmlUi/Core.h>
#include <aurora/rmlui.hpp>
#include <fmt/format.h>
#include <imgui.h>

#include <cmath>

namespace dusk::ui {
namespace {

const Rml::String kDocumentSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/theme.rcss" />
    <link type="text/rcss" href="res/rml/mod_common.rcss" />
    <link type="text/rcss" href="res/rml/tabbing.rcss" />
    <link type="text/rcss" href="res/rml/popup.rcss" />
</head>
<body>
    <popup id="popup" />
</body>
</rml>
)RML";

/**
 * The co-op session at a glance, and the way out of it.
 *
 * Always in the menu, because the tab bar is built once at startup and a session can open later.
 * Outside a session it says so, and why the last one ended, which is the question a player who
 * just got dropped is actually asking.
 *
 * Rebuilt only when the SHAPE of the session changes (someone joins or leaves, the handshake
 * lands); RTT and "not responding" are rewritten in place every frame, so the page stays live
 * without the Leave button losing focus under the player's thumb once a second.
 */
class MultiplayerWindow : public Window {
public:
    MultiplayerWindow() {
        add_tab("Session", [this](Rml::Element* content) { build(content); });
    }

    void update() override {
        const mp::SessionStatus status = mp::network_manager().status();
        const std::string key = shape_of(status);
        if (key != mShape) {
            mShape = key;
            refresh_active_tab();
        } else {
            update_live(status);
        }
        Window::update();
    }

private:
    static std::string shape_of(const mp::SessionStatus& s) {
        std::string key = fmt::format("{}|{}|{}|{}|{}", static_cast<int>(s.role), s.connected,
            s.localPlayerId, s.endpoint, s.lastEndMessage);
        for (const auto& p : s.players) {
            key +=
                fmt::format("|{}:{}:{}:{}", p.playerId, p.handshakeComplete, p.color, p.nickname);
        }
        return key;
    }

    static std::string colored(const std::string& text, std::uint32_t rgb) {
        return fmt::format(
            R"(<span style="color: #{:06X};">{}</span>)", rgb & 0xFFFFFFu, escape(text));
    }

    static std::string live_text(const mp::PlayerStatus& p) {
        if (!p.direct) {
            return "via the host";
        }
        // Well before the transport's own 8-12 s give-up: a player who has gone quiet for a couple
        // of seconds is worth flagging, and is usually a hitch rather than a departure.
        if (p.silentMs >= 2000) {
            return fmt::format("not responding ({} s)", p.silentMs / 1000);
        }
        if (!p.handshakeComplete) {
            return "connecting…";
        }
        return p.rttMs != 0 ? fmt::format("{} ms", p.rttMs) : "measuring…";
    }

    void update_live(const mp::SessionStatus& status) {
        for (std::size_t i = 0; i < mLive.size() && i < status.players.size(); ++i) {
            const std::string text = escape(live_text(status.players[i]));
            if (mLive[i]->GetInnerRML() != text) {
                mLive[i]->SetInnerRML(text);
            }
        }
        update_presence();
    }

    /// Where each player is, and whether "Join" would be allowed. Presence changes never change the
    /// session's shape, so this is rewritten in place like the RTT.
    void update_presence() {
        for (auto& row : mPresence) {
            const std::string where = escape(mp::presence::describe(row.playerId));
            if (row.where->GetInnerRML() != where) {
                row.where->SetInnerRML(where);
            }
            const char* blocker = mp::presence::join_partner_blocker(row.playerId);
            const std::string tooltip = blocker != nullptr ? blocker : "";
            if (row.join != nullptr && tooltip != row.tooltip) {
                row.tooltip = tooltip;
                row.join->set_tooltip(tooltip);
            }
        }
    }

    static Rml::String friendly_fire_label() {
        return getSettings().multiplayer.friendlyFire.getValue() ? "Friendly fire: On" :
                                                                    "Friendly fire: Off";
    }

    /// Each player's own switch: it decides whether OTHER players' swords can hurt them.
    static void add_friendly_fire_toggle(Pane& pane) {
        auto& button = pane.add_button(friendly_fire_label());
        auto* buttonPtr = &button;
        button.on_pressed([buttonPtr] {
            mDoAud_seStartMenu(kSoundClick);
            auto& var = getSettings().multiplayer.friendlyFire;
            var.setValue(!var.getValue());
            config::save();
            buttonPtr->set_text(friendly_fire_label());
        });
        pane.add_text("When on, other players' sword attacks can hurt you.");
    }

    void build(Rml::Element* content) {
        mLive.clear();
        mPresence.clear();
        const mp::SessionStatus s = mp::network_manager().status();
        auto& pane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        if (s.role == mp::Role::Inactive) {
            pane.add_section("Co-op");
            pane.add_text("Not in a co-op session.");
            if (!s.lastEndMessage.empty()) {
                pane.add_text(s.lastEndMessage);
            }
            pane.add_section("Options");
            add_friendly_fire_toggle(pane);
            return;
        }

        pane.add_section("This game");
        if (s.role == mp::Role::Host) {
            pane.add_text(fmt::format("Hosting on UDP {}", s.endpoint));
        } else if (s.connected) {
            pane.add_text(fmt::format("Connected to {}", s.endpoint));
        } else {
            pane.add_text(fmt::format("Connecting to {}…", s.endpoint));
        }
        pane.add_rml(fmt::format(
            "You: {} (player {})", colored(s.localNickname, s.localColor), s.localPlayerId));

        pane.add_section(fmt::format("Other players ({})", s.players.size()));
        if (s.players.empty()) {
            pane.add_text(s.role == mp::Role::Host ? "Nobody has joined yet." : "Nobody else.");
        }
        for (const auto& p : s.players) {
            const char* tag =
                p.playerId == mp::kHostPlayerId && s.role == mp::Role::Client ? " — host" : "";
            const std::string id =
                p.handshakeComplete ? fmt::format(" (player {})", p.playerId) : "";
            auto* row = pane.add_rml(fmt::format(
                "{}{}{} · <span class=\"mp-live\"></span>", colored(p.nickname, p.color), tag, id));
            Rml::ElementList found;
            row->GetElementsByClassName(found, "mp-live");
            Rml::Element* live = found.empty() ? row : found.front();
            live->SetInnerRML(escape(live_text(p)));
            mLive.push_back(live);

            // Spec I.3/I.4 (mp-m4-m5-world.md): where they are, and the join-partner warp.
            if (!p.handshakeComplete) {
                continue;
            }
            PresenceRow presenceRow;
            presenceRow.playerId = p.playerId;
            presenceRow.where = pane.add_text(mp::presence::describe(p.playerId));
            const std::uint32_t playerId = p.playerId;
            auto& join = pane.add_button({
                .text = fmt::format("Join {}", p.nickname),
                .isDisabled =
                    [playerId] { return mp::presence::join_partner_blocker(playerId) != nullptr; },
            });
            join.on_pressed([playerId] {
                // Re-checked inside: the state can change between the frame that enabled the
                // button and the press.
                if (mp::presence::join_partner(playerId)) {
                    mDoAud_seStartMenu(kSoundClick);
                }
            });
            presenceRow.join = &join;
            mPresence.push_back(std::move(presenceRow));
        }
        update_presence();

        pane.add_section("Options");
        add_friendly_fire_toggle(pane);

        pane.add_section("Actions");
        const bool isHost = s.role == mp::Role::Host;
        const Rml::String label = isHost ? "End session for everyone" : "Leave session";
        auto& leave = pane.add_button(label);
        auto* leavePtr = &leave;
        auto confirming = std::make_shared<bool>(false);
        leave.on_nav_command([leavePtr, confirming, label](Rml::Event&, NavCommand cmd) {
            if (cmd == NavCommand::Confirm) {
                if (*confirming) {
                    mDoAud_seStartMenu(kSoundClick);
                    *confirming = false;
                    // Deferred to the next sim tick inside the session; see leave_session().
                    mp::network_manager().leave_session();
                } else {
                    *confirming = true;
                    leavePtr->set_text("Are you sure?");
                }
                return true;
            }
            if (cmd == NavCommand::Cancel && *confirming) {
                *confirming = false;
                leavePtr->set_text(label);
                return true;
            }
            return false;
        });
        leave.listen(Rml::EventId::Blur, [leavePtr, confirming, label](Rml::Event&) {
            *confirming = false;
            leavePtr->set_text(label);
        });
        if (!isHost) {
            pane.add_text("Saving stays off after you leave, until Dusklight is restarted: the "
                          "world in memory is still the host's.");
        }
    }

    std::string mShape;
    /// One live-text element per entry of status().players, in the same order.
    std::vector<Rml::Element*> mLive;

    /// One per admitted remote player: their location line and their Join button.
    struct PresenceRow {
        std::uint32_t playerId = 0;
        Rml::Element* where = nullptr;
        Component* join = nullptr;
        /// What the Join button's tooltip says now, so it is only rebuilt on a change.
        std::string tooltip;
    };
    std::vector<PresenceRow> mPresence;
};

}  // namespace

MenuBar::MenuBar()
    : Document(kDocumentSource, false, DocumentScope::MenuBar),
      mRoot(mDocument->GetElementById("popup")) {
    mTabBar = std::make_unique<TabBar>(mRoot, TabBar::Props{
                                                  .onClose =
                                                      [this] {
                                                          mDoAud_seStartMenu(kSoundMenuClose);
                                                          hide(false);
                                                      },
                                                  .autoSelect = false,
                                              });

    // Hide document after transition completion
    listen(mRoot, Rml::EventId::Transitionend, [this](Rml::Event& event) {
        if (event.GetTargetElement() == mRoot && !mRoot->HasAttribute("open") &&
            Document::visible())
        {
            Document::hide(mPendingClose);
        }
    });

    build_tabs();
}

void MenuBar::build_tabs() {
    mTabBar->add_tab("Settings", [this] { push(std::make_unique<SettingsWindow>()); });

    if (getSettings().backend.enableAdvancedSettings) {
        mTabBar->add_tab("Warp", [this] { push(std::make_unique<WarpWindow>()); });
        mTabBar->add_tab("Editor", [this] { push(std::make_unique<EditorWindow>()); });
    }

    // Only allow us to access achievements if we are playing on a game mode that uses them
    if (gamemode::getGameModeManager().isCurrentGameMode(gamemode::kVanillaGameModeId) ||
        gamemode::getGameModeManager().isCurrentGameMode(speedrun::kSpeedrunGameModeId))
    {
        mTabBar->add_tab("Achievements", [this] { push(std::make_unique<AchievementsWindow>()); });
    }
    mTabBar->add_tab("Multiplayer", [this] { push(std::make_unique<MultiplayerWindow>()); });
    mModsButton = &mTabBar->add_tab("Mods", [this] { push(std::make_unique<ModsWindow>()); });
    for (auto& tab : mods::svc::ui_mod_menu_tabs()) {
        mTabBar->add_tab(tab.label, std::move(tab.onSelected));
    }

    mTabBar->add_tab("Reset", [this] {
        mTabBar->set_active_tab(-1);
        const auto dismiss = [](Modal& modal) { modal.pop(); };
        push(std::make_unique<Modal>(Modal::Props{
            .title = "Reset Game",
            .bodyRml = "Unsaved progress will be lost.<br/>"
                       "<modal-tip>Tip: You can also reset by holding Start+X+B</modal-tip>",
            .actions =
                {
                    ModalAction{
                        .label = "Cancel",
                        .onPressed =
                            [this, dismiss](Modal& modal) {
                                mDoAud_seStartMenu(kSoundWindowClose);
                                dismiss(modal);
                            },
                    },
                    ModalAction{
                        .label = "Reset",
                        .onPressed =
                            [this, dismiss](Modal& modal) {
                                mDoAud_seStartMenu(kSoundClick);
                                if (fpcM_SearchByName(fpcNm_LOGO_SCENE_e)) {
                                    dismiss(modal);
                                    return;
                                }
                                dismiss(modal);
                                if (gamemode::getGameModeManager().getRegisteredGameModes().size() >
                                    1) {
                                    // If game modes are registered, return to prelaunch on reset.
                                    prelaunch_state().returnToPrelaunchOnReset = true;
                                }
                                hide(false);
                                JUTGamePad::C3ButtonReset::sResetSwitchPushing = true;
                            },
                    },
                },
            .onDismiss = dismiss,
            .icon = "question-mark",
        }));
    });
    mTabBar->add_tab("Quit", [this] {
        mTabBar->set_active_tab(-1);
        const auto dismiss = [](Modal& modal) { modal.pop(); };
        push(std::make_unique<Modal>(Modal::Props{
            .title = "Quit Dusklight",
            .bodyText = "Unsaved progress will be lost.",
            .actions =
                {
                    ModalAction{
                        .label = "Cancel",
                        .onPressed =
                            [dismiss](Modal& modal) {
                                mDoAud_seStartMenu(kSoundWindowClose);
                                dismiss(modal);
                            },
                    },
                    ModalAction{
                        .label = "Quit",
                        .onPressed =
                            [dismiss](Modal& modal) {
                                mDoAud_seStartMenu(kSoundClick);
                                dismiss(modal);
                                IsRunning = false;
                            },
                    },
                },
            .onDismiss = dismiss,
            .icon = "question-mark",
        }));
    });

    if (speedrun::isActive()) {
        mTabBar->add_tab("Reset Run", [this] {
            mTabBar->set_active_tab(-1);
            mDoAud_seStartMenu(kSoundClick);
            speedrun::g_speedrunInfo.reset();
            speedrun::reset();
            JUTGamePad::C3ButtonReset::sResetSwitchPushing = true;
            hide(false);
        });
    }
}

void MenuBar::show() {
    Document::show();
    mRoot->SetAttribute("open", "");
    mTabBar->set_active_tab(-1);
    if (!mTabBar->focus_tab(mFocusedTabTitle)) {
        mTabBar->focus();
    }
}

void MenuBar::hide(bool close) {
    mFocusedTabTitle = mTabBar->focused_tab_title();
    mRoot->RemoveAttribute("open");
    if (close) {
        mPendingClose = true;
    }
}

void MenuBar::update() {
    if (mModsButton) {
        set_mod_update_badge(*mModsButton);
    }
    update_safe_area();
    Document::update();
}

void MenuBar::update_safe_area() noexcept {
    if (mDocument == nullptr || mTabBar == nullptr) {
        return;
    }

    // Avoid ImGui menu bar if shown
    if (const auto* viewport = ImGui::GetMainViewport();
        viewport != nullptr && mTopMargin != viewport->WorkPos.y)
    {
        mTopMargin = viewport->WorkPos.y;
        mRoot->SetProperty(Rml::PropertyId::MarginTop, Rml::Property(mTopMargin, Rml::Unit::DP));
    }

    Rml::Context* context = mDocument->GetContext();
    Insets safeInsets = safe_area_insets(context);
    safeInsets = {
        0.0f,
        std::round(safeInsets.right),
        0.0f,
        std::round(safeInsets.left),
    };
    if (safeInsets == mTabBarPadding) {
        return;
    }

    mTabBarPadding = safeInsets;
    auto* tabBar = mTabBar->root();
    tabBar->SetProperty(
        Rml::PropertyId::PaddingRight, Rml::Property(safeInsets.right, Rml::Unit::PX));
    tabBar->SetProperty(
        Rml::PropertyId::PaddingLeft, Rml::Property(safeInsets.left, Rml::Unit::PX));
    if (auto* close = tabBar->QuerySelector("close")) {
        close->SetProperty(Rml::PropertyId::Right,
            Rml::Property(safeInsets.right + 8.0f * context->GetDensityIndependentPixelRatio(),
                Rml::Unit::PX));
    }
}

bool MenuBar::visible() const {
    return mRoot->HasAttribute("open");
}

bool MenuBar::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    if (!getSettings().backend.wasPresetChosen) {
        return true;
    }
    if (cmd == NavCommand::Cancel && visible()) {
        mDoAud_seStartMenu(kSoundMenuClose);
        hide(false);
        return true;
    }
    return Document::handle_nav_command(event, cmd);
}

bool MenuBar::focus() {
    return mTabBar->focus();
}

void MenuBar::refresh_tabs() {
    auto* menuBar = static_cast<MenuBar*>(find_document(DocumentScope::MenuBar));
    if (menuBar == nullptr) {
        return;
    }
    const auto focusedTitle = menuBar->mTabBar->focused_tab_title();
    if (!focusedTitle.empty()) {
        menuBar->mFocusedTabTitle = focusedTitle;
    }
    menuBar->mTabBar->clear_tabs();
    menuBar->build_tabs();
    if (menuBar->visible() && !menuBar->mTabBar->focus_tab(menuBar->mFocusedTabTitle)) {
        menuBar->mTabBar->focus();
    }
}

}  // namespace dusk::ui
