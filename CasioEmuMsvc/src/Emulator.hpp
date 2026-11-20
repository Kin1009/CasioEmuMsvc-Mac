#pragma once
#include "Config.hpp"
#include <SDL.h>
#include <SDL_image.h>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

#include "ModelInfo.h"
#include "ModelResourceStore.h"
#include "QrCode.h"
#include "Plugin/IArchitectureProvider.h"

namespace casioemu {
	class Chipset;
	class CPU;
	class MMU;

	/**
	 * A mutex that ensures that a thread cannot get the mutex right after it's released if there are another waiting thread.
	 */
	class FairRecursiveMutex {
		std::mutex m;
		std::thread::id holding;
		int recursive_count;
		std::queue<std::condition_variable> waiting;

	public:
		FairRecursiveMutex();
		~FairRecursiveMutex();
		void lock();
		void unlock();
	};

	class Emulator {
	public:
		SDL_Renderer* renderer = nullptr;
		SDL_Surface* interface_surface = nullptr;
		SDL_Texture* interface_texture = nullptr;
		SDL_Texture* scaled_interface_texture = nullptr;
		int scaled_interface_texture_w = 0;
		int scaled_interface_texture_h = 0;
		bool interface_is_svg = false;
		std::string interface_svg_data;
#ifdef SINGLE_WINDOW
		// The debugger owns the only window in single-window builds. Frame()
		// renders the complete calculator (face, keys, LCD and status sprites)
		// into this offscreen texture; the Calculator debugger window shows it.
		SDL_Texture* composite_texture = nullptr;
		int composite_texture_w = 0;
		int composite_texture_h = 0;
#endif
		unsigned int cycles_per_second;
		unsigned int eps_timer1_source_hz = 0;
		unsigned int timer_interval;
		/* Written by SetClockSpeed (UI thread) and by TimerCallback (timer
		 * thread); keep it atomic to avoid a data race. */
		std::atomic<Uint64> frame_cycle_remainder{0};
		std::atomic<Uint64> eps_timer1_cycle_remainder{0};
		bool running;
		std::atomic<bool> Paused;
		bool headless = false;
		// "headless" argv in the desktop build: render into offscreen targets
		// with SDL's dummy video driver so no display/WindowServer is needed.
		bool offscreen_rendering = false;
		std::atomic<bool> m_step_requested;
		unsigned int last_frame_tick_count;
		std::string model_path;
		std::shared_ptr<ModelResourceStore> model_resources;
		bool pause_on_mem_error;

#ifndef CASIOEMU_CORE_WEB
		std::atomic<bool> screenshot_requested{};
		std::atomic<bool> mirroring_requested{};
		std::atomic<bool> recording_requested{};
		std::atomic<bool> recording_stop_requested{};
		std::atomic<bool> recording_active{};
		std::atomic<unsigned int> recording_frame_count{};
		std::atomic<int> capture_scale{3};
		std::atomic<uint32_t> capture_background_rgb{0xd6e3d6};
#endif

		std::thread* tick_thread = nullptr;
		std::vector<SDL_TimerID> sdl_timers;

		// Composite-presentation cache (single-window builds): skip the full
		// face/keys/LCD redraw when no LCD frame was published and no input or
		// window event happened since the last render.
		std::atomic<bool> composite_dirty{true};
		uint64_t last_composite_version = ~0ull;

		// LCD publish diagnostics (exposed through `casioctl status`).
		std::atomic<uint64_t> lcd_publish_count{0};
		std::atomic<uint64_t> lcd_latency_last_ns{0};
		std::atomic<uint64_t> lcd_latency_max_ns{0};
		std::atomic<uint32_t> lcd_last_changed_pixels{0};

		SpriteInfo interface_background;
		SDL_Rect emu_rect{};

