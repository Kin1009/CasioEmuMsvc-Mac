// iPadOS UIKit bridge.  This file is only compiled into the CASIOEMU_IPADOS
// target.  It deliberately avoids AppKit (not available on iOS) and never uses
// JIT / runtime code generation.

#include "IPadOSBridge.h"
#include "SysDialog.h"

#import <TargetConditionals.h>

#if defined(CASIOEMU_IPADOS) && TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>

#include <SDL.h>
#include <SDL_syswm.h>

#include <cstdlib>
#include <ctime>
#include <mutex>

// Defined in IPadOSKeypad.cpp; keeps Ui.hpp out of this translation unit.
void IPadOSOpenSettingsWindow();

// ---------------------------------------------------------------------------
// Document picker delegate.  The object is retained for the lifetime of the
// process because UIDocumentPickerViewController has a weak delegate.
// ---------------------------------------------------------------------------
@interface CasioDocumentPickerDelegate : NSObject <UIDocumentPickerDelegate>
@property (nonatomic, copy) void (^completion)(NSURL* _Nullable url);
@end

@implementation CasioDocumentPickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController*)controller
didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    (void)controller;
    if (self.completion)
        self.completion(urls.firstObject);
    self.completion = nil;
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
    (void)controller;
    self.completion = nil;
}
@end

