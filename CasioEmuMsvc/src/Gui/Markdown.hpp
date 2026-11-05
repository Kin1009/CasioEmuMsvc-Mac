#pragma once
// Minimal markdown renderer for the Documentation pane.
//
// Supports headings, paragraphs, bold/italic, inline code, fenced code blocks,
// bullet/numbered lists, blockquotes, horizontal rules, links and simple pipe
// tables.  It is deliberately small and self-contained; unknown syntax is shown
// as plain text.

#include "imgui/imgui.h"
#include "ThemeManager.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <SDL.h>

namespace casioemu::markdown {

namespace detail {

inline std::string Trim(const std::string& text) {
	const auto first = text.find_first_not_of(" \t\r");
	if (first == std::string::npos)
		return {};
	const auto last = text.find_last_not_of(" \t\r");
	return text.substr(first, last - first + 1);
}

inline bool StartsWith(const std::string& text, const char* prefix) {
	return text.rfind(prefix, 0) == 0;
}

inline std::string Lowercase(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return text;
}

inline std::filesystem::path& BaseDirectory() {
	static std::filesystem::path directory;
	return directory;
}

inline void SetBaseDirectory(const std::filesystem::path& directory) {
	BaseDirectory() = directory;
}

inline std::string EncodeFileUrl(const std::filesystem::path& path) {
	std::string text = path.string();
	std::string encoded;
	encoded.reserve(text.size() + 8);
	for (char ch : text) {
		if (ch == ' ')
			encoded += "%20";
		else
			encoded += ch;
	}
	return "file://" + encoded;
}

inline void OpenUrl(const std::string& url) {
	if (url.empty() || url[0] == '#')
		return;
	std::string target = url;
	const bool external = url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0 ||
		url.rfind("mailto:", 0) == 0;
	if (!external) {
		std::error_code ec;
		const auto resolved = std::filesystem::weakly_canonical(BaseDirectory() / url, ec);
		target = EncodeFileUrl(ec ? (BaseDirectory() / url) : resolved);
	}
	if (std::getenv("CASIOEMU_DOC_LINK_TEST")) {
		std::fprintf(stderr, "[docs] link %s\n", target.c_str());
		return;
	}
	SDL_OpenURL(target.c_str());
}

inline bool g_link_valid = false;
inline ImVec2 g_link_min{};
inline ImVec2 g_link_max{};

enum class Style { Normal, Bold, Italic, Code, Link };

struct Run {
	std::string text;
	Style style = Style::Normal;
	std::string url;
};

// Split a line into styled runs.
inline std::vector<Run> ParseInline(const std::string& line) {
	std::vector<Run> runs;
	std::string text;
	auto flush = [&]() {
		if (!text.empty()) {
			runs.push_back({ text, Style::Normal, {} });
			text.clear();
		}
	};
	for (size_t i = 0; i < line.size();) {
		const char ch = line[i];
		if (ch == '\\' && i + 1 < line.size()) {
			text.push_back(line[i + 1]);
			i += 2;
			continue;
		}
		if (ch == '`') {
			const size_t end = line.find('`', i + 1);
			if (end != std::string::npos) {
				flush();
				runs.push_back({ line.substr(i + 1, end - i - 1), Style::Code, {} });
				i = end + 1;
				continue;
			}
		}
		if ((ch == '*' || ch == '_') && i + 1 < line.size() && line[i + 1] == ch) {
			const std::string marker = line.substr(i, 2);
			const size_t end = line.find(marker, i + 2);
			if (end != std::string::npos) {
				flush();
				runs.push_back({ line.substr(i + 2, end - i - 2), Style::Bold, {} });
				i = end + 2;
				continue;
			}
		}
		if (ch == '*' || ch == '_') {
			const size_t end = line.find(ch, i + 1);
			if (end != std::string::npos && end > i + 1) {
				flush();
				runs.push_back({ line.substr(i + 1, end - i - 1), Style::Italic, {} });
				i = end + 1;
				continue;
			}
		}
		if (ch == '!' && i + 1 < line.size() && line[i + 1] == '[') {
			const size_t close = line.find("](", i + 2);
			if (close != std::string::npos) {
				const size_t end = line.find(')', close + 2);
				if (end != std::string::npos) {
					flush();
					Run run;
					run.text = line.substr(i + 2, close - i - 2);
					run.style = Style::Link;
					run.url = line.substr(close + 2, end - close - 2);
					runs.push_back(run);
					i = end + 1;
					continue;
				}
			}
		}
		if (ch == '[') {
			const size_t close = line.find("](", i + 1);
			if (close != std::string::npos) {
				const size_t end = line.find(')', close + 2);
				if (end != std::string::npos) {
					flush();
					Run run;
					run.text = line.substr(i + 1, close - i - 1);
					run.style = Style::Link;
					run.url = line.substr(close + 2, end - close - 2);
					runs.push_back(run);
					i = end + 1;
					continue;
				}
			}
		}
		if (ch == '<' && line.find('>', i + 1) != std::string::npos) {
			const size_t end = line.find('>', i + 1);
			const std::string inner = line.substr(i + 1, end - i - 1);
			if (!inner.empty() && (std::isalpha(static_cast<unsigned char>(inner[0])) || inner[0] == '/' || inner[0] == '!')) {
				i = end + 1;
				continue;
			}
		}
		text.push_back(ch);
		++i;
	}
	flush();
	return runs;
}

// One word with its style; spaces are implicit between words.
struct Word {
	std::string text;
	Style style = Style::Normal;
	std::string url;
};

inline std::vector<Word> ToWords(const std::vector<Run>& runs) {
	std::vector<Word> words;
	for (const auto& run : runs) {
		std::string word;
		for (char ch : run.text) {
			if (ch == ' ' || ch == '\t') {
				if (!word.empty()) {
					words.push_back({ word, run.style, run.url });
					word.clear();
				}
			}
			else {
				word.push_back(ch);
			}
		}
		if (!word.empty())
			words.push_back({ word, run.style, run.url });
	}
	return words;
}

inline ImVec4 TextColor(Style style, const casioemu::plugin::ThemePalette& palette) {
	switch (style) {
	case Style::Bold: return palette.foreground;
	case Style::Italic: return palette.foreground;
	case Style::Code: return palette.warning;
	case Style::Link: return palette.link;
	default: return palette.foreground;
	}
}

} // namespace detail

// Render inline markdown for one block of text, wrapping at the content edge.
inline void RenderInline(const std::string& line) {
	using namespace detail;
	const auto& palette = ThemeManager::Instance().Colors();
	const auto words = ToWords(ParseInline(line));
	if (words.empty())
		return;
	const float space = ImGui::CalcTextSize(" ").x;
	const float start_x = ImGui::GetCursorPosX();
	const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
	bool first = true;
	for (const auto& word : words) {
		const float width = ImGui::CalcTextSize(word.text.c_str()).x;
		const float x_end = first ? ImGui::GetCursorScreenPos().x : ImGui::GetItemRectMax().x;
		if (!first && x_end + space + width > right) {
			// The cursor is already on the next line after the previous item;
			// SameLine is what would pull it back, so wrapping just skips it.
			ImGui::SetCursorPosX(start_x);
			first = true;
		}
		if (!first)
			ImGui::SameLine(0.0f, space);
		const bool is_code = word.style == Style::Code;
		ImVec2 pos = ImGui::GetCursorScreenPos();
		if (is_code) {
			ImGui::GetWindowDrawList()->AddRectFilled(
				ImVec2(pos.x - 2.0f, pos.y - 1.0f),
				ImVec2(pos.x + width + 2.0f, pos.y + ImGui::GetTextLineHeight() + 1.0f),
				ImGui::GetColorU32(ImGui::ColorConvertFloat4ToU32(
					ImVec4(palette.surfaceAlt.x, palette.surfaceAlt.y, palette.surfaceAlt.z, 0.9f))), 3.0f);
		}
		ImGui::PushStyleColor(ImGuiCol_Text, TextColor(word.style, palette));
		ImGui::TextUnformatted(word.text.c_str());
		ImGui::PopStyleColor();
		if (word.style == Style::Bold || word.style == Style::Link) {
			const ImVec2 min = ImGui::GetItemRectMin();
			const ImVec2 max = ImGui::GetItemRectMax();
			ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, max.y + 1.0f),
				ImVec2(max.x, max.y + 1.0f),
				ImGui::GetColorU32(TextColor(word.style, palette)), 1.0f);
		}
		if (word.style == Style::Link && ImGui::IsItemVisible()) {
			const ImVec2 min = ImGui::GetItemRectMin();
			const ImVec2 max = ImGui::GetItemRectMax();
			detail::g_link_valid = true;
			detail::g_link_min = min;
			detail::g_link_max = max;
		}
		if (word.style == Style::Link && ImGui::IsItemHovered()) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			if (!word.url.empty())
				ImGui::SetTooltip("%s", word.url.c_str());
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !word.url.empty())
				OpenUrl(word.url);
		}
		first = false;
	}
}

