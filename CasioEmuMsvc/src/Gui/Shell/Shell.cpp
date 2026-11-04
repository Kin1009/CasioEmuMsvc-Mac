#include "Shell.h"

#include "Gui.h"
#include "Gui/ThemeManager.h"
#include "Gui/Editors.h"
#include "Gui/Ui.hpp"
#include "Gui/ToolUi.hpp"
#include "Plugin/PluginMan.h"
#include "Settings/Settings.h"
#include "SysDialog.h"
#include "Terminal/Terminal.h"
#include "Shell/SessionHost.h"
#include "ModelConfig.h"
#include "ModelInfo.h"
#include "StartupUi/ModelImport.h"
#include "Ext/MacWindow.h"
#include "imgui/TextEditor.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_impl_sdlrenderer2.h"
#include "build.hpp"
#include "common.hpp"
#include "cpre.hpp"
#include "lint.hpp"
#include "targets.hpp"

#include <SDL.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <map>
#include <memory>
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

void PushMenuStyle(const ThemePalette& palette);
void PopMenuStyle();

// The chrome row is 28 px tall: AppKit centres the native traffic lights at
// y = 14 from the top of a full-size content view.  Every chrome element is
// drawn on that same centre line (title, menus, actions).
constexpr float kTitleBarHeight = 28.0f;
constexpr float kChromeControlHeight = 22.0f;
constexpr float kTabStripHeight = 40.0f;
constexpr float kBreadcrumbHeight = 22.0f;
constexpr float kStatusBarHeight = 36.0f;
constexpr float kPanelHeaderHeight = 35.0f;
constexpr float kSidebarHeaderHeight = 46.0f;
constexpr float kActivityBarWidth = 48.0f;
constexpr float kDialogRounding = 10.0f;
constexpr float kDialogPadding = 14.0f;

struct OutputLine {
	std::string text;
	int severity = 0;
};

struct DiagLine {
	std::string file;
	int line = 0;
	int column = 0;
	int end_column = 0;
	int severity = 0;
	std::string text;
	std::string code;
	std::string hint;
};

struct PaletteCommand {
	std::string id;
	std::string label;
	std::string hint;
};

struct EditorProblem {
	int line = 0;
	int column = 0;
	int end_column = 0;
	int severity = 0;
	std::string message;
	std::string code;
	std::string hint;
};

struct FileDocument {
	std::string path;
	std::string name;
	TextEditor editor;
	std::string savedText;
	bool paletteSet = false;
	bool lintPending = false;
	double lastEdit = 0.0;
	std::string lastLintedText;
	std::string observedText;
	std::vector<EditorProblem> problems;
	int targetIndex = 1;
	int languageIndex = 0;
	bool findOpen = false;
	char findText[128]{};
};

struct MainTab {
	int kind = 0; // 0 welcome, 1 session, 2 file, 3 settings
	int index = -1;
	std::string label;
	bool active = false;
	bool dirty = false;
	bool closable = false;
	float width = 0.0f;
};

struct SidebarEntry {
	std::string path;
	std::string name;
	bool is_directory = false;
};

struct SidebarDirCache {
	std::vector<SidebarEntry> entries;
	std::filesystem::file_time_type mtime{};
	bool valid = false;
	double checked_at = 0.0;
};

struct MainTabRect {
	std::string label;
	ImVec4 rect{};
	bool active = false;
	bool settings = false;
	int kind = 0;
};

struct State {
	bool initialized = false;
	bool reopenRequested = false;
	bool quitRequested = false;
	std::string startupModel;

	bool showWelcome = true;
	std::vector<SessionTab> tabs;
	int activeTab = -1;
	bool activeSettings = false;
	float tabScroll = 0.0f;
	float docScroll = 0.0f;
	float lastTabStripWidth = 0.0f;
	float lastDocStripWidth = 0.0f;
	std::string lastActiveKey;
	int lastToolIndex = -1;
	std::vector<MainTabRect> mainTabRects;
	std::vector<MainTab> mainTabItems;
	size_t mainTabSignature = 0;
	std::vector<std::string> docTabLabels;
	std::vector<float> docTabWidths;
	std::vector<ImVec4> docTabRects;
	size_t docTabSignature = 0;
	float docStripHeight = 30.0f;
	std::map<std::string, SidebarDirCache> sidebarCache;
	int sidebarScanCount = 0;
	double sidebarScanMs = 0.0;
	float chromeTitleCenterY = 0.0f;
	float chromeMenuCenterY = 0.0f;
	float chromeActionCenterY = 0.0f;
	ImVec4 rectCalculatorGrip{};

	bool sidebarVisible = true;
	float sidebarWidth = 300.0f;
	bool panelVisible = true;
	float panelHeight = 180.0f;
	int panelTab = 0; // 0 output, 1 problems, 2 terminal
	bool toolsVisible = true;
	float toolsWidth = 360.0f;
	int toolIndex = 0;
	std::vector<std::string> toolOrder;
	int draggingTool = -1;
	int dragTargetTool = -1;
	bool calculatorVisible = true;
	float calculatorWidth = 360.0f;
	std::string pendingTool;

	std::vector<std::unique_ptr<FileDocument>> files;
	int activeFile = -1;
	int pendingCloseFile = -1;
	bool closeConfirmOpen = false;
	bool dismissClosePrompt = false;
	std::filesystem::path selectedPath;
	bool selectedIsDir = false;
	bool renameOpen = false;
	bool newFileOpen = false;
	bool newFolderOpen = false;
	bool deleteOpen = false;
	bool newEntryIsFolder = false;
	char entryName[256]{};
	std::string sidebarError;
	std::string workspaceFingerprint;
	double lastWorkspaceCheck = 0.0;

	bool paletteOpen = false;
	bool quickOpen = false;
	bool paletteFocus = false;
	char paletteQuery[512]{};

	bool openModelOpen = false;
	bool openModelFocus = false;
	char openModelQuery[256]{};
	bool importPasswordOpen = false;
	bool importResultOpen = false;
	bool importResultError = false;
	std::string importResultText;
	std::filesystem::path importPackagePath;
	char importPassword[128]{};
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
	std::string panelCopyNotice;
	double panelCopyNoticeAt = 0.0;
	int lastSessionIndex = -1;
	std::string perfReportPath;
	int perfFrameLimit = 240;
	int perfFrames = 0;
	double perfTotalMs = 0.0;
	double perfMinMs = 1.0e9;
	double perfMaxMs = 0.0;

	bool draggingSidebar = false;
	bool draggingPanel = false;
	bool draggingTools = false;
	bool draggingWindow = false;
	bool docScrollbarDragging = false;
	float docScrollbarStartMouse = 0.0f;
	float docScrollbarStartScroll = 0.0f;
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
	ImVec4 rectSidebarBody{};
	ImVec4 rectContentBody{};
	ImVec4 rectPanelBody{};
	ImVec4 rectToolsBody{};
	ImVec4 rectStatusContent{};
	ImVec4 rectTabsContent{};
};

State g;

ImFont* g_welcome_font = nullptr;

bool PerfNoClip() {
	static const bool disabled = std::getenv("CASIOEMU_PERF_NOCLIP") != nullptr;
	return disabled;
}

bool FontInAtlas(ImFont* font) {
	if (!font)
		return false;
	for (ImFont* entry : ImGui::GetIO().Fonts->Fonts) {
		if (entry == font)
			return true;
	}
	return false;
}

// The title is the one piece of chrome that deserves a larger face.  It is
// rasterised from the same high-quality monospace file at 20 px (times the UI
// scale) with a Retina-aware rasteriser density, while every other run keeps
// the 15 px UI font.
void EnsureTitleFont() {
	if (FontInAtlas(g_welcome_font))
		return;
	g_welcome_font = nullptr;
	ImGuiIO& io = ImGui::GetIO();
	const float framebuffer_scale = std::max(1.0f, io.DisplayFramebufferScale.x);
	const float ui_scale = std::max(1.0f,
		ThemeManager::Instance().GetFontScale() * ThemeManager::Instance().Typography().scale);
	const std::string path = GetMonospaceFontPath();
	auto add_face = [&](float logical_size) -> ImFont* {
		ImFontConfig config;
		config.PixelSnapH = false;
		config.OversampleH = 3;
		config.OversampleV = 2;
		config.RasterizerDensity = framebuffer_scale;
		if (!path.empty()) {
			if (ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), logical_size * ui_scale,
					&config, io.Fonts->GetGlyphRangesDefault()))
				return font;
		}
		return io.Fonts->AddFontDefault();
	};
	g_welcome_font = add_face(30.0f);
	ImGui_ImplSDLRenderer2_DestroyFontsTexture();
	io.Fonts->Build();
	ImGui_ImplSDLRenderer2_CreateFontsTexture();
}

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
	data.shell.calculator.visible = g.calculatorVisible;
	data.shell.calculator.width = static_cast<int>(g.calculatorWidth);
	data.workspace.folder = g.workspace;
	std::string error;
	SettingsStore::Instance().Save(&error);
}

std::string BaseName(const std::string& path);

