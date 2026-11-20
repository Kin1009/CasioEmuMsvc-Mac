#pragma once
// CG100 / SH4 architecture-plugin debugger helpers (host side).
//
// The CG100 model is driven by an IEmulatorCore created by CG100SH4.plugin,
// NOT by the built-in nX-U16/U8 chipset.  The built-in MMU/RAM/ROM arrays are
// therefore empty for it.  These helpers expose only what the host can
// honestly display:
//
//   * core identity + status from the C++ IEmulatorCore interface;
//   * the live counters exported by the plugin's plain-C ABI, when the plugin
//     provides the optional `casioemu_arch_core_handle` accessor;
//   * the CG100 flash/FAT12 layout documented in MEMORY_FILESYSTEMS.md.
//
// Fields the plugin does not expose (SH4 registers, MMIO registers such as
// PFC/KEYSC/RTC/ETMU/flash controller) are explicitly labelled unavailable -
// we never invent values for them.
//
// Header-only on purpose: the desktop target globs Gui/*.cpp without
// CONFIGURE_DEPENDS, so adding a new translation unit would need a CMake
// reconfigure.  Everything below is inline.

#include "Ui.hpp"                       // ::m_emu, Emulator, IEmulatorCore
#include "ModelInfo.h"                  // HW_CG100
#include "Plugin/IArchitectureProvider.h"
#include "imgui/imgui.h"

#include <cstdint>
#include <cstdio>
#include <string>

#if defined(_WIN32)
# include <windows.h>
#else
# include <dlfcn.h>
#endif

namespace casioemu {
namespace cg100info {

// True only for a live CG100 session whose SH4 plugin core is attached.
inline bool IsActive() {
	return ::m_emu && ::m_emu->hardware_id == HW_CG100 && ::m_emu->arch_core.load() != nullptr;
}

inline IEmulatorCore* Core() {
	return IsActive() ? ::m_emu->arch_core.load() : nullptr;
}

inline void* Resolve(const char* name) {
#if defined(_WIN32)
	return reinterpret_cast<void*>(GetProcAddress(GetModuleHandle(nullptr), name));
#else
	return dlsym(RTLD_DEFAULT, name);
#endif
}

template <typename Fn>
inline Fn Symbol(const char* name) {
	return reinterpret_cast<Fn>(Resolve(name));
}

using HandleFn = void* (*)(void*);
using U64Fn = uint64_t (*)(void*);
using StrFn = const char* (*)(void*);
using IntFn = int (*)(void*);
using ListFn = int (*)(void*, char*, int);
using ReadRawFn = int (*)(void*, uint32_t, uint8_t*, int);

// The plugin is dlopen()ed with RTLD_GLOBAL.  Its optional C ABI accessor
// converts the host's IEmulatorCore* into the opaque CG100Core handle used by
// the cg100_* exports.  Older plugins did not export it; in that case all
// live-Counter fields are shown as unavailable (never fabricated).
inline void* Handle() {
	IEmulatorCore* core = Core();
	if (!core)
		return nullptr;
	static HandleFn handle_fn = Symbol<HandleFn>("casioemu_arch_core_handle");
	return handle_fn ? handle_fn(core) : nullptr;
}

struct Info {
	const char* core_id = nullptr;
	const char* status = nullptr;
	void* handle = nullptr;
	bool abi = false;       // handle accessor + cg100_* exports resolved
	uint64_t cycles = 0;
	uint64_t pushes = 0;
	const char* fault = nullptr;
	int usb_state = -1;
	const char* usb_text = nullptr;
	std::string files;
};

inline Info Read() {
	Info out{};
	if (IEmulatorCore* core = Core()) {
		out.core_id = core->GetId();
		out.status = core->GetStatusText();
	}
	out.handle = Handle();
	out.abi = out.handle != nullptr;
	if (out.abi) {
		static U64Fn cycles_fn = Symbol<U64Fn>("cg100_cycles");
		static U64Fn pushes_fn = Symbol<U64Fn>("cg100_pushes");
		static StrFn fault_fn = Symbol<StrFn>("cg100_fault");
		static IntFn usb_fn = Symbol<IntFn>("cg100_usb_status");
		static StrFn usb_text_fn = Symbol<StrFn>("cg100_usb_status_text");
		static ListFn list_fn = Symbol<ListFn>("cg100_usb_list");
		if (cycles_fn)
			out.cycles = cycles_fn(out.handle);
		if (pushes_fn)
			out.pushes = pushes_fn(out.handle);
		if (fault_fn)
			out.fault = fault_fn(out.handle);
		if (usb_fn)
			out.usb_state = usb_fn(out.handle);
		if (usb_text_fn)
			out.usb_text = usb_text_fn(out.handle);
		if (list_fn) {
			char buf[4096] = {};
			const int n = list_fn(out.handle, buf, static_cast<int>(sizeof(buf) - 1));
			if (n > 0) {
				buf[static_cast<size_t>(n) < sizeof(buf) - 1 ? static_cast<size_t>(n) : sizeof(buf) - 1] = '\0';
				out.files = buf;
			}
		}
	}
	return out;
}

inline bool ReadRaw(uint32_t addr, uint8_t* out, int n) {
	if (!Handle() || !out || n <= 0)
		return false;
	static ReadRawFn read_fn = Symbol<ReadRawFn>("cg100_usb_read_raw");
	return read_fn && read_fn(Handle(), addr, out, n) == n;
}

inline void Row(const char* label, const std::string& value) {
	ImGui::TextDisabled("%s", label);
	ImGui::SameLine(150.0f);
	ImGui::TextUnformatted(value.c_str());
}

inline std::string Hex(uint64_t value) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(value));
	return buf;
}

