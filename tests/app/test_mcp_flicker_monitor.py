#!/usr/bin/env python3
"""Exercise explicit sampled flicker monitoring with a real XR application."""


from pathlib import Path
import os
import sys
import time

from mcp_support import McpTest, eventually


def main():
    app, connector, probe = (str(Path(value).resolve()) for value in sys.argv[1:4])
    launch = {'executable': probe, 'environment': {
        'SIMXR_PROBE_INTERACTIVE': '1', 'SIMXR_PROBE_DYNAMIC_COLOR': '1',
        'SIMXR_PROBE_FRAMES': '120000', 'SIMXR_WIDTH': '128', 'SIMXR_HEIGHT': '96'}}
    if len(sys.argv) > 4:
        launch['runtime_manifest'] = str(Path(sys.argv[4]).resolve())
        launch['environment'].update({
            'SIMXR_CONTROL_SOCKET': rf'\\.\pipe\SimXR.Control.monitor.{os.getpid()}',
            'SIMXR_PROBE_SLEEP_MS': '1', 'SIMXR_PROBE_PROJECTION_ONLY': '1',
            'DISABLE_XR_APILAYER_NOVENDOR_OBSMirror': '1',
            'DISABLE_XR_APILAYER_VIRTUALDESKTOP_OCULUS_COMPATIBILITY': '1'})
    with McpTest('simxr-flicker-monitor-', windowless=True) as test:
        root = test.root

        log = test.log('host.log')

        client = test.connect(connector, '--app', app, '--windowless', log=log, name='monitor-owner')
        names = {item['name'] for item in client.rpc('tools/list')['result']['tools']}
        assert {'start_flicker_monitor', 'get_flicker_status', 'get_ui_flicker_status', 'stop_flicker_monitor'} <= names
        test.discover()

        launched = client.tool('simulator_launch_application', launch)
        assert not launched['error'], launched
        application = launched['data']['application_id']

        state = test.connected(client, application, timeout=30)

        time.sleep(.5)
        options = {'monitor_id': 'first', 'application_id': application, 'target': 'composed',
                   'timeout': 25, 'interval': .25}
        started = client.tool('start_flicker_monitor', options)
        assert not started['error'] and started['data']['phase'] == 'running', started
        replay = client.tool('start_flicker_monitor', options)
        assert not replay['error'] and replay['data']['monitor_id'] == 'first', replay
        conflict = client.tool('start_flicker_monitor', dict(options, target='ui'))
        assert conflict['error'], conflict
        other = test.connect(connector, '--app', app, '--windowless', log=log, name='monitor-other')
        assert other.tool('get_flicker_status', {'application_id': application})['data']['monitor_id'] == 'first'
        assert client.tool('get_flicker_status', {})['error']
        assert other.tool('stop_flicker_monitor', {'application_id': application, 'monitor_id': 'unknown'})['error']

        def analyzed():
            status = client.tool('get_flicker_status', {'application_id': application})
            assert not status['error'] and status['data']['phase'] != 'failed', status
            return status['data'] if status['data']['windows'] else None

        result = eventually(lambda: (value if (value := analyzed()) and value['events'] else None), timeout=50)
        assert result['generation'] == state['generation'] and result['frames_observed'] >= 2, result
        assert result['last_analysis']['frames_analyzed'] >= 2 and result['events'], result
        incident = client.tool('capture_flicker_window', {'application_id': application, 'timeout': 25,
                               'max_frames': 2, 'include_image': False, 'analyze': True})
        assert not incident['error'] and incident['data']['analysis']['frames_analyzed'] >= 2, incident
        assert analyzed()['phase'] == 'running'
        stopped = other.tool('stop_flicker_monitor', {'application_id': application, 'monitor_id': 'first'})
        assert not stopped['error'] and stopped['data']['phase'] == 'stopped', stopped
        assert not client.tool('stop_flicker_monitor', {'application_id': application, 'monitor_id': 'first'})['error']
        assert not client.tool('start_flicker_monitor', options)['error']  # Identical retry stays stopped.
        assert client.tool('get_ui_flicker_status', {'application_id': application})['data']['phase'] == 'not_started'

        second = dict(options, monitor_id='second', target='ui')
        assert not client.tool('start_flicker_monitor', second)['error']
        assert client.tool('start_flicker_monitor', options)['error']  # A second monitor cannot replace an active one.
        time.sleep(.3)
        stopped = client.tool('stop_flicker_monitor', {'application_id': application, 'monitor_id': 'second'})
        assert not stopped['error'] and stopped['data']['phase'] == 'stopped', stopped

        assert not other.tool('start_flicker_monitor', dict(options, monitor_id='other'))['error']
        before = eventually(analyzed, timeout=50)
        other.process.stdin.close()
        assert other.process.wait(timeout=15) == 0
        client.process.stdin.close()
        assert client.process.wait(timeout=15) == 0
        time.sleep(.5)
        client = test.connect(connector, '--app', app, '--windowless', log=log, name='monitor-reconnected')
        def continued():
            result = analyzed()
            return result if result and result['windows'] > before['windows'] else None
        result = eventually(continued, timeout=50)
        assert result['monitor_id'] == 'other' and result['phase'] == 'running', result
        assert 'missed_cadences' in result and result['unobserved_frames'] >= 0, result
        assert not client.tool('stop_flicker_monitor', {'application_id': application, 'monitor_id': 'other'})['error']
        reused = client.tool('start_flicker_monitor', options)
        assert not reused['error'] and reused['data']['phase'] == 'running', reused
        assert not client.tool('stop_flicker_monitor', {'application_id': application, 'monitor_id': 'first'})['error']
        assert not client.tool('start_flicker_monitor', dict(options, monitor_id='shutdown'))['error']
        assert not client.tool('simulator_stop_application', {'application_id': application})['error']
        assert not client.tool('simulator_shutdown', {})['error']
        for bridge in (client.process, other.process):
            if not bridge.stdin.closed:
                bridge.stdin.close()
            assert bridge.wait(timeout=15) == 0
        print('PASS application monitor detection, shared control, disconnect survival and shutdown')


    with McpTest('simxr-configured-monitor-', windowless=True) as test:
        test.env['SIMXR_MONITOR'] = 'composed'
        log = test.log('configured.log')
        client = test.connect(connector, '--app', app, '--windowless', log=log)
        test.discover()
        launched = client.tool('simulator_launch_application', launch)
        assert not launched['error'], launched
        application = launched['data']['application_id']
        state = test.connected(client, application)

        def configured():
            result = client.tool('get_flicker_status', {'application_id': application})
            assert not result['error'] and result['data']['phase'] != 'failed', result
            return result['data'] if result['data'].get('windows', 0) else None

        before = eventually(lambda: (value if (value := configured()) and value['events'] else None), timeout=50)
        assert before['monitor_id'].startswith('configured-') and before['events'], before
        # Monitoring does not monopolize the independent user automation worker.
        rig = client.tool('simulator_luau_run', {'application_id': application,
                          'source': 'simxr.wait(50)', 'name': 'concurrent-rig'})
        assert not rig['error'], rig
        stopped = client.tool('stop_flicker_monitor', {'application_id': application, 'monitor_id': before['monitor_id']})
        assert not stopped['error'] and stopped['data']['phase'] == 'stopped', stopped
        time.sleep(.4)
        assert configured()['phase'] == 'stopped'  # Explicit stop is respected until the next session.
        restarted = client.tool('simulator_restart_application', {'application_id': application,
                               'process_id': state['process_id']})
        assert not restarted['error'], restarted

        def replaced():
            status = client.tool('simulator_application_status', {'application_id': application})
            assert not status['error'], status
            data = status['data']
            return data if data['session_running'] and data['process_id'] != state['process_id'] else None

        replacement = eventually(replaced, timeout=30)
        test.track(replacement['process_id'])
        after = eventually(lambda: (value if (value := configured()) and
                                   value['monitor_id'] != before['monitor_id'] else None), timeout=50)
        assert after['runtime_process_id'] == replacement['runtime_process_id'], after
        assert not client.tool('simulator_stop_application', {'application_id': application})['error']
        def terminal():
            status = client.tool('get_flicker_status', {'application_id': application})
            return status['data'] if status['data']['phase'] != 'running' else None
        eventually(terminal)
        assert not client.tool('simulator_shutdown', {})['error']
        client.process.stdin.close()
        assert client.process.wait(timeout=15) == 0
        print('PASS configured monitoring, concurrent rig, explicit stop, restart and application lifetime')


if __name__ == '__main__':
    main()
