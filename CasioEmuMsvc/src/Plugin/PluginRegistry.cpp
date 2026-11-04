#include "PluginRegistry.h"

// The registry is deliberately GUI-free: everything the shell owns (theme,
// windows, settings, file dialog, PluginApi factory) is reached through
// PluginHost, so this file also links into the headless test harness.
#include "PluginHost.h"
#include "PluginHostApi.h"
#include "PluginLayout.h"
#include "PluginMan.h"
#include "PluginNative.h"
#include "PluginPayloads.h"
#include "PluginPermissions.h"
#include "PluginResources.h"

#include "../../../McpPlugin/json.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <sstream>
#include <system_error>

namespace casioemu::plugin {
namespace {

using json = nlohmann::json;

constexpr const char* kTrustFileName = "plugin-trust.json";

using PluginLoadFn = void (*)(PluginApi*);

std::filesystem::path ExecutableDir() {
	return Host().ExecutableDir();
}

bool IsPortableBuild() {
	std::error_code ec;
	return std::filesystem::exists(ExecutableDir() / "portable.flag", ec);
}

bool ReadTextFile(const std::filesystem::path& path, std::string& out, std::string& error) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		error = "cannot open " + path.string();
		return false;
	}
	std::ostringstream buffer;
	buffer << file.rdbuf();
	out = buffer.str();
	return true;
}

std::string JoinPermissions(const std::vector<std::string>& permissions) {
	std::string result;
	for (const auto& permission : permissions) {
		if (!result.empty())
			result += ", ";
		result += permission;
	}
	return result;
}

bool HasPluginExtension(const std::filesystem::path& path) {
	std::string extension = path.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return extension == ".plugin";
}

std::string TrustReason(const TrustEvaluation& evaluation) {
	if (evaluation.permissionsChanged)
		return "permissions changed";
	if (evaluation.hashChanged)
		return "package hash changed";
	return "not trusted yet";
}

} // namespace

PluginRegistry& PluginRegistry::Instance() {
	static PluginRegistry registry;
	return registry;
}

std::filesystem::path PluginRegistry::SettingsDir() const {
	if (IsPortableBuild())
		return ExecutableDir() / "settings";
	const char* home = std::getenv("HOME");
	if (!home || !*home)
		return ExecutableDir() / "settings";
	return std::filesystem::path(home) / "Library" / "Application Support" / "CasioEmuMsvc";
}

std::filesystem::path PluginRegistry::UserPluginsDir() const {
	return SettingsDir() / "Plugins";
}

std::filesystem::path PluginRegistry::AppPluginsDir() const {
	return ExecutableDir() / "plugins";
}

std::filesystem::path PluginRegistry::CacheDir() const {
	return SettingsDir() / "plugin-cache";
}

int PluginRegistry::HostApiVersion() const {
	return kHostApiVersion;
}

std::string PluginRegistry::Log() const {
	return g_PluginLoadLog;
}

void PluginRegistry::LogLine(const std::string& line) {
	g_PluginLoadLog += line;
	if (g_PluginLoadLog.empty() || g_PluginLoadLog.back() != '\n')
		g_PluginLoadLog += '\n';
}

void PluginRegistry::Initialize() {
	if (m_initialized)
		return;
	m_initialized = true;
	static bool exit_hook_registered = false;
	if (!exit_hook_registered) {
		exit_hook_registered = true;
		std::atexit(&PluginRegistry::AtExit);
	}
	LoadTrust();
	m_entries.clear();

	std::error_code ec;
	std::filesystem::create_directories(UserPluginsDir(), ec);
	std::filesystem::create_directories(CacheDir(), ec);

	LogLine("[INFO] Plugin host API v" + std::to_string(kHostApiVersion) + " (" + kHostVersion + ")");
	LogLine("[INFO] User plugins: " + UserPluginsDir().string());
	ScanDirectory(UserPluginsDir());
	ScanDirectory(AppPluginsDir());
	ScanDirectory(ExecutableDir() / ".." / "Resources" / "plugins");
	SaveTrust();
}

void PluginRegistry::Shutdown() {
	ShutdownInternal(true);
}

void PluginRegistry::ShutdownInternal(bool remove_windows) {
	for (auto& entry : m_entries) {
		if (entry.removed || !entry.loaded)
			continue;
		UnloadEntry(entry, remove_windows);
	}
	m_entries.clear();
	m_trust.Records().clear();
	m_activeThemeId.clear();
	m_activeThemeDocument.clear();
	m_initialized = false;
}

