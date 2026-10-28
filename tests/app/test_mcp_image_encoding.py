#!/usr/bin/env python3
"""Independently decode color conversion fixtures produced by the native encoder."""
import base64
from io import BytesIO
import subprocess
import sys

from PIL import Image


def main():
    output = subprocess.check_output([sys.argv[1], "--fixtures"], text=True)
    images = dict(line.split(":", 1) for line in output.splitlines())
    with Image.open(BytesIO(base64.b64decode(images["HDR"], validate=True))) as picture:
        assert picture.size == (2, 1)
        assert picture.convert("RGBA").tobytes() == bytes((0, 128, 255, 255, 0, 255, 0, 255))
    with Image.open(BytesIO(base64.b64decode(images["RGB10"], validate=True))) as picture:
        assert picture.size == (1, 1)
        assert picture.convert("RGBA").getpixel((0, 0)) == (0, 128, 255, 255)
    print("PASS native HDR and 10-bit PNG color conversion")


if __name__ == "__main__":
    main()
