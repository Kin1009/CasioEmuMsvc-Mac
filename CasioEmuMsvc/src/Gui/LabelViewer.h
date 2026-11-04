#pragma once
#include "Ui.hpp"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

class LabelViewer : public UIWindow {
private:
	struct Row {
		uint32_t addr = 0;
		std::string desc;
		std::string addr_text;
		std::string search; // lowercase "description address" for filtering
	};

	char m_SearchBuf[64] = {};
	std::vector<Row> m_labels;
	std::vector<Row> m_regions;
	std::vector<int> m_visibleLabels;
	std::vector<int> m_visibleRegions;
	unsigned m_cachedHardware = 0xFFFFFFFFu;
	std::size_t m_cachedRegionSignature = 0;
	char m_cachedFilter[64] = {};
	bool m_cacheValid = false;
	bool m_filterValid = false;
	int m_perfRows = 0;

	void RebuildCache();
	void RebuildFilter();
	float RenderRows(const char* tableId, const std::vector<Row>& rows,
		const std::vector<int>& visible, float height, int& shown);

public:
	LabelViewer() : UIWindow("Labels") {}
	void RenderCore() override;
};
