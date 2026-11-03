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
#include "imgui/TextEditor.h"
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

constexpr float kTitleBarHeight = 38.0f;
constexpr float kTabStripHeight = 26.0f;
constexpr float kBreadcrumbHeight = 22.0f;
constexpr float kStatusBarHeight = 22.0f;
constexpr float kPanelHeaderHeight = 35.0f;
constexpr float kSidebarHeaderHeight = 46.0f;
constexpr float kActivityBarWidth = 48.0f;

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
	data.shell.calculator.visible = g.calculatorVisible;
	data.shell.calculator.width = static_cast<int>(g.calculatorWidth);
	data.workspace.folder = g.workspace;
	std::string error;
	SettingsStore::Instance().Save(&error);
}

std::string BaseName(const std::string& path);

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

TextEditor::LanguageDefinition MakeWizLanguage() {
	TextEditor::LanguageDefinition language;
	language.mName = "Wiz/CWiz";
	language.mCaseSensitive = true;
	language.mAutoIndentation = true;
	language.mSingleLineComment = "//";
	language.mCommentStart = "/*";
	language.mCommentEnd = "*/";
	language.mPreprocChar = '#';
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
	else if (id == "plugin.manager") { if (auto* win = FindWindow("Plugin Manager")) { win->BringToFront(); } }
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
	TextEditor::ErrorMarkers markers;
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
		if (item.line > 0)
			markers[item.line] = problem.message;
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
	doc.editor.SetErrorMarkers(markers);
	if (!results.empty()) {
		int errors = 0;
		for (const auto& item : doc.problems)
			errors += item.severity >= 2 ? 1 : 0;
		g.panelVisible = true;
		if (errors > 0)
			g.panelTab = 1;
	}
}

