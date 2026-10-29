"""Acknowledged real-time sequence playback and exact native capture tickets."""
import json
from pathlib import Path
import sys
import tempfile
import time

from native_support import probe_environment, running_probe, export, require, wait_file
from simxr_control import Capability, Client


def main(probe, startup=False):
    with tempfile.TemporaryDirectory(prefix='sx-seq-') as temporary:
        data = Path(temporary)
        environment = probe_environment(data, SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_GPU_INSPECTION='1',
                           SIMXR_PROBE_RECORDING='1', SIMXR_PROBE_DYNAMIC_COLOR='1',
                           SIMXR_PROBE_CANONICAL_SEQUENCE='1', SIMXR_PROBE_FRAMES='8')

        steps = [{'frame': frame + (2 if startup else 0), 'head': {'x': .3 + .1 * frame},
                  'capture': frame < 2} for frame in range(4)]
        steps[0]['head'].update(y=1.8, z=-.4, yaw=.2, pitch=-.1, roll=.15)
        program = '\n'.join(json.dumps(step) for step in steps)
        owner = {'application_owned': True} if startup else {}
        if startup:
            source = data / 'startup.jsonl'
            source.write_text(program)
            environment['SIMXR_SEQUENCE'] = str(source)

        with running_probe([probe], environment) as host:
            wait_file(data / 'control.ready', host)
            # The existing application probe asserts .3/.4/.5/.6 head positions. Supply
            # that motion exclusively through acknowledged control, with no sequence file.
            require(not (data / 'sequence_command.json').exists() and not (data / 'sequence.jsonl').exists(),
                    'Native probe unexpectedly staged a legacy sequence')
            with Client(data / f'control.{host.pid}.sock') as client:
                require(client.hello['capabilities'] & Capability.POSE_SEQUENCES, 'Sequence capability missing')
                session = client.sessions()[0]
                if startup:
                    source.unlink()
                    queued = client.sequence_status(session, **owner)['document']
                    sequence = queued['sequence_id']
                    require(queued['status']['origin'] == 1 and queued['status']['active'], queued)
                    # Reading an application-owned execution must not adopt it into the connection.
                    with Client(data / f'control.{host.pid}.sock') as observer:
                        require(observer.sequence_status(session, **owner)['document']['sequence_id'] == sequence,
                                'Observer did not select startup execution')
                else:
                    uploaded = client.upload_sequence(session, program)
                    sequence = client.start_sequence(session, uploaded)['sequence_id']
                    queued = client.sequence_status(session, sequence, **owner)['document']
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
                            page = client.sequence_status(session, sequence, **owner)['document']
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
                        require(client.release_sequence_capture(session, capture['capture_id'], **owner)['released'], capture)
                    if frame < 10:
                        (data / f'recording.{frame}.continue').touch()
                page = client.sequence_status(session, sequence, **owner)['document']
                require([item['frame'] for item in page['captures']] == [3, 4] and
                        page['status']['origin'] == (1 if startup else 3) and not page['status']['active'] and
                        not page['held_captures'], page)
                client.stop_sequence(session, sequence, **owner)
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
                require(not any((data / name).exists() for name in (
                    'screenshot.bmp', 'screenshot_status.json', 'runtime_status.json',
                    'frame_info.json', 'flicker_status.json')), 'Legacy output files were produced')
                print('PASS: canonical real-time sequence; exact red/green captures; no sequence file; GPU-safe release')


if __name__ == '__main__':
    main(sys.argv[1], '--startup' in sys.argv[2:])
