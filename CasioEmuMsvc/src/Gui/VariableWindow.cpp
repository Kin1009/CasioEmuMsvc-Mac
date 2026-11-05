#include "VariableWindow.h"
#include "CwiiHelp.h"
#include "Models.h"
#include "Settings/Settings.h"
#include "ToolUi.hpp"
#include "Ui.hpp"
#include "imgui/imgui.h"
#include <algorithm>
#include <string>

std::string to_hex(unsigned int num) {
	std::string hex_digits = "0123456789ABCDEF";
	std::string result;

	while (num > 0) {
		result += hex_digits[num % 16];
		num /= 16;
	}

	// Reverse the result to get the correct order
	std::reverse(result.begin(), result.end());

	// If the result is empty, return "0"
	if (result.empty()) {
		result = "0";
	}

	return result;
}

namespace {

// The variable map is static per hardware id; rebuilding the vector every frame
// was pure allocation.  The values still come from RAM each frame.
const std::vector<casioemu::Variable>& CachedVariables(casioemu::HardwareId hardware) {
	static casioemu::HardwareId cached_hardware = static_cast<casioemu::HardwareId>(-1);
	static std::vector<casioemu::Variable> cached;
	if (cached.empty() || cached_hardware != hardware) {
		cached = casioemu::GetVariableOffsets(hardware);
		cached_hardware = hardware;
	}
	return cached;
}

} // namespace

