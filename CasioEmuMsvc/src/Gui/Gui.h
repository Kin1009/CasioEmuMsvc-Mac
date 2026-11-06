#pragma once
#include "Localization.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <imgui_impl_sdlrenderer2.h>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#ifdef __ANDROID__
#include "AndroidSystemFont.h"
#include "RuntimeFontGlyphCache.h"
#include <android/log.h>
#endif

#ifndef __ANDROID__
inline const ImWchar* GetCJKRanges() {
	static const ImWchar ranges[] = {
		0x0020, 0x00FF,
		0x2000, 0x206F,
		0x3000, 0x30FF,
		0x31F0, 0x31FF,
		0xFF00, 0xFFEF,
		0x4E00, 0x9FAF,
		0,
	};
	return ranges;
}
#endif

// -----------------------------------------------------------------------------
// 辅助：在路径列表中寻找第一个存在的字体文件
// -----------------------------------------------------------------------------
inline std::string FindBestFont(const std::vector<std::string>& candidates) {
	for (const auto& path : candidates) {
		if (std::filesystem::exists(path)) {
			return path;
		}
	}
	return "";
}

inline std::string GetUIFontPath() {
	std::vector<std::string> candidates;
#ifdef _WIN32
	candidates = { "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\tahoma.ttf" };
#elif defined(__ANDROID__)
	candidates = { "/system/fonts/Roboto-Regular.ttf", "/system/fonts/NotoSans-Regular.ttf" };
#elif defined(__APPLE__)
	candidates = { "/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc",
		"/System/Library/Fonts/HelveticaNeue.ttc", "/Library/Fonts/Arial.ttf" };
#else
	candidates = { "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
		"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf" };
#endif
	return FindBestFont(candidates);
}

// -----------------------------------------------------------------------------
// 获取等宽字体 (Monospace Font) - 核心修正
// -----------------------------------------------------------------------------
inline std::string GetMonospaceFontPath() {
	std::vector<std::string> candidates;

#ifdef _WIN32
	// Windows 等宽字体回退链
	candidates = {
		// 1. Cascadia (Win10/11 Terminal 默认字体，极其适合代码)
		"C:\\Windows\\Fonts\\CascadiaMono.ttf",
		"C:\\Windows\\Fonts\\CascadiaCode.ttf",
		// 2. Consolas (经典的编程字体)
		"C:\\Windows\\Fonts\\Consola.ttf",
		// 3. Courier New (最后的兜底)
		"C:\\Windows\\Fonts\\cour.ttf"};
#elif defined(__ANDROID__)
	candidates = {
		"/system/fonts/DroidSansMono.ttf",
		"/system/fonts/NotoSansMono-Regular.ttf"};
#elif defined(__APPLE__)
	candidates = {
		"/System/Library/Fonts/SFNSMono.ttf",
		"/System/Library/Fonts/Monaco.ttf",
		"/System/Library/Fonts/Menlo.ttc",
		"/Library/Fonts/Courier New.ttf"};
#else // Linux / Unix
	// Linux 等宽字体非常多，这里列出主流发行版的默认项
	candidates = {
		// Ubuntu / Debian
		"/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
		"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
		"/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
		// Fedora / Arch / Others
		"/usr/share/fonts/noto/NotoMono-Regular.ttf",
		"/usr/share/fonts/TTF/DejaVuSansMono.ttf",
		"/usr/share/fonts/gnu-free/FreeMono.ttf"};
#endif

	return FindBestFont(candidates);
}

