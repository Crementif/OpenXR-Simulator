"""Actual runtime C ABI -> game update point -> SDK, without XR or GPU."""
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
    with tempfile.TemporaryDirectory(prefix='simxr-native-game-') as temporary:
        stop = Path(temporary) / 'stop'
        environment = dict(os.environ, SIMXR_DATA_DIR=temporary,
                           VK_DRIVER_FILES='/nonexistent/simxr-no-graphics-driver.json')
        process = subprocess.Popen([sys.argv[1], sys.argv[2], str(stop)], stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, env=environment)
        path = Path(temporary) / f'control.{process.pid}.sock'
        client = None
        try:
            deadline = time.monotonic() + 5
            while not path.exists():
                assert process.poll() is None, process.communicate()[0]
                assert time.monotonic() < deadline, 'C ABI did not publish endpoint without XR'
                time.sleep(.005)
            client = Client(path)
            assert client.sessions() == []
            status = client.game_status()
            assert status['document']['capabilities'] == 1, status
            game = status['game_id']
            queued = client.game_command(None, game, 1, 'checkpoint:start', command_id=1)
            duplicate = client.game_command(None, game, 1, 'checkpoint:start', command_id=1)
            assert duplicate['operation_id'] == queued['operation_id'], duplicate
            while True:
                result = client.game_operation(None, game, queued['operation_id'])
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
                tail = observer.game_observations(offset=journal['next'])['document']
                assert not tail['gap'] and tail['observations'] == [], tail
                try:
                    observer.game_operation(None, int(game), int(queued['operation_id']))
                    raise AssertionError('Another connection read a private game operation')
                except ControlError:
                    pass
            if client is not None:
                client.close()
            stop.touch()
            output = process.communicate(timeout=5)[0]
            assert process.returncode == 0, output
            assert not path.exists(), 'Explicit C ABI shutdown left control socket behind'
            print(output.strip())
        finally:
            if client is not None:
                client.close()
            if process.poll() is None:
                process.terminate()
                process.communicate(timeout=5)


if __name__ == '__main__':
    run()
