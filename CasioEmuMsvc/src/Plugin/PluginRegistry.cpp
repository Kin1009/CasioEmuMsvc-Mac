#include "PluginRegistry.h"

// The host shares UIWindow with the plugin SDK.  Define _NO_FUND_API before
// including PluginApi.h so the header does not declare its own UIWindow; the
// canonical definition lives in Gui/Ui.hpp.
#define _NO_FUND_API
#include "PluginApi.h"
#include "PluginMan.h"
#include "ThemeTokens.h"
#include "ThemeManager.h"
#include "Ui.hpp"

#include "../../../McpPlugin/json.hpp"

#include <imgui.h>
#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <sstream>
#include <system_error>

extern PluginApi* g_pluginapi;

namespace casioemu::plugin {
namespace {

using json = nlohmann::json;

constexpr const char* kTrustFileName = "plugin-trust.json";

std::filesystem::path ExecutableDir() {
	char* base = SDL_GetBasePath();
	if (!base)
		return std::filesystem::current_path();
	std::filesystem::path path(base);
	SDL_free(base);
	return path;
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

bool SamePermissions(const std::vector<std::string>& lhs, const std::vector<std::string>& rhs) {
	return lhs == rhs;
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
	for (auto& entry : m_entries) {
		if (entry.handle) {
			dlclose(entry.handle);
			entry.handle = nullptr;
			entry.loaded = false;
		}
	}
	m_entries.clear();
	m_trust.clear();
	m_initialized = false;
}

void PluginRegistry::LoadTrust() {
	m_trust.clear();
	const std::filesystem::path path = SettingsDir() / kTrustFileName;
	std::ifstream file(path, std::ios::binary);
	if (!file)
		return;
	try {
		json document;
		file >> document;
		if (!document.is_object() || document.value("schema", 0) != 1)
			return;
		if (!document.contains("records") || !document["records"].is_array())
			return;
		for (const auto& item : document["records"]) {
			if (!item.is_object())
				continue;
			TrustRecord record;
			record.id = item.value("id", "");
			record.version = item.value("version", "");
			record.sha256 = item.value("sha256", "");
			record.enabled = item.value("enabled", true);
			record.blocked = item.value("blocked", false);
			if (item.contains("permissions") && item["permissions"].is_array()) {
				for (const auto& permission : item["permissions"]) {
					if (permission.is_string())
						record.permissions.push_back(permission.get<std::string>());
				}
			}
			if (!record.id.empty())
				m_trust.push_back(std::move(record));
		}
	}
	catch (const json::exception& error) {
		LogLine(std::string("[WARN] Cannot read plugin trust store: ") + error.what());
	}
}

void PluginRegistry::SaveTrust() const {
	std::error_code ec;
	std::filesystem::create_directories(SettingsDir(), ec);
	const std::filesystem::path path = SettingsDir() / kTrustFileName;
	const std::filesystem::path temp = path.string() + ".tmp";
	json document;
	document["schema"] = 1;
	document["records"] = json::array();
	for (const auto& record : m_trust) {
		json item;
		item["id"] = record.id;
		item["version"] = record.version;
		item["sha256"] = record.sha256;
		item["permissions"] = record.permissions;
		item["enabled"] = record.enabled;
		item["blocked"] = record.blocked;
		document["records"].push_back(std::move(item));
	}
	std::ofstream file(temp, std::ios::binary | std::ios::trunc);
	if (!file)
		return;
	file << document.dump(2);
	file.close();
	std::filesystem::rename(temp, path, ec);
	if (ec) {
		std::filesystem::remove(path, ec);
		std::filesystem::rename(temp, path, ec);
	}
}

PluginRegistry::TrustRecord* PluginRegistry::FindTrust(const std::string& id) {
	for (auto& record : m_trust) {
		if (record.id == id)
			return &record;
	}
	return nullptr;
}

const PluginRegistry::TrustRecord* PluginRegistry::FindTrust(const std::string& id) const {
	for (const auto& record : m_trust) {
		if (record.id == id)
			return &record;
	}
	return nullptr;
}

void PluginRegistry::UpsertTrust(const PluginEntry& entry, bool blocked) {
	TrustRecord* record = FindTrust(entry.Id());
	if (!record) {
		m_trust.push_back({});
		record = &m_trust.back();
	}
	record->id = entry.Id();
	record->version = entry.Version();
	record->sha256 = entry.package.sha256;
	record->permissions = entry.package.manifest.permissions;
	record->enabled = entry.enabled;
	record->blocked = blocked;
}

void PluginRegistry::RemoveTrust(const std::string& id) {
	m_trust.erase(std::remove_if(m_trust.begin(), m_trust.end(), [&](const TrustRecord& record) {
		return record.id == id;
	}), m_trust.end());
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
		std::string extension = item.path().extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (extension != ".plugin")
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
	const TrustRecord* record = FindTrust(manifest.id);
	const bool native = manifest.IsNative();
	const bool hash_changed = record && record->sha256 != entry.package.sha256;
	const bool permissions_changed = record && !SamePermissions(record->permissions, manifest.permissions);

	if (record && record->blocked) {
		entry.blocked = true;
		entry.enabled = false;
		entry.state = "Blocked";
		LogLine("[WARN] Blocked plugin " + manifest.id + " (hash " + entry.package.sha256.substr(0, 12) + ")");
		return;
	}

	if (record && !record->enabled) {
		entry.enabled = false;
		entry.trusted = true;
		entry.state = "Disabled";
		return;
	}

	if (record && !permissions_changed && !(native && hash_changed)) {
		entry.trusted = true;
		entry.enabled = true;
		if (hash_changed) {
			UpsertTrust(entry, false);
			SaveTrust();
			LogLine("[INFO] Updated declarative plugin " + manifest.id + " without a new prompt");
		}
		LoadEntry(entry);
		return;
	}

	entry.pendingTrust = true;
	entry.state = "Awaiting approval";
	LogLine("[INFO] Plugin " + manifest.id + " is waiting for approval");
}

void PluginRegistry::LoadEntry(PluginEntry& entry) {
	if (entry.loaded || !entry.enabled || entry.blocked || entry.removed || !entry.package.valid)
		return;

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
		if (!ApplyThemeDocument(text, error)) {
			entry.state = "Error";
			entry.error = error;
			LogLine("[ERROR] " + entry.Id() + ": " + error);
			entry.loaded = false;
			return false;
		}
		g_plugin_theme_active = true;
	}
	else {
		try {
			const json document = json::parse(text);
			if (!document.is_object() || document.value("schema", 0) != 1) {
				entry.state = "Error";
				entry.error = "declarative document must have schema 1";
				LogLine("[ERROR] " + entry.Id() + ": " + entry.error);
				entry.loaded = false;
				return false;
			}
			if (entry.package.manifest.type == PluginType::Inject && !document.contains("payloads")) {
				entry.state = "Error";
				entry.error = "inject plugin is missing 'payloads'";
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

	entry.document = std::move(text);
	entry.loaded = true;
	entry.state = "Loaded";
	entry.error.clear();
	LogLine("[SUCCESS] Loaded " + std::string(TypeName(entry.package.manifest.type)) + " plugin " + entry.Id());
	return true;
}

bool PluginRegistry::LoadNative(PluginEntry& entry) {
	const std::filesystem::path library = entry.package.root / entry.package.manifest.nativeEntry;
	std::error_code ec;
	if (!std::filesystem::exists(library, ec)) {
		entry.state = "Error";
		entry.error = "native entry not found: " + library.string();
		LogLine("[ERROR] " + entry.Id() + ": " + entry.error);
		return false;
	}

	void* handle = dlopen(library.string().c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		const char* reason = dlerror();
		entry.state = "Error";
		entry.error = reason ? reason : "dlopen failed";
		LogLine("[ERROR] " + entry.Id() + ": " + entry.error);
		return false;
	}

	auto load = reinterpret_cast<PluginLoad>(dlsym(handle, "fPluginLoad"));
	if (!load) {
		dlclose(handle);
		entry.state = "Error";
		entry.error = "missing fPluginLoad";
		LogLine("[ERROR] " + entry.Id() + ": " + entry.error);
		return false;
	}

	try {
		load(g_pluginapi);
	}
	catch (const std::exception& error) {
		dlclose(handle);
		entry.state = "Error";
		entry.error = std::string("fPluginLoad threw: ") + error.what();
		LogLine("[ERROR] " + entry.Id() + ": " + entry.error);
		return false;
	}
	catch (...) {
		dlclose(handle);
		entry.state = "Error";
		entry.error = "fPluginLoad threw an unknown exception";
		LogLine("[ERROR] " + entry.Id() + ": " + entry.error);
		return false;
	}

	entry.handle = handle;
	entry.loaded = true;
	entry.state = "Loaded";
	entry.error.clear();
	LogLine("[SUCCESS] Loaded native plugin " + entry.Id());
	return true;
}

void PluginRegistry::Approve(PluginEntry& entry, bool remember) {
	entry.blocked = false;
	entry.pendingTrust = false;
	entry.enabled = true;
	entry.trusted = true;
	if (remember) {
		UpsertTrust(entry, false);
		SaveTrust();
	}
	LoadEntry(entry);
}

void PluginRegistry::Reject(PluginEntry& entry) {
	entry.blocked = true;
	entry.pendingTrust = false;
	entry.enabled = false;
	entry.state = "Blocked";
	UpsertTrust(entry, true);
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

void PluginRegistry::SetEnabled(const std::string& id, bool enabled) {
	for (auto& entry : m_entries) {
		if (entry.removed || entry.Id() != id)
			continue;
		entry.enabled = enabled;
		if (enabled) {
			entry.blocked = false;
			if (entry.trusted) {
				LoadEntry(entry);
			}
			else {
				entry.pendingTrust = true;
				entry.state = "Awaiting approval";
			}
		}
		else {
			if (entry.IsNative() && entry.loaded) {
				entry.state = "Disabled (restart to unload)";
			}
			else {
				entry.loaded = false;
				entry.state = "Disabled";
			}
			if (entry.package.valid && entry.package.manifest.type == PluginType::Theme) {
				g_plugin_theme_active = false;
				ThemeManager::Instance().ApplyDefaultTheme();
			}
		}
		UpsertTrust(entry, entry.blocked);
		SaveTrust();
		return;
	}
}

void PluginRegistry::Remove(const std::string& id) {
	for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
		if (it->removed || it->Id() != id)
			continue;
		const bool loaded_native = it->IsNative() && it->loaded;
		const bool was_theme = it->package.valid && it->package.manifest.type == PluginType::Theme;
		if (loaded_native) {
			it->removed = true;
			it->enabled = false;
			it->state = "Removed (restart to unload)";
		}
		else {
			std::error_code ec;
			std::filesystem::remove_all(it->package.root, ec);
			m_entries.erase(it);
		}
		RemoveTrust(id);
		SaveTrust();
		if (!loaded_native && was_theme) {
			g_plugin_theme_active = false;
			ThemeManager::Instance().ApplyDefaultTheme();
		}
		LogLine("[INFO] Removed plugin " + id);
		return;
	}
}

void PluginRegistry::ReloadDeclarative() {
	for (auto& entry : m_entries) {
		if (entry.removed || !entry.loaded || entry.IsNative())
			continue;
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

void PluginRegistry::InstallFromFile(const std::filesystem::path& file) {
	std::error_code ec;
	if (!std::filesystem::is_regular_file(file, ec)) {
		LogLine("[ERROR] Not a plugin file: " + file.string());
		return;
	}
	std::filesystem::create_directories(UserPluginsDir(), ec);
	const std::filesystem::path destination = UserPluginsDir() / file.filename();
	if (std::filesystem::exists(destination, ec)) {
		LogLine("[ERROR] A plugin with this filename already exists: " + destination.filename().string());
		return;
	}
	std::filesystem::copy_file(file, destination, ec);
	if (ec) {
		LogLine("[ERROR] Cannot install plugin: " + ec.message());
		return;
	}
	AddPackage(destination);
}

int PluginRegistry::FirstPendingIndex() const {
	for (size_t i = 0; i < m_entries.size(); ++i) {
		if (!m_entries[i].removed && m_entries[i].pendingTrust)
			return static_cast<int>(i);
	}
	return -1;
}

void PluginRegistry::RenderTrustModal() {
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

	if (m_openModalRequested) {
		ImGui::OpenPopup("Plugin Trust###plugin_trust");
		m_openModalRequested = false;
		m_modalActive = true;
	}

	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(520, 0), ImVec2(760, FLT_MAX));
	if (m_modalActive && ImGui::BeginPopupModal("Plugin Trust###plugin_trust", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
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
		ImGui::TextDisabled("%s  |  %s", manifest.id.c_str(), TypeName(manifest.type));
		if (!manifest.author.empty())
			ImGui::TextDisabled("By %s", manifest.author.c_str());
		if (!manifest.description.empty()) {
			ImGui::Spacing();
			ImGui::TextWrapped("%s", manifest.description.c_str());
		}

		ImGui::Spacing();
		ImGui::TextUnformatted("Package SHA-256");
		ImGui::TextDisabled("%s", entry.package.sha256.c_str());

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
			ImGui::PushStyleColor(ImGuiCol_Text, UIHelpers::kColorWarning);
			ImGui::TextWrapped("This plugin includes native code. It runs in the same process with the same access as the app; the permission list above is a declaration, not a sandbox.");
			ImGui::PopStyleColor();
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::TextDisabled("Only allow plugins you trust. Always Allow remembers this exact version and hash.");
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
		// The popup was dismissed without one of our buttons (for example with
		// Escape).  Clear the flag so the next pending plugin can prompt.
		m_modalActive = false;
	}

	ImGui::End();
	ImGui::PopStyleVar();
}

} // namespace casioemu::plugin
