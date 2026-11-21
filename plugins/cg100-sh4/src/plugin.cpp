// plugin.cpp - ABI layer for the native C++ SH4 / fx-CG100 architecture plugin.
//
// Exports (C linkage):
//   fPluginLoadArch(PluginApi*)  - early pass: register the architecture provider
//   fPluginLoad(PluginApi*)      - normal pass: register plugin + provider
//   fPluginApiVersion / fPluginAbiSignature - ABI handshake
//   casioemu_arch_core_*         - optional casioctl/debugger bridge
//
// Unlike the reference plugin this module has NO Go / dylib dependency: the
// SH4 CPU and peripherals are compiled directly into this shared library.

#include "PluginApi.h"
// The real interpreter lives in the repo root (sh4_interp.cpp / cg100_mmio.cpp).
// native_core.h adapts it to the cg100::Bus / cg100::Sh4Cpu names used below.
#include "native_core.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  define SH4_EXPORT __declspec(dllexport)
#  define SH4_CLASS
#else
#  include <dlfcn.h>
#  define SH4_EXPORT __attribute__((visibility("default")))
#  define SH4_CLASS __attribute__((visibility("default")))
#endif

namespace {

constexpr int FB_W = 396;
constexpr int FB_H = 224;
constexpr uint64_t kCyclesPerSlice = 500000ull;
constexpr uint64_t kIps = 200000000ull; // host HW_CG100 budget; keeps KEYSC/RTC pacing in sync
constexpr char kStateMagic[8] = {'C','G','1','S','0','0','0','1'};

std::vector<uint8_t> ReadFile(const std::string& path) {
	std::vector<uint8_t> data;
	std::ifstream file(path, std::ios::binary);
	if (!file) return data;
	file.seekg(0, std::ios::end);
	const std::streamoff n = file.tellg();
	if (n <= 0) return data;
	file.seekg(0, std::ios::beg);
	data.resize(static_cast<size_t>(n));
	file.read(reinterpret_cast<char*>(data.data()), n);
	return data;
}

// SDL entry points resolve from the host process (SDL is linked into the host).
// This keeps SDL out of this plugin's link-time dependencies.
struct SdlBridge {
	using CreateTextureFn = void* (*)(void*, uint32_t, int, int, int);
	using UpdateTextureFn = int (*)(void*, const void*, const void*, int);
	using DestroyTextureFn = void (*)(void*);
	using RenderCopyFn = int (*)(void*, void*, const void*, const void*);

	CreateTextureFn createTexture{nullptr};
	UpdateTextureFn updateTexture{nullptr};
	DestroyTextureFn destroyTexture{nullptr};
	RenderCopyFn renderCopy{nullptr};

	static SdlBridge& Instance() {
		static SdlBridge bridge;
		return bridge;
	}

	SdlBridge() {
#if defined(_WIN32)
		HMODULE sdl = LoadLibraryA("SDL2.dll");
		if (!sdl) return;
		createTexture  = reinterpret_cast<CreateTextureFn>(GetProcAddress(sdl, "SDL_CreateTexture"));
		updateTexture  = reinterpret_cast<UpdateTextureFn>(GetProcAddress(sdl, "SDL_UpdateTexture"));
		destroyTexture = reinterpret_cast<DestroyTextureFn>(GetProcAddress(sdl, "SDL_DestroyTexture"));
		renderCopy     = reinterpret_cast<RenderCopyFn>(GetProcAddress(sdl, "SDL_RenderCopy"));
#else
		createTexture  = reinterpret_cast<CreateTextureFn>(dlsym(RTLD_DEFAULT, "SDL_CreateTexture"));
		updateTexture  = reinterpret_cast<UpdateTextureFn>(dlsym(RTLD_DEFAULT, "SDL_UpdateTexture"));
		destroyTexture = reinterpret_cast<DestroyTextureFn>(dlsym(RTLD_DEFAULT, "SDL_DestroyTexture"));
		renderCopy     = reinterpret_cast<RenderCopyFn>(dlsym(RTLD_DEFAULT, "SDL_RenderCopy"));
#endif
	}

