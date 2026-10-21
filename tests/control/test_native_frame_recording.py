"""Controlled consecutive image persistence, backpressure and cancellation on native Vulkan."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time

from test_native_quad import require, wait_file
from simxr_control import Client, record_frames
import simxr_control.frame_recording as recording


def run(probe, cancelled, failed=False):
    with tempfile.TemporaryDirectory(prefix='sx-record-') as temporary:
        data = Path(temporary)
        environment = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_GPU_INSPECTION='1',
                           SIMXR_PROBE_FRAMES='8')
        environment.pop('SIMXR_CONTROL_SOCKET', None)
        host = subprocess.Popen([probe], env=environment, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        thread = None
        allow_write = threading.Event()
        writing = threading.Event()
        cancel = threading.Event()
        errors, results = [], []
        original = recording._write_frame

        def slow_write(*args):
            writing.set()
            require(allow_write.wait(10), 'Persistence gate not released')
            if failed:
                raise OSError("Injected storage failure")
            original(*args)

        recording._write_frame = slow_write
        try:
            wait_file(data / 'control.ready', host)
            path = data / f'control.{host.pid}.sock'
            with Client(path) as observer:
                session, = observer.sessions()

                def record():
                    try:
                        with Client(path) as client:
                            results.append(record_frames(client, session, data / 'frames', 4,
                                                         mode='controlled', buffers=1, cancel=cancel))
                    except BaseException as error:
                        errors.append(error)

                thread = threading.Thread(target=record)
                thread.start()
                deadline = time.monotonic() + 10
                while not observer.status(session)['manual']:
                    require(time.monotonic() < deadline and not errors, errors)
                    time.sleep(.01)
                (data / 'control.continue').touch()
                require(writing.wait(10), f'No frame reached persistence: {errors}')
                # A full single-slot writer must prevent any subsequent admission.
                before = observer.status(session)['pipeline']
                time.sleep(.15)
                after = observer.status(session)['pipeline']
                require(before == after and after['permits'] == 0, (before, after))
                if cancelled:
                    cancel.set()
                allow_write.set()
                thread.join(15)
                require(not thread.is_alive(), 'Recorder worker did not stop')
                if failed:
                    require(len(errors) == 1 and isinstance(errors[0], RuntimeError), errors)
                elif cancelled:
                    require(len(errors) == 1 and isinstance(errors[0], InterruptedError), errors)
                else:
                    require(not errors and results[0]['phase'] == 'complete', errors)
                summary = json.loads((data / 'frames/recording.json').read_text())
                index = data / 'frames/frames.jsonl'
                entries = [json.loads(line) for line in index.read_text().splitlines()] if index.exists() else []
                expected_count = 0 if failed else 1 if cancelled else 4
                require(summary['phase'] == ('failed' if failed else 'cancelled' if cancelled else 'complete'), summary)
                require(len(entries) == expected_count == summary['persisted'], summary)
                require([entry['frame_id'] for entry in entries] == list(range(3, 3 + expected_count)), entries)
                for entry in entries:
                    metadata = json.loads((data / 'frames' / entry['metadata']).read_text())
                    require(metadata['capture']['frame']['frame_id'] == entry['frame_id'] and
                            metadata['frame']['frame_id'] == entry['frame_id'], 'Recording mixed image/state identities')
                    require(metadata['revisions'], 'Recording lost immutable feature definitions')
                    descriptor = metadata['image']['image_descriptor']
                    for layer in range(entry['images']):
                        pixels = data / 'frames' / f'frame-{entry["frame_id"]:020d}-{layer}.rgba'
                        require(pixels.stat().st_size == descriptor['width'] * descriptor['height'] * 4,
                                'Incomplete recorded image')
                wait_file(data / 'control.destroyed', host)
                (data / 'control.retained.continue').touch()
                output = host.communicate(timeout=15)[0]
                require(host.returncode == 0 and 'VUID-' not in output and 'Vulkan validation error' not in output, output)
                print(f'PASS: controlled recording {summary}, actual storage backpressure, resumed host and GPU-safe teardown')
        finally:
            allow_write.set()
            cancel.set()
            if host.poll() is None:
                host.kill()
                host.wait()
            if thread is not None:
                thread.join(5)
            recording._write_frame = original
            remaining = host.stdout.read() if not host.stdout.closed else ''
            if remaining:
                print(remaining, file=sys.stderr)
            host.stdout.close()


if __name__ == '__main__':
    run(sys.argv[1], False)
    run(sys.argv[1], True)
    run(sys.argv[1], False, True)
