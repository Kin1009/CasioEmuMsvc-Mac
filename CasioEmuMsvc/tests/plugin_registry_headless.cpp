// Headless end-to-end harness for the plugin registry.
//
// No SDL, no shell, no screenshots: a fake PluginHost supplies executable
// directory, theme application, window list, settings persistence and the
// per-plugin PluginApi factory.  The real PluginRegistry, trust store, package
// extractor, permission gates, payload/layout parsers and unload path are
// exercised exactly as in the app.
#include "PluginApi.h"
#include "PluginHost.h"
#include "PluginHostApi.h"
#include "PluginLayout.h"
#include "PluginMan.h"
#include "PluginManifest.h"
#include "PluginPackage.h"
#include "PluginPayloads.h"
#include "PluginPermissions.h"
#include "PluginRegistry.h"
#include "PluginResources.h"
#include "PluginTrust.h"
#include "ThemeTokens.h"

#include "miniz.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#ifndef CASIOEMU_PLUGIN_WINDOW_FIXTURE
#define CASIOEMU_PLUGIN_WINDOW_FIXTURE ""
#endif
#ifndef CASIOEMU_PLUGIN_BAD_API_FIXTURE
#define CASIOEMU_PLUGIN_BAD_API_FIXTURE ""
#endif
#ifndef CASIOEMU_PLUGIN_BAD_ABI_FIXTURE
#define CASIOEMU_PLUGIN_BAD_ABI_FIXTURE ""
#endif

// PluginMan.cpp provides these in the app; the harness provides them so the
// registry can log without pulling the platform loaders in.
std::string g_PluginLoadLog;
std::vector<PluginInfo> g_loadedPlugins;

using namespace casioemu::plugin;

namespace casioemu::plugin {
unsigned long HostUiWindowAbiSignature() {
	return PluginUiWindowAbiSignature();
}
} // namespace casioemu::plugin

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& what) {
	if (!condition) {
		std::cerr << "FAIL: " << what << "\n";
		++g_failures;
	}
	else {
		std::cout << "ok: " << what << "\n";
	}
}

struct FakeWindow {
	std::string name;
	bool open = true;
};

class FakeHost : public PluginHost {
public:
	explicit FakeHost(std::filesystem::path exe) : exe_(std::move(exe)) {}

	std::string ExecutableDir() override { return exe_.string(); }
	bool PickPluginFile(std::string& path) override {
		path = next_pick;
		return !path.empty();
	}

	bool ApplyPluginTheme(const std::string& document, std::string& error) override {
		if (!ApplyThemeDocument(document, error))
			return false;
		theme_active = true;
		++theme_applications;
		last_theme_document = document;
		return true;
	}
	void ResetTheme() override { theme_active = false; }
	bool SelectTheme(const std::string& themeId, std::string& error) override {
		if (themeId != "vscode-dark" && themeId != "vscode-light") {
			error = "unknown preset '" + themeId + "'";
			return false;
		}
		selected_theme = themeId;
		return true;
	}
	bool FontRebuildPending() override { return font_rebuild_pending; }

	std::vector<void*> Windows() override { return windows; }
	const char* WindowName(const void* window) override {
		const auto it = window_names.find(const_cast<void*>(window));
		return it != window_names.end() ? it->second.c_str() : "";
	}
	void SetWindowOpen(void* window, bool open) override {
		const auto it = window_open.find(window);
		if (it != window_open.end()) {
			*it->second = open;
			return;
		}
		if (auto* typed = static_cast<UIWindow*>(window))
			typed->open = open;
	}
	void RemoveWindow(void* window) override {
		windows.erase(std::remove(windows.begin(), windows.end(), window), windows.end());
		window_names.erase(window);
		window_open.erase(window);
	}
	void ReorderWindows(const std::vector<std::string>& names) override {
		std::vector<void*> ordered;
		for (const auto& name : names) {
			for (void* window : windows) {
				if (WindowName(window) == name &&
					std::find(ordered.begin(), ordered.end(), window) == ordered.end())
					ordered.push_back(window);
			}
		}
		if (ordered.empty())
			return;
		std::vector<void*> rest;
		for (void* window : windows)
			if (std::find(ordered.begin(), ordered.end(), window) == ordered.end())
				rest.push_back(window);
		windows = ordered;
		windows.insert(windows.end(), rest.begin(), rest.end());
	}

