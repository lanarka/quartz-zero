from PIL import Image
import sys

def convert_png_to_header(png_path, header_path, array_name="pixmap", alpha=True):
    img = Image.open(png_path).convert("RGBA")
    pixels = list(img.getdata())

    with open(header_path, 'w') as f:
        f.write(f"#ifndef {array_name.upper()}_H\n")
        f.write(f"#define {array_name.upper()}_H\n\n")
        f.write(f"#include <stdint.h>\n\n")
        f.write(f"const uint8_t {array_name}[] = {{\n")

        for i, pixel in enumerate(pixels):
            r, g, b, a = pixel
            if alpha:
                f.write(f"    0x{r:02X}, 0x{g:02X}, 0x{b:02X}, 0x{a:02X},")
            else:
                f.write(f"    0x{r:02X}, 0x{g:02X}, 0x{b:02X},")
            if (i + 1) % 4 == 0:
                f.write("\n")

        f.write("};\n\n")
        f.write(f"#define {array_name.upper()}_WIDTH {img.width}\n")
        f.write(f"#define {array_name.upper()}_HEIGHT {img.height}\n\n")
        f.write(f"#endif // {array_name.upper()}_H\n")

    print(f"Header file '{header_path}' generated with size {img.width}x{img.height}")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} input.png output.h")
        sys.exit(1)
    convert_png_to_header(sys.argv[1], sys.argv[2])
    sys.exit(0)
