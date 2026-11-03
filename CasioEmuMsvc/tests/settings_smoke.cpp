// Self-check for the JSON settings model and the built-in theme presets.
//
// It deliberately does not start ImGui: Settings.h has no UI dependency and the
// theme files are validated structurally with the same JSON library the loader
// uses.  Build with -DCASIOEMU_BUILD_SETTINGS_TESTS=ON and run the
// casioemu_settings_smoke binary.

#include "Settings/Settings.h"

#include "../../McpPlugin/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <unistd.h>

#ifndef CASIOEMU_THEMES_SOURCE_DIR
#define CASIOEMU_THEMES_SOURCE_DIR "resources/themes"
#endif

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
	if (condition)
		return;
	++g_failures;
	std::cerr << "[FAIL] " << message << '\n';
}

bool NearlyEqual(float lhs, float rhs) {
	return std::fabs(lhs - rhs) < 0.001f;
}

std::string ReadText(const std::filesystem::path& path) {
	std::ifstream input(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output << text;
}

void TestSettingsRoundTrip(const std::filesystem::path& root) {
	using namespace casioemu::settings;
	auto& store = SettingsStore::Instance();
	const auto path = root / "roundtrip" / "settings.json";
	store.SetPathForTesting(path);

	Check(store.Load() == LoadStatus::Missing, "missing file reports Missing");
	Check(store.Data().appearance.theme == "classic", "default theme is classic");
	Check(store.Data().language == "en_US", "default language is en_US");

	auto& data = store.Data();
	data.appearance.theme = "midnight-blue";
	data.appearance.uiScale = 1.25f;
	data.appearance.darkMode = true;
	data.editor.fontSize = 15.0f;
	data.editor.tabSize = 2;
	data.emulator.backgroundThrottle = 0.5f;
	data.integrations.discordRPC = false;
	data.calculator.injectionFilePath = "/tmp/injections.txt";
	data.shell.window.width = 1920;
	data.workspace.recentFiles = { "/tmp/a.wz", "/tmp/b.wz" };
	data.plugins.enabled = { "com.example.theme" };
	data.ui.hiddenTabs = { "Variables", "Watch" };
	data.ui.displayAddress = false;
	data.ui.displayHex = true;
	data.ui.showImPComplex = false;
	data.ui.alwaysShowImP = true;
	data.calculator.scale = 9.0f; // Normalize must resync it to appearance.uiScale.

	std::string error;
	Check(store.Save(&error), "save succeeds: " + error);
	Check(std::filesystem::exists(path), "settings.json exists");
	Check(!std::filesystem::exists(path.string() + ".tmp"), "atomic write leaves no temp file");

	store.ResetToDefaults();
	Check(store.Load() == LoadStatus::Loaded, "roundtrip file loads");
	Check(store.Data().appearance.theme == "midnight-blue", "theme persists");
	Check(NearlyEqual(store.Data().appearance.uiScale, 1.25f), "uiScale persists");
	Check(NearlyEqual(store.Data().calculator.scale, 1.25f), "legacy calculator.scale stays in sync");
	Check(NearlyEqual(store.Data().editor.fontSize, 15.0f), "editor font size persists");
	Check(store.Data().editor.tabSize == 2, "editor tab size persists");
	Check(NearlyEqual(store.Data().emulator.backgroundThrottle, 0.5f), "throttle persists");
	Check(!store.Data().integrations.discordRPC, "Discord RPC toggle persists");
	Check(store.Data().calculator.injectionFilePath == "/tmp/injections.txt", "injection path persists");
	Check(store.Data().shell.window.width == 1920, "window geometry persists");
	Check(store.Data().workspace.recentFiles.size() == 2, "recent files persist");
	Check(store.Data().plugins.enabled.size() == 1, "plugin list persists");
	Check(store.Data().ui.hiddenTabs.size() == 2, "hidden tabs persist");
	Check(!store.Data().ui.displayAddress, "Display Address checkbox persists");
	Check(store.Data().ui.displayHex, "Display Hex checkbox persists");
	Check(!store.Data().ui.showImPComplex, "Show ImP checkbox persists");
	Check(store.Data().ui.alwaysShowImP, "Always Show ImP checkbox persists");
}

void TestMalformedDocument(const std::filesystem::path& root) {
	using namespace casioemu::settings;
	auto& store = SettingsStore::Instance();
	const auto path = root / "malformed" / "settings.json";
	store.SetPathForTesting(path);
	WriteText(path, "{ this is not json ");

	std::string error;
	Check(store.Load(&error) == LoadStatus::Error, "malformed JSON reports Error");
	Check(!error.empty(), "malformed JSON reports an error message");
	Check(store.Data().appearance.theme == "classic", "malformed JSON falls back to defaults");
	Check(store.Data().editor.tabSize == 4, "defaults are complete after a parse error");

	// A load must never destroy the user's file; the next explicit save repairs it.
	Check(std::filesystem::exists(path), "failed load leaves the file alone");
	Check(store.Save(), "save repairs the file");
	Check(store.Load() == LoadStatus::Loaded, "repaired file loads");
}

void TestLegacyMigration(const std::filesystem::path& root) {
	using namespace casioemu::settings;
	auto& store = SettingsStore::Instance();
	const auto path = root / "migration" / "settings.json";
	store.SetPathForTesting(path);

	LegacyThemeValues legacy;
	legacy.valid = true;
	legacy.isDarkMode = false;
	legacy.scale = 1.4f;
	legacy.language = "ja_JP";
	legacy.injectionFilePath = "/old/hc-inj.txt";
	legacy.lowPerformanceMode = true;
	legacy.autoTint = true;
	legacy.seedColor = { 0.1f, 0.2f, 0.3f, 1.0f };
	legacy.enableDiscordRPC = false;
	legacy.enableUpdateChecker = false;
	legacy.windowX = 40;
	legacy.windowY = 50;
	legacy.windowW = 1280;
	legacy.windowH = 720;
	legacy.hiddenTabs = "|Variables|Watch|";

	std::string error;
	Check(store.MigrateFromLegacy(legacy, &error), "migration succeeds: " + error);
	Check(std::filesystem::exists(path), "migration writes settings.json");

	store.ResetToDefaults();
	Check(store.Load() == LoadStatus::Loaded, "migrated file loads");
	const auto& data = store.Data();
	Check(data.appearance.theme == "vscode-light", "isDarkMode=false maps to vscode-light");
	Check(!data.appearance.darkMode, "darkMode maps");
	Check(NearlyEqual(data.appearance.uiScale, 1.4f), "scale maps to uiScale");
	Check(data.language == "ja_JP", "language maps");
	Check(data.calculator.injectionFilePath == "/old/hc-inj.txt", "injection path maps");
	Check(data.emulator.lowPerformanceMode, "low performance flag maps");
	Check(data.appearance.autoTint, "auto tint maps");
	Check(NearlyEqual(data.appearance.seedColor[1], 0.2f), "seed color maps");
	Check(!data.integrations.discordRPC, "Discord RPC toggle maps");
	Check(!data.integrations.updateChecker, "update checker toggle maps");
	Check(data.shell.window.x == 40 && data.shell.window.height == 720, "window geometry maps");
	Check(data.ui.hiddenTabs.size() == 2 && data.ui.hiddenTabs[0] == "Variables", "hidden tab list maps");
}

std::vector<std::filesystem::path> ThemeFiles() {
	std::vector<std::filesystem::path> files;
	std::error_code ec;
	const std::filesystem::path directory(CASIOEMU_THEMES_SOURCE_DIR);
	if (!std::filesystem::is_directory(directory, ec)) {
		Check(false, std::string("theme directory is missing: ") + directory.string());
		return files;
	}
	for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
		if (entry.is_regular_file() && entry.path().extension() == ".json")
			files.push_back(entry.path());
	}
	return files;
}

