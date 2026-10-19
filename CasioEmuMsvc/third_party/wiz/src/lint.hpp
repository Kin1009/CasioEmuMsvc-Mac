// lint.hpp - static checks for Wiz (.wz) and CWiz (.cwiz) sources.
//
// The linter has two layers:
//   1. the compiler itself (syntax / semantic errors), and
//   2. a curated set of style and robustness rules that the compiler does not
//      report: unused variables, shadowing, unreachable statements, duplicate
//      target sections, tabs, long lines, trailing whitespace and TODO markers.
//
// It is deliberately conservative: every style finding is a warning, never an
// error, so a lint run cannot reject a program the compiler accepts.
#pragma once

#include <string>
#include <vector>

namespace wiz {

enum class LintSeverity { Error, Warning, Note };

struct LintDiagnostic {
    LintSeverity severity = LintSeverity::Warning;
    int line = 0;         // 1-based; 0 when the diagnostic has no position
    int col = 0;          // 1-based
    std::string rule;     // e.g. "readability/unused-variable"
    std::string message;
};

// `target` is a target name ("580", "880", "cw", "cwii", "cwapp").  The language
// is chosen by `file_name` (.cwiz => CWiz).  Compile diagnostics are included.
std::vector<LintDiagnostic> lint_source(const std::string& source,
                                        const std::string& file_name,
                                        const std::string& target);

// The rule ids the linter can emit, with a one-line description.
struct LintRuleInfo {
    const char* rule;
    const char* description;
};
const std::vector<LintRuleInfo>& lint_rules();

}  // namespace wiz
