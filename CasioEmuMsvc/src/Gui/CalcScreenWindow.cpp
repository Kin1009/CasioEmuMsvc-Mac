// CalcScreenWindow.cpp - the model's LCD inside the debugger, and the option to
// give it a dock area of its own.
//
// The desktop build shows the calculator in a second SDL window (Config.hpp does
// not define SINGLE_WINDOW). This window presents the same LCD as a debugger
// window, and the status-bar option "Dock screen" parks it in a dedicated dock
// node on the right of the debugger window: a separate area, not a tab and not a
// floating window. The pixels come from the screen peripheral's
// IScreenFrameProvider, so every model (nX-U8 / nX-U16 / EPS / TI) works.
#include "WizToolchain.h"

#include "Chipset/Chipset.hpp"
#include "Peripheral/Screen.hpp"

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

bool g_dock_calc_screen = false;

namespace {

constexpr const char* kScreenWindowName = "Calculator";

// The dock node reserved for the screen while the option is on.
ImGuiID g_screen_dock_node = 0;
bool g_dock_applied = false;

}  // namespace

class CalcScreenWindow : public UIWindow {
public:
	CalcScreenWindow() : UIWindow(kScreenWindowName) {
		inital_size = ImVec2(360, 300);
		flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
	}

	~CalcScreenWindow() override {
		if (texture_)
			SDL_DestroyTexture(texture_);
	}

	void RenderCore() override {
		using casioemu::IScreenFrameProvider;

		IScreenFrameProvider* provider = m_emu ? m_emu->chipset.QueryInterface<IScreenFrameProvider>() : nullptr;
		if (!provider) {
			ImGui::TextDisabled("This model has no pixel screen.");
			return;
		}

		// Keep the frame texture in sync with the model and the LCD size.
		provider->UpdateFrameAlpha();
		const int width = provider->GetFrameWidth();
		const int height = provider->GetFrameHeight();
		if (width <= 0 || height <= 0)
			return;
		if (!texture_ || width != tex_width_ || height != tex_height_)
			RecreateTexture(width, height);
		if (!texture_)
			return;

		const casioemu::ColourInfo& ink = m_emu->ModelDefinition.ink_color;
		frame_.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
		provider->WriteFrameRgba(frame_.data(), ink.r, ink.g, ink.b);
		SDL_UpdateTexture(texture_, nullptr, frame_.data(), width * 4);

		// The area left for the picture after the controls strip at the bottom.
		const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
		const ImVec2 avail = ImGui::GetContentRegionAvail();
		float area_w = avail.x, area_h = avail.y - footer;
		if (area_w < 1.0f || area_h < 1.0f)
			return;

		float scale = fit_ ? std::min(area_w / width, area_h / height) : zoom_;
		if (scale <= 0.0f)
			scale = 1.0f;
		const ImVec2 size(width * scale, height * scale);

		const ImVec2 origin = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(origin.x + (area_w - size.x) * 0.5f,
										origin.y + (area_h - size.y) * 0.5f));
		ImGui::Image((ImTextureID)texture_, size);
		// Put the cursor back below the picture so the footer lines up.
		ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + area_h));

		ImGui::Checkbox("Dock in main window", &g_dock_calc_screen);
		ImGui::SameLine();
		ImGui::Checkbox("Fit", &fit_);
		if (!fit_) {
			ImGui::SameLine();
			ImGui::SetNextItemWidth(90.0f);
			ImGui::SliderFloat("##zoom", &zoom_, 0.5f, 8.0f, "%.1fx");
		}
		ImGui::SameLine();
		ImGui::TextDisabled("%dx%d", width, height);
	}

private:
	void RecreateTexture(int width, int height) {
		if (texture_)
			SDL_DestroyTexture(texture_);
		texture_ = nullptr;
		if (!renderer)
			return;
		texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
									 SDL_TEXTUREACCESS_STREAMING, width, height);
		if (!texture_)
			return;
		SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND);
		tex_width_ = width;
		tex_height_ = height;
	}

	SDL_Texture* texture_ = nullptr;
	int tex_width_ = 0;
	int tex_height_ = 0;
	bool fit_ = true;
	float zoom_ = 2.0f;
	std::vector<uint8_t> frame_;
};

UIWindow* CreateCalcScreenWindow() {
	return new CalcScreenWindow();
}

void CalcScreenDockSync(bool docked, ImGuiID dockspace_id) {
	if (dockspace_id == 0)
		return;

	if (docked && !g_dock_applied) {
		// Carve a fixed area out of the right of the debugger dockspace. The
		// existing windows keep the left node; only the screen lives on the right.
		ImGuiID screen = 0, rest = 0;
		ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.30f, &screen, &rest);
		if (screen == 0)
			return;
		if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(screen)) {
			// NoTabBar: a separate area rather than another tab in the tab bar.
			node->LocalFlags |= ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoCloseButton |
								ImGuiDockNodeFlags_NoWindowMenuButton;
		}
		ImGui::DockBuilderDockWindow(kScreenWindowName, screen);
		ImGui::DockBuilderFinish(dockspace_id);
		g_screen_dock_node = screen;
		g_dock_applied = true;
	} else if (!docked && g_dock_applied) {
		// Give the space back and leave the screen docked as a normal tab.
		if (g_screen_dock_node)
			ImGui::DockBuilderRemoveNode(g_screen_dock_node);
		ImGui::DockBuilderDockWindow(kScreenWindowName, dockspace_id);
		ImGui::DockBuilderFinish(dockspace_id);
		g_screen_dock_node = 0;
		g_dock_applied = false;
	}
}
