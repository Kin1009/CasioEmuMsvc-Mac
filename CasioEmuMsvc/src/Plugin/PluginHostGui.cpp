#include "PluginHost.h"

#include "PluginHostApi.h"
#include "PluginLayout.h"
#define _NO_FUND_API
#include "PluginApi.h"
#include "ThemeManager.h"
#include "ThemeTokens.h"
#include "Ui.hpp"
#include "FileDialog.hpp"
#include "Settings/Settings.h"

#include <SDL.h>
#include <imgui.h>

#include <algorithm>
#include <filesystem>

namespace casioemu::plugin {
namespace {

class GuiPluginHost : public PluginHost {
public:
	std::string ExecutableDir() override {
		char* base = SDL_GetBasePath();
		if (!base)
			return std::filesystem::current_path().string();
		std::string path(base);
		SDL_free(base);
		return path;
	}

	bool PickPluginFile(std::string& path) override {
		char selected[4096]{};
		if (!FileDialog::ShowFileOpenDialog("Install Plugin", "Plugin packages (*.plugin)|*.plugin",
				selected, sizeof(selected)))
			return false;
		path = selected;
		return true;
	}

	bool ApplyPluginTheme(const std::string& document, std::string& error) override {
		if (!ApplyThemeDocument(document, error))
			return false;
		g_plugin_theme_active = true;
		return true;
	}

	void ResetTheme() override {
		g_plugin_theme_active = false;
		ThemeManager::Instance().ApplyDefaultTheme();
	}

	bool SelectTheme(const std::string& themeId, std::string& error) override {
		return ThemeManager::Instance().ApplyThemeByName(themeId, &error);
	}

	bool FontRebuildPending() override {
		return ThemeManager::Instance().IsFontRebuildRequested();
	}

	std::vector<void*> Windows() override {
		std::vector<void*> result;
		result.reserve(windows.size());
		for (UIWindow* window : windows)
			result.push_back(window);
		return result;
	}

	const char* WindowName(const void* window) override {
		const auto* typed = static_cast<const UIWindow*>(window);
		return typed && typed->name ? typed->name : "";
	}

	void SetWindowOpen(void* window, bool open) override {
		if (auto* typed = static_cast<UIWindow*>(window))
			typed->open = open;
	}

	void RemoveWindow(void* window) override {
		const auto* typed = static_cast<const UIWindow*>(window);
		windows.erase(std::remove(windows.begin(), windows.end(), typed), windows.end());
	}

	void ReorderWindows(const std::vector<std::string>& names) override {
		std::vector<UIWindow*> ordered;
		size_t first = windows.size();
		for (const auto& name : names) {
			for (size_t i = 0; i < windows.size(); ++i) {
				if (!windows[i] || !windows[i]->name || name != windows[i]->name)
					continue;
				if (std::find(ordered.begin(), ordered.end(), windows[i]) != ordered.end())
					continue;
				ordered.push_back(windows[i]);
				first = std::min(first, i);
				break;
			}
		}
		if (ordered.empty())
			return;
		std::vector<UIWindow*> remaining = ordered;
		windows.erase(std::remove_if(windows.begin(), windows.end(),
			[&](UIWindow* window) {
				const auto it = std::find(remaining.begin(), remaining.end(), window);
				if (it == remaining.end())
					return false;
				remaining.erase(it);
				return true;
			}), windows.end());
		const size_t insert_at = std::min(first, windows.size());
		windows.insert(windows.begin() + static_cast<std::ptrdiff_t>(insert_at),
			ordered.begin(), ordered.end());
	}

	bool ApplyShellLayout(const LayoutActions& actions, std::string& error) override {
		auto& store = settings::SettingsStore::Instance();
		if (!store.IsLoaded()) {
			std::string load_error;
			store.Load(&load_error);
		}
		auto& shell = store.Data().shell;

		if (actions.hasSidebar) {
			shell.sidebar.visible = actions.sidebarVisible;
			if (actions.hasSidebarWidth)
				shell.sidebar.width = actions.sidebarWidth;
		}
		if (actions.hasBottom) {
			shell.bottom.visible = actions.bottomVisible;
			if (actions.hasBottomHeight)
				shell.bottom.height = actions.bottomHeight;
			if (actions.hasBottomActive)
				shell.bottom.active = actions.bottomActive;
		}
		if (actions.hasTools) {
			shell.tools.visible = actions.toolsVisible;
			if (actions.hasToolsWidth)
				shell.tools.width = actions.toolsWidth;
		}
		store.Normalize();
		std::string save_error;
		if (!store.Save(&save_error)) {
			error = std::string(kLayoutInvalidCode) + ": cannot persist layout: " + save_error;
			return false;
		}
		return true;
	}

	PluginApi* CreateApi(const std::string& pluginId, const Manifest& manifest) override {
		return CreatePluginApi(pluginId, manifest);
	}

	void DestroyApi(PluginApi* api) override {
		DestroyPluginApi(api);
	}
};

GuiPluginHost g_gui_host;
const bool g_registered = (SetPluginHost(&g_gui_host), true);

} // namespace

unsigned long HostUiWindowAbiSignature() {
	return PluginUiWindowAbiSignature();
}

} // namespace casioemu::plugin