void PluginRegistry::AtExit() {
	if (Instance().m_initialized)
		Instance().ShutdownInternal(false);
}

void PluginRegistry::LoadTrust() {
	m_trust.Records().clear();
	std::string error;
	if (!m_trust.Load(SettingsDir() / kTrustFileName, &error) && !error.empty())
		LogLine("[WARN] Cannot read plugin trust store: " + error);
}

void PluginRegistry::SaveTrust() {
	std::string error;
	if (!m_trust.Save(SettingsDir() / kTrustFileName, &error))
		LogLine("[ERROR] Cannot write plugin trust store: " + error);
}

void PluginRegistry::ScanDirectory(const std::filesystem::path& directory) {
	std::error_code ec;
	if (!std::filesystem::is_directory(directory, ec)) {
		LogLine("[INFO] No plugin folder at " + directory.string());
		return;
	}
	for (const auto& item : std::filesystem::directory_iterator(directory, ec)) {
		if (ec)
			break;
		if (!item.is_regular_file(ec))
			continue;
		if (!HasPluginExtension(item.path()))
			continue;
		AddPackage(item.path());
	}
}

void PluginRegistry::AddPackage(const std::filesystem::path& file) {
	LogLine("[INFO] Reading " + file.string());
	PluginEntry entry;
	Package package;
	ExtractPackage(file, CacheDir(), package);
	entry.package = std::move(package);
	if (!entry.package.valid) {
		entry.state = "Invalid";
		entry.error = entry.package.code + ": " + entry.package.message;
		LogLine("[ERROR] " + file.filename().string() + ": " + entry.error);
		m_entries.push_back(std::move(entry));
		return;
	}

	const std::string id = entry.Id();
	for (const auto& existing : m_entries) {
		if (!existing.removed && existing.package.valid && existing.Id() == id) {
			LogLine("[WARN] Duplicate plugin id '" + id + "'; ignoring " + file.string());
			return;
		}
	}

	entry.state = "Discovered";
	m_entries.push_back(std::move(entry));
	ApplyTrustAndLoad(m_entries.back());
}

void PluginRegistry::ApplyTrustAndLoad(PluginEntry& entry) {
	if (!entry.package.valid || entry.removed)
		return;

	const Manifest& manifest = entry.package.manifest;
	const TrustRecord* record = m_trust.Find(manifest.id);
	const TrustEvaluation evaluation = EvaluateTrust(record, manifest.IsNative(),
		entry.package.sha256, manifest.permissions);

	entry.trustedAt = record ? record->trustedAt : std::string();

	switch (evaluation.decision) {
	case TrustDecision::Blocked:
		entry.blocked = true;
		entry.enabled = false;
		entry.trusted = true;
		entry.state = "Blocked";
		entry.error = "plugin.blocked: blocked by the trust store";
		LogLine("[WARN] Blocked plugin " + manifest.id + " (hash " +
			entry.package.sha256.substr(0, 12) + ")");
		return;
	case TrustDecision::Disabled:
		entry.enabled = false;
		entry.trusted = true;
		entry.state = "Disabled";
		return;
	case TrustDecision::Trusted: {
		entry.trusted = true;
		entry.enabled = true;
		if (evaluation.hashChanged) {
			// Declarative upgrade with unchanged permissions: record the new
			// hash in the per-hash history without prompting.
			TrustRecord updated;
			updated.id = manifest.id;
			updated.version = manifest.version;
			updated.sha256 = entry.package.sha256;
			updated.permissions = manifest.permissions;
			updated.enabled = true;
			updated.blocked = false;
			if (record)
				updated.history = record->history;
			m_trust.Upsert(std::move(updated));
			SaveTrust();
			entry.trustedAt = m_trust.Find(manifest.id) ? m_trust.Find(manifest.id)->trustedAt : entry.trustedAt;
			LogLine("[INFO] Accepted new hash for " + manifest.id + " without a new prompt");
		}
		else {
			LogLine("[INFO] Trusted plugin " + manifest.id + " (approved " + entry.trustedAt + ")");
		}
		LoadEntry(entry);
		return;
	}
	case TrustDecision::NoRecord:
	case TrustDecision::Prompt:
		entry.pendingTrust = true;
		entry.state = "Awaiting approval";
		LogLine("[INFO] Plugin " + manifest.id + " is waiting for approval (" +
			TrustReason(evaluation) + ")");
		return;
	}
}

