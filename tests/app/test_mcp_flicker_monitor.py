#!/usr/bin/env python3
"""Exercise explicit sampled flicker monitoring with a real XR application."""

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
    with tempfile.TemporaryDirectory(prefix='simxr-flicker-monitor-') as temporary:
        root = Path(temporary)
        env = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_MCP_DISCOVERY_DIR=str(root / 'discovery'),
                   SIMXR_COMPANION_SETTINGS=str(root / 'settings'), SIMXR_DETERMINISTIC='0',
                   SDL_VIDEODRIVER='simxr-unavailable-video-driver')
        for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'XR_RUNTIME_JSON', 'SIMXR_CONTROL_SOCKET'):
            env.pop(key, None)
        handles = []
        bridges = []
        with (root / 'host.log').open('wb') as log:
            try:
                def connect(name):
                    bridge = subprocess.Popen([connector, '--app', app, '--windowless'], env=env,
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1,
                        start_new_session=True)
                    bridges.append(bridge)
                    client = Client(bridge)
                    assert 'result' in client.rpc('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                        'clientInfo': {'name': name, 'version': '1'}})
                    client.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
                    return client

                client = connect('monitor-owner')
                names = {item['name'] for item in client.rpc('tools/list')['result']['tools']}
                assert {'start_flicker_monitor', 'get_flicker_status', 'get_ui_flicker_status', 'stop_flicker_monitor'} <= names
                discovery = eventually(lambda: next((p for p in (root / 'discovery').glob('*.json')), None))
                handles.append(os.pidfd_open(int(json.loads(discovery.read_text())['process_id'])))
                launched = client.tool('simulator_launch_application', {'executable': probe, 'environment': {
                    'SIMXR_PROBE_INTERACTIVE': '1', 'SIMXR_PROBE_DYNAMIC_COLOR': '1',
                    'SIMXR_PROBE_FRAMES': '120000', 'SIMXR_WIDTH': '128', 'SIMXR_HEIGHT': '96'}})
                assert not launched['error'], launched
                application = launched['data']['application_id']

                def connected():
                    status = client.tool('simulator_application_status', {'application_id': application})
                    assert not status['error'] and status['data']['launch_state'] != 'failed', status
                    return status['data'] if status['data']['session_running'] else None

                state = eventually(connected, timeout=30)
                handles.append(os.pidfd_open(int(state['process_id'])))
                time.sleep(.5)
                options = {'monitor_id': 'first', 'application_id': application, 'target': 'composed',
                           'timeout': 25, 'interval': .25}
                started = client.tool('start_flicker_monitor', options)
                assert not started['error'] and started['data']['phase'] == 'running', started
                replay = client.tool('start_flicker_monitor', options)
                assert not replay['error'] and replay['data']['monitor_id'] == 'first', replay
                conflict = client.tool('start_flicker_monitor', dict(options, target='ui'))
                assert conflict['error'], conflict
                other = connect('monitor-other')
                assert other.tool('get_flicker_status', {})['data']['phase'] == 'not_started'
                assert other.tool('stop_flicker_monitor', {'monitor_id': 'first'})['error']

                def analyzed():
                    status = client.tool('get_flicker_status', {})
                    assert not status['error'] and status['data']['phase'] != 'failed', status
                    return status['data'] if status['data']['windows'] else None

                result = eventually(analyzed, timeout=50)
                assert result['generation'] == state['generation'] and result['frames_observed'] >= 2, result
                assert result['last_analysis']['frames_analyzed'] >= 2 and result['events'], result
                stopped = client.tool('stop_flicker_monitor', {'monitor_id': 'first'})
                assert not stopped['error'] and stopped['data']['phase'] == 'stopped', stopped
                assert not client.tool('stop_flicker_monitor', {'monitor_id': 'first'})['error']
                assert not client.tool('start_flicker_monitor', options)['error']  # Identical retry stays stopped.
                assert client.tool('get_ui_flicker_status', {})['data']['phase'] == 'not_started'

                second = dict(options, monitor_id='second', target='ui')
                assert not client.tool('start_flicker_monitor', second)['error']
                assert client.tool('start_flicker_monitor', options)['error']  # Retired ID cannot restart.
                time.sleep(.3)
                stopped = client.tool('stop_flicker_monitor', {'monitor_id': 'second'})
                assert not stopped['error'] and stopped['data']['phase'] == 'stopped', stopped

                assert not other.tool('start_flicker_monitor', dict(options, monitor_id='other'))['error']
                time.sleep(.3)
                bridges[1].stdin.close()
                assert bridges[1].wait(timeout=15) == 0
                assert not client.tool('start_flicker_monitor', dict(options, monitor_id='third'))['error']
                result = eventually(analyzed, timeout=50)
                assert result['monitor_id'] == 'third', result
                assert not client.tool('stop_flicker_monitor', {'monitor_id': 'third'})['error']
                assert not client.tool('simulator_stop_application', {'application_id': application})['error']
                assert not client.tool('simulator_shutdown', {})['error']
                for bridge in bridges:
                    if not bridge.stdin.closed:
                        bridge.stdin.close()
                    assert bridge.wait(timeout=15) == 0
                print('PASS sampled flicker monitor detection, retry identity, isolation and stop')
            finally:
                if sys.exc_info()[0] is not None:
                    print((root / 'host.log').read_text(errors='replace'), file=sys.stderr)
                for bridge in bridges:
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
