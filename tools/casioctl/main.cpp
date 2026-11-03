#include "Commands.hpp"
#include "CtlClient.hpp"
#include "CtlJson.hpp"
#include "CtlSession.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace casioctl {
	int RunSelfTest(bool json_output, int timeout_ms);
}

namespace {
	constexpr int kExitOk = 0;
	constexpr int kExitUsage = 1;
	constexpr int kExitNotFound = 2;
	constexpr int kExitProtocol = 3;

	struct Options {
		bool json = false;
		int timeout_ms = 3000;
		std::string socket_path;
	};

	std::string Lower(std::string value) {
		for (char& ch : value)
			ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		return value;
	}

	bool LooksLikePid(const std::string& text) {
		if (text.empty())
			return false;
		for (char ch : text) {
			if (ch < '0' || ch > '9')
				return false;
		}
		return true;
	}

	bool ResolveSession(const std::string& target, casioctl::SessionInfo& session, std::string& error) {
		const std::vector<casioctl::SessionInfo> sessions = casioctl::ListSessions();
		if (sessions.empty()) {
			error = "no live CasioEmuMsvc sessions (is the emulator running?)";
			return false;
		}
		if (LooksLikePid(target)) {
			const int pid = std::atoi(target.c_str());
			for (const auto& candidate : sessions) {
				if (candidate.pid == pid) {
					session = candidate;
					return true;
				}
			}
			error = "no live session with pid " + target;
			return false;
		}
		std::vector<casioctl::SessionInfo> matches;
		for (const auto& candidate : sessions) {
			if (candidate.model == target) {
				session = candidate;
				return true;
			}
		}
		const std::string needle = Lower(target);
		for (const auto& candidate : sessions) {
			if (Lower(candidate.model).find(needle) != std::string::npos)
				matches.push_back(candidate);
		}
		if (matches.size() == 1) {
			session = matches.front();
			return true;
		}
		if (matches.empty()) {
			error = "no live session matches '" + target + "'";
			return false;
		}
		std::ostringstream out;
		out << "'" << target << "' is ambiguous; matching sessions:";
		for (const auto& match : matches)
			out << " " << match.pid << " (" << match.model << ")";
		error = out.str();
		return false;
	}

	void PrintSessionsTable(const std::vector<casioctl::SessionInfo>& sessions, size_t hidden_stale) {
		if (sessions.empty()) {
			std::cout << "no live sessions";
			if (hidden_stale)
				std::cout << " (" << hidden_stale << " stale session file(s) ignored)";
			std::cout << "\n";
			return;
		}
		std::cout << "PID     MODEL                           HARDWARE     STARTED               SOCKET\n";
		for (const auto& session : sessions) {
			char line[512];
			std::snprintf(line, sizeof(line), "%-7d %-31s %-12s %-21s %s\n",
				session.pid,
				session.model.c_str(),
				session.hardware.c_str(),
				session.started_at.c_str(),
				session.socket_path.c_str());
			std::cout << line;
		}
		if (hidden_stale)
			std::cout << "(" << hidden_stale << " stale session file(s) ignored)\n";
	}

	int PrintSessions(const Options& options) {
		size_t hidden = 0;
		const std::vector<casioctl::SessionInfo> sessions = casioctl::ListSessions(&hidden);
		if (options.json) {
			casioctl::Json result = casioctl::Json::Object();
			casioctl::Json list = casioctl::Json::Array();
			for (const auto& session : sessions) {
				casioctl::Json entry = casioctl::SessionToJson(session);
				entry["reachable"] = session.reachable;
				list.Push(std::move(entry));
			}
			result["sessions"] = std::move(list);
			result["stale"] = static_cast<uint64_t>(hidden);
			std::cout << result.Dump() << "\n";
		}
		else {
			PrintSessionsTable(sessions, hidden);
		}
		return kExitOk;
	}

