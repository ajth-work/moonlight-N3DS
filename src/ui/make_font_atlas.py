"""Build the shared runtime glyph atlas used by the UI toolkit.

The page artwork intentionally contains no text. This asset stores glyphs and
metrics only; UI strings are laid out and drawn by src/ui/ui.cpp at runtime.
Run this script on Windows with Pillow and the Segoe UI fonts installed.
"""

from pathlib import Path
import struct

from PIL import ImageFont


ROOT = Path(__file__).resolve().parents[2]
FONT_DIR = Path(r"C:\Windows\Fonts")
OUTPUT = ROOT / "3ds" / "data" / "ui_font.bin"

CHARACTERS = tuple(range(32, 127)) + tuple(range(0x00A0, 0x0100)) + (0x2014,)
FONT_FILES = (
    ("segoeuib.ttf", 10),
    ("segoeuib.ttf", 17),
    ("segoeuib.ttf", 20),
    ("segoeuib.ttf", 15),
    ("segoeuib.ttf", 14),
    ("segoeuib.ttf", 13),
    ("segoeuib.ttf", 12),
)

HEADER = struct.Struct("<4sHHHHIIIII")
GLYPH = struct.Struct("<BHbbBBHI")
KERNED_CHARACTERS = 95


def pack_coverage(mask):
    values = [(value + 8) // 17 for value in mask]
    packed = bytearray((len(values) + 1) // 2)
    for index in range(0, len(values), 2):
        high = values[index]
        low = values[index + 1] if index + 1 < len(values) else 0
        packed[index // 2] = (high << 4) | low
    return bytes(packed)


def build():
    charset_offset = HEADER.size
    glyph_offset = charset_offset + len(CHARACTERS) * 4
    glyph_count = len(FONT_FILES) * len(CHARACTERS)
    kern_offset = glyph_offset + glyph_count * GLYPH.size
    kern_size = len(FONT_FILES) * KERNED_CHARACTERS * KERNED_CHARACTERS
    bitmap_offset = kern_offset + kern_size

    fonts = [ImageFont.truetype(str(FONT_DIR / name), size=size)
             for name, size in FONT_FILES]

    glyph_records = []
    bitmap_data = bytearray()
    for font_id, font in enumerate(fonts):
        for char_index, codepoint in enumerate(CHARACTERS):
            character = chr(codepoint)
            mask, offset = font.getmask2(character, mode="L")
            width, height = mask.size
            x_offset, y_offset = offset
            advance = int(round(font.getlength(character) * 64))
            packed = pack_coverage(mask)
            glyph_records.append((font_id, char_index, x_offset, y_offset,
                                  width, height, advance,
                                  bitmap_offset + len(bitmap_data)))
            bitmap_data.extend(packed)

    kerning = bytearray()
    for font in fonts:
        single_widths = [font.getlength(chr(codepoint))
                         for codepoint in CHARACTERS[:KERNED_CHARACTERS]]
        for left_index, left in enumerate(CHARACTERS[:KERNED_CHARACTERS]):
            left_char = chr(left)
            for right_index, right in enumerate(
                    CHARACTERS[:KERNED_CHARACTERS]):
                right_char = chr(right)
                adjustment = font.getlength(left_char + right_char)
                adjustment -= single_widths[left_index] + single_widths[right_index]
                value = max(-127, min(127, int(round(adjustment * 16))))
                kerning.append(value & 0xFF)

    header = HEADER.pack(
        b"MLGF", 1, len(FONT_FILES), len(CHARACTERS), glyph_count,
        HEADER.size, charset_offset, glyph_offset, kern_offset, bitmap_offset,
    )
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with OUTPUT.open("wb") as output:
        output.write(header)
        output.write(struct.pack(f"<{len(CHARACTERS)}I", *CHARACTERS))
        for record in glyph_records:
            output.write(GLYPH.pack(*record))
        output.write(kerning)
        output.write(bitmap_data)

    print(f"Wrote {OUTPUT} ({OUTPUT.stat().st_size:,} bytes)")


if __name__ == "__main__":
    build()
