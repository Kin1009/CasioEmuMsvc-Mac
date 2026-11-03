#include "Shell.h"

#include "Gui.h"
#include "Gui/ThemeManager.h"
#include "Gui/Ui.hpp"
#include "Plugin/PluginMan.h"
#include "Settings/Settings.h"
#include "SysDialog.h"
#include "Terminal/Terminal.h"
#include "Shell/SessionHost.h"
#include "ModelConfig.h"
#include "ModelInfo.h"
#include "Ext/MacWindow.h"

#include <SDL.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

extern bool g_shellBuildRequest;
extern bool g_shellBuildRunRequest;

namespace casioemu::shell {
namespace {

using casioemu::plugin::ThemePalette;
using casioemu::settings::SettingsData;
using casioemu::settings::SettingsStore;

constexpr float kTitleBarHeight = 35.0f;
constexpr float kTabStripHeight = 35.0f;
constexpr float kBreadcrumbHeight = 22.0f;
constexpr float kStatusBarHeight = 22.0f;
constexpr float kPanelHeaderHeight = 35.0f;
constexpr float kSidebarHeaderHeight = 22.0f;
constexpr float kActivityBarWidth = 48.0f;

struct OutputLine {
	std::string text;
	int severity = 0;
};

struct DiagLine {
	std::string file;
	int line = 0;
	int column = 0;
	int severity = 0;
	std::string text;
};

struct PaletteCommand {
	std::string id;
	std::string label;
	std::string hint;
};

struct State {
	bool initialized = false;
	bool reopenRequested = false;
	bool quitRequested = false;
	std::string startupModel;

	bool showWelcome = true;
	std::vector<SessionTab> tabs;
	int activeTab = -1;

	bool sidebarVisible = true;
	float sidebarWidth = 300.0f;
	bool panelVisible = true;
	float panelHeight = 180.0f;
	int panelTab = 0; // 0 output, 1 problems, 2 terminal
	bool toolsVisible = true;
	float toolsWidth = 360.0f;

	bool paletteOpen = false;
	bool quickOpen = false;
	bool paletteFocus = false;
	char paletteQuery[512]{};

	bool openModelOpen = false;
	bool openModelFocus = false;
	char openModelQuery[256]{};
	struct ModelChoice {
		std::string path;
		std::string name;
		std::string version;
		std::string type;
	};
	std::vector<ModelChoice> modelChoices;
	bool modelChoicesLoaded = false;
	bool restoredSessions = false;

	std::string workspace;
	std::set<std::string> expanded;
	std::vector<std::string> quickFiles;
	bool quickFilesDirty = true;
	std::string lastWorkspaceForFiles;

	std::vector<OutputLine> output;
	std::vector<DiagLine> problems;
	size_t pluginLogConsumed = 0;
	bool lastBuildOk = true;
	std::string lastBuildStatus = "Ready";

	bool draggingSidebar = false;
	bool draggingPanel = false;
	bool draggingTools = false;
	bool draggingWindow = false;
	ImVec2 dragStartMouse{};
	ImVec2 dragStartWindow{};
	double lastLayoutSave = 0.0;

	terminal::Terminal terminal;
	bool terminalStarted = false;

	ImVec4 rectViewport{};
	ImVec4 rectTitle{};
	ImVec4 rectTabs{};
	ImVec4 rectActivity{};
	ImVec4 rectSidebar{};
	ImVec4 rectBreadcrumb{};
	ImVec4 rectContent{};
	ImVec4 rectPanel{};
	ImVec4 rectStatus{};
	ImVec4 rectTools{};
};

State g;

ImVec4 WithAlpha(ImVec4 color, float alpha) {
	color.w = alpha;
	return color;
}

float TextWidth(const char* text) {
	return ImGui::CalcTextSize(text).x;
}

void SaveLayout() {
	auto& data = SettingsStore::Instance().Data();
	data.shell.sidebar.visible = g.sidebarVisible;
	data.shell.sidebar.width = static_cast<int>(g.sidebarWidth);
	data.shell.bottom.visible = g.panelVisible;
	data.shell.bottom.height = static_cast<int>(g.panelHeight);
	data.shell.bottom.active = g.panelTab == 0 ? "output" : (g.panelTab == 1 ? "problems" : "terminal");
	data.shell.tools.visible = g.toolsVisible;
	data.shell.tools.width = static_cast<int>(g.toolsWidth);
	data.workspace.folder = g.workspace;
	std::string error;
	SettingsStore::Instance().Save(&error);
}

void SyncTabsFromSessions() {
	const auto views = session::List();
	g.tabs.clear();
	for (const auto& view : views) {
		SessionTab tab;
		tab.id = "session-" + std::to_string(view.index + 1);
		tab.title = view.title;
		tab.model = view.model;
		tab.rom = view.romPath;
		tab.modelPath = view.modelPath;
		tab.live = view.live;
		g.tabs.push_back(std::move(tab));
	}
	g.activeTab = session::ActiveIndex();
	if (g.activeTab >= static_cast<int>(g.tabs.size()))
		g.activeTab = static_cast<int>(g.tabs.size()) - 1;
	if (g.activeTab >= 0)
		g.showWelcome = false;
}

void SaveSessions() {
	auto& data = SettingsStore::Instance().Data();
	data.sessions.clear();
	const auto views = session::List();
	for (const auto& view : views) {
		settings::SessionRecord record;
		record.id = "session-" + std::to_string(view.index + 1);
		record.model = view.model.empty() ? view.title : view.model;
		record.rom = view.romPath;
		record.modelPath = view.modelPath;
		record.title = view.title;
		record.active = view.active;
		data.sessions.push_back(std::move(record));
	}
	std::string error;
	SettingsStore::Instance().Save(&error);
}

void LoadModelChoices() {
	if (g.modelChoicesLoaded)
		return;
	g.modelChoicesLoaded = true;
	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator("models", ec)) {
		if (!entry.is_directory())
			continue;
		const std::string name = entry.path().filename().string();
		if (name.empty() || name == ".online")
			continue;
		State::ModelChoice choice;
		choice.path = entry.path().string();
		choice.name = name;
		ModelInfo info{};
		std::string error;
		if (LoadModelInfoFromFolder(entry.path(), info, nullptr, &error)) {
			if (!info.model_name.empty())
				choice.name = info.model_name;
			choice.type = HardwareDisplayName(info.hardware_id);
			choice.version = info.rom_path;
		}
		g.modelChoices.push_back(std::move(choice));
	}
	std::sort(g.modelChoices.begin(), g.modelChoices.end(),
		[](const State::ModelChoice& lhs, const State::ModelChoice& rhs) { return lhs.name < rhs.name; });
}

void OpenModelAt(const std::string& path) {
	if (path.empty())
		return;
	std::string error;
	if (!session::OpenSession(path, &error)) {
		AppendOutput("[session] failed to open " + path + ": " + error);
		return;
	}
	SyncTabsFromSessions();
	g.showWelcome = false;
	g.openModelOpen = false;
	SaveSessions();
	AppendOutput("[session] opened " + path);
}

void SaveLayoutThrottled() {
	const double now = ImGui::GetTime();
	if (now - g.lastLayoutSave < 0.4)
		return;
	g.lastLayoutSave = now;
	SaveLayout();
}

std::string Timestamp() {
	const std::time_t now = std::time(nullptr);
	std::tm tm{};
	localtime_r(&now, &tm);
	char buffer[16];
	std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &tm);
	return buffer;
}

UIWindow* FindWindow(const char* name) {
	for (auto* win : windows) {
		if (win && win->name && std::strcmp(win->name, name) == 0)
			return win;
	}
	return nullptr;
}

bool IsCalculatorWindow(const UIWindow* win) {
	return win && win->name && std::strcmp(win->name, "Calculator") == 0;
}

void EnsureTabsForSettings() {
	// A persisted session list belongs to the previous run.  Keep the tabs as
	// dormant shells; the live tab is the model this process started with.
}

std::string BaseName(const std::string& path) {
	return std::filesystem::path(path).filename().string();
}

void OpenExternal(const std::filesystem::path& path) {
#ifdef __APPLE__
	std::string uri = "file://" + path.string();
	SDL_OpenURL(uri.c_str());
#else
	SDL_OpenURL(("file://" + path.string()).c_str());
#endif
}

void BuildQuickFiles() {
	g.quickFiles.clear();
	g.quickFilesDirty = false;
	g.lastWorkspaceForFiles = g.workspace;
	if (g.workspace.empty())
		return;
	std::error_code ec;
	for (auto it = std::filesystem::recursive_directory_iterator(
			 g.workspace, std::filesystem::directory_options::skip_permission_denied, ec);
		 it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
		if (ec)
			break;
		if (it.depth() > 8) {
			it.disable_recursion_pending();
			continue;
		}
		const std::string name = it->path().filename().string();
		if (!name.empty() && name[0] == '.') {
			if (it->is_directory())
				it.disable_recursion_pending();
			continue;
		}
		if (it->is_directory()) {
			const std::string lower = name;
			if (lower == "build" || lower == "node_modules" || lower == "third_party")
				it.disable_recursion_pending();
			continue;
		}
		g.quickFiles.push_back(it->path().string());
		if (g.quickFiles.size() >= 4000)
			break;
	}
	std::sort(g.quickFiles.begin(), g.quickFiles.end());
}