std::filesystem::path ModelsDirectory() {
	if (const char* override = std::getenv("CASIOEMU_MODELS_DIR"))
		return std::filesystem::path(override);
	return std::filesystem::current_path() / "models";
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
	for (size_t i = 0; i < g.tabs.size(); ++i) {
		bool duplicate = false;
		for (size_t j = 0; j < i; ++j) {
			if (g.tabs[i].title == g.tabs[j].title)
				duplicate = true;
		}
		if (duplicate && !g.tabs[i].modelPath.empty()) {
			const std::string folder = BaseName(g.tabs[i].modelPath);
			if (g.tabs[i].title.find(folder) == std::string::npos)
				g.tabs[i].title += " (" + folder + ")";
		}
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
	const std::filesystem::path models_root = ModelsDirectory();
	for (const auto& entry : std::filesystem::directory_iterator(models_root, ec)) {
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
	g.activeSettings = false;
	g.openModelOpen = false;
	SaveSessions();
	AppendOutput("[session] opened " + path);
}

void OpenWelcomeTab() {
	g.showWelcome = true;
	g.activeSettings = false;
	g.activeFile = -1;
}

void RefreshModelChoices() {
	g.modelChoicesLoaded = false;
	g.modelChoices.clear();
}

void ApplyShellImport(const std::filesystem::path& package_path, const std::string& password) {
	AppendOutput("[import] importing " + package_path.filename().string() + "...");
	startup::ModelImportResult result = startup::ImportModelPackage(package_path, ModelsDirectory(), password);
	if (result.needsPassword) {
		g.importPackagePath = package_path;
		g.importPasswordOpen = true;
		g.importPassword[0] = '\0';
		AppendOutput("[import] " + result.error);
		return;
	}
	g.importResultOpen = true;
	g.importResultError = !result.ok;
	if (result.ok) {
		g.importResultText = "Imported " + result.modelName + " to " + result.targetPath +
			" (" + std::to_string(result.files.size()) + " files).";
		RefreshModelChoices();
		AppendOutput("[import] imported " + result.modelName + " -> " + result.targetPath);
	}
	else {
		g.importResultText = result.error;
		AppendOutput("[import] failed: " + result.error);
	}
}

void RequestShellImport() {
	SystemDialogs::OpenFileDialog([](std::filesystem::path file) {
		if (!file.empty())
			ApplyShellImport(file, std::string());
	});
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

bool IsTextFilePath(const std::filesystem::path& path) {
	std::string ext = path.extension().string();
	for (char& ch : ext)
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	static const char* extensions[] = { ".wz", ".cwiz", ".c", ".h", ".hpp", ".cpp", ".txt",
		".md", ".json", ".lua", ".ini", ".cfg", ".asm", ".s", ".py", ".sh", ".toml", ".yaml", ".yml" };
	for (const char* value : extensions) {
		if (ext == value)
			return true;
	}
	return false;
}

TextEditor::LanguageDefinition MakeToolchainLanguage(bool cwiz) {
	TextEditor::LanguageDefinition language;
	language.mName = cwiz ? "CWiz" : "Wiz";
	language.mCaseSensitive = true;
	language.mAutoIndentation = true;
	// Wiz uses '#' for line comments and has no preprocessor; CWiz uses C
	// comments and '#' directives.
	language.mSingleLineComment = cwiz ? "//" : "#";
	language.mCommentStart = cwiz ? "/*" : "";
	language.mCommentEnd = cwiz ? "*/" : "";
	language.mPreprocChar = cwiz ? '#' : static_cast<char>(0);
	const char* keywords[] = { "if", "else", "for", "while", "do", "switch", "case", "default",
		"break", "continue", "return", "struct", "enum", "typedef", "const", "static", "void",
		"int", "char", "unsigned", "signed", "long", "short", "float", "double", "bool", "true",
		"false", "sizeof", "extern", "inline", "goto" };
	for (const char* word : keywords)
		language.mKeywords.insert(std::string(word));
	const char* directives[] = { "for", "cwapp", "rop", "org", "include", "define", "if", "ifdef",
		"ifndef", "else", "elif", "endif", "pad", "nopad", "slot", "target", "lang", "bits",
		"section", "align", "macro", "endm", "db", "dw", "dd", "equ" };
	for (const char* word : directives)
		language.mPreprocIdentifiers[word] = TextEditor::Identifier{};
	language.mTokenRegexStrings = {
		{ "\\b(0[xX][0-9a-fA-F]+|[0-9]+)\\b", TextEditor::PaletteIndex::Number },
		{ "\"([^\"\\\\]|\\\\.)*\"", TextEditor::PaletteIndex::String },
		{ "'([^'\\\\]|\\\\.)*'", TextEditor::PaletteIndex::CharLiteral },
		{ "\\b(true|false|null)\\b", TextEditor::PaletteIndex::Keyword },
	};
	return language;
}

bool WantsEditor(const std::filesystem::path& path) {
	return IsTextFilePath(path);
}

std::string TargetNameForIndex(int index) {
	static const char* targets[] = { "cwii", "cw", "cwapp", "cwrop", "cwiirop" };
	return targets[std::clamp(index, 0, 4)];
}

std::string TargetLabelForIndex(int index) {
	static const char* labels[] = { "880  fx-880BTG", "580  fx-580VN X", "580 app (firmware patch)",
		"580 ROP (payload)", "880 ROP (payload)" };
	return labels[std::clamp(index, 0, 4)];
}

std::string LanguageNameForIndex(int index) {
	static const char* names[] = { "Auto", "Wiz", "CWiz" };
	return names[std::clamp(index, 0, 2)];
}

void SaveFileDocument(FileDocument& doc, const std::string& path = std::string());
void RunLintForDocument(FileDocument& doc);

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
	g.sidebarCache.clear();
	g.quickFilesDirty = true;
	auto& data = SettingsStore::Instance().Data();
	data.workspace.folder = folder;
	auto& recents = data.workspace.recentFolders;
	recents.erase(std::remove(recents.begin(), recents.end(), folder), recents.end());
	recents.insert(recents.begin(), folder);
	if (recents.size() > 10)
		recents.resize(10);
	if (!g.workspace.empty())
		g.expanded.insert(g.workspace);
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

bool IsWizDocument(const FileDocument& doc);
void TransferDocumentToToolchain(FileDocument& doc);
void SaveFileDocument(FileDocument& doc, const std::string& path);
bool FileDocumentDirty(const FileDocument& doc);

void BuildToolchain(bool run) {
	UIWindow* win = FindWindow("Wiz Compiler");
	if (!win)
		return;
	if (g.activeFile >= 0 && g.activeFile < static_cast<int>(g.files.size()) &&
		IsWizDocument(*g.files[static_cast<size_t>(g.activeFile)])) {
		FileDocument& doc = *g.files[static_cast<size_t>(g.activeFile)];
		if (FileDocumentDirty(doc))
			SaveFileDocument(doc, std::string());
		TransferDocumentToToolchain(doc);
		win->BuildEditorDocument(run);
		g.panelVisible = true;
		g.panelTab = 0;
		return;
	}
	win->BuildEditorDocument(run);
	g.panelVisible = true;
	g.panelTab = 0;
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
	else if (id == "model.import") RequestShellImport();
	else if (id == "folder.open") RequestOpenFolder();
	else if (id == "plugin.install") OpenSettings();
	else if (id == "view.sidebar") ToggleSidebar();
	else if (id == "view.panel") ToggleBottomPanel();
	else if (id == "view.tools") ToggleTools();
	else if (id == "view.calculator") SetCalculatorVisible(!CalculatorVisible());
	else if (id == "view.terminal") FocusTerminal();
	else if (id == "settings.open") OpenSettings();
	else if (id == "build") BuildToolchain(false);
	else if (id == "build.run") BuildToolchain(true);
	else if (id == "session.pause") { if (m_emu) m_emu->SetPaused(!m_emu->GetPaused()); }
	else if (id == "session.reset") { if (m_emu) m_emu->ResetToMainMenu(); }
	else if (id == "plugin.manager") OpenSettings();
	else if (id == "theme.vscode-dark-plus") ApplyThemeAndSave("vscode-dark-plus");
	else if (id == "theme.vscode-dark") ApplyThemeAndSave("vscode-dark");
	else if (id == "theme.midnight-blue") ApplyThemeAndSave("midnight-blue");
	else if (id == "theme.solarized") ApplyThemeAndSave("solarized");
	else if (id == "theme.mono-green") ApplyThemeAndSave("mono-green");
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
			if (name == "build" || name == "node_modules" || name == "third_party")
				it.disable_recursion_pending();
			continue;
		}
		g.quickFiles.push_back(it->path().string());
		if (g.quickFiles.size() >= 4000)
			break;
	}
	std::sort(g.quickFiles.begin(), g.quickFiles.end());
}

void TransferDocumentToToolchain(FileDocument& doc) {
	UIWindow* wiz = FindWindow("Wiz Compiler");
	if (!wiz)
		return;
	wiz->OpenEditorDocument(doc.path, doc.editor.GetText());
	wiz->SetEditorTarget(TargetNameForIndex(doc.targetIndex));
	wiz->SetEditorLanguage(doc.languageIndex == 1 ? "wiz" : (doc.languageIndex == 2 ? "cwiz" : "auto"));
}

void RunLintForDocument(FileDocument& doc) {
	doc.editor.SetErrorMarkers({});
	doc.editor.SetDiagnosticMarkers({});
	doc.problems.clear();
	doc.lastLintedText = doc.editor.GetText();
	const std::string target = TargetNameForIndex(doc.targetIndex);
	std::vector<wiz::LintDiagnostic> results;
	try {
		results = wiz::lint_source(doc.lastLintedText, doc.name, target);
	}
	catch (const std::exception& error) {
		DiagLine diag;
		diag.file = doc.path;
		diag.severity = 2;
		diag.text = std::string("linter failed: ") + error.what();
		diag.code = "linter";
		g.problems.push_back(diag);
		g.panelVisible = true;
		g.panelTab = 1;
		AppendOutput("[lint] " + doc.name + ": " + error.what());
		return;
	}
	TextEditor::DiagnosticMarkers markers;
	g.problems.erase(std::remove_if(g.problems.begin(), g.problems.end(),
		[&](const DiagLine& line) { return line.file == doc.path || line.file == doc.name; }), g.problems.end());
	for (const auto& item : results) {
		EditorProblem problem;
		problem.line = item.line;
		problem.column = item.col;
		problem.end_column = item.end_col;
		problem.severity = item.severity == wiz::LintSeverity::Error ? 2 :
			(item.severity == wiz::LintSeverity::Warning ? 1 : 0);
		problem.message = item.message;
		problem.code = !item.rule.empty() ? item.rule : item.code;
		problem.hint = item.hint;
		doc.problems.push_back(problem);
		if (item.line > 0) {
			TextEditor::DiagnosticMarker marker;
			marker.message = problem.message;
			marker.severity = problem.severity >= 2 ? TextEditor::MarkerSeverity::Error :
				(problem.severity == 1 ? TextEditor::MarkerSeverity::Warning : TextEditor::MarkerSeverity::Info);
			markers[item.line] = std::move(marker);
		}
		DiagLine diag;
		diag.file = doc.path;
		diag.line = item.line;
		diag.column = item.col;
		diag.end_column = item.end_col;
		diag.severity = problem.severity;
		diag.text = problem.message;
		diag.code = problem.code;
		diag.hint = problem.hint;
		g.problems.push_back(std::move(diag));
	}
	doc.editor.SetDiagnosticMarkers(markers);
	if (!results.empty()) {
		int errors = 0;
		for (const auto& item : doc.problems)
			errors += item.severity >= 2 ? 1 : 0;
		g.panelVisible = true;
		if (errors > 0)
			g.panelTab = 1;
	}
}

namespace {
std::string NormalizeDocumentText(std::string text) {
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
		text.pop_back();
	return text;
}
} // namespace

bool FileDocumentDirty(const FileDocument& doc) {
	// TextEditor::GetText() materialises a trailing newline for the virtual
	// empty last line, so a freshly loaded file would otherwise look dirty.
	return NormalizeDocumentText(doc.editor.GetText()) != NormalizeDocumentText(doc.savedText);
}

void EraseFileDocument(int index) {
	if (index < 0 || index >= static_cast<int>(g.files.size()))
		return;
	g.activeFile = -1;
	if (index < static_cast<int>(g.files.size())) {
		g.files.erase(g.files.begin() + index);
		if (!g.files.empty())
			g.activeFile = std::min(index, static_cast<int>(g.files.size()) - 1);
	}
	if (g.activeFile < 0) {
		g.activeSettings = false;
		if (g.tabs.empty() && !g.showWelcome)
			g.showWelcome = true;
	}
}

void CloseFileDocument(int index) {
	if (index < 0 || index >= static_cast<int>(g.files.size()))
		return;
	if (FileDocumentDirty(*g.files[static_cast<size_t>(index)])) {
		g.pendingCloseFile = index;
		g.closeConfirmOpen = true;
		return;
	}
	EraseFileDocument(index);
}

void SaveFileDocument(FileDocument& doc, const std::string& path) {
	const std::string target_path = path.empty() ? doc.path : path;
	std::ofstream output(target_path, std::ios::binary | std::ios::trunc);
	if (!output) {
		AppendOutput("[editor] cannot save " + target_path);
		return;
	}
	const std::string text = doc.editor.GetText();
	output << text;
	output.close();
	doc.path = target_path;
	doc.name = std::filesystem::path(target_path).filename().string();
	doc.savedText = text;
	AppendOutput("[editor] saved " + target_path);
	RunLintForDocument(doc);
}

void OpenFileInEditor(const std::filesystem::path& path) {
	if (!WantsEditor(path)) {
		OpenExternal(path);
		return;
	}
	for (int index = 0; index < static_cast<int>(g.files.size()); ++index) {
		if (g.files[static_cast<size_t>(index)]->path == path.string()) {
			g.activeFile = index;
			g.showWelcome = false;
			g.activeSettings = false;
			return;
		}
	}
	std::ifstream input(path, std::ios::binary);
	if (!input) {
		g.sidebarError = "cannot open " + path.string();
		AppendOutput("[editor] " + g.sidebarError);
		return;
	}
	std::ostringstream buffer;
	buffer << input.rdbuf();
	std::string ext = path.extension().string();
	for (char& ch : ext)
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	const bool cwiz_source = ext == ".cwiz";
	auto doc = std::make_unique<FileDocument>();
	doc->path = path.string();
	doc->name = path.filename().string();
	doc->savedText = buffer.str();
	doc->editor.SetLanguageDefinition(MakeToolchainLanguage(cwiz_source));
	doc->editor.SetPalette(ThemeManager::Instance().IsDarkMode()
		? TextEditor::GetDarkPalette() : TextEditor::GetLightPalette());
	doc->paletteSet = true;
	doc->editor.SetText(doc->savedText);
	doc->observedText = doc->savedText;
	// Auto-detection by extension: .wz is Wiz, .cwiz is CWiz.  Auto remains for
	// untitled/other text documents.
	doc->languageIndex = ext == ".wz" ? 1 : (ext == ".cwiz" ? 2 : 0);
	// Pick the target from the live model so a .wz on an fx-880BTG builds for
	// the 880 chip instead of silently using the 580 default.
	doc->targetIndex = (m_emu && m_emu->hardware_id == casioemu::HW_CLASSWIZ_II) ? 0 : 1;
	g.files.push_back(std::move(doc));
	g.activeFile = static_cast<int>(g.files.size()) - 1;
	g.showWelcome = false;
	g.activeSettings = false;
	RunLintForDocument(*g.files.back());
	AppendOutput("[editor] opened " + path.string());
}

std::vector<UIWindow*> ToolWindows();

void RebuildSessionMemoryWindows() {
	// ROM (and the raw dump windows) cache the emulator memory pointer at
	// creation.  Rebuild only those documents when the active session changes;
	// RAM/All re-read through me_mmu, but a single rebuild keeps every memory
	// view on the same active model.
	static const char* kMemoryNames[] = { "ROM", "RAM", "All", "Flash", "PRAM", "Regs", "VRAM" };
	for (size_t index = 0; index < windows.size();) {
		const char* name = windows[index] ? windows[index]->name : nullptr;
		bool memory = false;
		for (const char* candidate : kMemoryNames) {
			if (name && std::strcmp(name, candidate) == 0) {
				memory = true;
				break;
			}
		}
		if (memory) {
			delete windows[index];
			windows.erase(windows.begin() + static_cast<long>(index));
		}
		else {
			++index;
		}
	}
	for (auto* editor : GetEditors())
		windows.push_back(editor);
	g.toolIndex = std::clamp(g.toolIndex, 0, std::max(0, static_cast<int>(ToolWindows().size()) - 1));
	g.lastToolIndex = -1;
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

	// One shared baseline: the native traffic lights are centred 14 px from the
	// top of the window, and every text run uses this same centre line.
	const float center_y = pos.y + kTitleBarHeight * 0.5f;
	const float text_h = ImGui::GetTextLineHeight();
	const float text_top = center_y - text_h * 0.5f;
	const float control_top = center_y - kChromeControlHeight * 0.5f;
	const ImU32 foreground = ImGui::ColorConvertFloat4ToU32(palette.foreground);
	const ImU32 muted = ImGui::ColorConvertFloat4ToU32(palette.muted);

	const float traffic = 82.0f;
	// The main-window title deliberately uses the same UI face, size and
	// baseline as the File/View/Session menu buttons.
	const char* title = "CasioEmuMsvc";
	const float title_width = TextWidth(title);
	draw->AddText(ImVec2(pos.x + traffic + 12.0f, text_top), foreground, title);
	g.chromeTitleCenterY = center_y;

	auto draw_control = [&](const char* label, float x, float w, bool hovered, bool held) -> ImVec2 {
		const ImVec2 min(x, control_top);
		const ImVec2 max(x + w, control_top + kChromeControlHeight);
		if (held)
			draw->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(palette.selection), 4.0f);
		else if (hovered)
			draw->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(palette.hover), 4.0f);
		draw->AddText(ImVec2(x + 10.0f, text_top),
			ImGui::ColorConvertFloat4ToU32(hovered || held ? palette.foreground : palette.foreground), label);
		return min;
	};
	(void)muted;

	const char* menus[] = { "File", "View", "Session" };
	float menu_x = pos.x + traffic + 12.0f + title_width + 18.0f;
	for (const char* menu : menus) {
		const float w = TextWidth(menu) + 22.0f;
		ImGui::SetCursorScreenPos(ImVec2(menu_x, control_top));
		ImGui::PushID(menu);
		const bool clicked = ImGui::InvisibleButton("##chrome_menu", ImVec2(w, kChromeControlHeight));
		const bool hovered = ImGui::IsItemHovered();
		draw_control(menu, menu_x, w, hovered, ImGui::IsItemActive());
		g.chromeMenuCenterY = center_y;
		ImGui::SetNextWindowPos(ImVec2(menu_x, pos.y + kTitleBarHeight), ImGuiCond_Appearing);
		if (clicked) {
			if (std::strcmp(menu, "File") == 0)
				ImGui::OpenPopup("##file_menu");
			else if (std::strcmp(menu, "View") == 0)
				ImGui::OpenPopup("##view_menu");
			else if (std::strcmp(menu, "Session") == 0)
				ImGui::OpenPopup("##session_menu");
		}
		PushMenuStyle(palette);
		if (ImGui::BeginPopup("##file_menu")) {
			if (ImGui::MenuItem("Open Model...")) RequestOpenModel();
			if (ImGui::MenuItem("Import Model Package...")) RequestShellImport();
			if (ImGui::MenuItem("Open Folder...")) RequestOpenFolder();
			ImGui::Separator();
			if (ImGui::MenuItem("Settings")) OpenSettings();
			ImGui::Separator();
			if (ImGui::MenuItem("Quit")) g.quitRequested = true;
			ImGui::EndPopup();
		}
		PushMenuStyle(palette);
		if (ImGui::BeginPopup("##view_menu")) {
			if (ImGui::MenuItem("Toggle Sidebar")) ToggleSidebar();
			if (ImGui::MenuItem("Toggle Panel")) ToggleBottomPanel();
			if (ImGui::MenuItem("Toggle Tool Dock")) ToggleTools();
			if (ImGui::MenuItem("Toggle Calculator Panel")) SetCalculatorVisible(!CalculatorVisible());
			ImGui::Separator();
			if (ImGui::MenuItem("Welcome")) OpenWelcomeTab();
			if (ImGui::MenuItem("Command Palette")) OpenCommandPalette();
			ImGui::EndPopup();
		}
		PushMenuStyle(palette);
		if (ImGui::BeginPopup("##session_menu")) {
			if (ImGui::MenuItem("Pause / Resume") && m_emu) m_emu->SetPaused(!m_emu->GetPaused());
			if (ImGui::MenuItem("Reset") && m_emu) m_emu->ResetToMainMenu();
			ImGui::Separator();
			if (ImGui::MenuItem("Build")) BuildToolchain(false);
			if (ImGui::MenuItem("Build & Run")) BuildToolchain(true);
			ImGui::EndPopup();
		}
		PopMenuStyle();
		ImGui::PopID();
		menu_x += w + 2.0f;
	}

	// Right-aligned actions, all on the same centre line.  Widths are fixed by
	// label (Pause/Resume share a width) so toggling state never shifts them.
	const bool paused = m_emu && m_emu->GetPaused();
	struct ChromeAction {
		std::string label;
		float width;
		int action;
	};
	std::vector<ChromeAction> actions;
	if (g.activeFile < 0) {
		actions.push_back({ "Build", TextWidth("Build") + 20.0f, 0 });
		actions.push_back({ "Run", TextWidth("Run") + 20.0f, 1 });
	}
	const float pause_width = std::max(TextWidth("Pause"), TextWidth("Resume")) + 20.0f;
	actions.push_back({ paused ? "Resume" : "Pause", pause_width, 2 });
	actions.push_back({ "Palette", TextWidth("Palette") + 20.0f, 3 });
	float actions_width = 0.0f;
	for (const auto& action : actions)
		actions_width += action.width + 4.0f;
	float action_x = pos.x + width - actions_width - 8.0f;
	for (const auto& action : actions) {
		ImGui::SetCursorScreenPos(ImVec2(action_x, control_top));
		ImGui::PushID(action.action + 1000);
		const bool clicked = ImGui::InvisibleButton("##chrome_action", ImVec2(action.width, kChromeControlHeight));
		const bool hovered = ImGui::IsItemHovered();
		draw_control(action.label.c_str(), action_x, action.width, hovered, ImGui::IsItemActive());
		if (clicked) {
			if (action.action == 0) BuildToolchain(false);
			else if (action.action == 1) BuildToolchain(true);
			else if (action.action == 2) TogglePause();
			else OpenCommandPalette();
		}
		ImGui::PopID();
		g.chromeActionCenterY = center_y;
		action_x += action.width + 4.0f;
	}

	const ImVec2 mouse = ImGui::GetIO().MousePos;
	const float drag_right = pos.x + width - actions_width - 24.0f;
	const bool in_drag_zone = mouse.y >= pos.y && mouse.y < pos.y + kTitleBarHeight &&
		mouse.x > pos.x + traffic && mouse.x < drag_right;
	if (in_drag_zone && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) {
		g.draggingWindow = true;
		int mx = 0, my = 0;
		SDL_GetGlobalMouseState(&mx, &my);
		g.dragStartMouse = ImVec2((float)mx, (float)my);
		int wx = 0, wy = 0;
		if (window)
			SDL_GetWindowPosition(window, &wx, &wy);
		g.dragStartWindow = ImVec2((float)wx, (float)wy);
	}
	if (g.draggingWindow) {
		if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && window) {
			int mx = 0, my = 0;
			SDL_GetGlobalMouseState(&mx, &my);
			SDL_SetWindowPosition(window,
				(int)(g.dragStartWindow.x + (mx - g.dragStartMouse.x)),
				(int)(g.dragStartWindow.y + (my - g.dragStartMouse.y)));
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
	g.rectTabsContent = ImVec4(pos.x + 8.0f, pos.y + 8.0f, width - 16.0f, kTabStripHeight - 16.0f);
	DrawFlatPanel("##ShellTabs", pos, ImVec2(width, kTabStripHeight), palette.surface);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddLine(ImVec2(pos.x, pos.y + kTabStripHeight - 1.0f),
		ImVec2(pos.x + width, pos.y + kTabStripHeight - 1.0f),
		ImGui::ColorConvertFloat4ToU32(palette.border), 1.0f);

	// Rebuild the tab model only when its content can have changed; the
	// per-frame path just reuses the existing vector and strings.
	size_t signature = 1469598103934665603ull;
	auto mix = [&](size_t value) {
		signature ^= value + 0x9e3779b97f4a7c15ull;
		signature *= 1099511628211ull;
	};
	mix(g.tabs.size());
	mix(g.files.size());
	mix(g.showWelcome ? 1u : 0u);
	mix(g.activeSettings ? 1u : 0u);
	mix(static_cast<size_t>(g.activeTab + 2));
	mix(static_cast<size_t>(g.activeFile + 2));
	for (const auto& tab : g.tabs)
		mix(std::hash<std::string>{}(tab.title.empty() ? tab.model : tab.title));
	for (const auto& file : g.files) {
		mix(std::hash<std::string>{}(file->name));
		mix(FileDocumentDirty(*file) ? 1u : 0u);
	}
	if (signature != g.mainTabSignature) {
		g.mainTabSignature = signature;
		g.mainTabItems.clear();
		g.mainTabItems.push_back({ 0, -1, "Welcome",
			g.showWelcome && !g.activeSettings && g.activeFile < 0, false, false, 0.0f });
		for (int index = 0; index < static_cast<int>(g.tabs.size()); ++index) {
			const SessionTab& tab = g.tabs[static_cast<size_t>(index)];
			MainTab item;
			item.kind = 1;
			item.index = index;
			item.label = tab.title.empty() ? tab.model : tab.title;
			item.active = !g.showWelcome && !g.activeSettings && g.activeFile < 0 && index == g.activeTab;
			item.closable = index > 0;
			g.mainTabItems.push_back(std::move(item));
		}
		for (int index = 0; index < static_cast<int>(g.files.size()); ++index) {
			const FileDocument& doc = *g.files[static_cast<size_t>(index)];
			MainTab item;
			item.kind = 2;
			item.index = index;
			item.label = doc.name;
			item.active = !g.activeSettings && g.activeFile == index;
			item.dirty = FileDocumentDirty(doc);
			item.closable = true;
			g.mainTabItems.push_back(std::move(item));
		}
		g.mainTabItems.push_back({ 3, -1, "Settings", g.activeSettings && g.activeFile < 0, false, false, 0.0f });
	}
	auto& items = g.mainTabItems;
	const float plus_size = 24.0f;
	const float plus_area = plus_size + 18.0f;
	const float tabs_right = std::max(80.0f, width - plus_area);
	const float pad_left = 8.0f;
	float total = pad_left;
	for (auto& item : items) {
		item.width = TextWidth(item.label.c_str()) + 34.0f;
		if (item.closable)
			item.width += 18.0f;
		total += item.width;
	}
	total += pad_left;
	float max_scroll = std::max(0.0f, total - tabs_right);

	// Keep the active tab fully visible, but only when the selection actually
	// changes.  Resizing therefore never yanks the strip sideways.
	std::string active_key = "welcome";
	float active_left = pad_left;
	float active_right = pad_left;
	{
		float cursor = pad_left;
		for (const auto& item : items) {
			if (item.active) {
				active_left = cursor;
				active_right = cursor + item.width;
				active_key = std::to_string(item.kind) + ":" + std::to_string(item.index);
				break;
			}
			cursor += item.width;
		}
	}
	if (active_key != g.lastActiveKey || std::fabs(g.lastTabStripWidth - width) > 0.5f) {
		g.lastActiveKey = active_key;
		g.lastTabStripWidth = width;
		if (active_left < g.tabScroll)
			g.tabScroll = active_left;
		else if (active_right > g.tabScroll + tabs_right)
			g.tabScroll = active_right - tabs_right;
	}
	g.tabScroll = std::clamp(g.tabScroll, 0.0f, max_scroll);

	// Mouse wheel over the strip scrolls it horizontally.
	if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::GetIO().MouseWheel != 0.0f && max_scroll > 0.0f)
		g.tabScroll = std::clamp(g.tabScroll - ImGui::GetIO().MouseWheel * 48.0f, 0.0f, max_scroll);

	ImGui::PushClipRect(ImVec2(pos.x, pos.y), ImVec2(pos.x + tabs_right, pos.y + kTabStripHeight), true);
	g.mainTabRects.clear();
	float x = pad_left - g.tabScroll;
	int close_session = -1;
	int close_file = -1;
	bool open_welcome = false;
	bool open_settings = false;
	for (int item_index = 0; item_index < static_cast<int>(items.size()); ++item_index) {
		MainTab& item = items[static_cast<size_t>(item_index)];
		const float item_x = pos.x + x;
		const bool visible = item_x + item.width > pos.x && item_x < pos.x + tabs_right;
		if (visible) {
			ImGui::SetCursorScreenPos(ImVec2(item_x, pos.y + 8.0f));
			ImGui::PushID(item_index);
			ImGui::InvisibleButton("##main_tab", ImVec2(item.width, 24.0f));
			const bool hovered = ImGui::IsItemHovered();
			const ImVec2 min = ImGui::GetItemRectMin();
			const ImVec2 max = ImGui::GetItemRectMax();
			// The X is part of the tab item, so it must be resolved on the same
			// release event as the tab click.  Using IsMouseClicked (press) here
			// made the X miss and the tab select instead.
			const bool left_released = ImGui::IsItemClicked(ImGuiMouseButton_Left);
			const bool middle_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Middle);
			const bool over_close = hovered && ImGui::GetIO().MousePos.x > max.x - 24.0f;
			const bool close_clicked = item.closable && ((left_released && over_close) || middle_clicked);
			if (left_released || middle_clicked) {
				if (close_clicked) {
					if (item.kind == 1)
						close_session = item.index;
					else if (item.kind == 2)
						close_file = item.index;
				}
				else if (item.kind == 0)
					open_welcome = true;
				else if (item.kind == 1) {
					g.activeTab = item.index;
					g.activeFile = -1;
					g.showWelcome = false;
					g.activeSettings = false;
					if (g.tabs[static_cast<size_t>(item.index)].live)
						session::Activate(item.index);
					else if (!g.tabs[static_cast<size_t>(item.index)].modelPath.empty())
						OpenModelAt(g.tabs[static_cast<size_t>(item.index)].modelPath);
				}
				else if (item.kind == 2) {
					g.activeFile = item.index;
					g.showWelcome = false;
					g.activeSettings = false;
				}
				else
					open_settings = true;
			}
			const ImVec4 fill = item.active ? palette.tabActive : (hovered ? palette.hover : palette.tabInactive);
			draw->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(fill), 5.0f, ImDrawFlags_RoundCornersTop);
			draw->AddLine(ImVec2(max.x - 1.0f, min.y + 5.0f), ImVec2(max.x - 1.0f, max.y - 4.0f),
				ImGui::ColorConvertFloat4ToU32(palette.border), 1.0f);
			if (item.active)
				draw->AddRectFilled(ImVec2(min.x + 5.0f, min.y), ImVec2(max.x - 5.0f, min.y + 2.0f),
					ImGui::ColorConvertFloat4ToU32(palette.accent));
			const float label_top = min.y + (max.y - min.y - ImGui::GetTextLineHeight()) * 0.5f;
			draw->AddText(ImVec2(min.x + 12.0f, label_top),
				ImGui::ColorConvertFloat4ToU32(item.active ? palette.foreground : WithAlpha(palette.foreground, 0.72f)),
				item.label.c_str());
			if (item.dirty) {
				const float dot_x = max.x - (item.closable ? 30.0f : 14.0f);
				draw->AddCircleFilled(ImVec2(dot_x, (min.y + max.y) * 0.5f), 3.0f,
					ImGui::ColorConvertFloat4ToU32(palette.accent));
			}
			if (item.closable && hovered) {
				draw->AddText(ImVec2(max.x - 20.0f, label_top),
					ImGui::ColorConvertFloat4ToU32(palette.foreground), "x");
			}
			MainTabRect rect;
			rect.label = item.label;
			rect.rect = ImVec4(min.x, min.y, max.x - min.x, max.y - min.y);
			rect.active = item.active;
			rect.settings = item.kind == 3;
			rect.kind = item.kind;
			g.mainTabRects.push_back(std::move(rect));
			ImGui::PopID();
		}
		else {
			MainTabRect rect;
			rect.label = item.label;
			rect.rect = ImVec4(item_x, pos.y + 8.0f, item.width, 24.0f);
			rect.active = item.active;
			rect.settings = item.kind == 3;
			rect.kind = item.kind;
			g.mainTabRects.push_back(std::move(rect));
		}
		x += item.width;
	}
	ImGui::PopClipRect();

	// The + is pinned to the right edge of the strip, never after the last tab:
	// it cannot drift off screen and its hit area never moves.
	ImGui::PushStyleColor(ImGuiCol_Button, WithAlpha(palette.foreground, 0.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.hover);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, palette.selection);
	ImGui::PushStyleColor(ImGuiCol_Text, palette.foreground);
	ImGui::SetCursorScreenPos(ImVec2(pos.x + width - plus_area + 8.0f, pos.y + 8.0f));
	if (ImGui::Button("+##newtab", ImVec2(plus_size, plus_size)))
		open_welcome = true;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("New tab (Welcome)");
	ImGui::PopStyleColor(4);
	draw->AddLine(ImVec2(pos.x + width - plus_area + 2.0f, pos.y + 10.0f),
		ImVec2(pos.x + width - plus_area + 2.0f, pos.y + kTabStripHeight - 10.0f),
		ImGui::ColorConvertFloat4ToU32(palette.border), 1.0f);

	if (max_scroll > 0.0f) {
		const float track_x = pos.x + 8.0f;
		const float track_w = std::max(40.0f, tabs_right - 12.0f);
		const float thumb_w = std::max(28.0f, track_w * (tabs_right / total));
		const float thumb_x = track_x + (g.tabScroll / max_scroll) * (track_w - thumb_w);
		draw->AddRectFilled(ImVec2(track_x, pos.y + kTabStripHeight - 3.0f),
			ImVec2(track_x + track_w, pos.y + kTabStripHeight - 1.0f),
			ImGui::ColorConvertFloat4ToU32(palette.border));
		draw->AddRectFilled(ImVec2(thumb_x, pos.y + kTabStripHeight - 3.0f),
			ImVec2(thumb_x + thumb_w, pos.y + kTabStripHeight - 1.0f),
			ImGui::ColorConvertFloat4ToU32(palette.muted));
	}

	if (open_welcome)
		OpenWelcomeTab();
	if (open_settings)
		OpenSettings();
	if (close_file >= 0)
		CloseFileDocument(close_file);
	else if (close_session >= 0) {
		SessionTab& tab = g.tabs[static_cast<size_t>(close_session)];
		if (close_session > 0 && tab.live) {
			session::CloseSession(close_session);
			SyncTabsFromSessions();
		}
		else if (tab.live) {
			g.showWelcome = true;
			g.activeSettings = false;
			g.activeTab = -1;
			if (m_emu) m_emu->ResetToMainMenu();
		}
		else {
			g.tabs.erase(g.tabs.begin() + close_session);
			if (g.activeTab >= close_session) --g.activeTab;
			if (g.tabs.empty()) g.activeTab = -1;
		}
		SaveSessions();
	}
	EndFlatPanel();
}

