#include "Commands.hpp"
#include "CtlClient.hpp"
#include "CtlJson.hpp"
#include "CtlSession.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace casioctl {
	namespace {
		class Checker {
		public:
			void Check(bool condition, const std::string& name, const std::string& detail = std::string()) {
				++checks_;
				if (condition)
					return;
				std::string message = name;
				if (!detail.empty())
					message += ": " + detail;
				failures_.push_back(message);
			}

			int checks() const { return checks_; }
			const std::vector<std::string>& failures() const { return failures_; }

		private:
			int checks_ = 0;
			std::vector<std::string> failures_;
		};

		int64_t ArgInt(const Json& value, const char* key, int64_t fallback = -1) {
			const Json* field = value.Find(key);
			return field ? field->AsInt(fallback) : fallback;
		}

		// A tiny stand-in for the in-app server: it speaks the same NDJSON
		// protocol, serves a fixed model and records any width/address mismatch.
		class FakeServer {
		public:
			explicit FakeServer(std::string path) : path_(std::move(path)) {}
			~FakeServer() {
				Stop();
			}

			bool Start(std::string& error) {
				sockaddr_un address{};
				address.sun_family = AF_UNIX;
				if (path_.size() >= sizeof(address.sun_path)) {
					error = "selftest socket path is too long";
					return false;
				}
				std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
				::unlink(path_.c_str());
				listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
				if (listen_fd_ < 0) {
					error = std::string("fake socket() failed: ") + std::strerror(errno);
					return false;
				}
				if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
					error = std::string("fake bind() failed: ") + std::strerror(errno);
					return false;
				}
#ifdef SO_NOSIGPIPE
				int one = 1;
				::setsockopt(listen_fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
				if (::listen(listen_fd_, 8) != 0) {
					error = std::string("fake listen() failed: ") + std::strerror(errno);
					return false;
				}
				stop_.store(false);
				thread_ = std::thread([this] { Run(); });
				return true;
			}

			void Stop() {
				stop_.store(true);
				if (thread_.joinable())
					thread_.join();
				if (listen_fd_ >= 0) {
					::close(listen_fd_);
					listen_fd_ = -1;
				}
				::unlink(path_.c_str());
			}

			int bad_request_count() const { return bad_requests_.load(); }

		private:
			void Run() {
				while (!stop_.load()) {
					pollfd descriptor{};
					descriptor.fd = listen_fd_;
					descriptor.events = POLLIN;
					const int ready = ::poll(&descriptor, 1, 20);
					if (ready <= 0)
						continue;
					const int client = ::accept(listen_fd_, nullptr, nullptr);
					if (client < 0)
						continue;
					std::string buffer;
					char chunk[4096];
					for (;;) {
						const ssize_t received = ::recv(client, chunk, sizeof(chunk), 0);
						if (received <= 0)
							break;
						buffer.append(chunk, static_cast<size_t>(received));
						size_t newline = 0;
						while ((newline = buffer.find('\n')) != std::string::npos) {
							std::string line = buffer.substr(0, newline);
							buffer.erase(0, newline + 1);
							if (!line.empty() && line.back() == '\r')
								line.pop_back();
							if (line.empty())
								continue;
							std::string reply = Handle(line);
							reply.push_back('\n');
							(void)!::send(client, reply.data(), reply.size(), 0);
						}
					}
					::close(client);
				}
			}

			std::string Handle(const std::string& line) {
				Json request;
				std::string error;
				if (!Json::Parse(line, request, error)) {
					bad_requests_.fetch_add(1);
					return "this is not JSON";
				}
				Json response = Json::Object();
				if (const Json* id = request.Find("id"))
					response["id"] = *id;
				const Json* command = request.Find("cmd");
				if (!command || !command->IsString()) {
					response["ok"] = false;
					response["error"] = "missing cmd";
					response["code"] = "bad-request";
					return response.Dump();
				}
				Json arguments = Json::Object();
				if (const Json* args = request.Find("args"))
					arguments = *args;
				Json result = Json::Object();
				std::string name = command->AsString();
				if (name == "hello") {
					result["protocol"] = kProtocolName;
					result["protocol_version"] = kProtocolVersion;
					result["app"] = kAppName;
					result["app_version"] = "selftest";
					result["pid"] = static_cast<int>(::getpid());
					result["model"] = "casioctl selftest";
					result["hardware"] = "TEST";
					result["paused"] = true;
					result["debugger"] = true;
					result["debugger_ui"] = true;
					result["snapshot_ui"] = true;
					result["keyboard"] = true;
					result["screenshot"] = false;
					result["capabilities"] = Json::Array();
					response["ok"] = true;
					response["result"] = std::move(result);
					return response.Dump();
				}
				if (name == "status") {
					result["paused"] = true;
					result["pc"] = 0x1234;
					result["model"] = "casioctl selftest";
					response["ok"] = true;
					response["result"] = std::move(result);
					return response.Dump();
				}
				if (name == "read") {
					if (ArgInt(arguments, "addr") != 0xE9E0 || ArgInt(arguments, "len") != 16) {
						bad_requests_.fetch_add(1);
						response["ok"] = false;
						response["error"] = "unexpected read arguments";
						response["code"] = "bad-args";
						return response.Dump();
					}
					result["address"] = 0xE9E0;
					result["length"] = 16;
					result["data"] = "000102030405060708090a0b0c0d0e0f";
					response["ok"] = true;
					response["result"] = std::move(result);
					return response.Dump();
				}
				response["ok"] = false;
				response["error"] = "unknown command";
				response["code"] = "unknown-command";
				return response.Dump();
			}

			std::string path_;
			int listen_fd_ = -1;
			std::atomic<bool> stop_{false};
			std::atomic<int> bad_requests_{0};
			std::thread thread_;
		};

		bool ParseJsonRoundTrip(const std::string& text, Json& value, std::string& error) {
			if (!Json::Parse(text, value, error))
				return false;
			Json reparsed;
			if (!Json::Parse(value.Dump(), reparsed, error))
				return false;
			return reparsed.Dump() == value.Dump();
		}

		bool RunParserChecks(Checker& checker) {
			std::string error;
			std::vector<std::string> tokens = Tokenize("key press \"Keypad 7\"", error);
			checker.Check(error.empty() && tokens.size() == 3 && tokens[2] == "Keypad 7",
				"tokenizer keeps quoted keys with spaces together", error);

			ParsedCommand parsed;
			const bool read_ok = ParseCommandLine(Tokenize("read 0xE9E0 16", error), parsed, error);
			checker.Check(read_ok && parsed.command == "read" &&
					ArgInt(parsed.arguments, "addr") == 0xE9E0 && ArgInt(parsed.arguments, "len") == 16,
				"parse read <addr> <len>", error);

			const bool reg_ok = ParseCommandLine(Tokenize("reg set pc 0x100", error), parsed, error);
			checker.Check(reg_ok && parsed.command == "reg.set" &&
					(parsed.arguments.Find("name") ? parsed.arguments.Find("name")->AsString() : "") == "pc" &&
					ArgInt(parsed.arguments, "value") == 0x100,
				"parse reg set", error);

			const bool bp_ok = ParseCommandLine(Tokenize("bp mem 0x1000 32", error), parsed, error);
			checker.Check(bp_ok && parsed.command == "bp.mem.set" &&
					ArgInt(parsed.arguments, "addr") == 0x1000 && ArgInt(parsed.arguments, "len") == 32 &&
					(parsed.arguments.Find("access") ? parsed.arguments.Find("access")->AsString() : "") == "both",
				"parse bp mem watch", error);

			const bool mem_clear_ok = ParseCommandLine(Tokenize("bp mem clear 0x1000 32 write", error), parsed, error);
			checker.Check(mem_clear_ok && parsed.command == "bp.mem.clear" &&
					(parsed.arguments.Find("access") ? parsed.arguments.Find("access")->AsString() : "") == "write",
				"parse bp mem clear", error);

			const bool matrix_ok = ParseCommandLine(Tokenize("key matrix 3 4 press", error), parsed, error);
			checker.Check(matrix_ok && parsed.command == "key.matrix" &&
					ArgInt(parsed.arguments, "ki") == 3 && ArgInt(parsed.arguments, "ko") == 4,
				"parse key matrix", error);

			const bool press_ok = ParseCommandLine(Tokenize("key press \"Keypad 7\"", error), parsed, error);
			checker.Check(press_ok && parsed.command == "key.press" &&
					(parsed.arguments.Find("name") ? parsed.arguments.Find("name")->AsString() : "") == "Keypad 7",
				"parse key press <name>", error);

			const bool hold_ok = ParseCommandLine(Tokenize("key hold Space 120", error), parsed, error);
			checker.Check(hold_ok && parsed.command == "key.hold" &&
					(parsed.arguments.Find("name") ? parsed.arguments.Find("name")->AsString() : "") == "Space" &&
					ArgInt(parsed.arguments, "ms") == 120,
				"parse key hold with duration", error);

			const bool shot_ok = ParseCommandLine(Tokenize("screenshot /Users/kin/Desktop/Wiz/temp/casioctl/calc.rgba --raw", error), parsed, error);
			checker.Check(shot_ok && parsed.command == "screenshot" &&
					(parsed.arguments.Find("format") ? parsed.arguments.Find("format")->AsString() : "") == "rgba",
				"parse screenshot --raw", error);

			const bool snapshot_ok = ParseCommandLine(Tokenize("snapshot load 3", error), parsed, error);
			checker.Check(snapshot_ok && parsed.command == "snapshot.load" && ArgInt(parsed.arguments, "id") == 3,
				"parse snapshot load", error);

			const bool disasm_ok = ParseCommandLine(Tokenize("disasm 0x100 5", error), parsed, error);
			checker.Check(disasm_ok && parsed.command == "disasm" &&
					ArgInt(parsed.arguments, "addr") == 0x100 && ArgInt(parsed.arguments, "count") == 5,
				"parse disasm", error);

			const bool inject_ok = ParseCommandLine(Tokenize("inject 0x200 de:ad:be:ef run", error), parsed, error);
			checker.Check(inject_ok && parsed.command == "inject" &&
					(parsed.arguments.Find("data") ? parsed.arguments.Find("data")->AsString() : "") == "deadbeef" &&
					(parsed.arguments.Find("run") ? parsed.arguments.Find("run")->AsBool() : false),
				"parse inject with bytes and run", error);

			checker.Check(!ParseCommandLine(Tokenize("read 0xZZ 4", error), parsed, error),
				"reject invalid address");
			checker.Check(!ParseCommandLine(Tokenize("read 0x100", error), parsed, error),
				"reject missing length");
			checker.Check(!ParseCommandLine(Tokenize("frobnicate", error), parsed, error),
				"reject unknown command");
			checker.Check(!ParseCommandLine(Tokenize("write 0x100 abc", error), parsed, error),
				"reject odd-length hex data");
			checker.Check(!ParseCommandLine(Tokenize("step 0", error), parsed, error),
				"reject a zero step count");

			checker.Check(ParseCommandLine(Tokenize("step-over", error), parsed, error) &&
					parsed.command == "step.over",
				"parse step-over");
			checker.Check(ParseCommandLine(Tokenize("step-out", error), parsed, error) &&
					parsed.command == "step.out",
				"parse step-out");
			checker.Check(ParseCommandLine(Tokenize("read-code 0x100 4", error), parsed, error) &&
					parsed.command == "read-code" && ArgInt(parsed.arguments, "addr") == 0x100 &&
					ArgInt(parsed.arguments, "count") == 4,
				"parse read-code");
			checker.Check(ParseCommandLine(Tokenize("write-code 0x100 deadbeef", error), parsed, error) &&
					parsed.command == "write-code" &&
					(parsed.arguments.Find("data") ? parsed.arguments.Find("data")->AsString() : "") == "deadbeef",
				"parse write-code");
			checker.Check(ParseCommandLine(Tokenize("bp hits 0x1000 read", error), parsed, error) &&
					parsed.command == "bp.hits" &&
					(parsed.arguments.Find("access") ? parsed.arguments.Find("access")->AsString() : "") == "read",
				"parse bp hits");
			checker.Check(ParseCommandLine(Tokenize("bp mem clearall", error), parsed, error) &&
					parsed.command == "bp.mem.clearall",
				"parse bp mem clearall");
			checker.Check(ParseCommandLine(Tokenize("bp mem 0x1000 2 write --record --compare 42 --mask ff --skip 3", error), parsed, error) &&
					parsed.command == "bp.mem.set" &&
					(parsed.arguments.Find("break_when_hit") ? !parsed.arguments.Find("break_when_hit")->AsBool(true) : false) &&
					(parsed.arguments.Find("compare_data") ? parsed.arguments.Find("compare_data")->AsBool(false) : false) &&
					ArgInt(parsed.arguments, "data") == 0x42 && ArgInt(parsed.arguments, "skip") == 3,
				"parse bp mem compare/record/skip options");
			checker.Check(ParseCommandLine(Tokenize("watch set 0x1000 42", error), parsed, error) &&
					parsed.command == "watch.set" && ArgInt(parsed.arguments, "addr") == 0x1000 &&
					ArgInt(parsed.arguments, "value") == 0x42,
				"parse watch set");
			checker.Check(ParseCommandLine(Tokenize("watch list", error), parsed, error) &&
					parsed.command == "watch.list",
				"parse watch list");
			checker.Check(ParseCommandLine(Tokenize("call start 0x100 0x200", error), parsed, error) &&
					parsed.command == "call.start" && ArgInt(parsed.arguments, "caller") == 0x100 &&
					ArgInt(parsed.arguments, "callee") == 0x200,
				"parse call start with filters");
			checker.Check(ParseCommandLine(Tokenize("call list 0x100", error), parsed, error) &&
					parsed.command == "call.list" && ArgInt(parsed.arguments, "function") == 0x100,
				"parse call list");
			checker.Check(ParseCommandLine(Tokenize("snapshot export /Users/kin/Desktop/Wiz/temp/casioctl/snap 3 --subtree", error), parsed, error) &&
					parsed.command == "snapshot.export" &&
					(parsed.arguments.Find("path") ? parsed.arguments.Find("path")->AsString() : "") == "/Users/kin/Desktop/Wiz/temp/casioctl/snap" &&
					ArgInt(parsed.arguments, "id") == 3 &&
					(parsed.arguments.Find("subtree") ? parsed.arguments.Find("subtree")->AsBool() : false),
				"parse snapshot export");
			checker.Check(ParseCommandLine(Tokenize("snapshot import /Users/kin/Desktop/Wiz/temp/casioctl/snap", error), parsed, error) &&
					parsed.command == "snapshot.import",
				"parse snapshot import");
			checker.Check(ParseCommandLine(Tokenize("hw cps 0x4000", error), parsed, error) &&
					parsed.command == "hw.cps" && ArgInt(parsed.arguments, "value") == 0x4000,
				"parse hw cps");
			checker.Check(ParseCommandLine(Tokenize("hw display set residual_alpha 1.5", error), parsed, error) &&
					parsed.command == "hw.display.set" &&
					(parsed.arguments.Find("residual_alpha") ? parsed.arguments.Find("residual_alpha")->AsDouble() : 0.0) == 1.5,
				"parse hw display set");
			checker.Check(ParseCommandLine(Tokenize("hw display set audio_enabled on", error), parsed, error) &&
					parsed.command == "hw.display.set" &&
					(parsed.arguments.Find("audio_enabled") ? parsed.arguments.Find("audio_enabled")->AsBool() : false),
				"parse hw display boolean");
			checker.Check(ParseCommandLine(Tokenize("hw record start", error), parsed, error) &&
					parsed.command == "hw.record" &&
					(parsed.arguments.Find("start") ? parsed.arguments.Find("start")->AsBool() : false),
				"parse hw record start");
			checker.Check(ParseCommandLine(Tokenize("hw reload-rom", error), parsed, error) &&
					parsed.command == "hw.reload-rom",
				"parse hw reload-rom");
			checker.Check(ParseCommandLine(Tokenize("hw qr", error), parsed, error) &&
					parsed.command == "hw.qr",
				"parse hw qr");
			checker.Check(!ParseCommandLine(Tokenize("hw display set nonsense 1", error), parsed, error),
				"reject an unknown display setting");

			Json value;
			const bool round_trip = ParseJsonRoundTrip(
				"{\"a\":[1,2,{\"b\":\"x\\n\\u00e9\"}],\"c\":true,\"d\":null,\"e\":-42}", value, error);
			checker.Check(round_trip, "JSON parse/dump round trip", error);
			checker.Check(value.Find("a") && value.Find("a")->Items().size() == 3 &&
					value.Find("d") && value.Find("d")->IsNull() && ArgInt(value, "e") == -42,
				"JSON values survive a round trip", error);
			Json rejected;
			checker.Check(!Json::Parse("{\"a\":1} trailing", rejected, error),
				"JSON parser rejects trailing garbage", error);
			checker.Check(!Json::Parse("{\"a\":", rejected, error),
				"JSON parser rejects truncated input", error);
			checker.Check(Json::Parse("{\"s\":\"\\ud83d\\ude00\"}", rejected, error),
				"JSON parser accepts surrogate pairs", error);

			return true;
		}
	} // namespace

	int RunSelfTest(bool json_output, int timeout_ms) {
		Checker checker;
		std::string error;

		RunParserChecks(checker);

		std::string scratch = ScratchRoot();
		std::string scratch_error;
		checker.Check(EnsureDirectory(scratch, scratch_error), "create the scratch directory", scratch_error);
		std::string pattern = scratch + "/selftest-XXXXXX";
		std::vector<char> buffer(pattern.begin(), pattern.end());
		buffer.push_back('\0');
		char* created = ::mkdtemp(buffer.data());
		checker.Check(created != nullptr, "create a temporary control directory", std::strerror(errno));
		if (!created) {
			// Cannot continue without a directory.
			std::cout << (json_output ? "{\"ok\":false,\"checks\":" + std::to_string(checker.checks()) +
					",\"failures\":[\"mkdtemp failed\"]}" : "selftest failed: mkdtemp failed") << "\n";
			return 1;
		}
		const std::string control_dir = created;
		::setenv("CASIOEMU_CTL_DIR", control_dir.c_str(), 1);

		const std::string socket_path = SessionSocketPath(static_cast<int>(::getpid()));
		FakeServer server(socket_path);
		checker.Check(server.Start(error), "start the fake control server", error);

		SessionInfo info;
		info.pid = static_cast<int>(::getpid());
		info.model = "casioctl selftest";
		info.model_path = control_dir + "/model";
		info.rom_path = "rom.bin";
		info.hardware = "TEST";
		info.hardware_id = 0;
		info.socket_path = socket_path;
		info.app_version = "selftest";
		info.protocol_version = kProtocolVersion;
		info.started_at = FormatUtcTimestamp(static_cast<int64_t>(std::time(nullptr)));
		checker.Check(WriteSessionFile(info, error), "write the session metadata file", error);

		// A session whose process is gone must be hidden by discovery.
		const pid_t child = ::fork();
		if (child == 0)
			::_exit(0);
		int child_status = 0;
		if (child > 0)
			::waitpid(child, &child_status, 0);
		const int dead_pid = child > 0 ? static_cast<int>(child) : 999999;
		SessionInfo dead;
		dead.pid = dead_pid;
		dead.model = "dead session";
		dead.socket_path = SessionSocketPath(dead_pid);
		dead.protocol_version = kProtocolVersion;
		std::string ignored;
		WriteSessionFile(dead, ignored);

		size_t hidden = 0;
		const std::vector<SessionInfo> sessions = ListSessions(&hidden);
		bool found = false;
		for (const auto& session : sessions) {
			if (session.pid == info.pid && session.model == info.model && session.reachable)
				found = true;
		}
		checker.Check(found && sessions.size() == 1, "discover exactly the live session",
			"found " + std::to_string(sessions.size()) + " session(s)");
		checker.Check(hidden >= 1, "hide the stale session file");

		Client client;
		client.SetTimeout(timeout_ms);
		Json response;
		const bool connected = client.Connect(socket_path, timeout_ms, error);
		checker.Check(connected, "connect to the fake server", error);
		if (connected) {
			checker.Check(client.Request("hello", Json::Object(), response, error),
				"hello handshake", error);
			const Json* result = response.Find("result");
			checker.Check(result && ArgInt(*result, "protocol_version") == kProtocolVersion,
				"protocol version matches", error);

			ParsedCommand parsed;
			ParseCommandLine(Tokenize("read 0xE9E0 16", error), parsed, error);
			checker.Check(client.Request(parsed.command, parsed.arguments, response, error),
				"read request round trip", error);
			result = response.Find("result");
			checker.Check(result && result->Find("data") &&
					result->Find("data")->AsString() == "000102030405060708090a0b0c0d0e0f",
				"read returns the expected bytes", error);
			checker.Check(server.bad_request_count() == 0,
				"the server saw the expected request shape");

			checker.Check(client.Request("status", Json::Object(), response, error),
				"status request round trip", error);
			result = response.Find("result");
			checker.Check(result && ArgInt(*result, "pc") == 0x1234, "status decodes", error);

			// Malformed JSON must come back as a plain protocol error.
			std::string raw_error;
			if (client.SendLine("this is not json", raw_error)) {
				std::string line;
				const bool got_line = client.ReadLine(line, raw_error);
				Json invalid;
				std::string parse_error;
				checker.Check(got_line && !Json::Parse(line, invalid, parse_error),
					"malformed request produces a protocol error", raw_error);
			}
			else {
				checker.Check(false, "send a malformed request", raw_error);
			}

			checker.Check(client.Request("frobnicate", Json::Object(), response, error),
				"unknown command still returns a response", error);
			const Json* ok = response.Find("ok");
			const Json* code = response.Find("code");
			checker.Check(ok && !ok->AsBool(true) && code && code->AsString() == "unknown-command",
				"unknown command is rejected with a code", error);
			client.Close();
		}

		server.Stop();
		RemoveSessionFiles(static_cast<int>(::getpid()));
		RemoveSessionFiles(dead_pid);
		::unsetenv("CASIOEMU_CTL_DIR");
		std::error_code remove_error;
		std::filesystem::remove_all(control_dir, remove_error);

		if (json_output) {
			Json report = Json::Object();
			report["ok"] = checker.failures().empty();
			report["checks"] = checker.checks();
			Json failures = Json::Array();
			for (const auto& failure : checker.failures())
				failures.Push(failure);
			report["failures"] = std::move(failures);
			std::cout << report.Dump() << "\n";
		}
		else if (checker.failures().empty()) {
			std::cout << "casioctl selftest: " << checker.checks() << " checks passed\n";
		}
		else {
			std::cout << "casioctl selftest: " << checker.failures().size() << " of " << checker.checks()
				<< " checks failed\n";
			for (const auto& failure : checker.failures())
				std::cout << "  [fail] " << failure << "\n";
		}
		return checker.failures().empty() ? 0 : 1;
	}

} // namespace casioctl
