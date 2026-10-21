"""Acknowledged real-time sequence playback and exact native capture tickets."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from test_native_quad import export, require, wait_file
from simxr_control import Capability, Client


def main(probe):
    with tempfile.TemporaryDirectory(prefix='sx-seq-') as temporary:
        data = Path(temporary)
        environment = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_GPU_INSPECTION='1',
                           SIMXR_PROBE_RECORDING='1', SIMXR_PROBE_DYNAMIC_COLOR='1',
                           SIMXR_PROBE_CANONICAL_SEQUENCE='1', SIMXR_PROBE_FRAMES='8')
        environment.pop('SIMXR_CONTROL_SOCKET', None)
        host = subprocess.Popen([probe], env=environment, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        try:
            wait_file(data / 'control.ready', host)
            # The existing application probe asserts .3/.4/.5/.6 head positions. Supply
            # that motion exclusively through acknowledged control, with no sequence file.
            require(not (data / 'sequence_command.json').exists() and not (data / 'sequence.jsonl').exists(),
                    'Native probe unexpectedly staged a legacy sequence')
            with Client(data / f'control.{host.pid}.sock') as client:
                require(client.hello['capabilities'] & Capability.POSE_SEQUENCES, 'Sequence capability missing')
                session = client.sessions()[0]
                steps = [{'frame': frame, 'head': {'x': .3 + .1 * frame}, 'capture': frame < 2}
                         for frame in range(4)]
                steps[0]['head'].update(y=1.8, z=-.4, yaw=.2, pitch=-.1, roll=.15)
                program = '\n'.join(json.dumps(step) for step in steps)
                uploaded = client.upload_sequence(session, program)
                sequence = client.start_sequence(session, uploaded)['sequence_id']
                queued = client.sequence_status(session, sequence)['document']
                require(queued['status']['origin'] == 0 and queued['captures'] == [], queued)
                (data / 'control.continue').touch()
                readers = []
                for frame in range(3, 11):
                    try:
                        wait_file(data / f'recording.{frame}.ready', host)
                    except AssertionError as error:
                        raise AssertionError(f'While waiting for sequence frame {frame}: {error}') from error
                    status = client.status(session)
                    require(not status['pipeline']['paused'] and status['pipeline']['permits'] == 0, status)
                    if frame < 5:
                        deadline = time.monotonic() + 5
                        while True:
                            page = client.sequence_status(session, sequence)['document']
                            captures = [item for item in page['captures'] if item['frame'] == frame]
                            if captures and captures[0]['phase'] == 'ready':
                                break
                            require(time.monotonic() < deadline, page)
                            time.sleep(.005)
                        capture = captures[0]
                        require(capture['admission'] == 'accepted' and capture['result'] == 0, capture)
                        reader = client.retain_image(session, capture=capture['capture_id'])['image_reader_id']
                        metadata, pixels = export(client, reader, 0)
                        require(client.image_info(reader)['frame']['frame_id'] == frame, metadata)
                        expected = bytes([255, 0, 0, 255] if frame == 3 else [0, 255, 0, 255])
                        require(pixels and pixels == expected * (len(pixels) // 4),
                                f'Frame {frame} pixels did not match its red/green source')
                        readers.append((reader, bytes(pixels)))
                        require(client.release_sequence_capture(session, capture['capture_id'])['released'], capture)
                    if frame < 10:
                        (data / f'recording.{frame}.continue').touch()
                page = client.sequence_status(session, sequence)['document']
                require([item['frame'] for item in page['captures']] == [3, 4] and
                        page['status']['origin'] == 3 and not page['status']['active'] and
                        not page['held_captures'], page)
                client.stop_sequence(session, sequence)
                (data / 'recording.10.continue').touch()
                wait_file(data / 'control.destroyed', host)
                for reader, original in readers:
                    _, pixels = export(client, reader, 0)
                    require(pixels == original, 'Retained sequence pixels changed after app-device destruction')
                    client.release_image(reader)
                require(not (data / 'sequence_command.json').exists(), 'Unexpected sequence file fallback')
                (data / 'control.retained.continue').touch()
                output = host.communicate(timeout=15)[0]
                require(host.returncode == 0 and 'VUID-' not in output and 'Vulkan validation error' not in output, output)
                print('PASS: canonical real-time sequence; exact red/green captures; no sequence file; GPU-safe release')
        finally:
            if host.poll() is None:
                host.kill()
                host.wait()
            remaining = host.stdout.read() if not host.stdout.closed else ''
            if remaining:
                print(remaining, file=sys.stderr)
            host.stdout.close()


if __name__ == '__main__':
    main(sys.argv[1])
