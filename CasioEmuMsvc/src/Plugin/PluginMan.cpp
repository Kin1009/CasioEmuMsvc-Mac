#include "PluginApi.h"
#include "PluginMan.h"
#include "PluginPackage.h"
#include "PlatformTags.h"
#include "PluginRegistry.h"
#include <iostream>
#include <fstream>
#include <cctype>
#include <filesystem>
#include <vector>
#if defined(__APPLE__) || defined(__linux__)
#include <sys/stat.h>
#endif
#if defined(__APPLE__)
#include <SDL.h>
#endif

extern PluginApi* g_pluginapi;
std::string g_PluginLoadLog = "";
std::vector<PluginInfo> g_loadedPlugins;

// ---------------------------------------------------------------------------
// Architecture-provider registry (host-owned singleton).
// ---------------------------------------------------------------------------
namespace {
	class ArchitectureRegistryImpl : public casioemu::IArchitectureRegistry {
	public:
		bool RegisterProvider(casioemu::IArchitectureProvider* provider) override {
			if (!provider) return false;
			for (auto* existing : providers)
				if (existing == provider) return true;
			providers.push_back(provider);
			g_PluginLoadLog += "[ARCH] Registered: " +
				std::string(provider->GetName() ? provider->GetName() : "?") + "\n";
			return true;
		}
		casioemu::IArchitectureProvider* FindProvider(uint16_t hardware_id) override {
			for (auto* provider : providers) {
				size_t count = 0;
				const uint16_t* ids = provider->GetSupportedHardwareIds(&count);
				for (size_t i = 0; i < count; ++i)
					if (ids && ids[i] == hardware_id) return provider;
			}
			return nullptr;
		}
		size_t GetProviderCount() const override { return providers.size(); }
		casioemu::IArchitectureProvider* GetProvider(size_t index) const override {
			return index < providers.size() ? providers[index] : nullptr;
		}
	private:
		std::vector<casioemu::IArchitectureProvider*> providers;
	};
	ArchitectureRegistryImpl g_archRegistry;
}

casioemu::IArchitectureRegistry* GetArchitectureRegistry() {
	return &g_archRegistry;
}

#if defined(__APPLE__) || defined(__linux__)
namespace {
	// Only *.plugin packages are accepted by the architecture pass -- raw
	// *.dylib / *.so files are deliberately NOT discovered (the Plugin Manager
	// accepts .plugin packages only). A .plugin is either a plain Mach-O dylib
	// (legacy, still supported) or a ZIP container resolved by
	// ResolvePluginNativeModule().
	bool IsPluginPackageFile(const std::filesystem::directory_entry& entry) {
		std::error_code ec;
		if (!entry.is_regular_file(ec))
			return false;
		std::string ext = entry.path().extension().string();
		for (char& c : ext)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return ext == ".plugin";
	}

	std::vector<std::filesystem::path> FindArchPluginModules() {
		std::vector<std::filesystem::path> candidates;
		const char* envDir = std::getenv("CASIOEMU_PLUGINS_DIR");
		if (envDir && *envDir) {
			std::error_code ec;
			for (auto& e : std::filesystem::directory_iterator(envDir, ec)) {
				if (IsPluginPackageFile(e))
					candidates.push_back(e.path());
			}
		}
#if defined(__APPLE__)
		// A .app launched via Finder / `open -a` has cwd "/", so look next to
		// the executable and inside the bundle's Resources directory.
		if (char* exe_dir = SDL_GetBasePath()) {
			for (const char* rel : {"plugins", "../Resources/plugins"}) {
				std::error_code ec;
				std::filesystem::path p = std::filesystem::path(exe_dir) / rel;
				if (!std::filesystem::exists(p, ec)) continue;
				for (auto& e : std::filesystem::directory_iterator(p, ec)) {
					if (IsPluginPackageFile(e))
						candidates.push_back(e.path());
				}
			}
			SDL_free(exe_dir);
		}
#endif
		for (const char* base : {"plugins", "./plugins", "../Resources/plugins"}) {
			std::error_code ec;
			if (!std::filesystem::exists(base, ec)) continue;
			for (auto& e : std::filesystem::directory_iterator(base, ec)) {
				if (IsPluginPackageFile(e))
					candidates.push_back(e.path());
			}
		}
		std::vector<std::filesystem::path> unique;
		for (auto& p : candidates) {
			bool seen = false;
			for (auto& u : unique) if (u == p) { seen = true; break; }
			if (!seen) unique.push_back(p);
		}
		return unique;
	}

	bool HasZipMagic(const std::filesystem::path& file) {
		std::ifstream input(file, std::ios::binary);
		if (!input)
			return false;
		unsigned char magic[4]{};
		input.read(reinterpret_cast<char*>(magic), sizeof(magic));
		// PK\x03\x04 = normal archive, PK\x05\x06 = empty, PK\x07\x08 = spanned.
		return magic[0] == 'P' && magic[1] == 'K' &&
			((magic[2] == 3 && magic[3] == 4) ||
			 (magic[2] == 5 && magic[3] == 6) ||
			 (magic[2] == 7 && magic[3] == 8));
	}

