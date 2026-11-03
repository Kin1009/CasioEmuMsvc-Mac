#include "Settings.h"

#include "../../../McpPlugin/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace casioemu::settings {
namespace {

using json = nlohmann::json;

const json* ObjectAt(const json& parent, const char* key) {
	auto it = parent.find(key);
	if (it == parent.end() || !it->is_object())
		return nullptr;
	return &*it;
}

const json* ArrayAt(const json& parent, const char* key) {
	auto it = parent.find(key);
	if (it == parent.end() || !it->is_array())
		return nullptr;
	return &*it;
}

std::string StringAt(const json& object, const char* key, const std::string& fallback) {
	auto it = object.find(key);
	if (it == object.end() || !it->is_string())
		return fallback;
	return it->get<std::string>();
}

bool BoolAt(const json& object, const char* key, bool fallback) {
	auto it = object.find(key);
	if (it == object.end() || !it->is_boolean())
		return fallback;
	return it->get<bool>();
}

float FloatAt(const json& object, const char* key, float fallback) {
	auto it = object.find(key);
	if (it == object.end() || !it->is_number())
		return fallback;
	return it->get<float>();
}

int IntAt(const json& object, const char* key, int fallback) {
	auto it = object.find(key);
	if (it == object.end() || !it->is_number_integer())
		return fallback;
	return it->get<int>();
}

std::vector<std::string> StringArrayAt(const json& object, const char* key) {
	std::vector<std::string> result;
	auto it = object.find(key);
	if (it == object.end() || !it->is_array())
		return result;
	for (const auto& item : *it) {
		if (item.is_string())
			result.push_back(item.get<std::string>());
	}
	return result;
}

std::array<float, 4> ColorAt(const json& object, const char* key, std::array<float, 4> fallback) {
	auto it = object.find(key);
	if (it == object.end() || !it->is_array() || it->size() != 4)
		return fallback;
	std::array<float, 4> value = fallback;
	for (size_t index = 0; index < 4; ++index) {
		const auto& component = (*it)[index];
		if (!component.is_number())
			return fallback;
		value[index] = component.get<float>();
	}
	return value;
}

json ColorToJson(const std::array<float, 4>& color) {
	return json::array({ color[0], color[1], color[2], color[3] });
}

json StringArrayToJson(const std::vector<std::string>& values) {
	json result = json::array();
	for (const auto& value : values)
		result.push_back(value);
	return result;
}

void Warn(const std::string& message) {
	std::fprintf(stderr, "[Settings][Warn] %s\n", message.c_str());
}

std::string Trim(const std::string& value) {
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return {};
	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

} // namespace

SettingsStore& SettingsStore::Instance() {
	static SettingsStore store;
	return store;
}

std::filesystem::path SettingsStore::ExecutableDir() {
#ifdef _WIN32
	wchar_t buffer[MAX_PATH];
	DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
	if (length > 0 && length < MAX_PATH) {
		std::error_code ec;
		auto canonical = std::filesystem::weakly_canonical(std::filesystem::path(buffer), ec);
		return (ec ? std::filesystem::path(buffer) : canonical).parent_path();
	}
#elif defined(__APPLE__)
	char buffer[4096];
	uint32_t size = sizeof(buffer);
	if (_NSGetExecutablePath(buffer, &size) == 0) {
		std::error_code ec;
		auto canonical = std::filesystem::weakly_canonical(std::filesystem::path(buffer), ec);
		return (ec ? std::filesystem::path(buffer) : canonical).parent_path();
	}
#else
	std::error_code read_error;
	auto target = std::filesystem::read_symlink("/proc/self/exe", read_error);
	if (!read_error)
		return target.parent_path();
#endif
	std::error_code cwd_error;
	auto cwd = std::filesystem::current_path(cwd_error);
	return cwd_error ? std::filesystem::path(".") : cwd;
}

bool SettingsStore::PortableModeEnabled() {
	std::error_code ec;
	return std::filesystem::exists(ExecutableDir() / "portable.flag", ec);
}

std::filesystem::path SettingsStore::DefaultFilePath() {
	const auto executable_dir = ExecutableDir();
	if (PortableModeEnabled())
		return executable_dir / "settings" / "settings.json";
#ifdef __APPLE__
	const char* home = std::getenv("HOME");
	if (home && *home)
		return std::filesystem::path(home) / "Library" / "Application Support" / "CasioEmuMsvc" / "settings.json";
#endif
	return executable_dir / "settings.json";
}

void SettingsStore::EnsurePath() const {
	if (m_pathLocked && m_pathResolved)
		return;
	if (!m_pathResolved) {
		m_path = DefaultFilePath();
		m_pathResolved = true;
	}
}

const std::filesystem::path& SettingsStore::FilePath() const {
	EnsurePath();
	return m_path;
}

void SettingsStore::SetPathForTesting(std::filesystem::path path) {
	m_path = std::move(path);
	m_pathResolved = true;
	m_pathLocked = true;
}

void SettingsStore::ResetToDefaults() {
	m_data = SettingsData{};
	Normalize();
}

bool SettingsStore::HasFile() const {
	EnsurePath();
	std::error_code ec;
	return std::filesystem::is_regular_file(m_path, ec);
}

void SettingsStore::Normalize() {
	SettingsData& d = m_data;
	d.schema = kSchemaVersion;
	d.appearance.skinVersion = 2;

	d.appearance.uiScale = std::clamp(d.appearance.uiScale, 0.8f, 2.0f);
	d.calculator.scale = d.appearance.uiScale;
	for (float& component : d.appearance.seedColor)
		component = std::clamp(component, 0.0f, 1.0f);
	if (d.calculator.injectionFilePath.empty())
		d.calculator.injectionFilePath = "./hc-inj.txt";

	d.editor.fontSize = std::clamp(d.editor.fontSize, 8.0f, 32.0f);
	d.editor.tabSize = std::clamp(d.editor.tabSize, 1, 16);

	d.emulator.backgroundThrottle = std::clamp(d.emulator.backgroundThrottle, 0.05f, 1.0f);

	d.shell.sidebar.width = std::clamp(d.shell.sidebar.width, 160, 600);
	d.shell.bottom.height = std::clamp(d.shell.bottom.height, 80, 800);
	d.shell.tools.width = std::clamp(d.shell.tools.width, 200, 800);
	d.shell.calculator.width = std::clamp(d.shell.calculator.width, 260, 700);
	d.shell.window.width = std::max(d.shell.window.width, 640);
	d.shell.window.height = std::max(d.shell.window.height, 400);
	if (d.shell.bottom.active != "output" && d.shell.bottom.active != "problems" && d.shell.bottom.active != "terminal")
		d.shell.bottom.active = "output";

	if (d.plugins.trustPath.empty())
		d.plugins.trustPath = "plugin-trust.json";
	if (d.language.empty())
		d.language = "en_US";
	if (d.appearance.theme.empty())
		d.appearance.theme = "classic";
}

LoadStatus SettingsStore::Load(std::string* error) {
	EnsurePath();
	m_loaded = true;
	m_lastError.clear();

	std::error_code exists_error;
	if (!std::filesystem::is_regular_file(m_path, exists_error)) {
		ResetToDefaults();
		m_lastStatus = LoadStatus::Missing;
		m_lastError = "settings.json not found at " + m_path.string();
		if (error)
			*error = m_lastError;
		return m_lastStatus;
	}

	std::ifstream input(m_path, std::ios::binary);
	if (!input) {
		ResetToDefaults();
		m_lastStatus = LoadStatus::Error;
		m_lastError = "cannot open " + m_path.string();
		Warn(m_lastError + "; using defaults");
		if (error)
			*error = m_lastError;
		return m_lastStatus;
	}

	std::ostringstream buffer;
	buffer << input.rdbuf();
	const std::string text = buffer.str();

	SettingsData parsed;
	try {
		const json document = json::parse(text);
		if (!document.is_object())
			throw std::runtime_error("settings document must be a JSON object");

		parsed.schema = IntAt(document, "schema", kSchemaVersion);
		if (parsed.schema < 1)
			parsed.schema = kSchemaVersion;

		if (const json* appearance = ObjectAt(document, "appearance")) {
			auto& a = parsed.appearance;
			a.theme = StringAt(*appearance, "theme", a.theme);
			a.pluginTheme = StringAt(*appearance, "pluginTheme", a.pluginTheme);
			a.uiScaleOverridden = BoolAt(*appearance, "uiScaleOverridden", a.uiScaleOverridden);
			a.themeExplicit = BoolAt(*appearance, "themeExplicit", a.themeExplicit);
			a.skinVersion = IntAt(*appearance, "skinVersion", a.skinVersion);
			a.uiScale = FloatAt(*appearance, "uiScale", a.uiScale);
			a.darkMode = BoolAt(*appearance, "darkMode", a.darkMode);
			a.autoTint = BoolAt(*appearance, "autoTint", a.autoTint);
			a.seedColor = ColorAt(*appearance, "seedColor", a.seedColor);
		}

		if (const json* ui = ObjectAt(document, "ui")) {
			parsed.ui.hiddenTabs = StringArrayAt(*ui, "hiddenTabs");
			parsed.ui.displayAddress = BoolAt(*ui, "displayAddress", parsed.ui.displayAddress);
			parsed.ui.displayHex = BoolAt(*ui, "displayHex", parsed.ui.displayHex);
			parsed.ui.showImPComplex = BoolAt(*ui, "showImPComplex", parsed.ui.showImPComplex);
			parsed.ui.alwaysShowImP = BoolAt(*ui, "alwaysShowImP", parsed.ui.alwaysShowImP);
		}

		if (const json* shell = ObjectAt(document, "shell")) {
			if (const json* sidebar = ObjectAt(*shell, "sidebar")) {
				parsed.shell.sidebar.visible = BoolAt(*sidebar, "visible", parsed.shell.sidebar.visible);
				parsed.shell.sidebar.width = IntAt(*sidebar, "width", parsed.shell.sidebar.width);
			}
			if (const json* bottom = ObjectAt(*shell, "bottom")) {
				parsed.shell.bottom.visible = BoolAt(*bottom, "visible", parsed.shell.bottom.visible);
				parsed.shell.bottom.height = IntAt(*bottom, "height", parsed.shell.bottom.height);
				parsed.shell.bottom.active = StringAt(*bottom, "active", parsed.shell.bottom.active);
			}
			if (const json* tools = ObjectAt(*shell, "tools")) {
				parsed.shell.tools.visible = BoolAt(*tools, "visible", parsed.shell.tools.visible);
				parsed.shell.tools.width = IntAt(*tools, "width", parsed.shell.tools.width);
			}
			if (const json* calculator = ObjectAt(*shell, "calculator")) {
				parsed.shell.calculator.visible = BoolAt(*calculator, "visible", parsed.shell.calculator.visible);
				parsed.shell.calculator.width = IntAt(*calculator, "width", parsed.shell.calculator.width);
			}
			if (const json* window = ObjectAt(*shell, "window")) {
				parsed.shell.window.x = IntAt(*window, "x", parsed.shell.window.x);
				parsed.shell.window.y = IntAt(*window, "y", parsed.shell.window.y);
				parsed.shell.window.width = IntAt(*window, "width", parsed.shell.window.width);
				parsed.shell.window.height = IntAt(*window, "height", parsed.shell.window.height);
				parsed.shell.window.fullscreen = BoolAt(*window, "fullscreen", parsed.shell.window.fullscreen);
			}
		}

		if (const json* workspace = ObjectAt(document, "workspace")) {
			parsed.workspace.folder = StringAt(*workspace, "folder", parsed.workspace.folder);
			parsed.workspace.recentFolders = StringArrayAt(*workspace, "recentFolders");
			parsed.workspace.recentFiles = StringArrayAt(*workspace, "recentFiles");
		}

		if (const json* sessions = ArrayAt(document, "sessions")) {
			for (const auto& item : *sessions) {
				if (!item.is_object())
					continue;
				SessionRecord record;
				record.id = StringAt(item, "id", record.id);
				record.model = StringAt(item, "model", record.model);
				record.rom = StringAt(item, "rom", record.rom);
				record.modelPath = StringAt(item, "modelPath", record.modelPath);
				record.title = StringAt(item, "title", record.title);
				record.active = BoolAt(item, "active", record.active);
				record.documents = StringArrayAt(item, "documents");
				parsed.sessions.push_back(std::move(record));
			}
		}

		if (const json* editor = ObjectAt(document, "editor")) {
			parsed.editor.showLineNumbers = BoolAt(*editor, "showLineNumbers", parsed.editor.showLineNumbers);
			parsed.editor.wordWrap = BoolAt(*editor, "wordWrap", parsed.editor.wordWrap);
			parsed.editor.fontSize = FloatAt(*editor, "fontSize", parsed.editor.fontSize);
			parsed.editor.tabSize = IntAt(*editor, "tabSize", parsed.editor.tabSize);
		}

		if (const json* calculator = ObjectAt(document, "calculator")) {
			parsed.calculator.injectionFilePath = StringAt(*calculator, "injectionFilePath", parsed.calculator.injectionFilePath);
			parsed.calculator.scale = FloatAt(*calculator, "scale", parsed.calculator.scale);
			// calculator.scale is a legacy alias; only use it when the new key is
			// absent from the document.
			if (!ObjectAt(document, "appearance") || !document["appearance"].contains("uiScale"))
				parsed.appearance.uiScale = parsed.calculator.scale;
		}

		if (const json* emulator = ObjectAt(document, "emulator")) {
			parsed.emulator.backgroundThrottle = FloatAt(*emulator, "backgroundThrottle", parsed.emulator.backgroundThrottle);
			parsed.emulator.lowPerformanceMode = BoolAt(*emulator, "lowPerformanceMode", parsed.emulator.lowPerformanceMode);
		}

		if (const json* integrations = ObjectAt(document, "integrations")) {
			parsed.integrations.discordRPC = BoolAt(*integrations, "discordRPC", parsed.integrations.discordRPC);
			parsed.integrations.updateChecker = BoolAt(*integrations, "updateChecker", parsed.integrations.updateChecker);
		}

		if (const json* plugins = ObjectAt(document, "plugins")) {
			parsed.plugins.enabled = StringArrayAt(*plugins, "enabled");
			parsed.plugins.disabled = StringArrayAt(*plugins, "disabled");
			parsed.plugins.trustPath = StringAt(*plugins, "trustPath", parsed.plugins.trustPath);
		}

		// The shell layout is VS Code-based, but the default component skin is
		// the original CasioEmuMsvc palette.  One-time migration for documents
		// written by the short-lived VS Code-skin default.
		if (parsed.appearance.skinVersion < 2) {
			parsed.appearance.theme = "classic";
			parsed.appearance.skinVersion = 2;
		}
		parsed.language = StringAt(document, "language", parsed.language);
	}
	catch (const std::exception& parse_error) {
		ResetToDefaults();
		m_lastStatus = LoadStatus::Error;
		m_lastError = parse_error.what();
		Warn("failed to parse " + m_path.string() + ": " + m_lastError + "; using defaults");
		if (error)
			*error = m_lastError;
		return m_lastStatus;
	}

	m_data = std::move(parsed);
	Normalize();
	m_lastStatus = LoadStatus::Loaded;
	if (error)
		error->clear();
	return m_lastStatus;
}

bool SettingsStore::Save(std::string* error) {
	EnsurePath();
	Normalize();

	json document = json::object();
	document["schema"] = kSchemaVersion;

	json appearance = json::object();
	appearance["theme"] = m_data.appearance.theme;
	if (m_data.appearance.pluginTheme.empty())
		appearance["pluginTheme"] = nullptr;
	else
		appearance["pluginTheme"] = m_data.appearance.pluginTheme;
	appearance["uiScaleOverridden"] = m_data.appearance.uiScaleOverridden;
	appearance["themeExplicit"] = m_data.appearance.themeExplicit;
	appearance["skinVersion"] = m_data.appearance.skinVersion;
	appearance["uiScale"] = m_data.appearance.uiScale;
	appearance["darkMode"] = m_data.appearance.darkMode;
	appearance["autoTint"] = m_data.appearance.autoTint;
	appearance["seedColor"] = ColorToJson(m_data.appearance.seedColor);
	document["appearance"] = appearance;

	json ui = json::object();
	ui["hiddenTabs"] = StringArrayToJson(m_data.ui.hiddenTabs);
	ui["displayAddress"] = m_data.ui.displayAddress;
	ui["displayHex"] = m_data.ui.displayHex;
	ui["showImPComplex"] = m_data.ui.showImPComplex;
	ui["alwaysShowImP"] = m_data.ui.alwaysShowImP;
	document["ui"] = ui;

	json shell = json::object();
	json sidebar = json::object();
	sidebar["visible"] = m_data.shell.sidebar.visible;
	sidebar["width"] = m_data.shell.sidebar.width;
	shell["sidebar"] = sidebar;
	json bottom = json::object();
	bottom["visible"] = m_data.shell.bottom.visible;
	bottom["height"] = m_data.shell.bottom.height;
	bottom["active"] = m_data.shell.bottom.active;
	shell["bottom"] = bottom;
	json tools = json::object();
	tools["visible"] = m_data.shell.tools.visible;
	tools["width"] = m_data.shell.tools.width;
	shell["tools"] = tools;
	json calculator_panel = json::object();
	calculator_panel["visible"] = m_data.shell.calculator.visible;
	calculator_panel["width"] = m_data.shell.calculator.width;
	shell["calculator"] = calculator_panel;
	json window = json::object();
	window["x"] = m_data.shell.window.x;
	window["y"] = m_data.shell.window.y;
	window["width"] = m_data.shell.window.width;
	window["height"] = m_data.shell.window.height;
	window["fullscreen"] = m_data.shell.window.fullscreen;
	shell["window"] = window;
	document["shell"] = shell;

	json workspace = json::object();
	if (m_data.workspace.folder.empty())
		workspace["folder"] = nullptr;
	else
		workspace["folder"] = m_data.workspace.folder;
	workspace["recentFolders"] = StringArrayToJson(m_data.workspace.recentFolders);
	workspace["recentFiles"] = StringArrayToJson(m_data.workspace.recentFiles);
	document["workspace"] = workspace;

	json sessions = json::array();
	for (const auto& record : m_data.sessions) {
		json item = json::object();
		item["id"] = record.id;
		item["model"] = record.model;
		item["rom"] = record.rom;
		item["modelPath"] = record.modelPath;
		item["title"] = record.title;
		item["active"] = record.active;
		item["documents"] = StringArrayToJson(record.documents);
		sessions.push_back(std::move(item));
	}
	document["sessions"] = sessions;

	json editor = json::object();
	editor["showLineNumbers"] = m_data.editor.showLineNumbers;
	editor["wordWrap"] = m_data.editor.wordWrap;
	editor["fontSize"] = m_data.editor.fontSize;
	editor["tabSize"] = m_data.editor.tabSize;
	document["editor"] = editor;

	json calculator = json::object();
	calculator["injectionFilePath"] = m_data.calculator.injectionFilePath;
	calculator["scale"] = m_data.calculator.scale;
	document["calculator"] = calculator;

	json emulator = json::object();
	emulator["backgroundThrottle"] = m_data.emulator.backgroundThrottle;
	emulator["lowPerformanceMode"] = m_data.emulator.lowPerformanceMode;
	document["emulator"] = emulator;

	json integrations = json::object();
	integrations["discordRPC"] = m_data.integrations.discordRPC;
	integrations["updateChecker"] = m_data.integrations.updateChecker;
	document["integrations"] = integrations;

	json plugins = json::object();
	plugins["enabled"] = StringArrayToJson(m_data.plugins.enabled);
	plugins["disabled"] = StringArrayToJson(m_data.plugins.disabled);
	plugins["trustPath"] = m_data.plugins.trustPath;
	document["plugins"] = plugins;

	document["language"] = m_data.language;

	std::error_code directory_error;
	if (!m_path.parent_path().empty())
		std::filesystem::create_directories(m_path.parent_path(), directory_error);

	std::filesystem::path temporary = m_path;
	temporary += ".tmp";
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output) {
			const std::string message = "cannot write " + temporary.string();
			Warn(message);
			if (error)
				*error = message;
			return false;
		}
		output << document.dump(2) << '\n';
		output.flush();
		if (!output) {
			const std::string message = "failed while writing " + temporary.string();
			Warn(message);
			if (error)
				*error = message;
			return false;
		}
	}

	std::error_code rename_error;
	std::filesystem::rename(temporary, m_path, rename_error);
	if (rename_error) {
		// Some platforms refuse to replace an existing file with rename().
		std::error_code remove_error;
		std::filesystem::remove(m_path, remove_error);
		rename_error.clear();
		std::filesystem::rename(temporary, m_path, rename_error);
	}
	if (rename_error) {
		std::error_code cleanup_error;
		std::filesystem::remove(temporary, cleanup_error);
		const std::string message = "cannot replace " + m_path.string() + ": " + rename_error.message();
		Warn(message);
		if (error)
			*error = message;
		return false;
	}

	m_loaded = true;
	m_lastError.clear();
	if (error)
		error->clear();
	return true;
}

