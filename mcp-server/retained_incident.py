"""Composed pre/post incident artifacts from a bounded native GPU window."""
import json
from pathlib import Path
import shutil
import tempfile
import time

from debug_control import _json_result
from legacy_control import SessionBinding
from retained_image import read_stereo_image, wait_for_image
from runtime_artifacts import CaptureFailed


class RetainedIncident:
    def __init__(self, directory, *, ui=False):
        self._ui = ui
        self._directory = Path(directory)
        self._binding = SessionBinding(directory, require_commands=False)
        self.latest = None

    def close(self):
        self._binding.close()

    @staticmethod
    def _crop_ui(image, rectangles):
        from PIL import Image
        eye_width = image.width // 2
        crops = []
        for eye, rectangle in enumerate(rectangles):
            x, y, width, height = (rectangle[key] for key in ('x', 'y', 'width', 'height'))
            if width == 0 and height == 0:
                crops.append(None)
                continue
            if min(x, y) < 0 or min(width, height) <= 0 or x + width > eye_width or y + height > image.height:
                raise CaptureFailed('UI rectangle is outside its exact historical image')
            crops.append(image.crop((eye * eye_width + x, y, eye * eye_width + x + width, y + height)))
        if len(crops) != 2 or not any(crops):
            raise CaptureFailed('Ready UI image has no visible historical quad rectangle')
        width = max(crop.width for crop in crops if crop is not None)
        height = max(crop.height for crop in crops if crop is not None)
        result = Image.new('RGBA', (width * 2, height), 'black')
        for eye, crop in enumerate(crops):
            if crop is not None:
                result.paste(crop, (eye * width, 0))
        return result

    def capture(self, timeout, wait):
        try:
            if not self._binding.connect():
                raise CaptureFailed('No runtime control endpoint found')
            from simxr_control import Capability, TargetKind
            client, session = self._binding.client, self._binding.session
            if client.sessions() != [session]:
                self._binding.close()
                raise CaptureFailed('Incident session changed; reconnect explicitly')
            needed = Capability.IMAGE_WINDOW | Capability.IMAGE_EXPORT
            if self._ui:
                needed |= Capability.QUAD_IMAGE_HISTORY
            if client.status(session)['capabilities'] & needed != needed:
                raise CaptureFailed('The selected runtime does not support incident capture')
            root = self._directory / 'retained-incidents' / f'{session.id}-{session.generation}'
            root.mkdir(parents=True, exist_ok=True)
            if sum(1 for path in root.glob('incident_*') if path.is_dir()) >= 40:
                raise CaptureFailed('Incident artifact limit reached for this session generation')
            deadline = time.monotonic() + timeout
            started = (client.begin_image_window(session, target=TargetKind.QUAD_LAYERS) if self._ui
                       else client.begin_image_window(session))
            window = started['image_window_id']
            directory = None
            committed = False
            try:
                while True:
                    document = client.image_window(window)['document']
                    if document['phase'] != 'collecting':
                        break
                    wait_for_image(deadline, wait)
                if document['phase'] != 'complete':
                    raise CaptureFailed('Incident window was interrupted before all following frames arrived')
                if not document['pre_count'] or document['post_count'] != 12:
                    raise CaptureFailed('Incident window has incomplete pre/post history')
                while any(row['phase'] == 'pending' for row in document['entries']):
                    wait_for_image(deadline, wait)
                    document = client.image_window(window)['document']
                gaps = [row for row in document['entries'] if row['phase'] not in ('ready', 'clear')]
                if gaps:
                    raise CaptureFailed('Incident contains unavailable frames: ' + json.dumps(_json_result(gaps)))
                directory = Path(tempfile.mkdtemp(prefix='incident_', dir=root))
                for entry in document['entries']:
                    wait_for_image(deadline, wait)
                    frame = entry['frame_id']
                    if entry['phase'] == 'clear':
                        from PIL import Image, ImageDraw
                        image = Image.new('RGB', (640, 320), 'black')
                        ImageDraw.Draw(image).text((12, 12), ('No visible quad (no GPU image)' if self._ui else 'Submitted clear (no GPU image)'), fill='white')
                        image.save(directory / f'frame{frame}_preview.bmp')
                        continue
                    reader = client.retain_window_image(window, frame)['image_reader_id']
                    try:
                        info = client.image_info(reader)
                        if (info['frame'] != {'generation': document['generation'], 'frame_id': frame} or
                                info['target']['capture_kind'] != (TargetKind.QUAD_LAYERS if self._ui else TargetKind.COMPOSED_EYES)):
                            raise CaptureFailed('Incident image identity differs from its window frame')
                        image = read_stereo_image(client, reader, info['image_descriptor'], (0, 1), deadline, wait,
                                                  16 * 1024 * 1024)
                        if self._ui:
                            image = self._crop_ui(image, entry['quad_rectangles'])
                        image.thumbnail((640, 360))
                        image.convert('RGB').save(directory / f'frame{frame}_preview.bmp')
                    finally:
                        client.release_image(reader)
                result = {'captureSource': 'openxr-simulator-ui-quad' if self._ui else 'openxr-simulator-composed-preview', 'transport': 'retained_image_window',
                          'incidentDirectory': str(directory), 'framesAvailable': len(document['entries']),
                          'session': str(session.id), 'window': _json_result(document), 'manual': True}
                (directory / 'incident.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
                committed = True
            finally:
                try:
                    client.release_image_window(window)
                except BaseException:
                    committed = False
                    raise
                finally:
                    if directory is not None and not committed:
                        shutil.rmtree(directory)
            self.latest = result
            return result
        except Exception as error:
            if self._binding.is_uncertain(error):
                self._binding.invalidate(error)
            raise CaptureFailed('Retained incident failed: ' + str(error)) from error
