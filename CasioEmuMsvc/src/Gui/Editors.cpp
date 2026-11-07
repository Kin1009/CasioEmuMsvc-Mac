#include "Editors.h"
#include "CPU.hpp"
#include "Chipset/Chipset.hpp"
#include "CodeViewer.hpp"
#include "Hooks.h"
#include "Localization.h"
#include "MemBreakPoint.hpp"
#include "Models.h"
#include "ToolUi.hpp"
#include "Ui.hpp"
#include "hex.hpp"
#include "ePSCpu.h"
#include "Chipset/T4xCore.hpp"
#include <algorithm>

namespace {
	constexpr uint32_t kEpsFlashBaseWord = 0x18000;
	constexpr size_t kEpsFlashBaseByte = static_cast<size_t>(kEpsFlashBaseWord) * 2;
}

float ram_edit_ov[0x100000]{};

// Per-window hex column selector.  MemoryEditor stores the byte count in
// `Cols`; one byte is two hex characters, so 16 columns is 32 characters (plus
// the ASCII pane).  Keeping the label on the left matches the rest of the
// debugger's choosers.
static void DrawColumnSelector(MemoryEditor& editor) {
	// The hex row is address + hex bytes + ASCII pane.  Cap the column count so
	// a narrow tool dock shows complete rows instead of clipping the line, and
	// let the Columns control shrink it further.
	const float char_width = std::max(1.0f, ImGui::CalcTextSize("0").x);
	const int max_columns = std::clamp(
		static_cast<int>((ImGui::GetContentRegionAvail().x - char_width * 22.0f) / (char_width * 4.0f)),
		2, 64);
	if (const char* forced = std::getenv("CASIOEMU_HEX_COLS"))
		editor.Cols = std::clamp(std::atoi(forced), 1, 64);
	if (const char* target = std::getenv("CASIOEMU_HEX_GOTO")) {
		if (!editor.GotoEnvApplied) {
			editor.GotoEnvApplied = true;
			editor.GotoAddr = static_cast<size_t>(std::strtoul(target, nullptr, 16));
		}
	}
	else if (editor.Cols > max_columns)
		editor.Cols = max_columns;
	ToolUi::Field("Columns");
	ImGui::SetNextItemWidth(std::min(160.0f, std::max(90.0f, ImGui::GetContentRegionAvail().x * 0.4f)));
	int cols = editor.Cols;
	if (ImGui::InputInt("##hex_columns", &cols, 1, 4)) {
		editor.Cols = std::clamp(cols, 1, max_columns);
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
		ImGui::BeginTooltip();
		ImGui::TextUnformatted("Bytes shown per line (2 hex characters per byte).");
		ImGui::EndTooltip();
	}
	ImGui::SameLine();
	ImGui::TextDisabled("bytes per line");
	ImGui::SameLine();
	ImGui::TextDisabled("Tint");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(std::min(150.0f, std::max(90.0f, ImGui::GetContentRegionAvail().x * 0.3f)));
	float tint_percent = MemoryEditor::EffectiveSpanTintOpacity(
		ThemeManager::Instance().IsDarkMode()) * 100.0f;
	if (ImGui::SliderFloat("##span_tint", &tint_percent, 0.0f, 100.0f, "%.0f%%"))
		MemoryEditor::SpanTintOpacity = std::clamp(tint_percent / 100.0f, 0.0f, 1.0f);
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
		ImGui::BeginTooltip();
		ImGui::TextUnformatted("Opacity of the RAM/ROM span highlights. 0% hides them, 100% is solid.");
		ImGui::EndTooltip();
	}
	ImGui::Spacing();
	ImGui::TextDisabled("Vibrancy");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(std::min(120.0f, std::max(70.0f, ImGui::GetContentRegionAvail().x * 0.25f)));
	float vibrancy = MemoryEditor::SpanTintSaturation * 100.0f;
	if (ImGui::SliderFloat("##span_vibrancy", &vibrancy, 0.0f, 100.0f, "%.0f%%"))
		MemoryEditor::SpanTintSaturation = std::clamp(vibrancy / 100.0f, 0.0f, 1.0f);
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
		ImGui::BeginTooltip();
		ImGui::TextUnformatted("Saturation of the span highlights. Higher values keep each span's hue vivid.");
		ImGui::EndTooltip();
	}
	ImGui::SameLine();
	ImGui::Checkbox("One colour", &MemoryEditor::SpanTintUniform);
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
		ImGui::BeginTooltip();
		ImGui::TextUnformatted("Use the picked colour for every span instead of each span's own hue.");
		ImGui::EndTooltip();
	}
	ImGui::SameLine();
	if (!MemoryEditor::SpanTintUniform)
		ImGui::BeginDisabled();
	ImGui::SetNextItemWidth(28.0f);
	ImGui::ColorEdit3("##span_colour", &MemoryEditor::SpanTintColor.x,
		ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha);
	if (!MemoryEditor::SpanTintUniform)
		ImGui::EndDisabled();
	ImGui::Spacing();
}