// -----------------------------------------------------------------------------
// 获取 CJK 字体
// 注意：即使是代码环境，CJK 字体通常也可以使用非严格等宽的黑体（Gothic），
// 因为 ImGui 会处理字符宽度，但最好还是选字形工整的。
// -----------------------------------------------------------------------------
inline std::string GetCJKFontPath(int* font_no = nullptr) {
	if (font_no) {
		*font_no = 0;
	}
	auto preference = "Localization.CJKPreference"_l;
	std::vector<std::string> candidates = {
		"temp/generated/fonts/CasioEmuGuiCJKSubset.otf",
		"./temp/generated/fonts/CasioEmuGuiCJKSubset.otf",
		"../temp/generated/fonts/CasioEmuGuiCJKSubset.otf"};

#ifdef _WIN32
	if (preference == "JP") {
		candidates.insert(candidates.end(), {
			"C:\\Windows\\Fonts\\msgothic.ttc", // MS Gothic 是日文等宽的首选
			"C:\\Windows\\Fonts\\YuGothM.ttc",
			"C:\\Windows\\Fonts\\meiryo.ttc"});
	}
	else if (preference == "KR") {
		candidates.insert(candidates.end(), {
			"C:\\Windows\\Fonts\\malgun.ttf",  // Malgun Gothic (맑은 고딕)
			"C:\\Windows\\Fonts\\gulim.ttc",   // Gulim (굴림)
			"C:\\Windows\\Fonts\\batang.ttc"   // Batang (바탕)
		});
	}
	else {
		candidates.insert(candidates.end(), {
			"C:\\Windows\\Fonts\\msyh.ttc",	  // 雅黑
			"C:\\Windows\\Fonts\\simhei.ttf", // 黑体 (较粗，但在某些低分屏上可读性好)
			"C:\\Windows\\Fonts\\simsun.ttc"  // 宋体 (最传统)
		});
	}
#elif defined(__ANDROID__)
	if (auto system_font = FindAndroidSystemCJKFont(preference)) {
		if (font_no) {
			*font_no = system_font->collection_index;
		}
		return system_font->path;
	}
	candidates.insert(candidates.end(), {
		"/system/fonts/NotoSansCJK-Regular.ttc",
		"/system/fonts/DroidSansFallback.ttf"});
#elif defined(__APPLE__)
	if (preference == "JP") {
		candidates.insert(candidates.end(), {
			"/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc",
			"/System/Library/Fonts/Supplemental/Osaka.ttf"
		});
	}
	else if (preference == "KR") {
		candidates.insert(candidates.end(), {
			"/System/Library/Fonts/AppleGothic.ttf",
			"/System/Library/Fonts/Supplemental/AppleGothic.ttf"
		});
	}
	else {
		candidates.insert(candidates.end(), {
			"/System/Library/Fonts/PingFang.ttc",
			"/System/Library/Fonts/STHeiti Light.ttc",
			"/System/Library/Fonts/Supplemental/Arial Unicode.ttf"
		});
	}
#else // Linux
	// 尝试寻找 CJK 的 Mono 版本 (如果有)，否则使用 Regular
	candidates.insert(candidates.end(), {
		"/usr/share/fonts/noto-cjk/NotoSansCJK-Mono.ttc", // 最佳：Noto 的等宽版本
		"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
		"/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
		"/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
		"/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
		"/usr/share/fonts/droid/DroidSansFallbackFull.ttf"});
#endif

	return FindBestFont(candidates);
}

