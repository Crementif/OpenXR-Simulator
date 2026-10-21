"""Actual public Luau recording success, explicit overrun and cancellation cleanup."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

from test_native_quad import require, wait_file
from simxr_control import Client


def run_case(mode):
    with tempfile.TemporaryDirectory(prefix='sx-lua-rec-') as temporary:
        data = Path(temporary)
        environment = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_RECORDING='1',
                           SIMXR_PROBE_GPU_INSPECTION='1', SIMXR_PROBE_DEPTH='1', SIMXR_PROBE_FRAMES='4')
        environment.pop('SIMXR_CONTROL_SOCKET', None)
        host = subprocess.Popen([sys.argv[1]], env=environment, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        runner = None
        try:
            wait_file(data / 'control.ready', host)
            if mode == 'success':
                source = (Path(__file__).resolve().parents[2] / 'examples/rigs/record_images.luau').read_text().replace(
                    '{capacity=2, frames="2"}', '{target="layer_depth", view=1, capacity=2, frames="2"}')
            else:
                source = '''assert(simxr.image_recording().available)
simxr.start_image_recording({capacity=1})
while simxr.image_recording().phase == "recording" do end
local recording = simxr.image_recording()
simxr.checkpoint("explicit overrun", recording.phase == "overrun" and recording.missed_frame == "4")
error("intentional failure after observed overrun")
'''
            (data / 'example.luau').write_text(source)
            endpoint = data / f'control.{host.pid}.sock'
            runner = subprocess.Popen([sys.argv[2], '--socket', str(endpoint), '--rig', str(data / 'example.luau'),
                                      '--json', str(data / 'report.json'), '--junit', str(data / 'report.xml')],
                                     cwd=data, env=environment, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True)
            with Client(endpoint) as client:
                session, = client.sessions()
                deadline = time.monotonic() + 10
                while client.status(session)['image_recording']['phase'] != 'recording':
                    require(runner.poll() is None, 'Runner stopped before recording began')
                    require(time.monotonic() < deadline, 'Recording did not start')
                    time.sleep(.005)
                if mode == 'cancel':
                    runner.send_signal(signal.SIGINT)
                else:
                    (data / 'control.continue').touch()
                    wait_file(data / 'recording.3.ready', host)
                    if mode == 'success':
                        wait_file(data / 'frame-3.rgba', runner)
                    (data / 'recording.3.continue').touch()
                    wait_file(data / 'recording.4.ready', host)
                output = runner.communicate(timeout=15)[0]
                report = json.loads((data / 'report.json').read_text())
                if mode == 'success':
                    require(runner.returncode == 0 and report['phase'] == 'complete', report)
                    images = [(data / f'frame-{frame}.rgba').read_bytes() for frame in (3, 4)]
                    require(len(images[0]) == len(images[1]) == 62 * 46 * 4 and images[0] != images[1],
                            'Luau persisted a missing or substituted frame')
                elif mode == 'overrun':
                    require(runner.returncode != 0 and report['phase'] == 'failed' and
                            'intentional failure' in report['error'], report)
                    require(any(item['name'] == 'explicit overrun' and item['passed']
                                for item in report['checkpoints']), report)
                else:
                    require(runner.returncode != 0 and report['phase'] == 'cancelled', report)
                require((data / 'report.xml').stat().st_size > 0, 'JUnit report absent')
                state = client.status(session)
                require(not state['manual'] and not state['pipeline']['paused'] and not state['pipeline']['permits'], state)
                require(not state['image_recording']['captures'] and state['image_recording']['phase'] != 'recording',
                        'Rig cleanup retained recording slots or continued recording')
                (data / 'control.continue').touch()
                for frame in range(3, 7):
                    (data / f'recording.{frame}.continue').touch()
                wait_file(data / 'control.destroyed', host)
                (data / 'control.retained.continue').touch()
            output = host.communicate(timeout=15)[0]
            require(host.returncode == 0, output)
            require('VUID-' not in output and 'Vulkan validation error' not in output, output)
        finally:
            for process in (runner, host):
                if process is not None and process.poll() is None:
                    process.kill()
                    process.communicate()
    print('PASS Luau recording ' + mode)


if __name__ == '__main__':
    for case in ('success', 'overrun', 'cancel'):
        run_case(case)
