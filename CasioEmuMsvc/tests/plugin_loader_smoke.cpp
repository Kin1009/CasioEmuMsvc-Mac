#include "PluginLayout.h"
#include "PluginManifest.h"
#include "PluginNative.h"
#include "PluginPackage.h"
#include "PluginPayloads.h"
#include "PluginPermissions.h"
#include "PluginResources.h"
#include "PluginTrust.h"

#include "miniz.h"

#include <dlfcn.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifndef CASIOEMU_TEST_FIXTURE_PATH
#define CASIOEMU_TEST_FIXTURE_PATH ""
#endif

using namespace casioemu::plugin;

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& what) {
	if (!condition) {
		std::cerr << "FAIL: " << what << "\n";
		++g_failures;
	}
}

void WriteFile(const std::filesystem::path& path, const std::string& text) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream output(path, std::ios::binary);
	output << text;
}

std::string ManifestText(const std::string& id, const std::string& type,
	const std::string& permissions, const std::string& native = "",
	const std::string& api_version = "") {
	return std::string("{\n") +
		"  \"schema\": 1,\n" +
		"  \"id\": \"" + id + "\",\n" +
		"  \"name\": \"Loader smoke test\",\n" +
		"  \"version\": \"1.0.0\",\n" +
		"  \"type\": \"" + type + "\",\n" +
		"  \"permissions\": " + permissions + ",\n" +
		"  \"entry_point\": \"assets/entry.json\"" +
		(native.empty() ? "" : ",\n  \"native_entry\": \"" + native + "\"") +
		(api_version.empty() ? "" : ",\n  \"api_version\": " + api_version) +
		"\n}\n";
}

bool MakeZip(const std::filesystem::path& path,
	const std::vector<std::pair<std::string, std::string>>& entries, bool compress = true) {
	std::filesystem::create_directories(path.parent_path());
	std::filesystem::remove(path);
	mz_zip_archive archive{};
	if (!mz_zip_writer_init_file(&archive, path.string().c_str(), 0))
		return false;
	bool ok = true;
	for (const auto& entry : entries) {
		const mz_uint level = compress ? MZ_DEFAULT_COMPRESSION : 0;
		if (!mz_zip_writer_add_mem(&archive, entry.first.c_str(), entry.second.data(),
				entry.second.size(), level))
			ok = false;
	}
	if (!mz_zip_writer_finalize_archive(&archive))
		ok = false;
	if (!mz_zip_writer_end(&archive))
		ok = false;
	return ok;
}

std::string ExamplePayloads() {
	return R"({
  "schema": 1,
  "payloads": [
    {
      "id": "example.hello",
      "name": "Hello payload",
      "description": "Example payload.",
      "target": "any",
      "address": "0xE9E0",
      "bytes": "3C 3C 66 66 7E 7E 66 66",
      "safe": true
    }
  ]
})";
}

std::string ExampleLayout() {
	return R"({
  "schema": 1,
  "name": "Compact Layout",
  "shell": {
    "type": "default",
    "sidebar": { "visible": true, "width": 220 },
    "bottom": { "visible": true, "height": 140, "active": "output" },
    "tools": {
      "visible": false,
      "width": 320,
      "tabs": ["Variables", "Hardware", "Labels", "Watch", "Funcs", "Code"],
      "hidden": ["Injection", "Bitmap"]
    },
    "sessions": { "tabs": "top", "showClose": true }
  }
})";
}

} // namespace

