// CwModeWindow.h - "Add Modes" panel for the fx-580VN X / fx-880BTG.
//
// The panel uses the in-process cwmode library (third_party/wiz) to add modes
// to the stock firmware and reboot the running model on the patched image.
// The stock ROM file is never modified.
#pragma once

class UIWindow;

UIWindow* CreateCwModeWindow();
