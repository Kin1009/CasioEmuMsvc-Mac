#include "WatchWindow.hpp"
#include "Chipset/CPU.hpp"
#include "Chipset/Chipset.hpp"
#include "Chipset/ePSCpu.h"
#include "Chipset/T4xCore.hpp"
#include "CodeViewer.hpp"
#include "Config.hpp"
#include "Models.h"
#include "Peripheral/BatteryBackedRAM.hpp"
#include "ToolUi.hpp"
#include "Ui.hpp"
#include "imgui/imgui.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <stdlib.h>

namespace {

// Registers are laid out in a real table so eight values per row line up and a
// narrow tool dock gets four per row instead of clipping.
int RegistersPerRow() {
	return ImGui::GetContentRegionAvail().x >= 620.0f ? 8 : 4;
}

} // namespace

void WatchWindow::PrepareRX() {
	auto eps = m_emu->chipset.epscpu;
	if (eps) {
		const auto snapshot = eps->DebugSnapshot();
		const auto& regs = snapshot.registers;
		snprintf(reg_pc, sizeof(reg_pc), "%05x", snapshot.program_counter);
		const auto format_indirect = [](uint8_t fsr, uint8_t bsr, char* out, size_t out_size) {
			if (fsr & 0x80)
				snprintf(out, out_size, "%05x", (uint32_t)((bsr << 7) | (fsr & 0x7f)));
			else
				snprintf(out, out_size, "%02x(SFR)", (uint32_t)(fsr & 0x7f));
		};
		format_indirect(regs[0x01], regs[0x02], reg_lr, sizeof(reg_lr));
		format_indirect(regs[0x04], regs[0x05], reg_ea, sizeof(reg_ea));
		format_indirect(regs[0x11], regs[0x12], reg_ex1, sizeof(reg_ex1));
		snprintf(reg_ex2, sizeof(reg_ex2), "%05x",
			(static_cast<uint32_t>(regs[0x23] & 0x03) << 8) | regs[0x22]);
		snprintf(reg_sp, sizeof(reg_sp), "%04x", snapshot.stack_pointer);
		snprintf(reg_psw, sizeof(reg_psw), "%02x", regs[0x0f]);
		snprintf(reg_dsr, sizeof(reg_dsr), "%02x", regs[0x02]);
	}
	else {
		for (int i = 0; i < 16; i++) {
            snprintf((char*)reg_rx[i], sizeof(reg_rx[i]), "%02x", m_emu->chipset.cpu.reg_r[i] & 0x0ff);
		}
        snprintf(reg_pc, sizeof(reg_pc), "%05x", (uint32_t)(m_emu->chipset.cpu.reg_csr << 16) | m_emu->chipset.cpu.reg_pc);
        snprintf(reg_lr, sizeof(reg_lr), "%05x", (uint32_t)(m_emu->chipset.cpu.reg_lcsr << 16) | m_emu->chipset.cpu.reg_lr);
        snprintf(reg_sp, sizeof(reg_sp), "%04x", m_emu->chipset.cpu.reg_sp | 0);
        snprintf(reg_ea, sizeof(reg_ea), "%04x", m_emu->chipset.cpu.reg_ea | 0);
        snprintf(reg_psw, sizeof(reg_psw), "%02x", m_emu->chipset.cpu.reg_psw | 0);
        snprintf(reg_dsr, sizeof(reg_dsr), "%02x", m_emu->chipset.cpu.reg_dsr | 0);
	}
}