inline void RenderCodeBlock(const std::vector<std::string>& lines) {
	const auto& palette = ThemeManager::Instance().Colors();
	const float height = std::min(360.0f,
		ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(lines.size()) + 20.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
	ImGui::PushStyleColor(ImGuiCol_ChildBg, palette.surfaceAlt);
	ImGui::PushStyleColor(ImGuiCol_Border, palette.border);
	ImGui::BeginChild("##md_code", ImVec2(0.0f, std::max(48.0f, height)),
		ImGuiChildFlags_Border | ImGuiChildFlags_AlwaysUseWindowPadding,
		ImGuiWindowFlags_HorizontalScrollbar);
	for (const auto& line : lines) {
		ImGui::PushStyleColor(ImGuiCol_Text, palette.foreground);
		ImGui::TextUnformatted(line.c_str());
		ImGui::PopStyleColor();
	}
	ImGui::EndChild();
	ImGui::PopStyleColor(2);
	ImGui::PopStyleVar(2);
}

inline std::string PlainCell(std::string text) {
	std::string out;
	for (size_t i = 0; i < text.size();) {
		if (text[i] == '[') {
			const auto close = text.find("](", i + 1);
			const auto end = close == std::string::npos ? std::string::npos : text.find(')', close + 2);
			if (close != std::string::npos && end != std::string::npos) {
				out += text.substr(i + 1, close - i - 1);
				i = end + 1;
				continue;
			}
		}
		if (text[i] == '`' || text[i] == '*' || text[i] == '_') {
			++i;
			continue;
		}
		out.push_back(text[i]);
		++i;
	}
	return out;
}

inline void RenderTable(const std::vector<std::vector<std::string>>& rows) {
	if (rows.empty())
		return;
	const auto& palette = ThemeManager::Instance().Colors();
	const int columns = static_cast<int>(rows.front().size());
	if (columns < 1)
		return;
	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 4.0f));
	if (ImGui::BeginTable("##md_table", columns,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
			ImGuiTableFlags_SizingStretchProp)) {
		for (int col = 0; col < columns; ++col)
			ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		for (int col = 0; col < columns; ++col) {
			ImGui::TableSetColumnIndex(col);
			const std::string cell = PlainCell(rows.front().size() > static_cast<size_t>(col) ? rows.front()[col] : std::string());
			ImGui::PushStyleColor(ImGuiCol_Text, palette.foreground);
			ImGui::TextUnformatted(cell.c_str());
			ImGui::PopStyleColor();
		}
		for (size_t row = 1; row < rows.size(); ++row) {
			ImGui::TableNextRow();
			for (int col = 0; col < columns; ++col) {
				ImGui::TableSetColumnIndex(col);
				const std::string cell = PlainCell(rows[row].size() > static_cast<size_t>(col) ? rows[row][col] : std::string());
				ImGui::TextUnformatted(cell.c_str());
			}
		}
		ImGui::EndTable();
	}
	ImGui::PopStyleVar();
}