// The memory views decode calculator glyph codes into their Unicode
// equivalents.  Those strings contain Greek letters, maths operators and
// arrows that neither the text face nor the CJK face necessarily carries in
// the atlas.  STIX Two Math covers the full set (and is the reference maths
// face on macOS); the other entries are fallbacks for other systems.
inline std::string GetMathFontPath() {
#ifdef _WIN32
	return FindBestFont({
		"C:\\Windows\\Fonts\\STIXTwoMath-Regular.otf",
		"C:\\Windows\\Fonts\\cambria.ttc",
		"C:\\Windows\\Fonts\\seguisym.ttf"});
#elif defined(__ANDROID__)
	return FindBestFont({
		"/system/fonts/NotoSansSymbols-Regular.ttf",
		"/system/fonts/NotoSansMath-Regular.ttf"});
#elif defined(__APPLE__)
	return FindBestFont({
		"/System/Library/Fonts/Supplemental/STIXTwoMath.otf",
		"/System/Library/Fonts/Supplemental/STIXGeneral.otf",
		"/System/Library/Fonts/Apple Symbols.ttf"});
#else
	return FindBestFont({
		"/usr/share/fonts/truetype/stix2/STIXTwoMath-Regular.otf",
		"/usr/share/fonts/opentype/stix/STIXTwoMath-Regular.otf",
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"});
#endif
}

inline const ImWchar* GetMemoryGlyphRanges() {
	static const ImWchar ranges[] = {
		0x0100, 0x024F, // Latin Extended-A/B (ħ, Ă, ...)
		0x0370, 0x03FF, // Greek
		0x2000, 0x206F, // general punctuation
		0x2070, 0x209F, // super/subscripts
		0x2100, 0x214F, // letterlike symbols
		0x2190, 0x21FF, // arrows
		0x2200, 0x22FF, // mathematical operators
		0x2300, 0x23FF, // technical symbols
		0x2500, 0x257F, // box drawing
		0x25A0, 0x25FF, // geometric shapes
		0x27F0, 0x27FF, // supplemental arrows
		// Mathematical alphanumerics are U+1D400..U+1D7FF and do not fit
		// ImWchar (16 bit); the decoded table folds them to plain letters.
		0,
	};
	return ranges;
}

// Korean needs a separate face on macOS: the Chinese-first fallbacks (Heiti,
// Hiragino) do not contain Hangul.  Returns the face that covers Hangul.
inline std::string GetKoreanFontPath(int* font_no = nullptr) {
	if (font_no) {
		*font_no = 0;
	}
#ifdef _WIN32
	const std::vector<std::string> candidates = {
		"C:\\Windows\\Fonts\\malgun.ttf",
		"C:\\Windows\\Fonts\\gulim.ttc",
		"C:\\Windows\\Fonts\\batang.ttc"};
#elif defined(__ANDROID__)
	if (auto system_font = FindAndroidSystemCJKFont("KR")) {
		if (font_no) {
			*font_no = system_font->collection_index;
		}
		return system_font->path;
	}
	const std::vector<std::string> candidates = {
		"/system/fonts/NotoSansCJK-Regular.ttc",
		"/system/fonts/DroidSansFallback.ttf"};
#elif defined(__APPLE__)
	const std::vector<std::string> candidates = {
		"/System/Library/Fonts/AppleSDGothicNeo.ttc",
		"/System/Library/Fonts/Supplemental/AppleGothic.ttf"};
#else
	const std::vector<std::string> candidates = {
		"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
		"/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
		"/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc"};
#endif
	return FindBestFont(candidates);
}

// A face that contains U+FFFD so ImGui's missing-glyph fallback is a
// replacement character rather than the ASCII '?'.
inline std::string GetFallbackGlyphFontPath() {
#ifdef _WIN32
	return FindBestFont({
		"C:\\Windows\\Fonts\\arialuni.ttf",
		"C:\\Windows\\Fonts\\seguisym.ttf",
		"C:\\Windows\\Fonts\\segoeui.ttf"});
#elif defined(__ANDROID__)
	return FindBestFont({
		"/system/fonts/NotoSansCJK-Regular.ttc",
		"/system/fonts/DroidSansFallback.ttf"});
#elif defined(__APPLE__)
	return FindBestFont({
		"/System/Library/Fonts/Supplemental/Arial Unicode.ttf"});
#else
	return FindBestFont({
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
		"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"});
#endif
}

// ImGui chooses '?' when the atlas has no U+FFFD.  Prefer the replacement
// glyph, then a hollow box, and only fall back to the font's own default.
inline void SetSafeFallbackGlyph(ImFont* font) {
	if (font == nullptr) {
		return;
	}
	const ImFontGlyph* glyph = font->FindGlyphNoFallback(0xFFFD);
	if (glyph == nullptr) {
		glyph = font->FindGlyphNoFallback(0x25A1); // WHITE SQUARE
	}
	if (glyph == nullptr) {
		glyph = font->FindGlyphNoFallback(0x2588); // FULL BLOCK
	}
	if (glyph != nullptr && glyph != font->FallbackGlyph) {
		font->FallbackChar = static_cast<ImWchar>(glyph->Codepoint);
		font->FallbackGlyph = glyph;
		font->FallbackAdvanceX = glyph->AdvanceX;
	}
}

// Text of the shipped documentation bundle.  Adding it to the atlas guarantees
// that Chinese/Japanese/Korean documentation renders in English mode even when
// the common CJK range does not contain a rare character.
inline const std::string& CollectDocumentationGlyphText() {
	static const std::string text = [] {
		std::string result;
		std::error_code ec;
		const auto cwd = std::filesystem::current_path(ec);
		const std::filesystem::path candidates[] = {
			cwd / ".." / "Resources" / "docs",
			cwd / "Resources" / "docs",
			cwd / "docs"};
		for (const auto& root : candidates) {
			if (!std::filesystem::is_directory(root, ec)) {
				ec.clear();
				continue;
			}
			for (auto it = std::filesystem::recursive_directory_iterator(root,
					std::filesystem::directory_options::skip_permission_denied, ec);
				it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
				if (ec) {
					ec.clear();
					continue;
				}
				std::string extension = it->path().extension().string();
				std::transform(extension.begin(), extension.end(), extension.begin(),
					[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
				if (extension != ".md" && extension != ".markdown" && extension != ".txt")
					continue;
				std::ifstream input(it->path(), std::ios::binary);
				if (!input)
					continue;
				result.append(std::istreambuf_iterator<char>(input),
					std::istreambuf_iterator<char>());
				result.push_back('\n');
			}
			break;
		}
		return result;
	}();
	return text;
}

// See ThemeManager.h for the unified API.

extern ImFont* g_mono_font;
extern ImFont* g_small_font;

inline const ImWchar* GetUiSymbolRanges() {
	static const ImWchar ranges[] = {
		0x2000, 0x206F, 0x2190, 0x21FF, 0x2300, 0x23FF,
		0x25A0, 0x25FF, 0x2600, 0x26FF, 0x2B00, 0x2BFF, 0,
	};
	return ranges;
}

inline void RebuildFont(float scale = 0.0f) {
	auto& io = ImGui::GetIO();
	ImFontConfig config = ImFontConfig();

	// 对于代码显示，PixelSnapH 非常重要，能防止字母边缘模糊
	config.PixelSnapH = true;

	io.Fonts->Clear();

#ifdef __ANDROID__
	constexpr float defaultscale = 3.0f;
#else
	constexpr float defaultscale = 1.0f;
#endif
	if (scale == 0) {
		scale = defaultscale;
	}

	// 1. Original CasioEmuMsvc UI face: the monospace system font at 15 px.
	std::string mono_font_path = GetMonospaceFontPath();
#ifdef __ANDROID__
	ImWchar extended_script_sample = 0;
	const char* extended_script_name = nullptr;
#endif
	if (!mono_font_path.empty()) {
		io.Fonts->AddFontFromFileTTF(mono_font_path.c_str(), 15 * scale, &config,
			io.Fonts->GetGlyphRangesDefault());
	}
	else {
		io.Fonts->AddFontDefault(&config);
	}
	g_mono_font = io.Fonts->Fonts.empty() ? nullptr : io.Fonts->Fonts[0];
	g_small_font = g_mono_font;

	// Merge additional glyph ranges based on localization settings.
#ifdef __APPLE__
	// Keep Vietnamese available even when the UI language is not vi_VN.  The
	// preview shows Vietnamese in plugin names, paths, compiler output, and the
	// CWiz examples, all of which use the UI font.  Monaco and SF Mono both
	// contain the full 1EA0-1EF9 range.
	const bool load_vietnamese = true;
#else
	const bool load_vietnamese = "Localization.LoadVietnamese"_l == "1" || "Localization.LoadVietnamese"_l == "true";
#endif
	const bool load_cyrillic = "Localization.LoadCyrillic"_l == "1" || "Localization.LoadCyrillic"_l == "true";
	const bool load_greek = "Localization.LoadGreek"_l == "1" || "Localization.LoadGreek"_l == "true";
	const bool load_thai = "Localization.LoadThai"_l == "1" || "Localization.LoadThai"_l == "true";
#ifdef __ANDROID__
	auto merge_system_font = [&](bool enabled, std::string_view language_tag,
		std::u16string_view sample, const ImWchar* ranges, ImWchar validation_sample,
		const char* script_name) {
		if (!enabled) {
			return;
		}
		extended_script_sample = validation_sample;
		extended_script_name = script_name;
		auto system_font = FindAndroidSystemFont(language_tag, sample);
		if (!system_font) {
			__android_log_print(ANDROID_LOG_ERROR, "CasioEmuFont",
				"No Android system font found for %s", script_name);
			return;
		}
		ImFontConfig merge_config = config;
		merge_config.MergeMode = true;
		merge_config.FontNo = system_font->collection_index;
		ImFont* font = io.Fonts->AddFontFromFileTTF(
			system_font->path.c_str(), 15 * scale, &merge_config, ranges);
		__android_log_print(font ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "CasioEmuFont",
			"%s Android system font for %s: %s (collection index %d)",
			font ? "Selected" : "Failed to load", script_name,
			system_font->path.c_str(), system_font->collection_index);
	};
	merge_system_font(load_vietnamese, "vi-VN", u"Tiếng Việt",
		io.Fonts->GetGlyphRangesVietnamese(), 0x1EBF, "Vietnamese");
	merge_system_font(load_cyrillic, "ru-RU", u"Русский",
		io.Fonts->GetGlyphRangesCyrillic(), 0x042F, "Cyrillic");
	merge_system_font(load_greek, "el-GR", u"Ελληνικά",
		io.Fonts->GetGlyphRangesGreek(), 0x03A9, "Greek");
	merge_system_font(load_thai, "th-TH", u"ไทย",
		io.Fonts->GetGlyphRangesThai(), 0x0E44, "Thai");
#else
	const std::string merge_font_path = mono_font_path;
	if (!merge_font_path.empty()) {
		config.MergeMode = true;
		if (load_vietnamese)
			io.Fonts->AddFontFromFileTTF(merge_font_path.c_str(), 15 * scale, &config, io.Fonts->GetGlyphRangesVietnamese());
		if (load_cyrillic)
			io.Fonts->AddFontFromFileTTF(merge_font_path.c_str(), 15 * scale, &config, io.Fonts->GetGlyphRangesCyrillic());
		if (load_greek)
			io.Fonts->AddFontFromFileTTF(merge_font_path.c_str(), 15 * scale, &config, io.Fonts->GetGlyphRangesGreek());
		if (load_thai)
			io.Fonts->AddFontFromFileTTF(merge_font_path.c_str(), 15 * scale, &config, io.Fonts->GetGlyphRangesThai());
		config.MergeMode = false;
	}
#endif

	// 2. 合并 CJK 字体
#ifdef __ANDROID__
	const std::string enable_cjk = "Localization.EnableCJK"_l;
	const bool locale_needs_cjk = enable_cjk == "1" || enable_cjk == "true";
	const std::string runtime_glyph_text = RuntimeFontGlyphCache::Instance().GetText();
	const bool needs_cjk_font = locale_needs_cjk || !runtime_glyph_text.empty();
	if (needs_cjk_font) {
		const std::string localized_text = g_local.CollectFontGlyphText();
		ImFontGlyphRangesBuilder glyph_builder;
		glyph_builder.AddText(localized_text.c_str());
		glyph_builder.AddText(runtime_glyph_text.c_str());
		ImVector<ImWchar> localized_ranges;
		glyph_builder.BuildRanges(&localized_ranges);

		int cjk_font_no = 0;
		std::string cjk_font_path = GetCJKFontPath(&cjk_font_no);

		if (!cjk_font_path.empty()) {
			ImFontConfig merge_config = config;
			merge_config.MergeMode = true;
			merge_config.FontNo = cjk_font_no;

			// 稍微放大一点 CJK 字体，通常中文比同号的英文显得小
			ImFont* cjk_font = io.Fonts->AddFontFromFileTTF(
				cjk_font_path.c_str(), 16 * scale, &merge_config, localized_ranges.Data);
			if (cjk_font) {
				__android_log_print(ANDROID_LOG_INFO, "CasioEmuFont",
					"Selected CJK system font: %s (collection index %d, localized ranges %d)",
					cjk_font_path.c_str(), cjk_font_no, localized_ranges.Size / 2);
			}
			else {
				__android_log_print(ANDROID_LOG_ERROR, "CasioEmuFont",
					"Failed to load CJK system font: %s (collection index %d)",
					cjk_font_path.c_str(), cjk_font_no);
			}
		}
		else {
			__android_log_print(ANDROID_LOG_ERROR, "CasioEmuFont", "No CJK system font found");
		}
	}
	const bool font_build_succeeded = io.Fonts->Build();
	SetSafeFallbackGlyph(g_mono_font);
	if (extended_script_sample != 0) {
		const bool sample_present = font_build_succeeded && !io.Fonts->Fonts.empty()
			&& io.Fonts->Fonts[0]->FindGlyphNoFallback(extended_script_sample) != nullptr;
		__android_log_print(
			font_build_succeeded && sample_present ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
			"CasioEmuFont",
			"Atlas build=%s, glyphs=%d, %s sample U+%04X present=%s",
			font_build_succeeded ? "true" : "false",
			io.Fonts->Fonts.empty() ? 0 : io.Fonts->Fonts[0]->Glyphs.Size,
			extended_script_name,
			static_cast<unsigned int>(extended_script_sample),
			sample_present ? "true" : "false");
	}
	if (locale_needs_cjk) {
		ImWchar sample = 0x4E2D;
		const std::string preference = "Localization.CJKPreference"_l;
		if (preference == "JP") {
			sample = 0x65E5;
		}
		else if (preference == "KR") {
			sample = 0xD55C;
		}
		const bool sample_present = font_build_succeeded && !io.Fonts->Fonts.empty()
			&& io.Fonts->Fonts[0]->FindGlyphNoFallback(sample) != nullptr;
		__android_log_print(
			font_build_succeeded && sample_present ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
			"CasioEmuFont",
			"Atlas build=%s, glyphs=%d, sample U+%04X present=%s",
			font_build_succeeded ? "true" : "false",
			io.Fonts->Fonts.empty() ? 0 : io.Fonts->Fonts[0]->Glyphs.Size,
			static_cast<unsigned int>(sample),
			sample_present ? "true" : "false");
	}
#else
	// The atlas must carry CJK even when the UI locale is English: the
	// documentation viewer, file names, model names and plugin output can all
	// contain Chinese/Japanese/Korean text.  EnableCJK selects the full CJK
	// block; otherwise a common + kana + Hangul range is always present.
	// The atlas keeps a pointer to this range array and may rebuild the atlas
	// later (for example when the title face is added), so it must outlive this
	// call.
	static ImVector<ImWchar> cjk_ranges;
	cjk_ranges.clear();
	{
		const std::string enable_cjk = "Localization.EnableCJK"_l;
		const bool locale_needs_cjk = enable_cjk == "1" || enable_cjk == "true";
		const std::string language = g_local.GetCurrentLanguage();

		ImFontGlyphRangesBuilder cjk_builder;
		// EnableCJK selects the full CJK block for CJK UI locales.  Other
		// locales always get the common CJK + kana + Hangul ranges plus the
		// exact text of the shipped documentation, so Chinese docs render in
		// English mode without paying for the full 21k-ideograph atlas.
		if (locale_needs_cjk || language.rfind("zh", 0) == 0 ||
			language.rfind("ja", 0) == 0 || language.rfind("ko", 0) == 0) {
			cjk_builder.AddRanges(GetCJKRanges());
		}
		else {
			cjk_builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
			cjk_builder.AddRanges(io.Fonts->GetGlyphRangesJapanese());
		}
		cjk_builder.AddRanges(io.Fonts->GetGlyphRangesKorean());
		cjk_builder.AddText("\xEF\xBF\xBD"); // U+FFFD replacement character
		const std::string localized_text = g_local.CollectFontGlyphText();
		if (!localized_text.empty()) {
			cjk_builder.AddText(localized_text.c_str());
		}
		const std::string& documentation_text = CollectDocumentationGlyphText();
		if (!documentation_text.empty()) {
			cjk_builder.AddText(documentation_text.c_str());
		}
		cjk_builder.BuildRanges(&cjk_ranges);

		int codepoint_count = 0;
		for (int index = 0; index + 1 < cjk_ranges.Size && cjk_ranges[index] != 0; index += 2) {
			codepoint_count += static_cast<int>(cjk_ranges[index + 1]) -
				static_cast<int>(cjk_ranges[index]) + 1;
		}

		int cjk_font_no = 0;
		const std::string cjk_font_path = GetCJKFontPath(&cjk_font_no);
		ImFontConfig merge_config = config;
		merge_config.MergeMode = true;
		merge_config.FontNo = cjk_font_no;
		// CJK glyphs are dense; 1x oversampling keeps the atlas sane.
		merge_config.OversampleH = 1;
		merge_config.OversampleV = 1;
		if (!cjk_font_path.empty()) {
			io.Fonts->AddFontFromFileTTF(cjk_font_path.c_str(), 16 * scale,
				&merge_config, cjk_ranges.Data);
		}

		int korean_font_no = 0;
		const std::string korean_font_path = GetKoreanFontPath(&korean_font_no);
		if (!korean_font_path.empty() && korean_font_path != cjk_font_path) {
			ImFontConfig korean_config = merge_config;
			korean_config.FontNo = korean_font_no;
			io.Fonts->AddFontFromFileTTF(korean_font_path.c_str(), 16 * scale,
				&korean_config, io.Fonts->GetGlyphRangesKorean());
		}

		const std::string fallback_font_path = GetFallbackGlyphFontPath();
		if (!fallback_font_path.empty()) {
			static const ImWchar replacement_range[] = {0xFFFD, 0xFFFD, 0};
			ImFontConfig fallback_config = merge_config;
			fallback_config.FontNo = 0;
			io.Fonts->AddFontFromFileTTF(fallback_font_path.c_str(), 16 * scale,
				&fallback_config, replacement_range);
		}

		const std::string math_font_path = GetMathFontPath();
		if (!math_font_path.empty()) {
			ImFontConfig math_config = config;
			math_config.MergeMode = true;
			math_config.OversampleH = 1;
			math_config.OversampleV = 1;
			io.Fonts->AddFontFromFileTTF(math_font_path.c_str(), 15 * scale,
				&math_config, GetMemoryGlyphRanges());
		}

		fprintf(stderr,
			"[Font] locale=%s enable_cjk=%d cjk=%s korean=%s fallback=%s math=%s ranges=%d codepoints=%d\n",
			language.c_str(), locale_needs_cjk ? 1 : 0,
			cjk_font_path.empty() ? "(none)" : cjk_font_path.c_str(),
			korean_font_path.empty() ? "(none)" : korean_font_path.c_str(),
			fallback_font_path.empty() ? "(none)" : fallback_font_path.c_str(),
			math_font_path.empty() ? "(none)" : math_font_path.c_str(),
			cjk_ranges.Size, codepoint_count);
	}
	io.Fonts->Build();
	SetSafeFallbackGlyph(g_mono_font);
	if (!io.Fonts->Fonts.empty()) {
		ImFont* font = io.Fonts->Fonts[0];
		const size_t pixels = static_cast<size_t>(io.Fonts->TexWidth) *
			static_cast<size_t>(io.Fonts->TexHeight);
		fprintf(stderr,
			"[Font] glyphs=%d atlas=%dx%d alpha=%.1fMB rgba=%.1fMB fallback=U+%04X\n",
			font->Glyphs.Size, io.Fonts->TexWidth, io.Fonts->TexHeight,
			static_cast<double>(pixels) / (1024.0 * 1024.0),
			static_cast<double>(pixels * 4) / (1024.0 * 1024.0),
			static_cast<unsigned int>(font->FallbackChar));
	}
#endif
}
