#pragma once

#include "CtlJson.hpp"

#include <cstdint>
#include <string>

namespace casioctl {

	// Newline-delimited JSON client for the in-app control socket.
	class Client {
	public:
		Client() = default;
		~Client();
		Client(const Client&) = delete;
		Client& operator=(const Client&) = delete;

		bool Connect(const std::string& socket_path, int timeout_ms, std::string& error);
		void Close();
		bool IsConnected() const { return fd_ >= 0; }

		// Sends {"id":N,"cmd":...,"args":...} and waits for the matching reply.
		bool Request(const std::string& command, const Json& arguments, Json& response, std::string& error);

		// Raw access used by the self-test.
		bool SendLine(const std::string& line, std::string& error);
		bool ReadLine(std::string& line, std::string& error);

		void SetTimeout(int timeout_ms) { timeout_ms_ = timeout_ms; }
		int Timeout() const { return timeout_ms_; }

	private:
		int fd_ = -1;
		int64_t next_id_ = 1;
		int timeout_ms_ = 3000;
		std::string buffer_;
	};

} // namespace casioctl
