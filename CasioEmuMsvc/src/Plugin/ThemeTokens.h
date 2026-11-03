#pragma once

#include <imgui.h>

#include <string>

namespace casioemu::plugin {

// Palette extracted from tokens.colors.  Shell code (session tabs, sidebar,
// bottom panel) reads these directly instead of guessing ImGui style slots.
struct ThemePalette {
	ImVec4 background{ 0.122f, 0.122f, 0.122f, 1.0f }; // #1f1f1f editor/content
	ImVec4 surface{ 0.094f, 0.094f, 0.094f, 1.0f };    // #181818 panel/sidebar
	ImVec4 surfaceAlt{ 0.125f, 0.125f, 0.125f, 1.0f }; // #202020
	ImVec4 foreground{ 0.800f, 0.800f, 0.800f, 1.0f };// #cccccc
	ImVec4 muted{ 0.616f, 0.616f, 0.616f, 1.0f };     // #9d9d9d
	ImVec4 border{ 0.169f, 0.169f, 0.169f, 1.0f };    // #2b2b2b
	ImVec4 borderStrong{ 0.235f, 0.235f, 0.235f, 1.0f };// #3c3c3c
	ImVec4 accent{ 0.000f, 0.471f, 0.831f, 1.0f };    // #0078d4
	ImVec4 accentHover{ 0.008f, 0.431f, 0.757f, 1.0f };// #026ec1
	ImVec4 selection{ 0.149f, 0.310f, 0.471f, 1.0f }; // #264f78
	ImVec4 hover{ 0.165f, 0.176f, 0.180f, 1.0f };     // #2a2d2e
	ImVec4 error{ 0.945f, 0.298f, 0.298f, 1.0f };     // #f14c4c
	ImVec4 warning{ 0.800f, 0.655f, 0.000f, 1.0f };   // #cca700
	ImVec4 success{ 0.537f, 0.820f, 0.522f, 1.0f };   // #89d185
	ImVec4 link{ 0.302f, 0.667f, 0.988f, 1.0f };      // #4daafc (textLink)

	// Shell chrome tokens (VS Code Dark/Light Modern reference values).
	ImVec4 titleBar{ 0.094f, 0.094f, 0.094f, 1.0f };  // #181818
	ImVec4 activityBar{ 0.094f, 0.094f, 0.094f, 1.0f };// #181818
	ImVec4 statusBar{ 0.094f, 0.094f, 0.094f, 1.0f }; // #181818
	ImVec4 statusBarForeground{ 0.800f, 0.800f, 0.800f, 1.0f }; // #cccccc
	ImVec4 panel{ 0.094f, 0.094f, 0.094f, 1.0f };     // #181818
	ImVec4 tabActive{ 0.122f, 0.122f, 0.122f, 1.0f }; // #1f1f1f
	ImVec4 tabInactive{ 0.176f, 0.176f, 0.176f, 1.0f };// #2d2d2d
};

struct ThemeResult {
	std::string name;
	std::string variant = "dark";
	ThemePalette colors;
	std::string uiFamily = "SF Pro Text";
	std::string monoFamily = "SF Mono";
	float scale = 1.0f;
	ImGuiStyle style;
};

// Parse a theme document (built-in preset or theme plugin entry point).
// `typography.scale` / `typography.uiScale` are clamped to [0.8, 2.0].
bool ParseThemeDocument(const std::string& jsonText, ThemeResult& result, std::string& error);

// Read + parse a theme JSON file.
bool LoadThemeFile(const std::string& path, ThemeResult& result, std::string& error);

// Apply a theme document to the current ImGui style.
// Returns false and fills `error` when the document is malformed.
bool ApplyThemeDocument(const std::string& jsonText, std::string& error);

} // namespace casioemu::plugin
