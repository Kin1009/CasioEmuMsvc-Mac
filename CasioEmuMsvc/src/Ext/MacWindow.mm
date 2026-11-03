#include "MacWindow.h"

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>

#include <SDL.h>
#include <SDL_syswm.h>

namespace casioemu::mac {

void ConfigureFramelessWindow(SDL_Window* window) {
	if (!window)
		return;
	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);
	if (!SDL_GetWindowWMInfo(window, &info))
		return;
	if (info.subsystem != SDL_SYSWM_COCOA)
		return;
	NSWindow* ns_window = info.info.cocoa.window;
	if (!ns_window)
		return;

	// Keep the native title bar, transparency, resize, zoom, and fullscreen
	// behaviour; only move the content under it.
	ns_window.styleMask |= NSWindowStyleMaskFullSizeContentView;
	ns_window.titlebarAppearsTransparent = YES;
	ns_window.titleVisibility = NSWindowTitleHidden;
	ns_window.movableByWindowBackground = NO;
	ns_window.backgroundColor = [NSColor colorWithSRGBRed:0.094 green:0.094 blue:0.094 alpha:1.0];
	if (@available(macOS 11.0, *)) {
		ns_window.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
	}
	// The traffic lights are native; keep all three visible and in the normal
	// place.  Their superview is the title bar container that AppKit creates
	// for the full-size content view.
	[[ns_window standardWindowButton:NSWindowCloseButton] setHidden:NO];
	[[ns_window standardWindowButton:NSWindowMiniaturizeButton] setHidden:NO];
	[[ns_window standardWindowButton:NSWindowZoomButton] setHidden:NO];
}

void SetWindowBackground(SDL_Window* window, float r, float g, float b) {
	if (!window)
		return;
	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);
	if (!SDL_GetWindowWMInfo(window, &info))
		return;
	if (info.subsystem != SDL_SYSWM_COCOA)
		return;
	NSWindow* ns_window = info.info.cocoa.window;
	if (!ns_window)
		return;
	ns_window.backgroundColor = [NSColor colorWithSRGBRed:r green:g blue:b alpha:1.0];
}

float TitleBarHeight() {
	return 35.0f;
}

} // namespace casioemu::mac

#else

namespace casioemu::mac {
void ConfigureFramelessWindow(SDL_Window*) {}
void SetWindowBackground(SDL_Window*, float, float, float) {}
float TitleBarHeight() { return 0.0f; }
} // namespace casioemu::mac

#endif