struct HexEditor : public UIWindow, public MemoryEditor {
	void* data{};
	size_t size{};
	size_t display_base{};
	bool open_popup = false;
	size_t popup_p = 0;
	HexEditor(const char* name, void* data, size_t size, size_t base) : UIWindow(name), data(data), size(size), display_base(base) {
		flags = ImGuiWindowFlags_NoScrollbar;
		this->ram_edit_ov = ::ram_edit_ov;
		contextmenuuserdata = this;
		ContextMenuFn = [](void* userdata, size_t where) {
			((HexEditor*)userdata)->open_popup = true;
			((HexEditor*)userdata)->popup_p = where;
		};
	}
	void RenderCore() override {
		ToolUi::PageScope page("Memory Editor");
		ToolUi::Section(name);
		this->HighlightColor = ThemeManager::Instance().IsDarkMode()
			? IM_COL32(255, 255, 255, 45)
			: IM_COL32(0, 0, 0, 32);
		DrawColumnSelector(*this);
		ImGui::Spacing();
		const float body_height = std::max(200.0f, ImGui::GetContentRegionAvail().y);
		ToolUi::PanelScope frame("##hex_frame", body_height);
		this->DrawContents(data, size, display_base);
		if (open_popup) {
			ImGui::OpenPopup("ContextMenu");
			open_popup = false;
		}
		ToolUi::PopupScope popup("ContextMenu");
		if (popup) {
			UIHelpers::ClickableAddress(popup_p, UIHelpers::JumpTarget::Memory);
			if (ImGui::MenuItem("HexEditors.ContextMenu.MonitorWrite"_lc)) {
				SetMemBp(popup_p, true);
			}
			if (ImGui::MenuItem("HexEditors.ContextMenu.MonitorRead"_lc)) {
				SetMemBp(popup_p, false);
			}
		}
	}
	void GotoMemoryAddress(uint32_t addr) override {
		if (m_emu->chipset.t4x) addr -= static_cast<uint32_t>(display_base);
		BringToFront();
		GotoAddrAndHighlight(addr, addr + 1);
	}
};
struct SpansHexEditor : public UIWindow, public MemoryEditor {
	void* data{};
	size_t size{};
	size_t display_base{};
	std::vector<MarkedSpan> spans{};
	bool open_popup = false;
	size_t popup_p = 0;
	SpansHexEditor(const char* name, void* data, size_t size, size_t base, std::vector<MarkedSpan> spans) : UIWindow(name), data(data), size(size), display_base(base), spans(spans) {
		flags = ImGuiWindowFlags_NoScrollbar;
		this->ram_edit_ov = ::ram_edit_ov;
		contextmenuuserdata = this;
		ContextMenuFn = [](void* userdata, size_t where) {
			((SpansHexEditor*)userdata)->open_popup = true;
			((SpansHexEditor*)userdata)->popup_p = where;
			// ImGui::OpenPopup("ContextMenu");
		};
	}
	void RenderCore() override {
		ToolUi::PageScope page("Memory Editor");
		ToolUi::Section(name);
		this->HighlightColor = ThemeManager::Instance().IsDarkMode()
			? IM_COL32(255, 255, 255, 45)
			: IM_COL32(0, 0, 0, 32);
		DrawColumnSelector(*this);
		ImGui::Spacing();
		const float body_height = std::max(200.0f, ImGui::GetContentRegionAvail().y);
		ToolUi::PanelScope frame("##hex_frame", body_height);
		this->DrawContents(data, size, display_base, spans);
		if (open_popup) {
			ImGui::OpenPopup("ContextMenu");
			open_popup = false;
		}
		ToolUi::PopupScope popup("ContextMenu");
		if (popup) {
			UIHelpers::ClickableAddress(popup_p, UIHelpers::JumpTarget::Memory);
			if (ImGui::MenuItem("HexEditors.ContextMenu.MonitorWrite"_lc)) {
				SetMemBp(popup_p, true);
			}
			if (ImGui::MenuItem("HexEditors.ContextMenu.MonitorRead"_lc)) {
				SetMemBp(popup_p, false);
			}
		}
	}
	void GotoMemoryAddress(uint32_t addr) override {
		BringToFront();
		GotoAddrAndHighlight(addr, addr + 1);
	}
};
inline auto MMU_Hex(auto he) {
	he->ReadFn = [](const ImU8* data, size_t off) -> ImU8 {
		return me_mmu->ReadData((size_t)data + off, 0);
	};
	he->WriteFn = [](ImU8* data, size_t off, ImU8 d) {
		return me_mmu->WriteData((size_t)data + off, d, 0);
	};
	return he;
}
inline auto Highlight_Default(auto he) {
	he->HighlightFn = [](const ImU8* data, size_t off) -> bool {
		if ((size_t)(data + off) == m_emu->chipset.cpu.reg_sp) {
			return true;
		}
		if (casioemu::HasInputArea(m_emu->hardware_id) &&
			(size_t)(data + off) == casioemu::GetInputAreaOffset(m_emu->hardware_id) + *((unsigned char*)n_ram_buffer - casioemu::GetRamBaseAddr(m_emu->hardware_id) + casioemu::GetCursorOffset(m_emu->hardware_id))) {
			return true;
		}
		return false;
	};
	return he;
}
inline auto EPS_ROM_Hex(auto he) {
	he->ReadFn = [](const ImU8*, size_t off) -> ImU8 {
		if (!m_emu->chipset.epscpu)
			return 0xff;
		const auto word = m_emu->chipset.epscpu->ReadCodeWord(static_cast<uint32_t>(off / 2));
		return static_cast<ImU8>((off & 1) ? word : (word >> 8));
	};
	he->WriteFn = [](ImU8*, size_t off, ImU8 value) {
		auto* eps = m_emu->chipset.epscpu;
		if (!eps)
			return;
		const uint32_t word_address = static_cast<uint32_t>(off / 2);
		auto word = eps->ReadCodeWord(word_address);
		word = (off & 1)
			? static_cast<uint16_t>((word & 0xff00) | value)
			: static_cast<uint16_t>((word & 0x00ff) | (static_cast<uint16_t>(value) << 8));
		if (!eps->WriteCodeWord(word_address, word))
			return;
		eps->WriteRomImageWord(m_emu->chipset.rom_data, word_address, word);
	};
	return he;
}
inline auto EPS_FLASH_Hex(auto he) {
	he->ReadFn = [](const ImU8*, size_t off) -> ImU8 {
		if (!m_emu->chipset.epscpu)
			return 0xff;
		const auto word = m_emu->chipset.epscpu->ReadCodeWord(
			kEpsFlashBaseWord + static_cast<uint32_t>(off / 2));
		return static_cast<ImU8>((off & 1) ? word : (word >> 8));
	};
	he->WriteFn = [](ImU8*, size_t off, ImU8 value) {
		auto* eps = m_emu->chipset.epscpu;
		if (!eps)
			return;
		const uint32_t flash_word_offset = static_cast<uint32_t>(off / 2);
		const uint32_t word_address = kEpsFlashBaseWord + flash_word_offset;
		auto word = eps->ReadCodeWord(word_address);
		word = (off & 1)
			? static_cast<uint16_t>((word & 0xff00) | value)
			: static_cast<uint16_t>((word & 0x00ff) | (static_cast<uint16_t>(value) << 8));
		if (!eps->WriteCodeWord(word_address, word))
			return;
		eps->WriteFlashImageWord(m_emu->chipset.flash_data, flash_word_offset, word);
	};
	return he;
}

