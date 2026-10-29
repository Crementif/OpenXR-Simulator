"""Every accepted image survives pause/step until bounded oldest eviction."""
from pathlib import Path
import sys
import tempfile
import time

from native_support import probe_environment, running_probe, export, require, wait_file, command
from simxr_control import Capability, Client, CommandKind, ControlError, ErrorCode


def retain(client, session, frame, states=None):
    deadline = time.monotonic() + 5
    while True:
        try:
            image = client.retain_preview(session, frame=frame)
            require(image['frame']['frame_id'] == frame, 'Selected frame was substituted')
            require('image_reader_id' in image or image['image_available'] is False, image)
            if states is not None:
                require('tracking' in image.get('document', {}), 'Historical image lost tracking state')
                states[frame] = image['document']['tracking']
            return image.get('image_reader_id')
        except ControlError as error:
            require(error.code == ErrorCode.BUSY and time.monotonic() < deadline, error.record)
            time.sleep(.005)


def main(probe):
    with tempfile.TemporaryDirectory(prefix='simxr-continuous-') as temporary:
        data = Path(temporary)
        environment = probe_environment(data, SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_GPU_INSPECTION='1',
                           SIMXR_PROBE_RECORDING='1', SIMXR_PROBE_DYNAMIC_COLOR='1',
                           SIMXR_PROBE_FRAMES='302', SIMXR_WIDTH='64', SIMXR_HEIGHT='48')
        with running_probe([probe], environment) as host:
            wait_file(data / 'control.ready', host)
            with Client(data / f'control.{host.pid}.sock') as client:
                session, = client.sessions()
                require(client.status(session)['capabilities'] & Capability.CONTINUOUS_PREVIEWS,
                        'Continuous preview capability absent')
                (data / 'control.continue').touch()
                wait_file(data / 'recording.3.ready', host)
                client.configure_previews(session, 1280, 1024)
                first = retain(client, session, 3)
                lease = client.acquire(session)
                paused = command(client, lease, CommandKind.PAUSE)
                for capture in paused.get('captures', []):
                    command(client, lease, CommandKind.RELEASE, capture=capture)
                again = retain(client, session, 3)
                require(again == first, 'Pause discarded the old frame image')
                states = {}
                for frame in range(4, 305):
                    (data / f'recording.{frame - 1}.continue').touch()
                    for _ in range(8):
                        stepped = command(client, lease, CommandKind.STEP, advance_display=True)
                        if stepped['frame']['frame_id'] == frame:
                            break
                        require(stepped['frame']['frame_id'] < frame, stepped)
                        if stepped.get('capture_id'):
                            command(client, lease, CommandKind.RELEASE, capture=stepped['capture_id'])
                    else:
                        raise AssertionError('Step did not drain pre-pause presentation backlog')
                    wait_file(data / f'recording.{frame}.ready', host)
                    image = retain(client, session, frame, states if frame in (4, 100, 204) else None)
                    if frame in (4, 100, 204):
                        _, pixels = export(client, image, 0)
                        expected = bytes((0, 255, 0, 255)) if frame % 2 == 0 else bytes((255, 0, 0, 255))
                        require(pixels == expected * (64 * 48), f'Wrong pixels for stepped frame {frame}')
                    client.release_image(image)
                    if stepped.get('capture_id'):
                        command(client, lease, CommandKind.RELEASE, capture=stepped['capture_id'])
                    for capture in stepped.get('captures', []):
                        if capture != stepped.get('capture_id'):
                            command(client, lease, CommandKind.RELEASE, capture=capture)
                    client.renew(lease)
                status = client.status(session)
                frames = status['preview_frame_ids']
                require(frames[-1] == 304 and len(frames) > 256, status)
                require(frames == list(range(frames[0], 305)), 'Retained history has missing frames')
                require(status['preview_bytes'] <= status['preview_budget'], 'History exceeded memory budget')
                for frame in frames:
                    client.renew(lease)
                    image = retain(client, session, frame)
                    if image is not None and image != first:
                        client.release_image(image)
                client.configure_previews(session, 64, 64)
                deadline = time.monotonic() + 15
                while True:
                    shrunk = client.status(session)
                    require(not shrunk['preview_error'], shrunk)
                    if shrunk['preview_bytes'] <= shrunk['preview_budget']:
                        break
                    require(time.monotonic() < deadline, shrunk)
                    client.renew(lease)
                    time.sleep(.02)
                require(shrunk['preview_budget'] == 64 * 1024 * 1024, shrunk)
                require(shrunk['preview_ram_budget'] == 64 * 1024 * 1024 and shrunk['preview_ram_bytes'] > 0, shrunk)
                require(shrunk['preview_frame_ids'] == frames, 'GPU shrink discarded archived images')
                for frame in (4, 100, 7, 204):
                    archived_states = {}
                    image = retain(client, session, frame, archived_states)
                    if frame in states:
                        require(archived_states[frame] == states[frame], 'Archived pose changed after CPU history retirement')
                    _, pixels = export(client, image, 0)
                    expected = (0, 255, 0, 255) if frame % 2 == 0 else (255, 0, 0, 255)
                    require(all(abs(value - expected[index % 4]) <= 5 for index, value in enumerate(pixels)),
                            f'Wrong archived pixels for frame {frame}')
                    client.release_image(image)
                    client.renew(lease)
                command(client, lease, CommandKind.RESUME)
                client.relinquish(lease)
                (data / 'recording.304.continue').touch()
                wait_file(data / 'control.destroyed', host)
                _, pixels = export(client, first, 0)
                require(pixels == bytes((255, 0, 0, 255)) * (64 * 48),
                        'Retained reader changed after eviction and application-device destruction')
                client.release_image(first)
                (data / 'control.retained.continue').touch()
            output = host.communicate(timeout=15)[0]
            require(host.returncode == 0 and 'VUID-' not in output and 'Vulkan validation error' not in output, output)
            print('PASS 301 strict steps with live budget growth/shrink and more than 256 frames: every image, pause survival, exact pixels, H.264 decode, bounded bytes, GPU teardown')


if __name__ == '__main__':
    main(sys.argv[1])
