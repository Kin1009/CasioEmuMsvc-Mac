#include "Injector.hpp"
#include "Chipset/Chipset.hpp"
#include "Config.hpp"
#include "Models.h"
#include "Peripheral/BatteryBackedRAM.hpp"
#include "Plugin/PluginPayloads.h"
#include "Theme.h"
#include "ToolUi.hpp"
#include "Ui.hpp"
#ifdef CASIOEMU_CORE_WEB
#include "WebDebuggerGui.h"
#endif
#include "hex.hpp"
#include "imgui/imgui.h"
#include <Gui.h>
#include <Localization.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>

Injector::Injector() : UIWindow("Payload"), needsReload(false), isReloading(false), isShuttingDown(false) {
	injectors.push_back(InjectorData());
	injectionFilePath = ThemeManager::Instance().Settings().injectionFilePath;
	InitCustomInjectionsFile();
#ifndef CASIOEMU_CORE_WEB
	AsyncLoadCustomInjections();
#endif
}

Injector::~Injector() {
	isShuttingDown.store(true);

	if (reloadThread.joinable()) {
		reloadThread.join();
	}
}

std::string Injector::GetFileModifiedTime(const std::string& filepath) {
	try {
		auto ftime = std::filesystem::last_write_time(filepath);
		auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
		return std::to_string(std::chrono::system_clock::to_time_t(sctp));
	}
	catch (...) {
		return "";
	}
}

void Injector::AsyncLoadCustomInjections() {
	try {
		if (isShuttingDown.load()) {
			return;
		}

		if (reloadThread.joinable()) {
			reloadThread.join();
		}

		reloadThread = std::thread([this]() {
			try {
				if (!isShuttingDown.load()) {
					BackgroundReload();
				}
			}
			catch (...) {
				// Catch exceptions
			}
		});
	}
	catch (...) {
		// Thread creation error handling
	}
}

void Injector::TrimString(std::string& str) {
	// Trim from start
	str.erase(str.begin(), std::find_if(str.begin(), str.end(), [](unsigned char ch) {
		return !std::isspace(ch);
	}));

	// Trim from end
	str.erase(std::find_if(str.rbegin(), str.rend(), [](unsigned char ch) {
		return !std::isspace(ch);
	}).base(),
		str.end());
}

bool Injector::IsHexString(const std::string& str) {
	if (str.empty())
		return false;
	std::string processed = str;
	if (processed.substr(0, 2) == "0x" || processed.substr(0, 2) == "0X") {
		processed = processed.substr(2);
	}
	return !processed.empty() &&
		   processed.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}

void Injector::InitCustomInjectionsFile() {
	const std::filesystem::path filepath = injectionFilePath;
	if (filepath.has_parent_path()) {
		std::filesystem::create_directories(filepath.parent_path());
	}

	if (std::filesystem::exists(filepath)) {
		return;
	}

	const std::string template_content = R"(# Custom Injection Template
# Format: name = {
#     address = "hex_data",
#     address = "hex_data"
# }
)";

	try {
		std::ofstream file(filepath);
		if (!file) {
			throw std::runtime_error("Failed to create injection template file");
		}
		file << template_content;
		file.close();
#ifdef CASIOEMU_CORE_WEB
		WebDebuggerRequestFsSync();
#endif
	}
	catch (const std::exception& e) {
		// Handle error
	}
}

void Injector::BackgroundReload() {
	try {
		if (isShuttingDown.load()) {
			return;
		}

		if (isReloading.exchange(true)) {
			return; // If it is reloading then exit
		}

		std::string currentModTime;
		try {
			currentModTime = GetFileModifiedTime(injectionFilePath);
		}
		catch (...) {
			isReloading.store(false);
			return;
		}

		if (currentModTime == lastModifiedTime && !needsReload) {
			isReloading.store(false);
			return;
		}

		if (isShuttingDown.load()) {
			isReloading.store(false);
			return;
		}

		std::string content;
		try {
			std::ifstream file(injectionFilePath, std::ios::binary);
			if (!file.is_open()) {
				isReloading.store(false);
				return;
			}

			content = std::string(
				(std::istreambuf_iterator<char>(file)),
				std::istreambuf_iterator<char>());
			file.close();
		}
		catch (...) {
			isReloading.store(false);
			return;
		}

		if (isShuttingDown.load()) {
			isReloading.store(false);
			return;
		}

		try {
			std::lock_guard<std::mutex> lock(injectionMutex);
			if (ParseCustomInjections(content)) {
				lastModifiedTime = currentModTime;
				needsReload = false;
			}
		}
		catch (...) {
			// Handling analysis errors
		}
	}
	catch (...) {
		// Catch any exception
	}
	isReloading.store(false);
}

