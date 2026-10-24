#include "UpdateChecker.h"
#include "Config.hpp"
#include "../../../McpPlugin/json.hpp"
#ifndef __ANDROID__
#include <curl/curl.h>
#else
#include <jni.h>
#include <SDL_system.h>
#endif
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <SDL.h>

namespace casioemu {
namespace {
// The macOS fork publishes its own universal builds as vMAJOR.MINOR.PATCH
// releases. Do not point at the upstream repository: its stable-<timestamp>
// auto-builds are not the packages this app installs.
constexpr const char* kApi = "https://api.github.com/repos/Kin1009/CasioEmuMsvc-Mac/releases?per_page=1";
constexpr const char* kReleaseBase = "https://github.com/Kin1009/CasioEmuMsvc-Mac/releases/tag/";
constexpr const char* kCachePath = "update-check.json";
constexpr auto kCacheLifetime = std::chrono::hours(24 * 3);
constexpr std::size_t kMaxResponseSize = 1024 * 1024;
size_t Write(void* p, size_t s, size_t n, void* u) {
	const auto bytes = s * n; auto& body = *static_cast<std::string*>(u);
	if (bytes > kMaxResponseSize || body.size() > kMaxResponseSize - bytes) return 0;
	body.append(static_cast<char*>(p), bytes); return bytes;
}

// GitHub release notes contain typographic punctuation (non-breaking hyphens,
// curly quotes, ...) that the bundled ImGui font subset may not carry. Without
// this, those bytes render as '?' in the middle of words such as "write?-back".
std::string UiText(std::string text) {
	static constexpr std::pair<std::string_view, std::string_view> replacements[] = {
		{"\xE2\x80\x90", "-"}, {"\xE2\x80\x91", "-"}, {"\xE2\x80\x92", "-"},
		{"\xE2\x80\x93", "-"}, {"\xE2\x80\x94", "-"}, {"\xE2\x80\x95", "-"},
		{"\xE2\x80\x98", "'"}, {"\xE2\x80\x99", "'"},
		{"\xE2\x80\x9C", "\""}, {"\xE2\x80\x9D", "\""},
		{"\xE2\x80\xA2", "*"}, {"\xE2\x80\xA6", "..."},
		{"\xC2\xA0", " "}, {"\xE2\x80\xAF", " "}, {"\xC2\xAD", ""},
	};
	for (const auto& [from, to] : replacements) {
		std::string out;
		out.reserve(text.size());
		std::size_t pos = 0;
		while (pos < text.size()) {
			const std::size_t hit = text.find(from, pos);
			if (hit == std::string::npos) {
				out.append(text, pos, std::string::npos);
				break;
			}
			out.append(text, pos, hit - pos);
			out.append(to);
			pos = hit + from.size();
		}
		text = std::move(out);
	}
	return text;
}

bool SetTrustedReleaseUrl(UpdateInfo& release) {
	// Only follow a simple vMAJOR.MINOR.PATCH tag from the fork. This rejects
	// the old upstream stable-<timestamp>-<hash> cache entries as well.
	constexpr std::string_view prefix = "v";
	if (release.tag.size() < prefix.size() + 5 || release.tag.size() > 32
		|| release.tag.compare(0, prefix.size(), prefix) != 0) return false;
	int dots = 0;
	bool digit = false;
	for (std::size_t i = prefix.size(); i < release.tag.size(); ++i) {
		const char ch = release.tag[i];
		if (ch >= '0' && ch <= '9') {
			digit = true;
		} else if (ch == '.') {
			if (!digit) return false;
			++dots;
			digit = false;
		} else {
			return false;
		}
	}
	if (!digit || dots != 2) return false;
	release.url = std::string(kReleaseBase) + release.tag;
	return true;
}

UpdateInfo EvaluateRelease(UpdateInfo release) {
	SDL_Log("[UpdateChecker] Local commit=%s ref=%s remote tag=%s", GIT_COMMIT_HASH, GIT_LATEST_TAG, release.tag.c_str());
	if (release.tag.empty() || release.tag == GIT_LATEST_TAG || release.tag.find(GIT_COMMIT_HASH) != std::string::npos) {
		SDL_Log("[UpdateChecker] No update available");
		return {};
	}
	SDL_Log("[UpdateChecker] Update available: %s", release.tag.c_str());
	return release;
}

std::string LocalDate();

std::optional<UpdateInfo> LoadCachedRelease() {
	std::ifstream input{kCachePath, std::ios::binary};
	if (!input) return std::nullopt;
	try {
		const auto cache = nlohmann::json::parse(input);
		const auto checked_at = cache.value("checked_at", std::int64_t{});
		const auto now = std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		if (checked_at <= 0 || now < checked_at || now - checked_at >= std::chrono::duration_cast<std::chrono::seconds>(kCacheLifetime).count()) {
			SDL_Log("[UpdateChecker] Cached result expired");
			return std::nullopt;
		}
		UpdateInfo release{cache.value("tag", ""), {}, UiText(cache.value("title", "")),
			UiText(cache.value("notes", ""))};
		if (!SetTrustedReleaseUrl(release)) {
			SDL_Log("[UpdateChecker] Ignoring cache with invalid release tag");
			return std::nullopt;
		}
		SDL_Log("[UpdateChecker] Using cached release result: %s", release.tag.c_str());
		if (!release.tag.empty() && cache.value("dismissed_tag", "") == release.tag && cache.value("dismissed_on", "") == LocalDate()) {
			SDL_Log("[UpdateChecker] Update reminder suppressed for today: %s", release.tag.c_str());
			return UpdateInfo{};
		}
		return release;
	}
	catch (const std::exception& e) {
		SDL_Log("[UpdateChecker] Ignoring invalid cache: %s", e.what());
		return std::nullopt;
	}
}

void SaveCachedRelease(const UpdateInfo& release) {
	try {
		const auto checked_at = std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		const nlohmann::json cache{{"checked_at", checked_at}, {"tag", release.tag},
			{"title", release.title}, {"notes", release.notes}};
		std::ofstream output{kCachePath, std::ios::binary | std::ios::trunc};
		if (!output) throw std::runtime_error("cannot open cache file");
		output << cache.dump();
		if (!output) throw std::runtime_error("cannot write cache file");
		SDL_Log("[UpdateChecker] Cached release result for 1 hour");
	}
	catch (const std::exception& e) {
		SDL_Log("[UpdateChecker] Failed to save cache: %s", e.what());
	}
}

std::string LocalDate() {
	const std::time_t now = std::time(nullptr);
	std::tm local{};
#ifdef _WIN32
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif
	char value[11]{};
	std::strftime(value, sizeof(value), "%Y-%m-%d", &local);
	return value;
}

void SaveDismissal(const std::string& tag) {
	try {
		std::ifstream input{kCachePath, std::ios::binary};
		if (!input) throw std::runtime_error("cache file does not exist");
		auto cache = nlohmann::json::parse(input);
		cache["dismissed_tag"] = tag;
		cache["dismissed_on"] = LocalDate();
		std::ofstream output{kCachePath, std::ios::binary | std::ios::trunc};
		if (!output) throw std::runtime_error("cannot open cache file");
		output << cache.dump();
		if (!output) throw std::runtime_error("cannot write cache file");
		SDL_Log("[UpdateChecker] Suppressed %s for today", tag.c_str());
	}
	catch (const std::exception& e) {
		SDL_Log("[UpdateChecker] Failed to save dismissal: %s", e.what());
	}
}

UpdateInfo Check() {
	if (auto cached = LoadCachedRelease()) return EvaluateRelease(std::move(*cached));
	std::string body;
	const char* api = kApi;
	SDL_Log("[UpdateChecker] Checking releases: %s", api);
#ifdef __ANDROID__
	auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
	if (!env) throw std::runtime_error("Android runtime unavailable");
	jobject activity = static_cast<jobject>(SDL_AndroidGetActivity()); jclass cls = env->GetObjectClass(activity);
	jmethodID method = env->GetStaticMethodID(cls, "checkUpdate", "(Ljava/lang/String;)[B");
	if (!method) throw std::runtime_error("Android update API unavailable");
	jstring url = env->NewStringUTF(api); auto bytes = static_cast<jbyteArray>(env->CallStaticObjectMethod(cls, method, url));
	env->DeleteLocalRef(url); if (!bytes) throw std::runtime_error("release request failed");
	const jsize len = env->GetArrayLength(bytes); body.assign(static_cast<size_t>(len), '\0');
	env->GetByteArrayRegion(bytes, 0, len, reinterpret_cast<jbyte*>(body.data())); env->DeleteLocalRef(bytes);
#else
	CURL* c = curl_easy_init(); if (!c) throw std::runtime_error("curl init failed");
	curl_slist* h = nullptr;
	h = curl_slist_append(h, "Accept: application/vnd.github+json");
	h = curl_slist_append(h, "User-Agent: CasioEmuMsvc-update-checker");
	curl_easy_setopt(c, CURLOPT_URL, api); curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L); curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 10000L); curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, Write); curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	const auto code = curl_easy_perform(c); long status = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
	SDL_Log("[UpdateChecker] HTTP result: curl=%d status=%ld bytes=%zu", static_cast<int>(code), status, body.size());
	curl_slist_free_all(h); curl_easy_cleanup(c); if (code != CURLE_OK || status < 200 || status >= 300) throw std::runtime_error("release request failed");

