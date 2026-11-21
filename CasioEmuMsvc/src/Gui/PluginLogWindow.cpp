// LEGACY / no longer instantiated.  The plugin manager now lives entirely in
// Settings > Plugins (see Gui/Theme.cpp, SettingsWindow::RenderPluginList and
// friends).  This translation unit is kept compiling so the build's GUI source
// glob stays stable; nothing creates a "Plugin Manager" tool window any more.
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
	ToolUi::Description("Only .plugin packages are accepted. Native dylibs must be embedded inside a .plugin (ZIP) package.");
	ImGui::Spacing();
	{
		ToolUi::PanelScope panel("##plugin_actions");
		if (ToolUi::AccentButton("Install .plugin...")) {
			// Native picker + callback: the old synchronous InstallFromDialog
			// always reported "no file selected" because its ImGui popup was
			// never drawn. Status updates when the native dialog returns.
			registry.RequestInstall([this](bool installed, std::string error) {
				if (installed)
					m_status = "Installed .plugin package.";
				else
					m_status = "Install failed: " + (error.empty() ? std::string("unknown error") : error);
			});
		}
		ImGui::SameLine();
		if (ImGui::Button("Open Plugins Folder"))
			registry.OpenPluginsFolder();
		ImGui::SameLine();
		if (ImGui::Button("Reload")) {
			registry.Reload();
			m_status = "Plugin folders re-scanned.";
		}
		ImGui::SameLine();
		if (ImGui::Button("Reload Declarative"))
			registry.ReloadDeclarative();
		ToolUi::Hint("Plugins are stored inside the app bundle (Contents/MacOS/plugins). Installed packages may be a ZIP container with embedded dylibs.");
		if (!m_status.empty()) {
			ImGui::Spacing();
			const bool failed = m_status.find("failed") != std::string::npos ||
				m_status.find("Failed") != std::string::npos;
			ImGui::PushStyleColor(ImGuiCol_Text,
				failed ? ToolUi::Palette().error : ToolUi::Palette().success);
			ImGui::TextWrapped("%s", m_status.c_str());
			ImGui::PopStyleColor();
		}
	}

	ToolUi::Section("Installed Plugins");
	if (entries.empty()) {
		ToolUi::EmptyState("No plugins installed.");
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
			if (entry.zipContainer) {
				ImGui::SameLine();
				ImGui::TextDisabled("[ZIP]");
				if (ImGui::IsItemHovered() && !entry.embeddedDylibs.empty()) {
					ImGui::BeginTooltip();
					ImGui::TextUnformatted("Embedded dylibs:");
					for (const auto& dylib : entry.embeddedDylibs)
						ImGui::BulletText("%s", dylib.c_str());
					ImGui::EndTooltip();
				}
			}

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
		ToolUi::Hint("No log entries yet.");
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
	ImGui::TextWrapped("Remove plugin '%s'? Its extracted package cache, trust record and the .plugin file in the app's plugins folder are deleted. This cannot be undone.",
		m_removeId.c_str());
	ImGui::Spacing();
	if (ToolUi::AccentButton("Remove", ImVec2(120, 0))) {
		casioemu::plugin::PluginRegistry::Instance().Remove(m_removeId);
		m_status = "Removed " + m_removeId + ".";
		m_removeId.clear();
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel", ImVec2(120, 0)) || dialog.CloseRequested()) {
		m_removeId.clear();
		ImGui::CloseCurrentPopup();
	}
}
