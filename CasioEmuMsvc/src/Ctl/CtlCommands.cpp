#include "CtlCommands.hpp"

#if !defined(_WIN32) && !defined(CASIOEMU_WEB) && !defined(__EMSCRIPTEN__) && !defined(CASIOEMU_CORE_WEB)

#include "CtlSession.hpp"
#include "Config.hpp"
#include "Emulator.hpp"
#include "Chipset/Chipset.hpp"
#include "Models.h"
#include "PluginApi.h"

#include <SDL.h>
#include <SDL_image.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>
#include <unistd.h>

// Owned by Plugin/PluginApi_Impl.cpp (always linked into the desktop app).
extern PluginApi* g_pluginapi;

// Debugger windows owned by the GUI (Gui/Ui.cpp). The plugin debugger interface
// requires them for disassembly, breakpoints and snapshots; they only need to
// be forward declared here.
class CodeViewer;
class SnapshotWindow;
extern CodeViewer* code_viewer;
extern SnapshotWindow* snapshot_window;

namespace casioctl {
	namespace {
		constexpr uint32_t kMaxRead = 1024 * 1024;
		constexpr uint32_t kMaxWrite = 256 * 1024;
		constexpr uint32_t kMaxDisasm = 4096;
		constexpr uint32_t kMaxBreakpointRange = 256;

		PluginApi* Api() {
			return g_pluginapi;
		}

		IDebugger* Debugger() {
			return Api() ? Api()->QueryInterface<IDebugger>() : nullptr;
		}

		IKeyboard* Keyboard() {
			return Api() ? Api()->QueryInterface<IKeyboard>() : nullptr;
		}

		IChipset* ChipsetInterface() {
			return Api() ? Api()->QueryInterface<IChipset>() : nullptr;
		}

		IDebugger& RequireDebugger() {
			IDebugger* debugger = Debugger();
			if (!debugger)
				throw CommandError("debugger-unavailable", "the in-process debugger interface is not available");
			return *debugger;
		}

		IKeyboard& RequireKeyboard() {
			IKeyboard* keyboard = Keyboard();
			if (!keyboard)
				throw CommandError("unsupported", "this model has no keyboard automation interface");
			return *keyboard;
		}

