#include "ThemeTokens.h"

#include "../../../McpPlugin/json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string_view>

namespace casioemu::plugin {
namespace {

using json = nlohmann::json;

bool ParseHexColor(std::string_view text, ImVec4& color) {
	if (text.size() != 7 && text.size() != 9)
		return false;
	if (text.front() != '#')
		return false;
	auto hex_value = [](char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	};
	int values[4] = { 0, 0, 0, 255 };
	for (size_t i = 0; i < (text.size() - 1) / 2; ++i) {
		const int high = hex_value(text[1 + i * 2]);
		const int low = hex_value(text[2 + i * 2]);
		if (high < 0 || low < 0)
			return false;
		values[i] = high * 16 + low;
	}
	color = ImVec4(values[0] / 255.0f, values[1] / 255.0f, values[2] / 255.0f, values[3] / 255.0f);
	return true;
}

bool ReadColor(const json& colors, const char* key, ImVec4& color, bool& present, std::string& error) {
	present = false;
	auto it = colors.find(key);
	if (it == colors.end())
		return true;
	present = true;
	if (!it->is_string()) {
		error = std::string("colors.") + key + " must be a #RRGGBB or #RRGGBBAA string";
		return false;
	}
	if (!ParseHexColor(it->get<std::string>(), color)) {
		error = std::string("colors.") + key + " is not a valid color";
		return false;
	}
	return true;
}

float NumberOr(const json& object, const char* key, float fallback) {
	auto it = object.find(key);
	if (it == object.end() || !it->is_number())
		return fallback;
	return it->get<float>();
}

bool ReadStringToken(const json& object, const char* key, std::string& out, std::string& error) {
	auto it = object.find(key);
	if (it == object.end())
		return true;
	if (!it->is_string()) {
		error = std::string("typography.") + key + " must be a string";
		return false;
	}
	out = it->get<std::string>();
	return true;
}

} // namespace

bool ParseThemeDocument(const std::string& jsonText, ThemeResult& result, std::string& error) {
	json document;
	try {
		document = json::parse(jsonText);
	}
	catch (const json::exception& parse_error) {
		error = parse_error.what();
		return false;
	}

	if (!document.is_object()) {
		error = "theme document must be a JSON object";
		return false;
	}
	if (!document.contains("schema") || !document["schema"].is_number_integer() || document["schema"].get<int>() != 1) {
		error = "theme schema must be 1";
		return false;
	}

	result = ThemeResult{};

	if (document.contains("name")) {
		if (!document["name"].is_string()) {
			error = "theme name must be a string";
			return false;
		}
		result.name = document["name"].get<std::string>();
	}

	if (document.contains("image") && document["image"].is_string())
		result.image = document["image"].get<std::string>();
	result.variant = "dark";
	if (document.contains("variant")) {
		if (!document["variant"].is_string()) {
			error = "theme variant must be 'dark' or 'light'";
			return false;
		}
		result.variant = document["variant"].get<std::string>();
	}
	if (result.variant != "dark" && result.variant != "light") {
		error = "theme variant must be 'dark' or 'light'";
		return false;
	}

	ImGuiStyle& style = result.style;
	if (result.variant == "light")
		ImGui::StyleColorsLight(&style);
	else
		ImGui::StyleColorsDark(&style);

	auto tokens_it = document.find("tokens");
	if (tokens_it == document.end() || !tokens_it->is_object()) {
		error = "theme is missing the 'tokens' object";
		return false;
	}
	const json& tokens = *tokens_it;

	if (auto colors_it = tokens.find("colors"); colors_it != tokens.end()) {
		if (!colors_it->is_object()) {
			error = "tokens.colors must be an object";
			return false;
		}
		const json& colors = *colors_it;
		ImVec4 value;
		bool present = false;

		if (!ReadColor(colors, "background", value, present, error)) return false;
		if (present) {
			result.colors.background = value;
			style.Colors[ImGuiCol_WindowBg] = value;
			style.Colors[ImGuiCol_PopupBg] = value;
			style.Colors[ImGuiCol_MenuBarBg] = value;
			style.Colors[ImGuiCol_TitleBg] = value;
		}
		if (!ReadColor(colors, "surface", value, present, error)) return false;
		if (present) {
			result.colors.surface = value;
			style.Colors[ImGuiCol_ChildBg] = value;
			style.Colors[ImGuiCol_FrameBg] = value;
			style.Colors[ImGuiCol_Tab] = value;
			style.Colors[ImGuiCol_TabUnfocused] = value;
		}
		if (!ReadColor(colors, "surfaceAlt", value, present, error)) return false;
		if (present) {
			result.colors.surfaceAlt = value;
			style.Colors[ImGuiCol_FrameBgHovered] = value;
			style.Colors[ImGuiCol_TabUnfocusedActive] = value;
		}
		if (!ReadColor(colors, "foreground", value, present, error)) return false;
		if (present) {
			result.colors.foreground = value;
			style.Colors[ImGuiCol_Text] = value;
		}
		if (!ReadColor(colors, "muted", value, present, error)) return false;
		if (present) {
			result.colors.muted = value;
			style.Colors[ImGuiCol_TextDisabled] = value;
		}
		if (!ReadColor(colors, "border", value, present, error)) return false;
		if (present) {
			result.colors.border = value;
			style.Colors[ImGuiCol_Border] = value;
			style.Colors[ImGuiCol_Separator] = value;
		}
		if (!ReadColor(colors, "accent", value, present, error)) return false;
		if (present) {
			result.colors.accent = value;
			style.Colors[ImGuiCol_CheckMark] = value;
			style.Colors[ImGuiCol_SliderGrab] = value;
			style.Colors[ImGuiCol_HeaderActive] = value;
			style.Colors[ImGuiCol_TabActive] = value;
			style.Colors[ImGuiCol_ButtonActive] = value;
		}
		if (!ReadColor(colors, "accentHover", value, present, error)) return false;
		if (present) {
			result.colors.accentHover = value;
			style.Colors[ImGuiCol_ButtonHovered] = value;
			style.Colors[ImGuiCol_HeaderHovered] = value;
			style.Colors[ImGuiCol_SliderGrabActive] = value;
			style.Colors[ImGuiCol_TabHovered] = value;
		}
		if (!ReadColor(colors, "selection", value, present, error)) return false;
		if (present) {
			result.colors.selection = value;
			style.Colors[ImGuiCol_Header] = value;
			style.Colors[ImGuiCol_TextSelectedBg] = value;
			style.Colors[ImGuiCol_NavHighlight] = value;
		}
		if (!ReadColor(colors, "hover", value, present, error)) return false;
		if (present) {
			result.colors.hover = value;
			style.Colors[ImGuiCol_FrameBgHovered] = value;
			style.Colors[ImGuiCol_HeaderHovered] = value;
			style.Colors[ImGuiCol_TabHovered] = value;
		}
		if (!ReadColor(colors, "error", value, present, error)) return false;
		if (present)
			result.colors.error = value;
		if (!ReadColor(colors, "warning", value, present, error)) return false;
		if (present)
			result.colors.warning = value;
		if (!ReadColor(colors, "success", value, present, error)) return false;
		if (present)
			result.colors.success = value;

		// Optional shell chrome tokens.  Unknown extra keys stay forward
		// compatible; these are simply not read.
		const struct {
			const char* key;
			ImVec4* target;
		} extras[] = {
			{ "borderStrong", &result.colors.borderStrong },
			{ "link", &result.colors.link },
			{ "titleBar", &result.colors.titleBar },
			{ "activityBar", &result.colors.activityBar },
			{ "statusBar", &result.colors.statusBar },
			{ "statusBarForeground", &result.colors.statusBarForeground },
			{ "panel", &result.colors.panel },
			{ "tabActive", &result.colors.tabActive },
			{ "tabInactive", &result.colors.tabInactive },
		};
		for (const auto& extra : extras) {
			if (!ReadColor(colors, extra.key, value, present, error)) return false;
			if (present)
				*extra.target = value;
		}
		style.Colors[ImGuiCol_TitleBg] = result.colors.titleBar;
		style.Colors[ImGuiCol_TitleBgActive] = result.colors.titleBar;
		style.Colors[ImGuiCol_TextLink] = result.colors.link;
		style.Colors[ImGuiCol_Tab] = result.colors.tabInactive;
		style.Colors[ImGuiCol_TabHovered] = result.colors.hover;
		style.Colors[ImGuiCol_TabActive] = result.colors.tabActive;
		style.Colors[ImGuiCol_TabSelected] = result.colors.tabActive;
		style.Colors[ImGuiCol_TabUnfocused] = result.colors.tabInactive;
		style.Colors[ImGuiCol_TabUnfocusedActive] = result.colors.tabActive;
		style.Colors[ImGuiCol_TabDimmed] = result.colors.tabInactive;
		style.Colors[ImGuiCol_TabDimmedSelected] = result.colors.tabActive;
		style.Colors[ImGuiCol_Button] = result.colors.surfaceAlt;
		style.Colors[ImGuiCol_ButtonHovered] = result.colors.hover;
		style.Colors[ImGuiCol_ButtonActive] = result.colors.selection;
		style.Colors[ImGuiCol_FrameBg] = result.colors.surfaceAlt;
		style.Colors[ImGuiCol_FrameBgHovered] = result.colors.hover;
		style.Colors[ImGuiCol_FrameBgActive] = result.colors.selection;
		style.Colors[ImGuiCol_Header] = result.colors.selection;
		style.Colors[ImGuiCol_HeaderHovered] = result.colors.hover;
		style.Colors[ImGuiCol_HeaderActive] = result.colors.selection;
		style.Colors[ImGuiCol_ScrollbarBg] = result.colors.background;
		style.Colors[ImGuiCol_ScrollbarGrab] = result.colors.borderStrong;
		style.Colors[ImGuiCol_ScrollbarGrabHovered] = result.colors.muted;
		style.Colors[ImGuiCol_ScrollbarGrabActive] = result.colors.accent;
		style.Colors[ImGuiCol_Separator] = result.colors.border;
		style.Colors[ImGuiCol_TextSelectedBg] = result.colors.selection;
		style.Colors[ImGuiCol_NavHighlight] = result.colors.accent;
		style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
	}

	if (auto metrics_it = tokens.find("metrics"); metrics_it != tokens.end()) {
		if (!metrics_it->is_object()) {
			error = "tokens.metrics must be an object";
			return false;
		}
		const json& metrics = *metrics_it;
		style.WindowRounding = NumberOr(metrics, "windowRounding", style.WindowRounding);
		style.ChildRounding = NumberOr(metrics, "childRounding", style.ChildRounding);
		style.PopupRounding = NumberOr(metrics, "popupRounding", style.PopupRounding);
		style.FrameRounding = NumberOr(metrics, "frameRounding", style.FrameRounding);
		style.TabRounding = NumberOr(metrics, "tabRounding", style.TabRounding);
		style.GrabRounding = NumberOr(metrics, "grabRounding", style.GrabRounding);
		style.ScrollbarRounding = NumberOr(metrics, "scrollbarRounding", style.ScrollbarRounding);
		style.WindowBorderSize = NumberOr(metrics, "borderSize", style.WindowBorderSize);
		style.FrameBorderSize = NumberOr(metrics, "frameBorderSize", style.FrameBorderSize);
		style.ScrollbarSize = NumberOr(metrics, "scrollbarSize", style.ScrollbarSize);
		const float spacing = NumberOr(metrics, "spacing", 0.0f);
		if (spacing > 0.0f)
			style.ItemSpacing = ImVec2(spacing, spacing);
		const float padding = NumberOr(metrics, "padding", 0.0f);
		if (padding > 0.0f)
			style.WindowPadding = ImVec2(padding, padding);
		style.FramePadding.x = NumberOr(metrics, "framePaddingX", style.FramePadding.x);
		style.FramePadding.y = NumberOr(metrics, "framePaddingY", style.FramePadding.y);
	}

	if (auto typography_it = tokens.find("typography"); typography_it != tokens.end()) {
		if (!typography_it->is_object()) {
			error = "tokens.typography must be an object";
			return false;
		}
		const json& typography = *typography_it;
		if (!ReadStringToken(typography, "uiFamily", result.uiFamily, error)) return false;
		if (!ReadStringToken(typography, "monoFamily", result.monoFamily, error)) return false;
		// `scale` is the schema name; `uiScale` is accepted for the older
		// PLUGIN_SPEC draft where it appeared under typography.
		const float scale = NumberOr(typography, "scale", NumberOr(typography, "uiScale", 1.0f));
		result.scale = std::clamp(scale, 0.8f, 2.0f);
	}

	return true;
}

bool LoadThemeFile(const std::string& path, ThemeResult& result, std::string& error) {
	std::ifstream input(path, std::ios::binary);
	if (!input) {
		error = "cannot open theme file " + path;
		return false;
	}
	std::ostringstream buffer;
	buffer << input.rdbuf();
	return ParseThemeDocument(buffer.str(), result, error);
}

bool ApplyThemeDocument(const std::string& jsonText, std::string& error) {
	ThemeResult result;
	if (!ParseThemeDocument(jsonText, result, error))
		return false;
	ImGui::GetStyle() = result.style;
	return true;
}

} // namespace casioemu::plugin
