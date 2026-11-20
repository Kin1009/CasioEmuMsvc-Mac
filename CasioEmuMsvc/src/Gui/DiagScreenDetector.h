// DiagScreenDetector.h -- pixel-only recognition of the CG100 diagnostics dialog.
//
// This is a HOST-SIDE UI helper for the "To diagnostics mode" macro button. It
// looks at ordinary RGBA pixels from the plugin framebuffer; there is no OCR,
// no emulator-core knowledge and no hardcoded diagnostic state anywhere in the
// core or the plugin.
//
// Reference frame (396x224, agentZ_diag_02_final.png):
//   * white rounded dialog box, top border at y ~= 33, bottom border at y ~= 174
//   * near-white interior (x 50..340, y 46..165), ~87% of pixels are white
//   * three dark text bands at y 49..67, 73..91 and 121..139
// Normal HOME screens and the other modal dialogs do NOT have a long dark
// horizontal run both near y=33 and near y=174, so the two border runs are the
// primary discriminator. Coordinates are normalised to the 396x224 reference
// so a slightly different framebuffer size still works.
#pragma once

#include <cstddef>
#include <cstdint>

namespace casioemu {
namespace ui {

inline bool LooksLikeDiagDialog(const uint8_t* rgba, int w, int h) {
	if (!rgba || w < 200 || h < 100)
		return false;

	const auto sx = [w](int v) { return static_cast<int>(static_cast<int64_t>(v) * w / 396); };
	const auto sy = [h](int v) { return static_cast<int>(static_cast<int64_t>(v) * h / 224); };

	const auto pixel = [rgba, w](int x, int y) -> const uint8_t* {
		return rgba + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
	};
	const auto dark = [&pixel](int x, int y) {
		const uint8_t* p = pixel(x, y);
		return p[0] < 96 && p[1] < 96 && p[2] < 96;
	};
	const auto white = [&pixel](int x, int y) {
		const uint8_t* p = pixel(x, y);
		return p[0] > 190 && p[1] > 190 && p[2] > 190;
	};

	// Longest consecutive run of dark pixels inside one row, over [xa, xb).
	const auto longest_dark_run = [&dark](int y, int xa, int xb) {
		int best = 0, cur = 0;
		for (int x = xa; x < xb; ++x) {
			if (dark(x, y)) {
				++cur;
				if (cur > best) best = cur;
			} else {
				cur = 0;
			}
		}
		return best;
	};

	const int bx0 = sx(35), bx1 = sx(360);
	const int need = (bx1 - bx0) * 55 / 100; // >= 55% of the box width

	int top_run = 0;
	for (int y = sy(28); y <= sy(44); ++y)
		if (longest_dark_run(y, bx0, bx1) > top_run) top_run = longest_dark_run(y, bx0, bx1);
	if (top_run < need)
		return false;

	int bottom_run = 0;
	for (int y = sy(167); y <= sy(184); ++y)
		if (longest_dark_run(y, bx0, bx1) > bottom_run) bottom_run = longest_dark_run(y, bx0, bx1);
	if (bottom_run < need)
		return false;

	// The inside of the dialog is mostly white paper, including between lines.
	uint32_t white_count = 0, total = 0;
	for (int y = sy(46); y <= sy(165); ++y) {
		for (int x = sx(50); x <= sx(340); ++x) {
			++total;
			if (white(x, y)) ++white_count;
		}
	}
	if (total == 0 || white_count * 100 < static_cast<uint32_t>(total) * 72)
		return false;

	// At least two distinct rows of dark glyphs separated by blank paper.
	// (The genuine screen has three: the two title lines and the key hint.)
	const int tx0 = sx(60), tx1 = sx(336);
	const int y0 = sy(45), y1 = sy(150);
	const int min_dark = sx(18);
	const int min_height = sy(6);
	int bands = 0, band_start = -1;
	for (int y = y0; y <= y1; ++y) {
		int n = 0;
		for (int x = tx0; x < tx1; ++x)
			if (dark(x, y)) ++n;
		if (n >= min_dark) {
			if (band_start < 0) band_start = y;
		} else if (band_start >= 0) {
			if (y - band_start >= min_height)
				++bands;
			band_start = -1;
		}
	}
	if (band_start >= 0 && y1 - band_start + 1 >= min_height)
		++bands;
	return bands >= 2;
}

} // namespace ui
} // namespace casioemu
