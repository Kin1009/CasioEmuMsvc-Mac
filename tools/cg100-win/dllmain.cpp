// CG100SH4.plugin -- SH4 / fx-CG100 architecture provider for CasioEmuMsvc (macOS).
//
// Exports:
//   fPluginLoadArch - early pass: register the architecture provider only.
//   fPluginLoad     - normal pass: register plugin + add an ImGui fallback window.
//
// The emulated machine is the real Go SH4 core (libcig100core.dylib), loaded
// through the frozen C ABI in cg100core.h.
//
// Ownership of the C ABI names belongs to CasioEmuMsvc (the host), which
// declares them in src/Plugin/ArchCoreABI.h. This plugin IMPLEMENTS those
// names; cg100core.h is now a thin compatibility shim that includes the
// host header. Signatures/symbols are unchanged (ARCH ABI v1).

// Windows port: the host statically links SDL2 into CasioEmuMsvc.exe and
// exports the few SDL entry points we need (--export-all-symbols on MinGW),
// so GetProcAddress(GetModuleHandle(nullptr), ...) replaces dlsym(RTLD_DEFAULT).
#include "PluginApi.h"
#include "cg100core.h"

#include <SDL_pixels.h>   // pixel-format constants only; SDL calls via GetProcAddress
#include <SDL_render.h>   // SDL_TextureAccess constants only

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#ifdef _WIN32
# include <windows.h>
#else
# include <dlfcn.h>
#endif

// MinGW does not implicitly export extern "C" functions from a DLL; make the
// ABI symbols explicit so the host loader's GetProcAddress() finds them.
#if defined(_WIN32)
# define CASIO_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
# define CASIO_PLUGIN_EXPORT extern "C"
#endif

namespace {

constexpr int FB_W = CG100_FB_W;   // 396
constexpr int FB_H = CG100_FB_H;   // 224

const char* kDefaultFlash = "/Volumes/Code/casio-tools/temp/cg100_bootable_flash.bin";
const char* kDefaultState = "/Volumes/Code/casio-tools/temp/cg100_home_state_OFF.bin";

// Host Emulator.cpp budgets HW_CG100 at 200 MIPS; hand the same rate to the Go
// core so its RTC / 32.768 kHz counter / 33 Hz KEYSC time base stays in sync
// with the host clock instead of the core's 70 MIPS default.
constexpr uint64_t kCg100Ips = 200000000ULL;

std::string EnvOr(const char* name, const char* fallback) {
	const char* v = std::getenv(name);
	return (v && *v) ? std::string(v) : std::string(fallback);
}

std::vector<uint8_t> ReadFile(const std::string& path) {
	std::vector<uint8_t> data;
	std::ifstream f(path, std::ios::binary);
	if (!f) return data;
	f.seekg(0, std::ios::end);
	std::streamoff n = f.tellg();
	if (n <= 0) return data;
	f.seekg(0, std::ios::beg);
	data.resize(static_cast<size_t>(n));
	f.read(reinterpret_cast<char*>(data.data()), n);
	return data;
}

// ---------------------------------------------------------------------------
// SDL entry points resolve from the host process (SDL2 is statically linked
// into CasioEmuMsvc). No SDL link-time dependency for this plugin.
// ---------------------------------------------------------------------------
struct SDLBridge {
	using CreateTextureFn = void* (*)(void*, uint32_t, int, int, int);
	using UpdateTextureFn = int (*)(void*, const void*, const void*, int);
	using DestroyTextureFn = void (*)(void*);
	using RenderCopyFn = int (*)(void*, void*, const void*, const void*);

	CreateTextureFn createTexture = nullptr;
	UpdateTextureFn updateTexture = nullptr;
	DestroyTextureFn destroyTexture = nullptr;
	RenderCopyFn renderCopy = nullptr;

	static SDLBridge& Instance() {
		static SDLBridge b;
		return b;
	}
	SDLBridge() {
#if defined(_WIN32)
		HMODULE host = GetModuleHandle(nullptr);
		createTexture  = reinterpret_cast<CreateTextureFn>(GetProcAddress(host, "SDL_CreateTexture"));
		updateTexture  = reinterpret_cast<UpdateTextureFn>(GetProcAddress(host, "SDL_UpdateTexture"));
		destroyTexture = reinterpret_cast<DestroyTextureFn>(GetProcAddress(host, "SDL_DestroyTexture"));
		renderCopy     = reinterpret_cast<RenderCopyFn>(GetProcAddress(host, "SDL_RenderCopy"));
#elif defined(__APPLE__) || defined(__linux__)
		createTexture  = reinterpret_cast<CreateTextureFn>(dlsym(RTLD_DEFAULT, "SDL_CreateTexture"));
		updateTexture  = reinterpret_cast<UpdateTextureFn>(dlsym(RTLD_DEFAULT, "SDL_UpdateTexture"));
		destroyTexture = reinterpret_cast<DestroyTextureFn>(dlsym(RTLD_DEFAULT, "SDL_DestroyTexture"));
		renderCopy     = reinterpret_cast<RenderCopyFn>(dlsym(RTLD_DEFAULT, "SDL_RenderCopy"));
#endif
	}
	bool ok() const { return createTexture && updateTexture && destroyTexture && renderCopy; }
};

// ---------------------------------------------------------------------------
// CG100Core -- implements casioemu::IEmulatorCore on top of the C ABI.
// ---------------------------------------------------------------------------
class CG100Core : public casioemu::IEmulatorCore {
public:
	const char* GetId() const override { return "sh4.cg100.core"; }

	bool Initialize(const casioemu::ArchInitInfo& info) override {
		m_lastError.clear();
		std::string flashPath;
		const char* envFlash = std::getenv("CG100_FLASH");
		if (envFlash && *envFlash) {
			flashPath = envFlash;
		} else if (info.flash_path && *info.flash_path && std::ifstream(info.flash_path, std::ios::binary)) {
			flashPath = info.flash_path;
		} else {
			flashPath = EnvOr("CG100_FLASH", kDefaultFlash);
		}
		m_flash = ReadFile(flashPath);
		if (m_flash.empty()) {
			m_lastError = "Cannot read flash image: " + flashPath;
			return false;
		}
		m_flashPath = flashPath;
		m_renderer = info.renderer;
		m_lcdX = info.lcd_x;
		m_lcdY = info.lcd_y;
		m_lcdW = info.lcd_w;
		m_lcdH = info.lcd_h;
		return true;
	}

