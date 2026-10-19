#pragma once

#include <SDL.h>

namespace casioemu {
	inline const char* PreferredAcceleratedRendererDriver() {
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
		return "opengles2";
#elif defined(__APPLE__)
		// Metal avoids the deprecated OpenGL path and presents the offscreen
		// calculator texture with much less CPU overhead on macOS.  If this SDL
		// build has no Metal renderer, CreateRendererWithTargetTexture() falls
		// back to the default/OpenGL/software paths automatically.
		return "metal";
#else
		return "opengl";
#endif
	}

	inline void SetPreferredRendererDriverHint(bool respect_existing_hint = true) {
		if (respect_existing_hint && SDL_GetHint(SDL_HINT_RENDER_DRIVER)) {
			return;
		}
		SDL_SetHint(SDL_HINT_RENDER_DRIVER, PreferredAcceleratedRendererDriver());
	}
}
