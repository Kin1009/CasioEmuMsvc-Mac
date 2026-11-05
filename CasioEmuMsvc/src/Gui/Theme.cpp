#include "Theme.h"
#include "Peripheral/OrdinaryLcdHistory.hpp"
#include "Peripheral/ScreenGate.hpp"
#include "FileDialog.hpp"
#include "Markdown.hpp"
#include "ModelInfo.h"
#include "Models.h"
#include "Settings/Settings.h"
#include "Shell/Shell.h"
#include "Shell/SessionHost.h"
#include "SnapshotWindow.h"
#include "ToolUi.hpp"
#ifndef CASIOEMU_CORE_WEB
#include "SysDialog.h"
#else
#include "WebDebuggerGui.h"
#endif
#include "Ui.hpp"
#include <Gui.h>
#include <Localization.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <string>
#include <sstream>
#include <system_error>
#include <vector>
#include <vibration.h>
#ifndef CASIOEMU_CORE_WEB
#include "Ext/DiscordRPC.h"
#endif

extern SDL_Surface* background;
extern SDL_Texture* bg_txt;

// Emulator display globals owned by Gui/HwController.cpp.
extern bool audio_enable;
extern int screen_buffer_select;
extern float screen_flashing_brightness_coeff;
extern bool screen_residual_enabled;

// Settings is a complete, categorised view of every persisted option:
// General, Appearance, Editor, Emulator, Calculator, Paths & Injections,
// Plugins, Integrations, Language, Window & Layout, Advanced.  Every control
// writes straight into the JSON settings store through ThemeManager so a
// restart reproduces exactly what the user sees here.
class SettingsWindow : public UIWindow {
private:
	enum Category {
		kGeneral = 0,
		kAppearance,
		kEditor,
		kEmulator,
		kCalculator,
		kPaths,
		kPlugins,
		kIntegrations,
		kLanguage,
		kWindow,
		kAdvanced,
		kDocumentation,
		kCategoryCount
	};

	int category = kGeneral;
	bool tabsApplied = false;
	std::string themeError;
	std::string statusMessage;
	double statusTime = 0.0;
	bool confirmReset = false;
	char tempInjectionFilePath[256]{};

	struct DocEntry {
		std::string title;
		std::string category;
		std::string modified;
		std::string rel;
		std::filesystem::path path;
	};
	std::vector<DocEntry> docs;
	bool docsScanned = false;
	int docSelected = -1;
	char docSearch[96]{};
	std::string docBody;
	std::string docLoadedPath;
	float docPendingScroll = -1.0f;
	float docCurrentScroll = 0.0f;
	int docScrollApply = 0;
	std::map<std::string, float> docScroll;
	DocEntry* DocAt(int index) {
		return index >= 0 && index < static_cast<int>(docs.size()) ? &docs[static_cast<size_t>(index)] : nullptr;
	}

	// Short labels keep the navigation readable at 140 px; the full group name
	// is shown as a tooltip on hover.
	static const char* CategoryLabel(int value) {
		switch (value) {
		case kGeneral: return "General";
		case kAppearance: return "Appearance";
		case kEditor: return "Editor";
		case kEmulator: return "Emulator";
		case kCalculator: return "Calculator";
		case kPaths: return "Paths";
		case kPlugins: return "Plugins";
		case kIntegrations: return "Integrations";
		case kLanguage: return "Language";
		case kWindow: return "Window";
		case kAdvanced: return "Advanced";
		case kDocumentation: return "Documentation";
		default: return "Settings";
		}
	}

	static const char* CategoryFullName(int value) {
		switch (value) {
		case kPaths: return "Paths & Injections";
		case kWindow: return "Window & Layout";
		default: return CategoryLabel(value);
		}
	}

	std::string HiddenTabsValue() const {
		std::string hidden;
		for (const auto* win : windows) {
			if (win && win->name && std::strcmp(win->name, "Settings") != 0 && !win->open) {
				hidden += "|";
				hidden += win->name;
			}
		}
		if (!hidden.empty()) hidden += "|";
		return hidden;
	}

	void ApplySavedTabVisibility(const ThemeSettings& settings) {
		const std::string hidden(settings.hiddenTabs);
		if (hidden.empty()) {
			for (auto* win : windows)
				if (win && win->name && std::strcmp(win->name, "Settings") == 0) win->open = true;
			return;
		}
		for (auto* win : windows) {
			if (!win || !win->name) continue;
			if (std::strcmp(win->name, "Settings") == 0) {
				win->open = true;
				continue;
			}
			const std::string token = std::string("|") + win->name + "|";
			if (hidden.find(token) != std::string::npos)
				win->open = false;
		}
	}

	static void SaveNow() {
		ThemeManager::Instance().SaveSettings();
	}

	void Notify(const char* message) {
		statusMessage = message;
		statusTime = ImGui::GetTime();
	}

	// ---------------------------------------------------------------- helpers
	static void OpenInFinder(const std::filesystem::path& path) {
#ifdef __APPLE__
		if (path.empty())
			return;
		std::string text = path.string();
		std::string encoded;
		encoded.reserve(text.size() + 8);
		for (char ch : text) {
			if (ch == ' ')
				encoded += "%20";
			else
				encoded += ch;
		}
		const std::string url = "file://" + encoded;
		SDL_OpenURL(url.c_str());
#else
		(void)path;
#endif
	}

	static UIWindow* FindTool(const char* name) {
		for (auto* win : windows)
			if (win && win->name && std::strcmp(win->name, name) == 0)
				return win;
		return nullptr;
	}

	// ---------------------------------------------------------------- general
	void RenderGeneral(casioemu::settings::SettingsData& data) {
		ToolUi::Section("Application");
		ToolUi::Description("Stored as JSON and applied on launch.");
		ImGui::Spacing();
		{
			ToolUi::PanelScope panel("##settings_about");
			const char* commit = "unknown";
			const char* build_time = "unknown";
#ifdef CASIOEMU_BUILD_COMMIT
			commit = CASIOEMU_BUILD_COMMIT;
#endif
#ifdef CASIOEMU_BUILD_TIME
			build_time = CASIOEMU_BUILD_TIME;
#endif
			ImGui::PushTextWrapPos(0.0f);
			ImGui::Text("Build: %s (%s UTC)", commit, build_time);
			ImGui::Text("Settings schema: %d", casioemu::settings::kSchemaVersion);
			ImGui::PopTextWrapPos();
		}

		ToolUi::Section("Workspace");
		{
			ToolUi::PanelScope panel("##settings_workspace");
			ImGui::TextWrapped("Folder: %s", data.workspace.folder.empty() ? "(not set)" : data.workspace.folder.c_str());
			ImGui::Spacing();
			if (ImGui::Button("Open Folder"))
				casioemu::shell::RequestOpenFolder();
			ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Reveal").x + 20.0f);
			if (ImGui::Button("Reveal"))
				OpenInFinder(data.workspace.folder);
			ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Open Recent").x + 20.0f);
			if (ImGui::Button("Open Recent"))
				casioemu::shell::OpenQuickOpen();
			if (!data.workspace.recentFolders.empty())
				ToolUi::Hint("Recent folders are in the Explorer.");
			else
				ToolUi::Hint("No recent folders.");
		}

		ToolUi::Section("Sessions");
		{
			ToolUi::PanelScope panel("##settings_sessions");
			const auto sessions = casioemu::session::List();
			if (sessions.empty()) {
				ToolUi::Hint("No live sessions.");
			}
			else {
				for (const auto& session : sessions) {
					ImGui::PushTextWrapPos(0.0f);
					ImGui::Text("%s%s  -  %s", session.title.c_str(), session.active ? "  (active)" : "",
						session.modelPath.c_str());
					ImGui::PopTextWrapPos();
				}
			}
			ImGui::Spacing();
			if (ImGui::Button("New Session"))
				casioemu::shell::RequestOpenModel();
			ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Reset Session").x + 20.0f);
			if (ImGui::Button("Reset Session"))
				casioemu::shell::ResetSession();
			ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Pause / Resume").x + 20.0f);
			if (ImGui::Button("Pause / Resume"))
				casioemu::shell::TogglePause();
		}
	}

