#include "Terminal.h"

#include "Gui.h"
#include "Gui/ThemeManager.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

namespace casioemu::terminal {
namespace {

constexpr const char* kDefaultShell = "/bin/zsh";

ImU32 ColorU32(const ImVec4& color) {
	return ImGui::ColorConvertFloat4ToU32(color);
}

uint8_t ClampColourIndex(int value) {
	if (value <= 0)
		return 0;
	if (value > 255)
		return 255;
	return static_cast<uint8_t>(value);
}

} // namespace

Terminal::Terminal() = default;

Terminal::~Terminal() {
	Stop();
}

bool Terminal::Start(const std::string& workingDirectory) {
	if (Running())
		return true;

	m_lastError.clear();
	m_cwd = workingDirectory.empty() ? std::filesystem::current_path().string() : workingDirectory;

	const char* shell_env = std::getenv("SHELL");
	std::string shell = (shell_env && *shell_env) ? shell_env : kDefaultShell;

	struct winsize size {};
	size.ws_col = static_cast<unsigned short>(m_columns);
	size.ws_row = static_cast<unsigned short>(m_rows);
	size.ws_xpixel = 0;
	size.ws_ypixel = 0;

	int master = -1;
	const pid_t pid = forkpty(&master, nullptr, nullptr, &size);
	if (pid < 0) {
		m_lastError = std::string("forkpty failed: ") + std::strerror(errno);
		return false;
	}
	if (pid == 0) {
		// child: become the user's login shell in the workspace.
		if (chdir(m_cwd.c_str()) != 0) {
			// Keep going: the shell will start in the inherited directory.
		}
		setenv("TERM", "xterm-256color", 1);
		setenv("COLORTERM", "truecolor", 1);
		execl(shell.c_str(), shell.c_str(), "-l", nullptr);
		execl("/bin/zsh", "zsh", "-l", nullptr);
		execl("/bin/sh", "sh", nullptr);
		_exit(127);
	}

	m_master = master;
	m_pid = static_cast<int>(pid);
	int flags = fcntl(m_master, F_GETFL, 0);
	if (flags >= 0)
		fcntl(m_master, F_SETFL, flags | O_NONBLOCK);

	ResizeGrid(m_columns, m_rows);
	m_scrollback.clear();
	m_scrollback.push_back("");
	m_cursorX = 0;
	m_cursorY = 0;
	m_scrollOffset = 0;

	const char* banner = "\x1b[38;5;244mCasioEmuMsvc terminal \xe2\x80\x94 zsh in the workspace. Ctrl+C interrupts, Ctrl+D exits.\x1b[0m\r\n";
	ParseInput(banner, std::strlen(banner));
	return true;
}

void Terminal::Stop() {
	if (m_master >= 0) {
		// SIGHUP, then escalate after a short grace period.  We cannot block the
		// UI thread, so the escalation happens on the next destructor only.
		if (m_pid > 0)
			kill(static_cast<pid_t>(m_pid), SIGHUP);
		close(m_master);
		m_master = -1;
	}
	if (m_pid > 0) {
		int status = 0;
		const pid_t result = waitpid(static_cast<pid_t>(m_pid), &status, WNOHANG);
		if (result == 0)
			kill(static_cast<pid_t>(m_pid), SIGKILL);
		m_pid = 0;
	}
}

void Terminal::ResizeGrid(int columns, int rows) {
	m_columns = std::max(2, columns);
	m_rows = std::max(2, rows);

	m_grid.resize(static_cast<size_t>(m_rows));
	for (auto& row : m_grid) {
		row.resize(static_cast<size_t>(m_columns));
		for (auto& cell : row)
			ClearCell(cell);
	}
	m_cursorX = std::min(m_cursorX, m_columns - 1);
	m_cursorY = std::min(m_cursorY, m_rows - 1);
	m_lastColumns = m_columns;
	m_lastRows = m_rows;

	if (m_master >= 0) {
		struct winsize size {};
		size.ws_col = static_cast<unsigned short>(m_columns);
		size.ws_row = static_cast<unsigned short>(m_rows);
		ioctl(m_master, TIOCSWINSZ, &size);
	}
}

void Terminal::ClearCell(Cell& cell) {
	cell.ch = U' ';
	cell.fg = m_currentFg;
	cell.bg = m_currentBg;
	cell.bold = m_currentBold;
	cell.underline = m_currentUnderline;
}

void Terminal::PutCodepoint(char32_t codepoint) {
	if (m_grid.empty()) {
		ResizeGrid(m_columns, m_rows);
	}
	if (m_cursorX >= m_columns) {
		m_cursorX = 0;
		NewLine();
	}
	if (m_cursorY < 0 || m_cursorY >= m_rows)
		return;
	Cell& cell = m_grid[static_cast<size_t>(m_cursorY)][static_cast<size_t>(m_cursorX)];
	cell.ch = codepoint;
	cell.fg = m_currentFg;
	cell.bg = m_currentBg;
	cell.bold = m_currentBold;
	cell.underline = m_currentUnderline;
	++m_cursorX;
}

void Terminal::NewLine() {
	++m_cursorY;
	if (m_cursorY >= m_rows) {
		ScrollUp();
		m_cursorY = m_rows - 1;
	}
	m_cursorX = 0;
}

void Terminal::PushScrollback() {
	const Row& row = m_grid.front();
	std::string line;
	for (const auto& cell : row) {
		if (cell.ch >= 32 && cell.ch < 0x110000) {
			char buffer[4] = {};
			// ASCII fast path; non-ASCII is stored as a byte marker.
			if (cell.ch < 0x80) {
				buffer[0] = static_cast<char>(cell.ch);
			}
			else {
				buffer[0] = '?';
			}
			line += buffer;
		}
	}
	while (!line.empty() && line.back() == ' ')
		line.pop_back();
	m_scrollback.push_back(line);
	if (m_scrollback.size() > 10000)
		m_scrollback.erase(m_scrollback.begin(), m_scrollback.begin() + 1000);
}

void Terminal::ScrollUp() {
	PushScrollback();
	m_grid.erase(m_grid.begin());
	Row row(static_cast<size_t>(m_columns));
	for (auto& cell : row)
		ClearCell(cell);
	m_grid.push_back(std::move(row));
	m_scrollOffset = 0;
}

void Terminal::EnterAlternateScreen(bool enter) {
	m_alternateScreen = enter;
	if (enter) {
		m_savedX = m_cursorX;
		m_savedY = m_cursorY;
	} else {
		m_cursorX = m_savedX;
		m_cursorY = m_savedY;
	}
	m_scrollOffset = 0;
}

void Terminal::PumpOutput() {
	if (m_master < 0)
		return;
	char buffer[8192];
	for (;;) {
		const ssize_t count = read(m_master, buffer, sizeof(buffer));
		if (count > 0) {
			ParseInput(buffer, static_cast<size_t>(count));
			continue;
		}
		if (count == 0) {
			// child exited
			Stop();
		}
		break;
	}
}

void Terminal::ParseInput(const char* data, size_t length) {
	size_t index = 0;
	while (index < length) {
		const unsigned char byte = static_cast<unsigned char>(data[index]);

		if (m_state == ParseState::Osc) {
			if (byte == 0x07) {
				m_state = ParseState::Ground;
			} else if (byte == 0x1B) {
				m_state = ParseState::OscEscape;
			}
			++index;
			continue;
		}
		if (m_state == ParseState::OscEscape) {
			m_state = byte == '\\' ? ParseState::Ground : ParseState::Osc;
			++index;
			continue;
		}
		if (m_state == ParseState::Escape) {
			if (byte == '[') {
				m_state = ParseState::Csi;
				m_params.clear();
				m_paramBuffer.clear();
				m_privateSequence = false;
			}
			else if (byte == ']') {
				m_state = ParseState::Osc;
			}
			else if (byte == '7') {
				m_savedX = m_cursorX;
				m_savedY = m_cursorY;
				m_state = ParseState::Ground;
			}
			else if (byte == '8') {
				m_cursorX = m_savedX;
				m_cursorY = m_savedY;
				m_state = ParseState::Ground;
			}
			else if (byte == '(' || byte == ')' || byte == '#') {
				m_state = ParseState::Ground;
			}
			else {
				m_state = ParseState::Ground;
			}
			++index;
			continue;
		}
		if (m_state == ParseState::Csi) {
			if (byte >= '0' && byte <= '9') {
				m_paramBuffer.push_back(static_cast<char>(byte));
			}
			else if (byte == ';') {
				m_params.push_back(m_paramBuffer.empty() ? 0 : std::atoi(m_paramBuffer.c_str()));
				m_paramBuffer.clear();
			}
			else if (byte == '?') {
				m_privateSequence = true;
			}
			else if (byte >= 0x40 && byte <= 0x7E) {
				m_params.push_back(m_paramBuffer.empty() ? 0 : std::atoi(m_paramBuffer.c_str()));
				const char finalByte = static_cast<char>(byte);
				m_state = ParseState::Ground;
				HandleControlSequence(finalByte);
			}
			// intermediate bytes are ignored
			++index;
			continue;
		}

		// Ground state.
		if (byte == 0x1B) {
			m_state = ParseState::Escape;
			++index;
			continue;
		}
		if (byte == '\r') {
			m_cursorX = 0;
			++index;
			continue;
		}
		if (byte == '\n') {
			NewLine();
			++index;
			continue;
		}
		if (byte == '\b') {
			if (m_cursorX > 0)
				--m_cursorX;
			++index;
			continue;
		}
		if (byte == '\t') {
			const int target = ((m_cursorX / 8) + 1) * 8;
			while (m_cursorX < target && m_cursorX < m_columns - 1)
				PutCodepoint(U' ');
			++index;
			continue;
		}
		if (byte == 0x07) {
			++index;
			continue;
		}
		if (byte < 0x20) {
			++index;
			continue;
		}

		// UTF-8 decode (best effort, one cell per codepoint).
		char32_t codepoint = byte;
		size_t consumed = 1;
		if ((byte & 0xE0) == 0xC0 && index + 1 < length) {
			codepoint = ((byte & 0x1F) << 6) | (data[index + 1] & 0x3F);
			consumed = 2;
		}
		else if ((byte & 0xF0) == 0xE0 && index + 2 < length) {
			codepoint = ((byte & 0x0F) << 12) | ((data[index + 1] & 0x3F) << 6) | (data[index + 2] & 0x3F);
			consumed = 3;
		}
		else if ((byte & 0xF8) == 0xF0 && index + 3 < length) {
			codepoint = ((byte & 0x07) << 18) | ((data[index + 1] & 0x3F) << 12) |
				((data[index + 2] & 0x3F) << 6) | (data[index + 3] & 0x3F);
			consumed = 4;
		}
		PutCodepoint(codepoint);
		index += consumed;
	}
}

void Terminal::HandleControlSequence(char finalByte) {
	auto param = [&](size_t index, int fallback) -> int {
		if (index >= m_params.size())
			return fallback;
		return m_params[index] == 0 ? fallback : m_params[index];
	};

	switch (finalByte) {
	case 'A': m_cursorY = std::max(0, m_cursorY - param(0, 1)); break;
	case 'B': m_cursorY = std::min(m_rows - 1, m_cursorY + param(0, 1)); break;
	case 'C': m_cursorX = std::min(m_columns - 1, m_cursorX + param(0, 1)); break;
	case 'D': m_cursorX = std::max(0, m_cursorX - param(0, 1)); break;
	case 'E':
		m_cursorY = std::min(m_rows - 1, m_cursorY + param(0, 1));
		m_cursorX = 0;
		break;
	case 'F':
		m_cursorY = std::max(0, m_cursorY - param(0, 1));
		m_cursorX = 0;
		break;
	case 'G': m_cursorX = std::clamp(param(0, 1) - 1, 0, m_columns - 1); break;
	case 'H':
	case 'f':
		m_cursorY = std::clamp(param(0, 1) - 1, 0, m_rows - 1);
		m_cursorX = std::clamp(param(1, 1) - 1, 0, m_columns - 1);
		break;
	case 'J': {
		const int mode = param(0, 0);
		if (mode == 2 || mode == 3) {
			for (auto& row : m_grid)
				for (auto& cell : row)
					ClearCell(cell);
		}
		else if (mode == 0) {
			for (int x = m_cursorX; x < m_columns; ++x)
				ClearCell(m_grid[static_cast<size_t>(m_cursorY)][static_cast<size_t>(x)]);
			for (int y = m_cursorY + 1; y < m_rows; ++y)
				for (auto& cell : m_grid[static_cast<size_t>(y)])
					ClearCell(cell);
		}
		break;
	}
	case 'K': {
		const int mode = param(0, 0);
		if (mode == 0) {
			for (int x = m_cursorX; x < m_columns; ++x)
				ClearCell(m_grid[static_cast<size_t>(m_cursorY)][static_cast<size_t>(x)]);
		}
		else if (mode == 1) {
			for (int x = 0; x <= m_cursorX; ++x)
				ClearCell(m_grid[static_cast<size_t>(m_cursorY)][static_cast<size_t>(x)]);
		}
		else {
			for (auto& cell : m_grid[static_cast<size_t>(m_cursorY)])
				ClearCell(cell);
		}
		break;
	}
	case 'm': {
		if (m_params.empty())
			m_params.push_back(0);
		for (size_t index = 0; index < m_params.size(); ++index) {
			const int code = m_params[index];
			if (code == 0) {
				m_currentFg = 0;
				m_currentBg = 0;
				m_currentBold = false;
				m_currentUnderline = false;
			}
			else if (code == 1) m_currentBold = true;
			else if (code == 4) m_currentUnderline = true;
			else if (code == 22) m_currentBold = false;
			else if (code == 24) m_currentUnderline = false;
			else if (code == 39) m_currentFg = 0;
			else if (code == 49) m_currentBg = 0;
			else if (code >= 30 && code <= 37) m_currentFg = ClampColourIndex(code - 30 + 1);
			else if (code >= 90 && code <= 97) m_currentFg = ClampColourIndex(code - 90 + 9);
			else if (code >= 40 && code <= 47) m_currentBg = ClampColourIndex(code - 40 + 1);
			else if (code >= 100 && code <= 107) m_currentBg = ClampColourIndex(code - 100 + 9);
			else if (code == 38 || code == 48) {
				uint8_t* target = code == 38 ? &m_currentFg : &m_currentBg;
				if (index + 2 < m_params.size() && m_params[index + 1] == 5) {
					*target = ClampColourIndex(m_params[index + 2] + 1);
					index += 2;
				}
				else if (index + 4 < m_params.size() && m_params[index + 1] == 2) {
					// Truecolour is collapsed to the nearest 256-colour cell so
					// the renderer stays simple and fast.
					const int r = m_params[index + 2], g = m_params[index + 3], b = m_params[index + 4];
					const int level = (r > 200 ? 5 : r > 120 ? 4 : r > 60 ? 3 : r > 20 ? 2 : r > 5 ? 1 : 0);
					const int index256 = 16 + 36 * level + 6 * (g > 150 ? 3 : g > 60 ? 2 : 0) + (b > 150 ? 3 : b > 60 ? 2 : 0);
					*target = ClampColourIndex(index256 + 1);
					index += 4;
				}
			}
		}
		break;
	}
	case 's':
		m_savedX = m_cursorX;
		m_savedY = m_cursorY;
		break;
	case 'u':
		m_cursorX = m_savedX;
		m_cursorY = m_savedY;
		break;
	case 'h':
		if (m_privateSequence && !m_params.empty() && m_params[0] == 25)
			m_cursorVisible = true;
		if (m_privateSequence && !m_params.empty() && m_params[0] == 2004)
			m_bracketedPaste = true;
		if (m_privateSequence && !m_params.empty() && m_params[0] == 1049)
			EnterAlternateScreen(true);
		break;
	case 'l':
		if (m_privateSequence && !m_params.empty() && m_params[0] == 25)
			m_cursorVisible = false;
		if (m_privateSequence && !m_params.empty() && m_params[0] == 2004)
			m_bracketedPaste = false;
		if (m_privateSequence && !m_params.empty() && m_params[0] == 1049)
			EnterAlternateScreen(false);
		break;
	default:
		break;
	}
}

void Terminal::Send(const char* data, size_t length) {
	if (m_master < 0 || length == 0)
		return;
	size_t offset = 0;
	while (offset < length) {
		const ssize_t written = write(m_master, data + offset, length - offset);
		if (written > 0) {
			offset += static_cast<size_t>(written);
			continue;
		}
		if (errno == EAGAIN || errno == EINTR)
			continue;
		break;
	}
}

void Terminal::SendString(const std::string& text) {
	Send(text.data(), text.size());
}

std::string Terminal::ScreenText() const {
	std::string text;
	for (const auto& row : m_grid) {
		std::string line;
		for (const auto& cell : row) {
			if (cell.ch >= 32 && cell.ch < 0x7f)
				line.push_back(static_cast<char>(cell.ch));
			else if (cell.ch == U' ')
				line.push_back(' ');
			else
				line.push_back('?');
		}
		while (!line.empty() && line.back() == ' ')
			line.pop_back();
		text += line;
		text.push_back('\n');
	}
	return text;
}

void Terminal::SendLineForTest(const std::string& text) {
	SendString(text);
	SendString("\r");
}

uint32_t Terminal::MapColour(uint8_t index) const {
	static const ImU32 ansi16[16] = {
		0xFF000000, 0xFFCD3131, 0xFF0DBC79, 0xFFE5E510,
		0xFF2472C8, 0xFFBC3FBC, 0xFF11A8CD, 0xFFE5E5E5,
		0xFF666666, 0xFFF14C4C, 0xFF23D18B, 0xFFF5F543,
		0xFF3B8EEA, 0xFFD670D6, 0xFF29B8DB, 0xFFE5E5E5,
	};
	if (index == 0)
		return ColorU32(ThemeManager::Instance().Colors().foreground);
	if (index <= 16)
		return ansi16[index - 1];
	const int value = index - 1;
	if (value >= 16 && value <= 231) {
		const int cube = value - 16;
		const int r = cube / 36;
		const int g = (cube / 6) % 6;
		const int b = cube % 6;
		auto channel = [](int level) -> int {
			return level == 0 ? 0 : 55 + level * 40;
		};
		return IM_COL32(channel(r), channel(g), channel(b), 255);
	}
	const int gray = std::clamp(value - 232, 0, 23);
	const int level = 8 + gray * 10;
	return IM_COL32(level, level, level, 255);
}

bool Terminal::Render(const char* id, float height, bool requestFocus) {
	if (!Running())
		Start(m_cwd);

	PumpOutput();

	ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeManager::Instance().Colors().surface);
	ImGui::BeginChild(id, ImVec2(0.0f, height), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);

