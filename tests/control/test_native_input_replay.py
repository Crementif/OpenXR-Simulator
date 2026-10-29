"""Recorded source cleanup survives editable Luau replay at the intermediate frame."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from native_support import probe_environment, running_probe, require, wait_file
from simxr_control import Client, collect_recording, to_luau


def main(probe, executable):
    with tempfile.TemporaryDirectory(prefix='sx-input-replay-') as temporary:
        data = Path(temporary)
        environment = probe_environment(data, SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_GPU_INSPECTION='1',
                           SIMXR_PROBE_RECORDING='1', SIMXR_PROBE_INTERACTIVE='1',
                           SIMXR_PROBE_CANONICAL_SEQUENCE='1', SIMXR_PROBE_FRAMES='14')
        with running_probe([probe], environment) as host:
            runner = None
            try:
                wait_file(data / 'control.ready', host)
                endpoint = data / f'control.{host.pid}.sock'
                with Client(endpoint) as client, Client(endpoint) as recorder:
                    session, = client.sessions()
                    steps = [{'frame': frame, 'head': {'x': .3 + .1 * frame}} for frame in range(4)]
                    steps[0]['head'].update(y=1.8, z=-.4, yaw=.2, pitch=-.1, roll=.15)
                    program = client.upload_sequence(session, '\n'.join(json.dumps(step) for step in steps))
                    sequence = client.start_sequence(session, program)['sequence_id']
                    (data / 'control.continue').touch()
                    for frame in range(3, 8):
                        wait_file(data / f'recording.{frame}.ready', host)
                        if frame != 7:
                            (data / f'recording.{frame}.continue').touch()
                    client.stop_sequence(session, sequence)
                    # An independent recorder observes control-source retirement and its successor.
                    # Start after the probe releases its own startup controls at frame 7.
                    recorder.start_recording(session)
                    client.schedule(session, [{'kind': 'take_control', 'channel': 'left_controls'},
                                              {'kind': 'controller_controls', 'hand': 0, 'trigger': .75}], frame=8)
                    (data / 'recording.7.continue').touch()
                    wait_file(data / 'recording.8.ready', host)
                    client.release_control(session)
                    client.schedule(session, [{'kind': 'take_control', 'channel': 'gaze'},
                                              {'kind': 'gaze', 'yaw': .2}], frame=10)
                    (data / 'recording.8.continue').touch()
                    wait_file(data / 'recording.9.ready', host)
                    state = client.simulation_status(session)['document']
                    require(state['configuration']['controllers'][0]['controls']['trigger'] == 0, state)
                    (data / 'recording.9.continue').touch()
                    wait_file(data / 'recording.10.ready', host)
                    recorder.stop_recording(session)
                    recording = collect_recording(recorder, session)
                    require(recording['version'] == 3 and recording['total'] == 3, recording)
                    require(recording['transactions'][1]['phase'] == 'released' and
                            recording['transactions'][1]['after_frame'], recording)
                    client.release_control(session)
                    script = data / 'replay.luau'
                    script.write_text(to_luau(recording))
                    runner = subprocess.Popen([executable, '--socket', str(endpoint), '--rig', str(script),
                                               '--seed', str(recording['seed']), '--json', str(data / 'report.json')],
                                              env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                    # The application is stopped at its frame-10 gate; the runner owns
                    # subsequent strict step permits and actual display submissions.
                    deadline = time.monotonic() + 5
                    while not client.status(session)['pipeline']['paused']:
                        require(runner.poll() is None and time.monotonic() < deadline,
                                'Replay did not acquire its pause before the next app boundary')
                        time.sleep(.005)
                    (data / 'recording.10.continue').touch()
                    for frame in range(11, 17):
                        wait_file(data / f'recording.{frame}.ready', host)
                        state = client.simulation_status(session)['document']
                        if frame in (12, 13, 14):
                            trigger = state['configuration']['controllers'][0]['controls']['trigger']
                            require(trigger == (.75 if frame == 12 else 0), (frame, state))
                            print(f'Replayed frame {frame}: left trigger {trigger}')
                        (data / f'recording.{frame}.continue').touch()
                    output = runner.communicate(timeout=15)[0]
                    report = json.loads((data / 'report.json').read_text())
                    require(runner.returncode == 0 and report['phase'] == 'complete', (output, report))
                    wait_file(data / 'control.destroyed', host)
                    (data / 'control.retained.continue').touch()
                output = host.communicate(timeout=15)[0]
                require(host.returncode == 0, output)
                require('VUID-' not in output and 'Vulkan validation error' not in output, output)
            finally:
                if runner is not None and runner.poll() is None:
                    runner.kill()
                    runner.communicate()
    print('PASS native input release recording and editable replay')


if __name__ == '__main__':
    main(*sys.argv[1:])