	bool ok() const { return createTexture && updateTexture && destroyTexture && renderCopy; }
};

// SDL_PIXELFORMAT_RGBA32 / SDL_TEXTUREACCESS_STREAMING as numeric constants so
// this plugin needs no SDL headers at build time. Verified against the host's
// SDL_pixels.h: on little-endian SDL_PIXELFORMAT_RGBA32 == ABGR8888 (0x16762004).
#if defined(_WIN32) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
constexpr uint32_t kSdlPixelFormatRgba32 = 0x16762004u;
#else
constexpr uint32_t kSdlPixelFormatRgba32 = 0x16462004u;
#endif
constexpr int kSdlTextureAccessStreaming = 1;

} // namespace (helpers)

// The implementation classes live at global scope (with default visibility)
// so their vtables -- the IArchitectureProvider / IEmulatorCore ABI surface --
// are emitted as exportable symbols, not private anonymous-namespace symbols.

// ---- The core --------------------------------------------------------------
class SH4_CLASS Sh4Core : public casioemu::IEmulatorCore {
public:
	// GetId is the key function (defined out-of-line) so the vtable is emitted
	// as an external symbol in this translation unit.
	const char* GetId() const override;

	bool Initialize(const casioemu::ArchInitInfo& info) override {
		m_lastError.clear();
		std::string flashPath;
		if (info.flash_path && *info.flash_path &&
		    std::ifstream(info.flash_path, std::ios::binary).good()) {
			flashPath = info.flash_path;
		} else if (const char* env = std::getenv("CG100_FLASH"); env && *env) {
			flashPath = env;
		} else {
			m_lastError = "No flash image supplied (ArchInitInfo::flash_path / CG100_FLASH)";
			return false;
		}

		m_flash = ReadFile(flashPath);
		if (m_flash.empty()) {
			m_lastError = "Cannot read flash image: " + flashPath;
			return false;
		}
		if (!m_bus.loadFlash(m_flash.data(), m_flash.size())) {
			m_lastError = "Flash image too large for a 32 MiB NOR window";
			return false;
		}
		m_flashPath = flashPath;
		m_renderer = info.renderer;
		m_lcdX = info.lcd_x;
		m_lcdY = info.lcd_y;
		m_lcdW = info.lcd_w;
		m_lcdH = info.lcd_h;
		m_status = "initialized";
		return true;
	}

	void Reset() override {
		DestroyTexture();
		m_cpu.reset();
		m_cpu.setIps(kIps);
		m_bus.keysc().releaseAll();
		m_bus.keysc().setScanPeriod(22000000ull); // ~33 Hz at 200 MIPS
		m_rgba.assign(static_cast<size_t>(FB_W) * FB_H * 4, 0);
		m_status = "reset (cold)";
		// A verified save-state can be dropped in to skip the first-run wizard.
		if (const char* statePath = std::getenv("CG100_STATE"); statePath && *statePath) {
			const auto blob = ReadFile(statePath);
			if (!blob.empty() && LoadState(blob.data(), blob.size())) {
				m_status = "reset (loaded save-state)";
				RefreshFramebuffer();
			}
		}
	}

	void Tick() override {
		m_cpu.run(kCyclesPerSlice);
		RefreshFramebuffer();
	}

