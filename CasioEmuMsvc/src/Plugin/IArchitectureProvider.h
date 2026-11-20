#pragma once
// IArchitectureProvider / IEmulatorCore / IArchitectureRegistry
//
// Extension point that lets a plugin contribute a *new CPU architecture* to
// CasioEmuMsvc. It is intentionally a separate interface obtained through
// PluginApi::QueryInterface(CASIOEMU_ARCH_REGISTRY_NAME) so that the existing
// PluginApi vtable is NOT changed: plugins compiled against the old header
// keep working unchanged.
//
// Host side:
//   * PluginApi_Impl::QueryInterface returns the global IArchitectureRegistry
//     for CASIOEMU_ARCH_REGISTRY_NAME.
//   * PluginMan owns the registry implementation and calls fPluginLoadArch()
//     during the early (pre-Emulator) plugin pass.
//   * casioemu.cpp, when ModelDefinition.hardware_id == HW_CG100 (10), asks the
//     registry for a provider, creates an IEmulatorCore and delegates Tick(),
//     RenderFrame() and key input to it instead of the built-in chipset.
//
// The CG100 core uses a fixed 396x224 big-endian RGB565 framebuffer. The
// plugin's RenderFrame() implementation is responsible for converting it to
// the texture format expected by the SDL_Renderer passed in ArchFrameInfo.

#include <cstddef>
#include <cstdint>

// Stable, ABI-friendly name used with PluginApi::QueryInterface(const char*).
#define CASIOEMU_ARCH_REGISTRY_NAME "casioemu.IArchitectureRegistry.v1"

namespace casioemu {

	/// Hardware ids understood by an architecture provider (model config.bin
	/// hardware_id field). Keys are deliberately SDL-free so the interface can
	/// also be used by headless frontends.
	enum class ArchKey : uint32_t {
		None = 0,
		Up, Down, Left, Right,
		EXE, EXIT,
		F1, F2, F3, F4, F5, F6,
		Shift, Alpha, Home, ON, AC, X10X,
		D0, D1, D2, D3, D4, D5, D6, D7, D8, D9,
		Dot, Plus, Minus, Multiply, Divide,
		Power, Format,
		KeyCount
	};

	/// Parameters handed to IEmulatorCore::Initialize.
	struct ArchInitInfo {
		const char* model_path{nullptr};    ///< directory containing config.bin
		const char* flash_path{nullptr};    ///< absolute path to the 32 MiB flash image
		const char* rom_path{nullptr};      ///< may be nullptr/empty for CG100
		uint16_t hardware_id{0};           ///< HW_CG100 == 10
		void* renderer{nullptr};           ///< SDL_Renderer* (opaque here)
		int lcd_x{0};                      ///< LCD rectangle inside the interface sprite
		int lcd_y{0};
		int lcd_w{0};
		int lcd_h{0};
		uint32_t reserved{0};
	};

	/// Where the core should draw its framebuffer. The host has already bound
	/// a render target; `renderer` is the same SDL_Renderer* passed in init.
	struct ArchFrameInfo {
		void* renderer{nullptr};
		int dst_x{0};
		int dst_y{0};
		int dst_w{0};
		int dst_h{0};
		uint32_t reserved{0};
	};

	/// A running emulator instance supplied by a plugin.
	class IEmulatorCore {
	public:
		virtual ~IEmulatorCore() = default;
		virtual const char* GetId() const = 0;
		virtual bool Initialize(const ArchInitInfo& info) = 0;
		virtual void Reset() = 0;
		/// Advance the machine by one host time slice (called from the tick thread).
		virtual void Tick() = 0;
		/// Draw the live framebuffer into the currently bound render target.
		/// Implementations must convert big-endian RGB565 (CG100: 396x224) to
		/// the SDL texture format (e.g. RGBA8888) before uploading.
		virtual void RenderFrame(const ArchFrameInfo& info) = 0;
		virtual void Key(ArchKey key, bool pressed) = 0;
		virtual void ReleaseAll() = 0;
		/// Short status string used by the fallback/debug ImGui window.
		virtual const char* GetStatusText() const = 0;
	};

	/// Implemented by plugins, registered into IArchitectureRegistry.
	class IArchitectureProvider {
	public:
		virtual ~IArchitectureProvider() = default;
		virtual const char* GetId() const = 0;
		virtual const char* GetName() const = 0;
		virtual uint32_t GetVersion() const = 0;
		/// Returns a pointer to a static array of supported hardware_id values.
		virtual const uint16_t* GetSupportedHardwareIds(size_t* count) const = 0;
		virtual IEmulatorCore* CreateCore() = 0;
		virtual void DestroyCore(IEmulatorCore* core) = 0;
	};

	/// Implemented by the host. Obtain via
	/// api->QueryInterface(CASIOEMU_ARCH_REGISTRY_NAME).
	class IArchitectureRegistry {
	public:
		virtual ~IArchitectureRegistry() = default;
		virtual bool RegisterProvider(IArchitectureProvider* provider) = 0;
		virtual IArchitectureProvider* FindProvider(uint16_t hardware_id) = 0;
		virtual size_t GetProviderCount() const = 0;
		virtual IArchitectureProvider* GetProvider(size_t index) const = 0;
	};

} // namespace casioemu
