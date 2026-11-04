#include "SessionHost.h"

#include "Chipset/Chipset.hpp"
#include "Emulator.hpp"
#include "Gui/Ui.hpp"
#include "Peripheral/Screen.hpp"
#include "Settings/Settings.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
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
	int mirrorAge = 0;
	uint64_t lastMirrorVersion = ~static_cast<uint64_t>(0);
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
	if (index < 0 || index >= static_cast<int>(g_entries.size()))
		return;
	Entry& entry = g_entries[static_cast<size_t>(index)];
	if (!entry.emulator)
		return;
	m_emu = entry.emulator;
	me_mmu = &entry.emulator->chipset.mmu;
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

bool OpenSession(const std::string& modelPath, std::string* error) {
	if (modelPath.empty()) {
		if (error)
			*error = "model path is empty";
		return false;
	}
	const std::string wanted = CanonicalModelPath(modelPath);
	for (const auto& entry : g_entries) {
		if (entry.live && CanonicalModelPath(entry.modelPath) == wanted) {
			// Already open: just select it.  Canonicalising both sides keeps
			// "models/X", "./models/X" and an absolute path the same session.
			Activate(static_cast<int>(&entry - g_entries.data()));
			return true;
		}
	}

	std::map<std::string, std::string> argv_map;
	argv_map["model"] = modelPath;
	try {
		auto emulator = std::make_unique<Emulator>(argv_map, false, nullptr);
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
	if (index <= 0 || index >= static_cast<int>(g_entries.size()))
		return;
	Entry& entry = g_entries[static_cast<size_t>(index)];
	if (entry.mirror)
		SDL_DestroyTexture(entry.mirror);
	entry.mirror = nullptr;
	if (entry.emulator)
		entry.emulator->Shutdown();
	// Destructor joins the tick thread before the chipset is destroyed.
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
			entry.emulator->Shutdown();
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
	// The LCD publishes a monotonic frame version.  Secondary sessions used to
	// read back and upload their whole composite every displayed frame; skip
	// that work while the frame is unchanged, with a periodic safety refresh
	// for non-LCD state (key highlights/status sprites).
	uint64_t frame_version = ~static_cast<uint64_t>(0);
	if (auto* provider = entry.emulator->chipset.QueryInterface<casioemu::IScreenFrameProvider>())
		frame_version = provider->FrameVersion();
	++entry.mirrorAge;
	static const bool always_copy = std::getenv("CASIOEMU_PERF_MIRROR_ALWAYS") != nullptr;
	if (!always_copy && entry.mirror && frame_version != ~static_cast<uint64_t>(0) &&
		frame_version == entry.lastMirrorVersion && entry.mirrorAge < 20) {
		++entry.mirrorSkips;
		return;
	}
	entry.mirrorAge = 0;
	entry.lastMirrorVersion = frame_version;
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
