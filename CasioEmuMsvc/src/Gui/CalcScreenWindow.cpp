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
#include "DiagScreenDetector.h"
#include "Peripheral/Screen.hpp"
#include "Shell/SessionHost.h"

#include <SDL.h>

#include <algorithm>
#include <chrono>
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

		float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
		// The diag macro button adds one more controls line on arch-core
		// sessions; reserve it so the Fit / zoom row stays inside the panel.
		if (m_emu && m_emu->HasArchitectureCore())
			footer += ImGui::GetFrameHeightWithSpacing();

#ifdef SINGLE_WINDOW
		// The whole calculator, as the external window used to render it.  Use
		// the session mirror: a secondary emulator's texture belongs to its own
		// hidden SDL renderer and cannot be drawn by the main renderer.
		int image_w = 0;
		int image_h = 0;
		if (SDL_Texture* composite = casioemu::session::ActiveTexture(&image_w, &image_h)) {
			if (image_w > 0 && image_h > 0) {
				Placement place = DrawCentred(reinterpret_cast<ImTextureID>(composite), image_w,
											  image_h, footer, fit_, zoom_);
				ForwardClicks(place, image_w, image_h);
				DrawPressedKeyHighlight(place, image_w, image_h);
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

	// Board models (e.g. the SH4 CG100) bypass the built-in Keyboard peripheral,
	// so its Frame() never draws the dark pressed-key overlay that Graph Light
	// and the other models show (Keyboard.cpp: SDL_RenderFillRect black, alpha
	// 127). Reproduce that look here with ImGui: map each pressed button rect
	// from face/model pixels into the placed (possibly scaled) skin texture.
	void DrawPressedKeyHighlight(const Placement& place, int width, int height) {
		if (!m_emu || place.w <= 0.0f || place.h <= 0.0f || width <= 0 || height <= 0)
			return;
		const auto rects = m_emu->GetPressedButtonRects();
		if (rects.empty())
			return;
		const float sx = place.w / static_cast<float>(width);
		const float sy = place.h / static_cast<float>(height);
		ImDrawList* draw = ImGui::GetWindowDrawList();
		for (const auto& r : rects) {
			if (r.w <= 0 || r.h <= 0)
				continue;
			const ImVec2 p_min(place.x + r.x * sx, place.y + r.y * sy);
			const ImVec2 p_max(place.x + (r.x + r.w) * sx, place.y + (r.y + r.h) * sy);
			const float span = std::min(p_max.x - p_min.x, p_max.y - p_min.y);
			const float rounding = std::min(12.0f * std::min(sx, sy), span * 0.35f);
			draw->AddRectFilled(p_min, p_max, IM_COL32(0, 0, 0, 127), rounding);
		}
	}

	// --- Genuine OS diag entry buttons (plugin/arch cores only) --------------
	// These do NOT press keys. Each calls the plugin's state-configure+reboot
	// export: the plugin patches REAL calculator state (mode 0: factory-blank
	// flash model word 0x300 = FF FF FF FF; mode 1: exam marker erased and the
	// boot key-combo held IN THE CORE), resets the CPU to the reset vector, and
	// lets the OS itself enter diag on the next boot. No fake text is drawn and
	// no diagnostic function is called from the host. The plugin holds/releases
	// the in-core combo itself (via its normal Tick loop), so no UI key handling
	// is involved at all.
	void StartDiag(int mode) {
		if (!m_emu || !m_emu->HasArchitectureCore())
			return;
		if (diag_cooldown_ && std::chrono::steady_clock::now() - diag_last_click_ <
		                          std::chrono::milliseconds(1500))
			return; // re-entry guard: do not interrupt an in-progress diag boot
		diag_last_click_ = std::chrono::steady_clock::now();
		diag_cooldown_ = true;
		m_emu->ArchEnterDiag(mode);
		diag_status_ = mode == 0
			? "SERVICE diag: factory-blank 0x300, booting..."
			: "FACTORY diag: combo held in-core, booting (auto-release after boot)...";
	}

	void DrawControls(int width, int height) {
		if (m_emu && m_emu->HasArchitectureCore()) {
			const bool diag_running = false; // calls are synchronous; guard is time-based
			if (diag_running)
				ImGui::BeginDisabled(); // cannot re-trigger while a diag boot is in progress
			const bool svc_clicked = ImGui::Button("To diagnostics mode (SERVICE)");
			const bool svc_tip = ImGui::IsItemHovered();
			ImGui::SameLine();
			const bool fac_clicked = ImGui::Button("To diagnostics mode (FACTORY)");
			const bool fac_tip = ImGui::IsItemHovered();
			if (diag_running)
				ImGui::EndDisabled();
			if (svc_clicked && !diag_running)
				StartDiag(0); // factory-blank flash 0x300 -> service menu
			if (fac_clicked && !diag_running)
				StartDiag(1); // boot key-combo gate -> DIAGNOSTIC MODE
			if (!diag_status_.empty()) {
				ImGui::SameLine();
				ImGui::TextUnformatted(diag_status_.c_str());
			}
			if (svc_tip)
				ImGui::SetTooltip("Patches the factory-blank flash model word (0x300 = FF FF FF FF) and RESETs. The OS's own boot gate enters the [BaseROM MAIN] SERVICE menu (1.PATTERN 5.SERVICE / 4.VERSION ...). No keys, no fake output.");
			if (fac_tip)
				ImGui::SetTooltip("Erases the exam marker, RESETs, and holds AC + x10^x + ON IN THE CORE. The OS's own boot gate enters '=DIAGNOSTIC MODE= / Factory Use Only!'. The combo is released once the dialog appears; no UI keypresses or fake output.");
			ImGui::SameLine();
			const bool reboot_clicked = ImGui::Button("Reboot");
			const bool reboot_tip = ImGui::IsItemHovered();
			if (reboot_clicked)
				m_emu->ArchReboot();
			if (reboot_tip)
				ImGui::SetTooltip("Physical RESET button: true CPU reset to the reset vector (0x80000000), then boot the OS fresh");
		}
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

	bool diag_cooldown_ = false;                 // re-entry guard for diag buttons
	std::chrono::steady_clock::time_point diag_last_click_{};
	std::string diag_status_;
};

UIWindow* CreateCalcScreenWindow() {
	return new CalcScreenWindow();
}