bool MatchesFilter(const std::string& text, const std::string& filter) {
	if (filter.empty())
		return true;
	std::string needle;
	for (char ch : filter)
		needle.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
	std::string haystack;
	for (char ch : text)
		haystack.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
	return haystack.find(needle) != std::string::npos;
}

void SetWorkspace(const std::string& folder) {
	g.workspace = folder;
	g.expanded.clear();
	g.quickFilesDirty = true;
	auto& data = SettingsStore::Instance().Data();
	data.workspace.folder = folder;
	auto& recents = data.workspace.recentFolders;
	recents.erase(std::remove(recents.begin(), recents.end(), folder), recents.end());
	recents.insert(recents.begin(), folder);
	if (recents.size() > 10)
		recents.resize(10);
	SaveLayout();
	AppendOutput("[workspace] " + (folder.empty() ? std::string("(none)") : folder));
}

void ApplyThemeAndSave(const std::string& id) {
	std::string error;
	if (!ThemeManager::Instance().ApplyThemeByName(id, &error)) {
		AppendOutput("[theme] " + error);
		return;
	}
	AppendOutput("[theme] " + id);
}

void BuildToolchain(bool run) {
	// The compiler window owns the build pipeline; ask it to start a build on
	// its next frame through the shared request flags.
	::g_shellBuildRequest = true;
	::g_shellBuildRunRequest = run;
	UIWindow* win = FindWindow("Wiz Compiler");
	if (win)
		win->BringToFront();
	g.toolsVisible = true;
}

void ExecuteCommand(const std::string& id) {
	if (id == "session.new") {
		if (m_emu) {
			m_emu->ResetToMainMenu();
			g.showWelcome = false;
			if (g.activeTab < 0 && !g.tabs.empty())
				g.activeTab = 0;
			g.lastBuildStatus = "Calculator reset";
		}
	}
	else if (id == "model.open") RequestOpenModel();
	else if (id == "folder.open") RequestOpenFolder();
	else if (id == "plugin.install") RequestInstallPlugin();
	else if (id == "view.sidebar") ToggleSidebar();
	else if (id == "view.panel") ToggleBottomPanel();
	else if (id == "view.tools") ToggleTools();
	else if (id == "view.terminal") FocusTerminal();
	else if (id == "settings.open") OpenSettings();
	else if (id == "build") BuildToolchain(false);
	else if (id == "build.run") BuildToolchain(true);
	else if (id == "session.pause") { if (m_emu) m_emu->SetPaused(!m_emu->GetPaused()); }
	else if (id == "session.reset") { if (m_emu) m_emu->ResetToMainMenu(); }
	else if (id == "plugin.manager") { if (auto* win = FindWindow("Plugin Manager")) { win->BringToFront(); g.toolsVisible = true; } }
	else if (id == "theme.vscode-dark") ApplyThemeAndSave("vscode-dark");
	else if (id == "theme.vscode-light") ApplyThemeAndSave("vscode-light");
	else if (id == "theme.midnight-blue") ApplyThemeAndSave("midnight-blue");
	else if (id == "theme.solarized") ApplyThemeAndSave("solarized");
	else if (id == "theme.mono-green") ApplyThemeAndSave("mono-green");
}

std::vector<PaletteCommand> Commands() {
	return {
		{ "session.new", "Session: New Calculator", "Ctrl+N" },
		{ "model.open", "File: Open Model...", "Ctrl+O" },
		{ "folder.open", "File: Open Folder...", "Ctrl+Shift+O" },
		{ "plugin.install", "Plugins: Install Plugin...", "" },
		{ "plugin.manager", "Plugins: Open Plugin Manager", "" },
		{ "view.sidebar", "View: Toggle Sidebar", "Ctrl+B" },
		{ "view.panel", "View: Toggle Bottom Panel", "Ctrl+J" },
		{ "view.tools", "View: Toggle Tool Dock", "" },
		{ "view.terminal", "Terminal: Focus Terminal", "Ctrl+`" },
		{ "settings.open", "Preferences: Open Settings", "Ctrl+," },
		{ "session.pause", "Session: Pause / Resume", "" },
		{ "session.reset", "Session: Reset", "" },
		{ "build", "Build: Build", "F7" },
		{ "build.run", "Build: Build & Run", "F5" },
		{ "theme.vscode-dark", "Preferences: Color Theme - VS Code Dark Modern", "" },
		{ "theme.vscode-light", "Preferences: Color Theme - VS Code Light Modern", "" },
		{ "theme.midnight-blue", "Preferences: Color Theme - Midnight Blue", "" },
		{ "theme.solarized", "Preferences: Color Theme - Solarized", "" },
		{ "theme.mono-green", "Preferences: Color Theme - Mono Green", "" },
	};
}

void DrawFlatPanel(const char* name, ImVec2 pos, ImVec2 size, ImVec4 background, ImGuiWindowFlags extra = 0) {
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, background);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin(name, nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking | extra);
}

void EndFlatPanel() {
	ImGui::End();
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor();
}

void DrawSeparatorH(float x0, float x1, float y, ImU32 color) {
	ImGui::GetWindowDrawList()->AddLine(ImVec2(x0, y), ImVec2(x1, y), color, 1.0f);
}

void DrawSeparatorV(float x, float y0, float y1, ImU32 color) {
	ImGui::GetWindowDrawList()->AddLine(ImVec2(x, y0), ImVec2(x, y1), color, 1.0f);
}

