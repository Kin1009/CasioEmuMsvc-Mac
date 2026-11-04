#include "SnapshotWindow.h"
#include "Snapshot.h"
#include "Emulator.hpp"
#include "ToolUi.hpp"
#include "Ui.hpp"
#ifdef CASIOEMU_CORE_WEB
#include "WebDebuggerGui.h"
#else
#include "Ext/SysDialog.h"
#endif
#include "imgui/imgui.h"
#include <SDL.h>
#include <SDL_image.h>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include "Localization.h"

namespace {
constexpr const char* kSnapshotAutoSaveFileName = "snapshots.snapshot";
}

// ============================================================
// Helpers
// ============================================================

static std::string FormatTimestamp(int64_t ts) {
    time_t t = static_cast<time_t>(ts);
    struct tm tm_info;
#ifdef _WIN32
    localtime_s(&tm_info, &t);
#else
    localtime_r(&t, &tm_info);
#endif
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    return buf;
}

#ifdef CASIOEMU_CORE_WEB
static std::filesystem::path WebSnapshotExportPath(const char* fileName) {
    const auto dir = std::filesystem::path(WebDebuggerExportDir());
    std::filesystem::create_directories(dir);
    return dir / fileName;
}

static std::filesystem::path WebSnapshotImportPath() {
    const auto dir = std::filesystem::path("/tmp/imports");
    std::filesystem::create_directories(dir);
    return dir / "snapshot.snapshot";
}
#endif

static std::filesystem::path SnapshotAutoSavePath() {
    if (!::m_emu) return {};
    return ::m_emu->GetModelFilePath(kSnapshotAutoSaveFileName);
}

static void RequestSnapshotFsSync() {
#ifdef CASIOEMU_CORE_WEB
    WebDebuggerRequestFsSync();
#endif
}

// ============================================================
// SnapshotWindow
// ============================================================

SnapshotWindow::SnapshotWindow()
    : UIWindow("Snapshots"/*"SnapshotWindow.Title"_lc*/) { // Reminder here: don't localize ImGui window titles as they are linked to imgui presistent storage key
    inital_size = ImVec2(880, 560);
    memset(m_LabelBuf, 0, sizeof(m_LabelBuf));
}

SnapshotWindow::~SnapshotWindow() {
    m_Preview.Free();
}

void SnapshotWindow::ShowError(const std::string& msg) {
    m_ErrorMsg  = msg;
    m_ShowError = true;
}

void SnapshotWindow::LoadPreview(const SnapshotNode& node) {
    if (m_Preview.NodeId == node.Id && m_Preview.Tex) return;
    m_Preview.Free();
    m_Preview.NodeId = node.Id;
    if (node.PreviewPng.empty() || !::renderer) return;

    // Load BMP from memory using SDL_RWops
    SDL_RWops* rw = SDL_RWFromConstMem(node.PreviewPng.data(),
                                       static_cast<int>(node.PreviewPng.size()));
    if (!rw) return;

    SDL_Surface* surface = SDL_LoadBMP_RW(rw, 1); // closes rw
    if (!surface) return;

    m_Preview.Tex    = SDL_CreateTextureFromSurface(::renderer, surface);
    m_Preview.Width  = surface->w;
    m_Preview.Height = surface->h;
    SDL_FreeSurface(surface);
}

void SnapshotWindow::TryLoadPreview(uint32_t id) {
    for (const auto& n : m_Manager.Nodes)
        if (n.Id == id) { LoadPreview(n); return; }
    m_Preview.Free();
}

void SnapshotWindow::CheckWebImportResult() {
#ifdef CASIOEMU_CORE_WEB
    if (m_WebImportPath.empty()) return;
    int result = 0;
    if (!WebDebuggerConsumeFileResult(m_WebImportPath.string().c_str(), &result)) return;
    const auto importPath = m_WebImportPath;
    m_WebImportPath.clear();
    if (result != 0) {
        if (result != 1) ShowError("Snapshot import was not completed.");
        return;
    }
    try {
        m_Manager.ImportFromFile(importPath);
        RequestSnapshotFsSync();
    } catch (const std::exception& e) {
        ShowError(e.what());
    }
#endif
}

void SnapshotWindow::EnsureAutoSaveLoaded() {
    const auto path = SnapshotAutoSavePath();
    if (path.empty() || path == m_AutoSavePath) return;

    m_AutoSavePath = path;
    m_Manager = SnapshotManager();
    m_Manager.SetAutoSavePath(path);
    m_SelectedId = 0;
    m_Preview.Free();
    try {
        m_Manager.LoadAutoSaveFile();
    } catch (const std::exception& e) {
        ShowError(e.what());
    }
}

// ============================================================
// Toolbar
// ============================================================