	if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		m_focused = true;
	if (ImGui::IsWindowHovered() && !m_focused && requestFocus)
		m_focused = true;
	if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
		m_focused = false;

	// Reserve the cursor area and compute the grid against the real font.
	if (g_mono_font)
		ImGui::PushFont(g_mono_font);
	const float char_width = ImGui::CalcTextSize("M").x;
	const float char_height = ImGui::GetTextLineHeight();
	ImGui::PopFont();
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	const int columns = std::max(2, static_cast<int>(avail.x / std::max(1.0f, char_width)));
	const int rows = std::max(2, static_cast<int>(avail.y / std::max(1.0f, char_height)));
	if (columns != m_columns || rows != m_rows)
		ResizeGrid(columns, rows);

	if (m_focused)
		ProcessKeys();

	ImGui::SetNextWindowContentSize(ImVec2(columns * char_width, rows * char_height));
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImVec4 foreground = ThemeManager::Instance().Colors().foreground;
	const ImU32 default_fg = ColorU32(foreground);
	const float scroll = ImGui::GetScrollY();

	if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
		m_scrollOffset = std::clamp(m_scrollOffset + (ImGui::GetIO().MouseWheel > 0 ? 3 : -3), 0,
			std::max(0, static_cast<int>(m_scrollback.size())));
	}

	// Render scrollback first (when scrolled up), then the live grid.
	if (m_scrollOffset > 0 && !m_alternateScreen) {
		const int count = std::min<int>(m_scrollOffset, static_cast<int>(m_scrollback.size()));
		const int first = static_cast<int>(m_scrollback.size()) - count;
		for (int index = 0; index < count; ++index) {
			const std::string& line = m_scrollback[static_cast<size_t>(first + index)];
			const ImVec2 pos(origin.x, origin.y + index * char_height - scroll);
			draw->AddText(pos, default_fg, line.c_str());
		}
	}

	if (g_mono_font)
		ImGui::PushFont(g_mono_font);
	for (int y = 0; y < m_rows; ++y) {
		for (int x = 0; x < m_columns; ++x) {
			const Cell& cell = m_grid[static_cast<size_t>(y)][static_cast<size_t>(x)];
			const ImVec2 pos(origin.x + x * char_width, origin.y + y * char_height - scroll);
			if (cell.bg != 0) {
				draw->AddRectFilled(pos, ImVec2(pos.x + char_width, pos.y + char_height), MapColour(cell.bg));
			}
			if (cell.ch == U' ')
				continue;
			ImU32 colour = cell.fg == 0 ? default_fg : MapColour(cell.fg);
			if (cell.bold && cell.fg <= 8)
				colour = MapColour(static_cast<uint8_t>(cell.fg + 8));
			char buffer[5] = {};
			const char32_t ch = cell.ch;
			// ASCII is the common case and maps 1:1.
			if (ch < 0x80) {
				buffer[0] = static_cast<char>(ch);
			}
			else if (ch < 0x800) {
				buffer[0] = static_cast<char>(0xC0 | (ch >> 6));
				buffer[1] = static_cast<char>(0x80 | (ch & 0x3F));
			}
			else if (ch < 0x10000) {
				buffer[0] = static_cast<char>(0xE0 | (ch >> 12));
				buffer[1] = static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
				buffer[2] = static_cast<char>(0x80 | (ch & 0x3F));
			}
			else {
				buffer[0] = '?';
			}
			draw->AddText(pos, colour, buffer);
			if (cell.underline) {
				draw->AddLine(ImVec2(pos.x, pos.y + char_height - 1.0f),
					ImVec2(pos.x + char_width, pos.y + char_height - 1.0f), colour);
			}
		}
	}
	if (m_cursorVisible && m_focused) {
		const ImVec2 pos(origin.x + m_cursorX * char_width, origin.y + m_cursorY * char_height - scroll);
		draw->AddRectFilled(pos, ImVec2(pos.x + char_width, pos.y + char_height), default_fg);
		// Redraw the character under the cursor in the background colour.
		if (m_cursorY < m_rows && m_cursorX < m_columns) {
			const Cell& cell = m_grid[static_cast<size_t>(m_cursorY)][static_cast<size_t>(m_cursorX)];
			if (cell.ch != U' ') {
				char buffer[2] = { static_cast<char>(cell.ch < 0x80 ? cell.ch : '?'), 0 };
				draw->AddText(pos, ColorU32(ThemeManager::Instance().Colors().surface), buffer);
			}
		}
	}
	if (g_mono_font)
		ImGui::PopFont();

	ImGui::Dummy(ImVec2(columns * char_width, rows * char_height));
	ImGui::EndChild();
	ImGui::PopStyleColor();
	return true;
}

