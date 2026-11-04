#pragma once
#include <cstring>
#include "Chipset/MMU.hpp"
#include "Emulator.hpp"
#include "LabelFile.h"
#include "ThemeManager.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"


class CodeViewer;
class SnapshotWindow;
CodeViewer* test_gui(bool* guiCreated, SDL_Window*, SDL_Renderer*);
void gui_cleanup();
void gui_loop();
extern char* n_ram_buffer;
extern int top_bar_size;
extern casioemu::MMU* me_mmu;
extern casioemu::Emulator* m_emu;
extern SDL_Window* window;
extern SDL_Renderer* renderer;
extern bool calculator_panel_focused;
extern std::vector<Label> g_labels;
extern uint32_t pc_cache;
extern CodeViewer* code_viewer;
extern SnapshotWindow* snapshot_window;
extern std::vector<class UIWindow*> windows;

void SetDebugbreak(void); 
class UIWindow {
public:
	UIWindow(const char* name) : name(name) {
#ifdef __ANDROID__
		inital_size = ImVec2(
			800 * ThemeManager::Instance().fontScale,
			800 * ThemeManager::Instance().fontScale);
#else
		inital_size = ImVec2(800, 800);
#endif
	}
	const char* name{};
	bool open = true;
	bool bring_to_front_requested = false;
	ImVec2 inital_size;
	ImGuiWindowFlags flags{};

	virtual void Render() {
		// Settings is the recovery point for hidden tabs and must always remain available.
		if (name && std::strcmp(name, "Settings") == 0)
			open = true;
		if (!open)
			return;
#ifdef __ANDROID__
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
			ImVec2(ThemeManager::Instance().padding, ThemeManager::Instance().padding));
#endif
		if (bring_to_front_requested) {
			ImGui::SetNextWindowFocus();
			bring_to_front_requested = false;
		}
		ImGui::SetNextWindowSize(inital_size, ImGuiCond_FirstUseEver);
		const bool was_open = open;
		const bool is_calculator = name && std::strcmp(name, "Calculator") == 0;
#ifdef CASIOEMU_CORE_WEB
		const bool visible = ImGui::Begin(name, nullptr, flags);
#else
		const bool visible = ImGui::Begin(name, &open, flags);
#endif
		if (visible) {
			if (is_calculator)
				calculator_panel_focused = ImGui::IsWindowFocused();
			RenderCore();
		}
		else if (is_calculator) {
			// Docked but not the selected tab, collapsed, or just closed:
			// RenderCore() did not run, so refresh the flag here instead of
			// letting a stale "focused" value from an earlier frame survive.
			calculator_panel_focused = false;
		}
		ImGui::End();
		// Closing the Calculator tab means "reboot the calculator", not "hide
		// the only way to see the model".  Reopen it on the main menu.
		if (was_open && !open && is_calculator) {
			calculator_panel_focused = false;
			open = true;
			bring_to_front_requested = true;
			if (m_emu)
				m_emu->ResetToMainMenu();
		}
		if (name && std::strcmp(name, "Settings") == 0)
			open = true;

#ifdef __ANDROID__
		ImGui::PopStyleVar();
#endif
	}
	void BringToFront() {
		open = true;
		bring_to_front_requested = true;
	}
	virtual void RenderCore() = 0;
	// Optional integration hooks used by the shell's file editor and build
	// overlay.  Default no-ops keep every existing tool unchanged.
	virtual bool OpenEditorDocument(const std::string& /*path*/, const std::string& /*text*/) { return false; }
	virtual void SetEditorTarget(const std::string& /*target*/) {}
	virtual void SetEditorLanguage(const std::string& /*language*/) {}
	virtual void SetEditorRopPadding(int /*padding*/) {}
	virtual void SetEditorOriginalRom(const std::string& /*path*/) {}
	virtual bool CopyBuildLauncher() { return false; }
	virtual void BuildEditorDocument(bool /*run*/) {}
	virtual void PreprocessEditorDocument() {}
	virtual void LintEditorDocument() {}
	// Optional: navigate the window to a specific memory address. No-op by default.
	virtual void GotoMemoryAddress(uint32_t /*addr*/) {}
	virtual ~UIWindow() {}


};