void Injector::PrecomputeInjectionValues(InjectionPair& pair) {
	// Process address
	std::string addr = pair.address;
	if (addr.substr(0, 2) == "0x" || addr.substr(0, 2) == "0X") {
		addr = addr.substr(2);
	}
	pair.addr_value = std::stoul(addr, nullptr, 16);

	// Process data - remove all whitespace
	std::string cleaned_data = pair.data;
	cleaned_data.erase(
		std::remove_if(cleaned_data.begin(), cleaned_data.end(),
			[](char c) { return std::isspace(c); }),
		cleaned_data.end());

	pair.data_bytes.clear();
	pair.data_bytes.reserve(cleaned_data.length() / 2);

	for (size_t i = 0; i + 1 < cleaned_data.length(); i += 2) {
		std::string byte_str = cleaned_data.substr(i, 2);
		if (IsHexString(byte_str)) {
			pair.data_bytes.push_back(HexToByte(byte_str));
		}
	}
}

bool Injector::ParseCustomInjections(const std::string& content) {
	std::vector<CustomInjection> newInjections;

	// State machine for robust parsing
	enum class ParseState {
		NAME,		 // Looking for name
		EQUALS,		 // Looking for equals sign after name
		OPEN_BRACE,	 // Looking for opening brace
		KEY,		 // Looking for address
		KEY_EQUALS,	 // Looking for equals sign after address
		VALUE_START, // Looking for quote
		VALUE,		 // Reading value data
		AFTER_VALUE, // Looking for comma or close brace
	};

	// First pass: preprocess to handle comments and normalize content
	std::string preprocessed;
	bool in_comment = false;

	for (size_t i = 0; i < content.length(); ++i) {
		char c = content[i];

		if (c == '#') {
			in_comment = true;
		}
		else if (c == '\n') {
			in_comment = false;
			preprocessed += ' '; // Convert newlines to spaces
		}
		else if (!in_comment) {
			preprocessed += c;
		}
	}

	// Second pass: parse with state machine
	ParseState state = ParseState::NAME;
	std::string current_name;
	std::string current_address;
	std::string current_value;
	CustomInjection current_injection;
	bool in_quotes = false;
	int brace_level = 0;

	for (size_t i = 0; i < preprocessed.length(); ++i) {
		char c = preprocessed[i];

		switch (state) {
		case ParseState::NAME:
			if (std::isalnum(c) || c == '_') {
				current_name += c;
			}
			else if (c == '=' && !current_name.empty()) {
				TrimString(current_name);
				state = ParseState::EQUALS;
			}
			else if (!std::isspace(c)) {
				// Invalid character, reset
				current_name.clear();
			}
			break;

		case ParseState::EQUALS:
			if (c == '{') {
				current_injection = CustomInjection();
				current_injection.name = current_name;
				current_name.clear();
				brace_level = 1;
				state = ParseState::KEY;
			}
			else if (!std::isspace(c) && c != '=') {
				// Unexpected character
				state = ParseState::NAME;
				current_name.clear();
			}
			break;

		case ParseState::OPEN_BRACE:
			if (c == '{') {
				current_injection = CustomInjection();
				current_injection.name = current_name;
				current_name.clear();
				brace_level = 1;
				state = ParseState::KEY;
			}
			else if (!std::isspace(c)) {
				// Unexpected character
				state = ParseState::NAME;
				current_name.clear();
			}
			break;

		case ParseState::KEY:
			if (std::isxdigit(c) || c == 'x' || c == 'X' || c == '0') {
				current_address += c;
			}
			else if (c == '=' && !current_address.empty()) {
				TrimString(current_address);
				state = ParseState::VALUE_START;
			}
			else if (c == '}' && brace_level == 1) {
				// End of block
				if (!current_injection.pairs.empty()) {
					newInjections.push_back(std::move(current_injection));
				}
				brace_level = 0;
				state = ParseState::NAME;
			}
			else if (!std::isspace(c)) {
				// Reset on unexpected char
				current_address.clear();
			}
			break;

		case ParseState::VALUE_START:
			if (c == '"') {
				in_quotes = true;
				current_value.clear();
				state = ParseState::VALUE;
			}
			else if (!std::isspace(c)) {
				// Reset on unexpected char
				state = ParseState::KEY;
				current_address.clear();
			}
			break;

		case ParseState::VALUE:
			if (c == '"' && in_quotes) {
				in_quotes = false;
				// Process the pair
				if (IsHexString(current_address)) {
					InjectionPair pair;
					pair.address = current_address;
					pair.data = current_value;

					try {
						PrecomputeInjectionValues(pair);
						if (!pair.data_bytes.empty()) {
							current_injection.pairs.push_back(std::move(pair));
						}
					}
					catch (...) {
						// Invalid hex, skip
					}
				}

				current_address.clear();
				current_value.clear();
				state = ParseState::AFTER_VALUE;
			}
			else if (in_quotes) {
				// Accept any character inside quotes
				current_value += c;
			}
			break;

		case ParseState::AFTER_VALUE:
			if (c == ',') {
				state = ParseState::KEY;
			}
			else if (c == '}' && brace_level == 1) {
				if (!current_injection.pairs.empty()) {
					newInjections.push_back(std::move(current_injection));
				}
				brace_level = 0;
				state = ParseState::NAME;
			}
			else if (c == '{') {
				brace_level++;
			}
			else if (c == '}') {
				brace_level--;
				if (brace_level == 0) {
					if (!current_injection.pairs.empty()) {
						newInjections.push_back(std::move(current_injection));
					}
					state = ParseState::NAME;
				}
			}
			break;

		case ParseState::KEY_EQUALS:
			if (c == '=') {
				state = ParseState::VALUE_START;
			}
			else if (!std::isspace(c)) {
				state = ParseState::KEY;
				current_address.clear();
			}
			break;
		}
	}

	// Handle case where file ends in a valid state
	if (state == ParseState::AFTER_VALUE && brace_level == 1) {
		if (!current_injection.pairs.empty()) {
			newInjections.push_back(std::move(current_injection));
		}
	}

	customInjections = std::move(newInjections);
	return true;
}

