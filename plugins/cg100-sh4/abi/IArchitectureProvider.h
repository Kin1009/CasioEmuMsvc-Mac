#pragma once
// Vendored verbatim from the authoritative host header:
//   /Volumes/Code/_imports/Desktop/CasioEmuMsvc-Mac/CasioEmuMsvc/src/Plugin/IArchitectureProvider.h
//
// This is the REAL CasioEmuMsvc architecture-provider ABI. Do not reorder
// virtual methods: vtable order is the ABI. Kept as a local copy so this
// plugin builds standalone (no dependency on the host source tree).

#include <cstddef>
#include <cstdint>

// Stable, ABI-friendly name used with PluginApi::QueryInterface(const char*).
#define CASIOEMU_ARCH_REGISTRY_NAME "casioemu.IArchitectureRegistry.v1"

namespace casioemu {

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

	struct ArchInitInfo {
		const char* model_path{nullptr};
		const char* flash_path{nullptr};
		const char* rom_path{nullptr};
		uint16_t hardware_id{0};           // HW_CG100 == 14 in this host tree
		void* renderer{nullptr};
		int lcd_x{0};
		int lcd_y{0};
		int lcd_w{0};
		int lcd_h{0};
		uint32_t reserved{0};
	};

	struct ArchFrameInfo {
		void* renderer{nullptr};
		int dst_x{0};
		int dst_y{0};
		int dst_w{0};
		int dst_h{0};
		uint32_t reserved{0};
	};

	class IEmulatorCore {
	public:
		virtual ~IEmulatorCore() = default;
		virtual const char* GetId() const = 0;
		virtual bool Initialize(const ArchInitInfo& info) = 0;
		virtual void Reset() = 0;
		virtual void Tick() = 0;
		virtual void RenderFrame(const ArchFrameInfo& info) = 0;
		virtual void Key(ArchKey key, bool pressed) = 0;
		virtual void ReleaseAll() = 0;
		virtual const char* GetStatusText() const = 0;
	};

	class IArchitectureProvider {
	public:
		virtual ~IArchitectureProvider() = default;
		virtual const char* GetId() const = 0;
		virtual const char* GetName() const = 0;
		virtual uint32_t GetVersion() const = 0;
		virtual const uint16_t* GetSupportedHardwareIds(size_t* count) const = 0;
		virtual IEmulatorCore* CreateCore() = 0;
		virtual void DestroyCore(IEmulatorCore* core) = 0;
	};

	class IArchitectureRegistry {
	public:
		virtual ~IArchitectureRegistry() = default;
		virtual bool RegisterProvider(IArchitectureProvider* provider) = 0;
		virtual IArchitectureProvider* FindProvider(uint16_t hardware_id) = 0;
		virtual size_t GetProviderCount() const = 0;
		virtual IArchitectureProvider* GetProvider(size_t index) const = 0;
	};

} // namespace casioemu