void RenderTitleBar(const ThemePalette& palette) {
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	const ImVec2 pos = viewport->Pos;
	const float width = viewport->Size.x;
	DrawFlatPanel("##ShellTitle", pos, ImVec2(width, kTitleBarHeight), palette.titleBar);

	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddLine(ImVec2(pos.x, pos.y + kTitleBarHeight - 1.0f),
		ImVec2(pos.x + width, pos.y + kTitleBarHeight - 1.0f),
		ImGui::ColorConvertFloat4ToU32(palette.border), 1.0f);

	// Native traffic lights live at x 0..78.  Start the app title just after
	// them (12 px padding) so the chrome does not look empty.
	const float traffic = 82.0f;
	const float line_height = ImGui::GetTextLineHeight();
	const float text_y = pos.y + (kTitleBarHeight - line_height) * 0.5f;
	draw->AddText(ImVec2(pos.x + traffic + 12.0f, text_y),
		ImGui::ColorConvertFloat4ToU32(palette.foreground), "CasioEmuMsvc");

	// Original-style menu labels (native menu remains the real command host).
	const char* menues[] = { "File", "Edit", "View", "Session", "Help" };
	float menu_x = pos.x + traffic + 12.0f + TextWidth("CasioEmuMsvc") + 18.0f;
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.hover);
	ImGui::PushStyleColor(ImGuiCol_Text, palette.foreground);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f, 3.0f));
	for (const char* menu : menues) {
		ImGui::SetCursorScreenPos(ImVec2(menu_x, pos.y + 5.0f));
		ImGui::PushID(menu);
		if (ImGui::Button(menu, ImVec2(0, 25))) {
			if (std::strcmp(menu, "File") == 0)
				ImGui::OpenPopup("##file_menu");
			else if (std::strcmp(menu, "View") == 0)
				ImGui::OpenPopup("##view_menu");
			else if (std::strcmp(menu, "Session") == 0)
				ImGui::OpenPopup("##session_menu");
			else if (std::strcmp(menu, "Help") == 0)
				OpenCommandPalette();
		}
		if (ImGui::BeginPopup("##file_menu")) {
			if (ImGui::MenuItem("Open Model...")) RequestOpenModel();
			if (ImGui::MenuItem("Open Folder...")) RequestOpenFolder();
			if (ImGui::MenuItem("Settings...")) OpenSettings();
			ImGui::Separator();
			if (ImGui::MenuItem("Quit")) g.quitRequested = true;
			ImGui::EndPopup();
		}
		if (ImGui::BeginPopup("##view_menu")) {
			if (ImGui::MenuItem("Toggle Sidebar")) ToggleSidebar();
			if (ImGui::MenuItem("Toggle Panel")) ToggleBottomPanel();
			if (ImGui::MenuItem("Toggle Tool Dock")) ToggleTools();
			if (ImGui::MenuItem("Command Palette")) OpenCommandPalette();
			ImGui::EndPopup();
		}
		if (ImGui::BeginPopup("##session_menu")) {
			if (ImGui::MenuItem("Pause / Resume") && m_emu) m_emu->SetPaused(!m_emu->GetPaused());
			if (ImGui::MenuItem("Reset") && m_emu) m_emu->ResetToMainMenu();
			if (ImGui::MenuItem("Build")) BuildToolchain(false);
			if (ImGui::MenuItem("Build & Run")) BuildToolchain(true);
			ImGui::EndPopup();
		}
		ImGui::PopID();
		menu_x += TextWidth(menu) + 22.0f;
	}
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(3);

	const std::string title = g.showWelcome
		? std::string("CasioEmuMsvc")
		: (g.activeTab >= 0 && g.activeTab < (int)g.tabs.size() ? g.tabs[g.activeTab].title : "CasioEmuMsvc");
	const float command_x = pos.x + std::max(430.0f, menu_x + 8.0f);
	const float command_width = std::min(360.0f, width - (command_x - pos.x) - 320.0f);
	if (command_width > 160.0f) {
		ImGui::SetCursorScreenPos(ImVec2(command_x, pos.y + 5.0f));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, WithAlpha(palette.surfaceAlt, 0.85f));
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);
		ImGui::BeginChild("##CommandCenter", ImVec2(command_width, 25.0f), ImGuiChildFlags_None,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		const std::string label = "Search (Cmd+Shift+P)";
		ImGui::SetCursorPos(ImVec2(10.0f, 4.0f));
		ImGui::TextDisabled("%s", label.c_str());
		if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			OpenCommandPalette();
		ImGui::EndChild();
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();
	}

	// Right-hand toolbar: build, run, pause, settings.
	ImGui::SetCursorScreenPos(ImVec2(pos.x + width - 300.0f, pos.y + 4.0f));
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.hover);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, palette.selection);
	ImGui::PushStyleColor(ImGuiCol_Text, palette.foreground);
	if (ImGui::Button("Build", ImVec2(52, 26)))
		BuildToolchain(false);
	ImGui::SameLine();
	if (ImGui::Button("Run", ImVec2(44, 26)))
		BuildToolchain(true);
	ImGui::SameLine();
	if (ImGui::Button(m_emu && m_emu->GetPaused() ? "Resume" : "Pause", ImVec2(60, 26))) {
		if (m_emu)
			m_emu->SetPaused(!m_emu->GetPaused());
	}
	ImGui::SameLine();
	if (ImGui::Button("Palette", ImVec2(62, 26)))
		OpenCommandPalette();
	ImGui::SameLine();
	if (ImGui::Button("Settings", ImVec2(64, 26)))
		OpenSettings();
	ImGui::PopStyleColor(4);

	// Drag the window by the title bar.  Skip the command centre and buttons.
	const ImVec2 mouse = ImGui::GetIO().MousePos;
	const bool in_drag_zone = mouse.y >= pos.y && mouse.y < pos.y + kTitleBarHeight &&
		mouse.x > pos.x + traffic && mouse.x < pos.x + width - 310.0f;
	if (in_drag_zone && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) {
		g.draggingWindow = true;
		int mx = 0, my = 0;
		SDL_GetGlobalMouseState(&mx, &my);
		g.dragStartMouse = ImVec2((float)mx, (float)my);
		int wx = 0, wy = 0;
		if (window)
			SDL_GetWindowPosition(window, &wx, &wy);
		g.dragStartWindow = ImVec2(static_cast<float>(wx), static_cast<float>(wy));
	}
	if (g.draggingWindow) {
		if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && window) {
			int mx = 0, my = 0;
			SDL_GetGlobalMouseState(&mx, &my);
			SDL_SetWindowPosition(window,
				static_cast<int>(g.dragStartWindow.x + (mx - g.dragStartMouse.x)),
				static_cast<int>(g.dragStartWindow.y + (my - g.dragStartMouse.y)));
		}
		else {
			g.draggingWindow = false;
		}
	}

	EndFlatPanel();
}

void RenderTabStrip(const ThemePalette& palette) {
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	const ImVec2 pos(viewport->Pos.x, viewport->Pos.y + kTitleBarHeight);
	const float width = viewport->Size.x;
	DrawFlatPanel("##ShellTabs", pos, ImVec2(width, kTabStripHeight), palette.surface);
	const bool small = g_small_font != nullptr;
	if (small)
		ImGui::PushFont(g_small_font);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddLine(ImVec2(pos.x, pos.y + kTabStripHeight - 1.0f),
		ImVec2(pos.x + width, pos.y + kTabStripHeight - 1.0f),
		ImGui::ColorConvertFloat4ToU32(palette.border), 1.0f);

	ImGui::SetCursorScreenPos(ImVec2(pos.x + 8.0f, pos.y + 4.0f));
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.hover);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, palette.selection);

	auto tab_at = [&](int index) -> SessionTab* {
		if (index < 0 || index >= static_cast<int>(g.tabs.size()))
			return nullptr;
		return &g.tabs[static_cast<size_t>(index)];
	};

	const bool welcomeActive = g.showWelcome;
	float x = pos.x + 8.0f;
	if (welcomeActive) {
		const char* label = "Welcome";
		const float tab_width = TextWidth(label) + 34.0f;
		ImGui::SetCursorScreenPos(ImVec2(x, pos.y));
		ImGui::InvisibleButton("##welcome_tab", ImVec2(tab_width, kTabStripHeight));
		if (ImGui::IsItemClicked())
			g.showWelcome = true;
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		draw->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(palette.tabActive));
		draw->AddRectFilled(ImVec2(min.x, min.y), ImVec2(max.x, min.y + 2.0f),
			ImGui::ColorConvertFloat4ToU32(palette.accent));
		draw->AddText(ImVec2(min.x + 14.0f, min.y + 9.0f),
			ImGui::ColorConvertFloat4ToU32(palette.foreground), label);
		x += tab_width;
	}

	for (int index = 0; index < static_cast<int>(g.tabs.size()); ++index) {
		SessionTab& tab = g.tabs[static_cast<size_t>(index)];
		const bool active = (!welcomeActive && index == g.activeTab);
		std::string label = tab.title.empty() ? tab.model : tab.title;
		if (!tab.live)
			label += "  (not running)";
		const float tab_width = std::min(240.0f, TextWidth(label.c_str()) + 52.0f);
		ImGui::SetCursorScreenPos(ImVec2(x, pos.y));
		ImGui::PushID(index);
		ImGui::InvisibleButton("##tab", ImVec2(tab_width, kTabStripHeight));
		const bool hovered = ImGui::IsItemHovered();
		if (ImGui::IsItemClicked()) {
			g.activeTab = index;
			g.showWelcome = false;
			if (tab.live)
				session::Activate(index);
			else if (!tab.modelPath.empty())
				OpenModelAt(tab.modelPath);
		}
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		draw->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(active ? palette.background :
			(hovered ? palette.hover : palette.tabInactive)));
		if (active) {
			draw->AddRectFilled(ImVec2(min.x, max.y - 1.0f), ImVec2(max.x, max.y),
				ImGui::ColorConvertFloat4ToU32(palette.background));
		}
		const ImU32 text_color = ImGui::ColorConvertFloat4ToU32(active ? palette.foreground : palette.muted);
		draw->AddText(ImVec2(min.x + 12.0f, min.y + (kTabStripHeight - ImGui::GetTextLineHeight()) * 0.5f), text_color, label.c_str());
		// close button
		const ImVec2 close_pos(min.x + tab_width - 22.0f, min.y + (kTabStripHeight - ImGui::GetTextLineHeight()) * 0.5f);
		if (hovered) {
			draw->AddText(close_pos, text_color, "x");
			ImGui::SetCursorScreenPos(ImVec2(min.x + tab_width - 26.0f, min.y + 4.0f));
			ImGui::InvisibleButton("##close", ImVec2(22.0f, 27.0f));
			if (ImGui::IsItemClicked()) {
				if (index > 0 && tab.live) {
					session::CloseSession(index);
					SyncTabsFromSessions();
				}
				else if (tab.live) {
					g.showWelcome = true;
					g.activeTab = -1;
					if (m_emu)
						m_emu->ResetToMainMenu();
				}
				else {
					g.tabs.erase(g.tabs.begin() + index);
					if (g.activeTab >= index)
						--g.activeTab;
					if (g.tabs.empty())
						g.activeTab = -1;
				}
				SaveSessions();
				ImGui::PopID();
				break;
			}
		}
		ImGui::PopID();
		x += tab_width;
	}

	ImGui::SetCursorScreenPos(ImVec2(x + 4.0f, pos.y + 5.0f));
	if (ImGui::Button("+##newtab", ImVec2(28.0f, 25.0f))) {
		LoadModelChoices();
		g.openModelOpen = true;
		g.openModelFocus = true;
		g.openModelQuery[0] = '\0';
	}
	ImGui::SetCursorScreenPos(ImVec2(pos.x + width - 92.0f, pos.y + 6.0f));
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.hover);
	ImGui::PushStyleColor(ImGuiCol_Text, palette.muted);
	if (ImGui::Button("...", ImVec2(30, 23)))
		OpenCommandPalette();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("More Actions (Cmd+Shift+P)");
	ImGui::SameLine();
	if (ImGui::Button("||", ImVec2(30, 23)))
		g.toolsVisible = !g.toolsVisible;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Toggle Tool Dock");
	ImGui::PopStyleColor(3);
	if (small)
		ImGui::PopFont();
	EndFlatPanel();
}

