// ???
#include <SDL_rect.h>
#include "ModelConfig.h"
#include "ModelInfo.h"
// 

#include "StartupUi.h"
#include "OnlineLoopbackServer.h"
#include "OnlineModelClient.h"
#include "OnlineModelPackage.h"
#include "ModelImport.h"
#include "UpdateChecker.h"
#include <OnlineBuildConfig.h>
#include "3rd_licenses.h"
#include "Binary.h"
#include "Config.hpp"
#include "Gui/imgui/imgui.h"
#include "Gui/imgui/imgui_impl_sdl2.h"
#include "Gui/imgui/imgui_impl_sdlrenderer2.h"
#include "Localization.h"
#include "RendererBackend.h"
#include "RomPackage.h"
#include "Romu.h"
#include "SysDialog.h"
#include "Ui.hpp"
#include "sdl_win32_extra.h"
#include <Gui.h>
#include <Gui/Theme.h>
#include <Gui/ThemeManager.h>
#include "Ext/MacWindow.h"
#include "Ext/Screenshot.h"
#include <SDL.h>
#include <SDL_image.h>
#include <array>
#include <atomic>
#include <filesystem>
#include <imgui.h>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>

#ifdef _WIN32
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windows.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#endif
#include "Ext/Random.hpp"
#include "DiscordRPC.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

#ifdef __ANDROID__
#include "../Gui/ThemeManager.h"
#include <SDL.h>
#include <SDL_system.h>
#include <android/log.h>
#include <jni.h>

static JavaVM* g_VM = nullptr;

extern "C"
{
	JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
		g_VM = vm;
		return JNI_VERSION_1_6;
	}
}

// Helper function to get JNIEnv
bool GetJNIEnv(JNIEnv** env) {
	if (g_VM == nullptr) {
		return false;
	}

	jint result = g_VM->GetEnv((void**)env, JNI_VERSION_1_6);
	if (result == JNI_EDETACHED) {
		if (g_VM->AttachCurrentThread(env, nullptr) != 0) {
			return false;
		}
	}
	else if (result != JNI_OK) {
		return false;
	}
	return true;
}

// Helper function to detach current thread from JVM
void DetachCurrentThread() {
	if (g_VM != nullptr) {
		g_VM->DetachCurrentThread();
	}
}
#endif

inline SDL_Window* window2;
inline SDL_Renderer* renderer2;
inline std::vector<UIWindow*>* windows2;

void RenderClippedSprite(SDL_Renderer* renderer, SDL_Texture* texture, const casioemu::SpriteInfo& sprite) {
	SDL_Rect srcRect = sprite.src;
	SDL_Rect destRect = sprite.dest;

	// Calculate the aspect ratio of src and dest
	float aspectRatioSrc = (float)srcRect.w / (float)srcRect.h;
	float aspectRatioDest = (float)destRect.w / (float)destRect.h;

	// Adjust the srcRect to fit within the destRect, keeping the aspect ratio
	if (aspectRatioSrc > aspectRatioDest) {
		int newHeight = srcRect.w / aspectRatioDest;
		srcRect.y += (srcRect.h - newHeight) / 2;
		srcRect.h = newHeight;
	}
	else {
		int newWidth = srcRect.h * aspectRatioDest;
		srcRect.x += (srcRect.w - newWidth) / 2;
		srcRect.w = newWidth;
	}

	// Render the texture using the adjusted source and destination rectangles
	SDL_RenderCopy(renderer, texture, &srcRect, &destRect);
}

class ModelEditor : public UIWindow {
	std::filesystem::path pth;
	casioemu::ModelInfo mi;
	int v;
	int k;
	char path1[260];
	char path2[260];
	char path3[260];
	char name[260];
	SDL_Texture* sdl_t = 0;
	ImVec2 imgSz;
	casioemu::SpriteInfo imgSp;
	float color[3];

	char buffer[260];
	char buffer2[12];

	casioemu::ButtonInfo* btninfo{};
	bool init_failed = false;
	std::string selected_sprite_key;

public:
	ModelEditor(std::filesystem::path path) : UIWindow("Model Editor##114514"), pth(path) {
		try {
			std::string error;
			if (!casioemu::LoadModelInfoFromFolder(path, mi, nullptr, &error)) {
				throw std::runtime_error(error);
			}
			v = mi.csr_mask;
			k = mi.pd_value;
			strncpy(path1, mi.interface_path.c_str(), sizeof(path1) - 1);
			path1[sizeof(path1) - 1] = '\0';
			strncpy(path2, mi.rom_path.c_str(), sizeof(path2) - 1);
			path2[sizeof(path2) - 1] = '\0';
			strncpy(path3, mi.flash_path.c_str(), sizeof(path3) - 1);
			path3[sizeof(path3) - 1] = '\0';
			strncpy(name, mi.model_name.c_str(), sizeof(name) - 1);
			name[sizeof(name) - 1] = '\0';
			color[0] = mi.ink_color.r / 255.0f;
			color[1] = mi.ink_color.g / 255.0f;
			color[2] = mi.ink_color.b / 255.0f;
			LoadInterface();
		}
		catch (const std::exception& e) {
			std::cerr << "Failed to load ModelEditor: " << e.what() << std::endl;
			init_failed = true;
		}
	}
	~ModelEditor() override {
		if (sdl_t)
			SDL_DestroyTexture(sdl_t);
	}
	bool IsBoardModel() const {
		return !mi.board_path.empty();
	}
	static bool HasSvgExtension(const std::filesystem::path& path) {
		auto ext = path.extension().string();
		for (auto& ch : ext)
			ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		return ext == ".svg";
	}
	SDL_Surface* LoadInterfaceSurface(const std::filesystem::path& path, const casioemu::SpriteInfo& interface_sprite) {
		if (!HasSvgExtension(path))
			return IMG_Load(path.string().c_str());

		std::ifstream stream(path, std::ios::binary);
		if (!stream)
			return nullptr;
		const std::string svg{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
		if (svg.empty())
			return nullptr;

		const int width = interface_sprite.src.w > 0 ? interface_sprite.src.w : 1;
		const int height = interface_sprite.src.h > 0 ? interface_sprite.src.h : 1;
		SDL_RWops* rw = SDL_RWFromConstMem(svg.data(), static_cast<int>(svg.size()));
		if (!rw)
			return nullptr;
		SDL_Surface* surface = IMG_LoadSizedSVG_RW(rw, width, height);
		SDL_RWclose(rw);
		if (!surface)
			SDL_Log("[StartupUI][Warn] IMG_LoadSizedSVG_RW failed for editor interface SVG: %s", IMG_GetError());
		return surface;
	}
	void RenderSprite(const casioemu::SpriteInfo& sprite, ImTextureID texture_id, const ImVec2& texture_size, const ImVec2& render_size) {

		// 计算 UV 坐标
		ImVec2 uv0(
			sprite.src.x / texture_size.x,
			sprite.src.y / texture_size.y);
		ImVec2 uv1(
			(sprite.src.x + sprite.src.w) / texture_size.x,
			(sprite.src.y + sprite.src.h) / texture_size.y);
		ImVec4 tint_clr{1, 1, 1, 1};
		if (!mi.enable_new_screen) {
			tint_clr.x = mi.ink_color.r / 255.0f;
			tint_clr.y = mi.ink_color.g / 255.0f;
			tint_clr.z = mi.ink_color.b / 255.0f;
		}
		// 渲染裁剪后的图像
		ImGui::Image(
			texture_id,
			render_size,
			uv0,
			uv1, tint_clr);
	}
	void RenderSprite2(const casioemu::SpriteInfo& sprite, ImTextureID texture_id, const ImVec2& texture_size, const ImVec2& render_size) {

		// 计算 UV 坐标
		ImVec2 uv0(
			sprite.src.x / texture_size.x,
			sprite.src.y / texture_size.y);
		ImVec2 uv1(
			(sprite.src.x + sprite.src.w) / texture_size.x,
			(sprite.src.y + sprite.src.h) / texture_size.y);
		// 渲染裁剪后的图像
		ImGui::Image(
			texture_id,
			render_size,
			uv0,
			uv1);
	}
	void RenderSprite3(const casioemu::SpriteInfo& sprite, ImTextureID texture_id, const ImVec2& texture_size, const ImVec2& render_size) {

		// 计算 UV 坐标
		ImVec2 uv0(
			sprite.src.x / texture_size.x,
			sprite.src.y / texture_size.y);
		ImVec2 uv1(
			(sprite.src.x + sprite.src.w) / texture_size.x,
			(sprite.src.y + sprite.src.h) / texture_size.y);
		ImVec4 tint_clr{1, 1, 1, 1};
		tint_clr.x = mi.ink_color.r / 255.0f;
		tint_clr.y = mi.ink_color.g / 255.0f;
		tint_clr.z = mi.ink_color.b / 255.0f;
		// 渲染裁剪后的图像
		ImGui::Image(
			texture_id,
			render_size,
			uv0,
			uv1, tint_clr);
	}
	void LoadInterface() {
		if (sdl_t) {
			SDL_DestroyTexture(sdl_t);
			sdl_t = nullptr;
		}
		if (mi.sprites.find("rsd_interface") != mi.sprites.end()) {
			SDL_Surface* surface = LoadInterfaceSurface(pth / mi.interface_path, mi.sprites["rsd_interface"]);
			if (surface) {
				sdl_t = SDL_CreateTextureFromSurface(renderer2, surface);
				imgSz = {(float)surface->w, (float)surface->h};
				SDL_FreeSurface(surface);
			}
			imgSp = mi.sprites["rsd_interface"];
		}
	}
	void RenderCore() override {
		if (init_failed) {
			ImGui::TextColored({1.0f, 0.0f, 0.0f, 1.0f}, "Failed to load model configuration. The file might be corrupted.");
			if (ImGui::Button("Close")) {
				open = false;
			}
			return;
		}

		auto y = ImGui::GetCursorPosY();
		const bool is_board_model = IsBoardModel();
		const int model_width = is_board_model ? imgSp.dest.w : imgSp.src.w;
		auto scaleFactor = model_width > 0 ? (400.f / model_width) : 1.0f;
		if (sdl_t != 0) {
			ImGui::SetCursorPosX(0);
			RenderSprite2(imgSp, (ImTextureID)sdl_t, imgSz, {400, 400.0f * imgSp.dest.h / imgSp.dest.w});
			if (!is_board_model) {
				for (auto& sp : mi.sprites) {
					if (sp.first != "rsd_pixel" && sp.first != "rsd_interface") {
						ImGui::SetCursorPos({(float)sp.second.dest.x * scaleFactor, (float)sp.second.dest.y * scaleFactor + y});
						RenderSprite(sp.second, (ImTextureID)sdl_t, imgSz, {scaleFactor * (float)sp.second.dest.w, scaleFactor * (float)sp.second.dest.h});
						if (sp.first == selected_sprite_key) {
							auto min_p = ImGui::GetItemRectMin();
							auto max_p = ImGui::GetItemRectMax();
							ImGui::GetWindowDrawList()->AddRect(min_p, max_p, IM_COL32(255, 0, 0, 255), 0.0f, 0, 2.0f);
						}
					}
				}
				auto sp2 = mi.sprites["rsd_pixel"];
				if (mi.hardware_id == casioemu::HW_ES_PLUS || mi.hardware_id == casioemu::HW_FX_5800P || casioemu::IsEpsFamily(mi.hardware_id)) {
					for (size_t j = 0; j < 31; j++) {
						for (size_t i = 0; i < 96; i++) {
							ImGui::SetCursorPos({(float)(sp2.dest.x + i * sp2.dest.w) * scaleFactor, (float)(sp2.dest.y + j * sp2.dest.h) * scaleFactor + y});
							RenderSprite3(sp2, (ImTextureID)sdl_t, imgSz, {scaleFactor * (float)sp2.dest.w, scaleFactor * (float)sp2.dest.h});
						}
					}
				}
				else {
					for (size_t j = 0; j < 63; j++) {
						for (size_t i = 0; i < 192; i++) {
							ImGui::SetCursorPos({(float)(sp2.dest.x + i * sp2.dest.w) * scaleFactor, (float)(sp2.dest.y + j * sp2.dest.h) * scaleFactor + y});
							RenderSprite3(sp2, (ImTextureID)sdl_t, imgSz, {scaleFactor * (float)sp2.dest.w, scaleFactor * (float)sp2.dest.h});
						}
					}
				}
			}
		}
		else {
			ImGui::SetCursorPosX(0);
			ImGui::Dummy({400, 600});
			ImGui::SetCursorPos({0, y});
			ImGui::TextUnformatted("Failed to load interface preview.");
		}
		for (size_t button_index = 0; button_index < mi.buttons.size(); ++button_index) {
			auto& btn = mi.buttons[button_index];
			ImGui::SetCursorPos({scaleFactor * btn.rect.x, scaleFactor * btn.rect.y + y});
			ImGui::PushID(btn.kiko + 20);
			const ImVec2 button_size{scaleFactor * btn.rect.w, scaleFactor * btn.rect.h};
			const bool clicked = is_board_model
				? ImGui::InvisibleButton(btn.keyname.c_str(), button_size)
				: ImGui::Button(btn.keyname.c_str(), button_size);
			if (clicked) {
				btninfo = &btn;
				strncpy(buffer, btn.keyname.c_str(), sizeof(buffer) - 1);
				buffer[sizeof(buffer) - 1] = '\0';
				SDL_itoa(btn.kiko, buffer2, 16);
			}
			if (is_board_model && btninfo == &btn) {
				auto min_p = ImGui::GetItemRectMin();
				auto max_p = ImGui::GetItemRectMax();
				ImGui::GetWindowDrawList()->AddRect(min_p, max_p, IM_COL32(255, 0, 0, 255), 0.0f, 0, 2.0f);
			}
			ImGui::PopID();
		}
		ImGui::SetCursorPos({400, y});
		if (ImGui::BeginChild("Model Info")) {
			if (ImGui::BeginTabBar("ModelEditorTabs")) {
				if (ImGui::BeginTabItem("General")) {
					ImGui::TextUnformatted("ModelEditor.Name"_lc);
					if (ImGui::InputText("##name", name, 260)) {
						mi.model_name = name;
					}
					ImGui::TextUnformatted("ModelEditor.InterfacePath"_lc);
					if (ImGui::InputText("##path1", path1, 260)) {
						mi.interface_path = path1;
					}
					ImGui::TextUnformatted("ModelEditor.RomPath"_lc);
					if (ImGui::InputText("##path2", path2, 260)) {
						mi.rom_path = path2;
					}
					ImGui::TextUnformatted("ModelEditor.FlashDumpPath"_lc);
					if (ImGui::InputText("##path3", path3, 260)) {
						mi.flash_path = path3;
					}
					ImGui::TextUnformatted("ModelEditor.CsrMask"_lc);
					if (ImGui::SliderInt("##a", &v, 0, 15, "0x%X")) {
						mi.csr_mask = v;
					}
					ImGui::TextUnformatted("ModelEditor.PdValue"_lc);
					if (ImGui::SliderInt("##q", &k, 0, 15, "0x%X")) {
						mi.pd_value = k;
					}
					ImGui::TextUnformatted("ModelEditor.HardwareType"_lc);
					ImGui::SetNextItemWidth(80);
					const auto* selected_descriptor = casioemu::FindHardwareDescriptor(mi.hardware_id);
					if (ImGui::BeginCombo("##cb", selected_descriptor ? selected_descriptor->display_name : "Invalid")) {
						for (const auto& descriptor : casioemu::HARDWARE_DESCRIPTORS) {
							bool is_selected = (mi.hardware_id == descriptor.hardware_id);
							if (ImGui::Selectable(descriptor.display_name, is_selected)) {
								mi.hardware_id = descriptor.hardware_id;
							}
							if (is_selected)
								ImGui::SetItemDefaultFocus();
						}
						ImGui::EndCombo();
					}
					ImGui::Checkbox("ModelEditor.SampleRom"_lc, &mi.is_sample_rom);
					ImGui::Checkbox("ModelEditor.NewRenderMethod"_lc, &mi.enable_new_screen);
					ImGui::Checkbox("ModelEditor.RealRom"_lc, &mi.real_hardware);
					ImGui::Checkbox("ModelEditor.LegacyKO"_lc, &mi.legacy_ko);
					if (ImGui::ColorEdit3("ModelEditor.InkColor"_lc, color)) {
						mi.ink_color.r = color[0] * 255;
						mi.ink_color.g = color[1] * 255;
						mi.ink_color.b = color[2] * 255;
					}
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Buttons")) {
					if (is_board_model) {
						if (btninfo) {
							ImGui::TextUnformatted("Button geometry is read from board.svg.");
							if (ImGui::InputText("ModelEditor.KeyName"_lc, buffer, 260)) {
								btninfo->keyname = buffer;
							}
							if (ImGui::InputText("ModelEditor.KIKO"_lc, buffer2, 12)) {
								btninfo->kiko = SDL_strtol(buffer2, 0, 16);
							}
						}
						else {
							ImGui::Text("Select a button from the preview to edit.");
						}
					}
					else if (btninfo) {
						if (ImGui::InputText("ModelEditor.KeyName"_lc, buffer, 260)) {
							btninfo->keyname = buffer;
						}
						if (ImGui::InputText("ModelEditor.KIKO"_lc, buffer2, 12)) {
							btninfo->kiko = SDL_strtol(buffer2, 0, 16);
						}
						ImGui::InputInt("X", &btninfo->rect.x);
						ImGui::InputInt("Y", &btninfo->rect.y);
						ImGui::InputInt("W", &btninfo->rect.w);
						ImGui::InputInt("H", &btninfo->rect.h);
					}
					else {
						ImGui::Text("Select a button from the preview to edit.");
					}
					ImGui::EndTabItem();
				}
				if (!is_board_model && ImGui::BeginTabItem("Sprites")) {
					ImGui::Text("Sprite List");
					ImGui::BeginChild("SpriteList", {0, 150}, true);

					for (auto& [key, sprite] : mi.sprites) {
						if (ImGui::Selectable(key.c_str(), selected_sprite_key == key)) {
							selected_sprite_key = key;
						}
					}
					ImGui::EndChild();

					if (!selected_sprite_key.empty() && mi.sprites.count(selected_sprite_key)) {
						auto& sprite = mi.sprites[selected_sprite_key];
						ImGui::Text("Selected: %s", selected_sprite_key.c_str());
						ImGui::Separator();
						ImGui::Text("Source Rect");
						ImGui::InputInt("Src X", &sprite.src.x);
						ImGui::InputInt("Src Y", &sprite.src.y);
						ImGui::InputInt("Src W", &sprite.src.w);
						ImGui::InputInt("Src H", &sprite.src.h);

						ImGui::Text("Dest Rect");
						ImGui::InputInt("Dest X", &sprite.dest.x);
						ImGui::InputInt("Dest Y", &sprite.dest.y);
						ImGui::InputInt("Dest W", &sprite.dest.w);
						ImGui::InputInt("Dest H", &sprite.dest.h);
					}
					ImGui::EndTabItem();
				}
				if (is_board_model && ImGui::BeginTabItem("Status")) {
					if (mi.status_sprite_indexes.empty()) {
						ImGui::TextUnformatted("No status sprites configured.");
					}
					else {
						for (auto& [key, index] : mi.status_sprite_indexes) {
							ImGui::PushID(key.c_str());
							ImGui::TextUnformatted(key.c_str());
							ImGui::SameLine(160.0f);
							ImGui::SetNextItemWidth(80.0f);
							ImGui::InputInt("Index", &index);
							ImGui::PopID();
						}
					}
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}

			ImGui::Separator();
			if (ImGui::Button("Button.Save"_lc)) {
				std::set<int> used_kiko;
				bool duplicate_kiko = false;
				for (const auto& button : mi.buttons) {
					if (!used_kiko.insert(button.kiko).second) {
						duplicate_kiko = true;
						break;
					}
				}
				if (duplicate_kiko) {
					SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", "Duplicate KIKO values are not allowed.", nullptr);
					return;
				}
				casioemu::SaveModelInfoJson(pth, mi);
				this->open = false;
			}
		}
		ImGui::EndChild();
	}
};

inline char* stristr(const char* str1, const char* str2) {
	const char* p1 = str1;
	const char* p2 = str2;
	const char* r = *p2 == 0 ? str1 : 0;

	while (*p1 != 0 && *p2 != 0) {
		if (tolower((unsigned char)*p1) == tolower((unsigned char)*p2)) {
			if (r == 0) {
				r = p1;
			}

			p2++;
		}
		else {
			p2 = str2;
			if (r != 0) {
				p1 = r + 1;
			}

			if (tolower((unsigned char)*p1) == tolower((unsigned char)*p2)) {
				r = p1;
				p2++;
			}
			else {
				r = 0;
			}
		}

		p1++;
	}

	return *p2 == 0 ? (char*)r : 0;
}
inline std::string tohex(int n, int len) {
	std::string retval = "";
	for (int x = 0; x < len; x++) {
		retval = "0123456789ABCDEF"[n & 0xF] + retval;
		n >>= 4;
	}
	return retval;
}
inline std::string tohex(unsigned long long n, int len) {
	std::string retval = "";
	for (int x = 0; x < len; x++) {
		retval = "0123456789ABCDEF"[n & 0xF] + retval;
		n >>= 4;
	}
	return retval;
}
inline std::string sanitize_path(const std::string& input) {
	std::string result;
	for (unsigned char c : input) {
		// Allow ASCII alphanumeric, underscore, hyphen, dot, and space
		if ((c >= 'a' && c <= 'z') ||
			(c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') ||
			c == '_' || c == '-' || c == '.' || c == ' ') {
			result += c;
		}
		// Replace non-ASCII characters and unsafe characters with underscore
		else if ((unsigned char)c > 127 || c < 32) {
			result += '_';
		}
	}
	// Remove leading/trailing spaces and dots
	size_t start = result.find_first_not_of(" .");
	size_t end = result.find_last_not_of(" .");
	if (start != std::string::npos) {
		result = result.substr(start, end - start + 1);
	}
	else {
		result = "unnamed"; // Fallback if string becomes empty
	}
	// Collapse consecutive underscores
	size_t pos = 0;
	while ((pos = result.find("__", pos)) != std::string::npos) {
		result.erase(pos, 1);
	}
	return result;
}
#ifdef _WIN32
static bool CreateDesktopShortcut(const std::filesystem::path& model_path, const std::string& shortcut_name, const std::string& icon_path_str) {
	// Get the desktop path
	wchar_t desktopPath[MAX_PATH];
	if (FAILED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desktopPath))) {
		std::cerr << "[Shortcut] Failed to get desktop path\n";
		return false;
	}

	// Get the executable path
	wchar_t exePath[MAX_PATH];
	GetModuleFileNameW(NULL, exePath, MAX_PATH);

	// Get the working directory (exe directory)
	std::filesystem::path exe_dir = std::filesystem::path(exePath).parent_path();

	// Build the argument: the model path (absolute)
	std::filesystem::path abs_model = std::filesystem::absolute(model_path);
	std::wstring arguments = L"\"" + abs_model.wstring() + L"\"";

	// Build shortcut file path
	std::wstring sanitized_name(shortcut_name.begin(), shortcut_name.end());
	std::wstring lnkPath = std::wstring(desktopPath) + L"\\" + sanitized_name + L".lnk";

	IShellLinkW* psl = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&psl);
	if (FAILED(hr)) {
		std::cerr << "[Shortcut] CoCreateInstance failed\n";
		return false;
	}