bool Injector::ValidateHexPair(const std::string& hex) {
	if (hex.length() % 2 != 0)
		return false;
	return IsHexString(hex);
}

uint8_t Injector::HexToByte(const std::string& hex) {
	return static_cast<uint8_t>(std::stoul(hex, nullptr, 16));
}

bool Injector::ApplyInjection(const CustomInjection& inj, bool& show_info, std::string& info_msg) {
	try {
		for (const auto& pair : inj.pairs) {
			for (size_t i = 0; i < pair.data_bytes.size(); ++i) {
				me_mmu->WriteData(pair.addr_value + i, pair.data_bytes[i]);
			}
		}

		char buf[256];
		snprintf(buf, sizeof(buf), "Rop.CustomInjectApplied"_lc, inj.name.c_str());
		SetFeedback(buf, false);
		show_info = false;
		return true;
	}
	catch (const std::exception& e) {
		char buf[256];
		snprintf(buf, sizeof(buf), "Rop.CustomInjectError"_lc, inj.name.c_str());
		SetFeedback(buf, true);
		show_info = false;
		return false;
	}
}

void Injector::RenderCustomInjectTab(bool& show_info, std::string& info_msg) {
	ToolUi::Description("Injections are read from the injection file configured in Settings > Calculator.");
	static bool autoCheckFileChanges = true;
	{
		ToolUi::PanelScope panel("##custom_inject_toolbar");
		ImGui::TextWrapped("%s: %s", "Rop.CurrentInjectFile"_l.c_str(), injectionFilePath.c_str());

		if (ImGui::Checkbox("Rop.AutoReload"_lc, &autoCheckFileChanges)) {
			if (autoCheckFileChanges) {
				needsReload = true;
			}
		}

		ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Rop.ReloadCustomInjects"_lc).x + 20.0f);
		if (ImGui::Button("Rop.ReloadCustomInjects"_lc)) {
		if (!isReloading.load() && !isShuttingDown.load()) {
			std::string currentFilePath = ThemeManager::Instance().Settings().injectionFilePath;
			if (currentFilePath != injectionFilePath) {
				injectionFilePath = currentFilePath;
			}
			needsReload = true;

			AsyncLoadCustomInjections();

			SetFeedback("Rop.CustomInjectReloading"_lc, false);
			show_info = false;
		}
		if (isReloading.load()) {
			ImGui::SameLine();
			ImGui::TextUnformatted("Rop.Loading"_lc);
		}
		}
	}
	ImGui::Spacing();

	for (const auto& inj : customInjections) {
		if (ImGui::CollapsingHeader(inj.name.c_str())) {
			ImGui::TextUnformatted("Rop.Address"_lc);
			for (const auto& pair : inj.pairs) {
				std::string displayAddr = pair.address;
				if (displayAddr.substr(0, 2) != "0x" && displayAddr.substr(0, 2) != "0X") {
					displayAddr = "0x" + displayAddr;
				}
				ImGui::BulletText("%s", displayAddr.c_str());
			}

			ImGui::Spacing();
			if (ToolUi::AccentButton((inj.name + "##inject").c_str())) {
				ApplyInjection(inj, show_info, info_msg);
			}

			ImGui::Spacing();
		}
	}

	if (autoCheckFileChanges && !isReloading.load() && !isShuttingDown.load()) {
		std::string currentModTime = GetFileModifiedTime(injectionFilePath);
		if (currentModTime != lastModifiedTime) {
			needsReload = true;
			AsyncLoadCustomInjections();
		}
	}

	// Payloads contributed by installed `inject` plugins.  The plugin loader
	// validates address/bytes before registering, so only well-formed entries
	// reach this list.
	const auto plugin_payloads = casioemu::plugin::PluginPayloads();
	if (!plugin_payloads.empty()) {
		ToolUi::Section("Plugin payloads");
		ToolUi::Description("Registered by installed inject plugins.");
		for (const auto& payload : plugin_payloads) {
			ImGui::PushID(payload.id.c_str());
			if (ImGui::CollapsingHeader(payload.name.c_str())) {
				if (!payload.description.empty())
					ImGui::TextWrapped("%s", payload.description.c_str());
				ImGui::Text("Plugin: %s", payload.pluginId.c_str());
				ImGui::Text("Address: 0x%05X  |  %zu bytes  |  target: %s%s",
					payload.address, payload.bytes.size(), payload.target.c_str(),
					payload.safe ? "  |  safe" : "");
				if (ImGui::Button("Apply")) {
					std::string error;
					if (casioemu::plugin::ApplyPayloadBytes(payload,
							[](void*, uint32_t address, uint8_t byte) {
								if (!me_mmu)
									return false;
								me_mmu->WriteData(address, byte);
								return true;
							},
							nullptr, error)) {
						char buf[256];
						snprintf(buf, sizeof(buf), "Injected %zu bytes at 0x%05X", payload.bytes.size(),
							(unsigned int)payload.address);
						SetFeedback(buf, false);
						show_info = false;
					}
					else {
						SetFeedback(error, true);
						show_info = false;
					}
				}
			}
			ImGui::PopID();
		}
	}
}

