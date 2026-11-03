#pragma once

#include <string>
#include <vector>

namespace casioemu::plugin {

// Declarative `ui_revamp` actions.  Each `has*` flag records whether the
// document actually asked for the value; the registry only touches host state
// for actions the plugin requested.
struct LayoutActions {
	bool hasSidebar = false;
	bool sidebarVisible = true;
	bool hasSidebarWidth = false;
	int sidebarWidth = 240;

	bool hasBottom = false;
	bool bottomVisible = true;
	bool hasBottomHeight = false;
	int bottomHeight = 160;
	bool hasBottomActive = false;
	std::string bottomActive = "output";

	bool hasTools = false;
	bool toolsVisible = true;
	bool hasToolsWidth = false;
	int toolsWidth = 360;
	std::vector<std::string> toolOrder;
	std::vector<std::string> hiddenTools;

	bool hasSessions = false;
	bool hasSessionTabs = false;
	std::string sessionTabs = "top";
	bool hasSessionClose = false;
	bool sessionShowClose = true;

	bool hasTheme = false;
	std::string theme;
};

// Parses a layout document.  Unknown shell actions fail with
// "unsupported action: ..." so a plugin cannot report Loaded while doing
// nothing.  Actions the host can honour are recorded in `out`.
bool ParseLayoutDocument(const std::string& jsonText, LayoutActions& out, std::string& error);

// Stable prefixes used in entry errors.
inline constexpr const char* kLayoutInvalidCode = "layout.invalid";
inline constexpr const char* kLayoutUnsupportedCode = "layout.unsupported";

} // namespace casioemu::plugin