	void Reset() override {
		if (m_core) { cg100_destroy(m_core); m_core = nullptr; }
		DestroyTexture();
		if (m_flash.empty()) { m_lastError = "No flash loaded"; return; }
		m_core = cg100_create(m_flash.data(), static_cast<uint32_t>(m_flash.size()));
		if (!m_core) { m_lastError = "cg100_create failed"; return; }
		// Prefer a verified HOME save-state: it boots straight to the HOME menu
		// and avoids the ~90s first-run wizard inside the IDE app window.
		// A state replaces every bit of CPU/memory/MMIO state that a cold boot
		// produces, so skip cg100_reset()'s ~170M-cycle (~18 s) boot-to-language-
		// picker loop. reset_no_boot still installs the two HLE hooks, which are
		// Go function values and cannot live inside a serialised state.
		auto state = ReadFile(EnvOr("CG100_STATE", kDefaultState));
		if (!state.empty()) {
			cg100_reset_no_boot(m_core);
			cg100_set_ips(m_core, kCg100Ips);
			if (cg100_load_state(m_core, state.data(), static_cast<uint32_t>(state.size())) == 0) {
				m_wizardResult = 0;
				m_status = "HOME menu reached (loaded state)";
				RefreshFramebuffer();
				return;
			}
		}
		// No usable save-state: fall back to a full cold boot (unchanged order).
		cg100_reset(m_core);
		cg100_set_ips(m_core, kCg100Ips);
		cg100_run_to_idle(m_core, 400);
		// Verified first-run wizard -> HOME / MAIN MENU.
		m_wizardResult = cg100_goto_menu(m_core);
		m_status = m_wizardResult == 0 ? "HOME menu reached" : "booted (wizard helper nonzero)";
		RefreshFramebuffer();
	}

	// Physical RESET button on the back of the calculator: a genuine hardware
	// reset of the emulated CPU to the reset vector (0x80000000), followed by a
	// fresh cold boot of the OS. Deliberately does NOT load the HOME save-state:
	// that is a snapshot restore (not a CPU reset) and cannot be used to escape
	// a wedged "System ERROR / INTERRUPT" screen. This is NOT a no-op: the Go
	// core is destroyed and recreated and cg100_reset_no_boot() installs the
	// bootstrap and HLE hooks and sets PC = 0x80000000.
	//
	// Use the LIGHTWEIGHT reset here. cg100_reset() would synchronously spin
	// 340 x 500k = 170M cycles (plus cg100_run_to_idle's up to 200M more) on the
	// calling thread -- several seconds of frozen UI. reset_no_boot performs
	// the identical CPU/register bootstrap but leaves execution to the normal
	// Tick() loop (one 500k-cycle slice per frame), so the OS boots gradually
	// from the language picker while the host stays responsive.
	void Reboot() {
		if (m_core) { cg100_destroy(m_core); m_core = nullptr; }
		DestroyTexture();
		if (m_flash.empty()) { m_lastError = "No flash loaded"; return; }
		m_core = cg100_create(m_flash.data(), static_cast<uint32_t>(m_flash.size()));
		if (!m_core) { m_lastError = "cg100_create failed"; return; }
		cg100_reset_no_boot(m_core);     // PC -> 0x80000000, bootstrap + HLE hooks
		cg100_set_ips(m_core, kCg100Ips);
		m_wizardResult = -1;
		m_status = "hardware reset - booting...";
		m_diagComboReleaseIn = 0; // a plain reboot never inherits the diag combo
		RefreshFramebuffer();
	}

	// Genuine OS diag entry by CONFIGURING calculator state and rebooting --
	// no UI key presses, no injected output. Two documented, independent boot
	// gates of CG100 OS 2.00 (see temp/DIAG_GATE_FINAL.md / DIAG_ANGLE_PHASE.md):
	//   mode 0: flash model word 0x300..303 = FF FF FF FF (factory-blank). The
	//           OS's own is_erased gate at boot calls diag with r4=0 -> the
	//           [BaseROM MAIN] / 1.PATTERN 5.SERVICE service menu.
	//   mode 1: cold boot with the genuine diag combo held IN-CORE at (0,0)
	//           AC, (4,1) x10^x and (6,9) ON. A38 bit4 is latched by the core's
	//           own reset bootstrap and the update/exam marker is erased, so
	//           the OS calls its own diag with r4=1 -> "=DIAGNOSTIC MODE= /
	//           Factory Use Only!". Keys enter through cg100_key_down at the
	//           core boundary, NOT through the UI, and are held only until this
	//           core's Tick loop auto-releases them once the dialog is rendered.
	void EnterDiag(int mode) {
		if (m_flash.empty()) { m_lastError = "No flash loaded"; return; }
		if (mode == 0) {
			// Legitimate factory-blank state: the exact 4 bytes the OS compares.
			for (uint32_t a = 0x300; a <= 0x303 && a < m_flash.size(); ++a)
				m_flash[a] = 0xFF;
		} else {
			// 0x95FF at 0xB20002 = Exam ON, which SUPPRESSES the boot diag
			// key-check. Erased (0xFF) is the normal CG100 state. The shipped
			// flash already has this erased; re-assert it so a stale exam state
			// can never block the factory-diag gate.
			if (m_flash.size() > 0xB20003) {
				m_flash[0xB20002] = 0xFF;
				m_flash[0xB20003] = 0xFF;
			}
		}
		// Reboot from the now-configured flash: destroy+recreate the Go core
		// from m_flash, reset_no_boot -> PC at reset vector, A38 boot flag set.
		Reboot();
		if (!m_core) return;
		if (mode == 1) {
			// Hold the real diag combo in-core. No synchronous cycle spinning:
			// Tick() drives the boot one slice per frame and auto-releases these
			// keys once the OS has rendered its diag dialog.
			cg100_key_down(m_core, 0, 0);
			cg100_key_down(m_core, 4, 1);
			cg100_key_down(m_core, 6, 9);
			// Hold the combo for 550 real core slices (275M cycles). The OS
			// renders the diag dialog at ~200M cycles; Tick() releases the keys
			// afterwards so they cannot linger into diag navigation. Release is
			// done HERE with the real Go handle m_core: the generic
			// casioemu_arch_core_release_all C export takes an IEmulatorCore*
			// and cannot be used for this (it is a legacy no-op path).
			m_diagComboReleaseIn = 550;
			m_status = "factory diag: booting with combo held...";
		} else {
			m_status = "service diag: factory-blank 0x300, booting...";
		}
		RefreshFramebuffer();
	}

