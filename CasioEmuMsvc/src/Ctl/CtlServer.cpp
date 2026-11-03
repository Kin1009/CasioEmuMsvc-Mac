#include "CtlServer.hpp"

#if defined(__APPLE__) && !defined(CASIOEMU_WEB) && !defined(__EMSCRIPTEN__) && !defined(CASIOEMU_CORE_WEB)

#include "CtlCommands.hpp"
#include "CtlSession.hpp"
#include "Config.hpp"
#include "Emulator.hpp"
#include "Chipset/Chipset.hpp"
// Host build: let PluginApi.h pull in the canonical Ui.hpp definitions so the
// control server can inspect the debugger window vector for offscreen mode.
#define _NO_FUND_API
#include "PluginApi.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <iterator>
#include <map>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern PluginApi* g_pluginapi;

namespace casioemu::ctl {
	using casioctl::Json;
	using casioctl::EnsureControlDirectory;
	using casioctl::FormatUtcTimestamp;
	using casioctl::kProtocolVersion;
	using casioctl::SessionInfo;
	using casioctl::SessionJsonPath;
	using casioctl::SessionSocketPath;
	using casioctl::WriteSessionFile;

	namespace {
		constexpr size_t kMaxLineLength = 4 * 1024 * 1024;
		constexpr int kMaxRequestsPerFrame = 128;
		constexpr int kStepStallFrames = 600; // ~10 s at 60 fps

		struct Client {
			uint64_t id = 0;
			int fd = -1;
			std::string input;
			std::string output;
		};

		struct Inbound {
			uint64_t client = 0;
			std::string line;
		};

		struct HeldKey {
			uint8_t code = 0;
			std::chrono::steady_clock::time_point due;
		};

		enum class StepMode {
			Into,
			Over,
			Out,
		};

		struct PendingStep {
			bool active = false;
			StepMode mode = StepMode::Into;
			uint64_t client = 0;
			Json id;
			uint32_t total = 0;
			uint32_t done = 0;
			int stalled_frames = 0;
		};

		struct ServerState {
			Emulator* owner = nullptr;
			bool disabled = false;
			int listen_fd = -1;
			int wake_read = -1;
			int wake_write = -1;
			std::string socket_path;
			std::string json_path;
			std::thread thread;
			std::atomic<bool> stop{false};

			std::mutex inbound_mutex;
			std::deque<Inbound> inbound;
			std::mutex outbound_mutex;
			std::deque<std::pair<uint64_t, std::string>> outbound;

			// IO-thread only.
			std::map<uint64_t, Client> clients;
			uint64_t next_client_id = 1;

			// UI-thread only.
			bool windows_hidden = false;
			PendingStep pending;
			std::vector<HeldKey> held_keys;
			std::chrono::steady_clock::time_point started;
		};

		ServerState* g_state = nullptr;

		IDebugger* Debugger() {
			return g_pluginapi ? g_pluginapi->QueryInterface<IDebugger>() : nullptr;
		}

		void Log(const std::string& message) {
			std::fprintf(stderr, "[ctl] %s\n", message.c_str());
			std::fflush(stderr);
		}

		void SetNonBlocking(int fd) {
			const int flags = ::fcntl(fd, F_GETFL, 0);
			if (flags >= 0)
				::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		}

		void CloseClient(ServerState& state, std::map<uint64_t, Client>::iterator it) {
			if (it->second.fd >= 0)
				::close(it->second.fd);
			state.clients.erase(it);
		}

		void QueueOutbound(ServerState& state, uint64_t client, std::string line) {
			if (line.empty() || line.back() != '\n')
				line.push_back('\n');
			{
				std::lock_guard<std::mutex> lock(state.outbound_mutex);
				state.outbound.emplace_back(client, std::move(line));
			}
			if (state.wake_write >= 0) {
				const char byte = 'w';
				(void)!::write(state.wake_write, &byte, 1);
			}
		}

		void ReplyOk(ServerState& state, uint64_t client, const Json& id, const Json& result) {
			Json response = Json::Object();
			if (!id.IsNull())
				response["id"] = id;
			response["ok"] = true;
			response["result"] = result;
			QueueOutbound(state, client, response.Dump());
		}

