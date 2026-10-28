#!/usr/bin/env python3
"""Exercise native compatibility controls against deliberately gated XR frames."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

from test_mcp_connector import Client, eventually


def main():
    app, connector, probe = (str(Path(value).resolve()) for value in sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix='simxr-mcp-compat-') as temporary:
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
            try:
                client = Client(bridge)
                assert 'result' in client.rpc('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                    'clientInfo': {'name': 'compatibility-test', 'version': '1'}})
                client.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
                names = {item['name'] for item in client.rpc('tools/list')['result']['tools']}
                assert {'set_head_pose', 'set_controller_pose', 'hold_controller_inputs', 'set_ipd', 'set_fov',
                        'set_headset_profile', 'get_headset_profiles'} <= names
                profiles = client.tool('get_headset_profiles', {})
                assert not profiles['error'], profiles
                index = next(item for item in profiles['data']['profiles'] if item['name'] == 'index')
                assert abs(index['ipd_m'] - .063) < .0001 and abs(index['fov_radians'][0][0] + .9424778) < .001, index
                assert index['recommended_width'] == 1440 and index['recommended_height'] == 1600, index
                assert client.tool('get_headset_profiles', {'extra': 1})['error']
                discovery = eventually(lambda: next((p for p in (root / 'discovery').glob('*.json')), None))
                handles.append(os.pidfd_open(int(json.loads(discovery.read_text())['process_id'])))
                launched = client.tool('simulator_launch_application', {'executable': probe, 'environment': {
                    'SIMXR_PROBE_CONTROL': '1', 'SIMXR_PROBE_RECORDING': '1', 'SIMXR_PROBE_INTERACTIVE': '1',
                    'SIMXR_PROBE_FRAMES': '16'}})
                assert not launched['error'], launched
                application = launched['data']['application_id']
                def connected():
                    status = client.tool('simulator_application_status', {'application_id': application})
                    assert not status['error'], status
                    return status['data'] if status['data']['session_running'] else None
                state = eventually(connected, timeout=30)
                handles.append(os.pidfd_open(int(state['process_id'])))
                eventually(lambda: (root / 'control.ready').exists())
                opened = client.tool('debug_connect', {'application_id': application})
                assert not opened['error'], opened
                connection = opened['data']['connection']
                (root / 'control.continue').touch()
                eventually(lambda: (root / 'recording.3.ready').exists())
                for frame in range(3, 7):
                    (root / f'recording.{frame}.continue').touch()
                    eventually(lambda: (root / f'recording.{frame + 1}.ready').exists())
                def edit(tool_name, identity, **values):
                    return client.tool(tool_name, {'connection': connection, 'transaction_id': str(identity), **values})
                def applied(identity):
                    status = client.tool('simulation_status', {'connection': connection, 'transaction_id': str(identity)})
                    assert not status['error'], status
                    return status['data'] if status['data']['phase'] == 'applied' else None
                def advance(frame, transaction):
                    (root / f'recording.{frame}.continue').touch()
                    def next_frame():
                        status = client.tool('simulator_application_status', {'application_id': application})
                        assert not status['error'] and status['data']['launch_state'] != 'exited', status
                        return (root / f'recording.{frame + 1}.ready').exists()
                    eventually(next_frame)
                    eventually(lambda: applied(transaction))
                head = edit('set_head_pose', 101, x=1, y=1.7, z=2, yaw_deg=90)
                assert not head['error'] and head['data']['phase'] == 'queued', head
                duplicate = edit('set_head_pose', 101, x=1, y=1.7, z=2, yaw_deg=90)
                assert not duplicate['error'] and duplicate['data']['phase'] == 'queued' and duplicate['data']['transaction_id'] == '101' and duplicate['data']['replayed_receipt'], duplicate
                assert edit('set_head_pose', 101, x=2, y=1.7, z=2, yaw_deg=90)['error']
                blocked = edit('set_controller_pose', 102, hand=0, posX=0, posY=0, posZ=-1)
                assert blocked['error'] and 'pending' in blocked['data']['error'], blocked
                advance(7, 101)
                duplicate = edit('set_head_pose', 101, x=1, y=1.7, z=2, yaw_deg=90)
                assert not duplicate['error'] and duplicate['data']['phase'] == 'applied' and duplicate['data']['transaction_id'] == '101' and duplicate['data']['replayed_receipt'], duplicate
                pose = edit('set_controller_pose', 102, hand=0, posX=0, posY=0, posZ=-1, trigger=0.4)
                assert not pose['error'] and pose['data']['phase'] == 'queued', pose
                advance(8, 102)
                config = client.tool('simulation_status', {'connection': connection})['data']['document']['configuration']
                left = config['controllers'][0]
                assert abs(left['pose']['position'][0]) < 0.001 and abs(left['pose']['position'][2] - 2) < 0.001, left
                assert abs(left['controls']['trigger'] - 0.4) < 0.001, left
                controls = edit('hold_controller_inputs', 103, hand=0, grip=0.7, primary=1)
                assert not controls['error'], controls
                advance(9, 103)
                config = client.tool('simulation_status', {'connection': connection})['data']['document']['configuration']
                assert abs(config['controllers'][0]['controls']['trigger'] - 0.4) < 0.001
                assert abs(config['controllers'][0]['controls']['grip'] - 0.7) < 0.001 and config['controllers'][0]['controls']['primary']
                ipd = edit('set_ipd', 104, ipd_mm=80)
                assert not ipd['error'], ipd
                advance(10, 104)
                fov = edit('set_fov', 105, left_eye={'left_deg': -50, 'right_deg': 40}, right_eye={'left_deg': -40, 'right_deg': 50})
                assert not fov['error'], fov
                advance(11, 105)
                config = client.tool('simulation_status', {'connection': connection})['data']['document']['configuration']
                assert abs(config['views']['ipd'] - .08) < .0001 and abs(config['views']['outer_fov'][0] + .8726646) < .001
                assert edit('hold_controller_inputs', 106, hand=0, trigger=2)['error']
                cleared = edit('hold_controller_inputs', 107, hand=0, clear=True)
                assert not cleared['error'], cleared
                advance(12, 107)
                config = client.tool('simulation_status', {'connection': connection})['data']['document']['configuration']
                assert config['controllers'][0]['controls']['trigger'] == 0 and not config['controllers'][0]['controls']['primary']
                original_views = config['views']
                assert edit('set_headset_profile', 108, name='not-a-headset')['error']
                profile = edit('set_headset_profile', 108, name='index')
                assert not profile['error'] and profile['data']['phase'] == 'queued', profile
                retry = edit('set_headset_profile', 108, name='index')
                assert not retry['error'] and retry['data']['replayed_receipt'], retry
                assert edit('set_headset_profile', 108, name='quest3')['error']
                advance(13, 108)
                views = client.tool('simulation_status', {'connection': connection})['data']['document']['configuration']['views']
                assert abs(views['ipd'] - .063) < .0001 and abs(views['outer_fov'][0] + .9424778) < .001, views
                for key in ('outer_width', 'outer_height', 'inset_width', 'inset_height', 'inset_half_angle'):
                    assert views[key] == original_views[key], (key, original_views, views)
                generic = edit('set_headset_profile', 109, name='generic')
                assert not generic['error'], generic
                advance(14, 109)
                views = client.tool('simulation_status', {'connection': connection})['data']['document']['configuration']['views']
                assert abs(views['ipd'] - .064) < .0001 and abs(views['outer_fov'][0] + .7853982) < .001, views
                assert not client.tool('simulation_release_control', {'connection': connection})['error']
                assert not client.tool('debug_disconnect', {'connection': connection})['error']
                assert not client.tool('simulator_stop_application', {'application_id': application})['error']
                assert not client.tool('simulator_shutdown', {})['error']
                bridge.stdin.close()
                assert bridge.wait(timeout=15) == 0
                print('PASS native compatibility input transactions, applied-state preservation and pending guard')
            finally:
                if sys.exc_info()[0] is not None:
                    print((root / 'host.log').read_text(errors='replace'), file=sys.stderr)
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