void WatchWindow::ShowRX() {
	if (m_emu->chipset.epscpu) {
	}
	else {
		const int per_row = RegistersPerRow();
		if (ToolUi::TableScope table{"##rx_grid", per_row}) {
			for (int base = 0; base < 16; base += per_row) {
				ImGui::TableNextRow();
				for (int col = 0; col < per_row && base + col < 16; ++col) {
					ImGui::TableNextColumn();
					ImGui::TextColored(~UIHelpers::kColorMuted, "R%d", base + col);
					ImGui::SameLine();
					ImGui::TextUnformatted((char*)&reg_rx[base + col][0]);
				}
			}
		}
		if (ToolUi::TableScope table{"##er_grid", per_row / 2}) {
			for (int base = 0; base < 16; base += per_row) {
				ImGui::TableNextRow();
				for (int col = 0; col + 1 < per_row && base + col + 1 < 16; col += 2) {
					ImGui::TableNextColumn();
					uint16_t val = m_emu->chipset.cpu.reg_r[base + col + 1] << 8 |
						m_emu->chipset.cpu.reg_r[base + col];
					ImGui::TextColored(~UIHelpers::kColorMuted, "ER%d", base + col);
					ImGui::SameLine();
					ImGui::Text("%04x", val);
				}
			}
		}
	}
	auto show_sfr = ([&](char* ptr, const char* label) {
		ImGui::TextColored(~UIHelpers::kColorMuted, "%s", label);
		ImGui::SameLine();
		ImGui::TextUnformatted(ptr);
	});
	if (ToolUi::TableScope table{"##sfr_grid", 2}) {
		ImGui::TableNextRow();
		ImGui::TableNextColumn(); show_sfr(reg_pc, "PC");
		ImGui::TableNextColumn(); show_sfr(reg_sp, m_emu->chipset.epscpu ? "STKPTR" : "SP");
		ImGui::TableNextRow();
		ImGui::TableNextColumn(); show_sfr(reg_lr, m_emu->chipset.epscpu ? "INDF0" : "LR");
		ImGui::TableNextColumn(); show_sfr(reg_psw, m_emu->chipset.epscpu ? "STATUS" : "PSW");
		ImGui::TableNextRow();
		ImGui::TableNextColumn(); show_sfr(reg_ea, m_emu->chipset.epscpu ? "INDF1" : "EA");
		ImGui::TableNextColumn(); show_sfr(reg_dsr, m_emu->chipset.epscpu ? "BSR" : "DSR");
		if (m_emu->chipset.epscpu) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); show_sfr(reg_ex1, "INDF2");
			ImGui::TableNextColumn(); show_sfr(reg_ex2, "LCDAR");
		}
	}
}
void WatchWindow::ModRX() {
	auto edit_sfr = ([&](char* ptr, const char* label, int i, int width) {
		ImGui::TextColored(~UIHelpers::kColorMuted, "%s", label);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::CalcTextSize("0").x * (width + 2) + 12.0f);
		ImGui::PushID(i);
		ImGui::InputText("##reg", ptr, width + 1, ImGuiInputTextFlags_CharsHexadecimal);
		ImGui::PopID();
	});
	if (m_emu->chipset.epscpu) {
		const auto snapshot = m_emu->chipset.epscpu->DebugSnapshot();
		const auto& regs = snapshot.registers;
		const auto show_indirect = [&](const char* label, int id, char* ptr, uint8_t fsr) {
			if (regs[fsr] & 0x80) {
				edit_sfr(ptr, label, id, 6);
				return;
			}
			ImGui::TextColored(~UIHelpers::kColorMuted, "%s", label);
			ImGui::SameLine();
			ImGui::TextUnformatted(ptr);
		};
		if (ToolUi::TableScope table{"##eps_reg_edit", 2}) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); edit_sfr(reg_pc, "PC", 1, 6);
			ImGui::TableNextColumn(); edit_sfr(reg_sp, "STKPTR", 5, 4);
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); show_indirect("INDF0", 2, reg_lr, 0x01);
			ImGui::TableNextColumn(); edit_sfr(reg_psw, "STATUS", 6, 2);
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); show_indirect("INDF1", 3, reg_ea, 0x04);
			ImGui::TableNextColumn();
			ImGui::TextColored(~UIHelpers::kColorMuted, "BSR");
			ImGui::SameLine();
			ImGui::TextUnformatted(reg_dsr);
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); show_indirect("INDF2", 4, reg_ex1, 0x11);
			ImGui::TableNextColumn(); edit_sfr(reg_ex2, "LCDAR", 8, 6);
		}
		return;
	}
	const int per_row = RegistersPerRow();
	if (ToolUi::TableScope table{"##rx_edit", per_row}) {
		for (int base = 0; base < 16; base += per_row) {
			ImGui::TableNextRow();
			for (int col = 0; col < per_row && base + col < 16; ++col) {
				ImGui::TableNextColumn();
				const int index = base + col;
				ImGui::TextColored(~UIHelpers::kColorMuted, "R%d", index);
				ImGui::SameLine();
				ImGui::SetNextItemWidth(ImGui::CalcTextSize("0").x * 4.0f + 12.0f);
				ImGui::PushID(index);
				ImGui::InputText("##rx", (char*)&reg_rx[index][0], 3, ImGuiInputTextFlags_CharsHexadecimal);
				ImGui::PopID();
			}
		}
	}
	if (ToolUi::TableScope table{"##er_grid_edit", per_row / 2}) {
		for (int base = 0; base < 16; base += per_row) {
			ImGui::TableNextRow();
			for (int col = 0; col + 1 < per_row && base + col + 1 < 16; col += 2) {
				ImGui::TableNextColumn();
				uint16_t val = m_emu->chipset.cpu.reg_r[base + col + 1] << 8 |
					m_emu->chipset.cpu.reg_r[base + col];
				ImGui::TextColored(~UIHelpers::kColorMuted, "ER%d", base + col);
				ImGui::SameLine();
				ImGui::Text("%04x", val);
			}
		}
	}
	if (ToolUi::TableScope table{"##sfr_edit", 2}) {
		ImGui::TableNextRow();
		ImGui::TableNextColumn(); edit_sfr(reg_pc, "PC", 1, 6);
		ImGui::TableNextColumn(); edit_sfr(reg_sp, "SP", 4, 4);
		ImGui::TableNextRow();
		ImGui::TableNextColumn(); edit_sfr(reg_lr, "LR", 2, 6);
		ImGui::TableNextColumn(); edit_sfr(reg_psw, "PSW", 5, 2);
		ImGui::TableNextRow();
		ImGui::TableNextColumn(); edit_sfr(reg_ea, "EA", 3, 4);
		ImGui::TableNextColumn(); edit_sfr(reg_dsr, "DSR", 6, 2);
	}
}

