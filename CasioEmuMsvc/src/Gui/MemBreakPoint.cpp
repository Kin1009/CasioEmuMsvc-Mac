#include "MemBreakPoint.hpp"
#include "Chipset/CPU.hpp"
#include "Chipset/Chipset.hpp"
#include "Chipset/ePSCpu.h"
#include "Chipset/T4xCore.hpp"
#include "Emulator.hpp"
#include "Gui/Hooks.h"
#include "ToolUi.hpp"
#include "Ui.hpp"
#include "imgui/imgui.h"
#include <Localization.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <stdlib.h>

namespace {
	std::map<std::string, uint32_t> CaptureRegisters(const casioemu::CPU& cpu) {
		std::map<std::string, uint32_t> registers;
		registers["pc"] = (static_cast<uint32_t>(cpu.reg_csr.raw) << 16) | cpu.reg_pc.raw;
		registers["lr"] = (static_cast<uint32_t>(cpu.reg_lcsr.raw) << 16) | cpu.reg_lr.raw;
		registers["sp"] = cpu.reg_sp.raw;
		registers["dsr"] = cpu.reg_dsr.raw;
		registers["psw"] = cpu.reg_psw.raw;
		for (int i = 0; i < 16; ++i) {
			registers["r" + std::to_string(i)] = cpu.reg_r[i].raw;
		}
		for (int i = 0; i < 16; i += 2) {
			registers["er" + std::to_string(i)] =
				static_cast<uint16_t>(cpu.reg_r[i].raw | (cpu.reg_r[i + 1].raw << 8));
		}
		for (int i = 0; i < 16; i += 4) {
			registers["xr" + std::to_string(i)] =
				static_cast<uint32_t>(cpu.reg_r[i].raw)
				| (static_cast<uint32_t>(cpu.reg_r[i + 1].raw) << 8)
				| (static_cast<uint32_t>(cpu.reg_r[i + 2].raw) << 16)
				| (static_cast<uint32_t>(cpu.reg_r[i + 3].raw) << 24);
		}
		return registers;
	}
}

Breakpoints* membp_cv = 0;

