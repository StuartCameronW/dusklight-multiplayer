#include "ImGuiNameTags.hpp"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <vector>

#include <dolphin/gx/GXAurora.h>
#include "imgui.h"

#include "ImGuiEngine.hpp"
#include "JSystem/JUtility/JUTFader.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_remote_player.h"
#include "d/d_com_inf_game.h"
#include "d/d_meter2_info.h"
#include "dusk/logging.h"
#include "dusk/multiplayer/policy/mp_policy.hpp"
#include "dusk/multiplayer/replication/player_bridge.hpp"
#include "dusk/multiplayer/replication/replication_manager.hpp"
#include "dusk/settings.h"
#include "f_op/f_op_actor_iter.h"
#include "f_op/f_op_camera_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "m_Do/m_Do_graphic.h"

/**
 * Name tags are a Dusk-side 2D overlay rather than something the puppet draws itself, on purpose:
 *
 *  - Drawing from daRemotePlayer_c::draw() means emitting GX/J2D work from an actor that is already
 *    drawn twice per presented frame under frame interpolation, and whose double entry was Hang 4.
 *    An ImGui background-list overlay touches no GX or J3D state at all.
 *  - It runs inside mDoGph_Painter (ImGuiConsole::PreDraw), i.e. AFTER every actor has drawn and,
 *    with interpolation on, inside begin/end_presentation_camera. So the window camera's view and
 *    projection here are the exact matrices the 3D pass is about to render with, interpolated or
 *    not, and the puppet's head comes through the same frame_interp lerp as its model
 *    (daRemotePlayer_c::getNameTagAnchor). Neither end is a sim-tick value, so they cannot jitter
 *    against each other.
 */

