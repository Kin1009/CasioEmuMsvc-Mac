#pragma once

#include <atomic>
#include <thread>
#include <utility>

namespace casioemu {

// Owns one worker thread with an explicit stop-and-join contract.
//
// The running flag is set before the thread starts and stays true until the
// owner calls RequestStop(). The worker observes it through the reference it
// receives and must return promptly once it turns false. Join() is idempotent
// and, like the old inline Screen code, refuses to be called from the worker
// itself (that would self-join and deadlock).
//
// This exists so the LCD update thread lifetime can be exercised by a
// standalone regression test (tests/screen_thread_lifetime.cpp) instead of
// only through a full Emulator.
class ScreenWorkerThread {
public:
	ScreenWorkerThread() = default;

	~ScreenWorkerThread() {
		RequestStop();
		Join();
	}

	ScreenWorkerThread(const ScreenWorkerThread&) = delete;
	ScreenWorkerThread& operator=(const ScreenWorkerThread&) = delete;

	bool Running() const noexcept {
		return running_.load(std::memory_order_acquire);
	}

	bool Joinable() const noexcept {
		return thread_.joinable();
	}

	std::thread::id Id() const noexcept {
		return thread_.get_id();
	}

	// Joins any previous worker, then starts `body` with the shared running
	// flag. Reusing an instance after a stop is supported (the regression test
	// starts and stops the worker repeatedly).
	template <typename Body>
	void Start(Body&& body) {
		Join();
		running_.store(true, std::memory_order_release);
		thread_ = std::thread([this, body = std::forward<Body>(body)]() mutable {
			body(static_cast<const std::atomic_bool&>(running_));
		});
	}

	void RequestStop() noexcept {
		running_.store(false, std::memory_order_release);
	}

	void Join() {
		if (!thread_.joinable())
			return;
		if (thread_.get_id() == std::this_thread::get_id())
			std::terminate();
		thread_.join();
	}

	void Stop() {
		RequestStop();
		Join();
	}

private:
	std::atomic_bool running_{false};
	std::thread thread_;
};

} // namespace casioemu
