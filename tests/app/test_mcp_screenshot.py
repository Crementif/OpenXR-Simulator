#!/usr/bin/env python3
"""Exercise one-call native MCP screenshots, cancellation and exact stereo selection."""

import base64
from concurrent.futures import ThreadPoolExecutor
import http.client
from io import BytesIO
import json
from pathlib import Path
import sys
import time
from PIL import Image

from mcp_support import Client, McpTest, eventually


def main():
    app, connector, probe = (str(Path(value).resolve()) for value in sys.argv[1:4])
    with McpTest('simxr-mcp-screenshot-', windowless=True) as test:
        root = test.root

        log = test.log('host.log')
        bridge = test.bridge(connector, '--app', app, '--windowless', log=log)
        worker_bridge = None
        client = Client(bridge)
        client.initialize('screenshot-observer')

        names = {entry['name'] for entry in client.rpc('tools/list')['result']['tools']}
        assert 'capture_screenshot' in names and 'validate_stereo' in names
        record = test.discover()

        launched = client.tool('simulator_launch_application', {'executable': probe, 'environment': {
            'SIMXR_PROBE_CONTROL': '1', 'SIMXR_PROBE_GPU_INSPECTION': '1', 'SIMXR_PROBE_RECORDING': '1',
            'SIMXR_PROBE_FRAMES': '4', 'SIMXR_WIDTH': '128', 'SIMXR_HEIGHT': '96'}})
        assert not launched['error'], launched
        application = launched['data']['application_id']

        state = test.connected(client, application, timeout=30)

        eventually(lambda: (root / 'control.ready').exists(), timeout=15)
        opened = client.tool('debug_connect', {'application_id': application})
        assert not opened['error'], opened
        connection = opened['data']['connection']
        worker_bridge = test.bridge(connector, '--app', app, '--windowless', log=log)
        worker = Client(worker_bridge)
        worker.initialize('screenshot-worker')

        (root / 'control.continue').touch()
        eventually(lambda: (root / 'recording.3.ready').exists(), timeout=15)
        def capture(arguments):
            return worker.rpc('tools/call', {'name': 'capture_screenshot', 'arguments': dict(application_id=application, **arguments)})
        def inspect(response, expected_target, eyes, expected_frame=None):
            result = response['result']
            assert not result['isError'], result
            content = result['content']
            metadata = json.loads(content[0]['text'])
            assert metadata['target'] == expected_target and metadata['eye'] == eyes, metadata
            assert metadata['frame']['generation'] == state['generation'], metadata
            if expected_frame:
                assert metadata['frame']['frame_id'] == str(expected_frame), metadata
            if expected_target != 'current_display':
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
                    if expected_target == 'projection_layers':
                        channel = 0 if eye['eye'] == 'left' else 2
                        assert center[channel] > 240 and corner[channel] > 240, (eye, center, corner)
                        assert center[2 - channel] < 15 and corner[2 - channel] < 15, (eye, center, corner)
                    if expected_target == 'quad_layers':
                        assert center[0] > 240 and center[2] < 15 and max(corner) < 15, (eye, center, corner)
            return metadata
        before = client.tool('debug_status', {'connection': connection})['data']
        preview = inspect(capture({'eye': 'both', 'target': 'current_display', 'format': 'png', 'max_width': 64}), 'current_display', 'both')
        assert preview['resized'] and preview['source_width'] == 128 and preview['source_height'] == 96
        assert preview['display']['source'] == preview['frame']
        after = client.tool('debug_status', {'connection': connection})['data']
        assert before['manual'] == after['manual'] and before['pipeline']['permits'] == after['pipeline']['permits']
        assert not (root / 'recording.4.ready').exists()
        inspect(capture({'eye': 'right', 'target': 'current_display', 'format': 'jpeg', 'max_width': 64}), 'current_display', 'right')
        owner = client.tool('debug_acquire', {'connection': connection})
        assert not owner['error'], owner
        conflict = capture({'target': 'projection_layers'})
        assert conflict['result']['isError'], conflict
        assert not client.tool('debug_relinquish', {'connection': connection, 'lease': owner['data']['lease']})['error']
        time.sleep(.2)
        timeout = capture({'target': 'projection_layers', 'timeout': 0.25})
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
                'params': {'name': 'capture_screenshot', 'arguments': {'application_id': application, 'target': 'quad_layers', 'timeout': 10}}})
            def recording_active():
                status = client.tool('debug_status', {'connection': connection})
                return not status['error'] and status['data']['image_recording']['phase'] == 'recording'
            eventually(recording_active, timeout=5)
            assert request('POST', {'jsonrpc': '2.0', 'method': 'notifications/cancelled', 'params': {'requestId': 17}})[0] == 202
            assert cancelled.result(timeout=5)[0] == 202
            eventually(recording_stopped, timeout=10)
        assert request('DELETE')[0] == 204
        with ThreadPoolExecutor(max_workers=1) as executor:
            projection = executor.submit(capture, {'target': 'projection_layers', 'eye': 'both', 'format': 'png', 'max_width': 64, 'timeout': 10})
            eventually(recording_active, timeout=5)
            time.sleep(7.3)
            assert not projection.done(), 'The connector must preserve a valid long capture request'
            (root / 'recording.3.continue').touch()
            inspect(projection.result(timeout=10), 'projection_layers', 'both', 4)
            eventually(lambda: (root / 'recording.4.ready').exists(), timeout=15)
            time.sleep(.2)
            quad = executor.submit(capture, {'target': 'quad_layers', 'eye': 'right', 'format': 'png', 'max_width': 64, 'timeout': 10})
            eventually(recording_active, timeout=5)
            (root / 'recording.4.continue').touch()
            inspect(quad.result(timeout=10), 'quad_layers', 'right', 5)
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


if __name__ == '__main__':
    main()
