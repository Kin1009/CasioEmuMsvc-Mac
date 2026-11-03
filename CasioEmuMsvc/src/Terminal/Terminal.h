#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace casioemu::terminal {

// A small xterm-compatible terminal backed by a real PTY (forkpty).
// It implements the control sequences needed by zsh, git, vim and similar
// programs: cursor movement, erase, SGR 16/256/truecolour, scrolling, alternate
// screen, bracketed paste and window-size propagation.
class Terminal {
public:
	Terminal();
	~Terminal();

	bool Start(const std::string& workingDirectory);
	void Stop();
	bool Running() const { return m_master >= 0; }
	bool Focused() const { return m_focused; }
	void SetFocused(bool focused) { m_focused = focused; }

	// Draws the terminal inside the current child/window and consumes keyboard
	// input while focused.  Returns true when the user clicked the terminal.
	bool Render(const char* id, float height, bool requestFocus);

	const std::string& LastError() const { return m_lastError; }
	const std::string& WorkingDirectory() const { return m_cwd; }

	// Headless self-test helpers: a plain-text view of the visible grid and a
	// way to type a command without an ImGui input queue.
	std::string ScreenText() const;
	void SendLineForTest(const std::string& text);

private:
	struct Cell {
		char32_t ch = U' ';
		uint8_t fg = 0; // 0 = default foreground
		uint8_t bg = 0; // 0 = default background
		bool bold = false;
		bool underline = false;
	};

	using Row = std::vector<Cell>;

	void PumpOutput();
	void ParseInput(const char* data, size_t length);
	void HandleControlSequence(char finalByte);
	void PutCodepoint(char32_t codepoint);
	void NewLine();
	void ScrollUp();
	void ClearCell(Cell& cell);
	void ResizeGrid(int columns, int rows);
	void Send(const char* data, size_t length);
	void SendString(const std::string& text);
	void ProcessKeys();
	uint32_t MapColour(uint8_t index) const;
	void PushScrollback();
	void EnterAlternateScreen(bool enter);

	int m_master = -1;
	int m_pid = 0;
	std::string m_cwd;
	std::string m_lastError;

	std::vector<Row> m_grid;
	std::vector<std::string> m_scrollback;
	int m_scrollOffset = 0;
	int m_columns = 80;
	int m_rows = 24;
	int m_cursorX = 0;
	int m_cursorY = 0;
	bool m_cursorVisible = true;
	bool m_bracketedPaste = false;
	bool m_focused = false;
	bool m_alternateScreen = false;

	// parser state
	std::vector<int> m_params;
	std::string m_paramBuffer;
	enum class ParseState { Ground, Escape, Csi, Osc, OscEscape } m_state = ParseState::Ground;
	bool m_privateSequence = false;

	// SGR state
	uint8_t m_currentFg = 0;
	uint8_t m_currentBg = 0;
	bool m_currentBold = false;
	bool m_currentUnderline = false;

	// saved cursor
	int m_savedX = 0;
	int m_savedY = 0;

	int m_lastColumns = 0;
	int m_lastRows = 0;
	float m_charWidth = 8.0f;
	float m_charHeight = 16.0f;
	std::vector<float> m_charWidths;
};

} // namespace casioemu::terminal
