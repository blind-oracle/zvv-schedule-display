#!/usr/bin/env python3
"""Convert the VBZ Adafruit-GFX font (vbzfont.h) into a BDF font usable by ESPHome.

Upstream font: https://github.com/sschueller/vbz-fahrgastinformation/blob/main/lib/Display/src/vbzfont.h
Copyright (c) 2022 Stefan Schüller, MIT License.

The upstream font has a few quirks that this script normalises:

* Every bitmap row is stored as 2 bytes (16 bit wide) regardless of the glyph width.
* yOffset is measured from the *top* of the text line (the original firmware sets the
  cursor to the row top), not from the baseline as in regular GFX fonts.
* Umlauts live at '{', '|', '}' and two icons at 0x1E / 0x1F.

Output metrics: FONT_ASCENT 10, FONT_DESCENT 2 (line height 12). ESPHome positions a glyph
at y + (ascent - bitmap_top) = y + yOffset, so drawing with TextAlign::TOP_LEFT at the row top
reproduces the original layout pixel for pixel.

Usage: gfx2bdf.py <vbzfont.h> <out.bdf> [--preview]
"""

import re
import sys

ASCENT = 10
DESCENT = 2

# Upstream code point -> Unicode code point of the output glyph.
REMAP = {
    0x7B: 0x00E4,  # ä
    0x7C: 0x00F6,  # ö
    0x7D: 0x00FC,  # ü
    0x1E: 0xE000,  # tram icon ("departing now")
    0x1F: 0xE001,  # wheelchair / low-floor icon
}

# Glyphs that are missing upstream, drawn in the same style.
# Each entry: code point -> (advance, x_offset, y_offset_from_top, rows)
EXTRA = {
    ord("J"): (7, 1, 1, [
        "..####",
        "....##",
        "....##",
        "....##",
        "....##",
        "....##",
        "##..##",
        "##..##",
        ".####.",
    ]),
    ord("Q"): (7, 1, 1, [
        ".####.",
        "##..##",
        "##..##",
        "##..##",
        "##..##",
        "##..##",
        "##.###",
        "##..##",
        ".###.#",
    ]),
    ord("X"): (7, 1, 1, [
        "##..##",
        "##..##",
        ".####.",
        ".####.",
        "..##..",
        ".####.",
        ".####.",
        "##..##",
        "##..##",
    ]),
    ord("j"): (5, 1, 0, [
        "..##",
        "....",
        "..##",
        "..##",
        "..##",
        "..##",
        "..##",
        "..##",
        "..##",
        "..##",
        "###.",
    ]),
    ord("x"): (7, 1, 3, [
        "##..##",
        "##..##",
        ".####.",
        "..##..",
        ".####.",
        "##..##",
        "##..##",
    ]),
    ord("("): (4, 1, 1, [
        ".##",
        "##.",
        "##.",
        "##.",
        "##.",
        "##.",
        "##.",
        "##.",
        ".##",
    ]),
    ord(")"): (4, 1, 1, [
        "##.",
        ".##",
        ".##",
        ".##",
        ".##",
        ".##",
        ".##",
        ".##",
        "##.",
    ]),
    ord("+"): (7, 1, 3, [
        "..##..",
        "..##..",
        "######",
        "######",
        "..##..",
        "..##..",
    ]),
    ord("!"): (3, 1, 1, [
        "##",
        "##",
        "##",
        "##",
        "##",
        "##",
        "..",
        "##",
        "##",
    ]),
    ord("?"): (7, 1, 1, [
        ".####.",
        "##..##",
        "....##",
        "...##.",
        "..##..",
        "..##..",
        "......",
        "..##..",
        "..##..",
    ]),
}


def parse_gfx(path):
    src = open(path, encoding="utf-8").read()
    bm_src = re.search(r"vbzfontBitmap\[\]\s*PROGMEM\s*=\s*\{(.*?)\};", src, re.S).group(1)
    bm_src = re.sub(r"/\*.*?\*/", "", bm_src, flags=re.S)
    bitmap = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", bm_src)]

    gl_src = re.search(r"vbzfontGlyph\[\]\s*PROGMEM\s*=\s*\{(.*?)\};", src, re.S).group(1)
    gl_src = re.sub(r"/\*.*?\*/", "", gl_src, flags=re.S)
    glyphs = [tuple(int(v) for v in m) for m in re.findall(
        r"\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}", gl_src)]

    first = int(re.search(r"vbzfontGlyph,\s*(\d+)\s*,\s*(\d+)", src.replace("(GFXglyph *)", "")).group(1))
    return bitmap, glyphs, first


