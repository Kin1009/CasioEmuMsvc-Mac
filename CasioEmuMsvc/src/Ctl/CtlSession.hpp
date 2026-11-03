#pragma once

// Session discovery shared by the in-app control server and the casioctl CLI.
// Only POSIX APIs (dirent/stat/unix sockets) are used.

#include "CtlJson.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace casioctl {

	inline constexpr int kProtocolVersion = 1;
	inline constexpr const char* kProtocolName = "casioctl";
	inline constexpr const char* kAppName = "CasioEmuMsvc";

	// Directory holding session-<pid>.sock and session-<pid>.json.
	// CASIOEMU_CTL_DIR overrides the macOS default (useful for tests and for
	// users who want the control socket on a shorter path).
	std::string ControlDirectory();

	// Scratch root for tests and diagnostic artifacts. This project keeps all
	// scratch data under Wiz/temp; override with CASIOCTL_TEMP_ROOT when the
	// tree lives elsewhere. Never falls back to TMPDIR.
	std::string ScratchRoot();
	bool EnsureDirectory(const std::string& path, std::string& error);

	std::string SessionSocketPath(int pid);
	std::string SessionJsonPath(int pid);
	std::string SessionFilePrefix();

	bool EnsureControlDirectory(std::string& error);

	struct SessionInfo {
		int pid = 0;
		std::string model;
		std::string model_path;
		std::string rom_path;
		std::string hardware;
		int hardware_id = 0;
		std::string socket_path;
		std::string app_version;
		std::string started_at;
		int protocol_version = 0;
		bool reachable = false;
	};

	Json SessionToJson(const SessionInfo& info);
	// Fills only the fields present in the JSON document; pid/socket keep their
	// defaults when absent.
	SessionInfo SessionFromJson(const Json& document);

	// Writes session-<pid>.json atomically with 0600 permissions.
	bool WriteSessionFile(const SessionInfo& info, std::string& error);
	bool ReadSessionFile(const std::string& path, SessionInfo& info, std::string& error);
	bool RemoveSessionFiles(int pid);

	bool ProcessAlive(int pid);
	// Non-blocking connect probe with a short poll timeout.
	bool SocketReachable(const std::string& socket_path, int timeout_ms = 100);

	// Enumerates session-*.json, drops entries whose process is gone, whose
	// socket file is missing/not a socket, or which cannot be reached.
	std::vector<SessionInfo> ListSessions(size_t* hidden_stale = nullptr);

	std::string FormatUtcTimestamp(int64_t unix_seconds);

} // namespace casioctl