	void Tick() override {
		if (!m_core || m_paused) return;
		// One 500,000-cycle slice per host UI frame keeps the window responsive.
		cg100_run(m_core, 1);
		// Auto-release the in-core diag combo once the OS has had time to render
		// its genuine diag dialog. Counts core slices, not wall time, so it is
		// deterministic regardless of host frame pacing.
		if (m_diagComboReleaseIn > 0 && --m_diagComboReleaseIn == 0) {
			cg100_release_all(m_core);
			m_status = "factory diag entered (combo released)";
		}
		RefreshFramebuffer();
	}

	void RenderFrame(const casioemu::ArchFrameInfo& info) override {
		if (!m_core || !SDLBridge::Instance().ok()) return;
		void* renderer = info.renderer ? info.renderer : m_renderer;
		if (!renderer) return;
		EnsureTexture(renderer, FB_W, FB_H);
		if (!m_texture) return;
		SDLBridge::Instance().updateTexture(m_texture, nullptr, m_rgba.data(), FB_W * 4);

		struct Rect { int x, y, w, h; };
		int dstW = info.dst_w > 0 ? info.dst_w : m_lcdW;
		int dstH = info.dst_h > 0 ? info.dst_h : m_lcdH;
		if (dstW <= 0) dstW = FB_W;
		if (dstH <= 0) dstH = FB_H;
		// Preserve the 396:224 aspect ratio, centre in the destination.
		const float scale = std::min(static_cast<float>(dstW) / FB_W,
		                            static_cast<float>(dstH) / FB_H);
		Rect dst{};
		dst.w = static_cast<int>(FB_W * scale);
		dst.h = static_cast<int>(FB_H * scale);
		dst.x = info.dst_x + (dstW - dst.w) / 2;
		dst.y = info.dst_y + (dstH - dst.h) / 2;
		SDLBridge::Instance().renderCopy(renderer, m_texture, nullptr, &dst);
	}

	// Keyboard keys are HELD: IEmulatorCore::Key(key,true/false) maps to
	// cg100_key_down/up, so multiple keys can be down in the KEYSC matrix at the
	// same scan (SHIFT+digit, ON+x10^x+AC, ...). Mouse clicks go through the
	// separate KeyMatrix()/arch_key_matrix_fn path, which stays a self-releasing
	// cg100_key tap.
	void Key(casioemu::ArchKey key, bool pressed) override {
		if (!m_core) return;
		// Hardware-verified (ONPROBE.g3a on a real CG100): ON is an ordinary
		// matrix key at (6,9) -- KIUDATA word4 = 0x4000, gint KEY_ON = 0xa6.
		// It is held/released through the SAME cg100_key_down/up path as every
		// other key; there is no separate power-on event path any more.
		int row = -1, col = -1;
		switch (key) {
		// CG100 OS 2.00 matrix coords, derived from the CG100 itself (live OS
		// key-queue probe at 0x8c15fed2). The old CG50-derived coords were wrong
		// for the whole navigation cluster - notably LEFT/RIGHT were swapped.
		case casioemu::ArchKey::Up: row = 3; col = 9; break;    // 0x7542
		case casioemu::ArchKey::Down: row = 3; col = 7; break;  // 0x7547
		case casioemu::ArchKey::Left: row = 4; col = 8; break;  // 0x7544 (was 2,8 = RIGHT)
		case casioemu::ArchKey::Right: row = 2; col = 8; break; // 0x7545 (was 1,7 = softkey)
		case casioemu::ArchKey::EXE: row = 2; col = 1; break;   // 0x7534
		case casioemu::ArchKey::EXIT: row = 5; col = 8; break;  // 0x7532
		case casioemu::ArchKey::F1: row = 6; col = 9; break;
		case casioemu::ArchKey::F2: row = 5; col = 9; break;
		case casioemu::ArchKey::F3: row = 4; col = 9; break;
		case casioemu::ArchKey::F4: row = 3; col = 9; break;
		case casioemu::ArchKey::F5: row = 2; col = 9; break;
		case casioemu::ArchKey::F6: row = 1; col = 9; break;
		case casioemu::ArchKey::Shift: row = 6; col = 7; break; // 0x7536
		case casioemu::ArchKey::Alpha: row = 5; col = 7; break; // 0x7537
		case casioemu::ArchKey::Home: row = 5; col = 9; break;  // 0x7533
		case casioemu::ArchKey::ON: row = 6; col = 9; break;   // hardware-verified ON: getkey 0xa6, word4=0x4000
		case casioemu::ArchKey::AC: row = 0; col = 0; break;   // AC getkey 0x753f at (0,0)
		case casioemu::ArchKey::X10X: row = 4; col = 1; break; // x10^x / ×10ˣ, getkey 0x000f
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
		case casioemu::ArchKey::Dot: row = 5; col = 1; break;
		case casioemu::ArchKey::Plus: row = 3; col = 2; break;
		case casioemu::ArchKey::Minus: row = 2; col = 2; break;
		case casioemu::ArchKey::Multiply: row = 3; col = 3; break;
		case casioemu::ArchKey::Divide: row = 2; col = 3; break;
		default: break;
		}
		if (row >= 0 && col >= 0) {
			if (pressed)
				cg100_key_down(m_core, static_cast<uint32_t>(row), static_cast<uint32_t>(col));
			else
				cg100_key_up(m_core, static_cast<uint32_t>(row), static_cast<uint32_t>(col));
			m_status = std::string(pressed ? "down (" : "up   (") +
				std::to_string(row) + "," + std::to_string(col) + ")";
		}
	}

