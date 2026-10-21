"""Explicit screenshots through retained GPU image/export ownership."""
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

    def _retain_filtered(self, client, session, layer, deadline, wait):
        from simxr_control import CommandKind, TargetKind
        target = TargetKind.PROJECTION_LAYERS if layer == 'projection' else TargetKind.QUAD_LAYERS
        lease = client.acquire(session)
        retained = None
        renewal = time.monotonic() + 1
        try:
            operation = client.submit(lease, CommandKind.START_IMAGE_RECORDING, target=target,
                                      recording_capacity=1, recording_frames=1)['operation_id']
            while True:
                if time.monotonic() >= renewal:
                    client.renew(lease)
                    renewal = time.monotonic() + 1
                result = client.operation(session, operation)
                if result['phase'] not in ('queued', 'pending'):
                    break
                self._wait(deadline, wait)
            if result['phase'] != 'complete':
                raise CaptureFailed('Filtered screenshot could not start: ' + result['progress'])
            capture, = result['captures']
            client.acknowledge(lease, result['command_id'])
            while True:
                if time.monotonic() >= renewal:
                    client.renew(lease)
                    renewal = time.monotonic() + 1
                snapshot = client.capture(session, capture)
                if snapshot['phase'] == 'ready':
                    inspection = client.inspect(session, capture)
                    try:
                        frame = inspection['document']
                        settings = client.revision(frame['view_settings']['revision_id'])['document']
                        composition = {'frame': {'generation': str(frame['generation']), 'frame_id': str(frame['frame_id'])},
                                       'ipd': settings['ipd'], 'fovs': settings['outer_fov']}
                    finally:
                        client.release_inspection(inspection['inspection_id'])
                    retained = client.retain_image(session, capture)
                    retained['composition'] = composition
                    return retained
                if snapshot['phase'] not in ('reserved', 'issued', 'begun', 'gpu_pending'):
                    raise CaptureFailed('Filtered screenshot is unavailable: ' + snapshot['phase'])
                self._wait(deadline, wait)
        except BaseException as error:
            if self._binding.is_uncertain(error):
                self._binding.invalidate(error)
            raise
        finally:
            # Relinquish stops the finite recording and releases its reserved/captured slots.
            # A successfully retained reader owns its image independently.
            if self._binding.client is client:
                try:
                    client.relinquish(lease)
                except BaseException:
                    if retained is not None:
                        client.release_image(retained['image_reader_id'])
                    raise

    def capture(self, eye, timeout, wait, layer='all'):
        """None selects legacy only before admission, for absent capability/discovery."""
        if eye not in ('both', 'left', 'right'):
            raise ValueError('Screenshot eye must be both, left or right')
        if layer not in ('all', 'projection', 'quad'):
            raise ValueError('Screenshot layer must be all, projection or quad')
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
            needed = Capability.IMAGE_EXPORT | (Capability.LIVE_PREVIEW if layer == 'all' else
                     Capability.IMAGE_RECORDING | Capability.FILTERED_COMPOSITION)
            if client.status(session)['capabilities'] & needed != needed:
                return None
            deadline = time.monotonic() + timeout
            if layer != 'all':
                retained = self._retain_filtered(client, session, layer, deadline, wait)
            else:
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
                composition = retained.get('composition')
                if composition is not None and composition['frame'] != {key: str(value) for key, value in info['frame'].items()}:
                    raise CaptureFailed('Captured image and composition settings have different frame identities')
                descriptor = info['image_descriptor']
                width, height, layers = (descriptor[key] for key in ('width', 'height', 'array_layers'))
                if descriptor['format'] != 43 or layers != 2 or width <= 0 or height <= 0:
                    raise CaptureFailed('Retained screenshot requires the canonical RGBA8 sRGB stereo image')
                slices = (0, 1) if eye == 'both' else (0,) if eye == 'left' else (1,)
                size = width * height * 4
                if size * len(slices) > self.MAX_BYTES:
                    raise CaptureFailed('Retained screenshot exceeds the 128 MiB image budget')
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
                metadata = {'transport': 'retained_preview' if layer == 'all' else 'retained_capture',
                            'layer': layer, 'session': str(info['session']),
                            'frame': {key: str(value) for key, value in info['frame'].items()},
                            'target': info['target'], 'width': output.width, 'height': output.height,
                            'eye': eye, 'format': descriptor['format']}
                if composition is not None:
                    metadata['composition'] = composition
                return encoded.getvalue(), metadata
            finally:
                client.release_image(reader)
        except Exception as error:
            if self._binding.is_uncertain(error):
                self._binding.invalidate(error)
            raise CaptureFailed('Retained screenshot failed: ' + str(error)) from error
