#!/usr/bin/env python3
"""Generate native 12px/8px bitmap fonts for the terminal UI.

The firmware keeps the original 16px Unifont table for primary text. This
script adds compact native 12px UI text and 8px indicator/symbol glyphs so the
device does not need to scale the 16px bitmap at runtime.
"""

from __future__ import annotations

import argparse
import gzip
import json
import os
import subprocess
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
FONT12_PATH = ROOT / "字体" / "江城斜黑体 200W.ttf"
UNIFONT_PATH = ROOT / "喵哎 ESP32 S3 墨水屏" / "tools" / "unifont.hex.gz"
OUT_FILE = ROOT / "firmware" / "main" / "ui_font_sizes_data.h"
FONT_THRESHOLD_12 = 72
FONT_THRESHOLD_8 = 72
try:
    from fontTools.ttLib import TTFont
except ImportError:
    TTFont = None

ASCII_CPS = range(32, 127)
CJK_START = 0x4E00
CJK_END = 0x9FFF
SYMBOL_CPS = sorted({
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00D7, 0x00D8, 0x00F7,
    0x03A3, 0x03A6, 0x03A9, 0x0394, 0x03B1, 0x03B2, 0x03B3,
    0x03B8, 0x03BB, 0x03BC, 0x03C0, 0x03C1, 0x03C3, 0x03C4,
    0x03C6, 0x207B, 0x2103, 0x2109, 0x2127, 0x2202, 0x2206,
    0x2211, 0x2212, 0x221A, 0x221D, 0x221E, 0x2220, 0x222B,
    0x2234, 0x223F, 0x2248, 0x2260, 0x2264, 0x2265, 0x2300,
    0x2393, 0x23DA, 0x23E6, 0x26A1,
})

MANUAL_12 = {
    0x207B: [
        "000000000000",
        "000000000000",
        "001111000000",
        "001111000000",
        "000000000000",
        "000000000000",
        "000000000000",
        "000000000000",
        "000000000000",
        "000000000000",
        "000000000000",
        "000000000000",
    ],
    0x223F: [
        "000000000000",
        "000000000000",
        "001110001100",
        "010001010010",
        "100001100010",
        "100001000100",
        "010010001000",
        "001100001110",
        "000000000000",
        "000000000000",
        "000000000000",
        "000000000000",
    ],
    0x2264: [
        "000000000000",
        "000000000000",
        "000000110000",
        "000011000000",
        "001100000000",
        "110000000000",
        "001100000000",
        "000011000000",
        "000000110000",
        "000000000000",
        "111111110000",
        "000000000000",
    ],
    0x2265: [
        "000000000000",
        "000000000000",
        "110000000000",
        "001100000000",
        "000011000000",
        "000000110000",
        "000011000000",
        "001100000000",
        "110000000000",
        "000000000000",
        "111111110000",
        "000000000000",
    ],
    0x2300: [
        "000000100000",
        "000001000000",
        "000111100000",
        "001001010000",
        "010010010000",
        "010100010000",
        "011000010000",
        "010000100000",
        "001001000000",
        "000111100000",
        "001000000000",
        "010000000000",
    ],
    0x2393: [
        "000000000000",
        "111111110000",
        "000000000000",
        "001001000000",
        "001001000000",
        "000000000000",
        "000000000000",
        "111111110000",
        "000000000000",
        "001001000000",
        "001001000000",
        "000000000000",
    ],
    0x23E6: [
        "111111110000",
        "000000000000",
        "001110001100",
        "010001010010",
        "100001100010",
        "100001000100",
        "010010001000",
        "001100001110",
        "000000000000",
        "111111110000",
        "000000000000",
        "000000000000",
    ],
    0x26A1: [
        "001100000000",
        "001100000000",
        "011000000000",
        "011111110000",
        "000011100000",
        "000111000000",
        "001111111000",
        "000110000000",
        "001100000000",
        "011000000000",
        "010000000000",
        "000000000000",
    ],
}

