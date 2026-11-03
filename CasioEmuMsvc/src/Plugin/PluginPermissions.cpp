#include "PluginPermissions.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <iterator>
#include <mutex>

namespace casioemu::plugin {
namespace {

struct Grant {
	const char* permission;
	std::initializer_list<Capability> capabilities;
};

// Keep this table in sync with kPermissionNames in PluginManifest.cpp.
constexpr std::array<Grant, 10> kGrants = { {
	{ "emulator.read", { Capability::EmulatorRead } },
	{ "emulator.write", { Capability::EmulatorWrite } },
	{ "emulator.code", { Capability::EmulatorCode, Capability::EmulatorWrite } },
	{ "emulator.inject", { Capability::Inject } },
	{ "filesystem.read", { Capability::FilesystemRead } },
	{ "filesystem.write", { Capability::FilesystemWrite } },
	{ "network", { Capability::Network } },
	{ "process", { Capability::Process, Capability::Native } },
	{ "ui.shell", { Capability::Shell, Capability::Window, Capability::Theme, Capability::Native } },
	{ "clipboard", { Capability::Clipboard } },
} };

std::mutex g_denial_mutex;
std::vector<PermissionDenial> g_denials;

} // namespace

const char* CapabilityName(Capability capability) {
	switch (capability) {
	case Capability::EmulatorRead: return "emulator.read";
	case Capability::EmulatorWrite: return "emulator.write";
	case Capability::EmulatorCode: return "emulator.code";
	case Capability::Inject: return "inject";
	case Capability::FilesystemRead: return "filesystem.read";
	case Capability::FilesystemWrite: return "filesystem.write";
	case Capability::Network: return "network";
	case Capability::Process: return "process";
	case Capability::Shell: return "ui.shell";
	case Capability::Window: return "ui.window";
	case Capability::Theme: return "theme";
	case Capability::Clipboard: return "clipboard";
	case Capability::Native: return "native";
	}
	return "unknown";
}

std::vector<std::string> CapabilityPermissions(Capability capability) {
	std::vector<std::string> result;
	// Prefer the specific permission for the narrower capabilities, then the
	// broader ui.shell/process grants that also imply them.
	for (const auto& grant : kGrants) {
		if (std::find(grant.capabilities.begin(), grant.capabilities.end(), capability) != grant.capabilities.end())
			result.emplace_back(grant.permission);
	}
	std::sort(result.begin(), result.end(), [](const std::string& lhs, const std::string& rhs) {
		if (lhs.size() != rhs.size())
			return lhs.size() < rhs.size();
		return lhs < rhs;
	});
	return result;
}

PermissionSet::PermissionSet(const std::vector<std::string>& permissions) : m_permissions(permissions) {}

bool PermissionSet::Allows(Capability capability) const {
	for (const auto& permission : m_permissions) {
		if (PermissionGrants(permission, capability))
			return true;
	}
	return false;
}

bool PermissionGrants(const std::string& permission, Capability capability) {
	for (const auto& grant : kGrants) {
		if (permission != grant.permission)
			continue;
		return std::find(grant.capabilities.begin(), grant.capabilities.end(), capability) != grant.capabilities.end();
	}
	return false;
}

std::string RecordPermissionDenial(const std::string& pluginId, Capability capability,
	const std::string& api) {
	const auto permissions = CapabilityPermissions(capability);
	std::string message = "permission denied: '" + api + "' requires the '" +
		CapabilityName(capability) + "' capability (declare ";
	for (size_t i = 0; i < permissions.size(); ++i) {
		if (i > 0)
			message += " or ";
		message += "'" + permissions[i] + "'";
	}
	message += ")";

	{
		std::lock_guard<std::mutex> lock(g_denial_mutex);
		g_denials.push_back({ pluginId, CapabilityName(capability), api });
	}
	return message;
}

std::vector<PermissionDenial> TakePermissionDenials(const std::string& pluginId) {
	std::lock_guard<std::mutex> lock(g_denial_mutex);
	std::vector<PermissionDenial> taken;
	auto split = std::stable_partition(g_denials.begin(), g_denials.end(),
		[&](const PermissionDenial& denial) { return denial.pluginId != pluginId; });
	std::move(split, g_denials.end(), std::back_inserter(taken));
	g_denials.erase(split, g_denials.end());
	return taken;
}

std::vector<PermissionDenial> PendingPermissionDenials() {
	std::lock_guard<std::mutex> lock(g_denial_mutex);
	return g_denials;
}

void ClearPermissionDenials(const std::string& pluginId) {
	std::lock_guard<std::mutex> lock(g_denial_mutex);
	g_denials.erase(std::remove_if(g_denials.begin(), g_denials.end(),
		[&](const PermissionDenial& denial) { return denial.pluginId == pluginId; }), g_denials.end());
}

} // namespace casioemu::plugin
