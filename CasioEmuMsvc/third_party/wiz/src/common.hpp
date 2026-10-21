// common.hpp - small shared helpers and the compiler-error type.
#pragma once

#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace wiz {

// A source-located compiler error. `line`/`col` are 1-based, or -1 when unknown.
struct CompileError : std::runtime_error {
    int line;
    int col;
    explicit CompileError(const std::string& msg, int line = -1, int col = -1)
        : std::runtime_error(msg), line(line), col(col) {}
};

// An assembler error.
struct AsmError : CompileError {
    explicit AsmError(const std::string& msg, int line = -1, int col = -1)
        : CompileError(msg, line, col) {}
};

// printf-style formatting into a std::string.
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
inline std::string fmt(const char* f, ...) {
    va_list a1, a2;
    va_start(a1, f);
    va_copy(a2, a1);
    int n = std::vsnprintf(nullptr, 0, f, a1);
    va_end(a1);
    std::string s(n < 0 ? 0 : (size_t)n, '\0');
    if (n > 0) std::vsnprintf(&s[0], (size_t)n + 1, f, a2);
    va_end(a2);
    return s;
}

inline bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && std::memcmp(s.data(), p.data(), p.size()) == 0;
}

inline bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && std::memcmp(s.data() + s.size() - p.size(), p.data(), p.size()) == 0;
}

inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

inline std::string lstrip(const std::string& s, const char* extra = "") {
    size_t i = 0;
    while (i < s.size() && (is_space(s[i]) || std::strchr(extra, s[i]))) i++;
    return s.substr(i);
}

inline std::string rstrip(const std::string& s, const char* extra = "") {
    size_t i = s.size();
    while (i > 0 && (is_space(s[i - 1]) || std::strchr(extra, s[i - 1]))) i--;
    return s.substr(0, i);
}

inline std::string strip(const std::string& s, const char* extra = "") { return rstrip(lstrip(s, extra), extra); }

inline std::string to_lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

inline std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (true) {
        size_t n = s.find(sep, pos);
        if (n == std::string::npos) { out.push_back(s.substr(pos)); break; }
        out.push_back(s.substr(pos, n - pos));
        pos = n + 1;
    }
    return out;
}

inline std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); i++) { if (i) out += sep; out += v[i]; }
    return out;
}

inline bool contains(const std::vector<std::string>& v, const std::string& x) {
    for (const auto& e : v) if (e == x) return true;
    return false;
}

inline std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

// --------------------------------------------------------------- file helpers
// All of these return false / an empty result on I/O failure; they never throw.
std::string read_text_file(const std::string& path, bool* ok = nullptr);
std::vector<uint8_t> read_binary_file(const std::string& path, bool* ok = nullptr);
bool write_text_file(const std::string& path, const std::string& data);
bool write_binary_file(const std::string& path, const std::vector<uint8_t>& data);

}  // namespace wiz
