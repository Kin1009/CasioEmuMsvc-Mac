#include "CallAnalysis.h"
#include "Chipset/CPU.hpp"
#include "Hooks.h"
#include "ToolUi.hpp"
#include "Ui.hpp"
#include "imgui/imgui.h"
#include <Localization.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>

struct CallAnalysis : public UIWindow {
	std::mutex data_mutex;
	// Hot path flag: checked with a relaxed load before anything else, so a
	// disabled recorder costs one atomic read per call (no backtrace, no lock).
	std::atomic<bool> recording{false};
	bool check_caller = false;
	std::string message;
	char caller[260]{};
	uint32_t caller_v{};
	bool check_callee = false;
	char callee[260]{};
	uint32_t callee_v{};
	class FunctionCall {
	public:
		uint32_t pc{};
		uint32_t lr{};
		uint32_t xr0{};
		// eps6800 / T4x provide a formatted backtrace upstream; the classic CPU
		// path stores frame PCs and formats text only when the row is opened.
		std::string stack{};
		std::vector<uint32_t> frames{};
	};
	std::map<uint32_t, std::vector<FunctionCall>> funcs;
	std::vector<FunctionCall> viewing_calls;
	CallAnalysis() : UIWindow("Functions") {
		SetupHook(on_call_function, [this](casioemu::CPU& sender, const FunctionEventArgs& ea) {
			// Cheap no-op when the recorder is idle: no backtrace, no lock.
			if (!recording.load(std::memory_order_relaxed))
				return;
			const uint32_t xr0 = (static_cast<uint32_t>(sender.reg_r[3]) << 24) |
				(static_cast<uint32_t>(sender.reg_r[2]) << 16) |
				(static_cast<uint32_t>(sender.reg_r[1]) << 8) |
				static_cast<uint32_t>(sender.reg_r[0]);
			OnCallFunctionCpu(sender, ea.pc, ea.lr, xr0);
		});
		SetupHook(on_eps_call_function, [this](const EpsFunctionEventArgs& ea) {
			if (!recording.load(std::memory_order_relaxed))
				return;
			OnCallFunction(ea.function.pc, ea.function.lr, ea.accumulator, ea.backtrace);
		});
		SetupHook(on_t4x_call_function, [this](const StandaloneFunctionEventArgs& ea) {
			if (!recording.load(std::memory_order_relaxed))
				return;
			OnCallFunction(ea.function.pc, ea.function.lr, ea.accumulator, ea.backtrace);
		});
		// Headless profiling knob: start recording immediately.
		if (const char* auto_record = std::getenv("CASIOEMU_CALL_RECORDING"))
			recording.store(std::atoi(auto_record) != 0);
	}

	bool PassesFilters(uint32_t pc, uint32_t lr) const {
		if (check_caller && lr != caller_v)
			return false;
		if (check_callee && pc != callee_v)
			return false;
		return true;
	}

	void OnCallFunction(uint32_t pc, uint32_t lr, uint32_t xr0, const std::string& backtrace) {
		std::lock_guard lock(data_mutex);
		if (!recording.load(std::memory_order_relaxed) || !PassesFilters(pc, lr))
			return;
		FunctionCall fc{};
		fc.xr0 = xr0;
		fc.pc = pc;
		fc.lr = lr;
		fc.stack = backtrace;
		funcs[pc].push_back(std::move(fc));
	}

	void OnCallFunctionCpu(casioemu::CPU& cpu, uint32_t pc, uint32_t lr, uint32_t xr0) {
		std::lock_guard lock(data_mutex);
		if (!recording.load(std::memory_order_relaxed) || !PassesFilters(pc, lr))
			return;
		FunctionCall fc{};
		fc.xr0 = xr0;
		fc.pc = pc;
		fc.lr = lr;
		// Compact capture: frame PCs only.  Formatting happens when the user
		// opens the row, so a recording session does no stringstream work.
		auto stack = cpu.stack.get_const();
		fc.frames.reserve(stack->size());
		for (const auto& frame : *stack)
			fc.frames.push_back(frame.new_pc);
		funcs[pc].push_back(std::move(fc));
	}

	static std::string FormatFrames(const std::vector<uint32_t>& frames) {
		std::string text;
		text.reserve(frames.size() * 24);
		char line[64];
		for (uint32_t pc : frames) {
			std::snprintf(line, sizeof(line), "  function %06X\n", pc);
			text += line;
		}
		return text;
	}

	void DebugStart(bool filterCaller, uint32_t callerAddress, bool filterCallee, uint32_t calleeAddress) {
		std::lock_guard lock(data_mutex);
		check_caller = filterCaller;
		caller_v = callerAddress;
		check_callee = filterCallee;
		callee_v = calleeAddress;
		funcs.clear();
		recording.store(true);
	}

