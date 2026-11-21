// ImGui on-screen keypad for iPadOS.  Each key synthesizes a normal SDL key
// event so it goes through the exact same keyboard-binding path as a hardware
// keyboard or a bluetooth keypad.  No emulator-specific key state is touched.

#include "IPadOSKeypad.h"
#include "IPadOSBridge.h"
#include "Ui.hpp"

#include <SDL.h>
#include <imgui.h>

#include <array>
#include <cstring>
#include <string>

namespace {

struct KeyDef {
    const char* label;
    SDL_Keycode key;
};

// Standard Casio ClassWiz keyboard layout.  The model configurations bind
// these SDL key names to the actual calculator matrix, so the mapping remains
// data-driven (see ModelConfig / Keyboard.cpp).
constexpr std::array<KeyDef, 30> kKeys = {{
    {"ON", SDLK_F1}, {"SHIFT", SDLK_LSHIFT}, {"ALPHA", SDLK_LALT},
    {"MODE", SDLK_ESCAPE}, {"SET", SDLK_F2}, {"<", SDLK_LEFT},
    {"^", SDLK_UP}, {"v", SDLK_DOWN}, {">", SDLK_RIGHT},
    {"DEL", SDLK_BACKSPACE}, {"AC", SDLK_DELETE}, {"M+", SDLK_F3},
    {"7", SDLK_7}, {"8", SDLK_8}, {"9", SDLK_9}, {"/", SDLK_SLASH},
    {"4", SDLK_4}, {"5", SDLK_5}, {"6", SDLK_6}, {"*", SDLK_ASTERISK},
    {"1", SDLK_1}, {"2", SDLK_2}, {"3", SDLK_3}, {"-", SDLK_MINUS},
    {"0", SDLK_0}, {".", SDLK_PERIOD}, {"EXP", SDLK_e}, {"=", SDLK_RETURN},
    {"+", SDLK_PLUS}, {"(", SDLK_LEFTBRACKET},
}};

void PushKey(SDL_Keycode key) {
    SDL_Event down{};
    down.type = SDL_KEYDOWN;
    down.key.state = SDL_PRESSED;
    down.key.repeat = 0;
    down.key.keysym.sym = key;
    down.key.keysym.scancode = SDL_GetScancodeFromKey(key);
    SDL_PushEvent(&down);

    SDL_Event up{};
    up.type = SDL_KEYUP;
    up.key.state = SDL_RELEASED;
    up.key.repeat = 0;
    up.key.keysym.sym = key;
    up.key.keysym.scancode = down.key.keysym.scancode;
    SDL_PushEvent(&up);
}

bool g_visible = true;

} // namespace

// Defined here so IPadOSBridge.mm does not need to include Ui.hpp.
void IPadOSOpenSettingsWindow() {
    for (auto* win : windows) {
        if (!win || !win->name || std::strcmp(win->name, "Settings") != 0)
            continue;
        win->open = true;
        win->bring_to_front_requested = true;
        return;
    }
}

namespace casioemu::ipados::keypad {

void SetVisible(bool visible) { g_visible = visible; }
bool IsVisible() { return g_visible; }

void Render() {
    if (!g_visible)
        return;

    float safe_l = 0, safe_t = 0, safe_r = 0, safe_b = 0;
    GetSafeAreaInsets(safe_l, safe_t, safe_r, safe_b);

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float width = 430.0f;
    ImGui::SetNextWindowPos(
        ImVec2(display.x - width - safe_r - 12.0f,
               display.y - safe_b - 292.0f),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, 280.0f), ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    ImGui::Begin("iPad Keypad", &g_visible,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking);
    ImGui::SetWindowFontScale(1.15f);

    constexpr int columns = 6;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float button_width = (ImGui::GetContentRegionAvail().x -
                                spacing * (columns - 1)) / columns;

    int index = 0;
    for (const KeyDef& key : kKeys) {
        if (ImGui::Button(key.label, ImVec2(button_width, 36.0f)))
            PushKey(key.key);
        if ((++index % columns) != 0)
            ImGui::SameLine();
    }

    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    if (ImGui::Button("Settings", ImVec2(120.0f, 34.0f)))
        casioemu::ipados::OpenSettingsWindow();
    ImGui::SameLine();
    if (ImGui::Button("Hide keys", ImVec2(120.0f, 34.0f)))
        g_visible = false;
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace casioemu::ipados::keypad
