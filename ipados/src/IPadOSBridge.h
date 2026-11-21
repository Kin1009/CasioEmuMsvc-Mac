#pragma once
// UIKit bridge for the iPadOS build.  All of these entry points are safe to
// call from the SDL/ImGui thread; the Objective-C++ implementation marshals
// UI work onto the main thread when needed.
//
// JIT is never used: AsmJit and JitU8 are excluded from the iPadOS target.

#include <filesystem>
#include <functional>
#include <string>

namespace casioemu::ipados {

/// Called once after SDL_CreateWindow(). Configures the SDL UIView for
/// multitasking / Stage Manager resizing, safe-area layout, trackpad pointer
/// interactions and hardware-keyboard commands.
void ConfigureWindow(void* sdlWindow);

/// Safe-area insets of the current key window, in points (left, top, right,
/// bottom). Refreshed on every frame; zero until the window exists.
void GetSafeAreaInsets(float& left, float& top, float& right, float& bottom);

/// True when the app is running in a resizable split-view / Stage Manager scene.
bool IsMultiTaskingActive();

/// Present UIDocumentPickerViewController. `directories` selects folders
/// instead of files; the callback receives a path copied into the app sandbox.
/// The callback may be invoked asynchronously and always on the main thread.
void PresentDocumentPicker(bool directories,
                           std::function<void(std::filesystem::path)> callback);

/// Bring the in-app Settings ImGui window to the front.
void OpenSettingsWindow();

/// Absolute path to the app's Documents directory (sandbox container).
std::filesystem::path DocumentsDirectory();

/// Pump any deferred main-thread work. Called from the SDL event loop.
void PollMainThread();

} // namespace casioemu::ipados