		void ReplyError(ServerState& state, uint64_t client, const Json& id, const std::string& code, const std::string& message) {
			Json response = Json::Object();
			if (!id.IsNull())
				response["id"] = id;
			response["ok"] = false;
			response["error"] = message;
			response["code"] = code;
			QueueOutbound(state, client, response.Dump());
		}

		void ThreadMain(ServerState* state) {
			std::vector<pollfd> descriptors;
			while (!state->stop.load(std::memory_order_relaxed)) {
				{
					std::lock_guard<std::mutex> lock(state->outbound_mutex);
					while (!state->outbound.empty()) {
						auto item = std::move(state->outbound.front());
						state->outbound.pop_front();
						const auto it = state->clients.find(item.first);
						if (it != state->clients.end())
							it->second.output += item.second;
					}
				}

				descriptors.clear();
				pollfd wake_descriptor{};
				wake_descriptor.fd = state->wake_read;
				wake_descriptor.events = POLLIN;
				descriptors.push_back(wake_descriptor);
				pollfd listen_descriptor{};
				listen_descriptor.fd = state->listen_fd;
				listen_descriptor.events = POLLIN;
				descriptors.push_back(listen_descriptor);
				size_t first_client = descriptors.size();
				for (auto& entry : state->clients) {
					pollfd descriptor{};
					descriptor.fd = entry.second.fd;
					descriptor.events = POLLIN;
					if (!entry.second.output.empty())
						descriptor.events |= POLLOUT;
					descriptors.push_back(descriptor);
				}

				const int ready = ::poll(descriptors.data(), descriptors.size(), 100);
				if (state->stop.load(std::memory_order_relaxed))
					break;
				if (ready < 0) {
					if (errno == EINTR)
						continue;
					break;
				}
				if (ready == 0)
					continue;

				if (descriptors[0].revents & POLLIN) {
					char drain[64];
					while (::read(state->wake_read, drain, sizeof(drain)) > 0) {
					}
				}

				if (descriptors[1].revents & POLLIN) {
					for (;;) {
						const int fd = ::accept(state->listen_fd, nullptr, nullptr);
						if (fd < 0)
							break;
						SetNonBlocking(fd);
#ifdef SO_NOSIGPIPE
						int one = 1;
						::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
						Client client;
						client.id = state->next_client_id++;
						client.fd = fd;
						state->clients.emplace(client.id, std::move(client));
					}
				}

				size_t index = first_client;
				for (auto it = state->clients.begin(); it != state->clients.end();) {
					const short revents = index < descriptors.size() ? descriptors[index].revents : 0;
					++index;
					bool keep = !(revents & (POLLERR | POLLNVAL));
					bool peer_closed = (revents & POLLHUP) != 0;
					if (keep && (revents & POLLIN)) {
						char buffer[4096];
						for (;;) {
							const ssize_t received = ::recv(it->second.fd, buffer, sizeof(buffer), 0);
							if (received > 0) {
								it->second.input.append(buffer, static_cast<size_t>(received));
								if (it->second.input.size() > kMaxLineLength) {
									keep = false;
									break;
								}
								continue;
							}
							if (received == 0)
								peer_closed = true;
							else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
								keep = false;
							break;
						}
						size_t newline = 0;
						while (keep && (newline = it->second.input.find('\n')) != std::string::npos) {
							std::string line = it->second.input.substr(0, newline);
							it->second.input.erase(0, newline + 1);
							if (!line.empty() && line.back() == '\r')
								line.pop_back();
							if (line.empty())
								continue;
							std::lock_guard<std::mutex> lock(state->inbound_mutex);
							state->inbound.push_back(Inbound{it->second.id, std::move(line)});
						}
					}
					if (keep && (revents & POLLOUT) && !it->second.output.empty()) {
						const ssize_t sent = ::send(it->second.fd, it->second.output.data(),
							it->second.output.size(), 0);
						if (sent > 0)
							it->second.output.erase(0, static_cast<size_t>(sent));
						else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
							keep = false;
					}
					if (peer_closed)
						keep = false;
					if (!keep) {
						it = [&] {
							auto next = std::next(it);
							CloseClient(*state, it);
							return next;
						}();
					}
					else {
						++it;
					}
				}
			}
		}