void Breakpoints::DrawContent() {
	if (break_point_hash.empty()) {
		ToolUi::PanelScope panel("##bp_empty");
		ToolUi::EmptyState("No memory breakpoints yet.");
		return;
	}
	static int selected = -1;
	int remove_index = -1;
	if (ToolUi::TableScope table{"##bp_list", 5, ToolUi::TableFlags() | ImGuiTableFlags_ScrollY,
			ImGui::GetContentRegionAvail().y}) {
		ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 100.0f);
		ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 76.0f);
		ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed, 78.0f);
		ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 32.0f);
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();

		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(break_point_hash.size()));
		while (clipper.Step()) {
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
			MemBPData_t& data = break_point_hash[static_cast<size_t>(i)];
			ImGui::PushID(i);
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			char buf[16];
			snprintf(buf, sizeof(buf), "%05X", (unsigned)data.addr);
			if (ImGui::Selectable(buf, selected == i, ImGuiSelectableFlags_SpanAllColumns,
					ImVec2(0, ImGui::GetFrameHeight())))
				selected = i;
			const bool row_hovered = ImGui::IsItemHovered();
			ToolUi::ContextPopupScope popup;
			if (popup) {
				selected = i;
				ImGui::TextUnformatted("MemBP.BPType"_lc);
				if (ImGui::Button("HexEditors.ContextMenu.MonitorRead"_lc)) {
					target_addr = i;
					data.enableWrite = 0;
					data.records.clear();
					SyncCoreBreakpoints();
					ImGui::CloseCurrentPopup();
				}
				if (ImGui::Button("HexEditors.ContextMenu.MonitorWrite"_lc)) {
					data.enableWrite = true;
					target_addr = i;
					data.records.clear();
					SyncCoreBreakpoints();
					ImGui::CloseCurrentPopup();
				}
				if (m_emu && (m_emu->chipset.epscpu || m_emu->chipset.t4x)) {
					bool configuration_changed = false;
					configuration_changed |= ImGui::Checkbox("Enabled##ctx", &data.enabled);
					configuration_changed |= ImGui::Checkbox("Compare data", &data.compareData);
					if (data.compareData) {
						int compare_data = data.data;
						int compare_mask = data.mask;
						if (ImGui::InputInt("Data", &compare_data, 1, 16, ImGuiInputTextFlags_CharsHexadecimal)) {
							data.data = static_cast<uint8_t>(compare_data);
							configuration_changed = true;
						}
						if (ImGui::InputInt("Mask", &compare_mask, 1, 16, ImGuiInputTextFlags_CharsHexadecimal)) {
							data.mask = static_cast<uint8_t>(compare_mask);
							configuration_changed = true;
						}
					}
					int skip_count = static_cast<int>(std::min<uint64_t>(data.skipCount, 0x7fffffffu));
					if (ImGui::InputInt("Skip count", &skip_count, 1, 10)) {
						data.skipCount = static_cast<uint64_t>(std::max(skip_count, 0));
						configuration_changed = true;
					}
					if (configuration_changed)
						SyncCoreBreakpoints();
				}
				ImGui::Separator();
				if (ImGui::Button("MemBP.Delete"_lc))
					remove_index = i;
			}
			(void)row_hovered;

			ImGui::TableSetColumnIndex(1);
			ImGui::TextDisabled("%s", data.enableWrite ? "Write" : "Read");

			ImGui::TableSetColumnIndex(2);
			bool enabled = data.enabled;
			if (ImGui::Checkbox("##enabled", &enabled)) {
				data.enabled = enabled;
				SyncCoreBreakpoints();
			}

			ImGui::TableSetColumnIndex(3);
			ImGui::Text("%d", static_cast<int>(data.records.size()));

			ImGui::TableSetColumnIndex(4);
			if (ToolUi::CloseButton("##remove_bp", "Delete breakpoint"))
				remove_index = i;
			ImGui::PopID();
		}
		}
	}
	if (remove_index >= 0 && remove_index < static_cast<int>(break_point_hash.size())) {
		break_point_hash[static_cast<size_t>(remove_index)].records.clear();
		if (target_addr == remove_index)
			target_addr = -1;
		break_point_hash.erase(break_point_hash.begin() + remove_index);
		if (selected == remove_index)
			selected = -1;
		SyncCoreBreakpoints();
	}
}

