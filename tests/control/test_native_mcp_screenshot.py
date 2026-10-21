"""Legacy screenshot name using the canonical live preview/export protocol."""
from concurrent.futures import ThreadPoolExecutor
from io import BytesIO
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from test_native_quad import require, wait_file
from simxr_control import Client, CommandKind

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'mcp-server'))
from mcp_application import McpApplication
from tool_results import ImageResult, TextResult


def main():
    try:
        from PIL import Image
    except ImportError:
        print('SKIP: explicit MCP screenshots require Pillow')
        return 77
    with tempfile.TemporaryDirectory(prefix='sx-mcp-image-') as temporary:
        data = Path(temporary)
        environment = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_RECORDING='1',
                           SIMXR_PROBE_GPU_INSPECTION='1', SIMXR_PROBE_FRAMES='4', SIMXR_WIDTH='64', SIMXR_HEIGHT='48')
        environment.pop('SIMXR_CONTROL_SOCKET', None)
        host = subprocess.Popen([sys.argv[1]], env=environment, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        application = None
        try:
            wait_file(data / 'control.ready', host)
            endpoint = data / f'control.{host.pid}.sock'
            os.environ['SIMXR_CONTROL_SOCKET'] = str(endpoint)
            with Client(endpoint) as observer:
                session, = observer.sessions()
                (data / 'control.continue').touch()
                wait_file(data / 'recording.3.ready', host)
                lease = observer.acquire(session)
                pause = observer.wait(session, observer.submit(lease, CommandKind.PAUSE)['operation_id'])
                for capture in pause.get('captures', []):
                    observer.wait(session, observer.submit(lease, CommandKind.RELEASE, capture=capture)['operation_id'])
                observer.acknowledge(lease, pause['command_id'])
                for _ in range(16):
                    display = observer.wait(session, observer.submit(lease, CommandKind.DISPLAY_STEP)['operation_id'])
                    observer.wait(session, observer.submit(lease, CommandKind.RELEASE, capture=display['capture_id'])['operation_id'])
                    observer.acknowledge(lease, display['command_id'])
                    if display.get('frame', {}).get('frame_id') == 3:
                        break
                else:
                    raise AssertionError('Explicit fixture display did not select frame 3')
                before = observer.status(session)
                application = McpApplication(data)
                history = observer.projection_history(session, count=1)['document']
                require(len(history['entries']) == 1 and history['entries'][0]['frame_id'] == 3, history)
                log = json.loads(application.call('get_projection_log', {'max_entries': 1})[0].text)
                require(log['transport'] == 'frame_history' and log['entries'][0]['frame'] == '3', log)
                require(log['entries'][0]['left_rect'] == [0, 0, 64, 48] and
                        log['entries'][0]['right_fov']['aR'] > 0, log)
                require(not (data / 'projection_log_dump_request').exists(), 'Projection history used a file request')
                timed_out = application.call('capture_screenshot', {'layer': 'all', 'timeout': 0})
                require(not any(isinstance(value, ImageResult) for value in timed_out), timed_out)
                require('timed out' in str(timed_out), timed_out)
                for eye in ('both', 'left', 'right') + ('both',) * 8:
                    values = application.call('capture_screenshot', {'eye': eye, 'layer': 'all'})
                    pictures = [value for value in values if isinstance(value, ImageResult)]
                    require(len(pictures) == 1, values)
                    text = next(value.text for value in values if isinstance(value, TextResult))
                    metadata = json.loads(text.split('\n')[-1])
                    require(metadata['transport'] == 'retained_preview' and metadata['frame']['frame_id'] == '3', metadata)
                    with Image.open(BytesIO(pictures[0].data)) as picture:
                        require(picture.size == ((128 if eye == 'both' else 64), 48), picture.size)
                        red = picture.getpixel((8, 8))
                        blue = picture.getpixel((picture.width - 8, 8))
                        if eye != 'right':
                            require(red[0] > 200 and red[1] < 40 and red[2] < 40, red)
                        if eye != 'left':
                            require(blue[2] > 200 and blue[0] < 40 and blue[1] < 40, blue)
                after = observer.status(session)
                require(after['manual'] == before['manual'] and after['pipeline']['permits'] == before['pipeline']['permits'] == 0,
                        'Screenshot granted application admission')
                require(not (data / 'screenshot_request.json').exists() and not (data / 'screenshot_status.json').exists(),
                        'Canonical screenshot used an overwrite-prone file request')
                busy = application.call('capture_screenshot', {'layer': 'projection'})
                require(not any(isinstance(value, ImageResult) for value in busy), 'Filtered capture stole debugger ownership')
                observer.wait(session, observer.submit(lease, CommandKind.RESUME)['operation_id'])
                observer.relinquish(lease)
                timed_out = application.call('capture_screenshot', {'layer': 'quad', 'timeout': .02})
                require('timed out' in str(timed_out), timed_out)
                require(observer.status(session)['image_recording']['phase'] != 'recording', 'Timed-out recording remained active')
                with ThreadPoolExecutor(max_workers=1) as workers:
                    for frame, layer, eye in ((4, 'projection', 'both'), (5, 'quad', 'both'), (6, 'projection', 'right')):
                        before = observer.status(session)
                        result = workers.submit(application.call, 'capture_screenshot', {'layer': layer, 'eye': eye})
                        deadline = time.monotonic() + 5
                        while observer.status(session)['image_recording']['phase'] != 'recording':
                            require(not result.done(), result.result() if result.done() else 'Capture ended before admission')
                            require(time.monotonic() < deadline, 'Filtered capture did not reserve a slot')
                            time.sleep(.005)
                        (data / f'recording.{frame - 1}.continue').touch()
                        values = result.result(timeout=10)
                        pictures = [value for value in values if isinstance(value, ImageResult)]
                        require(len(pictures) == 1, values)
                        metadata = json.loads(next(value.text for value in values if isinstance(value, TextResult)).split('\n')[-1])
                        require(metadata['transport'] == 'retained_capture' and metadata['frame']['frame_id'] == str(frame)
                                and metadata['layer'] == layer, metadata)
                        with Image.open(BytesIO(pictures[0].data)) as picture:
                            require(picture.size == ((128 if eye == 'both' else 64), 48), picture.size)
                            right_center = picture.getpixel((picture.width - 32, 24))
                            right_corner = picture.getpixel((picture.width - 8, 8))
                            if layer == 'projection':
                                require(right_center[2] > 200 and right_center[0] < 40, right_center)
                                require(right_corner[2] > 200 and right_corner[0] < 40, right_corner)
                            else:
                                require(right_center[0] > 200 and right_center[2] < 40, right_center)
                                require(max(right_corner) < 40 and max(picture.getpixel((32, 24))) < 40,
                                        'Quad screenshot lost its right-eye visibility')
                        log = json.loads(application.call('get_projection_log', {'max_entries': 2})[0].text)
                        require([entry['frame'] for entry in log['entries']] == [str(frame - 1), str(frame)], log)
                        require(log['omitted_entries'] == frame - 4 and history['entries'][0]['frame_id'] == 3,
                                'Projection history lost its bound or changed an earlier reply')
                        after = observer.status(session)
                        require(after['manual'] == before['manual'] == False and
                                after['pipeline']['permits'] == before['pipeline']['permits'],
                                'Filtered screenshot altered application pacing')
                        require(after['image_recording']['phase'] != 'recording', 'Screenshot left recording active')
                    cancelled = workers.submit(application.call, 'capture_screenshot', {'layer': 'quad'})
                    deadline = time.monotonic() + 5
                    while observer.status(session)['image_recording']['phase'] != 'recording':
                        require(not cancelled.done(), 'Cancellation fixture did not start')
                        require(time.monotonic() < deadline, 'Cancellation fixture did not reserve a slot')
                        time.sleep(.005)
                    application.close()
                    require(not any(isinstance(value, ImageResult) for value in cancelled.result(timeout=5)),
                            'Cancelled capture returned an image without a new frame')
                    require(observer.status(session)['image_recording']['phase'] != 'recording',
                            'Cancelled capture left its reservation active')
                require(not (data / 'screenshot_request.json').exists(), 'Filtered capture used legacy files')
                application.close()
                application = None
                for frame in range(3, 7):
                    (data / f'recording.{frame}.continue').touch()
                wait_file(data / 'control.destroyed', host)
                (data / 'control.retained.continue').touch()
            output = host.communicate(timeout=15)[0]
            require(host.returncode == 0, output)
            require('VUID-' not in output and 'Vulkan validation error' not in output, output)
            print('PASS canonical MCP screenshot, exact frame/eyes, bounded repeated export, no admission or file requests')
        finally:
            if application is not None:
                application.close()
            if host.poll() is None:
                host.kill()
                host.communicate()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