void Injector::RenderInjectorTab(InjectorData& inj, int index, bool& show_info, std::string& info_msg) {
	auto valid_hex = [](char c) {
		return (c >= '0' && c <= '9') ||
			   (c >= 'a' && c <= 'f') ||
			   (c >= 'A' && c <= 'F');
	};

	ToolUi::Field("Address");
	ImGui::SetNextItemWidth(ImGui::CalcTextSize("00000000").x + 24.0f);
	ImGui::InputText(("##addr" + std::to_string(index)).c_str(), inj.addr, 10,
		ImGuiInputTextFlags_CharsHexadecimal);
	ImGui::SameLine();
	if (ImGui::Button(("Rop.Paste"_l + "##" + std::to_string(index)).c_str())) {
		if (ImGui::GetClipboardText() != nullptr) {
			strncpy(inj.data, ImGui::GetClipboardText(), sizeof(inj.data) - 1);
			inj.data[sizeof(inj.data) - 1] = '\0';
		}
	}
	ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Rop.Clear"_lc).x + 20.0f);
	if (ImGui::Button(("Rop.Clear"_l + "##" + std::to_string(index)).c_str())) {
		memset(inj.data, 0, sizeof(inj.data));
	}

	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
	ImGui::InputTextMultiline(
		("##hex" + std::to_string(index)).c_str(),
		inj.data,
		IM_ARRAYSIZE(inj.data) - 1,
		ImVec2(-1, ImGui::GetTextLineHeight() * 8));

	ImGui::Spacing();
	if (ToolUi::AccentButton(("Rop.Inject"_l + "##" + std::to_string(index)).c_str())) {
		auto plc = strtol(inj.addr, 0, 16);
		size_t i = 0, j = 0;
		char hex_buf[3];

		while (inj.data[i] != '\0' && inj.data[i + 1] != '\0') {
			if (inj.data[i] == ';' || inj.data[i + 1] == ';') {
				for (;; ++i) {
					if (inj.data[i] == '\0')
						goto exit;
					if (inj.data[i] == '\n') {
						++i;
						break;
					}
				}
			}
			else {
				if (!(valid_hex(inj.data[i]) && valid_hex(inj.data[i + 1]))) {
					++i;
					continue;
				}
				hex_buf[0] = inj.data[i];
				hex_buf[1] = inj.data[i + 1];
				hex_buf[2] = '\0';
				uint8_t byte = strtoul(hex_buf, nullptr, 16);
				me_mmu->WriteData(plc + j, byte);
				i += 2;
				++j;
			}
		}
	exit:
		char buf[128];
		snprintf(buf, sizeof(buf), "Injected %zu bytes at 0x%05X", j, (unsigned int)plc);
		SetFeedback(buf, false);
		show_info = false;
	}
}