void VariableWindow::RenderCore() {
	ToolUi::PageScope page("Variables");
	if (!n_ram_buffer) {
		ToolUi::Section("Variables");
		ToolUi::EmptyState("No known variable map for this hardware.");
		return;
	}
	const auto& vars = CachedVariables(m_emu->hardware_id);
	if (vars.empty()) {
		ToolUi::Section("Variables");
		ToolUi::EmptyState("No known variable map for this hardware.");
		return;
	}

	char* base_addr = n_ram_buffer - casioemu::GetRamBaseAddr(m_emu->hardware_id);
	static bool showaddr = false;
	static bool showhex = false;
	static bool showimg_auto = true;
	static bool showimg_f = false;
	static bool options_loaded = false;
	if (!options_loaded) {
		auto& ui = casioemu::settings::SettingsStore::Instance().Data().ui;
		showaddr = ui.displayAddress;
		showhex = ui.displayHex;
		showimg_auto = ui.showImPComplex;
		showimg_f = ui.alwaysShowImP;
		options_loaded = true;
	}

	bool is_in_im = (*(base_addr + casioemu::GetModeOffset(m_emu->hardware_id)) & 0xFF) == 0xC4;
	bool s_im = showimg_f ? 1 : (showimg_auto ? is_in_im : 0);

	ToolUi::Section("Variables");
	ToolUi::Description("Decoded BCD values for the active model.");

	int cols = 2;
	if (s_im) cols++;
	if (showhex) cols += (s_im ? 2 : 1);
	if (showaddr) cols += (s_im ? 2 : 1);

	const bool has_theta = m_emu->hardware_id == casioemu::HW_CLASSWIZ_II;
	const int theta_index = has_theta ? static_cast<int>(vars.size()) : -1;
	const int total_rows = static_cast<int>(vars.size()) + (has_theta ? 1 : 0);
	const float table_height = std::max(140.0f, ImGui::GetContentRegionAvail().y - 200.0f);

	if (ToolUi::TableScope table{"##vars_table", cols, ToolUi::TableFlags() | ImGuiTableFlags_ScrollY, table_height}) {
		ImGui::TableSetupColumn("VarWindow.Variable"_lc, ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("VarWindow.ReP"_lc, ImGuiTableColumnFlags_WidthStretch, 2.0f);
		if (s_im) {
			ImGui::TableSetupColumn("VarWindow.ImP"_lc, ImGuiTableColumnFlags_WidthStretch, 2.0f);
		}
		if (showhex) {
			ImGui::TableSetupColumn("Real Hex", ImGuiTableColumnFlags_WidthStretch, 1.5f);
			if (s_im) {
				ImGui::TableSetupColumn("Imag Hex", ImGuiTableColumnFlags_WidthStretch, 1.5f);
			}
		}
		if (showaddr) {
			ImGui::TableSetupColumn("Real Addr", ImGuiTableColumnFlags_WidthStretch, 1.2f);
			if (s_im) {
				ImGui::TableSetupColumn("Imag Addr", ImGuiTableColumnFlags_WidthStretch, 1.2f);
			}
		}
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();

		auto draw_row = [&](int row_index) {
			const bool theta = row_index == theta_index;
			const casioemu::Variable* variable = theta ? nullptr : &vars[static_cast<size_t>(row_index)];
			if (!theta && is_in_im && !strcmp(variable->Name, "PreAns"))
				return;
			ImGui::TableNextRow();

			ImGui::TableNextColumn();
			ImGui::TextUnformatted(theta ? "Theta" : variable->Name);

			const size_t real_offset = theta ? 0xBDEC : variable->RealPartOffset;
			const size_t imag_offset = real_offset + casioemu::GetReImOffset(m_emu->hardware_id);

			ImGui::TableNextColumn();
			if (theta) {
				auto value = casioemu::BCD2Str(n_ram_buffer + 0xBDEC - 0x9000);
				ImGui::TextUnformatted(value.c_str());
			}
			else {
				auto value = casioemu::BCD2Str(base_addr + real_offset);
				ImGui::TextUnformatted(value.c_str());
			}

			if (s_im) {
				ImGui::TableNextColumn();
				if (theta)
					ImGui::TextUnformatted("");
				else {
					auto value = casioemu::BCD2Str(base_addr + imag_offset);
					ImGui::TextUnformatted(value.c_str());
				}
			}

			if (showhex) {
				ImGui::TableNextColumn();
				if (theta)
					ImGui::TextUnformatted("");
				else {
					auto value = casioemu::ConvHex(base_addr + real_offset, casioemu::GetVariableSize(m_emu->hardware_id));
					ImGui::TextUnformatted(value.c_str());
				}
				if (s_im) {
					ImGui::TableNextColumn();
					if (theta)
						ImGui::TextUnformatted("");
					else {
						auto value = casioemu::ConvHex(base_addr + imag_offset, casioemu::GetVariableSize(m_emu->hardware_id));
						ImGui::TextUnformatted(value.c_str());
					}
				}
			}

			if (showaddr) {
				ImGui::TableNextColumn();
				UIHelpers::ClickableAddress(static_cast<uint32_t>(real_offset));
				if (s_im) {
					ImGui::TableNextColumn();
					if (theta)
						ImGui::TextUnformatted("");
					else
						UIHelpers::ClickableAddress(static_cast<uint32_t>(imag_offset));
				}
			}
		};

		ImGuiListClipper clipper;
		clipper.Begin(total_rows);
		while (clipper.Step()) {
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
				draw_row(row);
		}
	}

	ToolUi::Section("Display Options");
	{
		ToolUi::PanelScope panel("##var_options");
		bool changed = false;
		changed |= ToolUi::CheckboxFlow("VarWindow.ShowAddrOpt"_lc, &showaddr, true);
		changed |= ToolUi::CheckboxFlow("VarWindow.ShowHexOpt"_lc, &showhex, false);
		// The ImP options are longer; give them their own row so labels never clip.
		ImGui::NewLine();
		changed |= ToolUi::CheckboxFlow("VarWindow.ShowImPWhenComplex"_lc, &showimg_auto, true);
		changed |= ToolUi::CheckboxFlow("VarWindow.AlwaysShowImP"_lc, &showimg_f, false);
		if (changed) {
			auto& ui = casioemu::settings::SettingsStore::Instance().Data().ui;
			ui.displayAddress = showaddr;
			ui.displayHex = showhex;
			ui.showImPComplex = showimg_auto;
			ui.alwaysShowImP = showimg_f;
			std::string error;
			casioemu::settings::SettingsStore::Instance().Save(&error);
		}
	}
}