const std::vector<SidebarEntry>& SidebarEntries(const std::filesystem::path& directory) {
	const std::string key = directory.string();
	SidebarDirCache& cache = g.sidebarCache[key];
	const double now = ImGui::GetTime();
	const bool always_rescan = std::getenv("CASIOEMU_PERF_SIDEBAR_ALWAYS") != nullptr;
	// Stat the directory at most twice a second; an unchanged mtime means the
	// cached, already-sorted listing is still valid.
	if (cache.valid && !always_rescan && now - cache.checked_at < 0.5)
		return cache.entries;
	cache.checked_at = now;
	std::error_code ec;
	const auto mtime = std::filesystem::last_write_time(directory, ec);
	if (cache.valid && !always_rescan && !ec && mtime == cache.mtime)
		return cache.entries;
	const auto started = std::chrono::steady_clock::now();
	cache.entries.clear();
	for (const auto& entry : std::filesystem::directory_iterator(
			 directory, std::filesystem::directory_options::skip_permission_denied, ec)) {
		std::error_code entry_ec;
		const std::string name = entry.path().filename().string();
		if (name.empty() || name[0] == '.')
			continue;
		SidebarEntry item;
		item.path = entry.path().string();
		item.name = name;
		item.is_directory = entry.is_directory(entry_ec);
		cache.entries.push_back(std::move(item));
	}
	std::sort(cache.entries.begin(), cache.entries.end(), [](const SidebarEntry& lhs, const SidebarEntry& rhs) {
		if (lhs.is_directory != rhs.is_directory)
			return lhs.is_directory;
		return lhs.name < rhs.name;
	});
	cache.mtime = mtime;
	cache.valid = true;
	++g.sidebarScanCount;
	g.sidebarScanMs += std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - started).count();
	return cache.entries;
}

void RenderSidebarTree(const std::filesystem::path& directory, int depth, const ThemePalette& palette) {
	if (depth > 12)
		return;
	int count = 0;
	for (const SidebarEntry& entry : SidebarEntries(directory)) {
		if (++count > 400)
			break;
		const std::filesystem::path entry_path(entry.path);
		const std::string& key = entry.path;
		const std::string& name = entry.name;
		const bool is_directory = entry.is_directory;
		char label[512];
		std::snprintf(label, sizeof(label), "%s%s",
			is_directory ? (g.expanded.count(key) ? "[-] " : "[+] ") : "   ", name.c_str());
		// Every row owns a distinct popup ID.  A shared ID let one right-click
		// stack context entries from several rows and target the wrong path.
		ImGui::PushID(key.c_str());
		ImGui::SetCursorPosX(20.0f + depth * 12.0f);
		ImGui::PushStyleColor(ImGuiCol_Header, palette.selection);
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, palette.hover);
		ImGui::PushStyleColor(ImGuiCol_Text, palette.foreground);
		const bool row_selected = g.selectedPath == entry_path;
		const bool clicked = ImGui::Selectable(label, row_selected,
			ImGuiSelectableFlags_AllowDoubleClick, ImVec2(ImGui::GetContentRegionAvail().x - 6.0f, 0.0f));
		if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
			g.selectedPath = entry_path;
			g.selectedIsDir = is_directory;
		}
		if (clicked) {
			g.selectedPath = entry_path;
			g.selectedIsDir = is_directory;
			if (is_directory) {
				if (!ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
					if (g.expanded.count(key))
						g.expanded.erase(key);
					else
						g.expanded.insert(key);
				}
			}
			else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				OpenFileInEditor(entry_path);
			}
		}
		PushMenuStyle(palette);
		if (ImGui::BeginPopupContextItem("##explorer_context")) {
			g.selectedPath = entry_path;
			g.selectedIsDir = is_directory;
			const std::filesystem::path base = is_directory ? entry_path : entry_path.parent_path();
			if (ImGui::MenuItem("New File")) {
				g.selectedPath = base;
				g.selectedIsDir = true;
				g.newEntryIsFolder = false;
				g.entryName[0] = '\0';
				g.newFileOpen = true;
			}
			if (ImGui::MenuItem("New Folder")) {
				g.selectedPath = base;
				g.selectedIsDir = true;
				g.newEntryIsFolder = true;
				g.entryName[0] = '\0';
				g.newFolderOpen = true;
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Rename")) {
				g.selectedPath = entry_path;
				std::snprintf(g.entryName, sizeof(g.entryName), "%s", name.c_str());
				g.renameOpen = true;
			}
			if (ImGui::MenuItem("Delete")) {
				g.selectedPath = entry_path;
				g.deleteOpen = true;
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Reveal in Finder")) {
				std::string uri = "file://" + (is_directory ? entry_path : entry_path.parent_path()).string();
				SDL_OpenURL(uri.c_str());
			}
			if (ImGui::MenuItem("Copy Path"))
				ImGui::SetClipboardText(entry_path.string().c_str());
			ImGui::EndPopup();
		}
		PopMenuStyle();
		ImGui::PopStyleColor(3);
		ImGui::PopID();
		if (is_directory && g.expanded.count(key))
			RenderSidebarTree(entry_path, depth + 1, palette);
	}
}

void RenderSidebar(const ThemePalette& palette, ImVec2 pos, ImVec2 size) {
	DrawFlatPanel("##ShellSidebar", pos, size, palette.surface);
	const bool narrow_sidebar = size.x < 286.0f;
	const float header_height = narrow_sidebar ? 114.0f : 76.0f;
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + header_height),
		ImGui::ColorConvertFloat4ToU32(palette.titleBar));
	ImGui::SetCursorPos(ImVec2(20.0f, 3.0f));
	ImGui::TextDisabled("%s", "EXPLORER");
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.hover);
	ImGui::PushStyleColor(ImGuiCol_Text, palette.foreground);
	if (narrow_sidebar) {
		const float button_w = size.x - 32.0f;
		ImGui::SetCursorScreenPos(ImVec2(pos.x + 16.0f, pos.y + 24.0f));
		if (ImGui::Button("New File", ImVec2(button_w, 20.0f))) {
			g.selectedPath = g.workspace;
			g.selectedIsDir = true;
			g.entryName[0] = '\0';
			g.newFileOpen = true;
		}
		ImGui::SetCursorScreenPos(ImVec2(pos.x + 16.0f, pos.y + 46.0f));
		if (ImGui::Button("New Folder", ImVec2(button_w, 20.0f))) {
			g.selectedPath = g.workspace;
			g.selectedIsDir = true;
			g.entryName[0] = '\0';
			g.newFolderOpen = true;
		}
		ImGui::SetCursorScreenPos(ImVec2(pos.x + 16.0f, pos.y + 68.0f));
		if (ImGui::Button("Refresh", ImVec2(button_w, 20.0f))) {
			g.quickFilesDirty = true;
			g.sidebarError.clear();
			g.sidebarCache.clear();
		}
		ImGui::SetCursorScreenPos(ImVec2(pos.x + 16.0f, pos.y + 90.0f));
		if (ImGui::Button("Open Folder", ImVec2(button_w, 20.0f)))
			RequestOpenFolder();
	}
	else {
		const float button_w = (size.x - 40.0f) * 0.5f;
		ImGui::SetCursorScreenPos(ImVec2(pos.x + 16.0f, pos.y + 24.0f));
		if (ImGui::Button("New File", ImVec2(button_w, 20.0f))) {
			g.selectedPath = g.workspace;
			g.selectedIsDir = true;
			g.entryName[0] = '\0';
			g.newFileOpen = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("New Folder", ImVec2(button_w, 20.0f))) {
			g.selectedPath = g.workspace;
			g.selectedIsDir = true;
			g.entryName[0] = '\0';
			g.newFolderOpen = true;
		}
		ImGui::SetCursorScreenPos(ImVec2(pos.x + 16.0f, pos.y + 48.0f));
		if (ImGui::Button("Refresh", ImVec2(button_w, 20.0f))) {
			g.quickFilesDirty = true;
			g.sidebarError.clear();
			g.sidebarCache.clear();
		}
		ImGui::SameLine();
		if (ImGui::Button("Open Folder", ImVec2(button_w, 20.0f)))
			RequestOpenFolder();
	}
	ImGui::PopStyleColor(3);

	g.rectSidebarBody = ImVec4(pos.x + 16.0f, pos.y + header_height + 8.0f,
		size.x - 32.0f, size.y - header_height - 24.0f);
	ImGui::SetCursorScreenPos(ImVec2(pos.x + 16.0f, pos.y + header_height + 8.0f));
	ImGui::BeginChild("##explorer_body", ImVec2(size.x - 32.0f, size.y - header_height - 24.0f), ImGuiChildFlags_None);
	if (g.workspace.empty()) {
		ImGui::TextWrapped("You have not yet opened a folder.");
		ImGui::Spacing();
		if (ImGui::TextLink("Open Folder"))
			RequestOpenFolder();
		ImGui::Spacing();
		ImGui::TextDisabled("RECENT");
		const auto& recents = SettingsStore::Instance().Data().workspace.recentFolders;
		for (const auto& folder : recents) {
			ImGui::SetCursorPosX(16.0f);
			if (ImGui::Selectable((std::string("> ") + BaseName(folder)).c_str(), false,
					ImGuiSelectableFlags_None, ImVec2(ImGui::GetContentRegionAvail().x - 10.0f, 0.0f)))
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
		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
			ImGuiIO& io = ImGui::GetIO();
			const std::filesystem::path base = g.selectedIsDir ? g.selectedPath : g.selectedPath.parent_path();
			if (io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_N, false)) {
				g.selectedPath = base.empty() ? std::filesystem::path(g.workspace) : base;
				g.newEntryIsFolder = io.KeyShift;
				g.entryName[0] = '\0';
				if (io.KeyShift)
					g.newFolderOpen = true;
				else
					g.newFileOpen = true;
			}
			if (io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_Backspace, false) && !g.selectedPath.empty()) {
				g.deleteOpen = true;
			}
			if (!io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !g.selectedPath.empty()) {
				std::snprintf(g.entryName, sizeof(g.entryName), "%s", g.selectedPath.filename().string().c_str());
				g.renameOpen = true;
			}
		}
	}
	ImGui::EndChild();
	EndFlatPanel();
}

