#include "Theme.h"
#include "Peripheral/OrdinaryLcdHistory.hpp"
#include "FileDialog.hpp"
#include "Settings/Settings.h"
#ifndef CASIOEMU_CORE_WEB
#include "SysDialog.h"
#else
#include "WebDebuggerGui.h"
#endif
#include "Ui.hpp"
#include <Gui.h>
#include <Localization.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vibration.h>
#ifndef CASIOEMU_CORE_WEB
#include "Ext/DiscordRPC.h"
#endif

extern SDL_Surface* background;
extern SDL_Texture* bg_txt;

// Settings is organised into five categories that mirror the preview plan:
// Theme / Editor / Emulator / Plugins / System.  The JSON document is the
// source of truth; the theme manager keeps the legacy ThemeSettings fields in
// sync so older call sites keep working unchanged.
class SettingsWindow : public UIWindow {
private:
	bool showFileDialog = false;
	char tempInjectionFilePath[256]{};
	bool tabsApplied = false;
	std::string themeError;

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

	void RenderThemeTab(ThemeManager& tm, ThemeSettings& settings,
		casioemu::settings::SettingsData& data) {
		ImGui::TextWrapped("%s", "Settings.Theme.Description"_lc);
		ImGui::Spacing();

		UIHelpers::SectionHeader("Settings.Theme.Preset"_lc);
		const auto themes = tm.AvailableThemes();
		const std::string current = tm.CurrentThemeName();
		std::string currentLabel = current;
		for (const auto& info : themes) {
			if (info.id == current) {
				currentLabel = info.name + " (" + info.id + ")";
				break;
			}
		}
		if (tm.IsPluginThemeActive())
			ImGui::TextColored(UIHelpers::kColorWarning, "%s", "Settings.Theme.PluginActive"_lc);

		ImGui::SetNextItemWidth(360.0f);
		if (ImGui::BeginCombo("##theme_preset", currentLabel.c_str())) {
			for (const auto& info : themes) {
				const bool selected = info.id == current;
				const std::string label = info.name + "##" + info.id;
				if (ImGui::Selectable(label.c_str(), selected)) {
					std::string error;
					if (!tm.ApplyThemeByName(info.id, &error))
						themeError = error;
					else
						themeError.clear();
				}
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		if (!themeError.empty())
			ImGui::TextColored(UIHelpers::kColorError, "%s", themeError.c_str());

		if (ImGui::Button("Ui.DarkMode"_lc))
			tm.SetDarkMode();
		ImGui::SameLine();
		if (ImGui::Button("Ui.LightMode"_lc))
			tm.SetLightMode();

		ImGui::Spacing();
		float currentFontScale = tm.GetFontScale();
		ImGui::SetNextItemWidth(240.0f);
		if (ImGui::SliderFloat("Ui.Scale"_lc, &currentFontScale, 0.8f, 2.0f, "%.2f")) {
			settings.scale = currentFontScale;
			tm.SetFontScale(currentFontScale);
		}
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			SaveNow();
			tm.RequestFontRebuild();
		}
		ImGui::SameLine();
		if (ImGui::Button("Ui.ApplyScale"_lc)) {
			tm.RequestFontRebuild();
			SaveNow();
		}
		ImGui::TextDisabled("%s: %s / %s", "Settings.Theme.Typography"_lc,
			tm.Typography().uiFamily.c_str(), tm.Typography().monoFamily.c_str());

		UIHelpers::SectionHeader("Auto-Tint (MD3 Monet)");
		if (ImGui::Checkbox("Theme.Tint"_lc, &settings.enableAutoTint))
			SaveNow();
		if (settings.enableAutoTint) {
			ImVec4 seedColor = settings.seedColor;
			if (ImGui::ColorEdit4("Theme.SeedColor"_lc, (float*)&seedColor, ImGuiColorEditFlags_NoAlpha))
				tm.SetSeedColor(seedColor);
			if (ImGui::Button("Theme.MD"_lc)) {
#if !defined(TEST_BUILD) && !defined(CASIOEMU_CORE_WEB)
				seedColor = tm.ExtractDominantColor(bg_txt, renderer);
#endif
				tm.SetSeedColor(seedColor);
			}
		}

		UIHelpers::SectionHeader("Background"_lc);
		if (ImGui::Button("UI.ChangeBg"_lc)) {
#ifdef CASIOEMU_CORE_WEB
			WebDebuggerQueueOpenFile("/persist/background.jpg", "background.jpg");
#else
			SystemDialogs::OpenFileDialog([](std::filesystem::path pth) {
				std::filesystem::copy_file(pth, "background.jpg", std::filesystem::copy_options::overwrite_existing);
			});
			tm.RequestBgReload();
#endif
		}
#ifdef CASIOEMU_CORE_WEB
		int bgResult = 0;
		if (WebDebuggerConsumeFileResult("/persist/background.jpg", &bgResult) && bgResult == 0)
			tm.RequestBgReload();
#endif

		ImGui::Spacing();
		if (ImGui::CollapsingHeader("Settings.Theme.Advanced"_lc)) {
			ImGui::TextWrapped("%s", "Settings.Theme.AdvancedHint"_lc);
			auto& base = settings.isDarkMode ? settings.igs_dark : settings.igs_light;
			ImGuiStyle ims_backup;
			std::memcpy(&ims_backup, &ImGui::GetStyle(), sizeof(base));
			std::memcpy(&ImGui::GetStyle(), &base, sizeof(base));
			ImGui::ShowStyleEditor();
			std::memcpy(&base, &ImGui::GetStyle(), sizeof(base));
			std::memcpy(&ImGui::GetStyle(), &ims_backup, sizeof(base));
		}
	}

	void RenderEditorTab(ThemeManager& tm, casioemu::settings::SettingsData& data) {
		ImGui::TextWrapped("%s", "Settings.Editor.Description"_lc);
		ImGui::Spacing();
		UIHelpers::SectionHeader("Settings.Editor.Text"_lc);
		bool changed = false;
		changed |= ImGui::Checkbox("Settings.Editor.ShowLineNumbers"_lc, &data.editor.showLineNumbers);
		changed |= ImGui::Checkbox("Settings.Editor.WordWrap"_lc, &data.editor.wordWrap);
		ImGui::SetNextItemWidth(200.0f);
		changed |= ImGui::SliderFloat("Settings.Editor.FontSize"_lc, &data.editor.fontSize, 10.0f, 24.0f, "%.0f px");
		ImGui::SetNextItemWidth(200.0f);
		changed |= ImGui::SliderInt("Settings.Editor.TabSize"_lc, &data.editor.tabSize, 2, 8);
		if (changed)
			SaveNow();
		ImGui::Spacing();
		ImGui::TextDisabled("%s", "Settings.Editor.FutureHint"_lc);
	}

	void RenderEmulatorTab(ThemeManager& tm, ThemeSettings& settings,
		casioemu::settings::SettingsData& data) {
		ImGui::TextWrapped("%s", "Settings.Emulator.Description"_lc);
		ImGui::Spacing();

		UIHelpers::SectionHeader("Settings.Emulator.Performance"_lc);
#ifndef __ANDROID__
		bool low_performance = settings.lowPerformanceMode;
		if (ImGui::Checkbox("Ui.LowPerformanceMode"_lc, &low_performance)) {
			casioemu::ordinary_lcd_history::UntrackedChange change;
			settings.lowPerformanceMode = low_performance;
			SaveNow();
		}
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::TextUnformatted("Ui.LowPerformanceTooltip"_lc);
			ImGui::EndTooltip();
		}
#endif
#ifdef __ANDROID__
		ImGui::Checkbox("Ui.DisableVibration"_lc, &setting_DisableVibration);
#endif
		ImGui::SetNextItemWidth(240.0f);
		if (ImGui::SliderFloat("Settings.Emulator.Throttle"_lc, &data.emulator.backgroundThrottle, 0.05f, 1.0f, "%.2f"))
			data.emulator.backgroundThrottle = ImClamp(data.emulator.backgroundThrottle, 0.05f, 1.0f);
		if (ImGui::IsItemDeactivatedAfterEdit())
			SaveNow();
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::TextUnformatted("Settings.Emulator.ThrottleHint"_lc);
			ImGui::EndTooltip();
		}

