// build.cpp - implementation of the shared build pipeline.
#include "build.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>

#include "asm.hpp"
#include "asset_fallback.hpp"
#include "cpre.hpp"
#include "cwiz.hpp"
#include "wiz.hpp"

namespace wiz {

static bool has_asset(const std::string& dir) {
    bool ok = false;
    read_text_file(dir + "/runtime.asm", &ok);
    if (ok) return true;
    read_text_file(dir + "/template_ML620909.asm", &ok);
    return ok;
}

// Embeds call set_data_dir() at start-up, but lint/preview paths and the first
// compile can run before that.  Resolve a usable directory from the
// environment/known layouts; read_asset() still has an embedded fallback.
static std::string default_data_dir() {
    if (const char* e = std::getenv("WIZ_DATA_DIR"); e && *e) return e;
    for (const char* d : {"compiler", "data", "compiler/data", "."})
        if (has_asset(d)) return d;
    return "compiler";
}

static std::string g_data_dir = default_data_dir();

void set_data_dir(const std::string& dir) {
    if (!dir.empty()) g_data_dir = dir;
}
const std::string& data_dir() { return g_data_dir; }

// Look for `name` in the configured directory, then common bundle/repo
// layouts, then the copy embedded in wizcore.  `ok` is false only when the
// asset is unknown and every location failed.
static std::string read_asset(const std::string& name, bool& ok) {
    std::vector<std::string> dirs;
    auto add = [&](const std::string& d) {
        if (!d.empty() && !contains(dirs, d)) dirs.push_back(d);
    };
    add(g_data_dir);
    if (const char* e = std::getenv("WIZ_DATA_DIR")) add(e);
    add("compiler");
    add("compiler/data");
    add("data");
    add(".");
    for (const std::string& d : dirs) {
        std::string path = d + (d.back() == '/' ? "" : "/") + name;
        std::string text = read_text_file(path, &ok);
        if (ok) return text;
    }
    const std::string& embedded = embedded_asset(name);
    if (!embedded.empty()) {
        ok = true;
        return embedded;
    }
    ok = false;
    return "";
}

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
static bool is_org_directive(const std::string& line) {
    std::string t = to_lower(strip(line));
    return t.compare(0, 3, "org") == 0 &&
           (t.size() == 3 || t[3] == ' ' || t[3] == '\t');
}

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

std::string make_boot(const Target& target, uint32_t org) {
    bool ok = false;
    std::string t = read_asset(target.tmpl, ok);
    if (!ok) throw std::runtime_error("cannot read boot template " + target.tmpl +
                                      " (set WIZ_DATA_DIR or call set_data_dir())");
    t = replace_all(replace_all(t, "\r", ""), "\t", " ");
    std::vector<std::string> keep;
    for (const std::string& raw : split(t, '\n')) {
        std::string line = raw;
        if (is_dir_directive(line)) continue;
        if (is_b_main(line)) line = "    B __wiz_entry";
        if (org && is_org_directive(line)) line = fmt("    ORG %Xh", org);
        keep.push_back(line);
    }
    return join(keep, "\n") + "\n";
}

static const char* kCwappReturnLegacy =
    "__cwapp_return:\n"
    "    l r0, 0E7F4H\n"
    "    mov r1, #0\n"
    "    cmp r0, r1\n"
    "    bne __cwapp_return_boot\n"
    "    l r0, 0E5D3H\n"
    "    l r1, 0E5D4H\n"
    "    mov er4, #0\n"
    "    cmp er0, er4\n"
    "    bne __cwapp_return_go_l\n"
    "    b __cwapp_reset\n"
    "__cwapp_return_go_l:\n"
    "    mov er8, er0\n"
    "    mov r2, #0D5h\n"
    "    mov r3, #0E5h\n"
    "    mov r4, #0\n"
    "    mov r5, #0F0h\n"
    "__cwapp_return_loop_l:\n"
    "    cmp er0, er4\n"
    "    beq __cwapp_return_pop_l\n"
    "    l r6, [er2]\n"
    "    st r6, [er0]\n"
    "    add er0, #1\n"
    "    add er2, #1\n"
    "    b __cwapp_return_loop_l\n"
    "__cwapp_return_pop_l:\n"
    "    mov sp, er8\n"
    "    pop er14\n"
    "    pop er12\n"
    "    pop er10\n"
    "    pop er8\n"
    "    pop er6\n"
    "    pop er4\n"
    "    pop er2\n"
    "    pop er0\n"
    "    pop psw\n"
    "    pop lr\n"
    "    mov r0, #1\n"
    "    st r0, 0D113H\n"
    "    mov r0, #0\n"
    "    bl 2AD4CH\n"
    "    b 25E22h\n"
    "__cwapp_return_boot:\n"
    "    l r0, 0E5D3H\n"
    "    l r1, 0E5D4H\n"
    "    mov er4, #0\n"
    "    cmp er0, er4\n"
    "    bne __cwapp_return_go_b\n"
    "    b __cwapp_reset\n"
    "__cwapp_return_go_b:\n"
    "    mov er8, er0\n"
    "    mov r2, #0D5h\n"
    "    mov r3, #0E5h\n"
    "    mov r4, #0\n"
    "    mov r5, #0F0h\n"
    "__cwapp_return_loop_b:\n"
    "    cmp er0, er4\n"
    "    beq __cwapp_return_pop_b\n"
    "    l r6, [er2]\n"
    "    st r6, [er0]\n"
    "    add er0, #1\n"
    "    add er2, #1\n"
    "    b __cwapp_return_loop_b\n"
    "__cwapp_return_pop_b:\n"
    "    mov sp, er8\n"
    "    pop er14\n"
    "    pop er12\n"
    "    pop er10\n"
    "    pop er8\n"
    "    pop er6\n"
    "    pop er4\n"
    "    pop er2\n"
    "    pop er0\n"
    "    pop psw\n"
    "    mov r0, #0C1H\n"
    "    st r0, 0D111H\n"
    "    mov r0, #1\n"
    "    st r0, 0D113H\n"
    "    mov r0, #0\n"
    "    bl 2AD4CH\n"
    "    b 25E22h\n";

static const char* kCwappReturnExtra =
    "; cwapp clean return to the mode routine that called this app.\n"
    "__cwapp_return:\n"
    "    l r0, 0E5D3h\n"
    "    l r1, 0E5D4h\n"
    "    mov er4, #0\n"
    "    cmp er0, er4\n"
    "    bne __cwapp_return_go\n"
    "    b __cwapp_reset\n"
    "__cwapp_return_go:\n"
    "    mov er8, er0\n"
    "    mov r2, #0D5h\n"
    "    mov r3, #0E5h\n"
    "    mov r4, #0\n"
    "    mov r5, #0F0h\n"
    "__cwapp_return_loop:\n"
    "    cmp er0, er4\n"
    "    beq __cwapp_return_pop\n"
    "    l r6, [er2]\n"
    "    st r6, [er0]\n"
    "    add er0, #1\n"
    "    add er2, #1\n"
    "    b __cwapp_return_loop\n"
    "__cwapp_return_pop:\n"
    "    mov sp, er8\n"
    "    pop er14\n"
    "    pop er12\n"
    "    pop er10\n"
    "    pop er8\n"
    "    pop er6\n"
    "    pop er4\n"
    "    pop er2\n"
    "    pop er0\n"
    "    pop psw\n"
    "    mov r0, #0C1H\n"
    "    st r0, 0D111H\n"
    "    mov r0, #1\n"
    "    st r0, 0D113H\n"
    "    mov r0, #0\n"
    "    bl 2AD4CH\n"
    "    pop pc\n";

std::string runtime_for(const Target& target, bool extra_app) {
    std::string drop = target.planes == 2 ? ";@1p" : ";@2p";
    bool ok = false;
    std::string text = read_asset("runtime.asm", ok);
    if (!ok) throw std::runtime_error("cannot read runtime.asm (set WIZ_DATA_DIR or call set_data_dir())");
    std::vector<std::string> out;
    out.push_back(fmt("_ROWS EQU %d", target.rows));  // pixel() ignores y >= _ROWS
    // Drawing targets the shadow buffer (SB2); render() copies it to the LCD (SB1).
    out.push_back(fmt("_SB1 EQU %s", "0F800H"));
    out.push_back(fmt("_SB2 EQU %s", target.name == "cwii" ? "0CA54H" : "0DDD3H"));
    for (const std::string& raw : split(text, '\n')) {
        std::string line = raw;
        std::string r = rstrip(line);
        if (ends_with(r, drop)) continue;
        if (ends_with(r, ";@seg") && !target.rom_seg) continue;
        if (ends_with(r, ";@cwapp") && target.name != "cwapp") continue;
        if (ends_with(r, ";@off")) {  // VRAM row offset (cw: user y=0 is VRAM row 1)
            if (!target.row_off) continue;
            line = replace_all(line, "#32 ;@off", fmt("#%d ;@off", 32 * target.row_off));
        }
        out.push_back(line);
    }
    if (target.name == "cwapp") {
        const std::string block = extra_app ? kCwappReturnExtra : kCwappReturnLegacy;
        for (const std::string& line : split(block, '\n')) out.push_back(line);
    }
    return join(out, "\n");
}

// ------------------------------------------------------------------ pipeline
static BuildResult finish(const std::string& body, const std::map<std::string, int>& variables,
                          const Target& tg, uint32_t org = 0, bool extra_app = false) {
    std::string full = make_boot(tg, org) + body;
    if (tg.name != "fwnx") full += runtime_for(tg, extra_app);
    Assembler as(tg.core);
    auto img = as.assemble(full);
    BuildResult res;
    res.rom = to_bin(img);
    res.full_asm = full;
    res.variables = variables;
    res.syms = as.syms;
    return res;
}

// `import name` / `from name import symbol` - the upstream wizimport.py
// behaviour, implemented for the C++ driver.  Files are searched next to the
// including file and next to the main source, with .wz/.cw/no extension.
static std::string expand_wiz_imports(const std::string& src, const std::string& target,
                                      const std::string& path) {
    namespace fs = std::filesystem;
    const std::string main_dir = path.empty() ? std::string(".")
                                              : fs::path(path).parent_path().string();
    std::set<std::string> seen;
    if (!path.empty()) {
        std::error_code ec;
        seen.insert(fs::weakly_canonical(fs::absolute(path, ec), ec).string());
    }
    std::function<std::string(const std::string&, const std::string&)> expand =
        [&](const std::string& text, const std::string& cur_dir) -> std::string {
        std::string out;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            std::string t = strip(line);
            std::string name;
            if (t.rfind("import ", 0) == 0) name = strip(t.substr(7));
            else if (t.rfind("from ", 0) == 0) {
                size_t sp = t.find(" import ");
                if (sp != std::string::npos) name = strip(t.substr(5, sp - 5));
            }
            if (name.empty() || name.find_first_of(" .") != std::string::npos) {
                out += line + "\n";
                continue;
            }
            std::string found;
            for (const std::string& dir : {cur_dir, main_dir}) {
                for (const char* ext : {".wz", ".cw", ""}) {
                    std::string cand = dir + "/" + name + ext;
                    std::error_code ec;
                    if (fs::is_regular_file(cand, ec)) { found = cand; break; }
                }
                if (!found.empty()) break;
            }
            if (found.empty())
                throw CompileError("cannot import '" + name + "' (looked next to the source)",
                                   0, 0, "place " + name + ".wz next to the importing file",
                                   "wiz.import-not-found");
            std::error_code ec;
            std::string canon = fs::weakly_canonical(fs::absolute(found, ec), ec).string();
            out += "\n";
            if (seen.count(canon)) continue;
            seen.insert(canon);
            bool ok = false;
            std::string sub = read_text_file(found, &ok);
            if (!ok)
                throw CompileError("cannot read import '" + found + "'", 0, 0, "",
                                   "wiz.import-not-found");
            sub = select_sections(sub, get_target(target.empty() ? DEFAULT_TARGET : target));
            out += expand(sub, fs::path(found).parent_path().string());
        }
        return out;
    };
    return expand(src, main_dir);
}

