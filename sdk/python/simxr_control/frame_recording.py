"""Bounded image recording through exact-frame admission and reserved streams."""
from contextlib import ExitStack
import json
from pathlib import Path
import queue
import threading
import time

from .protocol import Capability, CommandKind, TargetKind


def _json(value):
    if isinstance(value, bytes):
        return value.hex()
    raise TypeError(f'Unsupported metadata value: {type(value).__name__}')


def _write_frame(directory, metadata, images):
    frame = metadata['capture']['frame']['frame_id']
    stem = f'frame-{frame:020d}'
    for layer, pixels in enumerate(images):
        (directory / f'{stem}-{layer}.rgba').write_bytes(pixels)
    (directory / f'{stem}.json').write_text(json.dumps(metadata, indent=2, default=_json) + '\n', encoding='utf-8')
    with (directory / 'frames.jsonl').open('a', encoding='utf-8') as index:
        index.write(json.dumps({'frame_id': frame, 'metadata': f'{stem}.json', 'images': len(images)}) + '\n')


class _Persistence:
    def __init__(self, directory, capacity):
        self.directory = directory
        self.slots = threading.BoundedSemaphore(capacity)
        self.items = queue.Queue(capacity)
        self.error = None
        self.written = 0
        self.worker = threading.Thread(target=self._run, name='simxr-frame-writer')
        self.worker.start()

    def _run(self):
        while True:
            item = self.items.get()
            if item is None:
                return
            try:
                if self.error is None:
                    _write_frame(self.directory, *item)
                    self.written += 1
            except BaseException as error:
                self.error = error
            finally:
                item = None
                self.slots.release()

    def check(self):
        if self.error is not None:
            raise RuntimeError('Frame persistence failed; recording is incomplete') from self.error

    def close(self):
        self.items.put(None)
        self.worker.join()
        self.check()


