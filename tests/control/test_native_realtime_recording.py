"""Realtime reserved capture stream: refill, explicit overrun and GPU-safe cleanup."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from test_native_quad import export, require, wait_file
from simxr_control import Capability, Client, CommandKind


def run(probe, overrun):
    with tempfile.TemporaryDirectory(prefix='sx-realtime-') as temporary:
        data = Path(temporary)
        environment = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_GPU_INSPECTION='1',
                           SIMXR_PROBE_RECORDING='1', SIMXR_PROBE_FRAMES='8')
        environment.pop('SIMXR_CONTROL_SOCKET', None)
        host = subprocess.Popen([probe], env=environment, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        try:
            wait_file(data / 'control.ready', host)
            with Client(data / f'control.{host.pid}.sock') as client:
                session, = client.sessions()
                require(client.status(session)['capabilities'] & Capability.IMAGE_RECORDING,
                        'Native image recording capability missing')
                lease = client.acquire(session)
                operation = client.submit(lease, CommandKind.START_IMAGE_RECORDING, recording_capacity=2)['operation_id']
                started = client.wait(session, operation)
                require(started['progress'] == 'recording_started', started)
                client.acknowledge(lease, started['command_id'])
                (data / 'control.continue').touch()
                if overrun:
                    for frame in (3, 4):
                        wait_file(data / f'recording.{frame}.ready', host)
                        (data / f'recording.{frame}.continue').touch()
                    wait_file(data / 'recording.5.ready', host)
                    state = client.status(session)
                    stream = state['image_recording']
                    require(stream['phase'] == 'overrun' and stream['length'] == 2 and
                            stream['frame']['frame_id'] == 5, stream)
                    require(not state['manual'] and not state['pipeline']['paused'] and state['pipeline']['permits'] == 0,
                            'Real-time overrun altered app pacing')
                    readers = []
                    for capture in stream['captures']:
                        deadline = time.monotonic() + 5
                        while client.capture(session, capture)['phase'] != 'ready':
                            require(time.monotonic() < deadline, 'Reserved output did not finish')
                            time.sleep(.01)
                        readers.append(client.retain_image(session, capture)['image_reader_id'])
                    diagnostics = client.diagnostics(session, frame=5)['document']
                    require(any(event['detail'] == 'image_recording_overrun' for event in diagnostics['events']),
                            'Overrun lost its first missed-frame diagnostic')
                else:
                    readers = []
                    for frame in range(3, 7):
                        wait_file(data / f'recording.{frame}.ready', host)
                        deadline = time.monotonic() + 5
                        while True:
                            state = client.status(session)
                            stream = state['image_recording']
                            require(stream['phase'] == 'recording', stream)
                            captured = [client.capture(session, identity) for identity in stream['captures']]
                            current = next((entry for entry in captured if
                                            (entry.get('frame') or {}).get('frame_id') == frame and entry['phase'] == 'ready'), None)
                            if current is not None:
                                break
                            require(time.monotonic() < deadline, captured)
                            time.sleep(.01)
                        reader = client.retain_image(session, current['capture_id'])['image_reader_id']
                        info, pixels = export(client, reader, 0)
                        require(pixels and client.image_info(reader)['frame']['frame_id'] == frame,
                                'Real-time recording substituted another frame')
                        client.release_image(reader)
                        operation = client.submit(lease, CommandKind.RELEASE, capture=current['capture_id'])['operation_id']
                        released = client.wait(session, operation)
                        require(released['progress'] == 'released', released)
                        client.acknowledge(lease, released['command_id'])
                        require(len(client.status(session)['image_recording']['captures']) == 2,
                                'Release failed to replenish the bounded stream')
                        if frame < 6:
                            (data / f'recording.{frame}.continue').touch()
                    require(client.status(session)['image_recording']['length'] == 4, 'Stream skipped a frame')
                operation = client.submit(lease, CommandKind.STOP_IMAGE_RECORDING)['operation_id']
                stopped = client.wait(session, operation)
                require(stopped['progress'] == 'recording_stopped', stopped)
                for capture in client.status(session)['image_recording']['captures']:
                    released = client.wait(session, client.submit(lease, CommandKind.RELEASE, capture=capture)['operation_id'])
                    require(released['progress'] == 'released', released)
                client.relinquish(lease)
                for frame in range(3, 11):
                    (data / f'recording.{frame}.continue').touch()
                wait_file(data / 'control.destroyed', host)
                for reader in readers:
                    _, pixels = export(client, reader, 0)
                    require(pixels, 'Retained recording image vanished with app device')
                    client.release_image(reader)
                (data / 'control.retained.continue').touch()
                output = host.communicate(timeout=15)[0]
                require(host.returncode == 0 and 'VUID-' not in output and 'Vulkan validation error' not in output, output)
                print(f'PASS: real-time {"overrun at exact frame 5" if overrun else "consecutive capture/refill"}; no pacing change; GPU-safe teardown')
        finally:
            if host.poll() is None:
                host.kill()
                host.wait()
            remaining = host.stdout.read() if not host.stdout.closed else ''
            if remaining:
                print(remaining, file=sys.stderr)
            host.stdout.close()


if __name__ == '__main__':
    run(sys.argv[1], True)
    run(sys.argv[1], False)
