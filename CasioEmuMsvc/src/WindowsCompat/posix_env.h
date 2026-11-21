#pragma once
// Minimal POSIX environment helpers for MinGW-w64 (no <unistd.h> setenv).
#ifdef _WIN32
# include <cstdlib>
# include <cstring>

// MinGW's CRT provides _putenv_s but not setenv()/unsetenv().
inline int setenv(const char* name, const char* value, int overwrite) {
	if (!name || !*name || !value || std::strchr(name, '='))
		return -1;
	if (!overwrite && std::getenv(name) != nullptr)
		return 0;
	return _putenv_s(name, value);
}

inline int unsetenv(const char* name) {
	if (!name || !*name || std::strchr(name, '='))
		return -1;
	return _putenv_s(name, "");
}
#endif