		UIHelpers::SectionHeader("Ui.InjectionFilePath"_lc);
		ImGui::SetNextItemWidth(360.0f);
		ImGui::InputText("##injection_file_path", tempInjectionFilePath, sizeof(tempInjectionFilePath));
		ImGui::SameLine();
#ifndef CASIOEMU_CORE_WEB
		if (ImGui::Button("Ui.Browse"_lc))
			showFileDialog = true;
		if (showFileDialog) {
			if (FileDialog::ShowFileOpenDialog("Select Injection File", "Text Files (*.txt){.txt},All Files (*.*){.*}",
					tempInjectionFilePath, sizeof(tempInjectionFilePath))) {
				showFileDialog = false;
			}
		}
#else
		if (ImGui::Button("Ui.Browse"_lc))
			WebDebuggerQueueOpenFile(tempInjectionFilePath, "hc-inj.txt");
#endif
		if (ImGui::Button("Files.Save"_lc)) {
			strncpy(settings.injectionFilePath, tempInjectionFilePath, sizeof(settings.injectionFilePath));
			settings.injectionFilePath[sizeof(settings.injectionFilePath) - 1] = '\0';
			SaveNow();
			tm.RequestFontRebuild();
		}
	}

	void RenderPluginsTab(ThemeManager& tm, casioemu::settings::SettingsData& data) {
		ImGui::TextWrapped("%s", "Settings.Plugins.Description"_lc);
		ImGui::Spacing();

		UIHelpers::SectionHeader("Settings.Plugins.Theme"_lc);
		if (tm.IsPluginThemeActive())
			ImGui::TextColored(UIHelpers::kColorSuccess, "%s", "Settings.Plugins.ThemeActive"_lc);
		else
			ImGui::TextDisabled("%s", "Settings.Plugins.ThemeInactive"_lc);
		ImGui::Text("%s: %s", "Settings.Plugins.ActivePreset"_lc,
			tm.CurrentThemeName().empty() ? "(none)" : tm.CurrentThemeName().c_str());

		UIHelpers::SectionHeader("Settings.Plugins.Registry"_lc);
		ImGui::Text("%s: %d", "Settings.Plugins.EnabledCount"_lc, (int)data.plugins.enabled.size());
		ImGui::Text("%s: %d", "Settings.Plugins.DisabledCount"_lc, (int)data.plugins.disabled.size());
		ImGui::TextDisabled("%s: %s", "Settings.Plugins.TrustStore"_lc, data.plugins.trustPath.c_str());

		UIWindow* manager = nullptr;
		for (auto* win : windows) {
			if (win && win->name && std::strcmp(win->name, "Plugin Manager") == 0) {
				manager = win;
				break;
			}
		}
		if (manager) {
			if (ImGui::Button("Settings.Plugins.OpenManager"_lc))
				manager->BringToFront();
		}
		else {
			ImGui::TextDisabled("%s", "Settings.Plugins.ManagerMissing"_lc);
		}
		ImGui::Spacing();
		ImGui::TextWrapped("%s", "Settings.Plugins.TrustHint"_lc);
	}

	void RenderSystemTab(ThemeManager& tm, ThemeSettings& settings,
		casioemu::settings::SettingsData& data) {
		ImGui::TextWrapped("%s", "Settings.System.Description"_lc);
		ImGui::Spacing();

		UIHelpers::SectionHeader("Language");
		if (strlen(settings.language) == 0)
			strncpy(settings.language, g_local.GetCurrentLanguage().c_str(), sizeof(settings.language));

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
		ImGui::SetNextItemWidth(300.0f);
		if (ImGui::BeginCombo("##language_combo", currentLabel.c_str())) {
			for (const auto& lang : availableLanguages) {
				const std::string dispName = g_local.Get(lang.displayNameKey);
				const std::string itemLabel = std::string("[") + lang.badge + "] " + dispName;
				const bool isSelected = (currentLangStr == lang.code);
				if (ImGui::Selectable(itemLabel.c_str(), isSelected)) {
					strncpy(settings.language, lang.code, sizeof(settings.language));
					settings.language[sizeof(settings.language) - 1] = '\0';
					g_local.ChangeLanguage(settings.language);
					tm.RequestFontRebuild();
					SaveNow();
				}
				if (isSelected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if (ImGui::Button("Ui.ForceUpdateLang"_lc)) {
			if (strlen(settings.language) > 0) {
				const std::string langFile = "./locales/" + std::string(settings.language) + ".lc";
				std::error_code ec;
				if (std::filesystem::exists(langFile, ec))
					std::filesystem::remove(langFile, ec);
				g_local.ChangeLanguage(settings.language);
				tm.RequestFontRebuild();
			}
		}
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::TextUnformatted("Ui.ForceUpdateLangDesc"_lc);
			ImGui::EndTooltip();
		}

#ifndef CASIOEMU_CORE_WEB
		UIHelpers::SectionHeader("Integrations");
		if (ImGui::Checkbox("Theme.EnableDiscordRPC"_lc, &settings.enableDiscordRPC)) {
			SaveNow();
			if (m_emu)
				DiscordRPC::UpdatePresence(m_emu->ModelDefinition.model_name);
			else
				DiscordRPC::UpdatePresence("");
		}
		if (ImGui::Checkbox("Theme.EnableUpdateChecker"_lc, &settings.enableUpdateChecker))
			SaveNow();
#endif

		UIHelpers::SectionHeader("Settings.System.Tabs"_lc);
		ImGui::TextWrapped("%s", "Settings.System.TabsHint"_lc);
		bool tabVisibilityChanged = false;
		for (auto* win : windows) {
			if (!win || !win->name) continue;
			const bool isSettings = std::strcmp(win->name, "Settings") == 0;
			if (isSettings) {
				bool alwaysVisible = true;
				ImGui::BeginDisabled();
				ImGui::Checkbox(win->name, &alwaysVisible);
				ImGui::EndDisabled();
				continue;
			}
			if (ImGui::Checkbox(win->name, &win->open))
				tabVisibilityChanged = true;
		}
		const std::string hiddenTabs = HiddenTabsValue();
		if (tabVisibilityChanged || hiddenTabs != settings.hiddenTabs) {
			strncpy(settings.hiddenTabs, hiddenTabs.c_str(), sizeof(settings.hiddenTabs));
			settings.hiddenTabs[sizeof(settings.hiddenTabs) - 1] = '\0';
			SaveNow();
		}

		UIHelpers::SectionHeader("Settings.System.Storage"_lc);
		auto& store = casioemu::settings::SettingsStore::Instance();
		ImGui::TextDisabled("%s: %s", "Settings.System.SettingsFile"_lc, store.FilePath().string().c_str());
		ImGui::TextDisabled("%s: %s", "Settings.System.Schema"_lc, "1");
		if (ImGui::Button("Settings.System.SaveNow"_lc))
			SaveNow();
		ImGui::SameLine();
		if (ImGui::Button("Settings.System.Reload"_lc)) {
			tm.LoadSettings();
			tabsApplied = false;
		}
	}

public:
	SettingsWindow() : UIWindow("Settings") {
		auto& settings = ThemeManager::Instance().Settings();
		strncpy(tempInjectionFilePath, settings.injectionFilePath, sizeof(tempInjectionFilePath));
		tempInjectionFilePath[sizeof(tempInjectionFilePath) - 1] = '\0';
	}

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

		if (ImGui::BeginTabBar("##settings_categories")) {
			if (ImGui::BeginTabItem("Settings.Tab.Theme"_lc)) {
				RenderThemeTab(tm, settings, data);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Settings.Tab.Editor"_lc)) {
				RenderEditorTab(tm, data);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Settings.Tab.Emulator"_lc)) {
				RenderEmulatorTab(tm, settings, data);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Settings.Tab.Plugins"_lc)) {
				RenderPluginsTab(tm, data);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Settings.Tab.System"_lc)) {
				RenderSystemTab(tm, settings, data);
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}
};

UIWindow* MakeSettingsWindow() {
	return new SettingsWindow();
}
