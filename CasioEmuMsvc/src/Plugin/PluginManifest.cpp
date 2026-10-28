#include "PluginManifest.h"

#include "../../../McpPlugin/json.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>

namespace casioemu::plugin {
namespace {

using json = nlohmann::json;

constexpr const char* kPermissionNames[] = {
	"emulator.read",
	"emulator.write",
	"emulator.code",
	"emulator.inject",
	"filesystem.read",
	"filesystem.write",
	"network",
	"process",
	"ui.shell",
	"clipboard",
};

constexpr const char* kPermissionDescriptions[] = {
	"Observe the model, ROM, memory, registers, and debugger state.",
	"Write RAM, registers, and memory monitors.",
	"Patch ROM or code bytes and hot-reload images.",
	"Register and run injection payloads.",
	"Read files outside the plugin package.",
	"Write files outside the plugin package.",
	"Make outbound network requests.",
	"Spawn processes, including terminal shells.",
	"Add or modify tabs, tools, layout, themes, or the shell.",
	"Read or write the clipboard.",
};

ManifestResult Fail(const std::string& code, const std::string& message) {
	ManifestResult result;
	result.ok = false;
	result.code = code;
	result.message = message;
	return result;
}

bool GetString(const json& object, const char* key, std::string& out, bool required, std::string& error) {
	auto it = object.find(key);
	if (it == object.end() || it->is_null()) {
		if (required) {
			error = std::string("missing required field '") + key + "'";
			return false;
		}
		return true;
	}
	if (!it->is_string()) {
		error = std::string("field '") + key + "' must be a string";
		return false;
	}
	out = it->get<std::string>();
	return true;
}

} // namespace

bool Manifest::HasPermission(const std::string& permission) const {
	return std::find(permissions.begin(), permissions.end(), permission) != permissions.end();
}

bool IsKnownPermission(const std::string& permission) {
	for (const char* known : kPermissionNames) {
		if (permission == known)
			return true;
	}
	return false;
}

const char* PermissionDescription(const std::string& permission) {
	for (size_t i = 0; i < std::size(kPermissionNames); ++i) {
		if (permission == kPermissionNames[i])
			return kPermissionDescriptions[i];
	}
	return "Unknown permission.";
}

const char* TypeName(PluginType type) {
	switch (type) {
	case PluginType::Theme: return "theme";
	case PluginType::Tab: return "tab";
	case PluginType::Inject: return "inject";
	case PluginType::UiRevamp: return "ui_revamp";
	case PluginType::Tool: return "tool";
	}
	return "unknown";
}

bool ParsePluginType(const std::string& name, PluginType& out) {
	if (name == "theme") { out = PluginType::Theme; return true; }
	if (name == "tab") { out = PluginType::Tab; return true; }
	if (name == "inject") { out = PluginType::Inject; return true; }
	if (name == "ui_revamp") { out = PluginType::UiRevamp; return true; }
	if (name == "tool") { out = PluginType::Tool; return true; }
	return false;
}

bool IsValidPluginId(const std::string& id) {
	if (id.size() < 3 || id.size() > 128)
		return false;
	bool has_dot = false;
	bool previous_dot = true;
	for (char c : id) {
		if (c == '.') {
			if (previous_dot)
				return false;
			has_dot = true;
			previous_dot = true;
			continue;
		}
		previous_dot = false;
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
			return false;
	}
	return has_dot && !previous_dot;
}

bool IsSafeRelativePath(const std::string& path) {
	if (path.empty())
		return false;
	if (path.front() == '/' || path.front() == '\\')
		return false;
	if (path.find('\\') != std::string::npos)
		return false;
	std::string normalized = path;
	if (normalized.back() == '/')
		normalized.pop_back();
	if (normalized.empty())
		return false;
	std::string component;
	std::stringstream stream(normalized);
	while (std::getline(stream, component, '/')) {
		if (component.empty() || component == ".")
			return false;
		if (component == "..")
			return false;
	}
	return true;
}

bool IsValidSemver(const std::string& version) {
	if (version.empty())
		return false;
	size_t index = 0;
	int numeric_parts = 0;
	while (numeric_parts < 3) {
		const size_t start = index;
		while (index < version.size() && std::isdigit(static_cast<unsigned char>(version[index])))
			++index;
		if (index == start)
			return false;
		++numeric_parts;
		if (numeric_parts < 3) {
			if (index >= version.size() || version[index] != '.')
				return false;
			++index;
		}
	}
	if (index < version.size() && version[index] != '-' && version[index] != '+')
		return false;
	for (size_t i = index; i < version.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(version[i]);
		if (!(std::isalnum(c) || c == '.' || c == '-' || c == '+'))
			return false;
	}
	return true;
}

namespace {

std::vector<std::string> SplitSemver(const std::string& version) {
	std::vector<std::string> parts;
	std::string current;
	for (char c : version) {
		if (c == '.' || c == '-' || c == '+') {
			parts.push_back(current);
			current.clear();
		}
		else {
			current.push_back(c);
		}
	}
	parts.push_back(current);
	return parts;
}

bool IsNumeric(const std::string& value) {
	return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); });
}

} // namespace

