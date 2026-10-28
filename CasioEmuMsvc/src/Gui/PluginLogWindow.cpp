#include "PluginLogWindow.hpp"
#include "Plugin/PluginMan.h"
#include "Plugin/PluginRegistry.h"
#include "imgui/imgui.h"

#include <sstream>

PluginLogWindow::PluginLogWindow() : UIWindow("Plugin Manager") {
	inital_size = ImVec2(900, 560);
	open = false;
}

void PluginLogWindow::RenderCore() {
	if (ImGui::BeginTabBar("PluginTabs")) {
		if (ImGui::BeginTabItem("Installed")) {
			RenderInstalled();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Developer")) {
			RenderDeveloper();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	RenderRemovePopup();
}

void PluginLogWindow::RenderInstalled() {
	auto& registry = casioemu::plugin::PluginRegistry::Instance();
	auto& entries = registry.Entries();

	if (ImGui::Button("Open Plugins Folder"))
		registry.OpenPluginsFolder();
	ImGui::SameLine();
	if (ImGui::Button("Reload Declarative"))
		registry.ReloadDeclarative();
	ImGui::SameLine();
	ImGui::TextDisabled("Drop a .plugin file onto the window to install it.");

	ImGui::Spacing();
	if (entries.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, UIHelpers::kColorMuted);
		ImGui::TextWrapped("No plugins installed. Open the plugins folder, add a .plugin package, then restart or reload.");
		ImGui::PopStyleColor();
		return;
	}

	const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable;
	if (!ImGui::BeginTable("PluginsTable", 6, flags))
		return;

	ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("Version");
	ImGui::TableSetupColumn("Type");
	ImGui::TableSetupColumn("State");
	ImGui::TableSetupColumn("Permissions");
	ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 210.0f);
	ImGui::TableHeadersRow();

	for (auto& entry : entries) {
		if (entry.removed)
			continue;

		ImGui::TableNextRow();
		const std::string row_id = entry.Id().empty() ? entry.package.file.string() : entry.Id();
		ImGui::PushID(row_id.c_str());

		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(entry.Name().empty() ? entry.package.file.filename().string().c_str() : entry.Name().c_str());
		ImGui::TextDisabled("%s", entry.Id().c_str());

		ImGui::TableSetColumnIndex(1);
		ImGui::TextUnformatted(entry.Version().c_str());

		ImGui::TableSetColumnIndex(2);
		ImGui::TextUnformatted(entry.package.valid ? casioemu::plugin::TypeName(entry.package.manifest.type) : "invalid");

		ImGui::TableSetColumnIndex(3);
		ImVec4 state_color = UIHelpers::kColorMuted;
		if (entry.state == "Loaded") state_color = UIHelpers::kColorSuccess;
		else if (entry.state == "Error" || entry.state == "Invalid" || entry.blocked) state_color = UIHelpers::kColorError;
		else if (entry.pendingTrust || entry.state == "Awaiting approval" || entry.state == "Deferred") state_color = UIHelpers::kColorWarning;
		ImGui::TextColored(state_color, "%s", entry.state.c_str());
		if (!entry.error.empty() && ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(480.0f);
			ImGui::TextUnformatted(entry.error.c_str());
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}

		ImGui::TableSetColumnIndex(4);
		if (entry.package.valid && !entry.package.manifest.permissions.empty()) {
			for (size_t i = 0; i < entry.package.manifest.permissions.size(); ++i) {
				if (i > 0)
					ImGui::SameLine();
				ImGui::TextDisabled("%s", entry.package.manifest.permissions[i].c_str());
			}
		}
		else {
			ImGui::TextDisabled("none");
		}

		ImGui::TableSetColumnIndex(5);
		if (!entry.package.valid) {
			if (ImGui::SmallButton("Reveal"))
				registry.RevealPath(entry.package.file);
		}
		else if (entry.pendingTrust || entry.blocked) {
			if (ImGui::SmallButton("Review"))
				registry.Review(entry.Id());
		}
		else if (entry.enabled) {
			if (ImGui::SmallButton("Disable"))
				registry.SetEnabled(entry.Id(), false);
		}
		else {
			if (ImGui::SmallButton("Enable"))
				registry.SetEnabled(entry.Id(), true);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Reveal"))
			registry.Reveal(entry.Id());
		ImGui::SameLine();
		if (ImGui::SmallButton("Remove")) {
			m_removeId = entry.Id();
			m_openRemovePopup = true;
		}

		ImGui::PopID();
	}

	ImGui::EndTable();
}

void PluginLogWindow::RenderDeveloper() {
	auto& registry = casioemu::plugin::PluginRegistry::Instance();
	ImGui::Text("Host API: v%d (%s)", registry.HostApiVersion(), casioemu::plugin::kHostVersion);
	ImGui::TextWrapped("Settings: %s", registry.SettingsDir().string().c_str());
	ImGui::TextWrapped("User plugins: %s", registry.UserPluginsDir().string().c_str());
	ImGui::TextWrapped("App plugins: %s", registry.AppPluginsDir().string().c_str());
	ImGui::TextWrapped("Package cache: %s", registry.CacheDir().string().c_str());
	ImGui::Separator();

	if (ImGui::Button("Copy Log"))
		ImGui::SetClipboardText(g_PluginLoadLog.c_str());
	ImGui::SameLine();
	if (ImGui::Button("Clear Log"))
		g_PluginLoadLog.clear();

	ImGui::BeginChild("PluginLog", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
	std::istringstream stream(g_PluginLoadLog);
	std::string line;
	while (std::getline(stream, line)) {
		if (line.find("[ERROR]") != std::string::npos || line.find("[FATAL]") != std::string::npos)
			ImGui::PushStyleColor(ImGuiCol_Text, UIHelpers::kColorError);
		else if (line.find("[WARN]") != std::string::npos)
			ImGui::PushStyleColor(ImGuiCol_Text, UIHelpers::kColorWarning);
		else if (line.find("[SUCCESS]") != std::string::npos)
			ImGui::PushStyleColor(ImGuiCol_Text, UIHelpers::kColorSuccess);
		else
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_Text]);
		ImGui::TextUnformatted(line.c_str());
		ImGui::PopStyleColor();
	}
	ImGui::EndChild();
}

void PluginLogWindow::RenderRemovePopup() {
	if (m_openRemovePopup) {
		ImGui::OpenPopup("Remove plugin?");
		m_openRemovePopup = false;
	}
	if (!ImGui::BeginPopupModal("Remove plugin?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		return;

	ImGui::TextWrapped("Remove plugin '%s'? The package cache and trust record are deleted. The original .plugin file is not touched.",
		m_removeId.c_str());
	ImGui::Spacing();
	if (ImGui::Button("Remove", ImVec2(120, 0))) {
		casioemu::plugin::PluginRegistry::Instance().Remove(m_removeId);
		m_removeId.clear();
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel", ImVec2(120, 0))) {
		m_removeId.clear();
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}
