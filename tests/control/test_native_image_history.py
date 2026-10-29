"""GPU history survives recycling/eviction and cannot starve live rendering."""
from contextlib import ExitStack
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from native_support import probe_environment, running_probe, export, require, wait_file, command
from simxr_control import Capability, Client, CommandKind, TargetKind
from simxr_control.errors import ControlError
from simxr_control.protocol import ErrorCode


def retain(client, session, frame):
    deadline = time.monotonic() + 5
    while True:
        try:
            return client.retain_historical_image(session, frame)['image_reader_id']
        except ControlError as error:
            require(error.code == ErrorCode.BUSY and time.monotonic() < deadline, error.record)
            time.sleep(.005)


ROOT = Path(__file__).resolve().parents[2]


def main():
    with tempfile.TemporaryDirectory(prefix='sx-history-') as temporary:
        data = Path(temporary)
        environment = probe_environment(data, SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_GPU_INSPECTION='1',
                           SIMXR_PROBE_RECORDING='1', SIMXR_PROBE_DYNAMIC_COLOR='1',
                           SIMXR_PROBE_FRAMES='36', SIMXR_WIDTH='64', SIMXR_HEIGHT='48')

        with running_probe([sys.argv[1]], environment) as host:
            wait_file(data / 'control.ready', host)
            with ExitStack() as stack:
                clients = [stack.enter_context(Client(data / f'control.{host.pid}.sock')) for _ in range(3)]
                client = clients[0]
                session, = client.sessions()
                require(client.status(session)['capabilities'] & Capability.IMAGE_HISTORY, 'History capability absent')
                baseline = client.status(session)
                (data / 'control.continue').touch()
                readers = []
                for frame in range(3, 39):
                    wait_file(data / f'recording.{frame}.ready', host)
                    if frame <= 26:
                        owner = clients[(frame - 3) // 8]
                        readers.append((owner, retain(owner, session, frame), frame))
                    if frame == 15:
                        runner = subprocess.run([sys.argv[2], '--socket', str(data / f'control.{host.pid}.sock'),
                                                 '--rig', str(ROOT / 'examples/rigs/export_history.luau'),
                                                 '--json', str(data / 'history-report.json')],
                                                cwd=data, env=environment, capture_output=True, text=True, timeout=10)
                        require(runner.returncode == 0, runner.stdout + runner.stderr)
                        require((data / 'history-left.rgba').read_bytes() == bytes((255, 0, 0, 255)) * (64 * 48),
                                'Luau historical export substituted source pixels')
                        require(json.loads((data / 'history-report.json').read_text())['phase'] == 'complete', 'Luau report missing')
                    if frame == 26:
                        history = client.image_history(session)['document']
                        require([entry['frame_id'] for entry in history['entries']] == list(range(17, 27)), history)
                        require(history['evicted'] >= 16, history)
                        try:
                            client.retain_historical_image(session, 3)
                        except ControlError as error:
                            require(error.code == ErrorCode.NOT_FOUND, error.record)
                        else:
                            raise AssertionError('Evicted history was fabricated')
                    if frame == 30:
                        history = client.image_history(session)['document']
                        require(history['unavailable'] >= 4 and history['entries'][-1]['phase'] == 'unavailable', history)
                        with Client(data / f'control.{host.pid}.sock') as preview:
                            selected = preview.retain_preview(session)
                            require(selected.get('frame', {}).get('frame_id', 0) >= frame - 2,
                                    'History pressure starved current live output')
                            preview.release_image(selected['image_reader_id'])
                        for owner, reader, _ in readers[2:]:
                            owner.release_image(reader)
                        readers = readers[:2]
                    if frame == 34:
                        recovered = retain(clients[1], session, frame)
                        clients[1].release_image(recovered)
                        status = client.status(session)
                        require(status['manual'] == baseline['manual'] == False and
                                status['pipeline']['permits'] == baseline['pipeline']['permits'],
                                'Historical readers changed application admission')
                        lease = client.acquire(session)
                        paused = command(client, lease, CommandKind.PAUSE)
                        for capture in paused.get('captures', []):
                            command(client, lease, CommandKind.RELEASE, capture=capture)
                        require(not client.image_history(session)['document']['entries'], 'Pause did not retire cache')
                        command(client, lease, CommandKind.RESUME)
                        client.relinquish(lease)
                    if frame == 38:
                        lease = client.acquire(session)
                        started = command(client, lease, CommandKind.START_IMAGE_RECORDING,
                                          target=TargetKind.LAYER_VIEW, recording_capacity=4, recording_frames=1)
                        require(len(started['captures']) == 4, started)
                        command(client, lease, CommandKind.STOP_IMAGE_RECORDING)
                        client.relinquish(lease)
                    (data / f'recording.{frame}.continue').touch()
                wait_file(data / 'control.destroyed', host)
                for owner, reader, frame in readers:
                    info = owner.image_info(reader)
                    require(info['frame']['frame_id'] == frame and info['target']['capture_kind'] == TargetKind.COMPOSED_EYES, info)
                    _, pixels = export(owner, reader, 0)
                    expected = bytes((255, 0, 0, 255)) if frame == 3 else bytes((0, 255, 0, 255))
                    require(pixels == expected * (64 * 48), f'Historical frame {frame} changed after source reuse/teardown')
                    owner.release_image(reader)
                (data / 'control.retained.continue').touch()
            output = host.communicate(timeout=15)[0]
            require(host.returncode == 0, output)
            require('VUID-' not in output and 'Vulkan validation error' not in output, output)
            print('PASS GPU history: exact old pixels, bounded pressure/recovery, independent capture budget, teardown')


if __name__ == '__main__':
    main()
