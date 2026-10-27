// WizToolchain.cpp - the Wiz / CWiz IDE window.
//
// The emulator links the Wiz toolchain in-process (`third_party/wiz`, the static
// library `wizcore`), so this window can compile a program, show the diagnostics
// and load the resulting image into the running model without spawning anything.
//
// Layout (mirrors the Rop Compiler window):
//   toolbar      target / language / file choosers and Build / Lint actions
//   left panel   the editor (TextEditor) with the cursor position
//   right panel  the log of the last build: ROM size, RAM used, notes
//   bottom       the diagnostics with their line and column
//
// The Wiz language is described in the Wiz repository (`docs/GUIDE.md`).
#include "WizToolchain.h"

#include "Chipset/Chipset.hpp"
#include "Chipset/ePSCpu.h"
#include "Ext/SysDialog.h"

#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "build.hpp"
#include "common.hpp"
#include "cpre.hpp"
#include "lint.hpp"
#include "patch.hpp"
#include "rop.hpp"
#include "targets.hpp"
#include "version.hpp"
#include "wiz.hpp"
#include "imgui/TextEditor.h"
#include "Ui.hpp"

namespace {

bool SourceHasCwapp(const std::string& src) {
	size_t pos = 0;
	while (pos <= src.size()) {
		size_t end = src.find('\n', pos);
		if (end == std::string::npos)
			end = src.size();
		const std::string line = src.substr(pos, end - pos);
		const size_t b = line.find_first_not_of(" \t");
		if (b != std::string::npos && line.compare(b, 6, "#cwapp") == 0 &&
			(line.size() == b + 6 || line[b + 6] == ' ' || line[b + 6] == '\t'))
			return true;
		if (end == src.size())
			break;
		pos = end + 1;
	}
	return false;
}

// `#slot n` in a CWiz source: the menu slot the author chose.  0 when absent.
int SourceMenuSlot(const std::string& src) {
	size_t pos = 0;
	while (pos <= src.size()) {
		size_t end = src.find('\n', pos);
		if (end == std::string::npos)
			end = src.size();
		const std::string line = src.substr(pos, end - pos);
		const size_t b = line.find_first_not_of(" \t");
		if (b != std::string::npos && line.compare(b, 5, "#slot") == 0 &&
			(line.size() == b + 5 || line[b + 5] == ' ' || line[b + 5] == '\t')) {
			char* e = nullptr;
			const long n = std::strtol(line.c_str() + b + 5, &e, 10);
			if (e && n >= 1 && n <= 12)
				return static_cast<int>(n);
		}
		if (end == src.size())
			break;
		pos = end + 1;
	}
	return 0;
}

// ---------------------------------------------------------------- data files
// runtime.asm and the boot templates are read from disk at build time. Prefer an
// env override, then the app bundle's Resources/wiz (installed by CMake), then
// the vendored tree the binary was built from.
std::string WizDataDir() {
	if (const char* env = std::getenv("WIZ_DATA_DIR"))
		if (*env)
			return env;
	if (const char* base = SDL_GetBasePath()) {
		const std::filesystem::path b(base);
		const std::filesystem::path candidates[] = {
			b / "wiz",
			b / ".." / "Resources" / "wiz",
		};
		for (const auto& c : candidates) {
			std::error_code ec;
			if (std::filesystem::is_regular_file(c / "runtime.asm", ec))
				return c.lexically_normal().string();
		}
	}
#ifdef CASIOEMU_WIZ_DATA_DIR
	return CASIOEMU_WIZ_DATA_DIR;
#else
	return "compiler";
#endif
}

std::string WizExamplesDir() {
	if (const char* env = std::getenv("WIZ_DATA_DIR")) {
		if (*env) {
			std::filesystem::path candidate = std::filesystem::path(env) / "examples";
			std::error_code ec;
			if (std::filesystem::is_directory(candidate, ec))
				return candidate.string();
		}
	}
	if (const char* base = SDL_GetBasePath()) {
		const std::filesystem::path b(base);
		const std::filesystem::path candidates[] = {
			b / "wiz" / "examples",
			b / ".." / "Resources" / "wiz" / "examples",
		};
		for (const auto& c : candidates) {
			std::error_code ec;
			if (std::filesystem::is_directory(c, ec))
				return c.lexically_normal().string();
		}
	}
	return (std::filesystem::path(WizDataDir()) / "examples").string();
}

// ---------------------------------------------------------------- small text
std::vector<std::string> SplitLines(const std::string& text) {
	std::vector<std::string> lines;
	std::string current;
	for (char c : text) {
		if (c == '\n') {
			lines.push_back(current);
			current.clear();
		} else if (c != '\r') {
			current.push_back(c);
		}
	}
	lines.push_back(current);
	return lines;
}

std::string JoinLines(const std::vector<std::string>& lines) {
	std::string out;
	for (size_t i = 0; i < lines.size(); ++i) {
		if (i)
			out.push_back('\n');
		out += lines[i];
	}
	return out;
}

// ---------------------------------------------------------------- targets
struct TargetChoice {
	const char* label;
	const char* name;   // wiz target name
	const char* lang;   // the language whose extension the sample uses
};
const TargetChoice kTargets[] = {
	{"880  fx-880BTG", "cwii", "cwiz"},
	{"580  fx-580VN X", "cw", "cwiz"},
	{"580 app (firmware patch)", "cwapp", "cwiz"},
	{"580 ROP (payload)", "cwrop", "cwiz"},
	{"880 ROP (payload)", "cwiirop", "cwiz"},
};

const char* kLanguages[] = {"Auto (by file name)", "Wiz (.wz)", "CWiz (.cwiz)"};

const char* kSampleSource =
	"// CWiz - the C-syntax language of the Wiz toolchain. Build it here, then\n"
	"// press \"Build & Run\" to load the image into the model that is running.\n"
	"// See the Wiz repository, docs/GUIDE.md, for the language reference.\n"
	"\n"
	"int presses = 0;\n"
	"\n"
	"void main() {\n"
	"    clear();\n"
    "    print(4, 4, \"Hello from CWiz\");\n"
	"    while (1) {\n"
	"        if (key_hit(K_AC)) { presses = presses + 1; }\n"
	"        print(4, 20, \"AC  \", presses);\n"
	"        delay(2);\n"
	"    }\n"
	"}\n";

enum class Sev { Error, Warning, Note };

struct Diag {
	Sev sev = Sev::Error;
	int line = -1;
	int col = -1;
	int end_col = -1;
	std::string msg;
	std::string hint;
	std::string code;
};

ImVec4 DiagColor(Sev sev) {
	const bool dark = ThemeManager::Instance().IsDarkMode();
	switch (sev) {
	case Sev::Error:
		return dark ? ImVec4(0.95f, 0.35f, 0.35f, 1.0f) : ImVec4(0.70f, 0.05f, 0.05f, 1.0f);
	case Sev::Warning:
		return dark ? ImVec4(0.95f, 0.80f, 0.20f, 1.0f) : ImVec4(0.60f, 0.38f, 0.00f, 1.0f);
	default:
		return dark ? ImVec4(0.45f, 0.75f, 0.95f, 1.0f) : ImVec4(0.05f, 0.35f, 0.65f, 1.0f);
	}
}

const char* SevName(Sev sev) {
	switch (sev) {
	case Sev::Error:   return "error";
	case Sev::Warning: return "warning";
	default:           return "note";
	}
}

// ---------------------------------------------------------------- the window
class ToolchainWindow : public UIWindow {
public:
	ToolchainWindow() : UIWindow("Wiz Compiler") {
		inital_size = ImVec2(960, 700);
		flags |= ImGuiWindowFlags_MenuBar;
		std::snprintf(file_name_, sizeof(file_name_), "%s", "program.cwiz");
		editor_.SetLanguageDefinition(TextEditor::LanguageDefinition::CPlusPlus());
		editor_.SetText(kSampleSource);
		AddLog(Sev::Note, "Wiz toolchain " WIZ_VERSION " - see the Wiz repository for the language reference.");
	}