	// ------------------------------------------------------------- appearance
	void RenderAppearance(ThemeManager& tm, ThemeSettings& settings,
		casioemu::settings::SettingsData& data) {
	// Cached palettes for the preset list swatches (preset files are static).
	static std::map<std::string, casioemu::plugin::ThemePalette> palette_cache;
	const auto palette_for = [&](const std::string& id) -> const casioemu::plugin::ThemePalette& {
		auto it = palette_cache.find(id);
		if (it == palette_cache.end()) {
			casioemu::plugin::ThemeResult result;
			std::string load_error;
			if (casioemu::plugin::LoadThemeFile((ThemeManager::ThemeDirectory() / (id + ".json")).string(),
					result, load_error))
				it = palette_cache.emplace(id, result.colors).first;
			else
				it = palette_cache.emplace(id, casioemu::plugin::ThemePalette{}).first;
		}
		return it->second;
	};

	// The requested darkness range leads the list; other shipped presets follow.
	static const char* preferred_order[] = {
		"dark", "dark-plus", "darker", "amoled", "midnight-blue", "graphite",
		"high-contrast-dark", "solarized", "vscode-dark",
		"vscode-dark-plus", "mono-green", "light-plus", "paper", "snow",
		"github-light", "high-contrast-light", "vscode-light"
	};
	const auto rank = [](const std::string& id) {
		for (int i = 0; i < static_cast<int>(IM_ARRAYSIZE(preferred_order)); ++i)
			if (id == preferred_order[i])
				return i;
		return static_cast<int>(IM_ARRAYSIZE(preferred_order));
	};
	auto themes = tm.AvailableThemes();
	std::stable_sort(themes.begin(), themes.end(), [&](const ThemeInfo& lhs, const ThemeInfo& rhs) {
		const int lr = rank(lhs.id);
		const int rr = rank(rhs.id);
		return lr == rr ? lhs.name < rhs.name : lr < rr;
	});

	ToolUi::Section("Color theme");
	ToolUi::Description("Click a preset; swatches show its palette.");
	ImGui::Spacing();

	const std::string saved = tm.CurrentThemeName();
	const std::string effective = saved == "classic" ? "dark" : saved;
	std::string currentLabel = saved.empty() ? "(none)" : saved;
	for (const auto& info : themes) {
		if (info.id == effective) {
			currentLabel = info.name;
			break;
		}
	}
	if (saved == "classic")
		currentLabel = "Dark (built-in classic)";
	if (tm.IsPluginThemeActive())
		ToolUi::Status("A plugin theme is active.", UIHelpers::kColorWarning);

	{
		ToolUi::PanelScope panel("##theme_list_panel");
		const float row_height = 44.0f;
		std::vector<ThemeInfo> dark_presets, light_presets, featured_presets, ordered;
		for (const auto& info : themes) {
			if (info.featured)
				featured_presets.push_back(info);
			else if (info.dark)
				dark_presets.push_back(info);
			else
				light_presets.push_back(info);
		}
		ordered.reserve(themes.size());
		ordered.insert(ordered.end(), dark_presets.begin(), dark_presets.end());
		ordered.insert(ordered.end(), light_presets.begin(), light_presets.end());
		ordered.insert(ordered.end(), featured_presets.begin(), featured_presets.end());
		const size_t light_start = dark_presets.size();
		const size_t featured_start = light_start + light_presets.size();
		for (size_t theme_index = 0; theme_index < ordered.size(); ++theme_index) {
			if (theme_index == 0 && !dark_presets.empty())
				ImGui::TextDisabled("Dark presets");
			if (theme_index == light_start && !light_presets.empty()) {
				ImGui::Spacing();
				ImGui::TextDisabled("Light presets");
			}
			if (theme_index == featured_start && !featured_presets.empty()) {
				ImGui::Spacing();
				ImGui::TextDisabled("Featured");
			}
			const ThemeInfo& info = ordered[theme_index];
			ImGui::PushID(info.id.c_str());
			const bool selected = info.id == effective;
			const ImVec2 row = ImGui::GetCursorScreenPos();
			const float row_width = ToolUi::RemainingWidth();
			const float text_x = row.x + 52.0f;
			const float name_width = ImGui::CalcTextSize(info.name.c_str()).x;
			const float name_available = std::max(24.0f, row.x + row_width - text_x - 8.0f);
			const bool two_line_name = name_width > name_available;
			const float this_row_height = two_line_name ? 58.0f : row_height;
			ImGui::InvisibleButton("##theme_row", ImVec2(row_width, this_row_height));
			const bool hovered = ImGui::IsItemHovered();
			if (ImGui::IsItemClicked()) {
				std::string error;
				if (!tm.ApplyThemeByName(info.id, &error))
					themeError = error;
				else
					themeError.clear();
			}
			ImDrawList* draw = ImGui::GetWindowDrawList();
			const auto& palette = palette_for(info.id);
			const float rounding = 6.0f;
			draw->AddRectFilled(row, ImVec2(row.x + row_width, row.y + this_row_height),
				ImGui::ColorConvertFloat4ToU32(selected ? ToolUi::WithAlpha(ToolUi::Palette().selection, 0.55f)
					: (hovered ? ToolUi::Palette().hover : ImVec4(0, 0, 0, 0))), rounding);
			draw->AddRect(row, ImVec2(row.x + row_width, row.y + this_row_height),
				ImGui::ColorConvertFloat4ToU32(selected ? ToolUi::Palette().accent : ToolUi::Palette().border),
				rounding, 0, selected ? 2.0f : 1.0f);
			if (selected)
				draw->AddRectFilled(ImVec2(row.x + 2.0f, row.y + 8.0f),
					ImVec2(row.x + 5.0f, row.y + this_row_height - 8.0f),
					ImGui::ColorConvertFloat4ToU32(ToolUi::Palette().accent), 2.0f);

			// Swatch: background, inner surface, accent stripe, foreground tick.
			const ImVec2 swatch(row.x + 12.0f, row.y + 7.0f);
			const ImVec2 swatch_end(swatch.x + 30.0f, swatch.y + 30.0f);
			draw->AddRectFilled(swatch, swatch_end, ImGui::ColorConvertFloat4ToU32(palette.background), 4.0f);
			draw->AddRectFilled(ImVec2(swatch.x + 4.0f, swatch.y + 4.0f),
				ImVec2(swatch.x + 20.0f, swatch.y + 20.0f), ImGui::ColorConvertFloat4ToU32(palette.surface), 3.0f);
			draw->AddRectFilled(ImVec2(swatch_end.x - 8.0f, swatch.y + 4.0f),
				ImVec2(swatch_end.x - 4.0f, swatch_end.y - 4.0f), ImGui::ColorConvertFloat4ToU32(palette.accent), 2.0f);
			draw->AddRectFilled(ImVec2(swatch.x + 4.0f, swatch_end.y - 6.0f),
				ImVec2(swatch.x + 20.0f, swatch_end.y - 4.0f), ImGui::ColorConvertFloat4ToU32(palette.foreground), 1.0f);
			draw->AddRect(swatch, swatch_end, ImGui::ColorConvertFloat4ToU32(ToolUi::Palette().border), 4.0f);

			const ImU32 text = ImGui::ColorConvertFloat4ToU32(ToolUi::Palette().foreground);
			const ImU32 muted = ImGui::ColorConvertFloat4ToU32(ToolUi::Palette().muted);
			// Wrap a long name at the last word that fits; never clip the label.
			std::string line1 = info.name;
			std::string line2;
			if (two_line_name) {
				const size_t split = info.name.find_last_of(' ', static_cast<size_t>(name_available / 7.0f));
				if (split != std::string::npos) {
					line1 = info.name.substr(0, split);
					line2 = info.name.substr(split + 1);
				}
			}
			draw->AddText(ImVec2(text_x, row.y + 5.0f), text, line1.c_str());
			if (!line2.empty()) {
				draw->AddText(ImVec2(text_x, row.y + 18.0f), text, line2.c_str());
				draw->AddText(ImVec2(text_x, row.y + 36.0f), muted, info.id.c_str());
			}
			else {
				// The id only: a variant tag made the line clip in a narrow dock.
				draw->AddText(ImVec2(text_x, row.y + 24.0f), muted, info.id.c_str());
			}
			ImGui::PopID();
			ImGui::Spacing();
		}
	}
	if (!themeError.empty())
		ToolUi::Status(themeError.c_str(), UIHelpers::kColorError);
	{
		char current_text[160];
		std::snprintf(current_text, sizeof(current_text), "Current: %s", currentLabel.c_str());
		ToolUi::Hint(current_text);
	}

	ToolUi::Field("Variant");
	if (ImGui::Button("Dark Mode"))
		tm.SetDarkMode();
	ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Light Mode").x + 20.0f);
	if (ImGui::Button("Light Mode"))
		tm.SetLightMode();

		ToolUi::Section("Scale and typography");
		{
			ToolUi::PanelScope panel("##settings_scale");
			float currentFontScale = tm.GetFontScale();
			ToolUi::Field("UI scale");
			ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x * 0.55f));
			if (ImGui::SliderFloat("##ui_scale", &currentFontScale, 0.8f, 2.0f, "%.2f")) {
				settings.scale = currentFontScale;
				data.appearance.uiScale = currentFontScale;
				tm.SetFontScale(currentFontScale);
			}
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				SaveNow();
				tm.RequestFontRebuild();
			}
			ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Apply").x + 20.0f);
			if (ImGui::Button("Apply")) {
				SaveNow();
				tm.RequestFontRebuild();
			}
			ImGui::TextDisabled("Typography: %s / %s", tm.Typography().uiFamily.c_str(),
				tm.Typography().monoFamily.c_str());
			ToolUi::Hint("Stays usable down to 0.8 scale on 13-inch.");
		}

		ToolUi::Section("Auto-tint");
		{
			ToolUi::PanelScope panel("##settings_tint");
			if (ImGui::Checkbox("Enable auto-tint (MD3 Monet)", &settings.enableAutoTint)) {
				data.appearance.autoTint = settings.enableAutoTint;
				SaveNow();
			}
			if (settings.enableAutoTint) {
				ImVec4 seedColor = settings.seedColor;
				ToolUi::Field("Seed color");
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				if (ImGui::ColorEdit4("##seed_color", (float*)&seedColor, ImGuiColorEditFlags_NoAlpha)) {
					tm.SetSeedColor(seedColor);
					data.appearance.seedColor = { seedColor.x, seedColor.y, seedColor.z, seedColor.w };
				}
				if (ImGui::Button("Extract")) {
#if !defined(TEST_BUILD) && !defined(CASIOEMU_CORE_WEB)
					seedColor = tm.ExtractDominantColor(bg_txt, renderer);
#endif
					tm.SetSeedColor(seedColor);
					data.appearance.seedColor = { seedColor.x, seedColor.y, seedColor.z, seedColor.w };
				}
			}
			else {
				ToolUi::Hint("Accent colour comes from the background.");
			}
		}

		ToolUi::Section("Background");
		{
			ToolUi::PanelScope panel("##settings_background");
			const bool has_background = std::filesystem::exists("background.jpg");
			ImGui::TextUnformatted(has_background ? "background.jpg is loaded." : "No custom background image.");
			ImGui::Spacing();
			if (ImGui::Button("Choose Image...")) {
#ifdef CASIOEMU_CORE_WEB
				WebDebuggerQueueOpenFile("/persist/background.jpg", "background.jpg");
#else
				SystemDialogs::OpenFileDialog([](std::filesystem::path pth) {
					std::error_code ec;
					std::filesystem::copy_file(pth, "background.jpg", std::filesystem::copy_options::overwrite_existing, ec);
				});
				tm.RequestBgReload();
#endif
			}
			ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Clear").x + 20.0f);
			if (ImGui::Button("Clear")) {
				std::error_code ec;
				std::filesystem::remove("background.jpg", ec);
				tm.RequestBgReload();
			}
#ifdef CASIOEMU_CORE_WEB
			int bgResult = 0;
			if (WebDebuggerConsumeFileResult("/persist/background.jpg", &bgResult) && bgResult == 0)
				tm.RequestBgReload();
#endif
			ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Reveal").x + 20.0f);
			if (ImGui::Button("Reveal"))
				OpenInFinder(std::filesystem::absolute("background.jpg").parent_path());
		}

		ToolUi::Section("Advanced style editor");
		if (ImGui::CollapsingHeader("Legacy ImGui style editor")) {
			ImGui::TextWrapped("%s", "Style edits are a legacy mirror.");
			auto& base = settings.isDarkMode ? settings.igs_dark : settings.igs_light;
			ImGuiStyle ims_backup;
			std::memcpy(&ims_backup, &ImGui::GetStyle(), sizeof(base));
			std::memcpy(&ImGui::GetStyle(), &base, sizeof(base));
			ImGui::ShowStyleEditor();
			std::memcpy(&base, &ImGui::GetStyle(), sizeof(base));
			std::memcpy(&ImGui::GetStyle(), &ims_backup, sizeof(base));
			if (ImGui::Button("Save Style"))
				SaveNow();
		}
	}

	// ----------------------------------------------------------------- editor
	void RenderEditor(ThemeManager& tm, casioemu::settings::SettingsData& data) {
		ToolUi::Section("Text editor");
		ToolUi::Description("Defaults for newly opened documents.");
		ImGui::Spacing();
		bool changed = false;
		{
			ToolUi::PanelScope panel("##settings_editor");
			changed |= ImGui::Checkbox("Show line numbers", &data.editor.showLineNumbers);
			changed |= ImGui::Checkbox("Word wrap", &data.editor.wordWrap);
			ToolUi::Field("Font size");
			ImGui::SetNextItemWidth(std::max(180.0f, ImGui::GetContentRegionAvail().x * 0.5f));
			changed |= ImGui::SliderFloat("##editor_font", &data.editor.fontSize, 10.0f, 24.0f, "%.0f px");
			ToolUi::Field("Tab size");
			ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x * 0.3f));
			changed |= ImGui::SliderInt("##editor_tab", &data.editor.tabSize, 1, 16);
			ToolUi::Hint("Per-document state is preserved.");
		}
		if (changed)
			SaveNow();
		ToolUi::Section("Diagnostics");
		{
			ToolUi::PanelScope panel("##settings_diag");
			ImGui::TextDisabled("Lint runs on open, save, and a 650 ms debounce while typing.");
			ImGui::TextDisabled("Problems are listed in the bottom panel and clicking one jumps to its location.");
		}
		(void)tm;
	}

	// --------------------------------------------------------------- emulator
	void RenderEmulator(ThemeManager& tm, ThemeSettings& settings,
		casioemu::settings::SettingsData& data) {
		auto& emu = data.emulator;
		ToolUi::Section("Performance");
		ToolUi::Description("Defaults to the model clock.");
		ImGui::Spacing();
		{
			ToolUi::PanelScope panel("##settings_perf");
			bool use_model_clock = emu.clockExponent <= 0;
			if (ImGui::Checkbox("Use model clock rate", &use_model_clock)) {
				emu.clockExponent = use_model_clock ? 0 : (m_emu ? static_cast<int>(std::lround(std::log2((double)m_emu->cycles.cycles_per_second))) : 20);
				if (m_emu && !use_model_clock)
					m_emu->cycles.Setup(Uint64{1} << std::clamp(emu.clockExponent, 1, 28), m_emu->cycles.timer_interval);
				if (m_emu && use_model_clock)
					m_emu->SetClockSpeed(1.0f);
				SaveNow();
			}
			if (!use_model_clock) {
				ToolUi::Field("Clock rate");
				int exponent = emu.clockExponent;
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				if (ImGui::SliderInt("##clock_exponent", &exponent, 1, 28, "2^%d CPS")) {
					emu.clockExponent = exponent;
					if (m_emu)
						m_emu->cycles.Setup(Uint64{1} << exponent, m_emu->cycles.timer_interval);
				}
				if (ImGui::IsItemDeactivatedAfterEdit())
					SaveNow();
			}
			if (m_emu) {
				char clock[96];
				std::snprintf(clock, sizeof(clock), "Current: %.3f MHz (%llu CPS)",
					(double)m_emu->cycles.cycles_per_second / 1024.0 / 1024.0,
					static_cast<unsigned long long>(m_emu->cycles.cycles_per_second));
				ToolUi::Hint(clock);
			}

			ToolUi::Field("Background");
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			if (ImGui::SliderFloat("##bg_throttle", &emu.backgroundThrottle, 0.05f, 1.0f, "%.2f x")) {
				emu.backgroundThrottle = std::clamp(emu.backgroundThrottle, 0.05f, 1.0f);
				casioemu::session::SetThrottle(emu.backgroundThrottle);
			}
			if (ImGui::IsItemDeactivatedAfterEdit())
				SaveNow();
			ToolUi::Hint("Cycle rate for inactive sessions.");

			bool low_performance = settings.lowPerformanceMode;
			if (ImGui::Checkbox("Power saving mode", &low_performance)) {
				casioemu::ordinary_lcd_history::UntrackedChange change;
				settings.lowPerformanceMode = low_performance;
				emu.lowPerformanceMode = low_performance;
				SaveNow();
			}
			ToolUi::Hint("Caps at 25 FPS to save power.");
		}

		ToolUi::Section("Run state");
		{
			ToolUi::PanelScope panel("##settings_runstate");
			if (!m_emu) {
				ToolUi::Hint("No calculator is running.");
			}
			else {
				bool paused = m_emu->GetPaused();
				if (ImGui::Checkbox("Pause emulation", &paused))
					m_emu->SetPaused(paused);
				ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Reboot").x + 20.0f);
				if (ImGui::Button("Reboot"))
					m_emu->ResetToMainMenu();
				ToolUi::Hint("Returns to the power-on menu.");
			}
		}

		ToolUi::Section("Audio");
		{
			ToolUi::PanelScope panel("##settings_audio");
			if (ImGui::Checkbox("Enable audio", &emu.audioEnabled)) {
				audio_enable = emu.audioEnabled;
				SaveNow();
			}
			ToolUi::Hint("Enable the model buzzer.");
		}

		ToolUi::Section("Display");
		{
			ToolUi::PanelScope panel("##settings_display");
			const bool lcd_options = m_emu && !casioemu::IsEpsFamily(m_emu->hardware_id) &&
				m_emu->hardware_id != casioemu::HW_TI_MULTI_VIEW;
			ImGui::BeginDisabled(!lcd_options);
			ToolUi::Field("Flash threshold");
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			if (ImGui::SliderInt("##lcd_flash", &emu.lcdFlashThreshold, 0, 0x3F)) {
				casioemu::screen_gate::SetFlashingThreshold(emu.lcdFlashThreshold);
			}
			if (ImGui::IsItemDeactivatedAfterEdit())
				SaveNow();
			ToolUi::Field("Flash brightness");
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			if (ImGui::SliderFloat("##lcd_brightness", &emu.lcdFlashBrightness, 1.0f, 8.0f)) {
				casioemu::ordinary_lcd_history::UntrackedChange change;
				screen_flashing_brightness_coeff = emu.lcdFlashBrightness;
			}
			if (ImGui::IsItemDeactivatedAfterEdit())
				SaveNow();
			ToolUi::Field("Screen buffer");
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			if (ImGui::SliderInt("##lcd_buffer", &emu.lcdBuffer, 0, 2)) {
				casioemu::ordinary_lcd_history::UntrackedChange change;
				screen_buffer_select = emu.lcdBuffer;
			}
			if (ImGui::IsItemDeactivatedAfterEdit())
				SaveNow();
			ImGui::EndDisabled();
			if (!lcd_options)
				ToolUi::Hint("Ordinary LCD models only.");
		}

		ToolUi::Section("Screenshots and recording");
		{
			ToolUi::PanelScope panel("##settings_capture");
			ToolUi::Field("Capture scale");
			ImGui::SetNextItemWidth(std::max(140.0f, ImGui::GetContentRegionAvail().x * 0.3f));
			if (ImGui::SliderInt("##capture_scale", &emu.captureScale, 1, 16, "%d x")) {
				emu.captureScale = std::clamp(emu.captureScale, 1, 16);
				if (m_emu) m_emu->capture_scale.store(emu.captureScale);
			}
			if (ImGui::IsItemDeactivatedAfterEdit())
				SaveNow();

			ToolUi::Field("Background");
			uint32_t rgb = static_cast<uint32_t>(emu.captureBackgroundRgb) & 0xFFFFFFu;
			float color[3] = {
				static_cast<float>((rgb >> 16) & 0xff) / 255.0f,
				static_cast<float>((rgb >> 8) & 0xff) / 255.0f,
				static_cast<float>(rgb & 0xff) / 255.0f };
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			if (ImGui::ColorEdit3("##capture_bg", color)) {
				const auto channel = [](float value) {
					return static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(value * 255.0f)), 0, 255));
				};
				emu.captureBackgroundRgb = static_cast<int>((channel(color[0]) << 16) | (channel(color[1]) << 8) | channel(color[2]));
				if (m_emu) m_emu->capture_background_rgb.store(static_cast<uint32_t>(emu.captureBackgroundRgb));
			}
			if (ImGui::IsItemDeactivatedAfterEdit())
				SaveNow();

			ImGui::Spacing();