void WatchWindow::UpdateRX() {
	if (auto* eps = m_emu->chipset.epscpu) {
		const auto snapshot = eps->DebugSnapshot();
		const auto set_indirect = [eps](uint32_t value, bool uses_ram, uint8_t fsr_addr, uint8_t bsr_addr) {
			eps->WriteDebugMemory(fsr_addr, static_cast<uint8_t>((uses_ram ? 0x80 : 0) | (value & 0x7f)));
			if (uses_ram)
				eps->WriteDebugMemory(bsr_addr, static_cast<uint8_t>(value >> 7));
		};
		eps->SetPC(static_cast<uint32_t>(strtoul(reg_pc, nullptr, 16)));
		set_indirect(static_cast<uint32_t>(strtoul(reg_lr, nullptr, 16)), (snapshot.registers[0x01] & 0x80) != 0, 0x01, 0x02);
		set_indirect(static_cast<uint32_t>(strtoul(reg_ea, nullptr, 16)), (snapshot.registers[0x04] & 0x80) != 0, 0x04, 0x05);
		set_indirect(static_cast<uint32_t>(strtoul(reg_ex1, nullptr, 16)), (snapshot.registers[0x11] & 0x80) != 0, 0x11, 0x12);
		eps->WriteDebugMemory(0x06, static_cast<uint8_t>(strtoul(reg_sp, nullptr, 16)));
		eps->WriteDebugMemory(0x0f, static_cast<uint8_t>(strtoul(reg_psw, nullptr, 16)));
		const auto lcdar = static_cast<uint32_t>(strtoul(reg_ex2, nullptr, 16));
		eps->WriteDebugMemory(0x22, static_cast<uint8_t>(lcdar & 0xff));
		eps->WriteDebugMemory(0x23,
			static_cast<uint8_t>((snapshot.registers[0x23] & 0xfc) | ((lcdar >> 8) & 0x03)));
		return;
	}
	for (int i = 0; i < 16; i++) {
		m_emu->chipset.cpu.reg_r[i] = (uint8_t)strtol((char*)reg_rx[i], nullptr, 16);
	}
	auto pc = strtol((char*)reg_pc, nullptr, 16);
	m_emu->chipset.cpu.reg_pc = (uint16_t)pc;
	m_emu->chipset.cpu.reg_csr = pc >> 16;
	pc = strtol((char*)reg_lr, nullptr, 16);
	m_emu->chipset.cpu.reg_lr = (uint16_t)pc;
	m_emu->chipset.cpu.reg_lcsr = pc >> 16;
	m_emu->chipset.cpu.reg_ea = (uint16_t)strtol((char*)reg_ea, nullptr, 16);
	m_emu->chipset.cpu.reg_sp = (uint16_t)strtol((char*)reg_sp, nullptr, 16);
	m_emu->chipset.cpu.reg_psw = (uint16_t)strtol((char*)reg_psw, nullptr, 16);
}

