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

} // namespace casioemu::mac