	void RenderCore() override {
		PickTargetFromModel();
		// Remember the model's real ROM before any Build & Run can replace
		// ModelDefinition.rom_path with a generated session image.
		if (stock_rom_path_.empty() && m_emu)
			StockRomPath();
		if (!data_dir_set_) {
			wiz::set_data_dir(WizDataDir());
			data_dir_set_ = true;
		}
		RefreshEditorPalette();

		RenderMenuBar();
		RenderToolbar();
		ImGui::Separator();

		const float diagnostics_height = 145.0f;
		const float main_height = std::max(
			160.0f, ImGui::GetContentRegionAvail().y - diagnostics_height - ImGui::GetStyle().ItemSpacing.y);

		if (ImGui::BeginTable("##wiz_layout", 2,
				ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
					ImGuiTableFlags_SizingStretchProp,
				ImVec2(-1, main_height))) {
			ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 0.60f);
			ImGui::TableSetupColumn("Output", ImGuiTableColumnFlags_WidthStretch, 0.40f);
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			RenderSourcePanel(main_height);

			ImGui::TableSetColumnIndex(1);
			RenderOutputPanel(main_height);

			ImGui::EndTable();
		}

		RenderDiagnosticsPanel(diagnostics_height);
		HandleShortcuts();
	}

private:
	// ------------------------------------------------------------ helpers
	const char* TargetName() const { return kTargets[target_idx_].name; }
	bool Is580() const {
		return m_emu && m_emu->ModelDefinition.model_name.find("580") != std::string::npos;
	}
	static const char* ModelLabel(const std::string& target) {
		if (target == "cwii")
			return "fx-880BTG";
		if (target == "cwapp")
			return "fx-580VN X app";
		return "fx-580VN X";
	}

	// The ROM that shipped with the model.  Build & Run / Patch replace
	// ModelDefinition.rom_path with a session image, so remember the stock one
	// before anything changes it.
	const std::string& StockRomPath() {
		if (!stock_rom_path_.empty() || !m_emu)
			return stock_rom_path_;
		std::string path = m_emu->ModelDefinition.rom_path;
		// A previous Build & Run may have left the model pointing at a session
		// image (``wiz/wiz-build.bin``, ``wiz/rom-patched-*.bin``).  Never treat
		// that generated file as the stock ROM: fall back to the model's ROM.
		if (path.rfind("wiz/", 0) == 0 || path.find("/wiz/") != std::string::npos) {
			if (m_emu->HasModelResource("rom.bin"))
				path = "rom.bin";
		}
		stock_rom_path_ = path;
		return stock_rom_path_;
	}

	// ROP entries are output kinds, not new hardware: they compile for the same
	// 580/880 chip but emit a gadget chain instead of a ROM image.
	bool IsRopTarget() const {
		const std::string name = TargetName();
		return name == "cwrop" || name == "cwiirop";
	}
	const char* HardwareTarget() const {
		const std::string name = TargetName();
		if (name == "cwrop")
			return "cw";
		if (name == "cwiirop")
			return "cwii";
		return TargetName();
	}

	const wiz::Target& Target() const { return wiz::get_target(HardwareTarget()); }

	std::string EffectiveLang() const {
		if (lang_idx_ == 1)
			return "wiz";
		if (lang_idx_ == 2)
			return "cwiz";
		if (wiz::ends_with(file_name_, ".cwiz"))
			return "cwiz";
		if (wiz::ends_with(file_name_, ".wz"))
			return "wiz";
		// No (or an unknown) extension: follow the target's language instead of
		// silently parsing a CWiz sample as Wiz.
		return kTargets[target_idx_].lang;
	}

	// The model in the emulator decides which calculator we program for.
	void PickTargetFromModel() {
		if (picked_target_ || !m_emu)
			return;
		picked_target_ = true;
		if (m_emu->hardware_id == casioemu::HW_CLASSWIZ)
			target_idx_ = 1;      // fx-580VN X
		else if (m_emu->hardware_id == casioemu::HW_CLASSWIZ_II)
			target_idx_ = 0;      // fx-880BTG
	}

	void AddLog(Sev sev, const std::string& text) {
		log_.push_back({sev, text});
	}

	// Compile once; a failure comes back as a single diagnostic.
	bool Compile(const std::string& src, wiz::BuildResult& out, Diag& diag,
				 const std::string& forced_target = std::string()) {
		try {
			const std::string target = forced_target.empty() ? HardwareTarget() : forced_target;
			if (EffectiveLang() == "cwiz")
				out = wiz::build_cwiz(src, target, file_name_);
			else
				out = wiz::build_source(src, target);
			return true;
		} catch (const wiz::CompileError& e) {
			diag = {Sev::Error, e.line, e.col, e.end_col, e.what(), e.hint, e.code};
			return false;
		} catch (const std::exception& e) {
			diag = {Sev::Error, -1, -1, -1, e.what()};
			std::string message = e.what();
			if (message.find("cannot read") != std::string::npos ||
				message.find("runtime.asm") != std::string::npos)
				diag.msg = message + "  (looked in " + wiz::data_dir() + ")";
			return false;
		}
	}

