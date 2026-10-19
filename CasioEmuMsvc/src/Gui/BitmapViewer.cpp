#include "Localization.h"
#include "ThemeManager.h"
#include "Ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// A bit-addressable memory viewer.  The grid always has more than one row: the
// number of rows is derived from the available height (or fixed with the Rows
// control).  Auto size shrinks the pixel blocks until the requested number of
// rows and the row width both fit, so resizing the dock never leaves a single
// clipped strip behind.
class BitmapViewer : public UIWindow {
public:
	BitmapViewer() : UIWindow("Bitmap") {}

	char bufaddr[16] = {};
	int width = 16;       // bits per row
	int size = 10;        // pixels per bit
	int bitOffset = 0;    // first bit inside the starting byte
	int rows = 0;         // 0 = fill the panel
	bool autoSize = true;
	double wheeldelta = 0;

	void RenderCore() override {
		// Labels come first, chooser second, matching the rest of the debugger.
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Address");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(140.0f);
		ImGui::InputText("##bitmap_address", bufaddr, sizeof(bufaddr),
						 ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);

		uint32_t addr = static_cast<uint32_t>(std::strtoul(bufaddr, nullptr, 16)) & 0xfffff;
		ImGui::SameLine();
		ImGui::SetNextItemWidth(120.0f);
		int address_edit = static_cast<int>(addr);
		if (ImGui::InputInt("##bitmap_address_int", &address_edit, 1, 16,
							ImGuiInputTextFlags_CharsHexadecimal)) {
			addr = static_cast<uint32_t>(address_edit) & 0xfffff;
			std::snprintf(bufaddr, sizeof(bufaddr), "%06X", addr);
		}

		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Bits / row");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(140.0f);
		if (ImGui::SliderInt("##bitmap_width", &width, 1, 256))
			width = std::max(1, width);

		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Rows (0 = auto)");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(140.0f);
		ImGui::InputInt("##bitmap_rows", &rows, 1, 4);
		if (rows < 0)
			rows = 0;

		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Pixel size");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(140.0f);
		if (autoSize)
			ImGui::BeginDisabled();
		ImGui::SliderInt("##bitmap_size", &size, 1, 64);
		if (autoSize)
			ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::Checkbox("Auto size", &autoSize);
		if (size < 1)
			size = 1;

		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Bit offset");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(140.0f);
		ImGui::SliderInt("##bitmap_bitoffset", &bitOffset, 0, 7);
		ImGui::Separator();

		const ImVec2 avail = ImGui::GetContentRegionAvail();
		const float scrollWidth = 16.0f;
		const float gridAreaWidth = std::max(32.0f, avail.x - scrollWidth - 10.0f);
		const float gridAreaHeight = std::max(32.0f, avail.y);

		int visible_rows = rows;
		if (visible_rows == 0) {
			visible_rows = std::max(1, static_cast<int>(gridAreaHeight) / std::max(1, size));
			// Keep the matrix a matrix even when the panel is short.
			visible_rows = std::max(visible_rows, 8);
		}

		if (autoSize) {
			const int fit_w = std::max(1, static_cast<int>(gridAreaWidth) / std::max(1, width));
			const int fit_h = std::max(1, static_cast<int>(gridAreaHeight) / std::max(1, visible_rows));
			size = std::clamp(std::min(fit_w, fit_h), 1, 64);
			if (rows == 0)
				visible_rows = std::max(8, static_cast<int>(gridAreaHeight) / size);
		}

		// If a fixed row count is larger than the panel, the virtual scrollbar
		// still keeps every row reachable; clamp only the drawn size here.
		const int numPixels = width * visible_rows;
		const ImVec2 gridSize(static_cast<float>(width * size),
							  static_cast<float>(visible_rows * size));

		ImGui::Dummy(gridSize);
		const ImVec2 startPos = ImGui::GetItemRectMin();
		const bool gridHovered = ImGui::IsItemHovered();
		ImDrawList* drawList = ImGui::GetWindowDrawList();

		const ImU32 pixel_on = ImGui::GetColorU32(ImGuiCol_Text);
		const ImU32 grid_line = ImGui::GetColorU32(ImGuiCol_Border);
		const ImU32 tick = ImGui::GetColorU32(ImGuiCol_TextDisabled);
		const ImU32 track = ImGui::GetColorU32(ImGuiCol_FrameBg);
		const ImU32 handle = ImGui::GetColorU32(ImGuiCol_SliderGrab);

		// Grid background, so light and dark themes both show the matrix.
		drawList->AddRectFilled(startPos, ImVec2(startPos.x + gridSize.x, startPos.y + gridSize.y),
								ImGui::GetColorU32(ImGuiCol_ChildBg));

		uint8_t currentByte = 0;
		int currentByteIndex = -1;
		for (int i = 0; i < numPixels; ++i) {
			const int effectiveBitPos = bitOffset + i;
			const int byteIndex = effectiveBitPos / 8;
			const int bitIndex = 7 - (effectiveBitPos % 8);

			const int col = i % width;
			const int row = i / width;
			const ImVec2 pixelTopLeft(startPos.x + col * size, startPos.y + row * size);
			const ImVec2 pixelBottomRight(pixelTopLeft.x + size, pixelTopLeft.y + size);

			if (byteIndex != currentByteIndex) {
				currentByte = me_mmu->ReadData((addr + byteIndex) & 0xfffff);
				currentByteIndex = byteIndex;
			}
			if (((currentByte >> bitIndex) & 1) != 0)
				drawList->AddRectFilled(pixelTopLeft, pixelBottomRight, pixel_on);
		}

		const ImVec2 endPos(startPos.x + gridSize.x, startPos.y + gridSize.y);
		if (size > 3) {
			drawList->AddRect(startPos, endPos, grid_line);

			for (int col = 0; col < width; ++col) {
				const float x = startPos.x + col * size;
				if (col % 8 == 0) {
					drawList->AddLine(ImVec2(x, startPos.y), ImVec2(x, startPos.y - 5.0f), tick);
					char label[16];
					std::snprintf(label, sizeof(label), "%X", col);
					drawList->AddText(ImVec2(x + 1, startPos.y - 17.0f), tick, label);
				}
			}
			for (int row = 0; row < visible_rows; ++row) {
				const float y = startPos.y + row * size;
				if (row % 8 == 0) {
					drawList->AddLine(ImVec2(endPos.x, y), ImVec2(endPos.x + 5.0f, y), tick);
					char label[16];
					const uint32_t row_addr = (addr + (bitOffset + row * width) / 8) & 0xfffff;
					std::snprintf(label, sizeof(label), "%06X", row_addr);
					drawList->AddText(ImVec2(endPos.x + 7.0f, y - 6.0f), tick, label);
				}
			}
		}

		// Virtual scrollbar: drag through the 1 MiB address space.
		{
			constexpr uint32_t kMemorySize = 0x100000;
			const int visibleBits = width * visible_rows;
			const int visibleBytes = (bitOffset + visibleBits + 7) / 8;
			const uint32_t maxEffectiveAddr =
				kMemorySize > static_cast<uint32_t>(visibleBytes) ? kMemorySize - visibleBytes : 0;

			const float effectiveAddr = static_cast<float>(addr) + bitOffset / 8.0f;
			const ImVec2 scrollPos(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - scrollWidth,
								   startPos.y);
			const ImVec2 scrollSize(scrollWidth - 1.0f, std::max(10.0f, gridSize.y));

			drawList->AddRectFilled(scrollPos,
									ImVec2(scrollPos.x + scrollWidth, scrollPos.y + scrollSize.y),
									track);
			float handleHeight = (visibleBytes / static_cast<float>(kMemorySize)) * scrollSize.y;
			handleHeight = std::max(10.0f, std::min(handleHeight, scrollSize.y));
			float handlePosY = 0.0f;
			if (maxEffectiveAddr > 0)
				handlePosY = (effectiveAddr / maxEffectiveAddr) * (scrollSize.y - handleHeight);
			drawList->AddRectFilled(ImVec2(scrollPos.x, scrollPos.y + handlePosY),
									ImVec2(scrollPos.x + scrollWidth, scrollPos.y + handlePosY + handleHeight),
									handle);

			ImGui::SetCursorScreenPos(scrollPos);
			ImGui::InvisibleButton("##bitmap_scroll", scrollSize);
			if (ImGui::IsItemActive()) {
				const float mouseY = ImGui::GetIO().MousePos.y;
				float newHandlePosY = mouseY - scrollPos.y - handleHeight / 2.0f;
				newHandlePosY = std::clamp(newHandlePosY, 0.0f, std::max(0.0f, scrollSize.y - handleHeight));
				const float newEffectiveAddr = (scrollSize.y - handleHeight > 0.0f)
					? (newHandlePosY / (scrollSize.y - handleHeight)) * maxEffectiveAddr
					: 0.0f;
				int newAddr = static_cast<int>(newEffectiveAddr);
				int newBitOffset = static_cast<int>(std::round((newEffectiveAddr - newAddr) * 8.0f));
				if (newBitOffset >= 8) {
					++newAddr;
					newBitOffset -= 8;
				}
				addr = static_cast<uint32_t>(newAddr) & 0xfffff;
				bitOffset = newBitOffset;
				std::snprintf(bufaddr, sizeof(bufaddr), "%06X", addr);
			}
		}

		// Mouse wheel scrolls whole rows.
		if (gridHovered) {
			wheeldelta += ImGui::GetIO().MouseWheel;
			if (std::abs(wheeldelta) >= 1.0) {
				const int rowDelta = static_cast<int>(std::trunc(wheeldelta));
				wheeldelta = std::fmod(wheeldelta, 1.0);
				ScrollBits(width * rowDelta, addr, bitOffset);
				std::snprintf(bufaddr, sizeof(bufaddr), "%06X", addr);
			}
		}

		// Arrow keys and page keys scroll too.
		if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput) {
			int rowDelta = 0;
			if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
				rowDelta = -1;
			else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
				rowDelta = 1;
			else if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
				rowDelta = -visible_rows;
			else if (ImGui::IsKeyPressed(ImGuiKey_PageDown))
				rowDelta = visible_rows;
			if (rowDelta != 0) {
				ScrollBits(width * rowDelta, addr, bitOffset);
				std::snprintf(bufaddr, sizeof(bufaddr), "%06X", addr);
			}
		}
	}

private:
	static void ScrollBits(int bitDelta, uint32_t& addr, int& bitOffset) {
		int newAddr = static_cast<int>(addr) + bitDelta / 8;
		int newBitOffset = bitOffset + bitDelta % 8;
		if (newBitOffset > 7) {
			newAddr += newBitOffset / 8;
			newBitOffset %= 8;
		} else if (newBitOffset < 0) {
			const int borrow = (-newBitOffset + 7) / 8;
			newAddr = newAddr > borrow ? newAddr - borrow : 0;
			newBitOffset += borrow * 8;
		}
		newAddr = newAddr & 0xfffff;
		addr = static_cast<uint32_t>(newAddr);
		bitOffset = newBitOffset;
	}
};

UIWindow* CreateBitmapViewer() {
	return new BitmapViewer();
}
