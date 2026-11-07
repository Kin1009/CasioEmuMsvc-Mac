// Mini memory editor for Dear ImGui (to embed in your game/tools)
// Get latest version at http://www.github.com/ocornut/imgui_club
//
// Right-click anywhere to access the Options menu!
// You can adjust the keyboard repeat delay/rate in ImGuiIO.
// The code assume a mono-space font for simplicity!
// If you don't use the default font, use ImGui::PushFont()/PopFont() to switch to a mono-space font before calling this.
//
// Usage:
//   // Create a window and draw memory editor inside it:
//   static MemoryEditor mem_edit_1;
//   static char data[0x10000];
//   size_t data_size = 0x10000;
//   mem_edit_1.DrawWindow("Memory Editor", data, data_size);
//
// Usage:
//   // If you already have a window, use DrawContents() instead:
//   static MemoryEditor mem_edit_2;
//   ImGui::Begin("MyWindow")
//   mem_edit_2.DrawContents(this, sizeof(*this), (size_t)this);
//   ImGui::End();
//
// Changelog:
// - v0.10: initial version
// - v0.23 (2017/08/17): added to github. fixed right-arrow triggering a byte write.
// - v0.24 (2018/06/02): changed DragInt("Rows" to use a %d data format (which is desirable since imgui 1.61).
// - v0.25 (2018/07/11): fixed wording: all occurrences of "Rows" renamed to "Columns".
// - v0.26 (2018/08/02): fixed clicking on hex region
// - v0.30 (2018/08/02): added data preview for common data types
// - v0.31 (2018/10/10): added OptUpperCaseHex option to select lower/upper casing display [@samhocevar]
// - v0.32 (2018/10/10): changed signatures to use void* instead of unsigned char*
// - v0.33 (2018/10/10): added OptShowOptions option to hide all the interactive option setting.
// - v0.34 (2019/05/07): binary preview now applies endianness setting [@nicolasnoble]
// - v0.35 (2020/01/29): using ImGuiDataType available since Dear ImGui 1.69.
// - v0.36 (2020/05/05): minor tweaks, minor refactor.
// - v0.40 (2020/10/04): fix misuse of ImGuiListClipper API, broke with Dear ImGui 1.79. made cursor position appears on left-side of edit box. option popup appears on mouse release. fix MSVC warnings where _CRT_SECURE_NO_WARNINGS wasn't working in recent versions.
// - v0.41 (2020/10/05): fix when using with keyboard/gamepad navigation enabled.
// - v0.42 (2020/10/14): fix for . character in ASCII view always being greyed out.
// - v0.43 (2021/03/12): added OptFooterExtraHeight to allow for custom drawing at the bottom of the editor [@leiradel]
// - v0.44 (2021/03/12): use ImGuiInputTextFlags_AlwaysOverwrite in 1.82 + fix hardcoded width.
// - v0.50 (2021/11/12): various fixes for recent dear imgui versions (fixed misuse of clipper, relying on SetKeyboardFocusHere() handling scrolling from 1.85). added default size.
//
// Todo/Bugs:
// - This is generally old/crappy code, it should work but isn't very good.. to be rewritten some day.
// - PageUp/PageDown are supported because we use _NoNav. This is a good test scenario for working out idioms of how to mix natural nav and our own...
// - Arrows are being sent to the InputText() about to disappear which for LeftArrow makes the text cursor appear at position 1 for one frame.
// - Using InputText() is awkward and maybe overkill here, consider implementing something custom.

#pragma once

#include "imgui/imgui.h"
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <cmath>
#include <optional>
#include <stdint.h> // uint8_t, etc.
#include <stdio.h>	// sprintf, scanf
#include <string>
#include <vector>

#include "MemoryTextMap.hpp"

#include "Config.hpp"
#include "Localization.h"
#include "ThemeManager.h"

#ifdef _MSC_VER
#define _PRISizeT "I"
#define ImSnprintf _snprintf
#else
#define _PRISizeT "z"
#define ImSnprintf snprintf
#endif

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // warning C4996: 'sprintf': This function or variable may be unsafe.
#endif

struct MemoryEditor {
	enum DataFormat {
		DataFormat_Bin = 0,
		DataFormat_Dec = 1,
		DataFormat_Hex = 2,
		DataFormat_COUNT
	};

	// Settings
	bool Open;										   // = true   // set to false when DrawWindow() was closed. ignore if not using DrawWindow().
	bool ReadOnly;									   // = false  // disable any editing.
	int Cols;										   // = 16     // number of columns to display.
	// Span fill opacity override, shared by every memory view (0 = invisible,
	// 1 = solid, negative = use the theme default).
	static inline float SpanTintOpacity = -1.0f;
	static float DefaultSpanTintOpacity(bool dark) { return dark ? 0.34f : 0.30f; }
	static float EffectiveSpanTintOpacity(bool dark) {
		return SpanTintOpacity >= 0.0f ? SpanTintOpacity : DefaultSpanTintOpacity(dark);
	}
	bool OptShowOptions;							   // = true   // display options button/context menu. when disabled, options will be locked unless you provide your own UI for them.
	bool OptShowDataPreview;						   // = false  // display a footer previewing the decimal/binary/hex/float representation of the currently selected bytes.
	bool OptShowHexII;								   // = false  // display values in HexII representation instead of regular hexadecimal: hide null/zero bytes, ascii values as ".X".
	bool OptShowAscii;								   // = true   // display ASCII representation on the right side.
	bool OptGreyOutZeroes;							   // = true   // display null/zero bytes using the TextDisabled color.
	bool OptUpperCaseHex;							   // = true   // display hexadecimal values as "FF" instead of "ff".
	int OptMidColsCount;							   // = 8      // set to 0 to disable extra spacing between every mid-cols.
	int OptAddrDigitsCount;							   // = 0      // number of addr digits to display (default calculated based on maximum displayed addr).
	float OptFooterExtraHeight;						   // = 0      // space to reserve at the bottom of the widget to add custom widgets
	ImU32 HighlightColor;							   //          // background color of highlighted bytes.
	ImU8 (*ReadFn)(const ImU8* data, size_t off);	   // = 0      // optional handler to read bytes.
	void (*WriteFn)(ImU8* data, size_t off, ImU8 d);   // = 0      // optional handler to write bytes.
	bool (*HighlightFn)(const ImU8* data, size_t off); //= 0      // optional handler to return Highlight property (to support non-contiguous highlighting).
	void* contextmenuuserdata = 0;
	void (*ContextMenuFn)(void* userdata, size_t off){};

	// [Internal State]
	bool ContentsWidthChanged;
	size_t DataPreviewAddr;
	size_t DataEditingAddr;
	bool DataEditingTakeFocus;
	char DataInputBuf[32];
	char AddrInputBuf[32];
	size_t GotoAddr;
	size_t HighlightMin, HighlightMax;
	int PreviewEndianess;
	ImGuiDataType PreviewDataType;
	float* ram_edit_ov = 0;

	MemoryEditor() {
		// Settings
		Open = true;
		ReadOnly = false;
#ifdef __ANDROID__
		Cols = 8;
#else
		Cols = 16;
#endif
		OptShowOptions = true;
		OptShowDataPreview = false;
		OptShowHexII = false;
		OptShowAscii = true;
		OptGreyOutZeroes = true;
		OptUpperCaseHex = true;
		OptMidColsCount = 8;
		OptAddrDigitsCount = 0;
		OptFooterExtraHeight = 0.0f;
		HighlightColor = IM_COL32(255, 255, 255, 50);
		ReadFn = NULL;
		WriteFn = NULL;
		HighlightFn = NULL;

		// State/Internals
		ContentsWidthChanged = false;
		DataPreviewAddr = DataEditingAddr = (size_t)-1;
		DataEditingTakeFocus = false;
		memset(DataInputBuf, 0, sizeof(DataInputBuf));
		memset(AddrInputBuf, 0, sizeof(AddrInputBuf));
		GotoAddr = (size_t)-1;
		HighlightMin = HighlightMax = (size_t)-1;
		PreviewEndianess = 0;
		PreviewDataType = ImGuiDataType_S32;
	}

	void GotoAddrAndHighlight(size_t addr_min, size_t addr_max) {
		GotoAddr = addr_min;
		HighlightMin = addr_min;
		HighlightMax = addr_max;
	}
	// Range selection and text decoding.
	size_t SelStart = (size_t)-1;
	size_t SelEnd = (size_t)-1;
	bool SelDragging = false;
	bool SelectionMenuRequested = false;
	bool ShowTextPanel = false;
	bool ShowSpansPanel = false;
	bool InlineSelectionShown = false;
	int InlineRevealAtFrame = 0;
	mutable int DimTextFrame = -1;
	mutable ImVec4 DimTextCache{};
	bool SelSpansEnabled = false;
	std::function<bool(size_t)> SelSpansAllowed;
	ImVec2 SelPopupPos{};
	char SelPopupId[48]{};
	char CopyNotice[64]{};
	int CopyNoticeFrame = -1000;
	int SelTestStage = 0;
	bool SelTestParsed = false;
	bool GotoEnvApplied = false;
	bool SelTestActive = false;
	size_t SelTestStartAddr = 0;
	size_t SelTestEndAddr = 0;

	struct MarkedSpan {
		size_t start{};
		size_t length{};
		ImColor color{};
		const char* desc{};
		bool operator<(const MarkedSpan& b) const {
			return start < b.start;
		}
	};
	using OptionalMarkedSpans = std::optional<std::vector<MarkedSpan>>;

	const char* GetSelectionPopupId() {
		if (SelPopupId[0] == '\0')
			std::snprintf(SelPopupId, sizeof(SelPopupId), "##mem_sel_%p", (void*)this);
		return SelPopupId;
	}

	bool HasSelection() const {
		return SelStart != (size_t)-1 && SelEnd != (size_t)-1;
	}

	void ClearSelection() {
		SelStart = SelEnd = (size_t)-1;
		SelDragging = false;
		SelectionMenuRequested = false;
		ShowTextPanel = false;
		ShowSpansPanel = false;
		InlineRevealAtFrame = 0;
	}

	void NormalizeSelection(size_t& first, size_t& last) const {
		first = SelStart < SelEnd ? SelStart : SelEnd;
		last = SelStart < SelEnd ? SelEnd : SelStart;
	}

	void SelectByte(size_t addr, bool extend) {
		// Clicking a byte leaves the single-byte editor so Cmd+C always
		// targets the range instead of a focused hex input.
		DataEditingAddr = DataPreviewAddr = (size_t)-1;
		DataEditingTakeFocus = false;
		if (extend && HasSelection())
			SelEnd = addr;
		else
			SelStart = SelEnd = addr;
		SelDragging = true;
		// The inline selection panel is visible by default for every new
		// selection.  The right-click menu can still hide either section.
		ShowTextPanel = true;
		ShowSpansPanel = true;
	}

	void ExtendSelection(size_t addr) {
		SelEnd = addr;
	}

	void ExtendSelectionBy(long delta, size_t mem_size) {
		if (!HasSelection() || delta == 0)
			return;
		const long value = static_cast<long>(SelEnd) + delta;
		if (value < 0)
			SelEnd = 0;
		else if (static_cast<size_t>(value) >= mem_size)
			SelEnd = mem_size > 0 ? mem_size - 1 : 0;
		else
			SelEnd = static_cast<size_t>(value);
	}

	static const char* LookupGlyph(uint8_t page, uint8_t code) {
		int low = 0;
		int high = MEMORY_GLYPH_COUNT - 1;
		while (low <= high) {
			const int middle = (low + high) / 2;
			const auto& glyph = MEMORY_GLYPHS[middle];
			if (glyph.page == page && glyph.code == code)
				return glyph.utf8;
			if (glyph.page < page || (glyph.page == page && glyph.code < code))
				low = middle + 1;
			else
				high = middle - 1;
		}
		return nullptr;
	}

	ImU8 ReadByte(const ImU8* data, size_t addr, size_t mem_size) const {
		if (addr >= mem_size)
			return 0;
		return ReadFn ? ReadFn(data, addr) : data[addr];
	}

	std::string SelectionHex(const ImU8* data, size_t mem_size) const {
		if (!HasSelection())
			return {};
		size_t first = 0, last = 0;
		NormalizeSelection(first, last);
		std::string text;
		text.reserve((last - first + 1) * 3);
		char byte[4];
		for (size_t addr = first; addr <= last && addr < mem_size; ++addr) {
			std::snprintf(byte, sizeof(byte), "%02X", ReadByte(data, addr, mem_size));
			if (!text.empty())
				text += ' ';
			text += byte;
		}
		return text;
	}