	int PrintResponse(const Options& options, const std::string& command, const casioctl::Json& response, bool allow_json) {
		if (allow_json && options.json) {
			std::cout << response.Dump() << "\n";
			return kExitOk;
		}
		const casioctl::Json* ok = response.Find("ok");
		if (!ok || !ok->AsBool(false)) {
			const casioctl::Json* error = response.Find("error");
			std::cerr << "error: " << (error ? error->AsString() : "unknown debugger error") << "\n";
			return kExitProtocol;
		}
		const casioctl::Json* result = response.Find("result");
		if (!result) {
			std::cerr << "error: response has no result\n";
			return kExitProtocol;
		}
		std::cout << casioctl::FormatResult(command, *result);
		return kExitOk;
	}

	bool MakeAbsolutePath(std::string& path, std::string& error) {
		if (path.empty()) {
			error = "empty path";
			return false;
		}
		std::error_code code;
		const std::filesystem::path absolute = std::filesystem::absolute(std::filesystem::path(path), code);
		if (code) {
			error = "cannot resolve path: " + code.message();
			return false;
		}
		path = absolute.string();
		return true;
	}

	bool SendCommand(casioctl::Client& client, casioctl::ParsedCommand parsed, casioctl::Json& response,
		std::string& transport_error) {
		if (parsed.command == "screenshot" || parsed.command == "snapshot.export" ||
			parsed.command == "snapshot.import") {
			std::string path = parsed.arguments.Find("path") ? parsed.arguments.Find("path")->AsString() : std::string();
			std::string path_error;
			if (!MakeAbsolutePath(path, path_error)) {
				transport_error = path_error;
				return false;
			}
			parsed.arguments["path"] = path;
		}
		std::string error;
		if (!client.Request(parsed.command, parsed.arguments, response, error)) {
			transport_error = error;
			return false;
		}
		return true;
	}

	int RunOneShot(const Options& options, const std::string& target, const std::vector<std::string>& command_tokens) {
		casioctl::ParsedCommand parsed;
		std::string error;
		if (!casioctl::ParseCommandLine(command_tokens, parsed, error)) {
			std::cerr << "error: " << error << "\n";
			return kExitUsage;
		}

		std::string socket_path = options.socket_path;
		if (socket_path.empty()) {
			casioctl::SessionInfo session;
			if (!ResolveSession(target, session, error)) {
				std::cerr << "error: " << error << "\n";
				return kExitNotFound;
			}
			socket_path = session.socket_path;
		}

		casioctl::Client client;
		client.SetTimeout(options.timeout_ms);
		if (!client.Connect(socket_path, options.timeout_ms, error)) {
			std::cerr << "error: " << error << "\n";
			return kExitNotFound;
		}

		casioctl::Json response;
		if (parsed.command != "hello" && !client.Request("hello", casioctl::Json::Object(), response, error)) {
			std::cerr << "error: protocol handshake failed: " << error << "\n";
			return kExitProtocol;
		}
		if (parsed.command != "hello") {
			const casioctl::Json* result = response.Find("result");
			const casioctl::Json* version = result ? result->Find("protocol_version") : nullptr;
			if (!version || version->AsInt(0) != casioctl::kProtocolVersion) {
				std::cerr << "error: unsupported control protocol version\n";
				return kExitProtocol;
			}
		}

		std::string transport_error;
		if (!SendCommand(client, parsed, response, transport_error)) {
			std::cerr << "error: " << transport_error << "\n";
			return kExitProtocol;
		}
		return PrintResponse(options, parsed.command, response, true);
	}