// protected:
//	// syntax: prompt_if_error(func)(...);
//	auto prompt_if_error(auto f) {
//	}

namespace UIHelpers {
	// Button with keyboard shortcut tooltip
	inline bool ButtonWithShortcut(const char* label, const char* shortcut, ImVec2 size = { 0, 0 }) {
		bool pressed = ImGui::Button(label, size);
		if (shortcut && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
			ImGui::BeginTooltip();
			ImGui::TextDisabled("Shortcut: %s", shortcut);
			ImGui::EndTooltip();
		}
		return pressed;
	}

	// (?) Help marker - displays a tooltip on hover
	inline void HelpMarker(const char* desc) {
		ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
			ImGui::TextUnformatted(desc);
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
	}

	// Group/Section header with separator line
	inline void SectionHeader(const char* label) {
		ImGui::Spacing();
		ImGui::TextUnformatted(label);
		ImGui::Separator();
		ImGui::Spacing();
	}

	// Width of a checkbox including its square and the label.
	inline float CheckboxWidth(const char* label) {
		const ImGuiStyle& style = ImGui::GetStyle();
		return ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
	}

	// Place a checkbox on the current row, or wrap to a new row when it would
	// be clipped.  `first` suppresses SameLine() for the leading item.
	inline bool CheckboxFlow(const char* label, bool* value, bool first) {
		const float width = CheckboxWidth(label);
		if (!first) {
			if (ImGui::GetContentRegionAvail().x < width + ImGui::GetStyle().ItemSpacing.x)
				ImGui::NewLine();
			else
				ImGui::SameLine();
		}
		return ImGui::Checkbox(label, value);
	}

	// SameLine() only when the upcoming widget fits; otherwise wrap.
	inline bool SameLineOrWrap(float width) {
		if (ImGui::GetContentRegionAvail().x < width + ImGui::GetStyle().ItemSpacing.x) {
			ImGui::NewLine();
			return false;
		}
		ImGui::SameLine();
		return true;
	}

	inline float ButtonWidth(const char* label) {
		return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
	}

	// Colored status badge
	inline void StatusBadge(const char* text, const ImVec4& color) {
		ImGui::PushStyleColor(ImGuiCol_Button, color);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
		ImGui::SmallButton(text);
		ImGui::PopStyleColor(3);
	}

	// Color constants (proper 0.0 - 1.0 range)
	inline constexpr ImVec4 kColorSuccess = ImVec4(0.30f, 0.80f, 0.30f, 1.0f);
	inline constexpr ImVec4 kColorWarning = ImVec4(0.95f, 0.80f, 0.20f, 1.0f);
	inline constexpr ImVec4 kColorError   = ImVec4(0.95f, 0.30f, 0.30f, 1.0f);
	inline constexpr ImVec4 kColorInfo    = ImVec4(0.40f, 0.60f, 0.90f, 1.0f);
	inline constexpr ImVec4 kColorMuted   = ImVec4(0.55f, 0.55f, 0.60f, 1.0f);

	// Jump target types: code address navigates the Code Viewer disassembler,
	// memory address navigates the RAM/ROM hex editor.
	enum class JumpTarget { Code, Memory, Both };

	// Clickable address link (implemented in Ui.cpp).
	// - Left-click: jumps according to `defaultTarget`
	// - Right-click: opens a context menu offering both Code Jump and Memory Jump
	void ClickableAddress(uint32_t addr, JumpTarget defaultTarget = JumpTarget::Code);

	// Programmatically navigate the memory editor to `addr`.
	void JumpToMemory(uint32_t addr);
}

inline constexpr ImGuiTableFlags pretty_table = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Resizable;
