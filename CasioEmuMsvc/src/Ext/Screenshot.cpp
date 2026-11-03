#include "Screenshot.h"

#include <SDL.h>
#include <SDL_image.h>

#include <cstdio>

namespace casioemu::shot {

bool SaveRendererPng(SDL_Renderer* renderer, const char* path) {
	if (!renderer || !path)
		return false;
	int width = 0, height = 0;
	if (SDL_GetRendererOutputSize(renderer, &width, &height) != 0 || width <= 0 || height <= 0)
		return false;
	SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!surface)
		return false;
	if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
			surface->pixels, surface->pitch) != 0) {
		SDL_FreeSurface(surface);
		return false;
	}
	const bool ok = IMG_SavePNG(surface, path) == 0;
	SDL_FreeSurface(surface);
	if (!ok)
		std::fprintf(stderr, "[shot] IMG_SavePNG(%s) failed: %s\n", path, IMG_GetError());
	return ok;
}

} // namespace casioemu::shot