	void RenderFrame(const casioemu::ArchFrameInfo& info) override {
		if (!SdlBridge::Instance().ok()) return;
		void* renderer = info.renderer ? info.renderer : m_renderer;
		if (!renderer) return;
		EnsureTexture(renderer);
		if (!m_texture) return;
		SdlBridge::Instance().updateTexture(m_texture, nullptr, m_rgba.data(), FB_W * 4);

		struct Rect { int x, y, w, h; };
		int dstW = info.dst_w > 0 ? info.dst_w : m_lcdW;
		int dstH = info.dst_h > 0 ? info.dst_h : m_lcdH;
		if (dstW <= 0) dstW = FB_W;
		if (dstH <= 0) dstH = FB_H;
		const float scale = std::min(static_cast<float>(dstW) / FB_W,
		                             static_cast<float>(dstH) / FB_H);
		Rect dst{};
		dst.w = static_cast<int>(FB_W * scale);
		dst.h = static_cast<int>(FB_H * scale);
		dst.x = info.dst_x + (dstW - dst.w) / 2;
		dst.y = info.dst_y + (dstH - dst.h) / 2;
		SdlBridge::Instance().renderCopy(renderer, m_texture, nullptr, &dst);
	}

	// Keyboard keys are HELD: Key(down/up) maps to KEYSC press/release so
	// modifier combos (SHIFT+digit, ON+x10^x+AC, ...) coexist in one scan.
	void Key(casioemu::ArchKey key, bool pressed) override {
		int row = -1, col = -1;
		switch (key) {
		case casioemu::ArchKey::Up:    row = 3; col = 9; break;
		case casioemu::ArchKey::Down:  row = 3; col = 7; break;
		case casioemu::ArchKey::Left:  row = 4; col = 8; break;
		case casioemu::ArchKey::Right: row = 2; col = 8; break;
		case casioemu::ArchKey::EXE:   row = 2; col = 1; break;
		case casioemu::ArchKey::EXIT:  row = 5; col = 8; break;
		case casioemu::ArchKey::F1:    row = 6; col = 9; break;
		case casioemu::ArchKey::F2:    row = 5; col = 9; break;
		case casioemu::ArchKey::F3:    row = 4; col = 9; break;
		case casioemu::ArchKey::F4:    row = 3; col = 9; break;
		case casioemu::ArchKey::F5:    row = 2; col = 9; break;
		case casioemu::ArchKey::F6:    row = 1; col = 9; break;
		case casioemu::ArchKey::Shift: row = 6; col = 7; break;
		case casioemu::ArchKey::Alpha: row = 5; col = 7; break;
		case casioemu::ArchKey::Home:  row = 5; col = 9; break;
		case casioemu::ArchKey::ON:    row = 6; col = 9; break;
		case casioemu::ArchKey::AC:    row = 0; col = 0; break;
		case casioemu::ArchKey::X10X:  row = 4; col = 1; break;
		case casioemu::ArchKey::D0: row = 6; col = 1; break;
		case casioemu::ArchKey::D1: row = 6; col = 2; break;
		case casioemu::ArchKey::D2: row = 5; col = 2; break;
		case casioemu::ArchKey::D3: row = 4; col = 2; break;
		case casioemu::ArchKey::D4: row = 6; col = 3; break;
		case casioemu::ArchKey::D5: row = 5; col = 3; break;
		case casioemu::ArchKey::D6: row = 4; col = 3; break;
		case casioemu::ArchKey::D7: row = 6; col = 4; break;
		case casioemu::ArchKey::D8: row = 5; col = 4; break;
		case casioemu::ArchKey::D9: row = 4; col = 4; break;
		case casioemu::ArchKey::Dot:      row = 5; col = 1; break;
		case casioemu::ArchKey::Plus:     row = 3; col = 2; break;
		case casioemu::ArchKey::Minus:    row = 2; col = 2; break;
		case casioemu::ArchKey::Multiply: row = 3; col = 3; break;
		case casioemu::ArchKey::Divide:   row = 2; col = 3; break;
		default: break;
		}
		if (row < 0 || col < 0) return;
		if (pressed) m_bus.keysc().press(static_cast<uint32_t>(row), static_cast<uint32_t>(col));
		else         m_bus.keysc().release(static_cast<uint32_t>(row), static_cast<uint32_t>(col));
	}

	void ReleaseAll() override { m_bus.keysc().releaseAll(); }
	const char* GetStatusText() const override { return m_status.c_str(); }

