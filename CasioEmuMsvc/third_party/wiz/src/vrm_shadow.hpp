#pragma once
// SB1 = F800-FFFF (display / screen buffer 1, common both models)
// SB2 = DDD3-E5D2 (580) / CA54-E253 (880) — cache / shadow
#define SB1_ADDR 0xF800
#define SB2_580  0xDDD3
#define SB2_880  0xCA54
// render(): copy SB2 -> SB1 (flush shadow to display)