	// miniz writes extracted files with the archive's stored mode (usually
	// 0644). dyld needs an executable mapping for Mach-O images, so add the
	// executable bits to every extracted file. Harmless for JSON assets.
	void MakeExtractedFilesExecutable(const std::filesystem::path& root) {
		std::error_code ec;
		if (!std::filesystem::is_directory(root, ec))
			return;
		for (auto& item : std::filesystem::recursive_directory_iterator(root, ec)) {
			if (ec)
				break;
			std::error_code file_ec;
			if (!item.is_regular_file(file_ec))
				continue;
			const std::string path = item.path().string();
			struct stat st{};
			if (::stat(path.c_str(), &st) == 0)
				::chmod(path.c_str(), static_cast<mode_t>(st.st_mode | 0111));
		}
	}

	std::filesystem::path FindEmbeddedNativeModule(const casioemu::plugin::Package& package) {
		// Prefer the manifest's native_entry, then fall back to any embedded
		// Mach-O / dylib so a container missing native_entry still loads.
		if (!package.manifest.nativeEntry.empty()) {
			const auto preferred = package.root / package.manifest.nativeEntry;
			std::error_code ec;
			if (std::filesystem::is_regular_file(preferred, ec))
				return preferred;
		}
		std::error_code ec;
		for (auto& item : std::filesystem::recursive_directory_iterator(package.root, ec)) {
			if (ec)
				break;
			std::error_code file_ec;
			if (!item.is_regular_file(file_ec))
				continue;
			const std::string ext = item.path().extension().string();
			if (ext == ".dylib" || ext == ".plugin" || ext == ".so")
				return item.path();
		}
		return {};
	}
}

std::vector<std::filesystem::path> ResolvePluginNativeModules(
	const std::filesystem::path& pluginFile,
	const std::filesystem::path& cacheDir,
	std::string* error) {
	const auto fail = [&](const std::string& message) -> std::vector<std::filesystem::path> {
		g_PluginLoadLog += "[ARCH] " + message + "\n";
		if (error)
			*error = message;
		return {};
	};

	std::error_code ec;
	if (!std::filesystem::is_regular_file(pluginFile, ec))
		return fail("not a plugin file: " + pluginFile.string());

	// Legacy/plain format: a bare Mach-O dylib named *.plugin. Load it in place
	// so the existing CG100SH4.plugin keeps working unchanged.
	if (!HasZipMagic(pluginFile))
		return { pluginFile };

	// ZIP container: metadata (plugin.json) + embedded native binaries.
	casioemu::plugin::Package package;
	if (!casioemu::plugin::ExtractPackage(pluginFile, cacheDir, package)) {
		return fail("cannot extract .plugin container " + pluginFile.filename().string() +
			": " + package.code + ": " + package.message);
	}

	const auto selection = casioemu::plugin::SelectPlatformBinariesForRoot(package.root);
	std::vector<std::filesystem::path> modules;
	if (selection.status == casioemu::plugin::PlatformBinarySelection::Status::Matched) {
		// Every binary whose <os>-<arch> matches this build is loaded.
		modules = selection.matched;
	}
	else if (selection.status == casioemu::plugin::PlatformBinarySelection::Status::NoMatch) {
		std::string available;
		for (const auto& key : selection.available) {
			if (!available.empty())
				available += ", ";
			available += key;
		}
		return fail(casioemu::plugin::NoCompatibleBinaryError() +
			(available.empty() ? std::string() : std::string(" (available: ") + available + ")") +
			" in " + pluginFile.filename().string());
	}
	else {
		// Legacy untagged ZIP: honour native_entry, then any embedded dylib.
		const std::filesystem::path native = FindEmbeddedNativeModule(package);
		if (native.empty())
			return fail(".plugin container has no native dylib: " + pluginFile.filename().string());
		modules.push_back(native);
	}

	MakeExtractedFilesExecutable(package.root);
	g_PluginLoadLog += "[ARCH] Extracted " + pluginFile.filename().string() +
		" -> " + package.root.string() + " (" + std::to_string(modules.size()) +
		" native module(s) for " + casioemu::plugin::CurrentPlatformKey() + ")\n";
	if (error)
		error->clear();
	return modules;
}

std::filesystem::path ResolvePluginNativeModule(
	const std::filesystem::path& pluginFile,
	const std::filesystem::path& cacheDir,
	std::string* error) {
	const auto modules = ResolvePluginNativeModules(pluginFile, cacheDir, error);
	return modules.empty() ? std::filesystem::path{} : modules.front();
}
#endif

#ifdef _WIN32
#include <Windows.h>
#include <string>

