#include "PluginLogWindow.hpp"
#include "Plugin/PluginMan.h"
#include "Plugin/PluginRegistry.h"
#include "ToolUi.hpp"
#include "imgui/imgui.h"

#include <sstream>

PluginLogWindow::PluginLogWindow() : UIWindow("Plugin Manager") {
	inital_size = ImVec2(900, 560);
	open = true;
}

void PluginLogWindow::RenderCore() {
	ToolUi::PageScope page("Plugin Manager");
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

	ToolUi::Section("Plugins");
	ToolUi::Description("Declarative themes, injectors, tools and native plugins. Native plugins run in-process, so review their permissions before approval.");
	ImGui::Spacing();
	{
		ToolUi::PanelScope panel("##plugin_actions");
		if (ToolUi::AccentButton("Install Plugin..."))
			registry.InstallFromDialog();
		ImGui::SameLine();
		if (ImGui::Button("Open Plugins Folder"))
			registry.OpenPluginsFolder();
		ImGui::SameLine();
		if (ImGui::Button("Reload Declarative"))
			registry.ReloadDeclarative();
		ToolUi::Hint("Or copy a .plugin into the plugins folder and restart.");
	}

	ToolUi::Section("Installed Plugins");
	if (entries.empty()) {
		ToolUi::EmptyState("No plugins installed. Use Install Plugin..., drop a .plugin into the plugins folder, then restart or reload.");
		return;
	}

	ImGui::BeginChild("##plugins_body", ImVec2(0, std::max(160.0f, ImGui::GetContentRegionAvail().y - 4.0f)));
	if (ToolUi::TableScope table{"PluginsTable", 6,
			ToolUi::TableFlags() | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY,
			ImGui::GetContentRegionAvail().y}) {
		ImGui::TableSetupScrollFreeze(1, 1);
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed, 80.0f);
		ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 90.0f);
		ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 120.0f);
		ImGui::TableSetupColumn("Permissions", ImGuiTableColumnFlags_WidthStretch, 1.4f);
		ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 230.0f);
		ImGui::TableHeadersRow();

		// Only entries that are still present; the clipper works on that count.
		std::vector<size_t> visible_entries;
		visible_entries.reserve(entries.size());
		for (size_t index = 0; index < entries.size(); ++index)
			if (!entries[index].removed)
				visible_entries.push_back(index);
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(visible_entries.size()));
		while (clipper.Step()) {
		for (int visible_index = clipper.DisplayStart; visible_index < clipper.DisplayEnd; ++visible_index) {
			auto& entry = entries[visible_entries[static_cast<size_t>(visible_index)]];

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
			ImGui::PushStyleColor(ImGuiCol_Text, entry.state == "Loaded" ? ToolUi::Palette().success
				: entry.state == "Loaded (limited)" ? ToolUi::Palette().warning
				: (entry.state == "Error" || entry.state == "Invalid" || entry.blocked) ? ToolUi::Palette().error
				: (entry.pendingTrust || entry.state == "Awaiting approval" || entry.state == "Deferred") ? ToolUi::Palette().warning
				: ToolUi::Palette().muted);
			ImGui::TextUnformatted(entry.state.c_str());
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered() && (!entry.error.empty() || entry.limited)) {
				ImGui::BeginTooltip();
				ImGui::PushTextWrapPos(480.0f);
				if (!entry.error.empty())
					ImGui::TextUnformatted(entry.error.c_str());
				if (entry.limited) {
					ImGui::TextUnformatted("Permission-limited. Denied capabilities:");
					for (const auto& capability : entry.deniedCapabilities)
						ImGui::BulletText("%s", capability.c_str());
				}
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
				if (entry.loaded) {
					if (ImGui::SmallButton("Disable"))
						registry.SetEnabled(entry.Id(), false);
				}
				else {
					if (ImGui::SmallButton("Load"))
						registry.SetEnabled(entry.Id(), true);
				}
			}
			else {
				if (ImGui::SmallButton("Enable"))
					registry.SetEnabled(entry.Id(), true);
			}
			if (entry.loaded && entry.IsNative()) {
				ImGui::SameLine();
				if (ImGui::SmallButton("Unload"))
					registry.Unload(entry.Id());
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
		}
	}
	ImGui::EndChild();
}

void PluginLogWindow::RenderDeveloper() {
	auto& registry = casioemu::plugin::PluginRegistry::Instance();
	ToolUi::Section("Host");
	{
		ToolUi::PanelScope panel("##plugin_host_info");
		ImGui::Text("Host API: v%d (%s)", registry.HostApiVersion(), casioemu::plugin::kHostVersion);
		ImGui::TextWrapped("Settings: %s", registry.SettingsDir().string().c_str());
		ImGui::TextWrapped("User plugins: %s", registry.UserPluginsDir().string().c_str());
		ImGui::TextWrapped("App plugins: %s", registry.AppPluginsDir().string().c_str());
		ImGui::TextWrapped("Package cache: %s", registry.CacheDir().string().c_str());
	}

	ToolUi::Section("Load log");
	if (ImGui::Button("Copy Log"))
		ImGui::SetClipboardText(g_PluginLoadLog.c_str());
	ImGui::SameLine();
	if (ImGui::Button("Clear Log"))
		g_PluginLoadLog.clear();
	ImGui::Spacing();

	ImGui::BeginChild("PluginLog", ImVec2(0, std::max(120.0f, ImGui::GetContentRegionAvail().y - 4.0f)),
		ImGuiChildFlags_Border, ImGuiWindowFlags_HorizontalScrollbar);
	std::istringstream stream(g_PluginLoadLog);
	std::string line;
	while (std::getline(stream, line)) {
		if (line.find("[ERROR]") != std::string::npos || line.find("[FATAL]") != std::string::npos)
			ImGui::PushStyleColor(ImGuiCol_Text, ToolUi::Palette().error);
		else if (line.find("[WARN]") != std::string::npos)
			ImGui::PushStyleColor(ImGuiCol_Text, ToolUi::Palette().warning);
		else if (line.find("[SUCCESS]") != std::string::npos)
			ImGui::PushStyleColor(ImGuiCol_Text, ToolUi::Palette().success);
		else
			ImGui::PushStyleColor(ImGuiCol_Text, ToolUi::Palette().foreground);
		ImGui::TextUnformatted(line.c_str());
		ImGui::PopStyleColor();
	}
	if (g_PluginLoadLog.empty())
		ToolUi::Hint("The plugin host has not logged anything yet.");
	ImGui::EndChild();
}

void PluginLogWindow::RenderRemovePopup() {
	if (m_openRemovePopup) {
		ImGui::OpenPopup("Remove plugin?###plugin_remove");
		m_openRemovePopup = false;
	}
	ToolUi::DialogScope dialog("Remove plugin?###plugin_remove", "Remove plugin", 460.0f);
	if (!dialog)
		return;
	ImGui::TextWrapped("Remove plugin '%s'? The package cache and trust record are deleted. The original .plugin file is not touched.",
		m_removeId.c_str());
	ImGui::Spacing();
	if (ToolUi::AccentButton("Remove", ImVec2(120, 0))) {
		casioemu::plugin::PluginRegistry::Instance().Remove(m_removeId);
		m_removeId.clear();
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel", ImVec2(120, 0)) || dialog.CloseRequested()) {
		m_removeId.clear();
		ImGui::CloseCurrentPopup();
	}
}