def gfx_rows(bitmap, offset, height):
    """Return glyph rows as lists of 0/1, 16 columns each (2 bytes per row)."""
    rows = []
    for r in range(height):
        hi, lo = bitmap[offset + 2 * r], bitmap[offset + 2 * r + 1]
        word = (hi << 8) | lo
        rows.append([(word >> (15 - c)) & 1 for c in range(16)])
    return rows


def trim_width(rows):
    width = 0
    for row in rows:
        for c, bit in enumerate(row):
            if bit:
                width = max(width, c + 1)
    return max(width, 1)


def build_glyphs(bitmap, glyphs, first):
    out = {}
    for i, (offset, _bw, height, advance, xo, yo) in enumerate(glyphs):
        code = first + i
        if height == 0 and code != 0x20:
            continue  # "No Bitmap Defined"
        rows = gfx_rows(bitmap, offset, height) if height else []
        width = trim_width(rows) if rows else 0
        rows = [row[:width] for row in rows]
        out[REMAP.get(code, code)] = (advance, xo, yo, width, rows)

    for code, (advance, xo, yo, art) in EXTRA.items():
        if code in out:
            continue
        width = max(len(line) for line in art)
        rows = [[1 if ch == "#" else 0 for ch in line.ljust(width, ".")] for line in art]
        out[code] = (advance, xo, yo, width, rows)
    return out


def write_bdf(glyphs, path):
    lines = [
        "STARTFONT 2.1",
        "COMMENT VBZ departure board font, converted from vbzfont.h",
        "COMMENT https://github.com/sschueller/vbz-fahrgastinformation",
        "COMMENT Copyright (c) 2022 Stefan Schueller - MIT License",
        "COMMENT Extra glyphs (J Q X j x ( ) + ! ?) added by tools/gfx2bdf.py",
        "FONT -vbz-fahrgastinfo-medium-r-normal--12-120-75-75-p-70-iso10646-1",
        "SIZE 12 75 75",
        f"FONTBOUNDINGBOX 16 {ASCENT + DESCENT} 0 {-DESCENT}",
        "STARTPROPERTIES 4",
        f"FONT_ASCENT {ASCENT}",
        f"FONT_DESCENT {DESCENT}",
        "PIXEL_SIZE 12",
        "DEFAULT_CHAR 32",
        "ENDPROPERTIES",
        f"CHARS {len(glyphs)}",
    ]
    for code in sorted(glyphs):
        advance, xo, yo, width, rows = glyphs[code]
        height = len(rows)
        bbx_y = ASCENT - (yo + height) if height else 0
        lines += [
            f"STARTCHAR U+{code:04X}",
            f"ENCODING {code}",
            f"SWIDTH {advance * 1000 // 12} 0",
            f"DWIDTH {advance} 0",
            f"BBX {width} {height} {xo if height else 0} {bbx_y}",
            "BITMAP",
        ]
        nbytes = (width + 7) // 8
        for row in rows:
            value = 0
            for c, bit in enumerate(row):
                if bit:
                    value |= 1 << (nbytes * 8 - 1 - c)
            lines.append(f"{value:0{nbytes * 2}X}")
        lines.append("ENDCHAR")
    lines.append("ENDFONT")
    with open(path, "w", encoding="ascii") as f:
        f.write("\n".join(lines) + "\n")


def preview(glyphs):
    for code in sorted(glyphs):
        advance, xo, yo, width, rows = glyphs[code]
        print(f"U+{code:04X} {chr(code)!r} adv={advance} xo={xo} yo={yo} w={width} h={len(rows)}")
        cell = [[" "] * (advance + 1) for _ in range(ASCENT + DESCENT)]
        for r, row in enumerate(rows):
            for c, bit in enumerate(row):
                y, x = yo + r, xo + c
                if bit and 0 <= y < len(cell) and 0 <= x < len(cell[0]):
                    cell[y][x] = "#"
        for r, line in enumerate(cell):
            marker = "_" if r == ASCENT - 1 else " "
            print(f"  |{''.join(line)}|{marker}")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    bitmap, raw, first = parse_gfx(sys.argv[1])
    glyphs = build_glyphs(bitmap, raw, first)
    write_bdf(glyphs, sys.argv[2])
    if "--preview" in sys.argv:
        preview(glyphs)
    chars = "".join(chr(c) for c in sorted(glyphs))
    print(f"Wrote {len(glyphs)} glyphs to {sys.argv[2]}: {chars!r}", file=sys.stderr)


if __name__ == "__main__":
    main()
