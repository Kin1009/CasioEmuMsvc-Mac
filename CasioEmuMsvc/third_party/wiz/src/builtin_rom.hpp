// builtin_rom.hpp - the fx-580VN X firmware image embedded in wizc.
//
// `wizc` patches a compiled #cwapp program into this ROM automatically; users do
// not need to keep a separate firmware dump next to their sources.
#pragma once

#include <cstddef>

namespace wiz {

extern const unsigned char kFx580VnRom[];
extern const std::size_t kFx580VnRomSize;

}  // namespace wiz
