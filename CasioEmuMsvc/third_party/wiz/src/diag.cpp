// diag.cpp - implementation of the shared diagnostics.
#include "diag.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>

namespace wiz {

const char* g_argv0 = nullptr;

static ColorMode g_mode = [] {
    if (std::getenv("NO_COLOR") != nullptr) return ColorMode::Never;
    if (const char* t = std::getenv("TERM"); t && std::strcmp(t, "dumb") == 0) return ColorMode::Never;
    return ColorMode::Auto;
}();

void set_color_mode(ColorMode m) { g_mode = m; }
ColorMode color_mode() { return g_mode; }

void set_color_mode_from_arg(const std::string& value) {
    if (value == "auto") g_mode = ColorMode::Auto;
    else if (value == "always") g_mode = ColorMode::Always;
    else if (value == "never") g_mode = ColorMode::Never;
    else throw std::runtime_error("invalid colour mode '" + value + "'");
}

bool stderr_is_tty() { return ::isatty(fileno(stderr)) != 0; }
bool stdout_is_tty() { return ::isatty(fileno(stdout)) != 0; }

static bool enabled(bool tty) {
    switch (g_mode) {
        case ColorMode::Always: return true;
        case ColorMode::Never: return false;
        case ColorMode::Auto:
        default:
            const char* t = std::getenv("TERM");
            return tty && (t == nullptr || std::strcmp(t, "dumb") != 0) &&
                   std::getenv("NO_COLOR") == nullptr;
    }
}

std::string paint(const std::string& text, const char* st, bool tty) {
    if (!enabled(tty)) return text;
    return std::string("\033[") + st + "m" + text + "\033[" + style::RESET + "m";
}

std::string bold(const std::string& t, bool tty) { return paint(t, style::BOLD, tty); }
std::string red(const std::string& t, bool tty) { return paint(t, style::ERROR, tty); }
std::string green(const std::string& t, bool tty) { return paint(t, style::OK, tty); }
std::string yellow(const std::string& t, bool tty) { return paint(t, style::WARNING, tty); }
std::string cyan(const std::string& t, bool tty) { return paint(t, style::INFO, tty); }
std::string dim(const std::string& t, bool tty) { return paint(t, style::DIM_TXT, tty); }

std::string source_line_at(const std::string& text, int line) {
    if (text.empty() || line < 1) return "";
    auto lines = split(text, '\n');
    if (line <= (int)lines.size()) return lines[line - 1];
    return "";
}

// ------------------------------------------------------------------ Diag
static std::string loc_string(const std::string& file, int line, int col) {
    if (file.empty()) return "";
    std::string s = file;
    if (line >= 1) {
        s += ":" + std::to_string(line);
        if (col >= 1) s += ":" + std::to_string(col);
    }
    return s;
}

void Diag::error(const std::string& msg, const std::string& file, int line, int col,
                 const std::string& src, const std::string& code) {
    errors++;
    const bool tty = stderr_is_tty();
    std::string loc = loc_string(file, line, col);
    std::string head = loc.empty() ? bold(tool, tty) : bold(loc, tty);
    std::string suffix = code.empty() ? "" : " [" + code + "]";
    std::fprintf(stderr, "%s: %s %s%s\n", head.c_str(), paint("error:", style::ERROR, tty).c_str(),
                 msg.c_str(), suffix.c_str());
    if (!src.empty()) {
        std::string text = rstrip(src, "\n");
        int width = line >= 1 ? (int)std::to_string(line).size() : 1;
        std::string gut = line >= 1 ? std::to_string(line) : std::string((size_t)width, ' ');
        while ((int)gut.size() < width) gut = " " + gut;
        std::fprintf(stderr, "   %s %s\n", dim(gut, tty).c_str(), text.c_str());
        if (col >= 1) {
            std::string caret((size_t)(col - 1), ' ');
            caret += "^";
            std::string pad((size_t)(width + 3), ' ');
            std::fprintf(stderr, "%s%s %s\n", pad.c_str(), dim("|", tty).c_str(),
                         paint(caret, style::CARET, tty).c_str());
        }
    }
}

void Diag::warning(const std::string& msg, const std::string& file, int line, int col,
                   const std::string& src) {
    warnings++;
    const bool tty = stderr_is_tty();
    std::string loc = loc_string(file, line, col);
    std::string head = loc.empty() ? bold(tool, tty) : bold(loc, tty);
    std::fprintf(stderr, "%s: %s %s\n", head.c_str(), paint("warning:", style::WARNING, tty).c_str(),
                 msg.c_str());
    if (!src.empty() && col >= 1) {
        int width = line >= 1 ? (int)std::to_string(line).size() : 1;
        std::string caret((size_t)(col - 1), ' ');
        caret += "^";
        std::string pad((size_t)(width + 3), ' ');
        std::fprintf(stderr, "%s%s %s\n", pad.c_str(), dim("|", tty).c_str(),
                     paint(caret, style::CARET, tty).c_str());
    }
}

void Diag::note(const std::string& msg, const std::string& file, int line, int col,
                const std::string& src) {
    notes++;
    const bool tty = stderr_is_tty();
    std::string loc = loc_string(file, line, col);
    std::string head = loc.empty() ? bold(tool, tty) : bold(loc, tty);
    std::fprintf(stderr, "%s: %s %s\n", head.c_str(), paint("note:", style::NOTE, tty).c_str(),
                 msg.c_str());
    (void)src;
}

void Diag::help(const std::string& msg) {
    if (msg.empty()) return;
    const bool tty = stderr_is_tty();
    std::fprintf(stderr, "   %s %s\n", paint("help:", style::HINT, tty).c_str(), msg.c_str());
}

void Diag::fatal(const std::string& msg, const std::string& file, int line, int col) {
    const bool tty = stderr_is_tty();
    std::string loc = loc_string(file, line, col);
    std::string head = loc.empty() ? bold(tool, tty) : bold(loc, tty);
    std::fprintf(stderr, "%s: %s %s\n", head.c_str(),
                 paint("fatal error:", style::ERROR, tty).c_str(), msg.c_str());
    std::fprintf(stderr, "%s: %s\n", bold(tool, tty).c_str(),
                 paint("compilation terminated.", style::ERROR, tty).c_str());
    std::exit(1);
}

int Diag::summary() {
    if (errors) {
        const bool tty = stderr_is_tty();
        std::fprintf(stderr, "%s: %s\n", bold(tool, tty).c_str(),
                     paint(fmt("%d error%s generated.", errors, errors == 1 ? "" : "s"),
                           style::ERROR, tty).c_str());
    }
    return errors ? 1 : 0;
}

void Diag::ok(const std::string& msg) {
    std::printf("%s %s\n", green("OK", stdout_is_tty()).c_str(), msg.c_str());
}

void Diag::info(const std::string& msg) {
    std::printf("%s %s\n", cyan("->", stdout_is_tty()).c_str(), msg.c_str());
}

std::string program_name() {
    if (g_argv0 && *g_argv0) {
        std::string s = g_argv0;
        size_t p = s.find_last_of('/');
        return p == std::string::npos ? s : s.substr(p + 1);
    }
    return "wizc";
}

}  // namespace wiz
