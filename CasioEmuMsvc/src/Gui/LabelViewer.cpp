#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include "LabelViewer.h"
#include "Models.h"
#include "ToolUi.hpp"
#include "Ui.hpp"
#include "imgui/imgui.h"
#include "stringhelper.h"

namespace {

std::string Lowercase(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return text;
}

} // namespace

// The label and MMU-region lists are static for a model, so they are built once
// (lowercased search strings included) and only rebuilt when the hardware id or
// the register map changes.  Rendering uses ImGuiListClipper so a 4,000-row
// list only submits the visible rows.
void LabelViewer::RebuildCache() {
	m_labels.clear();
	auto labels = casioemu::GetCommonMemLabels(m_emu->hardware_id);
	std::sort(labels.begin(), labels.end(), [](const MemoryEditor::MarkedSpan& lhs, const MemoryEditor::MarkedSpan& rhs) {
		return lhs.start < rhs.start;
	});
	m_labels.reserve(labels.size());
	for (const auto& label : labels) {
		Row row;
		row.addr = static_cast<uint32_t>(label.start);
		std::string desc = label.desc ? label.desc : "";
		ltrim(desc);
		char address[16];
		std::snprintf(address, sizeof(address), "%X", row.addr);
		row.addr_text = address;
		row.desc = std::move(desc);
		row.search = Lowercase(row.desc + " " + row.addr_text);
		m_labels.push_back(std::move(row));
	}

	m_regions.clear();
	auto regions = me_mmu->GetRegions();
	std::sort(regions.begin(), regions.end(), [](casioemu::MMURegion* lhs, casioemu::MMURegion* rhs) {
		return lhs->base < rhs->base;
	});
	m_regions.reserve(regions.size());
	for (auto* region : regions) {
		if (!region)
			continue;
		Row row;
		row.addr = static_cast<uint32_t>(region->base);
		char address[16];
		std::snprintf(address, sizeof(address), "%X", row.addr);
		row.addr_text = address;
		row.desc = region->description;
		row.search = Lowercase(row.desc + " " + row.addr_text);
		m_regions.push_back(std::move(row));
	}

	// Profiling hook: synthesize a long list to measure scrolling cost without a
	// 4,000-label model.  Only used by the headless perf runs.
	m_perfRows = 0;
	if (const char* requested = std::getenv("CASIOEMU_PERF_ROWS")) {
		const int wanted = std::atoi(requested);
		if (wanted > static_cast<int>(m_labels.size()) && !m_labels.empty()) {
			const std::vector<Row> source = m_labels;
			m_labels.clear();
			m_labels.reserve(static_cast<size_t>(wanted));
			for (int index = 0; index < wanted; ++index) {
				Row row = source[static_cast<size_t>(index) % source.size()];
				row.desc += " #" + std::to_string(index);
				row.search = Lowercase(row.desc + " " + row.addr_text);
				m_labels.push_back(std::move(row));
			}
			m_perfRows = wanted;
		}
	}

	m_cachedHardware = m_emu->hardware_id;
	std::size_t signature = m_regions.size();
	for (const auto& row : m_regions)
		signature = signature * 1315423911u + row.addr;
	m_cachedRegionSignature = signature;
	m_cacheValid = true;
	m_filterValid = false;
}

void LabelViewer::RebuildFilter() {
	const std::string filter = Lowercase(m_SearchBuf);
	m_visibleLabels.clear();
	m_visibleLabels.reserve(m_labels.size());
	for (int index = 0; index < static_cast<int>(m_labels.size()); ++index) {
		if (filter.empty() || m_labels[static_cast<size_t>(index)].search.find(filter) != std::string::npos)
			m_visibleLabels.push_back(index);
	}
	m_visibleRegions.clear();
	m_visibleRegions.reserve(m_regions.size());
	for (int index = 0; index < static_cast<int>(m_regions.size()); ++index) {
		if (filter.empty() || m_regions[static_cast<size_t>(index)].search.find(filter) != std::string::npos)
			m_visibleRegions.push_back(index);
	}
	std::snprintf(m_cachedFilter, sizeof(m_cachedFilter), "%s", m_SearchBuf);
	m_filterValid = true;
}

float LabelViewer::RenderRows(const char* tableId, const std::vector<Row>& rows,
	const std::vector<int>& visible, float height, int& shown) {
	shown = static_cast<int>(visible.size());
	const bool disable_clipper = std::getenv("CASIOEMU_PERF_NOCLIP") != nullptr;
	if (ToolUi::TableScope table{tableId, 3, ToolUi::TableFlags() | ImGuiTableFlags_ScrollY, height}) {
		ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 60.0f);
		ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 84.0f);
		ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();

		auto draw_row = [&](int visible_index) {
			const Row& row = rows[static_cast<size_t>(visible[static_cast<size_t>(visible_index)])];
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushID(visible_index);
			if (ImGui::SmallButton("Copy"))
				ImGui::SetClipboardText(row.addr_text.c_str());
			ImGui::PopID();
			ImGui::TableNextColumn();
			UIHelpers::ClickableAddress(row.addr);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(row.desc.c_str());
			if (ImGui::IsItemHovered() && !row.desc.empty())
				ImGui::SetTooltip("%s", row.desc.c_str());
		};

		if (disable_clipper) {
			for (int index = 0; index < static_cast<int>(visible.size()) && index < 5000; ++index)
				draw_row(index);
			return height;
		}
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(visible.size()));
		while (clipper.Step()) {
			for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
				draw_row(index);
		}
	}
	return height;
}

void LabelViewer::RenderCore() {
	ToolUi::PageScope page("Labels");
	ToolUi::Section("Labels");
	ToolUi::Description("Memory labels and hardware registers for the active model. Click an address to jump there.");

	ToolUi::Field("Search");
	ImGui::SetNextItemWidth(std::min(320.0f, ToolUi::RemainingWidth()));
	ImGui::InputTextWithHint("##labelsearch", "Address or description", m_SearchBuf, sizeof(m_SearchBuf));

	const std::size_t region_count = me_mmu ? me_mmu->GetRegions().size() : 0;
	if (!m_cacheValid || m_cachedHardware != m_emu->hardware_id ||
		(region_count != m_regions.size() && m_perfRows == 0))
		RebuildCache();
	if (!m_filterValid || std::strcmp(m_cachedFilter, m_SearchBuf) != 0)
		RebuildFilter();

	const float list_height = std::max(140.0f, ImGui::GetContentRegionAvail().y * 0.40f);

	ToolUi::Section("Label.GeneralHeader"_lc);
	int shown = 0;
	RenderRows("##labels_table", m_labels, m_visibleLabels, list_height, shown);
	if (shown == 0)
		ToolUi::Hint("No memory labels match the search.");

	ToolUi::Section("Label.SfrsHeader"_lc);
	RenderRows("##regs_table", m_regions, m_visibleRegions, list_height, shown);
	if (shown == 0)
		ToolUi::Hint("No hardware registers match the search.");
}
