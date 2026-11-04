#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <string>

namespace casioemu {
struct UpdateInfo {
	std::string tag;
	std::string url;
	std::string title;
	std::string notes;
};

class UpdateChecker {
public:
	UpdateChecker();
	~UpdateChecker();
	void Start();
	bool Ready() const;
	UpdateInfo TakeResult();
	void DismissForToday(const std::string& tag) const;
private:
	struct State { std::mutex mutex; UpdateInfo result; std::atomic<bool> ready{false}; };
	std::shared_ptr<State> state_;
	std::atomic<bool> started_{false};
	// Joined in the destructor so a detached curl/JSON worker can never log
	// through SDL after SDL_Quit.
	std::thread thread_;
};
}
