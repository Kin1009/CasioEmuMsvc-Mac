#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace casioemu::plugin {

inline constexpr int kManifestSchema = 1;
inline constexpr int kHostApiVersion = 2;
inline constexpr const char* kHostVersion = "2.1.0";

enum class PluginType {
	Theme,
	Tab,
	Inject,
	UiRevamp,
	Tool,
};

struct Manifest {
	int schema = kManifestSchema;
	std::string id;
	std::string name;
	std::string version;
	std::string author;
	std::string description;
	std::string homepage;
	std::string minHost;
	std::vector<std::string> platforms{ "macos" };
	PluginType type = PluginType::Theme;
	std::string typeName;
	std::vector<std::string> permissions;
	std::string entryPoint;
	std::vector<std::string> assets;
	std::string nativeEntry;
	int apiVersion = 1;

	[[nodiscard]] bool HasPermission(const std::string& permission) const;
	[[nodiscard]] bool IsNative() const { return !nativeEntry.empty(); }
};

struct ManifestResult {
	bool ok = false;
	Manifest manifest;
	std::string code;
	std::string message;
};

ManifestResult ParseManifest(const std::string& jsonText);
ManifestResult LoadManifestFile(const std::filesystem::path& path);

[[nodiscard]] bool IsKnownPermission(const std::string& permission);
[[nodiscard]] const char* PermissionDescription(const std::string& permission);
[[nodiscard]] const char* TypeName(PluginType type);
[[nodiscard]] bool ParsePluginType(const std::string& name, PluginType& out);
[[nodiscard]] bool IsSafeRelativePath(const std::string& path);
[[nodiscard]] bool IsValidPluginId(const std::string& id);
[[nodiscard]] bool IsValidSemver(const std::string& version);
[[nodiscard]] int CompareSemver(const std::string& lhs, const std::string& rhs);

} // namespace casioemu::plugin