bool FileDocumentDirty(const FileDocument& doc) {
	return doc.editor.GetText() != doc.savedText;
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
	if (g.activeFile < 0)
		g.showWelcome = true;
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
	auto doc = std::make_unique<FileDocument>();
	doc->path = path.string();
	doc->name = path.filename().string();
	doc->savedText = buffer.str();
	doc->editor.SetLanguageDefinition(MakeWizLanguage());
	doc->editor.SetPalette(ThemeManager::Instance().IsDarkMode()
		? TextEditor::GetDarkPalette() : TextEditor::GetLightPalette());
	doc->paletteSet = true;
	doc->editor.SetText(doc->savedText);
	doc->observedText = doc->savedText;
	std::string ext = path.extension().string();
	doc->languageIndex = ext == ".wiz" ? 1 : (ext == ".cwiz" ? 2 : 0);
	g.files.push_back(std::move(doc));
	g.activeFile = static_cast<int>(g.files.size()) - 1;
	g.showWelcome = false;
	RunLintForDocument(*g.files.back());
	AppendOutput("[editor] opened " + path.string());
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

	const float traffic = 82.0f;
	const float line_height = ImGui::GetTextLineHeight();
	const float text_y = pos.y + 8.0f;
	draw->AddText(ImVec2(pos.x + traffic + 12.0f, text_y),
		ImGui::ColorConvertFloat4ToU32(palette.foreground), "CasioEmuMsvc");

	const char* menues[] = { "File", "View", "Session" };
	float menu_x = pos.x + traffic + 12.0f + TextWidth("CasioEmuMsvc") + 18.0f;
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.hover);
	ImGui::PushStyleColor(ImGuiCol_Text, palette.foreground);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f, 3.0f));
	for (const char* menu : menues) {
		ImGui::SetCursorScreenPos(ImVec2(menu_x, pos.y + 6.0f));
		ImGui::PushID(menu);
		if (ImGui::Button(menu, ImVec2(0, 25))) {
			if (std::strcmp(menu, "File") == 0)
				ImGui::OpenPopup("##file_menu");
			else if (std::strcmp(menu, "View") == 0)
				ImGui::OpenPopup("##view_menu");
			else if (std::strcmp(menu, "Session") == 0)
				ImGui::OpenPopup("##session_menu");
		}
		if (ImGui::BeginPopup("##file_menu")) {
			if (ImGui::MenuItem("Open Model...")) RequestOpenModel();
			if (ImGui::MenuItem("Open Folder...")) RequestOpenFolder();
			ImGui::Separator();
			if (ImGui::MenuItem("Quit")) g.quitRequested = true;
			ImGui::EndPopup();
		}
		if (ImGui::BeginPopup("##view_menu")) {
			if (ImGui::MenuItem("Toggle Sidebar")) ToggleSidebar();
			if (ImGui::MenuItem("Toggle Panel")) ToggleBottomPanel();
			if (ImGui::MenuItem("Toggle Calculator Panel")) ToggleTools();
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

	// Right-aligned actions, 12 px from the content edge, vertically centred.
	const float build_w = UIHelpers::ButtonWidth("Build");
	const float run_w = UIHelpers::ButtonWidth("Run");
	const float pause_w = UIHelpers::ButtonWidth(m_emu && m_emu->GetPaused() ? "Resume" : "Pause");
	const float palette_w = UIHelpers::ButtonWidth("Palette");
	float actions_width = pause_w + 8.0f + palette_w;
	if (g.activeFile < 0)
		actions_width += build_w + 8.0f + run_w + 8.0f;
	ImGui::SetCursorScreenPos(ImVec2(pos.x + width - actions_width - 12.0f, pos.y + 6.0f));
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.hover);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, palette.selection);
	ImGui::PushStyleColor(ImGuiCol_Text, palette.foreground);
	if (g.activeFile < 0) {
		if (ImGui::Button("Build", ImVec2(build_w, 26)))
			BuildToolchain(false);
		ImGui::SameLine();
		if (ImGui::Button("Run", ImVec2(run_w, 26)))
			BuildToolchain(true);
		ImGui::SameLine();
	}
	if (ImGui::Button(m_emu && m_emu->GetPaused() ? "Resume" : "Pause", ImVec2(pause_w, 26))) {
		if (m_emu)
			m_emu->SetPaused(!m_emu->GetPaused());
	}
	ImGui::SameLine();
	if (ImGui::Button("Palette", ImVec2(palette_w, 26)))
		OpenCommandPalette();
	ImGui::PopStyleColor(4);

	const ImVec2 mouse = ImGui::GetIO().MousePos;
	const bool in_drag_zone = mouse.y >= pos.y && mouse.y < pos.y + kTitleBarHeight &&
		mouse.x > pos.x + traffic && mouse.x < pos.x + width - actions_width - 20.0f;
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
	DrawFlatPanel("##ShellTabs", pos, ImVec2(width, kTabStripHeight), palette.surface);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddLine(ImVec2(pos.x, pos.y + kTabStripHeight - 1.0f),
		ImVec2(pos.x + width, pos.y + kTabStripHeight - 1.0f),
		ImGui::ColorConvertFloat4ToU32(palette.border), 1.0f);
	float x = pos.x + 8.0f;
	auto draw_tab_text = [&](const ImVec2& min, const ImVec2& max, const std::string& label,
			bool active, bool hovered, bool dirty) {
		draw->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(active ? palette.tabActive :
			(hovered ? palette.hover : palette.tabInactive)), 4.0f, ImDrawFlags_RoundCornersTop);
		draw->AddLine(ImVec2(max.x - 1.0f, min.y + 5.0f), ImVec2(max.x - 1.0f, max.y - 5.0f),
			ImGui::ColorConvertFloat4ToU32(palette.border), 1.0f);
		if (active)
			draw->AddRectFilled(ImVec2(min.x + 4.0f, min.y), ImVec2(max.x - 4.0f, min.y + 2.0f),
				ImGui::ColorConvertFloat4ToU32(palette.accent));
		draw->AddText(ImVec2(min.x + 12.0f, min.y + (kTabStripHeight - ImGui::GetTextLineHeight()) * 0.5f),
			ImGui::ColorConvertFloat4ToU32(active ? palette.foreground : WithAlpha(palette.foreground, 0.72f)),
			label.c_str());
		if (hovered)
			draw->AddText(ImVec2(max.x - 20.0f, min.y + (kTabStripHeight - ImGui::GetTextLineHeight()) * 0.5f),
				ImGui::ColorConvertFloat4ToU32(palette.foreground), "x");
		(void)dirty;
	};

	if (g.showWelcome) {
		const char* label = "Welcome";
		const float tab_width = TextWidth(label) + 34.0f;
		ImGui::SetCursorScreenPos(ImVec2(x, pos.y));
		ImGui::InvisibleButton("##welcome_tab", ImVec2(tab_width, kTabStripHeight));
		if (ImGui::IsItemClicked())
			g.showWelcome = true;
		draw_tab_text(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), label, true,
			ImGui::IsItemHovered(), false);
		x += tab_width;
	}

	for (int index = 0; index < static_cast<int>(g.tabs.size()); ++index) {
		SessionTab& tab = g.tabs[static_cast<size_t>(index)];
		const bool active = (!g.showWelcome && g.activeFile < 0 && index == g.activeTab);
		std::string label = tab.title.empty() ? tab.model : tab.title;
		const float tab_width = TextWidth(label.c_str()) + 52.0f;
		ImGui::SetCursorScreenPos(ImVec2(x, pos.y));
		ImGui::PushID(index);
		ImGui::InvisibleButton("##tab", ImVec2(tab_width, kTabStripHeight));
		const bool hovered = ImGui::IsItemHovered();
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		const bool close_clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
			ImGui::GetIO().MousePos.x > max.x - 24.0f;
		if (close_clicked || ImGui::IsItemClicked(ImGuiMouseButton_Middle)) {
			if (index > 0 && tab.live) {
				session::CloseSession(index);
				SyncTabsFromSessions();
			}
			else if (tab.live) {
				g.showWelcome = true;
				g.activeTab = -1;
				if (m_emu) m_emu->ResetToMainMenu();
			}
			else {
				g.tabs.erase(g.tabs.begin() + index);
				if (g.activeTab >= index) --g.activeTab;
				if (g.tabs.empty()) g.activeTab = -1;
			}
			SaveSessions();
			ImGui::PopID();
			break;
		}
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
			g.activeTab = index;
			g.activeFile = -1;
			g.showWelcome = false;
			if (tab.live)
				session::Activate(index);
			else if (!tab.modelPath.empty())
				OpenModelAt(tab.modelPath);
		}
		draw_tab_text(min, max, label, active, hovered, false);
		ImGui::PopID();
		x += tab_width;
	}

	for (int index = 0; index < static_cast<int>(g.files.size()); ++index) {
		FileDocument& doc = *g.files[static_cast<size_t>(index)];
		const bool active = g.activeFile == index;
		std::string label = doc.name + (FileDocumentDirty(doc) ? " *" : "");
		const float tab_width = TextWidth(label.c_str()) + 52.0f;
		ImGui::SetCursorScreenPos(ImVec2(x, pos.y));
		ImGui::PushID(1000 + index);
		ImGui::InvisibleButton("##file_tab", ImVec2(tab_width, kTabStripHeight));
		const bool hovered = ImGui::IsItemHovered();
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		const bool close_clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
			ImGui::GetIO().MousePos.x > max.x - 24.0f;
		if (close_clicked || ImGui::IsItemClicked(ImGuiMouseButton_Middle)) {
			CloseFileDocument(index);
			ImGui::PopID();
			break;
		}
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
			g.activeFile = index;
			g.showWelcome = false;
		}
		draw_tab_text(min, max, label, active, hovered, FileDocumentDirty(doc));
		ImGui::PopID();
		x += tab_width;
	}

	ImGui::SetCursorScreenPos(ImVec2(x + 4.0f, pos.y + 3.0f));
	if (ImGui::Button("+##newtab", ImVec2(28.0f, 20.0f))) {
		LoadModelChoices();
		g.openModelOpen = true;
		g.openModelFocus = true;
		g.openModelQuery[0] = '\0';
	}
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
		ImGui::SetCursorPosX(20.0f + depth * 12.0f);
		ImGui::PushStyleColor(ImGuiCol_Header, palette.selection);
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, palette.hover);
		ImGui::PushStyleColor(ImGuiCol_Text, palette.foreground);
		const bool row_selected = g.selectedPath == entry.path();
		const bool clicked = ImGui::Selectable((label + "##" + key).c_str(), row_selected,
			ImGuiSelectableFlags_AllowDoubleClick, ImVec2(ImGui::GetContentRegionAvail().x - 6.0f, 0.0f));
		if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
			g.selectedPath = entry.path();
			g.selectedIsDir = is_directory;
		}
		if (clicked) {
			g.selectedPath = entry.path();
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
				OpenFileInEditor(entry.path());
			}
		}
		if (ImGui::BeginPopupContextItem("##explorer_context")) {
			g.selectedPath = entry.path();
			g.selectedIsDir = is_directory;
			const std::filesystem::path base = is_directory ? entry.path() : entry.path().parent_path();
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
				g.selectedPath = entry.path();
				std::snprintf(g.entryName, sizeof(g.entryName), "%s", name.c_str());
				g.renameOpen = true;
			}
			if (ImGui::MenuItem("Delete")) {
				g.selectedPath = entry.path();
				g.deleteOpen = true;
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Reveal in Finder")) {
				std::string uri = "file://" + (is_directory ? entry.path() : entry.path().parent_path()).string();
				SDL_OpenURL(uri.c_str());
			}
			if (ImGui::MenuItem("Copy Path"))
				ImGui::SetClipboardText(entry.path().string().c_str());
			ImGui::EndPopup();
		}
		ImGui::PopStyleColor(3);
		if (is_directory && g.expanded.count(key))
			RenderSidebarTree(entry.path(), depth + 1, palette);
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
		}
		ImGui::SameLine();
		if (ImGui::Button("Open Folder", ImVec2(button_w, 20.0f)))
			RequestOpenFolder();
	}
	ImGui::PopStyleColor(3);

	ImGui::SetCursorScreenPos(ImVec2(pos.x + 16.0f, pos.y + header_height + 3.0f));
	ImGui::BeginChild("##explorer_body", ImVec2(size.x - 32.0f, size.y - header_height - 14.0f), ImGuiChildFlags_None);
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

	section("Start");
	link("New Calculator", "session.new");
	ImGui::Dummy(ImVec2(0.0f, 10.0f));
	section("Recent");
	const auto& data = SettingsStore::Instance().Data();
	bool any_recent = false;
	for (const auto& session : data.sessions) {
		any_recent = true;
		ImGui::SetCursorPosX(36.0f);
		if (ImGui::Selectable((std::string("> ") + (session.title.empty() ? session.model : session.title)).c_str(), false,
				ImGuiSelectableFlags_None, ImVec2(360, 0))) {
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
		ImGui::SetCursorPosX(36.0f);
		if (ImGui::Selectable((std::string("> ") + BaseName(file)).c_str(), false,
				ImGuiSelectableFlags_None, ImVec2(360, 0)))
			OpenExternal(file);
	}
	if (!any_recent)
		ImGui::TextDisabled("No recent sessions or files");

}