	void DoBuild(bool run_after) {
		log_.clear();
		diags_.clear();
		built_ok_ = false;
		editor_.SetErrorMarkers({});
		wiz::set_data_dir(WizDataDir());

		const std::string src = editor_.GetText();
		// A ROP target (or a `#rop` source) is a gadget-chain payload, not a ROM
		// image: compile it with the ROP backend and hand the bytes to the
		// debugger.
		if (IsRopTarget() || wiz::source_is_rop(src)) {
			DoRopBuild(src, run_after);
			return;
		}
		// `wizc` auto-detects a #cwapp section.  The debugger must do the same,
		// otherwise selecting "580 fx-580VN X" builds a plain 1 KB program and
		// then tries to load it over the firmware.
		if (TargetName() == std::string("cw") && EffectiveLang() == "cwiz" && SourceHasCwapp(src)) {
			target_idx_ = 2;
			AddLog(Sev::Note, "source has #cwapp; switching to the firmware app target");
		}
		const std::string target = TargetName();

		AddLog(Sev::Note, wiz::fmt("building for %s (%s)", target.c_str(), ModelLabel(target)));
		Diag diag;
		wiz::BuildResult result;
		if (!Compile(src, result, diag)) {
			diags_.push_back(diag);
			if (diag.line > 0)
				editor_.SetErrorMarkers({ {diag.line, diag.hint.empty()
													  ? diag.msg
													  : diag.msg + "\n" + diag.hint} });
			AddLog(Sev::Error, wiz::fmt("%s: %s", SevName(diag.sev), diag.msg.c_str()));
			if (!diag.hint.empty()) AddLog(Sev::Note, "help: " + diag.hint);
			return;
		}

		rop_launcher_hex_.clear();
		last_build_ = std::move(result);
		last_build_source_ = src;
		built_ok_ = true;
		for (const std::string& note : wiz::last_notes())
			AddLog(Sev::Note, note);
		ReportStats();
		if (run_after) {
			if (target == "cwapp" && Is580())
				PatchCurrentROM();
			else
				LoadIntoEmulator();
		}
	}

	void DoRopBuild(const std::string& src, bool run_after) {
		log_.clear();
		diags_.clear();
		built_ok_ = false;
		editor_.SetErrorMarkers({});

		const std::string model = std::string(HardwareTarget()) == "cwii" ? "880" : "580";
		AddLog(Sev::Note, wiz::fmt("building for rop (%s)", ModelLabel(model == "880" ? "cwii" : "cw")));

		wiz::RopResult rop = wiz::build_rop(src, model);
		std::istringstream lines(rop.log);
		std::string line;
		while (std::getline(lines, line)) {
			const size_t start = line.find_first_not_of(" \t;");
			if (start == std::string::npos)
				continue;
			const std::string text = line.substr(start);
			if (text.rfind("error", 0) == 0)
				AddLog(Sev::Error, text);
			else if (text.rfind("warning", 0) == 0)
				AddLog(Sev::Warning, text);
		}
		if (!rop.ok) {
			AddLog(Sev::Error, rop.error);
			diags_.push_back({Sev::Error, -1, -1, -1, rop.error});
			return;
		}
		if (rop.bytes.empty()) {
			AddLog(Sev::Error, "the payload is empty");
			return;
		}

		built_ok_ = true;
		rop_bytes_ = std::move(rop.bytes);
		rop_base_ = rop.base;
		rop_launcher_ = std::move(rop.launcher);
		rop_launcher_base_ = rop.launcher_base;
		rop_launcher_hex_ = rop.launcher_hex;
		AddLog(Sev::Note, wiz::fmt("payload %zu bytes @ %04X, launcher %zu bytes @ %04X",
								   rop_bytes_.size(), rop_base_, rop_launcher_.size(),
								   rop_launcher_base_));
		AddLog(Sev::Note, "launcher: " + rop_launcher_hex_);

		std::string directory;
		if (m_emu)
			directory = m_emu->GetModelFilePath("wiz");
		if (directory.empty())
			directory = "wiz";
		std::error_code ec;
		std::filesystem::create_directories(directory, ec);
		bin_path_ = (std::filesystem::path(directory) / (Stem() + ".bin")).string();
		WriteFile(bin_path_, std::string(rop_bytes_.begin(), rop_bytes_.end()));
		AddLog(Sev::Note, wiz::fmt("wrote %s", bin_path_.c_str()));

		if (run_after)
			InjectRop();
	}

	void InjectRop() {
		if (!m_emu || !me_mmu || rop_bytes_.empty()) {
			AddLog(Sev::Error, "the debugger is not attached");
			return;
		}
		// Clear the injection area first (flappy-bird / repeated-run fix)
		for (uint32_t z = rop_base_; z < rop_base_ + 256; ++z) me_mmu->WriteData(z, 0, 0);
		for (uint32_t z = rop_launcher_base_; z < rop_launcher_base_ + 32; ++z) me_mmu->WriteData(z, 0, 0);

		auto write_block = [this](uint32_t base, const std::vector<uint8_t>& bytes, const char* what) {
			for (size_t i = 0; i < bytes.size(); ++i)
				me_mmu->WriteData(base + static_cast<uint32_t>(i), bytes[i], 0);
			for (size_t i = 0; i < bytes.size(); ++i) {
				if (me_mmu->ReadData(base + static_cast<uint32_t>(i), false) != bytes[i]) {
					AddLog(Sev::Error, wiz::fmt("%s write failed @ %04X+%zu (not RAM)", what, base, i));
					if (std::strcmp(what, "launcher") == 0)
						AddLog(Sev::Error, "use #launcher common (580 D180, 880 9268)");
					return false;
				}
			}
			AddLog(Sev::Note, wiz::fmt("%s verified @ %04X (%zu bytes)", what, base, bytes.size()));
			return true;
		};
		if (!write_block(rop_base_, rop_bytes_, "payload"))
			return;
		if (!write_block(rop_launcher_base_, rop_launcher_, "launcher"))
			return;
		if (rop_launcher_base_ == 0xD180 || rop_launcher_base_ == 0x9268)
			AddLog(Sev::Note, "press LEFT then EXE");
	}