	psl->SetPath(exePath);
	psl->SetArguments(arguments.c_str());
	psl->SetWorkingDirectory(exe_dir.wstring().c_str());
	psl->SetDescription(L"CasioEmu Model Shortcut");

	// Set custom icon if provided
	if (!icon_path_str.empty()) {
		std::filesystem::path icon_abs = std::filesystem::absolute(icon_path_str);
		if (std::filesystem::exists(icon_abs)) {
			psl->SetIconLocation(icon_abs.wstring().c_str(), 0);
		}
	}

	IPersistFile* ppf = nullptr;
	hr = psl->QueryInterface(IID_IPersistFile, (void**)&ppf);
	if (SUCCEEDED(hr)) {
		hr = ppf->Save(lnkPath.c_str(), TRUE);
		ppf->Release();
	}
	psl->Release();

	if (FAILED(hr)) {
		std::cerr << "[Shortcut] Failed to save shortcut\n";
		return false;
	}
	return true;
}
#elif defined(__ANDROID__)
static bool CreateDesktopShortcut(const std::filesystem::path& model_path, const std::string& shortcut_name, const std::string& icon_path_str) {
	JNIEnv* env = nullptr;
	if (!GetJNIEnv(&env)) {
		std::cerr << "[Shortcut] Failed to get JNI environment\n";
		return false;
	}

	jobject activity = (jobject)SDL_AndroidGetActivity();
	if (!activity) {
		std::cerr << "[Shortcut] Failed to get Android activity\n";
		return false;
	}

	jclass gameClass = env->FindClass("com/tele/u8emulator/Game");
	if (!gameClass) {
		env->DeleteLocalRef(activity);
		std::cerr << "[Shortcut] Failed to find Game class\n";
		return false;
	}

	jmethodID method = env->GetStaticMethodID(gameClass, "createModelShortcut",
		"(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
	if (!method) {
		env->DeleteLocalRef(gameClass);
		env->DeleteLocalRef(activity);
		std::cerr << "[Shortcut] Failed to find createModelShortcut method\n";
		return false;
	}

	std::filesystem::path abs_model = std::filesystem::absolute(model_path);
	jstring jModelPath = env->NewStringUTF(abs_model.string().c_str());
	jstring jName = env->NewStringUTF(shortcut_name.c_str());
	jstring jIconPath = env->NewStringUTF(icon_path_str.c_str());

	env->CallStaticVoidMethod(gameClass, method, jModelPath, jName, jIconPath);

	env->DeleteLocalRef(jIconPath);
	env->DeleteLocalRef(jName);
	env->DeleteLocalRef(jModelPath);
	env->DeleteLocalRef(gameClass);
	env->DeleteLocalRef(activity);
	return true;
}
#elif defined(__linux__)
static bool CreateDesktopShortcut(const std::filesystem::path& model_path, const std::string& shortcut_name, const std::string& icon_path_str) {
	// Get the desktop directory
	std::filesystem::path desktop_dir;
	const char* xdg_desktop = std::getenv("XDG_DESKTOP_DIR");
	if (xdg_desktop) {
		desktop_dir = xdg_desktop;
	}
	else {
		const char* home = std::getenv("HOME");
		if (!home) {
			std::cerr << "[Shortcut] HOME environment variable not set\n";
			return false;
		}
		desktop_dir = std::filesystem::path(home) / "Desktop";
	}

	if (!std::filesystem::exists(desktop_dir)) {
		std::filesystem::create_directories(desktop_dir);
	}

	// Get executable path
	std::filesystem::path exe_path = std::filesystem::canonical("/proc/self/exe");
	std::filesystem::path abs_model = std::filesystem::absolute(model_path);

	std::string sanitized = shortcut_name;
	for (auto& c : sanitized) {
		if (c == '/' || c == '\\')
			c = '_';
	}

	std::filesystem::path shortcut_file = desktop_dir / (sanitized + ".desktop");

	std::ofstream ofs(shortcut_file);
	if (!ofs) {
		std::cerr << "[Shortcut] Failed to create .desktop file\n";
		return false;
	}

	ofs << "[Desktop Entry]\n";
	ofs << "Type=Application\n";
	ofs << "Name=" << shortcut_name << "\n";
	ofs << "Exec=\"" << exe_path.string() << "\" \"" << abs_model.string() << "\"\n";
	ofs << "Path=" << exe_path.parent_path().string() << "\n";
	if (!icon_path_str.empty()) {
		std::filesystem::path icon_abs = std::filesystem::absolute(icon_path_str);
		if (std::filesystem::exists(icon_abs)) {
			ofs << "Icon=" << icon_abs.string() << "\n";
		}
	}
	ofs << "Terminal=false\n";
	ofs << "Categories=Game;Emulator;\n";
	ofs.close();

	// Make executable
	std::filesystem::permissions(shortcut_file,
		std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
		std::filesystem::perm_options::add);

	return true;
}
#elif defined(__APPLE__)
static bool CreateDesktopShortcut(const std::filesystem::path& model_path, const std::string& shortcut_name, const std::string& icon_path_str) {
	std::cerr << "[Shortcut] macOS shortcut creation not implemented yet\n";
	return false;
}
#endif

namespace casioemu {
	namespace {
		const char* L(const char* key, const char* english) {
			const char* value = g_local.GetCStr(key);
			return std::strcmp(value, key) == 0 ? english : value;
		}

		constexpr float kStartupChromeHeight = 28.0f;
		constexpr float kStartupDialogRounding = 10.0f;
		constexpr float kStartupDialogPadding = 14.0f;

		inline ImVec4 WithAlphaColor(ImVec4 color, float alpha) {
			color.w = alpha;
			return color;
		}

		void PushStartupDialogStyle(const plugin::ThemePalette& palette) {
			ImGui::PushStyleColor(ImGuiCol_WindowBg, palette.surface);
			ImGui::PushStyleColor(ImGuiCol_PopupBg, palette.surface);
			ImGui::PushStyleColor(ImGuiCol_Border, palette.borderStrong);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, kStartupDialogRounding);
			ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, kStartupDialogRounding);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kStartupDialogPadding, kStartupDialogPadding));
		}

		void PopStartupDialogStyle() {
			ImGui::PopStyleVar(4);
			ImGui::PopStyleColor(3);
		}

		void DrawStartupDialogHeader(const plugin::ThemePalette& palette, const char* title, bool* open,
				bool close_enabled = true) {
			ImDrawList* draw = ImGui::GetWindowDrawList();
			const ImVec2 window_pos = ImGui::GetWindowPos();
			const float width = ImGui::GetWindowWidth();
			const float height = 34.0f;
			const float text_h = ImGui::GetTextLineHeight();
			draw->AddText(ImVec2(window_pos.x + kStartupDialogPadding,
				window_pos.y + (height - text_h) * 0.5f),
				ImGui::ColorConvertFloat4ToU32(palette.foreground), title);
			const float close_size = 22.0f;
			ImGui::SetCursorPos(ImVec2(width - kStartupDialogPadding - close_size, 6.0f));
			const bool clicked = ImGui::InvisibleButton("##startup_dialog_close", ImVec2(close_size, close_size));
			const bool hovered = ImGui::IsItemHovered();
			const ImVec2 close_min = ImGui::GetItemRectMin();
			const ImVec2 close_max = ImGui::GetItemRectMax();
			if (hovered)
				draw->AddRectFilled(close_min, close_max,
					ImGui::ColorConvertFloat4ToU32(palette.hover), 4.0f);
			const ImU32 color = ImGui::ColorConvertFloat4ToU32(
				!close_enabled ? WithAlphaColor(palette.muted, 0.4f) : (hovered ? palette.foreground : palette.muted));
			const float inset = 6.5f;
			draw->AddLine(ImVec2(close_min.x + inset, close_min.y + inset),
				ImVec2(close_max.x - inset, close_max.y - inset), color, 1.4f);
			draw->AddLine(ImVec2(close_max.x - inset, close_min.y + inset),
				ImVec2(close_min.x + inset, close_max.y - inset), color, 1.4f);
			draw->AddLine(ImVec2(window_pos.x, window_pos.y + height),
				ImVec2(window_pos.x + width, window_pos.y + height),
				ImGui::ColorConvertFloat4ToU32(palette.border), 1.0f);
			ImGui::SetCursorPos(ImVec2(kStartupDialogPadding, height + 10.0f));
			if (clicked && close_enabled) {
				if (open)
					*open = false;
				ImGui::CloseCurrentPopup();
			}
		}

		bool BeginStartupModal(const plugin::ThemePalette& palette, const char* id, const char* title,
				bool* open, const ImVec2& size) {
			ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
			PushStartupDialogStyle(palette);
			if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
					ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking)) {
				PopStartupDialogStyle();
				return false;
			}
			DrawStartupDialogHeader(palette, title, open);
			return true;
		}

		void EndStartupModal() {
			ImGui::EndPopup();
			PopStartupDialogStyle();
		}
	} // namespace

	class StartupUi {
	public:
		std::map<std::array<char, 8>, std::string> RomNames;
		struct Model {
		public:
			std::filesystem::path path;
			std::string name;
			std::string version;
			std::string type;
			std::string id;
			std::string checksum;
			std::string checksum2;
			std::string sum_good;
			bool realhw;
			bool show_sum = true;
		};
		std::vector<Model> models;
		std::filesystem::path selected_path{};
		std::shared_ptr<ModelResourceStore> selected_resources;
		char online_api[512] = "";
		bool show_online_popup = false;
		OnlineAuthRequest online_auth{};
		std::unique_ptr<OnlineLoopbackServer> online_loopback;
		std::string online_access_token;
		bool online_authorization_pending = false;
		Uint64 online_authorization_deadline = 0;
		Uint64 online_browser_open_at = 0;
		std::string online_browser_uri;
		std::string online_approval_grant;
		std::string online_status;
		std::vector<OnlineModelEntry> online_models;
		int selected_online_model = -1;
		char online_search_txt[200]{};
		char online_filter[32] = "##";
		bool online_hide_emu = false;
		bool close_online_popup_requested = false;
		enum class OnlineOperation { Idle, StartLogin, LoadModels, PollAuthorization, Logout, DownloadModel };
		struct OnlineTaskResult {
			OnlineOperation operation = OnlineOperation::Idle;
			OnlineAuthRequest auth{};
			std::string access_token;
			std::vector<OnlineModelEntry> models;
			std::vector<uint8_t> archive;
			std::string model_id;
			std::string error;
			bool authentication_error = false;
		};
		OnlineOperation online_operation = OnlineOperation::Idle;
		std::future<OnlineTaskResult> online_task;
		UpdateChecker update_checker;
		UpdateInfo update_info;
		bool show_update = false;
		bool update_result_consumed = false;
		StartupUi() {
			// The update check reaches out to GitHub, so it only runs when the
			// user left the setting enabled (Theme > Integrations).  Headless
			// verification modes skip it so a notification cannot cover the
			// window under test.
			if (ThemeManager::Instance().Settings().enableUpdateChecker &&
				!std::getenv("CASIOEMU_SHOT_STARTUP") && !std::getenv("CASIOEMU_SHOW_ONLINE"))
				update_checker.Start();
			std::ifstream api_settings{"online_api.cfg"};
			if (api_settings) {
				std::string value;
				std::getline(api_settings, value);
				if (!value.empty()) std::snprintf(online_api, sizeof(online_api), "%s", value.c_str());
			}
			try {
				OnlineModelClient client{online_api};
				online_access_token = LoadOnlineToken(client.ApiBase());
			}
			catch (...) {
				online_access_token.clear();
			}
			std::ifstream ifs2{"roms.db", std::ifstream::binary};
			if (ifs2) {
				try {
					Binary::Read(ifs2, RomNames);
				}
				catch (const std::exception& e) {
					std::cerr << "[StartupUI][Warn] Failed to read \"roms.db\": " << e.what() << ". Names may be inaccurate!\n";
				}
			}
			else {
				printf("[StartupUI][Warn] \"roms.db\" not found. Names may be inaccurate!\n");
			}
			Reload();
			settings_window.reset(MakeSettingsWindow());
		}
		~StartupUi() {
			if (reload_thread.joinable())
				reload_thread.join();
		}
		void Reload() {
			if (reload_thread.joinable())
				reload_thread.join();
			loading = true;
			std::error_code ec;
			const std::filesystem::path models_dir = startup::DefaultModelsDirectory();
			std::filesystem::create_directories(models_dir, ec);
			reload_thread = std::thread([this, models_dir]() {
				models.clear();
				std::error_code iter_ec;
				for (auto& dir : std::filesystem::directory_iterator(models_dir, iter_ec)) {
					if (dir.path().filename() == ".online") continue;
					if (dir.is_directory()) {
						try {
							printf("[StartupUI][Info] Checking %s\n", dir.path().string().c_str());
							std::error_code ec;
							std::string load_error;
							ModelInfo mi{};
							if (!LoadModelInfoFromFolder(dir.path(), mi, nullptr, &load_error)) {
								printf("[StartupUI][Info] Unable to load model configuration for %s: %s\n", dir.path().string().c_str(), load_error.c_str());
								continue;
							}
							Model mod{};
							mod.path = dir;
							mod.name = mi.model_name;
							mod.realhw = mi.real_hardware;
							mod.type = HardwareStartupFilter(mi.hardware_id);
							{
								std::filesystem::path romPath = dir.path() / mi.rom_path;
								if (mi.rom_path.empty() || !std::filesystem::exists(romPath) || !std::filesystem::is_regular_file(romPath, ec))
									continue;
								std::ifstream ifs2(romPath, std::ios::in | std::ios::binary);
								if (!ifs2)
									continue;
								std::vector<byte> rom{std::istreambuf_iterator<char>{ifs2.rdbuf()}, std::istreambuf_iterator<char>{}};
								ifs2.close();
								std::vector<byte> flash{};

								if (!mi.flash_path.empty()) {
									std::filesystem::path flashPath = dir.path() / mi.flash_path;
									if (std::filesystem::exists(flashPath) && std::filesystem::is_regular_file(flashPath, ec)) {
										std::ifstream ifs3(flashPath, std::ios::in | std::ios::binary);
										if (ifs3)
											flash = {std::istreambuf_iterator<char>{ifs3.rdbuf()}, std::istreambuf_iterator<char>{}};
									}
								}
								auto ri = rom_info(rom, flash, mi.real_hardware);
								if (ri.type != 0) {
									switch (ri.type) {
									case RomInfo::ES:
										mod.type = "ES";
										break;
									case RomInfo::ESP:
										mod.type = "ESP";
										break;
									case RomInfo::ESP2nd:
										mod.type = "ESP2nd";
										break;
									case RomInfo::CWX:
										mod.type = "CWX";
										break;
									case RomInfo::CWII:
										mod.type = "CWII";
										break;
									case RomInfo::Fx5800p:
										mod.type = "Fx5800p";
										break;
									default:
										mod.type = "???";
										break;
									}
								}
								if (ri.ok) {
									mod.version = ri.ver;
									std::array<char, 8> key{};
									memcpy(key.data(), mod.version.data(), 6);
									auto iter = RomNames.find(key);
									if (iter != RomNames.end())
										mod.name = iter->second;
									mod.checksum = tohex(ri.real_sum, 4);
									mod.checksum2 = tohex(ri.desired_sum, 4);
									mod.sum_good = ri.real_sum == ri.desired_sum ? "OK" : "NG";
									// Safely form version key and id
									std::array<char, 8> key2{};
									std::memset(key2.data(), 0, key2.size());
									std::memcpy(key2.data(), mod.version.data(), std::min<std::size_t>(6, mod.version.size()));
									mod.id = tohex(*(unsigned long long*)ri.cid, 8);
									if (ri.type == RomInfo::ES) {
										auto a = get_pd(mi.pd_value);
										mod.version += std::string(" (P") + a + ")";
									}
								}
								else {
									mod.show_sum = false;
								}
								printf("[StartupUI][Debug] Model Summary\n"
									   "[StartupUI][Debug] Name: %s\n"
									   "[StartupUI][Debug] Type: %s\n",
									mod.name.c_str(), mod.type.c_str());
							}
							models.push_back(mod);
						}
						catch (const std::exception& e) {
							std::cerr << "[StartupUI][Error] Failed to load model from " << dir.path().string() << ": " << e.what() << std::endl;
							continue;
						}
					}
				}
				loading = false;
			});
		}
		std::vector<std::string> recently_used{};
		char search_txt[200]{};
		const char* current_filter = "##";
		bool not_show_emu = false;
		std::atomic<bool> loading{false};
		std::thread reload_thread;
		bool settings_page = false;
		std::unique_ptr<UIWindow> settings_window;
		bool show_shortcut_popup = false;
		std::filesystem::path shortcut_model_path{};
		char shortcut_name[260]{};
		char shortcut_icon[260]{};
		char password[256]{};
		bool show_password_input = false;
		std::filesystem::path current_file{};
		RomPackage current_rp{};
		bool password_error = false;

		// Model package import (Import... button and the encrypted-password flow).
		std::filesystem::path pending_import_package{};
		std::string import_status;
		bool import_error = false;
		bool import_busy = false;
		std::future<startup::ModelImportResult> import_task;
		bool show_import_result = false;
		startup::ModelImportResult last_import{};
		bool import_password_pending = false;

		inline std::string generate_random_string(size_t length) {
			return util::Random::random_string(length);
		}
		inline std::string create_unique_directory(const std::string& base_name) {
			std::string sanitized_name = sanitize_path(base_name);
			std::string dir_name = sanitized_name;
			while (std::filesystem::exists("./models/" + dir_name)) {
				dir_name = sanitized_name + "." + generate_random_string(6); // TODO: WTF is this dot for? Just add random string directly
			}
			return dir_name;
		}

		void SaveOnlineApiAddress() {
			std::ofstream settings{"online_api.cfg", std::ios::trunc};
			if (settings) settings << online_api;
		}

		void BeginImportModelPackage(const std::filesystem::path& package_path) {
			if (import_busy)
				return;
			pending_import_package = package_path;
			import_busy = true;
			import_error = false;
			import_password_pending = false;
			password[0] = '\0';
			password_error = false;
			import_status = "Importing " + package_path.filename().string() + "...";
			const std::filesystem::path models_dir = startup::DefaultModelsDirectory();
			import_task = std::async(std::launch::async, [package_path, models_dir] {
				return startup::ImportModelPackage(package_path, models_dir);
			});
		}

		void RetryImportModelPackage(const std::string& key) {
			if (import_busy || pending_import_package.empty())
				return;
			import_busy = true;
			import_error = false;
			import_password_pending = false;
			import_status = "Decrypting and importing " + pending_import_package.filename().string() + "...";
			const std::filesystem::path package_path = pending_import_package;
			const std::filesystem::path models_dir = startup::DefaultModelsDirectory();
			import_task = std::async(std::launch::async, [package_path, models_dir, key] {
				return startup::ImportModelPackage(package_path, models_dir, key);
			});
		}

		void PollImportTask() {
			if (!import_busy || !import_task.valid() ||
				import_task.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
				return;
			startup::ModelImportResult result = import_task.get();
			import_busy = false;
			last_import = result;
			if (result.needsPassword) {
				import_password_pending = true;
				password[0] = '\0';
				password_error = false;
				import_status = "Encrypted package: enter its password.";
				return;
			}
			show_import_result = true;
			if (result.ok) {
				import_error = false;
				import_status = "Imported " + result.modelName + " (" +
					std::to_string(result.files.size()) + " files)";
				Reload();
			}
			else {
				import_error = true;
				import_status = "Import failed: " + result.error;
			}
		}

		void RunMassRoundTripTest(const std::filesystem::path& scratch, const std::filesystem::path& report_path) {
			std::error_code ec;
			std::filesystem::create_directories(scratch, ec);
			const std::filesystem::path source = startup::DefaultModelsDirectory();
			const char* names[] = { "fx-580VN X", "fx-880BTG" };
			bool all_ok = true;
			std::ostringstream report;
			for (int mode = 0; mode < 2; ++mode) {
				const std::string mode_name = mode == 0 ? "fixed" : "per-model";
				const auto export_dir = scratch / (mode_name + "-packages");
				const auto models_dir = scratch / (mode_name + "-models");
				std::filesystem::remove_all(export_dir, ec);
				std::filesystem::remove_all(models_dir, ec);
				std::filesystem::create_directories(export_dir, ec);
				std::filesystem::create_directories(models_dir, ec);
				const std::string passwords[] = {
					mode == 0 ? "roundtrip" : "pass580",
					mode == 0 ? "roundtrip" : "pass880",
				};
				for (int i = 0; i < 2; ++i) {
					const std::filesystem::path folder = source / names[i];
					const std::filesystem::path package = export_dir / (sanitize_path(names[i]) + ".package");
					std::string error;
					bool ok = ExportPackageForModel(folder, package, passwords[i], error);
					if (ok) {
						auto imported = startup::ImportModelPackage(package, models_dir, passwords[i]);
						ok = imported.ok;
						error = imported.error;
					}
					report << mode_name << " " << names[i] << "=" << (ok ? "PASS" : "FAIL");
					if (!ok) {
						all_ok = false;
						report << " (" << error << ")";
					}
					report << "\n";
				}
			}
			std::ofstream out(report_path, std::ios::trunc);
			if (out) {
				out << report.str();
				out << (all_ok ? "RESULT PASS\n" : "RESULT FAIL\n");
			}
		}

		void RunImportSelfTest(const std::filesystem::path& package_path, const std::filesystem::path& report_path) {
			std::error_code ec;
			if (reload_thread.joinable())
				reload_thread.join();
			loading = false;
			const std::filesystem::path models_dir = startup::DefaultModelsDirectory();
			if (!std::getenv("CASIOEMU_MODELS_DIR")) {
				std::ofstream report(report_path, std::ios::trunc);
				if (report)
					report << "RESULT FAIL\nCASIOEMU_MODELS_DIR is required for the import self-test.\n";
				return;
			}
			std::filesystem::remove_all(models_dir, ec);
			const char* password = std::getenv("CASIOEMU_SELFTEST_PACKAGE_PASSWORD");
			startup::ModelImportResult result = startup::ImportModelPackage(package_path, models_dir,
				password ? password : std::string());
			ModelInfo registered{};
			std::string load_error;
			const bool registered_ok = result.ok &&
				LoadModelInfoFromFolder(result.targetPath, registered, nullptr, &load_error);
			std::ofstream report(report_path, std::ios::trunc);
			if (!report)
				return;
			report << "package=" << package_path.string() << '\n';
			report << "models_dir=" << models_dir.string() << '\n';
			report << "import=" << (result.ok ? "PASS" : "FAIL") << '\n';
			report << "encrypted=" << (result.encrypted ? "yes" : "no") << '\n';
			report << "needs_password=" << (result.needsPassword ? "yes" : "no") << '\n';
			report << "model_name=" << result.modelName << '\n';
			report << "target=" << result.targetPath << '\n';
			report << "files=";
			for (size_t i = 0; i < result.files.size(); ++i)
				report << (i ? "," : "") << result.files[i];
			report << '\n';
			report << "registered=" << (registered_ok ? "PASS" : "FAIL") << '\n';
			if (!result.ok)
				report << "error=" << result.error << '\n';
			if (!load_error.empty())
				report << "load_error=" << load_error << '\n';
			const bool expect_password_prompt = std::getenv("CASIOEMU_SELFTEST_EXPECT_PASSWORD") != nullptr;
			const bool prompt_ok = expect_password_prompt && result.needsPassword;
			report << "password_prompt=" << (prompt_ok ? "PASS" : "FAIL") << '\n';
			report << ((result.ok && registered_ok) || prompt_ok ? "RESULT PASS\n" : "RESULT FAIL\n");
			report.close();
			if (result.ok)
				Reload();
		}

		bool OnlineBusy() const {
			return online_operation != OnlineOperation::Idle;
		}

		void BeginLoadOnlineModels() {
			if (OnlineBusy() || online_access_token.empty()) return;
			const std::string api = online_api;
			const std::string token = online_access_token;
			online_operation = OnlineOperation::LoadModels;
			online_status = "StartupUI.OnlineLoading"_lc;
			online_task = std::async(std::launch::async, [api, token] {
				OnlineTaskResult result;
				result.operation = OnlineOperation::LoadModels;
				try { result.models = OnlineModelClient{api}.ListModels(token); }
				catch (const OnlineAuthenticationError& error) { result.authentication_error = true; result.error = error.what(); }
				catch (const std::exception& error) { result.error = error.what(); }
				return result;
			});
		}

		void ClearOnlineSessionState() {
			if (online_loopback) {
				online_loopback->Stop();
				online_loopback.reset();
			}
			online_access_token.clear();
			online_authorization_pending = false;
			online_authorization_deadline = 0;
			online_browser_open_at = 0;
			online_browser_uri.clear();
			online_approval_grant.clear();
			online_auth = {};
			online_models.clear();
			selected_online_model = -1;
		}

		void InvalidateOnlineLogin() {
			ClearOnlineSessionState();
			ClearOnlineToken(OnlineModelClient{online_api}.ApiBase());
			online_status = "StartupUI.OnlineLoginExpired"_lc;
		}

		void BeginLogoutOnline() {
			if (OnlineBusy()) return;
			const std::string api = online_api;
			const std::string token = online_access_token;
			online_operation = OnlineOperation::Logout;
			online_status = "StartupUI.OnlineLoggingOut"_lc;
			online_task = std::async(std::launch::async, [api, token] {
				OnlineTaskResult result;
				result.operation = OnlineOperation::Logout;
				try { if (!token.empty()) OnlineModelClient{api}.RevokeDevice(token); }
				catch (...) {}
				return result;
			});
		}

		void BeginOnlineLogin() {
			if (OnlineBusy()) return;
#ifdef __ANDROID__
			ClearOnlineAuthorizationCallback();
#endif
			SaveOnlineApiAddress();
			OnlineModelClient client{online_api};
			ClearOnlineToken(client.ApiBase());
			ClearOnlineSessionState();
#ifdef __ANDROID__
			const std::string redirect_uri = "u8emu://online-auth";
#else
			auto loopback = std::make_unique<OnlineLoopbackServer>();
			const std::string redirect_uri = loopback->RedirectUri();
			online_loopback = std::move(loopback);
#endif
			const std::string api = online_api;
			online_operation = OnlineOperation::StartLogin;
			online_status = "StartupUI.OnlineStartingLogin"_lc;
			online_task = std::async(std::launch::async, [api, redirect_uri] {
				OnlineTaskResult result;
				result.operation = OnlineOperation::StartLogin;
				try { result.auth = OnlineModelClient{api}.StartAuthorization(redirect_uri); }
				catch (const std::exception& error) { result.error = error.what(); }
				return result;
			});
		}

		void BeginCompleteOnlineLogin() {
			if (OnlineBusy() || !online_authorization_pending) return;
			const std::string api = online_api;
			const std::string approval_grant = online_approval_grant;
			online_operation = OnlineOperation::PollAuthorization;
			online_status = "StartupUI.OnlineCompletingLogin"_lc;
			online_task = std::async(std::launch::async, [api, approval_grant] {
				OnlineTaskResult result;
				result.operation = OnlineOperation::PollAuthorization;
				try { OnlineModelClient{api}.PollAuthorization(approval_grant, result.access_token); }
				catch (const std::exception& error) { result.error = error.what(); }
				return result;
			});
		}

#ifdef __ANDROID__
		void ClearOnlineAuthorizationCallback() {
			auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
			jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
			if (!env || !activity) return;
			jclass activity_class = env->GetObjectClass(activity);
			jmethodID method = activity_class
				? env->GetMethodID(activity_class, "clearOnlineAuthorizationCallback", "()V")
				: nullptr;
			if (method) env->CallVoidMethod(activity, method);
			if (env->ExceptionCheck()) env->ExceptionClear();
			if (activity_class) env->DeleteLocalRef(activity_class);
			env->DeleteLocalRef(activity);
		}

		bool OpenOnlineAuthorization(const std::string& uri) {
			auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
			jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
			if (!env || !activity) return false;
			jclass activity_class = env->GetObjectClass(activity);
			jmethodID method = activity_class
				? env->GetMethodID(activity_class, "openOnlineAuthorization", "(Ljava/lang/String;)Z")
				: nullptr;
			jstring value = env->NewStringUTF(uri.c_str());
			const bool opened = method && value && env->CallBooleanMethod(activity, method, value) == JNI_TRUE;
			if (env->ExceptionCheck()) env->ExceptionClear();
			if (value) env->DeleteLocalRef(value);
			if (activity_class) env->DeleteLocalRef(activity_class);
			env->DeleteLocalRef(activity);
			return opened;
		}

		std::optional<std::string> ConsumeOnlineAuthorizationCallback() {
			auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
			jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
			if (!env || !activity) return std::nullopt;
			jclass activity_class = env->GetObjectClass(activity);
			jmethodID method = activity_class
				? env->GetMethodID(activity_class, "consumeOnlineAuthorizationCallback", "()Ljava/lang/String;")
				: nullptr;
			jstring grant = method ? static_cast<jstring>(env->CallObjectMethod(activity, method)) : nullptr;
			std::optional<std::string> result;
			if (grant) {
				const char* chars = env->GetStringUTFChars(grant, nullptr);
				if (chars) { result = chars; env->ReleaseStringUTFChars(grant, chars); }
				env->DeleteLocalRef(grant);
			}
			if (env->ExceptionCheck()) env->ExceptionClear();
			if (activity_class) env->DeleteLocalRef(activity_class);
			env->DeleteLocalRef(activity);
			return result;
		}
#endif

		void PollOnlineTask() {
			if (!OnlineBusy() || !online_task.valid()
				|| online_task.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
			OnlineTaskResult result = online_task.get();
			online_operation = OnlineOperation::Idle;
			if (result.authentication_error) {
				InvalidateOnlineLogin();
				return;
			}
			if (!result.error.empty()) {
				if ((result.operation == OnlineOperation::StartLogin || result.operation == OnlineOperation::PollAuthorization)
					&& online_loopback) {
					online_loopback->Stop();
					online_loopback.reset();
				}
				if (result.operation == OnlineOperation::PollAuthorization) {
					online_authorization_pending = false;
				}
				online_status = result.error;
				return;
			}
			switch (result.operation) {
			case OnlineOperation::StartLogin:
				online_auth = std::move(result.auth);
				online_authorization_pending = true;
				online_authorization_deadline = SDL_GetTicks64() + 600000;
				online_status = "StartupUI.OnlineBrowserOpened"_lc;
				online_browser_uri = online_auth.verification_uri;
				online_browser_open_at = SDL_GetTicks64() + 250;
				break;
			case OnlineOperation::LoadModels:
				online_models = std::move(result.models);
				selected_online_model = online_models.empty() ? -1 : 0;
				online_status = std::to_string(online_models.size()) + " " + std::string("StartupUI.OnlineModelsLoaded"_lc);
				break;
			case OnlineOperation::PollAuthorization:
				online_access_token = std::move(result.access_token);
				online_authorization_pending = false;
				online_authorization_deadline = 0;
				if (online_loopback) { online_loopback->Stop(); online_loopback.reset(); }
				SaveOnlineToken(OnlineModelClient{online_api}.ApiBase(), online_access_token);
				online_status = OnlineTokenPersistenceAvailable()
					? std::string("StartupUI.OnlineLoginSuccess"_lc)
					: std::string("StartupUI.OnlineLoginSessionOnly"_lc);
				BeginLoadOnlineModels();
				break;
			case OnlineOperation::Logout:
				ClearOnlineToken(OnlineModelClient{online_api}.ApiBase());
				ClearOnlineSessionState();
				online_status = "StartupUI.OnlineLoggedOut"_lc;
				break;
			case OnlineOperation::DownloadModel:
				try {
					selected_resources = LoadOnlineModelPackage(result.archive, result.model_id);
					selected_path = "memory-online-" + result.model_id;
					close_online_popup_requested = true;
				}
				catch (const std::exception& error) { online_status = error.what(); }
				break;
			default:
				break;
			}
		}

		void BeginDownloadOnlineModel() {
			if (OnlineBusy()) return;
			if (selected_online_model < 0 || selected_online_model >= static_cast<int>(online_models.size())
				|| !OnlineModelVisible(online_models[selected_online_model])) {
				online_status = "Select an online model first.";
				return;
			}
			const std::string api = online_api;
			const std::string token = online_access_token;
			const std::string model_id = online_models[selected_online_model].id;
			online_operation = OnlineOperation::DownloadModel;
			online_status = "StartupUI.OnlineDownloading"_lc;
			online_task = std::async(std::launch::async, [api, token, model_id] {
				OnlineTaskResult result;
				result.operation = OnlineOperation::DownloadModel;
				result.model_id = model_id;
				try { result.archive = OnlineModelClient{api}.DownloadModel(token, model_id); }
				catch (const OnlineAuthenticationError& error) { result.authentication_error = true; result.error = error.what(); }
				catch (const std::exception& error) { result.error = error.what(); }
				return result;
			});
		}

		bool OnlineModelVisible(const OnlineModelEntry& item) const {
			const bool matches_filter = std::strcmp(online_filter, "##") == 0 || item.model_type == online_filter;
			const bool matches_search = stristr(item.name.c_str(), online_search_txt) != nullptr || stristr(item.id.c_str(), online_search_txt) != nullptr;
			const bool matches_rom_kind = !online_hide_emu || item.real_hardware;
			return matches_filter && matches_search && matches_rom_kind;
		}

		bool ExportPackageForModel(const std::filesystem::path& model_folder,
				const std::filesystem::path& destination, const std::string& password, std::string& error) {
			try {
				RomPackage package;
				package.Load(model_folder);
				if (!password.empty())
					package.Encrypt(password);
				std::error_code ec;
				std::filesystem::create_directories(destination.parent_path(), ec);
				std::ofstream out(destination, std::ios::binary | std::ios::trunc);
				if (!out) {
					error = "cannot create " + destination.string();
					return false;
				}
				Binary::Write(out, package);
				return true;
			}
			catch (const std::exception& exception) {
				error = exception.what();
				return false;
			}
		}

		struct ImportEntry {
			std::filesystem::path path;
			bool folder = false;
			bool ok = false;
			bool skipped = false;
			std::string status = "Pending";
			char password[128]{};
		};
		struct ExportEntry {
			int model_index = -1;
			bool selected = false;
			std::string status = "Pending";
			char password[128]{};
		};
		bool show_import_dialog = false;
		bool show_export_dialog = false;
		std::vector<ImportEntry> import_entries;
		std::vector<ExportEntry> export_entries;
		bool import_per_model = false;
		bool export_per_model = false;
		char import_password[128]{};
		char export_password[128]{};
		char export_destination[512]{};
		std::string export_manifest;
		void AddImportPath(const std::filesystem::path& path) {
			std::error_code ec;
			if (std::filesystem::is_directory(path, ec)) {
				for (const auto& entry : std::filesystem::directory_iterator(path,
						std::filesystem::directory_options::skip_permission_denied, ec)) {
					if (entry.is_regular_file(ec) && entry.path().extension() == ".package") {
						ImportEntry item;
						item.path = entry.path();
						import_entries.push_back(std::move(item));
					}
				}
			}
			else if (std::filesystem::is_regular_file(path, ec)) {
				ImportEntry item;
				item.path = path;
				import_entries.push_back(std::move(item));
			}
		}
		void RunImportEntries() {
			const std::filesystem::path models_dir = startup::DefaultModelsDirectory();
			for (auto& entry : import_entries) {
				if (entry.ok || entry.skipped)
					continue;
				const std::string password = import_per_model ? entry.password : import_password;
				auto result = startup::ImportModelPackage(entry.path, models_dir, password);
				entry.ok = result.ok;
				entry.status = result.ok ? "Imported " + result.modelName : result.error;
			}
			Reload();
		}
		void RunExportEntries() {
			const std::filesystem::path destination(export_destination);
			if (destination.empty()) {
				export_manifest = "Choose a destination.";
				return;
			}
			std::error_code ec;
			std::filesystem::create_directories(destination, ec);
			std::ofstream manifest(destination / "export-manifest.txt", std::ios::trunc);
			manifest << "CasioEmuMsvc model export\n";
			for (auto& entry : export_entries) {
				if (!entry.selected || entry.model_index < 0 ||
					entry.model_index >= static_cast<int>(models.size()))
					continue;
				const auto& model = models[static_cast<size_t>(entry.model_index)];
				const std::string password = export_per_model ? entry.password : export_password;
				const std::string name = sanitize_path(model.name) + ".package";
				std::string error;
				const bool ok = ExportPackageForModel(model.path, destination / name, password, error);
				entry.status = ok ? "Exported " + name : error;
				manifest << (ok ? "OK " : "FAIL ") << name << "  " << model.path.string()
					<< (error.empty() ? std::string() : "  " + error) << "\n";
			}
			export_manifest = "Manifest: " + (destination / "export-manifest.txt").string();
		}
		void RenderMassImportDialog(const plugin::ThemePalette& palette) {
			if (!show_import_dialog) return;
			ImGui::OpenPopup("Import Models##mass_import");
			bool open = true;
			if (BeginStartupModal(palette, "Import Models##mass_import", L("Startup.ImportModelsTitle", "Import Models"), &open,
					ImVec2(760.0f, 560.0f))) {
				if (ImGui::Button(L("Startup.AddFiles", "Add Files..."), ImVec2(0, 26))) {
					SystemDialogs::OpenFileDialog([this](std::filesystem::path path) {
						if (!path.empty()) AddImportPath(path);
					});
				}
				ImGui::SameLine();
				if (ImGui::Button(L("Startup.AddFolder", "Add Folder..."), ImVec2(0, 26))) {
					SystemDialogs::OpenFolderDialog([this](std::filesystem::path path) {
						if (!path.empty()) AddImportPath(path);
					});
				}
				ImGui::SameLine();
				ImGui::Checkbox(L("Startup.PasswordPerModel", "Password per model"), &import_per_model);
				if (!import_per_model) {
					ImGui::SetNextItemWidth(260.0f);
					ImGui::InputTextWithHint("##import_fixed_password", L("Startup.FixedPassword", "Fixed password"), import_password,
						sizeof(import_password), ImGuiInputTextFlags_Password);
				}
				ImGui::Separator();
				ImGui::BeginChild("##import_entries", ImVec2(0, 380.0f), ImGuiChildFlags_Border);
				for (size_t index = 0; index < import_entries.size(); ++index) {
					auto& entry = import_entries[index];
					ImGui::PushID(static_cast<int>(index));
					ImGui::TextUnformatted(entry.path.filename().string().c_str());
					ImGui::SameLine(360.0f);
					ImGui::TextDisabled("%s", entry.status.c_str());
					if (import_per_model) {
						ImGui::SetNextItemWidth(220.0f);
						ImGui::InputText("##item_password", entry.password, sizeof(entry.password),
							ImGuiInputTextFlags_Password);
						ImGui::SameLine();
					}
					if (ImGui::SmallButton("Skip")) entry.skipped = true;
					ImGui::SameLine();
					if (ImGui::SmallButton("Retry")) { entry.ok = false; entry.skipped = false; entry.status = "Pending"; }
					ImGui::PopID();
				}
				if (import_entries.empty())
					ImGui::TextDisabled("%s", L("Startup.NoPackages", "No packages selected."));
				ImGui::EndChild();
				if (ImGui::Button(L("Startup.Import", "Import"), ImVec2(120, 26))) RunImportEntries();
				ImGui::SameLine();
				if (ImGui::Button(L("Startup.Cancel", "Cancel"), ImVec2(120, 26))) { show_import_dialog = false; ImGui::CloseCurrentPopup(); }
				EndStartupModal();
			}
			if (!open) show_import_dialog = false;
		}
		void RenderMassExportDialog(const plugin::ThemePalette& palette) {
			if (!show_export_dialog) return;
			ImGui::OpenPopup("Export Models##mass_export");
			bool open = true;
			if (BeginStartupModal(palette, "Export Models##mass_export", L("Startup.ExportModelsTitle", "Export Models"), &open,
					ImVec2(760.0f, 580.0f))) {
				if (export_entries.size() != models.size()) {
					std::vector<ExportEntry> rebuilt;
					for (int index = 0; index < static_cast<int>(models.size()); ++index) {
						ExportEntry entry;
						entry.model_index = index;
						rebuilt.push_back(entry);
					}
					export_entries = std::move(rebuilt);
				}
				if (ImGui::Button(L("Startup.ChooseDestination", "Choose Destination..."), ImVec2(0, 26))) {
					SystemDialogs::OpenFolderDialog([this](std::filesystem::path path) {
						std::snprintf(export_destination, sizeof(export_destination), "%s", path.string().c_str());
					});
				}
				ImGui::SameLine();
				ImGui::TextDisabled("%s", export_destination[0] ? export_destination : "(no destination)");
				ImGui::Checkbox(L("Startup.PasswordPerModel", "Password per model"), &export_per_model);
				if (!export_per_model) {
					ImGui::SetNextItemWidth(260.0f);
					ImGui::InputTextWithHint("##export_fixed_password", L("Startup.FixedPassword", "Fixed password"), export_password,
						sizeof(export_password), ImGuiInputTextFlags_Password);
				}
				ImGui::Separator();
				ImGui::BeginChild("##export_entries", ImVec2(0, 400.0f), ImGuiChildFlags_Border);
				for (auto& entry : export_entries) {
					if (entry.model_index < 0 || entry.model_index >= static_cast<int>(models.size())) continue;
					auto& model = models[static_cast<size_t>(entry.model_index)];
					ImGui::PushID(entry.model_index);
					ImGui::Checkbox(model.name.c_str(), &entry.selected);
					ImGui::SameLine(360.0f);
					ImGui::TextDisabled("%s", entry.status.c_str());
					if (export_per_model) {
						ImGui::SetNextItemWidth(220.0f);
						ImGui::InputText("##item_password", entry.password, sizeof(entry.password),
							ImGuiInputTextFlags_Password);
					}
					ImGui::PopID();
				}
				ImGui::EndChild();
				if (!export_manifest.empty()) ImGui::TextDisabled("%s", export_manifest.c_str());
				if (ImGui::Button(L("Startup.Export", "Export"), ImVec2(120, 26))) RunExportEntries();
				ImGui::SameLine();
				if (ImGui::Button(L("Startup.Cancel", "Cancel"), ImVec2(120, 26))) { show_export_dialog = false; ImGui::CloseCurrentPopup(); }
				EndStartupModal();
			}
			if (!open) show_export_dialog = false;
		}

		void RenderOnlineModels() {
			const plugin::ThemePalette& palette = ThemeManager::Instance().Colors();
			if (show_online_popup) {
				ImGui::OpenPopup("StartupUI.OnlineModels"_lc);
				show_online_popup = false;
				BeginLoadOnlineModels();
			}
			PollOnlineTask();
			const Uint64 online_now = SDL_GetTicks64();
			if (!online_browser_uri.empty() && online_now >= online_browser_open_at) {
				const std::string uri = std::move(online_browser_uri);
				online_browser_uri.clear();
				online_browser_open_at = 0;
#ifdef __ANDROID__
				if (!OpenOnlineAuthorization(uri)) online_status = "Failed to open browser authorization.";
#else
				if (SDL_OpenURL(uri.c_str()) != 0)
					online_status = std::string("SDL_OpenURL failed: ") + SDL_GetError();
#endif
			}
			if (online_authorization_pending && online_authorization_deadline
				&& online_now >= online_authorization_deadline) {
				online_authorization_pending = false;
				if (online_loopback) { online_loopback->Stop(); online_loopback.reset(); }
				online_status = "StartupUI.OnlineAuthorizationExpired"_lc;
			}
			const ImVec2 viewport_size = ImGui::GetMainViewport()->WorkSize;
#ifdef __ANDROID__
			const ImVec2 online_popup_size(viewport_size.x * 0.96f, viewport_size.y * 0.90f);
#else
			const ImVec2 online_popup_size(
				(std::min)(1180.0f, viewport_size.x * 0.90f),
				(std::min)(820.0f, viewport_size.y * 0.88f));
#endif
			ImGui::SetNextWindowSize(online_popup_size, ImGuiCond_Appearing);
			PushStartupDialogStyle(palette);
			if (!ImGui::BeginPopupModal("StartupUI.OnlineModels"_lc, nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
				ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
				PopStartupDialogStyle();
				return;
			}
			bool online_dialog_open = true;
			DrawStartupDialogHeader(palette, "StartupUI.OnlineModels"_lc, &online_dialog_open, !OnlineBusy());
			if (close_online_popup_requested) {
				close_online_popup_requested = false;
				ImGui::CloseCurrentPopup();
			}

			if (online_authorization_pending && !OnlineBusy() && online_loopback && online_loopback->Completed()) {
				online_approval_grant = online_loopback->ApprovalGrant();
				BeginCompleteOnlineLogin();
			}
#ifdef __ANDROID__
			if (online_authorization_pending && !OnlineBusy()) {
				if (auto grant = ConsumeOnlineAuthorizationCallback()) {
					online_approval_grant = std::move(*grant);
					BeginCompleteOnlineLogin();
				}
			}
#endif
			if (online_authorization_pending && online_loopback) {
				const auto loopback_error = online_loopback->Error();
				if (!loopback_error.empty()) online_status = loopback_error;
			}

			ImGui::TextUnformatted("StartupUI.OnlineApiAddress"_lc);
			ImGui::SetNextItemWidth(-1);
			const bool api_address_disabled = !online_access_token.empty() || online_authorization_pending || OnlineBusy();
			if (api_address_disabled) ImGui::BeginDisabled();
			if (ImGui::InputText("##OnlineApiAddress", online_api, sizeof(online_api))) {
				ClearOnlineSessionState();
				SaveOnlineApiAddress();
			}
			if (api_address_disabled) ImGui::EndDisabled();

			const bool network_controls_disabled = OnlineBusy();
			if (network_controls_disabled) ImGui::BeginDisabled();
			if (online_access_token.empty() && !online_authorization_pending && ImGui::Button("StartupUI.OnlineLogin"_lc)) {
				try { BeginOnlineLogin(); }
				catch (const std::exception& e) { online_status = e.what(); }
			}
			if (online_authorization_pending) {
				if (ImGui::Button("StartupUI.OnlineRelogin"_lc)) {
					try { BeginOnlineLogin(); }
					catch (const std::exception& e) { online_status = e.what(); }
				}
			}
			if (!online_access_token.empty() && ImGui::Button("Button.Refresh"_lc)) {
				BeginLoadOnlineModels();
			}
			if (!online_access_token.empty()) {
				ImGui::SameLine();
				if (ImGui::Button("StartupUI.OnlineLogout"_lc)) BeginLogoutOnline();
			}
			if (network_controls_disabled) ImGui::EndDisabled();

			if (!online_status.empty()) {
				if (OnlineBusy()) {
					static constexpr char spinner[] = "|/-\\";
					const int frame = static_cast<int>(ImGui::GetTime() * 8.0) & 3;
					ImGui::Text("%c", spinner[frame]);
					ImGui::SameLine();
				}
				ImGui::TextWrapped("%s", online_status.c_str());
			}
			ImGui::Separator();

			ImGui::SetNextItemWidth(220.0f);
			ImGui::InputText("StartupUI.SearchBoxHeader"_lc, online_search_txt, 200);
			ImGui::SameLine();
			std::vector<std::string> online_types{"##"};
			for (const auto& item : online_models) {
				if (std::find(online_types.begin(), online_types.end(), item.model_type) == online_types.end())
					online_types.push_back(item.model_type);
			}
			std::sort(online_types.begin() + 1, online_types.end());
			ImGui::SetNextItemWidth(110.0f);
			if (ImGui::BeginCombo("##OnlineTypeFilter", online_filter)) {
				for (const auto& type : online_types) {
					const bool is_selected = std::strcmp(online_filter, type.c_str()) == 0;
					if (ImGui::Selectable(type.c_str(), is_selected)) {
						std::snprintf(online_filter, sizeof(online_filter), "%s", type.c_str());
					}
					if (is_selected) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			ImGui::Checkbox("StartupUI.DontShowEmuRom"_lc, &online_hide_emu);

			const float list_height = (std::max)(180.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.0f);
			if (ImGui::BeginChild("OnlineModelList", ImVec2(0, list_height), true)) {
				for (int index = 0; index < static_cast<int>(online_models.size()); ++index) {
					const auto& item = online_models[index];
					if (!OnlineModelVisible(item)) continue;
					ImGui::PushID(index);
					const std::string label = item.name + "  [" + item.id + "]  " + item.model_type;
					if (ImGui::Selectable(label.c_str(), selected_online_model == index)) selected_online_model = index;
					ImGui::PopID();
				}
			}
			ImGui::EndChild();

			const bool launch_controls_disabled = OnlineBusy();
			if (launch_controls_disabled) ImGui::BeginDisabled();
			ImGui::Dummy(ImVec2(0.0f, 4.0f));
			if (ImGui::Button("StartupUI.OnlineLaunch"_lc, ImVec2(0, 26))) BeginDownloadOnlineModel();
			ImGui::SameLine();
			if (ImGui::Button("Button.Negative"_lc, ImVec2(0, 26))) ImGui::CloseCurrentPopup();
			if (launch_controls_disabled) ImGui::EndDisabled();
			ImGui::EndPopup();
			PopStartupDialogStyle();
		}

		void LaunchModel(const Model& model) {
			selected_path = model.path;
			auto iter = std::find_if(recently_used.begin(), recently_used.end(),
				[&](const std::string& value) { return value == model.path.string(); });
			if (iter != recently_used.end())
				recently_used.erase(iter);
			recently_used.insert(recently_used.begin(), model.path.string());
			if (recently_used.size() > 8)
				recently_used.resize(8);
		}

		void DrawStartupTitleBar(const plugin::ThemePalette& palette) {
			const ImVec2 pos = ImGui::GetMainViewport()->Pos;
			const float width = ImGui::GetMainViewport()->Size.x;
			ImDrawList* draw = ImGui::GetWindowDrawList();
			draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + kStartupChromeHeight),
				ImGui::ColorConvertFloat4ToU32(palette.titleBar));
			const float center_y = pos.y + kStartupChromeHeight * 0.5f;
			const float text_h = ImGui::GetTextLineHeight();
			const float text_top = center_y - text_h * 0.5f;
			const float control_top = center_y - 11.0f;
			draw->AddText(ImVec2(pos.x + 94.0f, text_top),
				ImGui::ColorConvertFloat4ToU32(palette.foreground), "CasioEmuMsvc");
			draw->AddLine(ImVec2(pos.x, pos.y + kStartupChromeHeight - 1.0f),
				ImVec2(pos.x + width, pos.y + kStartupChromeHeight - 1.0f),
				ImGui::ColorConvertFloat4ToU32(palette.border));

			struct Action {
				const char* label;
				int id;
				float width;
			};
			const Action actions[] = {
				{ L("Startup.ImportModels", "Import models..."), 0, ImGui::CalcTextSize(L("Startup.ImportModels", "Import models...")).x + 20.0f },
				{ L("Startup.ExportModels", "Export models..."), 1, ImGui::CalcTextSize(L("Startup.ExportModels", "Export models...")).x + 20.0f },
				{ L("Startup.Refresh", "Refresh"), 2, ImGui::CalcTextSize(L("Startup.Refresh", "Refresh")).x + 20.0f },
				{ L("Startup.Online", "Online"), 3, ImGui::CalcTextSize(L("Startup.Online", "Online")).x + 20.0f },
			};
			float total = 0.0f;
			for (const auto& action : actions)
				total += action.width + 6.0f;
			float action_x = pos.x + width - total - 10.0f;
			for (const auto& action : actions) {
				ImGui::SetCursorScreenPos(ImVec2(action_x, control_top));
				ImGui::PushID(action.id + 2000);
				const bool clicked = ImGui::InvisibleButton("##startup_action", ImVec2(action.width, 22.0f));
				const bool hovered = ImGui::IsItemHovered();
				if (hovered || ImGui::IsItemActive())
					draw->AddRectFilled(ImVec2(action_x, control_top),
						ImVec2(action_x + action.width, control_top + 22.0f),
						ImGui::ColorConvertFloat4ToU32(palette.hover), 4.0f);
				draw->AddText(ImVec2(action_x + 10.0f, text_top),
					ImGui::ColorConvertFloat4ToU32(palette.foreground), action.label);
				ImGui::PopID();
				if (clicked) {
					if (action.id == 0) {
						show_import_dialog = true;
					}
					else if (action.id == 1) {
						export_entries.clear();
						show_export_dialog = true;
					}
					else if (action.id == 2)
						Reload();
					else
						show_online_popup = true;
				}
				action_x += action.width + 6.0f;
			}
		}

		void RenderModelRow(Model& model, int& id) {
			ImGui::PushID(id++);
			ImGui::SetCursorPosX(20.0f);
			ImGui::PushStyleColor(ImGuiCol_Header, ThemeManager::Instance().Colors().selection);
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ThemeManager::Instance().Colors().hover);
			if (ImGui::Selectable(model.name.c_str(), selected_path == model.path,
					ImGuiSelectableFlags_AllowDoubleClick, ImVec2(ImGui::GetContentRegionAvail().x - 12.0f, 0.0f))) {
				selected_path = model.path;
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					LaunchModel(model);
			}
			ImGui::PopStyleColor(2);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s  %s  [%s]", model.name.c_str(), model.version.c_str(), model.type.c_str());
			if (ImGui::BeginPopupContextItem("##model_ctx")) {
				if (ImGui::MenuItem("Launch")) LaunchModel(model);
				if (ImGui::MenuItem("Edit Model...")) {
					try { windows2->push_back(new ModelEditor(model.path)); }
					catch (const std::exception&) {
						SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", "Failed to open model editor.", nullptr);
					}
				}
				ImGui::EndPopup();
			}
			ImGui::PopID();
		}

		void RenderStartupSidebar(const plugin::ThemePalette& palette, float width, float height) {
			ImGui::PushStyleColor(ImGuiCol_ChildBg, palette.surface);
			ImGui::BeginChild("##startup_sidebar", ImVec2(width, height), ImGuiChildFlags_None);
			ImDrawList* draw = ImGui::GetWindowDrawList();
			const ImVec2 start = ImGui::GetWindowPos();
			draw->AddRectFilled(start, ImVec2(start.x + width, start.y + 22.0f),
				ImGui::ColorConvertFloat4ToU32(palette.titleBar));
			ImGui::SetCursorPos(ImVec2(20.0f, 3.0f));
			ImGui::TextDisabled("%s", L("Startup.Models", "MODELS"));
			ImGui::SetCursorPos(ImVec2(16.0f, 28.0f));
			ImGui::SetNextItemWidth(width - 32.0f);
			ImGui::InputTextWithHint("##model_search", L("Startup.SearchModels", "Search models"), search_txt, sizeof(search_txt));
			ImGui::SetCursorPosX(16.0f);
			ImGui::SetNextItemWidth(width - 32.0f);
			const char* filter_label = strcmp(current_filter, "##") == 0 ? L("Startup.AllTypes", "All types") : current_filter;
			if (ImGui::BeginCombo("##model_filter", filter_label)) {
				std::vector<const char*> items = { "##", "ES", "ESP", "ESP2nd", "CWX", "CWII", "Fx5800p" };
				for (auto* item : items) {
					if (ImGui::Selectable(item, current_filter == item)) current_filter = item;
				}
				ImGui::EndCombo();
			}
			ImGui::Checkbox(L("Startup.HideRoms", "Hide emulator ROMs"), &not_show_emu);
			ImGui::Separator();
			int id = 0;
			if (loading) {
				ImGui::TextDisabled("%s", L("Startup.Scanning", "Scanning models..."));
			}
			else {
				ImGui::SetCursorPosX(20.0f);
				ImGui::TextDisabled("%s", L("Startup.AllModels", "ALL MODELS"));
				for (auto& model : models) {
					const bool matches_filter = strcmp(current_filter, "##") == 0 || current_filter == model.type;
					const bool matches_search = stristr(model.name.c_str(), search_txt) != nullptr ||
						stristr(model.version.c_str(), search_txt) != nullptr;
					if (matches_filter && matches_search && (not_show_emu ? model.realhw : 1))
						RenderModelRow(model, id);
				}
			}
			ImGui::EndChild();
			ImGui::PopStyleColor();
		}

		void RenderStartupWelcome(const plugin::ThemePalette& palette, float width, float height) {
			ImGui::PushStyleColor(ImGuiCol_ChildBg, palette.background);
			ImGui::BeginChild("##startup_welcome", ImVec2(width, height), ImGuiChildFlags_None);
			ImGui::SetCursorPos(ImVec2(36.0f, 30.0f));
			ImGui::SetWindowFontScale(2.1f);
			ImGui::TextUnformatted("CasioEmuMsvc");
			ImGui::SetWindowFontScale(1.0f);
			ImGui::SetCursorPosX(37.0f);
			ImGui::TextDisabled("Casio nX-U8/100 & nX-U16/100 emulator and debugger");
			ImGui::Dummy(ImVec2(0, 14));
			auto link = [&](const char* label, auto&& action) {
				ImGui::SetCursorPosX(36.0f);
				ImGui::PushStyleColor(ImGuiCol_Text, palette.link);
				if (ImGui::Selectable(label, false, ImGuiSelectableFlags_None, ImVec2(360, 0)))
					action();
				ImGui::PopStyleColor();
			};
			ImGui::SetCursorPosX(36.0f);
			ImGui::TextDisabled("%s", L("Startup.Start", "Start"));
			link(L("Startup.NewCalculator", "New Calculator"), [&] {
				if (selected_path.empty()) {
					import_status = "Select a model from the Models list first.";
					import_error = true;
					return;
				}
				auto iter = std::find_if(models.begin(), models.end(),
					[&](const Model& item) { return item.path == selected_path; });
				if (iter != models.end()) LaunchModel(*iter);
			});
			link(L("Startup.OpenFolder", "Open Folder..."), [&] {
				SystemDialogs::OpenFolderDialog([](std::filesystem::path folder) {
					auto& data = casioemu::settings::SettingsStore::Instance().Data();
					data.workspace.folder = folder.string();
					casioemu::settings::SettingsStore::Instance().Save();
				});
			});
			ImGui::Dummy(ImVec2(0, 14));
			ImGui::SetCursorPosX(36.0f);
			ImGui::TextDisabled("%s", L("Startup.Recent", "Recent"));
			bool any = false;
			if (loading) {
				ImGui::SetCursorPosX(36.0f);
				ImGui::TextDisabled("%s", L("Startup.Scanning", "Scanning models..."));
			}
			for (const auto& path : loading ? std::vector<std::string>{} : recently_used) {
				auto iter = std::find_if(models.begin(), models.end(),
					[&](const Model& item) { return item.path == path; });
				if (iter == models.end()) continue;
				any = true;
				link(iter->name.c_str(), [&] { LaunchModel(*iter); });
			}
			if (!any && !loading) {
				const auto& saved = casioemu::settings::SettingsStore::Instance().Data().sessions;
				for (const auto& record : saved) {
					any = true;
					ImGui::SetCursorPosX(36.0f);
					ImGui::PushStyleColor(ImGuiCol_Text, palette.link);
					const std::string row = record.title.empty() ? record.model : record.title;
					if (ImGui::Selectable(row.c_str(), false, ImGuiSelectableFlags_None, ImVec2(360, 0))) {
						auto iter = std::find_if(models.begin(), models.end(),
							[&](const Model& item) {
								if (!record.modelPath.empty()) {
									std::error_code path_ec;
									const auto lhs = std::filesystem::weakly_canonical(item.path, path_ec);
									const auto rhs = std::filesystem::weakly_canonical(record.modelPath, path_ec);
									if (!path_ec && lhs == rhs)
										return true;
								}
								return !record.model.empty() && item.name == record.model;
							});
						if (iter != models.end()) LaunchModel(*iter);
					}
					ImGui::PopStyleColor();
				}
			}
			if (!any && !loading) {
				ImGui::SetCursorPosX(36.0f);
				ImGui::TextDisabled("%s", L("Startup.NoRecent", "No recent models yet"));
			}
			ImGui::EndChild();
			ImGui::PopStyleColor();
		}

		void RenderStartupStatus(const plugin::ThemePalette& palette, float width, float height) {
			const ImVec2 pos = ImGui::GetMainViewport()->Pos;
			ImDrawList* draw = ImGui::GetWindowDrawList();
			const ImVec2 min(pos.x, pos.y + ImGui::GetMainViewport()->Size.y - height);
			draw->AddRectFilled(min, ImVec2(min.x + width, min.y + height),
				ImGui::ColorConvertFloat4ToU32(palette.statusBar));
			char count[96];
			if (loading)
				std::snprintf(count, sizeof(count), "Scanning models...");
			else
				std::snprintf(count, sizeof(count), "Models: %d", (int)models.size());
			draw->AddText(ImVec2(min.x + 12.0f, min.y + 2.0f),
				ImGui::ColorConvertFloat4ToU32(palette.statusBarForeground), count);
			if (!import_status.empty()) {
				const float count_width = ImGui::CalcTextSize(count).x;
				const ImU32 status_color = ImGui::ColorConvertFloat4ToU32(
					import_error ? palette.error : palette.statusBarForeground);
				std::string status = (import_error ? "   |   " : "   |   ") + import_status;
				draw->AddText(ImVec2(min.x + 12.0f + count_width + 8.0f, min.y + 2.0f),
					status_color, status.c_str());
			}
			const std::string right = std::string("Language: ") + g_local.GetCurrentLanguage() +
				"   |   Settings: JSON   |   Schema 1   |   build " CASIOEMU_BUILD_COMMIT " " CASIOEMU_BUILD_TIME;
			const float right_width = ImGui::CalcTextSize(right.c_str()).x;
			draw->AddText(ImVec2(min.x + width - right_width - 12.0f, min.y + 2.0f),
				ImGui::ColorConvertFloat4ToU32(palette.statusBarForeground), right.c_str());
		}

		void RenderStartupSettings(const plugin::ThemePalette& palette, float width, float height) {
			ImGui::PushStyleColor(ImGuiCol_ChildBg, palette.background);
			ImGui::BeginChild("##startup_settings", ImVec2(width, height), ImGuiChildFlags_None);
			ImGui::SetCursorPos(ImVec2(36.0f, 30.0f));
			ImGui::SetWindowFontScale(1.7f);
			ImGui::TextUnformatted("Settings");
			ImGui::SetWindowFontScale(1.0f);
			ImGui::SetCursorPosX(37.0f);
			ImGui::TextDisabled("Appearance and language are shared with the session shell.");
			ImGui::Dummy(ImVec2(0, 18));

			ImGui::SetCursorPosX(36.0f);
			ImGui::TextDisabled("Appearance");
			ImGui::SetCursorPosX(36.0f);
			const std::string current = ThemeManager::Instance().CurrentThemeName();
			ImGui::SetNextItemWidth(320.0f);
			if (ImGui::BeginCombo("##startup_theme", current.c_str())) {
				for (const auto& info : ThemeManager::Instance().AvailableThemes()) {
					if (ImGui::Selectable((info.name + "##" + info.id).c_str(), info.id == current))
						ThemeManager::Instance().ApplyThemeByName(info.id);
				}
				ImGui::EndCombo();
			}

			ImGui::Dummy(ImVec2(0, 14));
			ImGui::SetCursorPosX(36.0f);
			ImGui::TextDisabled("Language");
			ImGui::SetCursorPosX(36.0f);
			static const char* languages[] = { "en_US", "zh_CN", "ja_JP", "vi_VN", "ru_RU", "ko_KR", "fr_FR", "de_DE", "es_ES" };
			const std::string current_language = ThemeManager::Instance().Settings().language;
			ImGui::SetNextItemWidth(220.0f);
			if (ImGui::BeginCombo("##startup_language", current_language.c_str())) {
				for (const char* language : languages) {
					if (ImGui::Selectable(language, current_language == language)) {
						std::snprintf(ThemeManager::Instance().Settings().language,
							sizeof(ThemeManager::Instance().Settings().language), "%s", language);
						g_local.ChangeLanguage(language);
						ThemeManager::Instance().SaveSettings();
						ThemeManager::Instance().RequestFontRebuild();
					}
				}
				ImGui::EndCombo();
			}

			ImGui::Dummy(ImVec2(0, 14));
			ImGui::SetCursorPosX(36.0f);
			ImGui::TextDisabled("Workspace");
			ImGui::SetCursorPosX(36.0f);
			const auto& workspace = casioemu::settings::SettingsStore::Instance().Data().workspace.folder;
			ImGui::TextWrapped("%s", workspace.empty() ? "(none)" : workspace.c_str());
			ImGui::SetCursorPosX(36.0f);
			if (ImGui::Button("Open Folder...", ImVec2(140, 24))) {
				SystemDialogs::OpenFolderDialog([](std::filesystem::path folder) {
					auto& data = casioemu::settings::SettingsStore::Instance().Data();
					data.workspace.folder = folder.string();
					casioemu::settings::SettingsStore::Instance().Save();
				});
			}
			ImGui::EndChild();
			ImGui::PopStyleColor();
		}

		static std::filesystem::path PreviousThemeRecordPath() {
			std::error_code ec;
			auto path = casioemu::settings::SettingsStore::Instance().FilePath();
			if (path.empty())
				path = casioemu::settings::SettingsStore::DefaultFilePath();
			const auto parent = path.parent_path();
			if (!parent.empty())
				std::filesystem::create_directories(parent, ec);
			return parent / "last-theme.txt";
		}
		static std::string ReadPreviousTheme() {
			std::ifstream in(PreviousThemeRecordPath());
			std::string value;
			if (std::getline(in, value) && !value.empty() && value != "anh-sang-cua-dang")
				return value;
			return "classic";
		}
		static void RememberPreviousTheme(const std::string& id) {
			if (id.empty() || id == "anh-sang-cua-dang")
				return;
			static std::string last;
			if (last == id)
				return;
			last = id;
			std::ofstream out(PreviousThemeRecordPath(), std::ios::trunc);
			if (out)
				out << id << '\n';
		}
		bool RenderSpecialTheme() {
			static SDL_Texture* texture = nullptr;
			static int image_w = 0;
			static int image_h = 0;
			if (!texture) {
				const char* candidates[] = {
					"../Resources/themes/anh-sang-cua-dang.jpeg",
					"Resources/themes/anh-sang-cua-dang.jpeg",
					"resources/themes/anh-sang-cua-dang.jpeg",
					"CasioEmuMsvc/resources/themes/anh-sang-cua-dang.jpeg",
				};
				for (const char* path : candidates) {
					SDL_Surface* surface = IMG_Load(path);
					if (!surface)
						continue;
					image_w = surface->w;
					image_h = surface->h;
					if (renderer2)
						texture = SDL_CreateTextureFromSurface(renderer2, surface);
					SDL_FreeSurface(surface);
					if (texture)
						break;
				}
			}
			const ImGuiViewport* viewport = ImGui::GetMainViewport();
			const ImVec2 pos = viewport->Pos;
			const ImVec2 size = viewport->Size;
			ImDrawList* draw = ImGui::GetForegroundDrawList();
			if (texture && image_w > 0 && image_h > 0 && size.x > 0 && size.y > 0) {
				const float window_aspect = size.x / size.y;
				const float image_aspect = static_cast<float>(image_w) / static_cast<float>(image_h);
				ImVec2 uv0(0.0f, 0.0f);
				ImVec2 uv1(1.0f, 1.0f);
				if (window_aspect > image_aspect) {
					const float visible = image_aspect / window_aspect;
					uv0.y = (1.0f - visible) * 0.5f;
					uv1.y = 1.0f - uv0.y;
				}
				else {
					const float visible = window_aspect / image_aspect;
					uv0.x = (1.0f - visible) * 0.5f;
					uv1.x = 1.0f - uv0.x;
				}
				draw->AddImage(reinterpret_cast<ImTextureID>(texture), pos,
					ImVec2(pos.x + size.x, pos.y + size.y), uv0, uv1);
			}
			else {
				draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(0, 0, 0, 255));
			}
			ImGuiIO& io = ImGui::GetIO();
			io.WantCaptureMouse = true;
			io.WantCaptureKeyboard = true;
			if (std::getenv("CASIOEMU_SPECIAL_THEME_REVERT_TEST")) {
				static int test_frame = 0;
				++test_frame;
				if (test_frame == 12)
					io.AddMouseButtonEvent(0, true);
				else if (test_frame == 14)
					io.AddMouseButtonEvent(0, false);
			}
			if (io.MouseClicked[0]) {
				std::string target = ReadPreviousTheme();
				std::string error;
				if (!ThemeManager::Instance().ApplyThemeByName(target, &error))
					ThemeManager::Instance().ApplyThemeByName("classic", &error);
				ThemeManager::Instance().SaveSettings();
				std::fprintf(stderr, "[theme-revert] %s\n", target.c_str());
			}
			return true;
		}

		void RenderLegacy() {
			const plugin::ThemePalette& palette = ThemeManager::Instance().Colors();
			ImGuiStyle& startup_style = ImGui::GetStyle();
			startup_style.WindowRounding = 8.0f;
			startup_style.ChildRounding = 8.0f;
			startup_style.PopupRounding = 9.0f;
			startup_style.FrameRounding = 6.0f;
			startup_style.TabRounding = 6.0f;
			startup_style.ScrollbarRounding = 10.0f;
			startup_style.GrabRounding = 6.0f;
			const std::string active_theme = ThemeManager::Instance().CurrentThemeName();
			if (active_theme != "anh-sang-cua-dang")
				RememberPreviousTheme(active_theme);
			else if (RenderSpecialTheme())
				return;
			if (settings_page && std::getenv("CASIOEMU_STARTUP_APPEARANCE")) {
				static int click_frame = 0;
				++click_frame;
				ImGuiIO& io = ImGui::GetIO();
				if (click_frame == 16) {
					io.AddMousePosEvent(420.0f, 115.0f);
					io.AddMouseButtonEvent(0, true);
				}
				else if (click_frame == 18) {
					io.AddMouseButtonEvent(0, false);
				}
				else if (click_frame >= 22 && click_frame <= 30) {
					io.AddMousePosEvent(800.0f, 620.0f);
					io.AddMouseWheelEvent(0.0f, -4.0f);
				}
			}
			if (std::getenv("CASIOEMU_SHOW_MASS_IMPORT")) {
				static bool once = false;
				if (!once) { once = true; show_import_dialog = true; }
			}
			if (std::getenv("CASIOEMU_SHOW_MASS_EXPORT")) {
				static bool once = false;
				if (!once) { once = true; export_entries.clear(); show_export_dialog = true; }
			}
			if (std::getenv("CASIOEMU_STARTUP_SETTINGS")) {
				static bool opened = false;
				if (!opened) {
					opened = true;
					settings_page = true;
				}
			}
			if (std::getenv("CASIOEMU_SHOW_ONLINE")) {
				static bool online_opened = false;
				if (!online_opened) {
					online_opened = true;
					show_online_popup = true;
					online_status = "Sign in to browse online models.";
				}
			}
			if (std::getenv("CASIOEMU_SHOW_UPDATE")) {
				update_info.tag = "v9.9.9";
				update_info.title = "Preview verification build";
				update_info.notes = "A compact update card used for headless verification.\nSecond line is intentionally omitted.";
				update_info.url = "https://example.invalid/";
				show_update = true;
				update_result_consumed = true;
			}
			if (update_checker.Ready() && !update_result_consumed) {
				update_info = update_checker.TakeResult();
				update_result_consumed = true;
				show_update = !update_info.tag.empty();
			}
			if (show_update) {
				ImGui::OpenPopup("StartupUI.UpdateAvailableTitle"_lc);
				bool open = show_update;
				if (BeginStartupModal(palette, "StartupUI.UpdateAvailableTitle"_lc,
						"Update Available", &open, ImVec2(460.0f, 205.0f))) {
					ImGui::TextWrapped("%s %s", "StartupUI.UpdateAvailableMessage"_lc, update_info.tag.c_str());
					if (!update_info.notes.empty()) {
						std::string first_line = update_info.notes.substr(0, update_info.notes.find('\n'));
						if (first_line.size() > 120)
							first_line = first_line.substr(0, 117) + "...";
						ImGui::TextDisabled("%s", first_line.c_str());
					}
					ImGui::Dummy(ImVec2(0.0f, 6.0f));
					if (ImGui::Button("StartupUI.OpenReleasePage"_lc, ImVec2(0, 26)))
						SDL_OpenURL(update_info.url.c_str());
					ImGui::SameLine();
					if (ImGui::Button("StartupUI.UpdateLater"_lc, ImVec2(0, 26))) {
						show_update = false;
						ImGui::CloseCurrentPopup();
					}
					EndStartupModal();
				}
				if (!open)
					show_update = false;
			}
			const ImGuiViewport* viewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(viewport->Pos);
			ImGui::SetNextWindowSize(viewport->Size);
			ImGui::PushStyleColor(ImGuiCol_WindowBg, palette.background);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
			ImGui::Begin("##startup_shell", nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
				ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
			DrawStartupTitleBar(palette);
			ImDrawList* draw = ImGui::GetWindowDrawList();
			const float strip_top = viewport->Pos.y + kStartupChromeHeight;
			const float strip_height = 26.0f;
			draw->AddRectFilled(ImVec2(viewport->Pos.x, strip_top),
				ImVec2(viewport->Pos.x + viewport->Size.x, strip_top + strip_height),
				ImGui::ColorConvertFloat4ToU32(palette.surface));
			// Welcome | Settings.  Each screen has exactly one Settings entry;
			// this launcher page renders the same SettingsWindow as the shell.
			ImGui::SetCursorScreenPos(ImVec2(viewport->Pos.x, strip_top));
			ImGui::InvisibleButton("##launcher_welcome_tab", ImVec2(120.0f, strip_height));
			if (ImGui::IsItemClicked())
				settings_page = false;
			ImGui::SetCursorScreenPos(ImVec2(viewport->Pos.x + 120.0f, strip_top));
			ImGui::InvisibleButton("##launcher_settings_tab", ImVec2(110.0f, strip_height));
			if (ImGui::IsItemClicked())
				settings_page = true;
			draw->AddRectFilled(ImVec2(viewport->Pos.x, strip_top),
				ImVec2(viewport->Pos.x + 120.0f, strip_top + strip_height),
				ImGui::ColorConvertFloat4ToU32(settings_page ? palette.tabInactive : palette.tabActive));
			draw->AddRectFilled(ImVec2(viewport->Pos.x + 120.0f, strip_top),
				ImVec2(viewport->Pos.x + 230.0f, strip_top + strip_height),
				ImGui::ColorConvertFloat4ToU32(settings_page ? palette.tabActive : palette.tabInactive));
			draw->AddRectFilled(ImVec2(viewport->Pos.x + (settings_page ? 124.0f : 4.0f), strip_top),
				ImVec2(viewport->Pos.x + (settings_page ? 226.0f : 116.0f), strip_top + 2.0f),
				ImGui::ColorConvertFloat4ToU32(palette.accent));
			draw->AddText(ImVec2(viewport->Pos.x + 18.0f,
				strip_top + (strip_height - ImGui::GetTextLineHeight()) * 0.5f),
				ImGui::ColorConvertFloat4ToU32(settings_page ? palette.muted : palette.foreground), L("Startup.Welcome", "Welcome"));
			draw->AddText(ImVec2(viewport->Pos.x + 134.0f,
				strip_top + (strip_height - ImGui::GetTextLineHeight()) * 0.5f),
				ImGui::ColorConvertFloat4ToU32(settings_page ? palette.foreground : palette.muted), "Settings");
			if (import_busy) {
				static constexpr char spinner[] = "|/-\\";
				const int frame = static_cast<int>(ImGui::GetTime() * 8.0) & 3;
				const std::string busy = std::string(1, spinner[frame]) + "  " + import_status;
				const float busy_w = ImGui::CalcTextSize(busy.c_str()).x;
				draw->AddText(ImVec2(viewport->Pos.x + viewport->Size.x - busy_w - 16.0f,
					strip_top + (strip_height - ImGui::GetTextLineHeight()) * 0.5f),
					ImGui::ColorConvertFloat4ToU32(palette.muted), busy.c_str());
			}
			const float body_y = kStartupChromeHeight + strip_height + 1.0f;
			const float status_h = 22.0f;
			const float body_h = viewport->Size.y - body_y - status_h;
			const float sidebar_w = std::min(300.0f, viewport->Size.x * 0.32f);
			ImGui::SetCursorScreenPos(ImVec2(viewport->Pos.x, viewport->Pos.y + body_y));
			RenderStartupSidebar(palette, sidebar_w, body_h);
			ImGui::SetCursorScreenPos(ImVec2(viewport->Pos.x + sidebar_w + 1.0f, viewport->Pos.y + body_y));
			if (settings_page) {
				const float settings_w = viewport->Size.x - sidebar_w - 1.0f;
				ImGui::PushStyleColor(ImGuiCol_ChildBg, palette.background);
				ImGui::BeginChild("##startup_settings_host", ImVec2(settings_w, body_h), ImGuiChildFlags_None);
				ImGui::SetCursorPos(ImVec2(12.0f, 10.0f));
				ImGui::BeginChild("##startup_settings_body", ImVec2(settings_w - 24.0f, body_h - 20.0f), ImGuiChildFlags_None);
				if (settings_window)
					settings_window->RenderCore();
				ImGui::EndChild();
				ImGui::EndChild();
				ImGui::PopStyleColor();
			}
			else {
				RenderStartupWelcome(palette, viewport->Size.x - sidebar_w - 1.0f, body_h);
			}
			RenderStartupStatus(palette, viewport->Size.x, status_h);
			RenderOnlineModels();
			RenderMassImportDialog(palette);
			RenderMassExportDialog(palette);
			PollImportTask();

			if (import_password_pending)
				ImGui::OpenPopup("##import_password");
			if (import_password_pending) {
				bool open = true;
				if (BeginStartupModal(palette, "##import_password", "Encrypted Package", &open,
						ImVec2(470.0f, 220.0f))) {
					ImGui::TextWrapped("%s", last_import.error.empty()
						? "Enter the package password."
						: last_import.error.c_str());
					ImGui::Dummy(ImVec2(0.0f, 4.0f));
					ImGui::SetNextItemWidth(-1.0f);
					const bool submitted = ImGui::InputText("##import_password_value", password,
						IM_ARRAYSIZE(password), ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
					if (password_error)
						ImGui::TextColored(ImVec4(0.95f, 0.3f, 0.3f, 1.0f), "Wrong password. Try again.");
					ImGui::Dummy(ImVec2(0.0f, 6.0f));
					if (ImGui::Button("Import", ImVec2(110, 26)) || submitted) {
						if (password[0] == '\0') {
							password_error = true;
						}
						else {
							RetryImportModelPackage(password);
							import_password_pending = false;
							ImGui::CloseCurrentPopup();
						}
					}
					ImGui::SameLine();
					if (ImGui::Button("Cancel", ImVec2(110, 26))) {
						import_password_pending = false;
						import_status = "Import cancelled.";
						ImGui::CloseCurrentPopup();
					}
					EndStartupModal();
				}
				if (!open) {
					import_password_pending = false;
					import_status = "Import cancelled.";
				}
			}

			if (show_import_result)
				ImGui::OpenPopup("##import_result");
			if (show_import_result) {
				bool open = true;
				if (BeginStartupModal(palette, "##import_result",
						last_import.ok ? "Import Complete" : "Import Failed", &open,
						ImVec2(520.0f, last_import.ok ? 220.0f : 200.0f))) {
					if (last_import.ok) {
						ImGui::Text("Imported %s", last_import.modelName.c_str());
						ImGui::TextDisabled("%s", last_import.targetPath.c_str());
						ImGui::Dummy(ImVec2(0.0f, 4.0f));
						std::string files;
						for (size_t i = 0; i < last_import.files.size(); ++i)
							files += (i ? ", " : "") + last_import.files[i];
						ImGui::TextWrapped("Registered files: %s", files.c_str());
					}
					else {
						ImGui::TextWrapped("%s", last_import.error.c_str());
					}
					ImGui::Dummy(ImVec2(0.0f, 8.0f));
					if (ImGui::Button("OK", ImVec2(110, 26))) {
						show_import_result = false;
						ImGui::CloseCurrentPopup();
					}
					EndStartupModal();
				}
				if (!open)
					show_import_result = false;
			}
			ImGui::End();
			ImGui::PopStyleVar(3);
			ImGui::PopStyleColor();
		}
		void RenderHeaders() {
			ImGui::TableSetupColumn("StartupUI.RomName"_lc, ImGuiTableColumnFlags_WidthStretch, 200);
			ImGui::TableSetupColumn("StartupUI.RomVer"_lc, ImGuiTableColumnFlags_WidthFixed, 120);
			ImGui::TableSetupColumn("StartupUI.RomSum"_lc, ImGuiTableColumnFlags_WidthFixed, 130);
			ImGui::TableSetupColumn("StartupUI.RomType"_lc, ImGuiTableColumnFlags_WidthFixed, 70);
			// ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80);
			ImGui::TableHeadersRow();
		}

		void RenderModel(const Model& model, int& i) {
			static char password[60]{};
			static bool pwd_op = true;
			ImGui::TableNextRow();
			ImGui::PushID(i++);
			ImGui::TableNextColumn();
			if (ImGui::Selectable(model.name.c_str())) {
				ImGui::OpenPopup("ContextMenu");
				pwd_op = false;
			}
			if (ImGui::BeginPopup("ContextMenu")) {
				if (pwd_op) {
					ImGui::InputText("##input_pwd", password, 60);

#ifdef __ANDROID__
					if (ImGui::MenuItem("StartupUI.Export"_lc)) {
						RomPackage rp{};
						try {
							rp.Load(model.path);
							if (*password != 0) {
								rp.Encrypt(password);
							}
							else {
								rp.Encrypt("0x0d000721");
								std::cout << "Using default password 0x0d000721\n";
							}
							memset(password, 0, 60);

							SystemDialogs::SaveFileDialog(model.name + ".package",
								[rp](std::filesystem::path fl) {
									JNIEnv* env = nullptr;
									if (!GetJNIEnv(&env)) {
										std::cerr << "Failed to get JNI environment" << std::endl;
										return;
									}

									try {
										std::vector<unsigned char> buffer;
										{
											std::stringstream ss;
											Binary::Write(ss, rp);
											std::string str = ss.str();
											buffer.assign(str.begin(), str.end());
										}

										jobject activity = (jobject)SDL_AndroidGetActivity();
										if (!activity) {
											throw std::runtime_error("Failed to get Android activity");
										}

										jclass gameClass = env->FindClass("com/tele/u8emulator/Game");
										if (!gameClass) {
											env->DeleteLocalRef(activity);
											throw std::runtime_error("Failed to find Game class");
										}

										jmethodID exportMethod = env->GetStaticMethodID(gameClass, "exportData", "([BLandroid/net/Uri;)V");
										if (!exportMethod) {
											env->DeleteLocalRef(gameClass);
											env->DeleteLocalRef(activity);
											throw std::runtime_error("Failed to find exportData method");
										}

										jbyteArray jdata = env->NewByteArray(buffer.size());
										env->SetByteArrayRegion(jdata, 0, buffer.size(), (jbyte*)buffer.data());

										jclass uriClass = env->FindClass("android/net/Uri");
										if (!uriClass) {
											env->DeleteLocalRef(jdata);
											env->DeleteLocalRef(gameClass);
											env->DeleteLocalRef(activity);
											throw std::runtime_error("Failed to find Uri class");
										}

										jmethodID parseMethod = env->GetStaticMethodID(uriClass, "parse",
											"(Ljava/lang/String;)Landroid/net/Uri;");
										if (!parseMethod) {
											env->DeleteLocalRef(jdata);
											env->DeleteLocalRef(uriClass);
											env->DeleteLocalRef(gameClass);
											env->DeleteLocalRef(activity);
											throw std::runtime_error("Failed to find parse method");
										}

										jstring jpath = env->NewStringUTF(fl.string().c_str());
										jobject uri = env->CallStaticObjectMethod(uriClass, parseMethod, jpath);

										// Export the data
										env->CallStaticVoidMethod(gameClass, exportMethod, jdata, uri);

										// Cleanup
										env->DeleteLocalRef(jdata);
										env->DeleteLocalRef(jpath);
										env->DeleteLocalRef(uri);
										env->DeleteLocalRef(uriClass);
										env->DeleteLocalRef(gameClass);
										env->DeleteLocalRef(activity);

										// Close popup after successful export
										ImGui::CloseCurrentPopup();
									}
									catch (const std::exception& e) {
										std::cerr << "Export failed: " << e.what() << std::endl;
										SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Export Error", e.what(), nullptr);
									}
								});
						}
						catch (const std::exception& e) {
							std::cerr << "Export preparation failed: " << e.what() << std::endl;
							SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Export Error", e.what(), nullptr);
						}
					}
#else
					if (ImGui::MenuItem("StartupUI.Export"_lc)) {
						RomPackage rp{};
						try {
							rp.Load(model.path);
							if (*password != 0) {
								rp.Encrypt(password);
							}
							else {
								rp.Encrypt("0x0d000721");
								std::cout << "Using default password 0x0d000721\n";
							}
							memset(password, 0, 60);

							SystemDialogs::SaveFileDialog(model.name + ".package",
								[rp](std::filesystem::path fl) {
									try {
										std::ofstream ofs(fl, std::ios::binary);
										if (!ofs) {
											throw std::runtime_error("Cannot create output file");
										}
										Binary::Write(ofs, rp);
										ofs.close();
									}
									catch (const std::exception& e) {
										std::cerr << "Export failed: " << e.what() << std::endl;
										SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Export Error", e.what(), nullptr);
									}
								});
						}
						catch (const std::exception& e) {
							std::cerr << "Export preparation failed: " << e.what() << std::endl;
							SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Export Error", e.what(), nullptr);
						}
					}
#endif
				}
				else {
					if (ImGui::MenuItem("StartupUI.Launch"_lc)) {
						selected_path = model.path;
						auto iter = std::find_if(recently_used.begin(), recently_used.end(),
							[&](auto& x) {
								return x == model.path.string();
							});
						if (iter != recently_used.end())
							recently_used.erase(iter);
						recently_used.insert(recently_used.begin(), model.path.string());
						if (recently_used.size() > 5) {
							recently_used.resize(5);
						}
					}
#ifdef _WIN32
					if (ImGui::MenuItem("StartupUI.Reveal"_lc)) {
						std::wstring wpath = std::filesystem::path(model.path).wstring();
						ShellExecuteW(NULL, L"open", L"explorer.exe", wpath.c_str(), NULL, SW_SHOWNORMAL);
					}
#endif
					if (ImGui::MenuItem("StartupUI.Edit"_lc)) {
						try {
							windows2->push_back(new ModelEditor(model.path));
						}
						catch (const std::exception& e) {
							SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", "Failed to open model editor.", nullptr);
						}
					}
					if (ImGui::MenuItem("StartupUI.CreateShortcut"_lc)) {
						strncpy(shortcut_name, model.name.c_str(), sizeof(shortcut_name) - 1);
						shortcut_name[sizeof(shortcut_name) - 1] = '\0';
						shortcut_icon[0] = '\0';
						shortcut_model_path = model.path;
						show_shortcut_popup = true;
					}
					if (ImGui::MenuItem("StartupUI.Export"_lc)) {
						ImGui::EndPopup();
						ImGui::OpenPopup("ContextMenu");
						pwd_op = true;
						goto ed;
					}
				}
				ImGui::EndPopup();
			}
		ed:
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(model.version.c_str());
			ImGui::TableNextColumn();
			if (model.realhw) {
				if (model.show_sum) {
					ImGui::Text("%s (%s) %s", model.checksum.c_str(), model.checksum2.c_str(), model.sum_good.c_str());
				}
				else {
					ImGui::TextUnformatted("Table.NotAvailable"_lc);
				}
			}
			else {
				ImGui::TextUnformatted("StartupUI.EmulatorRom"_lc);
			}
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(model.type.c_str());
			ImGui::SameLine();
			ImGui::Dummy({0, ImGui::GetTextLineHeightWithSpacing()});
			ImGui::PopID();
		}
		void Render() { RenderLegacy(); }
	};
} // namespace casioemu
class LicenseWindow : public UIWindow {
public:
	LicenseWindow() : UIWindow("License") {
	}
	void RenderCore() override {
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextUnformatted(licenses_str);
		ImGui::PopTextWrapPos();
	}
	void Render() override {
		if (!open)
			return;
		auto& io = ImGui::GetIO();
		ImGui::SetNextWindowSize({io.DisplaySize.x, io.DisplaySize.y}, ImGuiCond_Appearing);
		ImGui::SetNextWindowPos({});
		if (ImGui::Begin(name, &open, flags)) {
			RenderCore();
		}
		ImGui::End();
	}
};
class CopyrightWatermark : public UIWindow {
	std::vector<UIWindow*>* windows3;

public:
	CopyrightWatermark(std::vector<UIWindow*>* windows3) : UIWindow("Copyright Warning"), windows3(windows3) {
	}
	void RenderCore() override {
		ImGui::TextWrapped(startup_copyright_warn);
		if (ImGui::Button("CopyrightWatermark.Dismiss"_lc))
			open = false;
		if (ImGui::Button("CopyrightWatermark.Licenses"_lc)) {
			windows3->push_back(new LicenseWindow());
		}
		if (ImGui::Button("CopyrightWatermark.VisitOfficialRepo"_lc)) {
			SDL_OpenURL("https://github.com/telecomadm1145/CasioEmuMsvc");
		}
	}
	void Render() override {
		if (!open)
			return;
		auto& io = ImGui::GetIO();
		ImGui::SetNextWindowSize({io.DisplaySize.x, io.DisplaySize.y}, ImGuiCond_Appearing);
		ImGui::SetNextWindowPos({});
		if (ImGui::Begin(name, &open, flags)) {
			RenderCore();
		}
		ImGui::End();
	}
};

void HandleStartupEvent(const SDL_Event& event) {
#ifdef __ANDROID__
	if (event.type == SDL_TEXTINPUT) {
		ThemeManager::Instance().RegisterInputGlyphs(event.text.text);
	}
#endif
	ImGui_ImplSDL2_ProcessEvent(&event);
}

StartupSelection sui_loop() {
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
		SDL_Log("SDL_Init failed in StartupUI: %s", SDL_GetError());
		return {};
	}
	if (IMG_Init(IMG_INIT_PNG) != IMG_INIT_PNG) {
		SDL_Log("IMG_Init failed in StartupUI: %s", IMG_GetError());
		SDL_Quit();
		return {};
	}
	SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
	windows2 = new std::vector<UIWindow*>();
	casioemu::StartupUi ui;
	if (const char* mass = std::getenv("CASIOEMU_MASS_TEST")) {
		const char* scratch = std::getenv("CASIOEMU_MASS_TMP");
		if (scratch && *scratch)
			ui.RunMassRoundTripTest(scratch, mass);
	}
	if (const char* report = std::getenv("CASIOEMU_SELFTEST_IMPORT")) {
		const char* package = std::getenv("CASIOEMU_SELFTEST_PACKAGE");
		if (package && *package)
			ui.RunImportSelfTest(package, report);
		else {
			std::ofstream output(report, std::ios::trunc);
			if (output)
				output << "RESULT FAIL\nCASIOEMU_SELFTEST_PACKAGE is required.\n";
		}
	}
	{
		std::ifstream ifs1{"recent.bin", std::ifstream::binary};
		if (ifs1) {
			try {
				Binary::Read(ifs1, ui.recently_used);
			}
			catch (const std::exception& e) {
				std::cerr << "[StartupUI][Warn] Failed to read \"recent.bin\": " << e.what() << ". Starting with no recent files.\n";
				ui.recently_used.clear();
			}
		}
	}
	const bool shot_mode = std::getenv("CASIOEMU_SHOT_STARTUP") != nullptr;
	window2 = SDL_CreateWindow(
		"CasioEmuMsvc",
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		1280, 860,
		(shot_mode ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN) | (SDL_WINDOW_RESIZABLE));
#ifdef __APPLE__
	casioemu::mac::ConfigureFramelessWindow(window2);
#endif
	casioemu::SetPreferredRendererDriverHint();
	renderer2 = SDL_CreateRenderer(window2, -1, shot_mode ? SDL_RENDERER_SOFTWARE : (SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED));
#ifdef _WIN32
	EnableDarkTitleBar(GetSDLWindowHandle(window2));
#endif
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
	if (renderer2 == nullptr) {
		SDL_Log("Error creating SDL_Renderer: %s", SDL_GetError());
		if (window2)
			SDL_DestroyWindow(window2);
		IMG_Quit();
		SDL_Quit();
		return {};
	}
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	// RebuildFont();
	io.WantCaptureKeyboard = true;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_NavNoCaptureKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = 1.0f;
#ifdef __ANDROID__
	windows2->push_back(new CopyrightWatermark(windows2));
#endif
	ImGui_ImplSDL2_InitForSDLRenderer(window2, renderer2);
	ImGui_ImplSDLRenderer2_Init(renderer2);
	ThemeManager::Instance().RequestFontRebuild();
	ThemeManager::Instance().ProcessFontRebuild();
	auto frame_event = SDL_RegisterEvents(1);
	std::atomic<bool> exited = {false};
	std::thread t3([&]() {
		SDL_Event se{};
		se.type = frame_event;
		if (window2)
			se.user.windowID = SDL_GetWindowID(window2);
		while (!exited) {
			if (window2)
				SDL_PushEvent(&se);
			SDL_Delay(24); // ~40fps
		}
	});
	bool done = false;
	bool once = true;
	std::vector<std::string> languages;
	int selected_language_index = 0;
	bool needs_render = true;
	while (!done) {
		SDL_Event event;
		if (SDL_WaitEvent(&event)) {
			HandleStartupEvent(event);
			if (event.type == SDL_QUIT) {
				done = true;
			}
			if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window2)) {
				done = true;
			}
			if (event.type == frame_event) {
				needs_render = true;
			}
			while (SDL_PollEvent(&event)) {
				HandleStartupEvent(event);
				if (event.type == SDL_QUIT) {
					done = true;
				}
				if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window2)) {
					done = true;
				}
				if (event.type == frame_event) {
					needs_render = true;
				}
			}
		}
		if (needs_render) {
			ImGui_ImplSDLRenderer2_NewFrame();
			ImGui_ImplSDL2_NewFrame();
			ImGui::NewFrame();
			ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
			ImGui::SetNextWindowDockID(ImGui::GetCurrentContext()->DockContext.Nodes.Data[0].key, ImGuiCond_FirstUseEver); // TODO: ????????
			ui.Render();
			// static std::once_flag size_once;
			// std::call_once(size_once, []() {
			//	ImGui::DockBuilderSetNodeSize(ImGui::GetWindowDockID(), ImGui::GetMainViewport()->Size);
			// });
			for (auto& wind : *windows2) {
				wind->Render();
			}
			if (once && !shot_mode && !std::filesystem::exists("locale.txt")) {
				if (languages.empty()) {
					const std::string locales_dir = "./locales/";
					if (std::filesystem::exists(locales_dir)) {
						for (const auto& entry : std::filesystem::directory_iterator(locales_dir)) {
							if (entry.is_regular_file()) {
								languages.push_back(entry.path().stem().string());
							}
						}
					}
					if (languages.empty()) {
						languages.push_back("en_US");
					}
					std::sort(languages.begin(), languages.end());
					auto english = std::find(languages.begin(), languages.end(), "en_US");
					selected_language_index = english == languages.end()
						? 0 : static_cast<int>(english - languages.begin());
				}
				ImGui::OpenPopup("LanguageChooser");
				once = false;
			}
			ImVec2 center = ImGui::GetMainViewport()->GetCenter();
			ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
			bool language_open = true;
			if (casioemu::BeginStartupModal(ThemeManager::Instance().Colors(), "LanguageChooser",
					"Choose Language", &language_open, ImVec2(430.0f, 195.0f))) {
				ImGui::TextWrapped("Please choose your language to continue.");
				ImGui::Dummy(ImVec2(0.0f, 4.0f));
				if (!languages.empty()) {
					const char* preview_value = languages[selected_language_index].c_str();
					ImGui::SetNextItemWidth(-1.0f);
					if (ImGui::BeginCombo("##language_choice", preview_value)) {
						for (int n = 0; n < (int)languages.size(); n++) {
							const bool is_selected = (selected_language_index == n);
							if (ImGui::Selectable(languages[n].c_str(), is_selected)) {
								selected_language_index = n;
							}
							if (is_selected) {
								ImGui::SetItemDefaultFocus();
							}
						}
						ImGui::EndCombo();
					}
				}
				ImGui::Dummy(ImVec2(0.0f, 8.0f));
				if (ImGui::Button("Ok", ImVec2(120, 26))) {
					if (!languages.empty() && selected_language_index < languages.size()) {
						const std::string& selected_lang = languages[selected_language_index];
						g_local.ChangeLanguage(selected_lang);
						ThemeManager::Instance().RequestFontRebuild();
						ImGui::CloseCurrentPopup();
						DiscordRPC::UpdatePresence("");
					}
				}
				casioemu::EndStartupModal();
			}
			ImGui::EndFrame();
			ImGui::Render();
			SDL_RenderSetScale(renderer2, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
			SDL_SetRenderDrawColor(renderer2, 0, 0, 0, 255);
			SDL_RenderClear(renderer2);
			ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
			SDL_RenderPresent(renderer2);
			if (const char* shot = std::getenv("CASIOEMU_SHOT_STARTUP")) {
				static int shot_frame = 0;
				++shot_frame;
				const int wanted = [] {
					const char* value = std::getenv("CASIOEMU_SHOT_FRAMES");
					return value ? std::max(1, std::atoi(value)) : 12;
				}();
				if (shot_frame >= wanted) {
					const bool ok = casioemu::shot::SaveRendererPng(renderer2, shot);
					std::printf("[shot] %s startup=%s\n", shot, ok ? "ok" : "failed");
					done = true;
				}
			}
			needs_render = false;
		}
		DiscordRPC::Update();
		ThemeManager::Instance().ProcessFontRebuild();
		if (!ui.selected_path.empty()) {
			done = true;
		}
	}
	exited = true;
	if (t3.joinable()) {
		t3.join();
	}
	ImGui_ImplSDLRenderer2_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();
	for (auto& wind : *windows2) {
		delete wind;
	}
	delete windows2;
	SDL_DestroyRenderer(renderer2);
	SDL_DestroyWindow(window2);
	std::ofstream ofs{"recent.bin", std::ofstream::binary};
	if (ofs)
		Binary::Write(ofs, ui.recently_used);
	else {
		std::cout << "[Warn] Cannot write to recent.bin.\n";
	}
	IMG_Quit();
	SDL_Quit();
	return {ui.selected_path.string(), std::move(ui.selected_resources)};
}
