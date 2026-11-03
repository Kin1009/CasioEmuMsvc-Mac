#pragma once

// Debugger command surface exposed by the in-app control server. Everything in
// here must be called on the UI thread (the thread that owns the emulator and
// the ImGui debugger state).

#include "CtlJson.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace casioemu {
	class Emulator;
}

namespace casioctl {

	struct CommandError : std::runtime_error {
		CommandError(std::string error_code, const std::string& message)
			: std::runtime_error(message), code(std::move(error_code)) {}
		std::string code;
	};

	struct CommandOutcome {
		Json result;
		// Set by "key hold": the server releases this key after the delay so the
		// UI thread is never blocked by a sleep.
		bool schedule_release = false;
		uint8_t release_code = 0;
		int release_delay_ms = 0;
	};

	CommandOutcome ExecuteCommand(casioemu::Emulator& emulator, const std::string& command, const Json& arguments);

	// "step [n]" is deferred by the server: it runs one emulated instruction per
	// UI frame and then completes the request. This validates the count.
	uint32_t ParseStepCount(const Json& arguments);

	Json BuildHello(casioemu::Emulator& emulator);

} // namespace casioctl