	// The linter is the same engine as the `wizlint` command-line tool: the
	// compiler's own diagnostics plus the curated style/robustness rules.
	void RunLint() {
		diags_.clear();
		editor_.SetErrorMarkers({});
		wiz::set_data_dir(WizDataDir());

		const std::vector<wiz::LintDiagnostic> results =
			wiz::lint_source(editor_.GetText(), file_name_, HardwareTarget());

		TextEditor::ErrorMarkers markers;
		int errors = 0, warnings = 0, notes = 0;
		for (const wiz::LintDiagnostic& item : results) {
			Sev sev = Sev::Note;
			if (item.severity == wiz::LintSeverity::Error) {
				sev = Sev::Error;
				++errors;
			} else if (item.severity == wiz::LintSeverity::Warning) {
				sev = Sev::Warning;
				++warnings;
			} else {
				++notes;
			}
			std::string message =
				item.rule.empty() ? item.message : item.message + "  [" + item.rule + "]";
			if (!item.hint.empty()) message += "\n    help: " + item.hint;
			Diag d;
			d.sev = sev;
			d.line = item.line;
			d.col = item.col;
			d.end_col = item.end_col;
			d.msg = item.message + (item.rule.empty() ? "" : "  [" + item.rule + "]");
			d.hint = item.hint;
			d.code = item.code;
			diags_.push_back(std::move(d));
			if (item.line > 0)
				markers[item.line] = item.hint.empty() ? item.message
													   : item.message + "\n" + item.hint;
		}
		editor_.SetErrorMarkers(markers);
		AddLog(errors ? Sev::Error : (warnings ? Sev::Warning : Sev::Note),
			   wiz::fmt("lint: %d error(s), %d warning(s), %d note(s)", errors, warnings, notes));
	}

	void ReportStats() {
		const wiz::Target& tg = Target();
		int ram_high = 0;
		for (const auto& item : last_build_.variables)
			ram_high = std::max(ram_high, item.second);
		const int ram_used = ram_high ? ram_high + 2 - tg.var_base : 0;
		const int ram_total = tg.var_limit - tg.var_base;
		WriteArtifacts();
		AddLog(Sev::Note, wiz::fmt("ROM %zu / %d bytes   RAM %d / %d bytes   %zu variable(s)",
								   last_build_.rom.size(), tg.rom_limit, ram_used, ram_total,
								   last_build_.variables.size()));
		AddLog(Sev::Note, wiz::fmt("wrote %s & %s", bin_path_.c_str(), asm_path_.c_str()));
	}

	// Write source, listing and image next to the model, so an outer toolchain
	// (patch_menu, wizrun, a real calculator) can pick them up.
	void WriteArtifacts() {
		std::string directory;
		if (m_emu)
			directory = m_emu->GetModelFilePath("wiz");
		if (directory.empty())
			directory = "wiz";
		std::error_code ec;
		std::filesystem::create_directories(directory, ec);

		const std::string stem = Stem();
		bin_path_ = (std::filesystem::path(directory) / (stem + ".bin")).string();
		asm_path_ = (std::filesystem::path(directory) / (stem + ".asm")).string();
		src_path_ = (std::filesystem::path(directory) / std::string(file_name_)).string();

		WriteFile(bin_path_, std::string(last_build_.rom.begin(), last_build_.rom.end()));
		WriteFile(asm_path_, last_build_.full_asm);
		WriteFile(src_path_, editor_.GetText());
	}

	static void WriteFile(const std::string& path, const std::string& data) {
		std::ofstream stream(path, std::ios::binary);
		if (stream && !data.empty())
			stream.write(data.data(), static_cast<std::streamsize>(data.size()));
	}

	// Patch the stock ROM in memory, write the result as a session image and
	// reboot the model on it.  The stock file is never modified: the emulator
	// runs a duplicate, and "Stock ROM" switches back.
	void PatchCurrentROM(bool in_place = false, const std::string& base_file = std::string()) {
		if (!m_emu)
			return;
		if (!Is580()) {
			AddLog(Sev::Error, "the menu patch is only wired for the fx-580VN X (this model is not a 580)");
			return;
		}
		const std::string src = editor_.GetText();
		if (EffectiveLang() != "cwiz" || !SourceHasCwapp(src)) {
			AddLog(Sev::Error, "Patch ROM is for a CWiz #cwapp source; this source has no #cwapp section");
			AddLog(Sev::Note, "a plain program uses Build & Run instead");
			return;
		}

		// Build with the app target and patch the *compiled image*, not a
		// re-parse of the editor.  This keeps the language and target identical
		// to the successful build and avoids odd parser errors from a mismatch.
		wiz::BuildResult image;
		Diag diag;
		if (!Compile(src, image, diag, "cwapp")) {
			diags_.push_back(diag);
			if (diag.line > 0)
				editor_.SetErrorMarkers({ {diag.line, diag.msg} });
			AddLog(Sev::Error, wiz::fmt("%s: %s", SevName(diag.sev), diag.msg.c_str()));
			return;
		}

		const std::string stock = StockRomPath();
		const std::string base = base_file.empty() ? stock : base_file;
		std::vector<uint8_t> rom;
		try {
			if (base_file.empty()) {
				rom = m_emu->ReadModelResource(base);
			} else {
				std::ifstream in(base_file, std::ios::binary);
				rom.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			}
		} catch (const std::exception& e) {
			AddLog(Sev::Error, std::string("cannot read the ROM: ") + e.what());
			return;
		}
		if (rom.size() != 262144) {
			AddLog(Sev::Error, wiz::fmt("the ROM (%s) is %zu bytes, not a 256 KB fx-580VN X dump",
									  base.c_str(), rom.size()));
			return;
		}

		// A #slot n directive in the source chooses the menu slot; otherwise the
		// selector next to the button does.
		int patch_slot = slot_ + 1;
		if (const int from_source = SourceMenuSlot(src)) {
			patch_slot = from_source;
			slot_ = from_source - 1;
			AddLog(Sev::Note, wiz::fmt("using #slot %d from the source", patch_slot));
		}

		// The bundled dump is a different firmware revision from the one the
		// hook addresses were read from; allow the mismatch and report it.
		wiz::RomPatchReport report = wiz::patch_rom_image(std::move(rom), image, patch_slot,
													 /*force=*/true,
													 /*allow_hook_mismatch=*/true);
		for (const std::string& warning : report.warnings)
			AddLog(Sev::Warning, warning);
		if (!report.ok) {
			AddLog(Sev::Error, wiz::fmt("patch: %s%s", report.error.c_str(),
										report.error_line > 0
											? wiz::fmt(" (line %d)", report.error_line).c_str()
											: ""));
			return;
		}

		std::string name;
		if (in_place)
			name = stock;
		else if (base_file.empty())
			name = wiz::fmt("wiz/rom-patched-%c.bin", "123456789ABC"[patch_slot - 1]);
		else {
			std::string stem = std::filesystem::path(base_file).stem().string();
			if (stem.empty())
				stem = "custom";
			name = "wiz/rom-" + stem + ".bin";
		}
		try {
			// Keep the untouched base once, so "Stock ROM" can always recover.
			if (in_place && !m_emu->HasModelResource(stock + ".orig"))
				m_emu->WriteModelSessionResource(stock + ".orig", rom);
			m_emu->WriteModelSessionResource(name, report.rom);
		} catch (const std::exception& e) {
			AddLog(Sev::Error, std::string("cannot write the patched image: ") + e.what());
			return;
		}

		const bool was_paused = m_emu->GetPaused();
		m_emu->SetPaused(true);
		std::string error;
		bool loaded = false;
		{
			std::lock_guard<casioemu::FairRecursiveMutex> lock(m_emu->access_mx);
			const std::string previous = m_emu->ModelDefinition.rom_path;
			m_emu->ModelDefinition.rom_path = name;
			loaded = m_emu->chipset.ReloadRom(error);
			if (!loaded)
				m_emu->ModelDefinition.rom_path = previous;
		}
		m_emu->SetPaused(was_paused);
		if (!loaded) {
			AddLog(Sev::Error, "the emulator refused the patched image: " + error);
			return;
		}

		stock_rom_ = stock;
		AddLog(Sev::Note, wiz::fmt("patched slot %c (%d bytes) & rebooted%s",
								  "123456789ABC"[patch_slot - 1], report.code_size,
								  in_place ? " (original ROM overwritten, backup at .orig)"
										   : (base_file.empty() ? "" : " (custom ROM copy)")));
	}

