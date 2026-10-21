"""Bounded export of an independently retained canonical stereo GPU image."""
import time
from runtime_artifacts import CaptureFailed


def wait_for_image(deadline, wait):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise CaptureFailed('Retained image timed out')
    wait(min(.005, remaining))


def read_stereo_image(client, reader, descriptor, slices, deadline, wait, max_bytes):
    from PIL import Image
    width, height, layers = (descriptor[key] for key in ('width', 'height', 'array_layers'))
    if descriptor['format'] != 43 or layers != 2 or width <= 0 or height <= 0:
        raise CaptureFailed('Retained image requires canonical RGBA8 sRGB stereo pixels')
    size = width * height * 4
    if size * len(slices) > max_bytes:
        raise CaptureFailed('Retained image exceeds its byte budget')
    output = Image.new('RGBA', (width * len(slices), height))
    for position, array_slice in enumerate(slices):
        started = client.start_export(reader, layer=array_slice)
        ticket = started['export_id']
        try:
            if started['total_bytes'] != size:
                raise CaptureFailed('Retained export dimensions do not match its image')
            while True:
                status = client.export_info(ticket)
                if status['phase'] == 'complete':
                    break
                if status['phase'] != 'pending':
                    raise CaptureFailed('Retained image export failed: ' + status['phase'])
                wait_for_image(deadline, wait)
            pixels = bytearray()
            while len(pixels) < size:
                wait(0)
                part = client.read_export(ticket, offset=len(pixels), length=min(size - len(pixels), 256 * 1024))
                chunk = part['bytes']
                if part['offset'] != len(pixels) or not chunk or len(chunk) > size - len(pixels):
                    raise CaptureFailed('Retained export returned an invalid pixel chunk')
                pixels.extend(chunk)
                if time.monotonic() > deadline:
                    raise CaptureFailed('Retained image read timed out')
            output.paste(Image.frombytes('RGBA', (width, height), bytes(pixels)), (position * width, 0))
        finally:
            client.release_export(ticket)
    return output