void Breakpoints::DrawFindContent() {
	if (target_addr == -1) {
		ToolUi::Hint("MemBP.NoBPHint"_lc);
		return;
	}
	int write = break_point_hash[target_addr].enableWrite;
	static ImGuiTableFlags flags = ToolUi::TableFlags() | ImGuiTableFlags_ScrollY;
	char header[128];
	snprintf(header, sizeof(header), "MemBP.MonitoringHint"_lc, break_point_hash[target_addr].addr);
	if (ImGui::BeginTable("##outputtable", 2, flags)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn(header, ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Access", ImGuiTableColumnFlags_WidthFixed, 90.0f);
		ImGui::TableHeadersRow();
		int i = 0;
		if (m_emu && (m_emu->chipset.epscpu || m_emu->chipset.t4x)) {
			auto hits = m_emu->chipset.t4x ? m_emu->chipset.t4x->MemoryBreakpointHits(break_point_hash[target_addr].addr, write != 0) :
				m_emu->chipset.epscpu->MemoryBreakpointHits(break_point_hash[target_addr].addr, write != 0);
			for (const auto& hit : hits) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				UIHelpers::ClickableAddress(hit.program_counter, UIHelpers::JumpTarget::Code);
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%s %02X", hit.write ? "W" : "R", hit.value);
			}
		}
		for (auto kv : break_point_hash[target_addr].records) {
			uint32_t pc_addr = kv.first;
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			UIHelpers::ClickableAddress(pc_addr, UIHelpers::JumpTarget::Code);
			ImGui::TableSetColumnIndex(1);
			ImGui::PushID(i++);
			if (ImGui::SmallButton("MemBP.ViewCallstack"_lc)) {
				const std::string& fx = kv.second.stacktrace;
				SDL_ShowSimpleMessageBox(0, "Call stack", fx.c_str(), 0);
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
}


void Breakpoints::SetupHooks() {
	SetupHook(on_memory_read, [&](casioemu::MMU& sender, MemoryEventArgs& mea) {
		TryTrigBp(mea.offset, false);
	});
	SetupHook(on_memory_write, [&](casioemu::MMU& sender, MemoryEventArgs& mea) {
		TryTrigBp(mea.offset, true);
	});
	SetupHook(on_instruction, [&](casioemu::CPU& sender, InstructionEventArgs& iea) {
		if (RegisterBreakpointTriggered(sender.reg_sp.raw))
			SetDebugbreak();
	});
	SetupHook(on_eps_instruction, [&](InstructionEventArgs& iea) {
		if (RegisterBreakpointTriggered(iea.stack_pointer))
			iea.should_break = true;
	});
	membp_cv = this;
}

bool Breakpoints::RegisterBreakpointTriggered(uint32_t value) const {
	const uint64_t config = register_breakpoint_config.load(std::memory_order_acquire);
	if ((config & (uint64_t{1} << 63)) == 0)
		return false;
	const auto target = static_cast<uint32_t>(config);
	switch (static_cast<uint8_t>(config >> 32)) {
	case 1:
		return value == target;
	case 2:
		return value != target;
	case 3:
		return value > target;
	case 4:
		return value < target;
	case 5:
		return value >= target;
	case 6:
		return value <= target;
	default:
		return false;
	}
}

void Breakpoints::UpdateRegisterBreakpointConfig() {
	const uint64_t config = (break_on_sp ? (uint64_t{1} << 63) : 0) |
		(static_cast<uint64_t>(static_cast<uint8_t>(reg_compare_mode)) << 32) |
		static_cast<uint32_t>(target_sp);
	register_breakpoint_config.store(config, std::memory_order_release);
}

void Breakpoints::TryTrigBp(uint32_t addr, bool write) {
	std::lock_guard lock(breakpoints_mutex);
	for (auto& bp : break_point_hash) {
		if (bp.addr != addr || bp.enableWrite != write)
			continue;
		if (bp.breakWhenHit) {
			SetDebugbreak();
		}
		else {
			bp.records[(m_emu->chipset.cpu.reg_csr << 16) | m_emu->chipset.cpu.reg_pc] =
				Record{m_emu->chipset.cpu.GetBacktrace(),
					(unsigned int)(m_emu->chipset.cpu.reg_lcsr << 16) | m_emu->chipset.cpu.reg_lr,
					CaptureRegisters(m_emu->chipset.cpu)};
		}
	}
}

void Breakpoints::RenderCore() {
	std::lock_guard lock(breakpoints_mutex);
	ToolUi::PageScope page("Breakpoints");
	if (m_emu && (m_emu->chipset.epscpu || m_emu->chipset.t4x)) {
		const uint64_t version = m_emu->chipset.t4x ? m_emu->chipset.t4x->MemoryBreakpointsVersion() : m_emu->chipset.epscpu->MemoryBreakpointsVersion();
		if (version != last_core_breakpoint_version) {
			RefreshCoreBreakpoints();
			last_core_breakpoint_version = version;
		}
	}
	if (ImGui::BeginTabBar("Breakpoints")) {
		if (ImGui::BeginTabItem("Memory")) {
			ToolUi::Section("Memory Breakpoints");
			ToolUi::Description("Break or record on memory reads and writes.");
			ImGui::Spacing();
			{
				ToolUi::PanelScope panel("##bp_add_panel");
				static char buf[10] = {0};
				ToolUi::Field("Address (hex)");
				ImGui::SetNextItemWidth(ImGui::CalcTextSize("00000000").x + 24.0f);
				ImGui::InputText("##addressin", buf, 10, ImGuiInputTextFlags_CharsHexadecimal);
				ImGui::SameLine();
				if (ToolUi::AccentButton("MemBP.AddAddr"_lc)) {
					break_point_hash.push_back({.addr = (uint32_t)strtol(buf, nullptr, 16)});
					SyncCoreBreakpoints();
				}
				ToolUi::SameLineOrWrap(ImGui::GetContentRegionAvail().x + 1.0f);
				ImGui::Checkbox("MemBP.BreakWhenHit"_lc, &break_on_cv);
				if (target_addr >= 0 && static_cast<size_t>(target_addr) < break_point_hash.size() &&
					break_point_hash[target_addr].breakWhenHit != break_on_cv) {
					break_point_hash[target_addr].breakWhenHit = break_on_cv;
					SyncCoreBreakpoints();
				}
			}
			ImGui::Spacing();
			const float list_height = break_on_cv ? std::max(120.0f, ImGui::GetContentRegionAvail().y - 90.0f)
												  : std::max(120.0f, ImGui::GetContentRegionAvail().y * 0.40f);
			ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
			ImGui::BeginChild("##srcollingmbp", ImVec2(0, list_height), ImGuiChildFlags_Border);
			DrawContent();
			ImGui::EndChild();
			ImGui::PopStyleVar();
			if (!break_on_cv) {
				ToolUi::Section("Recorded Hits");
				if (target_addr >= 0 && static_cast<size_t>(target_addr) < break_point_hash.size()) {
					if (ImGui::Button("MemBP.ClearRec"_lc))
						break_point_hash[target_addr].records.clear();
					ImGui::SameLine();
				}
				ImGui::BeginChild("##findoutput", ImVec2(0, std::max(100.0f, ImGui::GetContentRegionAvail().y - 8.0f)));
				DrawFindContent();
				ImGui::EndChild();
			}
			ImGui::EndTabItem();
		}
		if (!m_emu->chipset.t4x && ImGui::BeginTabItem("Register")) {
			ToolUi::Section("Register Breakpoint");
			ToolUi::Description("Break when the stack pointer matches.");
			ImGui::Spacing();
			ToolUi::PanelScope panel("##bp_register_panel");
			const bool eps6800 = m_emu && m_emu->chipset.epscpu;
			static char buf[10] = {0};
			ToolUi::Field("Comparison");
			ImGui::SetNextItemWidth(std::min(260.0f, std::max(140.0f, ImGui::GetContentRegionAvail().x)));
			ImGui::Combo("##regcmp", &reg_compare_mode, "Disabled\0Equal\0Not Equal\0Greater\0Less\0Greater or Equal\0Less or Equal\0");
			if (eps6800)
				ToolUi::Hint("EPS6800 STKPTR (00-1F)");
			ToolUi::Field(eps6800 ? "STKPTR (hex)" : "SP value (hex)");
			ImGui::SetNextItemWidth(ImGui::CalcTextSize("00000000").x);
			if (ImGui::InputText("##addressin2", buf, 10, ImGuiInputTextFlags_CharsHexadecimal)) {
				target_sp = (uint16_t)strtol(buf, nullptr, 16);
				if (eps6800)
					target_sp &= 0x1f;
			}
			ImGui::SameLine();
			ImGui::Checkbox(eps6800 ? "Break on STKPTR" : "BP.SPHint"_lc, &break_on_sp);
			UpdateRegisterBreakpointConfig();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
}

void Breakpoints::RefreshCoreBreakpoints() {
	if (!m_emu || (!m_emu->chipset.epscpu && !m_emu->chipset.t4x))
		return;
	/* Track the selected breakpoint by (address, write) instead of by index so
	 * a core reorder or length change cannot silently retarget the checkbox. */
	bool have_selected = target_addr >= 0 && static_cast<size_t>(target_addr) < break_point_hash.size();
	const uint32_t selected_addr = have_selected ? break_point_hash[target_addr].addr : 0;
	const bool selected_write = have_selected ? break_point_hash[target_addr].enableWrite : false;
	const auto core_breakpoints = m_emu->chipset.t4x ? m_emu->chipset.t4x->MemoryBreakpoints() : m_emu->chipset.epscpu->MemoryBreakpoints();
	std::vector<MemBPData_t> refreshed;
	refreshed.reserve(core_breakpoints.size());
	for (const auto& item : core_breakpoints) {
		refreshed.push_back({
			.enableWrite = item.write,
			.breakWhenHit = item.break_when_hit,
			.enabled = item.enabled,
			.compareData = item.compare_data,
			.data = item.data,
			.mask = item.mask,
			.skipCount = item.skip_count,
			.addr = item.address});
	}
	break_point_hash = std::move(refreshed);
	if (have_selected) {
		const auto it = std::find_if(break_point_hash.begin(), break_point_hash.end(), [&](const MemBPData_t& bp) {
			return bp.addr == selected_addr && bp.enableWrite == selected_write;
		});
		if (it != break_point_hash.end()) {
			target_addr = static_cast<int>(std::distance(break_point_hash.begin(), it));
			break_on_cv = it->breakWhenHit;
			return;
		}
	}
	target_addr = -1;
	break_on_cv = false;
}

void Breakpoints::ExternalAddBp(uint32_t addr, bool write) {
	ExternalAddBp(addr, write, false);
}

void Breakpoints::ExternalAddBp(uint32_t addr, bool write, bool breakWhenHit,
	bool enabled, bool compareData, uint8_t data, uint8_t mask, uint64_t skipCount) {
	std::lock_guard lock(breakpoints_mutex);
	break_point_hash.push_back({.enableWrite = write, .breakWhenHit = breakWhenHit,
		.enabled = enabled, .compareData = compareData, .data = data, .mask = mask,
		.skipCount = skipCount, .addr = addr});
	target_addr = break_point_hash.size() - 1;
	break_on_cv = breakWhenHit;
	SyncCoreBreakpoints();
}

bool Breakpoints::ExternalRemoveBp(uint32_t addr, bool write) {
	std::lock_guard lock(breakpoints_mutex);
	auto it = std::find_if(break_point_hash.begin(), break_point_hash.end(), [&](const MemBPData_t& bp) {
		return bp.addr == addr && bp.enableWrite == write;
	});
	if (it == break_point_hash.end())
		return false;
	const auto removed = static_cast<int>(std::distance(break_point_hash.begin(), it));
	break_point_hash.erase(it);
	if (target_addr == removed)
		target_addr = -1;
	else if (target_addr > removed)
		--target_addr;
	SyncCoreBreakpoints();
	return true;
}

void Breakpoints::ExternalClearBps() {
	std::lock_guard lock(breakpoints_mutex);
	break_point_hash.clear();
	target_addr = -1;
	break_on_cv = false;
	SyncCoreBreakpoints();
}

void Breakpoints::SyncCoreBreakpoints() {
	if (!m_emu || (!m_emu->chipset.epscpu && !m_emu->chipset.t4x))
		return;
	auto sync = [&](auto* core) {
	const auto core_breakpoints = core->MemoryBreakpoints();
	for (const auto& item : break_point_hash) {
		casioemu::Eps6800MemoryBreakpoint breakpoint{};
		breakpoint.address = item.addr;
		breakpoint.write = item.enableWrite;
		breakpoint.enabled = item.enabled;
		breakpoint.break_when_hit = item.breakWhenHit;
		breakpoint.compare_data = item.compareData;
		breakpoint.data = item.data;
		breakpoint.mask = item.mask;
		breakpoint.skip_count = item.skipCount;
		core->AddMemoryBreakpoint(breakpoint);
	}
	/* Reconcile removals without clearing the whole core list, so retained
	 * breakpoints keep their hit_count and skip-pending state. */
	for (const auto& core_bp : core_breakpoints) {
		const auto it = std::find_if(break_point_hash.begin(), break_point_hash.end(), [&](const MemBPData_t& bp) {
			return bp.addr == core_bp.address && bp.enableWrite == core_bp.write;
		});
		if (it == break_point_hash.end())
			core->RemoveMemoryBreakpoint(core_bp.address, core_bp.write);
	}
	};
	if (m_emu->chipset.t4x) sync(m_emu->chipset.t4x); else sync(m_emu->chipset.epscpu);
}

std::vector<MemBPData_t> Breakpoints::ExternalListBps() const {
	std::lock_guard lock(breakpoints_mutex);
	return break_point_hash;
}

std::vector<std::pair<uint32_t, Record>> Breakpoints::ExternalListHits(uint32_t addr, bool write) const {
	std::lock_guard lock(breakpoints_mutex);
	std::vector<std::pair<uint32_t, Record>> result;
	auto it = std::find_if(break_point_hash.begin(), break_point_hash.end(), [&](const MemBPData_t& bp) {
		return bp.addr == addr && bp.enableWrite == write;
	});
	if (it == break_point_hash.end())
		return result;
	result.reserve(it->records.size());
	for (const auto& record : it->records)
		result.push_back(record);
	return result;
}

void SetMemBp(uint32_t addr, bool write) {
	if (membp_cv) {
		membp_cv->ExternalAddBp(addr, write);
	}
}