	std::string SelectionText(const ImU8* data, size_t mem_size) const {
		if (!HasSelection())
			return {};
		size_t first = 0, last = 0;
		NormalizeSelection(first, last);
		std::string text;
		char escape[8];
		for (size_t addr = first; addr <= last; ++addr) {
			const uint8_t byte = ReadByte(data, addr, mem_size);
			if (byte == 0) {
				text += ' ';
				continue;
			}
			if (byte >= 0x20 && byte < 0x7F) {
				text += static_cast<char>(byte);
				continue;
			}
			if (byte >= 0xF0 && addr + 1 <= last) {
				if (const char* glyph = LookupGlyph(byte, ReadByte(data, addr + 1, mem_size))) {
					text += glyph;
					++addr;
					continue;
				}
			}
			if (byte >= 0xF0) {
				std::snprintf(escape, sizeof(escape), "\\x%02X", byte);
				text += escape;
				continue;
			}
			if (const char* glyph = LookupGlyph(0, byte)) {
				text += glyph;
				continue;
			}
			std::snprintf(escape, sizeof(escape), "\\x%02X", byte);
			text += escape;
		}
		return text;
	}

	void DrawCopyNotice(ImDrawList* draw_list) const {
		if (CopyNotice[0] == '\0' || ImGui::GetFrameCount() - CopyNoticeFrame >= 120)
			return;
		const ImVec2 text_size = ImGui::CalcTextSize(CopyNotice);
		const ImVec2 window_pos = ImGui::GetWindowPos();
		const ImVec2 window_size = ImGui::GetWindowSize();
		const ImVec2 pos(window_pos.x + window_size.x - text_size.x - 16.0f,
			window_pos.y + window_size.y - text_size.y - 12.0f);
		const ImVec2 padding(8.0f, 5.0f);
		draw_list->AddRectFilled(ImVec2(pos.x - padding.x, pos.y - padding.y),
			ImVec2(pos.x + text_size.x + padding.x, pos.y + text_size.y + padding.y),
			ImGui::GetColorU32(ImGuiCol_PopupBg, 0.95f), 6.0f);
		draw_list->AddRect(ImVec2(pos.x - padding.x, pos.y - padding.y),
			ImVec2(pos.x + text_size.x + padding.x, pos.y + text_size.y + padding.y),
			ImGui::GetColorU32(ImGuiCol_Border), 6.0f);
		draw_list->AddText(pos, ImGui::GetColorU32(ImGuiCol_Text), CopyNotice);
	}

	void CopySelection(bool as_text, const ImU8* data, size_t mem_size) {
		const std::string text = as_text ? SelectionText(data, mem_size) : SelectionHex(data, mem_size);
		if (text.empty())
			return;
		ImGui::SetClipboardText(text.c_str());
		size_t first = 0, last = 0;
		NormalizeSelection(first, last);
		if (as_text)
			std::snprintf(CopyNotice, sizeof(CopyNotice), "Copied text");
		else
			std::snprintf(CopyNotice, sizeof(CopyNotice), "Copied %d bytes", static_cast<int>(last - first + 1));
		CopyNoticeFrame = ImGui::GetFrameCount();
	}

	struct Sizes {
		int AddrDigitsCount;
		float LineHeight;
		float GlyphWidth;
		float HexCellWidth;
		float SpacingBetweenMidCols;
		float PosHexStart;
		float PosHexEnd;
		float PosAsciiStart;
		float PosAsciiEnd;
		float WindowWidth;

		Sizes() { memset(this, 0, sizeof(*this)); }
	};

	void CalcSizes(Sizes& s, size_t mem_size, size_t base_display_addr) {
		ImGuiStyle& style = ImGui::GetStyle();
		s.AddrDigitsCount = OptAddrDigitsCount;
		if (s.AddrDigitsCount == 0)
			for (size_t n = base_display_addr + mem_size - 1; n > 0; n >>= 4)
				s.AddrDigitsCount++;
		s.LineHeight = ImGui::GetTextLineHeight();
		s.GlyphWidth = ImGui::CalcTextSize("F").x + 1;					// We assume the font is mono-space
		s.HexCellWidth = (float)(int)(s.GlyphWidth * 2.5f);				// "FF " we include trailing space in the width to easily catch clicks everywhere
		s.SpacingBetweenMidCols = (float)(int)(s.HexCellWidth * 0.25f); // Every OptMidColsCount columns we add a bit of extra spacing
		s.PosHexStart = (s.AddrDigitsCount + 2) * s.GlyphWidth;
		s.PosHexEnd = s.PosHexStart + (s.HexCellWidth * Cols);
		s.PosAsciiStart = s.PosAsciiEnd = s.PosHexEnd;
		if (OptShowAscii) {
			s.PosAsciiStart = s.PosHexEnd + s.GlyphWidth * 1;
			if (OptMidColsCount > 0)
				s.PosAsciiStart += (float)((Cols + OptMidColsCount - 1) / OptMidColsCount) * s.SpacingBetweenMidCols;
			s.PosAsciiEnd = s.PosAsciiStart + Cols * s.GlyphWidth;
		}
		s.WindowWidth = s.PosAsciiEnd + style.ScrollbarSize + style.WindowPadding.x * 2 + s.GlyphWidth;
	}
	void DrawSelection(ImDrawList* draw_list, const Sizes& s, const ImVec2& line_min,
		size_t line_i, size_t line_start, size_t line_end) const {
		if (!HasSelection())
			return;
		size_t first = 0, last = 0;
		NormalizeSelection(first, last);
		if (last < line_start || first > line_end)
			return;
		const size_t first_col = std::max(first, line_start) - line_start;
		const size_t last_col = std::min(last, line_end) - line_start;
		const int mid = OptMidColsCount;
		const float group = (mid > 0) ? s.SpacingBetweenMidCols : 0.0f;
		const float trailing = s.HexCellWidth - ImGui::CalcTextSize("00").x;
		auto cell_x = [&](size_t n) {
			return line_min.x + s.PosHexStart + s.HexCellWidth * static_cast<float>(n) +
				(mid > 0 ? static_cast<float>(n / static_cast<size_t>(mid)) * group : 0.0f);
		};
		const ImU32 color = ImGui::GetColorU32(ImGuiCol_TextSelectedBg, 0.5f);
		// Draw one block per 8-byte group.  A 1..7 byte selection therefore
		// cannot look like a full line, and the group gap stays visible.
		for (size_t segment_first = first_col; segment_first <= last_col;) {
			size_t segment_last = last_col;
			if (mid > 0) {
				const size_t boundary = (segment_first / static_cast<size_t>(mid) + 1) *
					static_cast<size_t>(mid);
				if (boundary <= last_col)
					segment_last = boundary - 1;
			}
			const float x0 = cell_x(segment_first);
			const float x1 = cell_x(segment_last) + s.HexCellWidth - trailing;
			draw_list->AddRectFilled(ImVec2(x0, line_min.y + 1.0f),
				ImVec2(x1, line_min.y + s.LineHeight - 1.0f), color, 3.0f);
			if (std::getenv("CASIOEMU_HEX_SPAN_REPORT")) {
				std::fprintf(stderr,
					"[selrect] row=%zu cols=%d seg_first=%zu seg_last=%zu x0=%.2f x1=%.2f\n",
					line_i, Cols, segment_first, segment_last, x0, x1);
			}
			if (segment_last >= last_col)
				break;
			segment_first = segment_last + 1;
		}
	}
	std::optional<size_t> HitTestAddress(const Sizes& s, const ImVec2& line_min,
		size_t line_start, size_t line_end, const ImVec2& mouse) const {
		const int mid = OptMidColsCount;
		const float group = (mid > 0) ? s.SpacingBetweenMidCols : 0.0f;
		for (int n = 0; n < Cols && line_start + static_cast<size_t>(n) <= line_end; ++n) {
			const float x0 = line_min.x + s.PosHexStart +
				s.HexCellWidth * static_cast<float>(n) +
				(mid > 0 ? static_cast<float>(n / mid) * group : 0.0f);
			if (mouse.x >= x0 && mouse.x < x0 + s.HexCellWidth)
				return line_start + static_cast<size_t>(n);
		}
		if (OptShowAscii) {
			const float ascii_x0 = line_min.x + s.PosAsciiStart;
			const float ascii_x1 = ascii_x0 + s.GlyphWidth * static_cast<float>(Cols);
			if (mouse.x >= ascii_x0 && mouse.x < ascii_x1) {
				const size_t column = static_cast<size_t>((mouse.x - ascii_x0) /
					std::max(1.0f, s.GlyphWidth));
				if (column < static_cast<size_t>(Cols) && line_start + column <= line_end)
					return line_start + column;
			}
		}
		return std::nullopt;
	}

	OptionalMarkedSpans TestMarkedSpans() const {
		const char* spec = std::getenv("CASIOEMU_HEX_SPAN_TEST");
		OptionalMarkedSpans result;
		if (!spec || !*spec)
			return result;
		result.emplace();
		std::string text = spec;
		size_t begin = 0;
		int index = 0;
		while (begin <= text.size()) {
			const size_t end = text.find(';', begin);
			const std::string token = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
			unsigned long long start = 0;
			unsigned long long length = 0;
			if (std::sscanf(token.c_str(), "%llx:%llx", &start, &length) == 2 && length > 0) {
				MarkedSpan span;
				span.start = static_cast<size_t>(start);
				span.length = static_cast<size_t>(length);
				span.color = (index % 2) ? ImColor(40, 40, 150, 255) : ImColor(40, 150, 40, 255);
				span.desc = (index % 2) ? "test span B" : "test span A";
				result->push_back(span);
				++index;
			}
			if (end == std::string::npos)
				break;
			begin = end + 1;
		}
		if (result->empty())
			result.reset();
		return result;
	}

	static float SrgbLuminance(const ImVec4& color) {
		auto linear = [](float value) {
			return value <= 0.03928f ? value / 12.92f :
				std::pow((value + 0.055f) / 1.055f, 2.4f);
		};
		return 0.2126f * linear(color.x) + 0.7152f * linear(color.y) +
			0.0722f * linear(color.z);
	}

	static ImVec4 Composite(const ImVec4& foreground, const ImVec4& background) {
		const float alpha = foreground.w;
		return ImVec4(foreground.x * alpha + background.x * (1.0f - alpha),
			foreground.y * alpha + background.y * (1.0f - alpha),
			foreground.z * alpha + background.z * (1.0f - alpha), 1.0f);
	}

	static float ContrastRatio(const ImVec4& a, const ImVec4& b) {
		const float high = std::max(SrgbLuminance(a), SrgbLuminance(b));
		const float low = std::min(SrgbLuminance(a), SrgbLuminance(b));
		return (high + 0.05f) / (low + 0.05f);
	}

	static ImVec4 EnsureContrast(const ImVec4& foreground, const ImVec4& background,
		float target = 4.6f) {
		if (ContrastRatio(Composite(foreground, background), background) >= target)
			return foreground;
		const ImVec4 normal = ImGui::GetStyleColorVec4(ImGuiCol_Text);
		float low = 0.0f;
		float high = 1.0f;
		ImVec4 candidate = normal;
		for (int step = 0; step < 12; ++step) {
			const float blend = (low + high) * 0.5f;
			candidate = ImVec4(
				foreground.x + (normal.x - foreground.x) * blend,
				foreground.y + (normal.y - foreground.y) * blend,
				foreground.z + (normal.z - foreground.z) * blend,
				foreground.w + (normal.w - foreground.w) * blend);
			if (ContrastRatio(Composite(candidate, background), background) >= target)
				high = blend;
			else
				low = blend;
		}
		return candidate;
	}

	ImVec4 EditorBackground() const {
		ImVec4 background = ImGui::GetStyleColorVec4(ImGuiCol_ChildBg);
		if (background.w < 0.01f)
			background = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
		if (background.w < 0.01f)
			background = ImVec4(0.10f, 0.10f, 0.10f, 1.0f);
		background.w = 1.0f;
		return background;
	}

	struct SpanTintInfo {
		ImVec4 fill{};
		ImVec4 border{};
		ImU32 fill_u32 = 0;
		ImU32 border_u32 = 0;
		bool add_border = false;
		float normal_contrast = 0.0f;
		float dim_contrast = 0.0f;
		float base_dim_contrast = 0.0f;
		bool dark = true;
	};

