#include "PluginManifest.h"
#include "PluginPackage.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace casioemu::plugin;

namespace {

void WriteFile(const std::filesystem::path& path, const std::string& text) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream output(path, std::ios::binary);
	output << text;
}

std::string ManifestText(const std::string& id, const std::string& type,
                         const std::string& permissions, const std::string& native = "") {
	return std::string("{\n") +
		"  \"schema\": 1,\n" +
		"  \"id\": \"" + id + "\",\n" +
		"  \"name\": \"Loader smoke test\",\n" +
		"  \"version\": \"1.0.0\",\n" +
		"  \"type\": \"" + type + "\",\n" +
		"  \"permissions\": " + permissions + ",\n" +
		"  \"entry_point\": \"assets/entry.json\"" +
		(native.empty() ? "" : ",\n  \"native_entry\": \"" + native + "\"") +
		"\n}\n";
}

} // namespace

int main() {
	ManifestResult result = ParseManifest(ManifestText("com.example.theme", "theme", "[\"ui.shell\"]"));
	assert(result.ok);
	assert(result.manifest.type == PluginType::Theme);
	assert(result.manifest.HasPermission("ui.shell"));

	result = ParseManifest(ManifestText("com.example.tool", "tool", "[\"ui.shell\"]"));
	assert(!result.ok);
	assert(result.code == "manifest.field");
	result = ParseManifest(ManifestText("com.example.tool", "tool", "[\"ui.shell\"]", "lib/example.dylib"));
	assert(result.ok);
	assert(result.manifest.IsNative());

	result = ParseManifest(ManifestText("NotAnId", "theme", "[]"));
	assert(!result.ok);
	assert(result.code == "manifest.id");
	result = ParseManifest(ManifestText("com.example.bad", "theme", "[\"root\"]"));
	assert(!result.ok);
	assert(result.code == "manifest.permission");

	assert(IsSafeRelativePath("assets/theme.json"));
	assert(IsSafeRelativePath("assets/"));
	assert(!IsSafeRelativePath("../escape"));
	assert(!IsSafeRelativePath("/absolute"));
	assert(!IsSafeRelativePath("assets/../../escape"));

	const std::filesystem::path root =
		std::filesystem::temp_directory_path() / ("casioemu-plugin-test-" + std::to_string(::getpid()));
	std::filesystem::remove_all(root);
	WriteFile(root / "plugin.json", ManifestText("com.example.fixture", "theme", "[\"ui.shell\"]"));
	WriteFile(root / "assets" / "entry.json",
		"{\"schema\":1,\"variant\":\"dark\",\"tokens\":{\"colors\":{\"background\":\"#1f1f1f\"}}}");
	std::filesystem::create_directories(root / "cache");
	const int zip_result = std::system(("cd \"" + root.string() + "\" && zip -q -r fixture.plugin .").c_str());
	assert(zip_result == 0);

	Package package;
	const bool extracted = ExtractPackage(root / "fixture.plugin", root / "cache", package);
	if (!extracted) {
		std::cerr << package.code << ": " << package.message << "\n";
		return 1;
	}
	assert(package.valid);
	assert(package.manifest.id == "com.example.fixture");
	assert(!package.sha256.empty());
	assert(std::filesystem::exists(package.root / "assets" / "entry.json"));

	std::filesystem::remove_all(root);
	std::cout << "plugin loader smoke test passed\n";
	return 0;
}