	std::string Stem() const {
		std::string s = file_name_;
		const size_t dot = s.rfind('.');
		if (dot != std::string::npos)
			s.resize(dot);
		return s.empty() ? std::string("program") : s;
	}

	// ------------------------------------------------------------ emulator
	void LoadIntoEmulator() {
		if (!m_emu)
			return;
		if (std::string(TargetName()) == "cwapp") {
			AddLog(Sev::Note, "this target is a firmware patch: patch it into the ROM with patch_menu "
							  "and run the patched firmware");
			return;
		}
		const std::string stock = StockRomPath();
		if (stock.empty()) {
			AddLog(Sev::Error, "this model has no ROM file to replace");
			return;
		}

		size_t stock_size = 0;
		try {
			stock_size = m_emu->ReadModelResource(stock).size();
		} catch (const std::exception&) {
		}
		if (stock_size && last_build_.rom.size() > stock_size) {
			AddLog(Sev::Error, wiz::fmt("the image is %zu bytes but the model ROM is only %zu",
										last_build_.rom.size(), stock_size));
			return;
		}
		// A plain Wiz program is a short image (about 1 KB) that starts at
		// address 0.  The emulator expects a full ROM buffer and copies the
		// image over the previous one; pad it with erased flash (0xFF) instead
		// of leaving the stock firmware behind it.
		std::vector<uint8_t> image = last_build_.rom;
		if (stock_size && image.size() < stock_size) {
			image.resize(stock_size, 0xFF);
		}

		// Keep the image beside the model and point the model's rom_path at it, so
		// the stock ROM stays untouched on disk.
		std::string directory = m_emu->GetModelFilePath("wiz");
		if (!directory.empty()) {
			std::error_code ec;
			std::filesystem::create_directories(directory, ec);
		}
		const std::string name = directory.empty() ? "wiz-build.bin" : "wiz/wiz-build.bin";
		try {
			m_emu->WriteModelSessionResource(name, image);
		} catch (const std::exception& e) {
			AddLog(Sev::Error, std::string("cannot write the image: ") + e.what());
			return;
		}

		const bool was_paused = m_emu->GetPaused();
		m_emu->SetPaused(true);
		std::string error;
		bool loaded = false;
		{
			std::lock_guard<casioemu::FairRecursiveMutex> lock(m_emu->access_mx);
			const std::string previous = m_emu->ModelDefinition.rom_path;
			m_emu->ModelDefinition.rom_path = name;
			loaded = m_emu->chipset.ReloadRom(error);
			if (!loaded)
				m_emu->ModelDefinition.rom_path = previous;
		}
		m_emu->SetPaused(was_paused);
		if (!loaded) {
			AddLog(Sev::Error, "the emulator refused the image: " + error);
			return;
		}
		stock_rom_ = stock;
		AddLog(Sev::Note, "loaded into the running model");
	}

	void RestoreStockRom() {
		if (!m_emu)
			return;
		const std::string stock = stock_rom_.empty() ? StockRomPath() : stock_rom_;
		if (stock.empty()) {
			AddLog(Sev::Error, "no stock ROM path is known");
			return;
		}
		// An in-place patch overwrote the stock resource; put the saved copy back.
		if (m_emu->HasModelResource(stock + ".orig")) {
			try {
				m_emu->WriteModelSessionResource(stock, m_emu->ReadModelResource(stock + ".orig"));
			} catch (const std::exception&) {
			}
		}
		const bool was_paused = m_emu->GetPaused();
		m_emu->SetPaused(true);
		std::string error;
		bool loaded = false;
		{
			std::lock_guard<casioemu::FairRecursiveMutex> lock(m_emu->access_mx);
			m_emu->ModelDefinition.rom_path = stock;
			loaded = m_emu->chipset.ReloadRom(error);
		}
		m_emu->SetPaused(was_paused);
		AddLog(loaded ? Sev::Note : Sev::Error,
			   loaded ? "the stock ROM is loaded again" : "cannot reload the stock ROM: " + error);
	}

	void DoPreprocess() {
		const std::string src = editor_.GetText();
		try {
			const wiz::Target& tg = wiz::get_target(TargetName());
			std::string text = EffectiveLang() == "cwiz"
								   ? wiz::select_sections(wiz::preprocess_cwiz(src, file_name_, tg.name), tg)
								   : wiz::select_sections(src, tg);
			log_.clear();
			AddLog(Sev::Note, DumpDataDir());
			AddLog(Sev::Note, "--- preprocessed source ---");
			int line = 0;
			for (const std::string& item : SplitLines(text))
				log_.push_back({Sev::Note, wiz::fmt("%4d  %s", ++line, item.c_str())});
		} catch (const wiz::CompileError& e) {
			AddLog(Sev::Error, wiz::fmt("preprocess: %s (line %d)", e.what(), e.line));
		} catch (const std::exception& e) {
			AddLog(Sev::Error, std::string("preprocess: ") + e.what());
		}
	}

	std::string DumpDataDir() const { return "data files: " + wiz::data_dir(); }