	bool ApplyShellLayout(const LayoutActions& actions, std::string& error) override {
		(void)error;
		last_layout = actions;
		layout_applied = true;
		if (actions.hasSidebar)
			shell_sidebar_width = actions.sidebarWidth;
		if (actions.hasBottom)
			shell_bottom_height = actions.bottomHeight;
		if (actions.hasTools)
			shell_tools_visible = actions.toolsVisible;
		return true;
	}

	void AddTestWindow(void* window, const std::string& name, bool* open) {
		windows.push_back(window);
		window_names[window] = name;
		window_open[window] = open;
	}
	void AddPluginWindow(UIWindow* window) {
		windows.push_back(window);
		window_names[window] = window->name ? window->name : "";
	}

	PluginApi* CreateApi(const std::string& pluginId, const Manifest& manifest) override;
	void DestroyApi(PluginApi* api) override;

	std::filesystem::path exe_;
	std::vector<void*> windows;
	std::map<void*, std::string> window_names;
	std::map<void*, bool*> window_open;
	std::vector<std::string> registered;
	std::string next_pick;
	bool theme_active = false;
	bool font_rebuild_pending = false;
	int theme_applications = 0;
	std::string last_theme_document;
	std::string selected_theme;
	bool layout_applied = false;
	LayoutActions last_layout;
	int shell_sidebar_width = 240;
	int shell_bottom_height = 160;
	bool shell_tools_visible = true;
};

class HeadlessApi : public PluginApi {
public:
	HeadlessApi(std::string id, PermissionSet permissions, FakeHost* host,
		std::vector<std::string>* registered)
		: id_(std::move(id)), permissions_(std::move(permissions)), host_(host),
		  registered_(registered) {}

	int GetVersion() override { return PLUGIN_API_VERSION; }

	void AddWindow(UIWindow* window) override {
		if (!window)
			return;
		if (!permissions_.Allows(Capability::Window)) {
			RecordPermissionDenial(id_, Capability::Window, "PluginApi::AddWindow");
			return;
		}
		TrackPluginWindow(id_, window);
		host_->AddPluginWindow(window);
	}

	bool RegisterPlugin(const char* id, const char* name, int version) override {
		return RegisterPlugin(id, name, std::to_string(version).c_str(), "", "legacy");
	}
	void AssertFundamentalSTL(size_t, size_t, size_t, size_t) override {}
	void* QueryInterface(const char* name) override {
		if (!permissions_.Allows(Capability::EmulatorRead))
			RecordPermissionDenial(id_, Capability::EmulatorRead,
				name ? std::string("QueryInterface<") + name + ">" : "QueryInterface");
		return nullptr;
	}
	void* GetImGuiContext() override {
		if (!permissions_.Allows(Capability::Window)) {
			RecordPermissionDenial(id_, Capability::Window, "PluginApi::GetImGuiContext");
			return nullptr;
		}
		return ImGui::GetCurrentContext();
	}
	bool RegisterPlugin(const char* id, const char* name, const char* version, const char* author,
		const char* desc) override {
		(void)author;
		(void)desc;
		registered_->push_back(id ? id : "");
		std::cout << "  [plugin] " << (name ? name : "") << " " << (version ? version : "") << "\n";
		return true;
	}

private:
	std::string id_;
	PermissionSet permissions_;
	FakeHost* host_;
	std::vector<std::string>* registered_;
};

PluginApi* FakeHost::CreateApi(const std::string& pluginId, const Manifest& manifest) {
	return new HeadlessApi(pluginId, PermissionSet(manifest.permissions), this, &registered);
}

void FakeHost::DestroyApi(PluginApi* api) {
	delete static_cast<HeadlessApi*>(api);
}

void WriteFile(const std::filesystem::path& path, const std::string& text) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream output(path, std::ios::binary);
	output << text;
}

bool MakeZip(const std::filesystem::path& path,
	const std::vector<std::pair<std::string, std::string>>& entries) {
	std::filesystem::create_directories(path.parent_path());
	mz_zip_archive archive{};
	if (!mz_zip_writer_init_file(&archive, path.string().c_str(), 0))
		return false;
	for (const auto& entry : entries)
		mz_zip_writer_add_mem(&archive, entry.first.c_str(), entry.second.data(), entry.second.size(),
			MZ_DEFAULT_COMPRESSION);
	const bool ok = mz_zip_writer_finalize_archive(&archive) != 0;
	mz_zip_writer_end(&archive);
	return ok;
}

