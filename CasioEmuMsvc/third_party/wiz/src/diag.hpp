// diag.hpp - shared, gcc-style command-line diagnostics with colour.
#pragma once

#include <string>

#include "common.hpp"

namespace wiz {

enum class ColorMode { Auto, Always, Never };

void set_color_mode(ColorMode m);
void set_color_mode_from_arg(const std::string& value);
ColorMode color_mode();

bool stderr_is_tty();
bool stdout_is_tty();

namespace style {
inline const char* RESET = "0";
inline const char* BOLD = "1";
inline const char* DIM = "2";
inline const char* ERROR = "1;31";     // bold red
inline const char* WARNING = "1;35";   // bold magenta (gcc)
inline const char* NOTE = "1;36";      // bold cyan
inline const char* LOCATION = "1";     // bold
inline const char* CARET = "1;32";     // bold green
inline const char* OK = "32";          // green
inline const char* INFO = "36";        // cyan
inline const char* DIM_TXT = "2";
}  // namespace style

std::string paint(const std::string& text, const char* st, bool tty);
std::string bold(const std::string& t, bool tty);
std::string red(const std::string& t, bool tty);
std::string green(const std::string& t, bool tty);
std::string yellow(const std::string& t, bool tty);
std::string cyan(const std::string& t, bool tty);
std::string dim(const std::string& t, bool tty);

// The 1-based line `line` of `text`, or "" if out of range.
std::string source_line_at(const std::string& text, int line);

// argv[0] of the running tool, set by each driver's main(); used by program_name().
extern const char* g_argv0;

struct Diag {
    std::string tool;
    int errors = 0;
    int warnings = 0;
    int notes = 0;

    explicit Diag(std::string tool_) : tool(std::move(tool_)) {}

    void error(const std::string& msg, const std::string& file = "", int line = -1, int col = -1,
               const std::string& src = "");
    void warning(const std::string& msg, const std::string& file = "", int line = -1, int col = -1,
                 const std::string& src = "");
    void note(const std::string& msg, const std::string& file = "", int line = -1, int col = -1,
              const std::string& src = "");
    [[noreturn]] void fatal(const std::string& msg, const std::string& file = "", int line = -1,
                            int col = -1);
    int summary();
    void ok(const std::string& msg);
    void info(const std::string& msg);
};

std::string program_name();

}  // namespace wiz
