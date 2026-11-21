#pragma once
// LEGACY: the plugin manager UI moved to Settings > Plugins (Gui/Theme.cpp).
// This class is no longer instantiated or registered as a tool window.
#include "Ui.hpp"
#include <string>

extern std::string g_PluginLoadLog;

class PluginLogWindow : public UIWindow {
public:
	PluginLogWindow();
	void RenderCore() override;

private:
	std::string m_removeId;
	std::string m_status;
	bool m_openRemovePopup = false;
	void RenderInstalled();
	void RenderDeveloper();
	void RenderRemovePopup();
};