void RenderSidebarTree(const std::filesystem::path& directory, int depth, const ThemePalette& palette) {
	if (depth > 12)
		return;
	std::error_code ec;
	std::vector<std::filesystem::directory_entry> entries;
	for (const auto& entry : std::filesystem::directory_iterator(
			 directory, std::filesystem::directory_options::skip_permission_denied, ec)) {
		entries.push_back(entry);
	}
	std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
		if (lhs.is_directory() != rhs.is_directory())
			return lhs.is_directory();
		return lhs.path().filename().string() < rhs.path().filename().string();
	});
	int count = 0;
	for (const auto& entry : entries) {
		if (++count > 400)
			break;
		const std::string name = entry.path().filename().string();
		if (name.empty() || name[0] == '.')
			continue;
		const std::string key = entry.path().string();
		const bool is_directory = entry.is_directory(ec);
		const std::string label = (is_directory ? (g.expanded.count(key) ? "[-] " : "[+] ") : "   ") + name;
		ImGui::SetCursorPosX(12.0f + depth * 12.0f);
		ImGui::PushStyleColor(ImGuiCol_Header, palette.selection);
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, palette.hover);
		ImGui::PushStyleColor(ImGuiCol_Text, palette.foreground);
		if (ImGui::Selectable((label + "##" + key).c_str(), false, ImGuiSelectableFlags_None)) {
			if (is_directory) {
				if (g.expanded.count(key))
					g.expanded.erase(key);
				else
					g.expanded.insert(key);
			}
			else {
				OpenExternal(entry.path());
				AppendOutput("[open] " + entry.path().string());
			}
		}
		ImGui::PopStyleColor(3);
		if (is_directory && g.expanded.count(key))
			RenderSidebarTree(entry.path(), depth + 1, palette);
	}
}

void RenderActivityBar(const ThemePalette& palette, ImVec2 pos, ImVec2 size) {
	DrawFlatPanel("##ShellActivity", pos, size, palette.activityBar);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddLine(ImVec2(pos.x + size.x - 1.0f, pos.y), ImVec2(pos.x + size.x - 1.0f, pos.y + size.y),
		ImGui::ColorConvertFloat4ToU32(palette.border), 1.0f);
	const ImU32 fg = ImGui::ColorConvertFloat4ToU32(palette.foreground);
	const ImU32 muted = ImGui::ColorConvertFloat4ToU32(palette.muted);
	const ImU32 accent = ImGui::ColorConvertFloat4ToU32(palette.accent);
	const struct { const char* label; const char* tooltip; } items[] = {
		{ "EX", "Explorer" }, { "SR", "Search" }, { "SC", "Source Control" },
		{ "RN", "Run and Debug" }, { "PL", "Plugins" },
	};
	float y = pos.y + 6.0f;
	for (const auto& item : items) {
		ImGui::SetCursorScreenPos(ImVec2(pos.x + 4.0f, y));
		ImGui::InvisibleButton(item.label, ImVec2(40.0f, 36.0f));
		const bool hovered = ImGui::IsItemHovered();
		const bool selected = std::strcmp(item.label, "EX") == 0;
		if (hovered)
			draw->AddRectFilled(ImVec2(pos.x + 4.0f, y), ImVec2(pos.x + 44.0f, y + 36.0f),
				ImGui::ColorConvertFloat4ToU32(palette.hover));
		if (selected)
			draw->AddRectFilled(ImVec2(pos.x, y), ImVec2(pos.x + 2.0f, y + 36.0f), accent);
		const float text_width = TextWidth(item.label);
		draw->AddText(ImVec2(pos.x + (size.x - text_width) * 0.5f,
			y + (36.0f - ImGui::GetTextLineHeight()) * 0.5f),
			(selected || hovered) ? fg : muted, item.label);
		if (hovered)
			ImGui::SetTooltip("%s", item.tooltip);
		y += 40.0f;
	}
	const float bottom_y = pos.y + size.y - 36.0f;
	ImGui::SetCursorScreenPos(ImVec2(pos.x + 4.0f, bottom_y));
	ImGui::InvisibleButton("##activity_settings", ImVec2(40.0f, 32.0f));
	draw->AddText(ImVec2(pos.x + (size.x - TextWidth("SET")) * 0.5f,
		bottom_y + (32.0f - ImGui::GetTextLineHeight()) * 0.5f),
		ImGui::IsItemHovered() ? fg : muted, "SET");
	if (ImGui::IsItemClicked())
		OpenSettings();
	EndFlatPanel();
}

void RenderBreadcrumb(const ThemePalette& palette, ImVec2 pos, ImVec2 size) {
	DrawFlatPanel("##ShellBreadcrumb", pos, size, palette.background);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddLine(ImVec2(pos.x, pos.y + size.y - 1.0f), ImVec2(pos.x + size.x, pos.y + size.y - 1.0f),
		ImGui::ColorConvertFloat4ToU32(palette.border), 1.0f);
	const bool small = g_small_font != nullptr;
	if (small)
		ImGui::PushFont(g_small_font);
	std::string text = "CasioEmuMsvc";
	if (g.activeTab >= 0 && g.activeTab < static_cast<int>(g.tabs.size()))
		text += "  >  " + g.tabs[static_cast<size_t>(g.activeTab)].title;
	if (!g.workspace.empty())
		text += "  >  " + BaseName(g.workspace);
	draw->AddText(ImVec2(pos.x + 12.0f, pos.y + 4.0f),
		ImGui::ColorConvertFloat4ToU32(palette.muted), text.c_str());
	if (small)
		ImGui::PopFont();
	EndFlatPanel();
}

void RenderSidebar(const ThemePalette& palette, ImVec2 pos, ImVec2 size) {
	DrawFlatPanel("##ShellSidebar", pos, size, palette.surface);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + kSidebarHeaderHeight),
		ImGui::ColorConvertFloat4ToU32(palette.titleBar));
	draw->AddText(ImVec2(pos.x + 12.0f, pos.y + 4.0f),
		ImGui::ColorConvertFloat4ToU32(palette.muted), "EXPLORER");
	ImGui::SetCursorScreenPos(ImVec2(pos.x + size.x - 70.0f, pos.y + 2.0f));
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.hover);
	ImGui::PushStyleColor(ImGuiCol_Text, palette.foreground);
	if (ImGui::Button("Open", ImVec2(46.0f, 18.0f)))
		RequestOpenFolder();
	ImGui::SameLine();
	if (ImGui::Button("+", ImVec2(18.0f, 18.0f)))
		RequestOpenFolder();
	ImGui::PopStyleColor(3);

	ImGui::SetCursorScreenPos(ImVec2(pos.x + 8.0f, pos.y + kSidebarHeaderHeight + 3.0f));
	ImGui::BeginChild("##explorer_body", ImVec2(size.x - 16.0f, size.y - kSidebarHeaderHeight - 10.0f), ImGuiChildFlags_None);
	if (g.workspace.empty()) {
		ImGui::TextWrapped("You have not yet opened a folder.");
		ImGui::Spacing();
		if (ImGui::TextLink("Open Folder"))
			RequestOpenFolder();
		ImGui::Spacing();
		ImGui::TextDisabled("RECENT");
		const auto& recents = SettingsStore::Instance().Data().workspace.recentFolders;
		for (const auto& folder : recents) {
			if (ImGui::Selectable((std::string("> ") + BaseName(folder)).c_str()))
				SetWorkspace(folder);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", folder.c_str());
		}
	}
	else {
		ImGui::TextDisabled("WORKSPACE");
		const std::string folder_label = std::string(g.expanded.count(g.workspace) ? "[-] " : "[+] ") + BaseName(g.workspace);
		if (ImGui::Selectable(folder_label.c_str())) {
			if (g.expanded.count(g.workspace))
				g.expanded.erase(g.workspace);
			else
				g.expanded.insert(g.workspace);
		}
		if (g.expanded.empty() || g.expanded.count(g.workspace))
			RenderSidebarTree(g.workspace, 1, palette);
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::TextDisabled("OPEN EDITORS");
		ImGui::TextDisabled("No open editors");
	}
	ImGui::EndChild();
	EndFlatPanel();
}