inline std::vector<std::string> SplitTableRow(const std::string& line) {
	std::vector<std::string> cells;
	std::string cell;
	for (size_t i = 0; i < line.size(); ++i) {
		const char ch = line[i];
		if (ch == '\\' && i + 1 < line.size() && line[i + 1] == '|') {
			cell.push_back('|');
			++i;
		}
		else if (ch == '|') {
			cells.push_back(detail::Trim(cell));
			cell.clear();
		}
		else {
			cell.push_back(ch);
		}
	}
	cells.push_back(detail::Trim(cell));
	if (!cells.empty() && cells.front().empty())
		cells.erase(cells.begin());
	if (!cells.empty() && cells.back().empty())
		cells.pop_back();
	return cells;
}

inline bool IsTableSeparator(const std::string& line) {
	const std::string trimmed = detail::Trim(line);
	if (trimmed.size() < 3 || trimmed.front() != '|')
		return false;
	for (char ch : trimmed) {
		if (ch != '|' && ch != '-' && ch != ':' && ch != ' ' && ch != '\t')
			return false;
	}
	return true;
}

inline std::string Sanitize(std::string text) {
	const std::pair<const char*, const char*> replacements[] = {
		{ "\xE2\x80\x94", "-" }, { "\xE2\x80\x93", "-" }, { "\xE2\x80\xA6", "..." },
		{ "\xE2\x80\x98", "'" }, { "\xE2\x80\x99", "'" },
		{ "\xE2\x80\x9C", "\"" }, { "\xE2\x80\x9D", "\"" },
		{ "\xC2\xA0", " " }, { "\xC2\xAD", "" },
	};
	for (const auto& [from, to] : replacements) {
		size_t position = 0;
		while ((position = text.find(from, position)) != std::string::npos) {
			text.replace(position, std::strlen(from), to);
			position += std::strlen(to);
		}
	}
	return text;
}