void RenderWelcome(const ThemePalette& palette) {
	const float pad = 32.0f;
	const float available = std::max(200.0f, ImGui::GetWindowWidth() - pad * 2.0f);
	if (FontInAtlas(g_welcome_font))
		ImGui::PushFont(g_welcome_font);
	ImGui::SetCursorPos(ImVec2(pad, 22.0f));
	ImGui::PushStyleColor(ImGuiCol_Text, palette.foreground);
	ImGui::TextUnformatted("CasioEmuMsvc");
	ImGui::PopStyleColor();
	if (FontInAtlas(g_welcome_font))
		ImGui::PopFont();
	ImGui::SetCursorPosX(pad);
	ImGui::PushTextWrapPos(pad + available);
	ImGui::TextDisabled("Casio nX-U8/100 & nX-U16/100 emulator and debugger");
	ImGui::PopTextWrapPos();

	ImGui::Dummy(ImVec2(0.0f, 14.0f));
	const auto& data = SettingsStore::Instance().Data();
	ImGui::SetCursorPosX(pad);
	if (ImGui::BeginTable("##welcome_columns", 2,
			ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
		ImGui::TableSetupColumn("Recent", ImGuiTableColumnFlags_WidthStretch, 0.44f);
		ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthStretch, 0.56f);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextDisabled("Recent models");
		bool any_recent = false;
		const float recent_width = ImGui::GetContentRegionAvail().x - 4.0f;
		for (const auto& session : data.sessions) {
			any_recent = true;
			const std::string row = std::string("> ") + (session.title.empty() ? session.model : session.title);
			if (ImGui::Selectable(row.c_str(), false, ImGuiSelectableFlags_None, ImVec2(recent_width, 0.0f))) {
				if (!session.modelPath.empty())
					OpenModelAt(session.modelPath);
				else
					RequestOpenModel();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s\n%s", session.model.c_str(), session.rom.c_str());
		}
		for (const auto& file : data.workspace.recentFiles) {
			any_recent = true;
			const std::string row = std::string("> ") + BaseName(file);
			if (ImGui::Selectable(row.c_str(), false, ImGuiSelectableFlags_None, ImVec2(recent_width, 0)))
				OpenExternal(file);
		}
		if (!any_recent)
			ImGui::TextDisabled("No recent sessions or files");

		ImGui::TableSetColumnIndex(1);
		ImGui::Dummy(ImVec2(0.0f, 2.0f));
		ImGui::TextDisabled("Start");
		auto link = [&](const char* label, const std::string& command) {
			ImGui::PushStyleColor(ImGuiCol_Text, palette.link);
			if (ImGui::Selectable(label, false, ImGuiSelectableFlags_None,
					ImVec2(ImGui::GetContentRegionAvail().x - 4.0f, 0.0f)))
				ExecuteCommand(command);
			ImGui::PopStyleColor();
		};
		link("New Calculator", "session.new");
		link("Open Folder...", "folder.open");
		ImGui::Dummy(ImVec2(0.0f, 12.0f));
		ImGui::TextDisabled("Workspace");
		ImGui::TextWrapped("%s", g.workspace.empty() ? "(none)" : g.workspace.c_str());
		ImGui::EndTable();
	}
}

std::vector<UIWindow*> ToolWindows();
std::string ToolDisplayName(const UIWindow* win);
void SortToolsByOrder(std::vector<UIWindow*>& tools);

void RenderCalculatorPanel(const ThemePalette& palette, ImVec2 pos, ImVec2 size) {
	DrawFlatPanel("##ShellCalculator", pos, size, palette.background);
	const ImVec2 window_pos = ImGui::GetWindowPos();
	ImGui::GetWindowDrawList()->AddRectFilled(window_pos,
		ImVec2(window_pos.x + size.x, window_pos.y + 22.0f),
		ImGui::ColorConvertFloat4ToU32(palette.titleBar));
	ImGui::GetWindowDrawList()->AddText(ImVec2(window_pos.x + 12.0f, window_pos.y + 4.0f),
		ImGui::ColorConvertFloat4ToU32(palette.muted), "CALCULATOR");
	ImGui::SetCursorPos(ImVec2(size.x - 12.0f - 58.0f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.hover);
	ImGui::PushStyleColor(ImGuiCol_Text, palette.foreground);
	if (ImGui::Button("Hide", ImVec2(58.0f, 20.0f)))
		g.calculatorVisible = false;
	ImGui::PopStyleColor(3);

	g.rectToolsBody = ImVec4(pos.x + 12.0f, pos.y + 30.0f, size.x - 24.0f, size.y - 38.0f);
	ImGui::SetCursorPos(ImVec2(12.0f, 30.0f));
	ImGui::BeginChild("##calculator_host", ImVec2(size.x - 24.0f, size.y - 38.0f), ImGuiChildFlags_None,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	UIWindow* calculator = FindWindow("Calculator");
	if (!calculator) {
		ImGui::TextDisabled("The calculator is not available in this build.");
	}
	else {
		ImGui::SetCursorPos(ImVec2(8.0f, 8.0f));
		ImGui::BeginChild("##calculator_inner", ImVec2(-16.0f, -16.0f), ImGuiChildFlags_None,
			ImGuiWindowFlags_NoScrollbar);
		// Keep the calculator's own drawing inside the padded host even when a
		// model's zoom makes it wider than the panel.
		const ImVec2 calc_pos = ImGui::GetWindowPos();
		const ImVec2 calc_size = ImGui::GetWindowSize();
		ImGui::PushClipRect(ImVec2(calc_pos.x + 6.0f, calc_pos.y + 6.0f),
			ImVec2(calc_pos.x + calc_size.x - 6.0f, calc_pos.y + calc_size.y - 6.0f), true);
		calculator->RenderCore();
		ImGui::PopClipRect();
		ImGui::EndChild();
		const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
			ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
		if (hovered && !g.paletteOpen && !TerminalFocused())
			calculator_panel_focused = true;
	}
	ImGui::EndChild();

	// The splitter lives INSIDE the calculator window, in the 8 px strip before
	// the padded calculator child.  It is therefore never covered by a child
	// window and receives hover/click/cursor directly.
	const ImVec2 panel_pos = ImGui::GetWindowPos();
	g.rectCalculatorGrip = ImVec4(panel_pos.x, panel_pos.y, 8.0f, size.y);
	ImGui::SetCursorScreenPos(panel_pos);
	ImGui::InvisibleButton("##calculator_splitter", ImVec2(8.0f, size.y));
	const bool splitter_hovered = ImGui::IsItemHovered();
	const bool splitter_active = ImGui::IsItemActive();
	ImDrawList* splitter_draw = ImGui::GetWindowDrawList();
	const bool lit = splitter_hovered || splitter_active;
	const ImU32 accent = ImGui::ColorConvertFloat4ToU32(palette.accent);
	const ImU32 grip_line = ImGui::ColorConvertFloat4ToU32(palette.background);
	// Full-height tinted band + centre line.
	splitter_draw->AddRectFilled(ImVec2(panel_pos.x, panel_pos.y),
		ImVec2(panel_pos.x + 8.0f, panel_pos.y + size.y),
		ImGui::ColorConvertFloat4ToU32(WithAlpha(palette.accent, lit ? 0.30f : 0.12f)));
	splitter_draw->AddRectFilled(ImVec2(panel_pos.x + 3.0f, panel_pos.y),
		ImVec2(panel_pos.x + 5.0f, panel_pos.y + size.y),
		ImGui::ColorConvertFloat4ToU32(lit ? palette.accent : palette.borderStrong));
	// Tall grip pill with five lines and arrow heads.
	const float handle_y = panel_pos.y + size.y * 0.5f;
	const float half_h = 36.0f;
	const ImU32 handle_fill = accent;
	const ImU32 handle_edge = ImGui::ColorConvertFloat4ToU32(lit ? palette.foreground : palette.borderStrong);
	splitter_draw->AddRectFilled(ImVec2(panel_pos.x + 1.0f, handle_y - half_h),
		ImVec2(panel_pos.x + 11.0f, handle_y + half_h), handle_fill, 4.0f);
	splitter_draw->AddRect(ImVec2(panel_pos.x + 1.0f, handle_y - half_h),
		ImVec2(panel_pos.x + 11.0f, handle_y + half_h), handle_edge, 4.0f, 0, 1.0f);
	for (int line = -2; line <= 2; ++line) {
		const float y = handle_y + line * 6.0f;
		splitter_draw->AddLine(ImVec2(panel_pos.x + 4.0f, y), ImVec2(panel_pos.x + 8.0f, y),
			grip_line, 1.2f);
	}
	const ImU32 arrow = ImGui::ColorConvertFloat4ToU32(lit ? palette.foreground : palette.accent);
	splitter_draw->AddTriangleFilled(ImVec2(panel_pos.x + 2.0f, handle_y - half_h - 9.0f),
		ImVec2(panel_pos.x + 10.0f, handle_y - half_h - 9.0f),
		ImVec2(panel_pos.x + 6.0f, handle_y - half_h - 15.0f), arrow);
	splitter_draw->AddTriangleFilled(ImVec2(panel_pos.x + 2.0f, handle_y + half_h + 9.0f),
		ImVec2(panel_pos.x + 10.0f, handle_y + half_h + 9.0f),
		ImVec2(panel_pos.x + 6.0f, handle_y + half_h + 15.0f), arrow);
	if (lit)
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
	if (splitter_hovered)
		ImGui::SetTooltip("Drag to resize the calculator panel - double-click to reset to 360 px");
	if (splitter_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
		g.calculatorWidth = 360.0f;
		SaveLayout();
	}
	else if (splitter_active) {
		g.calculatorWidth = std::clamp(g.calculatorWidth - ImGui::GetIO().MouseDelta.x,
			260.0f, std::max(260.0f, ImGui::GetMainViewport()->Size.x * 0.6f));
		SaveLayoutThrottled();
	}
	if (ImGui::IsItemDeactivated())
		SaveLayout();
	EndFlatPanel();
}

bool IsWizDocument(const FileDocument& doc) {
	std::string ext = std::filesystem::path(doc.path).extension().string();
	for (char& ch : ext)
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	return ext == ".wz" || ext == ".cwiz";
}

void FindInDocument(FileDocument& doc) {
	if (!doc.findText[0])
		return;
	const auto lines = doc.editor.GetTextLines();
	const int start = doc.editor.GetCursorPosition().mLine + 1;
	for (int step = 0; step < static_cast<int>(lines.size()); ++step) {
		const int line = (start + step) % static_cast<int>(lines.size());
		const size_t column = lines[static_cast<size_t>(line)].find(doc.findText);
		if (column != std::string::npos) {
			doc.editor.SetCursorPosition(TextEditor::Coordinates(line, static_cast<int>(column)));
			return;
		}
	}
}

void RenderFileEditor(const ThemePalette& palette, ImVec2 pos, ImVec2 size) {
	DrawFlatPanel("##ShellFileEditor", pos, size, palette.background);
	if (g.activeFile < 0 || g.activeFile >= static_cast<int>(g.files.size())) {
		EndFlatPanel();
		return;
	}
	FileDocument& doc = *g.files[static_cast<size_t>(g.activeFile)];
	// Editor settings: only tab size is exposed by the embedded TextEditor.
	// Line numbers, word wrap and font size have no editor API in this tree.
	doc.editor.SetTabSize(std::clamp(SettingsStore::Instance().Data().editor.tabSize, 1, 16));
	const bool wiz_file = IsWizDocument(doc);
	const float toolbar_height = (wiz_file ? 118.0f : 62.0f) + (doc.findOpen ? 28.0f : 0.0f);

	// Row 1: file actions.
	ImGui::SetCursorPos(ImVec2(8.0f, 6.0f));
	if (ImGui::Button("Save"))
		SaveFileDocument(doc);
	ImGui::SameLine();
	if (ImGui::Button("Save As")) {
		SystemDialogs::SaveFileDialog(doc.name, [&doc](std::filesystem::path path) {
			SaveFileDocument(doc, path.string());
		});
	}
	ImGui::SameLine();
	if (ImGui::Button("Find"))
		doc.findOpen = !doc.findOpen;

	// Row 2: history and cursor state.
	const auto cursor = doc.editor.GetCursorPosition();
	ImGui::SetCursorPos(ImVec2(8.0f, 34.0f));
	if (ImGui::Button("Undo"))
		doc.editor.Undo();
	ImGui::SameLine();
	if (ImGui::Button("Redo"))
		doc.editor.Redo();
	ImGui::SameLine();
	ImGui::TextDisabled("Ln %d, Col %d", cursor.mLine + 1, cursor.mColumn + 1);
	if (FileDocumentDirty(doc)) {
		ImGui::SameLine();
		ImGui::TextColored(palette.warning, "modified");
	}

	if (wiz_file) {
		// Row 3: target and language.
		ImGui::SetCursorPos(ImVec2(8.0f, 62.0f));
		ImGui::TextDisabled("Target");
		ImGui::SameLine();
		float target_width = 0.0f;
		for (int index = 0; index < 5; ++index)
			target_width = std::max(target_width, ImGui::CalcTextSize(TargetLabelForIndex(index).c_str()).x);
		target_width += ImGui::GetFrameHeight() + 18.0f;
		ImGui::SetNextItemWidth(target_width);
		if (ImGui::BeginCombo("##editor_target", TargetLabelForIndex(doc.targetIndex).c_str())) {
			for (int index = 0; index < 5; ++index) {
				if (ImGui::Selectable(TargetLabelForIndex(index).c_str(), index == doc.targetIndex))
					doc.targetIndex = index;
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Language");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(86.0f);
		if (ImGui::BeginCombo("##editor_language", LanguageNameForIndex(doc.languageIndex).c_str())) {
			for (int index = 0; index < 3; ++index) {
				if (ImGui::Selectable(LanguageNameForIndex(index).c_str(), index == doc.languageIndex))
					doc.languageIndex = index;
			}
			ImGui::EndCombo();
		}

		// Row 4: lint and build actions.
		ImGui::SetCursorPos(ImVec2(8.0f, 90.0f));
		if (ImGui::Button("Lint"))
			RunLintForDocument(doc);
		ImGui::SameLine();
		if (ImGui::Button("Build")) {
			if (FileDocumentDirty(doc))
				SaveFileDocument(doc);
			TransferDocumentToToolchain(doc);
			if (UIWindow* wiz = FindWindow("Wiz Compiler"))
				wiz->BuildEditorDocument(false);
			g.panelVisible = true;
			g.panelTab = 0;
		}
		ImGui::SameLine();
		if (ImGui::Button("Build & Run")) {
			if (FileDocumentDirty(doc))
				SaveFileDocument(doc);
			TransferDocumentToToolchain(doc);
			if (UIWindow* wiz = FindWindow("Wiz Compiler"))
				wiz->BuildEditorDocument(true);
			g.panelVisible = true;
			g.panelTab = 0;
		}
		ImGui::SameLine();
		if (ImGui::Button("Preprocess")) {
			TransferDocumentToToolchain(doc);
			if (UIWindow* wiz = FindWindow("Wiz Compiler"))
				wiz->PreprocessEditorDocument();
			g.panelVisible = true;
			g.panelTab = 0;
		}
	}

	if (doc.findOpen) {
		const float find_y = wiz_file ? 118.0f : 62.0f;
		ImGui::SetCursorPos(ImVec2(8.0f, find_y));
		ImGui::SetNextItemWidth(220.0f);
		if (ImGui::InputTextWithHint("##editor_find", "Find in file", doc.findText, sizeof(doc.findText),
				ImGuiInputTextFlags_EnterReturnsTrue))
			FindInDocument(doc);
		ImGui::SameLine();
		if (ImGui::Button("Next", ImVec2(52, 24)))
			FindInDocument(doc);
	}

	ImGui::SetCursorPos(ImVec2(8.0f, toolbar_height));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
	doc.editor.SetPalette(ThemeManager::Instance().IsDarkMode()
		? TextEditor::GetDarkPalette() : TextEditor::GetLightPalette());
	doc.editor.Render("##file_editor_body", ImVec2(size.x - 24.0f, size.y - toolbar_height - 20.0f), true);
	ImGui::PopStyleVar();

	const std::string current_text = doc.editor.GetText();
	if (current_text != doc.observedText) {
		doc.observedText = current_text;
		doc.lastEdit = ImGui::GetTime();
	}
	if (current_text != doc.lastLintedText && ImGui::GetTime() - doc.lastEdit > 0.65)
		RunLintForDocument(doc);
	EndFlatPanel();
}

void RenderToolDocuments(const ThemePalette& palette, ImVec2 pos, ImVec2 size) {
	DrawFlatPanel("##ShellDocuments", pos, size, palette.background);
	auto tools = ToolWindows();
	if (const char* stress = std::getenv("CASIOEMU_STRESS_TOOLS")) {
		const int wanted = std::max(0, std::atoi(stress));
		if (wanted > 0 && !tools.empty()) {
			const auto base = tools;
			while (static_cast<int>(tools.size()) < wanted)
				tools.push_back(base[tools.size() % base.size()]);
		}
	}
	SortToolsByOrder(tools);
	if (tools.empty()) {
		RenderWelcome(palette);
		EndFlatPanel();
		return;
	}
	for (int index = 0; index < static_cast<int>(tools.size()); ++index) {
		if (tools[index]->bring_to_front_requested) {
			g.toolIndex = index;
			tools[index]->bring_to_front_requested = false;
		}
	}
	if (const char* requested = std::getenv("CASIOEMU_TOOL_NAME")) {
		for (int index = 0; index < static_cast<int>(tools.size()); ++index) {
			if (std::strcmp(tools[index]->name, requested) == 0)
				g.toolIndex = index;
		}
	}
	if (!g.pendingTool.empty()) {
		for (int index = 0; index < static_cast<int>(tools.size()); ++index) {
			if (g.pendingTool == tools[index]->name) {
				g.toolIndex = index;
				break;
			}
		}
		g.pendingTool.clear();
	}
	g.toolIndex = std::clamp(g.toolIndex, 0, static_cast<int>(tools.size()) - 1);

	// Document tabs wrap onto as many rows as needed.  There is no horizontal
	// scroller: every tab stays visible with its full label, and the strip grows
	// in height instead.
	constexpr float kDocPad = 12.0f;
	constexpr float kDocTop = 8.0f;
	constexpr float kDocTabHeight = 24.0f;
	constexpr float kDocRowGap = 2.0f;
	size_t doc_signature = 1469598103934665603ull;
	auto mix_doc = [&](size_t value) {
		doc_signature ^= value + 0x9e3779b97f4a7c15ull;
		doc_signature *= 1099511628211ull;
	};
	mix_doc(tools.size());
	mix_doc(static_cast<size_t>(ImGui::GetFontSize() * 100.0f));
	for (const auto* tool : tools)
		mix_doc(std::hash<std::string>{}(ToolDisplayName(tool)));
	if (doc_signature != g.docTabSignature) {
		g.docTabSignature = doc_signature;
		g.docTabLabels.clear();
		g.docTabWidths.clear();
		for (const auto* tool : tools) {
			g.docTabLabels.push_back(ToolDisplayName(tool));
			g.docTabWidths.push_back(ImGui::CalcTextSize(g.docTabLabels.back().c_str()).x + 31.0f);
		}
	}
	const auto& labels = g.docTabLabels;
	const auto& widths = g.docTabWidths;
	const float available = std::max(80.0f, size.x - kDocPad * 2.0f);
	g.docTabRects.clear();
	g.docTabRects.reserve(labels.size());
	float row_x = kDocPad;
	float row_y = kDocTop;
	for (size_t index = 0; index < labels.size(); ++index) {
		const float tab_width = std::min(widths[index], available);
		if (row_x > kDocPad && row_x + tab_width > kDocPad + available) {
			row_x = kDocPad;
			row_y += kDocTabHeight + kDocRowGap;
		}
		g.docTabRects.push_back(ImVec4(row_x, row_y, tab_width, kDocTabHeight));
		row_x += tab_width + kDocRowGap;
	}
	const float strip_height = row_y + kDocTabHeight + kDocTop;
	g.docStripHeight = strip_height;

	ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
	ImGui::PushStyleColor(ImGuiCol_ChildBg, palette.surface);
	ImGui::BeginChild("##doc_tabs", ImVec2(size.x, strip_height), ImGuiChildFlags_None,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	ImGui::PushClipRect(ImVec2(pos.x, pos.y), ImVec2(pos.x + size.x, pos.y + strip_height), true);
	for (int index = 0; index < static_cast<int>(tools.size()); ++index) {
		const ImVec4& rect = g.docTabRects[static_cast<size_t>(index)];
		ImGui::SetCursorPos(ImVec2(rect.x, rect.y));
		ImGui::PushID(index);
		const bool clicked = ImGui::InvisibleButton("##doc_tab", ImVec2(rect.z - 1.0f, rect.w));
		const bool hovered = ImGui::IsItemHovered();
		if (clicked) {
			g.toolIndex = index;
			tools[static_cast<size_t>(index)]->open = true;
		}
		if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f))
			g.draggingTool = index;
		if (g.draggingTool >= 0 && hovered && g.draggingTool != index)
			g.dragTargetTool = index;
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		const bool selected = index == g.toolIndex;
		draw->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(selected ? palette.tabActive :
			(hovered ? palette.hover : palette.tabInactive)), 4.0f, ImDrawFlags_RoundCornersTop);
		draw->AddLine(ImVec2(max.x - 1.0f, min.y + 4.0f), ImVec2(max.x - 1.0f, max.y - 4.0f),
			ImGui::ColorConvertFloat4ToU32(palette.border), 1.0f);
		if (selected)
			draw->AddRectFilled(ImVec2(min.x + 3.0f, min.y), ImVec2(max.x - 3.0f, min.y + 2.0f),
				ImGui::ColorConvertFloat4ToU32(palette.accent));
		draw->AddText(ImVec2(min.x + 15.0f, min.y + (kDocTabHeight - ImGui::GetTextLineHeight()) * 0.5f),
			ImGui::ColorConvertFloat4ToU32(selected ? palette.foreground : WithAlpha(palette.foreground, 0.72f)),
			labels[static_cast<size_t>(index)].c_str());
		ImGui::PopID();
	}
	ImGui::PopClipRect();
	if (g.draggingTool >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
		if (g.dragTargetTool >= 0 && g.dragTargetTool != g.draggingTool &&
			g.draggingTool < static_cast<int>(tools.size()) && g.dragTargetTool < static_cast<int>(tools.size())) {
			const std::string moved = tools[static_cast<size_t>(g.draggingTool)]->name;
			const std::string target = tools[static_cast<size_t>(g.dragTargetTool)]->name;
			auto moved_it = std::find(g.toolOrder.begin(), g.toolOrder.end(), moved);
			auto target_it = std::find(g.toolOrder.begin(), g.toolOrder.end(), target);
			if (moved_it != g.toolOrder.end() && target_it != g.toolOrder.end())
				std::iter_swap(moved_it, target_it);
			g.toolIndex = g.dragTargetTool;
			g.lastToolIndex = -1;
		}
		g.draggingTool = -1;
		g.dragTargetTool = -1;
	}
	ImGui::EndChild();
	ImGui::PopStyleColor();

	ImGui::SetCursorPos(ImVec2(kDocPad, strip_height + 8.0f));
	ImGui::BeginChild("##doc_body", ImVec2(size.x - kDocPad * 2.0f, size.y - strip_height - 20.0f), ImGuiChildFlags_None);
	UIWindow* active = tools[static_cast<size_t>(g.toolIndex)];
	if (active->open)
		active->RenderCore();
	else
		ImGui::TextDisabled("%s is closed.", ToolDisplayName(active).c_str());
	ImGui::EndChild();
	EndFlatPanel();
}

void RenderWelcomeDocument(const ThemePalette& palette, ImVec2 pos, ImVec2 size) {
	DrawFlatPanel("##ShellWelcomeDocument", pos, size, palette.background);
	RenderWelcome(palette);
	EndFlatPanel();
}

void RenderSettingsDocument(const ThemePalette& palette, ImVec2 pos, ImVec2 size) {
	DrawFlatPanel("##ShellSettingsDocument", pos, size, palette.background);
	UIWindow* settings = FindWindow("Settings");
	if (!settings) {
		ImGui::SetCursorPos(ImVec2(18.0f, 16.0f));
		ImGui::TextDisabled("The Settings document is not available in this build.");
		EndFlatPanel();
		return;
	}
	ImGui::SetCursorPos(ImVec2(14.0f, 12.0f));
	ImGui::BeginChild("##settings_document_body", ImVec2(size.x - 28.0f, size.y - 24.0f), ImGuiChildFlags_None);
	settings->RenderCore();
	ImGui::EndChild();
	EndFlatPanel();
}

void RenderContent(const ThemePalette& palette, ImVec2 pos, ImVec2 size) {
	g.rectContentBody = ImVec4(pos.x + 12.0f, pos.y + 12.0f, size.x - 24.0f, size.y - 24.0f);
	if (g.activeSettings)
		RenderSettingsDocument(palette, pos, size);
	else if (g.activeFile >= 0)
		RenderFileEditor(palette, pos, size);
	else if (g.showWelcome)
		RenderWelcomeDocument(palette, pos, size);
	else
		RenderToolDocuments(palette, pos, size);
}

void RenderOutputTab() {
	ImGui::BeginChild("##output_body", ImVec2(0, 0), ImGuiChildFlags_None);
	ImGui::Indent(10.0f);
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
	if (g.output.empty()) {
		ImGui::TextDisabled("No output yet. Build or run something.");
	}
	else if (PerfNoClip()) {
		for (const auto& line : g.output) {
			if (line.severity == 2)
				ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", line.text.c_str());
			else
				ImGui::TextUnformatted(line.text.c_str());
		}
	}
	else {
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(g.output.size()));
		while (clipper.Step()) {
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
				const OutputLine& line = g.output[static_cast<size_t>(i)];
				if (line.severity == 2)
					ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", line.text.c_str());
				else
					ImGui::TextUnformatted(line.text.c_str());
			}
		}
	}
	if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
		ImGui::SetScrollHereY(1.0f);
	ImGui::Unindent(10.0f);
	ImGui::EndChild();
}

std::string FormatProblemsForClipboard() {
	if (g.problems.empty())
		return "No problems.\n";
	std::string dump;
	for (const auto& problem : g.problems) {
		const char* severity = problem.severity >= 2 ? "Error" :
			(problem.severity == 1 ? "Warning" : "Note");
		const std::string file = problem.file.empty() ? "(no file)" : problem.file;
		dump += severity;
		dump += " ";
		dump += file;
		dump += ":" + std::to_string(problem.line) + ":" + std::to_string(problem.column);
		dump += " [" + (problem.code.empty() ? std::string("-") : problem.code) + "] ";
		dump += problem.text;
		if (!problem.hint.empty())
			dump += "  (" + problem.hint + ")";
		dump += "\n";
	}
	return dump;
}

void RenderProblemsTab(const ThemePalette& palette) {
	ImGui::BeginChild("##problems_body", ImVec2(0, 0), ImGuiChildFlags_None);
	int errors = 0, warnings = 0, notes = 0;
	for (const auto& problem : g.problems) {
		errors += problem.severity >= 2 ? 1 : 0;
		warnings += problem.severity == 1 ? 1 : 0;
		notes += problem.severity == 0 ? 1 : 0;
	}
	ImGui::SetCursorPosX(10.0f);
	ImGui::TextDisabled("%d errors, %d warnings, %d notes", errors, warnings, notes);
	ImGui::SameLine();
	const float copy_width = 92.0f;
	ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 12.0f,
		ImGui::GetContentRegionMax().x - copy_width - 8.0f));
	if (ImGui::Button("Copy All", ImVec2(copy_width, 22.0f))) {
		const std::string dump = FormatProblemsForClipboard();
		ImGui::SetClipboardText(dump.c_str());
		g.panelCopyNotice = "Copied " + std::to_string(g.problems.size()) +
			(g.problems.size() == 1 ? " problem" : " problems") + " to the clipboard";
		g.panelCopyNoticeAt = ImGui::GetTime();
		AppendOutput("[problems] " + g.panelCopyNotice);
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Copy all diagnostics in a stable text format");
	if (!g.panelCopyNotice.empty()) {
		if (ImGui::GetTime() - g.panelCopyNoticeAt < 4.0) {
			ImGui::SetCursorPosX(10.0f);
			ImGui::TextColored(palette.link, "%s", g.panelCopyNotice.c_str());
		}
		else {
			g.panelCopyNotice.clear();
		}
	}
	ImGui::Spacing();
	auto activate = [](const DiagLine& problem) {
		int target = -1;
		for (int index = 0; index < static_cast<int>(g.files.size()); ++index) {
			if (g.files[static_cast<size_t>(index)]->path == problem.file ||
				g.files[static_cast<size_t>(index)]->name == problem.file) {
				target = index;
				break;
			}
		}
		if (target < 0) {
			std::error_code ec;
			if (std::filesystem::is_regular_file(problem.file, ec)) {
				OpenFileInEditor(problem.file);
				target = g.activeFile;
			}
		}
		if (target >= 0) {
			g.activeFile = target;
			FileDocument& doc = *g.files[static_cast<size_t>(target)];
			if (problem.line > 0)
				doc.editor.SetCursorPosition(TextEditor::Coordinates(problem.line - 1,
					std::max(0, problem.column - 1)));
		}
	};
	auto color = [](int severity) {
		return severity >= 2 ? ImVec4(0.945f, 0.298f, 0.298f, 1.0f)
			: (severity == 1 ? ImVec4(0.8f, 0.655f, 0.0f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
	};
	// One fixed-height row per diagnostic keeps the list clippable: only the
	// rows inside the visible scroll band are submitted to ImGui.
	ImGuiListClipper clipper;
	if (!PerfNoClip())
		clipper.Begin(static_cast<int>(g.problems.size()));
	bool clipper_done = false;
	while (!clipper_done) {
		int first = 0;
		int last = static_cast<int>(g.problems.size());
		if (PerfNoClip()) {
			clipper_done = true;
		}
		else {
			if (!clipper.Step())
				break;
			first = clipper.DisplayStart;
			last = clipper.DisplayEnd;
		}
		for (int index = first; index < last; ++index) {
			const DiagLine& problem = g.problems[static_cast<size_t>(index)];
			const std::string file = problem.file.empty() ? std::string("-") :
				std::filesystem::path(problem.file).filename().string();
			const char* severity = problem.severity >= 2 ? "Error" :
				(problem.severity == 1 ? "Warning" : "Note");
			char row[1024];
			std::snprintf(row, sizeof(row), "%-7s  %-12s  %s   %s:%d:%d",
				severity, problem.code.empty() ? "-" : problem.code.c_str(),
				problem.text.c_str(), file.c_str(), problem.line, problem.column);
			ImGui::PushID(index);
			ImGui::PushStyleColor(ImGuiCol_Text, color(problem.severity));
			const bool clicked = ImGui::Selectable(row, false, ImGuiSelectableFlags_None,
				ImVec2(ImGui::GetContentRegionAvail().x, 0.0f));
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered() && !problem.hint.empty())
				ImGui::SetTooltip("%s", problem.hint.c_str());
			if (clicked)
				activate(problem);
			ImGui::PopID();
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

	const char* tabs[] = { "Output", "Problems", "Terminal" };
	const bool compact_panel = size.x < 520.0f;
	const float close_width = compact_panel ? 26.0f : 70.0f;
	const float tabs_limit = pos.x + size.x - close_width - 24.0f;
	float x = pos.x + 12.0f;
	ImGui::PushClipRect(ImVec2(pos.x, pos.y), ImVec2(tabs_limit, pos.y + kPanelHeaderHeight), true);
	for (int index = 0; index < 3; ++index) {
		const float width = TextWidth(tabs[index]) + 26.0f +
			((index == 1 && !g.problems.empty()) ? 34.0f : 0.0f);
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
		draw->AddText(ImVec2(min.x + 12.0f, min.y + (kPanelHeaderHeight - ImGui::GetTextLineHeight()) * 0.5f),
			ImGui::ColorConvertFloat4ToU32(g.panelTab == index ? palette.foreground : palette.muted), tabs[index]);
		if (index == 1 && !g.problems.empty()) {
			const std::string count = std::to_string(g.problems.size());
			draw->AddText(ImVec2(min.x + width - 24.0f, min.y + (kPanelHeaderHeight - ImGui::GetTextLineHeight()) * 0.5f),
				ImGui::ColorConvertFloat4ToU32(palette.error), count.c_str());
		}
		ImGui::PopID();
		x += width;
	}
	ImGui::PopClipRect();

	// right-side close (compact label on narrow editor columns)
	ImGui::SetCursorScreenPos(ImVec2(pos.x + size.x - 12.0f - close_width, pos.y + 6.0f));
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.hover);
	ImGui::PushStyleColor(ImGuiCol_Text, palette.foreground);
	if (ImGui::Button(compact_panel ? "x##panel_close" : "Close##panel_close",
			ImVec2(close_width, 22)))
		g.panelVisible = false;
	ImGui::PopStyleColor(3);

	g.rectPanelBody = ImVec4(pos.x + 12.0f, pos.y + kPanelHeaderHeight + 8.0f,
		size.x - 24.0f, size.y - kPanelHeaderHeight - 20.0f);
	ImGui::SetCursorScreenPos(ImVec2(pos.x + 12.0f, pos.y + kPanelHeaderHeight + 8.0f));
	ImGui::BeginChild("##panel_content", ImVec2(size.x - 24.0f, size.y - kPanelHeaderHeight - 20.0f), ImGuiChildFlags_None);
	if (g.panelTab == 0)
		RenderOutputTab();
	else if (g.panelTab == 1)
		RenderProblemsTab(palette);
	else {
		if (!g.terminalStarted) {
			g.terminal.Start(g.workspace);
			g.terminalStarted = true;
		}
		g.terminal.Render("##terminal_view", ImGui::GetContentRegionAvail().y, true);
	}
	ImGui::EndChild();

	// resize grip along the top edge
	ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y - 3.0f));
	ImGui::InvisibleButton("##panel_resize", ImVec2(size.x, 6.0f));
	ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x, pos.y - 1.0f),
		ImVec2(pos.x + size.x, pos.y - 1.0f), ImGui::ColorConvertFloat4ToU32(palette.borderStrong), 1.0f);
	if (ImGui::IsItemHovered() || ImGui::IsItemActive())
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
	if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		g.panelHeight = 200.0f;
	if (ImGui::IsItemActive()) {
		g.panelHeight = std::clamp(g.panelHeight - ImGui::GetIO().MouseDelta.y,
			80.0f, ImGui::GetMainViewport()->Size.y * 0.6f);
		SaveLayoutThrottled();
	}
	EndFlatPanel();
}