void RenderWelcome(const ThemePalette& palette) {
	ImGui::SetCursorPos(ImVec2(32.0f, 28.0f));
	ImGui::PushStyleColor(ImGuiCol_Text, palette.foreground);
	ImGui::SetWindowFontScale(2.0f);
	ImGui::TextUnformatted("CasioEmuMsvc");
	ImGui::SetWindowFontScale(1.0f);
	ImGui::PopStyleColor();
	ImGui::SetCursorPosX(33.0f);
	ImGui::TextDisabled("Casio nX-U8/100 & nX-U16/100 emulator and debugger");

	ImGui::Dummy(ImVec2(0.0f, 12.0f));
	const float column_width = std::max(240.0f, ImGui::GetContentRegionAvail().x * 0.45f);

	auto section = [&](const char* title) {
		ImGui::TextDisabled("%s", title);
	};
	auto link = [&](const char* label, const std::string& command) {
		const ImVec4 link_color = palette.link;
		ImGui::PushStyleColor(ImGuiCol_Text, link_color);
		if (ImGui::Selectable(label, false, ImGuiSelectableFlags_None, ImVec2(column_width, 0.0f)))
			ExecuteCommand(command);
		ImGui::PopStyleColor();
	};

	ImGui::Columns(2, "##welcome_columns", false);
	ImGui::SetColumnWidth(0, column_width + 40.0f);
	section("Start");
	link("New Calculator", "session.new");
	link("Open Model...", "model.open");
	link("Open Folder...", "folder.open");
	link("Install Plugin...", "plugin.install");
	ImGui::Dummy(ImVec2(0.0f, 10.0f));
	section("Recent");
	const auto& data = SettingsStore::Instance().Data();
	bool any_recent = false;
	for (const auto& session : data.sessions) {
		any_recent = true;
		if (ImGui::Selectable((std::string("> ") + (session.title.empty() ? session.model : session.title)).c_str()))
			g.lastBuildStatus = "Restore with File > Open Model";
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s\n%s", session.model.c_str(), session.rom.c_str());
	}
	for (const auto& file : data.workspace.recentFiles) {
		any_recent = true;
		if (ImGui::Selectable((std::string("> ") + BaseName(file)).c_str()))
			OpenExternal(file);
	}
	if (!any_recent)
		ImGui::TextDisabled("No recent sessions or files");

	ImGui::NextColumn();
	section("Walkthroughs");
	ImGui::TextDisabled("Get started with the debugger");
	link("Open the Wiz compiler", "ui.toolchain");
	link("Show the plugin manager", "plugin.manager");
	link("Browse the model list", "model.open");
	ImGui::Dummy(ImVec2(0.0f, 10.0f));
	section("Help");
	if (ImGui::TextLink("Documentation"))
		SDL_OpenURL("https://github.com/telecomadm1145/CasioEmuMsvc");
	if (ImGui::TextLink("Keyboard shortcuts"))
		OpenCommandPalette();
	ImGui::Columns(1);
}

void RenderCalculator(const ThemePalette& palette) {
	UIWindow* calculator = FindWindow("Calculator");
	if (!calculator) {
		ImGui::TextDisabled("The calculator is not available in this build.");
		return;
	}
	ImGui::PushStyleColor(ImGuiCol_ChildBg, palette.background);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, palette.background);
	if (ImGui::BeginChild("##calculator_host", ImGui::GetContentRegionAvail(), ImGuiChildFlags_None,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
		ImGui::SetCursorPos(ImVec2(8.0f, 8.0f));
		ImGui::BeginChild("##calculator_inner", ImVec2(-16.0f, -16.0f), ImGuiChildFlags_None,
			ImGuiWindowFlags_NoScrollbar);
		calculator->RenderCore();
		ImGui::EndChild();
		const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
		if (hovered && !g.paletteOpen && !TerminalFocused())
			calculator_panel_focused = true;
	}
	ImGui::EndChild();
	ImGui::PopStyleColor(2);
}

void RenderContent(const ThemePalette& palette, ImVec2 pos, ImVec2 size) {
	DrawFlatPanel("##ShellContent", pos, size, palette.background);
	ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
	if (g.showWelcome) {
		RenderWelcome(palette);
	}
	else if (g.activeTab >= 0 && g.activeTab < static_cast<int>(g.tabs.size())) {
		const SessionTab& tab = g.tabs[static_cast<size_t>(g.activeTab)];
		if (tab.live)
			RenderCalculator(palette);
		else {
			ImGui::Dummy(ImVec2(0.0f, 40.0f));
			ImGui::SetCursorPosX(32.0f);
			ImGui::TextDisabled("%s", tab.title.c_str());
			ImGui::SetCursorPosX(32.0f);
			ImGui::TextWrapped("This session was restored from the previous run but is not running in this process. "
				"CasioEmuMsvc keeps one live emulator per process; open the model again to run it.");
			ImGui::SetCursorPosX(32.0f);
			if (ImGui::Button("Open Model..."))
				RequestOpenModel();
		}
	}
	else {
		RenderWelcome(palette);
	}
	EndFlatPanel();
}

void RenderOutputTab() {
	ImGui::BeginChild("##output_body", ImVec2(0, 0), ImGuiChildFlags_None);
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
	for (const auto& line : g.output) {
		if (line.severity == 2)
			ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", line.text.c_str());
		else
			ImGui::TextUnformatted(line.text.c_str());
	}
	if (g.output.empty())
		ImGui::TextDisabled("No output yet. Build or run something.");
	if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
		ImGui::SetScrollHereY(1.0f);
	ImGui::EndChild();
}

void RenderProblemsTab() {
	ImGui::BeginChild("##problems_body", ImVec2(0, 0), ImGuiChildFlags_None);
	if (g.problems.empty()) {
		ImGui::TextDisabled("No problems have been detected in the workspace.");
	}
	for (const auto& problem : g.problems) {
		const ImVec4 color = problem.severity >= 2 ? ImVec4(0.945f, 0.298f, 0.298f, 1.0f)
			: (problem.severity == 1 ? ImVec4(0.8f, 0.655f, 0.0f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
		const std::string label = problem.file.empty()
			? problem.text
			: (problem.file + ":" + std::to_string(problem.line) + ":" + std::to_string(problem.column) + "  " + problem.text);
		ImGui::TextColored(color, "%s", label.c_str());
		if (ImGui::IsItemClicked()) {
			if (auto* win = FindWindow("Wiz Compiler"))
				win->BringToFront();
		}
	}
	ImGui::EndChild();
}

void RenderBottomPanel(const ThemePalette& palette, ImVec2 pos, ImVec2 size) {
	DrawFlatPanel("##ShellPanel", pos, size, palette.surface);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddLine(ImVec2(pos.x, pos.y), ImVec2(pos.x + size.x, pos.y),
		ImGui::ColorConvertFloat4ToU32(palette.borderStrong), 1.0f);
	draw->AddRectFilled(ImVec2(pos.x, pos.y), ImVec2(pos.x + size.x, pos.y + kPanelHeaderHeight),
		ImGui::ColorConvertFloat4ToU32(palette.surface));

	const char* tabs[] = { "OUTPUT", "PROBLEMS", "TERMINAL" };
	float x = pos.x + 12.0f;
	for (int index = 0; index < 3; ++index) {
		const float width = TextWidth(tabs[index]) + 26.0f;
		ImGui::SetCursorScreenPos(ImVec2(x, pos.y));
		ImGui::PushID(index);
		ImGui::InvisibleButton("##paneltab", ImVec2(width, kPanelHeaderHeight));
		const bool hovered = ImGui::IsItemHovered();
		if (ImGui::IsItemClicked()) {
			g.panelTab = index;
			if (index == 2 && !g.terminalStarted) {
				g.terminal.Start(g.workspace);
				g.terminalStarted = true;
			}
		}
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		if (g.panelTab == index) {
			draw->AddRectFilled(ImVec2(min.x, max.y - 2.0f), max,
				ImGui::ColorConvertFloat4ToU32(palette.accent));
		}
		else if (hovered) {
			draw->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(palette.hover));
		}
		draw->AddText(ImVec2(min.x + 12.0f, min.y + 10.0f),
			ImGui::ColorConvertFloat4ToU32(g.panelTab == index ? palette.foreground : palette.muted), tabs[index]);
		if (index == 1 && !g.problems.empty()) {
			const std::string count = std::to_string(g.problems.size());
			draw->AddText(ImVec2(min.x + width - 18.0f, min.y + 10.0f),
				ImGui::ColorConvertFloat4ToU32(palette.error), count.c_str());
		}
		ImGui::PopID();
		x += width;
	}

	// right-side chevron / close
	ImGui::SetCursorScreenPos(ImVec2(pos.x + size.x - 58.0f, pos.y + 6.0f));
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.hover);
	ImGui::PushStyleColor(ImGuiCol_Text, palette.foreground);
	if (ImGui::Button("v##panel_collapse", ImVec2(22, 22)))
		g.panelVisible = false;
	ImGui::SameLine();
	if (ImGui::Button("x##panel_close", ImVec2(22, 22)))
		g.panelVisible = false;
	ImGui::PopStyleColor(3);

	ImGui::SetCursorScreenPos(ImVec2(pos.x + 1.0f, pos.y + kPanelHeaderHeight));
	ImGui::BeginChild("##panel_content", ImVec2(size.x - 2.0f, size.y - kPanelHeaderHeight - 1.0f), ImGuiChildFlags_None);
	if (g.panelTab == 0)
		RenderOutputTab();
	else if (g.panelTab == 1)
		RenderProblemsTab();
	else {
		if (!g.terminalStarted) {
			g.terminal.Start(g.workspace);
			g.terminalStarted = true;
		}
		g.terminal.Render("##terminal_view", ImGui::GetContentRegionAvail().y, true);
	}
	ImGui::EndChild();

	// resize grip along the top edge
	ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y - 2.0f));
	ImGui::InvisibleButton("##panel_resize", ImVec2(size.x, 5.0f));
	if (ImGui::IsItemActive()) {
		g.panelHeight = std::clamp(g.panelHeight - ImGui::GetIO().MouseDelta.y,
			80.0f, ImGui::GetMainViewport()->Size.y * 0.6f);
		SaveLayoutThrottled();
	}
	EndFlatPanel();
}