	void SaveSource() {
		std::string directory = m_emu ? m_emu->GetModelFilePath("wiz") : std::string();
		if (directory.empty())
			directory = "wiz";
		std::error_code ec;
		std::filesystem::create_directories(directory, ec);
		src_path_ = (std::filesystem::path(directory) / std::string(file_name_)).string();
		WriteFile(src_path_, editor_.GetText());
		AddLog(Sev::Note, "saved " + src_path_);
	}

	void LoadSource() {
		std::string directory = m_emu ? m_emu->GetModelFilePath("wiz") : std::string();
		if (directory.empty())
			directory = "wiz";
		const std::string path = (std::filesystem::path(directory) / std::string(file_name_)).string();
		std::ifstream stream(path, std::ios::binary);
		if (!stream) {
			AddLog(Sev::Error, "cannot open " + path);
			return;
		}
		std::string text{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
		editor_.SetText(text);
		src_path_ = path;
		AddLog(Sev::Note, "loaded " + path);
	}

	void LoadExample(const std::filesystem::path& path) {
		std::ifstream stream(path, std::ios::binary);
		if (!stream) {
			AddLog(Sev::Error, "cannot open " + path.string());
			return;
		}
		std::string text{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
		editor_.SetText(text);
		editor_.SetCursorPosition(TextEditor::Coordinates());

		const std::string name = path.filename().string();
		std::snprintf(file_name_, sizeof file_name_, "%s", name.c_str());
		const std::string ext = path.extension().string();
		lang_idx_ = (ext == ".wz") ? 1 : 2;

		const bool is_rop = text.find("#rop") != std::string::npos;
		const bool is_880 = text.find("880") != std::string::npos &&
		                    text.find("#for") != std::string::npos;
		if (text.find("#cwapp") != std::string::npos)
			target_idx_ = 2;
		else if (is_rop)
			target_idx_ = is_880 ? 4 : 3;
		else
			target_idx_ = is_880 ? 0 : 1;
		// Keep the target the example asks for instead of re-deriving it from
		// the model that happens to be running.
		picked_target_ = true;

		built_ok_ = false;
		rop_launcher_hex_.clear();
		diags_.clear();
		editor_.SetErrorMarkers({});
		AddLog(Sev::Note, "loaded example " + name);
	}

	static std::string ExampleCategoryLabel(std::string name) {
		std::replace(name.begin(), name.end(), '_', ' ');
		if (!name.empty())
			name[0] = (char)std::toupper((unsigned char)name[0]);
		return name;
	}

	static std::string ExampleFileLabel(const std::filesystem::path& path) {
		const std::string ext = path.extension().string();
		return path.stem().string() + "  (" + (ext == ".wz" ? "Wiz" : "CWiz") + ")";
	}

	static std::set<std::string> NestedExampleNames(const std::filesystem::path& root) {
		std::set<std::string> names;
		std::error_code ec;
		for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
			if (!it->is_regular_file(ec) || it->path().parent_path() == root)
				continue;
			const std::string ext = it->path().extension().string();
			if (ext == ".wz" || ext == ".cwiz")
				names.insert(it->path().filename().string());
		}
		return names;
	}

	void RenderExampleDir(const std::filesystem::path& dir, bool root,
	                      const std::set<std::string>& nested) {
		std::error_code ec;
		std::vector<std::filesystem::path> dirs;
		std::vector<std::filesystem::path> files;
		for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
			if (it->is_directory(ec)) {
				dirs.push_back(it->path());
			} else if (it->is_regular_file(ec)) {
				const std::string ext = it->path().extension().string();
				if (ext != ".wz" && ext != ".cwiz")
					continue;
				if (root && nested.count(it->path().filename().string()))
					continue;
				files.push_back(it->path());
			}
		}
		std::sort(dirs.begin(), dirs.end());
		std::sort(files.begin(), files.end());

		for (const auto& sub : dirs) {
			ImGui::PushID(sub.filename().string().c_str());
			if (ImGui::BeginMenu(ExampleCategoryLabel(sub.filename().string()).c_str())) {
				RenderExampleDir(sub, false, nested);
				ImGui::EndMenu();
			}
			ImGui::PopID();
		}
		for (const auto& file : files) {
			ImGui::PushID(file.string().c_str());
			if (ImGui::MenuItem(ExampleFileLabel(file).c_str()))
				LoadExample(file);
			ImGui::PopID();
		}
	}

	void RenderExamplesMenu() {
		const std::string root = WizExamplesDir();
		std::error_code ec;
		if (!std::filesystem::is_directory(root, ec)) {
			ImGui::MenuItem("(examples not found)", nullptr, false, false);
			return;
		}
		const std::set<std::string> nested = NestedExampleNames(root);
		RenderExampleDir(root, true, nested);
	}

	// ------------------------------------------------------------ chrome
	void RefreshEditorPalette() {
		const bool dark = ThemeManager::Instance().IsDarkMode();
		if (dark == palette_dark_ && palette_ready_)
			return;
		palette_dark_ = dark;
		palette_ready_ = true;
		editor_.SetPalette(dark ? TextEditor::GetDarkPalette() : TextEditor::GetLightPalette());
	}

	static void ChooserLabel(const char* text) {
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(text);
		ImGui::SameLine();
	}

