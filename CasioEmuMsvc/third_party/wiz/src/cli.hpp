// cli.hpp - a tiny command-line parser plus data-directory autodetection.
//
// The grammar covers what the drivers need: long options (--target), short
// options (-o), "--opt=value", an optional-value option (--color[=WHEN]) and
// bare positionals after "--".
#pragma once

#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common.hpp"

namespace wiz {

struct CliOption {
    std::string flag;                    // exact spelling: "--target", "-o", "-Wall"
    std::string name;                    // canonical key
    bool takes_value = false;
    bool value_optional = false;         // --color[=WHEN]
    std::string const_value = "always";  // value used when the option is bare
    std::vector<std::string> allowed;    // restrict the consumed value
};

struct Cli {
    std::vector<std::string> positional;
    std::set<std::string> present;
    std::map<std::string, std::string> values;

    bool has(const std::string& n) const { return present.count(n) != 0; }
    std::string value(const std::string& n, const std::string& def = "") const {
        auto it = values.find(n);
        return it == values.end() ? def : it->second;
    }
};

inline bool parse_cli(int argc, char** argv, const std::vector<CliOption>& opts, Cli& out,
                      std::string& error) {
    bool no_more = false;
    for (int i = 1; i < argc; i++) {
        std::string tok = argv[i];
        if (!no_more && tok == "--") {
            no_more = true;
            continue;
        }
        if (!no_more && tok.size() > 1 && tok[0] == '-') {
            std::string flag = tok, inline_val;
            bool has_inline = false;
            size_t eq = tok.find('=');
            if (eq != std::string::npos) {
                flag = tok.substr(0, eq);
                inline_val = tok.substr(eq + 1);
                has_inline = true;
            }
            const CliOption* found = nullptr;
            for (const auto& o : opts)
                if (o.flag == flag) {
                    found = &o;
                    break;
                }
            if (!found) {
                error = "unrecognized option '" + flag + "'";
                return false;
            }
            out.present.insert(found->name);
            if (!found->takes_value) {
                if (has_inline) {
                    error = "option '" + flag + "' takes no value";
                    return false;
                }
                out.values[found->name] = "";
                continue;
            }
            if (has_inline) {
                out.values[found->name] = inline_val;
                continue;
            }
            if (found->value_optional) {
                if (i + 1 < argc &&
                    (found->allowed.empty() || contains(found->allowed, std::string(argv[i + 1]))) &&
                    (!found->allowed.empty() || argv[i + 1][0] != '-')) {
                    out.values[found->name] = argv[++i];
                } else {
                    out.values[found->name] = found->const_value;
                }
                continue;
            }
            if (i + 1 >= argc) {
                error = "option '" + flag + "' requires a value";
                return false;
            }
            out.values[found->name] = argv[++i];
            continue;
        }
        out.positional.push_back(tok);
    }
    return true;
}

// Find the directory holding runtime.asm / the boot templates. Called once at
// start-up with argv[0]; falls back to "./compiler".
inline std::string autodetect_data_dir(const char* argv0) {
    namespace fs = std::filesystem;
    std::vector<std::string> cands;
    if (const char* r = std::getenv("WIZ_ROOT")) {
        cands.push_back(std::string(r) + "/compiler");
        cands.push_back(r);
    }
    std::error_code ec;
    fs::path exe;
    if (argv0 && *argv0) {
        exe = fs::weakly_canonical(fs::path(argv0), ec);
        if (!ec) exe = exe.parent_path();
        else exe = fs::path(argv0).parent_path();
    }
    if (!exe.empty()) {
        cands.push_back((exe / ".." / "compiler").string());
        cands.push_back((exe / "compiler").string());
        cands.push_back(exe.string());
    }
    fs::path cwd = fs::current_path(ec);
    if (!ec) {
        cands.push_back((cwd / "compiler").string());
        cands.push_back(cwd.string());
    }
    for (const auto& c : cands) {
        std::error_code e2;
        if (fs::exists(fs::path(c) / "runtime.asm", e2)) return c;
    }
    return "compiler";
}

// ------------------------------------------------------------------ path bits
// os.path.splitext(...)[0]
inline std::string splitext_root(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string dir = slash == std::string::npos ? "" : path.substr(0, slash + 1);
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot == std::string::npos) return path;
    size_t lead = 0;
    while (lead < base.size() && base[lead] == '.') lead++;
    if (dot < lead) return path;
    return dir + base.substr(0, dot);
}

inline std::string path_basename(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

inline std::string path_dirname(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

}  // namespace wiz
