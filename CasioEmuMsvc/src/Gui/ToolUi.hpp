#pragma once
// Shared visual language for the debugger tool pages.
//
// Part 1 of the UI revamp owns the shell chrome (title bar, session tabs,
// explorer, bottom panel, status bar).  This header gives every tool document
// the same margins, section headers, grouped panels, aligned label/control
// rows, consistent row heights and dialog chrome, so the tool pages stop
// looking like raw ImGui demos.
//
// Everything here is header-only and inline: the tool pages only pull in
// ThemeManager + ImGui, and no new translation unit is required.

#include "ThemeManager.h"
#include "Plugin/ThemeTokens.h"
#include "Ui.hpp"

#include "imgui/imgui.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace ToolUi {

using casioemu::plugin::ThemePalette;

inline const ThemePalette& Palette() {
	return ThemeManager::Instance().Colors();
}

inline ImU32 Col(const ImVec4& color) {
	return ImGui::ColorConvertFloat4ToU32(color);
}

inline ImVec4 WithAlpha(ImVec4 color, float alpha) {
	color.w = alpha;
	return color;
}

inline float UiScale() {
	return std::max(0.8f, ThemeManager::Instance().GetFontScale());
}

// One consistent row height for every control in a tool page.
inline float RowHeight() {
	return ImGui::GetFrameHeight();
}

// Remaining width to the window's right content edge.  GetContentRegionAvail()
// is based on WorkRect, which grows with content inside auto-resizing children,
// so it can over-report space and let a button wrap off the panel edge.
inline float RemainingWidth() {
	// Screen-space check: WorkRect/ContentRegionRect can grow with content inside
	// auto-resizing children, which let buttons wrap off the panel edge.  The
	// page scrollbar lives inside the right margin, so subtract it too.
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const ImGuiStyle& style = ImGui::GetStyle();
	float right = window->Pos.x + window->Size.x - style.WindowPadding.x - window->Scroll.x;
	if (window->ScrollbarY)
		right -= window->ScrollbarSizes.x;
	return right - ImGui::GetCursorScreenPos().x;
}

// ---------------------------------------------------------------- page scope
// 16 px content margins.  The shell already supplies 8 px of child padding;
// PageScope adds a second inset so text never touches the panel edge.  RAII so
// an early return in a tool's RenderCore cannot unbalance the style stack.
//
// When CASIOEMU_TOOL_LAYOUT_REPORT is set, the destructor appends one line per
// page describing horizontal overflow.  That is the headless layout check for
// the tool pages: a widget wider than the content region is clipped, and the
// report calls it out with the page name.
struct PageScope {
	// A page is an inset child of the shell's document body: 8 px on the left,
	// right, top and bottom, so no table, button, header or scrollbar can touch
	// the panel edge.  The shell's document child has no padding of its own, so
	// the margin has to come from here.  `inset=false` is used by pages the shell
	// already pads (the calculator panel).
	explicit PageScope(const char* page_name = "Tool", bool inset = true, bool scroll = true)
		: name(page_name), inset_(inset), scroll_(scroll) {
		if (std::getenv("CASIOEMU_PERF_TIMING"))
			timing_ = std::getenv("CASIOEMU_PERF_TIMING")[0] != '0';
		start_ = std::chrono::steady_clock::now();
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 4.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		const ImVec2 avail = ImGui::GetContentRegionAvail();
		if (inset_) {
			ImGui::Indent(8.0f);
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
		}
		ImGuiWindowFlags page_flags = ImGuiWindowFlags_NoBackground;
		if (!scroll_)
			page_flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
		ImGui::BeginChild("##tool_page",
			inset_ ? ImVec2(avail.x - 16.0f, avail.y - 16.0f) : ImVec2(0.0f, 0.0f),
			ImGuiChildFlags_None, page_flags);
		if (!scroll_) {
			// Window state (including a stale scroll offset from an earlier
			// scrollable layout) is persisted by id; pin it so the content
			// region equals the visible area and the fixed footer cannot drift.
			ImGui::SetScrollX(0.0f);
			ImGui::SetScrollY(0.0f);
		}
	}
	~PageScope() {
		if (timing_) {
			const double ms = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - start_).count();
			if (const char* path = std::getenv("CASIOEMU_TOOL_LAYOUT_REPORT")) {
				if (std::FILE* file = std::fopen(path, "a")) {
					std::fprintf(file, "PAGE %s ui_ms=%.3f\n", name, ms);
					std::fclose(file);
				}
			}
		}
		RecordOverflow();
		ImGui::EndChild();
		if (inset_)
			ImGui::Unindent(8.0f);
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(3);
	}
	PageScope(const PageScope&) = delete;
	PageScope& operator=(const PageScope&) = delete;

	const char* name = "Tool";