void Injector::RenderCore() {
	static int range = 64;
	static char buf[10] = {0};
	static MemoryEditor editor;
	static bool show_info = false;
	static std::string info_msg;
	ToolUi::PageScope page("Payload");
	if (!initialLoadRequested) {
		initialLoadRequested = true;
		AsyncLoadCustomInjections();
	}
	if (!m_emu || !me_mmu || !n_ram_buffer) {
		ToolUi::Section("Payload");
		ToolUi::EmptyState("The injector is unavailable until the emulator RAM is ready.");
		return;
	}
	auto inputbase = m_emu->hardware_id == casioemu::HardwareId::HW_CLASSWIZ_II ? 0x9268 : 0xD180;
		char* base_addr = n_ram_buffer - casioemu::GetRamBaseAddr(m_emu->hardware_id);
	// Check if injection file path has changed in settings
	std::string currentFilePath = ThemeManager::Instance().Settings().injectionFilePath;
	if (currentFilePath != injectionFilePath) {
		injectionFilePath = currentFilePath;
		InitCustomInjectionsFile();
		needsReload = true;
		if (!isReloading.load()) {
			AsyncLoadCustomInjections();
		}
	}

	// A tab bar splits the width evenly and truncates labels in a narrow dock;
	// a wrapping segmented row keeps every label complete.
	ToolUi::Section("Payload");
	static int rop_tab = [] {
		// Headless verification: open a specific payload tab.
		if (const char* requested = std::getenv("CASIOEMU_PAYLOAD_TAB"))
			return std::clamp(std::atoi(requested), 0, 3);
		return 0;
	}();
	{
		static const char* tab_labels[] = {
			"Rop.XAnMode"_lc, "Rop.InjectHex"_lc, "Rop.Input"_lc, "Rop.CustomInject"_lc
		};
		const ImGuiStyle& style = ImGui::GetStyle();
		const float row_start = ImGui::GetCursorPosX();
		const float row_width = ImGui::GetWindowContentRegionMax().x - row_start;
		float used = 0.0f;
		for (int i = 0; i < IM_ARRAYSIZE(tab_labels); ++i) {
			const bool selected = rop_tab == i;
			const float button_width = ImGui::CalcTextSize(tab_labels[i]).x + style.FramePadding.x * 2.0f;
			if (i > 0) {
				if (used + button_width > row_width) {
					ImGui::NewLine();
					used = 0.0f;
				}
				else {
					ImGui::SameLine();
				}
			}
			ImGui::PushStyleColor(ImGuiCol_Button, selected ? ToolUi::Palette().selection : ImVec4(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ToolUi::Palette().hover);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ToolUi::Palette().selection);
			ImGui::PushStyleColor(ImGuiCol_Text, selected ? ToolUi::Palette().foreground : ToolUi::Palette().muted);
			if (ImGui::Button(tab_labels[i]))
				rop_tab = i;
			ImGui::PopStyleColor(4);
			used += button_width + style.ItemSpacing.x;
		}
	}
	ImGui::Spacing();

	{
		if (rop_tab == 0) {
			ToolUi::Section("An input");
			ToolUi::Description("Fill the answer input area with dummy digits of the requested length.");
			ImGui::Spacing();
			{
				ToolUi::PanelScope panel("##rop_an_panel");
				ToolUi::Field("Length");
				ImGui::SetNextItemWidth(ImGui::CalcTextSize("0000").x + 24.0f);
				ImGui::InputText("##off", buf, 9, ImGuiInputTextFlags_CharsDecimal);
				ImGui::SameLine();
				if (ToolUi::AccentButton("Rop.InputAn"_lc)) {
					int off = atoi(buf);
					if (off > 100) {
						memset(base_addr + inputbase, 0x31, 100);
						memset(base_addr + inputbase + 100, 0xa6, 1);
						memset(base_addr + inputbase + 101, 0x31, off - 100);
					}
					else {
						memset(base_addr + inputbase, 0x31, off);
					}
					*(base_addr + inputbase + off) = 0xfd;
					*(base_addr + inputbase + off + 1) = 0x20;
					SetFeedback("Rop.AnInputed"_lc, false);
					show_info = false;
				}
			}
		}
		else if (rop_tab == 1) {
			ToolUi::Section("Hex injection");
			ToolUi::Description("Write hex bytes to any address. Semicolons start a comment until the end of the line.");
			ImGui::Spacing();
			if (ToolUi::AccentButton("Rop.AddInjector"_lc)) {
				injectors.push_back(InjectorData());
			}
			ImGui::Spacing();
			for (size_t i = 0; i < injectors.size(); i++) {
				ImGui::PushID(static_cast<int>(i));
				ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
				ImGui::BeginChild(("##injector_card" + std::to_string(i)).c_str(), ImVec2(0, 0),
					ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY);
				std::string header = "Rop.InjectorNum"_l + " " + std::to_string(i + 1);
				if (injectors.size() > 1) {
					if (ImGui::Button(("Rop.RemoveInjector"_l + "##" + std::to_string(i)).c_str())) {
						injectors.erase(injectors.begin() + i);
						ImGui::EndChild();
						ImGui::PopStyleVar();
						ImGui::PopID();
						break;
					}
					ImGui::SameLine();
				}
				ImGui::TextUnformatted(header.c_str());
				RenderInjectorTab(injectors[i], static_cast<int>(i), show_info, info_msg);
				ImGui::EndChild();
				ImGui::PopStyleVar();
				ImGui::PopID();
				ImGui::Spacing();
			}
		}
		else if (rop_tab == 2) {
			ToolUi::Section("Input buffer");
			ToolUi::Description("Edit the raw bytes that Load to Input Area copies into the input region.");
			ImGui::Spacing();
			ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
			ImGui::BeginChild("RopInput", ImVec2(0, std::max(160.0f, ImGui::GetContentRegionAvail().y - 80.0f)),
				ImGuiChildFlags_Border);
			editor.DrawContents(data_buf, range);
			ImGui::EndChild();
			ImGui::PopStyleVar();

			ToolUi::Field("Size");
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			ImGui::SliderInt("##range", &range, 64, 1024);

			if (ToolUi::AccentButton("Rop.LoadToInputArea"_lc)) {
				memcpy(base_addr + inputbase, data_buf, range);
				SetFeedback("Rop.LoadedTip"_lc, false);
				show_info = false;
			}

		}
		else if (rop_tab == 3) {
			ToolUi::Section("Custom injections");
			RenderCustomInjectTab(show_info, info_msg);
		}
	}

	if (ImGui::GetTime() - feedback_time < 3.0) {
		ImGui::Spacing();
		{
			ToolUi::PanelScope panel("##rop_feedback");
			ToolUi::Status(feedback_msg.c_str(), is_feedback_error ? ToolUi::Palette().error : ToolUi::Palette().success);
		}
	}

	if (show_info) {
		ImGui::OpenPopup("Rop.InfoPopup"_lc);
	}

	{
		ToolUi::DialogScope dialog("Rop.InfoPopup"_lc, "Injection", 460.0f);
		if (dialog) {
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(info_msg.c_str());
			ImGui::PopTextWrapPos();
			ImGui::Spacing();
			if (ToolUi::AccentButton("Button.Positive"_lc, ImVec2(120, 0)) || dialog.CloseRequested()) {
				show_info = false;
				ImGui::CloseCurrentPopup();
			}
		}
	}
}