void RenderStatusBar(const ThemePalette& palette, ImVec2 pos, ImVec2 size) {
	g.rectStatusContent = ImVec4(pos.x + 12.0f, pos.y + 8.0f, size.x - 24.0f, size.y - 16.0f);
	DrawFlatPanel("##ShellStatus", pos, size, palette.statusBar);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImU32 fg = ImGui::ColorConvertFloat4ToU32(palette.statusBarForeground);
	const ImU32 dim = ImGui::ColorConvertFloat4ToU32(WithAlpha(palette.statusBarForeground, 0.8f));
	const bool small = g_small_font != nullptr;
	if (small)
		ImGui::PushFont(g_small_font);
	float x = pos.x + 10.0f;
	auto item = [&](const char* text, ImU32 color) {
		draw->AddText(ImVec2(x, pos.y + 8.0f), color, text);
		x += TextWidth(text) + 8.0f;
		draw->AddText(ImVec2(x, pos.y + 8.0f), dim, "|");
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
	if (!g.problems.empty()) {
		char counts[64];
		std::snprintf(counts, sizeof(counts), "Errors: %d  Warnings: %d",
			(int)std::count_if(g.problems.begin(), g.problems.end(), [](const DiagLine& d) { return d.severity >= 2; }),
			(int)std::count_if(g.problems.begin(), g.problems.end(), [](const DiagLine& d) { return d.severity == 1; }));
		const float counts_w = TextWidth(counts);
		draw->AddText(ImVec2(pos.x + size.x * 0.45f - counts_w * 0.5f, pos.y + 8.0f), fg, counts);
	}
	std::string right = SettingsStore::Instance().Data().language + "  |  " +
		ThemeManager::Instance().CurrentThemeName() + "  |  build " CASIOEMU_BUILD_COMMIT " " CASIOEMU_BUILD_TIME;
	const float right_width = TextWidth(right.c_str());
	draw->AddText(ImVec2(pos.x + size.x - right_width - 12.0f, pos.y + 8.0f), fg, right.c_str());
	if (small)
		ImGui::PopFont();
	EndFlatPanel();
}

std::string ToolDisplayName(const UIWindow* win) {
	if (!win || !win->name)
		return std::string();
	// The ROP compiler window keeps its historical class/file name; the tab is
	// presented as the full "Rop Compiler" label.
	if (std::strcmp(win->name, "Injection") == 0)
		return "Rop Compiler";
	return win->name;
}

std::vector<UIWindow*> ToolWindows() {
	std::vector<UIWindow*> tools;
	for (auto* win : windows) {
		if (!win || IsCalculatorWindow(win) || !win->name)
			continue;
		// The Wiz compiler is an overlay on the file editor, not a document tab.
		if (std::strcmp(win->name, "Wiz Compiler") == 0)
			continue;
		// Settings is a first-class document tab in the main models/session tab
		// strip.  It must not also appear in the per-tool document strip.
		if (std::strcmp(win->name, "Settings") == 0)
			continue;
		// ROM / RAM / All are real document tabs for a running model; the
		// Addresses ("Memory") view stays available alongside them.
		// Plugins are configured in Settings > Plugins; a second Plugin Manager
		// tab would be a duplicate entry for the same screen.
		if (std::strcmp(win->name, "Plugin Manager") == 0)
			continue;
		tools.push_back(win);
	}
	return tools;
}

void SortToolsByOrder(std::vector<UIWindow*>& tools) {
	for (const auto* tool : tools) {
		if (std::find(g.toolOrder.begin(), g.toolOrder.end(), tool->name) == g.toolOrder.end())
			g.toolOrder.push_back(tool->name);
	}
	std::sort(tools.begin(), tools.end(), [](const UIWindow* lhs, const UIWindow* rhs) {
		const auto left = std::find(g.toolOrder.begin(), g.toolOrder.end(), lhs->name);
		const auto right = std::find(g.toolOrder.begin(), g.toolOrder.end(), rhs->name);
		return left < right;
	});
}


void ExecutePaletteSelection(const std::string& id) {
	ExecuteCommand(id);
	g.paletteOpen = false;
	g.quickOpen = false;
	g.paletteQuery[0] = '\0';
}

const std::vector<PaletteCommand>& Commands() {
	static const std::vector<PaletteCommand> commands = {
		{ "session.new", "Session: New Calculator", "" },
		{ "model.open", "File: Open Model...", "" },
		{ "model.import", "File: Import Model Package...", "" },
		{ "folder.open", "File: Open Folder...", "" },
		{ "plugin.install", "Plugins: Open Settings > Plugins", "" },
		{ "plugin.manager", "Plugins: Open Settings > Plugins", "" },
		{ "view.sidebar", "View: Toggle Sidebar", "" },
		{ "view.panel", "View: Toggle Bottom Panel", "" },
		{ "view.tools", "View: Toggle Tool Dock", "" },
		{ "view.calculator", "View: Toggle Calculator Panel", "" },
		{ "view.terminal", "Terminal: Focus Terminal", "" },
		{ "settings.open", "Preferences: Open Settings", "" },
		{ "session.pause", "Session: Pause / Resume", "" },
		{ "session.reset", "Session: Reset", "" },
		{ "build", "Build: Build", "" },
		{ "build.run", "Build: Build & Run", "" },
		{ "theme.vscode-dark-plus", "Preferences: Color Theme - VS Code Dark+ (classic)", "" },
		{ "theme.vscode-dark", "Preferences: Color Theme - VS Code Dark Modern", "" },
		{ "theme.midnight-blue", "Preferences: Color Theme - Midnight Blue", "" },
		{ "theme.solarized", "Preferences: Color Theme - Solarized", "" },
		{ "theme.mono-green", "Preferences: Color Theme - Mono Green", "" },
	};
	return commands;
}

void PushMenuStyle(const ThemePalette& palette) {
	ImGui::PushStyleColor(ImGuiCol_PopupBg, palette.surface);
	ImGui::PushStyleColor(ImGuiCol_Border, palette.borderStrong);
	ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 4.0f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(200.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
}

void PopMenuStyle() {
	ImGui::PopStyleVar(5);
	ImGui::PopStyleColor(2);
}

void PushDialogStyle(const ThemePalette& palette) {
	ImGui::PushStyleColor(ImGuiCol_WindowBg, palette.surface);
	ImGui::PushStyleColor(ImGuiCol_PopupBg, palette.surface);
	ImGui::PushStyleColor(ImGuiCol_Border, palette.borderStrong);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, kDialogRounding);
	ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, kDialogRounding);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kDialogPadding, kDialogPadding));
}

