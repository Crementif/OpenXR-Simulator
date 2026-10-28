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
from simxr_control import Client, TargetKind, record_frames
import simxr_control.frame_recording as recording


def run(probe, cancelled, failed=False, mode='controlled', overrun=False, hdr=False):
    with tempfile.TemporaryDirectory(prefix='sx-record-') as temporary:
        data = Path(temporary)
        environment = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_GPU_INSPECTION='1',
                           SIMXR_PROBE_FRAMES='8')
        if hdr:
            environment['SIMXR_PROBE_HDR32'] = '1'
        if mode == 'realtime':
            environment['SIMXR_PROBE_RECORDING'] = '1'
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
                            results.append(record_frames(client, session, data / 'frames', 8 if overrun else 4,
                                                         mode=mode, buffers=1, cancel=cancel,
                                                         target=TargetKind.LAYER_VIEW if hdr else TargetKind.COMPOSED_EYES))
                    except BaseException as error:
                        errors.append(error)

                thread = threading.Thread(target=record)
                thread.start()
                deadline = time.monotonic() + 10
                while not (observer.status(session)['manual'] if mode == 'controlled' else
                           observer.status(session)['image_recording']['phase'] == 'recording'):
                    require(time.monotonic() < deadline and not errors, errors)
                    time.sleep(.01)
                (data / 'control.continue').touch()
                require(writing.wait(10), f'No frame reached persistence: {errors}')
                before = observer.status(session)['pipeline']
                time.sleep(.15)
                after = observer.status(session)['pipeline']
                require(after['permits'] == 0 and after['paused'] == (mode == 'controlled'), after)
                if mode == 'controlled':
                    require(before == after, (before, after))
                if cancelled:
                    cancel.set()
                if overrun:
                    for frame in (3, 4):
                        (data / f'recording.{frame}.continue').touch()
                        wait_file(data / f'recording.{frame + 1}.ready', host)
                    require(observer.status(session)['image_recording']['phase'] == 'overrun',
                            'Slow persistence did not produce explicit runtime overrun')
                allow_write.set()
                if mode == 'realtime' and not overrun:
                    for frame in range(3, 7):
                        deadline = time.monotonic() + 10
                        index = data / 'frames/frames.jsonl'
                        while not index.exists() or len(index.read_text().splitlines()) < frame - 2:
                            require(time.monotonic() < deadline and not errors, errors)
                            time.sleep(.01)
                        (data / f'recording.{frame}.continue').touch()
                    for frame in range(7, 11):
                        (data / f'recording.{frame}.continue').touch()
                if overrun:
                    for frame in range(3, 11):
                        (data / f'recording.{frame}.continue').touch()
                thread.join(15)
                require(not thread.is_alive(), 'Recorder worker did not stop')
                if failed or overrun:
                    require(len(errors) == 1 and isinstance(errors[0], RuntimeError), errors)
                elif cancelled:
                    require(len(errors) == 1 and isinstance(errors[0], InterruptedError), errors)
                else:
                    require(not errors and results[0]['phase'] == 'complete', errors)
                summary = json.loads((data / 'frames/recording.json').read_text())
                index = data / 'frames/frames.jsonl'
                entries = [json.loads(line) for line in index.read_text().splitlines()] if index.exists() else []
                expected_count = 0 if failed else 1 if cancelled or overrun else 4
                require(summary['phase'] == ('failed' if failed or overrun else 'cancelled' if cancelled else 'complete'), summary)
                if overrun:
                    require(summary['overrun']['frame']['frame_id'] == 5, summary)
                if failed or cancelled or overrun:
                    require(summary.get('error'), 'Partial recording omitted its failure/cancellation reason')
                require(len(entries) == expected_count == summary['persisted'], summary)
                require([entry['frame_id'] for entry in entries] == list(range(3, 3 + expected_count)), entries)
                for entry in entries:
                    metadata = json.loads((data / 'frames' / entry['metadata']).read_text())
                    require(metadata['capture']['frame']['frame_id'] == entry['frame_id'] and
                            metadata['frame']['frame_id'] == entry['frame_id'], 'Recording mixed image/state identities')
                    require(metadata['revisions'], 'Recording lost immutable feature definitions')
                    descriptor = metadata['image']['image_descriptor']
                    require(descriptor['format'] == (109 if hdr else 43), descriptor)
                    for layer in range(entry['images']):
                        pixels = data / 'frames' / f'frame-{entry["frame_id"]:020d}-{layer}.rgba'
                        require(pixels.stat().st_size == descriptor['width'] * descriptor['height'] * (16 if hdr else 4),
                                'Incomplete recorded image')
                wait_file(data / 'control.destroyed', host)
                (data / 'control.retained.continue').touch()
                output = host.communicate(timeout=15)[0]
                require(host.returncode == 0 and 'VUID-' not in output and 'Vulkan validation error' not in output, output)
                print(f'PASS: {mode} recording {summary}, explicit pacing/overrun semantics, running host and GPU-safe teardown')
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
    if '--hdr' in sys.argv:
        run(sys.argv[1], False, hdr=True)
        sys.exit(0)
    run(sys.argv[1], False, hdr=True)
    run(sys.argv[1], False)
    run(sys.argv[1], True)
    run(sys.argv[1], False, True)
    run(sys.argv[1], False, mode='realtime')
    run(sys.argv[1], False, mode='realtime', overrun=True)
