"""Public incident workflow preserves pre/post pixels and retires cancelled windows."""
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
from unittest.mock import patch

from native_support import probe_environment, require, running_probe, wait_file
from simxr_control import Client

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'mcp-server'))
from mcp_application import McpApplication
from tool_results import ImageResult
from PIL import Image


def main():
    ui = "--ui" in sys.argv
    capture_tool = 'capture_ui_flicker_window' if ui else 'capture_flicker_window'
    status_tool = 'get_ui_flicker_status' if ui else 'get_flicker_status'
    with tempfile.TemporaryDirectory(prefix='sx-incident-') as temporary:
        data = Path(temporary)
        environment = probe_environment(data, SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_GPU_INSPECTION='1', SIMXR_PROBE_RECORDING='1',
                           SIMXR_PROBE_DYNAMIC_COLOR='1', SIMXR_PROBE_CLEAR_FRAME='19', SIMXR_PROBE_FRAMES='26', SIMXR_WIDTH='64', SIMXR_HEIGHT='48')
        if ui:
            environment['SIMXR_PROBE_MOVING_QUAD'] = '1'
        with running_probe([sys.argv[1]], environment) as host:
            application = None
            try:
                wait_file(data / 'control.ready', host)
                with Client(data / f'control.{host.pid}.sock') as observer, ThreadPoolExecutor(max_workers=1) as executor:
                    session, = observer.sessions()
                    application = McpApplication(data)
                    admitted = threading.Event()
                    original = Client.begin_image_window
                    def begin(client, session, **kwargs):
                        result = original(client, session, **kwargs)
                        admitted.set()
                        return result
                    (data / 'control.continue').touch()
                    with patch.object(Client, 'begin_image_window', begin):
                        for frame in range(3, 29):
                            wait_file(data / f'recording.{frame}.ready', host)
                            if frame == 12:
                                capture = executor.submit(application.call, capture_tool, {'timeout': 5, 'max_frames': 8})
                                require(admitted.wait(2), 'Public incident did not admit a native window')
                            if frame == 24:
                                values = capture.result(timeout=5)
                                require(any(isinstance(value, ImageResult) for value in values), values)
                                report = json.loads(values[0].text)
                                require(report['transport'] == 'retained_image_window' and report['framesAvailable'] == 22, report)
                                require(report['window']['trigger_frame'] == '12' and report['window']['pre_count'] == 10, report)
                                directory = Path(report['incidentDirectory'])
                                for selected in (3, 4, 23, 24):
                                    with Image.open(directory / f'frame{selected}_preview.bmp') as image:
                                        expected = (255, 0, 0) if selected % 2 else (0, 255, 0)
                                        if ui:
                                            rectangle = next(row for row in report['window']['entries'] if row['frame_id'] == str(selected))['quad_rectangles'][1]
                                            width, height = rectangle['width'], rectangle['height']
                                            require(image.size == (width * 2, height) and image.getpixel((width // 2, height // 2)) == (0, 0, 0) and
                                                    image.getpixel((width + width // 2, height // 2)) == expected,
                                                    'UI incident used world pixels or a different historical rectangle')
                                        else:
                                            require(image.size == (128, 48) and image.getpixel((16, 16)) == expected and
                                                    image.getpixel((80, 16)) == (0, 0, 255), 'Incident pixels differ from the exact frame')
                                require(report['window']['entries'][16]['phase'] == 'clear', 'Submitted clear lost its identity')
                                with Image.open(directory / 'frame19_preview.bmp') as cleared:
                                    require(cleared.getpixel((600, 300)) == (0, 0, 0), 'Clear placeholder is not black')
                                require(not (data / 'flicker_capture_request.json').exists() and not (data / 'ui_flicker_capture_request.json').exists(), 'Native incident wrote a legacy request')
                                status = json.loads(application.call(status_tool, {'include_images': False})[0].text)
                                require(status['incidentDirectory'] == str(directory), status)
                                timed_out = application.call(capture_tool, {'timeout': 1})
                                require('timed out' in timed_out[0].text.lower(), timed_out)
                                replacement = observer.begin_image_window(session)['image_window_id']
                                observer.release_image_window(replacement)
                                admitted.clear()
                                capture = executor.submit(application.call, capture_tool, {'timeout': 15})
                                require(admitted.wait(2), 'Cancellation fixture was not admitted')
                                application.close()
                                require('closed' in capture.result(timeout=2)[0].text.lower(), 'Close did not interrupt incident wait')
                                replacement = observer.begin_image_window(session)['image_window_id']
                                observer.release_image_window(replacement)
                                require(len(list((data / 'retained-incidents').glob('*/*/incident.json'))) == 1,
                                        'Failed incidents left published artifacts')
                            (data / f'recording.{frame}.continue').touch()
                output = host.communicate(timeout=15)[0]
                require(host.returncode == 0, output)
                require('VUID-' not in output and 'Vulkan validation error' not in output, output)
                print('PASS MCP pre/post incident pixels, artifact identity, timeout/cancellation retirement, no legacy request')
            finally:
                if application is not None:
                    application.close()


if __name__ == '__main__':
    main()