	void RenderMenuBar() {
		if (!ImGui::BeginMenuBar())
			return;
		if (ImGui::BeginMenu("File")) {
			if (ImGui::MenuItem("New / sample", nullptr, false, true))
				editor_.SetText(kSampleSource);
			if (ImGui::MenuItem("Save", "Ctrl+S"))
				SaveSource();
			if (ImGui::MenuItem("Load", "Ctrl+O"))
				LoadSource();
			if (ImGui::BeginMenu("Examples"))
			{
				RenderExamplesMenu();
				ImGui::EndMenu();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Build", "F5"))
				DoBuild(false);
			if (ImGui::MenuItem("Build & Run", "Ctrl+F5"))
				DoBuild(true);
			ImGui::Separator();
			if (ImGui::MenuItem("Preprocess"))
				DoPreprocess();
			if (ImGui::MenuItem("Lint", "Ctrl+L"))
				RunLint();
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Calculator")) {
			if (ImGui::MenuItem("Patch ROM (slot)"))
				PatchCurrentROM();
			if (ImGui::MenuItem("Patch original ROM", nullptr, false, Is580()))
				PatchCurrentROM(true);
			if (ImGui::MenuItem("Custom ROM...", nullptr, false, Is580()))
				SystemDialogs::OpenFileDialog([this](std::filesystem::path f) {
					PatchCurrentROM(false, f.string());
				});
			if (ImGui::MenuItem("Stock ROM", nullptr, false, !StockRomPath().empty()))
				RestoreStockRom();
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Help")) {
			ImGui::MenuItem("Wiz reference: docs/GUIDE.md", nullptr, false, false);
			ImGui::MenuItem("Built-ins: docs/API.md", nullptr, false, false);
			ImGui::EndMenu();
		}
		ImGui::EndMenuBar();
	}

	void RenderToolbar() {
		ChooserLabel("Target");
		ImGui::SetNextItemWidth(230.0f);
		if (ImGui::BeginCombo("##wiz_target", kTargets[target_idx_].label)) {
			for (int i = 0; i < IM_ARRAYSIZE(kTargets); ++i) {
				const bool selected = i == target_idx_;
				if (ImGui::Selectable(kTargets[i].label, selected))
					target_idx_ = i;
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		ChooserLabel("Language");
		ImGui::SetNextItemWidth(180.0f);
		if (ImGui::BeginCombo("##wiz_language", kLanguages[lang_idx_])) {
			for (int i = 0; i < IM_ARRAYSIZE(kLanguages); ++i) {
				const bool selected = i == lang_idx_;
				if (ImGui::Selectable(kLanguages[i], selected))
					lang_idx_ = i;
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		ChooserLabel("File");
		ImGui::SetNextItemWidth(220.0f);
		ImGui::InputText("##wiz_file", file_name_, sizeof(file_name_));

		// Primary actions: green when the last build succeeded, red otherwise,
		// matching the Rop Compiler's compile button.
		const bool ok = built_ok_;
		ImGui::PushStyleColor(ImGuiCol_Button, ok ? ImVec4(0.15f, 0.55f, 0.15f, 1.0f)
												 : ImVec4(0.55f, 0.12f, 0.12f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ok ? ImVec4(0.20f, 0.70f, 0.20f, 1.0f)
														 : ImVec4(0.70f, 0.18f, 0.18f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ok ? ImVec4(0.10f, 0.45f, 0.10f, 1.0f)
														: ImVec4(0.45f, 0.08f, 0.08f, 1.0f));
		if (ImGui::Button("Build"))
			DoBuild(false);
		ImGui::PopStyleColor(3);
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
			ImGui::BeginTooltip();
			ImGui::TextDisabled("F5");
			ImGui::EndTooltip();
		}
		ImGui::SameLine();
		if (ImGui::Button("Build & Run"))
			DoBuild(true);
		ImGui::SameLine();
		if (ImGui::Button("Lint"))
			RunLint();
		ImGui::SameLine();
		if (ImGui::Button("Preprocess"))
			DoPreprocess();
		ImGui::SameLine();
		if (ImGui::Button("Save"))
			SaveSource();
		ImGui::SameLine();
		if (ImGui::Button("Load"))
			LoadSource();
		if (!rop_launcher_hex_.empty()) {
			ImGui::SameLine();
			if (ImGui::Button("Copy launcher"))
				ImGui::SetClipboardText(rop_launcher_hex_.c_str());
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
				ImGui::BeginTooltip();
				ImGui::TextDisabled("Copy the %zu-byte launcher hex", rop_launcher_.size());
				ImGui::EndTooltip();
			}
		}

		if (Is580()) {
			ImGui::SameLine();
			ChooserLabel("Slot");
			ImGui::SetNextItemWidth(58.0f);
			ImGui::Combo("##wiz_slot", &slot_, "1\0" "2\0" "3\0" "4\0" "5\0" "6\0"
												  "7\0" "8\0" "9\0" "A\0" "B\0" "C\0");
			ImGui::SameLine();
			if (ImGui::Button("Patch ROM"))
				PatchCurrentROM();
			if (!StockRomPath().empty()) {
				ImGui::SameLine();
				if (ImGui::Button("Stock ROM"))
					RestoreStockRom();
			}
		}
	}

	void RenderSourcePanel(float height) {
		ImGui::BeginChild("##wiz_source_panel", ImVec2(-1, height), true);
		ImGui::TextUnformatted("Source");
		ImGui::SameLine();
		ImGui::TextDisabled("%s  -  %s", file_name_, TargetName());
		ImGui::Separator();

		const float status_height = ImGui::GetTextLineHeightWithSpacing() + 2.0f;
		const float editor_height = std::max(80.0f, ImGui::GetContentRegionAvail().y - status_height);
		editor_.Render("##wiz_source", ImVec2(-1, editor_height), true);
		const auto cursor = editor_.GetCursorPosition();
		ImGui::TextDisabled("Ln %d, Col %d   |   %d lines", cursor.mLine + 1, cursor.mColumn + 1,
							editor_.GetTotalLines());
		ImGui::EndChild();
	}

	void RenderClearRamSection() {
		static char ram_addr_str[32] = "E9E0";
		static int ram_size_int = 256;
		ImGui::PushItemWidth(80);
		ImGui::InputText("Addr", ram_addr_str, sizeof(ram_addr_str), ImGuiInputTextFlags_CharsHexadecimal);
		ImGui::PopItemWidth();
		ImGui::SameLine();
		ImGui::SetNextItemWidth(120);
		if (ImGui::InputInt("Size", &ram_size_int)) ram_size_int = std::max(1, std::min(ram_size_int, 65536));
		ImGui::SameLine();
		if (ImGui::Button("Clear RAM")) {
			uint32_t addr = 0;
			std::istringstream ss(ram_addr_str);
			if ((ss >> std::hex >> addr) && me_mmu) {
				for (int i = 0; i < ram_size_int; ++i)
					me_mmu->WriteData(addr + static_cast<uint32_t>(i), 0, 0);
				AddLog(Sev::Note, wiz::fmt("cleared RAM [%04X] %d bytes", addr, ram_size_int));
			} else {
				AddLog(Sev::Error, "Clear RAM: invalid hex address");
			}
		}
	}

	void RenderOutputPanel(float height) {
		ImGui::BeginChild("##wiz_output_panel", ImVec2(-1, height), true);
		ImGui::TextUnformatted("Build output");
		ImGui::SameLine();
		ImGui::TextDisabled("%s", built_ok_ ? "last build: ok" : "last build: not run or failed");
		ImGui::SameLine();
		RenderClearRamSection();
		ImGui::Separator();

		if (ImGui::BeginTabBar("##wiz_output_tabs")) {
			if (ImGui::BeginTabItem("Log")) {
				if (ImGui::Button("Clear"))
					log_.clear();
				ImGui::SameLine();
				if (ImGui::Button("Copy"))
					ImGui::SetClipboardText(LogText().c_str());
				ImGui::Separator();

				ImGui::BeginChild("##wiz_log", ImVec2(-1, -1), false);
				for (const auto& item : log_) {
					ImGui::PushStyleColor(ImGuiCol_Text, DiagColor(item.first));
					ImGui::TextWrapped("%s", item.second.c_str());
					ImGui::PopStyleColor();
				}
				if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
					ImGui::SetScrollHereY(1.0f);
				ImGui::EndChild();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Result")) {
				RenderResultTab();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		ImGui::EndChild();
	}

	void RenderDiagnosticsPanel(float height) {
		ImGui::BeginChild("##wiz_diagnostics_panel", ImVec2(-1, height), true);
		ImGui::TextUnformatted("Diagnostics");
		ImGui::SameLine();
		if (ImGui::SmallButton("Lint now"))
			RunLint();
		ImGui::SameLine();
		ImGui::TextDisabled("compiler diagnostics + unused/shadowing/unreachable/style rules");
		ImGui::Separator();

		int errors = 0, warnings = 0, notes = 0;
		for (const Diag& d : diags_) {
			errors += d.sev == Sev::Error ? 1 : 0;
			warnings += d.sev == Sev::Warning ? 1 : 0;
			notes += d.sev == Sev::Note ? 1 : 0;
		}
		if (diags_.empty()) {
			ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.35f, 1.0f), "No diagnostics.");
		} else {
			ImGui::Text("%d error(s), %d warning(s), %d note(s)", errors, warnings, notes);
		}

		ImGui::BeginChild("##wiz_lint", ImVec2(-1, -1), false);
		for (size_t i = 0; i < diags_.size(); ++i) {
			const Diag& d = diags_[i];
			ImGui::PushID(static_cast<int>(i));
			ImGui::PushStyleColor(ImGuiCol_Text, DiagColor(d.sev));
			ImGui::TextUnformatted(SevName(d.sev));
			ImGui::PopStyleColor();
			ImGui::SameLine();
			if (d.line > 0 && ImGui::SmallButton(wiz::fmt("%s:%d:%d", file_name_, d.line, d.col).c_str()))
				editor_.SetCursorPosition({d.line - 1, std::max(0, d.col - 1)});
			else if (d.line <= 0)
				ImGui::TextDisabled("%s", file_name_);
			ImGui::SameLine();
			ImGui::TextWrapped("%s", d.msg.c_str());
			if (!d.hint.empty()) {
				ImGui::Indent(24.0f);
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.75f, 0.85f, 1.0f));
				ImGui::TextWrapped("help: %s", d.hint.c_str());
				ImGui::PopStyleColor();
				ImGui::Unindent(24.0f);
			}
			if (!d.code.empty()) {
				ImGui::Indent(24.0f);
				ImGui::TextDisabled("%s", d.code.c_str());
				ImGui::Unindent(24.0f);
			}
			ImGui::PopID();
		}
		ImGui::EndChild();
		ImGui::EndChild();
	}

	void HandleShortcuts() {
		const ImGuiIO& io = ImGui::GetIO();
		const bool mod = io.ConfigMacOSXBehaviors ? (io.KeySuper || io.KeyCtrl) : io.KeyCtrl;
		if (ImGui::IsKeyPressed(ImGuiKey_F5))
			DoBuild(mod);
		if (mod && ImGui::IsKeyPressed(ImGuiKey_L))
			RunLint();
		if (mod && ImGui::IsKeyPressed(ImGuiKey_S))
			SaveSource();
		if (mod && ImGui::IsKeyPressed(ImGuiKey_O))
			LoadSource();
	}

	void RenderResultTab() {
		if (last_build_.rom.empty() && rop_bytes_.empty()) {
			ImGui::TextDisabled("No build result yet. Run Build & Run first.");
			return;
		}
		if (!rop_bytes_.empty()) {
			ImGui::Separator();
			ImGui::Text("ROP payload @ %04X (%zu bytes)", rop_base_, rop_bytes_.size());
			ImGui::Text("Launcher @ %04X (%zu bytes)  hex: %s",
				rop_launcher_base_, rop_launcher_.size(), rop_launcher_hex_.c_str());
			ImGui::SameLine();
			if (ImGui::Button("Copy launcher"))
				ImGui::SetClipboardText(rop_launcher_hex_.c_str());
			ImGui::Separator();
		}
		if (!last_build_.rom.empty()) {
			if (ImGui::Button("Download .bin")) {
				AddLog(Sev::Note, wiz::fmt("download: %zu ROM bytes", last_build_.rom.size()));
			}
			ImGui::SameLine();
			if (ImGui::Button("Copy hex")) {
				std::string hex;
				for (uint8_t b : last_build_.rom) {
					char buf[4];
					snprintf(buf, sizeof(buf), "%02X ", b);
					hex += buf;
				}
				ImGui::SetClipboardText(hex.c_str());
			}
		}
		if (!rop_bytes_.empty()) {
			std::string hex;
			for (uint8_t b : rop_bytes_) {
				char buf[4];
				snprintf(buf, sizeof(buf), "%02X ", b);
				hex += buf;
			}
			ImGui::InputTextMultiline("Payload hex", hex.data(), hex.size() + 1,
				ImVec2(-FLT_MIN, 120), ImGuiInputTextFlags_ReadOnly);
		}
	}

	std::string LogText() const {
		std::string out;
		for (const auto& item : log_) {
			out += SevName(item.first);
			out += ": ";
			out += item.second;
			out.push_back('\n');
		}
		return out;
	}

	TextEditor editor_;
	bool palette_dark_ = false;
	bool palette_ready_ = false;
	int target_idx_ = 0;
	int lang_idx_ = 0;
	int slot_ = 0;
	char file_name_[128] = {};
	bool data_dir_set_ = false;
	bool picked_target_ = false;
	bool built_ok_ = false;
	std::vector<std::pair<Sev, std::string>> log_;
	std::vector<Diag> diags_;
	wiz::BuildResult last_build_;
	std::vector<uint8_t> rop_bytes_;
	uint32_t rop_base_ = 0;
	std::vector<uint8_t> rop_launcher_;
	uint32_t rop_launcher_base_ = 0;
	std::string rop_launcher_hex_;
	std::string last_build_source_;
	std::string stock_rom_;
	std::string stock_rom_path_;
	std::string bin_path_;
	std::string asm_path_;
	std::string src_path_;
};

}  // namespace

UIWindow* CreateToolchainWindow() {
	return new ToolchainWindow();
}
