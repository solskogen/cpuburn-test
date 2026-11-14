#!/usr/bin/env python3
"""Print the digest that session_86box.py's `wait screen` looks for: of the
region X,Y,W,H of each PNG screenshot given ("-" where the region is not
inside it). With frames recorded on emulated time, the runs of one digest
show how long a screen held, and whether the digest is unique to it."""

import argparse
from pathlib import Path
import sys

from session_86box import region_digest


def region(text):
    try:
        x, y, w, h = (int(v, 0) for v in text.split(","))
    except ValueError:
        raise argparse.ArgumentTypeError("use X,Y,W,H") from None
    if x < 0 or y < 0 or w <= 0 or h <= 0:
        raise argparse.ArgumentTypeError("X and Y from 0, W and H from 1")
    return x, y, w, h


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("region", type=region, metavar="X,Y,W,H")
    parser.add_argument("png", type=Path, nargs="+")
    args = parser.parse_args()
    for path in args.png:
        print(region_digest(path.read_bytes(), args.region) or "-", path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
