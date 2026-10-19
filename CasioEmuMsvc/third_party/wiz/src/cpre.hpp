// cpre.hpp - the CWiz preprocessor.
//
// CWiz source may carry C-style directives. They are processed before the
// target-section filter and before lexing:
//
//     #define NAME value            object-like macro
//     #define NAME(a, b) value      function-like macro
//     #undef NAME
//     #ifdef NAME / #ifndef NAME / #if EXPR / #elif EXPR / #else / #endif
//     #include "file.cwiz"         inlined, with include guards via #pragma once
//     #pragma once
//     #error message
//     #for 580, 880                 target section (like `for 580`, see targets.hpp)
//
// Predefined macros: __CWIZ__ (1), and exactly one of __CW__ / __CWII__ / __CWAPP__
// (1, the others 0), plus __ROWS__ (the row count of the target).
//
// The output is line-preserving: a directive line becomes an empty line, so the
// line numbers reported by diagnostics match the input. `#include` is the one
// exception; cwiz_line_origin() lets callers map an output line back to the file
// and line it really came from.
#pragma once

#include <string>

namespace wiz {

struct CwizPreprocessInfo {
    std::string text;
    bool has_cwapp = false;  // the source contains a #cwapp section
    int slot = 2;            // #slot n (1..12); 2 when the directive is absent
};

// Preprocess `src` (which came from file `file`) for target `target` (an internal
// target name such as "cw", "cwii" or "cwapp"). Throws CwizError on a bad directive.
std::string preprocess_cwiz(const std::string& src, const std::string& file,
                            const std::string& target);

// As above, plus the #cwapp / #slot information the compiler driver needs to
// patch the built-in fx-580VN X ROM automatically.
CwizPreprocessInfo preprocess_cwiz_info(const std::string& src, const std::string& file,
                                        const std::string& target);

struct CwizOrigin {
    std::string file;  // empty when unknown
    int line = 0;      // 1-based line in `file`
};

// Map a 1-based line of the preprocessed text back to its origin.
CwizOrigin cwiz_line_origin(int line);

// Where an error reported at (fallback_file, line) really happened. Falls back to
// the arguments when no line map is available (e.g. for a Wiz build).
struct CwizPos {
    std::string file;
    int line = 0;
};
CwizPos cwiz_error_pos(const std::string& fallback_file, int line);

}  // namespace wiz
