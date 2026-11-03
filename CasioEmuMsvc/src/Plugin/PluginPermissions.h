#pragma once

#include "PluginManifest.h"

#include <string>
#include <vector>

namespace casioemu::plugin {

// Capabilities are the runtime units the host checks.  A manifest declares
// permissions; each permission grants one or more capabilities.  See
// docs/PLUGIN-GUIDE.md ("Permission enforcement") for the public table.
enum class Capability {
	EmulatorRead,
	EmulatorWrite,
	EmulatorCode,
	Inject,
	FilesystemRead,
	FilesystemWrite,
	Network,
	Process,
	Shell,
	Window,
	Theme,
	Clipboard,
	Native,
};

[[nodiscard]] const char* CapabilityName(Capability capability);

// The permission a plugin author should declare to obtain `capability`.
// When several permissions grant the same capability this returns the most
// specific one (the first entry is the canonical recommendation).
[[nodiscard]] std::vector<std::string> CapabilityPermissions(Capability capability);

// A resolved grant set.  `Allows` is true when the manifest declared at least
// one permission that grants the capability.
class PermissionSet {
public:
	PermissionSet() = default;
	explicit PermissionSet(const std::vector<std::string>& permissions);

	[[nodiscard]] bool Allows(Capability capability) const;
	[[nodiscard]] bool Empty() const { return m_permissions.empty(); }
	[[nodiscard]] const std::vector<std::string>& Permissions() const { return m_permissions; }
	[[nodiscard]] const std::string& PluginId() const { return m_pluginId; }
	void SetPluginId(std::string id) { m_pluginId = std::move(id); }

private:
	std::string m_pluginId;
	std::vector<std::string> m_permissions;
};

// Stable error code used when a capability check fails.
inline constexpr const char* kPermissionDeniedCode = "plugin.permission";

// A denial is recorded by the host API and drained by the registry so the
// plugin manager can mark the entry "limited".
struct PermissionDenial {
	std::string pluginId;
	std::string capability;
	std::string api;
};

// Builds "permission denied: <api> requires the '<capability>' capability
// (declare permission 'x' | 'y')" and records the denial for `pluginId`.
std::string RecordPermissionDenial(const std::string& pluginId, Capability capability,
	const std::string& api);

std::vector<PermissionDenial> TakePermissionDenials(const std::string& pluginId);
std::vector<PermissionDenial> PendingPermissionDenials();
void ClearPermissionDenials(const std::string& pluginId);

// Strict mapping used by the manifest parser tests: returns false when the
// permission is unknown.
[[nodiscard]] bool PermissionGrants(const std::string& permission, Capability capability);

} // namespace casioemu::plugin
