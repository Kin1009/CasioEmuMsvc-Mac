#include "PluginApi.h"
#include "PluginMan.h"
#include "PluginPackage.h"
#include "PluginRegistry.h"
#include <iostream>
#include <fstream>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <vector>
#if defined(__ANDROID__)
#include <SDL.h>  // SDL_AndroidGetExternalStoragePath() / JNI helpers
#endif
#if defined(__APPLE__) || defined(__linux__) || defined(__ANDROID__)
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

#if defined(__APPLE__) || defined(__linux__) || defined(__ANDROID__)
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

#if defined(__ANDROID__)
	// Android package naming: <stem>-android-arm64-android.so (32-bit ARM uses
	// the wildcard "any" tag). Keep in sync with PlatformTags.h on the host.
	std::string AndroidPlatformKey() {
#if defined(__aarch64__) || defined(_M_ARM64)
		return "android-arm64-android";
#elif defined(__x86_64__) || defined(_M_X64)
		return "android-x64";
#elif defined(__i386__) || defined(_M_IX86)
		return "android-x32";
#else
		return "android-any";
#endif
	}

	int AndroidModuleScore(const std::filesystem::path& path) {
		const std::string name = path.filename().string();
		const std::string key = AndroidPlatformKey();
		if (name.find(key) != std::string::npos)
			return 3;
		if (name.find("-android-any") != std::string::npos ||
			name.find("-any.so") != std::string::npos ||
			name.find("-android.so") != std::string::npos)
			return 2;
		return 1;
	}

	// Prefer an embedded binary carrying this device's platform tag.  A plain
	// "<stem>.so" remains usable as a last resort (legacy packages).
	std::filesystem::path FindAndroidEmbeddedNativeModule(const casioemu::plugin::Package& package) {
		std::error_code ec;
		std::filesystem::path best;
		int bestScore = -1;
		for (auto& item : std::filesystem::recursive_directory_iterator(package.root, ec)) {
			if (ec)
				break;
			std::error_code file_ec;
			if (!item.is_regular_file(file_ec))
				continue;
			if (item.path().extension().string() != ".so")
				continue;
			const int score = AndroidModuleScore(item.path());
			if (score > bestScore) {
				bestScore = score;
				best = item.path();
			}
		}
		// A manifest native_entry is authoritative when it points at a real
		// Android library, even if its file name carries no platform tag.
		if (!package.manifest.nativeEntry.empty()) {
			const auto preferred = package.root / package.manifest.nativeEntry;
			if (std::filesystem::is_regular_file(preferred, ec) &&
				preferred.extension().string() == ".so")
				return preferred;
		}
		return best;
	}

	std::vector<std::filesystem::path> AndroidPluginSearchDirs() {
		std::vector<std::filesystem::path> dirs;
		// Java sets these to the app-private files/cache directories.
		for (const char* var : {"CASIOEMU_PLUGIN_PACKAGES_DIR", "CASIOEMU_PLUGINS_DIR",
		                        "CASIOEMU_NATIVE_LIBRARY_DIR"}) {
			const char* value = std::getenv(var);
			if (value && *value)
				dirs.emplace_back(value);
		}
		if (const char* external = SDL_AndroidGetExternalStoragePath()) {
			dirs.emplace_back(std::filesystem::path(external) / "plugins");
			dirs.emplace_back(std::filesystem::path(external) / "models");
		}
		const char* home = std::getenv("HOME");
		if (home && *home)
			dirs.emplace_back(std::filesystem::path(home) / "plugins");
		dirs.emplace_back("plugins");
		std::vector<std::filesystem::path> unique;
		for (const auto& dir : dirs) {
			std::error_code ec;
			if (!std::filesystem::is_directory(dir, ec))
				continue;
			bool seen = false;
			for (const auto& used : unique)
				if (used == dir) { seen = true; break; }
			if (!seen)
				unique.push_back(dir);
		}
		return unique;
	}

	// Collect both ZIP packages (.plugin) and already-extracted Android shared
	// libraries (.so).  A load_order.txt from the legacy Java plugin bridge is
	// honoured as an extra source of plain .so names.
	std::vector<std::filesystem::path> FindAndroidPluginModules() {
		std::vector<std::filesystem::path> result;
		for (const auto& dir : AndroidPluginSearchDirs()) {
			std::error_code ec;
			const bool isNativeLibraryDir =
				(std::getenv("CASIOEMU_NATIVE_LIBRARY_DIR") &&
				 dir == std::filesystem::path(std::getenv("CASIOEMU_NATIVE_LIBRARY_DIR")));
			for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
				if (ec)
					break;
				const auto ext = entry.path().extension().string();
				if (ext != ".plugin" && ext != ".so")
					continue;
				// Gradle's nativeLibraryDir also holds SDL2, SDL2_image, etc.
				// Only pick up libraries explicitly named as plugins there.
				if (isNativeLibraryDir) {
					const std::string name = entry.path().filename().string();
					if (name.find("plugin") == std::string::npos &&
						name.find("sh4") == std::string::npos)
						continue;
				}
				result.push_back(entry.path());
			}
			std::ifstream order(dir / "load_order.txt");
			std::string line;
			while (std::getline(order, line)) {
				if (line.empty())
					continue;
				std::filesystem::path candidate = dir / line;
				if (std::filesystem::is_regular_file(candidate, ec))
					result.push_back(candidate);
			}
		}
		std::vector<std::filesystem::path> unique;
		for (const auto& path : result) {
			std::error_code ec;
			const auto key = std::filesystem::weakly_canonical(path, ec);
			bool seen = false;
			for (const auto& used : unique)
				if (used == key) { seen = true; break; }
			if (!seen)
				unique.push_back(key);
		}
		return unique;
	}

	std::filesystem::path AndroidPluginCacheDir() {
		const char* pluginsDir = std::getenv("CASIOEMU_PLUGINS_DIR");
		if (pluginsDir && *pluginsDir)
			return std::filesystem::path(pluginsDir) / ".cache";
		if (const char* external = SDL_AndroidGetExternalStoragePath())
			return std::filesystem::path(external) / ".plugin-cache";
		return ".plugin-cache";
	}