bool PluginRegistry::RequireCapability(PluginEntry& entry, Capability capability,
	const std::string& phase) {
	PermissionSet permissions(entry.package.manifest.permissions);
	if (permissions.Allows(capability))
		return true;

	std::string message = "permission denied: '" + phase + "' requires the '" +
		std::string(CapabilityName(capability)) + "' capability (declare ";
	const auto accepted = CapabilityPermissions(capability);
	for (size_t i = 0; i < accepted.size(); ++i) {
		if (i > 0)
			message += " or ";
		message += "'" + accepted[i] + "'";
	}
	message += ")";

	entry.state = "Error";
	entry.error = std::string(kPermissionDeniedCode) + ": " + message;
	entry.error += " (declared permissions: " +
		(entry.package.manifest.permissions.empty() ? std::string("none") :
			JoinPermissions(entry.package.manifest.permissions)) + ")";
	entry.limited = true;
	const std::string name = CapabilityName(capability);
	if (std::find(entry.deniedCapabilities.begin(), entry.deniedCapabilities.end(), name) == entry.deniedCapabilities.end())
		entry.deniedCapabilities.push_back(name);
	LogLine("[ERROR] " + entry.Id() + ": " + entry.error);
	return false;
}

void PluginRegistry::ApplyDenials(PluginEntry& entry) {
	const auto denials = TakePermissionDenials(entry.Id());
	if (denials.empty())
		return;
	entry.limited = true;
	for (const auto& denial : denials) {
		if (std::find(entry.deniedCapabilities.begin(), entry.deniedCapabilities.end(), denial.capability) ==
			entry.deniedCapabilities.end())
			entry.deniedCapabilities.push_back(denial.capability);
		const std::string message = "plugin.permission: '" + denial.api + "' requires the '" +
			denial.capability + "' capability";
		if (entry.error.empty())
			entry.error = message;
		LogLine("[WARN] " + entry.Id() + ": " + message);
	}
	if (entry.loaded)
		entry.state = "Loaded (limited)";
}

void PluginRegistry::LoadEntry(PluginEntry& entry) {
	if (entry.loaded || !entry.enabled || entry.blocked || entry.removed || !entry.package.valid)
		return;
	if (entry.libraryRetained) {
		entry.state = "Restart required (hooks retained)";
		entry.error = "plugin.lifecycle: the previous library is still mapped because it "
			"registered emulation hooks; restart the app to load this plugin again";
		LogLine("[WARN] " + entry.Id() + ": " + entry.error);
		return;
	}

	if (entry.package.manifest.IsNative()) {
		if (LoadNative(entry))
			return;
		if (entry.package.manifest.type == PluginType::UiRevamp) {
			LogLine("[WARN] Native shell for " + entry.Id() + " failed; trying the declarative fallback");
			LoadDeclarative(entry);
		}
		return;
	}
	LoadDeclarative(entry);
}

bool PluginRegistry::LoadDeclarative(PluginEntry& entry) {
	std::string text;
	std::string error;
	const std::filesystem::path document_path = entry.package.root / entry.package.manifest.entryPoint;
	if (!ReadTextFile(document_path, text, error)) {
		entry.state = "Error";
		entry.error = error;
		LogLine("[ERROR] " + entry.Id() + ": " + error);
		entry.loaded = false;
		return false;
	}

	if (entry.package.manifest.type == PluginType::Theme) {
		if (!RequireCapability(entry, Capability::Theme, "theme application"))
			return false;
		if (!Host().ApplyPluginTheme(text, error)) {
			entry.state = "Error";
			entry.error = error;
			LogLine("[ERROR] " + entry.Id() + ": " + error);
			entry.loaded = false;
			return false;
		}
		m_activeThemeId = entry.Id();
		m_activeThemeDocument = text;
		m_themeStale = false;
	}
	else {
		if (entry.package.manifest.type == PluginType::Inject) {
			if (!RequireCapability(entry, Capability::Inject, "inject payload registration"))
				return false;
			std::vector<InjectionPayload> payloads;
			std::string inject_error;
			if (!ParseInjectDocument(text, entry.Id(), payloads, inject_error)) {
				entry.state = "Error";
				entry.error = "inject.invalid: " + inject_error;
				LogLine("[ERROR] " + entry.Id() + ": " + entry.error);
				entry.loaded = false;
				return false;
			}
			RegisterPluginPayloads(entry.Id(), std::move(payloads));
		}
		else if (entry.package.manifest.type == PluginType::UiRevamp) {
			if (!RequireCapability(entry, Capability::Shell, "layout application"))
				return false;
			LayoutActions actions;
			std::string layout_error;
			if (!ParseLayoutDocument(text, actions, layout_error)) {
				entry.state = "Error";
				entry.error = layout_error;
				LogLine("[ERROR] " + entry.Id() + ": " + layout_error);
				entry.loaded = false;
				return false;
			}
			if (!ApplyLayout(entry, actions, layout_error)) {
				entry.state = "Error";
				entry.error = layout_error;
				LogLine("[ERROR] " + entry.Id() + ": " + layout_error);
				entry.loaded = false;
				return false;
			}
		}
		else {
			// Generic declarative document validation for future types.
			try {
				const json document = json::parse(text);
				if (!document.is_object() || document.value("schema", 0) != 1) {
					entry.state = "Error";
					entry.error = "declarative document must have schema 1";
					LogLine("[ERROR] " + entry.Id() + ": " + entry.error);
					entry.loaded = false;
					return false;
				}
			}
			catch (const json::exception& parse_error) {
				entry.state = "Error";
				entry.error = parse_error.what();
				LogLine("[ERROR] " + entry.Id() + ": " + entry.error);
				entry.loaded = false;
				return false;
			}
		}
	}

	entry.document = std::move(text);
	entry.loaded = true;
	entry.state = "Loaded";
	entry.error.clear();
	LogLine("[SUCCESS] Loaded " + std::string(TypeName(entry.package.manifest.type)) + " plugin " + entry.Id());
	ApplyDenials(entry);
	return true;
}