namespace dusk {
namespace {

constexpr borealis::Log Log{"dusk::nametag"};

/// Height above the head joint's origin (which sits at the base of the skull) to hang the tag,
/// in world units. Clears Link's cap.
constexpr float kHeadClearance = 40.0f;

/// Camera distance over which the tag fades out, in world units. Link is roughly 140 tall, so the
/// tag is fully visible to about 20 Links away and gone by about 30.
constexpr float kFadeStart = 2800.0f;
constexpr float kFadeEnd = 4200.0f;

/// Tag text height as a fraction of the game image's height, so it scales with window size and
/// DPI alike (ImGui works in logical units; the font atlas is already rasterised at the display
/// scale by ImGuiEngine_Initialize).
constexpr float kTextHeightFraction = 0.034f;

/// Why tags are not being drawn this frame, for the transition log.
enum class Gate {
    Shown,
    NoSession,
    PolicyOff,
    NotPlayable,  ///< no local Link, a cutscene/event, or the title screen (world_is_playable)
    Paused,
    Menu,     ///< dMeter2 window open: pause screen, item wheel, map, ...
    Message,  ///< a message box is up
    Fading,   ///< screen fade / scene transition
    NoCamera,
};

const char* gate_name(Gate gate) {
    switch (gate) {
    case Gate::Shown:
        return "shown";
    case Gate::NoSession:
        return "no session";
    case Gate::PolicyOff:
        return "policy off";
    case Gate::NotPlayable:
        return "not playable (no Link / event / title)";
    case Gate::Paused:
        return "paused";
    case Gate::Menu:
        return "menu window open";
    case Gate::Message:
        return "message up";
    case Gate::Fading:
        return "fading";
    case Gate::NoCamera:
        return "no camera";
    }
    return "?";
}

/**
 * The same conditions under which the game keeps its own HUD and the touch controls out of the way
 * (src/dusk/ui/touch_controls.cpp controls_available(), d_meter2.cpp's window-status switch). The
 * tag is HUD, so it goes away with the HUD.
 */
Gate hud_gate() {
    if (!mp::world_is_playable()) {
        return Gate::NotPlayable;
    }
    if (dComIfGp_isPauseFlag()) {
        return Gate::Paused;
    }
    if (dMeter2Info_getWindowStatus() != 0) {
        return Gate::Menu;
    }
    if (dComIfGp_getMesgStatus() != 0) {
        return Gate::Message;
    }
    const JUTFader* fader = mDoGph_gInf_c::getFader();
    if (fader == nullptr || fader->getStatus() != JUTFader::Wait || mDoGph_gInf_c::isFade() ||
        dComIfGp_isEnableNextStage())
    {
        return Gate::Fading;
    }
    return Gate::Shown;
}

/// The camera the 3D pass renders with: window 0's, exactly as mDoGph_Painter picks it.
view_class* render_view() {
    if (dComIfGp_getWindowNum() == 0) {
        return nullptr;
    }
    dDlst_window_c* window = dComIfGp_getWindow(0);
    if (window == nullptr) {
        return nullptr;
    }
    camera_process_class* camera = dComIfGp_getCamera(window->getCameraID());
    return camera != nullptr ? &camera->view : nullptr;
}

/// Where the GX framebuffer lands inside the window, in ImGui's logical coordinates. Mirrors
/// Aurora's calculate_present_viewport: the framebuffer is fitted and centred, which is the whole
/// window unless the aspect ratio is locked.
struct ScreenRect {
    float x, y, w, h;
};

ScreenRect game_rect() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    u32 fbW = 0;
    u32 fbH = 0;
    AuroraGetRenderSize(&fbW, &fbH);
    if (fbW == 0 || fbH == 0 || display.x <= 0.0f || display.y <= 0.0f) {
        return {0.0f, 0.0f, display.x, display.y};
    }
    const float scale =
        std::min(display.x / static_cast<float>(fbW), display.y / static_cast<float>(fbH));
    const float w = static_cast<float>(fbW) * scale;
    const float h = static_cast<float>(fbH) * scale;
    return {(display.x - w) * 0.5f, (display.y - h) * 0.5f, w, h};
}

struct Projected {
    float x, y;      ///< screen position, ImGui logical units
    float distance;  ///< from the camera eye, world units
};

/// World point -> screen, through the render view. False when the point is behind the camera.
bool project(const view_class& view, const ScreenRect& rect, const cXyz& world, Projected& out) {
    // View space. GX looks down -Z, so a point in front of the camera has negative z.
    const Mtx& v = view.viewMtx;
    const float vx = v[0][0] * world.x + v[0][1] * world.y + v[0][2] * world.z + v[0][3];
    const float vy = v[1][0] * world.x + v[1][1] * world.y + v[1][2] * world.z + v[1][3];
    const float vz = v[2][0] * world.x + v[2][1] * world.y + v[2][2] * world.z + v[2][3];

    const Mtx44& p = view.projMtx;
    const float cx = p[0][0] * vx + p[0][1] * vy + p[0][2] * vz + p[0][3];
    const float cy = p[1][0] * vx + p[1][1] * vy + p[1][2] * vz + p[1][3];
    const float cw = p[3][0] * vx + p[3][1] * vy + p[3][2] * vz + p[3][3];
    if (cw <= view.near_) {
        return false;
    }

    float ndcX = cx / cw;
    const float ndcY = cy / cw;
    // Mirror mode flips the finished 3D image horizontally (mDoGph_Painter's GXCopyTex + flipped
    // ortho quad), so a point drawn at +x on the framebuffer is SEEN at -x.
    if (getSettings().game.enableMirrorMode) {
        ndcX = -ndcX;
    }

    out.x = rect.x + (ndcX * 0.5f + 0.5f) * rect.w;
    out.y = rect.y + (0.5f - ndcY * 0.5f) * rect.h;
    out.distance = std::sqrt(vx * vx + vy * vy + vz * vz);
    return true;
}

struct PuppetEntry {
    daRemotePlayer_c* puppet;
};

void* collect_puppet(void* actor, void* data) {
    if (fpcM_GetName(actor) == fpcNm_REMOTE_PLAYER_e) {
        daRemotePlayer_c* puppet = static_cast<daRemotePlayer_c*>(actor);
        if (puppet->createComplete()) {
            static_cast<std::vector<PuppetEntry>*>(data)->push_back({puppet});
        }
    }
    return nullptr;  // keep iterating
}

/// Per-player log state. Positions are logged when a tag first appears and then every few
/// seconds, so a run's log shows where the tag actually landed without flooding it.
struct TagLog {
    bool everShown = false;
    std::chrono::steady_clock::time_point lastLog{};
    /// Why this player's tag was last NOT drawn, logged on change. Without it a tag that never
    /// appears is indistinguishable from one that was correctly culled.
    const char* lastCull = nullptr;
    int cullLogsLeft = 32;
};

void note_cull(TagLog& log, const char* reason, std::uint32_t playerId) {
    if (reason == log.lastCull) {
        return;
    }
    log.lastCull = reason;
    if (reason != nullptr && log.cullLogsLeft > 0) {
        --log.cullLogsLeft;
        Log.info("Name tag for player {} hidden: {}", playerId, reason);
    }
}

std::unordered_map<std::uint32_t, TagLog> s_tagLog;
Gate s_lastGate = Gate::NoSession;
int s_gateLogsLeft = 64;

void note_gate(Gate gate) {
    if (gate == s_lastGate) {
        return;
    }
    if (s_gateLogsLeft > 0) {
        --s_gateLogsLeft;
        Log.info("Name tags: {} -> {}", gate_name(s_lastGate), gate_name(gate));
    }
    s_lastGate = gate;
}

}  // namespace

