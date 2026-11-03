#pragma once

struct SDL_Renderer;

namespace casioemu::shot {

// Reads the current renderer target and writes a PNG.  Works with the
// offscreen SDL video driver and does not depend on the display being awake.
bool SaveRendererPng(SDL_Renderer* renderer, const char* path);

} // namespace casioemu::shot