bool PluginRegistry::LoadNative(PluginEntry& entry) {
	if (!RequireCapability(entry, Capability::Native, "native library loading"))
		return false;

	const std::filesystem::path library = entry.package.root / entry.package.manifest.nativeEntry;
	std::error_code ec;
	if (!std::filesystem::exists(library, ec)) {
		entry.state = "Error";
		entry.error = std::string("native.entry: native entry not found: ") + library.string();
		LogLine("[ERROR] " + entry.Id() + ": " + entry.error);
		return false;
	}

	void* handle = dlopen(library.string().c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		const char* reason = dlerror();
		entry.state = "Error";
		entry.error = std::string("native.load: ") + (reason ? reason : "dlopen failed");
		LogLine("[ERROR] " + entry.Id() + ": " + entry.error);
		return false;
	}

	auto load = reinterpret_cast<PluginLoadFn>(dlsym(handle, "fPluginLoad"));
	if (!load) {
		dlclose(handle);
		entry.state = "Error";
		entry.error = "native.symbol: missing fPluginLoad";
		LogLine("[ERROR] " + entry.Id() + ": " + entry.error);
		return false;
	}

	// ABI handshake.  Missing exports are accepted for v1/v2 packages that
	// predate the check, but a mismatch is always fatal.
	using ApiVersionFn = int (*)();
	using AbiSignatureFn = unsigned long (*)();
	unsigned long plugin_signature = 0;
	if (auto version_fn = reinterpret_cast<ApiVersionFn>(dlsym(handle, kPluginApiVersionSymbol))) {
		const int plugin_api = version_fn();
		if (plugin_api > kHostApiVersion || plugin_api < kHostApiMinVersion) {
			dlclose(handle);
			entry.state = "Error";
			entry.error = "native.api: plugin targets API v" + std::to_string(plugin_api) +
				"; host supports v" + std::to_string(kHostApiMinVersion) + ".." +
				std::to_string(kHostApiVersion);
			LogLine("[ERROR] " + entry.Id() + ": " + entry.error);
			return false;
		}
	}
	if (auto signature_fn = reinterpret_cast<AbiSignatureFn>(dlsym(handle, kPluginAbiSignatureSymbol))) {
		plugin_signature = signature_fn();
		const unsigned long host_signature = HostUiWindowAbiSignature();
		if (plugin_signature != host_signature) {
			dlclose(handle);
			entry.state = "Error";
			entry.error = "native.api: UIWindow ABI mismatch (plugin " +
				std::to_string(plugin_signature) + ", host " + std::to_string(host_signature) +
				"); rebuild the plugin with the SDK headers in this tree";
			LogLine("[ERROR] " + entry.Id() + ": " + entry.error);
			return false;
		}
	}
	else {
		LogLine("[WARN] " + entry.Id() + ": no fPluginAbiSignature export; assuming legacy UIWindow layout");
	}

	PluginApi* api = Host().CreateApi(entry.Id(), entry.package.manifest);
	try {
		load(api);
	}
	catch (const std::exception& error) {
		Host().DestroyApi(api);
		dlclose(handle);
		entry.state = "Error";
		entry.error = std::string("native.load: fPluginLoad threw: ") + error.what();
		LogLine("[ERROR] " + entry.Id() + ": " + entry.error);
		return false;
	}
	catch (...) {
		Host().DestroyApi(api);
		dlclose(handle);
		entry.state = "Error";
		entry.error = "native.load: fPluginLoad threw an unknown exception";
		LogLine("[ERROR] " + entry.Id() + ": " + entry.error);
		return false;
	}

	entry.handle = handle;
	entry.apiContext = api;
	entry.loaded = true;
	entry.state = "Loaded";
	entry.error.clear();
	LogLine("[SUCCESS] Loaded native plugin " + entry.Id());
	ApplyDenials(entry);
	return true;
}

