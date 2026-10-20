// CalcScreenWindow.cpp - the complete calculator inside the debugger.
//
// The desktop build is single-window: the debugger owns the only OS window, so
// there is no separate calculator window any more. Emulator::Frame() renders
// the whole model (face, key labels, LCD and status sprites) into an offscreen
// texture, and this window presents exactly that image - the same picture the
// old external window showed. The LCD-only path is kept as a fallback for
// builds or models without a model face.
#include "WizToolchain.h"

#include "Chipset/Chipset.hpp"
#include "Peripheral/Screen.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr const char* kScreenWindowName = "Calculator";

struct Placement {
	float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
	bool hovered = false;
};

// Draw a texture centred in the available area (minus the controls strip).
// In Fit mode the calculator grows until one pair of edges is reached.  Use the
// exact fractional scale (not floor) so a 1.34x panel really fills the panel;
// the texture keeps nearest-neighbour filtering, so the keys stay crisp.
Placement DrawCentred(ImTextureID image, int width, int height, float footer, bool fit, float zoom) {
	Placement place;
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	float area_w = avail.x;
	float area_h = avail.y - footer;
	if (area_w < 1.0f || area_h < 1.0f)
		return place;

	float raw = std::min(area_w / static_cast<float>(width), area_h / static_cast<float>(height));
	float scale = fit ? raw : zoom;
	if (scale <= 0.0f)
		scale = 1.0f;
	place.w = width * scale;
	place.h = height * scale;

	const ImVec2 origin = ImGui::GetCursorScreenPos();
	place.x = origin.x + (area_w - place.w) * 0.5f;
	place.y = origin.y + (area_h - place.h) * 0.5f;
	ImGui::SetCursorScreenPos(ImVec2(place.x, place.y));
	ImGui::Image(image, ImVec2(place.w, place.h));
	place.hovered = ImGui::IsItemHovered();
	// Leave the cursor below the picture so the controls line up at the bottom.
	ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + area_h));
	return place;
}

}  // namespace

class CalcScreenWindow : public UIWindow {
public:
	CalcScreenWindow() : UIWindow(kScreenWindowName) {
		inital_size = ImVec2(420, 700);
		flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
	}

	~CalcScreenWindow() override {
		if (texture_)
			SDL_DestroyTexture(texture_);
	}

	void RenderCore() override {
		if (!m_emu) {
			ImGui::TextDisabled("The calculator is not running.");
			return;
		}

		const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;

#ifdef SINGLE_WINDOW
		// The whole calculator, as the external window used to render it.
		if (SDL_Texture* composite = m_emu->GetCompositeTexture()) {
			int image_w = m_emu->GetCompositeTextureWidth();
			int image_h = m_emu->GetCompositeTextureHeight();
			if (image_w <= 0 || image_h <= 0)
				SDL_QueryTexture(composite, nullptr, nullptr, &image_w, &image_h);
			if (image_w > 0 && image_h > 0) {
				Placement place = DrawCentred(reinterpret_cast<ImTextureID>(composite), image_w,
											  image_h, footer, fit_, zoom_);
				ForwardClicks(place, image_w, image_h);
				DrawControls(image_w, image_h);
				return;
			}
		}
#endif

		using casioemu::IScreenFrameProvider;
		IScreenFrameProvider* provider = m_emu->chipset.QueryInterface<IScreenFrameProvider>();
		if (!provider) {
			ImGui::TextDisabled("This model has no pixel screen.");
			return;
		}

		// Fallback: only the LCD pixels (models without a model face, or builds
		// that still use a separate calculator window).
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

		Placement place = DrawCentred(reinterpret_cast<ImTextureID>(texture_), width, height,
									  footer, fit_, zoom_);
		ForwardClicks(place, width, height);
		DrawControls(width, height);
	}

private:
	// Clicking the picture presses the calculator key under the cursor. The old
	// external window did this through the window rectangle; the docked image is
	// scaled, so map the mouse position back to model coordinates here.
	void ForwardClicks(const Placement& place, int width, int height) {
		if (!m_emu || !place.hovered || place.w <= 0.0f || place.h <= 0.0f)
			return;
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		int mx = static_cast<int>((mouse.x - place.x) / place.w * width);
		int my = static_cast<int>((mouse.y - place.y) / place.h * height);
		mx = std::max(0, std::min(width - 1, mx));
		my = std::max(0, std::min(height - 1, my));
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			m_emu->ModelMouseEvent(mx, my, true);
		if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
			m_emu->ModelMouseEvent(mx, my, false);
	}

	void DrawControls(int width, int height) {
		ImGui::Checkbox("Fit", &fit_);
		ImGui::SameLine();
		bool original = !fit_ && std::abs(zoom_ - 1.0f) < 0.001f;
		if (ImGui::Checkbox("Original size (1:1)", &original)) {
			if (original) {
				fit_ = false;
				zoom_ = 1.0f;
			} else {
				fit_ = true;
			}
		}
		if (!fit_) {
			// The slider moves in whole pixels of its own track, so some exact
			// values (2.00x) can be skipped.  Keep two decimals and let the
			// buttons step precisely.
			ImGui::SameLine();
			if (ImGui::SmallButton("-##zoom"))
				zoom_ = std::max(0.25f, std::round((zoom_ - 0.01f) * 100.0f) / 100.0f);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(110.0f);
			if (ImGui::SliderFloat("##zoom", &zoom_, 0.25f, 8.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp))
				zoom_ = std::round(zoom_ * 100.0f) / 100.0f;
			ImGui::SameLine();
			if (ImGui::SmallButton("+##zoom"))
				zoom_ = std::min(8.0f, std::round((zoom_ + 0.01f) * 100.0f) / 100.0f);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("%dx%d", width, height);
		if (m_emu && m_emu->GetPaused()) {
			ImGui::SameLine();
			ImGui::TextDisabled("(paused)");
		}
	}

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