private:
	bool inset_ = true;
	bool scroll_ = true;
	bool timing_ = false;
	std::chrono::steady_clock::time_point start_{};

	void RecordOverflow() const {
		const char* path = std::getenv("CASIOEMU_TOOL_LAYOUT_REPORT");
		if (!path || !*path)
			return;
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		if (!window || window->SkipItems)
			return;
		// WorkRect excludes the page scrollbar (which lives inside the right
		// margin by design); ContentRegionRect does not.
		const float content_right = window->WorkRect.Max.x;
		const float overflow = window->DC.CursorMaxPos.x - content_right;
		const float width = content_right - window->WorkRect.Min.x;
		static int frames = 0;
		if (++frames < 6)
			return;
		struct Entry { float overflow; float width; };
		static std::map<std::string, Entry> worst;
		Entry& entry = worst[name];
		if (entry.width == 0.0f || overflow > entry.overflow + 0.5f) {
			entry.overflow = overflow;
			entry.width = width;
			std::FILE* file = std::fopen(path, "a");
			if (file) {
				std::fprintf(file, "%s %s overflow=%.1fpx width=%.1fpx\n",
					overflow > 1.0f ? "FAIL" : "PASS", name, overflow, width);
				std::fclose(file);
			}
		}
	}
};

// -------------------------------------------------------------- section header
// Accent bar + label + hairline.  This replaces the default "plain text then
// separator" look with an actual hierarchy cue.
inline void Section(const char* label) {
	ImGui::Spacing();
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const float text_h = ImGui::GetTextLineHeight();
	const ImVec2 text_size = ImGui::CalcTextSize(label);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(ImVec2(pos.x, pos.y + 1.0f),
		ImVec2(pos.x + 3.0f, pos.y + text_h - 1.0f), Col(Palette().accent), 1.5f);
	ImGui::SetCursorScreenPos(ImVec2(pos.x + 10.0f, pos.y));
	ImGui::PushStyleColor(ImGuiCol_Text, Palette().foreground);
	ImGui::TextUnformatted(label);
	ImGui::PopStyleColor();
	const float line_y = pos.y + text_h + 4.0f;
	draw->AddLine(ImVec2(pos.x, line_y), ImVec2(pos.x + text_size.x + 10.0f, line_y),
		Col(Palette().border), 1.0f);
	ImGui::SetCursorScreenPos(ImVec2(pos.x, line_y + 5.0f));
	ImGui::Spacing();
}

inline void Description(const char* text) {
	ImGui::PushStyleColor(ImGuiCol_Text, Palette().muted);
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextUnformatted(text);
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
}

// ---------------------------------------------------------------- row helpers
// Muted, right-aligned label in a fixed column, then the control starts at a
// stable x so a page's inputs line up vertically.
inline void FieldLabel(const char* label, float column_width = 132.0f) {
	ImGui::AlignTextToFramePadding();
	const float start_x = ImGui::GetCursorPosX();
	const float available = RemainingWidth();
	const float text_w = ImGui::CalcTextSize(label).x;
	// A label that cannot share the row without overlapping its control gets a
	// row of its own: full text, never truncated or drawn over the widget.
	if (text_w + 12.0f > available * 0.55f) {
		ImGui::PushStyleColor(ImGuiCol_Text, Palette().muted);
		ImGui::TextUnformatted(label);
		ImGui::PopStyleColor();
		return;
	}
	const float column = std::clamp(std::max(column_width, text_w + 12.0f),
		72.0f, available * 0.55f);
	ImGui::SetCursorPosX(start_x + std::max(0.0f, column - 8.0f - text_w));
	ImGui::PushStyleColor(ImGuiCol_Text, Palette().muted);
	ImGui::TextUnformatted(label);
	ImGui::PopStyleColor();
	ImGui::SameLine();
	ImGui::SetCursorPosX(start_x + column);
}

inline void Field(const char* label, float column_width = 132.0f) {
	FieldLabel(label, column_width);
}

// Muted secondary line that never clips: wraps inside the content region.
inline void Hint(const char* text) {
	ImGui::PushStyleColor(ImGuiCol_Text, Palette().muted);
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextUnformatted(text);
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
}

inline void Status(const char* text, const ImVec4& color) {
	ImGui::PushStyleColor(ImGuiCol_Text, color);
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextUnformatted(text);
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
}

