#include "multiplayer_window.hpp"

#include "bool_button.hpp"
#include "dusk/config.hpp"
#include "dusk/settings.h"
#include "number_button.hpp"
#include "pane.hpp"
#include "prelaunch.hpp"
#include "string_button.hpp"
#include "ui.hpp"

#include "m_Do/m_Do_audio.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace dusk::ui {
namespace {

/// Matches what the wire format and the session layer are comfortable showing over a player.
constexpr int kMaxNicknameLength = 16;
/// "255.255.255.255:65535" is 21; leave room for a LAN host name.
constexpr int kMaxAddressLength = 64;
constexpr int kMinPort = 1;
constexpr int kMaxPort = 65535;

struct PaletteColor {
    const char* name;
    std::uint32_t rgb;
};

/// Same order and values as kPlayerColors in multiplayer/session/network_manager.cpp, which is
/// what the host hands out under "Automatic". Keep the two in step.
constexpr std::array<PaletteColor, 8> kPalette{{
    {"Blue", 0x4FC3F7},
    {"Red", 0xEF5350},
    {"Green", 0x66BB6A},
    {"Amber", 0xFFCA28},
    {"Purple", 0xAB47BC},
    {"Orange", 0xFF7043},
    {"Teal", 0x26A69A},
    {"Pink", 0xEC407A},
}};

std::string hex_color(std::uint32_t rgb) {
    return fmt::format("{:06X}", rgb & 0xFFFFFFu);
}

std::string color_label(const std::string& hex) {
    if (hex.empty()) {
        return "Automatic";
    }
    for (const auto& color : kPalette) {
        if (hex_color(color.rgb) == hex) {
            return color.name;
        }
    }
    // Set by hand in the config file, or by an older palette.
    return "#" + hex;
}

std::string trimmed(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

/// "host" or "host:port". ENet 1.3 is IPv4 only, so more than one colon (an IPv6 literal) is
/// refused here rather than producing a connect attempt that can only fail.
bool is_usable_address(const std::string& address) {
    if (address.empty()) {
        return false;
    }
    return std::count(address.begin(), address.end(), ':') <= 1 && address.front() != ':';
}

int configured_port() {
    return std::clamp(getSettings().multiplayer.port.getValue(), kMinPort, kMaxPort);
}

void add_help(Pane& pane, const char* text) {
    pane.clear();
    pane.add_text(text);
}

}  // namespace

MultiplayerWindow::MultiplayerWindow(LaunchCallback launch) : mLaunch(std::move(launch)) {
    add_tab("Multiplayer", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);
        auto& settings = getSettings().multiplayer;

        leftPane.add_section("Host");
        leftPane.register_control(
            leftPane.add_button("Host Game").on_pressed([this] { start(true); }), rightPane,
            [](Pane& pane) {
                pane.clear();
                pane.add_text("Start the game and wait for other players to join.");
                pane.add_text(fmt::format(
                    "Other players connect to this computer's IP address on port {} (UDP). "
                    "Players outside your local network need that port forwarded to you.",
                    configured_port()));
            });

        leftPane.add_section("Join");
        leftPane.register_control(
            leftPane.add_child<StringButton>(StringButton::Props{
                .key = "Address",
                .getValue =
                    [&settings] {
                        const auto& value = settings.lastJoinAddress.getValue();
                        return value.empty() ? Rml::String{"Not set"} : Rml::String{value};
                    },
                .setValue =
                    [&settings](Rml::String value) {
                        settings.lastJoinAddress.setValue(trimmed(value));
                        config::save();
                    },
                .maxLength = kMaxAddressLength,
            }),
            rightPane, [](Pane& pane) {
                add_help(pane,
                    "The host's address, as \"host\" or \"host:port\". Without a port, the port "
                    "in Options is used. IPv4 only.");
            });
        leftPane.register_control(leftPane
                                      .add_button({
                                          .text = "Join Game",
                                          .isDisabled =
                                              [&settings] {
                                                  return !is_usable_address(
                                                      settings.lastJoinAddress.getValue());
                                              },
                                      })
                                      .on_pressed([this] { start(false); }),
            rightPane, [](Pane& pane) {
                add_help(pane, "Start the game and connect to the host at the address above.");
            });

        leftPane.add_section("Options");
        leftPane.register_control(leftPane.add_child<StringButton>(StringButton::Props{
                                      .key = "Nickname",
                                      .getValue =
                                          [&settings] {
                                              const auto& value = settings.nickname.getValue();
                                              return value.empty() ? Rml::String{"Player"} :
                                                                     Rml::String{value};
                                          },
                                      .setValue =
                                          [&settings](Rml::String value) {
                                              settings.nickname.setValue(trimmed(value));
                                              config::save();
                                          },
                                      .maxLength = kMaxNicknameLength,
                                  }),
            rightPane, [](Pane& pane) { add_help(pane, "The name shown to the other players."); });
        leftPane.register_control(leftPane.add_child<NumberButton>(NumberButton::Props{
                                      .key = "Port",
                                      .getValue = [] { return configured_port(); },
                                      .setValue =
                                          [&settings](int value) {
                                              settings.port.setValue(
                                                  std::clamp(value, kMinPort, kMaxPort));
                                              config::save();
                                          },
                                      .min = kMinPort,
                                      .max = kMaxPort,
                                  }),
            rightPane, [](Pane& pane) {
                add_help(pane,
                    "The UDP port to host on, and to join on when the address does not include "
                    "one. Everyone in a session must agree on it.");
            });
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Colour",
                .getValue = [&settings] { return color_label(settings.color.getValue()); },
            }),
            rightPane, [&settings](Pane& pane) {
                pane.clear();
                pane.add_button(
                        {
                            .text = "Automatic",
                            .isSelected = [&settings] { return settings.color.getValue().empty(); },
                        })
                    .on_pressed([&settings] {
                        mDoAud_seStartMenu(kSoundItemChange);
                        settings.color.setValue("");
                        config::save();
                    });
                for (const auto& color : kPalette) {
                    const std::string hex = hex_color(color.rgb);
                    pane.add_button(
                            {
                                .text = color.name,
                                .isSelected = [&settings,
                                                  hex] { return settings.color.getValue() == hex; },
                            })
                        .on_pressed([&settings, hex] {
                            mDoAud_seStartMenu(kSoundItemChange);
                            settings.color.setValue(hex);
                            config::save();
                        });
                }
                pane.add_text("Automatic lets the host give every player a different colour.");
            });

        leftPane.register_control(
            leftPane.add_child<BoolButton>(BoolButton::Props{
                .key = "Friendly Fire",
                .getValue = [&settings] { return settings.friendlyFire.getValue(); },
                .setValue =
                    [&settings](bool value) {
                        settings.friendlyFire.setValue(value);
                        config::save();
                    },
            }),
            rightPane, [](Pane& pane) {
                add_help(pane,
                    "Lets other players' sword attacks hurt you. Each player chooses this for "
                    "themselves: turning it on only makes YOU hittable.");
            });

        leftPane.register_control(
            leftPane.add_button("Back").on_pressed([this] { request_close(); }), rightPane,
            [](Pane& pane) { add_help(pane, "Return to the main menu."); });
    });
}

void MultiplayerWindow::update() {
    // Host/Join with no disc opens the picker, the same as "Select Disc Image". The prelaunch menu
    // only surfaces the verification progress while it is on top, so surface it from here too.
    if (top_document() == this) {
        try_push_verification_modal(*this);
    }
    Window::update();
}

void MultiplayerWindow::start(bool host) {
    if (!mLaunch) {
        return;
    }

    const auto& settings = getSettings().multiplayer;
    // Owned here for the duration of the call; begin_deferred_session copies them.
    const std::string address = trimmed(settings.lastJoinAddress.getValue());
    const std::string nickname = trimmed(settings.nickname.getValue());
    const std::string color = settings.color.getValue();

    if (!host && !is_usable_address(address)) {
        return;
    }

    mp::StartupOptions options;
    options.host = host;
    options.connect = host ? nullptr : address.c_str();
    options.port = static_cast<unsigned short>(configured_port());
    options.nickname = nickname.empty() ? nullptr : nickname.c_str();
    options.color = color.empty() ? nullptr : color.c_str();

    mLaunch(*this, options);
}

}  // namespace dusk::ui
