#pragma once
// Touch-friendly on-screen calculator keypad (ImGui overlay, iPadOS only).

namespace casioemu::ipados::keypad {
/// Draw the keypad. Must be called between ImGui::NewFrame() and ImGui::Render().
void Render();
/// Show/hide the keypad (also toggled by the toolbar button).
void SetVisible(bool visible);
[[nodiscard]] bool IsVisible();
} // namespace casioemu::ipados::keypad