MANUAL_8 = {
    0x207B: [
        "00000000",
        "00111000",
        "00111000",
        "00000000",
        "00000000",
        "00000000",
        "00000000",
        "00000000",
    ],
    0x223F: [
        "00000000",
        "01100110",
        "10011001",
        "10010010",
        "01100110",
        "00000000",
        "00000000",
        "00000000",
    ],
    0x2264: [
        "00000000",
        "00001100",
        "00110000",
        "11000000",
        "00110000",
        "00001100",
        "11111100",
        "00000000",
    ],
    0x2265: [
        "00000000",
        "11000000",
        "00110000",
        "00001100",
        "00110000",
        "11000000",
        "11111100",
        "00000000",
    ],
    0x2300: [
        "00010000",
        "00100000",
        "01110000",
        "10101000",
        "11001000",
        "10010000",
        "01110000",
        "10000000",
    ],
    0x2393: [
        "11111100",
        "00000000",
        "00101000",
        "00000000",
        "11111100",
        "00000000",
        "00101000",
        "00000000",
    ],
    0x23E6: [
        "11111100",
        "00000000",
        "01100110",
        "10011001",
        "01100110",
        "00000000",
        "11111100",
        "00000000",
    ],
    0x26A1: [
        "00110000",
        "01100000",
        "01111100",
        "00011000",
        "00111110",
        "00011000",
        "00110000",
        "00000000",
    ],
}


def load_font(size: int) -> ImageFont.FreeTypeFont:
    if not FONT12_PATH.exists():
        raise FileNotFoundError(FONT12_PATH)
    return ImageFont.truetype(str(FONT12_PATH), size=size)


def load_font_coverage(path: Path) -> set[int]:
    if TTFont is None:
        return load_font_coverage_subprocess(path)
    try:
        font = TTFont(str(path), fontNumber=0)
        cps: set[int] = set()
        for table in font["cmap"].tables:
            cps.update(table.cmap.keys())
        return cps
    except Exception:
        return load_font_coverage_subprocess(path)


def load_font_coverage_subprocess(path: Path) -> set[int]:
    script = (
        "import json, sys\n"
        "from fontTools.ttLib import TTFont\n"
        "font = TTFont(sys.argv[1], fontNumber=0)\n"
        "cps = set()\n"
        "for table in font['cmap'].tables:\n"
        "    cps.update(table.cmap.keys())\n"
        "print(json.dumps(sorted(cps)))\n"
    )
    env = os.environ.copy()
    env.pop("PYTHONPATH", None)
    proc = subprocess.run(
        ["python3", "-c", script, str(path)],
        check=True,
        capture_output=True,
        text=True,
        env=env,
    )
    return set(json.loads(proc.stdout))


def load_unifont_glyphs(cps: set[int]) -> dict[int, list[int]]:
    glyphs: dict[int, list[int]] = {}
    if not UNIFONT_PATH.exists():
        return glyphs
    with gzip.open(UNIFONT_PATH, "rt", encoding="ascii") as f:
        for line in f:
            line = line.strip()
            if not line or ":" not in line:
                continue
            cp_s, hex_s = line.split(":", 1)
            cp = int(cp_s, 16)
            if cp in cps:
                glyphs[cp] = list(bytes.fromhex(hex_s))
                if len(glyphs) == len(cps):
                    break
    return glyphs


def render_glyph(font: ImageFont.FreeTypeFont, ch: str, width: int, height: int,
                 threshold: int = 72) -> list[str]:
    img = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(img)
    bbox = draw.textbbox((0, 0), ch, font=font)
    gw = bbox[2] - bbox[0]
    gh = bbox[3] - bbox[1]
    x = (width - gw) // 2 - bbox[0]
    y = (height - gh) // 2 - bbox[1]
    draw.text((x, y), ch, font=font, fill=255)
    rows: list[str] = []
    for yy in range(height):
        bits = []
        for xx in range(width):
            bits.append("1" if img.getpixel((xx, yy)) >= threshold else "0")
        rows.append("".join(bits))
    return rows