void ShowNameTags() {
    const auto& players = mp::replication_manager().players();
    if (players.empty()) {
        if (s_lastGate != Gate::NoSession) {
            note_gate(Gate::NoSession);
            s_tagLog.clear();
        }
        return;
    }
    if (!mp::g_mpPolicy.showNameTags) {
        note_gate(Gate::PolicyOff);
        return;
    }
    Gate gate = hud_gate();
    view_class* view = gate == Gate::Shown ? render_view() : nullptr;
    if (gate == Gate::Shown && view == nullptr) {
        gate = Gate::NoCamera;
    }
    note_gate(gate);
    if (gate != Gate::Shown) {
        return;
    }

    std::vector<PuppetEntry> puppets;
    fopAcIt_Judge(collect_puppet, &puppets);
    if (puppets.empty()) {
        return;
    }

    const ScreenRect rect = game_rect();
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGuiEngine::fontLarge != nullptr ? ImGuiEngine::fontLarge : ImGui::GetFont();
    const float textSize = std::clamp(rect.h * kTextHeightFraction, 12.0f, 64.0f);
    const float outline = std::max(1.0f, textSize / 18.0f);
    const auto now = std::chrono::steady_clock::now();

    for (const PuppetEntry& entry : puppets) {
        const auto playerIt = players.find(entry.puppet->getPlayerId());
        if (playerIt == players.end()) {
            continue;
        }
        const mp::RemotePlayer& player = playerIt->second;
        TagLog& log = s_tagLog[player.playerId];

        cXyz head;
        if (!entry.puppet->getNameTagAnchor(&head)) {
            note_cull(log, "no anchor (no pose or body yet)", player.playerId);
            continue;
        }
        const cXyz anchor(head.x, head.y + kHeadClearance, head.z);

        Projected at;
        if (!project(*view, rect, anchor, at)) {
            note_cull(log, "behind the camera", player.playerId);
            continue;
        }
        if (at.distance >= kFadeEnd) {
            note_cull(log, "beyond fade distance", player.playerId);
            continue;
        }
        const float fade = at.distance <= kFadeStart ?
                               1.0f :
                               1.0f - (at.distance - kFadeStart) / (kFadeEnd - kFadeStart);

        const char* name = player.nickname.empty() ? "Player" : player.nickname.c_str();
        const ImVec2 size = font->CalcTextSizeA(textSize, FLT_MAX, 0.0f, name);
        const ImVec2 pos(std::floor(at.x - size.x * 0.5f), std::floor(at.y - size.y));

        // Off-screen: the whole tag is outside the game image.
        if (pos.x + size.x < rect.x || pos.x > rect.x + rect.w || pos.y + size.y < rect.y ||
            pos.y > rect.y + rect.h)
        {
            note_cull(log, "off-screen", player.playerId);
            continue;
        }

        const int alpha = static_cast<int>(std::lround(255.0f * fade));
        const ImU32 color = IM_COL32(
            (player.color >> 16) & 0xFF, (player.color >> 8) & 0xFF, player.color & 0xFF, alpha);
        const ImU32 shadow = IM_COL32(0, 0, 0, alpha * 3 / 4);
        // A dark outline keeps any player colour readable against sky, snow or water.
        static const ImVec2 kOffsets[] = {
            {-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
        for (const ImVec2& o : kOffsets) {
            drawList->AddText(
                font, textSize, ImVec2(pos.x + o.x * outline, pos.y + o.y * outline), shadow, name);
        }
        drawList->AddText(font, textSize, pos, color, name);

        note_cull(log, nullptr, player.playerId);
        if (!log.everShown || now - log.lastLog >= std::chrono::seconds(3)) {
            // The puppet's feet through the same projection, so the log alone says whether the tag
            // landed over the puppet: same x (give or take the lean), a sensible height above.
            Projected feet{};
            const bool feetOk = project(*view, rect, entry.puppet->current.pos, feet);
            Log.info(
                "Name tag {} for player {} '{}' #{:06X}: tag ({:.0f},{:.0f}) feet "
                "{}({:.0f},{:.0f}) "
                "in game rect ({:.0f},{:.0f} {:.0f}x{:.0f}), dist {:.0f}, alpha {}, size {:.1f}, "
                "mirror {}, interp {}",
                log.everShown ? "at" : "FIRST SHOWN", player.playerId, name, player.color, at.x,
                at.y, feetOk ? "" : "behind ", feet.x, feet.y, rect.x, rect.y, rect.w, rect.h,
                at.distance, alpha, textSize, getSettings().game.enableMirrorMode ? "on" : "off",
                getSettings().game.enableFrameInterpolation.getValue() != FrameInterpMode::Off ?
                    "on" :
                    "off");
            log.everShown = true;
            log.lastLog = now;
        }
    }
}

}  // namespace dusk
