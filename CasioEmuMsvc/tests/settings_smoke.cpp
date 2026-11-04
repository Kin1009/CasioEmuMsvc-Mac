// Self-check for the JSON settings model and the built-in theme presets.
//
// It deliberately does not start ImGui: Settings.h has no UI dependency and the
// theme files are validated structurally with the same JSON library the loader
// uses.  Build with -DCASIOEMU_BUILD_SETTINGS_TESTS=ON and run the
// casioemu_settings_smoke binary.

#include "Settings/Settings.h"

#include "../../McpPlugin/json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
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

struct Rgb {
	float r = 0.0f, g = 0.0f, b = 0.0f;
};

bool ParseHexColor(const std::string& text, Rgb& out) {
	std::string hex = text;
	if (!hex.empty() && hex.front() == '#')
		hex.erase(hex.begin());
	if (hex.size() != 6)
		return false;
	for (char ch : hex) {
		if (!std::isxdigit(static_cast<unsigned char>(ch)))
			return false;
	}
	out.r = static_cast<float>(std::stoi(hex.substr(0, 2), nullptr, 16)) / 255.0f;
	out.g = static_cast<float>(std::stoi(hex.substr(2, 2), nullptr, 16)) / 255.0f;
	out.b = static_cast<float>(std::stoi(hex.substr(4, 2), nullptr, 16)) / 255.0f;
	return true;
}

float RelativeLuminance(const Rgb& color) {
	const auto channel = [](float value) {
		return value <= 0.03928f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
	};
	return 0.2126f * channel(color.r) + 0.7152f * channel(color.g) + 0.0722f * channel(color.b);
}

