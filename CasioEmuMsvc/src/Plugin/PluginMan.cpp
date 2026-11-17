#include "PluginApi.h"
#include "PluginMan.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
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
	// Discover bare .plugin/.dylib/.so modules in CASIOEMU_PLUGINS_DIR and the
	// bundle/cwd plugin folders. Only the early architecture pass uses this list;
	// the new UI's PluginRegistry handles packaged plugins separately.
	std::vector<std::filesystem::path> FindArchPluginModules() {
		std::vector<std::filesystem::path> candidates;
		const char* envDir = std::getenv("CASIOEMU_PLUGINS_DIR");
		if (envDir && *envDir) {
			std::error_code ec;
			for (auto& e : std::filesystem::directory_iterator(envDir, ec)) {
				auto ext = e.path().extension().string();
				if (ext == ".plugin" || ext == ".dylib" || ext == ".so")
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
					auto ext = e.path().extension().string();
					if (ext == ".plugin" || ext == ".dylib" || ext == ".so")
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
				auto ext = e.path().extension().string();
				if (ext == ".plugin" || ext == ".dylib" || ext == ".so")
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
#elif defined(__APPLE__)
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
	for (auto& path : FindArchPluginModules()) {
		const std::string p = path.string();
		void* h = dlopen(p.c_str(), RTLD_NOW | RTLD_GLOBAL);
		if (!h) {
			g_PluginLoadLog += "[ARCH] dlopen failed: " + std::string(dlerror()) + "\n";
			continue;
		}
		auto load = reinterpret_cast<PluginLoadArch>(dlsym(h, "fPluginLoadArch"));
		if (load) {
			load(g_pluginapi);
			g_PluginLoadLog += "[ARCH] Registered via " + p + "\n";
		}
	}
}
#else
void LoadArchitecturePlugins() {}
#endif