	int RunRepl(const Options& options, const std::string& target) {
		std::string socket_path = options.socket_path;
		std::string model;
		if (socket_path.empty()) {
			casioctl::SessionInfo session;
			std::string error;
			if (!ResolveSession(target, session, error)) {
				std::cerr << "error: " << error << "\n";
				return kExitNotFound;
			}
			socket_path = session.socket_path;
			model = session.model;
		}

		casioctl::Client client;
		client.SetTimeout(options.timeout_ms);
		std::string error;
		if (!client.Connect(socket_path, options.timeout_ms, error)) {
			std::cerr << "error: " << error << "\n";
			return kExitNotFound;
		}

		casioctl::Json response;
		if (!client.Request("hello", casioctl::Json::Object(), response, error)) {
			std::cerr << "error: protocol handshake failed: " << error << "\n";
			return kExitProtocol;
		}
		const casioctl::Json* hello = response.Find("result");
		const bool protocol_ok = hello && hello->Find("protocol_version") &&
			hello->Find("protocol_version")->AsInt(0) == casioctl::kProtocolVersion;
		if (!protocol_ok) {
			std::cerr << "error: unsupported control protocol version\n";
			return kExitProtocol;
		}
		if (model.empty() && hello->Find("model"))
			model = hello->Find("model")->AsString();
		std::cout << "attached to " << model << " (" << socket_path << ")\n";
		std::cout << "type 'help' for commands, 'quit' to leave\n";

		const bool interactive = ::isatty(STDIN_FILENO) != 0;
		std::string line;
		for (;;) {
			if (interactive) {
				std::cout << "casioctl> " << std::flush;
			}
			if (!std::getline(std::cin, line))
				break;
			std::string token_error;
			const std::vector<std::string> tokens = casioctl::Tokenize(line, token_error);
			if (!token_error.empty()) {
				std::cerr << "error: " << token_error << "\n";
				continue;
			}
			if (tokens.empty())
				continue;
			const std::string verb = Lower(tokens[0]);
			if (verb == "quit" || verb == "exit")
				break;
			if (verb == "help") {
				std::cout << casioctl::CommandHelp();
				continue;
			}
			casioctl::ParsedCommand parsed;
			std::string parse_error;
			if (!casioctl::ParseCommandLine(tokens, parsed, parse_error)) {
				std::cerr << "error: " << parse_error << "\n";
				continue;
			}
			casioctl::Json reply;
			std::string transport_error;
			if (!SendCommand(client, parsed, reply, transport_error)) {
				std::cerr << "error: " << transport_error << "\n";
				return kExitProtocol;
			}
			PrintResponse(options, parsed.command, reply, true);
		}
		return kExitOk;
	}

	void PrintUsage(std::ostream& out) {
		out <<
			"casioctl - debugger control channel for a running CasioEmuMsvc session\n"
			"\n"
			"Usage:\n"
			"  casioctl sessions [--json]\n"
			"  casioctl attach <pid|model> [--json] [--timeout ms]\n"
			"  casioctl -s <pid> [--json] <command> [arguments...]\n"
			"  casioctl --socket <path> <command> [arguments...]\n"
			"  casioctl selftest [--json]\n"
			"  casioctl --version\n"
			"\n"
			"Options:\n"
			"  --json             print the raw response envelope as one JSON line\n"
			"  --timeout <ms>     socket timeout (default 3000)\n"
			"  --socket <path>    bypass discovery and use this UNIX socket\n"
			"  -s, --session <id> session pid (or use 'attach <model>')\n"
			"  -h, --help         this help\n"
			"\n"
			"Exit codes: 0 ok, 1 usage, 2 session not found, 3 protocol/command error.\n"
			"\n"
			"Examples:\n"
			"  casioctl sessions\n"
			"  casioctl attach 4242\n"
			"  casioctl -s 4242 read 0xE9E0 16\n"
			"  casioctl -s 4242 --json status\n"
			"  casioctl -s 4242 screenshot ./calc.png\n"
			"\n"
			<< casioctl::CommandHelp();
	}

