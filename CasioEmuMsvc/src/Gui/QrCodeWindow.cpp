#include "QrCodeWindow.h"

#include "Emulator.hpp"
#include "Localization.h"
#include "ToolUi.hpp"
#include "imgui/imgui.h"
#include <string>
#include <SDL.h>

namespace {
	void RenderPayloadActions(const std::string& data) {
		if (ToolUi::AccentButton("QrCode.Copy"_lc)) {
			ImGui::SetClipboardText(data.c_str());
		}
		if (data.starts_with("http://") || data.starts_with("https://")) {
			ImGui::SameLine();
			if (ImGui::Button("QrCode.OpenUrl"_lc)) {
				SDL_OpenURL(data.c_str());
			}
		}
	}
}

QrCodeWindow::QrCodeWindow() : UIWindow("QR Code") {
	inital_size = ImVec2(700, 420);
}

void QrCodeWindow::RenderCore() {
	ToolUi::PageScope page("QR Code");
	m_emu->qr_code.Poll(*m_emu);
	const auto qr = m_emu->qr_code.GetState();

	ToolUi::Section("Status");
	{
		ToolUi::PanelScope panel("##qr_status");
		ImGui::Text("QrCode.StatusFmt"_lc, qr.Active ? "QrCode.Active"_lc : "QrCode.Inactive"_lc);
		ImGui::SameLine();
		ImGui::Text("QrCode.CompleteFmt"_lc, qr.Complete ? "QrCode.Yes"_lc : "QrCode.No"_lc);
		ImGui::SameLine();
		ImGui::Text("QrCode.VersionFmt"_lc, qr.Version);
		ImGui::NewLine();
		ImGui::Text("QrCode.LengthFmt"_lc, qr.Data.size());
		ImGui::SameLine();
		ImGui::Text("QrCode.HistoryFmt"_lc, qr.History.size());
		if (qr.Active && qr.RealTotalPages != 0) {
			ImGui::NewLine();
			ImGui::Text("QrCode.RealRomProgressFmt"_lc,
				static_cast<unsigned>(qr.RealCurrentPage),
				static_cast<unsigned>(qr.RealTotalPages),
				qr.RealPageLengths.size(),
				static_cast<unsigned>(qr.RealTotalPages));
		}
	}

	ToolUi::Section("QrCode.Current"_lc);
	if (qr.Active && qr.Complete) {
		RenderPayloadActions(qr.Data);
		ImGui::Spacing();
		ToolUi::PanelScope payload("##qr_current_payload", 96.0f);
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextUnformatted(qr.Data.c_str());
		ImGui::PopTextWrapPos();
	}
	else if (qr.Active) {
		ToolUi::EmptyState("QrCode.IncompleteCapture"_lc);
		if (!qr.RealCurrentPageData.empty()) {
			ToolUi::PanelScope payload("##qr_current_page_payload", 96.0f);
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(qr.RealCurrentPageData.c_str());
			ImGui::PopTextWrapPos();
		}
	}
	else {
		ToolUi::EmptyState("QrCode.NoCurrent"_lc);
	}

	ToolUi::Section("QrCode.History"_lc);
	if (qr.History.empty()) {
		ToolUi::EmptyState("QrCode.NoHistory"_lc);
		return;
	}

	ImGui::BeginChild("##qr_history", ImVec2(0, std::max(120.0f, ImGui::GetContentRegionAvail().y - 4.0f)));
	for (auto it = qr.History.rbegin(); it != qr.History.rend(); ++it) {
		ImGui::PushID(static_cast<int>(it->Id));
		const auto header = localstr(
			"QrCode.HistoryEntryFmt",
			static_cast<unsigned long long>(it->Id),
			it->Version,
			it->Data.size());
		if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
			RenderPayloadActions(it->Data);
			ImGui::Spacing();
			ToolUi::PanelScope payload("payload", 88.0f);
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(it->Data.c_str());
			ImGui::PopTextWrapPos();
		}
		ImGui::PopID();
	}
	ImGui::EndChild();
}