void RenderStatusBar(const ThemePalette& palette, ImVec2 pos, ImVec2 size) {
	DrawFlatPanel("##ShellStatus", pos, size, palette.statusBar);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImU32 fg = ImGui::ColorConvertFloat4ToU32(palette.statusBarForeground);
	const ImU32 dim = ImGui::ColorConvertFloat4ToU32(WithAlpha(palette.statusBarForeground, 0.8f));
	const bool small = g_small_font != nullptr;
	if (small)
		ImGui::PushFont(g_small_font);
	float x = pos.x + 10.0f;
	auto item = [&](const char* text, ImU32 color) {
		draw->AddText(ImVec2(x, pos.y + 2.0f), color, text);
		x += TextWidth(text) + 8.0f;
		draw->AddText(ImVec2(x, pos.y + 2.0f), dim, "|");
		x += TextWidth("|") + 8.0f;
	};
	const bool paused = m_emu && m_emu->GetPaused();
	item(paused ? "Paused" : "Running", fg);
	const std::string live = (g.activeTab >= 0 && g.activeTab < (int)g.tabs.size())
		? g.tabs[g.activeTab].title : "No session";
	item(live.c_str(), fg);
	char pc[32];
	std::snprintf(pc, sizeof(pc), "PC %05X", pc_cache);
	item(pc, fg);
	item(g.lastBuildStatus.c_str(), fg);
	std::string right = "Ln 1, Col 1   UTF-8   LF   " +
		SettingsStore::Instance().Data().language + "   " +
		ThemeManager::Instance().CurrentThemeName();
	const float right_width = TextWidth(right.c_str());
	draw->AddText(ImVec2(pos.x + size.x - right_width - 10.0f, pos.y + 2.0f), fg, right.c_str());
	if (small)
		ImGui::PopFont();
	EndFlatPanel();
}

void RenderToolDock(const ThemePalette& palette, ImVec2 pos, ImVec2 size, ImGuiID& out_id) {
	out_id = 0;
	if (!g.toolsVisible || size.x < 80.0f)
		return;
	DrawFlatPanel("##ShellToolDock", pos, size, palette.surface, ImGuiWindowFlags_NoDocking);
	ImGuiID dockspace = ImGui::GetID("CasioEmuToolDock");
	out_id = dockspace;
	ImGui::DockSpace(dockspace, ImVec2(0, 0), ImGuiDockNodeFlags_AutoHideTabBar);
	EndFlatPanel();
}

void RenderToolWindows(ImGuiID dock_id) {
	if (!g.toolsVisible)
		return;
	for (auto* win : windows) {
		if (!win || IsCalculatorWindow(win))
			continue;
		ImGui::SetNextWindowDockID(dock_id, ImGuiCond_FirstUseEver);
		win->Render();
	}
}

void ExecutePaletteSelection(const std::string& id) {
	ExecuteCommand(id);
	g.paletteOpen = false;
	g.quickOpen = false;
	g.paletteQuery[0] = '\0';
}

void RenderOverlays(const ThemePalette& palette) {
	if (g.openModelOpen) {
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		const float width = std::min(680.0f, viewport->Size.x - 120.0f);
		ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + (viewport->Size.x - width) * 0.5f,
			viewport->Pos.y + 70.0f));
		ImGui::SetNextWindowSize(ImVec2(width, 420.0f));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, palette.surface);
		ImGui::PushStyleColor(ImGuiCol_Border, palette.borderStrong);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
		if (ImGui::Begin("##open_model_overlay", nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking)) {
			ImGui::TextDisabled("Open Model");
			ImGui::Separator();
			if (g.openModelFocus) {
				ImGui::SetKeyboardFocusHere();
				g.openModelFocus = false;
			}
			ImGui::SetNextItemWidth(-1.0f);
			const bool submitted = ImGui::InputTextWithHint("##open_model_query", "Search installed models",
				g.openModelQuery, sizeof(g.openModelQuery), ImGuiInputTextFlags_EnterReturnsTrue);
			LoadModelChoices();
			std::string first_match;
			int shown = 0;
			ImGui::BeginChild("##open_model_list", ImVec2(0, 330.0f), ImGuiChildFlags_None);
			for (const auto& choice : g.modelChoices) {
				if (!MatchesFilter(choice.name + " " + choice.type, g.openModelQuery))
					continue;
				if (first_match.empty())
					first_match = choice.path;
				if (++shown > 200)
					break;
				const std::string label = choice.name + "##" + choice.path;
				if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || submitted)
						OpenModelAt(choice.path);
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s\n%s", choice.path.c_str(), choice.type.c_str());
			}
			if (shown == 0)
				ImGui::TextDisabled("No models found in ./models");
			ImGui::EndChild();
			if (submitted && !first_match.empty())
				OpenModelAt(first_match);
			if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
				g.openModelOpen = false;
		}
		ImGui::End();
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(2);
	}
	if (!g.paletteOpen && !g.quickOpen)
		return;
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	const bool quick = g.quickOpen;
	const float width = std::min(680.0f, viewport->Size.x - 120.0f);
	const ImVec2 pos(viewport->Pos.x + (viewport->Size.x - width) * 0.5f, viewport->Pos.y + 60.0f);
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, palette.surface);
	ImGui::PushStyleColor(ImGuiCol_Border, palette.borderStrong);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
	if (ImGui::Begin("##palette_overlay", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking)) {
		if (g.paletteFocus) {
			ImGui::SetKeyboardFocusHere();
			g.paletteFocus = false;
		}
		ImGui::SetNextItemWidth(-1.0f);
		const bool submitted = ImGui::InputText("##palette_query", g.paletteQuery, sizeof(g.paletteQuery),
			ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::Separator();

		if (quick) {
			if (g.quickFilesDirty || g.lastWorkspaceForFiles != g.workspace)
				BuildQuickFiles();
			int shown = 0;
			if (g.workspace.empty())
				ImGui::TextDisabled("Open a folder to use Quick Open.");
			for (const auto& file : g.quickFiles) {
				const std::string relative = std::filesystem::relative(file, g.workspace).string();
				if (!MatchesFilter(relative, g.paletteQuery))
					continue;
				if (++shown > 14)
					break;
				if (ImGui::Selectable(relative.c_str(), false, ImGuiSelectableFlags_None)) {
					OpenExternal(file);
					g.quickOpen = false;
					g.paletteQuery[0] = '\0';
				}
			}
			if (submitted && shown > 0) {
				for (const auto& file : g.quickFiles) {
					const std::string relative = std::filesystem::relative(file, g.workspace).string();
					if (MatchesFilter(relative, g.paletteQuery)) {
						OpenExternal(file);
						g.quickOpen = false;
						g.paletteQuery[0] = '\0';
						break;
					}
				}
			}
		}
		else {
			const auto commands = Commands();
			int shown = 0;
			for (const auto& command : commands) {
				if (!MatchesFilter(command.label, g.paletteQuery))
					continue;
				if (++shown > 14)
					break;
				if (ImGui::Selectable((command.label + "##" + command.id).c_str(), false,
						ImGuiSelectableFlags_None)) {
					ExecutePaletteSelection(command.id);
				}
			}
			if (submitted && shown > 0) {
				for (const auto& command : commands) {
					if (MatchesFilter(command.label, g.paletteQuery)) {
						ExecutePaletteSelection(command.id);
						break;
					}
				}
			}
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
			g.paletteOpen = false;
			g.quickOpen = false;
			g.paletteQuery[0] = '\0';
		}
	}
	ImGui::End();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor(2);
}

