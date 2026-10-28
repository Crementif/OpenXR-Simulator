#!/usr/bin/env python3
"""Exercise native stereo analysis through a windowless host and real XR process."""

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

from test_mcp_connector import Client, eventually


def main():
    app, connector, probe = (str(Path(value).resolve()) for value in sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix='simxr-stereo-mcp-') as temporary:
        root = Path(temporary)
        env = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_MCP_DISCOVERY_DIR=str(root / 'discovery'),
                   SIMXR_COMPANION_SETTINGS=str(root / 'settings'), SIMXR_DETERMINISTIC='0',
                   SDL_VIDEODRIVER='simxr-unavailable-video-driver')
        for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'XR_RUNTIME_JSON', 'SIMXR_CONTROL_SOCKET'):
            env.pop(key, None)
        handles = []
        with (root / 'bridge.log').open('wb') as log:
            bridge = subprocess.Popen([connector, '--app', app, '--windowless'], env=env, stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1, start_new_session=True)
            other_bridge = None
            try:
                client = Client(bridge)
                assert 'result' in client.rpc('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                    'clientInfo': {'name': 'stereo-test', 'version': '1'}})
                client.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
                names = {item['name'] for item in client.rpc('tools/list')['result']['tools']}
                assert 'analyze_stereo_consistency' in names
                discovery = eventually(lambda: next((p for p in (root / 'discovery').glob('*.json')), None))
                handles.append(os.pidfd_open(int(json.loads(discovery.read_text())['process_id'])))
                launched = client.tool('simulator_launch_application', {'executable': probe, 'environment': {
                    'SIMXR_PROBE_INTERACTIVE': '1', 'SIMXR_PROBE_FRAMES': '120000',
                    'SIMXR_WIDTH': '128', 'SIMXR_HEIGHT': '96'}})
                assert not launched['error'], launched
                application = launched['data']['application_id']

                def connected():
                    status = client.tool('simulator_application_status', {'application_id': application})
                    assert not status['error'] and status['data']['launch_state'] != 'failed', status
                    return status['data'] if status['data']['session_running'] else None

                state = eventually(connected, timeout=30)
                handles.append(os.pidfd_open(int(state['process_id'])))
                assert client.tool('analyze_stereo_consistency', {'application_id': application, 'threshold': 0})['error']
                assert client.tool('analyze_stereo_consistency', {'application_id': application, 'frames': 0})['error']
                result = client.tool('analyze_stereo_consistency', {'application_id': application,
                    'frames': 2, 'interval': .05, 'timeout': 15, 'include_image': False})
                assert not result['error'], result
                data = result['data']
                assert not data['incomplete'] and len(data['samples']) == 2, data
                for sample in data['samples']:
                    assert sample['ok'] and sample['frame']['generation'] == state['generation'], sample
                    assert sample['analysis_width'] == 128 and sample['analysis_height'] == 96, sample
                    assert sample['verdict'].startswith('INCONCLUSIVE_'), sample
                    assert sample['frusta_source'] == 'captured_image_views' and len(sample['image_views']) == 2, sample
                    assert sample['infinity_disparity_pct'] is not None or sample['verdict'] == 'INCONCLUSIVE_NO_FEATURES'
                other_bridge = subprocess.Popen([connector, '--app', app, '--windowless'], env=env,
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1, start_new_session=True)
                worker = Client(other_bridge)
                assert 'result' in worker.rpc('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                    'clientInfo': {'name': 'stereo-cancel', 'version': '1'}})
                worker.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
                worker.send({'jsonrpc': '2.0', 'id': 901, 'method': 'tools/call', 'params': {
                    'name': 'analyze_stereo_consistency', 'arguments': {'application_id': application,
                        'frames': 30, 'interval': 1, 'timeout': 5, 'include_image': False}}})
                time.sleep(.5)
                worker.send({'jsonrpc': '2.0', 'method': 'notifications/cancelled', 'params': {'requestId': 901}})

                def available():
                    result = client.tool('analyze_stereo_consistency', {'application_id': application,
                        'timeout': 10, 'include_image': False})
                    if result['error'] and ('busy' in result['data']['error'] or 'lease' in result['data']['error']):
                        return None
                    assert not result['error'], result
                    return result

                eventually(available, timeout=20)
                other_bridge.stdin.close()
                assert other_bridge.wait(timeout=15) == 0

                assert not client.tool('simulator_stop_application', {'application_id': application})['error']
                assert not client.tool('simulator_shutdown', {})['error']
                bridge.stdin.close()
                assert bridge.wait(timeout=15) == 0
                print('PASS native MCP stereo sampling, exact image views and explicit inconclusive result')
            finally:
                if sys.exc_info()[0] is not None:
                    print((root / 'bridge.log').read_text(errors='replace'), file=sys.stderr)
                if other_bridge and other_bridge.poll() is None:
                    os.killpg(other_bridge.pid, signal.SIGTERM)
                    other_bridge.wait(timeout=10)
                if bridge.poll() is None:
                    os.killpg(bridge.pid, signal.SIGTERM)
                    bridge.wait(timeout=10)
                for handle in handles:
                    try:
                        signal.pidfd_send_signal(handle, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    os.close(handle)


if __name__ == '__main__':
    main()
