#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace casioemu::screen_scan {
	inline int UpdateScanAlpha(
		float* screen_scan_alpha,
		std::array<float, 64>& screen_scan_curve,
		float& screen_scan_curve_coeff,
		bool& screen_scan_curve_valid,
		uint64_t t,
		int screen_refresh_rate,
		int flashing_threshold, float brightness_coeff) {
		int n = (static_cast<uint64_t>((t * screen_refresh_rate) / 250)) % 64;

		if (screen_refresh_rate < flashing_threshold) {
			for (size_t i = 0; i < 64; i++) {
				screen_scan_alpha[i] = 1.0f;
			}
			return n;
		}

		if (!screen_scan_curve_valid || screen_scan_curve_coeff != brightness_coeff) {
			// 计算归一化所需的归一化因子
			float normalization_factor = 0.0f;
			std::array<float, 64> exp_values{};

			for (size_t i = 0; i < 64; i++) {
				exp_values[i] = std::exp(-brightness_coeff * i / 64.0f);
				normalization_factor += exp_values[i];
			}

			// 归一化
			for (size_t i = 0; i < 64; i++) {
				screen_scan_curve[i] = std::pow(exp_values[i] / normalization_factor * 80., 0.2);
			}
			screen_scan_curve_coeff = brightness_coeff;
			screen_scan_curve_valid = true;
		}

		// A real LCD multiplexes its rows once per refresh period; the eye (and a
		// 60 Hz display) integrates that wave over the frame. Publishing the
		// instantaneous wave with a phase that advances at the scan-report rate
		// aliases into a bright band that visibly crawls down the panel ("moving
		// lines"/scanline tearing). Present the temporal average instead: the
		// overall contrast is preserved while every row of a published frame has
		// the same factor. The phase `n` is still returned because screen_report
		// (F03B) and the scan state machine must stay cycle-accurate.
		float average = 0.0f;
		for (size_t i = 0; i < 64; i++)
			average += screen_scan_curve[i];
		average /= 64.0f;
		for (size_t i = 0; i < 64; i++)
			screen_scan_alpha[i] = average;

		return n;
	}

}
