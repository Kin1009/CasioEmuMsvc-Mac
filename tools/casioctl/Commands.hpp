#pragma once

#include "CtlJson.hpp"

#include <string>
#include <vector>

namespace casioctl {

	struct ParsedCommand {
		std::string command;
		Json arguments;
	};

	std::vector<std::string> Tokenize(const std::string& line, std::string& error);

	// Maps a CLI command ("read 0xE9E0 16") onto the wire command
	// ("read" with {"addr":59872,"len":16}). Returns false on usage errors.
	bool ParseCommandLine(const std::vector<std::string>& tokens, ParsedCommand& parsed, std::string& error);

	// Human-readable rendering of a successful result.
	std::string FormatResult(const std::string& command, const Json& result);

	std::string CommandHelp();

} // namespace casioctl