		// Optional plugin-provided architecture core (e.g. SH4 / CG100).
		// When set, Frame()/Tick()/UIEvent() delegate to this core while the
		// normal single-window IDE skin, ImGui debugger tabs and the Calculator
		// dock panel remain fully intact. The core's framebuffer is composited
		// into the LCD rectangle of the model skin.
		std::atomic<IEmulatorCore*> arch_core{nullptr};
		SDL_Texture* arch_lcd_texture = nullptr;
		int arch_lcd_tex_w = 0;
		int arch_lcd_tex_h = 0;
		Uint32 last_arch_tick_ms = 0;
		using ArchFramebufferFn = int (*)(void*, unsigned char*, int);
		using ArchKeyMatrixFn = void (*)(void*, int, int);
		using ArchPowerOnFn = void (*)(void*); // physical ON: hardware wake, no getkey
		using ArchRebootFn = void (*)(void*);  // physical RESET: CPU reset to reset vector
		using ArchEnterDiagFn = void (*)(void*, int); // configure flash/boot state + reboot into OS diag
		using ArchHeldKeyFn = void (*)(void*, int, int);
		using ArchReleaseAllFn = void (*)(void*);
		ArchFramebufferFn arch_framebuffer_fn = nullptr;
		ArchKeyMatrixFn arch_key_matrix_fn = nullptr;
		ArchPowerOnFn arch_power_on_fn = nullptr;
		ArchRebootFn arch_reboot_fn = nullptr;
		ArchEnterDiagFn arch_enter_diag_fn = nullptr;
		// Held-key exports of the plugin C ABI. These are the same raw-matrix
		// down/up calls the plugin's keyboard path uses; a UI macro holds real
		// keys through power-on instead of telling the emulator "enter diag".
		ArchHeldKeyFn arch_key_down_fn = nullptr;
		ArchHeldKeyFn arch_key_up_fn = nullptr;
		ArchReleaseAllFn arch_release_all_fn = nullptr;

		// Idempotence at the host/plugin held-key boundary: remember which
		// (row,col) levels we have asserted so a repeated key_down (or key_up)
		// cannot create a second press/release edge in the plugin/core. The
		// diag macro asserts each key once, but the guard keeps it safe if any
		// other path re-asserts a level. A mutex keeps this correct if input
		// ever arrives off the UI thread.
		std::mutex arch_held_mx;
		std::set<std::pair<int, int>> arch_held_keys;

		// Board models (CG100) bypass the built-in Keyboard peripheral, so its
		// Frame() never draws the dark pressed-key overlay that Graph Light and
		// the other models show. Record the pressed button rects (in face/model
		// coordinates) here instead; the Calculator dock panel renders them over
		// the placed skin texture. Mirrors Keyboard::Frame() for built-in models.
		mutable std::mutex pressed_rect_mx;
		std::vector<Rect> pressed_button_rects;
		bool mouse_highlight_active = false;
		Rect mouse_highlight_rect{};

		void AddButtonHighlight(const Rect& rect);
		void RemoveButtonHighlight(const Rect& rect);

		void SetArchitectureCore(IEmulatorCore* core);

		/**
		 * A bunch of internally used methods for encapsulation purposes.
		 */
		void LoadModelDefition();
		void TimerCallback();
		void SetupLuaAPI();
		void SetupInternals();
		void RunStartupScript();

	public:
		ModelInfo ModelDefinition{};
		SDL_Window* window = nullptr;
		Emulator(std::map<std::string, std::string>& argv_map, bool Paused = false,
			std::shared_ptr<ModelResourceStore> resources = {});
		Emulator(ModelInfo def, bool paused = false, bool headless = true, std::string modelPath = "");
		~Emulator();

		// Serializes execution with snapshot save/restore, including rollback.
		FairRecursiveMutex access_mx;
		HardwareId hardware_id;
		std::map<std::string, std::string>& argv_map;

		// private:
	public:
		/**
		 * The cycle manager structure. This structure is reset every time the
		 * emulator starts emulating CPU cycles and in every timer callback
		 * it's queried for the number of cycles that need to be emulated in the
		 * callback. This ensures that only as many cycles are emulated in a period
		 * of time as many would be in real life.
		 *
		 * Note that it's assumed that the GetDelta function is called once every
		 * timer_interval milliseconds. It's up to the timer to make sure that
		 * there's no drift.
		 */
		struct Cycles {
			void Setup(Uint64 cycles_per_second, unsigned int timer_interval);
			void Reset();
			Uint64 GetDelta();
			Uint64 ticks_now, cycles_emulated, cycles_per_second;
			unsigned int timer_interval;
		} cycles;
		/**
		 * A reference to the emulator chipset. This object holds all CPU, MMU, memory and
		 * peripheral state. The emulator interfaces with the chipset by issuing interrupts
		 * and rendering the screen buffer. It may also read internal state for testing purposes.
		 */
		Chipset& chipset;

