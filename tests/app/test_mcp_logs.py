#!/usr/bin/env python3
"""Exercise bounded native runtime-log reads against a real XR process."""

import os
from pathlib import Path
import sys

from mcp_support import Client, McpTest, eventually


def main():
    app, connector, probe = (str(Path(value).resolve()) for value in sys.argv[1:4])
    with McpTest('simxr-logs-', windowless=True) as test:
        root = test.root
        child_data = root / 'runtime ü'
        child_data.mkdir()
        test.env['SIMXR_DATA_DIR'] = str(root / 'host-data')

        log = test.log('bridge.log')
        bridge = test.bridge(connector, '--app', app, '--windowless', log=log)
        client = Client(bridge)
        client.initialize('native-log-test')

        names = {entry['name'] for entry in client.rpc('tools/list')['result']['tools']}
        assert {'read_logs', 'clear_logs'} <= names
        test.discover(client)

        launched = client.tool('simulator_launch_application', {'executable': probe, 'environment': {
            'SIMXR_DATA_DIR': str(child_data), 'SIMXR_PROBE_INTERACTIVE': '1', 'SIMXR_PROBE_FRAMES': '120000',
            'SIMXR_WIDTH': '128', 'SIMXR_HEIGHT': '96'}})
        assert not launched['error'], launched
        application = launched['data']['application_id']

        state = test.connected(client, application, timeout=30)

        assert state['inferred_runtime_data_directory'] == str(child_data), state
        def live_log():
            result = client.tool('read_logs', {'application_id': application})
            assert not result['error'], result
            return result['data'] if result['data']['available'] and result['data']['lines'] else None
        actual = eventually(live_log, timeout=20)
        assert actual['runtime_process_id'] == state['runtime_process_id']
        assert actual['directory_source'] == 'inferred_from_launch' and not actual['identity_verified']
        assert Path(actual['path']).parent == child_data and any('create instance' in line for line in actual['lines']), actual
        real_clear = client.tool('clear_logs', {'application_id': application})
        assert not real_clear['error'] and real_clear['data']['cleared'], real_clear
        assert Path(real_clear['data']['path']) == Path(actual['path']) and Path(actual['path']).is_file()
        assert not client.tool('read_logs', {'application_id': application})['error']

        fixture = root / 'fixture Ω'
        fixture.mkdir()
        path = fixture / ('openxr_simulator.' + state['runtime_process_id'] + '.log')
        path.write_bytes(b'x' * (256 * 1024) + b'\nold\nFirst\nSecond ERROR\nthird\ninvalid\xff\n')
        response = client.tool('read_logs', {'application_id': application, 'directory': str(fixture),
                                            'lines': 3, 'filter': 'eRrOr'})
        assert not response['error'], response
        data = response['data']
        assert data['directory_source'] == 'explicit' and data['tail_truncated'] and data['lines_truncated']
        assert data['lines'] == ['Second ERROR'] and not data['changed_during_read'], data
        unfiltered = client.tool('read_logs', {'application_id': application, 'directory': str(fixture), 'lines': 1})
        assert unfiltered['data']['lines'] == ['invalid\ufffd'], unfiltered
        assert client.tool('read_logs', {'application_id': application, 'lines': 4294967297})['error']
        unrelated = fixture / 'openxr_simulator.123456789.log'
        unrelated.write_text('other process', encoding='utf-8')
        linked_file = fixture / 'hard-linked-backup'
        os.link(path, linked_file)
        assert client.tool('clear_logs', {'application_id': application, 'directory': str(fixture)})['error']
        assert path.stat().st_size > 0 and linked_file.stat().st_size > 0
        linked_file.unlink()
        cleared = client.tool('clear_logs', {'application_id': application, 'directory': str(fixture)})
        assert not cleared['error'] and cleared['data']['cleared'] and not cleared['data']['identity_verified'], cleared
        assert path.exists() and path.stat().st_size == 0 and unrelated.read_text() == 'other process'
        with path.open('ab') as stream:
            stream.write(b'new log entry\n')
        fresh = client.tool('read_logs', {'application_id': application, 'directory': str(fixture)})
        assert not fresh['error'] and fresh['data']['lines'] == ['new log entry'], fresh
        path.unlink()
        missing = client.tool('clear_logs', {'application_id': application, 'directory': str(fixture)})
        assert not missing['error'] and not missing['data']['cleared'], missing
        assert client.tool('clear_logs', {'application_id': application, 'lines': 1})['error']
        bad = root / 'bad'
        bad.mkdir()
        name = bad / path.name
        name.symlink_to(path)
        assert client.tool('read_logs', {'application_id': application, 'directory': str(bad)})['error']
        assert client.tool('clear_logs', {'application_id': application, 'directory': str(bad)})['error']
        name.unlink()
        os.mkfifo(name)
        assert client.tool('read_logs', {'application_id': application, 'directory': str(bad)})['error']
        assert client.tool('clear_logs', {'application_id': application, 'directory': str(bad)})['error']

        (root / 'target' / 'inner').mkdir(parents=True)
        (root / 'linked').symlink_to(root / 'target' / 'inner', target_is_directory=True)
        second_data = root / 'linked' / '..' / 'other runtime'
        second_data.mkdir()
        launched_other = client.tool('simulator_launch_application', {'executable': probe, 'environment': {
            'SIMXR_DATA_DIR': str(second_data), 'SIMXR_PROBE_INTERACTIVE': '1', 'SIMXR_PROBE_FRAMES': '120000',
            'SIMXR_WIDTH': '128', 'SIMXR_HEIGHT': '96'}})
        assert not launched_other['error'], launched_other
        other_id = launched_other['data']['application_id']

        other = test.connected(client, other_id, timeout=30)

        def other_log():
            result = client.tool('read_logs', {'application_id': other_id})
            assert not result['error'], result
            return result['data'] if result['data']['available'] else None
        distinct = eventually(other_log, timeout=20)
        assert other['runtime_process_id'] != state['runtime_process_id']
        assert distinct['runtime_process_id'] == other['runtime_process_id'] and Path(distinct['path']).parent == second_data
        assert not client.tool('simulator_stop_application', {'application_id': other_id})['error']
        assert not client.tool('simulator_stop_application', {'application_id': application})['error']
        assert not client.tool('simulator_shutdown', {})['error']
        bridge.stdin.close()
        assert bridge.wait(timeout=15) == 0
        print('PASS native log targeting, bounded tail, safe scoped clearing and non-regular refusal')


if __name__ == '__main__':
    main()
