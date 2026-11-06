#!/usr/bin/env python3
"""Generate src/Gui/MemoryTextMap.hpp from the public ClassWiz font map.

The calculator stores most non-ASCII glyphs as two bytes: a page byte in
0xF0..0xF4 followed by a glyph code.  Single-byte glyphs live on page 0.
The source of truth is cw2tools_en/public_font_map_dist.json, which lists
the language-specific input maps for the CY and EY models.

Math alphanumerics and subscript letters are normalised to their plain
Unicode equivalents (NFKC), and small-capital variants are folded to the
base letter, so the decoded text can be rendered by ordinary text fonts.
The calculator still uses the styled glyph on screen; this map describes
the character it represents.

Usage:
    python3 tools/gen_memory_text_map.py [font_map.json] [output.hpp]
"""

import json
import pathlib
import sys
import unicodedata

REPO = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_SOURCE = REPO / "cw2tools_en" / "public_font_map_dist.json"
DEFAULT_OUTPUT = REPO / "CasioEmuMsvc" / "src" / "Gui" / "MemoryTextMap.hpp"

SMALL_CAPITALS = {
    "\u1d00": "A",  # ᴀ
    "\u0299": "B",  # ʙ
    "\u1d04": "C",  # ᴄ
    "\ua730": "F",  # ꜰ
    "\u026a": "I",  # ɪ
    "\u1d0a": "J",  # ᴊ
    "\u1d0b": "K",  # ᴋ
    "\u0274": "N",  # ɴ
    "\u1d18": "P",  # ᴘ
    "\u1d1b": "T",  # ᴛ
}


def normalise(text: str) -> str:
    text = unicodedata.normalize("NFKC", text)
    return "".join(SMALL_CAPITALS.get(char, char) for char in text)


def collect(model_maps):
    entries = {}

    def add(page, code, text):
        text = normalise(text)
        if text and len(text) <= 8:
            entries.setdefault((page, code), text)

    for code, text in model_maps["CY"]["UN_00"]["inputMap"].items():
        try:
            byte = int(code, 16)
        except ValueError:
            continue
        if 0x10 <= byte <= 0xEF:
            add(0x00, byte, text)

    for model, key in (
        ("CY", "UN_F0"),
        ("CY", "CN_F1"),
        ("EY", "CN_F2"),
        ("CY", "SR_F3"),
        ("CY", "VN_F4"),
    ):
        for code, text in model_maps[model][key]["inputMap"].items():
            if len(code) != 4:
                continue
            try:
                page = int(code[:2], 16)
                glyph = int(code[2:], 16)
            except ValueError:
                continue
            add(page, glyph, text)
    return sorted(entries.items())


def main():
    source = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SOURCE
    output = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_OUTPUT
    data = json.loads(source.read_text(encoding="utf-8"))
    entries = collect(data)

    lines = [
        "#pragma once",
        "// Generated from cw2tools_en/public_font_map_dist.json",
        "// (CY UN_00/UN_F0/CN_F1/SR_F3/VN_F4 and EY CN_F2).",
        "// Two-byte calculator font map: page 0x00 is a single byte, 0xF0..0xF4",
        "// take the next byte.  Run tools/gen_memory_text_map.py to regenerate.",
        "// Do not edit by hand.",
        "",
        "#include <cstdint>",
        "",
        'struct MemoryGlyph { uint8_t page; uint8_t code; const char* utf8; };',
        "",
        "static const MemoryGlyph MEMORY_GLYPHS[] = {",
    ]
    for (page, code), text in entries:
        escaped = text.replace("\\", "\\\\").replace('"', '\\"')
        lines.append(f'    {{0x{page:02X}, 0x{code:02X}, "{escaped}"}},')
    lines += [
        "};",
        "static const int MEMORY_GLYPH_COUNT = "
        "static_cast<int>(sizeof(MEMORY_GLYPHS) / sizeof(MEMORY_GLYPHS[0]));",
        "",
    ]
    output.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {output} ({len(entries)} entries)")


if __name__ == "__main__":
    main()