int main() {
	// ---------------------------------------------------------- manifest
	ManifestResult result = ParseManifest(ManifestText("com.example.theme", "theme", "[\"ui.shell\"]"));
	Check(result.ok, "theme manifest parses");
	Check(result.manifest.type == PluginType::Theme, "theme type");
	Check(result.manifest.HasPermission("ui.shell"), "permission recorded");

	result = ParseManifest(ManifestText("com.example.tool", "tool", "[\"ui.shell\"]"));
	Check(!result.ok && result.code == "manifest.field", "native type requires native_entry");
	result = ParseManifest(ManifestText("com.example.tool", "tool", "[\"ui.shell\"]", "lib/example.dylib"));
	Check(result.ok && result.manifest.IsNative(), "native tool parses");

	result = ParseManifest(ManifestText("NotAnId", "theme", "[]"));
	Check(!result.ok && result.code == "manifest.id", "invalid id rejected");
	result = ParseManifest(ManifestText("com.example.bad", "theme", "[\"root\"]"));
	Check(!result.ok && result.code == "manifest.permission", "unknown permission rejected");

	result = ParseManifest(ManifestText("com.example.api", "theme", "[]", "", "3"));
	Check(result.ok && result.manifest.apiVersion == 3, "api_version 3 accepted");
	result = ParseManifest(ManifestText("com.example.api", "theme", "[]", "", "4"));
	Check(!result.ok && result.code == "manifest.field", "api_version 4 rejected");

	{
		std::string manifest = ManifestText("com.example.types", "theme", "[]");
		manifest.insert(manifest.find("\"type\""), "\"author\": 42,\n  ");
		result = ParseManifest(manifest);
		Check(!result.ok && result.code == "manifest.field", "wrong-typed optional string rejected");
	}

	// ------------------------------------------------------- permissions
	{
		const PermissionSet shell({ "ui.shell" });
		Check(shell.Allows(Capability::Shell), "ui.shell -> shell");
		Check(shell.Allows(Capability::Window), "ui.shell -> window");
		Check(shell.Allows(Capability::Theme), "ui.shell -> theme");
		Check(shell.Allows(Capability::Native), "ui.shell -> native");
		Check(!shell.Allows(Capability::Inject), "ui.shell does not grant inject");

		const PermissionSet inject({ "emulator.inject" });
		Check(inject.Allows(Capability::Inject), "emulator.inject -> inject");
		Check(!inject.Allows(Capability::Window), "emulator.inject does not grant windows");

		const PermissionSet process({ "process" });
		Check(process.Allows(Capability::Native), "process -> native");

		const PermissionSet read({ "emulator.read" });
		Check(read.Allows(Capability::EmulatorRead), "emulator.read -> read");
		Check(!read.Allows(Capability::EmulatorWrite), "emulator.read does not grant write");

		const std::string message = RecordPermissionDenial("com.example.none", Capability::Inject, "test");
		Check(message.find("permission denied") != std::string::npos, "denial message is clear");
		Check(message.find("emulator.inject") != std::string::npos, "denial names the permission");
		const auto denials = TakePermissionDenials("com.example.none");
		Check(denials.size() == 1 && denials[0].capability == "inject", "denial recorded and drained");
		Check(TakePermissionDenials("com.example.none").empty(), "denial drained only once");
	}

	// ------------------------------------------------------- path rules
	Check(IsSafeRelativePath("assets/theme.json"), "safe path");
	Check(IsSafeRelativePath("assets/"), "safe directory path");
	Check(!IsSafeRelativePath("../escape"), "parent traversal rejected");
	Check(!IsSafeRelativePath("/absolute"), "absolute path rejected");
	Check(!IsSafeRelativePath("assets/../../escape"), "nested traversal rejected");
	Check(!IsSafeRelativePath("assets\\file"), "backslash rejected");

	// Scratch lives under CASIOEMU_PLUGIN_TEST_ROOT (set by CTest to
	// /Users/kin/Desktop/Wiz/temp/plugin-tests).  Never use the system temp dir.
	const char* scratch_env = std::getenv("CASIOEMU_PLUGIN_TEST_ROOT");
	const std::filesystem::path scratch_root =
		scratch_env && *scratch_env ? scratch_env : std::filesystem::current_path() / "plugin-test-scratch";
	std::filesystem::create_directories(scratch_root);
	const std::filesystem::path root =
		scratch_root / (std::string("casioemu-plugin-test-") + std::to_string(::getpid()));
	std::filesystem::remove_all(root);

	// ---------------------------------------------------- valid extraction
	const std::vector<std::pair<std::string, std::string>> good_entries = {
		{ "plugin.json", ManifestText("com.example.fixture", "theme", "[\"ui.shell\"]") },
		{ "assets/entry.json",
			"{\"schema\":1,\"variant\":\"dark\",\"tokens\":{\"colors\":{\"background\":\"#1f1f1f\"}}}" },
	};
	Check(MakeZip(root / "fixture.plugin", good_entries), "fixture zip written");
	Package package;
	Check(ExtractPackage(root / "fixture.plugin", root / "cache", package),
		"valid package extracts");
	Check(package.valid && package.manifest.id == "com.example.fixture", "package manifest parsed");
	Check(package.sha256.size() == 64, "sha256 is 64 hex characters");
	Check(std::filesystem::exists(package.root / "assets" / "entry.json"), "asset extracted");

	// Re-extracting the same file must hit the cache without re-reading.
	Package cached;
	Check(ExtractPackage(root / "fixture.plugin", root / "cache", cached), "cache hit extracts");
	Check(cached.sha256 == package.sha256 && cached.root == package.root, "cache key is the hash");

	// ------------------------------------------------------ zip traversal
	Check(MakeZip(root / "traversal.plugin", {
		{ "../evil.txt", "owned" },
		{ "plugin.json", ManifestText("com.example.evil", "theme", "[]") },
	}), "traversal zip written");
	Package traversal;
	Check(!ExtractPackage(root / "traversal.plugin", root / "cache", traversal), "traversal rejected");
	Check(traversal.code == "package.path", "traversal reports package.path");
	Check(!std::filesystem::exists(root / "evil.txt"), "traversal wrote nothing outside the cache");

	// --------------------------------------------------- case collision
	Check(MakeZip(root / "case.plugin", {
		{ "plugin.json", ManifestText("com.example.case", "theme", "[]") },
		{ "assets/A.txt", "one" },
		{ "assets/a.txt", "two" },
	}), "case-collision zip written");
	Package collision;
	Check(!ExtractPackage(root / "case.plugin", root / "cache", collision), "case collision rejected");
	Check(collision.code == "package.path", "case collision reports package.path");

	// -------------------------------------------------------- symlink
	{
		const std::filesystem::path stage = root / "symlink-stage";
		std::filesystem::create_directories(stage);
		WriteFile(stage / "plugin.json", ManifestText("com.example.symlink", "theme", "[]"));
		std::error_code ec;
		std::filesystem::create_symlink("plugin.json", stage / "link.json", ec);
		Check(!ec, "symlink fixture created");
		const int zip_result = std::system(("cd \"" + stage.string() +
			"\" && zip -q -y -r ../symlink.plugin .").c_str());
		Check(zip_result == 0, "symlink zip written");
		Package symlink_package;
		Check(!ExtractPackage(root / "symlink.plugin", root / "cache", symlink_package),
			"symlink entry rejected");
		Check(symlink_package.code == "package.path", "symlink reports package.path");
	}

	// ------------------------------------------------------ size limits
	{
		ExtractionLimits limits;
		limits.maxEntryBytes = 8;
		Package too_big;
		Check(!ExtractPackage(root / "fixture.plugin", root / "limit-cache", too_big, limits),
			"per-entry limit enforced");
		Check(too_big.code == "package.invalid_zip", "size limit reports package.invalid_zip");
	}
	{
		const std::string zeros(2 * 1024 * 1024, '\0');
		Check(MakeZip(root / "ratio.plugin", {
			{ "plugin.json", ManifestText("com.example.ratio", "theme", "[]") },
			{ "assets/zeros.bin", zeros },
		}), "ratio zip written");
		ExtractionLimits limits;
		limits.ratioFloorBytes = 1024;
		limits.maxCompressionRatio = 10;
		Package ratio;
		Check(!ExtractPackage(root / "ratio.plugin", root / "limit-cache", ratio, limits),
			"compression ratio limit enforced");
		Check(ratio.code == "package.invalid_zip", "ratio limit reports package.invalid_zip");
	}

	// ----------------------------------------------------------- trust
	{
		TrustStore store;
		TrustRecord record;
		record.id = "com.example.trust";
		record.version = "1.0.0";
		record.sha256 = std::string(64, 'a');
		record.permissions = { "ui.shell" };
		store.Upsert(record);
		const TrustRecord* saved = store.Find("com.example.trust");
		Check(saved != nullptr && !saved->trustedAt.empty(), "trusted_at written");
		Check(saved && saved->history.size() == 1, "per-hash history written");

		const std::string new_hash(64, 'b');
		TrustEvaluation evaluation = EvaluateTrust(saved, true, new_hash, { "ui.shell" });
		Check(evaluation.decision == TrustDecision::Prompt && evaluation.hashChanged,
			"native hash change re-prompts");
		evaluation = EvaluateTrust(saved, false, new_hash, { "ui.shell" });
		Check(evaluation.decision == TrustDecision::Trusted, "declarative hash change is accepted");
		evaluation = EvaluateTrust(saved, false, std::string(64, 'a'), { "network", "ui.shell" });
		Check(evaluation.decision == TrustDecision::Prompt && evaluation.permissionsChanged,
			"permission change re-prompts");
		evaluation = EvaluateTrust(saved, false, std::string(64, 'a'), { "ui.shell", "ui.shell" });
		Check(evaluation.decision == TrustDecision::Trusted, "permission order/duplicates do not re-prompt");

		TrustRecord second;
		second.id = "com.example.trust";
		second.version = "1.1.0";
		second.sha256 = new_hash;
		second.permissions = { "ui.shell" };
		second.history = saved->history;
		store.Upsert(second);
		Check(store.Find("com.example.trust")->history.size() == 2, "history keeps both hashes");

		const std::filesystem::path trust_path = root / "trust" / "plugin-trust.json";
		Check(store.Save(trust_path), "trust store saves");
		TrustStore reloaded;
		Check(reloaded.Load(trust_path), "trust store loads");
		const TrustRecord* round_tripped = reloaded.Find("com.example.trust");
		Check(round_tripped && round_tripped->history.size() == 2, "history survives round-trip");
		Check(round_tripped && !round_tripped->trustedAt.empty(), "trusted_at survives round-trip");

		TrustRecord blocked = *round_tripped;
		blocked.blocked = true;
		Check(EvaluateTrust(&blocked, true, new_hash, { "ui.shell" }).decision == TrustDecision::Blocked,
			"blocked record wins over hash");
		blocked.blocked = false;
		blocked.enabled = false;
		Check(EvaluateTrust(&blocked, true, new_hash, { "ui.shell" }).decision == TrustDecision::Disabled,
			"disabled record");
	}

	// --------------------------------------------------------- payloads
	{
		std::vector<InjectionPayload> payloads;
		std::string error;
		Check(ParseInjectDocument(ExamplePayloads(), "com.example.inject", payloads, error),
			"inject document parses");
		Check(payloads.size() == 1 && payloads[0].address == 0xE9E0, "payload address parsed");
		Check(payloads[0].bytes.size() == 8 && payloads[0].safe, "payload bytes parsed");

		RegisterPluginPayloads("com.example.inject", payloads);
		InjectionPayload found;
		Check(FindPluginPayload("example.hello", found) && found.address == 0xE9E0,
			"payload registered in the host view");
		ClearPluginPayloads("com.example.inject");
		Check(PluginPayloads().empty(), "payloads cleared");

		std::vector<InjectionPayload> invalid;
		Check(!ParseInjectDocument(R"({"schema":1,"payloads":[{"id":"x","name":"x","address":"nope","bytes":"00"}]})",
			"com.example.inject", invalid, error), "bad address rejected");
		Check(!ParseInjectDocument(R"({"schema":1,"payloads":[{"id":"x","name":"x","address":"0","bytes":"000"}]})",
			"com.example.inject", invalid, error), "odd byte count rejected");
		Check(!ParseInjectDocument(R"({"schema":1,"payloads":[]})", "com.example.inject", invalid, error),
			"empty payload list rejected");
	}

	// ----------------------------------------------------------- layout
	{
		LayoutActions actions;
		std::string error;
		Check(ParseLayoutDocument(ExampleLayout(), actions, error), "example layout parses");
		Check(actions.sidebarWidth == 220 && actions.hasSidebarWidth, "sidebar width parsed");
		Check(!actions.toolsVisible && actions.hiddenTools.size() == 2, "tools hidden parsed");
		Check(actions.toolOrder.size() == 6, "tool order parsed");
		Check(actions.sessionTabs == "top", "session tab placement parsed");

		Check(!ParseLayoutDocument(R"({"schema":1,"shell":{"sessions":{"tabs":"bottom"}}})",
			actions, error), "unsupported tab placement rejected");
		Check(error.find("unsupported action") != std::string::npos, "unsupported error is explicit");

		Check(!ParseLayoutDocument(R"({"schema":1,"shell":{"unknownAction":true}})",
			actions, error), "unknown layout action rejected");
		Check(!ParseLayoutDocument(R"({"schema":1,"shell":{"sidebar":{"width":10}}})",
			actions, error), "out-of-range sidebar width rejected");

		Check(ParseLayoutDocument(R"({"schema":1,"theme":"vscode-dark"})", actions, error),
			"theme selection parses");
		Check(actions.hasTheme && actions.theme == "vscode-dark", "theme id recorded");
	}

	// ---------------------------------------------------------- unload
	{
		const std::filesystem::path fixture = CASIOEMU_TEST_FIXTURE_PATH;
		if (fixture.empty()) {
			std::cerr << "WARN: fixture path not configured; unload test skipped\n";
			++g_failures;
		}
		else {
			void* handle = dlopen(fixture.string().c_str(), RTLD_NOW | RTLD_LOCAL);
			Check(handle != nullptr, std::string("fixture dylib opens: ") + (handle ? "" : dlerror()));
			if (handle) {
				auto count = reinterpret_cast<int (*)()>(dlsym(handle, "fixtureUnloadCount"));
				Check(count != nullptr && count() == 0, "fixture unload count starts at zero");
				const UnloadCallResult unload = CallPluginUnload(handle, nullptr);
				Check(unload.had_callback && !unload.threw, "fPluginUnload is discovered and called");
				Check(count && count() == 1, "fPluginUnload ran once");
				dlclose(handle);
			}
		}
	}

	// -------------------------------------------------- window tracking
	{
		int fake_window = 0;
		TrackPluginWindow("com.example.native", &fake_window);
		Check(IsTrackedPluginWindow(&fake_window), "plugin window tracked");
		MarkPluginHookRegistration("com.example.native");
		Check(PluginHasHookRegistrations("com.example.native"), "hook registration recorded");
		const auto taken = TakePluginWindows("com.example.native");
		Check(taken.size() == 1 && taken[0] == &fake_window, "plugin windows can be removed");
		Check(!IsTrackedPluginWindow(&fake_window), "window no longer tracked");
		ClearPluginResources("com.example.native");
		Check(!PluginHasHookRegistrations("com.example.native"), "plugin resources cleared");
	}

	std::filesystem::remove_all(root);
	if (g_failures != 0) {
		std::cerr << g_failures << " plugin loader smoke check(s) failed\n";
		return 1;
	}
	std::cout << "plugin loader smoke test passed\n";
	return 0;
}
