#include "ThemeTokens.h"

#include "../../../McpPlugin/json.hpp"

#include <imgui.h>

#include <cctype>
#include <cmath>
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

bool ReadColor(const json& colors, const char* key, ImVec4& color, std::string& error) {
	auto it = colors.find(key);
	if (it == colors.end())
		return true;
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

} // namespace

bool ApplyThemeDocument(const std::string& jsonText, std::string& error) {
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

	std::string variant = "dark";
	if (document.contains("variant")) {
		if (!document["variant"].is_string()) {
			error = "theme variant must be 'dark' or 'light'";
			return false;
		}
		variant = document["variant"].get<std::string>();
	}
	if (variant != "dark" && variant != "light") {
		error = "theme variant must be 'dark' or 'light'";
		return false;
	}

	ImGuiStyle style;
	if (variant == "light")
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
		if (!ReadColor(colors, "background", value, error)) return false;
		if (colors.contains("background")) {
			style.Colors[ImGuiCol_WindowBg] = value;
			style.Colors[ImGuiCol_PopupBg] = value;
			style.Colors[ImGuiCol_MenuBarBg] = value;
			style.Colors[ImGuiCol_TitleBg] = value;
		}
		if (!ReadColor(colors, "surface", value, error)) return false;
		if (colors.contains("surface")) {
			style.Colors[ImGuiCol_ChildBg] = value;
			style.Colors[ImGuiCol_FrameBg] = value;
			style.Colors[ImGuiCol_Tab] = value;
			style.Colors[ImGuiCol_TabUnfocused] = value;
		}
		if (!ReadColor(colors, "surfaceAlt", value, error)) return false;
		if (colors.contains("surfaceAlt")) {
			style.Colors[ImGuiCol_FrameBgHovered] = value;
			style.Colors[ImGuiCol_TabUnfocusedActive] = value;
		}
		if (!ReadColor(colors, "foreground", value, error)) return false;
		if (colors.contains("foreground"))
			style.Colors[ImGuiCol_Text] = value;
		if (!ReadColor(colors, "muted", value, error)) return false;
		if (colors.contains("muted"))
			style.Colors[ImGuiCol_TextDisabled] = value;
		if (!ReadColor(colors, "border", value, error)) return false;
		if (colors.contains("border")) {
			style.Colors[ImGuiCol_Border] = value;
			style.Colors[ImGuiCol_Separator] = value;
		}
		if (!ReadColor(colors, "accent", value, error)) return false;
		if (colors.contains("accent")) {
			style.Colors[ImGuiCol_CheckMark] = value;
			style.Colors[ImGuiCol_SliderGrab] = value;
			style.Colors[ImGuiCol_HeaderActive] = value;
			style.Colors[ImGuiCol_TabActive] = value;
			style.Colors[ImGuiCol_ButtonActive] = value;
		}
		if (!ReadColor(colors, "accentHover", value, error)) return false;
		if (colors.contains("accentHover")) {
			style.Colors[ImGuiCol_ButtonHovered] = value;
			style.Colors[ImGuiCol_HeaderHovered] = value;
			style.Colors[ImGuiCol_SliderGrabActive] = value;
			style.Colors[ImGuiCol_TabHovered] = value;
		}
		if (!ReadColor(colors, "selection", value, error)) return false;
		if (colors.contains("selection")) {
			style.Colors[ImGuiCol_Header] = value;
			style.Colors[ImGuiCol_TextSelectedBg] = value;
			style.Colors[ImGuiCol_NavHighlight] = value;
		}
		if (!ReadColor(colors, "hover", value, error)) return false;
		if (colors.contains("hover")) {
			style.Colors[ImGuiCol_FrameBgHovered] = value;
			style.Colors[ImGuiCol_HeaderHovered] = value;
			style.Colors[ImGuiCol_TabHovered] = value;
		}
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

	ImGui::GetStyle() = style;
	return true;
}

} // namespace casioemu::plugin