void PluginRegistry::Approve(PluginEntry& entry, bool remember) {
	entry.blocked = false;
	entry.pendingTrust = false;
	entry.enabled = true;
	entry.trusted = true;
	if (remember) {
		TrustRecord record;
		record.id = entry.Id();
		record.version = entry.Version();
		record.sha256 = entry.package.sha256;
		record.permissions = entry.package.manifest.permissions;
		record.enabled = true;
		record.blocked = false;
		if (const TrustRecord* existing = m_trust.Find(entry.Id()))
			record.history = existing->history;
		m_trust.Upsert(std::move(record));
		SaveTrust();
		entry.trustedAt = m_trust.Find(entry.Id()) ? m_trust.Find(entry.Id())->trustedAt : CurrentUtcTimestamp();
	}
	LoadEntry(entry);
}

void PluginRegistry::Reject(PluginEntry& entry) {
	entry.blocked = true;
	entry.pendingTrust = false;
	entry.enabled = false;
	entry.state = "Blocked";
	entry.error = "plugin.blocked: blocked by the user";
	TrustRecord record;
	record.id = entry.Id();
	record.version = entry.Version();
	record.sha256 = entry.package.sha256;
	record.permissions = entry.package.manifest.permissions;
	record.enabled = false;
	record.blocked = true;
	m_trust.Upsert(std::move(record));
	SaveTrust();
	LogLine("[WARN] User blocked plugin " + entry.Id());
}

void PluginRegistry::Review(const std::string& id) {
	for (auto& entry : m_entries) {
		if (!entry.removed && entry.Id() == id) {
			entry.pendingTrust = true;
			entry.blocked = false;
			entry.state = "Awaiting approval";
			return;
		}
	}
}

void PluginRegistry::HidePluginWindows(PluginEntry& entry) {
	const auto tracked = TakePluginWindows(entry.Id());
	for (void* pointer : tracked) {
		Host().SetWindowOpen(pointer, false);
		Host().RemoveWindow(pointer);
	}
}

void PluginRegistry::ClearRuntimeState(PluginEntry& entry) {
	entry.loaded = false;
	entry.handle = nullptr;
	entry.apiContext = nullptr;
	entry.limited = false;
	entry.deniedCapabilities.clear();
	entry.document.clear();
	ClearPluginPayloads(entry.Id());
	ClearPluginResources(entry.Id());
	if (m_activeThemeId == entry.Id()) {
		m_activeThemeId.clear();
		m_activeThemeDocument.clear();
		m_themeStale = false;
	}
}

bool PluginRegistry::Unload(const std::string& id) {
	for (auto& entry : m_entries) {
		if (entry.removed || entry.Id() != id)
			continue;
		return UnloadEntry(entry, true);
	}
	return false;
}

bool PluginRegistry::UnloadEntry(PluginEntry& entry, bool remove_windows) {
	const std::string id = entry.Id();
	if (!entry.loaded) {
		if (!entry.libraryRetained)
			entry.state = "Unloaded";
		return true;
	}

	void* handle = entry.handle;
	PluginApi* api = static_cast<PluginApi*>(entry.apiContext);
	const bool was_theme = entry.package.valid && entry.package.manifest.type == PluginType::Theme;

	// Remove windows before fPluginUnload so a plugin that deletes its own
	// windows cannot leave dangling pointers in the host render loop.  At
	// process exit the GUI is already torn down, so only forget the tracking
	// entries.
	if (remove_windows)
		HidePluginWindows(entry);
	else
		(void)TakePluginWindows(entry.Id());

	if (handle) {
		const UnloadCallResult result = CallPluginUnload(handle, api);
		if (result.had_callback && !result.threw)
			LogLine("[INFO] Called fPluginUnload for " + id);
		else if (result.threw)
			LogLine("[ERROR] " + id + ": fPluginUnload threw: " + result.error);
	}

	const bool retain_library = handle && PluginHasHookRegistrations(id);
	if (handle && !retain_library) {
		dlclose(handle);
		LogLine("[INFO] Unloaded native library " + id);
	}
	else if (retain_library) {
		entry.libraryRetained = true;
		LogLine("[WARN] " + id + ": fPluginUnload ran but the library stays mapped "
			"because the plugin registered emulation hooks that cannot be unregistered");
	}

	if (api && !retain_library) {
		Host().DestroyApi(api);
		entry.apiContext = nullptr;
	}
	else if (retain_library) {
		// Keep the facade alive: hook callbacks still reference its guards.
		entry.apiContext = api;
	}

	entry.handle = retain_library ? handle : nullptr;
	entry.loaded = false;
	entry.state = retain_library ? "Unloaded (library retained: hooks)" : "Unloaded";
	entry.error.clear();
	if (was_theme && remove_windows)
		Host().ResetTheme();
	return true;
}

