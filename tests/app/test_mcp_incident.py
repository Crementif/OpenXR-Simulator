#!/usr/bin/env python3
"""Exercise native MCP pre/post contact sheets against exact retained XR frames."""
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


def exercise(app, connector, probe, ui):
    with McpTest('simxr-mcp-incident-', windowless=True) as test:
        root = test.root

        worker_bridge = None
        log = test.log('host.log')
        bridge = test.bridge(connector, '--app', app, '--windowless', log=log)
        client = Client(bridge)
        client.initialize('incident-control')

        names = {item['name'] for item in client.rpc('tools/list')['result']['tools']}
        tool = 'capture_ui_flicker_window' if ui else 'capture_flicker_window'
        assert tool in names
        incident_dir = root / 'evidence ü' / 'incident'
        incident_dir.parent.mkdir()
        invalid = client.tool(tool, {'application_id': 'none', 'directory': str(incident_dir.parent)})
        assert invalid['error'] and 'already exists' in invalid['data']['error'], invalid
        record_path = eventually(lambda: next((p for p in (root / 'discovery').glob('*.json')), None))
        record = json.loads(record_path.read_text())
        test.track(int(record['process_id']))
        launched = client.tool('simulator_launch_application', {'executable': probe, 'environment': {
            'SIMXR_PROBE_CONTROL': '1', 'SIMXR_PROBE_GPU_INSPECTION': '1', 'SIMXR_PROBE_RECORDING': '1',
            'SIMXR_PROBE_DYNAMIC_COLOR': '1', 'SIMXR_PROBE_MOVING_QUAD': '1', 'SIMXR_PROBE_CLEAR_FRAME': '18',
            'SIMXR_PROBE_FRAMES': '26', 'SIMXR_WIDTH': '64', 'SIMXR_HEIGHT': '48'}})
        assert not launched['error'], launched
        application = launched['data']['application_id']

        state = test.connected(client, application, timeout=30)

        eventually(lambda: (root / 'control.ready').exists(), timeout=15)
        opened = client.tool('debug_connect', {'application_id': application})
        assert not opened['error'], opened
        connection = opened['data']['connection']
        worker_bridge = test.bridge(connector, '--app', app, '--windowless', log=log)
        worker = Client(worker_bridge)
        worker.initialize('incident-worker')

        (root / 'control.continue').touch()
        target = 'quad_layers' if ui else 'composed_eyes'
        with ThreadPoolExecutor(max_workers=1) as executor:
            capture = None
            for frame in range(3, 25):
                eventually(lambda frame=frame: (root / f'recording.{frame}.ready').exists(), timeout=15)
                if frame == 12:
                    capture = executor.submit(worker.rpc, 'tools/call', {'name': tool,
                        'arguments': {'application_id': application, 'timeout': 30, 'max_frames': 12,
                                      'directory': str(incident_dir), 'analyze': True}})
                    time.sleep(.5)
                    assert not capture.done(), 'Incident must wait for following frames'
                    busy = client.tool('debug_begin_image_window', {'connection': connection, 'target': target})
                    assert busy['error'], 'Incident did not acquire the shared image window'
                if frame == 24:
                    response = capture.result(timeout=35)['result']
                    assert not response['isError'], response
                    content = response['content']
                    assert len(content) == 2 and content[1]['type'] == 'image' and content[1]['mimeType'] == 'image/jpeg'
                    report = json.loads(content[0]['text'])
                    window = report['window']
                    assert report['generation'] == state['generation'] and report['target'] == target
                    assert window['trigger_frame'] == '12' and window['pre_count'] == 10 and window['post_count'] == 12, report
                    assert len(window['entries']) == 22 and len(report['selected_frames']) == 12
                    assert len(report['saved_frames']) == 22 and report['directory'] == str(incident_dir), report
                    analysis = report['analysis']
                    assert analysis['mode'] == 'on_demand_window' and analysis['frames_analyzed'] + analysis['frames_skipped'] == 22, analysis
                    assert all(event['frame_id'] in {entry['frame_id'] for entry in window['entries']} for event in analysis['events'])
                    if not ui:
                        assert analysis['frames_analyzed'] == 22 and analysis['detected'], analysis
                    assert json.loads((incident_dir / 'manifest.json').read_text()) == report
                    assert (incident_dir / 'contact.jpg').read_bytes() == base64.b64decode(content[1]['data'], validate=True)
                    for entry, saved in zip(window['entries'], report['saved_frames']):
                        assert entry['frame_id'] == saved['frame_id'] and entry['phase'] == saved['phase']
                        with Image.open(incident_dir / saved['file']) as image:
                            assert image.format == 'PNG' and image.size == (320, 104)
                            sample_x, sample_y = (80, 59) if ui else (64, 51)
                            left = image.getpixel((sample_x, sample_y))[:3]
                            right = image.getpixel((sample_x + 160, sample_y))[:3]
                            frame_id = int(saved['frame_id'])
                            if saved['phase'] == 'clear':
                                assert frame_id == 18 and max(left + right) == 0, (saved, left, right)
                            elif ui:
                                expected = 0 if frame_id % 2 else 1
                                assert max(left) < 35 and right[expected] > 180 and right[1-expected] < 60, (saved, left, right)
                            else:
                                expected = 0 if frame_id % 2 else 1
                                assert left[expected] > 180 and left[1-expected] < 60 and right[2] > 180, (saved, left, right)
                    with Image.open(BytesIO(base64.b64decode(content[1]['data'], validate=True))) as decoded:
                        sheet = decoded.convert('RGB')
                        assert sheet.size == (640, 624), sheet.size
                        for row in report['selected_frames']:
                            index = row['cell']; cell_x = index % 2 * 320; cell_y = index // 2 * 104
                            frame_id = int(row['frame_id'])
                            sample_x, sample_y = (80, 59) if ui else (64, 51)
                            left = sheet.getpixel((cell_x + sample_x, cell_y + sample_y))
                            right = sheet.getpixel((cell_x + sample_x + 160, cell_y + sample_y))
                            if row['phase'] == 'clear':
                                assert frame_id == 18 and max(left + right) < 20, (row, left, right)
                            elif ui:
                                expected = 0 if frame_id % 2 else 1
                                assert max(left) < 35 and right[expected] > 180 and right[1-expected] < 60, (row, left, right)
                            else:
                                expected = 0 if frame_id % 2 else 1
                                assert left[expected] > 180 and left[1-expected] < 60 and right[2] > 180, (row, left, right)
                    timeout = worker.rpc('tools/call', {'name': tool, 'arguments': {'application_id': application, 'timeout': 0.2}})
                    assert timeout['result']['isError'] and 'timed out' in timeout['result']['content'][0]['text'], timeout
                    replacement = client.tool('debug_begin_image_window', {'connection': connection, 'target': target})
                    assert not replacement['error'], replacement
                    assert not client.tool('debug_release_image_window', {'connection': connection, 'window': replacement['data']['window']})['error']
                    headers = {'Authorization': 'Bearer ' + record['token'], 'Accept': 'application/json, text/event-stream',
                               'Content-Type': 'application/json'}
                    def request(method, message=None):
                        transport = http.client.HTTPConnection('127.0.0.1', record['port'], timeout=10)
                        try:
                            transport.request(method, '/mcp', None if message is None else json.dumps(message), headers)
                            result = transport.getresponse()
                            return result.status, result.read(), result.getheader('MCP-Session-Id')
                        finally:
                            transport.close()
                    status, _, session = request('POST', {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize',
                        'params': {'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'incident-cancel', 'version': '1'}}})
                    assert status == 200 and session
                    headers['MCP-Session-Id'] = session
                    assert request('POST', {'jsonrpc': '2.0', 'method': 'notifications/initialized'})[0] == 202
                    cancel = executor.submit(request, 'POST', {'jsonrpc': '2.0', 'id': 17, 'method': 'tools/call',
                        'params': {'name': tool, 'arguments': {'application_id': application, 'timeout': 15}}})
                    time.sleep(.5)
                    assert not cancel.done()
                    assert client.tool('debug_begin_image_window', {'connection': connection, 'target': target})['error']
                    assert request('POST', {'jsonrpc': '2.0', 'method': 'notifications/cancelled', 'params': {'requestId': 17}})[0] == 202
                    assert cancel.result(timeout=5)[0] == 202
                    replacement = client.tool('debug_begin_image_window', {'connection': connection, 'target': target})
                    assert not replacement['error'], replacement
                    assert not client.tool('debug_release_image_window', {'connection': connection, 'window': replacement['data']['window']})['error']
                    assert request('DELETE')[0] == 204
                (root / f'recording.{frame}.continue').touch()
        for frame in range(25, 29):
            eventually(lambda frame=frame: (root / f'recording.{frame}.ready').exists(), timeout=15)
            (root / f'recording.{frame}.continue').touch()
        def exited():
            status = client.tool('simulator_application_status', {'application_id': application})
            return status['data'] if not status['error'] and status['data']['launch_state'] == 'exited' else None
        assert eventually(exited, timeout=15)['exit_code'] == 0
        assert not client.tool('simulator_shutdown', {})['error']
        bridge.stdin.close()
        assert bridge.wait(timeout=10) == 0


if __name__ == '__main__':
    app, connector, probe = (str(Path(value).resolve()) for value in sys.argv[1:4])
    exercise(app, connector, probe, False)
    exercise(app, connector, probe, True)
    print('PASS native MCP composed/UI incident sheets, exact frame colors, clears and timeout cleanup')