void TestThemeFiles() {
	using json = nlohmann::json;
	const auto files = ThemeFiles();
	Check(files.size() >= 5, "all five built-in presets exist");

	const std::vector<std::string> requiredColors = {
		"background", "surface", "surfaceAlt", "foreground", "muted", "border",
		"accent", "accentHover", "selection", "hover", "error", "warning", "success"
	};

	for (const auto& file : files) {
		const std::string text = ReadText(file);
		Check(text.size() < 3 || (unsigned char)text[0] != 0xEF, file.filename().string() + " has no UTF-8 BOM");
		json document;
		try {
			document = json::parse(text);
		}
		catch (const json::exception& parse_error) {
			Check(false, file.filename().string() + " parses: " + parse_error.what());
			continue;
		}
		Check(document.value("schema", 0) == 1, file.filename().string() + " schema is 1");
		const std::string variant = document.value("variant", "");
		Check(variant == "dark" || variant == "light", file.filename().string() + " variant is dark/light");
		Check(document.contains("name") && document["name"].is_string(), file.filename().string() + " has a name");
		if (!document.contains("tokens") || !document["tokens"].is_object())
			continue;
		const auto& tokens = document["tokens"];
		Check(tokens.contains("colors") && tokens["colors"].is_object(), file.filename().string() + " has colors");
		Check(tokens.contains("metrics") && tokens["metrics"].is_object(), file.filename().string() + " has metrics");
		Check(tokens.contains("typography") && tokens["typography"].is_object(), file.filename().string() + " has typography");

		if (tokens.contains("colors") && tokens["colors"].is_object()) {
			for (const auto& key : requiredColors) {
				const auto& color = tokens["colors"];
				Check(color.contains(key) && color[key].is_string(), file.filename().string() + " colors." + key);
			}
		}
		if (tokens.contains("typography") && tokens["typography"].is_object()) {
			const auto& typography = tokens["typography"];
			Check(typography.contains("uiFamily") && typography["uiFamily"].is_string(), file.filename().string() + " typography.uiFamily");
			Check(typography.contains("monoFamily") && typography["monoFamily"].is_string(), file.filename().string() + " typography.monoFamily");
			if (typography.contains("scale")) {
				const float scale = typography["scale"].get<float>();
				Check(scale >= 0.8f && scale <= 2.0f, file.filename().string() + " typography.scale in [0.8, 2.0]");
			}
			else {
				Check(false, file.filename().string() + " typography.scale is present");
			}
		}
	}
}

} // namespace

int main() {
	// Scratch must not use the system temp dir (repository rule): CTest points
	// CASIOEMU_TEST_ROOT at Wiz/temp, otherwise use the build directory.
	const char* scratch_env = std::getenv("CASIOEMU_TEST_ROOT");
	const auto base = scratch_env && *scratch_env
		? std::filesystem::path(scratch_env)
		: std::filesystem::current_path() / "plugin-test-scratch";
	const auto root = base / ("casioemu-settings-smoke-" + std::to_string(static_cast<long long>(::getpid())));
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
	std::filesystem::create_directories(root, ec);

	TestSettingsRoundTrip(root);
	TestMalformedDocument(root);
	TestLegacyMigration(root);
	TestThemeFiles();

	std::filesystem::remove_all(root, ec);
	if (g_failures == 0) {
		std::cout << "[OK] settings and theme self-check passed\n";
		return 0;
	}
	std::cerr << "[" << g_failures << " failure(s)] settings/theme self-check\n";
	return 1;
}