#ifndef CASIOEMU_CORE_WEB
			if (!m_emu) ImGui::BeginDisabled();
			if (ToolUi::AccentButton("Take Screenshot")) {
				if (m_emu) m_emu->screenshot_requested.store(true);
			}
			ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Start Recording").x + 20.0f);
			if (!m_emu) ImGui::EndDisabled();
			const bool recording = m_emu && m_emu->recording_active.load();
			if (!m_emu) ImGui::BeginDisabled();
			if (ImGui::Button(recording ? "Stop Recording" : "Start Recording")) {
				if (m_emu) {
					if (recording)
						m_emu->recording_stop_requested.store(true);
					else
						m_emu->recording_requested.store(true);
				}
			}
			if (!m_emu) ImGui::EndDisabled();
			if (recording) {
				ImGui::SameLine();
				ToolUi::Status("Recording", UIHelpers::kColorWarning);
			}
#endif
		}
		(void)tm;
	}

	// ------------------------------------------------------------- calculator
	void RenderCalculator(ThemeManager& tm, ThemeSettings& settings,
		casioemu::settings::SettingsData& data) {
		ToolUi::Section("Model");
		ToolUi::Description("Model and ROM of the active session.");
		ImGui::Spacing();
		{
			ToolUi::PanelScope panel("##settings_model");
			if (!m_emu) {
				ToolUi::Hint("No calculator is running.");
			}
			else {
				const auto& info = m_emu->ModelDefinition;
				ImGui::Text("Model: %s", info.model_name.empty() ? "(unknown)" : info.model_name.c_str());
				ImGui::Text("Hardware: %s", casioemu::HardwareDisplayName(m_emu->hardware_id));
				ImGui::TextWrapped("ROM: %s", info.rom_path.empty() ? "(built in)" : info.rom_path.c_str());
				ImGui::TextWrapped("Model folder: %s", m_emu->model_path.c_str());
				ImGui::Spacing();
				if (ImGui::Button("Open Model..."))
					casioemu::shell::RequestOpenModel();
				ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Reveal Folder").x + 20.0f);
				if (ImGui::Button("Reveal Folder"))
					OpenInFinder(m_emu->model_path);
			}
		}

		ToolUi::Section("Save state and RAM");
		{
			ToolUi::PanelScope panel("##settings_save");
			if (!m_emu) {
				ToolUi::Hint("No calculator is running.");
			}
			else {
				const size_t ram_size = casioemu::GetRamSize(m_emu->hardware_id);
				ImGui::Text("RAM window: %zu bytes, base 0x%zX", ram_size,
					casioemu::GetRamBaseAddr(m_emu->hardware_id));
				ImGui::TextDisabled("The battery-backed RAM image is written to the model folder on exit.");
				ImGui::Spacing();
				if (ToolUi::AccentButton("Save Snapshot Now")) {
					auto* snapshot = dynamic_cast<SnapshotWindow*>(FindTool("Snapshots"));
					if (snapshot && m_emu) {
						try {
							snapshot->DebugEnsureLoaded();
							snapshot->DebugManager().SaveSnapshot(*m_emu, 0, "Settings state");
							Notify("Snapshot saved.");
						}
						catch (const std::exception& error) {
							Notify(error.what());
						}
					}
				}
				ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Reboot").x + 20.0f);
				if (ImGui::Button("Reboot"))
					m_emu->ResetToMainMenu();
			}
		}

		ToolUi::Section("Calculator panel");
		{
			ToolUi::PanelScope panel("##settings_calc_panel");
			bool calculator_visible = casioemu::shell::CalculatorVisible();
			if (ImGui::Checkbox("Show calculator panel", &calculator_visible))
				casioemu::shell::SetCalculatorVisible(calculator_visible);
			ToolUi::Field("Panel width");
			int calculator_width = data.shell.calculator.width;
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			if (ImGui::SliderInt("##calculator_width", &calculator_width, 260, 700, "%d px")) {
				data.shell.calculator.width = calculator_width;
				casioemu::shell::SetCalculatorWidth(calculator_width);
			}
			if (ImGui::IsItemDeactivatedAfterEdit())
				SaveNow();

			ToolUi::Field("Face zoom");
			float zoom_percent = data.calculator.zoom <= 0.0f ? 0.0f : data.calculator.zoom * 100.0f;
			bool zoom_fit = data.calculator.zoom <= 0.0f;
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			if (ImGui::SliderFloat("##calculator_zoom", &zoom_percent, 0.0f, 400.0f,
					zoom_fit ? "Fit" : "%.0f%%")) {
				data.calculator.zoom = zoom_percent < 25.0f ? 0.0f : std::clamp(zoom_percent / 100.0f, 0.25f, 4.0f);
			}
			if (ImGui::IsItemDeactivatedAfterEdit())
				SaveNow();
			ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Fit").x + 20.0f);
			if (ImGui::Button("Fit")) {
				data.calculator.zoom = 0.0f;
				SaveNow();
			}
			ToolUi::Hint("Nearest-neighbour zoom.");
		}
		(void)tm;
		(void)settings;
	}

	// ----------------------------------------------------- paths and injections
	void RenderPaths(ThemeManager& tm, ThemeSettings& settings,
		casioemu::settings::SettingsData& data) {
		ToolUi::Section("Injection file");
		ToolUi::Description("Payloads are read from this file.");
		ImGui::Spacing();
		{
			ToolUi::PanelScope panel("##settings_paths");
			ToolUi::Field("File");
			ImGui::SetNextItemWidth(std::max(180.0f, ImGui::GetContentRegionAvail().x - 90.0f));
			ImGui::InputText("##injection_path", tempInjectionFilePath, sizeof(tempInjectionFilePath));
			ImGui::SameLine();
#ifndef CASIOEMU_CORE_WEB
			if (ImGui::Button("Browse"))
				showFileDialog = true;
			if (showFileDialog) {
				if (FileDialog::ShowFileOpenDialog("Select Injection File", "Text Files (*.txt){.txt},All Files (*.*){.*}",
						tempInjectionFilePath, sizeof(tempInjectionFilePath))) {
					showFileDialog = false;
				}
			}
#else
			if (ImGui::Button("Browse"))
				WebDebuggerQueueOpenFile(tempInjectionFilePath, "hc-inj.txt");
#endif
			if (ImGui::Button("Save Path")) {
				strncpy(settings.injectionFilePath, tempInjectionFilePath, sizeof(settings.injectionFilePath));
				settings.injectionFilePath[sizeof(settings.injectionFilePath) - 1] = '\0';
				data.calculator.injectionFilePath = tempInjectionFilePath;
				SaveNow();
				tm.RequestFontRebuild();
				Notify("Injection path saved.");
			}
			ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Reveal").x + 20.0f);
			if (ImGui::Button("Reveal"))
				OpenInFinder(std::filesystem::path(tempInjectionFilePath).parent_path());

			bool auto_reload = data.calculator.injectionAutoReload;
			if (ImGui::Checkbox("Reload automatically when the file changes", &auto_reload)) {
				data.calculator.injectionAutoReload = auto_reload;
				SaveNow();
			}
		}

		ToolUi::Section("Configuration folder");
		{
			ToolUi::PanelScope panel("##settings_config");
			auto& store = casioemu::settings::SettingsStore::Instance();
			ImGui::TextWrapped("Settings file: %s", store.FilePath().string().c_str());
			ImGui::TextWrapped("Portable mode: %s", casioemu::settings::SettingsStore::PortableModeEnabled() ? "on" : "off");
			ImGui::Spacing();
			if (ImGui::Button("Open Config Folder"))
				OpenInFinder(store.FilePath().parent_path());
			ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Reveal File").x + 20.0f);
			if (ImGui::Button("Reveal File"))
				OpenInFinder(store.FilePath());
			ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Save Settings").x + 20.0f);
			if (ImGui::Button("Save Settings")) {
				SaveNow();
				Notify("Settings saved.");
			}
		}

		ToolUi::Section("Workspace");
		{
			ToolUi::PanelScope panel("##settings_paths_workspace");
			ImGui::TextWrapped("%s", data.workspace.folder.empty() ? "(no workspace folder)" : data.workspace.folder.c_str());
			ImGui::Spacing();
			if (ImGui::Button("Change Workspace..."))
				casioemu::shell::RequestOpenFolder();
			ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Reveal").x + 20.0f);
			if (ImGui::Button("Reveal"))
				OpenInFinder(data.workspace.folder);
		}
	}

	// ---------------------------------------------------------------- plugins
	void RenderPlugins(ThemeManager& tm, casioemu::settings::SettingsData& data) {
		ToolUi::Section("Theme plugin");
		{
			ToolUi::PanelScope panel("##settings_plugin_theme");
			if (tm.IsPluginThemeActive())
				ToolUi::Status("A theme plugin currently owns the ImGui style.", UIHelpers::kColorSuccess);
			else
				ToolUi::Hint("No plugin theme active.");
			ImGui::Text("Active preset: %s", tm.CurrentThemeName().empty() ? "(none)" : tm.CurrentThemeName().c_str());
		}

		ToolUi::Section("Registry");
		{
			ToolUi::PanelScope panel("##settings_plugin_registry");
			ImGui::Text("Enabled plugins: %d", static_cast<int>(data.plugins.enabled.size()));
			ImGui::Text("Disabled plugins: %d", static_cast<int>(data.plugins.disabled.size()));
			ImGui::TextWrapped("Trust store: %s", data.plugins.trustPath.c_str());
			ImGui::Spacing();
			if (ImGui::Button("Install Plugin..."))
				casioemu::shell::RequestInstallPlugin();
			ToolUi::Hint("Open the Plugin Manager from a session tab.");
		}

		ToolUi::Section("Safety");
		ToolUi::Hint("Native plugins run in-process; review permissions.");
	}

	// ----------------------------------------------------------- integrations
	void RenderIntegrations(casioemu::settings::SettingsData& data) {
		ToolUi::Section("Discord");
		{
			ToolUi::PanelScope panel("##settings_discord");
			if (ImGui::Checkbox("Show activity in Discord", &data.integrations.discordRPC)) {
				ThemeManager::Instance().Settings().enableDiscordRPC = data.integrations.discordRPC;
				SaveNow();
#ifndef CASIOEMU_CORE_WEB
				if (m_emu)
					DiscordRPC::UpdatePresence(m_emu->ModelDefinition.model_name);
				else
					DiscordRPC::UpdatePresence("");
#endif
			}
			ToolUi::Hint("Show the active model in Discord.");
		}

		ToolUi::Section("Updates");
		{
			ToolUi::PanelScope panel("##settings_updates");
			if (ImGui::Checkbox("Check for updates on launch", &data.integrations.updateChecker)) {
				ThemeManager::Instance().Settings().enableUpdateChecker = data.integrations.updateChecker;
				SaveNow();
			}
			ToolUi::Hint("Opens the release page in your browser.");
		}
	}

	// --------------------------------------------------------------- language
	void RenderLanguage(ThemeManager& tm, ThemeSettings& settings,
		casioemu::settings::SettingsData& data) {
		static const struct {
			const char* code;
			const char* badge;
			const char* displayNameKey;
		} availableLanguages[] = {
			{"en_US", "EN", "Localization.Lang.en_US"},
			{"zh_CN", "ZH", "Localization.Lang.zh_CN"},
			{"ja_JP", "JA", "Localization.Lang.ja_JP"},
			{"vi_VN", "VI", "Localization.Lang.vi_VN"},
			{"ru_RU", "RU", "Localization.Lang.ru_RU"},
			{"ko_KR", "KO", "Localization.Lang.ko_KR"},
			{"fr_FR", "FR", "Localization.Lang.fr_FR"},
			{"de_DE", "DE", "Localization.Lang.de_DE"},
			{"es_ES", "ES", "Localization.Lang.es_ES"}
		};

		ToolUi::Section("Language");
		ToolUi::Description("Missing translations fall back to English.");
		ImGui::Spacing();

		if (strlen(settings.language) == 0)
			strncpy(settings.language, g_local.GetCurrentLanguage().c_str(), sizeof(settings.language));

		std::string currentLangStr(settings.language);
		std::string currentDisplayName = "Localization.LanguageName"_l;
		std::string currentBadge = "??";
		for (const auto& lang : availableLanguages) {
			if (currentLangStr == lang.code) {
				currentBadge = lang.badge;
				currentDisplayName = g_local.Get(lang.displayNameKey);
				break;
			}
		}
		const std::string currentLabel = std::string("[") + currentBadge + "] " + currentDisplayName;
		ToolUi::Field("Language");
		ImGui::SetNextItemWidth(std::min(320.0f, ImGui::GetContentRegionAvail().x));
		if (ImGui::BeginCombo("##language_combo", currentLabel.c_str())) {
			for (const auto& lang : availableLanguages) {
				const std::string dispName = g_local.Get(lang.displayNameKey);
				const std::string itemLabel = std::string("[") + lang.badge + "] " + dispName;
				const bool isSelected = (currentLangStr == lang.code);
				if (ImGui::Selectable(itemLabel.c_str(), isSelected)) {
					strncpy(settings.language, lang.code, sizeof(settings.language));
					settings.language[sizeof(settings.language) - 1] = '\0';
					data.language = lang.code;
					g_local.ChangeLanguage(settings.language);
					tm.RequestFontRebuild();
					SaveNow();
				}
				if (isSelected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		if (ImGui::Button("Reset Language Files")) {
			if (strlen(settings.language) > 0) {
				const std::string langFile = "./locales/" + std::string(settings.language) + ".lc";
				std::error_code ec;
				if (std::filesystem::exists(langFile, ec))
					std::filesystem::remove(langFile, ec);
				g_local.ChangeLanguage(settings.language);
				tm.RequestFontRebuild();
				Notify("Language files reset.");
			}
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Reload the built-in translation.");
		ToolUi::Hint("Plugin strings may be English-only.");
	}

	// ------------------------------------------------------------ window/layout
	void RenderWindowLayout(ThemeManager& tm, ThemeSettings& settings,
		casioemu::settings::SettingsData& data) {
		auto& shell = data.shell;
		ToolUi::Section("Panels");
		ToolUi::Description("Some sizes apply on next launch.");
		ImGui::Spacing();
		{
			ToolUi::PanelScope panel("##settings_panels");
			bool sidebar_visible = shell.sidebar.visible;
			if (ImGui::Checkbox("Explorer sidebar", &sidebar_visible)) {
				if (sidebar_visible != shell.sidebar.visible)
					casioemu::shell::ToggleSidebar();
				shell.sidebar.visible = sidebar_visible;
				SaveNow();
			}
			ToolUi::Field("Sidebar width");
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			if (ImGui::SliderInt("##sidebar_width", &shell.sidebar.width, 200, 600, "%d px"))
				shell.sidebar.width = std::clamp(shell.sidebar.width, 200, 600);
			if (ImGui::IsItemDeactivatedAfterEdit())
				SaveNow();

			bool bottom_visible = shell.bottom.visible;
			if (ImGui::Checkbox("Bottom panel", &bottom_visible)) {
				if (bottom_visible != shell.bottom.visible)
					casioemu::shell::ToggleBottomPanel();
				shell.bottom.visible = bottom_visible;
				SaveNow();
			}
			ToolUi::Field("Panel height");
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			if (ImGui::SliderInt("##panel_height", &shell.bottom.height, 120, 600, "%d px"))
				shell.bottom.height = std::clamp(shell.bottom.height, 120, 600);
			if (ImGui::IsItemDeactivatedAfterEdit())
				SaveNow();

			ToolUi::Field("Panel tab");
			ImGui::SetNextItemWidth(std::min(220.0f, ImGui::GetContentRegionAvail().x));
			if (ImGui::BeginCombo("##panel_tab", shell.bottom.active.c_str())) {
				for (const char* tab : { "output", "problems", "terminal" }) {
					if (ImGui::Selectable(tab, shell.bottom.active == tab)) {
						shell.bottom.active = tab;
						SaveNow();
					}
				}
				ImGui::EndCombo();
			}

			if (ImGui::Checkbox("Tool dock (next launch)", &shell.tools.visible))
				SaveNow();
			ToolUi::Field("Tool dock width");
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			if (ImGui::SliderInt("##tools_width", &shell.tools.width, 200, 800, "%d px"))
				shell.tools.width = std::clamp(shell.tools.width, 200, 800);
			if (ImGui::IsItemDeactivatedAfterEdit())
				SaveNow();

			if (ImGui::Checkbox("Calculator panel", &shell.calculator.visible))
				casioemu::shell::SetCalculatorVisible(shell.calculator.visible);
			ToolUi::Hint("Widths apply on next launch.");
			ToolUi::Field("Calculator width");
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			if (ImGui::SliderInt("##calc_width", &shell.calculator.width, 260, 700, "%d px")) {
				casioemu::shell::SetCalculatorWidth(shell.calculator.width);
			}
			if (ImGui::IsItemDeactivatedAfterEdit())
				SaveNow();
		}

		ToolUi::Section("Window");
		{
			ToolUi::PanelScope panel("##settings_window");
			int w = 0, h = 0;
			if (window)
				SDL_GetWindowSize(window, &w, &h);
			ImGui::Text("Window: %d x %d", w, h);
			ImGui::Text("Saved size: %d x %d at (%d, %d)", shell.window.width, shell.window.height,
				shell.window.x, shell.window.y);
			ImGui::Spacing();
			bool fullscreen = (window && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
			if (ImGui::Checkbox("Full screen", &fullscreen) && window) {
				SDL_SetWindowFullscreen(window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
				shell.window.fullscreen = fullscreen;
				SaveNow();
			}
			if (ImGui::Button("Minimise") && window)
				SDL_MinimizeWindow(window);
			ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Reset Layout").x + 20.0f);
			if (ImGui::Button("Reset Layout")) {
				casioemu::shell::SetCalculatorWidth(360);
				shell.calculator.width = 360;
				SaveNow();
				Notify("Layout reset. Restart to restore the default panel sizes.");
			}
		}

		ToolUi::Section("Tool documents");
		ToolUi::Description("Hidden tools stay listed.");
		{
			ToolUi::PanelScope panel("##settings_tabs");
			bool tabVisibilityChanged = false;
			bool first_tab = true;
			for (auto* win : windows) {
				if (!win || !win->name) continue;
				const bool isSettings = std::strcmp(win->name, "Settings") == 0;
				if (isSettings) {
					bool alwaysVisible = true;
					ImGui::BeginDisabled();
					ToolUi::CheckboxFlow("Settings (always available)", &alwaysVisible, first_tab);
					ImGui::EndDisabled();
					first_tab = false;
					continue;
				}
				if (ToolUi::CheckboxFlow(win->name, &win->open, first_tab))
					tabVisibilityChanged = true;
				first_tab = false;
			}
			const std::string hiddenTabs = HiddenTabsValue();
			if (tabVisibilityChanged || hiddenTabs != settings.hiddenTabs) {
				strncpy(settings.hiddenTabs, hiddenTabs.c_str(), sizeof(settings.hiddenTabs));
				settings.hiddenTabs[sizeof(settings.hiddenTabs) - 1] = '\0';
				SaveNow();
			}
		}
		(void)tm;
	}

	// ---------------------------------------------------------------- advanced
	void RenderAdvanced(ThemeManager& tm, ThemeSettings& settings,
		casioemu::settings::SettingsData& data) {
		ToolUi::Section("Storage");
		{
			ToolUi::PanelScope panel("##settings_storage");
			auto& store = casioemu::settings::SettingsStore::Instance();
			ImGui::TextWrapped("File: %s", store.FilePath().string().c_str());
			ImGui::Text("Schema: %d", data.schema);
			ImGui::Text("Last load: %s", store.LastLoadStatus() == casioemu::settings::LoadStatus::Loaded ? "loaded"
				: store.LastLoadStatus() == casioemu::settings::LoadStatus::Missing ? "defaults (no file)" : "error (defaults)");
			if (!store.LastError().empty())
				ToolUi::Status(store.LastError().c_str(), UIHelpers::kColorWarning);
			ImGui::Spacing();
			if (ImGui::Button("Save Now")) {
				SaveNow();
				Notify("Settings saved.");
			}
			ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Reload From Disk").x + 20.0f);
			if (ImGui::Button("Reload From Disk")) {
				tm.LoadSettings();
				tabsApplied = false;
				Notify("Settings reloaded.");
			}
			ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Open Config Folder").x + 20.0f);
			if (ImGui::Button("Open Config Folder"))
				OpenInFinder(store.FilePath().parent_path());
		}

		ToolUi::Section("Renderer");
		{
			ToolUi::PanelScope panel("##settings_renderer");
			static const char* renderers[] = { "default", "software", "metal", "opengl" };
			int current = 0;
			for (int i = 0; i < IM_ARRAYSIZE(renderers); ++i)
				if (data.advanced.renderer == renderers[i])
					current = i;
			ToolUi::Field("Backend");
			ImGui::SetNextItemWidth(std::min(220.0f, ImGui::GetContentRegionAvail().x));
			if (ImGui::BeginCombo("##renderer", renderers[current])) {
				for (int i = 0; i < IM_ARRAYSIZE(renderers); ++i) {
					if (ImGui::Selectable(renderers[i], i == current)) {
						data.advanced.renderer = renderers[i];
						// casioemu.cpp reads .renderer_hint.cfg before creating the
						// renderer; persist the hint for the next launch.
						std::ofstream hint(".renderer_hint.cfg", std::ios::trunc);
						if (hint)
							hint << data.advanced.renderer;
						SaveNow();
						Notify("Renderer saved. Restart to apply.");
					}
				}
				ImGui::EndCombo();
			}
			ToolUi::Hint("Applies on next launch.");
		}

		ToolUi::Section("Reset");
		{
			ToolUi::PanelScope panel("##settings_reset");
			ImGui::TextWrapped("Restore all defaults. Documents and snapshots are kept.");
			ImGui::Spacing();
			if (ImGui::Button("Reset All Settings"))
				confirmReset = true;
			if (!confirmReset)
				ToolUi::Hint("This cannot be undone.");
		}

		if (confirmReset)
			ImGui::OpenPopup("Reset settings###settings_reset_confirm");
		{
			ToolUi::DialogScope dialog("Reset settings###settings_reset_confirm", "Reset all settings", 460.0f);
			if (dialog) {
				ImGui::TextUnformatted("Reset every setting to its default value?");
				ImGui::Spacing();
				if (ToolUi::AccentButton("Reset", ImVec2(120, 0))) {
					auto& store = casioemu::settings::SettingsStore::Instance();
					store.ResetToDefaults();
					std::string error;
					store.Save(&error);
					settings = ThemeSettings{};
					tm.LoadSettings();
					tm.RequestFontRebuild();
					tabsApplied = false;
					confirmReset = false;
					Notify("All settings reset to defaults.");
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel", ImVec2(120, 0)) || dialog.CloseRequested()) {
					confirmReset = false;
					ImGui::CloseCurrentPopup();
				}
			}
		}
		(void)data;
	}

	// -------------------------------------------------------- documentation
	static std::string DocTitle(const std::filesystem::path& path) {
		std::ifstream input(path, std::ios::binary);
		if (input) {
			std::string line;
			for (int count = 0; count < 200 && std::getline(input, line); ++count) {
				size_t level = 0;
				while (level < line.size() && line[level] == '#')
					++level;
				if (level == 0 || level > 6)
					continue;
				std::string title = line.substr(level);
				const auto first = title.find_first_not_of(" \t\r");
				if (first == std::string::npos)
					continue;
				const auto last = title.find_last_not_of(" \t\r");
				title = title.substr(first, last - first + 1);
				std::string plain;
				for (size_t index = 0; index < title.size();) {
					const char ch = title[index];
					if (ch == '[') {
						const auto close = title.find("](", index + 1);
						const auto end = close == std::string::npos ? std::string::npos : title.find(')', close + 2);
						if (close != std::string::npos && end != std::string::npos) {
							plain += title.substr(index + 1, close - index - 1);
							index = end + 1;
							continue;
						}
					}
					if (ch == '`' || ch == '*' || ch == '_') {
						++index;
						continue;
					}
					plain.push_back(ch);
					++index;
				}
				return plain;
			}
		}
		return path.stem().string();
	}

	static std::string DocModified(const std::filesystem::path& path) {
		std::error_code ec;
		const auto stamp = std::filesystem::last_write_time(path, ec);
		if (ec)
			return {};
		const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
			stamp - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
		const std::time_t time = std::chrono::system_clock::to_time_t(system);
		std::tm local{};
#ifdef _WIN32
		localtime_s(&local, &time);
#else
		localtime_r(&time, &local);
#endif
		char buffer[32];
		std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
		return buffer;
	}

	void AddDoc(const std::filesystem::path& path, const char* category) {
		std::error_code ec;
		if (!std::filesystem::is_regular_file(path, ec))
			return;
		const auto canonical = std::filesystem::weakly_canonical(path, ec);
		const std::filesystem::path key = ec ? path : canonical;
		for (const auto& doc : docs)
			if (doc.path == key)
				return;
		if (docs.size() >= 600)
			return;
		DocEntry entry;
		entry.title = DocTitle(key);
		for (unsigned char ch : entry.title) {
			if (ch >= 0x80) {
				entry.title = key.stem().string();
				break;
			}
		}
		entry.category = category;
		entry.modified = DocModified(key);
		entry.path = key;
		entry.rel = key.filename().string();
		docs.push_back(std::move(entry));
	}

	void AddDocTree(const std::filesystem::path& root, const char* category, bool recursive) {
		std::error_code ec;
		if (!std::filesystem::is_directory(root, ec))
			return;
		const auto wanted = [](const std::filesystem::path& path) {
			std::string ext = path.extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
				return static_cast<char>(std::tolower(ch));
			});
			return ext == ".md" || ext == ".markdown" || ext == ".txt";
		};
		if (recursive) {
			for (auto it = std::filesystem::recursive_directory_iterator(root,
					std::filesystem::directory_options::skip_permission_denied, ec);
				it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
				if (ec) {
					ec.clear();
					continue;
				}
				std::error_code file_error;
				if (it->is_regular_file(file_error) && wanted(it->path()))
					AddDoc(it->path(), category);
			}
		}
		else {
			for (const auto& entry : std::filesystem::directory_iterator(root,
					std::filesystem::directory_options::skip_permission_denied, ec)) {
				if (ec)
					break;
				std::error_code file_error;
				if (entry.is_regular_file(file_error) && wanted(entry.path()))
					AddDoc(entry.path(), category);
			}
		}
	}

	static std::string ManifestDate(const std::string& stamp) {
		// ISO 8601 "YYYY-MM-DDTHH:MM:SSZ" -> "YYYY-MM-DD HH:MM".
		if (stamp.size() < 16)
			return stamp;
		std::string text = stamp.substr(0, 16);
		if (text.size() > 10 && text[10] == 'T')
			text[10] = ' ';
		return text;
	}

	void ScanManifest(const std::filesystem::path& root) {
		std::ifstream input(root / "manifest.tsv");
		if (!input)
			return;
		std::string line;
		bool header = true;
		while (std::getline(input, line)) {
			if (header) {
				header = false;
				if (line.rfind("path\t", 0) == 0)
					continue;
			}
			std::vector<std::string> fields;
			std::string field;
			for (char ch : line) {
				if (ch == '\t') {
					fields.push_back(field);
					field.clear();
				}
				else if (ch != '\r') {
					field.push_back(ch);
				}
			}
			fields.push_back(field);
			if (fields.size() < 2 || fields[0].empty())
				continue;
			const std::filesystem::path relative = fields[0];
			const std::filesystem::path full = root / relative;
			std::error_code ec;
			if (!std::filesystem::is_regular_file(full, ec))
				continue;
			DocEntry entry;
			entry.path = std::filesystem::weakly_canonical(full, ec);
			entry.rel = fields[0];
			const std::string category_path = relative.generic_string();
			if (category_path.rfind("wiz/", 0) == 0)
				entry.category = "Wiz";
			else if (category_path == "PRIVACY.md")
				entry.category = "Project";
			else
				entry.category = "Emulator";
			entry.title = DocTitle(entry.path);
			std::string manifest_title = casioemu::markdown::detail::Trim(fields[1]);
			if (!manifest_title.empty())
				entry.title = manifest_title;
			entry.modified = fields.size() > 3 ? ManifestDate(fields[3]) : DocModified(entry.path);
			docs.push_back(std::move(entry));
		}
	}

	void ScanDocuments() {
		docs.clear();
		const auto exe = casioemu::settings::SettingsStore::ExecutableDir();
		std::error_code ec;
		const auto bundle_docs = exe / ".." / "Resources" / "docs";
		bool manifest_found = std::filesystem::is_regular_file(bundle_docs / "manifest.tsv", ec);
		if (manifest_found)
			ScanManifest(bundle_docs);
		if (!manifest_found) {
			for (const auto& [root, category] : {
					std::pair{ bundle_docs / "emulator", "Emulator" },
					std::pair{ bundle_docs / "project", "Project" },
					std::pair{ bundle_docs / "wiz", "Wiz" } })
				AddDocTree(root, category, true);
			AddDoc(bundle_docs / "project" / "PRIVACY.md", "Project");
		}
		std::sort(docs.begin(), docs.end(), [](const DocEntry& lhs, const DocEntry& rhs) {
			if (lhs.category != rhs.category)
				return lhs.category < rhs.category;
			return lhs.title < rhs.title;
		});
		docsScanned = true;
		docSelected = -1;
		docBody.clear();
		docLoadedPath.clear();
		if (const char* search = std::getenv("CASIOEMU_SETTINGS_DOC_SEARCH"))
			std::snprintf(docSearch, sizeof(docSearch), "%s", search);
		if (const char* wanted = std::getenv("CASIOEMU_SETTINGS_DOC_OPEN")) {
			std::string needle = wanted;
			std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char ch) {
				return static_cast<char>(std::tolower(ch));
			});
			for (int index = 0; index < static_cast<int>(docs.size()); ++index) {
				std::string haystack = docs[static_cast<size_t>(index)].title + " " +
					docs[static_cast<size_t>(index)].path.string();
				std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char ch) {
					return static_cast<char>(std::tolower(ch));
				});
				if (haystack.find(needle) != std::string::npos) {
					SelectDocument(index);
					break;
				}
			}
		}
		if (const char* scroll = std::getenv("CASIOEMU_SETTINGS_DOC_SCROLL")) {
			docPendingScroll = static_cast<float>(std::atof(scroll));
			docScrollApply = 4;
		}
	}

	void SelectDocument(int index) {
		if (index == docSelected)
			return;
		if (auto* old = DocAt(docSelected))
			docScroll[old->path.string()] = docCurrentScroll;
		docSelected = index;
		docBody.clear();
		docLoadedPath.clear();
		docPendingScroll = 0.0f;
		docScrollApply = 0;
		if (auto* doc = DocAt(index)) {
			std::ifstream input(doc->path, std::ios::binary);
			std::stringstream buffer;
			buffer << input.rdbuf();
			docBody = buffer.str();
			// The pane already shows the title; drop a leading H1 that repeats it.
			if (!docBody.empty() && docBody[0] == '#') {
				const auto newline = docBody.find('\n');
				const std::string first = docBody.substr(0, newline == std::string::npos ? docBody.size() : newline);
				if (first.rfind("# ", 0) == 0)
					docBody.erase(0, newline == std::string::npos ? docBody.size() : newline + 1);
			}
			docLoadedPath = doc->path.string();
			const auto found = docScroll.find(docLoadedPath);
			docPendingScroll = found != docScroll.end() ? found->second : 0.0f;
			docScrollApply = 4;
		}
	}

	void RenderDocList(float width, float height) {
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ToolUi::WithAlpha(ToolUi::Palette().surfaceAlt, 0.7f));
		ImGui::PushStyleColor(ImGuiCol_Border, ToolUi::Palette().border);
		ImGui::BeginChild("##doc_list", ImVec2(width, height),
			ImGuiChildFlags_Border | ImGuiChildFlags_AlwaysUseWindowPadding);

		std::string filter = docSearch;
		std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char ch) {
			return static_cast<char>(std::tolower(ch));
		});
		int visible_count = 0;
		for (int index = 0; index < static_cast<int>(docs.size()); ++index) {
			const auto& doc = docs[static_cast<size_t>(index)];
			if (!filter.empty()) {
				std::string haystack = doc.title + " " + doc.category + " " + doc.path.string();
				std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char ch) {
					return static_cast<char>(std::tolower(ch));
				});
				if (haystack.find(filter) == std::string::npos)
					continue;
			}
			++visible_count;
			const bool selected = index == docSelected;
			ImGui::PushID(index);
			const float row_width = ImGui::GetContentRegionAvail().x;
			const ImVec2 top = ImGui::GetCursorScreenPos();
			const float wrap = std::max(40.0f, row_width - 12.0f);
			const ImVec2 title_size = ImGui::CalcTextSize(doc.title.c_str(), nullptr, false, wrap);
			const ImVec2 rel_size = ImGui::CalcTextSize(doc.rel.c_str(), nullptr, false, wrap);
			const float row_height = title_size.y + rel_size.y + ImGui::GetTextLineHeightWithSpacing() + 10.0f;
			ImGui::InvisibleButton("##doc_row", ImVec2(row_width, row_height));
			const bool hovered = ImGui::IsItemHovered();
			if (ImGui::IsItemClicked())
				SelectDocument(index);
			ImDrawList* draw = ImGui::GetWindowDrawList();
			draw->AddRectFilled(top, ImVec2(top.x + row_width, top.y + row_height),
				ImGui::ColorConvertFloat4ToU32(selected ? ToolUi::WithAlpha(ToolUi::Palette().selection, 0.65f)
					: (hovered ? ToolUi::Palette().hover : ImVec4(0, 0, 0, 0))), 5.0f);
			ImGui::SetCursorScreenPos(ImVec2(top.x + 6.0f, top.y + 4.0f));
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
			ImGui::TextUnformatted(doc.title.c_str());
			ImGui::PopTextWrapPos();
			ImGui::SetCursorScreenPos(ImVec2(top.x + 6.0f, top.y + 4.0f + title_size.y + 1.0f));
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
			ImGui::PushStyleColor(ImGuiCol_Text, ToolUi::Palette().muted);
			ImGui::TextUnformatted(doc.rel.c_str());
			ImGui::PopStyleColor();
			ImGui::PopTextWrapPos();
			ImGui::SetCursorScreenPos(ImVec2(top.x + 6.0f, top.y + 4.0f + title_size.y + rel_size.y + 2.0f));
			char detail[160];
			const std::string date = doc.modified.size() >= 10 ? doc.modified.substr(0, 10) : doc.modified;
			std::snprintf(detail, sizeof(detail), "%s  %s", doc.category.c_str(), date.c_str());
			ImGui::TextDisabled("%s", detail);
			ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + row_height));
			ImGui::PopID();
		}
		if (visible_count == 0)
			ToolUi::EmptyState("No documentation found.");
		ImGui::EndChild();
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar(2);
	}

	void RenderDocumentation() {
		ToolUi::Section("Documentation");
		ToolUi::Description("Browse and search project docs.");
		if (!docsScanned)
			ScanDocuments();

		ToolUi::Field("Search");
		ImGui::SetNextItemWidth(std::min(300.0f, std::max(120.0f, ToolUi::RemainingWidth() - 170.0f)));
		ImGui::InputTextWithHint("##doc_search", "Title or path", docSearch, sizeof(docSearch));
		ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Refresh").x + 20.0f);
		if (ImGui::Button("Refresh")) {
			docs.clear();
			docsScanned = false;
			ScanDocuments();
		}
		ToolUi::SameLineOrWrap(ImGui::CalcTextSize("000 / 000 docs").x + 20.0f);
		{
			std::string filter = docSearch;
			std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char ch) {
				return static_cast<char>(std::tolower(ch));
			});
			int visible = 0;
			for (const auto& doc : docs) {
				std::string haystack = doc.title + " " + doc.category + " " + doc.path.string();
				std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char ch) {
					return static_cast<char>(std::tolower(ch));
				});
				if (filter.empty() || haystack.find(filter) != std::string::npos)
					++visible;
			}
			ImGui::TextDisabled("%d / %d docs", visible, static_cast<int>(docs.size()));
		}

		ImGui::NewLine();
		const float avail = ToolUi::RemainingWidth();
		const bool stacked = avail < 480.0f;
		if (stacked && docSelected < 0) {
			RenderDocList(0.0f, 0.0f);
			return;
		}
		if (stacked) {
			if (ImGui::Button("Back to list")) {
				SelectDocument(-1);
				return;
			}
			ImGui::SameLine();
			ImGui::TextDisabled("%d docs", static_cast<int>(docs.size()));
		}
		else {
			const float list_width = std::min(320.0f, std::max(220.0f, avail * 0.32f));
			RenderDocList(list_width, 0.0f);
			ImGui::SameLine();
		}

		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ToolUi::WithAlpha(ToolUi::Palette().surfaceAlt, 0.35f));
		ImGui::PushStyleColor(ImGuiCol_Border, ToolUi::Palette().border);
		ImGui::BeginChild("##doc_view", ImVec2(0.0f, 0.0f),
			ImGuiChildFlags_Border | ImGuiChildFlags_AlwaysUseWindowPadding);
		if (docScrollApply > 0 && docPendingScroll >= 0.0f) {
			ImGui::SetScrollY(docPendingScroll);
			--docScrollApply;
		}
		if (auto* doc = DocAt(docSelected)) {
			ImGui::PushStyleColor(ImGuiCol_Text, ToolUi::Palette().accent);
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(doc->title.c_str());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
			ImGui::PushStyleColor(ImGuiCol_Text, ToolUi::Palette().muted);
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(doc->path.string().c_str());
			if (!doc->modified.empty())
				ImGui::Text("Modified %s", doc->modified.c_str());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
			ImGui::Separator();
			casioemu::markdown::detail::g_link_valid = false;
			casioemu::markdown::detail::SetBaseDirectory(doc->path.parent_path());
			casioemu::markdown::Render(docBody);
			docCurrentScroll = ImGui::GetScrollY();
		}
		else {
			ToolUi::EmptyState("Select a document.");
		}
		ImGui::EndChild();
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar(2);

		if (std::getenv("CASIOEMU_DOC_LINK_TEST")) {
			static int link_stage = 0;
			ImGuiIO& io = ImGui::GetIO();
			if (link_stage == 0 && docScrollApply == 0 && casioemu::markdown::detail::g_link_valid) {
				io.AddMousePosEvent((casioemu::markdown::detail::g_link_min.x + casioemu::markdown::detail::g_link_max.x) * 0.5f,
					(casioemu::markdown::detail::g_link_min.y + casioemu::markdown::detail::g_link_max.y) * 0.5f);
				io.AddMouseButtonEvent(0, true);
				link_stage = 1;
			}
			else if (link_stage == 1) {
				io.AddMouseButtonEvent(0, false);
				link_stage = 2;
			}
		}

		if (std::getenv("CASIOEMU_SETTINGS_DOC_SCROLLTEST") && !docs.empty()) {
			static int stage = 0;
			static float before = -1.0f;
			static int first = -1;
			if (stage == 0 && docSelected >= 0) {
				first = docSelected;
				docPendingScroll = 180.0f;
				docScrollApply = 4;
				stage = 1;
			}
			else if (stage == 1 && docScrollApply == 0) {
				before = docCurrentScroll;
				SelectDocument((first + 1) % static_cast<int>(docs.size()));
				stage = 2;
			}
			else if (stage == 2 && docScrollApply == 0) {
				SelectDocument(first);
				stage = 3;
			}
			else if (stage == 3 && docScrollApply == 0) {
				const float after = docCurrentScroll;
				std::fprintf(stderr, "[docs] scroll before=%.1f after=%.1f %s\n", before, after,
					std::fabs(before - after) < 2.0f ? "PASS" : "FAIL");
				stage = 4;
			}
		}
	}

