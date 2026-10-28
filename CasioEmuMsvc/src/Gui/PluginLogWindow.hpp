#pragma once
#include "Ui.hpp"
#include <string>

extern std::string g_PluginLoadLog;

class PluginLogWindow : public UIWindow {
public:
	PluginLogWindow();
	void RenderCore() override;

private:
	std::string m_removeId;
	bool m_openRemovePopup = false;
	void RenderInstalled();
	void RenderDeveloper();
	void RenderRemovePopup();
};
