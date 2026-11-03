#include "CtlSession.hpp"

#if !defined(_WIN32)

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace casioctl {
	namespace {
		bool EndsWith(const std::string& value, const std::string& suffix) {
			return value.size() >= suffix.size() &&
				value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
		}

		bool StartsWith(const std::string& value, const std::string& prefix) {
			return value.size() >= prefix.size() &&
				value.compare(0, prefix.size(), prefix) == 0;
		}

		bool ParsePid(const std::string& text, int& pid) {
			if (text.empty())
				return false;
			char* end = nullptr;
			errno = 0;
			const long value = std::strtol(text.c_str(), &end, 10);
			if (errno != 0 || !end || *end != '\0' || value <= 0 || value > 0x7fffffff)
				return false;
			pid = static_cast<int>(value);
			return true;
		}

		bool MakeDirectories(const std::string& path, std::string& error) {
			if (path.empty()) {
				error = "empty control directory";
				return false;
			}
			std::string current;
			size_t index = 0;
			if (path[0] == '/') {
				current = "/";
				index = 1;
			}
			while (index <= path.size()) {
				const size_t slash = path.find('/', index);
				const std::string part = path.substr(index, slash == std::string::npos ? std::string::npos : slash - index);
				if (!part.empty()) {
					if (current.size() > 1 || (current.size() == 1 && current[0] != '/'))
						current += "/";
					current += part;
					if (::mkdir(current.c_str(), 0700) != 0 && errno != EEXIST) {
						error = "mkdir " + current + ": " + std::strerror(errno);
						return false;
					}
				}
				if (slash == std::string::npos)
					break;
				index = slash + 1;
			}
			::chmod(path.c_str(), 0700);
			return true;
		}
	} // namespace

	std::string ControlDirectory() {
		const char* override_dir = std::getenv("CASIOEMU_CTL_DIR");
		if (override_dir && *override_dir)
			return override_dir;
		const char* home = std::getenv("HOME");
		const std::string base = (home && *home) ? std::string(home) : std::string(".");
		return base + "/Library/Application Support/" + kAppName + "/ctl";
	}

	std::string ScratchRoot() {
		const char* override_root = std::getenv("CASIOCTL_TEMP_ROOT");
		if (override_root && *override_root)
			return override_root;
		const char* home = std::getenv("HOME");
		const std::string base = (home && *home) ? std::string(home) : std::string(".");
		return base + "/Desktop/Wiz/temp/casioctl";
	}

	bool EnsureDirectory(const std::string& path, std::string& error) {
		return MakeDirectories(path, error);
	}

	std::string SessionFilePrefix() {
		return "session-";
	}

	std::string SessionSocketPath(int pid) {
		return ControlDirectory() + "/session-" + std::to_string(pid) + ".sock";
	}

	std::string SessionJsonPath(int pid) {
		return ControlDirectory() + "/session-" + std::to_string(pid) + ".json";
	}

	bool EnsureControlDirectory(std::string& error) {
		return MakeDirectories(ControlDirectory(), error);
	}

	Json SessionToJson(const SessionInfo& info) {
		Json document = Json::Object();
		document["pid"] = info.pid;
		document["model"] = info.model;
		document["model_path"] = info.model_path;
		document["rom_path"] = info.rom_path;
		document["hardware"] = info.hardware;
		document["hardware_id"] = info.hardware_id;
		document["socket"] = info.socket_path;
		document["app"] = kAppName;
		document["app_version"] = info.app_version;
		document["protocol"] = kProtocolName;
		document["protocol_version"] = info.protocol_version;
		document["started_at"] = info.started_at;
		return document;
	}

	SessionInfo SessionFromJson(const Json& document) {
		SessionInfo info;
		if (const Json* value = document.Find("pid"))
			info.pid = static_cast<int>(value->AsInt(0));
		if (const Json* value = document.Find("model"))
			info.model = value->AsString();
		if (const Json* value = document.Find("model_path"))
			info.model_path = value->AsString();
		if (const Json* value = document.Find("rom_path"))
			info.rom_path = value->AsString();
		if (const Json* value = document.Find("hardware"))
			info.hardware = value->AsString();
		if (const Json* value = document.Find("hardware_id"))
			info.hardware_id = static_cast<int>(value->AsInt(0));
		if (const Json* value = document.Find("socket"))
			info.socket_path = value->AsString();
		if (const Json* value = document.Find("app_version"))
			info.app_version = value->AsString();
		if (const Json* value = document.Find("started_at"))
			info.started_at = value->AsString();
		if (const Json* value = document.Find("protocol_version"))
			info.protocol_version = static_cast<int>(value->AsInt(0));
		return info;
	}

	bool WriteSessionFile(const SessionInfo& info, std::string& error) {
		if (!EnsureControlDirectory(error))
			return false;
		const std::string path = SessionJsonPath(info.pid);
		const std::string temporary = path + ".tmp." + std::to_string(::getpid());
		{
			std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
			if (!stream) {
				error = "cannot open " + temporary + ": " + std::strerror(errno);
				return false;
			}
			const std::string text = SessionToJson(info).DumpPretty();
			stream.write(text.data(), static_cast<std::streamsize>(text.size()));
			stream.put('\n');
			if (!stream) {
				error = "cannot write " + temporary;
				stream.close();
				::unlink(temporary.c_str());
				return false;
			}
		}
		::chmod(temporary.c_str(), 0600);
		if (::rename(temporary.c_str(), path.c_str()) != 0) {
			error = "cannot rename session file: " + std::string(std::strerror(errno));
			::unlink(temporary.c_str());
			return false;
		}
		return true;
	}

	bool ReadSessionFile(const std::string& path, SessionInfo& info, std::string& error) {
		std::ifstream stream(path, std::ios::binary);
		if (!stream) {
			error = "cannot open " + path;
			return false;
		}
		std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		Json document;
		if (!Json::Parse(text, document, error)) {
			error = path + ": " + error;
			return false;
		}
		info = SessionFromJson(document);
		return true;
	}

	bool RemoveSessionFiles(int pid) {
		bool removed = false;
		if (::unlink(SessionSocketPath(pid).c_str()) == 0)
			removed = true;
		if (::unlink(SessionJsonPath(pid).c_str()) == 0)
			removed = true;
		return removed;
	}

	bool ProcessAlive(int pid) {
		if (pid <= 0)
			return false;
		if (::kill(static_cast<pid_t>(pid), 0) == 0)
			return true;
		return errno == EPERM;
	}

	bool SocketReachable(const std::string& socket_path, int timeout_ms) {
		sockaddr_un address{};
		if (socket_path.empty())
			return false;
		if (socket_path.size() >= sizeof(address.sun_path))
			return false;
		const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0)
			return false;
		const int flags = ::fcntl(fd, F_GETFL, 0);
		::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		address.sun_family = AF_UNIX;
		std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
		const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
		bool reachable = false;
		if (rc == 0) {
			reachable = true;
		}
		else if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
			pollfd descriptor{};
			descriptor.fd = fd;
			descriptor.events = POLLOUT;
			const int ready = ::poll(&descriptor, 1, timeout_ms);
			if (ready > 0) {
				int error_value = 0;
				socklen_t length = sizeof(error_value);
				if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error_value, &length) == 0 && error_value == 0)
					reachable = true;
			}
		}
		::close(fd);
		return reachable;
	}

	std::vector<SessionInfo> ListSessions(size_t* hidden_stale) {
		std::vector<SessionInfo> sessions;
		size_t hidden = 0;
		DIR* directory = ::opendir(ControlDirectory().c_str());
		if (!directory) {
			if (hidden_stale)
				*hidden_stale = 0;
			return sessions;
		}
		const std::string prefix = SessionFilePrefix();
		while (dirent* entry = ::readdir(directory)) {
			const std::string name = entry->d_name;
			if (!StartsWith(name, prefix) || !EndsWith(name, ".json"))
				continue;
			int file_pid = 0;
			const std::string pid_text = name.substr(prefix.size(), name.size() - prefix.size() - 5);
			if (!ParsePid(pid_text, file_pid))
				continue;
			SessionInfo info;
			std::string error;
			const std::string path = ControlDirectory() + "/" + name;
			if (!ReadSessionFile(path, info, error)) {
				++hidden;
				continue;
			}
			if (info.pid <= 0)
				info.pid = file_pid;
			if (info.pid != file_pid) {
				++hidden;
				continue;
			}
			if (info.protocol_version != kProtocolVersion) {
				++hidden;
				continue;
			}
			if (info.socket_path.empty())
				info.socket_path = SessionSocketPath(info.pid);
			struct stat status{};
			const bool alive = ProcessAlive(info.pid);
			const bool socket_ok = ::stat(info.socket_path.c_str(), &status) == 0 && S_ISSOCK(status.st_mode);
			if (!alive || !socket_ok) {
				++hidden;
				continue;
			}
			info.reachable = SocketReachable(info.socket_path);
			if (!info.reachable) {
				++hidden;
				continue;
			}
			sessions.push_back(std::move(info));
		}
		::closedir(directory);
		std::sort(sessions.begin(), sessions.end(), [](const SessionInfo& left, const SessionInfo& right) {
			return left.pid < right.pid;
		});
		if (hidden_stale)
			*hidden_stale = hidden;
		return sessions;
	}

	std::string FormatUtcTimestamp(int64_t unix_seconds) {
		std::time_t value = static_cast<std::time_t>(unix_seconds);
		std::tm broken{};
		if (!::gmtime_r(&value, &broken))
			return std::string();
		char buffer[32];
		if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &broken) == 0)
			return std::string();
		return buffer;
	}

} // namespace casioctl

#endif // !_WIN32