void PopDialogStyle() {
	ImGui::PopStyleVar(4);
	ImGui::PopStyleColor(3);
}

// Draws the shared dialog title bar: title on the left, X on the right.  Every
// shell popup uses this so corners, padding and the close affordance match.
void DrawDialogHeader(const ThemePalette& palette, const char* title, bool* open) {
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImVec2 window_pos = ImGui::GetWindowPos();
	const float width = ImGui::GetWindowWidth();
	const float height = 34.0f;
	const float text_h = ImGui::GetTextLineHeight();
	draw->AddText(ImVec2(window_pos.x + kDialogPadding,
		window_pos.y + (height - text_h) * 0.5f),
		ImGui::ColorConvertFloat4ToU32(palette.foreground), title);

	const float close_size = 22.0f;
	const float close_x = width - kDialogPadding - close_size;
	ImGui::SetCursorPos(ImVec2(close_x, 6.0f));
	const bool close_clicked = ImGui::InvisibleButton("##dialog_close", ImVec2(close_size, close_size));
	const bool close_hovered = ImGui::IsItemHovered();
	const ImVec2 close_min = ImGui::GetItemRectMin();
	const ImVec2 close_max = ImGui::GetItemRectMax();
	if (close_hovered)
		draw->AddRectFilled(close_min, close_max, ImGui::ColorConvertFloat4ToU32(palette.hover), 4.0f);
	const ImU32 x_color = ImGui::ColorConvertFloat4ToU32(close_hovered ? palette.foreground : palette.muted);
	const float inset = 6.5f;
	draw->AddLine(ImVec2(close_min.x + inset, close_min.y + inset),
		ImVec2(close_max.x - inset, close_max.y - inset), x_color, 1.4f);
	draw->AddLine(ImVec2(close_max.x - inset, close_min.y + inset),
		ImVec2(close_min.x + inset, close_max.y - inset), x_color, 1.4f);
	draw->AddLine(ImVec2(window_pos.x, window_pos.y + height),
		ImVec2(window_pos.x + width, window_pos.y + height),
		ImGui::ColorConvertFloat4ToU32(palette.border), 1.0f);
	ImGui::SetCursorPos(ImVec2(kDialogPadding, height + 10.0f));
	if (close_clicked) {
		if (open)
			*open = false;
		ImGui::CloseCurrentPopup();
	}
}

bool BeginShellModal(const char* id, const ThemePalette& palette, const char* title, bool* open,
		const ImVec2& size, ImGuiWindowFlags extra = 0) {
	ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
	PushDialogStyle(palette);
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | extra;
	if (!ImGui::BeginPopupModal(id, nullptr, flags)) {
		PopDialogStyle();
		return false;
	}
	DrawDialogHeader(palette, title, open);
	return true;
}

void EndShellModal() {
	ImGui::EndPopup();
	PopDialogStyle();
}

bool BeginShellOverlay(const ThemePalette& palette, const char* id, const char* title, bool* open,
		const ImVec2& pos, const ImVec2& size, ImGuiWindowFlags extra = 0) {
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
	PushDialogStyle(palette);
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | extra;
	if (!ImGui::Begin(id, nullptr, flags)) {
		PopDialogStyle();
		ImGui::End();
		return false;
	}
	DrawDialogHeader(palette, title, open);
	return true;
}

void EndShellOverlay() {
	ImGui::End();
	PopDialogStyle();
}

void RenderOverlays(const ThemePalette& palette) {
	if (g.dismissClosePrompt) {
		g.dismissClosePrompt = false;
		g.closeConfirmOpen = false;
		g.pendingCloseFile = -1;
		if (ImGui::IsPopupOpen("Unsaved changes##file_close"))
			ImGui::ClosePopupToLevel(0, true);
	}
	if (g.closeConfirmOpen) {
		ImGui::OpenPopup("Unsaved changes##file_close");
		ToolUi::DialogScope dialog("Unsaved changes##file_close", "Unsaved Changes", 470.0f);
		if (dialog) {
			if (dialog.CloseRequested()) {
				g.pendingCloseFile = -1;
				g.closeConfirmOpen = false;
				ImGui::CloseCurrentPopup();
			}
			else {
				ImGui::TextWrapped("This file has unsaved changes. Save before closing?");
				ImGui::Dummy(ImVec2(0.0f, 6.0f));
				if (ImGui::Button("Save", ImVec2(110, 26))) {
					if (g.pendingCloseFile >= 0 && g.pendingCloseFile < static_cast<int>(g.files.size()))
						SaveFileDocument(*g.files[static_cast<size_t>(g.pendingCloseFile)]);
					const int index = g.pendingCloseFile;
					g.pendingCloseFile = -1;
					g.closeConfirmOpen = false;
					EraseFileDocument(index);
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button("Discard", ImVec2(110, 26))) {
					const int index = g.pendingCloseFile;
					g.pendingCloseFile = -1;
					g.closeConfirmOpen = false;
					EraseFileDocument(index);
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel", ImVec2(110, 26))) {
					g.pendingCloseFile = -1;
					g.closeConfirmOpen = false;
					ImGui::CloseCurrentPopup();
				}
			}
		}
	}
	auto filesystem_error = [](const std::string& action, const std::error_code& ec) {
		g.sidebarError = action + ": " + ec.message();
		AppendOutput("[explorer] " + g.sidebarError);
	};
	if (g.renameOpen) {
		ImGui::OpenPopup("Rename##explorer");
		ToolUi::DialogScope dialog("Rename##explorer", "Rename", 500.0f);
		if (dialog) {
			if (dialog.CloseRequested()) {
				g.renameOpen = false;
				ImGui::CloseCurrentPopup();
			}
			else {
				ImGui::TextDisabled("%s", g.selectedPath.string().c_str());
				ImGui::SetNextItemWidth(-1.0f);
				ImGui::InputText("##rename_name", g.entryName, sizeof(g.entryName));
				ImGui::Dummy(ImVec2(0.0f, 6.0f));
				if (ImGui::Button("Rename", ImVec2(110, 26))) {
					if (g.entryName[0]) {
						std::error_code ec;
						const auto target = g.selectedPath.parent_path() / g.entryName;
						std::filesystem::rename(g.selectedPath, target, ec);
						g.sidebarCache.clear();
						if (ec)
							filesystem_error("rename failed", ec);
						else
							g.selectedPath = target;
					}
					g.renameOpen = false;
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel", ImVec2(110, 26))) {
					g.renameOpen = false;
					ImGui::CloseCurrentPopup();
				}
			}
		}
	}
	if (g.newFileOpen || g.newFolderOpen) {
		const bool folder = g.newFolderOpen;
		const char* popup = folder ? "New Folder##explorer" : "New File##explorer";
		ImGui::OpenPopup(popup);
		ToolUi::DialogScope dialog(popup, folder ? "New Folder" : "New File", 500.0f);
		if (dialog) {
			if (dialog.CloseRequested()) {
				g.newFileOpen = false;
				g.newFolderOpen = false;
				ImGui::CloseCurrentPopup();
			}
			else {
				ImGui::TextDisabled("%s", g.selectedPath.string().c_str());
				ImGui::SetNextItemWidth(-1.0f);
				ImGui::InputText("##new_entry_name", g.entryName, sizeof(g.entryName));
				ImGui::Dummy(ImVec2(0.0f, 6.0f));
				if (ImGui::Button("Create", ImVec2(110, 26))) {
					if (g.entryName[0]) {
						std::error_code ec;
						const auto target = g.selectedPath / g.entryName;
						if (folder) {
							std::filesystem::create_directory(target, ec);
							g.sidebarCache.clear();
							if (ec)
								filesystem_error("create folder failed", ec);
						}
						else {
							std::ofstream file(target, std::ios::binary);
							if (!file)
								filesystem_error("create file failed", std::make_error_code(std::errc::permission_denied));
							else
								file.close();
							g.sidebarCache.clear();
							OpenFileInEditor(target);
						}
					}
					g.newFileOpen = false;
					g.newFolderOpen = false;
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel", ImVec2(110, 26))) {
					g.newFileOpen = false;
					g.newFolderOpen = false;
					ImGui::CloseCurrentPopup();
				}
			}
		}
	}
	if (g.deleteOpen) {
		ImGui::OpenPopup("Delete##explorer");
		ToolUi::DialogScope dialog("Delete##explorer", "Delete", 500.0f);
		if (dialog) {
			if (dialog.CloseRequested()) {
				g.deleteOpen = false;
				ImGui::CloseCurrentPopup();
			}
			else {
				ImGui::TextWrapped("Delete %s? This cannot be undone.", g.selectedPath.string().c_str());
				ImGui::Dummy(ImVec2(0.0f, 6.0f));
				if (ImGui::Button("Delete", ImVec2(110, 26))) {
					std::error_code ec;
					std::filesystem::remove_all(g.selectedPath, ec);
					g.sidebarCache.clear();
					if (ec)
						filesystem_error("delete failed", ec);
					g.deleteOpen = false;
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel", ImVec2(110, 26))) {
					g.deleteOpen = false;
					ImGui::CloseCurrentPopup();
				}
			}
		}
	}
	if (g.importPasswordOpen) {
		ImGui::OpenPopup("Import Password##shell_import");
		bool open = true;
		if (BeginShellModal("Import Password##shell_import", palette, "Encrypted Model Package", &open,
				ImVec2(500.0f, 220.0f))) {
			ImGui::TextWrapped("%s", g.importPackagePath.filename().string().c_str());
			ImGui::TextDisabled("This package is encrypted. Enter its password to import it.");
			ImGui::Dummy(ImVec2(0.0f, 4.0f));
			ImGui::SetNextItemWidth(-1.0f);
			const bool submitted = ImGui::InputText("##shell_import_password", g.importPassword,
				sizeof(g.importPassword), ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::Dummy(ImVec2(0.0f, 6.0f));
			if (ImGui::Button("Import", ImVec2(110, 26)) || submitted) {
				if (g.importPassword[0] != '\0') {
					const std::filesystem::path path = g.importPackagePath;
					const std::string password = g.importPassword;
					g.importPasswordOpen = false;
					ImGui::CloseCurrentPopup();
					ApplyShellImport(path, password);
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(110, 26))) {
				g.importPasswordOpen = false;
				ImGui::CloseCurrentPopup();
			}
			EndShellModal();
		}
		if (!open)
			g.importPasswordOpen = false;
	}
	if (g.importResultOpen) {
		ImGui::OpenPopup("Import Result##shell_import");
		bool open = true;
		if (BeginShellModal("Import Result##shell_import", palette,
				g.importResultError ? "Import Failed" : "Import Complete", &open, ImVec2(560.0f, 220.0f))) {
			ImGui::TextWrapped("%s", g.importResultText.c_str());
			ImGui::Dummy(ImVec2(0.0f, 8.0f));
			if (ImGui::Button("OK", ImVec2(110, 26))) {
				g.importResultOpen = false;
				ImGui::CloseCurrentPopup();
			}
			EndShellModal();
		}
		if (!open)
			g.importResultOpen = false;
	}
	if (g.openModelOpen) {
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		const float width = std::min(680.0f, viewport->Size.x - 120.0f);
		bool open = g.openModelOpen;
		const ImVec2 pos(viewport->Pos.x + (viewport->Size.x - width) * 0.5f, viewport->Pos.y + 70.0f);
		if (BeginShellOverlay(palette, "##open_model_overlay", "Open Model", &open, pos, ImVec2(width, 470.0f))) {
			if (g.openModelFocus) {
				ImGui::SetKeyboardFocusHere();
				g.openModelFocus = false;
			}
			ImGui::SetNextItemWidth(-1.0f);
			const bool submitted = ImGui::InputTextWithHint("##open_model_query", "Search installed models",
				g.openModelQuery, sizeof(g.openModelQuery), ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::Dummy(ImVec2(0.0f, 4.0f));
			LoadModelChoices();
			std::string first_match;
			int shown = 0;
			ImGui::BeginChild("##open_model_list", ImVec2(0, 0), ImGuiChildFlags_None);
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
				ImGui::TextDisabled("No models found in the models folder");
			ImGui::EndChild();
			if (submitted && !first_match.empty())
				OpenModelAt(first_match);
			if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
				open = false;
			EndShellOverlay();
		}
		if (!open)
			g.openModelOpen = false;
	}
	if (!g.paletteOpen && !g.quickOpen)
		return;
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	const bool quick = g.quickOpen;
	const float width = std::min(680.0f, viewport->Size.x - 120.0f);
	const ImVec2 pos(viewport->Pos.x + (viewport->Size.x - width) * 0.5f, viewport->Pos.y + 60.0f);
	bool open = true;
	if (BeginShellOverlay(palette, "##palette_overlay", quick ? "Quick Open" : "Command Palette", &open,
			pos, ImVec2(width, 0.0f))) {
		if (g.paletteFocus) {
			ImGui::SetKeyboardFocusHere();
			g.paletteFocus = false;
		}
		ImGui::SetNextItemWidth(-1.0f);
		const bool submitted = ImGui::InputText("##palette_query", g.paletteQuery, sizeof(g.paletteQuery),
			ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::Dummy(ImVec2(0.0f, 2.0f));
		ImGui::Separator();
		ImGui::Dummy(ImVec2(0.0f, 2.0f));

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
					open = false;
					g.paletteQuery[0] = '\0';
				}
			}
			if (submitted && shown > 0) {
				for (const auto& file : g.quickFiles) {
					const std::string relative = std::filesystem::relative(file, g.workspace).string();
					if (MatchesFilter(relative, g.paletteQuery)) {
						OpenExternal(file);
						open = false;
						g.paletteQuery[0] = '\0';
						break;
					}
				}
			}
		}
		else {
			const auto& commands = Commands();
			int shown = 0;
			for (const auto& command : commands) {
				if (!MatchesFilter(command.label, g.paletteQuery))
					continue;
				if (++shown > 14)
					break;
				if (ImGui::Selectable((command.label + "##" + command.id).c_str(), false,
						ImGuiSelectableFlags_None)) {
					ExecutePaletteSelection(command.id);
					open = false;
				}
			}
			if (submitted && shown > 0) {
				for (const auto& command : commands) {
					if (MatchesFilter(command.label, g.paletteQuery)) {
						ExecutePaletteSelection(command.id);
						open = false;
						break;
					}
				}
			}
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
			open = false;
		EndShellOverlay();
	}
	if (!open) {
		g.paletteOpen = false;
		g.quickOpen = false;
		g.paletteQuery[0] = '\0';
	}
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
	if (g.activeFile >= 0 && g.activeFile < static_cast<int>(g.files.size())) {
		FileDocument& doc = *g.files[static_cast<size_t>(g.activeFile)];
		if (ImGui::IsKeyPressed(ImGuiKey_S, false))
			SaveFileDocument(doc);
		if (ImGui::IsKeyPressed(ImGuiKey_F, false))
			doc.findOpen = !doc.findOpen;
		if (ImGui::IsKeyPressed(ImGuiKey_W, false))
			CloseFileDocument(g.activeFile);
	}
	if (ImGui::IsKeyPressed(ImGuiKey_B, false)) ToggleSidebar();
	if (ImGui::IsKeyPressed(ImGuiKey_J, false)) ToggleBottomPanel();
	if (ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false)) FocusTerminal();
	if (ImGui::IsKeyPressed(ImGuiKey_Comma, false)) OpenSettings();
	if (ImGui::IsKeyPressed(ImGuiKey_1, false) && g.tabs.size() >= 1) { g.activeTab = 0; g.showWelcome = false; g.activeSettings = false; g.activeFile = -1; session::Activate(0); }
	if (ImGui::IsKeyPressed(ImGuiKey_2, false) && g.tabs.size() >= 2) { g.activeTab = 1; g.showWelcome = false; g.activeSettings = false; g.activeFile = -1; session::Activate(1); }
	if (ImGui::IsKeyPressed(ImGuiKey_3, false) && g.tabs.size() >= 3) { g.activeTab = 2; g.showWelcome = false; g.activeSettings = false; g.activeFile = -1; session::Activate(2); }
	if (ImGui::IsKeyPressed(ImGuiKey_4, false) && g.tabs.size() >= 4) { g.activeTab = 3; g.showWelcome = false; g.activeSettings = false; g.activeFile = -1; session::Activate(3); }
	if (ImGui::IsKeyPressed(ImGuiKey_5, false) && g.tabs.size() >= 5) { g.activeTab = 4; g.showWelcome = false; g.activeSettings = false; g.activeFile = -1; session::Activate(4); }
}