	// Held-key bridge used by the raw-matrix C ABI exports below. Unlike Tap()/
	// KeyMatrix() (self-releasing), this leaves the key held until the matching
	// key_up / ReleaseAll. Crucially it goes through the real Go handle m_core;
	// the C exports themselves only receive an IEmulatorCore* (CG100Core*).
	void HeldKey(int row, int col, bool down) {
		if (!m_core || row < 0 || col < 0) return;
		if (down)
			cg100_key_down(m_core, static_cast<uint32_t>(row), static_cast<uint32_t>(col));
		else
			cg100_key_up(m_core, static_cast<uint32_t>(row), static_cast<uint32_t>(col));
		m_status = std::string(down ? "down (" : "up   (") +
			std::to_string(row) + "," + std::to_string(col) + ")";
	}

	void ReleaseAll() override {
		if (m_core) cg100_release_all(m_core);
	}
	const char* GetStatusText() const override { return m_status.c_str(); }

	// --- generic casioctl bridge -------------------------------------------
	// Raw matrix injection (used by `casioctl key matrix R C`). cg100_key
	// injects a self-releasing tap; a short run makes the OS scan it.
	void KeyMatrix(int row, int col) { Tap(row, col, 80); }

	// Legacy host entry: route ON through the ordinary held-matrix path now
	// that the real CG100 key is known to be (6,9). Hosts should normally call
	// Key(ArchKey::ON, down/up) directly; this shim leaves ON held until the
	// matching release/ReleaseAll arrives.
	void PowerOn() {
		if (!m_core) return;
		Key(casioemu::ArchKey::ON, true);
		cg100_run(m_core, 1);
		RefreshFramebuffer();
		m_status = "ON (6,9) held";
	}

	// --- concrete-only helpers used by the ImGui fallback window ------------
	void Tap(int row, int col, int slices = 80) {
		if (!m_core) return;
		cg100_key(m_core, static_cast<uint32_t>(row), static_cast<uint32_t>(col));
		// 80 x 500k = 40M cycles: the pacing verified by the C ABI harness.
		cg100_run(m_core, slices);
		RefreshFramebuffer();
	}
	void Demo2Plus3() {
		Tap(6, 2);        // digit 1: open Calculate from HOME
		Tap(5, 2);        // 2
		Tap(3, 2);        // +
		Tap(4, 2);        // 3
		Tap(2, 1, 120);   // EXE
		m_status = "sent 1 -> 2+3 EXE";
	}
	bool LoadHomeState() {
		auto state = ReadFile(EnvOr("CG100_STATE", kDefaultState));
		if (state.empty() || !m_core) return false;
		return cg100_load_state(m_core, state.data(), static_cast<uint32_t>(state.size())) == 0;
	}
	bool SaveStateTo(const std::string& path) {
		if (!m_core) return false;
		uint32_t n = cg100_state_len(m_core);
		if (!n) return false;
		std::vector<uint8_t> buf(n);
		uint32_t written = cg100_save_state(m_core, buf.data(), n);
		std::ofstream out(path, std::ios::binary);
		if (!out) return false;
		out.write(reinterpret_cast<const char*>(buf.data()), written);
		return written > 0;
	}
	const uint8_t* RGBA(int* w, int* h, int* stride) const {
		if (w) *w = FB_W;
		if (h) *h = FB_H;
		if (stride) *stride = FB_W * 4;
		return m_rgba.empty() ? nullptr : m_rgba.data();
	}
	// Opaque void* handle for the additive cg100_usb_* C ABI.
	void* CoreHandle() const { return m_core; }
	// Run a few 500k-cycle slices so the OS observes plug/eject MMIO changes.
	void RunSlices(int slices) {
		if (!m_core) return;
		cg100_run(m_core, slices);
		RefreshFramebuffer();
	}
	const std::string& Error() const { return m_lastError; }
	const std::string& FlashPath() const { return m_flashPath; }

private:
	void RefreshFramebuffer() {
		if (!m_core) return;
		uint16_t be[FB_W * FB_H];
		cg100_framebuffer(m_core, be, FB_W * FB_H);
		if (m_rgba.size() != static_cast<size_t>(FB_W) * FB_H * 4)
			m_rgba.resize(static_cast<size_t>(FB_W) * FB_H * 4);
		for (int i = 0; i < FB_W * FB_H; ++i) {
			const uint16_t p = static_cast<uint16_t>((be[i] << 8) | (be[i] >> 8)); // BE -> LE
			const uint8_t r = static_cast<uint8_t>(((p >> 11) & 0x1f) * 255 / 31);
			const uint8_t g = static_cast<uint8_t>(((p >> 5) & 0x3f) * 255 / 63);
			const uint8_t b = static_cast<uint8_t>((p & 0x1f) * 255 / 31);
			m_rgba[i * 4 + 0] = r;
			m_rgba[i * 4 + 1] = g;
			m_rgba[i * 4 + 2] = b;
			m_rgba[i * 4 + 3] = 255;
		}
	}
	void EnsureTexture(void* renderer, int w, int h) {
		if (!SDLBridge::Instance().ok() || !renderer) return;
		if (m_texture && m_texW == w && m_texH == h) return;
		DestroyTexture();
		m_texture = SDLBridge::Instance().createTexture(renderer, SDL_PIXELFORMAT_RGBA32,
			SDL_TEXTUREACCESS_STREAMING, w, h);
		m_texW = w;
		m_texH = h;
	}
	void DestroyTexture() {
		if (m_texture && SDLBridge::Instance().destroyTexture)
			SDLBridge::Instance().destroyTexture(m_texture);
		m_texture = nullptr;
	}

