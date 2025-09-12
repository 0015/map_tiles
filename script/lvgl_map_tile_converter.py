import os
import struct
import argparse
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from PIL import Image  # pip install pillow

# No implicit defaults; these are set from CLI in main()
INPUT_ROOT = None
OUTPUT_ROOT = None


# Convert RGB to 16-bit RGB565
def to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


# Strip .png or .bin extensions
def clean_tile_name(filename):
    name = filename
    while True:
        name, ext = os.path.splitext(name)
        if ext.lower() not in [".png", ".bin", ".jpg", ".jpeg"]:
            break
        filename = name
    return filename


# Create LVGL v9-compatible .bin image
def make_lvgl_bin(png_path, bin_path):
    im = Image.open(png_path).convert("RGB")
    w, h = im.size
    pixels = im.load()

    stride = (w * 16 + 7) // 8  # bytes per row (RGB565 = 16 bpp)
    flags = 0x00               # no compression, no premult
    color_format = 0x12        # RGB565
    magic = 0x19

    header = bytearray()
    header += struct.pack("<B", magic)
    header += struct.pack("<B", color_format)
    header += struct.pack("<H", flags)
    header += struct.pack("<H", w)
    header += struct.pack("<H", h)
    header += struct.pack("<H", stride)
    header += struct.pack("<H", 0)  # reserved

    body = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b = pixels[x, y]
            rgb565 = to_rgb565(r, g, b)
            body += struct.pack("<H", rgb565)

    os.makedirs(os.path.dirname(bin_path), exist_ok=True)

    if os.path.isdir(bin_path):
        # In case a folder mistakenly exists where a file should
        print(f"[Fix] Removing incorrect folder: {bin_path}")
        os.rmdir(bin_path)

    with open(bin_path, "wb") as f:
        f.write(header)
        f.write(body)

    print(f"[OK] {png_path} → {bin_path}")


# Yield (input_path, output_path) pairs for all PNG tiles under INPUT_ROOT
def _iter_tile_paths():
    for zoom in sorted(os.listdir(INPUT_ROOT)):
        zoom_path = os.path.join(INPUT_ROOT, zoom)
        if not os.path.isdir(zoom_path) or not zoom.isdigit():
            continue

        for x_tile in sorted(os.listdir(zoom_path)):
            x_path = os.path.join(zoom_path, x_tile)
            if not os.path.isdir(x_path) or not x_tile.isdigit():
                continue

            for y_file in sorted(os.listdir(x_path)):
                if not y_file.lower().endswith(".png"):
                    continue

                tile_base = clean_tile_name(y_file)
                input_path = os.path.join(INPUT_ROOT, zoom, x_tile, y_file)
                output_path = os.path.join(OUTPUT_ROOT, zoom, x_tile, f"{tile_base}.bin")
                yield input_path, output_path


def convert_all_tiles(jobs=1, force=False):
    """
    Convert tiles with optional threading.
    - jobs: number of worker threads (>=1)
    - force: if True, re-generate even if output exists
    """
    if not os.path.isdir(INPUT_ROOT):
        print(f"[ERROR] '{INPUT_ROOT}' not found.")
        return

    # Build task list (skip existing unless --force)
    tasks = []
    for input_path, output_path in _iter_tile_paths():
        if not force and os.path.isfile(output_path):
            print(f"[Skip] {output_path}")
            continue
        tasks.append((input_path, output_path))

    if not tasks:
        print("[INFO] Nothing to do.")
        return

    print(f"[INFO] Converting {len(tasks)} tiles with {jobs} thread(s)...")

    if jobs <= 1:
        # Serial path
        for inp, outp in tasks:
            try:
                make_lvgl_bin(inp, outp)
            except Exception as e:
                print(f"[Error] Failed to convert {inp} → {e}")
        return

    # Threaded path
    print_lock = threading.Lock()
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        future_map = {ex.submit(make_lvgl_bin, inp, outp): (inp, outp) for inp, outp in tasks}
        for fut in as_completed(future_map):
            inp, outp = future_map[fut]
            try:
                fut.result()
            except Exception as e:
                with print_lock:
                    print(f"[Error] Failed to convert {inp} → {e}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Convert OSM PNG tiles into LVGL-friendly .bin files (RGB565).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "-i", "--input",
        required=True,
        default=argparse.SUPPRESS,  # hide '(default: None)'
        help="Input root folder containing tiles in zoom/x/y.png structure",
    )
    parser.add_argument(
        "-o", "--output",
        required=True,
        default=argparse.SUPPRESS,  # hide '(default: None)'
        help="Output root folder where .bin tiles will be written",
    )
    parser.add_argument(
        "-j", "--jobs",
        type=int,
        default=os.cpu_count(),
        help="Number of worker threads",
    )
    parser.add_argument(
        "-f", "--force",
        action="store_true",
        help="Rebuild even if output file already exists",
    )

    args = parser.parse_args()

    # Basic checks
    if not os.path.isdir(args.input):
        parser.error(f"Input folder not found or not a directory: {args.input}")
    os.makedirs(args.output, exist_ok=True)

    # Apply CLI values
    INPUT_ROOT = args.input
    OUTPUT_ROOT = args.output

    convert_all_tiles(jobs=max(1, args.jobs), force=args.force)