static int extra_cwapp_line(const std::string& src) {
    int number = 0;
    for (const std::string& raw : split(src, '\n')) {
        number++;
        const std::string t = strip(raw);
        if (t.rfind("#cwapp", 0) == 0 && t.size() > 6 &&
            (t[6] == ' ' || t[6] == '\t'))
            return number;
    }
    return 0;
}

BuildResult build_source(const std::string& src, const std::string& target,
                         const std::string& path, uint32_t org) {
    const Target& tg = get_target(target.empty() ? DEFAULT_TARGET : target);
    const int extra_line = tg.name == "cwapp" ? extra_cwapp_line(src) : 0;
    if (extra_line > 0) {
        throw CompileError(
            "the '#cwapp <slot> <text> <icon> <selected>' form is CWiz only",
            extra_line, 0,
            "use a .cwiz source for an extra app, or the bare '#cwapp' + '#slot n' form",
            "build.cwapp-extra-wiz");
    }
    std::string selected = select_sections(expand_wiz_imports(src, target, path), tg);
    WizResult r = compile_to_asm(selected, tg);
    BuildResult res = finish(r.asm_body, r.variables, tg, org);
    res.cwapp = tg.name == "cwapp";
    return res;
}

BuildResult build_cwiz(const std::string& src, const std::string& target,
                       const std::string& path, uint32_t org) {
    const Target& tg = get_target(target.empty() ? DEFAULT_TARGET : target);
    // CWiz is the language with preprocessor directives; Wiz uses `#` for comments.
    CwizPreprocessInfo pp = preprocess_cwiz_info(src, path, tg.name);
    if (!pp.binary_path.empty()) {
        if (tg.name != "fwnx")
            throw CompileError("#binary is only valid for the fwnx target", 0, 0,
                               "build with --target fwnx, or remove #binary",
                               "build.binary-target");
        std::string binary_path = pp.binary_path;
        if (binary_path[0] != '/') {
            std::string dir = path.find_last_of('/') == std::string::npos
                                  ? std::string(".")
                                  : path.substr(0, path.find_last_of('/'));
            binary_path = dir + "/" + binary_path;
        }
        bool ok = false;
        std::vector<uint8_t> bin_data = read_binary_file(binary_path, &ok);
        if (!ok) throw CompileError("#binary file not found: " + binary_path, 0, 0,
                                    "place the image next to the source", "build.binary");
        std::string asm_body = "; firmware ROM binary image (reconstructed)\n";
        for (size_t i = 0; i < bin_data.size(); i += 8) {
            std::string line = "DB ";
            for (size_t j = i; j < std::min(i + 8, bin_data.size()); j++) {
                if (j > i) line += ",";
                line += fmt("0x%02X", (int)(uint8_t)bin_data[j]);
            }
            asm_body += line + "\n";
        }
        BuildResult res;
        res.rom = bin_data;
        res.full_asm = "ORG 0\n" + asm_body;
        res.variables = {};
        res.syms = {};
        res.cwapp = false;
        res.slot = pp.slot;
        return res;
    }
    std::string selected = select_sections(pp.text, tg);
    CwizResult r = compile_cwiz(selected, tg);
    std::map<std::string, int> vars;
    for (const auto& kv : r.variables) vars[kv.first] = kv.second.addr;
    BuildResult res = finish(r.asm_body, vars, tg, org, pp.cwapp_extra);
    res.cwapp = tg.name == "cwapp";
    res.slot = pp.slot;
    res.cwapp_extra = pp.cwapp_extra;
    res.cwapp_slot = pp.cwapp_extra ? pp.cwapp_slot : pp.slot;
    res.cwapp_text = pp.cwapp_text;
    res.cwapp_icon = pp.cwapp_icon;
    res.cwapp_icon_selected = pp.cwapp_icon_selected;
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
                     const std::string& forced_lang, uint32_t org) {
    bool ok = false;
    std::string text = read_text_file(path, &ok);
    if (!ok) throw CompileError("cannot read file: " + path);
    PathBuild pb;
    pb.lang = language_of(path, forced_lang);
    if (pb.lang == "cwiz")
        pb.r = build_cwiz(text, target, path, org);
    else
        pb.r = build_source(text, target, path, org);
    return pb;
}

}  // namespace wiz
