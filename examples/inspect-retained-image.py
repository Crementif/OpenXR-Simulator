#!/usr/bin/env python3
"""Export an already retained exact-frame image without advancing the application.

PYTHONPATH=sdk/python python3 examples/inspect-retained-image.py SOCKET CAPTURE OUTPUT_DIRECTORY \
    --target layer_depth --layer 0 --view 1

Obtain CAPTURE from an existing debugger operation. Raw color/depth must have
been retained before source reuse. An unavailable target fails explicitly.
"""
import argparse
import json
from pathlib import Path
import time

from simxr_control import Client, TargetKind


def json_value(value):
    if isinstance(value, bytes):
        return value.hex()
    raise TypeError(f"Unsupported metadata value: {type(value).__name__}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('socket')
    parser.add_argument('capture', type=int)
    parser.add_argument('output', type=Path)
    parser.add_argument('--target', choices=['composed_eyes', 'layer_view', 'layer_depth', 'layer_depth_linear'])
    parser.add_argument('--layer', type=int, default=0)
    parser.add_argument('--view', type=int, default=0)
    parser.add_argument('--slice', type=int, default=0, help='Output array slice; depth and raw views have one')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    with Client(args.socket, name='inspect-retained-image') as client:
        session, = client.sessions()
        selection = {} if args.target is None else dict(target=TargetKind[args.target.upper()], layer=args.layer, view=args.view)
        reader = client.retain_image(session, args.capture, **selection)['image_reader_id']
        try:
            image = client.image_info(reader)
            inspection = client.inspect(session, image['capture_id'])
            try:
                metadata = {'image': image, 'frame': inspection['document'], 'export_slice': args.slice}
                ticket = client.start_export(reader, layer=args.slice)['export_id']
                try:
                    deadline = time.monotonic() + 15
                    status = client.export_info(ticket)
                    while status['phase'] == 'pending':
                        if time.monotonic() >= deadline:
                            raise TimeoutError('Explicit image export did not complete')
                        time.sleep(.01)
                        status = client.export_info(ticket)
                    if status['phase'] != 'complete':
                        raise RuntimeError(status)
                    pixels = bytearray()
                    while len(pixels) < status['total_bytes']:
                        chunk = client.read_export(ticket, len(pixels), 64000)
                        if chunk['offset'] != len(pixels) or not chunk['bytes']:
                            raise RuntimeError('Incomplete export')
                        pixels.extend(chunk['bytes'])
                    descriptor = image['image_descriptor']
                    width, height = descriptor['width'], descriptor['height']
                    if len(pixels) != width * height * 4:
                        raise RuntimeError('Export is not tightly packed RGBA8')
                    rgb = bytearray(width * height * 3)
                    for channel in range(3):
                        rgb[channel::3] = pixels[channel::4]
                    (args.output / 'image.ppm').write_bytes(f'P6\n{width} {height}\n255\n'.encode() + rgb)
                    (args.output / 'metadata.json').write_text(json.dumps(metadata, indent=2, default=json_value) + '\n')
                finally:
                    client.release_export(ticket)
            finally:
                client.release_inspection(inspection['inspection_id'])
        finally:
            client.release_image(reader)
    print(f'Exported frame {image["frame"]["frame_id"]} to {args.output}')


if __name__ == '__main__':
    main()
