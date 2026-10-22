"""Depth crop/range fidelity and retained GPU lifetime on the real Vulkan path."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from test_native_quad import export, require, wait_file
from simxr_control import Capability, Client, CommandKind, TargetKind
from simxr_control.errors import ControlError
from simxr_control.protocol import ErrorCode


def release(client, lease, capture):
    operation = client.submit(lease, CommandKind.RELEASE, capture=capture)['operation_id']
    require(client.wait(lease.session, operation)['progress'] == 'released', 'Capture release failed')


def main():
    with tempfile.TemporaryDirectory(prefix='sx-depth-') as temporary:
        data = Path(temporary)
        environment = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_GPU_INSPECTION='1',
                           SIMXR_PROBE_DEPTH='1', SIMXR_PROBE_FRAMES='5')
        environment.pop('SIMXR_CONTROL_SOCKET', None)
        host = subprocess.Popen([sys.argv[1]], env=environment, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        try:
            wait_file(data / 'control.ready', host)
            with Client(data / f'control.{host.pid}.sock') as client:
                session, = client.sessions()
                needed = Capability.DEPTH_CAPTURE | Capability.IMAGE_EXPORT
                require(client.status(session)['capabilities'] & needed == needed, 'Depth capture unavailable')
                lease = client.acquire(session)
                pause = client.submit(lease, CommandKind.PAUSE)['operation_id']
                require(client.wait(session, pause)['progress'] == 'paused', 'Pause failed')
                readers = []
                commands = [(3, TargetKind.LAYER_DEPTH, 1),
                            (4, TargetKind.LAYER_DEPTH_LINEAR, 0),
                            (5, TargetKind.LAYER_DEPTH_LINEAR, 1),
                            (6, TargetKind.LAYER_DEPTH, 0),
                            (7, TargetKind.LAYER_DEPTH_LINEAR, 0)]
                for frame, target, view in commands:
                    client.renew(lease)
                    extra = [(TargetKind.LAYER_DEPTH, 0, 0), (TargetKind.LAYER_VIEW, 0, 1)] if frame == 3 and os.getenv('SIMXR_PROBE_BUNDLE') else []
                    operation = client.submit(lease, CommandKind.STEP, target=target, layer=0, view=view,
                                              additional_targets=extra)['operation_id']
                    if frame == 3:
                        (data / 'control.continue').touch()
                    record = client.wait(session, operation)
                    require(record['phase'] == ('failed' if frame == 7 else 'complete'), record)
                    require(record['frame']['frame_id'] == frame, record)
                    if extra:
                        require(len(record['captures']) == 3, record)
                        for capture_id, (kind, layer, selected_view) in zip(record['captures'][1:], extra):
                            member = client.capture(session, capture_id)
                            require(all(member['frame'][key] == record['frame'][key] for key in ('generation', 'frame_id')) and member['image_available'], member)
                            reader = client.retain_image(session, capture_id, target=kind,
                                                         layer=layer, view=selected_view)['image_reader_id']
                            readers.append((reader, frame, kind, selected_view))
                            release(client, lease, capture_id)
                    capture = client.capture(session, record['capture_id'])
                    require(capture['target']['capture_kind'] == target, capture)
                    inspected = client.inspect(session, record['capture_id'])
                    depth = inspected['document']['layers']['layers'][0]['views'][view]['depth']
                    require(depth['image']['array_slice'] == view, depth)
                    if frame >= 6:
                        require(depth['far_z'] == '+Infinity', depth)
                    if frame == 7:
                        require(not capture['image_available'], 'Infinite-range linear depth falsely available')
                    else:
                        require(record['progress'] == 'snapshot_ready' and capture['image_available'], record)
                        reader = client.retain_image(session, record['capture_id'], target=target,
                                                     layer=0, view=view)['image_reader_id']
                        readers.append((reader, frame, target, view))
                        if frame == 3:
                            example_env = dict(environment, PYTHONPATH=str(Path(__file__).resolve().parents[2] / 'sdk/python'))
                            result = subprocess.run([sys.executable,
                                str(Path(__file__).resolve().parents[2] / 'examples/inspect-retained-image.py'),
                                str(data / f'control.{host.pid}.sock'), str(record['capture_id']),
                                str(data / 'export'), '--target', 'layer_depth', '--view', '1'],
                                env=example_env, capture_output=True, text=True, timeout=20)
                            require(result.returncode == 0, result.stdout + result.stderr)
                            require((data / 'export/image.ppm').read_bytes().startswith(b'P6\n62 46\n255\n'),
                                    'Public exact-frame export example produced the wrong image')
                        try:
                            client.retain_image(session, record['capture_id'], target=TargetKind.LAYER_VIEW,
                                                layer=0, view=view)
                        except ControlError as error:
                            require(error.code == ErrorCode.NOT_FOUND, error.record)
                        else:
                            raise AssertionError('Unretained color was fabricated from an old application image')
                    client.release_inspection(inspected['inspection_id'])
                    require(client.status(session)['pipeline']['permits'] == 0, 'Image selection admitted hidden work')
                    release(client, lease, record['capture_id'])
                    client.acknowledge(lease, record['command_id'])
                    # Present the completed frame and release terminal display-operation readers.
                    for _ in range(16):
                        operation = client.submit(lease, CommandKind.DISPLAY_STEP)['operation_id']
                        displayed = client.wait(session, operation)
                        require(displayed['phase'] == 'complete', displayed)
                        selected = client.capture(session, displayed['capture_id'])
                        release(client, lease, displayed['capture_id'])
                        client.acknowledge(lease, displayed['command_id'])
                        if (selected.get('frame') or {}).get('frame_id') == frame:
                            break
                    else:
                        raise AssertionError('Paused display did not reach completed frame')
                resumed = client.submit(lease, CommandKind.RESUME)['operation_id']
                require(client.wait(session, resumed)['phase'] == 'complete', 'Resume failed')
                wait_file(data / 'control.destroyed', host)
                require(not client.sessions(), 'Public session survived destruction')
                for reader, frame, target, view in readers:
                    info = client.image_info(reader)
                    require(info['frame']['frame_id'] == frame, info)
                    descriptor = info['image_descriptor']
                    width, height = (64, 48) if target == TargetKind.LAYER_VIEW else (62, 46)
                    require(descriptor['width'] == width and descriptor['height'] == height, descriptor)
                    _, pixels = export(client, reader, 0)
                    require(len(pixels) == width * height * 4, 'Wrong export dimensions')
                    if target == TargetKind.LAYER_VIEW:
                        require(all(pixels[index:index + 4] == bytes((0, 0, 255, 255))
                                    for index in range(0, len(pixels), 4)), 'Bundled color changed after destruction')
                        client.release_image(reader)
                        continue
                    for y in range(46):
                        for x in range(62):
                            value = .1 * ((frame - 3) % 4 + 1) + .15 * view + .05 * ((x + 1) // 16) + .025 * ((y + 1) // 12)
                            if target == TargetKind.LAYER_DEPTH_LINEAR:
                                near, far = (10, .1) if frame == 5 else (.1, 10)
                                value = near * value / (far * (1 - value) + near * value)
                            encoded = 12.92 * value if value <= .0031308 else 1.055 * value ** (1 / 2.4) - .055
                            expected = round(encoded * 255)
                            offset = (y * 62 + x) * 4
                            actual = pixels[offset:offset + 4]
                            require(all(abs(component - expected) <= 2 for component in actual[:3]) and actual[3] == 255,
                                    f'Frame {frame} depth ({x}, {y}): {list(actual)}, expected {expected}')
                    client.release_image(reader)
                (data / 'control.retained.continue').touch()
                output = host.communicate(timeout=15)[0]
                require(host.returncode == 0, output)
                require('VUID-' not in output and 'Vulkan validation error' not in output, output)
                require('preserving native owners' not in output, output)
                print(output)
            print('PASS: exact cropped depth, finite/reversed ranges and honest infinite-range limitation after device destruction')
            return 0
        finally:
            if host.poll() is None:
                host.kill()
                host.wait()
            remaining = host.stdout.read() if not host.stdout.closed else ''
            if remaining:
                print(remaining, file=sys.stderr)
            host.stdout.close()


if __name__ == '__main__':
    raise SystemExit(main())
