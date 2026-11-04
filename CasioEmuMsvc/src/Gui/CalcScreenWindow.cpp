// CalcScreenWindow.cpp - the complete calculator inside the debugger.
//
// The desktop build is single-window: the debugger owns the only OS window, so
// there is no separate calculator window any more. Emulator::Frame() renders
// the whole model (face, key labels, LCD and status sprites) into an offscreen
// texture, and this window presents exactly that image - the same picture the
// old external window showed. The LCD-only path is kept as a fallback for
// builds or models without a model face.
#include "WizToolchain.h"
#include "Settings/Settings.h"
#include "Shell/SessionHost.h"
#include "ToolUi.hpp"

#include "Chipset/Chipset.hpp"
#include "Peripheral/Keyboard.hpp"
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
	float scale = 1.0f;
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

	// Leave room for the 6 px card frame on each side.
	const float framed_w = std::max(1.0f, area_w - 12.0f);
	const float framed_h = std::max(1.0f, area_h - 12.0f);
	float raw = std::min(framed_w / static_cast<float>(width), framed_h / static_cast<float>(height));
	float scale = fit ? raw : zoom;
	if (scale <= 0.0f)
		scale = 1.0f;
	place.scale = scale;
	place.w = width * scale;
	place.h = height * scale;

	const ImVec2 origin = ImGui::GetCursorScreenPos();
	place.x = origin.x + (area_w - place.w) * 0.5f;
	place.y = origin.y + (area_h - place.h) * 0.5f;
	// Rounded frame behind the face so the calculator reads as a card, not a
	// raw texture pasted onto the panel.
	ImGui::GetWindowDrawList()->AddRectFilled(
		ImVec2(place.x - 6.0f, place.y - 6.0f),
		ImVec2(place.x + place.w + 6.0f, place.y + place.h + 6.0f),
		ImGui::ColorConvertFloat4ToU32(ToolUi::WithAlpha(ToolUi::Palette().surfaceAlt, 0.55f)), 8.0f);
	ImGui::GetWindowDrawList()->AddRect(
		ImVec2(place.x - 6.0f, place.y - 6.0f),
		ImVec2(place.x + place.w + 6.0f, place.y + place.h + 6.0f),
		ImGui::ColorConvertFloat4ToU32(ToolUi::Palette().border), 8.0f);
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
		ToolUi::PageScope page("Calculator", false);

		const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y;

#ifdef SINGLE_WINDOW
		// The whole calculator, as the external window used to render it.
		int active_w = 0, active_h = 0;
		SDL_Texture* active_texture = casioemu::session::ActiveTexture(&active_w, &active_h);
		if (active_texture) {
			int image_w = active_w;
			int image_h = active_h;
			if (image_w <= 0 || image_h <= 0)
				SDL_QueryTexture(active_texture, nullptr, nullptr, &image_w, &image_h);
			if (image_w > 0 && image_h > 0) {
				Placement place = DrawCentred(reinterpret_cast<ImTextureID>(active_texture), image_w,
											  image_h, footer, fit_, zoom_);
				ForwardClicks(place, image_w, image_h);
				DrawControls(place.scale);
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
		DrawControls(place.scale);
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

	void DrawControls(float effective_scale) {
		if (!m_emu)
			return;
		// Zoom row.  Fit is the dynamic default; moving the slider stores an
		// explicit scale.  The texture is nearest-neighbour sampled, so every
		// zoom level stays crisp.
		LoadZoomOnce();
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));
		float percent = fit_ ? effective_scale * 100.0f : zoom_ * 100.0f;
		const float slider_width = std::max(90.0f,
			ToolUi::RemainingWidth() - ImGui::CalcTextSize("Fit").x - 76.0f);
		ImGui::SetNextItemWidth(slider_width);
		if (fit_)
			ImGui::BeginDisabled();
		if (ImGui::SliderFloat("##calculator_zoom", &percent, 25.0f, 400.0f, "%.0f%%")) {
			fit_ = false;
			zoom_ = std::clamp(percent / 100.0f, 0.25f, 4.0f);
		}
		if (ImGui::IsItemDeactivatedAfterEdit())
			SaveZoom();
		if (fit_)
			ImGui::EndDisabled();
		if (ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Fit").x + 20.0f)) {
			if (ImGui::Button("Fit")) {
				fit_ = true;
				SaveZoom();
			}
		}
		if (ToolUi::SameLineOrWrap(ImGui::CalcTextSize("100%").x + 20.0f)) {
			if (ImGui::Button("100%")) {
				fit_ = false;
				zoom_ = 1.0f;
				SaveZoom();
			}
		}

		// Status row: model and run state only.
		ImGui::NewLine();
		ToolUi::Status(m_emu->ModelDefinition.model_name.c_str(), ToolUi::Palette().foreground);
		ImGui::SameLine();
		if (m_emu->GetPaused())
			ToolUi::Status("paused", ToolUi::Palette().warning);
		else
			ToolUi::Status("running", ToolUi::Palette().success);
		ImGui::PopStyleVar();
	}

	void ApplyStoredZoom(float stored) {
		fit_ = stored <= 0.0f;
		if (!fit_)
			zoom_ = std::clamp(stored, 0.25f, 4.0f);
		persisted_zoom_ = stored;
	}

	void LoadZoomOnce() {
		const float stored = casioemu::settings::SettingsStore::Instance().Data().calculator.zoom;
		if (!zoom_loaded_) {
			zoom_loaded_ = true;
			ApplyStoredZoom(stored);
			return;
		}
		// Settings and "Reset all settings" can change the stored zoom; follow it.
		if (stored != persisted_zoom_)
			ApplyStoredZoom(stored);
	}

	void SaveZoom() {
		auto& calculator = casioemu::settings::SettingsStore::Instance().Data().calculator;
		calculator.zoom = fit_ ? 0.0f : zoom_;
		persisted_zoom_ = calculator.zoom;
		// Persist immediately so the zoom survives a restart.
		std::string error;
		casioemu::settings::SettingsStore::Instance().Save(&error);
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
		// Nearest-neighbour sampling keeps the LCD pixels and key glyphs crisp at
		// every zoom level and panel scale.
		SDL_SetTextureScaleMode(texture_, SDL_ScaleModeNearest);
		tex_width_ = width;
		tex_height_ = height;
	}

	SDL_Texture* texture_ = nullptr;
	int tex_width_ = 0;
	int tex_height_ = 0;
	bool fit_ = true;
	float zoom_ = 2.0f;
	bool zoom_loaded_ = false;
	float persisted_zoom_ = 0.0f;
	std::vector<uint8_t> frame_;
};

UIWindow* CreateCalcScreenWindow() {
	return new CalcScreenWindow();
}