void PluginRegistry::SetEnabled(const std::string& id, bool enabled) {
	for (auto& entry : m_entries) {
		if (entry.removed || entry.Id() != id)
			continue;

		TrustRecord* existing = m_trust.Find(entry.Id());
		if (!enabled) {
			if (entry.IsNative() && entry.loaded) {
				Unload(id);
				entry.state = entry.libraryRetained
					? "Disabled (library retained: hooks)" : "Disabled";
			}
			else {
				entry.loaded = false;
				if (entry.package.valid && entry.package.manifest.type == PluginType::Inject)
					ClearPluginPayloads(entry.Id());
				entry.state = "Disabled";
			}
			entry.enabled = false;
			if (entry.package.valid && entry.package.manifest.type == PluginType::Theme)
				Host().ResetTheme();
			if (existing) {
				existing->enabled = false;
				existing->trustedAt = CurrentUtcTimestamp();
				SaveTrust();
			}
			return;
		}

		entry.enabled = true;
		entry.blocked = false;
		if (existing) {
			existing->enabled = true;
			SaveTrust();
		}
		if (entry.trusted) {
			// Re-check the record: the package may have changed while disabled.
			const TrustRecord* record = m_trust.Find(entry.Id());
			const bool changed = !entry.package.valid || !record ||
				!SamePermissionSet(record->permissions, entry.package.manifest.permissions) ||
				(entry.IsNative() && record->sha256 != entry.package.sha256);
			if (changed) {
				entry.pendingTrust = true;
				entry.state = "Awaiting approval";
			}
			else {
				LoadEntry(entry);
			}
		}
		else {
			entry.pendingTrust = true;
			entry.state = "Awaiting approval";
		}
		return;
	}
}

void PluginRegistry::Remove(const std::string& id) {
	for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
		if (it->removed || it->Id() != id)
			continue;

		PluginEntry& entry = *it;
		const bool was_theme = entry.package.valid && entry.package.manifest.type == PluginType::Theme;
		const bool was_loaded = entry.loaded;
		if (was_loaded)
			Unload(id);

		const bool retained = entry.libraryRetained;
		if (retained) {
			entry.removed = true;
			entry.enabled = false;
			entry.state = "Removed (library retained: hooks)";
			entry.package.root.clear();
		}
		else {
			std::error_code ec;
			if (!entry.package.root.empty())
				std::filesystem::remove_all(entry.package.root, ec);
			ClearPluginPayloads(id);
			ClearPluginResources(id);
			m_entries.erase(it);
		}
		m_trust.Remove(id);
		SaveTrust();
		if (was_theme)
			Host().ResetTheme();
		LogLine("[INFO] Removed plugin " + id);
		return;
	}
}

void PluginRegistry::ReloadDeclarative() {
	for (auto& entry : m_entries) {
		if (entry.removed || !entry.loaded || entry.IsNative())
			continue;
		if (entry.package.manifest.type == PluginType::Inject)
			ClearPluginPayloads(entry.Id());
		entry.loaded = false;
		LoadDeclarative(entry);
	}
}

void PluginRegistry::Reveal(const std::string& id) const {
	for (const auto& entry : m_entries) {
		if (!entry.removed && entry.Id() == id) {
			RevealPath(entry.package.file);
			return;
		}
	}
}

void PluginRegistry::RevealPath(const std::filesystem::path& path) const {
	std::system(("open -R \"" + path.string() + "\" >/dev/null 2>&1").c_str());
}

void PluginRegistry::OpenPluginsFolder() const {
	std::error_code ec;
	std::filesystem::create_directories(UserPluginsDir(), ec);
	std::system(("open \"" + UserPluginsDir().string() + "\" >/dev/null 2>&1").c_str());
}

