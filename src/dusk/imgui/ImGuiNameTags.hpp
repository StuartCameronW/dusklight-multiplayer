#pragma once

namespace dusk {

/**
 * Draw a floating name tag over every remote player's puppet: their nickname, in their colour.
 *
 * Call once per presented frame from inside the ImGui frame (ImGuiConsole::PreDraw). It reads the
 * camera and the puppets' head joints as they are about to be RENDERED this frame — including the
 * frame-interpolated values — and draws into ImGui's background list, so it never touches GX or
 * J3D state. Draws nothing when no session is active.
 */
void ShowNameTags();

}  // namespace dusk
