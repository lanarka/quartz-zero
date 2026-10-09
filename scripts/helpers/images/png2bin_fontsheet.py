from PIL import Image
import sys

def convert_fontsheet_to_bin(png_path, bin_path, glyph_w=8, glyph_h=14, cols=16, count=95):
    """
    Converts a spritesheet PNG (a grid of glyph_w x glyph_h tiles, `cols`
    tiles per row, read left-to-right / top-to-bottom) into the .bin format
    expected by quartz: 'count' glyphs, each written out as its
    own contiguous glyph_w*glyph_h*4-byte RGBA block, one after another
    (NOT interleaved row-by-row across the whole sheet, which is what a
    plain png2bin.py conversion would give you).

    glyph(code) = atlas_base + (code-32)*(glyph_w*glyph_h*4)
    """
    img = Image.open(png_path).convert("RGBA")

    with open(bin_path, "wb") as f:
        for i in range(count):
            col = i % cols
            row = i // cols
            x0 = col * glyph_w
            y0 = row * glyph_h
            tile = img.crop((x0, y0, x0 + glyph_w, y0 + glyph_h))
            for r, g, b, a in tile.getdata():
                f.write(bytes([r, g, b, a]))

    total = count * glyph_w * glyph_h * 4
    print(f"Binary RGBA file '{bin_path}' was created "
          f"({count} glyphs, {glyph_w}x{glyph_h} each, {total} bytes)")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} input_sheet.png output.bin "
              f"[glyph_w=8] [glyph_h=14] [cols=16] [count=95]")
        sys.exit(1)
    glyph_w = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    glyph_h = int(sys.argv[4]) if len(sys.argv) > 4 else 14
    cols = int(sys.argv[5]) if len(sys.argv) > 5 else 16
    count = int(sys.argv[6]) if len(sys.argv) > 6 else 95
    convert_fontsheet_to_bin(sys.argv[1], sys.argv[2], glyph_w, glyph_h, cols, count)
    sys.exit(0)
