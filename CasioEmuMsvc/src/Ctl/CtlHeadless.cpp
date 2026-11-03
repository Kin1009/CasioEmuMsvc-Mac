// Headless video hint for the casioctl control channel.
//
// The normal desktop entry point initializes SDL video before the Emulator is
// constructed, so `headless=` in argv cannot switch the driver early enough on
// a machine with no WindowServer. A static initializer does run before main;
// CASIOEMU_HEADLESS=1 selects SDL's dummy driver there. The Emulator
// constructor also re-initializes video when `headless=` is present, covering
// the case where SDL video was already brought up on the Cocoa driver.

#include "Config.hpp"

#if defined(__APPLE__) && !defined(CASIOEMU_WEB) && !defined(__EMSCRIPTEN__) && !defined(CASIOEMU_CORE_WEB)

#include <SDL.h>
#include <cstdlib>

namespace {
	struct CtlHeadlessVideoHint {
		CtlHeadlessVideoHint() {
			const char* value = std::getenv("CASIOEMU_HEADLESS");
			if (value && *value && *value != '0')
				SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
		}
	};

	const CtlHeadlessVideoHint g_ctl_headless_video_hint;
} // namespace

#endif
