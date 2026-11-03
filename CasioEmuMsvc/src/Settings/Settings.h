#pragma once

// Versioned JSON settings for the preview shell.
//
// The document lives at
//   ~/Library/Application Support/CasioEmuMsvc/settings.json
// with a portable override at <executable-dir>/settings/settings.json when
// portable.flag exists next to the executable.
//
// This module deliberately has no dependency on ImGui or the theme manager so
// it can be unit-checked on its own.  Gui/ThemeManager.cpp owns the bridge
// between SettingsData and the legacy ThemeSettings/ImGui style state.

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace casioemu::settings {

constexpr int kSchemaVersion = 1;

// SDL_WINDOWPOS_CENTERED without including SDL in this header.
constexpr int kWindowPositionCentered = 0x2FFF0000;

struct AppearanceSettings {
	std::string theme = "classic";
	std::string pluginTheme;
	bool uiScaleOverridden = false;
	bool themeExplicit = false;
	int skinVersion = 1;
	float uiScale = 1.0f;
	bool darkMode = true;
	bool autoTint = false;
	std::array<float, 4> seedColor{ 0.5f, 0.5f, 0.5f, 1.0f };
};

// Window visibility that used to live in ThemeSettings::hiddenTabs.
struct UiSettings {
	std::vector<std::string> hiddenTabs;
};

struct SidebarSettings {
	bool visible = true;
	int width = 240;
};

struct BottomPanelSettings {
	bool visible = true;
	int height = 160;
	std::string active = "output";
};

struct ToolDockSettings {
	bool visible = true;
	int width = 360;
};

struct WindowGeometry {
	int x = kWindowPositionCentered;
	int y = kWindowPositionCentered;
	int width = 1600;
	int height = 1080;
	bool fullscreen = false;
};

struct CalculatorPanelSettings {
	bool visible = true;
	int width = 360;
};

struct ShellSettings {
	SidebarSettings sidebar;
	BottomPanelSettings bottom;
	ToolDockSettings tools;
	CalculatorPanelSettings calculator;
	WindowGeometry window;
};

struct WorkspaceSettings {
	std::string folder;
	std::vector<std::string> recentFolders;
	std::vector<std::string> recentFiles;
};

struct SessionRecord {
	std::string id;
	std::string model;
	std::string rom;
	std::string modelPath;
	std::string title;
	bool active = false;
	std::vector<std::string> documents;
};

struct EditorSettings {
	bool showLineNumbers = true;
	bool wordWrap = false;
	float fontSize = 13.0f;
	int tabSize = 4;
};

struct CalculatorSettings {
	std::string injectionFilePath = "./hc-inj.txt";
	// Legacy name for AppearanceSettings::uiScale, kept in the document for one
	// release so old tooling keeps working.
	float scale = 1.0f;
};

struct EmulatorSettings {
	float backgroundThrottle = 0.25f;
	bool lowPerformanceMode = false;
};

struct IntegrationSettings {
	bool discordRPC = true;
	bool updateChecker = true;
};

struct PluginSettings {
	std::vector<std::string> enabled;
	std::vector<std::string> disabled;
	std::string trustPath = "plugin-trust.json";
};

struct SettingsData {
	int schema = kSchemaVersion;
	AppearanceSettings appearance;
	UiSettings ui;
	ShellSettings shell;
	WorkspaceSettings workspace;
	std::vector<SessionRecord> sessions;
	EditorSettings editor;
	CalculatorSettings calculator;
	EmulatorSettings emulator;
	IntegrationSettings integrations;
	PluginSettings plugins;
	std::string language = "en_US";
};

// The scalar fields of the old theme.bin.  Filled by the theme manager (which
// owns the binary reader) and handed to SettingsStore for one-time migration.
struct LegacyThemeValues {
	bool valid = false;
	bool isDarkMode = true;
	float scale = 1.0f;
	std::string language;
	std::string injectionFilePath;
	bool lowPerformanceMode = false;
	bool autoTint = false;
	std::array<float, 4> seedColor{ 0.5f, 0.5f, 0.5f, 1.0f };
	bool enableDiscordRPC = true;
	bool enableUpdateChecker = true;
	int windowX = kWindowPositionCentered;
	int windowY = kWindowPositionCentered;
	int windowW = 1600;
	int windowH = 1080;
	std::string hiddenTabs;
};

enum class LoadStatus {
	Loaded,  // settings.json existed and parsed
	Missing, // no file; defaults are active
	Error,   // file existed but could not be read/parsed; defaults active
};

class SettingsStore {
public:
	static SettingsStore& Instance();

	// Loads the document.  Never throws.  On Error, Data() holds defaults.
	LoadStatus Load(std::string* error = nullptr);

	// Atomic write (temp file + rename) of Data().
	bool Save(std::string* error = nullptr);

	bool IsLoaded() const { return m_loaded; }
	bool HasFile() const;
	LoadStatus LastLoadStatus() const { return m_lastStatus; }
	const std::string& LastError() const { return m_lastError; }

	// Resolved document path (portable override or Application Support).
	const std::filesystem::path& FilePath() const;

	SettingsData& Data() { return m_data; }
	const SettingsData& Data() const { return m_data; }

	void ResetToDefaults();
	void Normalize();

	// Converts legacy theme.bin scalars into schema 1 and writes the document.
	// Only call when no valid settings.json exists.
	bool MigrateFromLegacy(const LegacyThemeValues& legacy, std::string* error = nullptr);

	// Test hook: pin the document path and disable portable/Application Support
	// resolution.  The caller owns the file lifecycle.
	void SetPathForTesting(std::filesystem::path path);

	static std::filesystem::path DefaultFilePath();
	static std::filesystem::path ExecutableDir();
	static bool PortableModeEnabled();

private:
	SettingsStore() = default;

	void EnsurePath() const;

	mutable std::filesystem::path m_path;
	mutable bool m_pathResolved = false;
	bool m_pathLocked = false;
	bool m_loaded = false;
	LoadStatus m_lastStatus = LoadStatus::Missing;
	std::string m_lastError;
	SettingsData m_data;
};

} // namespace casioemu::settings
