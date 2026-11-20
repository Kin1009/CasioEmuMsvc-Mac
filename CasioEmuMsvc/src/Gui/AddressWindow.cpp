#include "AddressWindow.h"
#include <Hooks.h>
#include <Localization.h>
#include <mutex>
#include "Cg100CoreInfo.hpp"
#include "ToolUi.hpp"
struct AddressInfo {
	uint32_t address;
	uint8_t value;
	bool locked;
	AddressInfo(uint32_t addr, uint8_t val, bool lock) : address(addr), value(val), locked(lock) {}
};

class AddressWindow : public UIWindow {
public:
	AddressWindow() : UIWindow("Memory") {
		SetupHooks();
	}

	void RenderCore() override {
		std::lock_guard lock(addresses_mutex);
		ToolUi::PageScope page("Memory");

		// CG100's built-in MMU is idle: the nX address watch below would read
		// empty memory. Replace it with a live view into the SH4 plugin's flash.
		if (casioemu::cg100info::IsActive()) {
			ToolUi::Section("CG100 / SH4 flash viewer");
			ToolUi::Description("Browse the emulated 32 MiB flash. The nX MMU address watch is not available for this core.");
			{
				ToolUi::PanelScope panel("##mem_cg100_hex");
				static uint32_t address = 0x00C83A00; // FAT12 BPB by default
				ToolUi::Field("Flash address (hex)");
				ImGui::SetNextItemWidth(ImGui::CalcTextSize("00FFFFFF").x + 24.0f);
				ImGui::InputScalar("##cg100_flash_addr", ImGuiDataType_U32, &address, 0, 0, "%06X",
					ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);
				ImGui::SameLine();
				ImGui::TextDisabled("0..0x1FFFFFF");
				struct Jump { const char* label; uint32_t addr; };
				const Jump jumps[] = {
					{"Boot", 0x0}, {"Marker", 0x00B20000}, {"Fugue", 0x00C80000},
					{"BPB", 0x00C83A00}, {"FAT1", 0x00C85000}, {"Root", 0x00C86000},
					{"Data", 0x00C8A000},
				};
				bool first = true;
				for (const auto& jump : jumps) {
					if (!first) ImGui::SameLine();
					first = false;
					if (ImGui::Button(jump.label)) address = jump.addr;
				}
				ImGui::Spacing();
				uint8_t bytes[128] = {};
				if (casioemu::cg100info::ReadRaw(address, bytes, static_cast<int>(sizeof(bytes)))) {
					for (int row = 0; row < 8; ++row) {
						char line[160];
						int pos = std::snprintf(line, sizeof(line), "%06X  ", address + static_cast<uint32_t>(row * 16));
						for (int col = 0; col < 16; ++col) {
							const uint8_t byte = bytes[static_cast<size_t>(row * 16 + col)];
							pos += std::snprintf(line + pos, sizeof(line) - static_cast<size_t>(pos),
								"%02X ", byte);
						}
						pos += std::snprintf(line + pos, sizeof(line) - static_cast<size_t>(pos), " |");
						for (int col = 0; col < 16; ++col) {
							const uint8_t byte = bytes[static_cast<size_t>(row * 16 + col)];
							pos += std::snprintf(line + pos, sizeof(line) - static_cast<size_t>(pos),
								"%c", byte >= 0x20 && byte < 0x7F ? static_cast<char>(byte) : '.');
						}
						std::snprintf(line + pos, sizeof(line) - static_cast<size_t>(pos), "|");
						ImGui::TextUnformatted(line);
					}
				}
				else {
					ImGui::TextWrapped("Raw flash reads require the emulated USB MSC to be connected "
						"(cg100_usb_plug). It is not connected in this session, so no live bytes are "
						"shown. The FAT12 volume summary and flash map below are reference data from "
						"MEMORY_FILESYSTEMS.md.");
				}
			}
			ToolUi::Section("FAT12 volume (fls0)");
			{
				ToolUi::PanelScope panel("##mem_cg100_fs");
				casioemu::cg100info::RenderFat12Summary();
			}
			ToolUi::Section("Flash regions");
			{
				ToolUi::PanelScope panel("##mem_cg100_map");
				casioemu::cg100info::RenderFlashMap();
			}
			return;
		}

		ToolUi::Section("AddressWindow.Header"_lc);
		ToolUi::Description("Watch an address; locked values are enforced.");
		ImGui::Spacing();

		RenderAddressTable();

		ToolUi::Section("Add Address");
		{
			ToolUi::PanelScope panel("##add_address_panel");
			ToolUi::Field("Address (hex)");
			ImGui::SetNextItemWidth(ImGui::CalcTextSize("00000000").x + 24.0f);
			ImGui::InputScalar("##new_address", ImGuiDataType_U32, &newAddress, 0, 0, "%X",
				ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);
			ImGui::SameLine();
			ImGui::BeginDisabled(newAddress == 0);
			if (ToolUi::AccentButton("AddressWindow.AddBtn"_lc))
				addresses.emplace_back(newAddress, 0, false);
			ImGui::EndDisabled();
			if (addresses.empty())
				ToolUi::Hint("Nothing watched yet.");
		}
	}