		std::string Lower(std::string value) {
			std::transform(value.begin(), value.end(), value.begin(),
				[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			return value;
		}

		std::string ToHex(const uint8_t* data, size_t size) {
			static const char kDigits[] = "0123456789abcdef";
			std::string out;
			out.resize(size * 2);
			for (size_t i = 0; i < size; ++i) {
				out[i * 2] = kDigits[data[i] >> 4];
				out[i * 2 + 1] = kDigits[data[i] & 0x0f];
			}
			return out;
		}

		bool ParseHexDigits(const std::string& text, uint64_t max_value, uint64_t& value) {
			std::string digits = text;
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

		uint64_t ArgUInt(const Json& arguments, const char* name, bool required, uint64_t fallback, uint64_t max_value) {
			const Json* value = arguments.Find(name);
			if (!value) {
				if (required)
					throw CommandError("bad-args", std::string("missing argument '") + name + "'");
				return fallback;
			}
			uint64_t result = 0;
			if (value->IsNumber()) {
				const int64_t signed_value = value->AsInt(-1);
				if (signed_value < 0 || static_cast<uint64_t>(signed_value) > max_value)
					throw CommandError("out-of-range", std::string("argument '") + name + "' is out of range");
				result = static_cast<uint64_t>(signed_value);
			}
			else if (value->IsString()) {
				if (!ParseHexDigits(value->AsString(), max_value, result))
					throw CommandError("bad-args", std::string("argument '") + name + "' is not a valid hex number");
			}
			else {
				throw CommandError("bad-args", std::string("argument '") + name + "' must be a number or hex string");
			}
			return result;
		}

		std::string ArgString(const Json& arguments, const char* name, const char* fallback) {
			const Json* value = arguments.Find(name);
			if (!value || !value->IsString()) {
				if (fallback)
					return fallback;
				throw CommandError("bad-args", std::string("missing string argument '") + name + "'");
			}
			return value->AsString();
		}

		bool ArgBool(const Json& arguments, const char* name, bool fallback) {
			const Json* value = arguments.Find(name);
			return value ? value->AsBool(fallback) : fallback;
		}

		std::vector<uint8_t> ArgBytes(const Json& arguments, const char* name) {
			const Json* value = arguments.Find(name);
			if (!value)
				throw CommandError("bad-args", std::string("missing argument '") + name + "'");
			std::vector<uint8_t> bytes;
			if (value->IsArray()) {
				bytes.reserve(value->Items().size());
				for (const Json& item : value->Items()) {
					const int64_t byte = item.AsInt(-1);
					if (byte < 0 || byte > 0xff)
						throw CommandError("bad-args", std::string("argument '") + name + "' has a non-byte value");
					bytes.push_back(static_cast<uint8_t>(byte));
				}
			}
			else if (value->IsString()) {
				std::string digits;
				for (char ch : value->AsString()) {
					if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == ':' || ch == ',' || ch == '_')
						continue;
					digits.push_back(ch);
				}
				if (digits.size() % 2 != 0)
					throw CommandError("bad-args", std::string("argument '") + name + "' must contain an even number of hex digits");
				bytes.reserve(digits.size() / 2);
				for (size_t i = 0; i < digits.size(); i += 2) {
					uint64_t byte = 0;
					if (!ParseHexDigits(digits.substr(i, 2), 0xff, byte))
						throw CommandError("bad-args", std::string("argument '") + name + "' contains a non-hex character");
					bytes.push_back(static_cast<uint8_t>(byte));
				}
			}
			else {
				throw CommandError("bad-args", std::string("argument '") + name + "' must be a hex string or byte array");
			}
			return bytes;
		}

		std::string RunModeName(IChipset::RunStatus status) {
			switch (status) {
			case IChipset::RunStatus::Stop:
				return "stop";
			case IChipset::RunStatus::Halt:
				return "halt";
			case IChipset::RunStatus::Run:
				return "run";
			case IChipset::RunStatus::DeepHalt:
				return "deep-halt";
			case IChipset::RunStatus::HaltH:
				return "halt-h";
			}
			return "unknown";
		}

		Json RegisterObject(const DebugRegisterInfo& info) {
			Json object = Json::Object();
			object["name"] = info.Name;
			object["value"] = info.Value;
			object["bits"] = info.BitWidth;
			return object;
		}

		bool ReadNamedRegister(IDebugger& debugger, const char* name, uint32_t& value, uint32_t& bits) {
			uint32_t result_value = 0;
			uint32_t result_bits = 0;
			if (!debugger.ReadRegister(name, result_value, result_bits))
				return false;
			value = result_value;
			bits = result_bits;
			return true;
		}

		void AddRegisterOrNull(Json& target, const char* key, IDebugger& debugger, const char* name) {
			uint32_t value = 0;
			uint32_t bits = 0;
			if (ReadNamedRegister(debugger, name, value, bits))
				target[key] = value;
			else
				target[key] = Json();
		}

		bool IsTiMathPrint(const casioemu::Emulator& emulator) {
			return emulator.hardware_id == casioemu::HW_TI_MATH_PRINT;
		}

		void ComputeKiKo(const casioemu::Emulator& emulator, uint8_t code, int& ki, int& ko) {
			if (IsTiMathPrint(emulator)) {
				static const int kimap[] = {7, 0, 1, 2, 3, 4, 5, 6};
				ki = kimap[code & 7];
				ko = (code >> 3);
				if (ki == 7 && ko > 0)
					--ko;
				return;
			}
			ki = code & 0x0f;
			ko = (code >> 4) & 0x0f;
		}

		struct ResolvedKey {
			uint8_t code = 0;
			std::string name;
			std::string kind;
		};

		ResolvedKey ResolveKey(casioemu::Emulator& emulator, const Json& arguments) {
			ResolvedKey result;
			if (const Json* code_value = arguments.Find("code")) {
				const int64_t code = code_value->AsInt(-1);
				if (code < 0 || code > 0xff)
					throw CommandError("bad-args", "key code must be between 0 and 255");
				result.code = static_cast<uint8_t>(code);
			}
			else {
				std::string token;
				if (const Json* name_value = arguments.Find("name"))
					token = name_value->AsString();
				if (token.empty())
					throw CommandError("bad-args", "missing key name or code");
				if (Lower(token) == "power") {
					result.code = static_cast<uint8_t>(casioemu::BUTTON_KIKO_POWER);
					result.name = "power";
					result.kind = "power";
					return result;
				}
				if (Lower(token) == "reset") {
					result.code = static_cast<uint8_t>(casioemu::BUTTON_KIKO_RESET);
					result.name = "reset";
					result.kind = "reset";
					return result;
				}
				for (const auto& button : emulator.ModelDefinition.buttons) {
					if (Lower(button.keyname) == Lower(token)) {
						result.code = static_cast<uint8_t>(button.kiko);
						result.name = button.keyname;
						break;
					}
				}
				if (result.name.empty()) {
					uint64_t numeric = 0;
					if (ParseHexDigits(token, 0xff, numeric)) {
						result.code = static_cast<uint8_t>(numeric);
						result.name = token;
					}
					else {
						throw CommandError("not-found", "unknown key '" + token + "'");
					}
				}
			}
			for (const auto& button : emulator.ModelDefinition.buttons) {
				if (static_cast<uint8_t>(button.kiko) == result.code) {
					if (result.name.empty())
						result.name = button.keyname;
					break;
				}
			}
			if (result.code == static_cast<uint8_t>(casioemu::BUTTON_KIKO_POWER))
				result.kind = "power";
			else if (result.code == static_cast<uint8_t>(casioemu::BUTTON_KIKO_RESET))
				result.kind = "reset";
			else
				result.kind = "button";
			return result;
		}

		Json KeyToJson(casioemu::Emulator& emulator, const ResolvedKey& key) {
			Json object = Json::Object();
			object["name"] = key.name;
			object["code"] = key.code;
			object["kind"] = key.kind;
			if (key.kind == "button") {
				int ki = 0;
				int ko = 0;
				ComputeKiKo(emulator, key.code, ki, ko);
				object["ki"] = ki;
				object["ko"] = ko;
			}
			return object;
		}

		bool HaveExecutable(const char* name) {
			const char* path = std::getenv("PATH");
			if (!path)
				return false;
			std::string search = path;
			size_t index = 0;
			while (index <= search.size()) {
				const size_t colon = search.find(':', index);
				const std::string directory = search.substr(index,
					colon == std::string::npos ? std::string::npos : colon - index);
				if (!directory.empty()) {
					const std::string candidate = directory + "/" + name;
					if (::access(candidate.c_str(), X_OK) == 0)
						return true;
				}
				if (colon == std::string::npos)
					break;
				index = colon + 1;
			}
			return false;
		}

		bool ScreenshotSupported(casioemu::Emulator& emulator) {
#ifdef SINGLE_WINDOW
			return emulator.renderer != nullptr;
#else
			(void)emulator;
			return false;
#endif
		}

		Json CaptureScreenshot(casioemu::Emulator& emulator, const Json& arguments) {
			const std::string path = ArgString(arguments, "path", nullptr);
			const std::string format = Lower(ArgString(arguments, "format", "png"));
			if (format != "png" && format != "rgba")
				throw CommandError("bad-args", "screenshot format must be 'png' or 'rgba'");
#ifndef SINGLE_WINDOW
			throw CommandError("unsupported", "screenshot requires a single-window build with a composited render target");
#else
			if (!emulator.renderer)
				throw CommandError("unsupported", "the emulator has no SDL renderer");
			SDL_Texture* texture = emulator.composite_texture;
			if (!texture)
				throw CommandError("no-frame", "the calculator has not rendered a frame yet");
			int width = 0;
			int height = 0;
			if (SDL_QueryTexture(texture, nullptr, nullptr, &width, &height) != 0 || width <= 0 || height <= 0)
				throw CommandError("internal", std::string("SDL_QueryTexture failed: ") + SDL_GetError());

			std::vector<uint8_t> bgra(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
			SDL_Texture* previous_target = SDL_GetRenderTarget(emulator.renderer);
			if (SDL_SetRenderTarget(emulator.renderer, texture) != 0)
				throw CommandError("internal", std::string("SDL_SetRenderTarget failed: ") + SDL_GetError());
			const int read_result = SDL_RenderReadPixels(emulator.renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
				bgra.data(), width * 4);
			SDL_SetRenderTarget(emulator.renderer, previous_target);
			if (read_result != 0)
				throw CommandError("internal", std::string("SDL_RenderReadPixels failed: ") + SDL_GetError());

			std::vector<uint8_t> rgba(bgra.size());
			for (size_t i = 0; i < bgra.size(); i += 4) {
				rgba[i + 0] = bgra[i + 2];
				rgba[i + 1] = bgra[i + 1];
				rgba[i + 2] = bgra[i + 0];
				rgba[i + 3] = bgra[i + 3];
			}

			if (format == "rgba") {
				std::ofstream stream(path, std::ios::binary | std::ios::trunc);
				if (!stream)
					throw CommandError("write-failed", "cannot open " + path + ": " + std::strerror(errno));
				stream.write(reinterpret_cast<const char*>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
				if (!stream)
					throw CommandError("write-failed", "cannot write " + path);
			}
			else {
				SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(rgba.data(), width, height, 32, width * 4,
					SDL_PIXELFORMAT_RGBA32);
				if (!surface)
					throw CommandError("internal", std::string("SDL_CreateRGBSurfaceWithFormatFrom failed: ") + SDL_GetError());
				const int saved = IMG_SavePNG(surface, path.c_str());
				SDL_FreeSurface(surface);
				if (saved != 0)
					throw CommandError("write-failed", std::string("IMG_SavePNG failed: ") + IMG_GetError());
			}

			Json result = Json::Object();
			result["path"] = path;
			result["format"] = format;
			result["width"] = width;
			result["height"] = height;
			result["bytes"] = static_cast<uint64_t>(rgba.size());
			return result;
#endif
		}

		Json SnapshotList(IDebugger& debugger) {
			Json list = Json::Array();
			for (const auto& snapshot : debugger.GetSnapshots()) {
				Json entry = Json::Object();
				entry["id"] = snapshot.Id;
				entry["parent"] = snapshot.ParentId;
				entry["label"] = snapshot.Label;
				entry["timestamp"] = static_cast<int64_t>(snapshot.Timestamp);
				entry["preview_bytes"] = static_cast<uint64_t>(snapshot.PreviewSize);
				entry["state_bytes"] = static_cast<uint64_t>(snapshot.StateSize);
				list.Push(std::move(entry));
			}
			Json result = Json::Object();
			result["snapshots"] = std::move(list);
			return result;
		}

		Json BuildCapabilities(casioemu::Emulator& emulator) {
			Json capabilities = Json::Array();
			const char* commands[] = {
				"status", "pause", "resume", "reset", "step", "step.over", "step.out",
				"regs", "reg.get", "reg.set",
				"read", "write", "read-code", "read-rom", "write-rom", "write-code", "disasm",
				"bp.set", "bp.clear", "bp.clearall", "bp.list", "bp.hits",
				"bp.mem.set", "bp.mem.clear", "bp.mem.clearall",
				"key.press", "key.release", "key.hold", "key.releaseall", "key.matrix", "key.list",
				"inject", "screenshot", "snapshot.list", "snapshot.save", "snapshot.load",
				"snapshot.delete", "snapshot.export", "snapshot.import",
				"watch.list", "watch.set", "watch.delete", "watch.clearall",
				"call.start", "call.stop", "call.clear", "call.list",
				"hw", "hw.cps", "hw.pd", "hw.display", "hw.display.set", "hw.interrupt",
				"hw.reload-rom", "hw.record", "hw.qr",
				"bt", "labels", "vars",
			};
			for (const char* command : commands)
				capabilities.Push(std::string(command));
			if (!ScreenshotSupported(emulator)) {
				Json filtered = Json::Array();
				for (const auto& item : capabilities.Items()) {
					if (item.AsString() != "screenshot")
						filtered.Push(item);
				}
				return filtered;
			}
			return capabilities;
		}
	} // namespace

	uint32_t ParseStepCount(const Json& arguments) {
		const uint64_t count = ArgUInt(arguments, "n", false, 1, 10000);
		if (count == 0)
			throw CommandError("bad-args", "step count must be at least 1");
		return static_cast<uint32_t>(count);
	}

	Json BuildHello(casioemu::Emulator& emulator) {
		Json result = Json::Object();
		result["protocol"] = kProtocolName;
		result["protocol_version"] = kProtocolVersion;
		result["app"] = kAppName;
		result["app_version"] = EMULATOR_VERSION;
		result["pid"] = static_cast<int>(::getpid());
		result["model"] = emulator.ModelDefinition.model_name;
		result["model_path"] = emulator.model_path;
		result["rom_path"] = emulator.ModelDefinition.rom_path;
		result["hardware"] = casioemu::HardwareDisplayName(emulator.hardware_id);
		result["hardware_id"] = static_cast<int>(emulator.hardware_id);
		result["real_hardware"] = emulator.ModelDefinition.real_hardware;
		result["offscreen_rendering"] = emulator.offscreen_rendering;
		result["paused"] = emulator.GetPaused();
		result["debugger"] = Debugger() != nullptr;
		result["debugger_ui"] = code_viewer != nullptr;
		result["snapshot_ui"] = snapshot_window != nullptr;
		result["keyboard"] = Keyboard() != nullptr;
		result["screenshot"] = ScreenshotSupported(emulator);
		result["capabilities"] = BuildCapabilities(emulator);
		Json limits = Json::Object();
		limits["read_max"] = kMaxRead;
		limits["write_max"] = kMaxWrite;
		limits["disasm_max"] = kMaxDisasm;
		limits["breakpoint_range_max"] = kMaxBreakpointRange;
		limits["step_max"] = 100000;
		result["limits"] = std::move(limits);
		return result;
	}

	CommandOutcome ExecuteCommand(casioemu::Emulator& emulator, const std::string& command, const Json& arguments) {		CommandOutcome outcome;

		if (command == "hello") {
			outcome.result = BuildHello(emulator);
			return outcome;
		}

		if (command == "shutdown") {
			// Leave through the normal main-loop teardown (the same path as a
			// window close) so shutdown lifetime bugs stay observable in tests.
			emulator.RequestQuit();
			Json result = Json::Object();
			result["quitting"] = true;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "status") {
			IDebugger& debugger = RequireDebugger();
			Json result = Json::Object();
			result["model"] = emulator.ModelDefinition.model_name;
			result["model_path"] = emulator.model_path;
			result["rom_path"] = emulator.ModelDefinition.rom_path;
			result["hardware"] = casioemu::HardwareDisplayName(emulator.hardware_id);
			result["hardware_id"] = static_cast<int>(emulator.hardware_id);
			result["real_hardware"] = emulator.ModelDefinition.real_hardware;
			result["offscreen_rendering"] = emulator.offscreen_rendering;
			result["paused"] = emulator.GetPaused();
			result["pc"] = debugger.GetProgramCounter();
			Json registers = Json::Object();
			AddRegisterOrNull(registers, "sp", debugger, "sp");
			AddRegisterOrNull(registers, "lr", debugger, "lr");
			result["registers"] = std::move(registers);
			result["cycles_per_second"] = emulator.cycles.cycles_per_second;
			result["rom"] = Json::Object();
			result["rom"]["base"] = 0;
			IChipset* chipset = ChipsetInterface();
			result["rom"]["size"] = chipset ? static_cast<uint64_t>(chipset->GetRomSize()) : 0;
			result["ram"] = Json::Object();
			result["ram"]["base"] = static_cast<uint64_t>(casioemu::GetRamBaseAddr(emulator.hardware_id));
			result["ram"]["size"] = static_cast<uint64_t>(casioemu::GetRamSize(emulator.hardware_id));
			result["run_mode"] = chipset ? RunModeName(chipset->GetStatus()) : "unknown";
			result["running"] = emulator.Running();
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "pause") {
			RequireDebugger().Pause();
			Json result = Json::Object();
			result["paused"] = emulator.GetPaused();
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "resume") {
			RequireDebugger().Resume();
			Json result = Json::Object();
			result["paused"] = emulator.GetPaused();
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "reset") {
			RequireDebugger().Reset();
			Json result = Json::Object();
			result["paused"] = emulator.GetPaused();
			result["pc"] = RequireDebugger().GetProgramCounter();
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "step") {
			// The server owns deferred stepping; reaching this point means the
			// request arrived without the server-side handler.
			throw CommandError("internal", "step must be handled by the control server");
		}

		if (command == "regs") {
			Json list = Json::Array();
			for (const auto& info : RequireDebugger().GetRegisters())
				list.Push(RegisterObject(info));
			Json result = Json::Object();
			result["registers"] = std::move(list);
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "reg.get") {
			IDebugger& debugger = RequireDebugger();
			const std::string name = ArgString(arguments, "name", nullptr);
			uint32_t value = 0;
			uint32_t bits = 0;
			if (!debugger.ReadRegister(name.c_str(), value, bits))
				throw CommandError("not-found", "unknown register '" + name + "'");
			Json result = Json::Object();
			result["name"] = name;
			result["value"] = value;
			result["bits"] = bits;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "reg.set") {
			IDebugger& debugger = RequireDebugger();
			const std::string name = ArgString(arguments, "name", nullptr);
			const uint64_t value = ArgUInt(arguments, "value", true, 0, 0xffffffffull);
			if (emulator.chipset.t4x && !emulator.GetPaused())
				throw CommandError("not-paused", "the TI MultiView debugger only accepts register writes while paused");
			if (!debugger.WriteRegister(name.c_str(), static_cast<uint32_t>(value)))
				throw CommandError("not-found", "cannot write register '" + name + "'");
			uint32_t read_value = 0;
			uint32_t bits = 0;
			debugger.ReadRegister(name.c_str(), read_value, bits);
			Json result = Json::Object();
			result["name"] = name;
			result["value"] = read_value;
			result["bits"] = bits;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "read") {
			const uint32_t address = static_cast<uint32_t>(ArgUInt(arguments, "addr", true, 0, 0xffffffffull));
			const uint32_t length = static_cast<uint32_t>(ArgUInt(arguments, "len", true, 0, kMaxRead));
			if (length == 0)
				throw CommandError("out-of-range", "length must be at least 1");
			const std::vector<uint8_t> data = RequireDebugger().ReadMemory(address, length);
			Json result = Json::Object();
			result["address"] = address;
			result["length"] = static_cast<uint64_t>(data.size());
			result["data"] = ToHex(data.data(), data.size());
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "write") {
			const uint32_t address = static_cast<uint32_t>(ArgUInt(arguments, "addr", true, 0, 0xffffffffull));
			const std::vector<uint8_t> data = ArgBytes(arguments, "data");
			if (data.empty())
				throw CommandError("bad-args", "no data to write");
			if (data.size() > kMaxWrite)
				throw CommandError("out-of-range", "write is too large");
			if (emulator.chipset.t4x && !emulator.GetPaused())
				throw CommandError("not-paused", "the TI MultiView debugger only accepts memory writes while paused");
			RequireDebugger().WriteMemory(address, data);
			Json result = Json::Object();
			result["address"] = address;
			result["length"] = static_cast<uint64_t>(data.size());
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "read-rom") {
			const uint32_t address = static_cast<uint32_t>(ArgUInt(arguments, "addr", false, 0, 0xffffffffull));
			const uint32_t length = static_cast<uint32_t>(ArgUInt(arguments, "len", false, kMaxRead, kMaxRead));
			IChipset* chipset = ChipsetInterface();
			if (!chipset)
				throw CommandError("unsupported", "the chipset interface is unavailable");
			const uint8_t* rom = static_cast<const uint8_t*>(chipset->GetRom());
			const size_t rom_size = chipset->GetRomSize();
			if (!rom || address >= rom_size)
				throw CommandError("out-of-range", "ROM address is outside the ROM image");
			const size_t available = rom_size - address;
			const size_t count = std::min<size_t>(length == 0 ? 1 : length, available);
			Json result = Json::Object();
			result["address"] = address;
			result["length"] = static_cast<uint64_t>(count);
			result["rom_size"] = static_cast<uint64_t>(rom_size);
			result["data"] = ToHex(rom + address, count);
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "write-rom" || command == "write-code") {
			const uint32_t address = static_cast<uint32_t>(ArgUInt(arguments, "addr", true, 0, 0xffffffffull));
			const std::vector<uint8_t> data = ArgBytes(arguments, "data");
			if (data.empty())
				throw CommandError("bad-args", "no data to write");
			if (data.size() > kMaxWrite)
				throw CommandError("out-of-range", "write is too large");
			RequireDebugger().WriteCode(address, data);
			Json result = Json::Object();
			result["address"] = address;
			result["length"] = static_cast<uint64_t>(data.size());
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "disasm") {
			IDebugger& debugger = RequireDebugger();
			const uint32_t address = static_cast<uint32_t>(ArgUInt(arguments, "addr", false, debugger.GetProgramCounter(), 0xffffffffull));
			const uint32_t count = static_cast<uint32_t>(ArgUInt(arguments, "count", false, 8, kMaxDisasm));
			Json lines = Json::Array();
			for (const auto& line : debugger.GetDisassembly(address, count == 0 ? 1 : count)) {
				Json entry = Json::Object();
				entry["address"] = line.Address;
				entry["text"] = line.Text;
				entry["label"] = line.IsLabel;
				entry["xref"] = line.CrossReference;
				lines.Push(std::move(entry));
			}
			Json result = Json::Object();
			result["address"] = address;
			result["lines"] = std::move(lines);
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "bp.set") {
			IDebugger& debugger = RequireDebugger();
			const uint32_t address = static_cast<uint32_t>(ArgUInt(arguments, "addr", true, 0, 0xffffffffull));
			// The legacy code viewer maps an arbitrary address to the nearest
			// disassembly row; asking for the middle of a multi-byte instruction
			// would silently move the breakpoint (and can leave a stray one at
			// offset 0). Reject non-boundaries before touching the breakpoint map.
			if (!emulator.chipset.t4x && !emulator.chipset.epscpu) {
				bool found = false;
				uint32_t nearest = 0;
				for (const auto& line : debugger.GetDisassembly(address, 8)) {
					if (line.IsLabel)
						continue;
					if (!found)
						nearest = line.Address;
					if (line.Address == address) {
						found = true;
						break;
					}
				}
				if (!found) {
					char buffer[128];
					std::snprintf(buffer, sizeof(buffer),
						"0x%X is not an instruction boundary (nearest 0x%X); disassemble around it first",
						address, nearest);
					throw CommandError("bad-args", buffer);
				}
			}
			if (!debugger.AddExecutionBreakpoint(address))
				throw CommandError("unsupported", "the debugger rejected the breakpoint (is the disassembler loaded?)");
			Json list = Json::Array();
			for (uint32_t value : debugger.GetExecutionBreakpoints())
				list.Push(value);
			Json result = Json::Object();
			result["address"] = address;
			result["breakpoints"] = std::move(list);
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "bp.clear") {
			IDebugger& debugger = RequireDebugger();
			const uint32_t address = static_cast<uint32_t>(ArgUInt(arguments, "addr", true, 0, 0xffffffffull));
			debugger.RemoveExecutionBreakpoint(address);
			Json list = Json::Array();
			for (uint32_t value : debugger.GetExecutionBreakpoints())
				list.Push(value);
			Json result = Json::Object();
			result["address"] = address;
			result["breakpoints"] = std::move(list);
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "bp.clearall") {
			IDebugger& debugger = RequireDebugger();
			debugger.ClearExecutionBreakpoints();
			Json result = Json::Object();
			result["breakpoints"] = Json::Array();
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "bp.list") {
			IDebugger& debugger = RequireDebugger();
			Json exec = Json::Array();
			for (uint32_t value : debugger.GetExecutionBreakpoints())
				exec.Push(value);
			Json memory = Json::Array();
			for (const auto& breakpoint : debugger.GetMemoryBreakpoints()) {
				Json entry = Json::Object();
				entry["address"] = breakpoint.Address;
				entry["write"] = breakpoint.Write;
				entry["break_when_hit"] = breakpoint.BreakWhenHit;
				entry["hits"] = static_cast<uint64_t>(breakpoint.HitCount);
				entry["enabled"] = breakpoint.Enabled;
				entry["compare_data"] = breakpoint.CompareData;
				entry["data"] = breakpoint.Data;
				entry["mask"] = breakpoint.Mask;
				entry["skip_count"] = static_cast<uint64_t>(breakpoint.SkipCount);
				memory.Push(std::move(entry));
			}
			Json result = Json::Object();
			result["exec"] = std::move(exec);
			result["mem"] = std::move(memory);
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "bp.mem.set") {
			IDebugger& debugger = RequireDebugger();
			const uint32_t address = static_cast<uint32_t>(ArgUInt(arguments, "addr", true, 0, 0xffffffffull));
			const uint32_t length = static_cast<uint32_t>(ArgUInt(arguments, "len", false, 1, kMaxBreakpointRange));
			const std::string access = Lower(ArgString(arguments, "access", "both"));
			const bool break_when_hit = ArgBool(arguments, "break_when_hit", true);
			const bool enabled = ArgBool(arguments, "enabled", true);
			const bool compare_data = ArgBool(arguments, "compare_data", false);
			const uint32_t compare_byte = static_cast<uint32_t>(ArgUInt(arguments, "data", false, 0, 0xff));
			const uint32_t compare_mask = static_cast<uint32_t>(ArgUInt(arguments, "mask", false, 0xff, 0xff));
			const uint64_t skip_count = ArgUInt(arguments, "skip", false, 0, std::numeric_limits<uint64_t>::max());
			const bool watch_reads = access == "both" || access == "read";
			const bool watch_writes = access == "both" || access == "write";
			if (!watch_reads && !watch_writes)
				throw CommandError("bad-args", "access must be 'read', 'write' or 'both'");
			if (length == 0)
				throw CommandError("out-of-range", "length must be at least 1");
			Json added = Json::Array();
			for (uint32_t offset = 0; offset < length; ++offset) {
				const uint32_t target = address + offset;
				if (watch_writes) {
					if (!debugger.AddMemoryBreakpoint(target, true, break_when_hit, enabled,
							compare_data, static_cast<uint8_t>(compare_byte),
							static_cast<uint8_t>(compare_mask), skip_count))
						throw CommandError("unsupported", "the debugger rejected the memory breakpoint");
					added.Push(target);
				}
				if (watch_reads) {
					if (!debugger.AddMemoryBreakpoint(target, false, break_when_hit, enabled,
							compare_data, static_cast<uint8_t>(compare_byte),
							static_cast<uint8_t>(compare_mask), skip_count))
						throw CommandError("unsupported", "the debugger rejected the memory breakpoint");
					added.Push(target);
				}
			}
			Json result = Json::Object();
			result["address"] = address;
			result["length"] = length;
			result["access"] = access;
			result["watched"] = std::move(added);
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "bp.mem.clear") {
			IDebugger& debugger = RequireDebugger();
			const uint32_t address = static_cast<uint32_t>(ArgUInt(arguments, "addr", true, 0, 0xffffffffull));
			const uint32_t length = static_cast<uint32_t>(ArgUInt(arguments, "len", false, 1, kMaxBreakpointRange));
			const std::string access = Lower(ArgString(arguments, "access", "both"));
			const bool clear_reads = access == "both" || access == "read";
			const bool clear_writes = access == "both" || access == "write";
			for (uint32_t offset = 0; offset < length; ++offset) {
				if (clear_writes)
					debugger.RemoveMemoryBreakpoint(address + offset, true);
				if (clear_reads)
					debugger.RemoveMemoryBreakpoint(address + offset, false);
			}
			Json result = Json::Object();
			result["address"] = address;
			result["length"] = length;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "key.press" || command == "key.release") {
			ResolvedKey key = ResolveKey(emulator, arguments);
			RequireKeyboard().PressCode(key.code, command == "key.press");
			outcome.result = KeyToJson(emulator, key);
			outcome.result["pressed"] = command == "key.press";
			return outcome;
		}

		if (command == "key.hold") {
			const ResolvedKey key = ResolveKey(emulator, arguments);
			const uint32_t delay = static_cast<uint32_t>(ArgUInt(arguments, "ms", false, 120, 60000));
			RequireKeyboard().PressCode(key.code, true);
			outcome.result = KeyToJson(emulator, key);
			outcome.result["hold_ms"] = delay;
			outcome.schedule_release = true;
			outcome.release_code = key.code;
			outcome.release_delay_ms = static_cast<int>(delay);
			return outcome;
		}

		if (command == "key.releaseall") {
			RequireKeyboard().ReleaseAll();
			Json result = Json::Object();
			result["released"] = true;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "key.matrix") {
			const int64_t ki = static_cast<int64_t>(ArgUInt(arguments, "ki", true, 0, 7));
			const int64_t ko = static_cast<int64_t>(ArgUInt(arguments, "ko", true, 0, 15));
			const std::string action = Lower(ArgString(arguments, "action", "press"));
			if (action != "press" && action != "release")
				throw CommandError("bad-args", "action must be 'press' or 'release'");
			RequireKeyboard().Key(static_cast<int>(ki), static_cast<int>(ko), action == "press");
			Json result = Json::Object();
			result["ki"] = ki;
			result["ko"] = ko;
			result["action"] = action;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "key.list") {
			Json keys = Json::Array();
			for (const auto& button : emulator.ModelDefinition.buttons) {
				ResolvedKey key;
				key.code = static_cast<uint8_t>(button.kiko);
				key.name = button.keyname;
				key.kind = key.code == static_cast<uint8_t>(casioemu::BUTTON_KIKO_POWER) ? "power"
					: key.code == static_cast<uint8_t>(casioemu::BUTTON_KIKO_RESET) ? "reset"
					: "button";
				keys.Push(KeyToJson(emulator, key));
			}
			Json result = Json::Object();
			result["keys"] = std::move(keys);
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "inject") {
			const uint32_t address = static_cast<uint32_t>(ArgUInt(arguments, "addr", true, 0, 0xffffffffull));
			const std::vector<uint8_t> data = ArgBytes(arguments, "data");
			const bool run = ArgBool(arguments, "run", false);
			if (data.empty())
				throw CommandError("bad-args", "no data to inject");
			if (data.size() > kMaxWrite)
				throw CommandError("out-of-range", "payload is too large");
			IDebugger& debugger = RequireDebugger();
			if (emulator.chipset.t4x && !emulator.GetPaused())
				throw CommandError("not-paused", "the TI MultiView debugger only accepts injection while paused");
			debugger.WriteMemory(address, data);
			if (run)
				debugger.Resume();
			Json result = Json::Object();
			result["address"] = address;
			result["length"] = static_cast<uint64_t>(data.size());
			result["running"] = !emulator.GetPaused();
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "read-code") {
			const uint32_t address = static_cast<uint32_t>(ArgUInt(arguments, "addr", true, 0, 0xffffffffull));
			const uint32_t count = static_cast<uint32_t>(ArgUInt(arguments, "count", false, 1, kMaxDisasm));
			const std::vector<uint16_t> words = RequireDebugger().ReadCode(address, count == 0 ? 1 : count);
			Json values = Json::Array();
			Json hex = Json::Array();
			for (size_t index = 0; index < words.size(); ++index) {
				values.Push(static_cast<uint32_t>(words[index]));
				char buffer[8];
				std::snprintf(buffer, sizeof(buffer), "%04x", words[index]);
				hex.Push(std::string(buffer));
			}
			Json result = Json::Object();
			result["address"] = address;
			result["count"] = static_cast<uint64_t>(words.size());
			result["words"] = std::move(values);
			result["hex"] = std::move(hex);
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "bp.hits") {
			IDebugger& debugger = RequireDebugger();
			const uint32_t address = static_cast<uint32_t>(ArgUInt(arguments, "addr", true, 0, 0xffffffffull));
			const std::string access = Lower(ArgString(arguments, "access", "write"));
			if (access != "read" && access != "write")
				throw CommandError("bad-args", "access must be 'read' or 'write'");
			Json hits = Json::Array();
			for (const auto& hit : debugger.GetMemoryBreakpointHits(address, access == "write")) {
				Json entry = Json::Object();
				entry["pc"] = hit.ProgramCounter;
				entry["lr"] = hit.LinkRegister;
				entry["stack"] = hit.Stack;
				entry["value"] = hit.Value;
				Json registers = Json::Array();
				for (const auto& register_info : hit.Registers)
					registers.Push(RegisterObject(register_info));
				entry["registers"] = std::move(registers);
				hits.Push(std::move(entry));
			}
			Json result = Json::Object();
			result["address"] = address;
			result["access"] = access;
			result["hits"] = std::move(hits);
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "bp.mem.clearall") {
			RequireDebugger().ClearMemoryBreakpoints();
			Json result = Json::Object();
			result["cleared"] = true;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "watch.list") {
			Json watches = Json::Array();
			for (const auto& watch : RequireDebugger().GetAddressLocks()) {
				Json entry = Json::Object();
				entry["address"] = watch.Address;
				entry["value"] = watch.Value;
				entry["locked"] = watch.Locked;
				watches.Push(std::move(entry));
			}
			Json result = Json::Object();
			result["watches"] = std::move(watches);
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "watch.set") {
			IDebugger& debugger = RequireDebugger();
			const uint32_t address = static_cast<uint32_t>(ArgUInt(arguments, "addr", true, 0, 0xffffffffull));
			const uint32_t value = static_cast<uint32_t>(ArgUInt(arguments, "value", true, 0, 0xff));
			const bool locked = ArgBool(arguments, "locked", true);
			debugger.SetAddressLock(address, static_cast<uint8_t>(value), locked);
			Json result = Json::Object();
			result["address"] = address;
			result["value"] = static_cast<uint8_t>(value);
			result["locked"] = locked;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "watch.delete") {
			IDebugger& debugger = RequireDebugger();
			const uint32_t address = static_cast<uint32_t>(ArgUInt(arguments, "addr", true, 0, 0xffffffffull));
			const bool removed = debugger.RemoveAddressLock(address);
			Json result = Json::Object();
			result["address"] = address;
			result["removed"] = removed;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "watch.clearall") {
			RequireDebugger().ClearAddressLocks();
			Json result = Json::Object();
			result["cleared"] = true;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "call.start") {
			IDebugger& debugger = RequireDebugger();
			const uint32_t caller = static_cast<uint32_t>(ArgUInt(arguments, "caller", false, 0, 0xffffffffull));
			const uint32_t callee = static_cast<uint32_t>(ArgUInt(arguments, "callee", false, 0, 0xffffffffull));
			debugger.StartCallRecording(caller != 0, caller, callee != 0, callee);
			Json result = Json::Object();
			result["recording"] = true;
			result["caller"] = caller;
			result["callee"] = callee;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "call.stop") {
			RequireDebugger().StopCallRecording();
			Json result = Json::Object();
			result["recording"] = false;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "call.clear") {
			RequireDebugger().ClearCallRecording();
			Json result = Json::Object();
			result["cleared"] = true;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "call.list") {
			IDebugger& debugger = RequireDebugger();
			const uint32_t function = static_cast<uint32_t>(ArgUInt(arguments, "function", false, 0, 0xffffffffull));
			Json calls = Json::Array();
			for (const auto& call : debugger.GetFunctionCalls(function)) {
				Json entry = Json::Object();
				entry["function"] = call.Function;
				entry["caller"] = call.Caller;
				entry["xr0"] = call.Xr0;
				entry["stack"] = call.Stack;
				calls.Push(std::move(entry));
			}
			Json result = Json::Object();
			result["calls"] = std::move(calls);
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "snapshot.export") {
			IDebugger& debugger = RequireDebugger();
			const std::string path = ArgString(arguments, "path", nullptr);
			const uint32_t id = static_cast<uint32_t>(ArgUInt(arguments, "id", false, 0, 0xffffffffull));
			const bool subtree = ArgBool(arguments, "subtree", false);
			std::string error;
			if (!debugger.ExportSnapshots(path.c_str(), id, subtree, error))
				throw CommandError("write-failed", error.empty() ? "snapshot export failed" : error);
			Json result = Json::Object();
			result["path"] = path;
			result["id"] = id;
			result["subtree"] = subtree;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "snapshot.import") {
			IDebugger& debugger = RequireDebugger();
			const std::string path = ArgString(arguments, "path", nullptr);
			std::string error;
			if (!debugger.ImportSnapshots(path.c_str(), error))
				throw CommandError("not-found", error.empty() ? "snapshot import failed" : error);
			Json result = Json::Object();
			result["path"] = path;
			result["imported"] = true;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "hw.cps") {
			const uint32_t value = static_cast<uint32_t>(ArgUInt(arguments, "value", true, 0, 0xffffffffull));
			RequireDebugger().SetCyclesPerSecond(value);
			Json result = Json::Object();
			result["cycles_per_second"] = emulator.cycles.cycles_per_second;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "hw.pd") {
			const uint32_t value = static_cast<uint32_t>(ArgUInt(arguments, "value", true, 0, 0xff));
			RequireDebugger().SetPdValue(static_cast<uint8_t>(value));
			Json result = Json::Object();
			result["pd_value"] = static_cast<int>(emulator.ModelDefinition.pd_value);
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "hw.display") {
			const DebugDisplaySettings settings = RequireDebugger().GetDisplaySettings();
			Json result = Json::Object();
			result["flashing_threshold"] = settings.FlashingThreshold;
			result["flashing_brightness"] = settings.FlashingBrightness;
			result["buffer"] = settings.BufferSelect;
			result["fading_enabled"] = settings.FadingEnabled;
			result["fading_coefficient"] = settings.FadingCoefficient;
			result["residual_enabled"] = settings.ResidualEnabled;
			result["residual_alpha"] = settings.ResidualAlphaScale;
			result["audio_enabled"] = settings.AudioEnabled;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "hw.display.set") {
			IDebugger& debugger = RequireDebugger();
			DebugDisplaySettings settings = debugger.GetDisplaySettings();
			if (const Json* value = arguments.Find("flashing_threshold"))
				settings.FlashingThreshold = static_cast<int>(value->AsInt(settings.FlashingThreshold));
			if (const Json* value = arguments.Find("flashing_brightness"))
				settings.FlashingBrightness = static_cast<float>(value->AsDouble(settings.FlashingBrightness));
			if (const Json* value = arguments.Find("buffer"))
				settings.BufferSelect = static_cast<int>(value->AsInt(settings.BufferSelect));
			if (const Json* value = arguments.Find("fading_enabled"))
				settings.FadingEnabled = value->AsBool(settings.FadingEnabled);
			if (const Json* value = arguments.Find("fading_coefficient"))
				settings.FadingCoefficient = static_cast<float>(value->AsDouble(settings.FadingCoefficient));
			if (const Json* value = arguments.Find("residual_enabled"))
				settings.ResidualEnabled = value->AsBool(settings.ResidualEnabled);
			if (const Json* value = arguments.Find("residual_alpha"))
				settings.ResidualAlphaScale = static_cast<float>(value->AsDouble(settings.ResidualAlphaScale));
			if (const Json* value = arguments.Find("audio_enabled"))
				settings.AudioEnabled = value->AsBool(settings.AudioEnabled);
			debugger.SetDisplaySettings(settings);
			const DebugDisplaySettings applied = debugger.GetDisplaySettings();
			Json result = Json::Object();
			result["flashing_threshold"] = applied.FlashingThreshold;
			result["flashing_brightness"] = applied.FlashingBrightness;
			result["buffer"] = applied.BufferSelect;
			result["fading_enabled"] = applied.FadingEnabled;
			result["fading_coefficient"] = applied.FadingCoefficient;
			result["residual_enabled"] = applied.ResidualEnabled;
			result["residual_alpha"] = applied.ResidualAlphaScale;
			result["audio_enabled"] = applied.AudioEnabled;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "hw.interrupt") {
			IChipset* chipset = ChipsetInterface();
			if (!chipset)
				throw CommandError("unsupported", "the chipset interface is unavailable");
			const uint32_t index = static_cast<uint32_t>(ArgUInt(arguments, "index", true, 0, 255));
			chipset->RaiseInterrupt(static_cast<int>(index));
			Json result = Json::Object();
			result["index"] = index;
			result["raised"] = emulator.chipset.t4x == nullptr;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "hw.reload-rom") {
			std::string error;
			if (!RequireDebugger().HotReloadRom(error))
				throw CommandError("reload-failed", error.empty() ? "ROM hot reload failed" : error);
			IChipset* chipset = ChipsetInterface();
			Json result = Json::Object();
			result["reloaded"] = true;
			result["rom_size"] = chipset ? static_cast<uint64_t>(chipset->GetRomSize()) : 0;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "hw.record") {
			const bool start = ArgBool(arguments, "start", true);
			// The ScreenRecorder uses an ffmpeg pipe on macOS. When ffmpeg is
			// absent the encoder's popen() still succeeds and the first frame
			// write raises SIGPIPE, killing the whole emulator. Refuse the
			// request instead of crashing; PNG screenshots still work.
			if (start && !HaveExecutable("ffmpeg"))
				throw CommandError("unsupported",
					"screen recording needs ffmpeg on PATH; install ffmpeg or use 'screenshot'");
			RequireDebugger().RequestRecording(start);
			Json result = Json::Object();
			result["requested"] = start;
			result["recording_active"] = emulator.recording_active.load();
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "hw.qr") {
			const DebugQrCodeInfo qr = RequireDebugger().GetQrCode();
			Json result = Json::Object();
			result["active"] = qr.Active;
			result["complete"] = qr.Complete;
			result["version"] = qr.Version;
			result["revision"] = static_cast<uint64_t>(qr.Revision);
			result["data"] = qr.Data;
			Json history = Json::Array();
			for (const auto& entry : qr.History) {
				Json item = Json::Object();
				item["id"] = static_cast<uint64_t>(entry.Id);
				item["version"] = entry.Version;
				item["data"] = entry.Data;
				history.Push(std::move(item));
			}
			result["history"] = std::move(history);
			result["real_current_page"] = qr.RealCurrentPage;
			result["real_total_pages"] = qr.RealTotalPages;
			result["real_current_page_data"] = qr.RealCurrentPageData;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "screenshot") {
			outcome.result = CaptureScreenshot(emulator, arguments);
			return outcome;
		}

		if (command == "snapshot.list") {
			outcome.result = SnapshotList(RequireDebugger());
			return outcome;
		}

		if (command == "snapshot.save") {
			IDebugger& debugger = RequireDebugger();
			const std::string label = ArgString(arguments, "label", "casioctl snapshot");
			const uint32_t parent = static_cast<uint32_t>(ArgUInt(arguments, "parent", false, 0, 0xffffffffull));
			const uint32_t id = debugger.SaveSnapshot(parent, label.c_str());
			if (id == 0)
				throw CommandError("save-failed", "the snapshot manager did not create a snapshot");
			Json result = Json::Object();
			result["id"] = id;
			result["parent"] = parent;
			result["label"] = label;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "snapshot.load") {
			IDebugger& debugger = RequireDebugger();
			const uint32_t id = static_cast<uint32_t>(ArgUInt(arguments, "id", true, 0, 0xffffffffull));
			std::string error;
			if (!debugger.LoadSnapshot(id, error))
				throw CommandError("not-found", error.empty() ? "cannot load snapshot" : error);
			Json result = Json::Object();
			result["id"] = id;
			result["paused"] = emulator.GetPaused();
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "snapshot.delete") {
			IDebugger& debugger = RequireDebugger();
			const uint32_t id = static_cast<uint32_t>(ArgUInt(arguments, "id", true, 0, 0xffffffffull));
			const bool deleted = debugger.DeleteSnapshot(id);
			Json result = Json::Object();
			result["id"] = id;
			result["deleted"] = deleted;
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "bt") {
			IDebugger& debugger = RequireDebugger();
			Json frames = Json::Array();
			for (const auto& frame : debugger.GetStackFrames()) {
				Json entry = Json::Object();
				entry["pc"] = frame.ProgramCounter;
				entry["lr"] = frame.LinkRegister;
				entry["sp"] = frame.StackPointer;
				entry["symbol"] = frame.Symbol;
				entry["lr_pushed"] = frame.LinkRegisterPushed;
				entry["is_jump"] = frame.IsJump;
				frames.Push(std::move(entry));
			}
			Json result = Json::Object();
			result["frames"] = std::move(frames);
			result["text"] = debugger.GetBacktrace();
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "labels") {
			IDebugger& debugger = RequireDebugger();
			const std::string query = ArgString(arguments, "query", "");
			const uint32_t limit = static_cast<uint32_t>(ArgUInt(arguments, "limit", false, 200, 5000));
			Json labels = Json::Array();
			for (const auto& label : debugger.GetLabels(query.c_str(), limit)) {
				Json entry = Json::Object();
				entry["address"] = label.Address;
				entry["name"] = label.Name;
				labels.Push(std::move(entry));
			}
			Json result = Json::Object();
			result["labels"] = std::move(labels);
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "vars") {
			IDebugger& debugger = RequireDebugger();
			Json variables = Json::Array();
			for (const auto& variable : debugger.GetVariables()) {
				Json entry = Json::Object();
				entry["name"] = variable.Name;
				entry["real_address"] = variable.RealAddress;
				entry["real_value"] = variable.RealValue;
				entry["real_hex"] = variable.RealHex;
				entry["has_imaginary"] = variable.HasImaginaryPart;
				if (variable.HasImaginaryPart) {
					entry["imaginary_address"] = variable.ImaginaryAddress;
					entry["imaginary_value"] = variable.ImaginaryValue;
					entry["imaginary_hex"] = variable.ImaginaryHex;
				}
				variables.Push(std::move(entry));
			}
			Json result = Json::Object();
			result["variables"] = std::move(variables);
			outcome.result = std::move(result);
			return outcome;
		}

		if (command == "hw") {
			Json result = Json::Object();
			result["hardware"] = casioemu::HardwareDisplayName(emulator.hardware_id);
			result["hardware_id"] = static_cast<int>(emulator.hardware_id);
			result["real_hardware"] = emulator.ModelDefinition.real_hardware;
			result["offscreen_rendering"] = emulator.offscreen_rendering;
			result["paused"] = emulator.GetPaused();
			result["running"] = emulator.Running();
			result["cycles_per_second"] = emulator.cycles.cycles_per_second;
			result["battery_voltage"] = emulator.BatteryVoltage;
			result["solar_voltage"] = emulator.SolarPanelVoltage;
			result["pd_value"] = static_cast<int>(emulator.ModelDefinition.pd_value);
			if (IChipset* chipset = ChipsetInterface())
				result["run_mode"] = RunModeName(chipset->GetStatus());
			else
				result["run_mode"] = "unknown";
			outcome.result = std::move(result);
			return outcome;
		}

		throw CommandError("unknown-command", "unknown command '" + command + "'");
	}

} // namespace casioctl

#endif // desktop POSIX build
