#include "OrdinaryLcdHistory.hpp"

#include <chrono>

#include <SDL.h>

namespace casioemu::ordinary_lcd_history {

namespace {

// Deliberately immortal. Screen update workers may still be inside
// TemporalTick()/the legacy tick path while other translation units run their
// static destructors at process exit. Destroying this mutex there turned a
// late lock into std::system_error from std::recursive_mutex::lock(), which
// escapes the worker lambda and terminates the process (SIGABRT).
//
// A function-local pointer static is constant-initialized/thread-safe and
// registers no destructor for the heap mutex, so a worker may lock it at any
// point during static destruction. Leaking one mutex per process is the price
// of a teardown-order-independent settings lease.
std::recursive_mutex& SettingsMutex() noexcept {
	static std::recursive_mutex* mutex = new std::recursive_mutex();
	return *mutex;
}

std::atomic_uint64_t untracked_revision{0};
std::atomic_uint64_t untracked_writers{0};

uint64_t SteadyNowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace

std::unique_lock<std::recursive_mutex> UntrackedChange::LockSettings() {
	return std::unique_lock(SettingsMutex());
}

void UntrackedChange::BeginChange() noexcept {
	if constexpr (kEnabled) {
		untracked_writers.fetch_add(1);
		untracked_revision.fetch_add(1);
	}
}

void UntrackedChange::EndChange() noexcept {
	if constexpr (kEnabled) {
		untracked_revision.fetch_add(1);
		untracked_writers.fetch_sub(1);
	}
}

uint64_t UntrackedRevision() noexcept {
	if constexpr (kEnabled)
		return untracked_revision.load();
	return 0;
}

bool UntrackedStableAt(uint64_t revision) noexcept {
	if constexpr (kEnabled)
		return untracked_writers.load() == 0 && untracked_revision.load() == revision;
	return true;
}

TransactionResult History::TryRecord(PrepareFn prepare, WriteFn write, void* context) {
	std::unique_lock<std::recursive_mutex> lock(mutex, std::try_to_lock);
	if (!lock.owns_lock()) {
		incomplete.store(true, std::memory_order_release);
		return TransactionResult::Failed;
	}
	if (Incomplete() || count == ring.size()) {
		incomplete.store(true, std::memory_order_release);
		return TransactionResult::Failed;
	}

	Event event{};
	const PrepareResult prepared = prepare(context, event);
	if (prepared == PrepareResult::Reject) {
		incomplete.store(true, std::memory_order_release);
		return TransactionResult::Rejected;
	}
	if (prepared == PrepareResult::NoChange) {
		// Same-value writes have no extra byte-side effect, but still execute
		// under the transaction lock so they cannot bypass Incomplete handling.
		write(context);
		return TransactionResult::Committed;
	}
	event.epoch = epoch;
	event.seq = ++next_seq;
	// Sampling after taking the mutex makes timestamp order follow commit order.
	event.steady_ns = SteadyNowNs();
	event.sdl_ms = SDL_GetTicks64();
	latest_event_steady_ns.store(event.steady_ns, std::memory_order_release);
	// The original write runs under this short history lock, preserving the
	// event/write order without taking Screen's state mutex.
	write(context);
	ring[(head + count) % ring.size()] = event;
	++count;
	// Wake the LCD worker as soon as the queue goes from empty to non-empty.
	// Notifying on every event would wake it thousands of times per second;
	// the worker coalesces everything that arrived by the time it ticks.
	if (count == 1) {
		pending_since_steady_ns.store(event.steady_ns, std::memory_order_release);
		ready.notify_one();
	}
	return TransactionResult::Committed;
}

void History::WaitForWork(std::chrono::milliseconds interval, const std::atomic_bool& running) {
	auto lock = Lock();
	// Wake on the very first queued event, not only after a large batch: a
	// sparse LCD write during an idle poll must be picked up immediately.
	ready.wait_for(lock, interval, [&] { return !running.load() || count != 0 || Incomplete(); });
}

Cutoff History::CaptureCutoff() const {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	const auto steady_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
	return {epoch, next_seq, steady_ns, SDL_GetTicks64()};
}

bool History::Covers(const Cutoff& cutoff) const {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	return !Incomplete() && cutoff.epoch == epoch && cutoff.end_seq <= next_seq;
}

ConsumeResult History::ConsumeUntil(const Cutoff& cutoff, Batch& batch) {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	batch.size = 0;
	const bool epoch_matches = cutoff.epoch == epoch;
	if (!epoch_matches)
		return {0, 0, epoch, false, false, false, Incomplete()};
	while (batch.size < batch.events.size() && count != 0 &&
		ring[head].epoch == cutoff.epoch && ring[head].seq <= cutoff.end_seq) {
		batch.events[batch.size++] = ring[head];
		head = (head + 1) % ring.size();
		--count;
	}
	const bool queue_epoch_matches = count == 0 || ring[head].epoch == cutoff.epoch;
	const bool cutoff_complete = queue_epoch_matches &&
		(count == 0 || ring[head].seq > cutoff.end_seq);
	return {
		batch.size,
		batch.size == 0 ? 0 : batch.events[batch.size - 1].seq,
		epoch,
		true,
		queue_epoch_matches,
		cutoff_complete,
		Incomplete()};
}

bool History::Incomplete() const {
	return incomplete.load(std::memory_order_acquire) ||
		!UntrackedStableAt(coverage_revision.load(std::memory_order_acquire));
}

bool History::HasPending() const {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	return count != 0 || Incomplete();
}

void History::InvalidateEpoch() {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	incomplete.store(true, std::memory_order_release);
	ready.notify_all();
}

void History::Reset() {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	head = 0;
	count = 0;
	++epoch;
	const auto revision = UntrackedRevision();
	coverage_revision.store(revision, std::memory_order_release);
	incomplete.store(!UntrackedStableAt(revision), std::memory_order_release);
}

} // namespace casioemu::ordinary_lcd_history
