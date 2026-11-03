#pragma once

struct SDL_Window;

namespace casioemu::mac {

// Turn the SDL window into a VS Code-style frameless macOS window: the content
// extends under a transparent title bar while the native traffic lights stay
// where macOS puts them.
void ConfigureFramelessWindow(SDL_Window* window);

// Re-apply the background colour after a theme switch so the title bar area
// does not flash the old colour.
void SetWindowBackground(SDL_Window* window, float r, float g, float b);

// Height of the native title-bar strip that the traffic lights occupy.
float TitleBarHeight();

// Native macOS application menu.  The shell layout is custom, but macOS apps
// are expected to expose the standard menu bar.  Call once after SDL_Init.
struct MenuActions {
	void (*open_model)() = nullptr;
	void (*open_folder)() = nullptr;
	void (*install_plugin)() = nullptr;
	void (*toggle_sidebar)() = nullptr;
	void (*toggle_panel)() = nullptr;
	void (*toggle_tools)() = nullptr;
	void (*settings)() = nullptr;
	void (*command_palette)() = nullptr;
	void (*build)() = nullptr;
	void (*build_run)() = nullptr;
	void (*pause_resume)() = nullptr;
	void (*reset)() = nullptr;
};
void InstallNativeMenu(const MenuActions& actions);

} // namespace casioemu::mac