// -------------------------------------------------------------------- panels
// Rounded, softly filled group with inner padding.  Used for forms, previews
// and empty states so they read as cards rather than flat ImGui regions.
struct PanelScope {
	PanelScope(const char* id, float height = 0.0f, bool border = true) {
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, border ? 1.0f : 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, WithAlpha(Palette().surfaceAlt, 0.55f));
		ImGui::PushStyleColor(ImGuiCol_Border, Palette().border);
		ImGuiChildFlags child_flags = ImGuiChildFlags_AlwaysUseWindowPadding;
		if (border)
			child_flags |= ImGuiChildFlags_Border;
		if (height <= 0.0f)
			child_flags |= ImGuiChildFlags_AutoResizeY;
		ImGui::BeginChild(id, ImVec2(0.0f, height), child_flags);
	}
	~PanelScope() {
		ImGui::EndChild();
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar(4);
	}
	PanelScope(const PanelScope&) = delete;
	PanelScope& operator=(const PanelScope&) = delete;
};

// ---------------------------------------------------------------- table chrome
inline ImGuiTableFlags TableFlags() {
	return ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
		ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
		ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings |
		ImGuiTableFlags_PadOuterX;
}

// Tables get a tinted header row and a rounded outer frame.
struct TableScope {
	explicit TableScope(const char* id, int columns, ImGuiTableFlags flags = TableFlags(),
		float height = 0.0f) {
		ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, Palette().surface);
		ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, Palette().borderStrong);
		ImGui::PushStyleColor(ImGuiCol_TableBorderLight, Palette().border);
		ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, WithAlpha(Palette().surfaceAlt, 0.35f));
		if (height > 0.0f) {
			flags |= ImGuiTableFlags_ScrollY;
			began_ = ImGui::BeginTable(id, columns, flags, ImVec2(0.0f, height));
		}
		else {
			began_ = ImGui::BeginTable(id, columns, flags);
		}
	}
	~TableScope() {
		if (began_)
			ImGui::EndTable();
		ImGui::PopStyleColor(4);
	}
	explicit operator bool() const { return began_; }
	TableScope(const TableScope&) = delete;
	TableScope& operator=(const TableScope&) = delete;

private:
	bool began_ = false;
};

// ---------------------------------------------------------------- buttons/X
// Square, monochrome X used by every dialog header and popup.  Drawn with the
// draw list so it has no dependency on a glyph or icon font.
inline bool CloseButton(const char* id = "##close", const char* tooltip = "Close") {
	ImGui::PushID(id);
	const float height = 22.0f;
	const float width = 24.0f;
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const bool pressed = ImGui::InvisibleButton("##x", ImVec2(width, height));
	const bool hovered = ImGui::IsItemHovered();
	ImDrawList* draw = ImGui::GetWindowDrawList();
	if (hovered) {
		draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
			Col(WithAlpha(Palette().error, 0.90f)), 4.0f);
	}
	const ImVec4 color = hovered ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : Palette().muted;
	const ImU32 fg = Col(color);
	const ImVec2 centre(pos.x + width * 0.5f, pos.y + height * 0.5f);
	const float arm = 4.0f;
	draw->AddLine(ImVec2(centre.x - arm, centre.y - arm), ImVec2(centre.x + arm, centre.y + arm), fg, 1.4f);
	draw->AddLine(ImVec2(centre.x - arm, centre.y + arm), ImVec2(centre.x + arm, centre.y - arm), fg, 1.4f);
	if (hovered && tooltip)
		ImGui::SetTooltip("%s", tooltip);
	ImGui::PopID();
	return pressed;
}

// Small pill button with an accent tint, for primary actions in a row.
inline bool AccentButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
	ImGui::PushStyleColor(ImGuiCol_Button, Palette().accent);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Palette().accentHover);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, Palette().accentHover);
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
	const bool pressed = ImGui::Button(label, size);
	ImGui::PopStyleColor(4);
	return pressed;
}

// ------------------------------------------------------------------- dialogs
// Every modal dialog uses the same frame: 16 px padding, 8 px rounding, a
// muted title, and an X in the top-right corner.  Returns false when the
// popup is not open; the destructor only runs for the true path.
class DialogScope {
public:
	DialogScope(const char* popup_id, const char* title, float width,
		ImGuiWindowFlags extra = 0) {
		if (ImGui::IsPopupOpen(popup_id))
			ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, FLT_MAX));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
		ImGui::PushStyleColor(ImGuiCol_PopupBg, Palette().surface);
		ImGui::PushStyleColor(ImGuiCol_Border, Palette().borderStrong);
		open_ = ImGui::BeginPopupModal(popup_id, nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoSavedSettings | extra);
		if (!open_)
			return;
		closed_ = DrawHeader(title);
	}
	~DialogScope() {
		if (open_)
			ImGui::EndPopup();
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar(4);
	}
	explicit operator bool() const { return open_; }
	// True when the user pressed the header X or Escape this frame; the caller
	// closes the popup so it can run its own cancel logic first.
	bool CloseRequested() const { return closed_ || ImGui::IsKeyPressed(ImGuiKey_Escape, false); }
	DialogScope(const DialogScope&) = delete;
	DialogScope& operator=(const DialogScope&) = delete;

