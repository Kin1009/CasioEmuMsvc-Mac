// Regression coverage for Screen LCD worker lifetime.
//
// 1. Start and stop the worker repeatedly; every stop must join and leave the
//    instance reusable.
// 2. Leave a worker running when the process exits, while it keeps taking the
//    process-wide settings lease. Destroying that lease during static
//    destruction used to make the worker throw std::system_error from
//    std::recursive_mutex::lock(), terminating with SIGABRT.
//
// The test intentionally calls std::exit() instead of returning from main:
// returning would destroy the local worker first and would not exercise the
// static-destruction path.

#include "Peripheral/OrdinaryLcdHistory.hpp"
#include "Peripheral/ScreenWorkerThread.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

std::atomic<unsigned long long> g_spins{0};

void Spin(const std::atomic_bool& running) {
	unsigned long long spins = 0;
	while (running.load(std::memory_order_acquire)) {
		if constexpr (casioemu::ordinary_lcd_history::kEnabled) {
			auto lease = casioemu::ordinary_lcd_history::UntrackedChange::LockSettings();
			(void)lease;
		}
		++spins;
	}
	g_spins.fetch_add(spins, std::memory_order_relaxed);
}

bool RepeatedStartStop() {
	casioemu::ScreenWorkerThread worker;
	constexpr int kRounds = 200;
	for (int round = 0; round != kRounds; ++round) {
		worker.Start(Spin);
		for (int wait = 0; wait != 1000 && !worker.Running(); ++wait)
			std::this_thread::sleep_for(std::chrono::microseconds(50));
		if (!worker.Running()) {
			std::fprintf(stderr, "screen_thread_lifetime: worker did not start (round %d)\n", round);
			return false;
		}
		worker.Stop();
		if (worker.Running() || worker.Joinable()) {
			std::fprintf(stderr, "screen_thread_lifetime: worker not stopped (round %d)\n", round);
			return false;
		}
	}
	return true;
}

} // namespace

int main() {
	if (!RepeatedStartStop())
		return 1;

	// Exit while the worker is still ticking and taking the settings lease.
	casioemu::ScreenWorkerThread worker;
	worker.Start(Spin);
	if (!worker.Running()) {
		std::fprintf(stderr, "screen_thread_lifetime: final worker did not start\n");
		return 1;
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(5));
	std::printf("screen_thread_lifetime: 200 start/stop rounds; exiting with a live worker\n");
	std::fflush(nullptr);
	std::exit(0);
}