		bool StartServer(ServerState& state, Emulator& emulator) {
			std::string error;
			if (!EnsureControlDirectory(error)) {
				Log("cannot create the control directory: " + error);
				return false;
			}
			const int pid = static_cast<int>(::getpid());
			state.socket_path = SessionSocketPath(pid);
			state.json_path = SessionJsonPath(pid);
			::unlink(state.socket_path.c_str());

			sockaddr_un address{};
			address.sun_family = AF_UNIX;
			if (state.socket_path.size() >= sizeof(address.sun_path)) {
				Log("control socket path is too long: " + state.socket_path);
				return false;
			}
			std::memcpy(address.sun_path, state.socket_path.c_str(), state.socket_path.size() + 1);

			const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
			if (fd < 0) {
				Log(std::string("socket() failed: ") + std::strerror(errno));
				return false;
			}
			if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
				Log(std::string("bind() failed: ") + std::strerror(errno));
				::close(fd);
				return false;
			}
			::chmod(state.socket_path.c_str(), 0600);
			if (::listen(fd, 16) != 0) {
				Log(std::string("listen() failed: ") + std::strerror(errno));
				::close(fd);
				::unlink(state.socket_path.c_str());
				return false;
			}
			SetNonBlocking(fd);
			state.listen_fd = fd;

			int pipe_fds[2] = {-1, -1};
			if (::pipe(pipe_fds) != 0) {
				Log(std::string("pipe() failed: ") + std::strerror(errno));
				::close(fd);
				state.listen_fd = -1;
				::unlink(state.socket_path.c_str());
				return false;
			}
			state.wake_read = pipe_fds[0];
			state.wake_write = pipe_fds[1];
			SetNonBlocking(state.wake_read);
			SetNonBlocking(state.wake_write);

			SessionInfo info;
			info.pid = pid;
			info.model = emulator.ModelDefinition.model_name;
			info.model_path = emulator.model_path;
			info.rom_path = emulator.ModelDefinition.rom_path;
			info.hardware = casioemu::HardwareDisplayName(emulator.hardware_id);
			info.hardware_id = static_cast<int>(emulator.hardware_id);
			info.socket_path = state.socket_path;
			info.app_version = EMULATOR_VERSION;
			info.protocol_version = casioctl::kProtocolVersion;
			info.started_at = casioctl::FormatUtcTimestamp(static_cast<int64_t>(std::time(nullptr)));
			if (!WriteSessionFile(info, error)) {
				Log("cannot write the session file: " + error);
				::close(state.wake_read);
				::close(state.wake_write);
				state.wake_read = state.wake_write = -1;
				::close(fd);
				state.listen_fd = -1;
				::unlink(state.socket_path.c_str());
				return false;
			}

			state.started = std::chrono::steady_clock::now();
			state.stop.store(false, std::memory_order_relaxed);
			state.thread = std::thread(ThreadMain, &state);
			Log("listening on " + state.socket_path + " (" + info.model + ")");
			return true;
		}

		void StopServer(ServerState& state) {
			state.stop.store(true, std::memory_order_relaxed);
			if (state.wake_write >= 0) {
				const char byte = 'q';
				(void)!::write(state.wake_write, &byte, 1);
			}
			if (state.thread.joinable())
				state.thread.join();
			for (auto& entry : state.clients) {
				if (entry.second.fd >= 0)
					::close(entry.second.fd);
			}
			state.clients.clear();
			if (state.listen_fd >= 0) {
				::close(state.listen_fd);
				state.listen_fd = -1;
			}
			if (state.wake_read >= 0) {
				::close(state.wake_read);
				state.wake_read = -1;
			}
			if (state.wake_write >= 0) {
				::close(state.wake_write);
				state.wake_write = -1;
			}
			if (!state.socket_path.empty())
				::unlink(state.socket_path.c_str());
			if (!state.json_path.empty())
				::unlink(state.json_path.c_str());
			state.socket_path.clear();
			state.json_path.clear();
			{
				std::lock_guard<std::mutex> lock(state.inbound_mutex);
				state.inbound.clear();
			}
			{
				std::lock_guard<std::mutex> lock(state.outbound_mutex);
				state.outbound.clear();
			}
			state.held_keys.clear();
			state.windows_hidden = false;
			state.pending = PendingStep{};
		}

		void ResetSteps(ServerState& state) {
			state.pending = PendingStep{};
		}