#endif
	const auto releases = nlohmann::json::parse(body); if (!releases.is_array() || releases.empty()) { SDL_Log("[UpdateChecker] No releases returned"); return {}; }
	const auto& json = releases.front();
	const auto string_value = [&json](const char* name) {
		const auto it = json.find(name);
		return it != json.end() && it->is_string() ? it->get<std::string>() : std::string{};
	};
	UpdateInfo release{string_value("tag_name"), {}, UiText(string_value("name")),
		UiText(string_value("body"))};
	if (!SetTrustedReleaseUrl(release)) {
		SDL_Log("[UpdateChecker] Ignoring release with invalid tag: %s", release.tag.c_str());
		return {};
	}
	SaveCachedRelease(release);
	return EvaluateRelease(std::move(release));
}
}
UpdateChecker::UpdateChecker() : state_(std::make_shared<State>()) {}
UpdateChecker::~UpdateChecker() = default;
void UpdateChecker::Start() {
	if (started_.exchange(true)) return;
	auto state = state_;
	std::thread([state]() {
		UpdateInfo result;
		try { result = Check(); }
		catch (const std::exception& e) { SDL_Log("[UpdateChecker] Check failed: %s", e.what()); }
		catch (...) { SDL_Log("[UpdateChecker] Check failed: unknown error"); }
		{ std::lock_guard lock(state->mutex); state->result = std::move(result); }
		state->ready.store(true, std::memory_order_release);
	}).detach();
}
bool UpdateChecker::Ready() const { return state_->ready.load(std::memory_order_acquire); }
UpdateInfo UpdateChecker::TakeResult() { std::lock_guard lock(state_->mutex); return state_->result; }
void UpdateChecker::DismissForToday(const std::string& tag) const { SaveDismissal(tag); }
}