def unifont_to_12(raw: list[int]) -> list[str]:
    if len(raw) == 16:
        src = [(b << 8) for b in raw]
    elif len(raw) == 32:
        src = [((raw[i * 2] << 8) | raw[i * 2 + 1]) for i in range(16)]
    else:
        return ["0" * 12 for _ in range(12)]
    rows: list[str] = []
    for row in range(12):
        r0 = (row * 16) // 12
        r1 = min(16, ((row + 1) * 16 + 11) // 12)
        bits = []
        for col in range(12):
            c0 = (col * 16) // 12
            c1 = min(16, ((col + 1) * 16 + 11) // 12)
            count = 0
            area = 0
            for rr in range(r0, r1):
                for cc in range(c0, c1):
                    area += 1
                    if src[rr] & (0x8000 >> cc):
                        count += 1
            bits.append("1" if count >= (2 if area > 2 else 1) else "0")
        rows.append("".join(bits))
    return rows


def unifont_to_8(raw: list[int]) -> list[str]:
    if len(raw) == 16:
        src_w = 8
        src = raw
    elif len(raw) == 32:
        src_w = 16
        src = [((raw[i * 2] << 8) | raw[i * 2 + 1]) for i in range(16)]
    else:
        return ["0" * 8 for _ in range(8)]
    rows: list[str] = []
    for row in range(8):
        r0 = (row * 16) // 8
        r1 = min(16, ((row + 1) * 16 + 7) // 8)
        bits = []
        for col in range(8):
            c0 = (col * src_w) // 8
            c1 = min(src_w, ((col + 1) * src_w + 7) // 8)
            count = 0
            area = 0
            for rr in range(r0, r1):
                for cc in range(c0, c1):
                    area += 1
                    if len(raw) == 16:
                        on = (src[rr] & (0x80 >> cc)) != 0
                    else:
                        on = (src[rr] & (0x8000 >> cc)) != 0
                    if on:
                        count += 1
            bits.append("1" if count >= (2 if area > 2 else 1) else "0")
        rows.append("".join(bits))
    return rows


def pack_rows(rows: list[str], width: int) -> list[int]:
    out: list[int] = []
    bytes_per_row = 1 if width <= 8 else 2
    for row in rows:
        value = 0
        for col, bit in enumerate(row[:width]):
            if bit == "1":
                value |= 1 << ((bytes_per_row * 8 - 1) - col)
        if bytes_per_row == 1:
            out.append(value & 0xFF)
        else:
            out.append((value >> 8) & 0xFF)
            out.append(value & 0xFF)
    return out


def c_array(data: list[int]) -> str:
    return ", ".join(f"0x{b:02X}" for b in data)


def main() -> None:
    global FONT12_PATH, UNIFONT_PATH, OUT_FILE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", type=Path, default=FONT12_PATH,
                        help="OFL-licensed JiangChengXieHei-compatible TTF source")
    parser.add_argument("--unifont", type=Path, default=UNIFONT_PATH,
                        help="GNU Unifont .hex or .hex.gz fallback source")
    parser.add_argument("--output", type=Path, default=OUT_FILE,
                        help="generated C header path")
    args = parser.parse_args()
    FONT12_PATH = args.font.expanduser().resolve()
    UNIFONT_PATH = args.unifont.expanduser().resolve()
    OUT_FILE = args.output.expanduser().resolve()
    if not FONT12_PATH.is_file() or not UNIFONT_PATH.is_file():
        parser.error("--font and --unifont must point to existing OFL-licensed source files")

    font12 = load_font(12)
    font8 = load_font(8)
    font_coverage = load_font_coverage(FONT12_PATH)
    fallback_cps = {
        cp for cp in range(CJK_START, CJK_END + 1)
        if font_coverage and cp not in font_coverage
    }
    fallback_cps |= {
        cp for cp in SYMBOL_CPS
        if cp not in MANUAL_12 and font_coverage and cp not in font_coverage
    }
    unifont = load_unifont_glyphs(fallback_cps)

    with OUT_FILE.open("w", encoding="utf-8", newline="\n") as f:
        f.write("/* Auto-generated by tools/gen_ui_font_sizes.py. Do not edit. */\n")
        f.write(f"/* 12px source: {FONT12_PATH.name}; missing glyph fallback: GNU Unifont. */\n")
        f.write("/* Font data is distributed under SIL Open Font License 1.1; see docs/licenses/OFL-1.1.txt. */\n")
        f.write("#pragma once\n")
        f.write("#include <stdint.h>\n\n")
        f.write("#define UI_FONT12_ASCII_W 8\n")
        f.write("#define UI_FONT12_H 12\n")
        f.write("#define UI_FONT12_ASCII_ADV 8\n")
        f.write("#define UI_FONT12_CJK_ADV 13\n")
        f.write("#define UI_FONT8_ASCII_W 6\n")
        f.write("#define UI_FONT8_H 8\n")
        f.write("#define UI_FONT8_ASCII_ADV 6\n")
        f.write("#define UI_FONT8_SYMBOL_ADV 9\n\n")

        f.write("static const uint8_t ui_font12_ascii[95][12] = {\n")
        for cp in ASCII_CPS:
            rows = render_glyph(font12, chr(cp), 8, 12, threshold=FONT_THRESHOLD_12)
            f.write(f"    {{ {c_array(pack_rows(rows, 8))} }}, /* 0x{cp:02X} */\n")
        f.write("};\n\n")

        f.write("static const uint8_t ui_font8_ascii[95][8] = {\n")
        for cp in ASCII_CPS:
            rows = render_glyph(font8, chr(cp), 6, 8, threshold=FONT_THRESHOLD_8)
            f.write(f"    {{ {c_array(pack_rows(rows, 6))} }}, /* 0x{cp:02X} */\n")
        f.write("};\n\n")

        f.write("static const uint8_t ui_font12_cjk[20992][24] = {\n")
        for cp in range(CJK_START, CJK_END + 1):
            if not font_coverage or cp in font_coverage:
                rows = render_glyph(font12, chr(cp), 12, 12, threshold=FONT_THRESHOLD_12)
                source = "ttf"
            else:
                rows = unifont_to_12(unifont.get(cp, []))
                source = "unifont"
            f.write(f"    {{ {c_array(pack_rows(rows, 12))} }}, /* U+{cp:04X} {source} */\n")
        f.write("};\n\n")
        f.write("#define UI_FONT12_CJK_START 0x4E00\n")
        f.write("#define UI_FONT12_CJK_END 0x9FFF\n")
        f.write("#define UI_FONT12_CJK_COUNT 20992\n\n")

        f.write("typedef struct { uint32_t cp; uint8_t bmp[24]; } ui_font12_symbol_glyph_t;\n")
        f.write(f"static const ui_font12_symbol_glyph_t ui_font12_symbols[{len(SYMBOL_CPS)}] = {{\n")
        for cp in SYMBOL_CPS:
            rows = MANUAL_12.get(cp)
            if rows is None:
                if not font_coverage or cp in font_coverage:
                    rows = render_glyph(font12, chr(cp), 12, 12, threshold=FONT_THRESHOLD_12)
                else:
                    rows = unifont_to_12(unifont.get(cp, []))
            f.write(f"    {{ 0x{cp:04X}, {{ {c_array(pack_rows(rows, 12))} }} }},\n")
        f.write("};\n")
        f.write(f"#define UI_FONT12_SYMBOL_COUNT {len(SYMBOL_CPS)}\n\n")

        f.write("typedef struct { uint32_t cp; uint8_t bmp[8]; } ui_font8_symbol_glyph_t;\n")
        f.write(f"static const ui_font8_symbol_glyph_t ui_font8_symbols[{len(SYMBOL_CPS)}] = {{\n")
        for cp in SYMBOL_CPS:
            rows = MANUAL_8.get(cp)
            if rows is None:
                if not font_coverage or cp in font_coverage:
                    rows = render_glyph(font8, chr(cp), 8, 8, threshold=FONT_THRESHOLD_8)
                else:
                    rows = unifont_to_8(unifont.get(cp, []))
            f.write(f"    {{ 0x{cp:04X}, {{ {c_array(pack_rows(rows, 8))} }} }},\n")
        f.write("};\n")
        f.write(f"#define UI_FONT8_SYMBOL_COUNT {len(SYMBOL_CPS)}\n")

    print(f"Generated {OUT_FILE}")
    print("  12px ASCII: 95 glyphs")
    print("  12px CJK:   20992 glyphs")
    if font_coverage:
        print(f"  {FONT12_PATH.name} CJK: {20992 - len(fallback_cps & set(range(CJK_START, CJK_END + 1)))}/20992")
        print(f"  Unifont fallback glyphs: {len(fallback_cps)}")
    print(f"  symbols:    {len(SYMBOL_CPS)} glyphs at 12px and 8px")


if __name__ == "__main__":
    main()
