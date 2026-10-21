"""Run the public cooperating-game example through the native optional C ABI."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'sdk/python'))
from simxr_control import Client, ControlError


def main():
    with tempfile.TemporaryDirectory(prefix='sx-game-') as temporary:
        endpoint = Path(temporary) / 'control.sock'
        environment = dict(os.environ, VK_DRIVER_FILES='/nonexistent/simxr-no-graphics-driver.json')
        process = subprocess.Popen([sys.executable, str(ROOT / 'examples/cooperating-game.py'),
                                    '--runtime', sys.argv[1], '--socket', str(endpoint)], env=environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            deadline = time.monotonic() + 10
            while not endpoint.exists():
                assert process.poll() is None, process.communicate()[0]
                assert time.monotonic() < deadline, 'Example never registered its bridge'
                time.sleep(.005)
            with Client(endpoint) as client:
                assert client.sessions() == [], 'Example unexpectedly required XR'
                state = client.game_status()
                assert state['document']['capabilities'] == 3, state
                game = state['game_id']
                for kind, payload, expected in ((2, '3', 3), (1, 'checkpoint:start', 0), (2, '7', 7)):
                    submitted = client.game_command(None, game, kind, payload)
                    while True:
                        operation = client.game_operation(None, game, submitted['operation_id'])
                        if operation['phase'] == 'complete':
                            break
                        assert operation['phase'] in ('queued', 'delivered') and time.monotonic() < deadline, operation
                        time.sleep(.005)
                    telemetry = json.loads(client.game_status()['document']['telemetry'])
                    assert telemetry['ticks'] == expected and abs(telemetry['position'] - expected * .1) < .0001, telemetry
                try:
                    client.game_command(None, game, 3, 'unadvertised custom command')
                    raise AssertionError('Unsupported game capability was accepted')
                except ControlError:
                    pass
            process.send_signal(signal.SIGINT)
            output = process.communicate(timeout=5)[0]
            assert process.returncode == 0 and not endpoint.exists(), output
            print('PASS public game model: actual reset/step, truthful capabilities, no XR/GPU, clean shutdown')
        finally:
            if process.poll() is None:
                process.terminate()
                process.communicate(timeout=5)


if __name__ == '__main__':
    main()
