"""Actual runtime C ABI -> game update point -> SDK/MCP, without XR or GPU."""
import importlib.util
import json
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'sdk/python'))
from simxr_control import Client, ControlError


def run():
    spec = importlib.util.spec_from_file_location('game_tools', ROOT / 'mcp-server/debug_control.py')
    adapter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(adapter)
    with tempfile.TemporaryDirectory(prefix='simxr-native-game-') as temporary:
        path, stop = Path(temporary) / 'control.sock', Path(temporary) / 'stop'
        # The C ABI/control route must work even when no Vulkan driver can load.
        environment = dict(os.environ, VK_DRIVER_FILES='/nonexistent/simxr-no-graphics-driver.json')
        process = subprocess.Popen([sys.argv[1], sys.argv[2], str(path), str(stop)], stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, env=environment)
        tools = adapter.DebugTools()
        try:
            deadline = time.monotonic() + 5
            while not path.exists():
                assert process.poll() is None, process.communicate()[0]
                assert time.monotonic() < deadline, 'C ABI did not publish endpoint without XR'
                time.sleep(.005)
            connected = tools.call('debug_connect', {'path': str(path)})
            assert connected['ok'] and connected['sessions'] == [], connected
            common = {'connection': connected['connection']}
            status = tools.call('game_bridge_status', common)
            assert status['ok'] and status['document']['capabilities'] == 1, status
            game = status['game_id']
            args = {**common, 'game': game, 'command_id': '1', 'kind': 1, 'payload': 'checkpoint:start'}
            queued = tools.call('game_bridge_command', args)
            assert queued['ok'], queued
            duplicate = tools.call('game_bridge_command', args)
            assert duplicate['ok'] and duplicate['operation_id'] == queued['operation_id'], duplicate
            while True:
                result = tools.call('game_bridge_operation', {**common, 'game': game, 'operation': queued['operation_id']})
                assert result['ok'], result
                if result['phase'] == 'complete':
                    assert result['document']['result'] == 'reset applied', result
                    break
                assert result['phase'] in ('queued', 'delivered') and time.monotonic() < deadline, result
                time.sleep(.005)
            with Client(path) as observer:
                status = observer.game_status()
                assert json.loads(status['document']['telemetry'])['reset_count'] == 1, status
                journal = observer.game_observations(offset=0)['document']
                assert not journal['gap'] and not journal['failed'], journal
                events = journal['observations']
                assert [event['kind'] for event in events] == ['registered', 'telemetry'], events
                assert events[1]['game_id'] == int(game) and events[1]['revision'] == 1, events
                assert json.loads(events[1]['payload'])['reset_count'] == 1, events
                for event in events:
                    assert event['observed_time']['domain'] in (1, 2), event
                    assert event['observed_time']['nanoseconds'] > 0, event
                    assert event['observed_time']['resolution'] > 0, event
                mcp_journal = tools.call('game_bridge_observations', {**common, 'offset': '0', 'count': 16})
                assert mcp_journal['ok'] and len(mcp_journal['document']['observations']) == 2, mcp_journal
                assert int(mcp_journal['document']['next']) == journal['next'], mcp_journal
                tail = observer.game_observations(offset=journal['next'])['document']
                assert not tail['gap'] and tail['observations'] == [], tail
                try:
                    observer.game_operation(None, int(game), int(queued['operation_id']))
                    raise AssertionError('Another connection read a private game operation')
                except ControlError:
                    pass
            tools.close()
            stop.touch()
            output = process.communicate(timeout=5)[0]
            assert process.returncode == 0, output
            assert not path.exists(), 'Explicit C ABI shutdown left control socket behind'
            print(output.strip())
        finally:
            tools.close()
            if process.poll() is None:
                process.terminate()
                process.communicate(timeout=5)


if __name__ == '__main__':
    run()