namespace {
UIWindow* KeyWindow() {
    if (@available(iOS 15.0, *)) {
        for (UIScene* scene in [UIApplication sharedApplication].connectedScenes) {
            if (![scene isKindOfClass:[UIWindowScene class]] ||
                scene.activationState != UISceneActivationStateForegroundActive)
                continue;
            for (UIWindow* win in ((UIWindowScene*)scene).windows)
                if (win.isKeyWindow) return win;
        }
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return [UIApplication sharedApplication].keyWindow;
#pragma clang diagnostic pop
}

UIViewController* TopViewController() {
    UIViewController* vc = KeyWindow().rootViewController;
    while (vc.presentedViewController)
        vc = vc.presentedViewController;
    return vc;
}

float g_safe_left = 0, g_safe_top = 0, g_safe_right = 0, g_safe_bottom = 0;
bool g_multitasking = false;

CasioDocumentPickerDelegate* PickerDelegate() {
    static CasioDocumentPickerDelegate* delegate = [CasioDocumentPickerDelegate new];
    return delegate;
}

std::filesystem::path CopyToDocuments(NSURL* url) {
    if (!url) return {};
    const BOOL secured = [url startAccessingSecurityScopedResource];
    @autoreleasepool {
        NSFileManager* fm = [NSFileManager defaultManager];
        NSURL* docs = [[fm URLsForDirectory:NSDocumentDirectory
                                  inDomains:NSUserDomainMask] firstObject];
        NSURL* imports = [docs URLByAppendingPathComponent:@"Imports" isDirectory:YES];
        [fm createDirectoryAtURL:imports
     withIntermediateDirectories:YES
                      attributes:nil
                           error:nil];
        NSString* name = url.lastPathComponent ?: @"imported.dat";
        NSURL* destination = [imports URLByAppendingPathComponent:name];
        // Avoid overwriting an earlier import with the same name.
        NSDateFormatter* fmt = [NSDateFormatter new];
        fmt.dateFormat = @"yyyyMMdd-HHmmss";
        NSString* stamp = [fmt stringFromDate:[NSDate date]];
        NSString* stem = name.stringByDeletingPathExtension;
        NSString* ext = name.pathExtension;
        NSString* unique = ext.length
            ? [NSString stringWithFormat:@"%@-%@.%@", stem, stamp, ext]
            : [NSString stringWithFormat:@"%@-%@", stem, stamp];
        destination = [imports URLByAppendingPathComponent:unique];

        NSError* error = nil;
        if (![fm copyItemAtURL:url toURL:destination error:&error]) {
            NSLog(@"[ipados] import copy failed: %@", error);
            if (secured) [url stopAccessingSecurityScopedResource];
            return {};
        }
        [destination setResourceValue:@YES
                              forKey:NSURLIsExcludedFromBackupKey
                               error:nil];
        if (secured) [url stopAccessingSecurityScopedResource];
        return std::filesystem::path([destination.path UTF8String]);
    }
}

// ---------------------------------------------------------------------------
// SystemDialogs implementation used by the existing ImGui file dialogs.
// ---------------------------------------------------------------------------
void Present(bool directories, std::function<void(std::filesystem::path)> callback) {
    @autoreleasepool {
        UIDocumentPickerViewController* picker = nil;
        if (directories) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            picker = [[UIDocumentPickerViewController alloc]
                initWithDocumentTypes:@[ @"public.folder" ]
                               inMode:UIDocumentPickerModeImport];
#pragma clang diagnostic pop
        } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            picker = [[UIDocumentPickerViewController alloc]
                initWithDocumentTypes:@[ @"public.data",
                                         @"public.item",
                                         @"com.pkware.zip-archive",
                                         @"com.casioemu.package",
                                         @"com.casioemu.plugin" ]
                               inMode:UIDocumentPickerModeImport];
#pragma clang diagnostic pop
        }
        picker.allowsMultipleSelection = NO;
        picker.modalPresentationStyle = UIModalPresentationFormSheet;
        PickerDelegate().completion = ^(NSURL* url) {
            std::filesystem::path path = CopyToDocuments(url);
            if (callback && !path.empty()) callback(path);
        };
        picker.delegate = PickerDelegate();
        UIViewController* vc = TopViewController();
        if (vc) {
            [vc presentViewController:picker animated:YES completion:nil];
        } else {
            NSLog(@"[ipados] no root view controller for document picker");
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public C++ API
// ---------------------------------------------------------------------------
namespace casioemu::ipados {

void ConfigureWindow(void* sdlWindow) {
    @autoreleasepool {
        SDL_Window* window = static_cast<SDL_Window*>(sdlWindow);
        if (window) {
            SDL_SysWMinfo info;
            SDL_VERSION(&info.version);
            if (SDL_GetWindowWMInfo(window, &info) && info.subsystem == SDL_SYSWM_UIKIT) {
                UIWindow* uiWindow = info.info.uikit.window;
                if (@available(iOS 13.0, *)) {
                    UIWindowScene* scene = uiWindow.windowScene;
                    scene.title = @"CasioEmuMsvc";
                    // Minimum size keeps the emulator panel usable in Slide Over
                    // and in a narrow Stage Manager pane; no maximum is set so
                    // the app can grow to the full screen.
                    scene.sizeRestrictions.minimumSize = CGSizeMake(640, 480);
                    scene.sizeRestrictions.maximumSize = CGSizeMake(CGFLOAT_MAX, CGFLOAT_MAX);
                    g_multitasking = YES;
                }
                if (@available(iOS 13.4, *)) {
                    UIView* view = uiWindow.rootViewController.view;
                    if (view) {
                        UIPointerInteraction* pointer =
                            [[UIPointerInteraction alloc] initWithDelegate:nil];
                        [view addInteraction:pointer];
                    }
                }
            }
        }

        // Keyboard shortcuts for the hardware keyboard / Magic Keyboard trackpad.
        // UIKeyCommand is handled by SDL as regular SDL_KEYDOWN events as well;
        // these commands cover the iPadOS system gestures (Cmd+Q is reserved by
        // the OS and is intentionally not registered).
        UIViewController* vc = TopViewController();
        if (vc) {
            NSArray<NSString*>* titles = @[ @"Toggle Sidebar", @"Settings", @"Reset" ];
            NSArray<NSString*>* keys = @[ @"b", @",", @"r" ];
            for (NSUInteger i = 0; i < titles.count; ++i) {
                UIKeyCommand* cmd = [UIKeyCommand keyCommandWithInput:keys[i]
                                                       modifierFlags:UIKeyModifierCommand
                                                              action:@selector(ipadosKeyCommand:)
                                                  discoverabilityTitle:titles[i]];
                [vc addKeyCommand:cmd];
            }
        }

        UIEdgeInsets insets = KeyWindow().safeAreaInsets;
        g_safe_left = insets.left;
        g_safe_top = insets.top;
        g_safe_right = insets.right;
        g_safe_bottom = insets.bottom;
    }
}

void GetSafeAreaInsets(float& left, float& top, float& right, float& bottom) {
    left = g_safe_left;
    top = g_safe_top;
    right = g_safe_right;
    bottom = g_safe_bottom;
}

bool IsMultiTaskingActive() {
    return g_multitasking;
}

void PresentDocumentPicker(bool directories,
                           std::function<void(std::filesystem::path)> callback) {
    Present(directories, std::move(callback));
}

void OpenSettingsWindow() {
    ::IPadOSOpenSettingsWindow();
}

std::filesystem::path DocumentsDirectory() {
    @autoreleasepool {
        NSURL* docs = [[NSFileManager defaultManager] URLsForDirectory:NSDocumentDirectory
                                                             inDomains:NSUserDomainMask].firstObject;
        return docs ? std::filesystem::path([docs.path UTF8String]) : std::filesystem::path{};
    }
}

void PollMainThread() {
    @autoreleasepool {
        UIEdgeInsets insets = KeyWindow().safeAreaInsets;
        g_safe_left = insets.left;
        g_safe_top = insets.top;
        g_safe_right = insets.right;
        g_safe_bottom = insets.bottom;
    }
}

} // namespace casioemu::ipados

// SystemDialogs is the cross-platform hook used by the existing file dialogs.
void SystemDialogs::OpenFileDialog(std::function<void(std::filesystem::path)> callback) {
    casioemu::ipados::PresentDocumentPicker(false, std::move(callback));
}
void SystemDialogs::SaveFileDialog(std::string preferred_name,
                                   std::function<void(std::filesystem::path)> callback) {
    (void)preferred_name;
    casioemu::ipados::PresentDocumentPicker(false, std::move(callback));
}
void SystemDialogs::OpenFolderDialog(std::function<void(std::filesystem::path)> callback) {
    casioemu::ipados::PresentDocumentPicker(true, std::move(callback));
}
void SystemDialogs::SaveFolderDialog(std::function<void(std::filesystem::path)> callback) {
    casioemu::ipados::PresentDocumentPicker(true, std::move(callback));
}

// UIKeyCommand target.  The selector is resolved dynamically so no category on
// SDL's internal view controller is required.
@interface NSObject (CasioEmuIPadOSKeys)
- (void)ipadosKeyCommand:(UIKeyCommand*)command;
@end

@implementation NSObject (CasioEmuIPadOSKeys)
- (void)ipadosKeyCommand:(UIKeyCommand*)command {
    // SDL already receives the hardware key as a normal SDL_KEYDOWN.  This
    // method exists so UIKeyCommand has a valid target; logging keeps the
    // action discoverable in iPadOS keyboard shortcuts.
    NSLog(@"[ipados] key command: %@", command.input);
}
@end

#else // non-iPad fallbacks (kept so the file can be compiled on macOS in tests)

namespace casioemu::ipados {
void ConfigureWindow(void*) {}
void GetSafeAreaInsets(float& l, float& t, float& r, float& b) { l = t = r = b = 0; }
bool IsMultiTaskingActive() { return false; }
void PresentDocumentPicker(bool, std::function<void(std::filesystem::path)>) {}
void OpenSettingsWindow() {}
std::filesystem::path DocumentsDirectory() { return {}; }
void PollMainThread() {}
} // namespace casioemu::ipados

void SystemDialogs::OpenFileDialog(std::function<void(std::filesystem::path)>) {}
void SystemDialogs::SaveFileDialog(std::string, std::function<void(std::filesystem::path)>) {}
void SystemDialogs::OpenFolderDialog(std::function<void(std::filesystem::path)>) {}
void SystemDialogs::SaveFolderDialog(std::function<void(std::filesystem::path)>) {}

#endif