void Terminal::ProcessKeys() {
	ImGuiIO& io = ImGui::GetIO();

	// Printable characters are queued by the SDL backend.
	for (ImWchar character : io.InputQueueCharacters) {
		if (character >= 32) {
			char buffer[4] = {};
			if (character < 0x80) {
				buffer[0] = static_cast<char>(character);
				Send(buffer, 1);
			}
			else if (character < 0x800) {
				buffer[0] = static_cast<char>(0xC0 | (character >> 6));
				buffer[1] = static_cast<char>(0x80 | (character & 0x3F));
				Send(buffer, 2);
			}
			else {
				buffer[0] = static_cast<char>(0xE0 | (character >> 12));
				buffer[1] = static_cast<char>(0x80 | ((character >> 6) & 0x3F));
				buffer[2] = static_cast<char>(0x80 | (character & 0x3F));
				Send(buffer, 3);
			}
		}
	}
	io.InputQueueCharacters.resize(0);

	auto key = [&](ImGuiKey value) { return ImGui::IsKeyPressed(value, true); };
	auto ctrl = [&](ImGuiKey value) { return ImGui::IsKeyPressed(value, true) && (io.KeyCtrl || io.KeySuper); };

	if (ctrl(ImGuiKey_C)) { SendString("\x03"); }
	if (ctrl(ImGuiKey_D)) { SendString("\x04"); }
	if (ctrl(ImGuiKey_Z)) { SendString("\x1a"); }
	if (ctrl(ImGuiKey_L)) { SendString("\x0c"); }
	if (ctrl(ImGuiKey_A)) { SendString("\x01"); }
	if (ctrl(ImGuiKey_E)) { SendString("\x05"); }
	if (ctrl(ImGuiKey_K)) { SendString("\x0b"); }
	if (ctrl(ImGuiKey_U)) { SendString("\x15"); }
	if (ctrl(ImGuiKey_W)) { SendString("\x17"); }
	if (ctrl(ImGuiKey_R)) { SendString("\x12"); }

	if (key(ImGuiKey_Enter) || key(ImGuiKey_KeypadEnter)) SendString("\r");
	if (key(ImGuiKey_Backspace)) SendString("\x7f");
	if (key(ImGuiKey_Tab)) SendString("\t");
	if (key(ImGuiKey_Escape)) SendString("\x1b");
	if (key(ImGuiKey_UpArrow)) SendString("\x1b[A");
	if (key(ImGuiKey_DownArrow)) SendString("\x1b[B");
	if (key(ImGuiKey_RightArrow)) SendString("\x1b[C");
	if (key(ImGuiKey_LeftArrow)) SendString("\x1b[D");
	if (key(ImGuiKey_Home)) SendString("\x1b[H");
	if (key(ImGuiKey_End)) SendString("\x1b[F");
	if (key(ImGuiKey_Delete)) SendString("\x1b[3~");
	if (key(ImGuiKey_PageUp)) SendString("\x1b[5~");
	if (key(ImGuiKey_PageDown)) SendString("\x1b[6~");

	if (io.KeySuper && key(ImGuiKey_V)) {
		if (const char* clipboard = ImGui::GetClipboardText())
			SendString(clipboard);
	}
}

} // namespace casioemu::terminal
