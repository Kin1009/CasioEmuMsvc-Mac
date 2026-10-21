// cwiz.hpp - CWiz, the C-syntax language of the Wiz toolchain.
//
// A complete, independent compiler: its own lexer, parser, semantic analyser and
// code generator. It targets the same nX-U8/U16 machines and links against the
// same runtime as the Wiz back end.
#pragma once

#include <map>
#include <string>

#include "common.hpp"
#include "targets.hpp"

namespace wiz {

struct CwizError : CompileError {
    explicit CwizError(const std::string& msg, int line = 0, int col = 0)
        : CompileError(msg, line, col) {}
};

struct CwizVarInfo {
    int addr = 0;
    std::string kind;
    int size = 0;
};

struct CwizResult {
    std::string asm_body;
    std::map<std::string, CwizVarInfo> variables;
};

CwizResult compile_cwiz(const std::string& src, const Target& target);

}  // namespace wiz
