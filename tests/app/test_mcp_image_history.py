#!/usr/bin/env python3
"""Verify native MCP exact historical images and bounded collection windows."""

import base64
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

from test_mcp_connector import Client, eventually


def exercise(app, connector, probe, target):
    with tempfile.TemporaryDirectory(prefix='simxr-mcp-image-history-') as temporary:
        root = Path(temporary)
        env = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_MCP_DISCOVERY_DIR=str(root / 'discovery'),
                   SIMXR_COMPANION_SETTINGS=str(root / 'settings'), SIMXR_DETERMINISTIC='0',
                   SDL_VIDEODRIVER='simxr-unavailable-video-driver')
        for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'XR_RUNTIME_JSON', 'SIMXR_CONTROL_SOCKET'):
            env.pop(key, None)
        handles = []
        second = None
        with (root / 'host.log').open('wb') as log:
            bridge = subprocess.Popen([connector, '--app', app, '--windowless'], env=env, stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1, start_new_session=True)
            try:
                client = Client(bridge)
                assert 'result' in client.rpc('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                    'clientInfo': {'name': 'image-history-test', 'version': '1'}})
                client.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
                names = {entry['name'] for entry in client.rpc('tools/list')['result']['tools']}
                assert {'debug_image_history', 'debug_retain_history', 'debug_begin_image_window',
                        'debug_image_window', 'debug_retain_window_image', 'debug_release_image_window'} <= names
                discovery = eventually(lambda: next((p for p in (root / 'discovery').glob('*.json')), None))
                handles.append(os.pidfd_open(int(json.loads(discovery.read_text())['process_id'])))
                launched = client.tool('simulator_launch_application', {'executable': probe, 'environment': {
                    'SIMXR_PROBE_CONTROL': '1', 'SIMXR_PROBE_GPU_INSPECTION': '1', 'SIMXR_PROBE_RECORDING': '1',
                    'SIMXR_PROBE_DYNAMIC_COLOR': '1', 'SIMXR_PROBE_MOVING_QUAD': '1', 'SIMXR_PROBE_FRAMES': '13',
                    'SIMXR_WIDTH': '64', 'SIMXR_HEIGHT': '48'}})
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
                second = subprocess.Popen([connector, '--app', app, '--windowless'], env=env, stdin=subprocess.PIPE,
                                          stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1, start_new_session=True)
                other = Client(second)
                assert 'result' in other.rpc('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                    'clientInfo': {'name': 'image-history-other', 'version': '1'}})
                other.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
                other_connection = other.tool('debug_connect', {'application_id': application})['data']['connection']
                assert client.tool('debug_image_history', {'connection': connection, 'count': 11})['error']
                assert client.tool('debug_image_history', {'connection': connection, 'target': 'projection_layers'})['error']
                (root / 'control.continue').touch()
                window = None
                lease = None
                for frame in range(3, 16):
                    eventually(lambda: (root / f'recording.{frame}.ready').exists(), timeout=15)
                    if frame == 3:
                        history = client.tool('debug_image_history', {'connection': connection, 'target': target, 'count': 1})
                        assert not history['error'] and history['data']['document']['target'] == target, history
                        started = client.tool('debug_begin_image_window', {'connection': connection, 'target': target})
                        assert not started['error'], started
                        window = started['data']['window']
                        assert started['data']['document']['trigger_frame'] == '3', started
                        assert client.tool('debug_begin_image_window', {'connection': connection, 'target': target})['error']
                        assert other.tool('debug_image_window', {'connection': other_connection, 'window': window})['error']
                        if target == 'composed_eyes':
                            def current_preview():
                                result = client.tool('debug_retain_preview', {'connection': connection})
                                return result['data'] if not result['error'] and result['data']['image_available'] else None
                            preview = eventually(current_preview)
                            assert preview['display']['source'] == preview['frame'], preview
                            assert preview['display']['presentation_policy'] in ('repeat', 'rotational'), preview
                            assert int(preview['display']['skipped_ticks']) >= 0
                            assert 'reprojection_result' in preview['display'], preview
                            preview_image = preview['image']
                            exact = client.tool('debug_retain_preview', {'connection': connection,
                                                'frame': preview['frame']['frame_id']})
                            assert not exact['error'] and exact['data']['frame'] == preview['frame'], exact
                            assert not client.tool('debug_release_image', {'connection': connection, 'image': exact['data']['image']})['error']
                            assert not client.tool('debug_release_image', {'connection': connection, 'image': preview_image})['error']
                            acquired = client.tool('debug_acquire', {'connection': connection})
                            assert not acquired['error'], acquired
                            lease = acquired['data']['lease']
                            recording = {'connection': connection, 'lease': lease, 'command_id': '1',
                                         'target': 'projection_layers', 'capacity': 2, 'frames': '1'}
                            assert client.tool('debug_start_image_recording', dict(recording, capacity=0))['error']
                            started = client.tool('debug_start_image_recording', recording)
                            duplicate = client.tool('debug_start_image_recording', recording)
                            assert not started['error'] and not duplicate['error'] and duplicate['data']['duplicate'], (started, duplicate)
                            assert duplicate['data']['operation_id'] == started['data']['operation_id']
                            def wait_operation(identity):
                                result = client.tool('debug_operation', {'connection': connection, 'operation': identity})
                                assert not result['error'], result
                                return result['data'] if result['data']['phase'] not in ('queued', 'pending') else None
                            assert eventually(lambda: wait_operation(started['data']['operation_id']))['progress'] == 'recording_started'
                    if frame == 4 and lease:
                        status = client.tool('debug_status', {'connection': connection})
                        stream = status['data']['image_recording']
                        assert not status['error'] and stream['phase'] == 'stopped' and stream['captured'] == '1', status
                        assert stream['requested'] == '1' and len(stream['captures']) == 1, stream
                        capture = stream['captures'][0]
                        def capture_ready():
                            result = client.tool('debug_capture', {'connection': connection, 'capture': capture})
                            return result['data'] if not result['error'] and result['data'].get('phase') == 'ready' else None
                        captured = eventually(capture_ready)
                        assert captured['frame']['frame_id'] == '4', captured
                        for command_id, name, expected in ((2, 'debug_pin_snapshot', True), (3, 'debug_unpin_snapshot', False)):
                            result = client.tool(name, {'connection': connection, 'lease': lease,
                                                        'command_id': str(command_id), 'capture': capture})
                            assert not result['error'], result
                            assert eventually(lambda: wait_operation(result['data']['operation_id']))['progress'] == 'snapshot_ready'
                            metadata = client.tool('debug_capture', {'connection': connection, 'capture': capture})
                            assert not metadata['error'] and metadata['data']['pinned'] == expected, metadata
                        configured = client.tool('debug_configure_snapshots', {'connection': connection, 'lease': lease,
                                                 'command_id': '4', 'budget_mib': 128})
                        assert not configured['error'], configured
                        assert eventually(lambda: wait_operation(configured['data']['operation_id']))['progress'] == 'snapshot_ready'
                        assert client.tool('debug_status', {'connection': connection})['data']['snapshot_budget_mib'] == 128
                        stopped = client.tool('debug_stop_image_recording', {'connection': connection, 'lease': lease, 'command_id': '5'})
                        assert not stopped['error'] and eventually(lambda: wait_operation(stopped['data']['operation_id']))['progress'] == 'recording_stopped'
                        released = client.tool('debug_release', {'connection': connection, 'lease': lease,
                                               'command_id': '6', 'capture': capture})
                        assert not released['error'] and eventually(lambda: wait_operation(released['data']['operation_id']))['progress'] == 'released'
                        unbounded = client.tool('debug_start_image_recording', {'connection': connection, 'lease': lease,
                                                'command_id': '7', 'capacity': 1, 'frames': '0'})
                        assert not unbounded['error'] and eventually(lambda: wait_operation(unbounded['data']['operation_id']))['progress'] == 'recording_started'
                    if frame == 5 and lease:
                        status = client.tool('debug_status', {'connection': connection})
                        stream = status['data']['image_recording']
                        assert stream['phase'] == 'recording' and stream['captured'] == '1' and stream['requested'] == '0', stream
                        overrun_capture, = stream['captures']
                    if frame == 6 and lease:
                        status = client.tool('debug_status', {'connection': connection})
                        stream = status['data']['image_recording']
                        assert stream['phase'] == 'overrun' and stream['missed_frame']['frame_id'] == '6', stream
                        stopped = client.tool('debug_stop_image_recording', {'connection': connection, 'lease': lease, 'command_id': '8'})
                        assert not stopped['error'] and eventually(lambda: wait_operation(stopped['data']['operation_id']))['progress'] == 'recording_stopped'
                        released = client.tool('debug_release', {'connection': connection, 'lease': lease,
                                               'command_id': '9', 'capture': overrun_capture})
                        assert not released['error'] and eventually(lambda: wait_operation(released['data']['operation_id']))['progress'] == 'released'
                        assert not client.tool('debug_relinquish', {'connection': connection, 'lease': lease})['error']
                        lease = None
                    if frame == 15:
                        projections = client.tool('debug_projection_history', {'connection': connection, 'count': 1})
                        assert not projections['error'], projections
                        projection = projections['data']['document']
                        assert projection['generation'] == state['generation'] and projection['entries'][-1]['frame_id'] == '15', projection
                        assert projection['entries'][-1]['ipd'] > 0, projection
                        high_level = client.tool('get_projection_log', {'application_id': application, 'max_entries': 1})
                        assert not high_level['error'], high_level
                        assert high_level['data']['projection']['entries'][-1] == projection['entries'][-1], high_level
                        assert high_level['data']['generation'] == state['generation'], high_level
                        assert client.tool('get_projection_log', {'application_id': application, 'max_entries': 65})['error']
                        assert client.tool('debug_projection_history', {'connection': connection, 'count': 65})['error']
                        observed = client.tool('debug_image_window', {'connection': connection, 'window': window})
                        assert not observed['error'], observed
                        document = observed['data']['document']
                        assert document['phase'] == 'complete' and document['post_count'] == 12, document
                        assert document['target'] == target and document['entries'][-1]['frame_id'] == '15', document
                        retained = client.tool('debug_retain_window_image', {'connection': connection, 'window': window, 'frame': '3'})
                        assert not retained['error'], retained
                        image = retained['data']['image']
                        assert other.tool('debug_image_info', {'connection': other_connection, 'image': image})['error']
                        released = client.tool('debug_release_image_window', {'connection': connection, 'window': window})
                        assert not released['error'] and released['data']['released'], released
                        assert client.tool('debug_image_window', {'connection': connection, 'window': window})['error']
                        released_again = client.tool('debug_release_image_window', {'connection': connection, 'window': window})
                        assert not released_again['error'] and not released_again['data']['released'], released_again
                        assert not client.tool('debug_image_info', {'connection': connection, 'image': image})['error']
                        exported = client.tool('debug_start_export', {'connection': connection, 'image': image,
                            'layer': 1 if target == 'quad_layers' else 0})
                        assert not exported['error'], exported
                        ticket = exported['data']['export']
                        def export_ready():
                            result = client.tool('debug_export_info', {'connection': connection, 'export': ticket})
                            assert not result['error'] and result['data']['phase'] != 'failed', result
                            return result['data'] if result['data']['phase'] == 'complete' else None
                        info = eventually(export_ready)
                        data = client.tool('debug_read_export', {'connection': connection, 'export': ticket,
                            'length': int(info['total_bytes'])})
                        assert not data['error'], data
                        pixels = base64.b64decode(data['data']['bytes'])
                        assert len(pixels) == 64 * 48 * 4
                        colors = {pixels[index:index+3] for index in range(0, len(pixels), 4)}
                        assert colors == ({b'\xff\0\0', b'\0\0\0'} if target == 'quad_layers' else {b'\xff\0\0'}), colors
                        assert not client.tool('debug_release_export', {'connection': connection, 'export': ticket})['error']
                        assert not client.tool('debug_release_image', {'connection': connection, 'image': image})['error']
                        retained = client.tool('debug_retain_history', {'connection': connection, 'target': target, 'frame': '15'})
                        assert not retained['error'] and retained['data']['frame']['frame_id'] == '15', retained
                        assert not client.tool('debug_release_image', {'connection': connection, 'image': retained['data']['image']})['error']
                        fresh = client.tool('debug_begin_image_window', {'connection': connection, 'target': target})
                        assert not fresh['error'] and fresh['data']['document']['phase'] == 'collecting', fresh
                        assert not client.tool('debug_release_image_window', {'connection': connection,
                                                'window': fresh['data']['window']})['error']
                    (root / f'recording.{frame}.continue').touch()
                def exited():
                    result = client.tool('simulator_application_status', {'application_id': application})
                    return result['data'] if not result['error'] and result['data']['launch_state'] == 'exited' else None
                assert eventually(exited, timeout=15)['exit_code'] == 0
                assert not client.tool('simulator_shutdown', {})['error']
                bridge.stdin.close()
                assert bridge.wait(timeout=10) == 0
                print(f'PASS native MCP {target} image history and twelve-frame window')
            finally:
                if second and second.poll() is None:
                    second.kill()
                    second.wait()
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


def main():
    app, connector, probe = (str(Path(value).resolve()) for value in sys.argv[1:4])
    for target in ('composed_eyes', 'quad_layers'):
        exercise(app, connector, probe, target)


if __name__ == '__main__':
    main()