float ContrastRatio(const Rgb& lhs, const Rgb& rhs) {
	const float a = RelativeLuminance(lhs);
	const float b = RelativeLuminance(rhs);
	const float hi = std::max(a, b);
	const float lo = std::min(a, b);
	return (hi + 0.05f) / (lo + 0.05f);
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
	data.calculator.injectionAutoReload = false;
	data.calculator.zoom = 1.75f;
	data.emulator.clockExponent = 20;
	data.emulator.audioEnabled = false;
	data.emulator.captureScale = 4;
	data.emulator.captureBackgroundRgb = 0x102030;
	data.emulator.lcdFlashThreshold = 12;
	data.emulator.lcdFlashBrightness = 2.5f;
	data.emulator.lcdBuffer = 2;
	data.advanced.renderer = "software";

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
	Check(!store.Data().calculator.injectionAutoReload, "injection auto-reload persists");
	Check(NearlyEqual(store.Data().calculator.zoom, 1.75f), "calculator zoom persists");
	Check(store.Data().emulator.clockExponent == 20, "clock exponent persists");
	Check(!store.Data().emulator.audioEnabled, "audio toggle persists");
	Check(store.Data().emulator.captureScale == 4, "capture scale persists");
	Check(store.Data().emulator.captureBackgroundRgb == 0x102030, "capture background persists");
	Check(store.Data().emulator.lcdFlashThreshold == 12, "LCD flash threshold persists");
	Check(NearlyEqual(store.Data().emulator.lcdFlashBrightness, 2.5f), "LCD flash brightness persists");
	Check(store.Data().emulator.lcdBuffer == 2, "LCD buffer persists");
	Check(store.Data().advanced.renderer == "software", "renderer hint persists");
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
	// Darkness range requested for the second UI pass + the presets kept as-is.
	const std::vector<std::pair<std::string, std::string>> required = {
		{ "dark", "Dark" },
		{ "dark-plus", "Dark+" },
		{ "darker", "Darker" },
		{ "amoled", "AMOLED (Pure Black)" },
		{ "midnight-blue", "Midnight Blue" },
		{ "graphite", "Graphite" },
		{ "high-contrast-dark", "High Contrast Dark" },
		{ "light-plus", "Light+" },
		{ "paper", "Paper" },
		{ "snow", "Snow" },
		{ "github-light", "GitHub Light" },
		{ "high-contrast-light", "High Contrast Light" },
		{ "anh-sang-cua-dang", "\u00c1nh s\u00e1ng c\u1ee7a \u0110\u1ea3ng" },
		{ "solarized", "Solarized" },
		{ "vscode-light", "VS Code Light Modern" },
	};
	Check(files.size() >= required.size(), "all darkness presets exist");

	const std::vector<std::string> requiredColors = {
		"background", "surface", "surfaceAlt", "foreground", "muted", "border",
		"accent", "accentHover", "selection", "hover", "error", "warning", "success"
	};

	std::map<std::string, json> by_id;
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
		const std::string id = file.stem().string();
		by_id[id] = document;
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

	// Exact names and the AMOLED spec.
	for (const auto& [id, name] : required) {
		auto it = by_id.find(id);
		if (it == by_id.end()) {
			Check(false, id + ".json is present");
			continue;
		}
		Check(it->second.value("name", std::string()) == name, id + ".json is named '" + name + "'");
		std::cout << "[theme] " << name << " (" << id << ") bg="
			<< it->second["tokens"]["colors"].value("background", std::string())
			<< " surface=" << it->second["tokens"]["colors"].value("surface", std::string()) << '\n';
	}
	if (auto it = by_id.find("anh-sang-cua-dang"); it != by_id.end()) {
		Check(it->second.value("image", std::string()) == "anh-sang-cua-dang.jpeg",
			"anh-sang-cua-dang points at its image asset");
	}
	if (auto it = by_id.find("amoled"); it != by_id.end()) {
		const auto& colors = it->second["tokens"]["colors"];
		Check(colors.value("background", std::string()) == "#000000", "AMOLED background is #000000");
		Check(colors.value("surface", std::string()) == "#0A0A0A", "AMOLED surface is #0A0A0A");
	}

	// Darkness ordering: Dark -> Dark+ -> Darker.
	const auto luminance_of = [&](const std::string& id) {
		Rgb rgb;
		if (!by_id.count(id) || !ParseHexColor(by_id[id]["tokens"]["colors"].value("background", std::string()), rgb))
			return -1.0f;
		return RelativeLuminance(rgb);
	};
	const float dark_lum = luminance_of("dark");
	const float plus_lum = luminance_of("dark-plus");
	const float darker_lum = luminance_of("darker");
	Check(dark_lum > plus_lum && plus_lum > darker_lum, "Dark > Dark+ > Darker background luminance");

	// Readability at every darkness level.  Presets that shipped before this
	// pass (Solarized, VS Code) are kept byte-for-byte as-is, so the strict
	// checks apply to the new darkness range only.
	const std::vector<std::string> darkness_ids = {
		"dark", "dark-plus", "darker", "amoled", "midnight-blue", "graphite",
		"high-contrast-dark", "light-plus", "paper", "snow", "github-light",
		"high-contrast-light", "anh-sang-cua-dang"
	};
	for (const auto& [id, document] : by_id) {
		if (std::find(darkness_ids.begin(), darkness_ids.end(), id) == darkness_ids.end())
			continue;
		if (!document.contains("tokens") || !document["tokens"].contains("colors"))
			continue;
		const auto& colors = document["tokens"]["colors"];
		Rgb background, surface, foreground, muted, accent, selection, error, warning, success;
		if (!ParseHexColor(colors.value("background", std::string()), background) ||
			!ParseHexColor(colors.value("surface", std::string()), surface) ||
			!ParseHexColor(colors.value("foreground", std::string()), foreground) ||
			!ParseHexColor(colors.value("muted", std::string()), muted) ||
			!ParseHexColor(colors.value("accent", std::string()), accent) ||
			!ParseHexColor(colors.value("selection", std::string()), selection) ||
			!ParseHexColor(colors.value("error", std::string()), error) ||
			!ParseHexColor(colors.value("warning", std::string()), warning) ||
			!ParseHexColor(colors.value("success", std::string()), success)) {
			Check(false, id + " colors parse as hex");
			continue;
		}
		Check(ContrastRatio(foreground, background) >= 7.0f, id + " foreground contrast >= 7");
		Check(ContrastRatio(muted, background) >= 4.5f, id + " muted contrast >= 4.5");
		Check(ContrastRatio(error, background) >= 4.5f, id + " error contrast >= 4.5");
		Check(ContrastRatio(warning, background) >= 4.5f, id + " warning contrast >= 4.5");
		Check(ContrastRatio(success, background) >= 4.5f, id + " success contrast >= 4.5");
		Check(ContrastRatio(accent, background) >= 4.0f, id + " accent contrast >= 4.0");
		Check(ContrastRatio(foreground, selection) >= 4.5f, id + " selected text contrast >= 4.5");
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