	// ---- bridge helpers (not part of IEmulatorCore) ------------------------
	void KeyMatrix(int row, int col) {
		m_bus.keysc().press(static_cast<uint32_t>(row), static_cast<uint32_t>(col));
		m_cpu.run(40000ull); // short tap so the OS scan sees it
		m_bus.keysc().release(static_cast<uint32_t>(row), static_cast<uint32_t>(col));
		RefreshFramebuffer();
	}

	const uint8_t* Rgba(int* w, int* h, int* stride) const {
		if (w) *w = FB_W;
		if (h) *h = FB_H;
		if (stride) *stride = FB_W * 4;
		return m_rgba.empty() ? nullptr : m_rgba.data();
	}

	bool SaveState(std::vector<uint8_t>& out) const {
		out.assign(kStateMagic, kStateMagic + 8);
		const uint32_t version = 1;
		const uint32_t cpuBytes = static_cast<uint32_t>(sizeof(cg100::Sh4State));
		const uint32_t headerBytes = 8 + 4 + 4 + 4;
		const uint32_t busBytes = static_cast<uint32_t>(m_busBlobSizeHint());
		out.reserve(headerBytes + cpuBytes + busBytes + 64);

		appendPod(out, version);
		appendPod(out, cpuBytes);
		appendPod(out, busBytes); // hint only; patched after serialization
		const uint32_t cpuOffset = static_cast<uint32_t>(out.size());
		const cg100::Sh4State snapshot = m_cpu.state();
		out.insert(out.end(),
		            reinterpret_cast<const uint8_t*>(&snapshot),
		            reinterpret_cast<const uint8_t*>(&snapshot) + sizeof(snapshot));
		const uint32_t busOffset = static_cast<uint32_t>(out.size());
		m_bus.serialize(out);
		const uint32_t total = static_cast<uint32_t>(out.size());
		std::memcpy(out.data() + 16, &total, sizeof(total)); // patch total size
		(void)cpuOffset; (void)busOffset;
		return true;
	}

	bool LoadState(const uint8_t* data, size_t len) {
		if (!data || len < 20 || std::memcmp(data, kStateMagic, 8) != 0) return false;
		size_t off = 8;
		uint32_t version = 0, cpuBytes = 0, total = 0;
		if (!takePod(data, len, off, version)) return false;
		if (!takePod(data, len, off, cpuBytes)) return false;
		if (!takePod(data, len, off, total)) return false;
		if (version != 1 || cpuBytes != sizeof(cg100::Sh4State) || total > len) return false;
		cg100::Sh4State snapshot{};
		if (off > len || len - off < sizeof(snapshot)) return false;
		std::memcpy(&snapshot, data + off, sizeof(snapshot));
		off += sizeof(snapshot);
		if (!m_bus.deserialize(data + off, len - off)) return false;
		m_cpu.setState(snapshot);
		RefreshFramebuffer();
		return true;
	}

	const std::string& Error() const { return m_lastError; }
	const std::string& FlashPath() const { return m_flashPath; }
	cg100::Sh4Cpu& Cpu() { return m_cpu; }
	cg100::Bus& Bus() { return m_bus; }

private:
	template <class T>
	static void appendPod(std::vector<uint8_t>& out, const T& value) {
		const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
		out.insert(out.end(), p, p + sizeof(T));
	}
	template <class T>
	static bool takePod(const uint8_t* data, size_t len, size_t& off, T& value) {
		if (off > len || len - off < sizeof(T)) return false;
		std::memcpy(&value, data + off, sizeof(T));
		off += sizeof(T);
		return true;
	}

	// Rough size hint so SaveState can reserve once. Overestimates slightly.
	size_t m_busBlobSizeHint() const {
		return m_bus.flashSize() + 8u * 1024u * 1024u + 2u * 1024u * 1024u +
		       396u * 224u * 2u + 256u * 1024u;
	}

