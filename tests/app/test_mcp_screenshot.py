#!/usr/bin/env python3
"""Exercise one-call native MCP screenshots, cancellation and exact stereo selection."""

import base64
from concurrent.futures import ThreadPoolExecutor
import http.client
from io import BytesIO
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

from PIL import Image
from test_mcp_connector import Client, eventually


def main():
    app, connector, probe = (str(Path(value).resolve()) for value in sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix='simxr-mcp-screenshot-') as temporary:
        root = Path(temporary)
        env = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_MCP_DISCOVERY_DIR=str(root / 'discovery'),
                   SIMXR_COMPANION_SETTINGS=str(root / 'settings'), SIMXR_DETERMINISTIC='0',
                   SDL_VIDEODRIVER='simxr-unavailable-video-driver')
        for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'XR_RUNTIME_JSON', 'SIMXR_CONTROL_SOCKET'):
            env.pop(key, None)
        handles = []
        with (root / 'host.log').open('wb') as log:
            bridge = subprocess.Popen([connector, '--app', app, '--windowless'], env=env, stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1, start_new_session=True)
            worker_bridge = None
            try:
                client = Client(bridge)
                assert 'result' in client.rpc('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                    'clientInfo': {'name': 'screenshot-observer', 'version': '1'}})
                client.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
                names = {entry['name'] for entry in client.rpc('tools/list')['result']['tools']}
                assert 'capture_screenshot' in names and 'validate_stereo' in names
                discovery = eventually(lambda: next((p for p in (root / 'discovery').glob('*.json')), None))
                record = json.loads(discovery.read_text())
                handles.append(os.pidfd_open(int(record['process_id'])))
                launched = client.tool('simulator_launch_application', {'executable': probe, 'environment': {
                    'SIMXR_PROBE_CONTROL': '1', 'SIMXR_PROBE_GPU_INSPECTION': '1', 'SIMXR_PROBE_RECORDING': '1',
                    'SIMXR_PROBE_FRAMES': '4', 'SIMXR_WIDTH': '128', 'SIMXR_HEIGHT': '96'}})
                assert not launched['error'], launched
                application = launched['data']['application_id']
                def connected():
                    status = client.tool('simulator_application_status', {'application_id': application})
                    assert not status['error'], status
                    return status['data'] if status['data']['session_running'] else None
                state = eventually(connected, timeout=30)
                handles.append(os.pidfd_open(int(state['process_id'])))
                eventually(lambda: (root / 'control.ready').exists(), timeout=15)
                opened = client.tool('debug_connect', {'application_id': application})
                assert not opened['error'], opened
                connection = opened['data']['connection']
                worker_bridge = subprocess.Popen([connector, '--app', app, '--windowless'], env=env, stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1, start_new_session=True)
                worker = Client(worker_bridge)
                assert 'result' in worker.rpc('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                    'clientInfo': {'name': 'screenshot-worker', 'version': '1'}})
                worker.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
                (root / 'control.continue').touch()
                eventually(lambda: (root / 'recording.3.ready').exists(), timeout=15)
                def capture(arguments):
                    return worker.rpc('tools/call', {'name': 'capture_screenshot', 'arguments': dict(application_id=application, **arguments)})
                def inspect(response, expected_layer, eyes, expected_frame=None):
                    result = response['result']
                    assert not result['isError'], result
                    content = result['content']
                    metadata = json.loads(content[0]['text'])
                    assert metadata['layer'] == expected_layer and metadata['eye'] == eyes, metadata
                    assert metadata['frame']['generation'] == state['generation'], metadata
                    if expected_frame:
                        assert metadata['frame']['frame_id'] == str(expected_frame), metadata
                    if expected_layer != 'all':
                        assert metadata['composition']['frame'] == metadata['frame'] and metadata['composition']['ipd'] > 0, metadata
                    expected_eyes = ['left', 'right'] if eyes == 'both' else [eyes]
                    assert [eye['eye'] for eye in metadata['eyes']] == expected_eyes
                    assert len(content) == len(expected_eyes) + 1
                    for image, eye in zip(content[1:], metadata['eyes']):
                        assert image['type'] == 'image' and image['mimeType'] in ('image/jpeg', 'image/png')
                        with Image.open(BytesIO(base64.b64decode(image['data']))) as picture:
                            assert picture.size == (eye['width'], eye['height']) == (64, 48), (picture.size, eye)
                            center = picture.convert('RGB').getpixel((32, 24))
                            corner = picture.convert('RGB').getpixel((56, 40))
                            if expected_layer == 'projection':
                                channel = 0 if eye['eye'] == 'left' else 2
                                assert center[channel] > 240 and corner[channel] > 240, (eye, center, corner)
                                assert center[2 - channel] < 15 and corner[2 - channel] < 15, (eye, center, corner)
                            if expected_layer == 'quad':
                                assert center[0] > 240 and center[2] < 15 and max(corner) < 15, (eye, center, corner)
                    return metadata
                before = client.tool('debug_status', {'connection': connection})['data']
                preview = inspect(capture({'eye': 'both', 'layer': 'all', 'format': 'png', 'max_width': 64}), 'all', 'both')
                assert preview['resized'] and preview['source_width'] == 128 and preview['source_height'] == 96
                assert preview['display']['source'] == preview['frame']
                after = client.tool('debug_status', {'connection': connection})['data']
                assert before['manual'] == after['manual'] and before['pipeline']['permits'] == after['pipeline']['permits']
                assert not (root / 'recording.4.ready').exists()
                inspect(capture({'eye': 'right', 'layer': 'all', 'format': 'jpeg', 'max_width': 64}), 'all', 'right')
                owner = client.tool('debug_acquire', {'connection': connection})
                assert not owner['error'], owner
                conflict = capture({'layer': 'projection'})
                assert conflict['result']['isError'], conflict
                assert not client.tool('debug_relinquish', {'connection': connection, 'lease': owner['data']['lease']})['error']
                time.sleep(.2)
                timeout = capture({'layer': 'projection', 'timeout': 0.25})
                assert timeout['result']['isError'] and 'timed out' in timeout['result']['content'][0]['text'], timeout
                def recording_stopped():
                    status = client.tool('debug_status', {'connection': connection})
                    return not status['error'] and status['data']['image_recording']['phase'] != 'recording'
                eventually(recording_stopped, timeout=10)
                headers = {'Authorization': 'Bearer ' + record['token'], 'Accept': 'application/json, text/event-stream',
                           'Content-Type': 'application/json'}
                def request(method, message=None):
                    transport = http.client.HTTPConnection('127.0.0.1', record['port'], timeout=15)
                    try:
                        transport.request(method, '/mcp', None if message is None else json.dumps(message), headers)
                        response = transport.getresponse()
                        return response.status, response.read(), response.getheader('MCP-Session-Id')
                    finally:
                        transport.close()
                status, body, session = request('POST', {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize',
                    'params': {'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'cancel', 'version': '1'}}})
                assert status == 200 and session
                headers['MCP-Session-Id'] = session
                assert request('POST', {'jsonrpc': '2.0', 'method': 'notifications/initialized'})[0] == 202
                with ThreadPoolExecutor(max_workers=1) as executor:
                    cancelled = executor.submit(request, 'POST', {'jsonrpc': '2.0', 'id': 17, 'method': 'tools/call',
                        'params': {'name': 'capture_screenshot', 'arguments': {'application_id': application, 'layer': 'quad', 'timeout': 10}}})
                    def recording_active():
                        status = client.tool('debug_status', {'connection': connection})
                        return not status['error'] and status['data']['image_recording']['phase'] == 'recording'
                    eventually(recording_active, timeout=5)
                    assert request('POST', {'jsonrpc': '2.0', 'method': 'notifications/cancelled', 'params': {'requestId': 17}})[0] == 202
                    assert cancelled.result(timeout=5)[0] == 202
                    eventually(recording_stopped, timeout=10)
                assert request('DELETE')[0] == 204
                with ThreadPoolExecutor(max_workers=1) as executor:
                    projection = executor.submit(capture, {'layer': 'projection', 'eye': 'both', 'format': 'png', 'max_width': 64, 'timeout': 10})
                    eventually(recording_active, timeout=5)
                    time.sleep(7.3)
                    assert not projection.done(), 'The connector must preserve a valid long capture request'
                    (root / 'recording.3.continue').touch()
                    inspect(projection.result(timeout=10), 'projection', 'both', 4)
                    eventually(lambda: (root / 'recording.4.ready').exists(), timeout=15)
                    time.sleep(.2)
                    quad = executor.submit(capture, {'layer': 'quad', 'eye': 'right', 'format': 'png', 'max_width': 64, 'timeout': 10})
                    eventually(recording_active, timeout=5)
                    (root / 'recording.4.continue').touch()
                    inspect(quad.result(timeout=10), 'quad', 'right', 5)
                    eventually(lambda: (root / 'recording.5.ready').exists(), timeout=15)
                    stereo = executor.submit(worker.tool, 'validate_stereo', {'application_id': application, 'timeout': 10})
                    eventually(recording_active, timeout=5)
                    (root / 'recording.5.continue').touch()
                    result = stereo.result(timeout=10)
                    assert not result['error'], result
                    assert result['data']['frame']['frame_id'] == '6', result
                    assert result['data']['composition']['frame'] == result['data']['frame'], result
                    assert result['data']['verdict'] == 'INCONCLUSIVE_NO_FEATURES', result
                eventually(lambda: (root / 'recording.6.ready').exists(), timeout=15)
                (root / 'recording.6.continue').touch()
                def exited():
                    result = client.tool('simulator_application_status', {'application_id': application})
                    return result['data'] if not result['error'] and result['data']['launch_state'] == 'exited' else None
                assert eventually(exited, timeout=15)['exit_code'] == 0
                assert not client.tool('simulator_shutdown', {})['error']
                bridge.stdin.close()
                assert bridge.wait(timeout=10) == 0
                print('PASS native one-call screenshots, stereo frame identity, GPU resize, timeout and cancellation')
            finally:
                if worker_bridge and worker_bridge.poll() is None:
                    worker_bridge.kill()
                    worker_bridge.wait()
                if bridge.poll() is None:
                    bridge.kill()
                    bridge.wait()
                for handle in handles:
                    try:
                        signal.pidfd_send_signal(handle, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    finally:
                        os.close(handle)


if __name__ == '__main__':
    main()
