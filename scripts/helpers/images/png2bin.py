from PIL import Image
import sys

def convert_png_to_bin(png_path, bin_path):
    img = Image.open(png_path).convert("RGBA")
    pixels = list(img.getdata())

    with open(bin_path, 'wb') as f:
        for r, g, b, a in pixels:
            f.write(bytes([r, g, b, a]))

    print(f"Binary RGBA file '{bin_path}' was created ({img.width}x{img.height}, {len(pixels) * 4} bytes)")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} input.png output.rgba")
        sys.exit(1)
    convert_png_to_bin(sys.argv[1], sys.argv[2])
    sys.exit(0)
