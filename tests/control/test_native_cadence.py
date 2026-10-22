"""A slow then fast application changes cadence without changing display refresh."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from simxr_control import Client
from test_native_quad import require


def wait_boundary(path, host):
    deadline = time.monotonic() + 15
    while not path.exists():
        require(host.poll() is None, f'Probe exited before {path.name}')
        require(time.monotonic() < deadline, f'Missing {path.name}')
        time.sleep(.0005)


def run(probe):
    with tempfile.TemporaryDirectory(prefix='sx-cadence-') as temporary:
        data = Path(temporary)
        environment = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_DETERMINISTIC='0',
                           SIMXR_WIDTH='64', SIMXR_HEIGHT='48', SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_GPU_INSPECTION='1', SIMXR_PROBE_RENDER_GATE='1',
                           SIMXR_PROBE_INTERACTIVE='1', SIMXR_PROBE_FRAMES='180',
                           SIMXR_APPLICATION_CADENCE='auto', SIMXR_PRESENTATION_POLICY='repeat')
        environment.pop('SIMXR_CONTROL_SOCKET', None)
        with (data / 'probe.log').open('w+') as log:
            host = subprocess.Popen([probe], env=environment, stdout=log, stderr=subprocess.STDOUT)
            try:
                wait_boundary(data / 'control.ready', host)
                with Client(data / f'control.{host.pid}.sock') as client:
                    session, = client.sessions()
                    (data / 'control.continue').touch()
                    for frame in range(3, 9):
                        wait_boundary(data / f'rendering.{frame}.ready', host)
                        # Hold actual work across several display deadlines.
                        time.sleep(.035)
                        (data / f'rendering.{frame}.continue').touch()
                    # No control round trips or gate waits in the fast workload.
                    for frame in range(9, 183):
                        (data / f'rendering.{frame}.continue').touch()
                    require(host.wait(timeout=15) == 0, 'Probe failed after cadence workflow')
                    samples = [tuple(map(int, (data / f'rendering.{frame}.ready').read_text().split()))
                               for frame in range(3, 183)]
                    periods, predictions = zip(*samples)
                    full = periods[0]
                    require(full > 0 and 2 * full in periods[:15], f'No half-rate transition: {periods}')
                    require(full in periods[40:], f'No recovery to full rate: {periods}')
                    require(set(periods) == {full, 2 * full}, periods)
                    require(all(a < b for a, b in zip(predictions, predictions[1:])),
                            'Predicted display times regressed')
                    print(f'Cadence periods: {full} -> {2 * full} -> {full}; '
                          f'{len(periods)} increasing predictions', flush=True)
                require(host.wait(timeout=15) == 0, 'Probe failed after cadence workflow')
                log.seek(0)
                output = log.read()
                require('Validation Error' not in output and 'VUID-' not in output, output)
            except Exception:
                if host.poll() is None:
                    host.kill()
                host.wait()
                log.seek(0)
                print(log.read(), file=sys.stderr)
                raise


if __name__ == '__main__':
    run(sys.argv[1])
