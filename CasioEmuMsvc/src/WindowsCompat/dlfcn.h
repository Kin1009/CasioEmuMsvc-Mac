#pragma once
// Win32 compatibility for the POSIX dlfcn.h API used by the plugin loader.
//
// MinGW-w64 does not ship dlfcn.h.  This header maps the small subset the
// emulator uses onto the Win32 module API:
//
//   dlopen()   -> LoadLibraryA()
//   dlsym()    -> GetProcAddress()
//   dlclose()  -> FreeLibrary()
//   dlerror()  -> per-thread string describing the last failure
//
// RTLD_DEFAULT is a sentinel handled by dlsym(): instead of only looking at
// the host EXE's export table (which is empty for a statically linked SDL),
// it searches every module opened through dlopen() with RTLD_GLOBAL as well.
// This reproduces the POSIX behaviour the macOS/Linux builds rely on for
// `dlsym(RTLD_DEFAULT, "casioemu_arch_core_*")` and the cg100_* C ABI.

#ifdef _WIN32

#include <windows.h>

#include <mutex>
#include <string>
#include <vector>

#define RTLD_LAZY   0x0001
#define RTLD_NOW    0x0002
#define RTLD_GLOBAL 0x0100
#define RTLD_LOCAL  0x0000

// Distinct non-module sentinel values.  RTLD_DEFAULT == nullptr keeps the
// "current process" meaning; RTLD_NEXT is accepted but has no Win32 analogue.
#define RTLD_DEFAULT ((void*)0)
#define RTLD_NEXT    ((void*)-1)

namespace casioemu_win_dl {

struct GlobalModules {
	std::mutex mutex;
	std::vector<HMODULE> modules;
	static GlobalModules& Instance() {
		static GlobalModules instance;
		return instance;
	}
	void Add(HMODULE module) {
		if (!module)
			return;
		std::lock_guard<std::mutex> lock(mutex);
		for (HMODULE existing : modules)
			if (existing == module)
				return;
		modules.push_back(module);
	}
	void Remove(HMODULE module) {
		std::lock_guard<std::mutex> lock(mutex);
		for (auto it = modules.begin(); it != modules.end(); ++it) {
			if (*it == module) {
				modules.erase(it);
				return;
			}
		}
	}
};

struct LastError {
	static std::string& Text() {
		static thread_local std::string text;
		return text;
	}
	static const char* Get() {
		return Text().empty() ? nullptr : Text().c_str();
	}
	static void Set(const std::string& text) {
		Text() = text;
	}
	static void Clear() {
		Text().clear();
	}
};

inline void FormatWin32Error(DWORD error, const char* what) {
	std::string text = what ? what : "Win32 error";
	if (error != 0) {
		char* message = nullptr;
		DWORD count = FormatMessageA(
			FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
				FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			reinterpret_cast<LPSTR>(&message), 0, nullptr);
		if (count && message) {
			text += ": ";
			text.append(message, count);
		}
		if (message)
			LocalFree(message);
	}
	LastError::Set(text);
}

inline HMODULE AsModule(void* handle) {
	return reinterpret_cast<HMODULE>(handle);
}

inline void* SearchGlobal(const char* name) {
	if (!name)
		return nullptr;
	// Current process first (matches the POSIX flat namespace).
	if (void* address = reinterpret_cast<void*>(
			GetProcAddress(GetModuleHandle(nullptr), name)))
		return address;
	// Then every module explicitly loaded with RTLD_GLOBAL.
	GlobalModules& globals = GlobalModules::Instance();
	std::lock_guard<std::mutex> lock(globals.mutex);
	for (HMODULE module : globals.modules) {
		if (void* address = reinterpret_cast<void*>(GetProcAddress(module, name)))
			return address;
	}
	return nullptr;
}

} // namespace casioemu_win_dl

inline void* dlopen(const char* path, int flags = RTLD_NOW) {
	casioemu_win_dl::LastError::Clear();
	if (!path)
		return reinterpret_cast<void*>(GetModuleHandle(nullptr));
	// Search the DLL's own directory for its dependencies first: .plugin
	// packages bundle cg100core.dll next to the plugin DLL under lib/.  Plain
	// LoadLibraryA() would only search the EXE directory / system dirs / PATH.
	HMODULE module = LoadLibraryExA(
		path, nullptr,
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (!module) {
		casioemu_win_dl::FormatWin32Error(GetLastError(), path);
		return nullptr;
	}
	if (flags & RTLD_GLOBAL)
		casioemu_win_dl::GlobalModules::Instance().Add(module);
	return reinterpret_cast<void*>(module);
}

inline void* dlsym(void* handle, const char* name) {
	casioemu_win_dl::LastError::Clear();
	if (handle == RTLD_DEFAULT)
		return casioemu_win_dl::SearchGlobal(name);
	if (handle == RTLD_NEXT)
		return nullptr; // not representable with the Win32 loader
	if (!handle)
		return casioemu_win_dl::SearchGlobal(name);
	FARPROC address = GetProcAddress(casioemu_win_dl::AsModule(handle), name);
	if (!address) {
		casioemu_win_dl::FormatWin32Error(GetLastError(), name);
		return nullptr;
	}
	return reinterpret_cast<void*>(address);
}

inline int dlclose(void* handle) {
	casioemu_win_dl::LastError::Clear();
	if (!handle || handle == RTLD_DEFAULT || handle == RTLD_NEXT)
		return 0;
	HMODULE module = casioemu_win_dl::AsModule(handle);
	casioemu_win_dl::GlobalModules::Instance().Remove(module);
	if (!FreeLibrary(module)) {
		casioemu_win_dl::FormatWin32Error(GetLastError(), "FreeLibrary");
		return 1;
	}
	return 0;
}

inline const char* dlerror(void) {
	return casioemu_win_dl::LastError::Get();
}

#endif // _WIN32
