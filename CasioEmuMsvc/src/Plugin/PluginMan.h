#pragma once
#include "IArchitectureProvider.h"
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

class PluginApi;  // forward declaration: keep this header safe to include from host TUs

// Global architecture-provider registry (owned by PluginMan.cpp).
casioemu::IArchitectureRegistry* GetArchitectureRegistry();

struct PluginInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string desc;
};

extern std::string g_PluginLoadLog;
extern std::vector<PluginInfo> g_loadedPlugins;
void LoadPlugins();

/// Early, architecture-only plugin pass. Loads plugin modules and calls their
/// OPTIONAL `fPluginLoadArch` export. Must run before Emulator construction.
/// Accepts ONLY *.plugin packages. A .plugin may be either a plain Mach-O dylib
/// (legacy format, still supported) or a ZIP container whose plugin.json
/// manifest points at an embedded native dylib; ZIP containers are extracted
/// into `cacheDir` before being loaded.
void LoadArchitecturePlugins();

/// Set of native modules dlopen'd by the early architecture pass. Declared
/// inline (C++17) so the function-local static is shared by every TU that
/// includes this header -- including the headless registry test, which does
/// not compile PluginMan.cpp. This lets the registry skip a native plugin that
/// the architecture pass already loaded, avoiding a double fPluginLoad.
inline std::vector<std::string>& ArchitectureLoadedModules() {
	static std::vector<std::string> modules;
	return modules;
}

inline std::string ArchitectureModuleKey(const std::filesystem::path& path) {
	std::error_code ec;
	auto canonical = std::filesystem::weakly_canonical(path, ec);
	if (ec)
		canonical = path;
	return canonical.string();
}

/// True when the given native module path was already dlopen'd by the early
/// architecture pass.
inline bool IsArchitectureModuleLoaded(const std::filesystem::path& path) {
	const std::string key = ArchitectureModuleKey(path);
	for (const std::string& loaded : ArchitectureLoadedModules())
		if (loaded == key)
			return true;
	return false;
}

inline void MarkArchitectureModuleLoaded(const std::filesystem::path& path) {
	const std::string key = ArchitectureModuleKey(path);
	if (!IsArchitectureModuleLoaded(path))
		ArchitectureLoadedModules().push_back(key);
}

#if defined(__APPLE__) || defined(__linux__)
/// Resolve a .plugin package to a loadable native Mach-O module.
///  - plain Mach-O .plugin -> returns `pluginFile` unchanged (backward compat)
///  - ZIP .plugin          -> extracts into `cacheDir` via the hardened
///                            ExtractPackage() and returns the embedded native
///                            entry (or an embedded *.dylib) from that cache.
/// Returns an empty path and fills `error` when the package cannot be resolved.
std::filesystem::path ResolvePluginNativeModule(
	const std::filesystem::path& pluginFile,
	const std::filesystem::path& cacheDir,
	std::string* error = nullptr);

/// Platform-aware variant.  For a ZIP .plugin containing
/// <name>-<os>-<arch>.<ext> binaries, returns ALL binaries matching the
/// current build (CASIOEMU_PLATFORM_OS / CASIOEMU_PLATFORM_ARCH).  A plain
/// Mach-O .plugin returns itself; a legacy untagged ZIP returns its
/// native_entry (or first embedded dylib).  Returns an empty vector and fills
/// `error` with "No native binary for <os>-<arch>" when a tagged package has
/// no compatible binary.
std::vector<std::filesystem::path> ResolvePluginNativeModules(
	const std::filesystem::path& pluginFile,
	const std::filesystem::path& cacheDir,
	std::string* error = nullptr);
#endif
