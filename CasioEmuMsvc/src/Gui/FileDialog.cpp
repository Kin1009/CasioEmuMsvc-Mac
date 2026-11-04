#include "FileDialog.hpp"
#include "ToolUi.hpp"
#include <Localization.h>
#include <cstdlib>

namespace FileDialog {
    struct FileFilter {
        std::string name;
        std::vector<std::string> extensions;
    };

    std::vector<FileFilter> ParseFilters(const char* filters) {
        std::vector<FileFilter> result;
        std::string input(filters);
        
        size_t pos = 0;
        while (pos < input.length()) {
            size_t nameEnd = input.find('{', pos);
            if (nameEnd == std::string::npos) break;
            
            FileFilter filter;
            filter.name = input.substr(pos, nameEnd - pos);
            
            size_t extStart = nameEnd + 1;
            size_t extEnd = input.find('}', extStart);
            if (extEnd == std::string::npos) break;
            
            std::string extensions = input.substr(extStart, extEnd - extStart);
            size_t extPos = 0;
            while (extPos < extensions.length()) {
                size_t nextPos = extensions.find(',', extPos);
                if (nextPos == std::string::npos) {
                    filter.extensions.push_back(extensions.substr(extPos));
                    break;
                }
                
                filter.extensions.push_back(extensions.substr(extPos, nextPos - extPos));
                extPos = nextPos + 1;
            }
            
            result.push_back(filter);
            pos = extEnd + 1;
            
            // Skip the comma after the filter
            if (pos < input.length() && input[pos] == ',') {
                pos++;
            }
        }
        
        return result;
    }

    bool ShowFileDialog(const char* title, const char* filters, char* selectedFilePath, size_t pathBufferSize, bool isSaveDialog) {
        static bool doneSelection = false;
        static std::string currentPath = ".";
        static std::vector<FileFilter> currentFilters;
        static int selectedFilterIndex = 0;
        static bool initialized = false;
        if (!initialized) {
            doneSelection = false;
            currentFilters = ParseFilters(filters);
            initialized = true;
        }

        bool dialogClosed = false;

        {
            ToolUi::DialogScope dialog(title, title, 560.0f);
            if (dialog) {
                ToolUi::Section("Location");
                {
                    ToolUi::PanelScope panel("##file_location");
                    ImGui::TextWrapped("%s: %s", "Files.CurrentPath"_lc, currentPath.c_str());
                    ImGui::Spacing();
                    if (ImGui::Button("Files.Back"_lc)) {
                        std::filesystem::path path(currentPath);
                        currentPath = path.parent_path().string();
                        if (currentPath.empty())
                            currentPath = ".";
                    }
                    ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Home").x + 20.0f);
                    if (ImGui::Button("Home")) {
                        const char* home = std::getenv("HOME");
                        if (home && *home)
                            currentPath = home;
                    }
                }

                ToolUi::Section("Files");
                ImGui::BeginChild("Files", ImVec2(0, std::max(160.0f, ImGui::GetContentRegionAvail().y - 96.0f)), ImGuiChildFlags_Border);

                try {
                    for (const auto& entry : std::filesystem::directory_iterator(currentPath)) {
                        const bool isDirectory = entry.is_directory();
                        const std::string filename = entry.path().filename().string();

                        if (isDirectory) {
                            if (ImGui::Selectable((filename + "/").c_str(), false))
                                currentPath = entry.path().string();
                        }
                    }

                    for (const auto& entry : std::filesystem::directory_iterator(currentPath)) {
                        const bool isDirectory = entry.is_directory();
                        const std::string filename = entry.path().filename().string();

                        if (!isDirectory) {
                            bool showFile = currentFilters.empty();

                            if (!showFile && selectedFilterIndex < (int)currentFilters.size()) {
                                for (const auto& ext : currentFilters[selectedFilterIndex].extensions) {
                                    std::string fileExt = entry.path().extension().string();
                                    if (ext == ".*" || (!ext.empty() && ext[0] == '.' && fileExt == ext)) {
                                        showFile = true;
                                        break;
                                    }
                                }
                            }

                            if (showFile) {
                                if (ImGui::Selectable(filename.c_str(), false)) {
                                    strncpy(selectedFilePath, (currentPath + "/" + filename).c_str(), pathBufferSize);
                                    selectedFilePath[pathBufferSize - 1] = '\0';
                                    doneSelection = true;
                                }
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    std::string error = std::string("Cannot read this folder: ") + e.what();
                    ToolUi::Status(error.c_str(), ToolUi::Palette().error);
                }
                ImGui::EndChild();

                // Filter selection
                if (!currentFilters.empty()) {
                    ToolUi::Field("Filter");
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    if (ImGui::BeginCombo("##FilterCombo", currentFilters[selectedFilterIndex].name.c_str())) {
                        for (int i = 0; i < (int)currentFilters.size(); i++) {
                            const bool isSelected = (selectedFilterIndex == i);
                            if (ImGui::Selectable(currentFilters[i].name.c_str(), isSelected))
                                selectedFilterIndex = i;
                            if (isSelected)
                                ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                }

                ImGui::Spacing();
                const bool accept = ImGui::Button(isSaveDialog ? "Files.Save"_lc : "Files.Open"_lc, ImVec2(110, 0));
                ToolUi::SameLineOrWrap(ImGui::CalcTextSize("Files.Cancel"_lc).x + 20.0f);
                const bool cancel = ImGui::Button("Files.Cancel"_lc, ImVec2(110, 0)) || dialog.CloseRequested();

                if (accept && doneSelection) {
                    ImGui::CloseCurrentPopup();
                    initialized = false;
                    dialogClosed = true;
                }
                else if (cancel) {
                    ImGui::CloseCurrentPopup();
                    initialized = false;
                    dialogClosed = true;
                }
            }
            else if (!ImGui::IsPopupOpen(title)) {
                ImGui::OpenPopup(title);
            }
        }

        return dialogClosed;
    }

    bool ShowFileOpenDialog(const char* title, const char* filters, char* selectedFilePath, size_t pathBufferSize) {
        return ShowFileDialog(title, filters, selectedFilePath, pathBufferSize, false);
    }

    bool ShowFileSaveDialog(const char* title, const char* filters, char* selectedFilePath, size_t pathBufferSize) {
        return ShowFileDialog(title, filters, selectedFilePath, pathBufferSize, true);
    }
}