"""Atomic pre/post GPU windows retain exact frames beyond eviction and teardown."""
from contextlib import ExitStack
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from test_native_quad import export, require, wait_file
from simxr_control import Capability, Client
from simxr_control.errors import ControlError
from simxr_control.protocol import ErrorCode


def rejected(action, code):
    try:
        action()
    except ControlError as error:
        require(error.code == code, error.record)
    else:
        raise AssertionError('Invalid window operation succeeded')


def main():
    with tempfile.TemporaryDirectory(prefix='sx-window-') as temporary:
        data = Path(temporary)
        environment = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_GPU_INSPECTION='1',
                           SIMXR_PROBE_RECORDING='1', SIMXR_PROBE_DYNAMIC_COLOR='1',
                           SIMXR_PROBE_FRAMES='36', SIMXR_WIDTH='64', SIMXR_HEIGHT='48')
        environment.pop('SIMXR_CONTROL_SOCKET', None)
        host = subprocess.Popen([sys.argv[1]], env=environment, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        try:
            wait_file(data / 'control.ready', host)
            with ExitStack() as stack:
                client = stack.enter_context(Client(data / f'control.{host.pid}.sock'))
                other = stack.enter_context(Client(data / f'control.{host.pid}.sock'))
                session, = client.sessions()
                require(client.status(session)['capabilities'] & Capability.IMAGE_WINDOW, 'Window capability absent')
                baseline = client.status(session)
                (data / 'control.continue').touch()
                for frame in range(3, 39):
                    wait_file(data / f'recording.{frame}.ready', host)
                    if frame == 12:
                        started = client.begin_image_window(session)
                        window = started['image_window_id']
                        doc = started['document']
                        require(doc['trigger_frame'] == 12 and doc['pre_count'] == 10 and doc['post_count'] == 0, doc)
                        rejected(lambda: other.begin_image_window(session), ErrorCode.BUSY)
                        rejected(lambda: other.image_window(window), ErrorCode.NOT_FOUND)
                    if frame == 24:
                        doc = client.image_window(window)['document']
                        require(doc['phase'] == 'complete' and doc['post_count'] == 12, doc)
                        require([row['frame_id'] for row in doc['entries']] == list(range(3, 25)), doc)
                    if frame == 34:
                        # Release of a second connection's collecting window frees admission immediately.
                        cancelled = other.begin_image_window(session)['image_window_id']
                        require(other.release_image_window(cancelled)['released'], 'Window release failed')
                        require(not other.release_image_window(cancelled)['released'], 'Window release not idempotent')
                        replacement = other.begin_image_window(session)['image_window_id']
                        other.release_image_window(replacement)
                        selected = other.retain_preview(session)
                        require(selected['frame']['frame_id'] >= frame - 2, 'Window retention starved live preview')
                        other.release_image(selected['image_reader_id'])
                        status = client.status(session)
                        require(status['manual'] == baseline['manual'] and status['pipeline']['permits'] == baseline['pipeline']['permits'],
                                'Window changed frame admission')
                    (data / f'recording.{frame}.continue').touch()
                wait_file(data / 'control.destroyed', host)
                doc = client.image_window(window)['document']
                require(doc['phase'] == 'complete' and all(row['phase'] == 'ready' for row in doc['entries']), doc)
                for frame in (3, 4, 23, 24):
                    reader = client.retain_window_image(window, frame)['image_reader_id']
                    _, pixels = export(client, reader, 0)
                    expected = bytes((255, 0, 0, 255)) if frame % 2 else bytes((0, 255, 0, 255))
                    require(pixels == expected * (64 * 48), f'Window substituted pixels for frame {frame}')
                    if frame == 24:
                        require(client.release_image_window(window)['released'], 'Window release failed')
                        require(client.image_info(reader)['frame']['frame_id'] == frame, 'Independent reader invalidated')
                    client.release_image(reader)
                rejected(lambda: client.image_window(window), ErrorCode.NOT_FOUND)
                (data / 'control.retained.continue').touch()
            output = host.communicate(timeout=15)[0]
            require(host.returncode == 0, output)
            require('VUID-' not in output and 'Vulkan validation error' not in output, output)
            print('PASS GPU window: atomic pre/post frames, cancellation, bounded ownership, live preview, teardown')
        finally:
            if host.poll() is None:
                host.kill()
                print(host.communicate()[0])


if __name__ == '__main__':
    main()