	// Hex digits and ASCII glyphs are drawn brighter than ordinary UI text so
	// they stay readable on the darker span tints.
	ImVec4 LighterTextColor() const {
		ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_Text);
		const float lift = 0.22f;
		c.x += (1.0f - c.x) * lift;
		c.y += (1.0f - c.y) * lift;
		c.z += (1.0f - c.z) * lift;
		return c;
	}

	SpanTintInfo SpanTint(ImU32 packed) const {
		ImVec4 source = ImGui::ColorConvertU32ToFloat4(packed);
		if (source.w <= 0.0f && source.x <= 0.0f && source.y <= 0.0f && source.z <= 0.0f)
			source = ImGui::ColorConvertU32ToFloat4(IM_COL32(110, 150, 210, 255));
		const ImVec4 background = EditorBackground();
		const bool dark = SrgbLuminance(background) < 0.45f;
		float hue = 0.0f, saturation = 0.0f, value = 0.0f;
		ImGui::ColorConvertRGBtoHSV(source.x, source.y, source.z, hue, saturation, value);
		// Keep the semantic hue, but never paint the raw model colour.  The
		// tint stays close to the editor background on both sides of the
		// luminance scale so normal and dimmed text keep their contrast.
		saturation = 0.30f;
		value = dark ? 0.36f : 0.70f;
		ImVec4 fill;
		ImGui::ColorConvertHSVtoRGB(hue, saturation, value, fill.x, fill.y, fill.z);
		const float opacity = EffectiveSpanTintOpacity(dark);
		fill.w = opacity;
		SpanTintInfo info;
		info.fill = fill;
		info.dark = dark;
		info.border = fill;
		info.border.w = std::clamp(opacity * 1.5f, 0.0f, 1.0f);
		// The outline, not the fill, makes the region obvious.
		info.add_border = true;
		info.fill_u32 = ImGui::ColorConvertFloat4ToU32(fill);
		info.border_u32 = ImGui::ColorConvertFloat4ToU32(info.border);
		const ImVec4 tinted_background = Composite(fill, background);
		const ImVec4 normal_text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
		const ImVec4 dim_text = DimTextColor();
		info.normal_contrast = ContrastRatio(Composite(normal_text, tinted_background), tinted_background);
		info.dim_contrast = ContrastRatio(Composite(dim_text, tinted_background), tinted_background);
		info.base_dim_contrast = ContrastRatio(Composite(dim_text, background), background);
		return info;
	}

	ImVec4 DimTextColor() const {
		const int frame = ImGui::GetFrameCount();
		if (DimTextFrame == frame)
			return DimTextCache;
		const ImVec4 background = EditorBackground();
		const bool dark = SrgbLuminance(background) < 0.45f;
		const float value = dark ? 0.36f : 0.70f;
		const float alpha = EffectiveSpanTintOpacity(dark);
		ImVec4 candidate = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
		// Sample the hue circle and raise the disabled text colour only as far
		// as the worst tinted background requires.
		for (int step = 0; step < 12; ++step) {
			ImVec4 tint;
			ImGui::ColorConvertHSVtoRGB(static_cast<float>(step) / 12.0f, 0.30f, value,
				tint.x, tint.y, tint.z);
			tint.w = alpha;
			candidate = EnsureContrast(candidate, Composite(tint, background));
		}
		DimTextFrame = frame;
		DimTextCache = candidate;
		return candidate;
	}

	void DrawSpanLine(ImDrawList* draw_list, const Sizes& s, const ImVec2& line_min,
		size_t line_i, size_t line_start_abs, size_t line_end_abs,
		const std::vector<MarkedSpan>& spans) const {
		const int mid = OptMidColsCount;
		const float group = (mid > 0) ? s.SpacingBetweenMidCols : 0.0f;
		const float trailing = s.HexCellWidth - ImGui::CalcTextSize("00").x;
		const float hex_origin = line_min.x + s.PosHexStart;
		auto cell_x = [&](size_t n) {
			return hex_origin + s.HexCellWidth * static_cast<float>(n) +
				(mid > 0 ? static_cast<float>(n / static_cast<size_t>(mid)) * group : 0.0f);
		};
		struct Intersection {
			size_t span = 0;
			size_t first = 0;
			size_t last = 0;
		};
		std::vector<Intersection> intersections;
		std::vector<int> overlap(static_cast<size_t>(Cols), 0);
		for (size_t span_index = 0; span_index < spans.size(); ++span_index) {
			const MarkedSpan& span = spans[span_index];
			if (span.length == 0 || span.start > line_end_abs)
				continue;
			const size_t span_last = span.start + span.length - 1;
			if (span_last < line_start_abs)
				continue;
			const size_t first = std::max(span.start, line_start_abs) - line_start_abs;
			const size_t last = std::min(span_last, line_end_abs) - line_start_abs;
			if (first >= static_cast<size_t>(Cols))
				continue;
			intersections.push_back({span_index, first, last});
			for (size_t column = first; column <= last && column < static_cast<size_t>(Cols); ++column)
				++overlap[column];
		}
		for (const Intersection& intersection : intersections) {
			const MarkedSpan& span = spans[intersection.span];
			const SpanTintInfo tint = SpanTint(static_cast<ImU32>(span.color));
			for (size_t segment_first = intersection.first; segment_first <= intersection.last;) {
				size_t group_end = intersection.last;
				if (mid > 0) {
					const size_t boundary = (segment_first / static_cast<size_t>(mid) + 1) *
						static_cast<size_t>(mid);
					if (boundary <= group_end)
						group_end = boundary - 1;
				}
				// Split where overlap changes so the per-span alpha always sums
				// to the same budget.  Overlapping spans never stack opaque.
				// The split may not cross the 8-byte group boundary.
				size_t segment_last = segment_first;
				while (segment_last < group_end &&
					overlap[segment_last + 1] == overlap[segment_first])
					++segment_last;
				const int stack = std::max(1, overlap[segment_first]);
				const float alpha = tint.fill.w / static_cast<float>(stack);
				ImVec4 fill = tint.fill;
				fill.w = alpha;
				const ImU32 fill_u32 = ImGui::ColorConvertFloat4ToU32(fill);
				const float x0 = cell_x(segment_first);
				const float x1 = cell_x(segment_last) + s.HexCellWidth - trailing;
				draw_list->AddRectFilled(ImVec2(x0, line_min.y + 1.0f),
					ImVec2(x1, line_min.y + s.LineHeight - 1.0f), fill_u32, 3.0f);
				if (tint.add_border) {
					draw_list->AddRect(ImVec2(x0 + 0.5f, line_min.y + 1.5f),
						ImVec2(x1 - 0.5f, line_min.y + s.LineHeight - 1.5f),
						tint.border_u32, 3.0f, 0, 1.0f);
				}
				if (std::getenv("CASIOEMU_HEX_SPAN_REPORT")) {
					std::fprintf(stderr,
						"[span] row=%zu span=%zu cols=%d first=%zu last=%zu seg_first=%zu seg_last=%zu "
						"x0=%.2f x1=%.2f hex_origin=%.2f cell_w=%.2f group=%.2f text_w=%.2f mid=%d "
						"y=%.2f h=%.2f bytes=%zu alpha=%.3f normal_contrast=%.2f dim_contrast=%.2f "
						"base_dim_contrast=%.2f\n",
						line_i, intersection.span, Cols, intersection.first, intersection.last,
						segment_first, segment_last, x0, x1, hex_origin, s.HexCellWidth, group,
						ImGui::CalcTextSize("00").x, mid, line_min.y + 1.0f,
						s.LineHeight - 2.0f, segment_last - segment_first + 1, alpha,
						tint.normal_contrast, tint.dim_contrast, tint.base_dim_contrast);
				}
				if (segment_last >= intersection.last)
					break;
				segment_first = segment_last + 1;
			}
		}
	}

	float InlinePanelHeight(bool /*spans_allowed*/) const {
		if (!HasSelection() || SelDragging || ImGui::GetFrameCount() < InlineRevealAtFrame ||
			(!ShowTextPanel && !ShowSpansPanel))
			return 0.0f;
		ImGuiStyle& style = ImGui::GetStyle();
		return style.ItemSpacing.y + ImGui::GetTextLineHeightWithSpacing() +
			style.ItemSpacing.y + 60.0f + style.ItemSpacing.y;
	}

	void RenderSelectionInline(const ImU8* mem_data, size_t mem_size, size_t base_display_addr,
		const std::vector<MarkedSpan>* spans, bool spans_allowed) {
		InlineSelectionShown = false;
		if (!HasSelection() || SelDragging || ImGui::GetFrameCount() < InlineRevealAtFrame ||
			(!ShowTextPanel && !ShowSpansPanel))
			return;
		size_t first = 0, last = 0;
		NormalizeSelection(first, last);
		const size_t absolute_first = first + base_display_addr;
		const size_t absolute_last = last + base_display_addr;
		const std::string decoded = SelectionText(mem_data, mem_size);
		ImGui::PushID(this);
		ImGui::Separator();
		ImGui::TextDisabled("Selection %zX..%zX (%d bytes)", absolute_first, absolute_last,
			static_cast<int>(last - first + 1));
		ImGui::SameLine();
		if (ImGui::Button("Copy"))
			CopySelection(true, mem_data, mem_size);
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
		ImGui::BeginChild("##sel_inline", ImVec2(0.0f, 60.0f),
			ImGuiChildFlags_Border | ImGuiChildFlags_AlwaysUseWindowPadding);
		if (ShowSpansPanel && spans_allowed && spans) {
			bool any = false;
			ImGui::TextUnformatted("Spans:");
			ImDrawList* swatch_list = ImGui::GetWindowDrawList();
			for (const auto& span : *spans) {
				if (span.length == 0)
					continue;
				const size_t span_last = span.start + span.length - 1;
				if (span.start > absolute_last || span_last < absolute_first)
					continue;
				any = true;
				ImGui::SameLine();
				const SpanTintInfo tint = SpanTint(static_cast<ImU32>(span.color));
				const ImVec2 swatch = ImGui::GetCursorScreenPos();
				swatch_list->AddRectFilled(ImVec2(swatch.x, swatch.y + 3.0f),
					ImVec2(swatch.x + 9.0f, swatch.y + 12.0f), tint.fill_u32, 2.0f);
				if (tint.add_border) {
					swatch_list->AddRect(ImVec2(swatch.x, swatch.y + 3.0f),
						ImVec2(swatch.x + 9.0f, swatch.y + 12.0f), tint.border_u32, 2.0f, 0, 1.0f);
				}
				ImGui::Dummy(ImVec2(11.0f, 10.0f));
				ImGui::SameLine();
				ImGui::TextUnformatted(span.desc ? span.desc : "(unnamed span)");
			}
			if (!any) {
				ImGui::SameLine();
				ImGui::TextDisabled("(none)");
			}
		}
		if (ShowTextPanel) {
			const bool blank = decoded.find_first_not_of(" \t\r\n") == std::string::npos;
			ImGui::TextWrapped("Text: %s", blank ? "(blank)" : decoded.c_str());
		}
		ImGui::EndChild();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(2);
		ImGui::PopID();
		InlineSelectionShown = true;
	}

	void DrawContents(void* mem_data_void, size_t mem_size, size_t base_display_addr = 0x0000) {
		if (Cols < 1)
			Cols = 1;

		ImU8* mem_data = (ImU8*)mem_data_void;
		Sizes s;
		CalcSizes(s, mem_size, base_display_addr);
		ImGuiStyle& style = ImGui::GetStyle();
		if (!SelTestParsed) {
			SelTestParsed = true;
			if (const char* spec = std::getenv("CASIOEMU_HEX_SEL_TEST")) {
				unsigned start = 0, end = 0;
				if (std::sscanf(spec, "%x,%x", &start, &end) == 2) {
					SelTestActive = true;
					SelTestStartAddr = start;
					SelTestEndAddr = end;
				}
			}
			if (SelTestActive) {
				std::fprintf(stderr, "[sel] glyph F110=%s F410=%s\n",
					LookupGlyph(0xF1, 0x10) ? LookupGlyph(0xF1, 0x10) : "?",
					LookupGlyph(0xF4, 0x10) ? LookupGlyph(0xF4, 0x10) : "?");
			}
		}

		const OptionalMarkedSpans test_spans = TestMarkedSpans();
		const std::vector<MarkedSpan>* spans_ptr = test_spans ? &test_spans.value() : nullptr;
		const bool spans_allowed = spans_ptr != nullptr;

		// We begin into our scrolling region with the 'ImGuiWindowFlags_NoMove' in order to prevent click from moving the window.
		// This is used as a facility since our main click detection code doesn't assign an ActiveId so the click would normally be caught as a window-move.
		const float height_separator = style.ItemSpacing.y + style.ItemInnerSpacing.y;
		float footer_height = OptFooterExtraHeight;
		if (OptShowOptions)
			footer_height += height_separator + ImGui::GetFrameHeightWithSpacing() * 1 + 14.0f;
		if (OptShowDataPreview)
			footer_height += height_separator + ImGui::GetFrameHeightWithSpacing() * 1 + ImGui::GetTextLineHeightWithSpacing() * 3;
		footer_height += InlinePanelHeight(spans_allowed);
		ImGui::BeginChild("##scrolling", ImVec2(0, -footer_height), false, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav);
		ImDrawList* draw_list = ImGui::GetWindowDrawList();

		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

		// We are not really using the clipper API correctly here, because we rely on visible_start_addr/visible_end_addr for our scrolling function.
		const int line_total_count = (int)((mem_size + Cols - 1) / Cols);
		ImGuiListClipper clipper;
		clipper.Begin(line_total_count, s.LineHeight);

		bool data_next = false;

		if (ReadOnly || DataEditingAddr >= mem_size)
			DataEditingAddr = (size_t)-1;
		if (DataPreviewAddr >= mem_size)
			DataPreviewAddr = (size_t)-1;

		size_t preview_data_type_size = OptShowDataPreview ? DataTypeGetSize(PreviewDataType) : 0;

		size_t data_editing_addr_next = (size_t)-1;
		if (DataEditingAddr != (size_t)-1) {
			// Move cursor but only apply on next frame so scrolling with be synchronized (because currently we can't change the scrolling while the window is being rendered)
			if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && (ptrdiff_t)DataEditingAddr >= (ptrdiff_t)Cols) {
				data_editing_addr_next = DataEditingAddr - Cols;
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && (ptrdiff_t)DataEditingAddr < (ptrdiff_t)mem_size - Cols) {
				data_editing_addr_next = DataEditingAddr + Cols;
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && (ptrdiff_t)DataEditingAddr > (ptrdiff_t)0) {
				data_editing_addr_next = DataEditingAddr - 1;
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && (ptrdiff_t)DataEditingAddr < (ptrdiff_t)mem_size - 1) {
				data_editing_addr_next = DataEditingAddr + 1;
			}
		}

		// Draw vertical separator
		ImVec2 window_pos = ImGui::GetWindowPos();
		if (OptShowAscii)
			draw_list->AddLine(ImVec2(window_pos.x + s.PosAsciiStart - s.GlyphWidth, window_pos.y), ImVec2(window_pos.x + s.PosAsciiStart - s.GlyphWidth, window_pos.y + 9999), ImGui::GetColorU32(ImGuiCol_Border));

		const ImU32 color_text = ImGui::ColorConvertFloat4ToU32(LighterTextColor());
		const ImU32 color_disabled = OptGreyOutZeroes
			? ImGui::ColorConvertFloat4ToU32(DimTextColor()) : color_text;

		const char* format_address = OptUpperCaseHex ? "%0*" _PRISizeT "X: " : "%0*" _PRISizeT "x: ";
		const char* format_data = OptUpperCaseHex ? "%0*" _PRISizeT "X" : "%0*" _PRISizeT "x";
		const char* format_byte = OptUpperCaseHex ? "%02X" : "%02x";
		const char* format_byte_space = OptUpperCaseHex ? "%02X " : "%02x ";

		bool hovered_any_byte = false;
		while (clipper.Step())
			for (int line_i = clipper.DisplayStart; line_i < clipper.DisplayEnd; line_i++) // display only visible lines
			{
				size_t addr = (size_t)(line_i * Cols);
				ImGui::Text(format_address, s.AddrDigitsCount, base_display_addr + addr);

				const ImVec2 line_min = ImGui::GetItemRectMin();
				const size_t line_start_sel = static_cast<size_t>(line_i * Cols);
				const size_t line_end_sel = std::min(line_start_sel + Cols - 1,
					mem_size > 0 ? mem_size - 1 : 0);
				if (spans_ptr) {
					DrawSpanLine(draw_list, s, line_min, static_cast<size_t>(line_i),
						base_display_addr + line_start_sel, base_display_addr + line_end_sel,
						*spans_ptr);
				}
				DrawSelection(draw_list, s, line_min, static_cast<size_t>(line_i),
					line_start_sel, line_end_sel);
				if (SelDragging && ImGui::GetIO().MouseDown[0]) {
					const ImVec2 mouse = ImGui::GetIO().MousePos;
					const bool in_line = mouse.y >= line_min.y &&
						mouse.y < line_min.y + s.LineHeight;
					if (in_line) {
						if (auto drag_addr = HitTestAddress(s, line_min, line_start_sel,
								line_end_sel, mouse))
							ExtendSelection(*drag_addr);
					}
				}

				// Draw Hexadecimal
				for (int n = 0; n < Cols && addr < mem_size; n++, addr++) {
					float byte_pos_x = s.PosHexStart + s.HexCellWidth * n;
					if (OptMidColsCount > 0)
						byte_pos_x += (float)(n / OptMidColsCount) * s.SpacingBetweenMidCols;
					ImGui::SameLine(byte_pos_x);

					// Draw highlight
					bool is_highlight_from_user_range = (addr >= HighlightMin && addr < HighlightMax);
					bool is_highlight_from_user_func = (HighlightFn && HighlightFn(mem_data, addr));
					bool is_highlight_from_preview = (addr >= DataPreviewAddr && addr < DataPreviewAddr + preview_data_type_size);
					if (is_highlight_from_user_range || is_highlight_from_user_func || is_highlight_from_preview) {
						ImVec2 pos = ImGui::GetCursorScreenPos();
						float highlight_width = s.GlyphWidth * 2;
						bool is_next_byte_highlighted = (addr + 1 < mem_size) && ((HighlightMax != (size_t)-1 && addr + 1 < HighlightMax) || (HighlightFn && HighlightFn(mem_data, addr + 1)));
						if (is_next_byte_highlighted || (n + 1 == Cols)) {
							highlight_width = s.HexCellWidth;
							if (OptMidColsCount > 0 && n > 0 && (n + 1) < Cols && ((n + 1) % OptMidColsCount) == 0)
								highlight_width += s.SpacingBetweenMidCols;
						}
						draw_list->AddRectFilled(pos, ImVec2(pos.x + highlight_width, pos.y + s.LineHeight), HighlightColor);
					}
					if (DataEditingAddr == addr) {
						// Display text input on current byte
						bool data_write = false;
						ImGui::PushID((void*)addr);
						if (DataEditingTakeFocus) {
							ImGui::SetKeyboardFocusHere(0);
							sprintf(AddrInputBuf, format_data, s.AddrDigitsCount, base_display_addr + addr);
							sprintf(DataInputBuf, format_byte, ReadFn ? ReadFn(mem_data, addr) : mem_data[addr]);
						}
						struct UserData {
							// FIXME: We should have a way to retrieve the text edit cursor position more easily in the API, this is rather tedious. This is such a ugly mess we may be better off not using InputText() at all here.
							static int Callback(ImGuiInputTextCallbackData* data) {
								UserData* user_data = (UserData*)data->UserData;
								if (!data->HasSelection())
									user_data->CursorPos = data->CursorPos;
								if (data->SelectionStart == 0 && data->SelectionEnd == data->BufTextLen) {
									// When not editing a byte, always refresh its InputText content pulled from underlying memory data
									// (this is a bit tricky, since InputText technically "owns" the master copy of the buffer we edit it in there)
									data->DeleteChars(0, data->BufTextLen);
									data->InsertChars(0, user_data->CurrentBufOverwrite);
									data->SelectionStart = 0;
									data->SelectionEnd = 2;
									data->CursorPos = 0;
								}
								return 0;
							}
							char CurrentBufOverwrite[3]; // Input
							int CursorPos;				 // Output
						};
						UserData user_data;
						user_data.CursorPos = -1;
						sprintf(user_data.CurrentBufOverwrite, format_byte, ReadFn ? ReadFn(mem_data, addr) : mem_data[addr]);
						ImGuiInputTextFlags flags = ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_NoHorizontalScroll | ImGuiInputTextFlags_CallbackAlways;
						flags |= ImGuiInputTextFlags_AlwaysOverwrite; // was ImGuiInputTextFlags_AlwaysInsertMode
						ImGui::SetNextItemWidth(s.GlyphWidth * 2);
						if (ImGui::InputText("##data", DataInputBuf, IM_ARRAYSIZE(DataInputBuf), flags, UserData::Callback, &user_data))
							data_write = data_next = true;
						else if (!DataEditingTakeFocus && !ImGui::IsItemActive())
							DataEditingAddr = data_editing_addr_next = (size_t)-1;
						DataEditingTakeFocus = false;
						if (user_data.CursorPos >= 2)
							data_write = data_next = true;
						if (data_editing_addr_next != (size_t)-1)
							data_write = data_next = false;
						unsigned int data_input_value = 0;
						if (data_write && sscanf(DataInputBuf, "%X", &data_input_value) == 1) {
							if (WriteFn)
								WriteFn(mem_data, addr, (ImU8)data_input_value);
							else
								mem_data[addr] = (ImU8)data_input_value;
						}
						ImGui::PopID();
					}
					else {
						// NB: The trailing space is not visible but ensure there's no gap that the mouse cannot click on.
						ImU8 b = ReadFn ? ReadFn(mem_data, addr) : mem_data[addr];

						if (OptShowHexII) {
							if ((b >= 32 && b < 128))
								ImGui::TextColored(LighterTextColor(), ".%c ", b);
							else if (b == 0xFF && OptGreyOutZeroes)
								ImGui::TextDisabled("## ");
							else if (b == 0x00)
								ImGui::Text("   ");
							else
								ImGui::TextColored(LighterTextColor(), format_byte_space, b);
						}
						else {
							if (b == 0 && OptGreyOutZeroes)
								ImGui::TextDisabled("00 ");
							else
								ImGui::TextColored(LighterTextColor(), format_byte_space, b);
						}
						if (ImGui::IsItemHovered()) {
							hovered_any_byte = true;
							const ImGuiIO& io = ImGui::GetIO();
							if (ImGui::IsMouseClicked(0))
								SelectByte(addr, io.KeyShift);
							if (ImGui::IsMouseDoubleClicked(0)) {
								DataEditingTakeFocus = !ReadOnly;
								data_editing_addr_next = addr;
							}
							if (ImGui::IsItemClicked(1)) {
								size_t sel_first = 0, sel_last = 0;
								NormalizeSelection(sel_first, sel_last);
								if (HasSelection() && addr >= sel_first && addr <= sel_last) {
									SelectionMenuRequested = true;
									SelPopupPos = ImVec2(ImGui::GetIO().MousePos.x + 8.0f,
										ImGui::GetIO().MousePos.y + 8.0f);
								}
								else if (ContextMenuFn) {
									ContextMenuFn(contextmenuuserdata, addr + base_display_addr);
								}
							}
						}
					}
				}

				if (OptShowAscii) {
					// Draw ASCII values
					ImGui::SameLine(s.PosAsciiStart);
					const ImVec2 ascii_pos = ImGui::GetCursorScreenPos();
					const size_t line_addr = static_cast<size_t>(line_i) * Cols;
					const size_t line_last = std::min(line_addr + Cols - 1,
						mem_size > 0 ? mem_size - 1 : 0);
					if (HasSelection()) {
						size_t first = 0, last = 0;
						NormalizeSelection(first, last);
						if (last >= line_addr && first <= line_last) {
							const size_t first_col = std::max(first, line_addr) - line_addr;
							const size_t last_col = std::min(last, line_last) - line_addr;
							draw_list->AddRectFilled(
								ImVec2(ascii_pos.x + s.GlyphWidth * static_cast<float>(first_col),
									ascii_pos.y + 1.0f),
								ImVec2(ascii_pos.x + s.GlyphWidth * static_cast<float>(last_col + 1),
									ascii_pos.y + s.LineHeight - 1.0f),
								ImGui::GetColorU32(ImGuiCol_TextSelectedBg, 0.5f), 3.0f);
						}
					}
					addr = line_addr;
					ImGui::PushID(line_i);
					ImGui::InvisibleButton("ascii", ImVec2(s.PosAsciiEnd - s.PosAsciiStart, s.LineHeight));
					const bool ascii_hovered = ImGui::IsItemHovered();
					if (ascii_hovered)
						hovered_any_byte = true;
					const ImGuiIO& ascii_io = ImGui::GetIO();
					const float ascii_rel = ascii_io.MousePos.x - ascii_pos.x;
					const int ascii_col = static_cast<int>(ascii_rel / std::max(1.0f, s.GlyphWidth));
					const size_t ascii_addr = line_addr + static_cast<size_t>(std::max(0, ascii_col));
					if (ImGui::IsItemClicked(0) && ascii_col >= 0 && ascii_col < Cols &&
						ascii_addr < mem_size)
						SelectByte(ascii_addr, ascii_io.KeyShift);
					if (ascii_hovered && ImGui::IsMouseDoubleClicked(0) && ascii_addr < mem_size) {
						DataEditingAddr = DataPreviewAddr = ascii_addr;
						DataEditingTakeFocus = true;
					}
					ImGui::PopID();
					ImVec2 pos = ascii_pos;
					for (int n = 0; n < Cols && addr < mem_size; n++, addr++) {
						if (addr == DataEditingAddr) {
							draw_list->AddRectFilled(pos, ImVec2(pos.x + s.GlyphWidth, pos.y + s.LineHeight), ImGui::GetColorU32(ImGuiCol_FrameBg));
							draw_list->AddRectFilled(pos, ImVec2(pos.x + s.GlyphWidth, pos.y + s.LineHeight), ImGui::GetColorU32(ImGuiCol_TextSelectedBg));
						}
						unsigned char c = ReadFn ? ReadFn(mem_data, addr) : mem_data[addr];
						char display_c = (c < 32 || c >= 128) ? '.' : c;
						draw_list->AddText(pos, (display_c == c) ? color_text : color_disabled, &display_c, &display_c + 1);
						pos.x += s.GlyphWidth;
					}
				}
			}
		{
			const ImGuiIO& io = ImGui::GetIO();
			if (SelDragging && ImGui::IsMouseReleased(0)) {
				SelDragging = false;
				// Reserve the footer on the next frame before drawing the panel,
				// so a completed drag never overlays the bytes.
				InlineRevealAtFrame = ImGui::GetFrameCount() + 1;
			}
			if (HasSelection() && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) && !hovered_any_byte)
				ClearSelection();
			if (HasSelection() && !SelDragging && ImGui::IsMouseClicked(0) &&
				!ImGui::IsPopupOpen(GetSelectionPopupId())) {
				const ImVec2 child_min = ImGui::GetWindowPos();
				const ImVec2 child_size = ImGui::GetWindowSize();
				const ImVec2 child_max(child_min.x + child_size.x, child_min.y + child_size.y);
				if (!ImGui::IsMouseHoveringRect(child_min, child_max))
					ClearSelection();
			}
			if (HasSelection() && !SelDragging && DataEditingAddr == (size_t)-1 &&
				(io.KeySuper || io.KeyCtrl) && ImGui::IsKeyPressed(ImGuiKey_C, false) &&
				ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
				CopySelection(false, mem_data, mem_size);
			}
			if (HasSelection() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
				!io.WantTextInput && io.KeyShift) {
				long delta = 0;
				if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
					delta = -1;
				else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
					delta = 1;
				else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
					delta = -static_cast<long>(Cols);
				else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
					delta = static_cast<long>(Cols);
				ExtendSelectionBy(delta, mem_size);
			}
		}
		DrawCopyNotice(draw_list);
		ImGui::PopStyleVar(2);
		ImGui::EndChild();
		RunSelectionTest(mem_data, mem_size, base_display_addr);

		// Notify the main window of our ideal child content size (FIXME: we are missing an API to get the contents size from the child)
		ImGui::SetCursorPosX(s.WindowWidth);
		ImGui::Dummy(ImVec2(0.0f, 0.0f));

		if (data_next && DataEditingAddr + 1 < mem_size) {
			DataEditingAddr = DataPreviewAddr = DataEditingAddr + 1;
			DataEditingTakeFocus = true;
		}
		else if (data_editing_addr_next != (size_t)-1) {
			DataEditingAddr = DataPreviewAddr = data_editing_addr_next;
			DataEditingTakeFocus = true;
		}

		const bool lock_show_data_preview = OptShowDataPreview;
		if (OptShowOptions) {
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
			DrawOptionsLine(s, mem_data, mem_size, base_display_addr);
		}
		RenderSelectionPopup(mem_data, mem_size, base_display_addr, spans_ptr);

		if (lock_show_data_preview) {
			ImGui::Separator();
			DrawPreviewLine(s, mem_data, mem_size, base_display_addr);
		}
		RenderSelectionInline(mem_data, mem_size, base_display_addr, spans_ptr, spans_allowed);
	}
	void RunSelectionTest(const ImU8* mem_data, size_t mem_size, size_t base_display_addr) {
		if (!SelTestActive || SelTestStage >= 10)
			return;
		if (SelTestStartAddr < base_display_addr || SelTestEndAddr < base_display_addr)
			return;
		const size_t start_offset = SelTestStartAddr - base_display_addr;
		const size_t end_offset = SelTestEndAddr - base_display_addr;
		if (start_offset >= mem_size || end_offset >= mem_size)
			return;
		ImGuiIO& io = ImGui::GetIO();

		if (SelTestStage == 0) {
			// Same entry points as the mouse handlers: click, then drag.
			SelectByte(start_offset, false);
			ExtendSelection(end_offset);
			SelDragging = false;
			size_t first = 0, last = 0;
			NormalizeSelection(first, last);
			std::fprintf(stderr, "[sel] drag range=%zX..%zX hex=%s %s\n",
				first + base_display_addr, last + base_display_addr,
				SelectionHex(mem_data, mem_size).c_str(),
				last == end_offset ? "PASS" : "FAIL");
			SelTestStage = 1;
			if (std::getenv("CASIOEMU_HEX_SEL_HOLD")) {
				SelTestActive = false;
				InlineRevealAtFrame = ImGui::GetFrameCount() + 1;
			}
		}
		else if (SelTestStage == 1) {
			const size_t extended = std::min(end_offset + 2, mem_size - 1);
			SelectByte(extended, true);
			SelDragging = false;
			size_t first = 0, last = 0;
			NormalizeSelection(first, last);
			std::fprintf(stderr, "[sel] shift extend=%zX..%zX %s\n",
				first + base_display_addr, last + base_display_addr,
				last == extended ? "PASS" : "FAIL");
			SelTestStage = 2;
		}
		else if (SelTestStage == 2) {
			// Drive the real shortcut path: focus the editor, queue Cmd+C and
			// let the normal key handler run on the next frame.
			ImGui::SetWindowFocus();
			io.AddKeyEvent(ImGuiMod_Super, true);
			io.AddKeyEvent(ImGuiKey_C, true);
			SelTestStage = 3;
		}
		else if (SelTestStage == 3) {
			io.AddKeyEvent(ImGuiKey_C, false);
			io.AddKeyEvent(ImGuiMod_Super, false);
			std::fprintf(stderr, "[sel] cmd+c bytes=%s notice=%s %s\n",
				SelectionHex(mem_data, mem_size).c_str(), CopyNotice,
				CopyNotice[0] ? "PASS" : "FAIL");
			SelTestStage = 4;
		}
		else if (SelTestStage == 4) {
			ShowTextPanel = true;
			ShowSpansPanel = true;
			InlineRevealAtFrame = 0;
			std::fprintf(stderr, "[sel] text=%s glyphs F110=%s F410=%s\n",
				SelectionText(mem_data, mem_size).c_str(),
				LookupGlyph(0xF1, 0x10) ? LookupGlyph(0xF1, 0x10) : "?",
				LookupGlyph(0xF4, 0x10) ? LookupGlyph(0xF4, 0x10) : "?");
			SelTestStage = 5;
		}
		else if (SelTestStage == 5) {
			CopySelection(true, mem_data, mem_size);
			std::fprintf(stderr, "[sel] copy decoded=%s %s\n",
				SelectionText(mem_data, mem_size).c_str(),
				CopyNoticeFrame >= 0 ? "PASS" : "FAIL");
			SelTestStage = 6;
		}
		else if (SelTestStage == 6) {
			Sizes hit_sizes;
			CalcSizes(hit_sizes, mem_size, base_display_addr);
			const ImVec2 hit_origin(0.0f, 0.0f);
			bool hit_ok = true;
			const int columns[] = {0, 1, 7, 8, 15, 31, 63};
			for (int column : columns) {
				if (column >= Cols)
					continue;
				const float group = OptMidColsCount > 0 ? hit_sizes.SpacingBetweenMidCols : 0.0f;
				const float hex_x = hit_origin.x + hit_sizes.PosHexStart +
					hit_sizes.HexCellWidth * static_cast<float>(column) +
					(OptMidColsCount > 0 ? static_cast<float>(column / OptMidColsCount) * group : 0.0f) +
					1.0f;
				const auto hex_hit = HitTestAddress(hit_sizes, hit_origin, start_offset,
					std::min(start_offset + static_cast<size_t>(Cols) - 1, mem_size - 1),
					ImVec2(hex_x, 1.0f));
				const float ascii_x = hit_origin.x + hit_sizes.PosAsciiStart +
					hit_sizes.GlyphWidth * static_cast<float>(column) + 0.5f;
				const auto ascii_hit = HitTestAddress(hit_sizes, hit_origin, start_offset,
					std::min(start_offset + static_cast<size_t>(Cols) - 1, mem_size - 1),
					ImVec2(ascii_x, 1.0f));
				if (!hex_hit || *hex_hit != start_offset + static_cast<size_t>(column) ||
					!ascii_hit || *ascii_hit != start_offset + static_cast<size_t>(column))
					hit_ok = false;
			}
			const float address_x = hit_sizes.PosHexStart * 0.5f;
			const auto address_hit = HitTestAddress(hit_sizes, hit_origin, start_offset,
				std::min(start_offset + static_cast<size_t>(Cols) - 1, mem_size - 1),
				ImVec2(address_x, 1.0f));
			if (address_hit)
				hit_ok = false;
			std::optional<size_t> gap_hit;
			if (OptMidColsCount > 0 && Cols > OptMidColsCount) {
				const size_t last_in_group = static_cast<size_t>(OptMidColsCount) - 1;
				const float group = hit_sizes.SpacingBetweenMidCols;
				const float gap_x = hit_origin.x + hit_sizes.PosHexStart +
					hit_sizes.HexCellWidth * static_cast<float>(last_in_group) + hit_sizes.HexCellWidth +
					group * 0.5f;
				gap_hit = HitTestAddress(hit_sizes, hit_origin, start_offset,
					std::min(start_offset + static_cast<size_t>(Cols) - 1, mem_size - 1),
					ImVec2(gap_x, 1.0f));
				if (gap_hit)
					hit_ok = false;
			}
			std::fprintf(stderr, "[sel] hit cols=%d address=%s gap=%s %s\n", Cols,
				address_hit ? "byte" : "ignored", gap_hit ? "byte" : "ignored",
				hit_ok ? "PASS" : "FAIL");
			std::fprintf(stderr, "[sel] inline=%s %s\n",
				InlineSelectionShown ? "visible" : "hidden",
				InlineSelectionShown ? "PASS" : "FAIL");
			SelTestStage = 7;
		}
		else if (SelTestStage == 7) {
			// Text-column drag uses the same SelectByte/ExtendSelection entry
			// points as a pointer drag over the ASCII column.
			SelectByte(start_offset, false);
			ExtendSelection(end_offset);
			SelDragging = false;
			InlineRevealAtFrame = ImGui::GetFrameCount() + 1;
			size_t first = 0, last = 0;
			NormalizeSelection(first, last);
			std::fprintf(stderr, "[sel] ascii drag=%zX..%zX %s\n",
				first + base_display_addr, last + base_display_addr,
				first == start_offset && last == end_offset ? "PASS" : "FAIL");
			SelTestStage = 8;
		}
		else if (SelTestStage == 8) {
			// Cross-column drag: start in hex, extend over the text column.
			SelectByte(start_offset, false);
			ExtendSelection(end_offset);
			SelDragging = false;
			InlineRevealAtFrame = ImGui::GetFrameCount() + 1;
			CopySelection(false, mem_data, mem_size);
			size_t first = 0, last = 0;
			NormalizeSelection(first, last);
			const bool bytes_ok = CopyNotice[0] == '\0' ||
				std::strncmp(CopyNotice, "Copied", 6) == 0;
			std::fprintf(stderr, "[sel] cross drag=%zX..%zX copy=%s %s\n",
				first + base_display_addr, last + base_display_addr, CopyNotice,
				first == start_offset && last == end_offset && bytes_ok ? "PASS" : "FAIL");
			SelTestStage = 9;
		}
		else if (SelTestStage == 9) {
			SelectByte(start_offset, false);
			const size_t extended = std::min(end_offset + 2, mem_size - 1);
			SelectByte(extended, true);
			SelDragging = false;
			InlineRevealAtFrame = ImGui::GetFrameCount() + 1;
			size_t first = 0, last = 0;
			NormalizeSelection(first, last);
			std::fprintf(stderr, "[sel] ascii shift=%zX..%zX %s\n",
				first + base_display_addr, last + base_display_addr,
				first == start_offset && last == extended ? "PASS" : "FAIL");
			SelTestStage = 10;
		}
	}

	void RenderSelectionPopup(const ImU8* mem_data, size_t mem_size, size_t base_display_addr,
		const std::vector<MarkedSpan>* spans) {
		const char* popup_id = GetSelectionPopupId();
		bool opened_now = false;
		if (SelectionMenuRequested) {
			SelectionMenuRequested = false;
			opened_now = true;
			ImGui::SetNextWindowPos(SelPopupPos, ImGuiCond_Always);
			ImGui::OpenPopup(popup_id);
		}
		if (!HasSelection())
			return;
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 5.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
		if (ImGui::BeginPopup(popup_id)) {
			size_t first = 0, last = 0;
			NormalizeSelection(first, last);
			const size_t absolute_first = first + base_display_addr;
			const size_t absolute_last = last + base_display_addr;
			const bool spans_allowed = SelSpansEnabled &&
				(!SelSpansAllowed || SelSpansAllowed(absolute_first));
			if (opened_now && SelTestActive)
				std::fprintf(stderr, "[sel] menu=%sText\n", spans_allowed ? "Spans," : "");
			if (spans_allowed && spans) {
				if (ImGui::Selectable("Spans", false, ImGuiSelectableFlags_DontClosePopups))
					ShowSpansPanel = !ShowSpansPanel;
				if (ShowSpansPanel) {
					bool any = false;
					for (const auto& span : *spans) {
						const size_t span_last = span.start + (span.length > 0 ? span.length - 1 : 0);
						if (span.start <= absolute_last && span_last >= absolute_first) {
							ImGui::BulletText("%s", span.desc ? span.desc : "(unnamed span)");
							any = true;
						}
					}
					if (!any)
						ImGui::TextDisabled("No spans");
				}
			}
			if (ImGui::Selectable("Text", false, ImGuiSelectableFlags_DontClosePopups))
				ShowTextPanel = !ShowTextPanel;
			if (ShowTextPanel) {
				const std::string decoded = SelectionText(mem_data, mem_size);
				const bool blank = decoded.find_first_not_of(" \t\r\n") == std::string::npos;
				ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
				ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
				ImGui::BeginChild("##decoded_text", ImVec2(240.0f, 64.0f),
					ImGuiChildFlags_Border | ImGuiChildFlags_AlwaysUseWindowPadding);
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextUnformatted(blank ? "(blank)" : decoded.c_str());
				ImGui::PopTextWrapPos();
				ImGui::EndChild();
				ImGui::PopStyleColor();
				ImGui::PopStyleVar(2);
				if (ImGui::Button("Copy"))
					CopySelection(true, mem_data, mem_size);
				ImGui::SameLine();
				ImGui::TextDisabled("%d bytes", static_cast<int>(last - first + 1));
			}
			ImGui::EndPopup();
		}
		ImGui::PopStyleVar(4);
	}
	std::optional<std::string> SpanDescription;

	void UpdateSpanDescriptions(const OptionalMarkedSpans& marked_spans, size_t base_display_addr) {
		if (!marked_spans.has_value() || DataEditingAddr == (size_t)-1) {
			SpanDescription = {};
			return;
		}

		std::optional<std::vector<std::string>> desc_to_show = {};
		for (auto& span : marked_spans.value()) {
			auto abs_editing_pos = DataEditingAddr + base_display_addr;
			if (abs_editing_pos >= span.start && abs_editing_pos <= span.start + span.length - 1 &&
				span.desc) {
				if (!desc_to_show.has_value()) {
					desc_to_show = std::vector<std::string>();
				}
				desc_to_show.value().push_back(span.desc);
			}
		}
		if (!desc_to_show.has_value()) {
			SpanDescription = {};
			return;
		}

		// desc_to_show.size() won't be empty
		auto& d = desc_to_show.value();
		std::string joined = d[0];
		if (d.size() > 1) {
			for (size_t i = 1; i < d.size(); ++i) {
				joined += ", ";
				joined += d[i];
			}
		}
		SpanDescription = joined;
	}

	// Standalone Memory Editor window
	void DrawWindow(const char* title, void* mem_data, size_t mem_size, size_t base_display_addr, const OptionalMarkedSpans& marked_spans) {
		Sizes s;
		CalcSizes(s, mem_size, base_display_addr);
		ImGui::SetNextWindowSize(ImVec2(s.WindowWidth, s.WindowWidth * 0.60f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(s.WindowWidth, FLT_MAX));

		Open = true;
		if (ImGui::Begin(title, &Open, ImGuiWindowFlags_NoScrollbar)) {
			if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
				ImGui::OpenPopup("context");
			DrawContents(mem_data, mem_size, base_display_addr, marked_spans);
			if (ContentsWidthChanged) {
				CalcSizes(s, mem_size, base_display_addr);
				ImGui::SetWindowSize(ImVec2(s.WindowWidth, ImGui::GetWindowSize().y));
			}
		}
		ImGui::End();
	}
	// Memory Editor contents only
	void DrawContents(void* mem_data_void, size_t mem_size, size_t base_display_addr, const OptionalMarkedSpans& marked_spans) {
		if (Cols < 1)
			Cols = 1;

		ImU8* mem_data = (ImU8*)mem_data_void;
		Sizes s;
		CalcSizes(s, mem_size, base_display_addr);
		ImGuiStyle& style = ImGui::GetStyle();
		if (!SelTestParsed) {
			SelTestParsed = true;
			if (const char* spec = std::getenv("CASIOEMU_HEX_SEL_TEST")) {
				unsigned start = 0, end = 0;
				if (std::sscanf(spec, "%x,%x", &start, &end) == 2) {
					SelTestActive = true;
					SelTestStartAddr = start;
					SelTestEndAddr = end;
				}
			}
			if (SelTestActive) {
				std::fprintf(stderr, "[sel] glyph F110=%s F410=%s\n",
					LookupGlyph(0xF1, 0x10) ? LookupGlyph(0xF1, 0x10) : "?",
					LookupGlyph(0xF4, 0x10) ? LookupGlyph(0xF4, 0x10) : "?");
			}
		}

		const OptionalMarkedSpans test_spans = TestMarkedSpans();
		const OptionalMarkedSpans& effective_spans = test_spans.has_value() ? test_spans : marked_spans;
		const bool spans_allowed = effective_spans.has_value() &&
			(SelSpansEnabled || test_spans.has_value());
		UpdateSpanDescriptions(effective_spans, base_display_addr);

		// We begin into our scrolling region with the 'ImGuiWindowFlags_NoMove' in order to prevent click from moving the window.
		// This is used as a facility since our main click detection code doesn't assign an ActiveId so the click would normally be caught as a window-move.
		const float height_separator = style.ItemSpacing.y;
		float footer_height = OptFooterExtraHeight;
		if (OptShowOptions)
			footer_height += height_separator + ImGui::GetFrameHeightWithSpacing() * 1 + 14.0f;
		if (OptShowDataPreview)
			footer_height += height_separator + ImGui::GetFrameHeightWithSpacing() * 1 + ImGui::GetTextLineHeightWithSpacing() * 3;
		if (SpanDescription.has_value())
			footer_height += height_separator + ImGui::GetFrameHeightWithSpacing() * 1 + ImGui::GetTextLineHeightWithSpacing() * 1;
		footer_height += InlinePanelHeight(spans_allowed);
		ImGui::BeginChild("##scrolling", ImVec2(0, -footer_height), false, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav);
		ImDrawList* draw_list = ImGui::GetWindowDrawList();

		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

		// We are not really using the clipper API correctly here, because we rely on visible_start_addr/visible_end_addr for our scrolling function.
		const int line_total_count = (int)((mem_size + Cols - 1) / Cols);
		ImGuiListClipper clipper;
		clipper.Begin(line_total_count, s.LineHeight);

		bool data_next = false;

		if (ReadOnly || DataEditingAddr >= mem_size)
			DataEditingAddr = (size_t)-1;
		if (DataPreviewAddr >= mem_size)
			DataPreviewAddr = (size_t)-1;

		size_t preview_data_type_size = OptShowDataPreview ? DataTypeGetSize(PreviewDataType) : 0;

		size_t data_editing_addr_next = (size_t)-1;
		if (DataEditingAddr != (size_t)-1) {
			// Move cursor but only apply on next frame so scrolling with be synchronized (because currently we can't change the scrolling while the window is being rendered)
			if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && (ptrdiff_t)DataEditingAddr >= (ptrdiff_t)Cols) {
				data_editing_addr_next = DataEditingAddr - Cols;
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && (ptrdiff_t)DataEditingAddr < (ptrdiff_t)mem_size - Cols) {
				data_editing_addr_next = DataEditingAddr + Cols;
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && (ptrdiff_t)DataEditingAddr > (ptrdiff_t)0) {
				data_editing_addr_next = DataEditingAddr - 1;
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && (ptrdiff_t)DataEditingAddr < (ptrdiff_t)mem_size - 1) {
				data_editing_addr_next = DataEditingAddr + 1;
			}
		}

		// Draw vertical separator
		ImVec2 window_pos = ImGui::GetWindowPos();
		if (OptShowAscii)
			draw_list->AddLine(ImVec2(window_pos.x + s.PosAsciiStart - s.GlyphWidth, window_pos.y), ImVec2(window_pos.x + s.PosAsciiStart - s.GlyphWidth, window_pos.y + 9999), ImGui::GetColorU32(ImGuiCol_Border));

		const ImU32 color_text = ImGui::ColorConvertFloat4ToU32(LighterTextColor());
		const ImU32 color_disabled = OptGreyOutZeroes
			? ImGui::ColorConvertFloat4ToU32(DimTextColor()) : color_text;

		const char* format_address = OptUpperCaseHex ? "%0*" _PRISizeT "X: " : "%0*" _PRISizeT "x: ";
		const char* format_data = OptUpperCaseHex ? "%0*" _PRISizeT "X" : "%0*" _PRISizeT "x";
		const char* format_byte = OptUpperCaseHex ? "%02X" : "%02x";
		const char* format_byte_space = OptUpperCaseHex ? "%02X " : "%02x ";

		bool hovered_any_byte = false;
		while (clipper.Step())
			for (int line_i = clipper.DisplayStart; line_i < clipper.DisplayEnd; line_i++) // display only visible lines
			{
				size_t addr = (size_t)(line_i * Cols);
				ImGui::Text(format_address, s.AddrDigitsCount, base_display_addr + addr);

				const ImVec2 line_min = ImGui::GetItemRectMin();
				const size_t line_start_sel = static_cast<size_t>(line_i * Cols);
				const size_t line_end_sel = std::min(line_start_sel + Cols - 1,
					mem_size > 0 ? mem_size - 1 : 0);
				if (effective_spans.has_value()) {
					DrawSpanLine(draw_list, s, line_min, static_cast<size_t>(line_i),
						base_display_addr + line_start_sel, base_display_addr + line_end_sel,
						effective_spans.value());
				}
				DrawSelection(draw_list, s, line_min, static_cast<size_t>(line_i),
					line_start_sel, line_end_sel);
				if (SelDragging && ImGui::GetIO().MouseDown[0]) {
					const ImVec2 mouse = ImGui::GetIO().MousePos;
					const bool in_line = mouse.y >= line_min.y &&
						mouse.y < line_min.y + s.LineHeight;
					if (in_line) {
						if (auto drag_addr = HitTestAddress(s, line_min, line_start_sel,
								line_end_sel, mouse))
							ExtendSelection(*drag_addr);
					}
				}

				// Draw Hexadecimal
				for (int n = 0; n < Cols && addr < mem_size; n++, addr++) {
					float byte_pos_x = s.PosHexStart + s.HexCellWidth * n;
					if (OptMidColsCount > 0)
						byte_pos_x += (float)(n / OptMidColsCount) * s.SpacingBetweenMidCols;
					ImGui::SameLine(byte_pos_x);

					// Draw highlight
					bool is_highlight_from_user_range = (addr >= HighlightMin && addr < HighlightMax);
					bool is_highlight_from_user_func = (HighlightFn && HighlightFn(mem_data, addr));
					bool is_highlight_from_preview = (addr >= DataPreviewAddr && addr < DataPreviewAddr + preview_data_type_size);
					if (is_highlight_from_user_range || is_highlight_from_user_func || is_highlight_from_preview) {
						ImVec2 pos = ImGui::GetCursorScreenPos();
						float highlight_width = s.GlyphWidth * 2;
						bool is_next_byte_highlighted = (addr + 1 < mem_size) && ((HighlightMax != (size_t)-1 && addr + 1 < HighlightMax) || (HighlightFn && HighlightFn(mem_data, addr + 1)));
						if (is_next_byte_highlighted || (n + 1 == Cols)) {
							highlight_width = s.HexCellWidth;
							if (OptMidColsCount > 0 && n > 0 && (n + 1) < Cols && ((n + 1) % OptMidColsCount) == 0)
								highlight_width += s.SpacingBetweenMidCols;
						}
						draw_list->AddRectFilled(pos, ImVec2(pos.x + highlight_width, pos.y + s.LineHeight), HighlightColor);
					}
					if (ram_edit_ov && ram_edit_ov[(size_t)mem_data + addr] > 0) {
						ImVec2 pos = ImGui::GetCursorScreenPos();
						float highlight_width = s.GlyphWidth * 2;
						bool is_next_byte_highlighted = (addr + 1 < mem_size) && ((HighlightMax != (size_t)-1 && addr + 1 < HighlightMax) || (HighlightFn && HighlightFn(mem_data, addr + 1)));
						if (is_next_byte_highlighted || (n + 1 == Cols)) {
							highlight_width = s.HexCellWidth;
							if (OptMidColsCount > 0 && n > 0 && (n + 1) < Cols && ((n + 1) % OptMidColsCount) == 0)
								highlight_width += s.SpacingBetweenMidCols;
						}
						draw_list->AddRectFilled(pos, ImVec2(pos.x + highlight_width, pos.y + s.LineHeight), ~ImColor{255, 0, 0, int(ram_edit_ov[(size_t)mem_data + addr])});
						ram_edit_ov[(size_t)mem_data + addr] -= 10;
					}

					if (DataEditingAddr == addr) {
						// Display text input on current byte
						bool data_write = false;
						ImGui::PushID((void*)addr);
						if (DataEditingTakeFocus) {
							ImGui::SetKeyboardFocusHere(0);
							sprintf(AddrInputBuf, format_data, s.AddrDigitsCount, base_display_addr + addr);
							sprintf(DataInputBuf, format_byte, ReadFn ? ReadFn(mem_data, addr) : mem_data[addr]);
						}
						struct UserData {
							// FIXME: We should have a way to retrieve the text edit cursor position more easily in the API, this is rather tedious. This is such a ugly mess we may be better off not using InputText() at all here.
							static int Callback(ImGuiInputTextCallbackData* data) {
								UserData* user_data = (UserData*)data->UserData;
								if (!data->HasSelection())
									user_data->CursorPos = data->CursorPos;
								if (data->SelectionStart == 0 && data->SelectionEnd == data->BufTextLen) {
									// When not editing a byte, always refresh its InputText content pulled from underlying memory data
									// (this is a bit tricky, since InputText technically "owns" the master copy of the buffer we edit it in there)
									data->DeleteChars(0, data->BufTextLen);
									data->InsertChars(0, user_data->CurrentBufOverwrite);
									data->SelectionStart = 0;
									data->SelectionEnd = 2;
									data->CursorPos = 0;
								}
								return 0;
							}
							char CurrentBufOverwrite[3]; // Input
							int CursorPos;				 // Output
						};
						UserData user_data;
						user_data.CursorPos = -1;
						sprintf(user_data.CurrentBufOverwrite, format_byte, ReadFn ? ReadFn(mem_data, addr) : mem_data[addr]);
						ImGuiInputTextFlags flags = ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_NoHorizontalScroll | ImGuiInputTextFlags_CallbackAlways;
						flags |= ImGuiInputTextFlags_AlwaysOverwrite; // was ImGuiInputTextFlags_AlwaysInsertMode
						ImGui::SetNextItemWidth(s.GlyphWidth * 2);
						if (ImGui::InputText("##data", DataInputBuf, IM_ARRAYSIZE(DataInputBuf), flags, UserData::Callback, &user_data))
							data_write = data_next = true;
						else if (!DataEditingTakeFocus && !ImGui::IsItemActive())
							DataEditingAddr = data_editing_addr_next = (size_t)-1;
						if (DataEditingTakeFocus)
							ImGui::SetKeyboardFocusHere(-1);
						DataEditingTakeFocus = false;
						if (user_data.CursorPos >= 2)
							data_write = data_next = true;
						if (data_editing_addr_next != (size_t)-1)
							data_write = data_next = false;

						if (data_write) {
							for (size_t i = 0;; ++i) {
								auto buf = DataInputBuf;
								if (buf[i * 2] == '\0' || buf[i * 2 + 1] == '\0')
									break;
								char byte_buf[3] = {buf[i * 2], buf[i * 2 + 1]};
								uint8_t input_value = static_cast<uint8_t>(strtoul(byte_buf, nullptr, 16));
								if (WriteFn) {
									WriteFn(mem_data, addr + i, input_value);
								}
								else {
									mem_data[addr + i] = (uint8_t)input_value;
								}
							}
						}
						ImGui::PopID();
					}
					else {
						// NB: The trailing space is not visible but ensure there's no gap that the mouse cannot click on.
						ImU8 b = ReadFn ? ReadFn(mem_data, addr) : mem_data[addr];

						if (OptShowHexII) {
							if ((b >= 32 && b < 128))
								ImGui::TextColored(LighterTextColor(), ".%c ", b);
							else if (b == 0xFF && OptGreyOutZeroes)
								ImGui::TextDisabled("## ");
							else if (b == 0x00)
								ImGui::Text("   ");
							else
								ImGui::TextColored(LighterTextColor(), format_byte_space, b);
						}
						else {
							if (b == 0 && OptGreyOutZeroes)
								ImGui::TextDisabled("00 ");
							else
								ImGui::TextColored(LighterTextColor(), format_byte_space, b);
						}
						if (ImGui::IsItemHovered()) {
							hovered_any_byte = true;
							const ImGuiIO& io = ImGui::GetIO();
							if (ImGui::IsMouseClicked(0))
								SelectByte(addr, io.KeyShift);
							if (ImGui::IsMouseDoubleClicked(0)) {
								DataEditingTakeFocus = !ReadOnly;
								data_editing_addr_next = addr;
							}
							if (ImGui::IsItemClicked(1)) {
								size_t sel_first = 0, sel_last = 0;
								NormalizeSelection(sel_first, sel_last);
								if (HasSelection() && addr >= sel_first && addr <= sel_last) {
									SelectionMenuRequested = true;
									SelPopupPos = ImVec2(ImGui::GetIO().MousePos.x + 8.0f,
										ImGui::GetIO().MousePos.y + 8.0f);
								}
								else if (ContextMenuFn) {
									ContextMenuFn(contextmenuuserdata, addr + base_display_addr);
								}
							}
						}
					}
				}

				if (OptShowAscii) {
					// Draw ASCII values
					ImGui::SameLine(s.PosAsciiStart);
					const ImVec2 ascii_pos = ImGui::GetCursorScreenPos();
					const size_t line_addr = static_cast<size_t>(line_i) * Cols;
					const size_t line_last = std::min(line_addr + Cols - 1,
						mem_size > 0 ? mem_size - 1 : 0);
					if (HasSelection()) {
						size_t first = 0, last = 0;
						NormalizeSelection(first, last);
						if (last >= line_addr && first <= line_last) {
							const size_t first_col = std::max(first, line_addr) - line_addr;
							const size_t last_col = std::min(last, line_last) - line_addr;
							draw_list->AddRectFilled(
								ImVec2(ascii_pos.x + s.GlyphWidth * static_cast<float>(first_col),
									ascii_pos.y + 1.0f),
								ImVec2(ascii_pos.x + s.GlyphWidth * static_cast<float>(last_col + 1),
									ascii_pos.y + s.LineHeight - 1.0f),
								ImGui::GetColorU32(ImGuiCol_TextSelectedBg, 0.5f), 3.0f);
						}
					}
					addr = line_addr;
					ImGui::PushID(line_i);
					ImGui::InvisibleButton("ascii", ImVec2(s.PosAsciiEnd - s.PosAsciiStart, s.LineHeight));
					const bool ascii_hovered = ImGui::IsItemHovered();
					if (ascii_hovered)
						hovered_any_byte = true;
					const ImGuiIO& ascii_io = ImGui::GetIO();
					const float ascii_rel = ascii_io.MousePos.x - ascii_pos.x;
					const int ascii_col = static_cast<int>(ascii_rel / std::max(1.0f, s.GlyphWidth));
					const size_t ascii_addr = line_addr + static_cast<size_t>(std::max(0, ascii_col));
					if (ImGui::IsItemClicked(0) && ascii_col >= 0 && ascii_col < Cols &&
						ascii_addr < mem_size)
						SelectByte(ascii_addr, ascii_io.KeyShift);
					if (ascii_hovered && ImGui::IsMouseDoubleClicked(0) && ascii_addr < mem_size) {
						DataEditingAddr = DataPreviewAddr = ascii_addr;
						DataEditingTakeFocus = true;
					}
					ImGui::PopID();
					ImVec2 pos = ascii_pos;
					for (int n = 0; n < Cols && addr < mem_size; n++, addr++) {
						if (addr == DataEditingAddr) {
							draw_list->AddRectFilled(pos, ImVec2(pos.x + s.GlyphWidth, pos.y + s.LineHeight), ImGui::GetColorU32(ImGuiCol_FrameBg));
							draw_list->AddRectFilled(pos, ImVec2(pos.x + s.GlyphWidth, pos.y + s.LineHeight), ImGui::GetColorU32(ImGuiCol_TextSelectedBg));
						}
						unsigned char c = ReadFn ? ReadFn(mem_data, addr) : mem_data[addr];
						char display_c = (c < 32 || c >= 128) ? '.' : c;
						draw_list->AddText(pos, (display_c == c) ? color_text : color_disabled, &display_c, &display_c + 1);
						pos.x += s.GlyphWidth;
					}
				}
			}
		{
			const ImGuiIO& io = ImGui::GetIO();
			if (SelDragging && ImGui::IsMouseReleased(0)) {
				SelDragging = false;
				// Reserve the footer on the next frame before drawing the panel,
				// so a completed drag never overlays the bytes.
				InlineRevealAtFrame = ImGui::GetFrameCount() + 1;
			}
			if (HasSelection() && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) && !hovered_any_byte)
				ClearSelection();
			if (HasSelection() && !SelDragging && ImGui::IsMouseClicked(0) &&
				!ImGui::IsPopupOpen(GetSelectionPopupId())) {
				const ImVec2 child_min = ImGui::GetWindowPos();
				const ImVec2 child_size = ImGui::GetWindowSize();
				const ImVec2 child_max(child_min.x + child_size.x, child_min.y + child_size.y);
				if (!ImGui::IsMouseHoveringRect(child_min, child_max))
					ClearSelection();
			}
			if (HasSelection() && !SelDragging && DataEditingAddr == (size_t)-1 &&
				(io.KeySuper || io.KeyCtrl) && ImGui::IsKeyPressed(ImGuiKey_C, false) &&
				ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
				CopySelection(false, mem_data, mem_size);
			}
			if (HasSelection() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
				!io.WantTextInput && io.KeyShift) {
				long delta = 0;
				if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
					delta = -1;
				else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
					delta = 1;
				else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
					delta = -static_cast<long>(Cols);
				else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
					delta = static_cast<long>(Cols);
				ExtendSelectionBy(delta, mem_size);
			}
		}
		DrawCopyNotice(draw_list);
		ImGui::PopStyleVar(2);
		ImGui::EndChild();
		RunSelectionTest(mem_data, mem_size, base_display_addr);

		// Notify the main window of our ideal child content size (FIXME: we are missing an API to get the contents size from the child)
		ImGui::SetCursorPosX(s.WindowWidth);
		ImGui::Dummy(ImVec2(0.0f, 0.0f));

		if (data_next && DataEditingAddr + 1 < mem_size) {
			DataEditingAddr = DataPreviewAddr = DataEditingAddr + 1;
			DataEditingTakeFocus = true;
		}
		else if (data_editing_addr_next != (size_t)-1) {
			DataEditingAddr = DataPreviewAddr = data_editing_addr_next;
			DataEditingTakeFocus = true;
		}

		const bool lock_show_data_preview = OptShowDataPreview;
		if (OptShowOptions) {
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
			DrawOptionsLine(s, mem_data, mem_size, base_display_addr);
		}
		RenderSelectionPopup(mem_data, mem_size, base_display_addr,
			effective_spans.has_value() ? &effective_spans.value() : nullptr);

		if (lock_show_data_preview) {
			ImGui::Separator();
			DrawPreviewLine(s, mem_data, mem_size, base_display_addr);
		}

		if (SpanDescription.has_value()) {
			ImGui::Separator();
			DrawSpanDescriptionsLine(SpanDescription.value());
		}
		RenderSelectionInline(mem_data, mem_size, base_display_addr,
			effective_spans.has_value() ? &effective_spans.value() : nullptr, spans_allowed);
	}
	static void DrawSpanDescriptionsLine(const std::string& desc_string) {
		ImGui::Text("Spans");
		ImGui::Text("%s", desc_string.c_str());
	}
	void DrawOptionsLine(const Sizes& s, void* mem_data, size_t mem_size, size_t base_display_addr) {
		IM_UNUSED(mem_data);
		ImGuiStyle& style = ImGui::GetStyle();
		const char* format_range = OptUpperCaseHex ? "%s %0*" _PRISizeT "X..%0*" _PRISizeT "X" : "%s %0*" _PRISizeT "x..%0*" _PRISizeT "x";

		// Options menu.  Chrome matches the tool-page dialogs: 10 px outer
		// padding, 8/5 item spacing, rounded corners and a border.
		if (ImGui::Button("HexEditor.Options"_lc))
			ImGui::OpenPopup("context");
		// Verification hook: open the menu without a mouse for screenshot checks.
		if (std::getenv("CASIOEMU_SHOW_MEMORY_OPTIONS")) {
			if (!ImGui::IsPopupOpen("context"))
				ImGui::OpenPopup("context");
			ImGui::SetNextWindowPos(ImVec2(340.0f, 300.0f), ImGuiCond_Always);
		}
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 5.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
		if (ImGui::IsPopupOpen("context"))
			ImGui::SetNextWindowSizeConstraints(ImVec2(220.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
		if (ImGui::BeginPopup("context")) {
			ImGui::SetNextItemWidth(s.GlyphWidth * 7 + style.FramePadding.x * 2.0f);
			if (ImGui::DragInt("##cols", &Cols, 0.2f, 4, 32, "HexEditor.ContextMenu.NCols"_lc)) {
				ContentsWidthChanged = true;
				if (Cols < 1)
					Cols = 1;
			}
			ImGui::Checkbox("HexEditor.ContextMenu.ShowDataPreview"_lc, &OptShowDataPreview);
			ImGui::Checkbox("HexEditor.ContextMenu.ShowHexII"_lc, &OptShowHexII);
			if (ImGui::Checkbox("HexEditor.ContextMenu.ShowAscii"_lc, &OptShowAscii)) {
				ContentsWidthChanged = true;
			}
			ImGui::Checkbox("HexEditor.ContextMenu.GreyOutZeros"_lc, &OptGreyOutZeroes);
			ImGui::Checkbox("HexEditor.ContextMenu.UppercaseHex"_lc, &OptUpperCaseHex);

			ImGui::EndPopup();
		}
		ImGui::PopStyleVar(4);

		ImGui::SameLine();
		ImGui::Text(format_range, "HexEditor.Range"_lc, s.AddrDigitsCount, base_display_addr, s.AddrDigitsCount, base_display_addr + mem_size - 1);
		ImGui::SameLine();
		ImGui::SetNextItemWidth((s.AddrDigitsCount + 1) * s.GlyphWidth + style.FramePadding.x * 2.0f);
		if (ImGui::InputText("##addr", AddrInputBuf, IM_ARRAYSIZE(AddrInputBuf), ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
			size_t goto_addr;
			if (sscanf(AddrInputBuf, "%" _PRISizeT "X", &goto_addr) == 1) {
				GotoAddr = goto_addr - base_display_addr;
				HighlightMin = HighlightMax = (size_t)-1;
			}
		}

		if (GotoAddr != (size_t)-1) {
			if (GotoAddr < mem_size) {
				ImGui::BeginChild("##scrolling");
				ImGui::SetScrollFromPosY(ImGui::GetCursorStartPos().y + (GotoAddr / Cols) * ImGui::GetTextLineHeight());
				ImGui::EndChild();
				DataEditingAddr = DataPreviewAddr = GotoAddr;
				DataEditingTakeFocus = true;
			}
			GotoAddr = (size_t)-1;
		}
	}

	void DrawPreviewLine(const Sizes& s, void* mem_data_void, size_t mem_size, size_t base_display_addr) {
		IM_UNUSED(base_display_addr);
		ImU8* mem_data = (ImU8*)mem_data_void;
		ImGuiStyle& style = ImGui::GetStyle();
		ImGui::AlignTextToFramePadding();
		ImGui::Text("%s", "HexEditor.PreviewAs"_lc);

		ImGui::SameLine();
		ImGui::SetNextItemWidth((s.GlyphWidth * 10.0f) + style.FramePadding.x * 2.0f + style.ItemInnerSpacing.x);
		if (ImGui::BeginCombo("##combo_type", DataTypeGetDesc(PreviewDataType), ImGuiComboFlags_HeightLargest)) {
			for (int n = 0; n < ImGuiDataType_COUNT; n++)
				if (ImGui::Selectable(DataTypeGetDesc((ImGuiDataType)n), PreviewDataType == n))
					PreviewDataType = (ImGuiDataType)n;
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth((s.GlyphWidth * 6.0f) + style.FramePadding.x * 2.0f + style.ItemInnerSpacing.x);
		ImGui::Combo("##combo_endianess", &PreviewEndianess, "LE\0BE\0\0");

		char buf[128] = "";
		float x = s.GlyphWidth * 6.0f;
		bool has_value = DataPreviewAddr != (size_t)-1;
		if (has_value)
			DrawPreviewData(DataPreviewAddr, mem_data, mem_size, PreviewDataType, DataFormat_Dec, buf, (size_t)IM_ARRAYSIZE(buf));
		ImGui::Text("Dec");
		ImGui::SameLine(x);
		ImGui::TextUnformatted(has_value ? buf : "N/A");
		if (has_value)
			DrawPreviewData(DataPreviewAddr, mem_data, mem_size, PreviewDataType, DataFormat_Hex, buf, (size_t)IM_ARRAYSIZE(buf));
		ImGui::Text("Hex");
		ImGui::SameLine(x);
		ImGui::TextUnformatted(has_value ? buf : "N/A");
		if (has_value)
			DrawPreviewData(DataPreviewAddr, mem_data, mem_size, PreviewDataType, DataFormat_Bin, buf, (size_t)IM_ARRAYSIZE(buf));
		buf[IM_ARRAYSIZE(buf) - 1] = 0;
		ImGui::Text("Bin");
		ImGui::SameLine(x);
		ImGui::TextUnformatted(has_value ? buf : "N/A");
	}
	// Utilities for Data Preview
	const char* DataTypeGetDesc(ImGuiDataType data_type) const {
		const char* descs[] = {"Int8", "Uint8", "Int16", "Uint16", "Int32", "Uint32", "Int64", "Uint64", "Float", "Double"};
		IM_ASSERT(data_type >= 0 && data_type < ImGuiDataType_COUNT);
		return descs[data_type];
	}

	size_t DataTypeGetSize(ImGuiDataType data_type) const {
		const size_t sizes[] = {1, 1, 2, 2, 4, 4, 8, 8, sizeof(float), sizeof(double)};
		IM_ASSERT(data_type >= 0 && data_type < ImGuiDataType_COUNT);
		return sizes[data_type];
	}

	const char* DataFormatGetDesc(DataFormat data_format) const {
		const char* descs[] = {"Bin", "Dec", "Hex"};
		IM_ASSERT(data_format >= 0 && data_format < DataFormat_COUNT);
		return descs[data_format];
	}

	bool IsBigEndian() const {
		uint16_t x = 1;
		char c[2];
		memcpy(c, &x, 2);
		return c[0] != 0;
	}

	static void* EndianessCopyBigEndian(void* _dst, void* _src, size_t s, int is_little_endian) {
		if (is_little_endian) {
			uint8_t* dst = (uint8_t*)_dst;
			uint8_t* src = (uint8_t*)_src + s - 1;
			for (int i = 0, n = (int)s; i < n; ++i)
				memcpy(dst++, src--, 1);
			return _dst;
		}
		else {
			return memcpy(_dst, _src, s);
		}
	}

	static void* EndianessCopyLittleEndian(void* _dst, void* _src, size_t s, int is_little_endian) {
		if (is_little_endian) {
			return memcpy(_dst, _src, s);
		}
		else {
			uint8_t* dst = (uint8_t*)_dst;
			uint8_t* src = (uint8_t*)_src + s - 1;
			for (int i = 0, n = (int)s; i < n; ++i)
				memcpy(dst++, src--, 1);
			return _dst;
		}
	}

	void* EndianessCopy(void* dst, void* src, size_t size) const {
		static void* (*fp)(void*, void*, size_t, int) = NULL;
		if (fp == NULL)
			fp = IsBigEndian() ? EndianessCopyBigEndian : EndianessCopyLittleEndian;
		return fp(dst, src, size, PreviewEndianess);
	}

	const char* FormatBinary(const uint8_t* buf, int width) const {
		IM_ASSERT(width <= 64);
		size_t out_n = 0;
		static char out_buf[64 + 8 + 1];
		int n = width / 8;
		for (int j = n - 1; j >= 0; --j) {
			for (int i = 0; i < 8; ++i)
				out_buf[out_n++] = (buf[j] & (1 << (7 - i))) ? '1' : '0';
			out_buf[out_n++] = ' ';
		}
		IM_ASSERT(out_n < IM_ARRAYSIZE(out_buf));
		out_buf[out_n] = 0;
		return out_buf;
	}

	// [Internal]
	void DrawPreviewData(size_t addr, const ImU8* mem_data, size_t mem_size, ImGuiDataType data_type, DataFormat data_format, char* out_buf, size_t out_buf_size) const {
		uint8_t buf[8];
		size_t elem_size = DataTypeGetSize(data_type);
		size_t size = addr + elem_size > mem_size ? mem_size - addr : elem_size;
		if (ReadFn)
			for (int i = 0, n = (int)size; i < n; ++i)
				buf[i] = ReadFn(mem_data, addr + i);
		else
			memcpy(buf, mem_data + addr, size);

		if (data_format == DataFormat_Bin) {
			uint8_t binbuf[8];
			EndianessCopy(binbuf, buf, size);
			ImSnprintf(out_buf, out_buf_size, "%s", FormatBinary(binbuf, (int)size * 8));
			return;
		}

		out_buf[0] = 0;
		switch (data_type) {
		case ImGuiDataType_S8: {
			int8_t int8 = 0;
			EndianessCopy(&int8, buf, size);
			if (data_format == DataFormat_Dec) {
				ImSnprintf(out_buf, out_buf_size, "%hhd", int8);
				return;
			}
			if (data_format == DataFormat_Hex) {
				ImSnprintf(out_buf, out_buf_size, "0x%02x", int8 & 0xFF);
				return;
			}
			break;
		}
		case ImGuiDataType_U8: {
			uint8_t uint8 = 0;
			EndianessCopy(&uint8, buf, size);
			if (data_format == DataFormat_Dec) {
				ImSnprintf(out_buf, out_buf_size, "%hhu", uint8);
				return;
			}
			if (data_format == DataFormat_Hex) {
				ImSnprintf(out_buf, out_buf_size, "0x%02x", uint8 & 0XFF);
				return;
			}
			break;
		}
		case ImGuiDataType_S16: {
			int16_t int16 = 0;
			EndianessCopy(&int16, buf, size);
			if (data_format == DataFormat_Dec) {
				ImSnprintf(out_buf, out_buf_size, "%hd", int16);
				return;
			}
			if (data_format == DataFormat_Hex) {
				ImSnprintf(out_buf, out_buf_size, "0x%04x", int16 & 0xFFFF);
				return;
			}
			break;
		}
		case ImGuiDataType_U16: {
			uint16_t uint16 = 0;
			EndianessCopy(&uint16, buf, size);
			if (data_format == DataFormat_Dec) {
				ImSnprintf(out_buf, out_buf_size, "%hu", uint16);
				return;
			}
			if (data_format == DataFormat_Hex) {
				ImSnprintf(out_buf, out_buf_size, "0x%04x", uint16 & 0xFFFF);
				return;
			}
			break;
		}
		case ImGuiDataType_S32: {
			int32_t int32 = 0;
			EndianessCopy(&int32, buf, size);
			if (data_format == DataFormat_Dec) {
				ImSnprintf(out_buf, out_buf_size, "%d", int32);
				return;
			}
			if (data_format == DataFormat_Hex) {
				ImSnprintf(out_buf, out_buf_size, "0x%08x", int32);
				return;
			}
			break;
		}
		case ImGuiDataType_U32: {
			uint32_t uint32 = 0;
			EndianessCopy(&uint32, buf, size);
			if (data_format == DataFormat_Dec) {
				ImSnprintf(out_buf, out_buf_size, "%u", uint32);
				return;
			}
			if (data_format == DataFormat_Hex) {
				ImSnprintf(out_buf, out_buf_size, "0x%08x", uint32);
				return;
			}
			break;
		}
		case ImGuiDataType_S64: {
			int64_t int64 = 0;
			EndianessCopy(&int64, buf, size);
			if (data_format == DataFormat_Dec) {
				ImSnprintf(out_buf, out_buf_size, "%lld", (long long)int64);
				return;
			}
			if (data_format == DataFormat_Hex) {
				ImSnprintf(out_buf, out_buf_size, "0x%016llx", (long long)int64);
				return;
			}
			break;
		}
		case ImGuiDataType_U64: {
			uint64_t uint64 = 0;
			EndianessCopy(&uint64, buf, size);
			if (data_format == DataFormat_Dec) {
				ImSnprintf(out_buf, out_buf_size, "%llu", (long long)uint64);
				return;
			}
			if (data_format == DataFormat_Hex) {
				ImSnprintf(out_buf, out_buf_size, "0x%016llx", (long long)uint64);
				return;
			}
			break;
		}
		case ImGuiDataType_Float: {
			float float32 = 0.0f;
			EndianessCopy(&float32, buf, size);
			if (data_format == DataFormat_Dec) {
				ImSnprintf(out_buf, out_buf_size, "%f", float32);
				return;
			}
			if (data_format == DataFormat_Hex) {
				ImSnprintf(out_buf, out_buf_size, "%a", float32);
				return;
			}
			break;
		}
		case ImGuiDataType_Double: {
			double float64 = 0.0;
			EndianessCopy(&float64, buf, size);
			if (data_format == DataFormat_Dec) {
				ImSnprintf(out_buf, out_buf_size, "%f", float64);
				return;
			}
			if (data_format == DataFormat_Hex) {
				ImSnprintf(out_buf, out_buf_size, "%a", float64);
				return;
			}
			break;
		}
		case ImGuiDataType_COUNT:
			break;
		} // Switch
		IM_ASSERT(0); // Shouldn't reach
	}
};

#undef _PRISizeT
#undef ImSnprintf

#ifdef _MSC_VER
#pragma warning(pop)
#endif