void SnapshotWindow::RenderToolbar() {
    bool hasEmu = (::m_emu != nullptr);
    bool hasSelected = (m_SelectedId != 0 && hasEmu);

    ToolUi::Section("Snapshots");
    ToolUi::Description("Save the full machine state, branch from an existing snapshot, and export or import snapshot files.");
    ImGui::Spacing();

    {
        ToolUi::PanelScope panel("##snapshot_actions");

        // --- Save / load / delete ---
        if (!hasEmu) ImGui::BeginDisabled();
        if (ToolUi::AccentButton("SnapshotWindow.Save"_lc)) {
            try {
                std::string lbl(m_LabelBuf);
                if (lbl.empty()) lbl = std::string("SnapshotWindow.SnapshotPrefix"_lc) + std::to_string(m_Manager.Nodes.size() + 1);
                uint32_t newId = m_Manager.SaveSnapshot(*::m_emu, m_SelectedId, lbl);
                RequestSnapshotFsSync();
                m_SelectedId = newId;
                TryLoadPreview(newId);
                memset(m_LabelBuf, 0, sizeof(m_LabelBuf));
            } catch (const std::exception& e) {
                ShowError(e.what());
            }
        }
        if (!hasEmu) ImGui::EndDisabled();
        ToolUi::SameLineOrWrap(ImGui::CalcTextSize("SnapshotWindow.Load"_lc).x + 20.0f);

        if (!hasSelected) ImGui::BeginDisabled();
        if (ImGui::Button("SnapshotWindow.Load"_lc)) {
            try {
                m_Manager.LoadSnapshot(*::m_emu, m_SelectedId);
                m_LoadSuccessTime = ImGui::GetTime();
            } catch (const std::exception& e) {
                ShowError(e.what());
            }
        }
        if (!hasSelected) ImGui::EndDisabled();
        ToolUi::SameLineOrWrap(ImGui::CalcTextSize("SnapshotWindow.Delete"_lc).x + 20.0f);

        if (!hasSelected) ImGui::BeginDisabled();
        if (ImGui::Button("SnapshotWindow.Delete"_lc)) {
            if (m_SelectedId != 0) {
                m_NodeToDelete = m_SelectedId;
                ImGui::OpenPopup("Delete Confirmation###snapshot_delete");
            }
        }
        if (!hasSelected) ImGui::EndDisabled();

        ImGui::Spacing();
        ToolUi::Hint("Selected snapshots can be exported alone or with their child snapshots.");

        // --- Export / import ---
        if (!hasSelected) ImGui::BeginDisabled();
        if (ImGui::Button("SnapshotWindow.ExportNode"_lc)) {
            if (m_SelectedId != 0) {
                uint32_t exportId = m_SelectedId;
#ifdef CASIOEMU_CORE_WEB
                try {
                    auto path = WebSnapshotExportPath("snapshot.snapshot");
                    m_Manager.ExportNode(path, exportId);
                    WebDebuggerQueueDownload(path.string().c_str(), "snapshot.snapshot");
                } catch (const std::exception& e) { ShowError(e.what()); }
#else
                SystemDialogs::SaveFileDialog("snapshot.snapshot", [this, exportId](std::filesystem::path path) {
                    try { m_Manager.ExportNode(path, exportId); }
                    catch (const std::exception& e) { ShowError(e.what()); }
                });
#endif
            }
        }
        if (!hasSelected) ImGui::EndDisabled();
        ToolUi::SameLineOrWrap(ImGui::CalcTextSize("SnapshotWindow.ExportSubtree"_lc).x + 20.0f);

        if (!hasSelected) ImGui::BeginDisabled();
        if (ImGui::Button("SnapshotWindow.ExportSubtree"_lc)) {
            if (m_SelectedId != 0) {
                uint32_t exportId = m_SelectedId;
#ifdef CASIOEMU_CORE_WEB
                try {
                    auto path = WebSnapshotExportPath("snapshot.snapshot");
                    m_Manager.ExportSubtree(path, exportId);
                    WebDebuggerQueueDownload(path.string().c_str(), "snapshot.snapshot");
                } catch (const std::exception& e) { ShowError(e.what()); }
#else
                SystemDialogs::SaveFileDialog("snapshot.snapshot", [this, exportId](std::filesystem::path path) {
                    try { m_Manager.ExportSubtree(path, exportId); }
                    catch (const std::exception& e) { ShowError(e.what()); }
                });
#endif
            }
        }
        if (!hasSelected) ImGui::EndDisabled();
        ToolUi::SameLineOrWrap(ImGui::CalcTextSize("SnapshotWindow.ExportAll"_lc).x + 20.0f);

        if (ImGui::Button("SnapshotWindow.ExportAll"_lc)) {
#ifdef CASIOEMU_CORE_WEB
            try {
                auto path = WebSnapshotExportPath("snapshots.snapshot");
                m_Manager.ExportAll(path);
                WebDebuggerQueueDownload(path.string().c_str(), "snapshots.snapshot");
            } catch (const std::exception& e) { ShowError(e.what()); }
#else
            SystemDialogs::SaveFileDialog("snapshots.snapshot", [this](std::filesystem::path path) {
                try { m_Manager.ExportAll(path); }
                catch (const std::exception& e) { ShowError(e.what()); }
            });
#endif
        }
        ToolUi::SameLineOrWrap(ImGui::CalcTextSize("SnapshotWindow.Import"_lc).x + 20.0f);

        if (ImGui::Button("SnapshotWindow.Import"_lc)) {
#ifdef CASIOEMU_CORE_WEB
            m_WebImportPath = WebSnapshotImportPath();
            WebDebuggerQueueOpenFile(m_WebImportPath.string().c_str(), "snapshot.snapshot");
#else
            SystemDialogs::OpenFileDialog([this](std::filesystem::path path) {
                try { m_Manager.ImportFromFile(path); }
                catch (const std::exception& e) { ShowError(e.what()); }
                RequestSnapshotFsSync();
            });
#endif
        }
    }

    // --- Label input ---
    ToolUi::Field("Label");
    ImGui::SetNextItemWidth(std::min(300.0f, ToolUi::RemainingWidth()));
    ImGui::InputTextWithHint("##snapshot_label", "SnapshotWindow.LabelHint"_lc, m_LabelBuf, sizeof(m_LabelBuf));
}