bool PluginRegistry::InstallFromFile(const std::filesystem::path& file, std::string* error) {
	const auto fail = [&](const std::string& message) {
		LogLine("[ERROR] " + message);
		if (error)
			*error = message;
		return false;
	};

	std::error_code ec;
	if (!std::filesystem::is_regular_file(file, ec))
		return fail("Not a plugin file: " + file.string());
	if (!HasPluginExtension(file))
		return fail("Not a .plugin package: " + file.filename().string());

	std::filesystem::create_directories(UserPluginsDir(), ec);
	const std::filesystem::path destination = UserPluginsDir() / file.filename();
	if (std::filesystem::exists(destination, ec))
		return fail("A plugin with this filename already exists: " + destination.filename().string());

	std::filesystem::copy_file(file, destination, ec);
	if (ec)
		return fail("Cannot install plugin: " + ec.message());

	const size_t before = m_entries.size();
	AddPackage(destination);
	if (m_entries.size() > before) {
		PluginEntry& added = m_entries.back();
		if (added.state == "Invalid" || added.state == "Error") {
			const std::string message = added.error;
			// Keep the copied file for inspection, but report the failure.
			if (error)
				*error = message;
			return false;
		}
	}
	LogLine("[SUCCESS] Installed " + destination.filename().string());
	if (error)
		error->clear();
	return true;
}

void PluginRegistry::InstallFromDialog() {
	std::string selected;
	if (!Host().PickPluginFile(selected))
		return;
	std::string error;
	if (!InstallFromFile(selected, &error))
		LogLine("[ERROR] Install failed: " + error);
}

int PluginRegistry::FirstPendingIndex() const {
	for (size_t i = 0; i < m_entries.size(); ++i) {
		if (!m_entries[i].removed && m_entries[i].pendingTrust)
			return static_cast<int>(i);
	}
	return -1;
}

bool PluginRegistry::ApplyLayout(PluginEntry& entry, const LayoutActions& actions, std::string& error) {
	if (!Host().ApplyShellLayout(actions, error))
		return false;

	// Show/hide named tool windows.  Unknown names are a harmless no-op; the
	// layout can target tools that only exist in other builds.
	std::vector<std::string> hide = actions.hiddenTools;
	if (actions.hasTools && !actions.toolsVisible)
		hide.insert(hide.end(), actions.toolOrder.begin(), actions.toolOrder.end());
	const auto open_windows = Host().Windows();
	for (const auto& name : hide) {
		bool found = false;
		for (void* window : open_windows) {
			const char* window_name = Host().WindowName(window);
			if (window_name && name == window_name) {
				Host().SetWindowOpen(window, false);
				found = true;
			}
		}
		if (!found)
			LogLine("[WARN] " + entry.Id() + ": layout hides unknown tool '" + name + "'");
	}

	if (!actions.toolOrder.empty())
		Host().ReorderWindows(actions.toolOrder);

	if (actions.hasTheme) {
		std::string theme_error;
		if (!Host().SelectTheme(actions.theme, theme_error)) {
			error = std::string(kLayoutUnsupportedCode) + ": unsupported action: cannot select theme '" +
				actions.theme + "': " + theme_error;
			return false;
		}
		LogLine("[INFO] " + entry.Id() + ": selected built-in theme '" + actions.theme + "'");
	}

	LogLine("[INFO] Applied declarative layout from " + entry.Id());
	return true;
}

void PluginRegistry::PumpFrame() {
	if (m_activeThemeDocument.empty())
		return;
	// The startup font rebuild (ThemeManager::ProcessFontRebuild) assigns the
	// user's saved style after the plugin scan.  Re-apply once it has run, and
	// again after any later font rebuild.
	if (Host().FontRebuildPending()) {
		m_themeStale = true;
		return;
	}
	if (!m_themeStale)
		return;
	std::string error;
	if (Host().ApplyPluginTheme(m_activeThemeDocument, error))
		LogLine("[INFO] Reapplied theme plugin " + m_activeThemeId + " after font rebuild");
	else
		LogLine("[ERROR] " + m_activeThemeId + ": cannot reapply theme: " + error);
	m_themeStale = false;
}

void PluginRegistry::ReapplyActiveTheme() {
	if (m_activeThemeDocument.empty())
		return;
	std::string error;
	if (Host().ApplyPluginTheme(m_activeThemeDocument, error))
		m_themeStale = false;
	else
		LogLine("[ERROR] " + m_activeThemeId + ": cannot reapply theme: " + error);
}