int CompareSemver(const std::string& lhs, const std::string& rhs) {
	const auto left = SplitSemver(lhs);
	const auto right = SplitSemver(rhs);
	for (size_t i = 0; i < 3; ++i) {
		const int a = i < left.size() && IsNumeric(left[i]) ? std::stoi(left[i]) : 0;
		const int b = i < right.size() && IsNumeric(right[i]) ? std::stoi(right[i]) : 0;
		if (a != b)
			return a < b ? -1 : 1;
	}
	// A version with a prerelease suffix sorts before the same version without one.
	const bool left_prerelease = lhs.find('-') != std::string::npos;
	const bool right_prerelease = rhs.find('-') != std::string::npos;
	if (left_prerelease != right_prerelease)
		return left_prerelease ? -1 : 1;
	return 0;
}

ManifestResult ParseManifest(const std::string& jsonText) {
	json document;
	try {
		document = json::parse(jsonText);
	}
	catch (const json::exception& error) {
		return Fail("manifest.invalid_json", error.what());
	}

	if (!document.is_object())
		return Fail("manifest.invalid_json", "plugin.json must be a JSON object");

	Manifest manifest;
	std::string error;

	if (!document.contains("schema") || !document["schema"].is_number_integer())
		return Fail("manifest.schema", "field 'schema' must be an integer");
	manifest.schema = document["schema"].get<int>();
	if (manifest.schema != kManifestSchema)
		return Fail("manifest.schema", "unsupported schema version " + std::to_string(manifest.schema));

	if (!GetString(document, "id", manifest.id, true, error))
		return Fail("manifest.field", error);
	if (!IsValidPluginId(manifest.id))
		return Fail("manifest.id", "id must be reverse-DNS-ish: lower-case letters, digits, '.', '_' or '-', with at least one dot");

	if (!GetString(document, "name", manifest.name, true, error))
		return Fail("manifest.field", error);
	if (manifest.name.empty() || manifest.name.size() > 80)
		return Fail("manifest.field", "name must be 1-80 characters");

	if (!GetString(document, "version", manifest.version, true, error))
		return Fail("manifest.field", error);
	if (!IsValidSemver(manifest.version))
		return Fail("manifest.version", "version must be semver (MAJOR.MINOR.PATCH)");

	GetString(document, "author", manifest.author, false, error);
	GetString(document, "description", manifest.description, false, error);
	GetString(document, "homepage", manifest.homepage, false, error);
	GetString(document, "min_host", manifest.minHost, false, error);
	GetString(document, "entry_point", manifest.entryPoint, true, error);
	GetString(document, "native_entry", manifest.nativeEntry, false, error);

	if (manifest.description.size() > 500)
		return Fail("manifest.field", "description must be at most 500 characters");
	if (!IsSafeRelativePath(manifest.entryPoint))
		return Fail("manifest.field", "entry_point must be a safe relative path");
	if (!manifest.nativeEntry.empty() && !IsSafeRelativePath(manifest.nativeEntry))
		return Fail("manifest.field", "native_entry must be a safe relative path");

	std::string type_name;
	if (!GetString(document, "type", type_name, true, error))
		return Fail("manifest.field", error);
	if (!ParsePluginType(type_name, manifest.type))
		return Fail("manifest.type", "unknown type '" + type_name + "'");
	manifest.typeName = type_name;

	if (document.contains("min_host")) {
		if (!IsValidSemver(manifest.minHost))
			return Fail("manifest.min_host", "min_host must be semver");
		if (CompareSemver(kHostVersion, manifest.minHost) < 0)
			return Fail("manifest.min_host", std::string("plugin requires host ") + manifest.minHost + " or newer");
	}

	if (document.contains("platforms")) {
		if (!document["platforms"].is_array())
			return Fail("manifest.platform", "platforms must be an array");
		manifest.platforms.clear();
		for (const auto& platform : document["platforms"]) {
			if (!platform.is_string())
				return Fail("manifest.platform", "platforms must contain strings");
			manifest.platforms.push_back(platform.get<std::string>());
		}
	}
	for (const auto& platform : manifest.platforms) {
		if (platform != "macos")
			return Fail("manifest.platform", "only the 'macos' platform is supported");
	}
	if (manifest.platforms.empty())
		manifest.platforms.push_back("macos");

	if (document.contains("permissions")) {
		if (!document["permissions"].is_array())
			return Fail("manifest.permission", "permissions must be an array");
		for (const auto& permission : document["permissions"]) {
			if (!permission.is_string())
				return Fail("manifest.permission", "permissions must contain strings");
			const std::string value = permission.get<std::string>();
			if (!IsKnownPermission(value))
				return Fail("manifest.permission", "unknown permission '" + value + "'");
			if (manifest.HasPermission(value))
				return Fail("manifest.permission", "duplicate permission '" + value + "'");
			manifest.permissions.push_back(value);
		}
	}

	if (document.contains("assets")) {
		if (!document["assets"].is_array())
			return Fail("manifest.field", "assets must be an array");
		for (const auto& asset : document["assets"]) {
			if (!asset.is_string())
				return Fail("manifest.field", "assets must contain strings");
			const std::string value = asset.get<std::string>();
			if (!IsSafeRelativePath(value))
				return Fail("manifest.field", "asset path must be a safe relative path");
			manifest.assets.push_back(value);
		}
	}

	if (document.contains("api_version")) {
		if (!document["api_version"].is_number_integer())
			return Fail("manifest.field", "api_version must be an integer");
		manifest.apiVersion = document["api_version"].get<int>();
		if (manifest.apiVersion < 1 || manifest.apiVersion > kHostApiVersion)
			return Fail("manifest.field", "unsupported api_version " + std::to_string(manifest.apiVersion));
	}

	const bool native_type = manifest.type == PluginType::Tab || manifest.type == PluginType::Tool;
	if (native_type && manifest.nativeEntry.empty())
		return Fail("manifest.field", "type '" + manifest.typeName + "' requires native_entry");
	if (manifest.type == PluginType::Theme && !manifest.nativeEntry.empty())
		return Fail("manifest.field", "theme plugins cannot include native code");
	if (manifest.type == PluginType::Inject && !manifest.nativeEntry.empty())
		return Fail("manifest.field", "inject plugins cannot include native code");

	ManifestResult result;
	result.ok = true;
	result.manifest = std::move(manifest);
	return result;
}

ManifestResult LoadManifestFile(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file)
		return Fail("manifest.missing", "cannot open plugin.json");
	std::ostringstream buffer;
	buffer << file.rdbuf();
	return ParseManifest(buffer.str());
}

} // namespace casioemu::plugin