void RenderShortcuts() {
	ImGuiIO& io = ImGui::GetIO();
	const bool super = io.KeySuper;
	if (!super)
		return;
	if (ImGui::IsKeyPressed(ImGuiKey_P, false)) {
		if (io.KeyShift) {
			OpenCommandPalette();
		}
		else {
			OpenQuickOpen();
		}
	}
	if (ImGui::IsKeyPressed(ImGuiKey_B, false)) ToggleSidebar();
	if (ImGui::IsKeyPressed(ImGuiKey_J, false)) ToggleBottomPanel();
	if (ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false)) FocusTerminal();
	if (ImGui::IsKeyPressed(ImGuiKey_Comma, false)) OpenSettings();
	if (ImGui::IsKeyPressed(ImGuiKey_1, false) && g.tabs.size() >= 1) { g.activeTab = 0; g.showWelcome = false; }
	if (ImGui::IsKeyPressed(ImGuiKey_2, false) && g.tabs.size() >= 2) { g.activeTab = 1; g.showWelcome = false; }
	if (ImGui::IsKeyPressed(ImGuiKey_3, false) && g.tabs.size() >= 3) { g.activeTab = 2; g.showWelcome = false; }
	if (ImGui::IsKeyPressed(ImGuiKey_4, false) && g.tabs.size() >= 4) { g.activeTab = 3; g.showWelcome = false; }
	if (ImGui::IsKeyPressed(ImGuiKey_5, false) && g.tabs.size() >= 5) { g.activeTab = 4; g.showWelcome = false; }
}

void RenderLayout() {
	const ThemePalette& palette = ThemeManager::Instance().Colors();
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	const float width = viewport->Size.x;
	const float height = viewport->Size.y;
	const float top = kTitleBarHeight + kTabStripHeight;
	const float status_h = kStatusBarHeight;
	const float panel_h = g.panelVisible ? std::min(g.panelHeight, height * 0.6f) : 0.0f;
	const float tools_w = g.toolsVisible ? std::min(g.toolsWidth, width * 0.45f) : 0.0f;
	const float sidebar_w = g.sidebarVisible ? std::min(g.sidebarWidth, width * 0.40f) : 0.0f;
	const float activity_w = kActivityBarWidth;
	const float content_x = viewport->Pos.x + activity_w + sidebar_w;
	const float content_w = std::max(160.0f, width - activity_w - sidebar_w - tools_w);
	const float body_h = std::max(160.0f, height - top - status_h);
	const float editor_h = std::max(80.0f, body_h - panel_h);
	const float editor_y = viewport->Pos.y + top + kBreadcrumbHeight;
	const float editor_content_h = std::max(40.0f, editor_h - kBreadcrumbHeight);
	const float panel_y = viewport->Pos.y + top + editor_h;

	g.rectViewport = ImVec4(viewport->Pos.x, viewport->Pos.y, width, height);
	g.rectTitle = ImVec4(viewport->Pos.x, viewport->Pos.y, width, kTitleBarHeight);
	g.rectTabs = ImVec4(viewport->Pos.x, viewport->Pos.y + kTitleBarHeight, width, kTabStripHeight);
	g.rectActivity = ImVec4(viewport->Pos.x, viewport->Pos.y + top, activity_w, body_h);
	g.rectSidebar = ImVec4(viewport->Pos.x + activity_w, viewport->Pos.y + top, sidebar_w, body_h);
	g.rectBreadcrumb = ImVec4(content_x, viewport->Pos.y + top, content_w, kBreadcrumbHeight);
	g.rectContent = ImVec4(content_x, editor_y, content_w, editor_content_h);
	g.rectPanel = ImVec4(content_x, panel_y, content_w, panel_h);
	g.rectStatus = ImVec4(viewport->Pos.x, viewport->Pos.y + height - status_h, width, status_h);
	g.rectTools = ImVec4(content_x + content_w, viewport->Pos.y + top, tools_w, body_h);

	RenderTitleBar(palette);
	RenderTabStrip(palette);
	RenderActivityBar(palette, ImVec2(viewport->Pos.x, viewport->Pos.y + top), ImVec2(activity_w, body_h));
	if (g.sidebarVisible)
		RenderSidebar(palette, ImVec2(viewport->Pos.x + activity_w, viewport->Pos.y + top),
			ImVec2(sidebar_w, body_h));
	RenderBreadcrumb(palette, ImVec2(content_x, viewport->Pos.y + top), ImVec2(content_w, kBreadcrumbHeight));

	ImGuiID dock_id = 0;
	if (g.toolsVisible)
		RenderToolDock(palette, ImVec2(content_x + content_w, viewport->Pos.y + top),
			ImVec2(tools_w, body_h), dock_id);
	RenderToolWindows(dock_id);

	RenderContent(palette, ImVec2(content_x, editor_y), ImVec2(content_w, editor_content_h));
	if (g.panelVisible)
		RenderBottomPanel(palette, ImVec2(content_x, panel_y), ImVec2(content_w, panel_h));
	RenderStatusBar(palette, ImVec2(viewport->Pos.x, viewport->Pos.y + height - status_h),
		ImVec2(width, status_h));

	if (g.sidebarVisible) {
		ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + activity_w + sidebar_w - 2.0f, viewport->Pos.y + top));
		ImGui::SetNextWindowSize(ImVec2(4.0f, body_h));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
		ImGui::Begin("##sidebar_grip", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground);
		ImGui::InvisibleButton("##grip", ImVec2(4.0f, body_h));
		if (ImGui::IsItemActive()) {
			g.sidebarWidth = std::clamp(g.sidebarWidth + ImGui::GetIO().MouseDelta.x, 200.0f, width * 0.45f);
			SaveLayoutThrottled();
		}
		ImGui::End();
		ImGui::PopStyleColor();
	}
	if (g.toolsVisible) {
		ImGui::SetNextWindowPos(ImVec2(content_x + content_w - 2.0f, viewport->Pos.y + top));
		ImGui::SetNextWindowSize(ImVec2(4.0f, body_h));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
		ImGui::Begin("##tools_grip", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground);
		ImGui::InvisibleButton("##grip", ImVec2(4.0f, body_h));
		if (ImGui::IsItemActive()) {
			g.toolsWidth = std::clamp(g.toolsWidth - ImGui::GetIO().MouseDelta.x, 240.0f, width * 0.6f);
			SaveLayoutThrottled();
		}
		ImGui::End();
		ImGui::PopStyleColor();
	}
	RenderOverlays(palette);
	RenderShortcuts();
}
} // namespace

void Initialize() {
	if (g.initialized)
		return;
	g.initialized = true;
	auto& data = SettingsStore::Instance().Data();
	g.sidebarVisible = data.shell.sidebar.visible;
	g.sidebarWidth = static_cast<float>(std::clamp(data.shell.sidebar.width, 200, 600));
	g.panelVisible = data.shell.bottom.visible;
	g.panelHeight = static_cast<float>(std::clamp(data.shell.bottom.height, 200, 700));
	g.panelTab = data.shell.bottom.active == "problems" ? 1 : (data.shell.bottom.active == "terminal" ? 2 : 0);
	g.toolsVisible = data.shell.tools.visible;
	g.toolsWidth = static_cast<float>(std::clamp(data.shell.tools.width, 240, 800));
	g.workspace = data.workspace.folder;

	SyncTabsFromSessions();

	// Mirror the plugin host's log into the Output panel.
	AppendOutput("[shell] CasioEmuMsvc preview shell initialized");
	if (!g.workspace.empty())
		AppendOutput("[workspace] " + g.workspace);
}

void Shutdown() {
	SaveLayout();
	SaveSessions();
	g.terminal.Stop();
}

void SetLiveSession(const std::string& modelPath, const std::string& modelName, const std::string& romPath) {
	if (!g.initialized)
		Initialize();
	SyncTabsFromSessions();
	if (g.activeTab < 0)
		g.activeTab = 0;
	g.showWelcome = false;
	(void)modelPath;
	(void)modelName;
	(void)romPath;
	SaveSessions();
}

void AppendOutput(const std::string& line) {
	OutputLine entry;
	entry.text = "[" + Timestamp() + "] " + line;
	g.output.push_back(std::move(entry));
	if (g.output.size() > 4000)
		g.output.erase(g.output.begin(), g.output.begin() + 1000);
}

void AppendDiagnostic(LogSeverity severity, const std::string& line) {
	DiagLine diag;
	diag.severity = static_cast<int>(severity);
	// Try to recognise file:line:column prefixes from the compiler.
	const auto first_colon = line.find(':');
	if (first_colon != std::string::npos) {
		const auto second_colon = line.find(':', first_colon + 1);
		if (second_colon != std::string::npos) {
			diag.file = line.substr(0, first_colon);
			try {
				diag.line = std::stoi(line.substr(first_colon + 1, second_colon - first_colon - 1));
				const auto third_colon = line.find(':', second_colon + 1);
				if (third_colon != std::string::npos) {
					diag.column = std::stoi(line.substr(second_colon + 1, third_colon - second_colon - 1));
					diag.text = line.substr(third_colon + 1);
				}
				else {
					diag.text = line.substr(second_colon + 1);
				}
			}
			catch (...) {
				diag.file.clear();
				diag.line = 0;
				diag.column = 0;
				diag.text = line;
			}
		}
	}
	if (diag.text.empty())
		diag.text = line;
	g.problems.push_back(std::move(diag));
	AppendOutput(line);
	if (g.problems.size() > 500)
		g.problems.erase(g.problems.begin(), g.problems.begin() + 100);
}

