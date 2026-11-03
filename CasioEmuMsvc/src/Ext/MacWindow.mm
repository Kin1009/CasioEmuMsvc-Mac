#include "MacWindow.h"

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>

#include <SDL.h>
#include <SDL_syswm.h>

static casioemu::mac::MenuActions g_menu_actions;

@interface CasioMenuTarget : NSObject
- (void)openModel:(id)sender;
- (void)openFolder:(id)sender;
- (void)installPlugin:(id)sender;
- (void)toggleSidebar:(id)sender;
- (void)togglePanel:(id)sender;
- (void)toggleTools:(id)sender;
- (void)openSettings:(id)sender;
- (void)commandPalette:(id)sender;
- (void)build:(id)sender;
- (void)buildRun:(id)sender;
- (void)pauseResume:(id)sender;
- (void)resetEmulator:(id)sender;
@end

@implementation CasioMenuTarget
- (void)openModel:(id)sender { if (g_menu_actions.open_model) g_menu_actions.open_model(); }
- (void)openFolder:(id)sender { if (g_menu_actions.open_folder) g_menu_actions.open_folder(); }
- (void)installPlugin:(id)sender { if (g_menu_actions.install_plugin) g_menu_actions.install_plugin(); }
- (void)toggleSidebar:(id)sender { if (g_menu_actions.toggle_sidebar) g_menu_actions.toggle_sidebar(); }
- (void)togglePanel:(id)sender { if (g_menu_actions.toggle_panel) g_menu_actions.toggle_panel(); }
- (void)toggleTools:(id)sender { if (g_menu_actions.toggle_tools) g_menu_actions.toggle_tools(); }
- (void)openSettings:(id)sender { if (g_menu_actions.settings) g_menu_actions.settings(); }
- (void)commandPalette:(id)sender { if (g_menu_actions.command_palette) g_menu_actions.command_palette(); }
- (void)build:(id)sender { if (g_menu_actions.build) g_menu_actions.build(); }
- (void)buildRun:(id)sender { if (g_menu_actions.build_run) g_menu_actions.build_run(); }
- (void)pauseResume:(id)sender { if (g_menu_actions.pause_resume) g_menu_actions.pause_resume(); }
- (void)resetEmulator:(id)sender { if (g_menu_actions.reset) g_menu_actions.reset(); }
@end

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

