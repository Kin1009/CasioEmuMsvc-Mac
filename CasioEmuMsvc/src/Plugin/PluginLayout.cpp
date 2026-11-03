#include "PluginLayout.h"

#include "../../../McpPlugin/json.hpp"

#include <algorithm>
#include <cctype>

namespace casioemu::plugin {
namespace {

using json = nlohmann::json;

bool ReadBool(const json& object, const char* key, bool& out, std::string& error) {
	auto it = object.find(key);
	if (it == object.end() || it->is_null())
		return true;
	if (!it->is_boolean()) {
		error = std::string("'") + key + "' must be a boolean";
		return false;
	}
	out = it->get<bool>();
	return true;
}

bool ReadInt(const json& object, const char* key, int& out, std::string& error) {
	auto it = object.find(key);
	if (it == object.end() || it->is_null())
		return true;
	if (!it->is_number_integer()) {
		error = std::string("'") + key + "' must be an integer";
		return false;
	}
	out = it->get<int>();
	return true;
}

bool ReadString(const json& object, const char* key, std::string& out, std::string& error) {
	auto it = object.find(key);
	if (it == object.end() || it->is_null())
		return true;
	if (!it->is_string()) {
		error = std::string("'") + key + "' must be a string";
		return false;
	}
	out = it->get<std::string>();
	return true;
}

bool ReadStringArray(const json& object, const char* key, std::vector<std::string>& out, std::string& error) {
	auto it = object.find(key);
	if (it == object.end() || it->is_null())
		return true;
	if (!it->is_array()) {
		error = std::string("'") + key + "' must be an array of strings";
		return false;
	}
	out.clear();
	for (const auto& item : *it) {
		if (!item.is_string()) {
			error = std::string("'") + key + "' must be an array of strings";
			return false;
		}
		out.push_back(item.get<std::string>());
	}
	return true;
}

const json* ObjectAt(const json& object, const char* key, std::string& error, const std::string& context) {
	auto it = object.find(key);
	if (it == object.end() || it->is_null())
		return nullptr;
	if (!it->is_object()) {
		error = context + "." + key + " must be an object";
		return nullptr;
	}
	return &*it;
}

bool IsKnownBottomTab(const std::string& id) {
	return id == "output" || id == "problems" || id == "terminal";
}

std::string Lower(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

} // namespace

bool ParseLayoutDocument(const std::string& jsonText, LayoutActions& out, std::string& error) {
	out = {};
	error.clear();
	json document;
	try {
		document = json::parse(jsonText);
	}
	catch (const json::exception& exception) {
		error = std::string(kLayoutInvalidCode) + ": layout document is not valid JSON: " + exception.what();
		return false;
	}
	if (!document.is_object()) {
		error = std::string(kLayoutInvalidCode) + ": layout document must be a JSON object";
		return false;
	}
	if (document.value("schema", 0) != 1) {
		error = std::string(kLayoutInvalidCode) + ": declarative document must have schema 1";
		return false;
	}

	// Theme selection is a documented action; accept it at the root and in the
	// shell object so layout packages can bundle a theme preference.
	if (!ReadString(document, "theme", out.theme, error)) {
		error = std::string(kLayoutInvalidCode) + ": " + error;
		return false;
	}
	out.hasTheme = !out.theme.empty();

	const json* shell = ObjectAt(document, "shell", error, "layout");
	if (!error.empty()) {
		error = std::string(kLayoutInvalidCode) + ": " + error;
		return false;
	}
	if (!shell)
		return true;

	static constexpr const char* kKnownShellKeys[] = {
		"type", "sidebar", "bottom", "tools", "sessions", "theme",
	};
	for (auto it = shell->begin(); it != shell->end(); ++it) {
		if (std::find_if(std::begin(kKnownShellKeys), std::end(kKnownShellKeys),
				[&](const char* key) { return it.key() == key; }) == std::end(kKnownShellKeys)) {
			error = std::string(kLayoutUnsupportedCode) + ": unsupported action 'shell." + it.key() + "'";
			return false;
		}
	}

	std::string type = "default";
	if (!ReadString(*shell, "type", type, error)) {
		error = std::string(kLayoutInvalidCode) + ": shell." + error;
		return false;
	}
	if (type != "default") {
		error = std::string(kLayoutUnsupportedCode) + ": unsupported action: shell.type '" + type +
			"' is not available in this host; only the default shell is supported";
		return false;
	}

	if (!out.hasTheme) {
		if (!ReadString(*shell, "theme", out.theme, error)) {
			error = std::string(kLayoutInvalidCode) + ": shell." + error;
			return false;
		}
		out.hasTheme = !out.theme.empty();
	}

	if (const json* sidebar = ObjectAt(*shell, "sidebar", error, "shell")) {
		out.hasSidebar = true;
		if (!ReadBool(*sidebar, "visible", out.sidebarVisible, error) ||
			!ReadInt(*sidebar, "width", out.sidebarWidth, error)) {
			error = std::string(kLayoutInvalidCode) + ": shell.sidebar." + error;
			return false;
		}
		out.hasSidebarWidth = sidebar->contains("width");
		if (out.sidebarWidth < 160 || out.sidebarWidth > 600) {
			error = std::string(kLayoutInvalidCode) + ": shell.sidebar.width must be 160-600";
			return false;
		}
	}
	else if (!error.empty()) {
		error = std::string(kLayoutInvalidCode) + ": " + error;
		return false;
	}

	if (const json* bottom = ObjectAt(*shell, "bottom", error, "shell")) {
		out.hasBottom = true;
		if (!ReadBool(*bottom, "visible", out.bottomVisible, error) ||
			!ReadInt(*bottom, "height", out.bottomHeight, error) ||
			!ReadString(*bottom, "active", out.bottomActive, error)) {
			error = std::string(kLayoutInvalidCode) + ": shell.bottom." + error;
			return false;
		}
		out.hasBottomHeight = bottom->contains("height");
		out.hasBottomActive = bottom->contains("active");
		if (out.hasBottomHeight && (out.bottomHeight < 80 || out.bottomHeight > 800)) {
			error = std::string(kLayoutInvalidCode) + ": shell.bottom.height must be 80-800";
			return false;
		}
		if (out.hasBottomActive && !IsKnownBottomTab(out.bottomActive)) {
			error = std::string(kLayoutUnsupportedCode) + ": unsupported action: shell.bottom.active '" +
				out.bottomActive + "' is not available (output, problems, terminal)";
			return false;
		}
	}
	else if (!error.empty()) {
		error = std::string(kLayoutInvalidCode) + ": " + error;
		return false;
	}

	if (const json* tools = ObjectAt(*shell, "tools", error, "shell")) {
		out.hasTools = true;
		if (!ReadBool(*tools, "visible", out.toolsVisible, error) ||
			!ReadInt(*tools, "width", out.toolsWidth, error) ||
			!ReadStringArray(*tools, "tabs", out.toolOrder, error) ||
			!ReadStringArray(*tools, "hidden", out.hiddenTools, error)) {
			error = std::string(kLayoutInvalidCode) + ": shell.tools." + error;
			return false;
		}
		out.hasToolsWidth = tools->contains("width");
		if (out.hasToolsWidth && (out.toolsWidth < 200 || out.toolsWidth > 800)) {
			error = std::string(kLayoutInvalidCode) + ": shell.tools.width must be 200-800";
			return false;
		}
	}
	else if (!error.empty()) {
		error = std::string(kLayoutInvalidCode) + ": " + error;
		return false;
	}

	if (const json* sessions = ObjectAt(*shell, "sessions", error, "shell")) {
		out.hasSessions = true;
		if (!ReadString(*sessions, "tabs", out.sessionTabs, error) ||
			!ReadBool(*sessions, "showClose", out.sessionShowClose, error)) {
			error = std::string(kLayoutInvalidCode) + ": shell.sessions." + error;
			return false;
		}
		out.hasSessionTabs = sessions->contains("tabs");
		out.hasSessionClose = sessions->contains("showClose");
		if (out.hasSessionTabs) {
			const std::string placement = Lower(out.sessionTabs);
			if (placement != "top") {
				error = std::string(kLayoutUnsupportedCode) + ": unsupported action: shell.sessions.tabs '" +
					out.sessionTabs + "' is not supported; this host only places session tabs at the top";
				return false;
			}
		}
		if (out.hasSessionClose && !out.sessionShowClose) {
			error = std::string(kLayoutUnsupportedCode) +
				": unsupported action: shell.sessions.showClose=false is not supported; the host always shows close buttons";
			return false;
		}
	}
	else if (!error.empty()) {
		error = std::string(kLayoutInvalidCode) + ": " + error;
		return false;
	}

	return true;
}

} // namespace casioemu::plugin