	std::vector<DebugAddressLockInfo> DebugList() const {
		std::lock_guard lock(addresses_mutex);
		std::vector<DebugAddressLockInfo> result;
		result.reserve(addresses.size());
		for (const auto& info : addresses)
			result.push_back({info.address, info.value, info.locked});
		return result;
	}

	void DebugSet(uint32_t address, uint8_t value, bool locked) {
		std::lock_guard lock(addresses_mutex);
		auto it = std::find_if(addresses.begin(), addresses.end(), [&](const AddressInfo& info) {
			return info.address == address;
		});
		if (it == addresses.end())
			addresses.emplace_back(address, value, locked);
		else {
			it->value = value;
			it->locked = locked;
		}
		if (!locked)
			UpdateMemoryValue(address, value);
	}

	bool DebugRemove(uint32_t address) {
		std::lock_guard lock(addresses_mutex);
		auto it = std::find_if(addresses.begin(), addresses.end(), [&](const AddressInfo& info) {
			return info.address == address;
		});
		if (it == addresses.end())
			return false;
		addresses.erase(it);
		return true;
	}

	void DebugClear() {
		std::lock_guard lock(addresses_mutex);
		addresses.clear();
	}

private:
	mutable std::recursive_mutex addresses_mutex;
	std::vector<AddressInfo> addresses;
	uint32_t newAddress = 0;

	void RenderAddressTable() {
		if (addresses.empty()) {
			ToolUi::PanelScope panel("##address_empty");
			ToolUi::EmptyState("No watched addresses.");
			return;
		}
		int remove_index = -1;
		if (ToolUi::TableScope table{"Addresses", 4}) {
			ImGui::TableSetupColumn("AddressWindow.Address"_lc, ImGuiTableColumnFlags_WidthFixed, 110.0f);
			ImGui::TableSetupColumn("AddressWindow.Value"_lc, ImGuiTableColumnFlags_WidthFixed, 90.0f);
			ImGui::TableSetupColumn("AddressWindow.Fixed"_lc, ImGuiTableColumnFlags_WidthFixed, 80.0f);
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();

			for (size_t i = 0; i < addresses.size(); ++i) {
				ImGui::PushID(i + 1145);
				auto& info = addresses[i];

				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				UIHelpers::ClickableAddress(info.address);

				ImGui::TableSetColumnIndex(1);
				uint8_t value = info.value;
				if (!info.locked)
					value = m_emu->chipset.mmu.ReadData(info.address);
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::InputScalar("##value", ImGuiDataType_U8, &value, 0, 0, "%x")) {
					info.value = value;
					UpdateMemoryValue(info.address, info.value);
				}

				ImGui::TableSetColumnIndex(2);
				bool locked = info.locked;
				if (ImGui::Checkbox("##lock", &locked)) {
					info.locked = locked;
				}

				ImGui::TableSetColumnIndex(3);
				if (ToolUi::CloseButton("##remove_address", "Stop watching this address"))
					remove_index = static_cast<int>(i);
				ImGui::PopID();
			}
		}
		if (remove_index >= 0 && remove_index < static_cast<int>(addresses.size()))
			addresses.erase(addresses.begin() + remove_index);
	}

	void RenderAddAddressControls() {}

	void UpdateMemoryValue(uint32_t address, uint8_t value) {
		m_emu->chipset.mmu.WriteData(address, value);
	}

	void SetupHooks() {
		SetupHook(on_memory_write, [this](casioemu::MMU& mmu, MemoryEventArgs& args) {
			std::lock_guard lock(addresses_mutex);
			for (const auto& info : addresses) {
				if (info.locked && info.address == args.offset) {
					args.handled = true;
				}
			}
		});
		SetupHook(on_memory_read, [this](casioemu::MMU& mmu, MemoryEventArgs& args) {
			std::lock_guard lock(addresses_mutex);
			for (const auto& info : addresses) {
				if (info.locked && info.address == args.offset) {
					args.value = info.value;
					args.handled = true;
				}
			}
		});
		SetupHook(on_eps_memory_write, [this](MemoryEventArgs& args) {
			std::lock_guard lock(addresses_mutex);
			for (const auto& info : addresses)
				if (info.locked && info.address == args.offset)
					args.handled = true;
		});
		SetupHook(on_eps_memory_read, [this](MemoryEventArgs& args) {
			std::lock_guard lock(addresses_mutex);
			for (const auto& info : addresses) {
				if (info.locked && info.address == args.offset) {
					args.value = info.value;
					args.handled = true;
				}
			}
		});
	}
};

static AddressWindow* g_addressWindow = nullptr;

UIWindow* CreateAddressWindow() {
	g_addressWindow = new AddressWindow();
	return g_addressWindow;
}

std::vector<DebugAddressLockInfo> DebugGetAddressLocks() {
	return g_addressWindow ? g_addressWindow->DebugList() : std::vector<DebugAddressLockInfo>{};
}

void DebugSetAddressLock(uint32_t address, uint8_t value, bool locked) {
	if (g_addressWindow)
		g_addressWindow->DebugSet(address, value, locked);
}

bool DebugRemoveAddressLock(uint32_t address) {
	return g_addressWindow && g_addressWindow->DebugRemove(address);
}

void DebugClearAddressLocks() {
	if (g_addressWindow)
		g_addressWindow->DebugClear();
}
