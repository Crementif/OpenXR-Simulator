#!/usr/bin/env python3
"""Exercise native controls controls against deliberately gated XR frames."""


from pathlib import Path
import sys

from mcp_support import Client, McpTest, eventually


def main():
    app, connector, probe = (str(Path(value).resolve()) for value in sys.argv[1:4])
    with McpTest('simxr-mcp-controls-', windowless=True) as test:
        root = test.root

        log = test.log('host.log')
        bridge = test.bridge(connector, '--app', app, '--windowless', log=log)
        client = Client(bridge)
        client.initialize('controls-test')

        names = {item['name'] for item in client.rpc('tools/list')['result']['tools']}
        assert {'set_head_pose', 'set_controller_pose', 'point_controller', 'set_controller_controls', 'set_views',
                'set_headset_profile', 'get_headset_profiles'} <= names
        profiles = client.tool('get_headset_profiles', {})
        assert not profiles['error'], profiles
        index = next(item for item in profiles['data']['profiles'] if item['name'] == 'index')
        assert abs(index['ipd'] - .063) < .0001 and abs(index['outer_fov'][0] + .9424778) < .001, index
        assert index['recommended_width'] == 1440 and index['recommended_height'] == 1600, index
        assert client.tool('get_headset_profiles', {'extra': 1})['error']
        test.discover()

        launched = client.tool('simulator_launch_application', {'executable': probe, 'environment': {
            'SIMXR_PROBE_CONTROL': '1', 'SIMXR_PROBE_RECORDING': '1', 'SIMXR_PROBE_INTERACTIVE': '1',
            'SIMXR_PROBE_FRAMES': '16'}})
        assert not launched['error'], launched
        application = launched['data']['application_id']

        test.connected(client, application, timeout=30)

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
        head = edit('set_head_pose', 101, position=[1, 1.7, 2], orientation=[0, 2**-.5, 0, 2**-.5])
        assert not head['error'] and head['data']['phase'] == 'queued', head
        duplicate = edit('set_head_pose', 101, position=[1, 1.7, 2], orientation=[0, 2**-.5, 0, 2**-.5])
        assert not duplicate['error'] and duplicate['data']['phase'] == 'queued' and duplicate['data']['transaction_id'] == '101' and duplicate['data']['replayed_receipt'], duplicate
        assert edit('set_head_pose', 101, position=[2, 1.7, 2], orientation=[0, 2**-.5, 0, 2**-.5])['error']
        blocked = edit('set_controller_pose', 102, hand=0, position=[0, 1.7, 2])
        assert blocked['error'] and 'pending' in blocked['data']['error'], blocked
        advance(7, 101)
        duplicate = edit('set_head_pose', 101, position=[1, 1.7, 2], orientation=[0, 2**-.5, 0, 2**-.5])
        assert not duplicate['error'] and duplicate['data']['phase'] == 'applied' and duplicate['data']['transaction_id'] == '101' and duplicate['data']['replayed_receipt'], duplicate
        pose = edit('set_controller_pose', 102, hand=0, position=[0, 1.7, 2])
        assert not pose['error'] and pose['data']['phase'] == 'queued', pose
        advance(8, 102)
        config = client.tool('simulation_status', {'connection': connection})['data']['document']['configuration']
        left = config['controllers'][0]
        assert abs(left['pose']['position'][0]) < 0.001 and abs(left['pose']['position'][2] - 2) < 0.001, left
        controls = edit('set_controller_controls', 103, hand=0, trigger=0.4, grip=0.7, primary=True)
        assert not controls['error'], controls
        advance(9, 103)
        config = client.tool('simulation_status', {'connection': connection})['data']['document']['configuration']
        assert abs(config['controllers'][0]['controls']['trigger'] - 0.4) < 0.001
        assert abs(config['controllers'][0]['controls']['grip'] - 0.7) < 0.001 and config['controllers'][0]['controls']['primary']
        ipd = edit('set_views', 104, ipd=.08)
        assert not ipd['error'], ipd
        advance(10, 104)
        fov = edit('set_views', 105, outer_fov=[-.8726646, .6981317, .7853982, -.7853982, -.6981317, .8726646, .7853982, -.7853982])
        assert not fov['error'], fov
        advance(11, 105)
        config = client.tool('simulation_status', {'connection': connection})['data']['document']['configuration']
        assert abs(config['views']['ipd'] - .08) < .0001 and abs(config['views']['outer_fov'][0] + .8726646) < .001
        assert edit('set_controller_controls', 106, hand=0, trigger=2)['error']
        for tool, values in (
            ('set_head_pose', {'yaw_deg': 90}),
            ('set_controller_pose', {'position': [0, 0, 0]}),
            ('set_controller_pose', {'hand': -1}),
            ('set_controller_pose', {'hand': 0, 'posX': 1}),
            ('set_controller_controls', {'hand': 0, 'primary': 1}),
            ('set_controller_controls', {'hand': 0, 'thumbClick': True}),
            ('set_controller_controls', {'hand': 0, 'clear': True}),
            ('set_views', {'ipd_mm': 64}),
            ('set_views', {'outer_fov': [0, 0, 0, 0]}),
        ):
            assert edit(tool, 106, **values)['error'], (tool, values)
        for retired in ('hold_controller_inputs', 'set_ipd', 'set_fov', 'get_head_tracking'):
            assert retired not in names, retired
        assert client.tool('debug_connect', {})['error']
        assert client.tool('get_head_pose', {})['error']
        assert client.tool('set_controller_pose', {'connection': connection, 'transaction_id': '106',
                           'hand': 0, 'session': '1'})['error']
        cleared = edit('set_controller_controls', 107, hand=0)
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
        print('PASS native controls input transactions, applied-state preservation and pending guard')


if __name__ == '__main__':
    main()
