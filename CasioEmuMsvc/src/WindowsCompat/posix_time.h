#pragma once
// POSIX localtime_r() for MinGW-w64, whose CRT exposes localtime_s instead.
#ifdef _WIN32
# include <ctime>
inline std::tm* localtime_r(const std::time_t* time, std::tm* result) {
	return localtime_s(result, time) == 0 ? result : nullptr;
}
#endif
