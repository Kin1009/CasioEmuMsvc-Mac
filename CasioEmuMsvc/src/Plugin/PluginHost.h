#pragma once

#include "PluginManifest.h"

#include <string>
#include <vector>

class PluginApi;

namespace casioemu::plugin {

struct LayoutActions;

// Narrow bridge between the plugin registry and the GUI shell.  Keeping the
// registry behind this interface makes the loader testable headlessly (see
// tests/plugin_registry_headless.cpp) and documents exactly which host
// services a plugin load can touch.
class PluginHost {
public:
	virtual ~PluginHost() = default;

	// Directory that owns `portable.flag`, `plugins/` and `settings/`.
	virtual std::string ExecutableDir() = 0;
	// Native file chooser used by InstallFromDialog.  Returns false when the
	// user cancels.
	virtual bool PickPluginFile(std::string& path) = 0;

	// Theme services.  ApplyPluginTheme parses a declarative theme document and
	// assigns ImGui's style (the registry retries it after font rebuilds).
	virtual bool ApplyPluginTheme(const std::string& document, std::string& error) = 0;
	virtual void ResetTheme() = 0;
	virtual bool SelectTheme(const std::string& themeId, std::string& error) = 0;
	virtual bool FontRebuildPending() = 0;

	// Window services used by unload and declarative layout.
	virtual std::vector<void*> Windows() = 0;
	virtual const char* WindowName(const void* window) = 0;
	virtual void SetWindowOpen(void* window, bool open) = 0;
	virtual void RemoveWindow(void* window) = 0;
	virtual void ReorderWindows(const std::vector<std::string>& names) = 0;

	// Persist sidebar/bottom/tool settings.
	virtual bool ApplyShellLayout(const LayoutActions& actions, std::string& error) = 0;

	// Per-plugin PluginApi facade (permission enforcement lives there).
	virtual PluginApi* CreateApi(const std::string& pluginId, const Manifest& manifest) = 0;
	virtual void DestroyApi(PluginApi* api) = 0;
};

// The process-wide host.  The GUI build registers GuiPluginHost from
// PluginHostGui.cpp; tests call SetPluginHost() with a fake before touching the
// registry.
PluginHost& Host();
void SetPluginHost(PluginHost* host);

} // namespace casioemu::plugin
