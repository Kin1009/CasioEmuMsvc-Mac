#include "Commands.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace casioctl {
	namespace {
		std::string Lower(std::string value) {
			for (char& ch : value)
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			return value;
		}

		bool ParseHex(const std::string& token, uint64_t max_value, uint64_t& value) {
			std::string digits = token;
			if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
				digits = digits.substr(2);
			if (digits.empty())
				return false;
			uint64_t result = 0;
			for (char ch : digits) {
				int digit = -1;
				if (ch >= '0' && ch <= '9')
					digit = ch - '0';
				else if (ch >= 'a' && ch <= 'f')
					digit = ch - 'a' + 10;
				else if (ch >= 'A' && ch <= 'F')
					digit = ch - 'A' + 10;
				else
					return false;
				if (result > (max_value - static_cast<uint64_t>(digit)) / 16)
					return false;
				result = result * 16 + static_cast<uint64_t>(digit);
			}
			value = result;
			return true;
		}

		bool ParseCount(const std::string& token, uint64_t max_value, uint64_t& value) {
			if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
				return ParseHex(token, max_value, value);
			if (token.empty())
				return false;
			uint64_t result = 0;
			for (char ch : token) {
				if (ch < '0' || ch > '9')
					return false;
				const uint64_t digit = static_cast<uint64_t>(ch - '0');
				if (result > (max_value - digit) / 10)
					return false;
				result = result * 10 + digit;
			}
			value = result;
			return true;
		}

		bool ParseAddress(const std::string& token, uint32_t& value) {
			uint64_t parsed = 0;
			if (!ParseHex(token, 0xffffffffull, parsed))
				return false;
			value = static_cast<uint32_t>(parsed);
			return true;
		}

		bool ParseLength(const std::string& token, uint32_t max_value, uint32_t& value) {
			uint64_t parsed = 0;
			if (!ParseCount(token, max_value, parsed))
				return false;
			value = static_cast<uint32_t>(parsed);
			return true;
		}

		bool ParseBytes(const std::string& token, std::string& hex, std::string& error) {
			hex.clear();
			for (char ch : token) {
				if (ch == ' ' || ch == '\t' || ch == ':' || ch == ',' || ch == '_')
					continue;
				if (!std::isxdigit(static_cast<unsigned char>(ch))) {
					error = "invalid hex byte string: " + token;
					return false;
				}
				hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
			}
			if (hex.empty() || hex.size() % 2 != 0) {
				error = "hex data must contain an even number of digits";
				return false;
			}
			return true;
		}

		std::string Join(const std::vector<std::string>& tokens, size_t from) {
			std::string result;
			for (size_t i = from; i < tokens.size(); ++i) {
				if (!result.empty())
					result.push_back(' ');
				result += tokens[i];
			}
			return result;
		}

		bool ParseNumberArgument(const std::string& token, uint64_t max_value, uint64_t& value) {
			return ParseCount(token, max_value, value);
		}

		std::vector<uint8_t> DecodeHex(const std::string& hex) {
			std::vector<uint8_t> bytes;
			bytes.reserve(hex.size() / 2);
			for (size_t i = 0; i + 1 < hex.size(); i += 2) {
				auto nibble = [](char ch) -> int {
					if (ch >= '0' && ch <= '9')
						return ch - '0';
					if (ch >= 'a' && ch <= 'f')
						return ch - 'a' + 10;
					if (ch >= 'A' && ch <= 'F')
						return ch - 'A' + 10;
					return 0;
				};
				bytes.push_back(static_cast<uint8_t>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
			}
			return bytes;
		}

		std::string HexDump(const std::vector<uint8_t>& bytes, uint32_t base, const char* title) {
			std::ostringstream out;
			if (title && *title)
				out << title << " (" << bytes.size() << " bytes)\n";
			char header[64];
			std::snprintf(header, sizeof(header), "%08x", base);
			for (size_t offset = 0; offset < bytes.size(); offset += 16) {
				const size_t count = std::min<size_t>(16, bytes.size() - offset);
				char address[16];
				std::snprintf(address, sizeof(address), "%08x", base + static_cast<uint32_t>(offset));
				out << address << "  ";
				for (size_t i = 0; i < 16; ++i) {
					if (i < count) {
						char byte[4];
						std::snprintf(byte, sizeof(byte), "%02x ", bytes[offset + i]);
						out << byte;
					}
					else {
						out << "   ";
					}
					if (i == 7)
						out << ' ';
				}
				out << " |";
				for (size_t i = 0; i < count; ++i) {
					const unsigned char ch = bytes[offset + i];
					out << (ch >= 0x20 && ch < 0x7f ? static_cast<char>(ch) : '.');
				}
				out << "|\n";
			}
			(void)header;
			return out.str();
		}

		std::string HexValue(uint32_t value, uint32_t bits) {
			char buffer[32];
			int width = static_cast<int>((bits + 3) / 4);
			if (width < 1)
				width = 1;
			if (width > 8)
				width = 8;
			std::snprintf(buffer, sizeof(buffer), "0x%0*X", width, value);
			return buffer;
		}

		std::string FormatAddressList(const Json& list) {
			std::ostringstream out;
			bool first = true;
			for (const auto& item : list.Items()) {
				if (!first)
					out << ", ";
				first = false;
				char buffer[16];
				std::snprintf(buffer, sizeof(buffer), "0x%X", static_cast<unsigned>(item.AsInt(0)));
				out << buffer;
			}
			if (first)
				out << "(none)";
			return out.str();
		}

		std::string FormatKey(const Json& result) {
			std::ostringstream out;
			const std::string name = result.Find("name") ? result.Find("name")->AsString() : std::string();
			const int code = result.Find("code") ? static_cast<int>(result.Find("code")->AsInt(-1)) : -1;
			if (result.Find("pressed") && result.Find("pressed")->AsBool())
				out << "pressed ";
			else if (result.Find("pressed"))
				out << "released ";
			else
				out << "key ";
			out << (name.empty() ? "?" : name);
			if (code >= 0) {
				char buffer[32];
				std::snprintf(buffer, sizeof(buffer), " (code 0x%02X", code);
				out << buffer;
				if (const Json* ki = result.Find("ki")) {
					if (const Json* ko = result.Find("ko"))
						out << ", ki=" << ki->AsInt(0) << " ko=" << ko->AsInt(0);
				}
				out << ")";
			}
			if (const Json* hold = result.Find("hold_ms"))
				out << ", release in " << hold->AsInt(0) << " ms";
			return out.str();
		}

		std::string FormatSnapshotList(const Json& result) {
			std::ostringstream out;
			const Json* snapshots = result.Find("snapshots");
			if (!snapshots || snapshots->Items().empty()) {
				out << "no snapshots\n";
				return out.str();
			}
			out << "ID    PARENT  LABEL                              SIZE\n";
			for (const auto& entry : snapshots->Items()) {
				char line[256];
				std::snprintf(line, sizeof(line), "%-5lld %-7lld %-34s %llu\n",
					static_cast<long long>(entry.Find("id") ? entry.Find("id")->AsInt(0) : 0),
					static_cast<long long>(entry.Find("parent") ? entry.Find("parent")->AsInt(0) : 0),
					entry.Find("label") ? entry.Find("label")->AsString().c_str() : "",
					static_cast<unsigned long long>(entry.Find("state_bytes") ? entry.Find("state_bytes")->AsInt(0) : 0));
				out << line;
			}
			return out.str();
		}

		std::string FormatStatus(const Json& result) {
			std::ostringstream out;
			auto field = [&](const char* key) -> std::string {
				const Json* value = result.Find(key);
				if (!value)
					return "-";
				if (value->IsString())
					return value->AsString();
				if (value->IsBool())
					return value->AsBool() ? "yes" : "no";
				return std::to_string(value->AsInt(0));
			};
			out << "model      " << field("model") << "\n";
			out << "hardware   " << field("hardware") << " (id " << field("hardware_id") << ")\n";
			out << "path       " << field("model_path") << "\n";
			out << "rom        " << field("rom_path") << "\n";
			out << "state      " << (result.Find("paused") && result.Find("paused")->AsBool() ? "paused" : "running") << "\n";
			const Json* registers = result.Find("registers");
			auto register_value = [&](const char* name) -> std::string {
				if (!registers)
					return "-";
				const Json* value = registers->Find(name);
				if (!value || value->IsNull())
					return "-";
				char buffer[24];
				std::snprintf(buffer, sizeof(buffer), "0x%X", static_cast<unsigned>(value->AsInt(0)));
				return buffer;
			};
			char pc[24];
			std::snprintf(pc, sizeof(pc), "0x%X", static_cast<unsigned>(result.Find("pc") ? result.Find("pc")->AsInt(0) : 0));
			out << "pc         " << pc << "  sp " << register_value("sp") << "  lr " << register_value("lr") << "\n";
			const Json* rom = result.Find("rom");
			const Json* ram = result.Find("ram");
			if (rom && ram) {
				char buffer[128];
				std::snprintf(buffer, sizeof(buffer), "rom image  %llu bytes @ 0x%llX\n",
					static_cast<unsigned long long>(rom->Find("size") ? rom->Find("size")->AsInt(0) : 0),
					static_cast<unsigned long long>(rom->Find("base") ? rom->Find("base")->AsInt(0) : 0));
				out << buffer;
				std::snprintf(buffer, sizeof(buffer), "ram        0x%llX..0x%llX (%llu bytes)\n",
					static_cast<unsigned long long>(ram->Find("base") ? ram->Find("base")->AsInt(0) : 0),
					static_cast<unsigned long long>((ram->Find("base") ? ram->Find("base")->AsInt(0) : 0) +
						(ram->Find("size") ? ram->Find("size")->AsInt(0) : 0)),
					static_cast<unsigned long long>(ram->Find("size") ? ram->Find("size")->AsInt(0) : 0));
				out << buffer;
			}
			out << "run mode   " << field("run_mode") << "\n";
			out << "cycles/s   " << field("cycles_per_second") << "\n";
			return out.str();
		}
	} // namespace

	std::vector<std::string> Tokenize(const std::string& line, std::string& error) {
		std::vector<std::string> tokens;
		std::string current;
		bool in_token = false;
		bool quoted = false;
		error.clear();
		for (size_t i = 0; i < line.size(); ++i) {
			const char ch = line[i];
			if (quoted) {
				if (ch == '\\' && i + 1 < line.size()) {
					const char next = line[++i];
					switch (next) {
					case 'n':
						current.push_back('\n');
						break;
					case 't':
						current.push_back('\t');
						break;
					default:
						current.push_back(next);
						break;
					}
					continue;
				}
				if (ch == '"') {
					quoted = false;
					continue;
				}
				current.push_back(ch);
				continue;
			}
			if (ch == '"') {
				quoted = true;
				in_token = true;
				continue;
			}
			if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
				if (in_token) {
					tokens.push_back(current);
					current.clear();
					in_token = false;
				}
				continue;
			}
			in_token = true;
			current.push_back(ch);
		}
		if (quoted) {
			error = "unterminated quote";
			return {};
		}
		if (in_token)
			tokens.push_back(current);
		return tokens;
	}

	bool ParseCommandLine(const std::vector<std::string>& tokens, ParsedCommand& parsed, std::string& error) {
		parsed = ParsedCommand{};
		parsed.arguments = Json::Object();
		if (tokens.empty()) {
			error = "empty command";
			return false;
		}
		const std::string& command = tokens[0];
		error.clear();

		auto expect = [&](size_t count, const char* usage) -> bool {
			if (tokens.size() != count) {
				error = std::string("usage: ") + usage;
				return false;
			}
			return true;
		};

		if (command == "status" || command == "pause" || command == "resume" || command == "reset" ||
			command == "regs" || command == "hello" || command == "bt" || command == "vars" ||
			command == "step-over" || command == "step-out" || command == "shutdown") {
			if (!expect(1, command.c_str()))
				return false;
			parsed.command = command == "step-over" ? "step.over"
				: command == "step-out" ? "step.out"
				: command;
			return true;
		}

		if (command == "step") {
			if (tokens.size() > 2) {
				error = "usage: step [count]";
				return false;
			}
			uint64_t count = 1;
			if (tokens.size() == 2 && !ParseNumberArgument(tokens[1], 10000, count)) {
				error = "step count must be a number from 1 to 10000";
				return false;
			}
			if (count == 0) {
				error = "step count must be at least 1";
				return false;
			}
			parsed.command = "step";
			parsed.arguments["n"] = count;
			return true;
		}

		if (command == "reg") {
			if (tokens.size() < 3) {
				error = "usage: reg get <name> | reg set <name> <value>";
				return false;
			}
			const std::string action = Lower(tokens[1]);
			if (action == "get") {
				if (tokens.size() != 3) {
					error = "usage: reg get <name>";
					return false;
				}
				parsed.command = "reg.get";
				parsed.arguments["name"] = tokens[2];
				return true;
			}
			if (action == "set") {
				if (tokens.size() != 4) {
					error = "usage: reg set <name> <value>";
					return false;
				}
				uint64_t value = 0;
				if (!ParseHex(tokens[3], 0xffffffffull, value)) {
					error = "register value must be hexadecimal (e.g. 0x1234)";
					return false;
				}
				parsed.command = "reg.set";
				parsed.arguments["name"] = tokens[2];
				parsed.arguments["value"] = value;
				return true;
			}
			error = "usage: reg get <name> | reg set <name> <value>";
			return false;
		}

		if (command == "read" || command == "read-rom") {
			if (tokens.size() != 3) {
				error = "usage: " + command + " <address> <length>";
				return false;
			}
			uint32_t address = 0;
			uint32_t length = 0;
			if (!ParseAddress(tokens[1], address)) {
				error = "invalid hexadecimal address '" + tokens[1] + "'";
				return false;
			}
			if (!ParseLength(tokens[2], 1024 * 1024, length) || length == 0) {
				error = "invalid length '" + tokens[2] + "'";
				return false;
			}
			parsed.command = command;
			parsed.arguments["addr"] = address;
			parsed.arguments["len"] = length;
			return true;
		}

		if (command == "read-code") {
			if (tokens.size() != 3) {
				error = "usage: read-code <address> <word count>";
				return false;
			}
			uint32_t address = 0;
			uint32_t count = 0;
			if (!ParseAddress(tokens[1], address)) {
				error = "invalid hexadecimal address '" + tokens[1] + "'";
				return false;
			}
			if (!ParseLength(tokens[2], 4096, count) || count == 0) {
				error = "invalid word count '" + tokens[2] + "'";
				return false;
			}
			parsed.command = "read-code";
			parsed.arguments["addr"] = address;
			parsed.arguments["count"] = count;
			return true;
		}

		if (command == "write" || command == "write-rom" || command == "write-code") {
			if (tokens.size() != 3) {
				error = "usage: " + command + " <address> <hexbytes>";
				return false;
			}
			uint32_t address = 0;
			if (!ParseAddress(tokens[1], address)) {
				error = "invalid hexadecimal address '" + tokens[1] + "'";
				return false;
			}
			std::string hex;
			if (!ParseBytes(tokens[2], hex, error))
				return false;
			parsed.command = command;
			parsed.arguments["addr"] = address;
			parsed.arguments["data"] = hex;
			return true;
		}

		if (command == "disasm") {
			if (tokens.size() > 3) {
				error = "usage: disasm [address] [count]";
				return false;
			}
			if (tokens.size() >= 2) {
				uint32_t address = 0;
				if (!ParseAddress(tokens[1], address)) {
					error = "invalid hexadecimal address '" + tokens[1] + "'";
					return false;
				}
				parsed.arguments["addr"] = address;
			}
			if (tokens.size() >= 3) {
				uint32_t count = 0;
				if (!ParseLength(tokens[2], 4096, count) || count == 0) {
					error = "invalid instruction count '" + tokens[2] + "'";
					return false;
				}
				parsed.arguments["count"] = count;
			}
			parsed.command = "disasm";
			return true;
		}

		if (command == "bp") {
			if (tokens.size() < 2) {
				error = "usage: bp set <addr> | bp clear <addr> | bp clearall | bp list | bp mem <addr> <len>";
				return false;
			}
			const std::string action = Lower(tokens[1]);
			if (action == "list" && tokens.size() == 2) {
				parsed.command = "bp.list";
				return true;
			}
			if (action == "clearall" && tokens.size() == 2) {
				parsed.command = "bp.clearall";
				return true;
			}
			if (action == "hits") {
				if (tokens.size() < 3 || tokens.size() > 4) {
					error = "usage: bp hits <address> [read|write]";
					return false;
				}
				uint32_t address = 0;
				if (!ParseAddress(tokens[2], address)) {
					error = "invalid hexadecimal address '" + tokens[2] + "'";
					return false;
				}
				std::string access = "write";
				if (tokens.size() == 4) {
					access = Lower(tokens[3]);
					if (access != "read" && access != "write") {
						error = "access must be read or write";
						return false;
					}
				}
				parsed.command = "bp.hits";
				parsed.arguments["addr"] = address;
				parsed.arguments["access"] = access;
				return true;
			}
			if (action == "set" || action == "clear") {
				if (tokens.size() != 3) {
					error = "usage: bp " + action + " <address>";
					return false;
				}
				uint32_t address = 0;
				if (!ParseAddress(tokens[2], address)) {
					error = "invalid hexadecimal address '" + tokens[2] + "'";
					return false;
				}
				parsed.command = action == "set" ? "bp.set" : "bp.clear";
				parsed.arguments["addr"] = address;
				return true;
			}
			if (action == "mem") {
				if (tokens.size() == 3 && Lower(tokens[2]) == "clearall") {
					parsed.command = "bp.mem.clearall";
					return true;
				}
				if (tokens.size() >= 3 && Lower(tokens[2]) == "clear") {
					if (tokens.size() < 4 || tokens.size() > 6) {
						error = "usage: bp mem clear <address> [length] [read|write|both]";
						return false;
					}
					uint32_t address = 0;
					if (!ParseAddress(tokens[3], address)) {
						error = "invalid hexadecimal address '" + tokens[3] + "'";
						return false;
					}
					uint32_t length = 1;
					if (tokens.size() >= 5 && !ParseLength(tokens[4], 256, length)) {
						error = "invalid length '" + tokens[4] + "'";
						return false;
					}
					std::string access = "both";
					if (tokens.size() >= 6) {
						access = Lower(tokens[5]);
						if (access != "read" && access != "write" && access != "both") {
							error = "access must be read, write or both";
							return false;
						}
					}
					parsed.command = "bp.mem.clear";
					parsed.arguments["addr"] = address;
					parsed.arguments["len"] = length;
					parsed.arguments["access"] = access;
					return true;
				}
				if (tokens.size() < 4) {
					error = "usage: bp mem <address> <length> [read|write|both] [--record] [--compare <hex>] [--mask <hex>] [--skip <n>]";
					return false;
				}
				uint32_t address = 0;
				uint32_t length = 0;
				if (!ParseAddress(tokens[2], address)) {
					error = "invalid hexadecimal address '" + tokens[2] + "'";
					return false;
				}
				if (!ParseLength(tokens[3], 256, length) || length == 0) {
					error = "invalid length '" + tokens[3] + "' (max 256)";
					return false;
				}
				std::string access = "both";
				bool break_when_hit = true;
				bool compare_data = false;
				uint64_t compare_byte = 0;
				uint64_t compare_mask = 0xff;
				uint64_t skip = 0;
				size_t index = 4;
				if (index < tokens.size()) {
					const std::string candidate = Lower(tokens[index]);
					if (candidate == "read" || candidate == "write" || candidate == "both") {
						access = candidate;
						++index;
					}
				}
				for (; index < tokens.size(); ++index) {
					const std::string flag = tokens[index];
					if (flag == "--record") {
						break_when_hit = false;
						continue;
					}
					auto value_token = [&](const char* what) -> bool {
						if (index + 1 >= tokens.size()) {
							error = std::string(flag) + " requires a " + what;
							return false;
						}
						++index;
						return true;
					};
					if (flag == "--compare") {
						if (!value_token("hex byte"))
							return false;
						if (!ParseHex(tokens[index], 0xff, compare_byte)) {
							error = "compare value must be a byte in hexadecimal";
							return false;
						}
						compare_data = true;
						continue;
					}
					if (flag == "--mask") {
						if (!value_token("hex byte"))
							return false;
						if (!ParseHex(tokens[index], 0xff, compare_mask)) {
							error = "mask must be a byte in hexadecimal";
							return false;
						}
						continue;
					}
					if (flag == "--skip") {
						if (!value_token("count"))
							return false;
						if (!ParseNumberArgument(tokens[index], 0xffffffffull, skip)) {
							error = "skip count must be an integer";
							return false;
						}
						continue;
					}
					error = "unknown bp mem option '" + flag + "'";
					return false;
				}
				parsed.command = "bp.mem.set";
				parsed.arguments["addr"] = address;
				parsed.arguments["len"] = length;
				parsed.arguments["access"] = access;
				parsed.arguments["break_when_hit"] = break_when_hit;
				parsed.arguments["compare_data"] = compare_data;
				parsed.arguments["data"] = compare_byte;
				parsed.arguments["mask"] = compare_mask;
				parsed.arguments["skip"] = skip;
				return true;
			}
			error = "usage: bp set <addr> | bp clear <addr> | bp clearall | bp list | bp mem <addr> <len>";
			return false;
		}

		if (command == "key") {
			if (tokens.size() < 2) {
				error = "usage: key press <name> | key release <name> | key hold <name> [ms] | key releaseall | key matrix <ki> <ko> [press|release] | key list";
				return false;
			}
			const std::string action = Lower(tokens[1]);
			if (action == "releaseall" && tokens.size() == 2) {
				parsed.command = "key.releaseall";
				return true;
			}
			if (action == "list" && tokens.size() == 2) {
				parsed.command = "key.list";
				return true;
			}
			if (action == "press" || action == "release") {
				if (tokens.size() < 3) {
					error = "usage: key " + action + " <name|code>";
					return false;
				}
				parsed.command = action == "press" ? "key.press" : "key.release";
				parsed.arguments["name"] = Join(tokens, 2);
				return true;
			}
			if (action == "hold") {
				if (tokens.size() < 3) {
					error = "usage: key hold <name> [milliseconds]";
					return false;
				}
				uint64_t delay = 120;
				size_t name_end = tokens.size();
				if (tokens.size() >= 4) {
					uint64_t parsed_delay = 0;
					if (ParseNumberArgument(tokens.back(), 60000, parsed_delay)) {
						delay = parsed_delay;
						name_end = tokens.size() - 1;
					}
				}
				std::string name;
				for (size_t i = 2; i < name_end; ++i) {
					if (!name.empty())
						name.push_back(' ');
					name += tokens[i];
				}
				parsed.command = "key.hold";
				parsed.arguments["name"] = name;
				parsed.arguments["ms"] = delay;
				return true;
			}
			if (action == "matrix") {
				if (tokens.size() < 4 || tokens.size() > 5) {
					error = "usage: key matrix <ki> <ko> [press|release]";
					return false;
				}
				uint64_t ki = 0;
				uint64_t ko = 0;
				if (!ParseNumberArgument(tokens[2], 7, ki) || !ParseNumberArgument(tokens[3], 15, ko)) {
					error = "matrix coordinates must be ki 0-7 and ko 0-15";
					return false;
				}
				std::string key_action = "press";
				if (tokens.size() == 5) {
					key_action = Lower(tokens[4]);
					if (key_action != "press" && key_action != "release") {
						error = "matrix action must be press or release";
						return false;
					}
				}
				parsed.command = "key.matrix";
				parsed.arguments["ki"] = ki;
				parsed.arguments["ko"] = ko;
				parsed.arguments["action"] = key_action;
				return true;
			}
			error = "usage: key press <name> | key release <name> | key hold <name> [ms] | key releaseall | key matrix <ki> <ko> [press|release] | key list";
			return false;
		}

		if (command == "screenshot") {
			if (tokens.size() < 2 || tokens.size() > 3) {
				error = "usage: screenshot <file.png|file.rgba> [--raw]";
				return false;
			}
			std::string format = "png";
			if (tokens.size() == 3) {
				if (tokens[2] != "--raw") {
					error = "usage: screenshot <file.png|file.rgba> [--raw]";
					return false;
				}
				format = "rgba";
			}
			parsed.command = "screenshot";
			parsed.arguments["path"] = tokens[1];
			parsed.arguments["format"] = format;
			return true;
		}

		if (command == "inject") {
			if (tokens.size() < 3 || tokens.size() > 4) {
				error = "usage: inject <address> <hexbytes> [run]";
				return false;
			}
			uint32_t address = 0;
			if (!ParseAddress(tokens[1], address)) {
				error = "invalid hexadecimal address '" + tokens[1] + "'";
				return false;
			}
			std::string hex;
			if (!ParseBytes(tokens[2], hex, error))
				return false;
			bool run = false;
			if (tokens.size() == 4) {
				if (Lower(tokens[3]) != "run") {
					error = "usage: inject <address> <hexbytes> [run]";
					return false;
				}
				run = true;
			}
			parsed.command = "inject";
			parsed.arguments["addr"] = address;
			parsed.arguments["data"] = hex;
			parsed.arguments["run"] = run;
			return true;
		}

		if (command == "snapshot") {
			if (tokens.size() < 2) {
				error = "usage: snapshot list | snapshot save [label] | snapshot load <id> | snapshot delete <id>";
				return false;
			}
			const std::string action = Lower(tokens[1]);
			if (action == "list" && tokens.size() == 2) {
				parsed.command = "snapshot.list";
				return true;
			}
			if (action == "export") {
				if (tokens.size() < 3 || tokens.size() > 5) {
					error = "usage: snapshot export <path> [id] [--subtree]";
					return false;
				}
				uint64_t id = 0;
				bool subtree = false;
				for (size_t index = 3; index < tokens.size(); ++index) {
					if (tokens[index] == "--subtree") {
						subtree = true;
						continue;
					}
					if (!ParseNumberArgument(tokens[index], 0xffffffffull, id)) {
						error = "usage: snapshot export <path> [id] [--subtree]";
						return false;
					}
				}
				parsed.command = "snapshot.export";
				parsed.arguments["path"] = tokens[2];
				parsed.arguments["id"] = id;
				parsed.arguments["subtree"] = subtree;
				return true;
			}
			if (action == "import") {
				if (tokens.size() != 3) {
					error = "usage: snapshot import <path>";
					return false;
				}
				parsed.command = "snapshot.import";
				parsed.arguments["path"] = tokens[2];
				return true;
			}
			if (action == "save") {
				parsed.command = "snapshot.save";
				parsed.arguments["label"] = tokens.size() > 2 ? Join(tokens, 2) : "casioctl snapshot";
				return true;
			}
			if (action == "load" || action == "delete") {
				if (tokens.size() != 3) {
					error = "usage: snapshot " + action + " <id>";
					return false;
				}
				uint64_t id = 0;
				if (!ParseNumberArgument(tokens[2], 0xffffffffull, id)) {
					error = "invalid snapshot id '" + tokens[2] + "'";
					return false;
				}
				parsed.command = action == "load" ? "snapshot.load" : "snapshot.delete";
				parsed.arguments["id"] = id;
				return true;
			}
			error = "usage: snapshot list | snapshot save [label] | snapshot load <id> | snapshot delete <id>";
			return false;
		}

		if (command == "watch") {
			if (tokens.size() < 2) {
				error = "usage: watch list | watch set <addr> <value> | watch delete <addr> | watch clearall";
				return false;
			}
			const std::string action = Lower(tokens[1]);
			if (action == "list" && tokens.size() == 2) {
				parsed.command = "watch.list";
				return true;
			}
			if (action == "clearall" && tokens.size() == 2) {
				parsed.command = "watch.clearall";
				return true;
			}
			if (action == "set") {
				if (tokens.size() != 4) {
					error = "usage: watch set <address> <value>";
					return false;
				}
				uint32_t address = 0;
				uint64_t value = 0;
				if (!ParseAddress(tokens[2], address)) {
					error = "invalid hexadecimal address '" + tokens[2] + "'";
					return false;
				}
				if (!ParseHex(tokens[3], 0xff, value)) {
					error = "watch value must be a byte in hexadecimal";
					return false;
				}
				parsed.command = "watch.set";
				parsed.arguments["addr"] = address;
				parsed.arguments["value"] = value;
				return true;
			}
			if (action == "delete") {
				if (tokens.size() != 3) {
					error = "usage: watch delete <address>";
					return false;
				}
				uint32_t address = 0;
				if (!ParseAddress(tokens[2], address)) {
					error = "invalid hexadecimal address '" + tokens[2] + "'";
					return false;
				}
				parsed.command = "watch.delete";
				parsed.arguments["addr"] = address;
				return true;
			}
			error = "usage: watch list | watch set <addr> <value> | watch delete <addr> | watch clearall";
			return false;
		}

		if (command == "call") {
			if (tokens.size() < 2) {
				error = "usage: call start [caller] [callee] | call stop | call clear | call list [function]";
				return false;
			}
			const std::string action = Lower(tokens[1]);
			if (action == "start") {
				if (tokens.size() > 4) {
					error = "usage: call start [caller address] [callee address]";
					return false;
				}
				uint32_t caller = 0;
				uint32_t callee = 0;
				if (tokens.size() >= 3 && !ParseAddress(tokens[2], caller)) {
					error = "invalid caller address '" + tokens[2] + "'";
					return false;
				}
				if (tokens.size() == 4 && !ParseAddress(tokens[3], callee)) {
					error = "invalid callee address '" + tokens[3] + "'";
					return false;
				}
				parsed.command = "call.start";
				parsed.arguments["caller"] = caller;
				parsed.arguments["callee"] = callee;
				return true;
			}
			if (action == "stop" && tokens.size() == 2) {
				parsed.command = "call.stop";
				return true;
			}
			if (action == "clear" && tokens.size() == 2) {
				parsed.command = "call.clear";
				return true;
			}
			if (action == "list") {
				if (tokens.size() > 3) {
					error = "usage: call list [function address]";
					return false;
				}
				uint32_t function = 0;
				if (tokens.size() == 3 && !ParseAddress(tokens[2], function)) {
					error = "invalid function address '" + tokens[2] + "'";
					return false;
				}
				parsed.command = "call.list";
				parsed.arguments["function"] = function;
				return true;
			}
			error = "usage: call start [caller] [callee] | call stop | call clear | call list [function]";
			return false;
		}

		if (command == "hw") {
			if (tokens.size() == 1) {
				parsed.command = "hw";
				return true;
			}
			const std::string action = Lower(tokens[1]);
			if (action == "cps" || action == "pd" || action == "interrupt") {
				if (tokens.size() != 3) {
					error = "usage: hw " + action + " <value>";
					return false;
				}
				uint64_t value = 0;
				if (action == "interrupt") {
					if (!ParseNumberArgument(tokens[2], 255, value)) {
						error = "interrupt index must be 0-255";
						return false;
					}
				}
				else if (!ParseHex(tokens[2], action == "pd" ? 0xff : 0xffffffffull, value)) {
					error = "hw " + action + " value must be hexadecimal";
					return false;
				}
				parsed.command = action == "cps" ? "hw.cps" : action == "pd" ? "hw.pd" : "hw.interrupt";
				parsed.arguments[action == "interrupt" ? "index" : "value"] = value;
				return true;
			}
			if (action == "display") {
				if (tokens.size() == 2) {
					parsed.command = "hw.display";
					return true;
				}
				if (tokens.size() < 5 || Lower(tokens[2]) != "set") {
					error = "usage: hw display | hw display set <key> <value>";
					return false;
				}
				const std::string key = Lower(tokens[3]);
				const std::string value = tokens[4];
				const bool is_bool = key == "fading_enabled" || key == "residual_enabled" || key == "audio_enabled";
				const bool is_float = key == "flashing_brightness" || key == "fading_coefficient" || key == "residual_alpha";
				const bool is_int = key == "flashing_threshold" || key == "buffer";
				if (!is_bool && !is_float && !is_int) {
					error = "unknown display setting '" + key + "'";
					return false;
				}
				Json parsed_value;
				if (is_bool) {
					const std::string lowered = Lower(value);
					if (lowered == "on" || lowered == "true" || lowered == "1" || lowered == "yes")
						parsed_value = true;
					else if (lowered == "off" || lowered == "false" || lowered == "0" || lowered == "no")
						parsed_value = false;
					else {
						error = "display boolean must be on/off";
						return false;
					}
				}
				else if (is_float) {
					char* end = nullptr;
					const double number = std::strtod(value.c_str(), &end);
					if (!end || *end != '\0') {
						error = "display value must be a number";
						return false;
					}
					parsed_value = number;
				}
				else {
					uint64_t number = 0;
					if (!ParseNumberArgument(value, 0xffffull, number)) {
						error = "display value must be an integer";
						return false;
					}
					parsed_value = number;
				}
				parsed.command = "hw.display.set";
				parsed.arguments[key] = parsed_value;
				return true;
			}
			if (action == "reload-rom" && tokens.size() == 2) {
				parsed.command = "hw.reload-rom";
				return true;
			}
			if (action == "record") {
				if (tokens.size() != 3) {
					error = "usage: hw record start|stop";
					return false;
				}
				const std::string mode = Lower(tokens[2]);
				if (mode != "start" && mode != "stop") {
					error = "usage: hw record start|stop";
					return false;
				}
				parsed.command = "hw.record";
				parsed.arguments["start"] = mode == "start";
				return true;
			}
			if (action == "qr" && tokens.size() == 2) {
				parsed.command = "hw.qr";
				return true;
			}
			error = "usage: hw | hw cps <n> | hw pd <v> | hw display [set <key> <value>] | hw interrupt <n> | hw reload-rom | hw record start|stop | hw qr";
			return false;
		}

		if (command == "labels") {
			parsed.command = "labels";
			if (tokens.size() > 1)
				parsed.arguments["query"] = Join(tokens, 1);
			return true;
		}

		error = "unknown command '" + command + "'";
		return false;
	}

	std::string FormatResult(const std::string& command, const Json& result) {
		std::ostringstream out;
		if (command == "hello") {
			auto field = [&](const char* key) -> std::string {
				const Json* value = result.Find(key);
				if (!value)
					return std::string("-");
				if (value->IsString())
					return value->AsString();
				if (value->IsBool())
					return value->AsBool() ? "yes" : "no";
				return std::to_string(value->AsInt(0));
			};
			out << "protocol   " << field("protocol") << " v" << field("protocol_version") << "\n";
			out << "app        " << field("app") << " " << field("app_version") << " (pid " << field("pid") << ")\n";
			out << "model      " << field("model") << " [" << field("hardware") << "]\n";
			out << "state      " << (result.Find("paused") && result.Find("paused")->AsBool() ? "paused" : "running") << "\n";
			out << "debugger   " << field("debugger") << " (ui " << field("debugger_ui") << ", snapshots " << field("snapshot_ui") << ")\n";
			out << "keyboard   " << field("keyboard") << ", screenshot " << field("screenshot") << "\n";
			const Json* capabilities = result.Find("capabilities");
			if (capabilities) {
				out << "commands   ";
				bool first = true;
				for (const auto& item : capabilities->Items()) {
					if (!first)
						out << ' ';
					first = false;
					out << item.AsString();
				}
				out << "\n";
			}
			return out.str();
		}
		if (command == "status")
			return FormatStatus(result);
		if (command == "pause" || command == "resume" || command == "reset") {
			const bool paused = result.Find("paused") && result.Find("paused")->AsBool();
			out << (command == "reset" ? "reset; " : "") << (paused ? "paused" : "running");
			if (const Json* pc = result.Find("pc")) {
				char buffer[24];
				std::snprintf(buffer, sizeof(buffer), ", pc=0x%X", static_cast<unsigned>(pc->AsInt(0)));
				out << buffer;
			}
			out << "\n";
			return out.str();
		}
		if (command == "step" || command == "step.over" || command == "step.out") {
			out << "stepped";
			if (command == "step.over")
				out << " over";
			else if (command == "step.out")
				out << " out";
			else
				out << " " << (result.Find("steps") ? result.Find("steps")->AsInt(0) : 0) << " instruction(s)";
			if (const Json* pc = result.Find("pc")) {
				char buffer[24];
				std::snprintf(buffer, sizeof(buffer), ", pc=0x%X", static_cast<unsigned>(pc->AsInt(0)));
				out << buffer;
			}
			out << "\n";
			return out.str();
		}
		if (command == "regs") {
			const Json* registers = result.Find("registers");
			if (!registers)
				return "no registers\n";
			for (const auto& register_info : registers->Items()) {
				const std::string name = register_info.Find("name") ? register_info.Find("name")->AsString() : "?";
				const uint32_t value = static_cast<uint32_t>(register_info.Find("value") ? register_info.Find("value")->AsInt(0) : 0);
				const uint32_t bits = static_cast<uint32_t>(register_info.Find("bits") ? register_info.Find("bits")->AsInt(0) : 0);
				out << name;
				for (size_t pad = name.size(); pad < 12; ++pad)
					out << ' ';
				out << HexValue(value, bits) << "  (" << bits << " bits)\n";
			}
			return out.str();
		}
		if (command == "reg.get" || command == "reg.set") {
			const std::string name = result.Find("name") ? result.Find("name")->AsString() : "?";
			const uint32_t value = static_cast<uint32_t>(result.Find("value") ? result.Find("value")->AsInt(0) : 0);
			const uint32_t bits = static_cast<uint32_t>(result.Find("bits") ? result.Find("bits")->AsInt(0) : 0);
			out << name << " = " << HexValue(value, bits) << " (" << bits << " bits)\n";
			return out.str();
		}
		if (command == "read" || command == "read-rom") {
			const uint32_t address = static_cast<uint32_t>(result.Find("address") ? result.Find("address")->AsInt(0) : 0);
			const std::string hex = result.Find("data") ? result.Find("data")->AsString() : std::string();
			return HexDump(DecodeHex(hex), address, command == "read-rom" ? "ROM" : nullptr);
		}
		if (command == "read-code") {
			const uint32_t address = static_cast<uint32_t>(result.Find("address") ? result.Find("address")->AsInt(0) : 0);
			const Json* words = result.Find("words");
			if (!words)
				return "no code words\n";
			for (size_t index = 0; index < words->Items().size(); ++index) {
				char buffer[48];
				std::snprintf(buffer, sizeof(buffer), "0x%04X  %04X\n",
					static_cast<unsigned>(address + index * 2),
					static_cast<unsigned>(words->Items()[index].AsInt(0)));
				out << buffer;
			}
			return out.str();
		}
		if (command == "write" || command == "write-rom" || command == "write-code") {
			out << "wrote " << (result.Find("length") ? result.Find("length")->AsInt(0) : 0) << " byte(s) at ";
			char buffer[24];
			std::snprintf(buffer, sizeof(buffer), "0x%X\n", static_cast<unsigned>(result.Find("address") ? result.Find("address")->AsInt(0) : 0));
			out << buffer;
			return out.str();
		}
		if (command == "disasm") {
			const Json* lines = result.Find("lines");
			if (!lines || lines->Items().empty())
				return "no disassembly (the Code window may not be loaded)\n";
			for (const auto& line : lines->Items()) {
				const uint32_t address = static_cast<uint32_t>(line.Find("address") ? line.Find("address")->AsInt(0) : 0);
				std::string text = line.Find("text") ? line.Find("text")->AsString() : std::string();
				if (line.Find("label") && line.Find("label")->AsBool()) {
					out << "                 " << text << "\n";
					continue;
				}
				// The legacy disassembler prefixes the text with the address.
				char prefix[16];
				std::snprintf(prefix, sizeof(prefix), "%04X", address);
				if (text.size() >= 4 && Lower(text.substr(0, 4)) == Lower(prefix)) {
					size_t offset = 4;
					while (offset < text.size() && text[offset] == ' ')
						++offset;
					text = text.substr(offset);
				}
				char buffer[24];
				std::snprintf(buffer, sizeof(buffer), "0x%04X  ", address);
				out << buffer << text << "\n";
			}
			return out.str();
		}
		if (command == "bp.list") {
			out << "exec ";
			out << (result.Find("exec") ? FormatAddressList(*result.Find("exec")) : "(none)") << "\n";
			const Json* memory = result.Find("mem");
			if (!memory || memory->Items().empty()) {
				out << "mem  (none)\n";
				return out.str();
			}
			out << "ADDRESS   TYPE   HITS  FLAGS\n";
			for (const auto& entry : memory->Items()) {
				char buffer[128];
				std::snprintf(buffer, sizeof(buffer), "0x%-7X %-6s %-5lld %s%s%s\n",
					static_cast<unsigned>(entry.Find("address") ? entry.Find("address")->AsInt(0) : 0),
					entry.Find("write") && entry.Find("write")->AsBool() ? "write" : "read",
					static_cast<long long>(entry.Find("hits") ? entry.Find("hits")->AsInt(0) : 0),
					entry.Find("break_when_hit") && entry.Find("break_when_hit")->AsBool() ? "break " : "record ",
					entry.Find("enabled") && !entry.Find("enabled")->AsBool() ? "disabled " : "",
					entry.Find("compare_data") && entry.Find("compare_data")->AsBool() ? "compare" : "");
				out << buffer;
			}
			return out.str();
		}
		if (command == "bp.set" || command == "bp.clear") {
			out << "exec breakpoints: "
				<< (result.Find("breakpoints") ? FormatAddressList(*result.Find("breakpoints")) : "(none)") << "\n";
			return out.str();
		}
		if (command == "bp.clearall")
			return "all execution breakpoints cleared\n";
		if (command == "bp.mem.set") {
			out << "watching " << (result.Find("length") ? result.Find("length")->AsInt(0) : 0) << " byte(s) at 0x";
			char buffer[24];
			std::snprintf(buffer, sizeof(buffer), "%X", static_cast<unsigned>(result.Find("address") ? result.Find("address")->AsInt(0) : 0));
			out << buffer << " (" << (result.Find("access") ? result.Find("access")->AsString() : "both") << ")\n";
			return out.str();
		}
		if (command == "bp.mem.clear")
			return "memory watch cleared\n";
		if (command == "key.press" || command == "key.release" || command == "key.hold")
			return FormatKey(result) + "\n";
		if (command == "key.releaseall")
			return "all keys released\n";
		if (command == "key.matrix") {
			char buffer[96];
			std::snprintf(buffer, sizeof(buffer), "%s ki=%lld ko=%lld\n",
				result.Find("action") ? result.Find("action")->AsString().c_str() : "press",
				static_cast<long long>(result.Find("ki") ? result.Find("ki")->AsInt(0) : 0),
				static_cast<long long>(result.Find("ko") ? result.Find("ko")->AsInt(0) : 0));
			return buffer;
		}
		if (command == "key.list") {
			const Json* keys = result.Find("keys");
			if (!keys)
				return "no model keys\n";
			for (const auto& key : keys->Items()) {
				char buffer[96];
				std::snprintf(buffer, sizeof(buffer), "%-24s code 0x%02X",
					key.Find("name") ? key.Find("name")->AsString().c_str() : "?",
					static_cast<unsigned>(key.Find("code") ? key.Find("code")->AsInt(0) : 0));
				out << buffer;
				if (key.Find("ki")) {
					out << "  ki=" << key.Find("ki")->AsInt(0);
					if (key.Find("ko"))
						out << " ko=" << key.Find("ko")->AsInt(0);
				}
				out << "\n";
			}
			return out.str();
		}
		if (command == "screenshot") {
			out << "wrote " << (result.Find("width") ? result.Find("width")->AsInt(0) : 0) << "x"
				<< (result.Find("height") ? result.Find("height")->AsInt(0) : 0) << " "
				<< (result.Find("format") ? result.Find("format")->AsString() : "png") << " image to "
				<< (result.Find("path") ? result.Find("path")->AsString() : "?") << "\n";
			return out.str();
		}
		if (command == "inject") {
			out << "injected " << (result.Find("length") ? result.Find("length")->AsInt(0) : 0) << " byte(s) at 0x";
			char buffer[24];
			std::snprintf(buffer, sizeof(buffer), "%X", static_cast<unsigned>(result.Find("address") ? result.Find("address")->AsInt(0) : 0));
			out << buffer << (result.Find("running") && result.Find("running")->AsBool() ? " and resumed\n" : "\n");
			return out.str();
		}
		if (command == "snapshot.list")
			return FormatSnapshotList(result);
		if (command == "snapshot.save") {
			out << "saved snapshot " << (result.Find("id") ? result.Find("id")->AsInt(0) : 0) << " (\""
				<< (result.Find("label") ? result.Find("label")->AsString() : "") << "\")\n";
			return out.str();
		}
		if (command == "snapshot.load")
			return "loaded snapshot " + std::to_string(result.Find("id") ? result.Find("id")->AsInt(0) : 0) + "\n";
		if (command == "snapshot.delete")
			return std::string(result.Find("deleted") && result.Find("deleted")->AsBool() ? "deleted" : "could not delete") +
				" snapshot " + std::to_string(result.Find("id") ? result.Find("id")->AsInt(0) : 0) + "\n";
		if (command == "bt") {
			const Json* frames = result.Find("frames");
			if (!frames || frames->Items().empty()) {
				out << "no stack frames\n";
				return out.str();
			}
			out << "PC        SYMBOL\n";
			for (const auto& frame : frames->Items()) {
				char buffer[64];
				std::snprintf(buffer, sizeof(buffer), "0x%-7X ",
					static_cast<unsigned>(frame.Find("pc") ? frame.Find("pc")->AsInt(0) : 0));
				out << buffer;
				if (frame.Find("symbol") && !frame.Find("symbol")->AsString().empty())
					out << frame.Find("symbol")->AsString();
				else
					out << "-";
				out << "\n";
			}
			return out.str();
		}
		if (command == "labels") {
			const Json* labels = result.Find("labels");
			if (!labels || labels->Items().empty())
				return "no matching labels\n";
			for (const auto& label : labels->Items()) {
				char buffer[32];
				std::snprintf(buffer, sizeof(buffer), "0x%-7X ", static_cast<unsigned>(label.Find("address") ? label.Find("address")->AsInt(0) : 0));
				out << buffer << (label.Find("name") ? label.Find("name")->AsString() : "") << "\n";
			}
			return out.str();
		}
		if (command == "vars") {
			const Json* variables = result.Find("variables");
			if (!variables || variables->Items().empty())
				return "no variables (run the calculator until RAM is initialised)\n";
			for (const auto& variable : variables->Items()) {
				out << (variable.Find("name") ? variable.Find("name")->AsString() : "?") << " = "
					<< (variable.Find("real_value") ? variable.Find("real_value")->AsString() : "?");
				if (variable.Find("has_imaginary") && variable.Find("has_imaginary")->AsBool())
					out << " + " << (variable.Find("imaginary_value") ? variable.Find("imaginary_value")->AsString() : "?") << "i";
				out << "\n";
			}
			return out.str();
		}
		if (command == "hw") {
			auto field = [&](const char* key) -> std::string {
				const Json* value = result.Find(key);
				if (!value)
					return "-";
				if (value->IsString())
					return value->AsString();
				if (value->IsBool())
					return value->AsBool() ? "yes" : "no";
				if (value->IsNumber() && value->GetType() == Json::Type::Double) {
					char buffer[32];
					std::snprintf(buffer, sizeof(buffer), "%.2f", value->AsDouble());
					return buffer;
				}
				return std::to_string(value->AsInt(0));
			};
			out << "hardware   " << field("hardware") << " (id " << field("hardware_id") << ")\n";
			out << "run mode   " << field("run_mode") << "\n";
			out << "paused     " << field("paused") << "\n";
			out << "cycles/s   " << field("cycles_per_second") << "\n";
			out << "battery    " << field("battery_voltage") << " V\n";
			out << "solar      " << field("solar_voltage") << " V\n";
			out << "PD         " << field("pd_value") << "\n";
			return out.str();
		}
		if (command == "bp.hits") {
			const Json* hits = result.Find("hits");
			if (!hits || hits->Items().empty())
				return "no recorded hits\n";
			out << "PC        LR        VALUE  STACK\n";
			for (const auto& hit : hits->Items()) {
				char buffer[96];
				std::snprintf(buffer, sizeof(buffer), "0x%-7X 0x%-7X 0x%02X   %s\n",
					static_cast<unsigned>(hit.Find("pc") ? hit.Find("pc")->AsInt(0) : 0),
					static_cast<unsigned>(hit.Find("lr") ? hit.Find("lr")->AsInt(0) : 0),
					static_cast<unsigned>(hit.Find("value") ? hit.Find("value")->AsInt(0) : 0),
					hit.Find("stack") ? hit.Find("stack")->AsString().c_str() : "");
				out << buffer;
			}
			return out.str();
		}
		if (command == "bp.mem.clearall")
			return "all memory breakpoints cleared\n";
		if (command == "watch.list") {
			const Json* watches = result.Find("watches");
			if (!watches || watches->Items().empty())
				return "no address watches\n";
			out << "ADDRESS   VALUE  LOCKED\n";
			for (const auto& watch : watches->Items()) {
				char buffer[64];
				std::snprintf(buffer, sizeof(buffer), "0x%-7X 0x%02X   %s\n",
					static_cast<unsigned>(watch.Find("address") ? watch.Find("address")->AsInt(0) : 0),
					static_cast<unsigned>(watch.Find("value") ? watch.Find("value")->AsInt(0) : 0),
					watch.Find("locked") && watch.Find("locked")->AsBool() ? "yes" : "no");
				out << buffer;
			}
			return out.str();
		}
		if (command == "watch.set") {
			char buffer[64];
			std::snprintf(buffer, sizeof(buffer), "watch 0x%X = 0x%02X (%s)\n",
				static_cast<unsigned>(result.Find("address") ? result.Find("address")->AsInt(0) : 0),
				static_cast<unsigned>(result.Find("value") ? result.Find("value")->AsInt(0) : 0),
				result.Find("locked") && result.Find("locked")->AsBool() ? "locked" : "unlocked");
			return buffer;
		}
		if (command == "watch.delete") {
			char buffer[96];
			std::snprintf(buffer, sizeof(buffer), "%s watch at 0x%X\n",
				result.Find("removed") && result.Find("removed")->AsBool() ? "removed" : "no",
				static_cast<unsigned>(result.Find("address") ? result.Find("address")->AsInt(0) : 0));
			return buffer;
		}
		if (command == "watch.clearall")
			return "all address watches cleared\n";
		if (command == "call.start") {
			char buffer[128];
			std::snprintf(buffer, sizeof(buffer), "call recording started (caller=%s, callee=%s)\n",
				result.Find("caller") && result.Find("caller")->AsInt(0) ? "filtered" : "any",
				result.Find("callee") && result.Find("callee")->AsInt(0) ? "filtered" : "any");
			return buffer;
		}
		if (command == "call.stop")
			return "call recording stopped\n";
		if (command == "call.clear")
			return "call recording cleared\n";
		if (command == "call.list") {
			const Json* calls = result.Find("calls");
			if (!calls || calls->Items().empty())
				return "no recorded calls\n";
			out << "FUNCTION  CALLER\n";
			for (const auto& call : calls->Items()) {
				char buffer[64];
				std::snprintf(buffer, sizeof(buffer), "0x%-7X 0x%X\n",
					static_cast<unsigned>(call.Find("function") ? call.Find("function")->AsInt(0) : 0),
					static_cast<unsigned>(call.Find("caller") ? call.Find("caller")->AsInt(0) : 0));
				out << buffer;
			}
			return out.str();
		}
		if (command == "snapshot.export")
			return "exported snapshots to " + (result.Find("path") ? result.Find("path")->AsString() : std::string()) + "\n";
		if (command == "snapshot.import")
			return "imported snapshots from " + (result.Find("path") ? result.Find("path")->AsString() : std::string()) + "\n";
		if (command == "hw.cps") {
			char buffer[64];
			std::snprintf(buffer, sizeof(buffer), "cycles/s = %llu\n",
				static_cast<unsigned long long>(result.Find("cycles_per_second") ? result.Find("cycles_per_second")->AsInt(0) : 0));
			return buffer;
		}
		if (command == "hw.pd") {
			char buffer[64];
			std::snprintf(buffer, sizeof(buffer), "PD = 0x%02X\n",
				static_cast<unsigned>(result.Find("pd_value") ? result.Find("pd_value")->AsInt(0) : 0));
			return buffer;
		}
		if (command == "hw.display" || command == "hw.display.set") {
			auto field = [&](const char* key) -> std::string {
				const Json* value = result.Find(key);
				if (!value)
					return std::string("-");
				if (value->IsBool())
					return value->AsBool() ? "on" : "off";
				if (value->GetType() == Json::Type::Double) {
					char buffer[32];
					std::snprintf(buffer, sizeof(buffer), "%.3f", value->AsDouble());
					return buffer;
				}
				return std::to_string(value->AsInt(0));
			};
			out << "flashing_threshold " << field("flashing_threshold") << "\n";
			out << "flashing_brightness " << field("flashing_brightness") << "\n";
			out << "buffer             " << field("buffer") << "\n";
			out << "fading_enabled     " << field("fading_enabled") << "\n";
			out << "fading_coefficient " << field("fading_coefficient") << "\n";
			out << "residual_enabled   " << field("residual_enabled") << "\n";
			out << "residual_alpha     " << field("residual_alpha") << "\n";
			out << "audio_enabled      " << field("audio_enabled") << "\n";
			return out.str();
		}
		if (command == "hw.interrupt") {
			char buffer[96];
			std::snprintf(buffer, sizeof(buffer), "interrupt %u %s\n",
				static_cast<unsigned>(result.Find("index") ? result.Find("index")->AsInt(0) : 0),
				result.Find("raised") && !result.Find("raised")->AsBool(true)
					? "ignored (the TI MultiView core has no maskable interrupt model)"
					: "raised");
			return buffer;
		}
		if (command == "hw.reload-rom") {
			char buffer[96];
			std::snprintf(buffer, sizeof(buffer), "ROM reloaded (%llu bytes)\n",
				static_cast<unsigned long long>(result.Find("rom_size") ? result.Find("rom_size")->AsInt(0) : 0));
			return buffer;
		}
		if (command == "hw.record") {
			char buffer[96];
			std::snprintf(buffer, sizeof(buffer), "recording %s (active: %s)\n",
				result.Find("requested") && result.Find("requested")->AsBool() ? "requested" : "stop requested",
				result.Find("recording_active") && result.Find("recording_active")->AsBool() ? "yes" : "no");
			return buffer;
		}
		if (command == "hw.qr") {
			out << "active   " << (result.Find("active") && result.Find("active")->AsBool() ? "yes" : "no") << "\n";
			out << "complete " << (result.Find("complete") && result.Find("complete")->AsBool() ? "yes" : "no") << "\n";
			out << "version  " << (result.Find("version") ? result.Find("version")->AsInt(0) : 0) << "\n";
			out << "data     " << (result.Find("data") ? result.Find("data")->AsString() : std::string()) << "\n";
			return out.str();
		}
		return result.DumpPretty() + "\n";
	}

	std::string CommandHelp() {
		return
			"Execution:\n"
			"  status | pause | resume | reset\n"
			"  step [n] | step-over | step-out   single-step (into/over/out)\n"
			"Registers and code:\n"
			"  regs                             list registers\n"
			"  reg get <name> | reg set <name> <hex>\n"
			"  disasm [addr] [count]            disassemble (default: PC, 8 lines)\n"
			"  read-code <addr> <words>         read 16-bit code words\n"
			"  read-rom <addr> <len>            read raw ROM bytes\n"
			"  write-rom|write-code <addr> <hex> patch code space/ROM\n"
			"Memory:\n"
			"  read <addr> <len>                memory hex dump\n"
			"  write <addr> <hexbytes>          write memory\n"
			"  inject <addr> <hex> [run]        write a payload, optionally resume\n"
			"Breakpoints and watches:\n"
			"  bp set <addr> | bp clear <addr> | bp clearall | bp list\n"
			"  bp mem <addr> <len> [read|write|both] [--record] [--compare <hex>]\n"
			"                                     [--mask <hex>] [--skip <n>]\n"
			"  bp mem clear <addr> [len] [access]\n"
			"  bp mem clearall\n"
			"  bp hits <addr> [read|write]      recorded memory-breakpoint hits\n"
			"  watch list | watch set <addr> <value> | watch delete <addr> | watch clearall\n"
			"Analysis:\n"
			"  bt                               stack frames and backtrace\n"
			"  labels [query]                   search labels\n"
			"  vars                             decoded calculator variables\n"
			"  call start [caller] [callee] | call stop | call clear | call list [function]\n"
			"Keyboard:\n"
			"  key press|release <name|code>    drive a model key\n"
			"  key hold <name> [ms]             press and release after ms\n"
			"  key releaseall                   release every held key\n"
			"  key matrix <ki> <ko> [press|release]\n"
			"  key list                         list the model's keys\n"
			"Hardware and media:\n"
			"  hw                               hardware state\n"
			"  hw cps <n> | hw pd <hex> | hw interrupt <n>\n"
			"  hw display | hw display set <key> <value>\n"
			"  hw reload-rom                    hot-reload the ROM image\n"
			"  hw record start|stop             screen recording\n"
			"  hw qr                            QR capture state\n"
			"  screenshot <file> [--raw]        save the composited calculator image\n"
			"Snapshots:\n"
			"  snapshot list | snapshot save [label] | snapshot load <id>\n"
			"  snapshot delete <id>\n"
			"  snapshot export <path> [id] [--subtree] | snapshot import <path>\n"
			"Protocol:\n"
			"  hello                            protocol/capability handshake\n"
			"  shutdown                         leave the main loop and exit the emulator\n"
			"  help                             this list\n"
			"  quit | exit                      leave the REPL\n"
			"\n"
			"Addresses and register values are hexadecimal (0x prefix optional);\n"
			"lengths and counts are decimal unless they start with 0x.\n";
	}

} // namespace casioctl
