#pragma once

#include <SDL.h>

#include <string>
#include <vector>

namespace casioemu {
class Emulator;
}

namespace casioemu::session {

struct SessionView {
	int index = 0;
	std::string title;
	std::string model;
	std::string modelPath;
	std::string romPath;
	bool live = false;
	bool active = false;
	bool paused = false;
};

// Called once from test_gui with the emulator created by the main loop.
void RegisterPrimary(Emulator* emulator, const std::string& modelPath,
	const std::string& modelName, const std::string& romPath);

// Creates a second live emulator.  Returns false and fills `error` when the
// model cannot be opened.  When `force_new` is true an already-open model is
// still opened in a new tab (two independent emulators of the same model).
bool OpenSession(const std::string& modelPath, std::string* error = nullptr,
	bool force_new = false);
void Activate(int index);
void CloseSession(int index);
void ShutdownAll();

Emulator* Active();
int ActiveIndex();
int Count();
bool Running(int index);
std::vector<SessionView> List();

// Screen for the active session on the main renderer.  For the primary session
// this is the emulator's own composite texture; secondary sessions are read
// back into a streaming texture owned by the main renderer.
SDL_Texture* ActiveTexture(int* width, int* height);
// Advance the active session by one UI frame and refresh the mirror texture.
void PrepareActiveFrame();

// Inactive sessions run at this fraction of the active cycle rate.
void SetThrottle(float fraction);

// Main renderer used to mirror secondary sessions' composite textures.
void SetMainRenderer(SDL_Renderer* renderer);

} // namespace casioemu::session
