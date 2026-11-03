#pragma once

#include "PluginPermissions.h"
#include "PluginPackage.h"
#include "PluginTrust.h"

#include <filesystem>
#include <string>
#include <vector>

namespace casioemu::plugin {

struct PluginEntry {
	Package package;
	bool trusted = false;
	bool trustedThisSession = false;
	bool blocked = false;
	bool enabled = true;
	bool loaded = false;
	bool pendingTrust = false;
	bool removed = false;
	// Runtime capability denials recorded by the PluginApi facade.
	bool limited = false;
	std::vector<std::string> deniedCapabilities;
	// Set when fPluginUnload ran but the library had to stay mapped because the
	// plugin registered hooks that the current API cannot unregister.
	bool libraryRetained = false;
	void* handle = nullptr;
	void* apiContext = nullptr;
	std::string trustedAt;
	std::string state = "Discovered";
	std::string error;
	std::string document;

	[[nodiscard]] bool IsNative() const { return package.manifest.IsNative(); }
	[[nodiscard]] const std::string& Id() const { return package.manifest.id; }
	[[nodiscard]] const std::string& Name() const { return package.manifest.name; }
	[[nodiscard]] const std::string& Version() const { return package.manifest.version; }
};

class PluginRegistry {
public:
	static PluginRegistry& Instance();

	void Initialize();
	// Calls fPluginUnload for loaded native plugins, hides their windows and
	// closes the libraries when it is safe to do so.  Safe to call twice.
	void Shutdown();
	// Per-frame host pump.  RenderTrustModal() calls it, so the existing call
	// site in Gui/Ui.cpp also services theme reapplication and trust state.
	void PumpFrame();
	void RenderTrustModal();

	// Copies `file` into the user plugin folder and loads it immediately.
	// Returns false and fills `error` on invalid input.
	bool InstallFromFile(const std::filesystem::path& file, std::string* error = nullptr);
	// Opens a native file chooser and installs the selected package.
	void InstallFromDialog();

	void ReloadDeclarative();
	void SetEnabled(const std::string& id, bool enabled);
	void Remove(const std::string& id);
	// Explicit unload: fPluginUnload, hide/remove plugin windows, drop payloads,
	// clear runtime state, then dlclose when safe.  The entry stays installed.
	bool Unload(const std::string& id);
	void Reveal(const std::string& id) const;
	void RevealPath(const std::filesystem::path& path) const;
	void OpenPluginsFolder() const;
	void Review(const std::string& id);

	// Re-applies the most recent plugin theme (used after the startup font
	// rebuild, which overwrites ImGui's style).
	void ReapplyActiveTheme();

	[[nodiscard]] std::vector<PluginEntry>& Entries() { return m_entries; }
	[[nodiscard]] const std::vector<PluginEntry>& Entries() const { return m_entries; }
	[[nodiscard]] std::string Log() const;
	[[nodiscard]] int HostApiVersion() const;

	[[nodiscard]] std::filesystem::path SettingsDir() const;
	[[nodiscard]] std::filesystem::path UserPluginsDir() const;
	[[nodiscard]] std::filesystem::path AppPluginsDir() const;
	[[nodiscard]] std::filesystem::path CacheDir() const;

private:
	PluginRegistry() = default;
	PluginRegistry(const PluginRegistry&) = delete;
	PluginRegistry& operator=(const PluginRegistry&) = delete;

	void LoadTrust();
	void SaveTrust();
	void ScanDirectory(const std::filesystem::path& directory);
	void AddPackage(const std::filesystem::path& file);
	void ApplyTrustAndLoad(PluginEntry& entry);
	void LoadEntry(PluginEntry& entry);
	bool LoadNative(PluginEntry& entry);
	bool LoadDeclarative(PluginEntry& entry);
	void Approve(PluginEntry& entry, bool remember);
	void Reject(PluginEntry& entry);
	void LogLine(const std::string& line);

	// Capability gate for the declarative loaders.  On failure sets the entry
	// state to Error with a plugin.permission error and returns false.
	bool RequireCapability(PluginEntry& entry, Capability capability, const std::string& phase);
	void ApplyDenials(PluginEntry& entry);
	void HidePluginWindows(PluginEntry& entry);
	bool UnloadEntry(PluginEntry& entry, bool removeWindows);
	void ShutdownInternal(bool removeWindows);
	static void AtExit();
	void ClearRuntimeState(PluginEntry& entry);
	bool ApplyLayout(PluginEntry& entry, const struct LayoutActions& actions, std::string& error);

	[[nodiscard]] int FirstPendingIndex() const;

	std::vector<PluginEntry> m_entries;
	TrustStore m_trust;
	bool m_initialized = false;
	int m_modalIndex = -1;
	bool m_openModalRequested = false;
	bool m_modalActive = false;
	std::string m_activeThemeId;
	std::string m_activeThemeDocument;
	bool m_themeStale = false;
};

} // namespace casioemu::plugin
