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
        assert picture.convert("RGBA").tobytes() == bytes((0, 188, 255, 255, 0, 255, 0, 255))
    with Image.open(BytesIO(base64.b64decode(images["RGB10"], validate=True))) as picture:
        assert picture.size == (1, 1)
        assert picture.convert("RGBA").getpixel((0, 0)) == (0, 188, 255, 255)
    for name, expected in (("BGRA", (0, 255, 49, 128)), ("HDR16", (0, 188, 255, 64))):
        with Image.open(BytesIO(base64.b64decode(images[name], validate=True))) as picture:
            assert picture.size == (1, 1)
            assert picture.convert("RGBA").getpixel((0, 0)) == expected
    expected = bytes(value for i in range(256) for value in (i, 255-i, i*73 % 256, i*31 % 256))
    byte_images = [value for name, value in images.items() if name.startswith("BYTES")]
    assert len(byte_images) == 4
    for value in byte_images:
        with Image.open(BytesIO(base64.b64decode(value, validate=True))) as picture:
            assert picture.size == (256, 1)
            assert picture.convert("RGBA").tobytes() == expected
    print("PASS shared image color conversion")


if __name__ == "__main__":
    main()
