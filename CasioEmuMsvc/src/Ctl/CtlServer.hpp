#pragma once

namespace casioemu {
	class Emulator;
}

namespace casioemu::ctl {
	// Called at the top of Emulator::Frame() on the UI thread. Lazily starts the
	// control server on the first call and drains queued debugger requests.
	void OnFrame(Emulator& emulator);

	// Called from Emulator::Shutdown() and the destructor. Stops the socket
	// thread and removes session-<pid>.sock / session-<pid>.json.
	void OnShutdown(Emulator& emulator);

} // namespace casioemu::ctl
