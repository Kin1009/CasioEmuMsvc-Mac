// WizToolchain.cpp - the Wiz / CWiz IDE window.
//
// The emulator links the Wiz toolchain in-process (`third_party/wiz`, the static
// library `wizcore`), so this window can compile a program, show the diagnostics
// and load the resulting image into the running model without spawning anything.
//
// Tabs:
//   Source   the editor (TextEditor), the target / language / file name and the
//            Build, Build & Run, Preprocess, Save and Load actions
//   Build    the log of the last build: the ROM size, the RAM used, notes
//   Linter   the diagnostics with their line and column, and the build report
//
// The Wiz language is described in the Wiz repository (`docs/GUIDE.md`).
#include "WizToolchain.h"

#include "Chipset/Chipset.hpp"

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "build.hpp"
#include "common.hpp"
#include "cpre.hpp"
#include "targets.hpp"
#include "version.hpp"
#include "wiz.hpp"
#include "imgui/TextEditor.h"

namespace {

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
	"    text(4, 4, \"Hello from CWiz\");\n"
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
	std::string msg;
};

ImVec4 DiagColor(Sev sev) {
	switch (sev) {
	case Sev::Error:   return ImVec4(0.95f, 0.35f, 0.35f, 1.0f);
	case Sev::Warning: return ImVec4(0.95f, 0.80f, 0.20f, 1.0f);
	default:           return ImVec4(0.45f, 0.75f, 0.95f, 1.0f);
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
	ToolchainWindow() : UIWindow("Toolchain") {
		inital_size = ImVec2(820, 620);
		editor_.SetLanguageDefinition(TextEditor::LanguageDefinition::CPlusPlus());
		editor_.SetText(kSampleSource);
		editor_.SetPalette(TextEditor::GetDarkPalette());
		std::snprintf(file_name_, sizeof(file_name_), "%s", "program.cwiz");
		AddLog(Sev::Note, "Wiz toolchain " WIZ_VERSION " - see the Wiz repository for the language reference.");
	}

	void RenderCore() override {
		PickTargetFromModel();
		if (!data_dir_set_) {
			wiz::set_data_dir(WizDataDir());
			data_dir_set_ = true;
		}

		if (ImGui::BeginTabBar("##wiz_tabs")) {
			if (ImGui::BeginTabItem("Source")) {
				RenderSourceTab();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Build")) {
				RenderLogTab();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Linter")) {
				RenderLinterTab();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}

private:
	// ------------------------------------------------------------ helpers
	const char* TargetName() const { return kTargets[target_idx_].name; }

	const wiz::Target& Target() const { return wiz::get_target(TargetName()); }

	std::string EffectiveLang() const {
		if (lang_idx_ == 1)
			return "wiz";
		if (lang_idx_ == 2)
			return "cwiz";
		return wiz::ends_with(file_name_, ".cwiz") ? "cwiz" : "wiz";
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
	bool Compile(const std::string& src, wiz::BuildResult& out, Diag& diag) {
		try {
			const std::string target = TargetName();
			if (EffectiveLang() == "cwiz")
				out = wiz::build_cwiz(src, target, file_name_);
			else
				out = wiz::build_source(src, target);
			return true;
		} catch (const wiz::CompileError& e) {
			diag = {Sev::Error, e.line, e.col, e.what()};
			return false;
		} catch (const std::exception& e) {
			diag = {Sev::Error, -1, -1, e.what()};
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
		const std::string target = TargetName();
		const wiz::Target& tg = wiz::get_target(target);

		AddLog(Sev::Note, wiz::fmt("building for %s (%s %s, %s)", target.c_str(), tg.chip.c_str(),
								   tg.core.c_str(), EffectiveLang().c_str()));
		Diag diag;
		wiz::BuildResult result;
		if (!Compile(src, result, diag)) {
			diags_.push_back(diag);
			if (diag.line > 0)
				editor_.SetErrorMarkers({ {diag.line, diag.msg} });
			AddLog(Sev::Error, wiz::fmt("%s: %s", SevName(diag.sev), diag.msg.c_str()));
			AddLog(Sev::Note, "the Linter tab lists the diagnostics");
			return;
		}

		last_build_ = std::move(result);
		built_ok_ = true;
		for (const std::string& note : wiz::last_notes())
			AddLog(Sev::Note, note);
		ReportStats();
		if (run_after)
			LoadIntoEmulator();
	}

	// The compiler stops at the first error. The linter blanks each reported line
	// and retries, so one pass usually shows several problems. It is approximate:
	// use the Build tab for the authoritative first error.
	void RunLint() {
		diags_.clear();
		editor_.SetErrorMarkers({});
		const std::string target = TargetName();
		std::vector<std::string> lines = SplitLines(editor_.GetText());
		if (lines.empty())
			lines.push_back("");
		std::vector<int> seen;
		for (int round = 0; round < 12; ++round) {
			wiz::BuildResult result;
			Diag diag;
			if (Compile(JoinLines(lines), result, diag))
				break;
			diags_.push_back(diag);
			if (diag.line < 1 || static_cast<size_t>(diag.line) > lines.size())
				break;
			if (std::find(seen.begin(), seen.end(), diag.line) != seen.end())
				break;
			seen.push_back(diag.line);
			lines[diag.line - 1].clear();
		}
		TextEditor::ErrorMarkers markers;
		for (const Diag& d : diags_)
			if (d.line > 0)
				markers[d.line] = d.msg;
		editor_.SetErrorMarkers(markers);
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
		AddLog(Sev::Note, wiz::fmt("wrote %s (and %s)", bin_path_.c_str(), asm_path_.c_str()));
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
		const std::string original = m_emu->ModelDefinition.rom_path;
		if (original.empty()) {
			AddLog(Sev::Error, "this model has no ROM file to replace");
			return;
		}

		size_t original_size = 0;
		try {
			original_size = m_emu->ReadModelResource(original).size();
		} catch (const std::exception&) {
		}
		if (original_size && last_build_.rom.size() > original_size) {
			AddLog(Sev::Error, wiz::fmt("the image is %zu bytes but the model ROM is only %zu",
										last_build_.rom.size(), original_size));
			return;
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
			m_emu->WriteModelSessionResource(name, last_build_.rom);
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
		stock_rom_ = original;
		AddLog(Sev::Note, "loaded into the running model (the stock ROM is still at " + original + ")");
	}

	void RestoreStockRom() {
		if (!m_emu || stock_rom_.empty())
			return;
		const bool was_paused = m_emu->GetPaused();
		m_emu->SetPaused(true);
		std::string error;
		bool loaded = false;
		{
			std::lock_guard<casioemu::FairRecursiveMutex> lock(m_emu->access_mx);
			m_emu->ModelDefinition.rom_path = stock_rom_;
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

	// ------------------------------------------------------------ tabs
	void RenderSourceTab() {
		ImGui::SetNextItemWidth(200.0f);
		if (ImGui::BeginCombo("Target", kTargets[target_idx_].label)) {
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
		ImGui::SetNextItemWidth(160.0f);
		if (ImGui::BeginCombo("Language", kLanguages[lang_idx_])) {
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
		ImGui::SetNextItemWidth(170.0f);
		ImGui::InputText("File", file_name_, sizeof(file_name_));

		if (ImGui::Button("Build"))
			DoBuild(false);
		ImGui::SameLine();
		if (ImGui::Button("Build & Run"))
			DoBuild(true);
		ImGui::SameLine();
		if (ImGui::Button("Preprocess"))
			DoPreprocess();
		ImGui::SameLine();
		if (ImGui::Button("Lint"))
			RunLint();
		ImGui::SameLine();
		if (ImGui::Button("Save"))
			SaveSource();
		ImGui::SameLine();
		if (ImGui::Button("Load"))
			LoadSource();
		ImGui::SameLine();
		if (ImGui::Button("Sample"))
			editor_.SetText(kSampleSource);
		if (!stock_rom_.empty()) {
			ImGui::SameLine();
			if (ImGui::Button("Stock ROM"))
				RestoreStockRom();
		}

		// The editor takes the rest of the tab.
		const ImVec2 size = ImGui::GetContentRegionAvail();
		editor_.Render("##wiz_source", ImVec2(size.x, std::max(size.y, 80.0f)), true);
	}

	void RenderLogTab() {
		if (ImGui::Button("Clear"))
			log_.clear();
		ImGui::SameLine();
		if (ImGui::Button("Copy"))
			ImGui::SetClipboardText(LogText().c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("%s", built_ok_ ? "last build: ok" : "last build: not run or failed");
		ImGui::Separator();
		ImGui::BeginChild("##wiz_log", ImVec2(0, 0), true);
		for (const auto& item : log_) {
			ImGui::PushStyleColor(ImGuiCol_Text, DiagColor(item.first));
			ImGui::TextUnformatted(item.second.c_str());
			ImGui::PopStyleColor();
		}
		if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
			ImGui::SetScrollHereY(1.0f);
		ImGui::EndChild();
	}

	void RenderLinterTab() {
		if (ImGui::Button("Lint now"))
			RunLint();
		ImGui::SameLine();
		ImGui::TextDisabled("the compiler stops at the first error; the linter blanks each reported "
							"line and retries to find more");
		ImGui::Separator();

		int errors = 0;
		for (const Diag& d : diags_)
			errors += d.sev == Sev::Error ? 1 : 0;
		if (diags_.empty())
			ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.35f, 1.0f), "No diagnostics.");
		else
			ImGui::TextColored(DiagColor(Sev::Error), "%d problem(s)", errors);

		ImGui::BeginChild("##wiz_lint", ImVec2(0, 0), true);
		for (size_t i = 0; i < diags_.size(); ++i) {
			const Diag& d = diags_[i];
			ImGui::PushID(static_cast<int>(i));
			ImGui::PushStyleColor(ImGuiCol_Text, DiagColor(d.sev));
			ImGui::Text("%s", SevName(d.sev));
			ImGui::PopStyleColor();
			ImGui::SameLine();
			if (d.line > 0 && ImGui::SmallButton(wiz::fmt("%s:%d:%d", file_name_, d.line, d.col).c_str()))
				editor_.SetCursorPosition({d.line - 1, std::max(0, d.col - 1)});
			else if (d.line <= 0)
				ImGui::TextDisabled("%s", file_name_);
			ImGui::SameLine();
			ImGui::TextWrapped("%s", d.msg.c_str());
			ImGui::PopID();
		}
		ImGui::EndChild();
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
	int target_idx_ = 0;
	int lang_idx_ = 0;
	char file_name_[128] = {};
	bool data_dir_set_ = false;
	bool picked_target_ = false;
	bool built_ok_ = false;
	std::vector<std::pair<Sev, std::string>> log_;
	std::vector<Diag> diags_;
	wiz::BuildResult last_build_;
	std::string stock_rom_;
	std::string bin_path_;
	std::string asm_path_;
	std::string src_path_;
};

}  // namespace

UIWindow* CreateToolchainWindow() {
	return new ToolchainWindow();
}
