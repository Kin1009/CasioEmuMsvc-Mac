// CwModeWindow.cpp - add modes to the stock firmware from the debugger.
//
// The Wiz Compiler adds a #cwapp program to the ROM; this companion panel adds
// whole modes (icon, name and entry point) using the cwmode library.  The patch
// is written as a session image, so the ROM resource on disk stays untouched
// and can be restored from the Wiz Compiler's "Stock ROM" action.
#include "CwModeWindow.h"

#ifndef CASIOEMU_CORE_WEB

#include "Chipset/Chipset.hpp"
#include "Ext/SysDialog.h"
#include "Ui.hpp"
#include "common.hpp"
#include "cwmode.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr int kMaxRows = 8;

struct ModeRow {
    char code[8] = "E1";
    char icon[512] = "";
    char text0[512] = "";
    char text1[512] = "";
    char entry[16] = "31000";
};

bool ParseHex(const char* text, uint32_t& out) {
    if (!text) return false;
    const char* p = text;
    while (*p == ' ' || *p == '\t') ++p;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    uint32_t value = 0;
    int digits = 0;
    for (; *p; ++p) {
        if (*p == ' ' || *p == '\t') break;
        int d;
        if (*p >= '0' && *p <= '9')
            d = *p - '0';
        else if (*p >= 'a' && *p <= 'f')
            d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F')
            d = *p - 'A' + 10;
        else
            return false;
        value = value * 16 + (uint32_t)d;
        if (++digits > 6) return false;
    }
    if (!digits) return false;
    out = value;
    return true;
}

void CopyPath(char* dst, size_t size, const std::filesystem::path& path) {
    const std::string text = path.string();
    std::snprintf(dst, size, "%s", text.c_str());
}

class CwModeWindow : public UIWindow {
public:
    CwModeWindow() : UIWindow("Add Modes") {
        inital_size = ImVec2(760, 420);
        rows_[0] = ModeRow{};
    }

    void RenderCore() override {
        if (!m_emu) {
            ImGui::TextUnformatted("No running model.");
            return;
        }
        const std::string& model = m_emu->ModelDefinition.model_name;
        const bool is580 = model.find("580") != std::string::npos;
        const bool is880 = model.find("880") != std::string::npos;
        if (!is580 && !is880) {
            ImGui::TextWrapped(
                "Add Modes is available for the fx-580VN X and fx-880BTG models only.");
            return;
        }

        ImGui::TextWrapped(
            "Adds whole modes to the stock firmware using the cwmode patcher. The ROM "
            "on disk is not modified: the result is stored as a session image and the "
            "model is rebooted on it.");
        ImGui::Text("Model: %s   (%s)", model.c_str(),
                    is580 ? "0x40000 ROM, text names" : "0x60000 code window, 64x13 text images");
        ImGui::TextDisabled(
            "Appended modes take the next shortcut labels in order: D, E, F, x, y, z, M, "
            "+, -, *, /, .");
        ImGui::TextDisabled(
            "Legends: A (-), B deg, C x^-1, D sin, E cos, F tan, x (, y ), z s<>d, M M+.");
        ImGui::Separator();

        int count = count_;
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Modes to add", &count, 1, 1)) {
            count_ = std::clamp(count, 1, kMaxRows);
        }
        ImGui::SameLine();
        ImGui::Checkbox("Overwrite non-blank flash", &force_);