	void RefreshFramebuffer() {
		const uint16_t* be = m_bus.framebuffer();
		if (!be) return;
		if (m_rgba.size() != static_cast<size_t>(FB_W) * FB_H * 4)
			m_rgba.resize(static_cast<size_t>(FB_W) * FB_H * 4);
		for (int i = 0; i < FB_W * FB_H; ++i) {
			const uint16_t p = static_cast<uint16_t>((be[i] << 8) | (be[i] >> 8)); // BE -> host
			const uint8_t r = static_cast<uint8_t>(((p >> 11) & 0x1Fu) * 255 / 31);
			const uint8_t g = static_cast<uint8_t>(((p >> 5) & 0x3Fu) * 255 / 63);
			const uint8_t b = static_cast<uint8_t>((p & 0x1Fu) * 255 / 31);
			m_rgba[i * 4 + 0] = r;
			m_rgba[i * 4 + 1] = g;
			m_rgba[i * 4 + 2] = b;
			m_rgba[i * 4 + 3] = 255;
		}
	}

	void EnsureTexture(void* renderer) {
		if (!SdlBridge::Instance().ok() || !renderer) return;
		if (m_texture && m_texW == FB_W && m_texH == FB_H) return;
		DestroyTexture();
		m_texture = SdlBridge::Instance().createTexture(
			renderer, kSdlPixelFormatRgba32, kSdlTextureAccessStreaming, FB_W, FB_H);
		m_texW = FB_W;
		m_texH = FB_H;
	}

	void DestroyTexture() {
		if (m_texture && SdlBridge::Instance().destroyTexture)
			SdlBridge::Instance().destroyTexture(m_texture);
		m_texture = nullptr;
	}

	cg100::Bus m_bus;
	cg100::Sh4Cpu m_cpu{m_bus};
	void* m_renderer{nullptr};
	void* m_texture{nullptr};
	int m_texW{0}, m_texH{0};
	int m_lcdX{0}, m_lcdY{0}, m_lcdW{0}, m_lcdH{0};
	std::vector<uint8_t> m_flash;
	std::vector<uint8_t> m_rgba;
	std::string m_flashPath;
	std::string m_lastError;
	std::string m_status{"idle"};
};

// ---- Provider --------------------------------------------------------------
class SH4_CLASS Sh4Provider : public casioemu::IArchitectureProvider {
public:
	const char* GetId() const override;
	const char* GetName() const override { return "SH4 Architecture (Native C++)"; }
	uint32_t GetVersion() const override { return 1; }

	const uint16_t* GetSupportedHardwareIds(size_t* count) const override {
		// HW_CG100 == 14 in this host tree (ModelInfo.h). Earlier forks used 10;
		// registering 10 here would hijack EPS6009, so 14 is the only safe id.
		static const uint16_t ids[] = {14};
		if (count) *count = 1;
		return ids;
	}

	casioemu::IEmulatorCore* CreateCore() override { return new Sh4Core(); }
	void DestroyCore(casioemu::IEmulatorCore* core) override { delete core; }
};

const char* Sh4Core::GetId() const { return "sh4.native.cg100.core"; }
const char* Sh4Provider::GetId() const { return "sh4.native.cg100"; }

Sh4Provider g_provider;
PluginApi* g_api = nullptr;

// ---- ABI handshake ---------------------------------------------------------
#if defined(_WIN32)
#  define SH4_PLUGIN_EXPORT_ABI() \
	extern "C" SH4_EXPORT int fPluginApiVersion() { return PLUGIN_API_VERSION; } \
	extern "C" SH4_EXPORT unsigned long fPluginAbiSignature() { return kPluginUiWindowAbiSignature; }
#else
#  define SH4_PLUGIN_EXPORT_ABI() PLUGIN_EXPORT_ABI()
#endif
SH4_PLUGIN_EXPORT_ABI();

// ---- Entry points ----------------------------------------------------------
extern "C" SH4_EXPORT void fPluginLoadArch(PluginApi* api) {
	if (!api) return;
	api->RegisterArchitectureProvider(&g_provider);
}

