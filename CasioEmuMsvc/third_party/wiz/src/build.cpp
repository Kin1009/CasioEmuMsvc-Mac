// build.cpp - implementation of the shared build pipeline.
#include "build.hpp"

#include <cstring>
#include <stdexcept>

#include "asm.hpp"
#include "cpre.hpp"
#include "cwiz.hpp"
#include "wiz.hpp"

namespace wiz {

static std::string g_data_dir = "compiler";

void set_data_dir(const std::string& dir) {
    if (!dir.empty()) g_data_dir = dir;
}
const std::string& data_dir() { return g_data_dir; }

// ------------------------------------------------------------------ templates
// `re.match(r'^\s*(type\s*\(|model\b|romwindow\b|extrn\b|public\b)', line, re.I)`
static bool is_dir_directive(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
    std::string rest = to_lower(line.substr(i));
    if (rest.compare(0, 4, "type") == 0) {
        size_t j = 4;
        while (j < rest.size() && (rest[j] == ' ' || rest[j] == '\t')) j++;
        if (j < rest.size() && rest[j] == '(') return true;
    }
    for (const char* w : {"model", "romwindow", "extrn", "public"}) {
        size_t n = std::strlen(w);
        if (rest.compare(0, n, w) == 0) {
            if (rest.size() == n || !(std::isalnum((unsigned char)rest[n]) || rest[n] == '_'))
                return true;
        }
    }
    return false;
}

// `re.match(r'^\s*b\s+_main\s*$', line, re.I)`
static bool is_b_main(const std::string& line) {
    std::vector<std::string> toks;
    std::string cur;
    for (char c : strip(line)) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) toks.push_back(cur);
    if (toks.size() != 2) return false;
    return to_lower(toks[0]) == "b" && to_lower(toks[1]) == "_main";
}

std::string make_boot(const Target& target) {
    bool ok = false;
    std::string t = read_text_file(g_data_dir + "/" + target.tmpl, &ok);
    if (!ok) throw std::runtime_error("cannot read boot template " + target.tmpl);
    t = replace_all(replace_all(t, "\r", ""), "\t", " ");
    std::vector<std::string> keep;
    for (const std::string& raw : split(t, '\n')) {
        std::string line = raw;
        if (is_dir_directive(line)) continue;
        if (is_b_main(line)) line = "    B __wiz_entry";
        keep.push_back(line);
    }
    return join(keep, "\n") + "\n";
}

std::string runtime_for(const Target& target) {
    std::string drop = target.planes == 2 ? ";@1p" : ";@2p";
    bool ok = false;
    std::string text = read_text_file(g_data_dir + "/runtime.asm", &ok);
    if (!ok) throw std::runtime_error("cannot read runtime.asm");
    std::vector<std::string> out;
    out.push_back(fmt("_ROWS EQU %d", target.rows));  // pixel() ignores y >= _ROWS
    for (const std::string& raw : split(text, '\n')) {
        std::string line = raw;
        std::string r = rstrip(line);
        if (ends_with(r, drop)) continue;
        if (ends_with(r, ";@seg") && !target.rom_seg) continue;
        if (ends_with(r, ";@off")) {  // VRAM row offset (cw: user y=0 is VRAM row 1)
            if (!target.row_off) continue;
            line = replace_all(line, "#32 ;@off", fmt("#%d ;@off", 32 * target.row_off));
        }
        out.push_back(line);
    }
    return join(out, "\n");
}

// ------------------------------------------------------------------ pipeline
static BuildResult finish(const std::string& body, const std::map<std::string, int>& variables,
                          const Target& tg) {
    std::string full = make_boot(tg) + body + runtime_for(tg);
    Assembler as(tg.core);
    auto img = as.assemble(full);
    BuildResult res;
    res.rom = to_bin(img);
    res.full_asm = full;
    res.variables = variables;
    res.syms = as.syms;
    return res;
}

BuildResult build_source(const std::string& src, const std::string& target) {
    const Target& tg = get_target(target.empty() ? DEFAULT_TARGET : target);
    std::string selected = select_sections(src, tg);
    WizResult r = compile_to_asm(selected, tg);
    BuildResult res = finish(r.asm_body, r.variables, tg);
    res.cwapp = tg.name == "cwapp";
    return res;
}

BuildResult build_cwiz(const std::string& src, const std::string& target,
                       const std::string& path) {
    const Target& tg = get_target(target.empty() ? DEFAULT_TARGET : target);
    // CWiz is the language with preprocessor directives; Wiz uses `#` for comments.
    CwizPreprocessInfo pp = preprocess_cwiz_info(src, path, tg.name);
    std::string selected = select_sections(pp.text, tg);
    CwizResult r = compile_cwiz(selected, tg);
    std::map<std::string, int> vars;
    for (const auto& kv : r.variables) vars[kv.first] = kv.second.addr;
    BuildResult res = finish(r.asm_body, vars, tg);
    res.cwapp = tg.name == "cwapp";
    res.slot = pp.slot;
    return res;
}

std::string language_of(const std::string& path, const std::string& forced) {
    if (!forced.empty()) return forced;
    std::string ext;
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) ext = to_lower(path.substr(dot));
    if (ext == ".cwiz") return "cwiz";
    return "wiz";
}

PathBuild build_path(const std::string& path, const std::string& target,
                     const std::string& forced_lang) {
    bool ok = false;
    std::string text = read_text_file(path, &ok);
    if (!ok) throw CompileError("cannot read file: " + path);
    PathBuild pb;
    pb.lang = language_of(path, forced_lang);
    if (pb.lang == "cwiz")
        pb.r = build_cwiz(text, target, path);
    else
        pb.r = build_source(text, target);
    return pb;
}

}  // namespace wiz
