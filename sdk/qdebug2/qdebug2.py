#!/usr/bin/env python3
"""
QDebugger II - a textual debugger for compiled QuartzVM binaries (.bin).

Usage:
    python3 qdebug2.py program.bin [--map program.map] [--ram 1048576]

If --map is not given, the tool looks for a file with the same name and a
.map extension next to the .bin file (exactly how qasm --map generates it).
"""

import argparse
import sys

from qd2 import loader
from qd2.app import run_app


def main():
    # Set up the command-line interface: a positional binary file plus two
    # optional overrides (map file path and RAM size).
    parser = argparse.ArgumentParser(
        description="QDebugger II - debugger for QuartzVM binaries")
    parser.add_argument("binfile", help="compiled .bin file (QuartzVM image)")
    parser.add_argument("--map", dest="map_file", default=None,
                         help="path to a .map file with labels (default: <binfile>.map)")
    parser.add_argument("--ram", dest="ram_size", type=int, default=1024 * 1024,
                         help="RAM size in bytes (default: 1048576, as in main.c)")
    args = parser.parse_args()

    # Parse the binary image header/segments; bail out cleanly on any
    # loading error instead of letting a raw traceback leak to the user.
    try:
        image = loader.load_image(args.binfile)
    except (loader.ImageError, OSError) as e:
        print(f"Error loading '{args.binfile}': {e}", file=sys.stderr)
        sys.exit(1)

    # Labels are optional: if no .map file is found (or none was given),
    # load_map() simply returns an empty dict and the debugger falls back
    # to showing raw addresses.
    map_path = args.map_file or loader.default_map_path(args.binfile)
    labels = loader.load_map(map_path)

    # Hand off to the Textual TUI application, which owns the whole
    # run/step/inspect loop from here on.
    run_app(image, labels, args.binfile, args.ram_size)


if __name__ == "__main__":
    main()
