#include "SessionHost.h"

#include "Chipset/Chipset.hpp"
#include "Emulator.hpp"
#include "Gui/Ui.hpp"
#include "Peripheral/Screen.hpp"
#include "Plugin/IArchitectureProvider.h"
#include "Plugin/PluginMan.h"
#include "Settings/Settings.h"
#include "Binary.h"
#include "ModelInfo.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <vector>

namespace casioemu::session {
namespace {

struct Entry {
	std::unique_ptr<Emulator> owned;
	Emulator* emulator = nullptr;
	std::string modelPath;
	std::string model;
	std::string title;
	std::string rom;
	bool live = true;
	bool primary = false;
	SDL_Texture* mirror = nullptr;
	int mirrorWidth = 0;
	int mirrorHeight = 0;
	uint64_t mirrorCopies = 0;
	uint64_t mirrorSkips = 0;
	double mirrorCopyMs = 0.0;
	std::vector<uint8_t> readback;
};

std::vector<Entry> g_entries;
int g_active = 0;
float g_throttle = 0.25f;
SDL_Renderer* g_mainRenderer = nullptr;

std::string ExecutableDirString() {
	return casioemu::settings::SettingsStore::ExecutableDir().string();
}

std::string CanonicalModelPath(const std::string& path) {
	if (path.empty())
		return path;
	std::error_code ec;
	const auto canonical = std::filesystem::weakly_canonical(path, ec);
	if (!ec)
		return canonical.string();
	return std::filesystem::absolute(path, ec).string();
}

void ApplyClockRates() {
	for (int index = 0; index < static_cast<int>(g_entries.size()); ++index) {
		Entry& entry = g_entries[static_cast<size_t>(index)];
		if (!entry.emulator)
			continue;
		if (!entry.primary && !entry.live)
			continue;
		entry.emulator->SetClockSpeed(index == g_active ? 1.0f : g_throttle);
	}
}

void PointGlobalsAt(int index) {
	if (index < 0 || index >= static_cast<int>(g_entries.size())) {
		// Every session is closed: the shell shows Welcome and no debugger
		// window may keep a pointer to a stopped/destroyed emulator.
		m_emu = nullptr;
		me_mmu = nullptr;
		return;
	}
	Entry& entry = g_entries[static_cast<size_t>(index)];
	if (!entry.emulator)
		return;
	m_emu = entry.emulator;
	me_mmu = &entry.emulator->chipset.mmu;
}

// Board models (the fx-CG100 SH4 pack) are driven by a plugin architecture
// core. The emulator intentionally skips chipset.Setup() for them, so a board
// model with no attached core is completely inert. main() attaches the core to
// the primary emulator from its config.bin; models opened from the GUI go
// through OpenSession() instead and previously never got a core -- that is why
// a freshly imported CG100 package opened as a tab "did nothing". Mirror the
// main() dispatch here so GUI/session-opened board models boot through the
// same plugin core.
void AttachArchitectureCoreForSession(Emulator& emulator, const std::string& modelPath) {
	using namespace casioemu;
	// The early architecture plugin pass normally ran in main(). Keep this
	// robust for hosts/tests that open sessions without going through main().
	if (!GetArchitectureRegistry() || GetArchitectureRegistry()->GetProviderCount() == 0)
		LoadArchitecturePlugins();

	ModelInfo model{};
	std::ifstream config_stream(std::filesystem::path(modelPath) / "config.bin", std::ios::binary);
	if (!config_stream)
		return;
	try {
		Binary::Read(config_stream, model);
	}
	catch (const std::exception& e) {
		std::fprintf(stderr, "[ArchProvider][session] Failed to read config.bin: %s\n", e.what());
		return;
	}
	// Only board models (CG100 packs carry board.svg + board_path) are
	// dispatched. Ordinary models keep their built-in core.
	if (model.board_path.empty() || model.hardware_id != HW_CG100)
		return;

	IArchitectureProvider* provider = GetArchitectureRegistry()->FindProvider(model.hardware_id);
	if (!provider)
		return;
	IEmulatorCore* core = provider->CreateCore();
	if (!core)
		return;

	ArchInitInfo info{};
	const std::string flash_path = model.flash_path.empty()
		? std::string() : emulator.GetModelFilePath(model.flash_path);
	const std::string rom_path = model.rom_path.empty()
		? std::string() : emulator.GetModelFilePath(model.rom_path);
	info.model_path = emulator.model_path.c_str();
	info.flash_path = flash_path.empty() ? nullptr : flash_path.c_str();
	info.rom_path = rom_path.empty() ? nullptr : rom_path.c_str();
	info.hardware_id = model.hardware_id;
	auto lcd = emulator.ModelDefinition.sprites.find("rsd_pixel");
	if (lcd != emulator.ModelDefinition.sprites.end()) {
		info.lcd_x = lcd->second.dest.x;
		info.lcd_y = lcd->second.dest.y;
		info.lcd_w = lcd->second.dest.w;
		info.lcd_h = lcd->second.dest.h;
	}
	if (core->Initialize(info)) {
		core->Reset();
		emulator.SetArchitectureCore(core);
		std::printf("[ArchCore] SH4 Architecture attached to session Emulator; LCD rect = (%d,%d,%d,%d)\n",
			info.lcd_x, info.lcd_y, info.lcd_w, info.lcd_h);
	}
	else {
		std::fprintf(stderr, "[ArchCore] session core Initialize() failed.\n");
		provider->DestroyCore(core);
	}
}

} // namespace

void RegisterPrimary(Emulator* emulator, const std::string& modelPath,
	const std::string& modelName, const std::string& romPath) {
	if (!emulator)
		return;
	if (!g_entries.empty()) {
		Entry& primary = g_entries.front();
		primary.emulator = emulator;
		primary.modelPath = modelPath;
		primary.model = modelName;
		primary.title = modelName.empty() ? modelPath : modelName;
		primary.rom = romPath;
		primary.live = true;
		primary.primary = true;
		return;
	}
	Entry entry;
	entry.emulator = emulator;
	entry.modelPath = modelPath;
	entry.model = modelName;
	entry.title = modelName.empty() ? modelPath : modelName;
	entry.rom = romPath;
	entry.primary = true;
	g_entries.push_back(std::move(entry));
	g_active = 0;
	PointGlobalsAt(0);
}

bool OpenSession(const std::string& modelPath, std::string* error, bool force_new) {
	if (modelPath.empty()) {
		if (error)
			*error = "model path is empty";
		return false;
	}
	const std::string wanted = CanonicalModelPath(modelPath);
	if (!force_new) {
		for (const auto& entry : g_entries) {
			if (entry.live && CanonicalModelPath(entry.modelPath) == wanted) {
				// Already open: just select it.  Canonicalising both sides keeps
				// "models/X", "./models/X" and an absolute path the same session.
				Activate(static_cast<int>(&entry - g_entries.data()));
				return true;
			}
		}
	}

	std::map<std::string, std::string> argv_map;
	argv_map["model"] = modelPath;
	try {
		auto emulator = std::make_unique<Emulator>(argv_map, false, nullptr);
		// GUI-opened board models need the plugin architecture core too, not
		// just the primary emulator created by main().
		AttachArchitectureCoreForSession(*emulator, modelPath);
		Entry entry;
		entry.emulator = emulator.get();
		entry.owned = std::move(emulator);
		entry.modelPath = modelPath;
		entry.model = entry.emulator->ModelDefinition.model_name;
		entry.title = entry.model.empty() ? modelPath : entry.model;
		entry.rom = entry.emulator->ModelDefinition.rom_path;
		entry.live = true;
		g_entries.push_back(std::move(entry));
		Activate(static_cast<int>(g_entries.size()) - 1);
		return true;
	}
	catch (const std::exception& exception) {
		if (error)
			*error = exception.what();
		return false;
	}
	catch (...) {
		if (error)
			*error = "unknown error while opening the model";
		return false;
	}
}

void Activate(int index) {
	if (index < 0 || index >= static_cast<int>(g_entries.size()))
		return;
	if (!g_entries[static_cast<size_t>(index)].emulator)
		return;
	const int previous = g_active;
	g_active = index;
	PointGlobalsAt(index);
	if (previous != index)
		ApplyClockRates();
}

void CloseSession(int index) {
	if (index < 0 || index >= static_cast<int>(g_entries.size()))
		return;
	Entry& entry = g_entries[static_cast<size_t>(index)];
	if (entry.mirror)
		SDL_DestroyTexture(entry.mirror);
	entry.mirror = nullptr;
	if (entry.emulator) {
		// Closing one tab must not post the process-wide shutdown event.
		entry.emulator->Shutdown(false);
		entry.emulator->StopAndJoin();
	}
	// Destructor joins the tick thread before the chipset is destroyed.  The
	// primary emulator is owned by main() (owned == null), so it is stopped and
	// detached from the session list but not deleted here.
	if (entry.owned)
		entry.owned.reset();
	g_entries.erase(g_entries.begin() + index);
	if (g_active >= index)
		--g_active;
	g_active = std::clamp(g_active, 0, std::max(0, static_cast<int>(g_entries.size()) - 1));
	PointGlobalsAt(g_active);
	ApplyClockRates();
}

void ShutdownAll() {
	// Stop every worker but do not destroy the emulator objects here: the
	// primary emulator is owned by the main loop's stack frame and secondary
	// emulators share process-wide debugger globals (me_mmu, m_emu).  Deleting
	// them before the primary destructor tears the chipset while the other
	// worker may still observe those globals, which aborts in FairRecursiveMutex.
	// The process is exiting, so release the owners after stopping the threads.
	for (auto& entry : g_entries) {
		if (entry.mirror) {
			SDL_DestroyTexture(entry.mirror);
			entry.mirror = nullptr;
		}
		if (entry.emulator) {
			entry.emulator->Shutdown(false);
			entry.emulator->StopAndJoin();
		}
	}
	Emulator* primary = g_entries.empty() ? nullptr : g_entries.front().emulator;
	for (auto& entry : g_entries) {
		// Threads are already joined.  Release instead of deleting: SDL audio /
		// renderer teardown after the UI event loop has stopped is not safe on
		// macOS when more than one device exists.  The process is exiting.
		if (entry.owned)
			entry.owned.release();
	}
	if (std::getenv("CASIOEMU_PERF_MIRROR_LOG")) {
		for (size_t i = 0; i < g_entries.size(); ++i) {
			const auto& entry = g_entries[i];
			std::fprintf(stderr, "[mirror] session=%zu copies=%llu skips=%llu copy_ms=%.2f\n",
				i, static_cast<unsigned long long>(entry.mirrorCopies),
				static_cast<unsigned long long>(entry.mirrorSkips), entry.mirrorCopyMs);
		}
	}
	g_entries.clear();
	g_active = 0;
	// Never leave the debugger globals pointing at a destroyed secondary.
	m_emu = primary;
	if (primary)
		me_mmu = &primary->chipset.mmu;
}

Emulator* Active() {
	if (g_active < 0 || g_active >= static_cast<int>(g_entries.size()))
		return nullptr;
	return g_entries[static_cast<size_t>(g_active)].emulator;
}

int ActiveIndex() {
	return g_active;
}

int Count() {
	return static_cast<int>(g_entries.size());
}

bool Running(int index) {
	if (index < 0 || index >= static_cast<int>(g_entries.size()))
		return false;
	const Entry& entry = g_entries[static_cast<size_t>(index)];
	return entry.emulator != nullptr && entry.emulator->Running();
}

std::vector<SessionView> List() {
	std::vector<SessionView> result;
	for (int index = 0; index < static_cast<int>(g_entries.size()); ++index) {
		const Entry& entry = g_entries[static_cast<size_t>(index)];
		SessionView view;
		view.index = index;
		view.title = entry.title;
		view.model = entry.model;
		view.modelPath = entry.modelPath;
		view.romPath = entry.rom;
		view.live = entry.live;
		view.active = index == g_active;
		view.paused = entry.emulator ? entry.emulator->GetPaused() : true;
		result.push_back(std::move(view));
	}
	return result;
}

SDL_Texture* ActiveTexture(int* width, int* height) {
	if (g_active < 0 || g_active >= static_cast<int>(g_entries.size()))
		return nullptr;
	Entry& entry = g_entries[static_cast<size_t>(g_active)];
	if (!entry.emulator)
		return nullptr;
#ifdef SINGLE_WINDOW
	// The primary emulator renders into the same SDL renderer as the shell, so
	// its composite texture can be used directly.
	if (entry.primary) {
		*width = entry.emulator->GetCompositeTextureWidth();
		*height = entry.emulator->GetCompositeTextureHeight();
		return entry.emulator->GetCompositeTexture();
	}
	if (width)
		*width = entry.mirrorWidth;
	if (height)
		*height = entry.mirrorHeight;
	return entry.mirror;
#else
	return nullptr;
#endif
}

void PrepareActiveFrame() {
	if (g_active < 0 || g_active >= static_cast<int>(g_entries.size()))
		return;
	Entry& entry = g_entries[static_cast<size_t>(g_active)];
	if (!entry.emulator || !entry.emulator->Running())
		return;
	entry.emulator->Frame();
#ifdef SINGLE_WINDOW
	if (entry.primary || !g_mainRenderer)
		return;
	SDL_Texture* composite = entry.emulator->GetCompositeTexture();
	if (!composite)
		return;
	int width = 0, height = 0;
	SDL_QueryTexture(composite, nullptr, nullptr, &width, &height);
	if (width <= 0 || height <= 0)
		return;
	if (!entry.mirror || entry.mirrorWidth != width || entry.mirrorHeight != height) {
		if (entry.mirror)
			SDL_DestroyTexture(entry.mirror);
		entry.mirror = SDL_CreateTexture(g_mainRenderer, SDL_PIXELFORMAT_RGBA32,
			SDL_TEXTUREACCESS_STREAMING, width, height);
		entry.mirrorWidth = width;
		entry.mirrorHeight = height;
		entry.readback.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
	}
	if (!entry.mirror)
		return;
	SDL_Renderer* source = entry.emulator->GetRenderer();
	if (!source)
		return;
	const auto mirror_started = std::chrono::steady_clock::now();
	SDL_SetRenderTarget(source, composite);
	SDL_RenderReadPixels(source, nullptr, SDL_PIXELFORMAT_RGBA32, entry.readback.data(), width * 4);
	SDL_SetRenderTarget(source, nullptr);
	SDL_UpdateTexture(entry.mirror, nullptr, entry.readback.data(), width * 4);
	entry.mirrorCopyMs += std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - mirror_started).count();
	++entry.mirrorCopies;
	if (std::getenv("CASIOEMU_PERF_MIRROR_LOG") && entry.mirrorCopies % 30 == 1) {
		std::fprintf(stderr, "[mirror-live] copies=%llu skips=%llu copy_ms=%.2f\n",
			static_cast<unsigned long long>(entry.mirrorCopies),
			static_cast<unsigned long long>(entry.mirrorSkips), entry.mirrorCopyMs);
		}
		
#endif
}

void SetThrottle(float fraction) {
	g_throttle = std::clamp(fraction, 0.05f, 1.0f);
	ApplyClockRates();
}

// Called by the shell once the main SDL renderer exists.
void SetMainRenderer(SDL_Renderer* renderer) {
	g_mainRenderer = renderer;
}

} // namespace casioemu::session