inline void Render(const std::string& text) {
	const auto& palette = ThemeManager::Instance().Colors();
	std::vector<std::string> lines;
	{
		const std::string clean = Sanitize(text);
		std::string line;
		for (char ch : clean) {
			if (ch == '\n') {
				lines.push_back(line);
				line.clear();
			}
			else if (ch != '\r') {
				line.push_back(ch);
			}
		}
		lines.push_back(line);
	}

	for (size_t i = 0; i < lines.size();) {
		const std::string line = lines[i];
		const std::string trimmed = detail::Trim(line);
		if (trimmed.empty()) {
			ImGui::Spacing();
			++i;
			continue;
		}
		if (detail::StartsWith(trimmed, "```")) {
			std::vector<std::string> code;
			++i;
			while (i < lines.size() && !detail::StartsWith(detail::Trim(lines[i]), "```")) {
				code.push_back(lines[i]);
				++i;
			}
			if (i < lines.size())
				++i;
			RenderCodeBlock(code);
			ImGui::Spacing();
			continue;
		}
		if (trimmed[0] == '#') {
			size_t level = 0;
			while (level < trimmed.size() && trimmed[level] == '#')
				++level;
			const std::string heading = detail::Trim(trimmed.substr(std::min(level, trimmed.size())));
			ImGui::Spacing();
			const ImVec4 color = level <= 2 ? palette.accent : palette.foreground;
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			ImGui::TextUnformatted(heading.c_str());
			ImGui::PopStyleColor();
			if (level == 1)
				ImGui::Separator();
			ImGui::Spacing();
			++i;
			continue;
		}
		if (trimmed == "---" || trimmed == "***" || trimmed == "___") {
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
			++i;
			continue;
		}
		if (trimmed[0] == '>') {
			std::string quote;
			while (i < lines.size() && detail::Trim(lines[i]).size() > 0 && detail::Trim(lines[i])[0] == '>') {
				std::string body = detail::Trim(lines[i]).substr(1);
				if (!body.empty() && body[0] == ' ')
					body.erase(body.begin());
				if (!quote.empty())
					quote += " ";
				quote += body;
				++i;
			}
			ImGui::PushStyleColor(ImGuiCol_Text, palette.muted);
			ImGui::Indent(10.0f);
			ImGui::TextWrapped("%s", quote.c_str());
			ImGui::Unindent(10.0f);
			ImGui::PopStyleColor();
			ImGui::Spacing();
			continue;
		}
		if ((trimmed[0] == '-' || trimmed[0] == '*' || trimmed[0] == '+') && trimmed.size() > 1 && trimmed[1] == ' ') {
			while (i < lines.size()) {
				const std::string item = detail::Trim(lines[i]);
				if (item.size() < 2 || (item[0] != '-' && item[0] != '*' && item[0] != '+') || item[1] != ' ')
					break;
				ImGui::Indent(12.0f);
				ImGui::PushStyleColor(ImGuiCol_Text, palette.accent);
				ImGui::TextUnformatted("-");
				ImGui::PopStyleColor();
				ImGui::SameLine(0.0f, 6.0f);
				RenderInline(item.substr(2));
				ImGui::Unindent(12.0f);
				++i;
			}
			ImGui::Spacing();
			continue;
		}
		{
			size_t digits = 0;
			while (digits < trimmed.size() && std::isdigit(static_cast<unsigned char>(trimmed[digits])))
				++digits;
			if (digits > 0 && digits + 1 < trimmed.size() && trimmed[digits] == '.' && trimmed[digits + 1] == ' ') {
				while (i < lines.size()) {
					const std::string item = detail::Trim(lines[i]);
					size_t count = 0;
					while (count < item.size() && std::isdigit(static_cast<unsigned char>(item[count])))
						++count;
					if (count == 0 || count + 1 >= item.size() || item[count] != '.' || item[count + 1] != ' ')
						break;
					const std::string marker = item.substr(0, count + 1);
					ImGui::Indent(12.0f);
					ImGui::PushStyleColor(ImGuiCol_Text, palette.accent);
					ImGui::TextUnformatted(marker.c_str());
					ImGui::PopStyleColor();
					ImGui::SameLine(0.0f, 6.0f);
					RenderInline(item.substr(count + 2));
					ImGui::Unindent(12.0f);
					++i;
				}
				ImGui::Spacing();
				continue;
			}
		}
		if (trimmed[0] == '|') {
			std::vector<std::vector<std::string>> rows;
			rows.push_back(SplitTableRow(trimmed));
			++i;
			if (i < lines.size() && IsTableSeparator(lines[i]))
				++i;
			while (i < lines.size() && detail::Trim(lines[i]).size() > 0 && detail::Trim(lines[i])[0] == '|') {
				rows.push_back(SplitTableRow(detail::Trim(lines[i])));
				++i;
			}
			RenderTable(rows);
			ImGui::Spacing();
			continue;
		}

		// Paragraph: join consecutive plain lines.
		std::string paragraph = trimmed;
		++i;
		while (i < lines.size()) {
			const std::string next = detail::Trim(lines[i]);
			if (next.empty())
				break;
			const char lead = next[0];
			if (lead == '#' || lead == '>' || lead == '|' || detail::StartsWith(next, "```") ||
				next == "---" || next == "***" || next == "___")
				break;
			if ((lead == '-' || lead == '*' || lead == '+') && next.size() > 1 && next[1] == ' ')
				break;
			paragraph += " " + next;
			++i;
		}
		RenderInline(paragraph);
	}
}

} // namespace casioemu::markdown