		void ReplyStep(ServerState& state, Emulator& emulator, Json id) {
			Json result = Json::Object();
			result["steps"] = state.pending.done;
			result["pc"] = Debugger() ? Debugger()->GetProgramCounter() : 0;
			result["paused"] = emulator.GetPaused();
			result["mode"] = state.pending.mode == StepMode::Into ? "into"
				: state.pending.mode == StepMode::Over ? "over"
				: "out";
			ReplyOk(state, state.pending.client, id, result);
			ResetSteps(state);
		}

		void StartStepping(ServerState& state, Emulator& emulator, uint64_t client, const Json& id,
			const Json& arguments, StepMode mode) {
			if (state.pending.active) {
				ReplyError(state, client, id, "busy", "a step request is already in progress");
				return;
			}
			uint32_t count = 1;
			try {
				if (mode == StepMode::Into)
					count = casioctl::ParseStepCount(arguments);
			}
			catch (const casioctl::CommandError& error) {
				ReplyError(state, client, id, error.code, error.what());
				return;
			}
			IDebugger* debugger = Debugger();
			if (!debugger) {
				ReplyError(state, client, id, "debugger-unavailable", "the in-process debugger interface is not available");
				return;
			}
			if (!emulator.GetPaused())
				debugger->Pause();
			if (mode == StepMode::Into && emulator.chipset.t4x) {
				// The TI MultiView core steps synchronously.
				for (uint32_t index = 0; index < count; ++index) {
					if (!debugger->StepInto()) {
						ReplyError(state, client, id, "step-failed", "the TI MultiView core refused to step");
						return;
					}
				}
				Json result = Json::Object();
				result["steps"] = count;
				result["pc"] = debugger->GetProgramCounter();
				result["paused"] = emulator.GetPaused();
				result["mode"] = "into";
				ReplyOk(state, client, id, result);
				return;
			}

			state.pending.active = true;
			state.pending.mode = mode;
			state.pending.client = client;
			state.pending.id = id;
			state.pending.total = count;
			state.pending.done = 0;
			state.pending.stalled_frames = 0;
			bool issued = false;
			if (mode == StepMode::Into)
				issued = debugger->StepInto();
			else if (mode == StepMode::Over)
				issued = debugger->StepOver();
			else
				issued = debugger->StepOut();
			if (!issued) {
				ResetSteps(state);
				ReplyError(state, client, id, "step-failed",
					mode == StepMode::Into ? "the debugger refused to step"
					: mode == StepMode::Over ? "the debugger refused to step over"
					: "the debugger refused to step out");
				return;
			}
			if (mode == StepMode::Into) {
				// Match CodeViewer::RequestStep: the step completes asynchronously
				// and the emulator must run until the instruction hook pauses it.
				emulator.SetPaused(false);
			}
			else {
				// StepOver/StepOut request the run themselves; ensure the emulator
				// is not left paused if a CodeViewer is not driving the request.
				emulator.SetPaused(false);
			}
		}

		void ServicePendingStep(ServerState& state, Emulator& emulator) {
			if (!state.pending.active)
				return;
			IDebugger* debugger = Debugger();
			if (!debugger) {
				ReplyError(state, state.pending.client, state.pending.id, "debugger-unavailable",
					"the in-process debugger interface disappeared");
				ResetSteps(state);
				return;
			}
			if (!emulator.GetPaused()) {
				if (++state.pending.stalled_frames > kStepStallFrames) {
					debugger->Pause();
					ReplyError(state, state.pending.client, state.pending.id, "step-timeout",
						"the emulator did not pause after the requested step");
					ResetSteps(state);
				}
				return;
			}
			++state.pending.done;
			state.pending.stalled_frames = 0;
			if (state.pending.mode != StepMode::Into || state.pending.done >= state.pending.total) {
				Json id = state.pending.id;
				ReplyStep(state, emulator, std::move(id));
				return;
			}
			if (!debugger->StepInto()) {
				ReplyError(state, state.pending.client, state.pending.id, "step-failed", "the debugger refused to step");
				ResetSteps(state);
				return;
			}
			emulator.SetPaused(false);
		}

		void ServiceHeldKeys(ServerState& state) {
			if (state.held_keys.empty())
				return;
			const auto now = std::chrono::steady_clock::now();
			IKeyboard* keyboard = g_pluginapi ? g_pluginapi->QueryInterface<IKeyboard>() : nullptr;
			std::vector<HeldKey> remaining;
			remaining.reserve(state.held_keys.size());
			for (const auto& held : state.held_keys) {
				if (held.due <= now) {
					if (keyboard)
						keyboard->PressCode(held.code, false);
				}
				else {
					remaining.push_back(held);
				}
			}
			state.held_keys.swap(remaining);
		}