private:
	bool DrawHeader(const char* title) {
		// Title row: title on the left, X pinned to the top-right.
		const float width = ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x;
		ImGui::PushStyleColor(ImGuiCol_Text, Palette().foreground);
		ImGui::TextUnformatted(title);
		ImGui::PopStyleColor();
		const float cursor_y = ImGui::GetCursorPosY() - ImGui::GetTextLineHeight() - ImGui::GetStyle().ItemSpacing.y;
		ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - 24.0f, cursor_y));
		const bool pressed = CloseButton("##dialog_close");
		(void)width;
		ImGui::Dummy(ImVec2(0.0f, 2.0f));
		ImGui::Separator();
		ImGui::Dummy(ImVec2(0.0f, 1.0f));
		return pressed;
	}

	bool open_ = false;
	bool closed_ = false;
};

// --------------------------------------------------------------------- popups
// Context menus and custom popups share the dialog chrome: 10 px outer padding,
// 8 px horizontal / 5 px vertical item spacing, 6 px rounded corners, a 1 px
// border, a minimum width, and a right margin so labels never touch the edge.
inline void PushPopupChrome() {
	const ThemePalette& palette = Palette();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 5.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
	ImGui::PushStyleColor(ImGuiCol_PopupBg, palette.surface);
	ImGui::PushStyleColor(ImGuiCol_Border, palette.borderStrong);
}

inline void PopPopupChrome() {
	ImGui::PopStyleColor(2);
	ImGui::PopStyleVar(4);
}

class PopupScope {
public:
	explicit PopupScope(const char* id, ImGuiWindowFlags flags = 0, float min_width = 190.0f)
		: open_(false) {
		if (ImGui::IsPopupOpen(id))
			ImGui::SetNextWindowSizeConstraints(ImVec2(min_width, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
		PushPopupChrome();
		open_ = ImGui::BeginPopup(id, flags);
	}
	~PopupScope() {
		if (open_)
			ImGui::EndPopup();
		PopPopupChrome();
	}
	explicit operator bool() const { return open_; }
	PopupScope(const PopupScope&) = delete;
	PopupScope& operator=(const PopupScope&) = delete;

private:
	bool open_ = false;
};

class ContextPopupScope {
public:
	explicit ContextPopupScope(const char* id = nullptr, float min_width = 190.0f)
		: open_(false) {
		if (id && ImGui::IsPopupOpen(id))
			ImGui::SetNextWindowSizeConstraints(ImVec2(min_width, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
		PushPopupChrome();
		open_ = ImGui::BeginPopupContextItem(id);
	}
	~ContextPopupScope() {
		if (open_)
			ImGui::EndPopup();
		PopPopupChrome();
	}
	explicit operator bool() const { return open_; }
	ContextPopupScope(const ContextPopupScope&) = delete;
	ContextPopupScope& operator=(const ContextPopupScope&) = delete;

private:
	bool open_ = false;
};

// --------------------------------------------------------------- empty state
inline void EmptyState(const char* text) {
	ImGui::Spacing();
	ImGui::PushStyleColor(ImGuiCol_Text, Palette().muted);
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextUnformatted(text);
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
	ImGui::Spacing();
}

// --------------------------------------------------------------- form helpers
// Checkbox on the current row, wrapping to a new line instead of clipping.
inline bool CheckboxFlow(const char* label, bool* value, bool first) {
	const float width = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
		ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
	if (!first) {
		if (RemainingWidth() < width + ImGui::GetStyle().ItemSpacing.x)
			ImGui::NewLine();
		else
			ImGui::SameLine();
	}
	return ImGui::Checkbox(label, value);
}

inline bool SameLineOrWrap(float width) {
	// `width` is normally the label's text width; add frame padding so a real
	// button also fits instead of overflowing the content region.
	// Measure from the previous item's right edge: after an item the cursor is
	// already on the next line, so GetCursorScreenPos() is not the row end.
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const ImGuiStyle& style = ImGui::GetStyle();
	float right = window->Pos.x + window->Size.x - style.WindowPadding.x - window->Scroll.x;
	if (window->ScrollbarY)
		right -= window->ScrollbarSizes.x;
	const float next = ImGui::GetItemRectMax().x + style.ItemSpacing.x;
	const float needed = width + 6.0f;
	if (next + needed > right) {
		ImGui::NewLine();
		return false;
	}
	ImGui::SameLine();
	return true;
}

} // namespace ToolUi