		float BatteryVoltage, SolarPanelVoltage;
		QrCodeCapture qr_code;

		bool Running();
		// Asynchronous request (control channel/test harness) to leave the main
		// loop through the normal teardown path. Idempotent.
		void RequestQuit();
		void HandleMemoryError();
		void Shutdown(bool notify_main_loop = true);
		// Stop the worker thread and join it without destroying the emulator.
		// Used by the shell before tearing down multi-session state.
		void StopAndJoin();
		// Reboot the emulated model to its power-on state (the calculator's main
		// menu).  Used when the calculator window is closed: the debugger stays
		// open instead of terminating the whole application.
		void ResetToMainMenu();
		void Tick();
		/**
		 * Called when SDL_WINDOWEVENT_EXPOSED event is received. Does not re-frame.
		 */
		void Repaint();
		void Frame();
		void WindowResize(int width, int height);
		void ExecuteCommand(std::string command);
		unsigned int GetCyclesPerSecond();
		void SetClockSpeed(float speed);
		bool GetPaused();
		void SetPaused(bool paused);
		void RequestStep();
		void UIEvent(SDL_Event event);
		// Send a mouse press/release already in model (face) coordinates. The
		// Calculator debugger window uses this so clicking the on-screen keys works
		// in single-window builds, where there is no emulator window rectangle.
		void ModelMouseEvent(int model_x, int model_y, bool down);

		// --- Plugin architecture-core UI macro helpers (CG100 diag button) -----
		// Thin wrappers over the plugin's plain-C exports. They press real keys
		// and read the real screen; no diagnostic mode is implemented here.
		bool HasArchitectureCore() const { return arch_core.load(std::memory_order_acquire) != nullptr; }
		void ArchSetHeldKey(int row, int col, bool down);
		void ArchReleaseAllHeldKeys();
		// Self-releasing key tap (down + deferred up) through the plugin's raw
		// matrix bridge: the same path a mouse click on an on-screen key uses.
		void ArchTapKey(int row, int col);
		bool ArchReadFramebufferRGBA(unsigned char* out, int outLen);
		void ArchPowerOn();
		// Physical RESET button: true hardware reset of the emulated CPU to
		// its reset vector and a fresh OS boot (not a snapshot restore).
		void ArchReboot();
		// Configure calculator state (flash model word / boot flags) and reset,
		// so the OS itself enters diag on the next boot. mode 0 = service menu,
		// mode 1 = "DIAGNOSTIC MODE / Factory Use Only!". No UI keypresses and
		// no injected output -- the plugin patches real state and reboots.
		void ArchEnterDiag(int mode);

		// --- Board-model pressed-key highlight (CG100) -------------------------
		// The arch/board path has no Keyboard::Frame(), so the Calculator panel
		// draws the dark overlay itself from these accessors. All rectangles are
		// in face/model (skin) pixel coordinates.
		std::vector<Rect> GetPressedButtonRects() const;
		// Resolve a kiko ((row << 4) | col) or an SDL key code to the matching
		// button rect from ModelDefinition, then add/remove its highlight.
		void SetButtonHighlight(int kiko, bool pressed);
		void SetButtonHighlightBySdlKey(int sdlKey, bool pressed);

		SDL_Renderer* GetRenderer();
		SDL_Texture* GetInterfaceTexture();
#ifdef SINGLE_WINDOW
		// The last complete model frame rendered by Frame() (single-window builds).
		SDL_Texture* GetCompositeTexture() { return composite_texture; }
		int GetCompositeTextureWidth() const { return composite_texture_w; }
		int GetCompositeTextureHeight() const { return composite_texture_h; }
#endif
		std::string GetModelFilePath(std::string relative_path) const;
		bool HasModelResource(const std::string& name) const;
		std::vector<std::uint8_t> ReadModelResource(const std::string& name) const;
		void WriteModelSessionResource(const std::string& name, const std::vector<std::uint8_t>& data);
		bool IsMemoryModel() const { return static_cast<bool>(model_resources); }

		friend class CPU;
		friend class MMU;
	};
} // namespace casioemu