	void DebugStop() {
		recording.store(false);
	}

	void DebugClear() {
		std::lock_guard lock(data_mutex);
		funcs.clear();
		viewing_calls.clear();
		message.clear();
	}

	std::vector<DebugFunctionCallInfo> DebugList(uint32_t function) {
		std::lock_guard lock(data_mutex);
		std::vector<DebugFunctionCallInfo> result;
		for (const auto& [address, calls] : funcs) {
			if (function && function != address)
				continue;
			for (const auto& call : calls) {
				std::string stack = call.stack;
				if (stack.empty() && !call.frames.empty())
					stack = FormatFrames(call.frames);
				result.push_back({call.pc, call.lr, call.xr0, std::move(stack)});
			}
		}
		return result;
	}

	void RenderCore() override {
		std::lock_guard lock(data_mutex);
		ToolUi::PageScope page("Functions");
		if (message.size()) {
			ToolUi::Section("CallAnalysis.Stacktrace"_lc);
			if (ImGui::Button("CallAnalysis.Close"_lc)) {
				message.clear();
				return;
			}
			ImGui::SameLine();
			if (ImGui::Button("Copy to Clipboard")) {
				ImGui::SetClipboardText(message.c_str());
			}
			ImGui::Spacing();
			ToolUi::PanelScope panel("##call_stack_text", 220.0f);
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(message.c_str());
			ImGui::PopTextWrapPos();
			return;
		}
		if (recording.load(std::memory_order_relaxed)) {
			ToolUi::Section("Recording");
			ToolUi::Description("Every function call is being captured. Stop the recording to inspect the results.");
			ImGui::Spacing();
			if (ToolUi::AccentButton("CallAnalysis.Stop"_lc))
				recording.store(false);
			ImGui::SameLine();
			if (ImGui::Button("CallAnalysis.Clear"_lc))
				funcs.clear();
			if (funcs.empty()) {
				ImGui::Spacing();
				ToolUi::EmptyState("Recording... Waiting for function calls to be triggered.");
			}
			else {
				ToolUi::Section("Recorded Functions");
				if (ToolUi::TableScope table{"##records", 2}) {
					ImGui::TableSetupColumn("CallAnalysis.Function"_lc, ImGuiTableColumnFlags_WidthStretch, 80);
					ImGui::TableSetupColumn("CallAnalysis.CallCount"_lc, ImGuiTableColumnFlags_WidthFixed, 90);
					ImGui::TableHeadersRow();
					for (auto& func : funcs) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(lookup_symbol(func.first, g_labels).c_str());
						ImGui::TableNextColumn();
						ImGui::Text("%d", (int)func.second.size());
					}
				}
			}
		}
		else {
			if (!viewing_calls.empty()) {
				ToolUi::Section("Calls");
				if (ImGui::Button("CallAnalysis.Close"_lc)) {
					viewing_calls.clear();
					return;
				}
				ImGui::SameLine();
				ImGui::TextDisabled("%d call(s)", static_cast<int>(viewing_calls.size()));
				ImGui::Spacing();
				const float table_height = std::max(160.0f, ImGui::GetContentRegionAvail().y - 8.0f);
				ImGui::BeginChild("##view_calls", ImVec2(0, table_height));
				if (ToolUi::TableScope table{"##records2", 10}) {
					ImGui::TableSetupScrollFreeze(0, 1);
					ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40);
					ImGui::TableSetupColumn("CallAnalysis.Function"_lc,
						ImGuiTableColumnFlags_WidthFixed, 90);
					ImGui::TableSetupColumn("CallAnalysis.Caller"_lc,
						ImGuiTableColumnFlags_WidthFixed, 90);
					ImGui::TableSetupColumn("R0", ImGuiTableColumnFlags_WidthFixed, 44);
					ImGui::TableSetupColumn("R1", ImGuiTableColumnFlags_WidthFixed, 44);
					ImGui::TableSetupColumn("R2", ImGuiTableColumnFlags_WidthFixed, 44);
					ImGui::TableSetupColumn("R3", ImGuiTableColumnFlags_WidthFixed, 44);
					ImGui::TableSetupColumn(m_emu->chipset.t4x ? "P0" : "ER0", ImGuiTableColumnFlags_WidthFixed, 56);
					ImGui::TableSetupColumn(m_emu->chipset.t4x ? "P2" : "ER2", ImGuiTableColumnFlags_WidthFixed, 56);
					ImGui::TableSetupColumn("Stack", ImGuiTableColumnFlags_WidthStretch, 1);
					ImGui::TableHeadersRow();
					int i = 0;
					for (auto& func : viewing_calls) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::Text("%d", i);
						ImGui::TableNextColumn();
						UIHelpers::ClickableAddress(func.pc);
						ImGui::TableNextColumn();
						UIHelpers::ClickableAddress(func.lr);
						ImGui::TableNextColumn();
						ImGui::Text("%02x", func.xr0 & 0xff);
						ImGui::TableNextColumn();
						ImGui::Text("%02x", (func.xr0 >> 8) & 0xff);
						ImGui::TableNextColumn();
						ImGui::Text("%02x", (func.xr0 >> 16) & 0xff);
						ImGui::TableNextColumn();
						ImGui::Text("%02x", (func.xr0 >> 24) & 0xff);
						ImGui::TableNextColumn();
						ImGui::Text("%04x", m_emu->chipset.t4x ? (func.xr0 & 15) | ((func.xr0 >> 8) & 15) << 4 : func.xr0 & 0xffff);
						ImGui::TableNextColumn();
						ImGui::Text("%04x", m_emu->chipset.t4x ? ((func.xr0 >> 16) & 15) | ((func.xr0 >> 24) & 15) << 4 : (func.xr0 >> 16) & 0xffff);
						ImGui::TableNextColumn();
						ImGui::PushID(i++);
						if (ImGui::SmallButton("CallAnalysis.Stacktrace"_lc)) {
							message = func.stack;
							if (message.empty() && !func.frames.empty())
								message = FormatFrames(func.frames);
						}
						ImGui::PopID();
					}
				}
				ImGui::EndChild();
				return;
			}
			ToolUi::Section("Call Analysis");
			ToolUi::Description("Record every function call, optionally filtered by caller or callee address, then inspect per-function counts.");
			ImGui::Spacing();
			if (ToolUi::AccentButton("CallAnalysis.StartRec"_lc)) {
				recording.store(true);
				funcs.clear();
			}
			ImGui::SameLine();
			if (ImGui::Button("CallAnalysis.Clear"_lc))
				funcs.clear();

			ToolUi::Section("CallAnalysis.Filters"_lc);
			{
				ToolUi::PanelScope panel("##call_filters");
				ToolUi::Field("Callee (hex)");
				ImGui::SetNextItemWidth(ImGui::CalcTextSize("00000000").x + 24.0f);
				if (ImGui::InputText("##callee", callee, 260, ImGuiInputTextFlags_CharsHexadecimal))
					callee_v = strtol(callee, 0, 16);
				ImGui::SameLine();
				ImGui::Checkbox("CallAnalysis.CalleeFilter"_lc, &check_callee);
				ToolUi::Field("Caller (hex)");
				ImGui::SetNextItemWidth(ImGui::CalcTextSize("00000000").x + 24.0f);
				if (ImGui::InputText("##caller", caller, 260, ImGuiInputTextFlags_CharsHexadecimal))
					caller_v = strtol(caller, 0, 16);
				ImGui::SameLine();
				ImGui::Checkbox("CallAnalysis.CallerFilter"_lc, &check_caller);
			}

			ToolUi::Section("Recorded Functions");
			if (funcs.empty()) {
				ToolUi::EmptyState("Click Start Recording above to begin capturing function calls.");
			}
			else if (ToolUi::TableScope table{"##records", 2}) {
				ImGui::TableSetupColumn("CallAnalysis.Function"_lc, ImGuiTableColumnFlags_WidthStretch, 80);
				ImGui::TableSetupColumn("CallAnalysis.CallCount"_lc, ImGuiTableColumnFlags_WidthFixed, 90);
				ImGui::TableHeadersRow();
				for (auto& func : funcs) {
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					if (ImGui::Selectable(lookup_symbol(func.first, g_labels).c_str(), false, ImGuiSelectableFlags_SpanAllColumns))
						viewing_calls = func.second;
					ImGui::TableNextColumn();
					ImGui::Text("%d", (int)func.second.size());
				}
			}
		}
	}
};

static CallAnalysis* g_callAnalysis = nullptr;

UIWindow* CreateCallAnalysisWindow() {
	g_callAnalysis = new CallAnalysis();
	return g_callAnalysis;
}

void DebugStartCallRecording(bool filterCaller, uint32_t caller, bool filterCallee, uint32_t callee) {
	if (g_callAnalysis)
		g_callAnalysis->DebugStart(filterCaller, caller, filterCallee, callee);
}

void DebugStopCallRecording() {
	if (g_callAnalysis)
		g_callAnalysis->DebugStop();
}

void DebugClearCallRecording() {
	if (g_callAnalysis)
		g_callAnalysis->DebugClear();
}

std::vector<DebugFunctionCallInfo> DebugGetFunctionCalls(uint32_t function) {
	return g_callAnalysis ? g_callAnalysis->DebugList(function) : std::vector<DebugFunctionCallInfo>{};
}