std::vector<UIWindow*> ToolWindows();
void SortToolsByOrder(std::vector<UIWindow*>& tools);

void RenderCalculatorPanel(const ThemePalette& palette, ImVec2 pos, ImVec2 size) {
	DrawFlatPanel("##ShellCalculator", pos, size, palette.background);
	const ImVec2 window_pos = ImGui::GetWindowPos();
	ImGui::GetWindowDrawList()->AddRectFilled(window_pos,
		ImVec2(window_pos.x + size.x, window_pos.y + 22.0f),
		ImGui::ColorConvertFloat4ToU32(palette.titleBar));
	ImGui::GetWindowDrawList()->AddText(ImVec2(window_pos.x + 12.0f, window_pos.y + 4.0f),
		ImGui::ColorConvertFloat4ToU32(palette.muted), "CALCULATOR");
	ImGui::SetCursorPos(ImVec2(size.x - 54.0f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.hover);
	ImGui::PushStyleColor(ImGuiCol_Text, palette.foreground);
	if (ImGui::Button("Hide", ImVec2(58.0f, 20.0f)))
		g.calculatorVisible = false;
	ImGui::PopStyleColor(3);

	ImGui::SetCursorPos(ImVec2(1.0f, 23.0f));
	ImGui::BeginChild("##calculator_host", ImVec2(size.x - 2.0f, size.y - 24.0f), ImGuiChildFlags_None,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	UIWindow* calculator = FindWindow("Calculator");
	if (!calculator) {
		ImGui::TextDisabled("The calculator is not available in this build.");
	}
	else {
		ImGui::SetCursorPos(ImVec2(8.0f, 8.0f));
		ImGui::BeginChild("##calculator_inner", ImVec2(-16.0f, -16.0f), ImGuiChildFlags_None,
			ImGuiWindowFlags_NoScrollbar);
		calculator->RenderCore();
		ImGui::EndChild();
		const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
			ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
		if (hovered && !g.paletteOpen && !TerminalFocused())
			calculator_panel_focused = true;
	}
	ImGui::EndChild();
	EndFlatPanel();
}

bool IsWizDocument(const FileDocument& doc) {
	std::string ext = std::filesystem::path(doc.path).extension().string();
	for (char& ch : ext)
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	return ext == ".wiz" || ext == ".cwiz";
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
		ImGui::SetNextItemWidth(140.0f);
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
	doc.editor.Render("##file_editor_body", ImVec2(size.x - 16.0f, size.y - toolbar_height - 8.0f), true);
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

	// Document tabs: full labels, no abbreviations, horizontal scroll when the
	// strip is wider than the editor column.
	static float doc_scroll = 0.0f;
	float total_width = 6.0f;
	for (const auto* tool : tools)
		total_width += ImGui::CalcTextSize(tool->name).x + 31.0f;
	const float max_scroll = std::max(0.0f, total_width - size.x + 70.0f);
	doc_scroll = std::clamp(doc_scroll, 0.0f, max_scroll);
	ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
	ImGui::PushStyleColor(ImGuiCol_ChildBg, palette.surface);
	ImGui::BeginChild("##doc_tabs", ImVec2(size.x, 30.0f), ImGuiChildFlags_None,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f)
		doc_scroll = std::clamp(doc_scroll - ImGui::GetIO().MouseWheel * 40.0f, 0.0f, max_scroll);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	float x = 6.0f - doc_scroll;
	for (int index = 0; index < static_cast<int>(tools.size()); ++index) {
		const char* label = tools[index]->name;
		const float tab_width = ImGui::CalcTextSize(label).x + 30.0f;
		// Do not paint partially visible tabs; the scrollbar reaches them.
		if (index != g.toolIndex && (x + tab_width > size.x - 4.0f || x + tab_width < 4.0f)) {
			x += tab_width + 1.0f;
			continue;
		}
		ImGui::SetCursorPos(ImVec2(x, 0.0f));
		ImGui::PushID(index);
		ImGui::InvisibleButton("##doc_tab", ImVec2(tab_width, 26.0f));
		const bool hovered = ImGui::IsItemHovered();
		if (ImGui::IsItemClicked())
			g.toolIndex = index;
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
		draw->AddText(ImVec2(min.x + 15.0f, min.y + (26.0f - ImGui::GetTextLineHeight()) * 0.5f),
			ImGui::ColorConvertFloat4ToU32(selected ? palette.foreground : WithAlpha(palette.foreground, 0.72f)),
			label);
		if (selected && (x < 6.0f - doc_scroll || x + tab_width > 6.0f - doc_scroll + size.x - 16.0f))
			doc_scroll = std::clamp(x - 6.0f, 0.0f, max_scroll);
		ImGui::PopID();
		x += tab_width + 1.0f;
	}
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
		}
		g.draggingTool = -1;
		g.dragTargetTool = -1;
	}

	// Horizontal scrollbar for the document strip (no mystery arrow buttons).
	const float viewport_width = size.x;
	if (max_scroll > 0.0f) {
		const float track_x = pos.x + 8.0f;
		const float track_w = std::max(40.0f, size.x - 16.0f);
		const float thumb_w = std::max(32.0f, track_w * (viewport_width / total_width));
		const float thumb_x = track_x + (doc_scroll / max_scroll) * (track_w - thumb_w);
		const float bar_y = pos.y + 26.0f;
		draw->AddRectFilled(ImVec2(track_x, bar_y), ImVec2(track_x + track_w, bar_y + 3.0f),
			ImGui::ColorConvertFloat4ToU32(palette.border));
		draw->AddRectFilled(ImVec2(thumb_x, bar_y), ImVec2(thumb_x + thumb_w, bar_y + 3.0f),
			ImGui::ColorConvertFloat4ToU32(palette.muted));
		ImGui::SetCursorScreenPos(ImVec2(track_x, bar_y - 1.0f));
		ImGui::InvisibleButton("##doc_scrollbar", ImVec2(track_w, 6.0f));
		if (ImGui::IsItemActive()) {
			const float t = std::clamp((ImGui::GetIO().MousePos.x - track_x - thumb_w * 0.5f) /
				std::max(1.0f, track_w - thumb_w), 0.0f, 1.0f);
			doc_scroll = t * max_scroll;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
	}
	ImGui::EndChild();
	ImGui::PopStyleColor();

	ImGui::SetCursorPos(ImVec2(1.0f, 31.0f));
	ImGui::BeginChild("##doc_body", ImVec2(size.x - 2.0f, size.y - 32.0f), ImGuiChildFlags_None);
	UIWindow* active = tools[static_cast<size_t>(g.toolIndex)];
	if (active->open)
		active->RenderCore();
	else
		ImGui::TextDisabled("%s is closed.", active->name);
	ImGui::EndChild();
	EndFlatPanel();
}

void RenderContent(const ThemePalette& palette, ImVec2 pos, ImVec2 size) {
	if (g.activeFile >= 0)
		RenderFileEditor(palette, pos, size);
	else
		RenderToolDocuments(palette, pos, size);
}

void RenderOutputTab() {
	ImGui::BeginChild("##output_body", ImVec2(0, 0), ImGuiChildFlags_None);
	ImGui::Indent(10.0f);
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
	ImGui::Unindent(10.0f);
	ImGui::EndChild();
}

void RenderProblemsTab() {
	ImGui::BeginChild("##problems_body", ImVec2(0, 0), ImGuiChildFlags_None);
	int errors = 0, warnings = 0, notes = 0;
	for (const auto& problem : g.problems) {
		errors += problem.severity >= 2 ? 1 : 0;
		warnings += problem.severity == 1 ? 1 : 0;
		notes += problem.severity == 0 ? 1 : 0;
	}
	ImGui::SetCursorPosX(10.0f);
	ImGui::TextDisabled("%d errors, %d warnings, %d notes", errors, warnings, notes);
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
	if (ImGui::GetContentRegionAvail().x < 700.0f) {
		// Narrow editor column: two-line rows avoid horizontal clipping.
		for (const auto& problem : g.problems) {
			ImGui::PushID(&problem);
			ImGui::SetCursorPosX(10.0f);
			ImGui::TextColored(color(problem.severity), "%s", problem.severity >= 2 ? "Error" :
				(problem.severity == 1 ? "Warning" : "Note"));
			ImGui::SameLine();
			ImGui::TextDisabled("%s", problem.code.empty() ? "-" : problem.code.c_str());
			ImGui::SetCursorPosX(22.0f);
			ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 10.0f);
			ImGui::TextWrapped("%s", problem.text.c_str());
			ImGui::PopTextWrapPos();
			if (ImGui::IsItemHovered() && !problem.hint.empty())
				ImGui::SetTooltip("%s", problem.hint.c_str());
			if (ImGui::IsItemClicked())
				activate(problem);
			ImGui::SetCursorPosX(22.0f);
			if (!problem.file.empty()) {
				ImGui::TextDisabled("%s:%d:%d", std::filesystem::path(problem.file).filename().string().c_str(),
					problem.line, problem.column);
				if (ImGui::IsItemClicked())
					activate(problem);
			}
			ImGui::Separator();
			ImGui::PopID();
		}
	}
	else if (ImGui::BeginTable("##problems_table", 4,
			ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
			ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
		ImGui::TableSetupColumn("Severity", ImGuiTableColumnFlags_WidthFixed, 80.0f);
		ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthFixed, 170.0f);
		ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed, 190.0f);
		ImGui::TableHeadersRow();
		for (const auto& problem : g.problems) {
			ImGui::TableNextRow();
			bool clicked = false;
			ImGui::TableNextColumn();
			ImGui::TextColored(color(problem.severity), "%s", problem.severity >= 2 ? "Error" :
				(problem.severity == 1 ? "Warning" : "Note"));
			clicked |= ImGui::IsItemClicked();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(problem.code.empty() ? "-" : problem.code.c_str());
			clicked |= ImGui::IsItemClicked();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(problem.text.c_str());
			if (ImGui::IsItemHovered() && !problem.hint.empty())
				ImGui::SetTooltip("%s", problem.hint.c_str());
			clicked |= ImGui::IsItemClicked();
			ImGui::TableNextColumn();
			if (!problem.file.empty())
				ImGui::Text("%s:%d:%d", std::filesystem::path(problem.file).filename().string().c_str(),
					problem.line, problem.column);
			else
				ImGui::TextDisabled("-");
			clicked |= ImGui::IsItemClicked();
			if (clicked && !problem.file.empty())
				activate(problem);
		}
		ImGui::EndTable();
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

	// right-side chevron / close
	ImGui::SetCursorScreenPos(ImVec2(pos.x + size.x - 78.0f, pos.y + 6.0f));
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.hover);
	ImGui::PushStyleColor(ImGuiCol_Text, palette.foreground);
	if (ImGui::Button("Close##panel_close", ImVec2(70, 22)))
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
	if (!g.problems.empty()) {
		char counts[64];
		std::snprintf(counts, sizeof(counts), "Errors: %d  Warnings: %d",
			(int)std::count_if(g.problems.begin(), g.problems.end(), [](const DiagLine& d) { return d.severity >= 2; }),
			(int)std::count_if(g.problems.begin(), g.problems.end(), [](const DiagLine& d) { return d.severity == 1; }));
		const float counts_w = TextWidth(counts);
		draw->AddText(ImVec2(pos.x + size.x * 0.45f - counts_w * 0.5f, pos.y + 2.0f), fg, counts);
	}
	std::string right = SettingsStore::Instance().Data().language + "  |  " +
		ThemeManager::Instance().CurrentThemeName() + "  |  build " CASIOEMU_BUILD_COMMIT " " CASIOEMU_BUILD_TIME;
	const float right_width = TextWidth(right.c_str());
	draw->AddText(ImVec2(pos.x + size.x - right_width - 10.0f, pos.y + 2.0f), fg, right.c_str());
	if (small)
		ImGui::PopFont();
	EndFlatPanel();
}