	int RunMain(int argc, char** argv) {
		Options options;
		std::string group;
		std::string target;
		std::vector<std::string> rest;

		size_t index = 1;
		for (; index < static_cast<size_t>(argc); ++index) {
			const std::string token = argv[index];
			if (token == "-h" || token == "--help") {
				PrintUsage(std::cout);
				return kExitOk;
			}
			if (token == "--version") {
				std::cout << "casioctl protocol " << casioctl::kProtocolVersion << "\n";
				return kExitOk;
			}
			if (token == "--json") {
				options.json = true;
				continue;
			}
			if (token == "--timeout") {
				if (index + 1 >= static_cast<size_t>(argc)) {
					std::cerr << "error: --timeout requires a value\n";
					return kExitUsage;
				}
				options.timeout_ms = std::atoi(argv[++index]);
				if (options.timeout_ms <= 0) {
					std::cerr << "error: --timeout must be positive\n";
					return kExitUsage;
				}
				continue;
			}
			if (token == "--socket") {
				if (index + 1 >= static_cast<size_t>(argc)) {
					std::cerr << "error: --socket requires a path\n";
					return kExitUsage;
				}
				options.socket_path = argv[++index];
				continue;
			}
			if (token == "-s" || token == "--session") {
				group = "oneshot";
				if (index + 1 >= static_cast<size_t>(argc)) {
					std::cerr << "error: -s requires a session pid\n";
					return kExitUsage;
				}
				target = argv[++index];
				++index;
				break;
			}
			group = token;
			++index;
			break;
		}

		if (group.empty()) {
			std::cerr << "error: no command given\n\n";
			PrintUsage(std::cerr);
			return kExitUsage;
		}

		auto collect_tail_options = [&]() -> bool {
			for (; index < static_cast<size_t>(argc); ++index) {
				const std::string token = argv[index];
				if (token == "--json") {
					options.json = true;
					continue;
				}
				if (token == "--timeout") {
					if (index + 1 >= static_cast<size_t>(argc))
						return false;
					options.timeout_ms = std::atoi(argv[++index]);
					if (options.timeout_ms <= 0)
						return false;
					continue;
				}
				if (token == "--socket") {
					if (index + 1 >= static_cast<size_t>(argc))
						return false;
					options.socket_path = argv[++index];
					continue;
				}
				rest.push_back(token);
			}
			return true;
		};

		if (group == "oneshot") {
			if (!collect_tail_options()) {
				std::cerr << "error: invalid option in command tail\n";
				return kExitUsage;
			}
			if (rest.empty()) {
				std::cerr << "error: no command given after -s\n";
				return kExitUsage;
			}
			return RunOneShot(options, target, rest);
		}
		if (group == "attach") {
			if (!collect_tail_options()) {
				std::cerr << "error: invalid option in command tail\n";
				return kExitUsage;
			}
			if (!options.socket_path.empty()) {
				return RunRepl(options, std::string());
			}
			if (rest.size() != 1) {
				std::cerr << "error: usage: casioctl attach <pid|model>\n";
				return kExitUsage;
			}
			return RunRepl(options, rest[0]);
		}
		if (group == "sessions") {
			for (; index < static_cast<size_t>(argc); ++index) {
				const std::string token = argv[index];
				if (token == "--json")
					options.json = true;
				else {
					std::cerr << "error: unknown option '" << token << "'\n";
					return kExitUsage;
				}
			}
			return PrintSessions(options);
		}
		if (group == "selftest") {
			for (; index < static_cast<size_t>(argc); ++index) {
				const std::string token = argv[index];
				if (token == "--json")
					options.json = true;
				else {
					std::cerr << "error: unknown option '" << token << "'\n";
					return kExitUsage;
				}
			}
			return casioctl::RunSelfTest(options.json, options.timeout_ms);
		}
		if (group == "help") {
			PrintUsage(std::cout);
			return kExitOk;
		}

		// Reaching here means the first positional was a debugger command
		// (casioctl --socket PATH read ...). Use it with the pid-less path.
		if (options.socket_path.empty()) {
			std::cerr << "error: unknown command '" << group << "'\n\n";
			PrintUsage(std::cerr);
			return kExitUsage;
		}
		rest.push_back(group);
		if (!collect_tail_options()) {
			std::cerr << "error: invalid option in command tail\n";
			return kExitUsage;
		}
		return RunOneShot(options, std::string(), rest);
	}
} // namespace

int main(int argc, char** argv) {
	return RunMain(argc, argv);
}