public:
	SettingsWindow() : UIWindow("Settings") {
		auto& settings = ThemeManager::Instance().Settings();
		strncpy(tempInjectionFilePath, settings.injectionFilePath, sizeof(tempInjectionFilePath));
		tempInjectionFilePath[sizeof(tempInjectionFilePath) - 1] = '\0';
		// Headless verification: CASIOEMU_SETTINGS_CATEGORY=<n> opens a category
		// directly so each settings page can be screenshotted and probed.
		if (const char* requested = std::getenv("CASIOEMU_SETTINGS_CATEGORY")) {
			category = std::clamp(std::atoi(requested), 0, static_cast<int>(kCategoryCount) - 1);
		}
		// Headless verification: apply a preset for a screenshot.  PREVIEW does
		// not persist; APPLY persists (used with a portable settings folder).
		if (const char* preview = std::getenv("CASIOEMU_PREVIEW_THEME")) {
			std::string error;
			ThemeManager::Instance().PreviewThemeByName(preview, &error);
		}
		if (const char* apply = std::getenv("CASIOEMU_APPLY_THEME")) {
			std::string error;
			ThemeManager::Instance().ApplyThemeByName(apply, &error);
		}
	}

	bool showFileDialog = false;

	void RenderCore() override {
		if (!casioemu::settings::SettingsStore::Instance().IsLoaded())
			ThemeManager::Instance().LoadSettings();

		auto& tm = ThemeManager::Instance();
		auto& settings = tm.Settings();
		auto& data = casioemu::settings::SettingsStore::Instance().Data();
		if (!tabsApplied) {
			ApplySavedTabVisibility(settings);
			tabsApplied = true;
		}

		// Left navigation + right content.  A category list scales better than a
		// tab bar once every settings group exists.  The nav shrinks with the
		// panel so the content column never drops below a usable width.
		const float nav_width = std::min(190.0f, std::max(140.0f, ImGui::GetContentRegionAvail().x * 0.24f));
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
		ImGui::BeginChild("##settings_nav", ImVec2(nav_width, 0), ImGuiChildFlags_Border);
		for (int index = 0; index < kCategoryCount; ++index) {
			const bool selected = category == index;
			ImGui::PushStyleColor(ImGuiCol_Button, selected ? ToolUi::Palette().selection : ImVec4(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ToolUi::Palette().hover);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ToolUi::Palette().selection);
			ImGui::PushStyleColor(ImGuiCol_Text, selected ? ToolUi::Palette().foreground : ToolUi::Palette().muted);
			if (ImGui::Button(CategoryLabel(index), ImVec2(-1.0f, 28.0f)))
				category = index;
			ImGui::PopStyleColor(4);
			if (std::strcmp(CategoryFullName(index), CategoryLabel(index)) != 0 && ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", CategoryFullName(index));
		}
		if (!statusMessage.empty() && ImGui::GetTime() - statusTime < 4.0) {
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::PushTextWrapPos(0.0f);
			ToolUi::Status(statusMessage.c_str(), UIHelpers::kColorSuccess);
			ImGui::PopTextWrapPos();
		}
		ImGui::EndChild();
		ImGui::PopStyleVar(2);

		ImGui::SameLine();
		ImGui::BeginChild("##settings_content", ImVec2(0, 0));
		ToolUi::PageScope page("Settings");
		switch (category) {
		case kGeneral: RenderGeneral(data); break;
		case kAppearance: RenderAppearance(tm, settings, data); break;
		case kEditor: RenderEditor(tm, data); break;
		case kEmulator: RenderEmulator(tm, settings, data); break;
		case kCalculator: RenderCalculator(tm, settings, data); break;
		case kPaths: RenderPaths(tm, settings, data); break;
		case kPlugins: RenderPlugins(tm, data); break;
		case kIntegrations: RenderIntegrations(data); break;
		case kLanguage: RenderLanguage(tm, settings, data); break;
		case kWindow: RenderWindowLayout(tm, settings, data); break;
		case kAdvanced: RenderAdvanced(tm, settings, data); break;
		case kDocumentation: RenderDocumentation(); break;
		default: break;
		}
		// Verification hook: park the settings page at a scroll offset so a
		// screenshot can show rows below the fold.
		if (const char* scroll = std::getenv("CASIOEMU_SETTINGS_SCROLL"))
			ImGui::SetScrollY(std::max(0.0f, static_cast<float>(std::atof(scroll))));
		ImGui::EndChild();
	}
};

UIWindow* MakeSettingsWindow() {
	return new SettingsWindow();
}