	void* m_core = nullptr;
	void* m_renderer = nullptr;
	void* m_texture = nullptr;
	int m_texW = 0, m_texH = 0;
	int m_lcdX = 0, m_lcdY = 0, m_lcdW = 0, m_lcdH = 0;
	std::vector<uint8_t> m_flash;
	std::vector<uint8_t> m_rgba;
	std::string m_flashPath;
	std::string m_lastError;
	std::string m_status = "idle";
	int m_wizardResult = -1;
	bool m_paused = false;
	// When >0, Tick() counts down core slices holding the in-core diag combo,
	// then releases it with the real Go handle (see EnterDiag mode 1).
	int m_diagComboReleaseIn = 0;
};

// ---------------------------------------------------------------------------
// CG100Provider -- registers hardware_id 10 (HW_CG100) with the host.
// ---------------------------------------------------------------------------
class CG100Provider : public casioemu::IArchitectureProvider {
public:
	const char* GetId() const override { return "sh4.cg100"; }
	const char* GetName() const override { return "SH4 Architecture"; }
	uint32_t GetVersion() const override { return 1; }
	const uint16_t* GetSupportedHardwareIds(size_t* count) const override {
		static const uint16_t ids[] = {14}; // HW_CG100 in the preview tree
		if (count) *count = 1;
		return ids;
	}
	casioemu::IEmulatorCore* CreateCore() override { return new CG100Core(); }
	void DestroyCore(casioemu::IEmulatorCore* core) override { delete core; }
};
CG100Provider g_provider;

PluginApi* g_api = nullptr;

// The debug window and the USB-drive window must act on the SAME core instance
// the host created through the architecture provider. The host does not expose
// that pointer to plugins, so mirror the core's flash/state configuration here:
// create one shared CG100Core on first use (booted to HOME via the same
// save-state path CG100Core::Reset() uses) and let both windows use it. This is
// the same pattern the historical CG100 SH4 Emulator / CG100 USB Drive plugin
// used (GetSharedCore() in the object strings).
static CG100Core* GetSharedCore() {
	static CG100Core* core = nullptr;
	static bool tried = false;
	if (tried) return core;
	tried = true;

	std::string flashPath = EnvOr("CG100_FLASH", kDefaultFlash);
	auto flash = ReadFile(flashPath);
	if (flash.empty()) return nullptr;

	core = new CG100Core();
	casioemu::ArchInitInfo info{};
	info.flash_path = flashPath.c_str();
	if (!core->Initialize(info)) {
		delete core;
		core = nullptr;
		return nullptr;
	}
	core->Reset();  // loads CG100_STATE (HOME menu) when available
	if (std::string(core->GetStatusText()).find("failed") != std::string::npos)
		core = nullptr;
	return core;
}

// ---------------------------------------------------------------------------
// ImGui windows. UIWindow is fully defined in the host PluginApi.h (imgui.h is
// included there when CASIOEMU_HEADLESS_PLUGIN is not set), so deriving from it
// is ABI-compatible with the host's window list.
// ---------------------------------------------------------------------------
#ifndef CASIOEMU_HEADLESS_PLUGIN

// This dylib links its own private ImGui copy, so its GImGui is separate from
// the host's. fPluginLoad binds it once; this fallback covers the case where
// the host context did not exist yet at load time. GetCurrentContext() reads
// THIS dylib's GImGui only, so a null value is a safe signal to retry.
inline void BindHostImGuiContext() {
	if (ImGui::GetCurrentContext() == nullptr && g_api) {
		if (void* ctx = g_api->GetImGuiContext())
			ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx));
	}
}

class CG100DebugWindow : public UIWindow {
public:
	CG100DebugWindow() : UIWindow("CG100 SH4 Emulator") {
		inital_size = ImVec2(560, 640);
	}

	void RenderCore() override {
		BindHostImGuiContext();
		CG100Core* core = GetSharedCore();
		if (!core) {
			ImGui::TextUnformatted("No CG100 core (check CG100_FLASH path).");
			return;
		}
		ImGui::TextUnformatted("SH4 / CG100 OS 2.00 (libcig100core.dylib)");
		ImGui::Text("Flash: %s", core->FlashPath().c_str());
		if (!core->Error().empty()) ImGui::Text("Error: %s", core->Error().c_str());
		ImGui::Text("Status: %s", core->GetStatusText());
		ImGui::Separator();
		if (ImGui::Button("Reset")) { core->Reboot(); }
		ImGui::SameLine();
		if (ImGui::Button("Press 2+3")) { core->Demo2Plus3(); }
		ImGui::SameLine();
		if (ImGui::Button("Load Save-State (HOME)")) {
			ImGui::TextUnformatted(core->LoadHomeState() ? "loaded HOME state" : "load failed");
		}
		if (ImGui::Button("Save State")) {
			const std::string path = EnvOr("CG100_SAVE", "/Volumes/Code/casio-tools/temp/cg100_plugin_state.bin");
			ImGui::TextUnformatted(core->SaveStateTo(path) ? ("saved " + path).c_str() : "save failed");
		}
		ImGui::Text("Info: %s", "SH4 core, 396x224 RGB565");
		ImGui::TextUnformatted("Keys: arrows / digits / EXE(Enter) / F1-F6 / F4 / F2");
		HandleKeyboard(core);
	}

private:
	void HandleKeyboard(CG100Core* core) {
		struct K { ImGuiKey key; casioemu::ArchKey ak; const char* label; };
		static const K keys[] = {
			{ImGuiKey_UpArrow, casioemu::ArchKey::Up, "Up"},
			{ImGuiKey_DownArrow, casioemu::ArchKey::Down, "Down"},
			{ImGuiKey_LeftArrow, casioemu::ArchKey::Left, "Left"},
			{ImGuiKey_RightArrow, casioemu::ArchKey::Right, "Right"},
			{ImGuiKey_Enter, casioemu::ArchKey::EXE, "EXE"},
			{ImGuiKey_Escape, casioemu::ArchKey::EXIT, "EXIT"},
			{ImGuiKey_F1, casioemu::ArchKey::F1, "F1"},
			{ImGuiKey_F2, casioemu::ArchKey::F2, "F2"},
			{ImGuiKey_F3, casioemu::ArchKey::F3, "F3"},
			{ImGuiKey_F4, casioemu::ArchKey::F4, "F4"},
			{ImGuiKey_F5, casioemu::ArchKey::F5, "F5"},
			{ImGuiKey_F6, casioemu::ArchKey::F6, "F6"},
			{ImGuiKey_LeftShift, casioemu::ArchKey::Shift, "Shift"},
			{ImGuiKey_RightShift, casioemu::ArchKey::Shift, "Shift"},
			{ImGuiKey_A, casioemu::ArchKey::Alpha, "Alpha"},
			{ImGuiKey_Home, casioemu::ArchKey::Home, "Home"},
		};
		for (const K& k : keys) {
			if (ImGui::IsKeyPressed(k.key, false)) core->Key(k.ak, true);
			if (ImGui::IsKeyReleased(k.key)) core->Key(k.ak, false);
		}
		ImGui::TextUnformatted(core->GetStatusText());
	}
};