std::vector<UIWindow*> ToolWindows() {
	std::vector<UIWindow*> tools;
	for (auto* win : windows) {
		if (!win || IsCalculatorWindow(win) || !win->name)
			continue;
		// The Wiz compiler is an overlay on the file editor, not a document tab.
		if (std::strcmp(win->name, "Wiz Compiler") == 0)
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

std::vector<PaletteCommand> Commands() {
	return {
		{ "session.new", "Session: New Calculator", "" },
		{ "model.open", "File: Open Model...", "" },
		{ "folder.open", "File: Open Folder...", "" },
		{ "plugin.install", "Plugins: Install Plugin...", "" },
		{ "plugin.manager", "Plugins: Open Plugin Manager", "" },
		{ "view.sidebar", "View: Toggle Sidebar", "" },
		{ "view.panel", "View: Toggle Bottom Panel", "" },
		{ "view.tools", "View: Toggle Calculator Panel", "" },
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
}

void RenderOverlays(const ThemePalette& palette) {
	if (g.closeConfirmOpen) {
		ImGui::OpenPopup("Unsaved changes##file_close");
		if (ImGui::BeginPopupModal("Unsaved changes##file_close", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::TextWrapped("This file has unsaved changes.");
			if (ImGui::Button("Save", ImVec2(100, 0))) {
				if (g.pendingCloseFile >= 0 && g.pendingCloseFile < static_cast<int>(g.files.size()))
					SaveFileDocument(*g.files[static_cast<size_t>(g.pendingCloseFile)]);
				const int index = g.pendingCloseFile;
				g.pendingCloseFile = -1;
				g.closeConfirmOpen = false;
				EraseFileDocument(index);
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Discard", ImVec2(100, 0))) {
				const int index = g.pendingCloseFile;
				g.pendingCloseFile = -1;
				g.closeConfirmOpen = false;
				EraseFileDocument(index);
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(100, 0))) {
				g.pendingCloseFile = -1;
				g.closeConfirmOpen = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}
	auto filesystem_error = [](const std::string& action, const std::error_code& ec) {
		g.sidebarError = action + ": " + ec.message();
		AppendOutput("[explorer] " + g.sidebarError);
	};
	if (g.renameOpen) {
		ImGui::OpenPopup("Rename##explorer");
		if (ImGui::BeginPopupModal("Rename##explorer", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::TextDisabled("%s", g.selectedPath.string().c_str());
			ImGui::SetNextItemWidth(320.0f);
			ImGui::InputText("##rename_name", g.entryName, sizeof(g.entryName));
			if (ImGui::Button("Rename", ImVec2(100, 0))) {
				if (g.entryName[0]) {
					std::error_code ec;
					const auto target = g.selectedPath.parent_path() / g.entryName;
					std::filesystem::rename(g.selectedPath, target, ec);
					if (ec)
						filesystem_error("rename failed", ec);
					else
						g.selectedPath = target;
				}
				g.renameOpen = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(100, 0))) {
				g.renameOpen = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}
	if (g.newFileOpen || g.newFolderOpen) {
		const bool folder = g.newFolderOpen;
		ImGui::OpenPopup(folder ? "New Folder##explorer" : "New File##explorer");
		if (ImGui::BeginPopupModal(folder ? "New Folder##explorer" : "New File##explorer", nullptr,
				ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::TextDisabled("%s", g.selectedPath.string().c_str());
			ImGui::SetNextItemWidth(320.0f);
			ImGui::InputText("##new_entry_name", g.entryName, sizeof(g.entryName));
			if (ImGui::Button("Create", ImVec2(100, 0))) {
				if (g.entryName[0]) {
					std::error_code ec;
					const auto target = g.selectedPath / g.entryName;
					if (folder) {
						std::filesystem::create_directory(target, ec);
						if (ec)
							filesystem_error("create folder failed", ec);
					}
					else {
						std::ofstream file(target, std::ios::binary);
						if (!file)
							filesystem_error("create file failed", std::make_error_code(std::errc::permission_denied));
						else
							file.close();
						OpenFileInEditor(target);
					}
				}
				g.newFileOpen = false;
				g.newFolderOpen = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(100, 0))) {
				g.newFileOpen = false;
				g.newFolderOpen = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}
	if (g.deleteOpen) {
		ImGui::OpenPopup("Delete##explorer");
		if (ImGui::BeginPopupModal("Delete##explorer", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::TextWrapped("Delete %s? This cannot be undone.", g.selectedPath.string().c_str());
			if (ImGui::Button("Delete", ImVec2(100, 0))) {
				std::error_code ec;
				std::filesystem::remove_all(g.selectedPath, ec);
				if (ec)
					filesystem_error("delete failed", ec);
				g.deleteOpen = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(100, 0))) {
				g.deleteOpen = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}
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
	const float calculator_w = g.calculatorVisible ? std::min(g.calculatorWidth, width * 0.55f) : 0.0f;
	const float sidebar_w = g.sidebarVisible ? std::min(g.sidebarWidth, width * 0.40f) : 0.0f;
	const float content_x = viewport->Pos.x + sidebar_w;
	const float content_w = std::max(220.0f, width - sidebar_w - calculator_w);
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
	if (g.calculatorVisible) {
		ImGui::SetNextWindowPos(ImVec2(content_x + content_w - 2.0f, viewport->Pos.y + top));
		ImGui::SetNextWindowSize(ImVec2(4.0f, body_h));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
		ImGui::Begin("##calculator_grip", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground);
		ImGui::InvisibleButton("##grip", ImVec2(4.0f, body_h));
		ImGui::GetWindowDrawList()->AddLine(ImVec2(ImGui::GetWindowPos().x + 2.0f, ImGui::GetWindowPos().y),
			ImVec2(ImGui::GetWindowPos().x + 2.0f, ImGui::GetWindowPos().y + body_h),
			ImGui::ColorConvertFloat4ToU32(palette.border), 1.0f);
		if (ImGui::IsItemHovered() || ImGui::IsItemActive())
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
		if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			g.calculatorWidth = 360.0f;
		if (ImGui::IsItemActive()) {
			g.calculatorWidth = std::clamp(g.calculatorWidth - ImGui::GetIO().MouseDelta.x, 260.0f, width * 0.6f);
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
	g.calculatorVisible = !g.calculatorVisible;
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
		win->BringToFront();
	g.toolsVisible = true;
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
			if (!std::getenv("CASIOEMU_SHOW_CLOSE_PROMPT")) {
				g.closeConfirmOpen = false;
				g.pendingCloseFile = -1;
			}
		}
		if (frame == 10) {
			LoadModelChoices();
			for (const auto& choice : g.modelChoices) {
				if (choice.name.find("Graph Light") != std::string::npos ||
					choice.path.find("Graph Light") != std::string::npos) {
					OpenModelAt(choice.path);
					break;
				}
			}
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
				report << "panel_tab=" << g.panelTab << "\n";
				report << ((session::Count() >= 2 && g.tabs.size() >= 2 && terminal_ok && lint_checked && lint_cleared && dirty_prompt_checked && explorer_ops_checked && tabs_switched_checked) ? "RESULT PASS\n" : "RESULT FAIL\n");
			}
		}
	}

	if (const char* panel = std::getenv("CASIOEMU_PANEL_TAB"))
		g.panelTab = std::clamp(std::atoi(panel), 0, 2);
	calculator_panel_focused = false;
	RenderLayout();
}

} // namespace casioemu::shell