void PluginRegistry::RenderTrustModal() {
	PumpFrame();

	if (!m_modalActive && !m_openModalRequested) {
		const int index = FirstPendingIndex();
		if (index >= 0) {
			m_modalIndex = index;
			m_openModalRequested = true;
		}
	}
	if (!m_modalActive && !m_openModalRequested)
		return;

	ImGui::SetNextWindowSize(ImVec2(1.0f, 1.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin("##PluginTrustHost", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);

	// The modal is closable through the title-bar X as well as the Later button.
	static bool trust_modal_open = true;
	if (m_openModalRequested) {
		ImGui::OpenPopup("Plugin Trust###plugin_trust");
		m_openModalRequested = false;
		m_modalActive = true;
		trust_modal_open = true;
	}

	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(520, 0), ImVec2(760, FLT_MAX));
	if (m_modalActive && ImGui::BeginPopupModal("Plugin Trust###plugin_trust", &trust_modal_open, ImGuiWindowFlags_AlwaysAutoResize)) {
		if (m_modalIndex < 0 || m_modalIndex >= static_cast<int>(m_entries.size()) || m_entries[m_modalIndex].removed) {
			m_modalActive = false;
			m_modalIndex = -1;
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			ImGui::End();
			ImGui::PopStyleVar();
			return;
		}

		PluginEntry& entry = m_entries[m_modalIndex];
		const Manifest& manifest = entry.package.manifest;
		ImGui::TextUnformatted(manifest.name.c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("%s", manifest.version.c_str());
		ImGui::TextDisabled("%s  |  %s  |  api v%d", manifest.id.c_str(), TypeName(manifest.type),
			manifest.apiVersion);
		if (!manifest.author.empty())
			ImGui::TextDisabled("By %s", manifest.author.c_str());
		if (!manifest.description.empty()) {
			ImGui::Spacing();
			ImGui::TextWrapped("%s", manifest.description.c_str());
		}

		ImGui::Spacing();
		ImGui::TextUnformatted("Package SHA-256");
		ImGui::TextDisabled("%s", entry.package.sha256.c_str());
		if (const TrustRecord* record = m_trust.Find(entry.Id())) {
			if (!record->trustedAt.empty())
				ImGui::TextDisabled("Approved %s  |  %zu hash(es) on record", record->trustedAt.c_str(),
					record->history.size());
			ImGui::TextDisabled("Previously approved: %s", record->sha256.substr(0, 16).c_str());
		}

		ImGui::Spacing();
		ImGui::TextUnformatted("This plugin requests:");
		if (manifest.permissions.empty()) {
			ImGui::BulletText("No special permissions");
		}
		else {
			for (const auto& permission : manifest.permissions)
				ImGui::BulletText("%s - %s", permission.c_str(), PermissionDescription(permission));
		}

		if (entry.IsNative()) {
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.65f, 0.00f, 1.0f));
			ImGui::TextWrapped("This plugin includes native code. It runs in the same process with the same access as the app; the host gates host-API calls by permission, but native code can still call the OS directly.");
			ImGui::PopStyleColor();
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::TextDisabled("Only allow plugins you trust. Always Allow remembers this exact hash; permission changes ask again.");
		ImGui::Spacing();

		if (ImGui::Button("Always Allow", ImVec2(140, 0))) {
			Approve(entry, true);
			m_modalActive = false;
			m_modalIndex = -1;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Allow Once", ImVec2(120, 0))) {
			entry.trustedThisSession = true;
			Approve(entry, false);
			m_modalActive = false;
			m_modalIndex = -1;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Block", ImVec2(90, 0))) {
			Reject(entry);
			m_modalActive = false;
			m_modalIndex = -1;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Later", ImVec2(90, 0))) {
			entry.pendingTrust = false;
			entry.state = "Deferred";
			m_modalActive = false;
			m_modalIndex = -1;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
	else if (m_modalActive) {
		// The popup was dismissed without one of our buttons (the title-bar X or
		// Escape).  Defer the entry, matching the Later button, and clear the
		// flag so the next pending plugin can prompt.
		if (!trust_modal_open) {
			trust_modal_open = true;
			if (m_modalIndex >= 0 && m_modalIndex < static_cast<int>(m_entries.size()) &&
				!m_entries[m_modalIndex].removed) {
				m_entries[m_modalIndex].pendingTrust = false;
				m_entries[m_modalIndex].state = "Deferred";
			}
			m_modalIndex = -1;
		}
		m_modalActive = false;
	}

	ImGui::End();
	ImGui::PopStyleVar();
}

} // namespace casioemu::plugin