def record_frames(client, session, directory, count, *, mode, target=TargetKind.COMPOSED_EYES,
                  layer=0, view=0, buffers=2, max_frame_bytes=64 * 1024 * 1024,
                  timeout=15, cancel=None):
    """Save consecutive newly admitted frames and all output slices.

    mode is explicit: 'controlled' admits only with persistence capacity;
    'realtime' leaves pacing unchanged and fails on reserved-stream overrun. The directory
    must be new. Frames contain source-format bytes, exact CPU/image metadata and immutable
    feature revisions; frames.jsonl indexes fully written frames. Explicit image
    export is the only readback. Cancellation raises InterruptedError after cleanup.
    Real-time recording uses reserved runtime captures, never live-preview polling.
    """
    if mode not in ('controlled', 'realtime'):
        raise ValueError("Select mode='controlled' or mode='realtime' explicitly")
    if type(count) is not int or not 1 <= count < 2**64 or type(buffers) is not int or not 1 <= buffers <= 4:
        raise ValueError('Positive frame count and one through four persistence buffers are required')
    if type(max_frame_bytes) is not int or max_frame_bytes < 4 or not 0 < timeout <= 120:
        raise ValueError('Invalid byte budget or operation timeout')
    status = client.status(session)
    if status['manual']:
        raise RuntimeError('Release the existing manual debugger before starting controlled recording')
    if not status['capabilities'] & Capability.IMAGE_EXPORT:
        raise RuntimeError('Image export is unavailable')
    if mode == 'realtime' and not status['capabilities'] & Capability.IMAGE_RECORDING:
        raise RuntimeError('Real-time reserved image recording is unavailable')
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=False)
    writer = _Persistence(directory, buffers)
    lease = None
    outstanding = None
    first = last = None
    summary = {'mode': mode, 'requested': count, 'persisted': 0, 'phase': 'failed', 'buffers': buffers}
    next_renewal = time.monotonic()

    def pulse():
        nonlocal next_renewal
        writer.check()
        if cancel is not None and cancel.is_set():
            raise InterruptedError('Frame recording cancelled')
        if lease is not None and time.monotonic() >= next_renewal:
            client.renew(lease)
            next_renewal = time.monotonic() + 5

    def command(kind, **arguments):
        nonlocal outstanding
        outstanding = client.submit(lease, kind, **arguments)['operation_id']
        deadline = time.monotonic() + timeout
        while True:
            pulse()
            result = client.operation(session, outstanding)
            if result['phase'] not in ('queued', 'pending'):
                if result['phase'] != 'complete':
                    raise RuntimeError(result)
                outstanding = None
                return result
            if time.monotonic() >= deadline:
                raise TimeoutError(f'Frame operation {outstanding} remains unfinished')
            time.sleep(.01)

    def release(capture):
        released = command(CommandKind.RELEASE, capture=capture)
        client.acknowledge(lease, released['command_id'])

    def stream():
        value = client.status(session)['image_recording']
        if value['phase'] == 'overrun':
            summary['overrun'] = value
            raise RuntimeError(f'Real-time recording overrun: {value}')
        return value

    try:
        with ExitStack() as cleanup:
            cleanup.callback(writer.close)
            lease = client.acquire(session)
            # Relinquishment resumes admission and retires lease-owned captures.
            cleanup.callback(client.relinquish, lease)
            cleanup.callback(lambda: client.submit(lease, CommandKind.CANCEL, operation=outstanding)
                             if outstanding is not None else None)
            if mode == 'realtime':
                cleanup.callback(client.submit, lease, CommandKind.STOP_IMAGE_RECORDING)
            if mode == 'controlled':
                paused = command(CommandKind.PAUSE)
                for capture in paused.get('captures', []):
                    release(capture)
                client.acknowledge(lease, paused['command_id'])
            else:
                started = command(CommandKind.START_IMAGE_RECORDING, target=target, layer=layer, view=view,
                                  recording_capacity=buffers, recording_frames=count)
                client.acknowledge(lease, started['command_id'])
            for _ in range(count):
                while not writer.slots.acquire(timeout=.05):
                    pulse()
                    if mode == 'realtime':
                        stream()
                handed_off = False
                capture_id = reader = inspection_id = None
                with ExitStack() as frame_cleanup:
                    frame_cleanup.callback(lambda: None if handed_off else writer.slots.release())
                    pulse()
                    if mode == 'controlled':
                        admitted = command(CommandKind.STRICT_ADMISSION, target=target, layer=layer, view=view)
                        capture_id = admitted['capture_id']
                    else:
                        deadline = time.monotonic() + timeout
                        while capture_id is None:
                            pulse()
                            state = stream()
                            candidates = [client.capture(session, identity) for identity in state['captures']]
                            candidates = [entry for entry in candidates if entry.get('frame') and
                                          (last is None or entry['frame']['frame_id'] > last)]
                            if candidates:
                                capture_id = min(candidates, key=lambda entry: entry['frame']['frame_id'])['capture_id']
                                break
                            if state['phase'] != 'recording' or time.monotonic() >= deadline:
                                raise RuntimeError(f'Real-time stream ended before the requested frame: {state}')
                            time.sleep(.01)
                    frame_cleanup.callback(release, capture_id)
                    deadline = time.monotonic() + timeout
                    while True:
                        pulse()
                        capture = client.capture(session, capture_id)
                        if capture['phase'] in ('ready', 'clear'):
                            break
                        if capture['phase'] in ('unavailable', 'cancelled', 'failed', 'released', 'discarded', 'empty'):
                            raise RuntimeError(capture)
                        if time.monotonic() >= deadline:
                            raise TimeoutError('Captured image did not become ready')
                        time.sleep(.01)
                    frame = capture['frame']['frame_id']
                    if last is not None and frame != last + 1:
                        raise RuntimeError(f'Nonconsecutive recording: expected {last + 1}, received {frame}')
                    if first is None:
                        first = frame
                    last = frame
                    inspection = client.inspect(session, capture_id)
                    inspection_id = inspection['inspection_id']
                    frame_cleanup.callback(client.release_inspection, inspection_id)
                    revisions = {}

                    def collect(value):
                        if isinstance(value, dict):
                            if 'revision_id' in value:
                                identity = value['revision_id']
                                if str(identity) not in revisions:
                                    revisions[str(identity)] = client.revision(identity)
                            for child in value.values():
                                collect(child)
                        elif isinstance(value, list):
                            for child in value:
                                collect(child)

                    collect(inspection['document'])
                    metadata = {'capture': capture, 'frame': inspection['document'], 'revisions': revisions}
                    images = []
                    if capture['image_available']:
                        reader = client.retain_image(session, capture_id)['image_reader_id']
                        frame_cleanup.callback(client.release_image, reader)
                        metadata['image'] = client.image_info(reader)
                        descriptor = metadata['image']['image_descriptor']
                        pixel_bytes = {37: 4, 43: 4, 44: 4, 50: 4, 64: 4, 97: 8, 109: 16}.get(descriptor['format'])
                        if pixel_bytes is None:
                            raise RuntimeError('Unsupported frame export color format')
                        expected = descriptor['width'] * descriptor['height'] * pixel_bytes
                        if expected * descriptor['array_layers'] > max_frame_bytes:
                            raise RuntimeError('Frame exceeds the configured persistence byte budget')
                        for slice_index in range(descriptor['array_layers']):
                            ticket = client.start_export(reader, layer=slice_index)['export_id']
                            try:
                                deadline = time.monotonic() + timeout
                                while True:
                                    pulse()
                                    exported = client.export_info(ticket)
                                    if exported['phase'] != 'pending':
                                        break
                                    if time.monotonic() >= deadline:
                                        raise TimeoutError('Explicit frame export did not complete')
                                    time.sleep(.01)
                                if exported['phase'] != 'complete' or exported['total_bytes'] != expected:
                                    raise RuntimeError(exported)
                                pixels = bytearray()
                                while len(pixels) < expected:
                                    pulse()
                                    if time.monotonic() >= deadline:
                                        raise TimeoutError('Explicit frame read did not complete')
                                    chunk = client.read_export(ticket, len(pixels), 64000)
                                    if (chunk['offset'] != len(pixels) or not chunk['bytes']
                                            or len(chunk['bytes']) > expected - len(pixels)):
                                        raise RuntimeError('Invalid frame export chunk')
                                    pixels.extend(chunk['bytes'])
                                images.append(pixels)
                                pixels = None
                            finally:
                                client.release_export(ticket)
                    writer.items.put((metadata, images))
                    metadata = images = None
                    handed_off = True
                # Retire old display readers while remaining paused. Their terminal
                # operation holds an image until acknowledged independently of capture.
                if mode == 'controlled':
                    for _ in range(16):
                        displayed = command(CommandKind.DISPLAY_STEP)
                        selected = client.capture(session, displayed['capture_id'])
                        release(displayed['capture_id'])
                        client.acknowledge(lease, displayed['command_id'])
                        if (selected.get('frame') or {}).get('frame_id') == frame:
                            break
                    else:
                        raise RuntimeError('Paused display failed to reach the recorded frame')
            while writer.written < count:
                pulse()
                time.sleep(.01)
            if mode == 'realtime':
                stream()
        summary['phase'] = 'complete'
    except BaseException as error:
        summary['phase'] = 'cancelled' if isinstance(error, InterruptedError) else 'failed'
        summary['error'] = f'{type(error).__name__}: {error}'
        if error.__cause__ is not None:
            summary['cause'] = str(error.__cause__)
        raise
    finally:
        summary.update(persisted=writer.written, first_frame=first, last_frame=last)
        (directory / 'recording.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    return summary