std::string ManifestJson(const std::string& id, const std::string& type, const std::string& permissions,
	const std::string& entry = "assets/entry.json", const std::string& native = "") {
	return std::string("{\n") +
		"  \"schema\": 1,\n" +
		"  \"id\": \"" + id + "\",\n" +
		"  \"name\": \"" + id + "\",\n" +
		"  \"version\": \"1.0.0\",\n" +
		"  \"type\": \"" + type + "\",\n" +
		"  \"api_version\": 3,\n" +
		"  \"permissions\": " + permissions + ",\n" +
		"  \"entry_point\": \"" + entry + "\"" +
		(native.empty() ? "" : ",\n  \"native_entry\": \"" + native + "\"") +
		"\n}\n";
}

std::string ThemeDocument(const std::string& color = "#102030") {
	return "{\"schema\":1,\"variant\":\"dark\",\"tokens\":{\"colors\":{\"background\":\"" + color +
		"\"}}}";
}

std::string PayloadDocument(const std::string& address = "0xE9E0") {
	return std::string("{\"schema\":1,\"payloads\":[{\"id\":\"example.hello\",\"name\":\"Hello\",") +
		"\"address\":\"" + address + "\",\"bytes\":\"3C 3C 66 66 7E 7E 66 66\",\"safe\":true}]}";
}

std::string LayoutDocument() {
	return R"({"schema":1,"shell":{"type":"default",
  "sidebar":{"visible":true,"width":220},
  "bottom":{"visible":true,"height":140,"active":"output"},
  "tools":{"visible":false,"width":320,"tabs":["Variables","Hardware","Labels"],"hidden":["Injection"]},
  "sessions":{"tabs":"top","showClose":true}}})";
}

std::string LayoutWithThemeDocument() {
	return R"({"schema":1,"shell":{"theme":"vscode-dark"}})";
}

PluginEntry* Find(PluginRegistry& registry, const std::string& id) {
	for (auto& entry : registry.Entries())
		if (!entry.removed && entry.Id() == id)
			return &entry;
	return nullptr;
}

struct PackageSpec {
	std::string file_name;
	std::string id;
	std::string type;
	std::string permissions;
	std::string entry_document;
	std::string layout_document;
	std::string native_entry;
};

std::filesystem::path BuildPackage(const std::filesystem::path& plugins_dir,
	const PackageSpec& spec, const std::filesystem::path& fixture) {
	const std::filesystem::path path = plugins_dir / spec.file_name;
	std::string document = spec.entry_document;
	if (!spec.layout_document.empty())
		document = spec.layout_document;
	std::vector<std::pair<std::string, std::string>> entries = {
		{ "plugin.json", ManifestJson(spec.id, spec.type, spec.permissions, "assets/entry.json",
			spec.native_entry) },
		{ "assets/entry.json", document },
	};
	if (!spec.native_entry.empty()) {
		std::ifstream input(fixture, std::ios::binary);
		std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		entries.push_back({ spec.native_entry, bytes });
	}
	MakeZip(path, entries);
	return path;
}

} // namespace