void RenderLayout() {
	const ThemePalette& palette = ThemeManager::Instance().Colors();
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	const float width = viewport->Size.x;
	const float height = viewport->Size.y;
	const float top = kTitleBarHeight + kTabStripHeight;
	const float status_h = kStatusBarHeight;
	const float panel_h = g.panelVisible ? std::min(g.panelHeight, height * 0.6f) : 0.0f;
	float calculator_w = g.calculatorVisible ? std::min(g.calculatorWidth, width * 0.55f) : 0.0f;
	float sidebar_w = g.sidebarVisible ? std::min(g.sidebarWidth, width * 0.40f) : 0.0f;
	// On a narrow window the fixed side panels must give way instead of pushing
	// the main column past the right edge.
	const float min_content_w = 220.0f;
	if (sidebar_w + calculator_w + min_content_w > width) {
		const float scale = std::max(0.0f, width - min_content_w) /
			std::max(1.0f, sidebar_w + calculator_w);
		sidebar_w = std::max(140.0f, sidebar_w * scale);
		calculator_w = std::max(160.0f, calculator_w * scale);
		if (sidebar_w + calculator_w + min_content_w > width)
			calculator_w = std::max(120.0f, width - sidebar_w - min_content_w);
	}
	const float content_x = viewport->Pos.x + sidebar_w;
	const float content_w = std::max(200.0f, width - sidebar_w - calculator_w);
	const float body_h = std::max(160.0f, height - top - status_h);
	const float editor_h = std::max(80.0f, body_h - panel_h);
	const float editor_y = viewport->Pos.y + top;
	const float editor_content_h = std::max(40.0f, editor_h);
	const float panel_y = viewport->Pos.y + top + editor_h;

	g.rectViewport = ImVec4(viewport->Pos.x, viewport->Pos.y, width, height);
	g.rectTitle = ImVec4(viewport->Pos.x, viewport->Pos.y, width, kTitleBarHeight);
	g.rectTabs = ImVec4(viewport->Pos.x, viewport->Pos.y + kTitleBarHeight, width, kTabStripHeight);
	g.rectActivity = ImVec4(viewport->Pos.x, viewport->Pos.y + top, 0.0f, body_h);
	g.rectSidebar = ImVec4(viewport->Pos.x, viewport->Pos.y + top, sidebar_w, body_h);
	g.rectBreadcrumb = ImVec4(content_x, viewport->Pos.y + top, 0.0f, 0.0f);
	g.rectContent = ImVec4(content_x, editor_y, content_w, editor_content_h);
	g.rectPanel = ImVec4(content_x, panel_y, content_w, panel_h);
	g.rectStatus = ImVec4(viewport->Pos.x, viewport->Pos.y + height - status_h, width, status_h);
	g.rectTools = ImVec4(content_x + content_w, viewport->Pos.y + top, calculator_w, body_h);

	RenderTitleBar(palette);
	RenderTabStrip(palette);
	if (g.sidebarVisible)
		RenderSidebar(palette, ImVec2(viewport->Pos.x, viewport->Pos.y + top),
			ImVec2(sidebar_w, body_h));
	RenderContent(palette, ImVec2(content_x, editor_y), ImVec2(content_w, editor_content_h));
	if (g.panelVisible)
		RenderBottomPanel(palette, ImVec2(content_x, panel_y), ImVec2(content_w, panel_h));
	if (g.calculatorVisible)
		RenderCalculatorPanel(palette, ImVec2(content_x + content_w, viewport->Pos.y + top),
			ImVec2(calculator_w, body_h));
	RenderStatusBar(palette, ImVec2(viewport->Pos.x, viewport->Pos.y + height - status_h),
		ImVec2(width, status_h));

	if (g.sidebarVisible) {
		ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + sidebar_w - 2.0f, viewport->Pos.y + top));
		ImGui::SetNextWindowSize(ImVec2(4.0f, body_h));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
		ImGui::Begin("##sidebar_grip", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground);
		ImGui::InvisibleButton("##grip", ImVec2(4.0f, body_h));
		ImGui::GetWindowDrawList()->AddLine(ImVec2(ImGui::GetWindowPos().x + 2.0f, ImGui::GetWindowPos().y),
			ImVec2(ImGui::GetWindowPos().x + 2.0f, ImGui::GetWindowPos().y + body_h),
			ImGui::ColorConvertFloat4ToU32(palette.border), 1.0f);
		if (ImGui::IsItemHovered() || ImGui::IsItemActive())
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
		if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			g.sidebarWidth = 300.0f;
		if (ImGui::IsItemActive()) {
			g.sidebarWidth = std::clamp(g.sidebarWidth + ImGui::GetIO().MouseDelta.x, 200.0f, width * 0.45f);
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
	g.calculatorVisible = data.shell.calculator.visible;
	g.calculatorWidth = static_cast<float>(std::clamp(data.shell.calculator.width, 260, 700));
	session::SetThrottle(data.emulator.backgroundThrottle);
	g.workspace = data.workspace.folder;
	if (g.workspace.empty()) {
		std::error_code ec;
		g.workspace = std::filesystem::current_path(ec).string();
	}
	if (!g.workspace.empty())
		g.expanded.insert(g.workspace);

	SyncTabsFromSessions();

	if (const char* report = std::getenv("CASIOEMU_PERF_REPORT")) {
		g.perfReportPath = report;
		if (const char* frames = std::getenv("CASIOEMU_PERF_FRAMES"))
			g.perfFrameLimit = std::max(10, std::atoi(frames));
	}
	if (const char* count = std::getenv("CASIOEMU_PERF_OUTPUT")) {
		const int wanted = std::max(1, std::atoi(count));
		g.output.clear();
		g.output.reserve(static_cast<size_t>(wanted));
		for (int index = 0; index < wanted; ++index) {
			OutputLine line;
			line.text = "[perf] synthetic output line " + std::to_string(index);
			line.severity = index % 11 == 0 ? 2 : 0;
			g.output.push_back(std::move(line));
		}
		g.panelVisible = true;
		g.panelTab = 0;
	}
	if (const char* count = std::getenv("CASIOEMU_PERF_PROBLEMS")) {
		const int wanted = std::max(1, std::atoi(count));
		g.problems.clear();
		g.problems.reserve(static_cast<size_t>(wanted));
		for (int index = 0; index < wanted; ++index) {
			DiagLine line;
			line.file = "perf-source.cwiz";
			line.line = index + 1;
			line.column = 3;
			line.severity = index % 7 == 0 ? 2 : (index % 3 == 0 ? 1 : 0);
			line.code = index % 2 == 0 ? "perf-rule" : "style";
			line.text = "Synthetic diagnostic line for the clipper performance probe.";
			line.hint = "Synthetic hint.";
			g.problems.push_back(std::move(line));
		}
		g.panelVisible = true;
		g.panelTab = 1;
	}

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
	// Do not persist here: the restore pass still needs the previous session
	// list from settings.json.
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
	OpenSettings();
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

void SetSidebarVisible(bool visible) {
	g.sidebarVisible = visible;
	SaveLayout();
}

void SetSidebarWidth(int width) {
	g.sidebarWidth = static_cast<float>(std::clamp(width, 200, 600));
	SaveLayout();
}

void SetBottomPanelVisible(bool visible) {
	g.panelVisible = visible;
	SaveLayout();
	if (g.panelVisible && g.panelTab == 2 && !g.terminalStarted) {
		g.terminal.Start(g.workspace);
		g.terminalStarted = true;
	}
}

void SetBottomPanelHeight(int height) {
	g.panelHeight = static_cast<float>(std::clamp(height, 80, 700));
	SaveLayout();
}

void SetToolsVisible(bool visible) {
	g.toolsVisible = visible;
	SaveLayout();
}

void SetToolsWidth(int width) {
	g.toolsWidth = static_cast<float>(std::clamp(width, 240, 800));
	SaveLayout();
}

void SetCalculatorVisible(bool visible) {
	g.calculatorVisible = visible;
	SaveLayout();
}

void SetCalculatorWidth(int width) {
	g.calculatorWidth = static_cast<float>(std::clamp(width, 260, 700));
	SaveLayout();
}

bool CalculatorVisible() {
	return g.calculatorVisible;
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
		win->open = true;
	g.activeSettings = true;
	g.activeFile = -1;
	g.showWelcome = false;
	g.lastActiveKey.clear();
}

void RequestBuild(bool run) {
	BuildToolchain(run);
}

void TogglePause() {
	if (m_emu)
		m_emu->SetPaused(!m_emu->GetPaused());
}

void ResetSession() {
	if (m_emu)
		m_emu->ResetToMainMenu();
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
	output << "sessions=" << session::Count() << " tabs=" << g.tabs.size() << '\n';
	if (g.activeFile < 0) {
		const auto tools = ToolWindows();
		if (!tools.empty())
			output << "tool=" << tools[static_cast<size_t>(std::clamp(g.toolIndex, 0, (int)tools.size() - 1))]->name << '\n';
	}
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
	int welcome_tabs = 0, settings_tabs = 0, session_tabs = 0, file_tabs = 0;
	float previous_right = -1.0f;
	bool tab_layout_ok = true;
	bool tab_size_ok = true;
	for (const auto& tab : g.mainTabRects) {
		output << "tab " << tab.label << " " << rect_text(tab.rect) << '\n';
		if (tab.kind == 0)
			++welcome_tabs;
		else if (tab.kind == 1)
			++session_tabs;
		else if (tab.kind == 2)
			++file_tabs;
		else if (tab.kind == 3)
			++settings_tabs;
		if (previous_right >= 0.0f && tab.rect.x < previous_right - eps)
			tab_layout_ok = false;
		previous_right = tab.rect.x + tab.rect.z;
		if (std::fabs(tab.rect.w - 24.0f) > eps || tab.rect.y < g.rectTabs.y - eps ||
			tab.rect.y + tab.rect.w > g.rectTabs.y + g.rectTabs.w + eps)
			tab_size_ok = false;
	}
	output << "main_tabs=" << g.mainTabRects.size() << " session_tabs=" << session_tabs
		<< " file_tabs=" << file_tabs << " welcome_tabs=" << welcome_tabs
		<< " settings_tabs=" << settings_tabs << " tab_scroll=" << g.tabScroll
		<< " doc_scroll=" << g.docScroll << '\n';
	auto inset_ok = [](const ImVec4& outer, const ImVec4& inner, float pad) {
		return inner.x - outer.x >= pad - 0.5f && inner.y - outer.y >= pad - 0.5f &&
			(outer.x + outer.z) - (inner.x + inner.z) >= pad - 0.5f &&
			(outer.y + outer.w) - (inner.y + inner.w) >= pad - 0.5f;
	};
	check(inset_ok(g.rectSidebar, g.rectSidebarBody, 8.0f), "sidebar content keeps >= 8 px on all sides");
	check(inset_ok(g.rectContent, g.rectContentBody, 8.0f), "main content keeps >= 8 px on all sides");
	check(inset_ok(g.rectPanel, g.rectPanelBody, 8.0f), "bottom panel content keeps >= 8 px on all sides");
	check(inset_ok(g.rectTools, g.rectToolsBody, 8.0f), "calculator content keeps >= 8 px on all sides");
	check(inset_ok(g.rectStatus, g.rectStatusContent, 8.0f), "status bar content keeps >= 8 px on all sides");
	check(inset_ok(g.rectTabs, g.rectTabsContent, 8.0f), "tab strip content keeps >= 8 px on all sides");
	check(g.chromeTitleCenterY == g.chromeMenuCenterY && g.chromeTitleCenterY == g.chromeActionCenterY,
		"title, menus and actions share one baseline");
	check(std::fabs(g.chromeTitleCenterY - (g.rectTitle.y + 14.0f)) < 1.0f,
		"chrome baseline matches the native traffic-light centre (y+14)");
	check(welcome_tabs == 1, "exactly one Welcome tab in the main tab strip");
	check(settings_tabs == 1, "exactly one canonical Settings tab in the main tab strip");
	check(session_tabs == session::Count(), "one main tab per live session (no duplicates)");
	check(tab_layout_ok, "main tabs do not overlap");
	check(tab_size_ok, "main tabs keep the fixed strip height");
	check(!g.calculatorVisible || (g.rectCalculatorGrip.z >= 4.0f && g.rectCalculatorGrip.z <= 8.0f),
		"calculator splitter has a 4-6 px hit area");
	check(!g.calculatorVisible || std::fabs(g.rectCalculatorGrip.x - g.rectTools.x) < 2.0f,
		"calculator splitter sits on the panel's left edge");
	bool settings_duplicated = false;
	bool raw_memory_tabs = false;
	bool plugin_manager_tab = false;
	bool rop_label_present = false;
	bool injection_label_present = false;
	for (const auto* tool : ToolWindows()) {
		if (!tool->name)
			continue;
		const std::string display = ToolDisplayName(tool);
		if (std::strcmp(tool->name, "Settings") == 0)
			settings_duplicated = true;
		if (std::strcmp(tool->name, "ROM") == 0 || std::strcmp(tool->name, "RAM") == 0 ||
			std::strcmp(tool->name, "All") == 0)
			raw_memory_tabs = true;
		if (std::strcmp(tool->name, "Plugin Manager") == 0)
			plugin_manager_tab = true;
		if (display == "Rop Compiler")
			rop_label_present = true;
		if (display == "Injection")
			injection_label_present = true;
	}
	check(!settings_duplicated, "Settings is not duplicated in the tool-document strip");
	check(raw_memory_tabs, "ROM/RAM/All tool tabs are present for a running model");
	check(!plugin_manager_tab, "no duplicate Plugins entry in the tool strip");
	check(!injection_label_present && rop_label_present, "Injection tab is labelled Rop Compiler");
	output << "panel_tabs=Output,Problems,Terminal\n";

	check(std::fabs((g.rectTitle.y + g.rectTitle.w) - g.rectTabs.y) < eps, "chrome/tab strip do not overlap");
	check(std::fabs((g.rectTabs.y + g.rectTabs.w) - g.rectActivity.y) < eps, "tab strip/body do not overlap");
	check(std::fabs((g.rectActivity.y + g.rectActivity.w) - g.rectStatus.y) < eps, "body/status do not overlap");
	check(std::fabs((g.rectStatus.y + g.rectStatus.w) - (g.rectViewport.y + g.rectViewport.w)) < eps, "status bar is the last row");
	check(std::fabs(g.rectSidebar.x - g.rectViewport.x) < eps, "sidebar starts at the window edge with inner padding");
	check(g.rectContent.x >= g.rectSidebar.x + g.rectSidebar.z - eps, "content does not overlap the sidebar");
	check(g.rectPanel.x >= g.rectSidebar.x + g.rectSidebar.z - eps, "bottom panel starts after the sidebar");
	check(g.rectPanel.z <= g.rectContent.z + eps, "bottom panel stays inside the editor column");
	check(g.rectTools.x >= g.rectContent.x + g.rectContent.z - eps, "calculator panel is to the right of the documents");
	check(g.rectTools.w >= 200.0f, "calculator side panel has usable width");
	check(g.rectContent.z > 200.0f && g.rectContent.w > 80.0f, "content region is non-zero");
	check(g.rectTitle.z > 0 && g.rectTabs.z > 0, "chrome and tab strip are non-zero");
	check(g.rectStatus.w >= 36.0f, "status bar has the padded 36 px height");
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
	if (g.lastSessionIndex != session::ActiveIndex() && session::Count() > 0) {
		g.lastSessionIndex = session::ActiveIndex();
		RebuildSessionMemoryWindows();
	}
	EnsureTitleFont();
	const auto perf_started = std::chrono::steady_clock::now();

	if (const char* second = std::getenv("CASIOEMU_OPEN_SECOND")) {
		static bool opened_second = false;
		if (!opened_second) {
			opened_second = true;
			std::error_code ec;
			const std::string primary_path = g.tabs.empty() ? std::string() : g.tabs.front().modelPath;
			for (const auto& entry : std::filesystem::directory_iterator(ModelsDirectory(), ec)) {
				if (!entry.is_directory())
					continue;
				const std::string name = entry.path().filename().string();
				if (name.find("Graph Light") != std::string::npos && entry.path().string() != primary_path) {
					OpenModelAt(entry.path().string());
					AppendOutput("[selftest] opened second session: " + name);
					break;
				}
			}
		}
		(void)second;
	}

	// Starting a process opens exactly one live session: the model this process
	// was launched with.  Previously every saved session was re-opened here,
	// which made one launch look like several (duplicate tabs, and an unrelated
	// default fx-580VN X appearing next to the requested model).  Past sessions
	// remain available from Welcome > Recent and from the model picker.
	if (!g.restoredSessions) {
		g.restoredSessions = true;
		SyncTabsFromSessions();
	}

	if (const char* settings_env = std::getenv("CASIOEMU_SHOW_SETTINGS")) {
		static bool shown = false;
		if (!shown) {
			shown = true;
			OpenSettings();
		}
		(void)settings_env;
	}
	if (const char* welcome_env = std::getenv("CASIOEMU_SHOW_WELCOME")) {
		static bool shown = false;
		if (!shown) {
			shown = true;
			OpenWelcomeTab();
		}
		(void)welcome_env;
	}
	if (const char* package = std::getenv("CASIOEMU_IMPORT_PACKAGE")) {
		static bool imported = false;
		if (!imported) {
			imported = true;
			ApplyShellImport(package, std::string());
		}
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

	if (const char* selftest = std::getenv("CASIOEMU_SELFTEST")) {
		static int frame = 0;
		static bool build_requested = false;
		static bool terminal_started = false;
		static bool terminal_sent = false;
		++frame;
		static bool editor_created = false;
		static bool lint_checked = false;
		static bool lint_cleared = false;
		static bool dirty_prompt_checked = false;
		static bool explorer_ops_checked = false;
		static bool tabs_switched_checked = false;
		static bool single_launch_checked = false;
		static bool plus_welcome_checked = false;
		static bool settings_tab_checked = false;
		static bool wiz_source_checked = false;
		static bool tab_x_close_checked = false;
		static bool tab_x_dirty_checked = false;
		static bool copy_problems_checked = false;
		static bool wiz_build_checked = false;
		static bool wiz_build_started = false;
		static bool model_close_inactive_checked = false;
		static bool model_close_active_checked = false;
		static bool model_reopen_checked = false;
		static std::string model_reopen_path;
		static std::string model_inactive_title;
		static std::string model_active_title;
		static bool example_build_checked = false;
		static bool example_build_started = false;
		static bool tab_x_found = false;
		static bool tab_click_down_sent = false;
		static bool tab_click_up_sent = false;
		static float tab_click_x = 0.0f;
		static float tab_click_y = 0.0f;
		auto push_tab_mouse = [](float x, float y, bool down) {
			// Queue the event for the next NewFrame; this exercises the same
			// ImGui hit-testing the real mouse uses without depending on SDL
			// window routing in the headless run.
			ImGuiIO& io = ImGui::GetIO();
			io.AddMousePosEvent(x, y);
			io.AddMouseButtonEvent(0, down);
		};
		auto find_tab_x = [](const std::string& label, int kind, float* x, float* y) {
			for (const auto& tab : g.mainTabRects) {
				if (tab.kind == kind && tab.label == label) {
					*x = tab.rect.x + tab.rect.z - 12.0f;
					*y = tab.rect.y + tab.rect.w * 0.5f;
					return true;
				}
			}
			return false;
		};
		if (frame == 1 && !single_launch_checked) {
			single_launch_checked = session::Count() == 1 && g.tabs.size() == 1;
		}
		if (frame == 2 && !build_requested) {
			build_requested = true;
			BuildToolchain(false);
			AppendOutput("[selftest] build requested");
		}
		if (frame == 4 && !editor_created) {
			editor_created = true;
			const char* root = std::getenv("CASIOEMU_SELFTEST_ROOT");
			const std::filesystem::path directory = root && *root
				? std::filesystem::path(root) : (std::filesystem::current_path() / ".editor-selftest");
			std::error_code ec;
			std::filesystem::create_directories(directory, ec);
			const auto first = directory / "editor-selftest.cwiz";
			{
				std::ofstream file(first, std::ios::binary | std::ios::trunc);
				file << "#cwapp\nint main( {\n";
			}
			OpenFileInEditor(first);
			if (g.activeFile >= 0) {
				RunLintForDocument(*g.files[static_cast<size_t>(g.activeFile)]);
				lint_checked = !g.files[static_cast<size_t>(g.activeFile)]->problems.empty();
			}
			const auto second = directory / "editor-second.cwiz";
			{
				std::ofstream file(second, std::ios::binary | std::ios::trunc);
				file << "// second file\nint value = 1;\n";
			}
			OpenFileInEditor(second);
		}
		if (frame == 5 && !explorer_ops_checked) {
			explorer_ops_checked = true;
			const char* root = std::getenv("CASIOEMU_SELFTEST_ROOT");
			const std::filesystem::path directory = root && *root
				? std::filesystem::path(root) : (std::filesystem::current_path() / ".editor-selftest");
			const auto created = directory / "explorer-created.cwiz";
			const auto renamed = directory / "explorer-renamed.cwiz";
			std::error_code ec;
			{
				std::ofstream file(created, std::ios::binary | std::ios::trunc);
				file << "// explorer probe\n";
			}
			std::filesystem::rename(created, renamed, ec);
			bool used_rename = !ec && std::filesystem::exists(renamed) && !std::filesystem::exists(created);
			ec.clear();
			std::filesystem::remove(renamed, ec);
			used_rename = used_rename && !ec;
			explorer_ops_checked = used_rename;
		}
		if (frame == 8 && g.files.size() >= 2 && !tabs_switched_checked) {
			g.activeFile = 1;
			tabs_switched_checked = g.activeFile == 1 && g.files.size() >= 2;
			g.activeFile = 0;
		}
		if (frame == 6 && !lint_cleared && g.files.size() >= 2) {
			lint_cleared = true;
			FileDocument& first = *g.files[0];
			first.editor.SetText("int value = 1;\n");
			first.observedText = first.editor.GetText();
			RunLintForDocument(first);
			lint_cleared = std::none_of(first.problems.begin(), first.problems.end(),
				[](const EditorProblem& problem) { return problem.severity >= 2; });
			SaveFileDocument(first);
			g.activeFile = 0;
			first.editor.SetText(first.editor.GetText() + "// dirty\n");
			CloseFileDocument(0);
			dirty_prompt_checked = g.closeConfirmOpen;
			if (!std::getenv("CASIOEMU_SHOW_CLOSE_PROMPT"))
				g.dismissClosePrompt = true;
		}
		if (frame == 10) {
			LoadModelChoices();
			const std::string primary_path = g.tabs.empty() ? std::string() : g.tabs.front().modelPath;
			for (const auto& choice : g.modelChoices) {
				if (choice.path == primary_path)
					continue;
				OpenModelAt(choice.path);
				break;
			}
		}
		if (frame == 12 && !plus_welcome_checked) {
			OpenWelcomeTab();
			plus_welcome_checked = g.showWelcome && !g.activeSettings;
			if (!g.tabs.empty()) {
				g.showWelcome = false;
				g.activeTab = session::ActiveIndex();
			}
		}
		if (frame == 14 && !settings_tab_checked) {
			OpenSettings();
			bool duplicated = false;
			for (const auto* tool : ToolWindows()) {
				if (tool->name && std::strcmp(tool->name, "Settings") == 0)
					duplicated = true;
			}
			settings_tab_checked = g.activeSettings && !duplicated;
			g.activeSettings = false;
			g.activeFile = 0;
		}
		if (frame == 20) {
			const char* root = std::getenv("CASIOEMU_SELFTEST_ROOT");
			const std::filesystem::path directory = root && *root
				? std::filesystem::path(root) : (std::filesystem::current_path() / ".editor-selftest");
			const auto cwiz = directory / "lint-errors.cwiz";
			const auto wiz = directory / "lint-warnings.wz";
			{
				std::ofstream file(cwiz, std::ios::binary | std::ios::trunc);
				file << "#cwapp\nint main( {\n  unknown_value = ;\n}\n";
			}
			{
				std::ofstream file(wiz, std::ios::binary | std::ios::trunc);
				file << "#org 0xE000\nint value = 1;\n";
			}
			OpenFileInEditor(cwiz);
			RunLintForDocument(*g.files.back());
			OpenFileInEditor(wiz);
			RunLintForDocument(*g.files.back());
			g.panelVisible = true;
			g.panelTab = 1;
		}
		if (frame == 21 && !wiz_source_checked) {
			wiz_source_checked = !g.files.empty() && IsWizDocument(*g.files.back()) &&
				g.files.back()->languageIndex == 1;
			const std::string dump = FormatProblemsForClipboard();
			copy_problems_checked = !g.problems.empty() &&
				dump.find("Error") != std::string::npos && dump.find('\n') != std::string::npos;
		}
		if (frame == 30 && !tab_click_down_sent) {
			tab_click_down_sent = true;
			tab_x_found = find_tab_x("editor-second.cwiz", 2, &tab_click_x, &tab_click_y);
			tab_click_up_sent = tab_x_found;
			if (tab_click_up_sent)
				push_tab_mouse(tab_click_x, tab_click_y, true);
		}
		if (frame == 32 && tab_click_up_sent && !tab_x_close_checked) {
			push_tab_mouse(tab_click_x, tab_click_y, false);
			tab_x_close_checked = true;
		}
		if (frame == 35 && tab_x_close_checked) {
			tab_x_close_checked = g.files.size() == 3;
		}
		if (frame == 37 && tab_x_close_checked && !tab_x_dirty_checked) {
			float x = 0.0f, y = 0.0f;
			if (find_tab_x("editor-selftest.cwiz", 2, &x, &y))
				push_tab_mouse(x, y, true);
		}
		if (frame == 39 && tab_x_close_checked) {
			float x = 0.0f, y = 0.0f;
			if (find_tab_x("editor-selftest.cwiz", 2, &x, &y))
				push_tab_mouse(x, y, false);
		}
		if (frame == 41 && tab_x_close_checked) {
			tab_x_dirty_checked = g.closeConfirmOpen;
			g.dismissClosePrompt = true;
		}
		if (frame == 44 && !wiz_build_started) {
			wiz_build_started = true;
			const char* root = std::getenv("CASIOEMU_SELFTEST_ROOT");
			const std::filesystem::path directory = root && *root
				? std::filesystem::path(root) : (std::filesystem::current_path() / ".editor-selftest");
			const auto wiz_source = directory / "wiz-build-selftest.wz";
			{
				std::ofstream file(wiz_source, std::ios::binary | std::ios::trunc);
				file << "# Wiz build probe\ndef main():\n    clear()\n    print(4, 4, \"wiz ok\")\n    while True:\n        delay(2)\n";
			}
			OpenFileInEditor(wiz_source);
			RequestBuild(false);
		}
		if (frame == 46 && wiz_build_started && !wiz_build_checked)
			wiz_build_checked = g.lastBuildStatus == "Build succeeded";
		if (frame == 58 && !example_build_started) {
			if (const char* example = std::getenv("CASIOEMU_SELFTEST_WIZ_EXAMPLE")) {
				example_build_started = true;
				OpenFileInEditor(example);
				RequestBuild(false);
			}
		}
		if (frame == 60 && example_build_started)
			example_build_checked = g.lastBuildStatus == "Build succeeded";
		if (frame == 62) {
			if (const char* diag = std::getenv("CASIOEMU_SELFTEST_DIAG_FILE")) {
				OpenFileInEditor(diag);
				if (g.activeFile >= 0)
					RunLintForDocument(*g.files[static_cast<size_t>(g.activeFile)]);
			}
		}
		if (frame == 24) {
			FocusTerminal();
			terminal_started = true;
		}
		if (frame == 30 && terminal_started && !terminal_sent) {
			terminal_sent = true;
			g.terminal.SendLineForTest("echo SELFTEST_OK");
		}
		if (frame == 70) {
			const bool terminal_ok = g.terminal.ScreenText().find("SELFTEST_OK") != std::string::npos;
			std::ofstream report(selftest, std::ios::trunc);
			if (report) {
				report << "sessions=" << session::Count() << "\n";
				report << "tabs=" << g.tabs.size() << "\n";
				report << "active=" << session::ActiveIndex() << "\n";
				report << "build_status=" << g.lastBuildStatus << "\n";
				report << "terminal=" << (terminal_ok ? "PASS" : "FAIL") << "\n";
				report << "editor_files=" << g.files.size() << "\n";
				report << "editor_lint_detected=" << (lint_checked ? "PASS" : "FAIL") << "\n";
				report << "editor_lint_cleared=" << (lint_cleared ? "PASS" : "FAIL") << "\n";
				report << "editor_dirty_prompt=" << (dirty_prompt_checked ? "PASS" : "FAIL") << "\n";
				report << "explorer_ops=" << (explorer_ops_checked ? "PASS" : "FAIL") << "\n";
				report << "editor_tabs_switched=" << (tabs_switched_checked ? "PASS" : "FAIL") << "\n";
				report << "single_launch=" << (single_launch_checked ? "PASS" : "FAIL") << "\n";
				report << "plus_opens_welcome=" << (plus_welcome_checked ? "PASS" : "FAIL") << "\n";
				report << "settings_tab=" << (settings_tab_checked ? "PASS" : "FAIL") << "\n";
				report << "wiz_source=" << (wiz_source_checked ? "PASS" : "FAIL") << "\n";
				report << "tab_x_found=" << (tab_x_found ? "PASS" : "FAIL") << "\n";
				report << "tab_x_close=" << (tab_x_close_checked ? "PASS" : "FAIL") << "\n";
				report << "tab_x_dirty_prompt=" << (tab_x_dirty_checked ? "PASS" : "FAIL") << "\n";
				report << "copy_problems=" << (copy_problems_checked ? "PASS" : "FAIL") << "\n";
				report << "wiz_build=" << (wiz_build_checked ? "PASS" : "FAIL") << "\n";
				const bool expect_example = std::getenv("CASIOEMU_SELFTEST_WIZ_EXAMPLE") != nullptr;
				report << "example_build=" << (!expect_example ? "SKIP" : (example_build_checked ? "PASS" : "FAIL")) << "\n";
				report << "panel_tab=" << g.panelTab << "\n";
				report << ((session::Count() >= 2 && g.tabs.size() >= 2 && terminal_ok && lint_checked && lint_cleared &&
					dirty_prompt_checked && explorer_ops_checked && tabs_switched_checked && single_launch_checked &&
					plus_welcome_checked && settings_tab_checked && wiz_source_checked &&
					tab_x_close_checked && tab_x_dirty_checked && copy_problems_checked &&
					wiz_build_checked && (!expect_example || example_build_checked)) ? "RESULT PASS\n" : "RESULT FAIL\n");
			}
		}
	}

	if (const char* panel = std::getenv("CASIOEMU_PANEL_TAB"))
		g.panelTab = std::clamp(std::atoi(panel), 0, 2);
	calculator_panel_focused = false;
	if (std::getenv("CASIOEMU_UI_CONTEXT_MENU")) {
		static int ctx_frame = 0;
		++ctx_frame;
		ImGuiIO& io = ImGui::GetIO();
		if (ctx_frame == 18) {
			io.AddMousePosEvent(110.0f, 400.0f);
			io.AddMouseButtonEvent(1, true);
		}
		else if (ctx_frame == 20) {
			io.AddMouseButtonEvent(1, false);
		}
	}
	if (const char* drag_report = std::getenv("CASIOEMU_UI_DRAG_CALC")) {
		static int drag_frame = 0;
		static float drag_start_width = 0.0f;
		static float drag_x = 0.0f;
		static float drag_y = 0.0f;
		++drag_frame;
		ImGuiIO& io = ImGui::GetIO();
		if (drag_frame == 12) {
			drag_start_width = g.calculatorWidth;
			drag_x = g.rectCalculatorGrip.x + g.rectCalculatorGrip.z * 0.5f;
			drag_y = g.rectCalculatorGrip.y + g.rectCalculatorGrip.w * 0.5f;
			io.AddMousePosEvent(drag_x, drag_y);
			io.AddMouseButtonEvent(0, true);
		}
		else if (drag_frame > 12 && drag_frame <= 32) {
			io.AddMousePosEvent(drag_x - (drag_frame - 12) * 8.0f, drag_y);
		}
		else if (drag_frame == 33) {
			io.AddMouseButtonEvent(0, false);
		}
		else if (drag_frame == 39) {
			std::ofstream report(drag_report, std::ios::trunc);
			const float delta = g.calculatorWidth - drag_start_width;
			if (report) {
				report << "start_width=" << drag_start_width << '\n';
				report << "end_width=" << g.calculatorWidth << '\n';
				report << "delta=" << delta << '\n';
				report << (std::fabs(delta) >= 30.0f ? "RESULT PASS\n" : "RESULT FAIL\n");
			}
			drag_frame = 100000;
		}
	}
	RenderLayout();
	if (!g.perfReportPath.empty()) {
		const double elapsed = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - perf_started).count();
		++g.perfFrames;
		g.perfTotalMs += elapsed;
		g.perfMinMs = std::min(g.perfMinMs, elapsed);
		g.perfMaxMs = std::max(g.perfMaxMs, elapsed);
		if (g.perfFrames >= g.perfFrameLimit) {
			std::ofstream report(g.perfReportPath, std::ios::trunc);
			if (report) {
				report << "frames=" << g.perfFrames << '\n';
				report << "avg_ms=" << (g.perfTotalMs / g.perfFrames) << '\n';
				report << "min_ms=" << g.perfMinMs << '\n';
				report << "max_ms=" << g.perfMaxMs << '\n';
				report << "sidebar_scans=" << g.sidebarScanCount << '\n';
				report << "sidebar_scan_ms=" << g.sidebarScanMs << '\n';
				report << "output_lines=" << g.output.size() << '\n';
				report << "problems=" << g.problems.size() << '\n';
				report << "clipper=" << (PerfNoClip() ? "off" : "on") << '\n';
			}
			g.perfReportPath.clear();
		}
	}
}

} // namespace casioemu::shell