void LoadPlugins() {
  g_PluginLoadLog += "[INFO] Starting Windows Plugin Loader\n";
	// Load all plugins in the plugins directory
	WIN32_FIND_DATAA findData;
	HANDLE hFind = FindFirstFileA("CasioEmuMsvc.Plugin.*.dll", &findData);
	if (hFind == INVALID_HANDLE_VALUE) {
	  g_PluginLoadLog += "[INFO] No plugins found.\n";
		return;
	}
	do {
	  g_PluginLoadLog += "[INFO] Found: " + std::string(findData.cFileName) + "\n";
		HMODULE hModule = LoadLibraryA(findData.cFileName);
		if (hModule) {
			auto load = (PluginLoad)GetProcAddress(hModule, "fPluginLoad");
			if (load) {
				load(g_pluginapi);
				g_PluginLoadLog += "[SUCCESS] Loaded plugin: " + std::string(findData.cFileName) + "\n";
            } else {
                g_PluginLoadLog += "[ERROR] Missing fPluginLoad in " + std::string(findData.cFileName) + "\n";
            }
        } else {
            g_PluginLoadLog += "[ERROR] Failed to load " + std::string(findData.cFileName) + "\n";
        }
    } while (FindNextFileA(hFind, &findData));
    FindClose(hFind);
}
#elif defined(__ANDROID__)
#include <dlfcn.h>
#include <android/log.h>
#include <jni.h>
#include <SDL.h>
#include <string>

void LoadPlugins() {
    g_PluginLoadLog += "[INFO] Starting Android Plugin Loader\n";
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    if (!env) {
        g_PluginLoadLog += "[FATAL] Failed to get JNIEnv\n";
        return;
    }

    jclass gameClass = env->FindClass("com/tele/u8emulator/Game");
    if (gameClass) {
        g_PluginLoadLog += "[INFO] Calling Java checkAndExtractPluginAssets()\n";
        jmethodID checkPluginMethod = env->GetStaticMethodID(gameClass, "checkAndExtractPluginAssets", "()V");
        if (checkPluginMethod) {
            env->CallStaticVoidMethod(gameClass, checkPluginMethod);
        }
    }

    const char* pluginsDir = getenv("CASIOEMU_PLUGINS_DIR");
    if (pluginsDir) {
        g_PluginLoadLog += "[INFO] Loading plugins from: " + std::string(pluginsDir) + "\n";
        std::string orderFilePath = std::string(pluginsDir) + "/load_order.txt";
        std::ifstream orderFile(orderFilePath);
        
        if (orderFile.is_open()) {
            std::string libName;
            while (std::getline(orderFile, libName)) {
                if (libName.empty()) continue;
                std::string fullPath = std::string(pluginsDir) + "/" + libName;
                g_PluginLoadLog += "[INFO] Attempting to load: " + libName + "\n";
                void* hModule = dlopen(fullPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
                if (hModule) {
                    auto load = (PluginLoad)dlsym(hModule, "fPluginLoad");
                    if (load) {
                        load(g_pluginapi);
                        g_PluginLoadLog += "[SUCCESS] Loaded plugin: " + libName + "\n";
                    } else {
                        g_PluginLoadLog += "[INFO] Loaded dependency: " + libName + "\n";
                    }
                } else {
                    g_PluginLoadLog += "[ERROR] dlopen failed for " + libName + "\n";
                    g_PluginLoadLog += "Reason: " + std::string(dlerror()) + "\n";
                }
            }
        } else {
            g_PluginLoadLog += "[WARN] load_order.txt not found.\n";
        }
    }
}
#elif defined(__APPLE__) || defined(__linux__)
#include "PluginRegistry.h"

void LoadPlugins() {
	casioemu::plugin::PluginRegistry::Instance().Initialize();
}
#else
void LoadPlugins() {}
#endif

#if defined(__APPLE__) || defined(__linux__)
#include <dlfcn.h>
void LoadArchitecturePlugins() {
	g_PluginLoadLog += "[ARCH] Starting early architecture plugin pass\n";
	const std::filesystem::path cacheDir = casioemu::plugin::PluginRegistry::Instance().CacheDir();
	for (auto& path : FindArchPluginModules()) {
		std::string error;
		const auto modules = ResolvePluginNativeModules(path, cacheDir, &error);
		if (modules.empty()) {
			g_PluginLoadLog += "[ARCH] Skipped " + path.filename().string() +
				(error.empty() ? std::string(" (no native module)") : ": " + error) + "\n";
			continue;
		}
		// Load EVERY binary that matches the current OS+architecture.  A
		// package may ship several arch plugins for the same platform.
		for (const auto& module : modules) {
			const std::string p = module.string();
			void* h = dlopen(p.c_str(), RTLD_NOW | RTLD_GLOBAL);
			if (!h) {
				g_PluginLoadLog += "[ARCH] dlopen failed for " + p + ": " + std::string(dlerror()) + "\n";
				continue;
			}
			MarkArchitectureModuleLoaded(module);
			auto load = reinterpret_cast<PluginLoadArch>(dlsym(h, "fPluginLoadArch"));
			if (load) {
				load(g_pluginapi);
				g_PluginLoadLog += "[ARCH] Registered via " + p + "\n";
			}
			else {
				g_PluginLoadLog += "[ARCH] No fPluginLoadArch export in " + p + "\n";
			}
		}
	}
}
#else
void LoadArchitecturePlugins() {}
#endif