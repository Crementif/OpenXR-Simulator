"""Explicit composed screenshots through retained preview/export ownership."""
from io import BytesIO
import time

from legacy_control import SessionBinding
from runtime_artifacts import CaptureFailed


class RetainedScreenshot:
    MAX_BYTES = 128 * 1024 * 1024

    def __init__(self, directory):
        self._binding = SessionBinding(directory, require_commands=False)

    def close(self):
        self._binding.close()

    @staticmethod
    def _wait(deadline, wait):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise CaptureFailed('Retained screenshot timed out')
        wait(min(.005, remaining))

    def capture(self, eye, timeout, wait):
        """None selects legacy only before admission, for absent capability/discovery."""
        if eye not in ('both', 'left', 'right'):
            raise ValueError('Screenshot eye must be both, left or right')
        try:
            if not self._binding.connect():
                return None
            from simxr_control import Capability, ControlError
            from simxr_control.protocol import ErrorCode
            from PIL import Image
            client, session = self._binding.client, self._binding.session
            if client.sessions() != [session]:
                self._binding.close()
                raise RuntimeError('Screenshot session changed; reconnect explicitly')
            needed = Capability.LIVE_PREVIEW | Capability.IMAGE_EXPORT
            if client.status(session)['capabilities'] & needed != needed:
                return None
            deadline = time.monotonic() + timeout
            while True:
                try:
                    retained = client.retain_preview(session)
                    break
                except ControlError as error:
                    if error.code not in (ErrorCode.BUSY, ErrorCode.NOT_FOUND):
                        raise
                    self._wait(deadline, wait)
            reader = retained.get('image_reader_id')
            if reader is None:
                raise CaptureFailed('The current preview is cleared; no retained image is available')
            try:
                info = client.image_info(reader)
                descriptor = info['image_descriptor']
                width, height, layers = (descriptor[key] for key in ('width', 'height', 'array_layers'))
                if descriptor['format'] != 43 or layers != 2 or width <= 0 or height <= 0:
                    raise CaptureFailed('Retained screenshot requires the canonical RGBA8 sRGB stereo image')
                slices = (0, 1) if eye == 'both' else (0,) if eye == 'left' else (1,)
                size = width * height * 4
                if size * len(slices) > self.MAX_BYTES:
                    raise CaptureFailed('Retained screenshot exceeds the 128 MiB image budget')
                output = Image.new('RGBA', (width * len(slices), height))
                for position, layer in enumerate(slices):
                    started = client.start_export(reader, layer=layer)
                    ticket = started['export_id']
                    try:
                        if started['total_bytes'] != size:
                            raise CaptureFailed('Retained export dimensions do not match its image')
                        while True:
                            status = client.export_info(ticket)
                            if status['phase'] == 'complete':
                                break
                            if status['phase'] != 'pending':
                                raise CaptureFailed('Retained screenshot export failed: ' + status['phase'])
                            self._wait(deadline, wait)
                        pixels = bytearray()
                        while len(pixels) < size:
                            part = client.read_export(ticket, offset=len(pixels), length=min(size - len(pixels), 65536))
                            chunk = part['bytes']
                            if part['offset'] != len(pixels) or not chunk or len(chunk) > size - len(pixels):
                                raise CaptureFailed('Retained export returned an invalid pixel chunk')
                            pixels.extend(chunk)
                            if time.monotonic() > deadline:
                                raise CaptureFailed('Retained screenshot read timed out')
                        output.paste(Image.frombytes('RGBA', (width, height), bytes(pixels)), (position * width, 0))
                    finally:
                        client.release_export(ticket)
                encoded = BytesIO()
                output.save(encoded, format='PNG')
                metadata = {'transport': 'retained_preview', 'session': str(info['session']),
                            'frame': {key: str(value) for key, value in info['frame'].items()},
                            'target': info['target'], 'width': output.width, 'height': output.height,
                            'eye': eye, 'format': descriptor['format']}
                return encoded.getvalue(), metadata
            finally:
                client.release_image(reader)
        except Exception as error:
            if self._binding.is_uncertain(error):
                self._binding.invalidate(error)
            raise CaptureFailed('Retained screenshot failed: ' + str(error)) from error