void WatchWindow::RenderCore() {
	ToolUi::PageScope page("Watch");
	if (auto* core = m_emu->chipset.t4x) {
		const auto state = core->Snapshot();
		ToolUi::Section("Core");
		{
			ToolUi::PanelScope panel("##t4x_status");
			ImGui::Text("T4x PC (word): %04X   SP: %02X", state.pc,
				((state.reg[9] & 7) * 16 + (state.reg[8] & 14)) / 2);
			ImGui::Text("Instructions: %llu   Cycles: %llu",
				static_cast<unsigned long long>(state.instructions),
				static_cast<unsigned long long>(state.cycles));
			ImGui::Text("%s   Pending: %02X", state.halted ? "HALT" : "RUN", state.pending);
			ImGui::Spacing();
			if (ImGui::Button(m_emu->GetPaused() ? "Continue" : "Pause")) {
				if (m_emu->GetPaused()) core->RequestContinue(); else core->CancelDebugRun();
				m_emu->SetPaused(!m_emu->GetPaused());
			}
			ImGui::SameLine();
			ImGui::BeginDisabled(!m_emu->GetPaused());
			if (ImGui::Button("Step")) { core->RequestStepInto(); m_emu->chipset.StepTiMultiView(); }
			ImGui::EndDisabled();
		}
		ToolUi::Section("Registers");
		if (ToolUi::TableScope table{"T4xRegisters", 8}) {
			for (unsigned i = 0; i < 64; ++i) {
				ImGui::TableNextColumn();
				ImGui::Text("R%02u: %X", i, state.reg[i]);
			}
		}
		const auto stop = core->LastDebugStop();
		if (stop.stopped())
			ToolUi::Status("Debug stop", UIHelpers::kColorWarning);
		ToolUi::Section("Call Stack");
		{
			const auto frames = core->StackFrames();
			ToolUi::PanelScope panel("##t4x_stack");
			if (frames.empty())
				ToolUi::EmptyState("No stack frames.");
			for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
				UIHelpers::ClickableAddress(it->pc, UIHelpers::JumpTarget::Code);
				ImGui::SameLine();
				ImGui::Text("%s return %04X SP %02X", it->interrupt ? "IRQ" : "CALL", it->lr, it->sp);
			}
		}
		return;
	}

	char_width = ImGui::CalcTextSize("F").x;
	casioemu::Chipset& chipset = m_emu->chipset;
	ToolUi::Section("Registers");
	auto rm = m_emu->chipset.run_mode;
	using casioemu::Chipset::RM_HALT;
	using casioemu::Chipset::RM_RUN;
	using casioemu::Chipset::RM_STOP;
	ImGui::TextUnformatted(("WatchWindow.CoreStatus"_l + ": " +
							(rm == RM_RUN ? "Run" : rm == RM_STOP ? "Stop" : rm == RM_HALT ? "Halt" :
							 rm == casioemu::Chipset::RM_DEEP_HALT ? "DEEP-HALT" : "HALT-H"))
			.c_str());
	PrepareRX();
	if (chipset.epscpu) {
		const auto debug = chipset.epscpu->DebugSnapshot();
		ImGui::Text("Instructions: %llu   Cycles: %llu",
			static_cast<unsigned long long>(debug.instruction_count),
			static_cast<unsigned long long>(debug.cycle_count));
		if (ImGui::Checkbox("Trace recording", &eps_trace_enabled))
			chipset.epscpu->EnableTrace(eps_trace_enabled);
		ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Trace capacity").x + 120.0f);
		ImGui::SetNextItemWidth(ImGui::CalcTextSize("000000").x);
		if (ImGui::InputInt("Trace capacity", &eps_trace_capacity, 256, 1024)) {
			eps_trace_capacity = std::clamp(eps_trace_capacity, 0, 65536);
			chipset.epscpu->SetTraceCapacity(static_cast<size_t>(eps_trace_capacity));
		}
		ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Clear Trace").x + 20.0f);
		if (ImGui::Button("Clear Trace"))
			chipset.epscpu->ClearTrace();
	}
	ImGui::Spacing();
	if (!m_emu->GetPaused()) {
		ShowRX();
		ImGui::Spacing();
		if (ToolUi::AccentButton("WatchWindow.Pause"_lc)) {
			if (m_emu->chipset.epscpu)
				m_emu->chipset.epscpu->CancelDebugRun();
			m_emu->SetPaused(1);
		}
	}
	else {
		ModRX();
		UpdateRX();
		ImGui::Spacing();
		if (ToolUi::AccentButton("WatchWindow.Continue"_lc)) {
			if (m_emu->chipset.epscpu)
				m_emu->chipset.epscpu->RequestContinue();
			m_emu->SetPaused(0);
		}
	}

	ToolUi::Section("Call Stack");
	{
		const float stack_height = std::max(120.0f, ImGui::GetContentRegionAvail().y * 0.42f);
		if (ToolUi::TableScope table{"##Stack_trace", 6,
				ToolUi::TableFlags() | ImGuiTableFlags_ScrollY, stack_height}) {
			ImGui::TableSetupColumn("WatchWindow.Function"_lc, ImGuiTableColumnFlags_WidthFixed, 104);
			ImGui::TableSetupColumn("PC", ImGuiTableColumnFlags_WidthFixed, 60);
			ImGui::TableSetupColumn("SP", ImGuiTableColumnFlags_WidthFixed, 40);
			ImGui::TableSetupColumn("ER0", ImGuiTableColumnFlags_WidthFixed, 40);
			ImGui::TableSetupColumn("ER2", ImGuiTableColumnFlags_WidthFixed, 40);
			ImGui::TableSetupColumn("LR", ImGuiTableColumnFlags_WidthStretch, 1);
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableHeadersRow();
			if (chipset.epscpu) {
				const auto snapshot = chipset.epscpu->DebugSnapshot();
				const int count = static_cast<int>(std::min<size_t>(snapshot.stack_pointer, snapshot.stack.size()));
				ImGuiListClipper clipper;
				clipper.Begin(count);
				while (clipper.Step()) {
					for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
						const auto pc = snapshot.stack[static_cast<size_t>(index)];
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(lookup_symbol(pc, g_labels).c_str());
						ImGui::TableNextColumn();
						UIHelpers::ClickableAddress(pc);
						ImGui::TableNextColumn();
						ImGui::Text("%04X", static_cast<unsigned>(index));
						ImGui::TableNextColumn();
						ImGui::Text("%04X", 0);
						ImGui::TableNextColumn();
						ImGui::Text("%04X", 0);
						ImGui::TableNextColumn();
						ImGui::TextUnformatted("");
					}
				}
			}
			else {
				auto stack = chipset.cpu.stack.get();
				if (stack->empty()) {
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::PushStyleColor(ImGuiCol_Text, UIHelpers::kColorMuted);
					ImGui::TextUnformatted("No stack frames. Run or step to update stack trace.");
					ImGui::PopStyleColor();
					for (int col = 1; col < 6; ++col) {
						ImGui::TableNextColumn();
						ImGui::TextUnformatted("");
					}
				}
				else {
					const int count = static_cast<int>(stack->size());
					ImGuiListClipper clipper;
					clipper.Begin(count);
					while (clipper.Step()) {
						for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
							const auto& frame = (*stack)[static_cast<size_t>(count - 1 - index)];
							ImGui::TableNextRow();
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(lookup_symbol(frame.new_pc, g_labels).c_str());
							ImGui::TableNextColumn();
							UIHelpers::ClickableAddress(frame.new_pc);
							ImGui::TableNextColumn();
							ImGui::Text("%04X", frame.sp);
							ImGui::TableNextColumn();
							ImGui::Text("%04X", frame.er0);
							ImGui::TableNextColumn();
							ImGui::Text("%04X", frame.er2);
							ImGui::TableNextColumn();
							if (frame.lr_pushed) {
								if (frame.lr == 0xffffff)
									ImGui::TextUnformatted("WatchWindow.LrDestroyed"_lc);
								else
									ImGui::TextUnformatted(lookup_symbol(frame.lr, g_labels).c_str());
							}
						}
					}
				}
			}
		}
	}
	if (chipset.epscpu) {
		ToolUi::Section("EPS6800 Trace Buffer");
		ImGui::BeginChild("##eps_trace_buffer", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 10), true);
		if (ToolUi::TableScope table{"##eps_trace_table", 7}) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("#");
			ImGui::TableSetupColumn("Cycle");
			ImGui::TableSetupColumn("PC");
			ImGui::TableSetupColumn("Opcode");
			ImGui::TableSetupColumn("Next PC");
			ImGui::TableSetupColumn("A");
			ImGui::TableSetupColumn("STATUS");
			ImGui::TableHeadersRow();
			const auto trace = chipset.epscpu->TraceBuffer();
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(trace.size()));
			while (clipper.Step()) {
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
					const auto& entry = trace[static_cast<size_t>(i)];
					ImGui::TableNextRow();
					ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(entry.instruction_count));
					ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(entry.cycle_count));
					ImGui::TableNextColumn(); UIHelpers::ClickableAddress(entry.program_counter, UIHelpers::JumpTarget::Code);
					ImGui::TableNextColumn(); ImGui::Text("%08X", entry.instruction);
					ImGui::TableNextColumn(); UIHelpers::ClickableAddress(entry.next_program_counter, UIHelpers::JumpTarget::Code);
					ImGui::TableNextColumn(); ImGui::Text("%02X", entry.accumulator);
					ImGui::TableNextColumn(); ImGui::Text("%02X", entry.status);
				}
			}
		}
		ImGui::EndChild();
	}
	ToolUi::Section("Stack Memory");
	static int range = 64;
	ToolUi::Field("View range");
	ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x * 0.45f));
	ImGui::SliderInt("##range", &range, 64, 2048);
	uint16_t offset = chipset.epscpu ? 0 : chipset.cpu.reg_sp & 0xffff;
	// Keep the hex row inside the panel: address + bytes + ASCII pane.
	{
		const float char_width = std::max(1.0f, ImGui::CalcTextSize("0").x);
		const int max_columns = std::clamp(
			static_cast<int>((ImGui::GetContentRegionAvail().x - char_width * 22.0f) / (char_width * 4.0f)),
			2, 64);
		if (mem_editor.Cols > max_columns)
			mem_editor.Cols = max_columns;
	}
	mem_editor.ReadFn = [](const ImU8* data, size_t off) -> ImU8 {
		return me_mmu->ReadData((size_t)data + off);
	};
	mem_editor.WriteFn = [](ImU8* data, size_t off, ImU8 d) {
		return me_mmu->WriteData((size_t)data + off, d);
	};
	auto rng = range;
	if (rng + offset >= casioemu::GetRamBaseAddr(m_emu->hardware_id) + casioemu::GetRamSize(m_emu->hardware_id)) {
		rng = casioemu::GetRamSize(m_emu->hardware_id) - offset + casioemu::GetRamBaseAddr(m_emu->hardware_id);
	}
	{
		const float body_height = std::max(200.0f, ImGui::GetContentRegionAvail().y);
		ToolUi::PanelScope frame("##stack_hex_frame", body_height);
		mem_editor.DrawContents((void*)static_cast<size_t>(offset), rng, offset);
	}
}
