#!/usr/bin/env python3
"""Exercise explicit sampled flicker monitoring with a real XR application."""


from pathlib import Path
import sys
import time

from mcp_support import McpTest, eventually


def main():
    app, connector, probe = (str(Path(value).resolve()) for value in sys.argv[1:4])
    with McpTest('simxr-flicker-monitor-', windowless=True) as test:
        root = test.root

        log = test.log('host.log')

        client = test.connect(connector, '--app', app, '--windowless', log=log, name='monitor-owner')
        names = {item['name'] for item in client.rpc('tools/list')['result']['tools']}
        assert {'start_flicker_monitor', 'get_flicker_status', 'get_ui_flicker_status', 'stop_flicker_monitor'} <= names
        test.discover()

        launched = client.tool('simulator_launch_application', {'executable': probe, 'environment': {
            'SIMXR_PROBE_INTERACTIVE': '1', 'SIMXR_PROBE_DYNAMIC_COLOR': '1',
            'SIMXR_PROBE_FRAMES': '120000', 'SIMXR_WIDTH': '128', 'SIMXR_HEIGHT': '96'}})
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

        result = eventually(analyzed, timeout=50)
        assert result['generation'] == state['generation'] and result['frames_observed'] >= 2, result
        assert result['last_analysis']['frames_analyzed'] >= 2 and result['events'], result
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
        def continued():
            result = analyzed()
            return result if result and result['windows'] > before['windows'] else None
        result = eventually(continued, timeout=50)
        assert result['monitor_id'] == 'other' and result['phase'] == 'running', result
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


if __name__ == '__main__':
    main()