bool SettingsStore::MigrateFromLegacy(const LegacyThemeValues& legacy, std::string* error) {
	SettingsData migrated;

	migrated.appearance.darkMode = legacy.isDarkMode;
	migrated.appearance.theme = legacy.isDarkMode ? "vscode-dark" : "vscode-light";
	migrated.appearance.uiScale = legacy.scale;
	migrated.appearance.uiScaleOverridden = std::fabs(legacy.scale - 1.0f) > 0.001f;
	migrated.appearance.autoTint = legacy.autoTint;
	migrated.appearance.seedColor = legacy.seedColor;

	migrated.calculator.injectionFilePath = legacy.injectionFilePath.empty() ? "./hc-inj.txt" : legacy.injectionFilePath;
	migrated.calculator.scale = legacy.scale;

	migrated.emulator.lowPerformanceMode = legacy.lowPerformanceMode;
	migrated.integrations.discordRPC = legacy.enableDiscordRPC;
	migrated.integrations.updateChecker = legacy.enableUpdateChecker;

	migrated.shell.window.x = legacy.windowX;
	migrated.shell.window.y = legacy.windowY;
	migrated.shell.window.width = legacy.windowW;
	migrated.shell.window.height = legacy.windowH;

	// old format: "|WindowA|WindowB|"
	std::string token;
	for (char character : legacy.hiddenTabs) {
		if (character == '|') {
			const std::string name = Trim(token);
			if (!name.empty())
				migrated.ui.hiddenTabs.push_back(name);
			token.clear();
		}
		else {
			token.push_back(character);
		}
	}
	{
		const std::string name = Trim(token);
		if (!name.empty())
			migrated.ui.hiddenTabs.push_back(name);
	}

	if (!legacy.language.empty())
		migrated.language = legacy.language;

	m_data = std::move(migrated);
	m_loaded = true;
	if (!Save(error))
		return false;
	std::fprintf(stderr, "[Settings][Info] Migrated theme.bin to %s\n", m_path.string().c_str());
	return true;
}

} // namespace casioemu::settings