class CG100UsbDriveWindow : public UIWindow {
public:
	CG100UsbDriveWindow() : UIWindow("CG100 USB Drive") {
		inital_size = ImVec2(760, 720);
	}

	void RenderCore() override {
		BindHostImGuiContext();
		CG100Core* core = GetSharedCore();
		void* handle = core ? core->CoreHandle() : nullptr;
		if (!handle) {
			ImGui::TextUnformatted("No CG100 core (check CG100_FLASH path).");
			return;
		}

		// --- connection mode + lifecycle ------------------------------------
		ImGui::TextUnformatted("Connection mode (OS 'Select Connection Mode' screen)");
		static const char* kModes[] = {"USB Flash", "OS Update", "USB Flash / Unlock Exam Mode"};
		int modeIndex = cg100_usb_mode(handle) - 1;
		if (modeIndex < 0) modeIndex = 2; // mode 3 default
		if (modeIndex > 2) modeIndex = 2;
		if (ImGui::Combo("##connmode", &modeIndex, kModes, 3)) {
			cg100_usb_set_mode(handle, modeIndex + 1);
			m_mode = modeIndex + 1;
		}
		ImGui::TextDisabled("'USB Flash / Unlock Exam Mode' drives Agent E's unlock path on eject.");
		if (ImGui::Button("Plug")) {
			cg100_usb_plug(handle);
			core->RunSlices(8); // let the OS observe the IRQ-mediated insert
			m_status = "cable inserted";
			RefreshFiles(handle);
		}
		ImGui::SameLine();
		if (ImGui::Button("Eject")) {
			cg100_usb_eject(handle);
			core->RunSlices(8);
			m_status = "drive ejected / cable removed";
			RefreshFiles(handle);
		}
		ImGui::SameLine();
		if (ImGui::Button("Refresh")) {
			RefreshFiles(handle);
			m_status = "refreshed";
		}
		const char* state = StateName(cg100_usb_status(handle));
		m_mode = cg100_usb_mode(handle);
		ImGui::Text("UsbState: %s   Mode: %d", state, m_mode);
		const char* detail = cg100_usb_status_text(handle);
		if (detail && *detail) ImGui::TextUnformatted(detail);
		if (!m_status.empty()) ImGui::TextUnformatted(m_status.c_str());

		const bool mounted = cg100_usb_status(handle) == CG100_USB_MOUNTED;
		if (!mounted) ImGui::TextUnformatted("Plug first: file operations require a mounted drive.");
		ImGui::Separator();

		// --- editable virtual disk file list -------------------------------
		if (ImGui::CollapsingHeader("Virtual disk  (fls0 / FAT12, editable)", ImGuiTreeNodeFlags_DefaultOpen)) {
			if (ImGui::BeginTable("##usbFiles", 3,
				ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
				ImVec2(0, 120))) {
				ImGui::TableSetupColumn("Name");
				ImGui::TableSetupColumn("Size");
				ImGui::TableSetupColumn("Delete");
				ImGui::TableHeadersRow();
				for (size_t i = 0; i < m_files.size(); ++i) {
					ImGui::PushID(static_cast<int>(i));
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					if (ImGui::Selectable(m_files[i].name.c_str(), m_selected == static_cast<int>(i)))
						m_selected = static_cast<int>(i);
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(m_files[i].size.c_str());
					ImGui::TableNextColumn();
					if (ImGui::SmallButton("x")) {
						if (cg100_usb_rm(handle, m_files[i].name.c_str()) == 0) {
							m_status = "deleted " + m_files[i].name;
							RefreshFiles(handle);
						} else {
							m_status = "delete failed";
						}
					}
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			if (m_files.empty()) ImGui::TextUnformatted("(empty drive)");

			ImGui::TextUnformatted("Add from host path:");
			ImGui::InputText("##addpath", m_addPath, sizeof(m_addPath));
			ImGui::SameLine();
			if (ImGui::Button("Add File")) {
				auto data = ReadFile(m_addPath);
				std::string name = BaseName(m_addPath);
				if (!data.empty() && !name.empty() &&
					cg100_usb_put(handle, name.c_str(), data.data(), static_cast<int>(data.size())) == 0) {
					m_status = "added " + name + " (" + std::to_string(data.size()) + " B)";
					RefreshFiles(handle);
				} else {
					m_status = "add failed (8.3 name? file empty?)";
				}
			}
			ImGui::TextUnformatted("Or type a script (written as a .py file):");
			ImGui::InputText("name##addname", m_addName, sizeof(m_addName));
			ImGui::InputTextMultiline("##pytext", m_text, sizeof(m_text), ImVec2(0, 80));
			if (ImGui::Button("Write Text As .py")) {
				std::string name = m_addName[0] ? m_addName : "SCRIPT.PY";
				if (name.find('.') == std::string::npos) name += ".PY";
				if (cg100_usb_put(handle, name.c_str(),
					reinterpret_cast<const uint8_t*>(m_text), static_cast<int>(std::strlen(m_text))) == 0) {
					m_status = "wrote " + name;
					RefreshFiles(handle);
				} else {
					m_status = "write failed (8.3 name?)";
				}
			}
			ImGui::TextUnformatted("Export selected to host:");
			ImGui::InputText("##exportpath", m_exportPath, sizeof(m_exportPath));
			ImGui::SameLine();
			if (ImGui::Button("Export")) {
				if (m_selected < 0 || m_selected >= static_cast<int>(m_files.size())) {
					m_status = "select a file first";
				} else {
					std::string name = m_files[m_selected].name;
					int n = cg100_usb_get_size(handle, name.c_str());
					if (n < 0) {
						m_status = "export failed";
					} else {
						std::vector<uint8_t> buf(static_cast<size_t>(n));
						int got = cg100_usb_get(handle, name.c_str(), buf.data(), n);
						std::ofstream out(m_exportPath, std::ios::binary);
						if (got == n && out) {
							out.write(reinterpret_cast<const char*>(buf.data()), n);
							m_status = "exported " + name;
						} else {
							m_status = "export failed";
						}
					}
				}
			}
		}
		ImGui::Separator();

		// --- raw hex editor (simplified: editable hex-string field) ---------
		if (ImGui::CollapsingHeader("Raw hex editor (selected file)")) {
			if (m_selected < 0 || m_selected >= static_cast<int>(m_files.size())) {
				ImGui::TextUnformatted("Select a file above to edit its bytes.");
			} else {
				std::string name = m_files[m_selected].name;
				int n = cg100_usb_get_size(handle, name.c_str());
				ImGui::Text("File: %s  (%u bytes)", name.c_str(), static_cast<unsigned>(n < 0 ? 0 : n));
				if (ImGui::Button("Reload From File")) LoadSelected(handle);
				if (n > 0 && m_hexBytes.empty()) LoadSelected(handle);
				ImGui::InputTextMultiline("##hexedit", m_hexText, sizeof(m_hexText), ImVec2(0, 80));
				if (ImGui::Button("Write Back To File")) {
					std::vector<uint8_t> bytes = ParseHex(m_hexText);
					if (cg100_usb_put(handle, name.c_str(), bytes.data(), static_cast<int>(bytes.size())) == 0) {
						m_status = "wrote back " + std::to_string(bytes.size()) + " B";
						RefreshFiles(handle);
					} else {
						m_status = "write-back failed";
					}
				}
			}
		}
		ImGui::Separator();

		// --- raw flash region reader/writer ---------------------------------
		if (ImGui::CollapsingHeader("Raw flash region")) {
			ImGui::InputText("addr##rawaddr", m_rawAddr, sizeof(m_rawAddr), ImGuiInputTextFlags_CharsHexadecimal);
			ImGui::InputText("len##rawlen", m_rawLen, sizeof(m_rawLen), ImGuiInputTextFlags_CharsDecimal);
			if (ImGui::Button("Read Region")) {
				uint32_t addr = static_cast<uint32_t>(std::strtoul(m_rawAddr, nullptr, 16));
				int len = std::atoi(m_rawLen);
				if (len < 1) len = 64;
				if (len > static_cast<int>(sizeof(m_rawBuf))) len = sizeof(m_rawBuf);
				int got = cg100_usb_read_raw(handle, addr, m_rawBuf, len);
				if (got > 0) {
					m_rawLenActual = got;
					m_status = "read " + std::to_string(got) + " B @ 0x" + m_rawAddr;
					RefreshRawText();
				} else {
					m_status = "read failed";
				}
			}
			ImGui::InputTextMultiline("##rawedit", m_rawText, sizeof(m_rawText), ImVec2(0, 60));
			if (ImGui::Button("Write Region Back")) {
				std::vector<uint8_t> bytes = ParseHex(m_rawText);
				uint32_t addr = static_cast<uint32_t>(std::strtoul(m_rawAddr, nullptr, 16));
				if (cg100_usb_write_raw(handle, addr, bytes.data(), static_cast<int>(bytes.size())) == 0)
					m_status = "wrote raw region";
				else
					m_status = "write failed";
			}
			if (m_rawLenActual > 0) ImGui::TextUnformatted(m_rawText);
		}
	}

private:
	struct FileRow { std::string name; std::string size; };

	static const char* StateName(int state) {
		switch (state) {
		case CG100_USB_DISCONNECTED: return "DISCONNECTED";
		case CG100_USB_CONNECTED: return "CONNECTED";
		case CG100_USB_CONFIGURED: return "CONFIGURED";
		case CG100_USB_MOUNTED: return "DRIVE_MOUNTED";
		case CG100_USB_EJECTED: return "EJECTED";
		default: return "(?)";
		}
	}

	static std::string BaseName(const std::string& path) {
		size_t slash = path.find_last_of("/\\");
		return slash == std::string::npos ? path : path.substr(slash + 1);
	}

	static std::vector<uint8_t> ParseHex(const char* text) {
		std::vector<uint8_t> out;
		unsigned int v = 0;
		int digits = 0;
		for (const char* p = text; *p; ++p) {
			unsigned int nib;
			char c = *p;
			if (c >= '0' && c <= '9') nib = static_cast<unsigned>(c - '0');
			else if (c >= 'a' && c <= 'f') nib = static_cast<unsigned>(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') nib = static_cast<unsigned>(c - 'A' + 10);
			else continue;
			v = (v << 4) | nib;
			if (++digits == 2) { out.push_back(static_cast<uint8_t>(v)); v = 0; digits = 0; }
		}
		return out;
	}

	static std::string HexText(const uint8_t* data, int n) {
		static const char* kDigits = "0123456789ABCDEF";
		std::string s;
		for (int i = 0; i < n; ++i) {
			if (i && (i % 16) == 0) s += '\n';
			s += kDigits[(data[i] >> 4) & 0xf];
			s += kDigits[data[i] & 0xf];
			s += ' ';
		}
		return s;
	}

	void RefreshRawText() {
		std::string s = HexText(m_rawBuf, m_rawLenActual);
		std::snprintf(m_rawText, sizeof(m_rawText), "%s", s.c_str());
	}

	void RefreshFiles(void* handle) {
		m_files.clear();
		m_selected = -1;
		m_hexBytes.clear();
		m_hexText[0] = '\0';
		char buf[8192];
		int n = cg100_usb_list(handle, buf, static_cast<int>(sizeof(buf)));
		if (n <= 0) return;
		std::istringstream in(buf);
		std::string line;
		while (std::getline(in, line)) {
			size_t tab = line.find('\t');
			if (tab == std::string::npos) continue;
			FileRow row;
			row.name = line.substr(0, tab);
			row.size = line.substr(tab + 1);
			if (!row.name.empty()) m_files.push_back(row);
		}
	}

	void LoadSelected(void* handle) {
		if (m_selected < 0 || m_selected >= static_cast<int>(m_files.size())) return;
		int n = cg100_usb_get_size(handle, m_files[m_selected].name.c_str());
		if (n <= 0) return;
		m_hexBytes.resize(static_cast<size_t>(n));
		if (cg100_usb_get(handle, m_files[m_selected].name.c_str(), m_hexBytes.data(), n) != n) return;
		std::string s = HexText(m_hexBytes.data(), n);
		std::snprintf(m_hexText, sizeof(m_hexText), "%s", s.c_str());
	}

	std::vector<FileRow> m_files;
	int m_selected = -1;
	int m_mode = CG100_CONN_USB_FLASH_UNLOCK_EXAM;
	std::string m_status;
	char m_addPath[512] = {};
	char m_addName[32] = {};
	char m_text[4096] = {};
	char m_exportPath[512] = {};
	char m_hexText[8192] = {};
	std::vector<uint8_t> m_hexBytes;
	char m_rawAddr[32] = "C83A00";
	char m_rawLen[16] = "64";
	int m_rawLenActual = 0;
	uint8_t m_rawBuf[4096] = {};
	char m_rawText[16384] = {};
};

#endif // !CASIOEMU_HEADLESS_PLUGIN

// Generic casioctl bridge exports. Defined inside the anonymous namespace so
// `CG100Core` (the class) is unambiguous against the C typedef of the same
// name from cg100core.h; extern "C" still gives them plain C linkage.
CASIO_PLUGIN_EXPORT void casioemu_arch_core_key_matrix(void* core, int row, int col) {
	auto* c = static_cast<CG100Core*>(core);
	if (c) c->KeyMatrix(row, col);
}

// Hardware wake for the dedicated physical ON button. The host calls this for
// the ON keyboard shortcut / ON skin hitbox; AC continues to use the matrix
// bridge above (getkey 0x753f). cg100_power_on injects no getkey.
CASIO_PLUGIN_EXPORT void casioemu_arch_core_power_on(void* core) {
	auto* c = static_cast<CG100Core*>(core);
	if (c) c->PowerOn();
}

// Physical RESET button (back of the calculator). Takes the same IEmulatorCore*
// handle as framebuffer/handle/power_on, and performs a TRUE CPU reset to the
// reset vector followed by a fresh OS cold boot. Not a snapshot restore, not a
// no-op: use this to escape the OS "System ERROR / INTERRUPT" screen.
CASIO_PLUGIN_EXPORT void casioemu_arch_core_reboot(void* core) {
	auto* c = static_cast<CG100Core*>(core);
	if (c) c->Reboot();
}

// Genuine OS diag entry by configuring calculator state (flash / boot flags)
// and rebooting -- the OS itself enters diag on the next boot. mode 0 = service
// menu (factory-blank flash 0x300), mode 1 = "DIAGNOSTIC MODE / Factory Use
// Only!" (boot key-combo gate, held in-core). No fake output, no UI keypresses.
// Takes the same IEmulatorCore* handle as casioemu_arch_core_reboot.
CASIO_PLUGIN_EXPORT void casioemu_arch_core_enter_diag(void* core, int mode) {
	auto* c = static_cast<CG100Core*>(core);
	if (c) c->EnterDiag(mode);
}

// Held-key bridge (raw matrix coordinates). The host passes arch_core.load(),
// i.e. an IEmulatorCore* (concrete CG100Core*), NOT the opaque Go handle.
// cg100_key_down/up/release_all require m_core; passing the C++ object made
// coreGet() return nil and silently no-op. Dispatch through member functions
// that use m_core, exactly like the other arch exports. Signatures unchanged.
CASIO_PLUGIN_EXPORT void casioemu_arch_core_key_down(void* core, int row, int col) {
	auto* c = static_cast<CG100Core*>(core);
	if (c) c->HeldKey(row, col, true);
}
CASIO_PLUGIN_EXPORT void casioemu_arch_core_key_up(void* core, int row, int col) {
	auto* c = static_cast<CG100Core*>(core);
	if (c) c->HeldKey(row, col, false);
}
CASIO_PLUGIN_EXPORT void casioemu_arch_core_release_all(void* core) {
	auto* c = static_cast<CG100Core*>(core);
	if (c) c->ReleaseAll();
}

CASIO_PLUGIN_EXPORT int casioemu_arch_core_framebuffer(void* core, unsigned char* out, int outLen) {
	auto* c = static_cast<CG100Core*>(core);
	if (!c || !out) return 0;
	int w = 0, h = 0, stride = 0;
	const uint8_t* px = c->RGBA(&w, &h, &stride);
	const int need = w * h * 4;
	if (!px || outLen < need) return 0;
	std::memcpy(out, px, static_cast<size_t>(need));
	return 1;
}

// Additive host bridge: exposes the opaque void* handle behind the
// IEmulatorCore* so the host debugger can call the plain-C cg100_* ABI
// (cycles/pushes/fault/USB state/raw flash reads) directly. Existing exports
// are unchanged.
CASIO_PLUGIN_EXPORT void* casioemu_arch_core_handle(void* core) {
	auto* c = static_cast<CG100Core*>(core);
	return c ? c->CoreHandle() : nullptr;
}

} // namespace

// (PLUGIN_EXPORT_ABI() removed: undefined in the current host PluginApi.h;
// the extern "C" fPluginLoad/fPluginLoadArch exports below are the ABI.)

// Early pass: register only the architecture provider. Never create windows
// here (the host's ImGui context / Emulator object may not exist yet).
CASIO_PLUGIN_EXPORT void fPluginLoadArch(PluginApi* api) {
	if (!api) return;
	api->RegisterArchitectureProvider(&g_provider);
}

CASIO_PLUGIN_EXPORT void fPluginLoad(PluginApi* api) {
	if (!api) return;
	g_api = api;
	PLUGINASSERTSTL(api);
	api->RegisterPlugin("cg100sh4", "SH4 Architecture", 1);
	api->RegisterArchitectureProvider(&g_provider);
	// This plugin links its own private copy of ImGui (DLLs do not share ImGui
	// globals such as GImGui). Point our copy at the host's context so the UI
	// windows below draw into the same context/frame the host renders.
#ifndef CASIOEMU_HEADLESS_PLUGIN
	if (void* ctx = api->GetImGuiContext())
		ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx));
#endif
	// Restore the two ImGui windows from the historical CG100 plugin. The debug
	// window owns the key / diag controls; the USB window drives the 16
	// cg100_usb_* exports on the same shared core.
#ifndef CASIOEMU_HEADLESS_PLUGIN
	api->AddWindow(new CG100DebugWindow());
	api->AddWindow(new CG100UsbDriveWindow());
#endif
}