void NotifyBuildFinished(bool ok) {
	g.lastBuildOk = ok;
	g.lastBuildStatus = ok ? "Build succeeded" : "Build failed";
	if (!ok) {
		g.panelVisible = true;
		g.panelTab = 1;
	}
}

bool ConsumeReopenRequest() {
	const bool result = g.reopenRequested;
	g.reopenRequested = false;
	return result;
}

bool ConsumeQuitRequest() {
	const bool result = g.quitRequested;
	g.quitRequested = false;
	return result;
}

const std::string& ConsumeStartupModel() {
	return g.startupModel;
}

bool TerminalFocused() {
	return g.panelVisible && g.panelTab == 2 && g.terminal.Focused();
}

void RequestOpenModel() {
	LoadModelChoices();
	g.openModelOpen = true;
	g.openModelFocus = true;
	g.openModelQuery[0] = '\0';
}

void RequestOpenFolder() {
	SystemDialogs::OpenFolderDialog([](std::filesystem::path path) {
		SetWorkspace(path.string());
	});
}

void RequestInstallPlugin() {
	if (auto* win = FindWindow("Plugin Manager")) {
		win->BringToFront();
		g.toolsVisible = true;
	}
}

void ToggleSidebar() {
	g.sidebarVisible = !g.sidebarVisible;
	SaveLayout();
}

void ToggleBottomPanel() {
	g.panelVisible = !g.panelVisible;
	SaveLayout();
	if (g.panelVisible && g.panelTab == 2 && !g.terminalStarted) {
		g.terminal.Start(g.workspace);
		g.terminalStarted = true;
	}
}

void ToggleTools() {
	g.toolsVisible = !g.toolsVisible;
	SaveLayout();
}

void FocusTerminal() {
	g.panelVisible = true;
	g.panelTab = 2;
	if (!g.terminalStarted) {
		g.terminal.Start(g.workspace);
		g.terminalStarted = true;
	}
	g.terminal.SetFocused(true);
	SaveLayout();
}

void OpenSettings() {
	if (auto* win = FindWindow("Settings"))
		win->BringToFront();
	g.toolsVisible = true;
}

void OpenCommandPalette() {
	g.paletteOpen = true;
	g.quickOpen = false;
	g.paletteFocus = true;
	g.paletteQuery[0] = '\0';
}

void OpenQuickOpen() {
	g.quickOpen = true;
	g.paletteOpen = false;
	g.paletteFocus = true;
	g.paletteQuery[0] = '\0';
	g.quickFilesDirty = true;
	BuildQuickFiles();
}

std::string WorkspaceFolder() {
	return g.workspace;
}

bool WriteLayoutReport(const std::string& path) {
	std::ofstream output(path, std::ios::trunc);
	if (!output)
		return false;
	auto rect_text = [](const ImVec4& r) {
		char buffer[128];
		std::snprintf(buffer, sizeof(buffer), "x=%.1f y=%.1f w=%.1f h=%.1f right=%.1f bottom=%.1f",
			r.x, r.y, r.z, r.w, r.x + r.z, r.y + r.w);
		return std::string(buffer);
	};
	bool ok = true;
	auto check = [&](bool condition, const std::string& name) {
		output << (condition ? "PASS " : "FAIL ") << name << '\n';
		if (!condition)
			ok = false;
	};
	output << "viewport " << rect_text(g.rectViewport) << '\n';
	output << "title " << rect_text(g.rectTitle) << '\n';
	output << "tabs " << rect_text(g.rectTabs) << '\n';
	output << "activity " << rect_text(g.rectActivity) << '\n';
	output << "sidebar " << rect_text(g.rectSidebar) << '\n';
	output << "breadcrumb " << rect_text(g.rectBreadcrumb) << '\n';
	output << "content " << rect_text(g.rectContent) << '\n';
	output << "panel " << rect_text(g.rectPanel) << '\n';
	output << "status " << rect_text(g.rectStatus) << '\n';
	output << "tools " << rect_text(g.rectTools) << '\n';
	const float eps = 0.6f;
	check(std::fabs((g.rectTitle.y + g.rectTitle.w) - g.rectTabs.y) < eps, "chrome/tab strip do not overlap");
	check(std::fabs((g.rectTabs.y + g.rectTabs.w) - g.rectActivity.y) < eps, "tab strip/body do not overlap");
	check(std::fabs((g.rectActivity.y + g.rectActivity.w) - g.rectStatus.y) < eps, "body/status do not overlap");
	check(std::fabs((g.rectStatus.y + g.rectStatus.w) - (g.rectViewport.y + g.rectViewport.w)) < eps, "status bar is the last row");
	check(g.rectActivity.x >= g.rectViewport.x - eps, "activity bar is inside the window");
	check(g.rectSidebar.x >= g.rectActivity.x + 47.0f, "sidebar has an activity-bar gutter");
	check(g.rectBreadcrumb.x >= g.rectSidebar.x + g.rectSidebar.z - eps, "content does not overlap the sidebar");
	check(g.rectPanel.x >= g.rectSidebar.x + g.rectSidebar.z - eps, "bottom panel starts after the sidebar");
	check(g.rectPanel.z <= g.rectContent.z + eps, "bottom panel stays inside the editor column");
	check(g.rectContent.z > 200.0f && g.rectContent.w > 80.0f, "content region is non-zero");
	check(g.rectTitle.z > 0 && g.rectTabs.z > 0, "chrome and tab strip are non-zero");
	check(g.rectStatus.w >= 20.0f, "status bar has the 22 px reference height");
	auto font = ImGui::GetIO().Fonts->Fonts.empty() ? nullptr : ImGui::GetIO().Fonts->Fonts[0];
	check(font != nullptr, "UI font is loaded");
	check(g_small_font != nullptr, "12 px chrome font is loaded");
	check(g_mono_font != nullptr, "13 px monospace font is loaded");
	if (g_small_font) {
		check(g_small_font->FindGlyph(0x25B8) != nullptr, "small font has chevron U+25B8");
		check(g_small_font->FindGlyph(0x2014) != nullptr, "small font has em dash U+2014");
		check(g_small_font->FindGlyph(0x2502) != nullptr, "small font has box drawing U+2502");
	}
	if (g_mono_font)
		check(g_mono_font->FindGlyph(U'M') != nullptr, "mono font has ASCII glyphs");
	output << (ok ? "RESULT PASS\n" : "RESULT FAIL\n");
	return ok;
}

void Render() {
	if (!g.initialized)
		Initialize();

	if (const char* second = std::getenv("CASIOEMU_OPEN_SECOND")) {
		static bool opened_second = false;
		if (!opened_second) {
			opened_second = true;
			std::error_code ec;
			for (const auto& entry : std::filesystem::directory_iterator("models", ec)) {
				if (!entry.is_directory())
					continue;
				const std::string name = entry.path().filename().string();
				if (name.find("Graph Light") != std::string::npos) {
					OpenModelAt(entry.path().string());
					AppendOutput("[selftest] opened second session: " + name);
					break;
				}
			}
		}
		(void)second;
	}

	if (!g.restoredSessions) {
		g.restoredSessions = true;
		const auto records = SettingsStore::Instance().Data().sessions;
		const int active = session::ActiveIndex();
		for (const auto& record : records) {
			if (record.modelPath.empty())
				continue;
			std::error_code ec;
			if (!std::filesystem::is_directory(record.modelPath, ec))
				continue;
			std::string error;
			if (session::OpenSession(record.modelPath, &error))
				AppendOutput("[session] restored " + record.modelPath);
		}
		SyncTabsFromSessions();
		if (active >= 0 && active < session::Count())
			session::Activate(active);
		g.activeTab = session::ActiveIndex();
	}

	// Mirror plugin-host messages into Output.
	const std::string& plugin_log = g_PluginLoadLog;
	if (plugin_log.size() > g.pluginLogConsumed) {
		std::string fresh = plugin_log.substr(g.pluginLogConsumed);
		g.pluginLogConsumed = plugin_log.size();
		std::stringstream stream(fresh);
		std::string line;
		while (std::getline(stream, line)) {
			if (!line.empty())
				AppendOutput("[plugins] " + line);
		}
	}

	calculator_panel_focused = false;
	RenderLayout();
}

} // namespace casioemu::shell