// ============================================================
// Tree
// ============================================================

void SnapshotWindow::RenderTreeNode(uint32_t parentId, int depth) {
    for (const auto& node : m_Manager.Nodes) {
        if (node.ParentId != parentId) continue;

        bool isRoot = (node.ParentId == 0);
        bool hasChildren = false;
        for (const auto& c : m_Manager.Nodes)
            if (c.ParentId == node.Id && c.Id != node.Id) { hasChildren = true; break; }

        std::string displayLabel = node.Label.empty()
            ? (std::string("SnapshotWindow.NodePrefix"_lc) + std::to_string(node.Id))
            : node.Label;
        displayLabel += "  [" + FormatTimestamp(node.Timestamp) + "]";

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow;
        if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf;
        if (node.Id == m_SelectedId) flags |= ImGuiTreeNodeFlags_Selected;

        bool open = ImGui::TreeNodeEx(
            reinterpret_cast<void*>(static_cast<intptr_t>(node.Id)),
            flags, "%s", displayLabel.c_str());

        if (ImGui::IsItemClicked()) {
            m_SelectedId = node.Id;
            TryLoadPreview(node.Id);
        }

        // Right-click context menu
        ToolUi::ContextPopupScope popup;
        if (popup) {
            if (ImGui::MenuItem("SnapshotWindow.SaveChild"_lc)) {
                if (::m_emu) {
                    try {
                        std::string lbl(m_LabelBuf);
                        if (lbl.empty()) lbl = std::string("SnapshotWindow.ChildOf"_lc) + std::to_string(node.Id);
                        uint32_t newId = m_Manager.SaveSnapshot(*::m_emu, node.Id, lbl);
                        RequestSnapshotFsSync();
                        m_SelectedId = newId;
                        TryLoadPreview(newId);
                        memset(m_LabelBuf, 0, sizeof(m_LabelBuf));
                    } catch (const std::exception& e) { ShowError(e.what()); }
                }
            }
            if (ImGui::MenuItem("SnapshotWindow.LoadThis"_lc)) {
                if (::m_emu) {
                    try { m_Manager.LoadSnapshot(*::m_emu, node.Id); }
                    catch (const std::exception& e) { ShowError(e.what()); }
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("SnapshotWindow.DeleteWithChildren"_lc)) {
                m_NodeToDelete = node.Id;
                ImGui::OpenPopup("Delete Confirmation###snapshot_delete");
            }
        }

        if (open) {
            RenderTreeNode(node.Id, depth + 1);
            ImGui::TreePop();
        }
    }
}

void SnapshotWindow::RenderTree() {
    const float avail = ToolUi::RemainingWidth();
    const bool side_by_side = avail >= 520.0f;
    float width = side_by_side ? std::max(250.0f, avail * 0.35f) : 0.0f;
    float height = side_by_side ? 0.0f : 190.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::BeginChild("##snap_tree", ImVec2(width, height), ImGuiChildFlags_Border);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));
    if (m_Manager.Nodes.empty()) {
        ToolUi::EmptyState("SnapshotWindow.NoSnapshots"_lc);
    } else {
        RenderTreeNode(0, 0);
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

// ============================================================
// Details panel (preview + info)
// ============================================================

void SnapshotWindow::RenderDetails() {
    ImGui::BeginChild("##snap_detail", ImVec2(0, 0), ImGuiChildFlags_None);

    // Find selected node
    const SnapshotNode* sel = nullptr;
    for (const auto& n : m_Manager.Nodes)
        if (n.Id == m_SelectedId) { sel = &n; break; }

    if (!sel) {
        ToolUi::EmptyState("SnapshotWindow.SelectToSeeDetails"_lc);
        ImGui::EndChild();
        return;
    }

    // Preview image
    if (m_Preview.Tex && m_Preview.NodeId == m_SelectedId) {
        float avail = ImGui::GetContentRegionAvail().x;
        float imgW  = static_cast<float>(m_Preview.Width);
        float imgH  = static_cast<float>(m_Preview.Height);
        float scale = avail / imgW;
        if (scale > 1.0f) scale = 1.0f;
        ImGui::Image(reinterpret_cast<ImTextureID>(m_Preview.Tex),
                     ImVec2(imgW * scale, imgH * scale));
    } else {
        ToolUi::EmptyState("SnapshotWindow.NoPreview"_lc);
    }

    ImGui::Spacing();
    {
        ToolUi::PanelScope panel("##snapshot_meta");
        ImGui::Text("SnapshotWindow.ID"_lc, sel->Id);
        ImGui::Text("SnapshotWindow.ParentID"_lc, sel->ParentId);
        ImGui::Text("SnapshotWindow.LabelFmt"_lc, sel->Label.c_str());
        ImGui::Text("SnapshotWindow.SavedFmt"_lc, FormatTimestamp(sel->Timestamp).c_str());
        ImGui::Text("SnapshotWindow.StateFmt"_lc,
                    static_cast<size_t>(sel->UncompressedStateSize),
                    sel->CompressedState.size());
        ImGui::Text("SnapshotWindow.PreviewFmt"_lc, sel->PreviewPng.size());
    }

    if (ImGui::GetTime() - m_LoadSuccessTime < 3.0) {
        ImGui::Spacing();
        ToolUi::Status("Loaded", UIHelpers::kColorSuccess);
    }

    ImGui::EndChild();
}

// ============================================================
// Main render
// ============================================================

void SnapshotWindow::RenderCore() {
    ToolUi::PageScope page("Snapshots");
    EnsureAutoSaveLoaded();
    CheckWebImportResult();
    RenderToolbar();

    // Error popup
    if (m_ShowError) {
        ImGui::OpenPopup("SnapshotWindow.ErrorTitle"_lc);
        m_ShowError = false;
    }
    {
        ToolUi::DialogScope dialog("SnapshotWindow.ErrorTitle"_lc, "Snapshot error", 480.0f);
        if (dialog) {
            ImGui::TextWrapped("%s", m_ErrorMsg.c_str());
            ImGui::Spacing();
            if (ToolUi::AccentButton("Button.Positive"_lc, ImVec2(120, 0)) || dialog.CloseRequested())
                ImGui::CloseCurrentPopup();
        }
    }

    // Delete Confirmation Popup
    {
        ToolUi::DialogScope dialog("Delete Confirmation###snapshot_delete", "Delete snapshot", 480.0f);
        if (dialog) {
            ImGui::TextUnformatted("Are you sure you want to delete this snapshot?");
            if (m_NodeToDelete != 0) {
                for (const auto& node : m_Manager.Nodes) {
                    if (node.Id == m_NodeToDelete) {
                        ImGui::TextDisabled("Snapshot: %s", node.Label.empty() ? (std::string("Node ") + std::to_string(node.Id)).c_str() : node.Label.c_str());
                        break;
                    }
                }
            }
            ImGui::Spacing();
            if (ToolUi::AccentButton("Button.Positive"_lc, ImVec2(120, 0))) {
                if (m_NodeToDelete != 0) {
                    m_Manager.DeleteNode(m_NodeToDelete);
                    RequestSnapshotFsSync();
                    if (m_SelectedId == m_NodeToDelete) {
                        m_SelectedId = 0;
                        m_Preview.Free();
                    }
                    m_NodeToDelete = 0;
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Button.Negative"_lc, ImVec2(120, 0)) || dialog.CloseRequested()) {
                m_NodeToDelete = 0;
                ImGui::CloseCurrentPopup();
            }
        }
    }

    // Tree | details side by side when there is room; stacked when the dock is
    // narrow so the details pane is never squeezed to nothing.
    ToolUi::Section("Snapshot Tree");
    const bool side_by_side = ToolUi::RemainingWidth() >= 520.0f;
    RenderTree();
    if (side_by_side)
        ImGui::SameLine();
    RenderDetails();
}

UIWindow* CreateSnapshotWindow() { return new SnapshotWindow(); }