#endif // __ANDROID__
}

std::filesystem::path ResolvePluginNativeModule(
	const std::filesystem::path& pluginFile,
	const std::filesystem::path& cacheDir,
	std::string* error) {
	const auto fail = [&](const std::string& message) -> std::filesystem::path {
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
		return pluginFile;

	// ZIP container: metadata (plugin.json) + embedded dylib(s).
	casioemu::plugin::Package package;
	if (!casioemu::plugin::ExtractPackage(pluginFile, cacheDir, package)) {
		return fail("cannot extract .plugin container " + pluginFile.filename().string() +
			": " + package.code + ": " + package.message);
	}

#if defined(__ANDROID__)
	const std::filesystem::path native = FindAndroidEmbeddedNativeModule(package);
#else
	const std::filesystem::path native = FindEmbeddedNativeModule(package);
#endif
	if (native.empty())
		return fail(".plugin container has no native library: " + pluginFile.filename().string());

	MakeExtractedFilesExecutable(package.root);
	g_PluginLoadLog += "[ARCH] Extracted " + pluginFile.filename().string() +
		" -> " + package.root.string() + "\n";
	if (error)
		error->clear();
	return native;
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

namespace {
	// The Java activity stores its app-private paths in static fields instead
	// of using android.system.Os.setenv() (a hidden platform API).  Publish
	// them into the native process environment here.
	std::string AndroidJavaPath(const char* method) {
		JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
		if (!env)
			return {};
		jclass cls = env->FindClass("com/tele/u8emulator/Game");
		if (!cls) {
			// A native thread without a Java frame may not find the app class
			// by name; the SDL activity's class is an equivalent fallback.
			jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
			if (activity)
				cls = env->GetObjectClass(activity);
		}
		if (!cls)
			return {};
		jmethodID id = env->GetStaticMethodID(cls, method, "()Ljava/lang/String;");
		if (!id) {
			env->DeleteLocalRef(cls);
			return {};
		}
		std::string result;
		jstring value = static_cast<jstring>(env->CallStaticObjectMethod(cls, id));
		if (value) {
			const char* chars = env->GetStringUTFChars(value, nullptr);
			if (chars) {
				result = chars;
				env->ReleaseStringUTFChars(value, chars);
			}
			env->DeleteLocalRef(value);
		}
		env->DeleteLocalRef(cls);
		return result;
	}

	void PublishAndroidEnvironment() {
		struct Entry { const char* method; const char* variable; };
		static const Entry entries[] = {
			{"getCacheDirPath", "TMPDIR"},
			{"getLegacyPluginsDir", "CASIOEMU_PLUGINS_DIR"},
			{"getPluginPackagesDir", "CASIOEMU_PLUGIN_PACKAGES_DIR"},
			{"getNativeLibraryDirPath", "CASIOEMU_NATIVE_LIBRARY_DIR"},
		};
		for (const auto& entry : entries) {
			const std::string value = AndroidJavaPath(entry.method);
			if (!value.empty())
				::setenv(entry.variable, value.c_str(), 1);
		}
	}

	// One dlopen handle per native module, shared by the early architecture
	// pass and the normal plugin pass.  Calling either entry point twice would
	// register a provider/plugin twice, so track both flags explicitly.
	struct AndroidModuleState {
		void* handle = nullptr;
		bool archLoaded = false;
		bool normalLoaded = false;
	};

	std::map<std::string, AndroidModuleState>& AndroidModules() {
		static std::map<std::string, AndroidModuleState> modules;
		return modules;
	}

	void LoadAndroidPluginModules(bool early) {
		const auto cacheDir = AndroidPluginCacheDir();
		std::error_code ec;
		std::filesystem::create_directories(cacheDir, ec);

		for (const auto& package : FindAndroidPluginModules()) {
			std::string error;
			const auto module = ResolvePluginNativeModule(package, cacheDir, &error);
			if (module.empty()) {
				g_PluginLoadLog += "[ANDROID] Skipped " + package.filename().string() +
					(error.empty() ? std::string(" (no native library)") : ": " + error) + "\n";
				continue;
			}
			const std::string key = ArchitectureModuleKey(module);
			auto& state = AndroidModules()[key];
			if (!state.handle) {
				const std::string path = module.string();
				state.handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
				if (!state.handle) {
					g_PluginLoadLog += "[ANDROID] dlopen failed for " + path + ": " +
						std::string(dlerror() ? dlerror() : "unknown error") + "\n";
					continue;
				}
				__android_log_print(ANDROID_LOG_INFO, "CasioEmuPlugin", "loaded %s", path.c_str());
			}

			if (early) {
				if (state.archLoaded)
					continue;
				auto load = reinterpret_cast<PluginLoadArch>(dlsym(state.handle, "fPluginLoadArch"));
				if (load) {
					state.archLoaded = true;
					MarkArchitectureModuleLoaded(module);
					load(g_pluginapi);
					g_PluginLoadLog += "[ARCH] Registered via " + module.string() + "\n";
				}
			}
			else {
				if (state.normalLoaded)
					continue;
				auto load = reinterpret_cast<PluginLoad>(dlsym(state.handle, "fPluginLoad"));
				if (load) {
					state.normalLoaded = true;
					load(g_pluginapi);
					g_PluginLoadLog += "[SUCCESS] Loaded plugin: " + module.filename().string() + "\n";
				}
				else {
					g_PluginLoadLog += "[INFO] Loaded dependency: " + module.filename().string() + "\n";
				}
			}
		}
	}
}

void LoadPlugins() {
	g_PluginLoadLog += "[INFO] Starting Android Plugin Loader\n";
	PublishAndroidEnvironment();
	JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
	if (!env) {
		g_PluginLoadLog += "[FATAL] Failed to get JNIEnv\n";
		return;
	}

	// The Java activity extracts packaged .plugin ZIPs and plugin APK libraries
	// into app-private storage; their paths were published above through JNI.
	jclass gameClass = env->FindClass("com/tele/u8emulator/Game");
	if (gameClass) {
		jmethodID method = env->GetStaticMethodID(gameClass, "checkAndExtractPluginAssets", "()V");
		if (method)
			env->CallStaticVoidMethod(gameClass, method);
		env->DeleteLocalRef(gameClass);
	}
	LoadAndroidPluginModules(false);
}

void LoadArchitecturePlugins() {
	g_PluginLoadLog += "[ARCH] Starting early architecture plugin pass (Android)\n";
	PublishAndroidEnvironment();
	LoadAndroidPluginModules(true);
}
#elif defined(__APPLE__)
#include "PluginRegistry.h"

void LoadPlugins() {
	casioemu::plugin::PluginRegistry::Instance().Initialize();
}
#else
void LoadPlugins() {}
#endif

#if (defined(__APPLE__) || defined(__linux__)) && !defined(__ANDROID__)
#include <dlfcn.h>
void LoadArchitecturePlugins() {
	g_PluginLoadLog += "[ARCH] Starting early architecture plugin pass\n";
	const std::filesystem::path cacheDir = casioemu::plugin::PluginRegistry::Instance().CacheDir();
	for (auto& path : FindArchPluginModules()) {
		std::string error;
		const std::filesystem::path module = ResolvePluginNativeModule(path, cacheDir, &error);
		if (module.empty()) {
			g_PluginLoadLog += "[ARCH] Skipped " + path.filename().string() +
				(error.empty() ? std::string(" (no native module)") : ": " + error) + "\n";
			continue;
		}
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
#elif !defined(__ANDROID__)
void LoadArchitecturePlugins() {}
#endif