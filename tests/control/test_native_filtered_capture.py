"""Projection/quad composition captures retain exact GPU output through source teardown."""

from pathlib import Path
import sys
import tempfile

from native_support import probe_environment, running_probe, command, export, require, wait_file
from simxr_control import Capability, Client, CommandKind, TargetKind
from simxr_control.errors import ControlError
from simxr_control.protocol import ErrorCode


def main():
    with tempfile.TemporaryDirectory(prefix='sx-filter-') as temporary:
        data = Path(temporary)
        environment = probe_environment(data, SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_GPU_INSPECTION='1',
                           SIMXR_WIDTH='64', SIMXR_HEIGHT='48')

        with running_probe([sys.argv[1]], environment) as host:
            wait_file(data / 'control.ready', host)
            with Client(data / f'control.{host.pid}.sock') as client:
                session, = client.sessions()
                require(client.status(session)['capabilities'] & Capability.FILTERED_COMPOSITION,
                        'Filtered composition capability absent')
                lease = client.acquire(session)
                paused = command(client, lease, CommandKind.PAUSE)
                for capture in paused.get('captures', []):
                    command(client, lease, CommandKind.RELEASE, capture=capture)
                readers = []
                for frame, target in ((3, TargetKind.PROJECTION_LAYERS), (4, TargetKind.QUAD_LAYERS)):
                    operation = client.submit(lease, CommandKind.STEP, target=target)['operation_id']
                    if frame == 3:
                        (data / 'control.continue').touch()
                    result = client.wait(session, operation)
                    require(result['phase'] == 'complete' and result['frame']['frame_id'] == frame, result)
                    capture = result['capture_id']
                    retained = client.retain_image(session, capture, target=target)
                    readers.append((retained['image_reader_id'], frame, target))
                    other = TargetKind.QUAD_LAYERS if target == TargetKind.PROJECTION_LAYERS else TargetKind.PROJECTION_LAYERS
                    try:
                        client.retain_image(session, capture, target=other)
                    except ControlError as error:
                        require(error.code == ErrorCode.NOT_FOUND, error.record)
                    else:
                        raise AssertionError('Unretained layer composition was substituted')
                    command(client, lease, CommandKind.RELEASE, capture=capture)
                    client.acknowledge(lease, result['command_id'])
                    for _ in range(16):
                        shown = command(client, lease, CommandKind.DISPLAY_STEP)
                        command(client, lease, CommandKind.RELEASE, capture=shown['capture_id'])
                        if shown.get('frame', {}).get('frame_id') == frame:
                            break
                    else:
                        raise AssertionError('Display did not reach filtered source frame')
                command(client, lease, CommandKind.RESUME)
                client.relinquish(lease)
                wait_file(data / 'control.destroyed', host)
                for reader, frame, target in readers:
                    info = client.image_info(reader)
                    require(info['frame']['frame_id'] == frame and info['target']['capture_kind'] == target, info)
                    for eye in (0, 1):
                        _, pixels = export(client, reader, eye)
                        require(len(pixels) == 64 * 48 * 4, 'Wrong filtered eye extent')
                        def pixel(x, y):
                            return list(pixels[(y * 64 + x) * 4:(y * 64 + x) * 4 + 3])
                        if target == TargetKind.PROJECTION_LAYERS:
                            expected = [255, 0, 0] if eye == 0 else [0, 0, 255]
                            require(pixel(8, 8) == expected and pixel(32, 24) == expected,
                                    f'Quad contaminated projection eye {eye}: {pixel(32, 24)}')
                        else:
                            require(pixel(8, 8) == [0, 0, 0] and pixel(32, 24) == ([0, 0, 0] if eye == 0 else [255, 0, 0]),
                                    f'Projection contaminated quad eye {eye}: {pixel(8, 8)}, {pixel(32, 24)}')
                    client.release_image(reader)
                (data / 'control.retained.continue').touch()
                output = host.communicate(timeout=15)[0]
                require(host.returncode == 0, output)
                require('VUID-' not in output and 'Vulkan validation error' not in output, output)
            print('PASS exact projection/quad GPU composition and retained output after application-device teardown')


if __name__ == '__main__':
    main()
