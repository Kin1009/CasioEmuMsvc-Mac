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

// A source-located compiler error. `line`/`col`/`end_col` are 1-based, or -1/0
// when unknown.  `code` is a stable machine-readable category
// ("syntax.expected-token", "sema.undefined-identifier", ...) and `hint` is an
// actionable suggestion printed after the diagnostic.
struct CompileError : std::runtime_error {
    int line;
    int col;
    int end_col;
    std::string code;
    std::string hint;
    explicit CompileError(const std::string& msg, int line = -1, int col = -1,
                          const std::string& hint = "", const std::string& code = "",
                          int end_col = -1)
        : std::runtime_error(msg),
          line(line),
          col(col),
          end_col(end_col),
          code(code),
          hint(hint) {}
};

// An assembler error.
struct AsmError : CompileError {
    explicit AsmError(const std::string& msg, int line = -1, int col = -1)
        : CompileError(msg, line, col, "", "asm.error") {}
};

// ---------------------------------------------------------------- suggestions
// Levenshtein distance, used for "did you mean ...?" hints.
inline int edit_distance(const std::string& a, const std::string& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = (int)j;
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= b.size(); j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            int v = prev[j - 1] + cost;
            if (prev[j] + 1 < v) v = prev[j] + 1;
            if (cur[j - 1] + 1 < v) v = cur[j - 1] + 1;
            cur[j] = v;
        }
        prev.swap(cur);
    }
    return prev[b.size()];
}

// The closest candidate to `word`, or "" if nothing is close enough.  The
// threshold keeps short names from producing silly suggestions.
inline std::string closest_match(const std::string& word,
                                 const std::vector<std::string>& candidates) {
    if (word.size() < 2) return "";
    std::string best;
    int best_d = 0;
    const int limit = word.size() <= 3 ? 1 : (word.size() <= 6 ? 2 : 3);
    for (const std::string& c : candidates) {
        if (c == word) return "";
        int d = edit_distance(word, c);
        if (d <= limit && (best.empty() || d < best_d)) {
            best = c;
            best_d = d;
        }
    }
    return best;
}

inline std::string did_you_mean(const std::string& word,
                                const std::vector<std::string>& candidates) {
    std::string hit = closest_match(word, candidates);
    return hit.empty() ? std::string() : "did you mean '" + hit + "'?";
}

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
