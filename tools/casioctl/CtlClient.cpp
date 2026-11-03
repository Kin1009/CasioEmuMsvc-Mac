#include "CtlClient.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace casioctl {
	namespace {
		void CloseFd(int& fd) {
			if (fd >= 0) {
				::close(fd);
				fd = -1;
			}
		}
	} // namespace

	Client::~Client() {
		Close();
	}

	void Client::Close() {
		CloseFd(fd_);
		buffer_.clear();
	}

	bool Client::Connect(const std::string& socket_path, int timeout_ms, std::string& error) {
		Close();
		sockaddr_un address{};
		address.sun_family = AF_UNIX;
		if (socket_path.empty()) {
			error = "empty socket path";
			return false;
		}
		if (socket_path.size() >= sizeof(address.sun_path)) {
			error = "socket path is too long: " + socket_path;
			return false;
		}
		std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);

		const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0) {
			error = std::string("socket() failed: ") + std::strerror(errno);
			return false;
		}
		const int flags = ::fcntl(fd, F_GETFL, 0);
		::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

		const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
		if (rc != 0) {
			if (errno != EINPROGRESS && errno != EWOULDBLOCK) {
				error = "cannot connect to " + socket_path + ": " + std::strerror(errno);
				::close(fd);
				return false;
			}
			pollfd descriptor{};
			descriptor.fd = fd;
			descriptor.events = POLLOUT;
			const int ready = ::poll(&descriptor, 1, timeout_ms);
			if (ready <= 0) {
				error = "timed out connecting to " + socket_path;
				::close(fd);
				return false;
			}
			int socket_error = 0;
			socklen_t length = sizeof(socket_error);
			if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0 || socket_error != 0) {
				error = "cannot connect to " + socket_path + ": " +
					std::strerror(socket_error ? socket_error : errno);
				::close(fd);
				return false;
			}
		}
		::fcntl(fd, F_SETFL, flags);
#ifdef SO_NOSIGPIPE
		int one = 1;
		::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
		fd_ = fd;
		timeout_ms_ = timeout_ms > 0 ? timeout_ms : 3000;
		buffer_.clear();
		return true;
	}

	bool Client::SendLine(const std::string& line, std::string& error) {
		if (fd_ < 0) {
			error = "not connected";
			return false;
		}
		std::string data = line;
		if (data.empty() || data.back() != '\n')
			data.push_back('\n');
		size_t offset = 0;
		while (offset < data.size()) {
			const ssize_t sent = ::send(fd_, data.data() + offset, data.size() - offset, 0);
			if (sent > 0) {
				offset += static_cast<size_t>(sent);
				continue;
			}
			if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
				pollfd descriptor{};
				descriptor.fd = fd_;
				descriptor.events = POLLOUT;
				const int ready = ::poll(&descriptor, 1, timeout_ms_);
				if (ready <= 0) {
					error = "timed out writing to the control socket";
					return false;
				}
				continue;
			}
			error = std::string("write failed: ") + std::strerror(errno);
			return false;
		}
		return true;
	}

	bool Client::ReadLine(std::string& line, std::string& error) {
		if (fd_ < 0) {
			error = "not connected";
			return false;
		}
		for (;;) {
			const size_t newline = buffer_.find('\n');
			if (newline != std::string::npos) {
				line = buffer_.substr(0, newline);
				buffer_.erase(0, newline + 1);
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				return true;
			}
			pollfd descriptor{};
			descriptor.fd = fd_;
			descriptor.events = POLLIN;
			const int ready = ::poll(&descriptor, 1, timeout_ms_);
			if (ready == 0) {
				error = "timed out waiting for the debugger response";
				return false;
			}
			if (ready < 0) {
				if (errno == EINTR)
					continue;
				error = std::string("poll failed: ") + std::strerror(errno);
				return false;
			}
			char chunk[4096];
			const ssize_t received = ::recv(fd_, chunk, sizeof(chunk), 0);
			if (received == 0) {
				error = "the emulator closed the control socket";
				Close();
				return false;
			}
			if (received < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
					continue;
				error = std::string("read failed: ") + std::strerror(errno);
				return false;
			}
			buffer_.append(chunk, static_cast<size_t>(received));
			if (buffer_.size() > 16 * 1024 * 1024) {
				error = "response is too large";
				return false;
			}
		}
	}

	bool Client::Request(const std::string& command, const Json& arguments, Json& response, std::string& error) {
		Json request = Json::Object();
		const int64_t id = next_id_++;
		request["id"] = id;
		request["cmd"] = command;
		request["args"] = arguments;
		if (!SendLine(request.Dump(), error))
			return false;
		for (int attempt = 0; attempt < 8; ++attempt) {
			std::string line;
			if (!ReadLine(line, error))
				return false;
			Json parsed;
			std::string parse_error;
			if (!Json::Parse(line, parsed, parse_error)) {
				error = "invalid JSON from the debugger: " + parse_error;
				return false;
			}
			if (!parsed.IsObject()) {
				error = "the debugger response is not a JSON object";
				return false;
			}
			const Json* response_id = parsed.Find("id");
			if (response_id && response_id->AsInt(-1) != id)
				continue; // stale reply from an earlier command; keep reading
			response = std::move(parsed);
			return true;
		}
		error = "the debugger response did not match the request id";
		return false;
	}

} // namespace casioctl