int main() {
	// Scratch lives under CASIOEMU_PLUGIN_TEST_ROOT (set by CTest to
	// /Users/kin/Desktop/Wiz/temp/plugin-tests).  Never use the system temp dir.
	const char* scratch_env = std::getenv("CASIOEMU_PLUGIN_TEST_ROOT");
	const std::filesystem::path scratch_root =
		scratch_env && *scratch_env ? scratch_env : std::filesystem::current_path() / "plugin-test-scratch";
	std::filesystem::create_directories(scratch_root);
	const std::filesystem::path root =
		scratch_root / (std::string("casioemu-registry-headless-") + std::to_string(::getpid()));
	std::filesystem::remove_all(root);
	const std::filesystem::path exe = root / "exe";
	const std::filesystem::path plugins = exe / "settings" / "Plugins";
	std::filesystem::create_directories(plugins);
	WriteFile(exe / "portable.flag", "");

	ImGui::CreateContext();

	FakeHost host(exe);
	SetPluginHost(&host);

	// Host windows that a layout can hide/reorder.
	FakeWindow injection_window;
	injection_window.name = "Injection";
	FakeWindow variables_window;
	variables_window.name = "Variables";
	FakeWindow hardware_window;
	hardware_window.name = "Hardware";
	host.AddTestWindow(&injection_window, "Injection", &injection_window.open);
	host.AddTestWindow(&variables_window, "Variables", &variables_window.open);
	host.AddTestWindow(&hardware_window, "Hardware", &hardware_window.open);

	const std::filesystem::path fixture = CASIOEMU_PLUGIN_WINDOW_FIXTURE;
	const std::filesystem::path bad_api_fixture = CASIOEMU_PLUGIN_BAD_API_FIXTURE;
	const std::filesystem::path bad_abi_fixture = CASIOEMU_PLUGIN_BAD_ABI_FIXTURE;
	Check(!fixture.empty() && std::filesystem::exists(fixture), "window fixture dylib exists");
	Check(!bad_api_fixture.empty() && std::filesystem::exists(bad_api_fixture), "bad-api fixture exists");
	Check(!bad_abi_fixture.empty() && std::filesystem::exists(bad_abi_fixture), "bad-abi fixture exists");

	const std::vector<PackageSpec> specs = {
		{ "theme-ok.plugin", "com.test.theme", "theme", R"(["ui.shell"])", ThemeDocument(), {}, {} },
		{ "theme-noperm.plugin", "com.test.theme.noperm", "theme", "[]", ThemeDocument("#203040"), {}, {} },
		{ "inject-ok.plugin", "com.test.inject", "inject", R"(["emulator.inject"])", PayloadDocument(), {}, {} },
		{ "inject-bad.plugin", "com.test.inject.bad", "inject", R"(["emulator.inject"])", PayloadDocument("nope"), {}, {} },
		{ "inject-noperm.plugin", "com.test.inject.noperm", "inject", "[]", PayloadDocument(), {}, {} },
		{ "layout-ok.plugin", "com.test.layout", "ui_revamp", R"(["ui.shell"])", "", LayoutDocument(), {} },
		{ "layout-theme.plugin", "com.test.layout.theme", "ui_revamp", R"(["ui.shell"])", "", LayoutWithThemeDocument(), {} },
		{ "layout-unsupported.plugin", "com.test.layout.bad", "ui_revamp", R"(["ui.shell"])", "",
			R"({"schema":1,"shell":{"sessions":{"tabs":"bottom"}}})", {} },
		{ "tool-ok.plugin", "com.test.tool", "tool", R"(["ui.shell"])", "{}", {}, "lib/fixture.dylib" },
		{ "tool-limited.plugin", "com.test.tool.limited", "tool", R"(["process"])", "{}", {}, "lib/fixture.dylib" },
		{ "tool-hash.plugin", "com.test.tool.hash", "tool", R"(["ui.shell"])", "{}", {}, "lib/fixture.dylib" },
		{ "tool-badapi.plugin", "com.test.tool.badapi", "tool", R"(["ui.shell"])", "{}", {}, "lib/fixture.dylib" },
		{ "tool-badabi.plugin", "com.test.tool.badabi", "tool", R"(["ui.shell"])", "{}", {}, "lib/fixture.dylib" },
		{ "theme-hash.plugin", "com.test.theme.hash", "theme", R"(["ui.shell"])", ThemeDocument(), {}, {} },
		{ "theme-perm.plugin", "com.test.theme.perm", "theme", R"(["ui.shell"])", ThemeDocument(), {}, {} },
	};

	// Build packages and the trust store before Initialize.
	TrustStore trust;
	for (const auto& spec : specs) {
		const std::filesystem::path spec_fixture =
			spec.id == "com.test.tool.badapi" ? bad_api_fixture :
			spec.id == "com.test.tool.badabi" ? bad_abi_fixture : fixture;
		const std::filesystem::path path = BuildPackage(plugins, spec, spec_fixture);
		std::string sha;
		std::string hash_error;
		Check(ComputeFileSha256(path, sha, hash_error), "hash " + spec.id);

		std::string trust_sha = sha;
		if (spec.id == "com.test.tool.hash")
			trust_sha = std::string(64, 'a');
		if (spec.id == "com.test.theme.hash")
			trust_sha = std::string(64, 'b');

		TrustRecord record;
		record.id = spec.id;
		record.version = "1.0.0";
		record.sha256 = trust_sha;
		if (spec.permissions == R"(["ui.shell"])")
			record.permissions = { "ui.shell" };
		else if (spec.permissions == R"(["emulator.inject"])")
			record.permissions = { "emulator.inject" };
		else if (spec.permissions == R"(["process"])")
			record.permissions = { "process" };
		else
			record.permissions = {};
		if (spec.id == "com.test.theme.perm")
			record.permissions = { "network" };
		trust.Upsert(record);
	}
	Check(trust.Save(exe / "settings" / "plugin-trust.json"), "seed trust store");

	auto& registry = PluginRegistry::Instance();
	registry.Initialize();

	// ------------------------------------------------------------ theme
	PluginEntry* theme = Find(registry, "com.test.theme");
	Check(theme && theme->state == "Loaded", "trusted theme loads");
	Check(host.theme_applications >= 1, "theme tokens applied through the host");
	const ImVec4 window_bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
	Check(std::abs(window_bg.x - (0x10 / 255.0f)) < 0.01f &&
		std::abs(window_bg.y - (0x20 / 255.0f)) < 0.01f &&
		std::abs(window_bg.z - (0x30 / 255.0f)) < 0.01f, "plugin theme background reached ImGui style");

	// Font rebuild re-applies the plugin theme.
	host.font_rebuild_pending = true;
	const int applications_before = host.theme_applications;
	registry.PumpFrame();
	registry.PumpFrame(); // still pending: must not re-apply yet
	Check(host.theme_applications == applications_before, "no reapply while font rebuild is pending");
	host.font_rebuild_pending = false;
	registry.PumpFrame();
	Check(host.theme_applications > applications_before, "theme re-applied after font rebuild");

	PluginEntry* noperm = Find(registry, "com.test.theme.noperm");
	Check(noperm && noperm->state == "Error", "theme without ui.shell fails to load");
	Check(noperm && noperm->error.rfind("plugin.permission", 0) == 0, "permission denial code present");

	// ---------------------------------------------------------- inject
	PluginEntry* inject = Find(registry, "com.test.inject");
	Check(inject && inject->state == "Loaded", "trusted inject loads");
	InjectionPayload payload;
	Check(FindPluginPayload("example.hello", payload) && payload.address == 0xE9E0,
		"inject payload registered in the host view");
	PluginEntry* inject_bad = Find(registry, "com.test.inject.bad");
	Check(inject_bad && inject_bad->state == "Error" &&
		inject_bad->error.rfind("inject.invalid", 0) == 0, "invalid payload is a load error");
	PluginEntry* inject_noperm = Find(registry, "com.test.inject.noperm");
	Check(inject_noperm && inject_noperm->state == "Error" &&
		inject_noperm->error.rfind("plugin.permission", 0) == 0,
		"inject without emulator.inject is denied");

	// ---------------------------------------------------------- layout
	PluginEntry* layout = Find(registry, "com.test.layout");
	Check(layout && layout->state == "Loaded", "trusted ui_revamp layout loads");
	Check(host.layout_applied && host.shell_sidebar_width == 220, "sidebar width applied");
	Check(host.shell_bottom_height == 140, "bottom height applied");
	Check(!host.shell_tools_visible, "tools visibility applied");
	Check(!injection_window.open, "hidden tool window closed");
	Check(host.windows.size() >= 3 && std::string(host.WindowName(host.windows[0])) == "Variables" &&
		std::string(host.WindowName(host.windows[1])) == "Hardware", "tool order applied");
	PluginEntry* layout_theme = Find(registry, "com.test.layout.theme");
	Check(layout_theme && layout_theme->state == "Loaded" && host.selected_theme == "vscode-dark",
		"theme selection action applied");
	PluginEntry* layout_bad = Find(registry, "com.test.layout.bad");
	Check(layout_bad && layout_bad->state == "Error" &&
		layout_bad->error.rfind("layout.unsupported", 0) == 0,
		"unsupported layout action fails instead of reporting Loaded");

	// ------------------------------------------------- trust decisions
	Check(Find(registry, "com.test.tool.hash")->state == "Awaiting approval",
		"native hash change re-prompts");
	{
		PluginEntry* bad_api = Find(registry, "com.test.tool.badapi");
		Check(bad_api && bad_api->state == "Error" &&
			bad_api->error.rfind("native.api", 0) == 0, "newer plugin API is rejected");
		PluginEntry* bad_abi = Find(registry, "com.test.tool.badabi");
		Check(bad_abi && bad_abi->state == "Error" &&
			bad_abi->error.rfind("native.api", 0) == 0, "UIWindow ABI mismatch is rejected");
	}
	PluginEntry* declarative_hash = Find(registry, "com.test.theme.hash");
	Check(declarative_hash && declarative_hash->state == "Loaded",
		"declarative hash change with same permissions auto-loads");
	{
		TrustStore reloaded;
		Check(reloaded.Load(exe / "settings" / "plugin-trust.json"), "reload trust store");
		const TrustRecord* record = reloaded.Find("com.test.theme.hash");
		Check(record && record->history.size() >= 2, "declarative upgrade recorded in hash history");
	}
	Check(Find(registry, "com.test.theme.perm")->state == "Awaiting approval",
		"permission change re-prompts");

	// ------------------------------------------------------- native load
	PluginEntry* tool = Find(registry, "com.test.tool");
	Check(tool && tool->state == "Loaded" && tool->handle != nullptr, "native tool loads");
	Check(host.registered.size() >= 1, "fPluginLoad registered the plugin with the host API");
	size_t fixture_windows = 0;
	for (void* window : host.windows)
		if (std::string(host.WindowName(window)) == "Fixture Window")
			++fixture_windows;
	Check(fixture_windows == 1, "native AddWindow reached the host window list");

	PluginEntry* limited = Find(registry, "com.test.tool.limited");
	Check(limited && limited->state == "Loaded (limited)", "API permission denial marks the plugin limited");
	Check(limited && std::find(limited->deniedCapabilities.begin(), limited->deniedCapabilities.end(),
		"ui.window") != limited->deniedCapabilities.end(), "denied capability recorded for the manager");
	size_t limited_windows = 0;
	for (void* window : host.windows)
		if (std::string(host.WindowName(window)) == "Fixture Window")
			++limited_windows;
	Check(limited_windows == 1, "denied AddWindow did not add a second window");

	// ---------------------------------------------------------- disable
	{
		const size_t windows_before = host.windows.size();
		registry.SetEnabled("com.test.tool", false);
		Check(tool->state == "Disabled", "Disable stops a loaded native plugin");
		Check(tool->handle == nullptr && !tool->libraryRetained, "Disable closed the library");
		Check(host.windows.size() == windows_before - 1, "Disable removed the plugin window");
		registry.SetEnabled("com.test.tool", true);
		Check(tool->state == "Loaded" && tool->handle != nullptr, "Enable reloads the stopped plugin");
	}

	// ----------------------------------------------------------- unload
	{
		const std::filesystem::path marker = root / "unload-marker.txt";
		::setenv("CASIOEMU_PLUGIN_FIXTURE_MARKER", marker.string().c_str(), 1);
		const size_t windows_before = host.windows.size();
		Check(registry.Unload("com.test.tool"), "Unload reports success");
		Check(tool->state == "Unloaded", "state is Unloaded");
		Check(tool->handle == nullptr && !tool->libraryRetained, "library closed when safe");
		Check(host.windows.size() == windows_before - 1, "plugin window removed from the host list");
		Check(std::filesystem::exists(marker), "fPluginUnload ran");
		std::ifstream marker_input(marker);
		int unload_count = 0;
		marker_input >> unload_count;
		Check(unload_count == 1, "fPluginUnload called exactly once");

		Check(registry.Unload("com.test.tool"), "second unload succeeds");
	}

	// ---------------------------------------------------------- install
	{
		const std::filesystem::path staged = root / "incoming" / "installed.plugin";
		MakeZip(staged, { { "plugin.json", ManifestJson("com.test.installed", "theme", R"(["ui.shell"])") },
			{ "assets/entry.json", ThemeDocument() } });
		std::string error;
		Check(registry.InstallFromFile(staged, &error), "InstallFromFile copies and loads a package");
		Check(std::filesystem::exists(exe / "settings" / "Plugins" / "installed.plugin"),
			"installed package lives in the user plugin folder");
		PluginEntry* installed = Find(registry, "com.test.installed");
		Check(installed && installed->state == "Awaiting approval",
			"new install prompts for trust");
		Check(!registry.InstallFromFile(staged, &error) && error.find("already exists") != std::string::npos,
			"duplicate install is refused with a clear error");
	}

	// -------------------------------------------------------- shutdown
	registry.Shutdown();
	Check(registry.Entries().empty(), "Shutdown clears runtime entries");
	SetPluginHost(nullptr);
	ImGui::DestroyContext();
	std::filesystem::remove_all(root);

	if (g_failures != 0) {
		std::cerr << g_failures << " headless registry check(s) failed\n";
		return 1;
	}
	std::cout << "plugin registry headless test passed\n";
	return 0;
}
