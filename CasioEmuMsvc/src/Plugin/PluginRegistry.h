#pragma once

#include "PluginPackage.h"

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
	void* handle = nullptr;
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
	void Shutdown();
	void RenderTrustModal();

	void InstallFromFile(const std::filesystem::path& file);
	void ReloadDeclarative();
	void SetEnabled(const std::string& id, bool enabled);
	void Remove(const std::string& id);
	void Reveal(const std::string& id) const;
	void RevealPath(const std::filesystem::path& path) const;
	void OpenPluginsFolder() const;
	void Review(const std::string& id);

	[[nodiscard]] std::vector<PluginEntry>& Entries() { return m_entries; }
	[[nodiscard]] const std::vector<PluginEntry>& Entries() const { return m_entries; }
	[[nodiscard]] std::string Log() const;
	[[nodiscard]] int HostApiVersion() const;

	[[nodiscard]] std::filesystem::path SettingsDir() const;
	[[nodiscard]] std::filesystem::path UserPluginsDir() const;
	[[nodiscard]] std::filesystem::path AppPluginsDir() const;
	[[nodiscard]] std::filesystem::path CacheDir() const;

private:
	struct TrustRecord {
		std::string id;
		std::string version;
		std::string sha256;
		std::vector<std::string> permissions;
		bool enabled = true;
		bool blocked = false;
	};

	PluginRegistry() = default;
	PluginRegistry(const PluginRegistry&) = delete;
	PluginRegistry& operator=(const PluginRegistry&) = delete;

	void LoadTrust();
	void SaveTrust() const;
	TrustRecord* FindTrust(const std::string& id);
	const TrustRecord* FindTrust(const std::string& id) const;
	void UpsertTrust(const PluginEntry& entry, bool blocked);
	void RemoveTrust(const std::string& id);

	void ScanDirectory(const std::filesystem::path& directory);
	void AddPackage(const std::filesystem::path& file);
	void ApplyTrustAndLoad(PluginEntry& entry);
	void LoadEntry(PluginEntry& entry);
	bool LoadNative(PluginEntry& entry);
	bool LoadDeclarative(PluginEntry& entry);
	void Approve(PluginEntry& entry, bool remember);
	void Reject(PluginEntry& entry);
	void LogLine(const std::string& line);

	[[nodiscard]] int FirstPendingIndex() const;

	std::vector<PluginEntry> m_entries;
	std::vector<TrustRecord> m_trust;
	bool m_initialized = false;
	int m_modalIndex = -1;
	bool m_openModalRequested = false;
	bool m_modalActive = false;
};

} // namespace casioemu::plugin
