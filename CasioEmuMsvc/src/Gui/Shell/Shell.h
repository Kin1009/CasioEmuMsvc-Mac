#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace casioemu::shell {

struct SessionTab {
	std::string id;
	std::string title;
	std::string model;
	std::string rom;
	std::string modelPath;
	bool live = false;
};

enum class LogSeverity { Note = 0, Warning = 1, Error = 2 };

// Called once after the debugger window and ImGui context exist.
void Initialize();
void Shutdown();
// Draws the complete shell (title bar, tabs, sidebar, content, panel, status).
// Called from gui_loop() after ImGui::NewFrame().
void Render();

// Session bookkeeping.  The active session is the one routed to the debugger
// tooling and input.
void SetLiveSession(const std::string& modelPath, const std::string& modelName,
	const std::string& romPath);
void AppendOutput(const std::string& line);
void AppendDiagnostic(LogSeverity severity, const std::string& line);
void NotifyBuildFinished(bool ok);

// Main-loop requests consumed by casioemu.cpp.
bool ConsumeReopenRequest();
bool ConsumeQuitRequest();
const std::string& ConsumeStartupModel();
bool TerminalFocused();

// Native menu / global shortcut entry points.
void RequestOpenModel();
void RequestOpenFolder();
void RequestInstallPlugin();
void ToggleSidebar();
void ToggleBottomPanel();
void ToggleTools();
void SetCalculatorVisible(bool visible);
void SetCalculatorWidth(int width);
bool CalculatorVisible();
void FocusTerminal();
void OpenSettings();
void OpenCommandPalette();
void RequestBuild(bool run);
void TogglePause();
void ResetSession();
void OpenQuickOpen();

std::string WorkspaceFolder();

// Headless verification: writes the last frame's region rects and automated
// overlap/font checks.  Returns false if the report cannot be written.
bool WriteLayoutReport(const std::string& path);

} // namespace casioemu::shell
