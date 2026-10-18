#!/usr/bin/env python3
"""Generates the profile icons: a white character on a solid square in a
classic Windows desktop colour; only the top-right corner is rounded.

    src/tip/NovaChangJie.ico   倉 on Windows 2000 desktop blue  (倉頡)
    src/tip/NovaQuick.ico      速 on Windows 98 desktop green   (速成)

Each size (16-256 px) is rendered on its own, so small sizes stay crisp:
the character is drawn at 8x and downsampled, then a light sharpen keeps
the strokes from blurring at 16-24 px.

Needs Pillow and Noto Sans TC Bold (SIL Open Font License), e.g.
https://github.com/notofonts/noto-cjk/raw/Sans2.004/Sans/SubsetOTF/TC/NotoSansTC-Bold.otf

    python tools/make_icons.py NotoSansTC-Bold.otf [--preview DIR]
"""

import argparse
import io
import os
import struct

from PIL import Image, ImageDraw, ImageFilter, ImageFont

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
WIN2000_BLUE = (0x3A, 0x6E, 0xA5)  # #3A6EA5
WIN98_GREEN = (0x00, 0x80, 0x80)  # #008080
ICONS = {
    "NovaChangJie.ico": ("倉", WIN2000_BLUE),
    "NovaQuick.ico": ("速", WIN98_GREEN),
}
RADIUS = 0.30  # Top-right corner radius as a share of the size.
CORNERS = (False, True, False, False)  # Rounded: top-left, top-right, bottom-right, bottom-left.
GLYPH = 0.76  # Character size as a share of the icon...
GLYPH_SMALL = 0.84  # ...and at 24 px and below, where every pixel of stroke counts.
SCALE = 8


def render(size, char, color, font_path):
    glyph = GLYPH_SMALL if size <= 24 else GLYPH
    big = size * SCALE
    mask = Image.new("L", (big, big), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, big - 1, big - 1), radius=int(big * RADIUS), fill=255,
                                              corners=CORNERS)
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    img.paste(color + (255,), (0, 0), mask)

    # Character, centred on its ink box.
    font = ImageFont.truetype(font_path, int(big * glyph * 1.12))
    layer = Image.new("L", (big, big), 0)
    d = ImageDraw.Draw(layer)
    box = d.textbbox((0, 0), char, font=font)
    w, h = box[2] - box[0], box[3] - box[1]
    fit = big * glyph / max(w, h)
    if abs(fit - 1) > 0.02:  # Scale the font so the ink box is `glyph` of the size.
        font = ImageFont.truetype(font_path, int(big * glyph * 1.12 * fit))
        box = d.textbbox((0, 0), char, font=font)
        w, h = box[2] - box[0], box[3] - box[1]
    d.text(((big - w) / 2 - box[0], (big - h) / 2 - box[1]), char, fill=255, font=font)
    img.paste((255, 255, 255, 255), (0, 0), layer)

    small = img.resize((size, size), Image.LANCZOS)
    if size <= 24:
        small = small.filter(ImageFilter.UnsharpMask(radius=0.6, percent=60, threshold=0))
    return small


def bmp_entry(img):
    size = img.width
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    data = bytearray(header)
    px = img.load()
    for y in reversed(range(size)):  # Bottom-up.
        for x in range(size):
            r, g, b, a = px[x, y]
            data += bytes((b, g, r, a))
    data += bytes(((size + 31) // 32) * 4 * size)  # AND mask; alpha is authoritative.
    return bytes(data)


def png_entry(img):
    out = io.BytesIO()
    img.save(out, "PNG")
    return out.getvalue()


def write_ico(path, images):
    # Small sizes as 32-bit BMP (every API reads them), 256 px as PNG.
    entries = [png_entry(im) if im.width >= 256 else bmp_entry(im) for im in images]
    blob = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for im, data in zip(images, entries):
        s = im.width % 256
        blob += struct.pack("<BBBBHHII", s, s, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for data in entries:
        blob += data
    with open(path, "wb") as f:
        f.write(blob)


def main():
    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("font", help="NotoSansTC-Bold.otf")
    ap.add_argument("--out", default=os.path.join(root, "src", "tip"))
    ap.add_argument("--preview", help="also write PNG previews here")
    args = ap.parse_args()
    for name, (char, color) in ICONS.items():
        images = [render(s, char, color, args.font) for s in SIZES]
        write_ico(os.path.join(args.out, name), images)
        print("wrote", os.path.join(args.out, name))
        if args.preview:
            os.makedirs(args.preview, exist_ok=True)
            stem = os.path.splitext(name)[0]
            for im in images:
                im.save(os.path.join(args.preview, f"{stem}-{im.width}px.png"))
            # Side by side at 1x on light and dark backgrounds, then enlarged 4x.
            row = sum(s + 8 for s in SIZES[:-1]) + 8
            for bg, tag in (((243, 243, 243), "light"), ((32, 32, 32), "dark")):
                sheet = Image.new("RGB", (row, 72), bg)
                x = 8
                for im in images[:-1]:
                    sheet.paste(im, (x, 64 - im.width), im)
                    x += im.width + 8
                sheet.resize((row * 4, 72 * 4), Image.NEAREST).save(
                    os.path.join(args.preview, f"{stem}-sheet-{tag}.png"))


if __name__ == "__main__":
    main()