// Common SH4 core/status block shared by the Hardware, Variables and Memory
// tool pages.  `compact` omits the longer explanatory text for narrow panes.
inline void RenderCoreStatus(bool compact = false) {
	if (!IsActive())
		return;
	const Info info = Read();
	Row("Provider core", info.core_id ? info.core_id : "unavailable");
	Row("Core status", info.status ? info.status : "unavailable");
	Row("C ABI bridge", info.abi ? "live (cg100_* exports)" : "not exported by plugin build");
	Row("Cycles", info.abi ? Hex(info.cycles) : "unavailable");
	Row("LCD presents", info.abi ? std::to_string(info.pushes) : "unavailable");
	if (info.abi) {
		Row("Fault", info.fault && *info.fault ? info.fault : "none");
		Row("USB state", info.usb_text && *info.usb_text
			? std::string(std::to_string(info.usb_state)) + " - " + info.usb_text
			: std::to_string(info.usb_state));
	}
	else {
		Row("Fault", "unavailable");
		Row("USB state", "unavailable");
	}
	if (!compact) {
		ImGui::Spacing();
		ImGui::TextDisabled("SH4 registers (PC / SP / GBR / SR / MAC / PR):");
		ImGui::TextWrapped("No register-query API is exposed by IEmulatorCore; the plugin only "
			"provides identity, status and the counters above. Register/MMIO values are "
			"therefore not shown rather than guessed.");
	}
}

// Static CG100 flash map from MEMORY_FILESYSTEMS.md (no live values implied).
inline void RenderFlashMap() {
	ImGui::TextDisabled("Boot 0x00000000-0x0001FFFF  (128 KiB boot code)");
	ImGui::TextDisabled("OS   0x00020000-0x000B5FFF  (CASIOWIN / youkan 2.00)");
	ImGui::TextDisabled("Marker 0x00B20000 (4 B state record)");
	ImGui::TextDisabled("Fugue metadata 0x00C80000-0x00C81FFF");
	ImGui::TextDisabled("fls0 FAT12 volume 0x00C83A00 (4,764,208 B / 4.55 MiB)");
	ImGui::TextDisabled("Unused / erased from 0x00CCE000");
}

inline void RenderFat12Summary() {
	const Info info = Read();
	ImGui::TextDisabled("Label 'CASIO'  FAT12  512 B/sector  8 sectors/cluster (4 KiB)");
	ImGui::TextDisabled("2 FATs: 0x00C85000 / 0x00C85800   root 0x00C86000   data 0x00C8A000");
	ImGui::TextDisabled("Max data cluster 1157   media 0xF8   main memory = \\fls0\\@MainMem");
	if (info.abi && !info.files.empty()) {
		ImGui::Spacing();
		ImGui::TextUnformatted("Files on the emulated fls0 volume:");
		ImGui::TextUnformatted(info.files.c_str());
	}
	else if (info.abi) {
		ImGui::Spacing();
		ImGui::TextDisabled("Live file list empty: the emulated USB MSC is not connected in this session.");
	}
	else {
		ImGui::Spacing();
		ImGui::TextDisabled("Live file list unavailable (no C ABI bridge in this plugin build).");
	}
}

} // namespace cg100info
} // namespace casioemu