extern "C" SH4_EXPORT void fPluginLoad(PluginApi* api) {
	if (!api) return;
	g_api = api;
	PLUGINASSERTSTL(api);
	api->RegisterPlugin("sh4native", "SH4 Architecture (Native C++)", "1.0",
	                    "CasioEmuMsvc", "Self-contained native SH4 core for fx-CG100 OS 2.00");
	api->RegisterArchitectureProvider(&g_provider);
}

// ---- Optional casioctl / debugger bridge -----------------------------------
// These mirror the export names of the reference plugin so existing tooling
// keeps working, but are implemented entirely by the native core.
extern "C" SH4_EXPORT void casioemu_arch_core_key_matrix(void* core, int row, int col) {
	static_cast<Sh4Core*>(core)->KeyMatrix(row, col);
}

extern "C" SH4_EXPORT void casioemu_arch_core_key_down(void* core, int row, int col) {
	if (!core || row < 0 || col < 0) return;
	static_cast<Sh4Core*>(core)->Bus().keysc().press(static_cast<uint32_t>(row), static_cast<uint32_t>(col));
}

extern "C" SH4_EXPORT void casioemu_arch_core_key_up(void* core, int row, int col) {
	if (!core || row < 0 || col < 0) return;
	static_cast<Sh4Core*>(core)->Bus().keysc().release(static_cast<uint32_t>(row), static_cast<uint32_t>(col));
}

extern "C" SH4_EXPORT void casioemu_arch_core_release_all(void* core) {
	if (core) static_cast<Sh4Core*>(core)->Bus().keysc().releaseAll();
}

extern "C" SH4_EXPORT void casioemu_arch_core_power_on(void* core) {
	if (!core) return;
	// ON is an ordinary matrix key at (6,9) on the CG100; hold it like any other.
	static_cast<Sh4Core*>(core)->Bus().keysc().press(6, 9);
	static_cast<Sh4Core*>(core)->Cpu().run(40000ull);
}

extern "C" SH4_EXPORT int casioemu_arch_core_framebuffer(void* core, unsigned char* out, int outLen) {
	auto* c = static_cast<Sh4Core*>(core);
	if (!c || !out) return 0;
	int w = 0, h = 0, stride = 0;
	const uint8_t* px = c->Rgba(&w, &h, &stride);
	const int need = w * h * 4;
	if (!px || outLen < need) return 0;
	std::memcpy(out, px, static_cast<size_t>(need));
	return 1;
}

extern "C" SH4_EXPORT void* casioemu_arch_core_handle(void* core) {
	return core; // the Sh4Core* itself; debugger uses the C exports above
}

extern "C" SH4_EXPORT int casioemu_arch_core_save_state(void* core, unsigned char* out, int cap) {
	auto* c = static_cast<Sh4Core*>(core);
	if (!c) return 0;
	std::vector<uint8_t> blob;
	if (!c->SaveState(blob)) return 0;
	const int need = static_cast<int>(blob.size());
	if (!out || cap < need) return -need;
	std::memcpy(out, blob.data(), static_cast<size_t>(need));
	return need;
}

extern "C" SH4_EXPORT int casioemu_arch_core_load_state(void* core, const unsigned char* data, int len) {
	auto* c = static_cast<Sh4Core*>(core);
	if (!c || !data || len <= 0) return 0;
	return c->LoadState(data, static_cast<size_t>(len)) ? 1 : 0;
}

extern "C" SH4_EXPORT const char* casioemu_arch_core_status(void* core) {
	auto* c = static_cast<Sh4Core*>(core);
	return c ? c->GetStatusText() : "";
}

// ABI-inspection helper: returns the vtable pointer of the registered
// IArchitectureProvider implementation. Referencing it here keeps the vtable
// externally visible (the Darwin linker otherwise demotes unused vtables to
// local symbols) and lets a harness verify the provider's vtable layout.
extern "C" SH4_EXPORT const void* casioemu_arch_provider_vtable() {
	return *reinterpret_cast<const void* const*>(static_cast<const void*>(&g_provider));
}