        const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##cwmode_rows", 5, flags)) {
            ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Icon", ImGuiTableColumnFlags_WidthStretch, 1.5f);
            ImGui::TableSetupColumn(is580 ? "Name 0" : "Text image 0",
                                    ImGuiTableColumnFlags_WidthStretch, 1.5f);
            ImGui::TableSetupColumn(is580 ? "Name 1" : "Text image 1",
                                    ImGuiTableColumnFlags_WidthStretch, 1.5f);
            ImGui::TableSetupColumn("Entry", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableHeadersRow();
            for (int i = 0; i < count_; ++i) {
                ImGui::TableNextRow();
                ImGui::PushID(i);
                ImGui::TableSetColumnIndex(0);
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##code", rows_[i].code, sizeof rows_[i].code);
                ImGui::TableSetColumnIndex(1);
                RenderPathCell(rows_[i].icon);
                ImGui::TableSetColumnIndex(2);
                RenderPathCell(rows_[i].text0);
                ImGui::TableSetColumnIndex(3);
                RenderPathCell(rows_[i].text1);
                ImGui::TableSetColumnIndex(4);
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##entry", rows_[i].entry, sizeof rows_[i].entry);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        if (ImGui::Button("Patch ROM and reboot")) Patch(is580);

        if (!status_.empty()) {
            ImGui::Spacing();
            ImGui::TextWrapped("%s", status_.c_str());
        }
    }

private:
    void RenderPathCell(char* path) {
        ImGui::SetNextItemWidth(-70.0f);
        ImGui::InputText("##path", path, 512);
        ImGui::SameLine();
        if (ImGui::SmallButton("...")) {
            SystemDialogs::OpenFileDialog(
                [path](std::filesystem::path chosen) { CopyPath(path, 512, chosen); });
        }
    }

    void Patch(bool is580) {
        status_.clear();
        std::vector<uint8_t> rom;
        try {
            if (m_emu->HasModelResource("rom.bin"))
                rom = m_emu->ReadModelResource("rom.bin");
            else
                rom = m_emu->ReadModelResource(m_emu->ModelDefinition.rom_path);
        } catch (const std::exception& error) {
            status_ = std::string("Cannot read the ROM: ") + error.what();
            return;
        }

        wiz::cwmode::Options options;
        options.model = is580 ? wiz::cwmode::Model::Cw580 : wiz::cwmode::Model::Cw880;
        options.force = force_;

        std::vector<wiz::cwmode::ModeSpec> modes;
        for (int i = 0; i < count_; ++i) {
            wiz::cwmode::ModeSpec spec;
            uint32_t code = 0;
            if (!ParseHex(rows_[i].code, code) || code > 0xFF) {
                status_ = wiz::fmt("Mode %d: the code must be a hex byte (for example E1)", i + 1);
                return;
            }
            spec.code = static_cast<uint8_t>(code);
            if (!ParseHex(rows_[i].entry, spec.entry)) {
                status_ = wiz::fmt("Mode %d: the entry address is not hex", i + 1);
                return;
            }
            std::string error;
            std::string icon = rows_[i].icon;
            if (icon.empty()) {
                status_ = wiz::fmt("Mode %d: choose an icon", i + 1);
                return;
            }
            if (!wiz::cwmode::load_image(icon, spec.icon, error)) {
                status_ = wiz::fmt("Mode %d: %s", i + 1, error.c_str());
                return;
            }
            if (is580) {
                spec.names = {rows_[i].text0, rows_[i].text1};
            } else {
                for (const char* path : {rows_[i].text0, rows_[i].text1}) {
                    wiz::cwmode::Image image;
                    if (!*path) {
                        status_ = wiz::fmt(
                            "Mode %d: the 880 needs two 64x13 text images per mode", i + 1);
                        return;
                    }
                    if (!wiz::cwmode::load_image(path, image, error)) {
                        status_ = wiz::fmt("Mode %d: %s", i + 1, error.c_str());
                        return;
                    }
                    spec.text_images.push_back(std::move(image));
                }
            }
            modes.push_back(std::move(spec));
        }

        wiz::cwmode::PatchResult result = wiz::cwmode::patch(rom, modes, options);
        if (!result.ok) {
            status_ = std::string("Cannot patch: ") + result.error;
            return;
        }

        const std::string name = wiz::fmt("wiz/rom-modes-%d.bin", count_);
        try {
            m_emu->WriteModelSessionResource(name, result.rom);
        } catch (const std::exception& error) {
            status_ = std::string("Cannot write the patched image: ") + error.what();
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
            if (!loaded) m_emu->ModelDefinition.rom_path = previous;
        }
        m_emu->SetPaused(was_paused);
        if (!loaded) {
            status_ = "The emulator refused the patched image: " + error;
            return;
        }
        status_ = wiz::fmt(
            "Added %d mode(s): %u -> %u, %u bytes at %X:%04X. The stock ROM on disk "
            "is unchanged; use Stock ROM in the Wiz Compiler to switch back.",
            count_, result.analysis.mode_count, result.mode_count_after,
            result.final_cursor - result.target, (result.target >> 16) & 0xFFFF,
            result.target & 0xFFFF);
    }

    int count_ = 1;
    std::array<ModeRow, kMaxRows> rows_{};
    bool force_ = false;
    std::string status_;
};

}  // namespace

UIWindow* CreateCwModeWindow() { return new CwModeWindow(); }

#else

UIWindow* CreateCwModeWindow() { return nullptr; }

#endif
