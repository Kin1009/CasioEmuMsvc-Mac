// wiz.hpp - the Wiz language compiler (lexer, parser, code generator).
//
// Wiz is the original indentation-based language:
//     score = 0
//     def square(n):
//         return n * n
//     def main():
//         clear()
//         for i in range(0, 64):
//             pixel(i, i)
#pragma once

#include <map>
#include <string>
#include <vector>

#include "common.hpp"
#include "targets.hpp"

namespace wiz {

struct WizError : CompileError {
    explicit WizError(const std::string& msg, int line = 0) : CompileError(msg, line) {}
};

struct WizResult {
    std::string asm_body;
    std::map<std::string, int> variables;  // qualified name -> RAM address
};

WizResult compile_to_asm(const std::string& src, const Target& target);

const std::vector<std::string>& last_notes();

// Key-name table (K_*) of one machine.
const std::map<std::string, int>& key_consts(const std::string& target_name);

}  // namespace wiz