void InstallNativeMenu(const MenuActions& actions) {
	g_menu_actions = actions;
	@autoreleasepool {
		NSApplication* app = [NSApplication sharedApplication];
		NSMenu* main_menu = [[NSMenu alloc] initWithTitle:@"CasioEmuMsvc"];
		CasioMenuTarget* target = [[CasioMenuTarget alloc] init];

		// App menu
		NSMenuItem* app_item = [[NSMenuItem alloc] initWithTitle:@"CasioEmuMsvc" action:nil keyEquivalent:@""];
		NSMenu* app_menu = [[NSMenu alloc] initWithTitle:@"CasioEmuMsvc"];
		[app_menu addItemWithTitle:@"About CasioEmuMsvc" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
		[app_menu addItem:[NSMenuItem separatorItem]];
		NSMenuItem* settings_item = [[NSMenuItem alloc] initWithTitle:@"Settings..." action:@selector(openSettings:) keyEquivalent:@","];
		settings_item.target = target;
		[app_menu addItem:settings_item];
		[app_menu addItem:[NSMenuItem separatorItem]];
		[app_menu addItemWithTitle:@"Hide CasioEmuMsvc" action:@selector(hide:) keyEquivalent:@"h"];
		[app_menu addItemWithTitle:@"Quit CasioEmuMsvc" action:@selector(terminate:) keyEquivalent:@"q"];
		app_item.submenu = app_menu;
		[main_menu addItem:app_item];

		auto add_menu = [&](NSString* title, NSString* firstTitle, SEL firstAction, NSString* key) {
			NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
			NSMenu* menu = [[NSMenu alloc] initWithTitle:title];
			if (firstTitle) {
				NSMenuItem* first = [[NSMenuItem alloc] initWithTitle:firstTitle action:firstAction keyEquivalent:key];
				first.target = target;
				[menu addItem:first];
			}
			item.submenu = menu;
			[main_menu addItem:item];
			return menu;
		};

		NSMenu* file = add_menu(@"File", @"Open Model...", @selector(openModel:), @"o");
		NSMenuItem* folder = [[NSMenuItem alloc] initWithTitle:@"Open Folder..." action:@selector(openFolder:) keyEquivalent:@"O"];
		folder.target = target;
		[folder setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagShift];
		[file addItem:folder];
		NSMenuItem* plugin = [[NSMenuItem alloc] initWithTitle:@"Install Plugin..." action:@selector(installPlugin:) keyEquivalent:@""];
		plugin.target = target;
		[file addItem:plugin];
		[file addItem:[NSMenuItem separatorItem]];
		[file addItemWithTitle:@"Close Window" action:@selector(performClose:) keyEquivalent:@"w"];

		NSMenu* edit = add_menu(@"Edit", nil, nil, nil);
		[edit addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
		[edit addItemWithTitle:@"Redo" action:@selector(redo:) keyEquivalent:@"Z"];
		[edit addItem:[NSMenuItem separatorItem]];
		[edit addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
		[edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
		[edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
		[edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];

		NSMenu* view = add_menu(@"View", @"Toggle Sidebar", @selector(toggleSidebar:), @"b");
		NSMenuItem* panel = [[NSMenuItem alloc] initWithTitle:@"Toggle Bottom Panel" action:@selector(togglePanel:) keyEquivalent:@"j"];
		panel.target = target;
		[view addItem:panel];
		NSMenuItem* tools = [[NSMenuItem alloc] initWithTitle:@"Toggle Tool Dock" action:@selector(toggleTools:) keyEquivalent:@""];
		tools.target = target;
		[view addItem:tools];
		[view addItem:[NSMenuItem separatorItem]];
		NSMenuItem* palette = [[NSMenuItem alloc] initWithTitle:@"Command Palette..." action:@selector(commandPalette:) keyEquivalent:@"P"];
		palette.target = target;
		[palette setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagShift];
		[view addItem:palette];
		[view addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];

		NSMenu* session = add_menu(@"Session", @"Build", @selector(build:), @"r");
		NSMenuItem* run = [[NSMenuItem alloc] initWithTitle:@"Build & Run" action:@selector(buildRun:) keyEquivalent:@"R"];
		run.target = target;
		[session addItem:run];
		NSMenuItem* pause = [[NSMenuItem alloc] initWithTitle:@"Pause / Resume" action:@selector(pauseResume:) keyEquivalent:@""];
		pause.target = target;
		[session addItem:pause];
		NSMenuItem* reset = [[NSMenuItem alloc] initWithTitle:@"Reset" action:@selector(resetEmulator:) keyEquivalent:@""];
		reset.target = target;
		[session addItem:reset];

		NSMenuItem* window_item = [[NSMenuItem alloc] initWithTitle:@"Window" action:nil keyEquivalent:@""];
		NSMenu* window_menu = [[NSMenu alloc] initWithTitle:@"Window"];
		[window_menu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
		[window_menu addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
		window_item.submenu = window_menu;
		[main_menu addItem:window_item];
		[app setWindowsMenu:window_menu];

		app.mainMenu = main_menu;
	}
}

} // namespace casioemu::mac

#else

namespace casioemu::mac {
void ConfigureFramelessWindow(SDL_Window*) {}
void SetWindowBackground(SDL_Window*, float, float, float) {}
float TitleBarHeight() { return 0.0f; }
void InstallNativeMenu(const MenuActions&) {}
} // namespace casioemu::mac

#endif