		void HandleLine(ServerState& state, Emulator& emulator, const Inbound& inbound) {
			Json request;
			std::string error;
			if (!casioctl::Json::Parse(inbound.line, request, error)) {
				ReplyError(state, inbound.client, Json(), "bad-request", "invalid JSON: " + error);
				return;
			}
			if (!request.IsObject()) {
				ReplyError(state, inbound.client, Json(), "bad-request", "request must be a JSON object");
				return;
			}
			const Json* id = request.Find("id");
			const Json id_value = id ? *id : Json();
			const Json* command = request.Find("cmd");
			if (!command || !command->IsString()) {
				ReplyError(state, inbound.client, id_value, "bad-request", "request is missing a string 'cmd'");
				return;
			}
			Json arguments = Json::Object();
			if (const Json* args = request.Find("args")) {
				if (!args->IsObject()) {
					ReplyError(state, inbound.client, id_value, "bad-request", "'args' must be an object");
					return;
				}
				arguments = *args;
			}
			const std::string name = command->AsString();
			if (name == "step") {
				StartStepping(state, emulator, inbound.client, id_value, arguments, StepMode::Into);
				return;
			}
			if (name == "step.over") {
				StartStepping(state, emulator, inbound.client, id_value, arguments, StepMode::Over);
				return;
			}
			if (name == "step.out") {
				StartStepping(state, emulator, inbound.client, id_value, arguments, StepMode::Out);
				return;
			}
			try {
				casioctl::CommandOutcome outcome = casioctl::ExecuteCommand(emulator, name, arguments);
				if (outcome.schedule_release) {
					HeldKey held;
					held.code = outcome.release_code;
					held.due = std::chrono::steady_clock::now() +
						std::chrono::milliseconds(outcome.release_delay_ms);
					state.held_keys.push_back(held);
				}
				ReplyOk(state, inbound.client, id_value, outcome.result);
			}
			catch (const casioctl::CommandError& command_error) {
				ReplyError(state, inbound.client, id_value, command_error.code, command_error.what());
			}
			catch (const std::exception& exception) {
				ReplyError(state, inbound.client, id_value, "internal", exception.what());
			}
		}

		void DrainInbound(ServerState& state, Emulator& emulator) {
			for (int budget = 0; budget < kMaxRequestsPerFrame; ++budget) {
				Inbound inbound;
				{
					std::lock_guard<std::mutex> lock(state.inbound_mutex);
					if (state.inbound.empty())
						break;
					inbound = std::move(state.inbound.front());
					state.inbound.pop_front();
				}
				HandleLine(state, emulator, inbound);
			}
		}
	} // namespace

	void OnFrame(Emulator& emulator) {
		if (!g_state) {
			g_state = new ServerState();
			g_state->owner = &emulator;
			if (!StartServer(*g_state, emulator))
				g_state->disabled = true;
		}
		else if (g_state->owner != &emulator) {
			// The previous Emulator went away without a destructor hook (e.g. a
			// reopened startup selection). Start a fresh session for this one.
			StopServer(*g_state);
			g_state->owner = &emulator;
			g_state->disabled = false;
			if (!StartServer(*g_state, emulator))
				g_state->disabled = true;
		}
		if (!g_state || g_state->disabled)
			return;
		if (emulator.offscreen_rendering && !g_state->windows_hidden) {
			// No display is available. Keep the debugger window objects alive
			// (IDebugger needs them) but stop ImGui from rendering them every
			// frame; the software renderer is otherwise many times slower than
			// the emulator itself.
			for (auto* window : windows) {
				if (window)
					window->open = false;
			}
			g_state->windows_hidden = true;
		}
		ServicePendingStep(*g_state, emulator);
		ServiceHeldKeys(*g_state);
		DrainInbound(*g_state, emulator);
	}

	void OnShutdown(Emulator& emulator) {
		if (!g_state)
			return;
		if (g_state->owner != &emulator)
			return;
		StopServer(*g_state);
		delete g_state;
		g_state = nullptr;
	}

} // namespace casioemu::ctl

#else

namespace casioemu::ctl {
	void OnFrame(Emulator&) {}
	void OnShutdown(Emulator&) {}
} // namespace casioemu::ctl

#endif
