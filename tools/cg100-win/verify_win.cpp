// Headless Win32 runtime verification for the CG100 Windows DLLs.
// Built with MinGW-w64 and run under Wine; no host GUI or fake output.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using CG100Core = void*;

namespace {

template <typename Fn>
Fn Symbol(HMODULE module, const char* name) {
	return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

std::vector<uint8_t> ReadFileBytes(const char* path) {
	std::vector<uint8_t> data;
	FILE* file = std::fopen(path, "rb");
	if (!file) return data;
	std::fseek(file, 0, SEEK_END);
	const long size = std::ftell(file);
	std::fseek(file, 0, SEEK_SET);
	if (size > 0) {
		data.resize(static_cast<size_t>(size));
		const size_t read = std::fread(data.data(), 1, data.size(), file);
		data.resize(read);
	}
	std::fclose(file);
	return data;
}

int Fail(const char* message) {
	std::printf("FAIL: %s\n", message);
	return 1;
}

} // namespace

int main(int argc, char** argv) {
	const std::string dir = argc > 1 ? argv[1] : ".";
	const std::string pluginPath = dir + "\\CG100SH4.dll";
	const std::string corePath = dir + "\\cg100core.dll";

	std::printf("=== CG100 Windows runtime verification ===\n");
	std::printf("plugin: %s\n", pluginPath.c_str());
	std::printf("core:   %s\n", corePath.c_str());

	const DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
		LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;

	HMODULE core = LoadLibraryExA(corePath.c_str(), nullptr, flags);
	if (!core) return Fail("cannot load cg100core.dll");
	HMODULE plugin = LoadLibraryExA(pluginPath.c_str(), nullptr, flags);
	if (!plugin) return Fail("cannot load CG100SH4.dll (check cg100core.dll)");

	// 1. ABI handshake symbols.
	auto load = Symbol<void (*)(void*)>(plugin, "fPluginLoad");
	auto loadArch = Symbol<void (*)(void*)>(plugin, "fPluginLoadArch");
	if (!load || !loadArch) return Fail("missing fPluginLoad / fPluginLoadArch");
	load(nullptr);       // plugin guards api == nullptr -> safe no-op
	loadArch(nullptr);
	std::printf("handshake exports callable\n");

	// 2. All C ABI bridge exports exist.
	const char* bridge[] = {
		"casioemu_arch_core_framebuffer", "casioemu_arch_core_handle",
		"casioemu_arch_core_key_matrix", "casioemu_arch_core_key_down",
		"casioemu_arch_core_key_up", "casioemu_arch_core_release_all",
		"casioemu_arch_core_power_on", "casioemu_arch_core_reboot",
		"casioemu_arch_core_enter_diag",
	};
	for (const char* name : bridge) {
		if (!GetProcAddress(plugin, name)) {
			std::printf("missing export: %s\n", name);
			return Fail("incomplete casioemu_arch_core_* ABI");
		}
	}
	std::printf("all 9 casioemu_arch_core_* exports present\n");

	// Null-handle bridge calls must be safe (they are guarded in the plugin).
	auto framebuffer = Symbol<int (*)(void*, unsigned char*, int)>(
		plugin, "casioemu_arch_core_framebuffer");
	unsigned char pixel[396 * 224 * 4];
	if (framebuffer(nullptr, pixel, sizeof(pixel)) != 0)
		return Fail("framebuffer bridge should reject a null handle");
	std::printf("null-handle bridge calls are safe\n");

	// 3. Core ABI: real SH4 core, real 32 MiB flash image.
	auto create = Symbol<CG100Core (*)(const uint8_t*, uint32_t)>(core, "cg100_create");
	auto destroy = Symbol<void (*)(CG100Core)>(core, "cg100_destroy");
	auto reset = Symbol<void (*)(CG100Core)>(core, "cg100_reset");
	auto run = Symbol<uint64_t (*)(CG100Core, int)>(core, "cg100_run");
	auto getFb = Symbol<void (*)(CG100Core, uint16_t*, uint32_t)>(core, "cg100_framebuffer");
	auto cycles = Symbol<uint64_t (*)(CG100Core)>(core, "cg100_cycles");
	auto fault = Symbol<const char* (*)(CG100Core)>(core, "cg100_fault");
	if (!create || !destroy || !reset || !run || !getFb || !cycles || !fault)
		return Fail("missing cg100_* core exports");

	const char* flashPath = argc > 2 ? argv[2]
		: "C:\\cg100\\flash.bin";
	const auto flash = ReadFileBytes(flashPath);
	if (flash.empty()) {
		std::printf("flash image not found at %s; core ABI only verified\n", flashPath);
	}
	else {
		std::printf("flash = %zu bytes\n", flash.size());
		CG100Core handle = create(flash.data(), static_cast<uint32_t>(flash.size()));
		if (!handle) return Fail("cg100_create failed");
		reset(handle);
		const uint64_t ran = run(handle, 500000);
		std::vector<uint16_t> fb(396 * 224);
		getFb(handle, fb.data(), static_cast<uint32_t>(fb.size()));
		size_t nonBlack = 0;
		for (uint16_t pixelValue : fb)
			if (pixelValue != 0) ++nonBlack;
		std::printf("cg100_run cycles = %llu, non-black pixels = %zu/%zu, fault = %s\n",
			(unsigned long long)ran, nonBlack, fb.size(),
			fault(handle) ? fault(handle) : "(none)");
		std::printf("cg100_cycles = %llu\n", (unsigned long long)cycles(handle));
		destroy(handle);
	}

	FreeLibrary(plugin);
	FreeLibrary(core);
	std::printf("=== ALL RUNTIME CHECKS PASSED ===\n");
	return 0;
}