inline auto EPS_VRAM_Hex(auto he) {
	he->ReadFn = [](const ImU8*, size_t off) -> ImU8 {
		return m_emu->chipset.epscpu ? m_emu->chipset.epscpu->ReadLcdMemory(off) : 0xff;
	};
	he->WriteFn = [](ImU8*, size_t off, ImU8 value) {
		if (m_emu->chipset.epscpu)
			m_emu->chipset.epscpu->WriteLcdMemory(off, value);
	};
	return he;
}

std::vector<UIWindow*> GetEditors() {
	std::vector<UIWindow*> windows;
	if (m_emu->chipset.t4x) {
		const char* names[] = {"T4x Registers (nibbles)", "T4x WRAM (nibbles)", "T4x DRAM (bytes)", "T4x ROM (bytes)"};
		const size_t sizes[] = {64, 1024, 4096, m_emu->chipset.rom_data.size()};
		const size_t bases[] = {0, 0x100, 0x1000, 0};
		for (unsigned space = 0; space < 4; ++space) {
			auto* editor = new HexEditor(names[space], reinterpret_cast<void*>(static_cast<uintptr_t>(space)), sizes[space], bases[space]);
			if (space == 3) editor->ContextMenuFn = nullptr; // ROM has execution breakpoints in CodeViewer.
			editor->ReadFn = [](const ImU8* data, size_t off) -> ImU8 {
				auto* core = m_emu->chipset.t4x;
				unsigned space = static_cast<unsigned>(reinterpret_cast<uintptr_t>(data));
				if (space != 3)
					return core->ReadMemory(space, static_cast<unsigned>(off));
				return core->ReadCodeByte(static_cast<unsigned>(off));
			};
			editor->WriteFn = [](ImU8* data, size_t off, ImU8 value) {
				if (!m_emu->GetPaused())
					return;
				auto* core = m_emu->chipset.t4x;
				unsigned space = static_cast<unsigned>(reinterpret_cast<uintptr_t>(data));
				if (space != 3)
					core->WriteMemory(space, static_cast<unsigned>(off), value);
				else {
					m_emu->chipset.WriteTiCode(off, {&value, 1});
				}
			};
			windows.push_back(editor);
		}
		return windows;
	}
	SetupHook(on_memory_write, [](casioemu::MMU& mmu, MemoryEventArgs& mea) {
		if (mea.offset < 0x80000)
			ram_edit_ov[mea.offset] = 255;
	});
	if (casioemu::IsEpsFamily(m_emu->hardware_id)) {
		const size_t rom_display_bytes = m_emu->chipset.epscpu->RomFormat() == casioemu::Eps6800RomFormat::UnpackedNibbles
											 ? m_emu->chipset.rom_data.size() / 2
											 : m_emu->chipset.rom_data.size();
		windows.push_back(EPS_ROM_Hex(new HexEditor{"ROM", nullptr, rom_display_bytes, 0}));
		if (!m_emu->chipset.flash_data.empty())
			windows.push_back(EPS_FLASH_Hex(new HexEditor{"Flash", nullptr, m_emu->chipset.flash_data.size(), kEpsFlashBaseByte}));
		windows.push_back(MMU_Hex(new HexEditor{"RAM", nullptr, 0x2080, 0}));
		windows.push_back(MMU_Hex(new HexEditor{"Regs", nullptr, 0x80, 0}));
		windows.push_back(EPS_VRAM_Hex(new HexEditor{"VRAM", nullptr, m_emu->chipset.epscpu->LcdRawSize(), 0}));
	}
	else {
		auto* rom_editor = new HexEditor{"ROM", m_emu->chipset.rom_data.data(), m_emu->chipset.rom_data.size(), 0};
		rom_editor->SelSpansEnabled = false;
		windows.push_back(rom_editor);
		auto* ram_editor = Highlight_Default(
			MMU_Hex(
				new SpansHexEditor{
					"RAM",
					(void*)casioemu::GetRamBaseAddr(m_emu->hardware_id),
					casioemu::GetRamEditorSize(m_emu->hardware_id),
					casioemu::GetRamBaseAddr(m_emu->hardware_id),
					GetCommonMemLabels(m_emu->hardware_id)}));
		ram_editor->SelSpansEnabled = true;
		windows.push_back(ram_editor);
		if (m_emu->hardware_id == casioemu::HW_FX_5800P) {
			auto* pram_editor = MMU_Hex(new SpansHexEditor{"PRAM", (void*)0x40000, 0x8000, 0x40000, GetCommonMemLabels(m_emu->hardware_id)});
			pram_editor->SelSpansEnabled = true;
			windows.push_back(pram_editor);
			windows.push_back(new HexEditor{"Flash", m_emu->chipset.flash_data.data(), m_emu->chipset.flash_data.size(), 0});
		}
		auto* all_editor = MMU_Hex(new SpansHexEditor{"All", 0, 0xfffff, 0, GetCommonMemLabels(m_emu->hardware_id)});
		all_editor->SelSpansEnabled = true;
		const auto hardware = m_emu->hardware_id;
		all_editor->SelSpansAllowed = [hardware](size_t address) {
			const size_t base = casioemu::GetRamBaseAddr(hardware);
			return address >= base && address < base + casioemu::GetRamEditorSize(hardware);
		};
		windows.push_back(all_editor);
	}
	return windows;
}
